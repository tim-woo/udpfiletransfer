CC=gcc
CFLAGS=-I.

all: c/client.c s/server.c
	$(CC) -o c/client c/client.c $(CFLAGS)
	$(CC) -o s/server s/server.c $(CFLAGS)

clean:
	rm c/client s/server