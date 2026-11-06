CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g

.PHONY: all clean

all: remoteshell server client

remoteshell: src/main.c src/shell.c src/shell.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) src/main.c src/shell.c -o $@ $(LDLIBS)

server: src/server.c src/shell.c src/net.c src/shell.h src/net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) src/server.c src/shell.c src/net.c -o $@ $(LDLIBS)

client: src/client.c src/net.c src/net.h src/shell.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) src/client.c src/net.c -o $@ $(LDLIBS)

clean:
	rm -f remoteshell server client

