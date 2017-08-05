server: server.c
	gcc server.c -lpthread -Wall -g -o server -rdynamic
client: client.c
	gcc client.c -lpthread -Wall -g -o client