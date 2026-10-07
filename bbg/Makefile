CC = gcc
CFLAGS = -Wall -Wextra -O2 -I../common

all: process1 process2

process1: process1.c ../common/common.h
	$(CC) $(CFLAGS) -o $@ process1.c

process2: process2.c ../common/common.h
	$(CC) $(CFLAGS) -o $@ process2.c

clean:
	rm -f process1 process2
