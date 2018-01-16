#include "functions.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>


#define MAX_BUFFER 1024

sqlite3 *db;
sqlite3_stmt *res; 
int utilizator_id=20;

typedef struct{
    const char *Question[MAX_BUFFER];
    const char *answ_1[MAX_BUFFER];
    const char *answ_2[MAX_BUFFER];
    const char *answ_3[MAX_BUFFER];
    //char answ_4[MAX_BUFFER];
    const char *correct_answ[MAX_BUFFER];
    int clientSocketFd;
}h_questions;

typedef struct{
    int user_ID;
    int clientSocketFd;
}menu;

typedef struct{
    char Name[MAX_BUFFER];
    char Surname[MAX_BUFFER];
    char username[MAX_BUFFER];
    char password[MAX_BUFFER];
    int clientSocketFd;
}register_info;

typedef struct {
    char username[MAX_BUFFER];
    char password[MAX_BUFFER];
    int clientSocketFd;
    pthread_mutex_t *login_mutex;
}login_info;


/*
Queue implementation using a char array.
Contains a mutex for functions to lock on before modifying the array,
and condition variables for when it's not empty or full.
*/
typedef struct {
    char *buffer[MAX_BUFFER];
    int head, tail;
    int full, empty;
    pthread_mutex_t *mutex;
    pthread_cond_t *notFull, *notEmpty;
} queue;

/*
Struct containing important data for the server to work.
Namely the list of client sockets, that list's mutex,
the server's socket for new connections, and the message queue
*/
typedef struct {
    fd_set serverReadFds;
    int socketFd;
    int clientSockets[MAX_BUFFER];
    int numClients;
    pthread_mutex_t *clientListMutex;
    queue *queue;
} chatDataVars;

/*
Simple struct to hold the chatDataVars and the new client's socket fd.
Used only in the client handler thread.
*/
typedef struct {
    chatDataVars *data;
    login_info *logInfo;
    register_info *regInfo;
    menu * menu;
    int clientSocketFd;
} clientHandlerVars;

const char *retrieve_answers();
const char *retrieve_questions();
bool create_account(char *surname,char *name,char *account,char *password);
bool verrify_account(char *username,char*password);
bool initialize_database();
void startChat(int socketFd);
//void buildMessage(char *result, char *name, char *msg);
void bindSocket(struct sockaddr_in *serverAddr, int socketFd, long port);
void removeClient(chatDataVars *data, int clientSocketFd);

void *quizzHandler(void *questions);
void *menu_handler(void *h_menu);
void *client_register(void *regInfo);
void *client_login(void* logInfo);
void *newClientHandler(void *data);
void *clientHandler(void *chv);
void *messageHandler(void *data);

void queueDestroy(queue *q);
queue* queueInit(void);
void queuePush(queue *q, char* msg);
char* queuePop(queue *q);

int main(int argc, char *argv[])
{
    struct sockaddr_in serverAddr;
    long port = 9999;
    int socketFd;

    while(initialize_database()==0);

    if(argc == 2) port = strtol(argv[1], NULL, 0);

    if((socketFd = socket(AF_INET, SOCK_STREAM, 0))== -1)
    {
        perror("Socket creation failed");
        exit(1);
    }

    bindSocket(&serverAddr, socketFd, port);

    if(listen(socketFd, 1) == -1)
    {
        perror("listen failed: ");
        exit(1);
    }

    startChat(socketFd);
    
    close(socketFd);
}

//Spawns the new client handler thread and message consumer thread
void startChat(int socketFd)
{
    chatDataVars data;
    data.numClients = 0;
    data.socketFd = socketFd;
    data.queue = queueInit();
    data.clientListMutex = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(data.clientListMutex, NULL);

    //Start thread to handle new client connections

    pthread_t connectionThread; /* threads ID's */
    if((pthread_create(&connectionThread, NULL, (void *)&newClientHandler, (void *)&data)) == 0)
    {
        fprintf(stderr, "\n[server] Connection handler started\n");
    }

    FD_ZERO(&(data.serverReadFds));
    FD_SET(socketFd, &(data.serverReadFds));

    //Start thread to handle messages received

    pthread_t messagesThread;   /* threads ID's */

    if((pthread_create(&messagesThread, NULL, (void *)&messageHandler, (void *)&data)) == 0)
    {
        fprintf(stderr, "\n[server] Message handler started\n");
    }


    /* pthread_join || Wait until thread is done its work */

    pthread_join(connectionThread, NULL);
    pthread_join(messagesThread, NULL);

    //queueDestroy(data.queue);
    pthread_mutex_destroy(data.clientListMutex);
    free(data.clientListMutex);
}

//Initializes queue
queue* queueInit(void)
{
    queue *q = (queue *)malloc(sizeof(queue));
    if(q == NULL)
    {
        perror("Couldn't allocate anymore memory!");
        exit(EXIT_FAILURE);
    }

    q->empty = 1;
    q->full = q->head = q->tail = 0;
    q->mutex = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
    if(q->mutex == NULL)
    {
        perror("Couldn't allocate anymore memory!");
        exit(EXIT_FAILURE);
    }
    pthread_mutex_init(q->mutex, NULL);

    q->notFull = (pthread_cond_t *) malloc(sizeof(pthread_cond_t));
    if(q->notFull == NULL)
    {
        perror("Couldn't allocate anymore memory!");
        exit(EXIT_FAILURE);   
    }
    pthread_cond_init(q->notFull, NULL);

    q->notEmpty = (pthread_cond_t *) malloc(sizeof(pthread_cond_t));
    if(q->notEmpty == NULL)
    {
        perror("Couldn't allocate anymore memory!");
        exit(EXIT_FAILURE);
    }
    pthread_cond_init(q->notEmpty, NULL);

    return q;
}

//Frees a queue
void queueDestroy(queue *q)
{
    pthread_mutex_destroy(q->mutex);
    pthread_cond_destroy(q->notFull);
    pthread_cond_destroy(q->notEmpty);
    free(q->mutex);
    free(q->notFull);
    free(q->notEmpty);
    free(q);
}

//Push to end of queue
void queuePush(queue *q, char* msg)
{
    q->buffer[q->tail] = msg;
    q->tail++;
    if(q->tail == MAX_BUFFER)
        q->tail = 0;
    if(q->tail == q->head)
        q->full = 1;
    q->empty = 0;
}

//Pop front of queue
char* queuePop(queue *q)
{
    char* msg = q->buffer[q->head];
    q->head++;
    if(q->head == MAX_BUFFER)
        q->head = 0;
    if(q->head == q->tail)
        q->empty = 1;
    q->full = 0;

    return msg;
}

//Sets up and binds the socket
void bindSocket(struct sockaddr_in *serverAddr, int socketFd, long port)
{
    memset(serverAddr, 0, sizeof(*serverAddr));
    serverAddr->sin_family = AF_INET;
    serverAddr->sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr->sin_port = htons(port);

    if(bind(socketFd, (struct sockaddr *)serverAddr, sizeof(struct sockaddr_in)) == -1)
    {
        perror("Socket bind failed: ");
        exit(1);
    }
}

//Removes the socket from the list of active client sockets and closes it
void removeClient(chatDataVars *data, int clientSocketFd)
{
    fprintf(stderr, "[server] Client removed from the list.\n");
    pthread_mutex_lock(data->clientListMutex);
    for(int i = 0; i < MAX_BUFFER; i++)
    {
        if(data->clientSockets[i] == clientSocketFd)
        {
            data->clientSockets[i] = 0;
            close(clientSocketFd);
            data->numClients--;
            i = MAX_BUFFER;
        }
    }
    pthread_mutex_unlock(data->clientListMutex);
}

//Thread to handle new connections. Adds client's fd to list of client fds and spawns a new clientHandler thread for it
void *newClientHandler(void *data)
{
    chatDataVars *chatData = (chatDataVars *) data;
    while(1)
    {
        int clientSocketFd = accept(chatData->socketFd, NULL, NULL);
        if(clientSocketFd > 0)
        {
            fprintf(stderr, "\n  --> Server accepted new client. client__SocketFd: %d\n", clientSocketFd);

            //Obtain lock on clients list and add new client in
            pthread_mutex_lock(chatData->clientListMutex);
            if(chatData->numClients < MAX_BUFFER)
            {
                //Add new client to list
                for(int i = 0; i < MAX_BUFFER; i++)
                {
                    if(!FD_ISSET(chatData->clientSockets[i], &(chatData->serverReadFds)))
                    {
                        chatData->clientSockets[i] = clientSocketFd;
                        i = MAX_BUFFER;
                    }
                }

                FD_SET(clientSocketFd, &(chatData->serverReadFds));

                //Spawn new thread to handle client's messages
                clientHandlerVars chv;
                chv.clientSocketFd = clientSocketFd;
                chv.data = chatData;

                pthread_t clientThread;
                if((pthread_create(&clientThread, NULL, (void *)&clientHandler, (void *)&chv)) == 0)
                {
                    chatData->numClients++;
                    fprintf(stderr, "  --> Client has joined login menu. client__SocketFd: %d\n", clientSocketFd);
                }
                else
                    close(clientSocketFd);
            }
            pthread_mutex_unlock(chatData->clientListMutex);
        }
    }
}

//The "producer" -- Listens for messages from client to add to message queue
void *clientHandler(void *chv)
{
    clientHandlerVars *vars = (clientHandlerVars *)chv;
    chatDataVars *data = (chatDataVars *)vars->data;

   /* login_info *logInfo = (login_info *)vars->logInfo;

    register_info * regInfo = (register_info *)vars->regInfo;*/

    queue *q = data->queue;
    
    int clientSocketFd = vars->clientSocketFd;

    char msgBuffer[MAX_BUFFER];
    while(1)
    {
        int numBytesRead = read(clientSocketFd, msgBuffer, MAX_BUFFER - 1);
        msgBuffer[numBytesRead] = '\0';
        fprintf(stderr, "%s\n", msgBuffer);

        //If the client sent /exit\n, remove them from the client list and close their socket
        if(strcmp(msgBuffer, "/exit\n") == 0)
        {
            fprintf(stderr, "  --> Client on socket %d has disconnected.\n", clientSocketFd);
            removeClient(data, clientSocketFd);
            return NULL;
        }

        if(strcmp(msgBuffer, "/login\n") == 0)
        {
            fprintf(stderr, "\n[server] A client typed /login.\n");

            login_info logInfo;
            logInfo.clientSocketFd = clientSocketFd;


            pthread_t loginThread;
            memset(msgBuffer, 0, MAX_BUFFER);
            if((pthread_create(&loginThread, NULL, (void *)&client_login, (void *)&logInfo)) == 0)
            {
                fprintf(stderr, "\n[ important! ] Client is trying to login. Socket: %d\n", clientSocketFd);
            }
            fflush(stdout);
            pthread_join(loginThread, NULL);
        
        }

        if(strcmp(msgBuffer, "/register\n") == 0)
        {
            fprintf(stderr, "\n [server] A client typed /register\n");

            register_info regInfo;
            regInfo.clientSocketFd = clientSocketFd;


            pthread_t registerThread;
            if((pthread_create(&registerThread, NULL, (void *)&client_register, (void *)&regInfo)) == 0)
            {
                fprintf(stderr, "\n[ important! ] Client is trying to register. Socket: %d\n", clientSocketFd);
            }
            pthread_join(registerThread, NULL);


        }
        if(strcmp(msgBuffer, "/quizz\n") == 0)
        {

            fprintf(stderr, "  --> Client on socket typed /quizz.\n");

            h_questions questions;
            questions.clientSocketFd = clientSocketFd;

            //Start thread to handle /quizz received

            pthread_t qThread;   // threads ID's 

            if((pthread_create(&qThread, NULL, (void *)&quizzHandler, (void *)&questions)) == 0)
        {
            fprintf(stderr, "[server] Quizz handler started\n");
        }


        // pthread_join || Wait until thread is done its work 
        pthread_join(qThread, NULL); 

            
        }


       /* else 
        {
            //Wait for queue to not be full before pushing message
            while(q->full)
            {
                pthread_cond_wait(q->notFull, q->mutex);
            }

            //Obtain lock, push message to queue, unlock, set condition variable
            pthread_mutex_lock(q->mutex);
            fprintf(stderr, "Pushing message to queue: %s\n", msgBuffer);
            queuePush(q, msgBuffer);
            pthread_mutex_unlock(q->mutex);
            pthread_cond_signal(q->notEmpty);
        }*/
    }
}
void *client_register(void *regInfo)
{
    //clientHandlerVars *vars = (clientHandlerVars *)chv;
    register_info *ri = (register_info *) regInfo;
    char msgBuffer[MAX_BUFFER];
    int count = 4;
    fprintf(stderr, "\n[server] Client transfer to register function.\n");

    int socket = ri->clientSocketFd;

    if(socket !=0 && write(socket, "\nType [1:] <enter> [2:] <enter> [3:] <enter> [4:]\n\n",MAX_BUFFER -1) == -1)
        perror("Socket write failed: ");

    if(socket != 0 && write(socket, "1. [Nume]: \n", MAX_BUFFER - 1) == -1)
        perror("Socket write failed: ");
    if(socket != 0 && write(socket, "2. [Prenume]: \n", MAX_BUFFER - 1) == -1)
        perror("Socket write failed: ");
    if(socket != 0 && write(socket, "3. [username]: \n", MAX_BUFFER - 1) == -1)
        perror("Socket write failed: ");
    if(socket != 0 && write(socket, "4. [password]: \n", MAX_BUFFER - 1) == -1)
        perror("Socket write failed: ");

    while(count>0){

        int numBytesRead = read(ri->clientSocketFd, msgBuffer, MAX_BUFFER - 1);
            msgBuffer[numBytesRead] = '\0';
        fprintf(stderr, "%s*\n", msgBuffer);

        if(count == 4) {

            strcpy(ri->Name, msgBuffer);
        //printf("\n%s\n",li->username );
        }
        if(count == 3) {
            strcpy(ri->Surname, msgBuffer);
            //printf("\n%s\n",li->password );
        }
        if(count == 2) {
            strcpy(ri->username, msgBuffer);
        //printf("\n%s\n",li->username );
        }
        if(count == 1) {
            strcpy(ri->password, msgBuffer);
            //printf("\n%s\n",li->password );
        }
            count--;
    }
    memset(msgBuffer, 0, MAX_BUFFER);

    ri->Name[strlen(ri->Name)-1]='\0';
    ri->Surname[strlen(ri->Surname)-1]='\0';
    ri->username[strlen(ri->username)-1]='\0';
    ri->password[strlen(ri->password)-1]='\0';

    //fprintf(stderr,"[server] Success!\n",create_account(ri->Name,ri->Surname,ri->username,ri->password));
    fprintf(stderr,"%d\n",create_account(ri->Name,ri->Surname,ri->username,ri->password));
    pthread_exit(0);
}

void *client_login(void *logInfo)
{
    //clientHandlerVars *inter = (clientHandlerVars *)vars;

    login_info *li = (login_info *) logInfo;

    //chatDataVars *data = (chatDataVars *) data;

    char msgBuffer[MAX_BUFFER];
    int count = 3;

    //memset(username, 0, MAX_BUFFER); memset(password, 0, MAX_BUFFER);
    fprintf(stderr, "\n[server] Client transfer to login function.\n");
    printf("%d",li->clientSocketFd);

    //pthread_mutex_lock(li->login_mutex);

    int socket = li->clientSocketFd;
    if(socket !=0 && write(socket, "\nType [1:] <enter> [2:]\n\n",MAX_BUFFER -1) == -1)
        perror("Socket write failed: ");
    if(socket != 0 && write(socket, "1. [username]: \n", MAX_BUFFER - 1) == -1)
        perror("Socket write failed: ");
    if(socket != 0 && write(socket, "2. [password]: \n", MAX_BUFFER - 1) == -1)
        perror("Socket write failed: ");
    fflush(stdin);
    while(count>0){

        int numBytesRead = read(li->clientSocketFd, msgBuffer, MAX_BUFFER - 1);
            msgBuffer[numBytesRead] = '\0';
        //fprintf(stderr, "%s\n", msgBuffer);

        if(count == 2) {
            strcpy(li->username, msgBuffer);
            fprintf(stderr, "  --> text received:%s",li->username);
        //printf("\n%s\n",li->username );
        }
        if(count == 1) {
            strcpy(li->password, msgBuffer);
            fprintf(stderr, "  --> text received:%s",li->password);
            //printf("\n%s\n",li->password );
        }
            count--;
    }
    li->username[strlen(li->username)-1]='\0';
    li->password[strlen(li->password)-1]='\0';

    if(verrify_account(li->username,li->password)) 
    {
        if(socket != 0 && write(socket, "--> Login successfull! <--\n", MAX_BUFFER - 1) == -1)
        { perror("Socket write failed: ");}

        fprintf(stderr, "[client] userID: %d\n", utilizator_id);

        menu h_menu;
        h_menu.clientSocketFd = li->clientSocketFd;
        h_menu.user_ID = utilizator_id;


        pthread_t menuThread;
        if((pthread_create(&menuThread, NULL, (void *)&menu_handler, (void *)&h_menu) == 0))
        {
            fprintf(stderr, "\n[ important! ] Client is loged id. Socket: %d\n", li->clientSocketFd);
        }
        pthread_join(menuThread, NULL);
    }
    else
    {
        if(socket != 0 && write(socket, "--> Login not successfull! <--\n", MAX_BUFFER - 1) == -1) perror("Socket write failed: ");
        pthread_exit(0);
    }
    //pthread_mutex_unlock(li->login_mutex);
}

void *menu_handler(void *h_menu)
{
    menu *m = (menu *) h_menu;
    /*clientHandlerVars *vars = (clientHandlerVars *)chv;
    chatDataVars * dt = (chatDataVars *)vars->data;
    menu * m = (menu *)vars->menu;*/
    char u_ID[10];
    sprintf(u_ID,"%d",m->user_ID);

    fprintf(stderr, "\n[server] Client transfer to menu function.\n");
    //fprintf(stderr, "%c\n", u_ID);
    int socket = m->clientSocketFd;
    if(socket != 0 && write(socket,"\nYou are now in the chat section  ||  Please type one of the commands: \n", MAX_BUFFER -1) == -1)
        perror("Socket write failed: ");
    if(socket != 0 && write(socket, "\n=============\n1. /quizz\n2. /exit\n============",MAX_BUFFER -1) == -1)
        perror("Socket write failed: ");

    if(socket != 0 && write(socket,"\nyour user_ID is: ",MAX_BUFFER - 1) == -1)
        perror("Socket write failed: ");
    //strcat(user,to_char(m->user_ID));
    if(socket != 0 && write(socket,u_ID,sizeof(char)) == -1)
        perror("Socket write failed: ");

    if(socket != 0 && write(socket,"\n<-----------------------CHAT--------------------->\n\n",MAX_BUFFER - 1) == -1)
        perror("Socket write failed: ");

    /* ====================================================================================================== */
    /*while(1){
    int numBytesRead = read(m->clientSocketFd, msgBuffer, MAX_BUFFER - 1);
            msgBuffer[numBytesRead] = '\0';
    if(strcmp("/stop_chat\n",msgBuffer) == 0){

    while(count > 0)
    {
        int numBytesRead = read(m->clientSocketFd, msgBuffer, MAX_BUFFER - 1);
            msgBuffer[numBytesRead] = '\0';

        if(strcmp("/quizz\n",msgBuffer) == 0)
        {
            fprintf(stderr, "  --> Client on socket %d typed /quizz.\n", m->clientSocketFd);

            h_questions questions;
            questions.clientSocketFd = m->clientSocketFd;

            //Start thread to handle /quizz received

            pthread_t qThread;   // threads ID's 

            if((pthread_create(&qThread, NULL, (void *)&quizzHandler, (void *)&questions)) == 0)
        {
            fprintf(stderr, "[server] Quizz handler started\n");
        }


        // pthread_join || Wait until thread is done its work 
        pthread_join(qThread, NULL); 

        }
        if(strcmp("/exit\n",msgBuffer) == 0) pthread_exit(0);
        count--;
    }
    }*/
    pthread_exit(0);

    /*while(1)
    {
               
        }
    }*/
}
void *quizzHandler(void *questions)
{
    h_questions * que = (h_questions *) questions;

    const char* request;
    const char* answers;

    fprintf(stderr, "\n[server] Client with socket FD %d transfer to quizz function.\n",que->clientSocketFd);
    
    int socket = que->clientSocketFd;
    if(socket !=0 && write(socket, "\n[info] Your quizz will be generated!",MAX_BUFFER -1) == -1)
        perror("Socket write failed: ");

    int count = 4;
    while(count > 0)
    {
        count--;
        if(socket !=0 && write(socket, "\n...",MAX_BUFFER -1) == -1)
        perror("Socket write failed: ");
        waitFor(1);
    }
    write(socket, "\n",MAX_BUFFER -1);
    //fprintf(stderr, "%s\n", request);
    //strcpy(question,retrieve_questions());
    //fprintf(stderr, "%s\n",question );

    //fprintf(stderr, "%s\n", answers);
    answers = retrieve_answers();
    request = retrieve_questions();

    strcpy(que->Question,request);
    strcpy(que->answ_1,answers);

    if(socket !=0 && write(socket, que->Question,MAX_BUFFER - 1) == -1)
        perror("Socket write failed: ");

    if(socket !=0 && write(socket, "\nPlease press <enter> to see answers!\n",MAX_BUFFER -1) == -1)
        perror("Socket write failed: ");

    if(socket !=0 && write(socket, que->answ_1,MAX_BUFFER - 1) == -1)
        perror("Socket write failed: ");
    fflush(stdout);





    pthread_exit(0);
}

//The "consumer" -- waits for the queue to have messages then takes them out and broadcasts to clients
void *messageHandler(void *data)
{
    chatDataVars *chatData = (chatDataVars *)data;
    queue *q = chatData->queue;
    int *clientSockets = chatData->clientSockets;

    while(1)
    {
        //Obtain lock and pop message from queue when not empty
        pthread_mutex_lock(q->mutex);
        while(q->empty)
        {
            pthread_cond_wait(q->notEmpty, q->mutex);
        }
        char* msg = queuePop(q);
        pthread_mutex_unlock(q->mutex);
        pthread_cond_signal(q->notFull);

        //Broadcast message to all connected clients
        fprintf(stderr, "Broadcasting message: %s\n", msg);
        for(int i = 0; i < chatData->numClients; i++)
        {
            int socket = clientSockets[i];
            if(socket != 0 && write(socket, msg, MAX_BUFFER - 1) == -1)
                perror("Socket write failed: ");
        }
    }
}

bool initialize_database()
{
    int rc = sqlite3_open("base.db3", &db);
    if (rc != SQLITE_OK) 
    {
        
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 0;
    }
    else fprintf(stderr, "[server|db] Database successfully opened!\n");
return 1;
} 

bool verrify_account(char *username,char*password)
{
  fprintf(stderr, "%s*%s\n",username,password );
  int rc,step;
  char *sql="SELECT ID, USERNAME, PASSWORD FROM CLIENTS WHERE USERNAME = ? AND PASSWORD = ?";
  rc = sqlite3_prepare_v2(db, sql, -1, &res, 0);
  if (rc == SQLITE_OK) 
  {
        sqlite3_bind_text(res, 1, username,strlen(username),0);
        sqlite3_bind_text(res, 2, password,strlen(password),0);
  }
  
  while((step = sqlite3_step(res))!=SQLITE_DONE)
    {
       if(step == SQLITE_ROW) 
        {    
          utilizator_id = (*sqlite3_column_text(res,0)) - '0';
           return 1;   
        }
    }
    sqlite3_finalize(res);
    //sqlite3_close(db);
return 0;
}
bool create_account(char *surname,char *name,char *account,char *password)
{
    fprintf(stderr, "*%s*%s*%s*%s*\n",surname,name,account,password);

    int step;
    char *sql = "INSERT INTO CLIENTS(NUME,PRENUME,USERNAME,PASSWORD) VALUES (?,?,?,?)"; 
    int rc = sqlite3_prepare_v2(db, sql, -1, &res, 0);
    if (rc != SQLITE_OK) 
    {
        fprintf(stderr, "Failed to execute statement: %s\n", sqlite3_errmsg(db));
    }
    if (rc == SQLITE_OK) 
    {
    sqlite3_bind_text(res, 4, password,strlen(password),0);
    sqlite3_bind_text(res, 1, surname,strlen(surname),0);
    sqlite3_bind_text(res, 2, name,strlen(name),0);
    sqlite3_bind_text(res, 3, account,strlen(account),0);
    }
     while((step = sqlite3_step(res))==SQLITE_DONE)
     {
       fprintf(stderr, "Contul %s a fost creat cu succes!",account);
       return 1;
     }
    return 0;
}

const char *retrieve_questions() 
{
    static char *question[1024];
    sqlite3 *db;
    char *err_msg = 0;
    sqlite3_stmt *res;
    
    int rc = sqlite3_open("questions.db3", &db);
    
    if (rc != SQLITE_OK) {
        
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        
        return 1;
    }
    else fprintf(stderr, "Database successfully opened!\n\n");
    
    char *sql = "SELECT id, que FROM Questions WHERE Id = ?";
        
    rc = sqlite3_prepare_v2(db, sql, -1, &res, 0);
    
    if (rc == SQLITE_OK) {
        
        sqlite3_bind_int(res, 1, 1);
    } else {
        
        fprintf(stderr, "Failed to execute statement: %s\n", sqlite3_errmsg(db));
    }
    
    int step = sqlite3_step(res);
    
    if (step == SQLITE_ROW) {
        
       // printf("%s: ", sqlite3_column_text(res, 0));
        //fprintf(stderr, "%s\n", sqlite3_column_text(res, 1));
       strcat(question,sqlite3_column_text(res, 0));
       strcat(question,": ");
       strcat(question,sqlite3_column_text(res, 1));
       fprintf(stderr, "%s\n", question);
        
    } 
    //fprintf(stderr, "%s\n", question);
    sqlite3_finalize(res);
    sqlite3_close(db);
    
    return question;
}
const char *retrieve_answers() 
{
    static char *question[1024];
    sqlite3 *db;
    char *err_msg = 0;
    sqlite3_stmt *res;
    
    int rc = sqlite3_open("questions.db3", &db);
    
    if (rc != SQLITE_OK) {
        
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        
        return 1;
    }
    else fprintf(stderr, "Database successfully opened!\n\n");
    
    char *sql = "SELECT ID, ans1, ans2, ans3 FROM Questions WHERE Id = ?";
        
    rc = sqlite3_prepare_v2(db, sql, -1, &res, 0);
    
    if (rc == SQLITE_OK) {
        
        sqlite3_bind_int(res, 1, 1);
    } else {
        
        fprintf(stderr, "Failed to execute statement: %s\n", sqlite3_errmsg(db));
    }
    
    int step = sqlite3_step(res);
    
    if (step == SQLITE_ROW) {
        
       // printf("%s: ", sqlite3_column_text(res, 0));
        //fprintf(stderr, "%s\n", sqlite3_column_text(res, 1));
       strcat(question,"\n1. ");
       strcat(question,sqlite3_column_text(res, 1));
       strcat(question,"  2. ");
       strcat(question,sqlite3_column_text(res, 2));
       strcat(question,"  3. ");
       strcat(question,sqlite3_column_text(res, 3));
       fprintf(stderr, "%s\n", question);
        
    } 
    //fprintf(stderr, "%s\n", question);
    sqlite3_finalize(res);
    sqlite3_close(db);
    
    return question;
}