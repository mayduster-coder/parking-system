
CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread -I../common

server: server.c ../common/common.h
	$(CC) $(CFLAGS) -o $@ server.c -lsqlite3

clean:
	rm -f server
