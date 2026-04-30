# Makefile for Railway Reservation System
# Compile with: make
# Run server:   ./server
# Run client:   ./client

CC = gcc
CFLAGS = -Wall -Wextra -g
LDFLAGS = -lpthread

all: server client

server: server.c
	$(CC) $(CFLAGS) -o server server.c $(LDFLAGS)

client: client.c
	$(CC) $(CFLAGS) -o client client.c

clean:
	rm -f server client server.log

.PHONY: all clean
