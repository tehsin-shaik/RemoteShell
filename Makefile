CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g

.PHONY: all clean

# Default target: build the shell.
all: remoteshell

# Rebuild when the source file changes.
remoteshell: src/main.c
	$(CC) $(CFLAGS) src/main.c -o remoteshell

# Remove the generated executable.
clean:
	rm -f remoteshell
