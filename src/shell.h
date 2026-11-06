#ifndef REMOTESHELL_SHELL_H
#define REMOTESHELL_SHELL_H

/* Space for up to 1023 input characters and the terminating '\0'. */
#define INPUT_SIZE 1024

/*
 * Parse and execute one command line using the shared shell engine.
 * Used by both the local shell and the remote server.
 *
 * input:
 *   A writable, null-terminated string with its trailing newline removed.
 *   Parsing modifies this string to separate commands and arguments.
 *
 * Returns:
 *   0: Continue the current shell session.
 *   1: End the current session after a valid "exit" command.
 *
 * The return value controls the session; it is not a program's exit status.
 * This function currently supports sequential use only, not concurrent threads.
 */
int shell_run_line(char *input);

#endif

