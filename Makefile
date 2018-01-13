#Makefile

all: client server

client: client.c
	gcc client.c -std=c99 -Wall -o client

server: server.c
	 gcc -O3 server.c -std=c99 -lsqlite3 -Wall -o server -lpthread -ldl

clean: rm -f *~ server client
