# Makefile for echo client/server using csapp.c

CC = gcc
CFLAGS = -Wall -g
OBJS = csapp.o

all: echoclient echoserveri

echoclient: echoclient.o $(OBJS)
	$(CC) $(CFLAGS) -o echoclient echoclient.o $(OBJS)

echoserveri: echoserveri.o $(OBJS)
	$(CC) $(CFLAGS) -o echoserveri echoserveri.o $(OBJS)

csapp.o: csapp.c csapp.h
	$(CC) $(CFLAGS) -c csapp.c

echoclient.o: echoclient.c csapp.h
	$(CC) $(CFLAGS) -c echoclient.c

echoserveri.o: echoserveri.c csapp.h
	$(CC) $(CFLAGS) -c echoserveri.c

clean:
	rm -f *.o echoclient echoserveri
