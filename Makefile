CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g

.PHONY: all clean

all: remoteshell server client server_worker

remoteshell: src/main.c src/shell.c src/shell.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) src/main.c src/shell.c -o $@ $(LDLIBS)

server: src/server.c src/net.c src/scheduler.c src/net.h src/shell.h src/scheduler.h | server_worker
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(LDFLAGS) src/server.c src/net.c src/scheduler.c -o $@ $(LDLIBS)

server_worker: src/server_worker.c src/shell.c src/net.c src/shell.h src/net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) src/server_worker.c src/shell.c src/net.c -o $@ $(LDLIBS)

client: src/client.c src/net.c src/net.h src/shell.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) src/client.c src/net.c -o $@ $(LDLIBS)

# Optional CPU-work program for demonstrating round-robin scheduling.
cpu_job: tests/cpu_job.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/cpu_job.c -o $@ $(LDLIBS)

clean:
	rm -f remoteshell server client server_worker cpu_job

