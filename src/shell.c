#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#include "shell.h"

/* Limits include the program name and allow up to three pipes. */
#define MAX_ARGS 64
#define MAX_COMMANDS 4

/*
 * Split a command into arguments separated by spaces, tabs, or carriage returns.
 *
 * strtok modifies input by replacing separators with '\0'.
 * The argument pointers refer to this same buffer; no allocation is needed.
 *
 * capacity includes space for the final NULL pointer.
 * Returns the argument count, or -1 if the array would overflow.
 */
static int parse_command(char *input, char *argv[], size_t capacity)
{
    size_t count = 0;
    char *token = strtok(input, " \t\r");

    while (token != NULL) {
        /* Always reserve the last position for NULL. */
        if (count >= capacity - 1) {
            argv[count] = NULL;
            return -1;
        }

        argv[count++] = token;
        token = strtok(NULL, " \t\r");
    }

    /* execvp uses NULL to identify the end of the argument array. */
    argv[count] = NULL;
    return (int)count;
}

/*
 * Execute one external program in a child process.
 * The calling process waits, then continues running the shell or server.
 */
static void execute_command(char *argv[])
{
    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        /*
         * Replace the child with the requested program.
         * Names without a slash are searched for using PATH.
         */
        execvp(argv[0], argv);

        /* Successful execvp never returns; reaching here means failure. */
        int saved_errno = errno;
        perror(argv[0]);

        /*
         * End the child without flushing inherited stdio buffers.
         * Use 127 for ENOENT and 126 for other execution failures.
         */
        _exit(saved_errno == ENOENT ? 127 : 126);
    }

    /* Wait for this child and collect its termination status. */
    int status;

    while (waitpid(pid, &status, 0) == -1) {
        /* A signal may interrupt the wait; retry in that case. */
        if (errno == EINTR) {
            continue;
        }

        perror("waitpid");
        return;
    }
}

/*
 * Handle commands that must run in the calling shell process.
 *
 * Returns:
 *   0: This is not a built-in; try external execution.
 *   1: The built-in was handled, including any reported error.
 *   2: A valid exit command requested the end of this session.
 */
static int handle_builtin(int argc, char *argv[])
{
    if (strcmp(argv[0], "exit") == 0) {
        /* Numeric exit arguments are not supported in this version. */
        if (argc != 1) {
            fprintf(stderr, "Usage: exit\n");
            return 1;
        }

        /* Let the caller close the session without stopping the server. */
        return 2;
    }

    if (strcmp(argv[0], "cd") == 0) {
        if (argc > 2) {
            fprintf(stderr, "Usage: cd [directory]\n");
            return 1;
        }

        const char *directory;

        if (argc == 1) {
            /* Without a directory argument, use the user's home directory. */
            directory = getenv("HOME");

            if (directory == NULL || directory[0] == '\0') {
                fprintf(stderr, "cd: HOME is not set\n");
                return 1;
            }
        } else {
            directory = argv[1];
        }

        /*
         * Change the calling process's directory so later commands use it.
         * Performing this only in a child would not update the shell.
         */
        if (chdir(directory) == -1) {
            perror("cd");
        }

        return 1;
    }

    return 0;
}

/*
 * Separate the input into commands at each pipe character.
 * Parse each command's arguments before executing any part of the line.
 *
 * commands stores an argument array for each command.
 * arg_counts stores the corresponding argument counts.
 *
 * Returns the command count, or -1 for invalid input.
 * Quotes and escape sequences are not interpreted.
 */
static int parse_pipeline(char *input,
                          char *commands[][MAX_ARGS + 1],
                          int arg_counts[])
{
    int count = 0;

    for (;;) {
        if (count == MAX_COMMANDS) {
            fprintf(stderr, "RemoteShell: maximum 3 pipes supported\n");
            return -1;
        }

        /*
         * End the current command at the next pipe.
         * Empty sections remain detectable, unlike splitting on pipes
         * with strtok, which would skip consecutive delimiters.
         */
        char *separator = strchr(input, '|');

        if (separator != NULL)
            *separator = '\0';

        int argc = parse_command(input, commands[count], MAX_ARGS + 1);

        if (argc < 0) {
            fprintf(stderr, "RemoteShell: too many arguments\n");
            return -1;
        }

        if (argc == 0) {
            fprintf(stderr, "RemoteShell: missing command beside pipe\n");
            return -1;
        }

        arg_counts[count++] = argc;

        /* No further pipe means the entire line has been parsed. */
        if (separator == NULL)
            return count;

        input = separator + 1;
    }
}

/* Close the read and write descriptors of each created pipe. */
static void close_pipes(int pipes[][2], int count)
{
    for (int i = 0; i < count; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

/*
 * Wait for each listed child to finish.
 * Passing NULL discards the status value while still completing
 * child-process cleanup. Interrupted waits are retried.
 */
static void wait_for_children(pid_t children[], int count)
{
    for (int i = 0; i < count; i++) {
        while (waitpid(children[i], NULL, 0) == -1) {
            if (errno != EINTR) {
                perror("waitpid");
                break;
            }
        }
    }
}

/*
 * Execute two to four commands connected by pipes.
 * Each command runs in its own child process.
 *
 * Command i:
 *   Reads from pipe i-1, unless it is the first command.
 *   Writes to pipe i, unless it is the last command.
 */
static void execute_pipeline(char *commands[][MAX_ARGS + 1], int count)
{
    int pipes[MAX_COMMANDS - 1][2];
    pid_t children[MAX_COMMANDS];
    int pipe_count = count - 1;

    /* N commands need N-1 pipes. Create them before starting children. */
    for (int i = 0; i < pipe_count; i++) {
        if (pipe(pipes[i]) == -1) {
            perror("pipe");

            /* Close only the pipes successfully created before this failure. */
            close_pipes(pipes, i);
            return;
        }
    }

    /*
     * Launch every command before waiting.
     * Producers and consumers must run together because pipes have
     * limited capacity.
     */
    for (int i = 0; i < count; i++) {
        children[i] = fork();

        if (children[i] == -1) {
            perror("fork");
            close_pipes(pipes, pipe_count);

            /* Cancel the commands already started for this incomplete setup. */
            for (int j = 0; j < i; j++) {
                if (kill(children[j], SIGKILL) == -1 && errno != ESRCH)
                    perror("kill");
            }

            wait_for_children(children, i);
            return;
        }

        if (children[i] == 0) {
            /* Read the preceding command's output through standard input. */
            if (i > 0 &&
                dup2(pipes[i - 1][0], STDIN_FILENO) == -1) {
                perror("dup2 stdin");
                _exit(EXIT_FAILURE);
            }

            /* Send standard output to the following command. */
            if (i < count - 1 &&
                dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                perror("dup2 stdout");
                _exit(EXIT_FAILURE);
            }

            /*
             * stdin/stdout retain the redirected connections.
             * Close all original pipe descriptors, including unused copies,
             * so readers can detect EOF when the writers finish.
             */
            close_pipes(pipes, pipe_count);

            execvp(commands[i][0], commands[i]);

            /* Report execution failure and terminate only this child. */
            int saved_errno = errno;
            perror(commands[i][0]);
            _exit(saved_errno == ENOENT ? 127 : 126);
        }
    }

    /* The parent does not transfer pipe data; close its copies before waiting. */
    close_pipes(pipes, pipe_count);
    wait_for_children(children, count);
}

/*
 * Shared entry point used by the local interface and remote server.
 *
 * input must be writable and have its trailing newline removed.
 * Parsing modifies the buffer. Output goes to stdout and stderr.
 *
 * Returns 1 to end the session, or 0 to continue.
 * This return value does not represent an external program's exit status.
 */
int shell_run_line(char *input)
{
    /* Empty or whitespace-only input requires no execution. */
    if (input[strspn(input, " \t\r")] == '\0')
        return 0;

    char *commands[MAX_COMMANDS][MAX_ARGS + 1];
    int arg_counts[MAX_COMMANDS];

    /* Validate the entire line before launching any command. */
    int count = parse_pipeline(input, commands, arg_counts);

    if (count < 0)
        return 0;

    if (count == 1) {
        /* Built-ins run in the calling process; other commands use a child. */
        int builtin = handle_builtin(arg_counts[0], commands[0]);

        if (builtin == 2)
            return 1;

        if (builtin == 0)
            execute_command(commands[0]);

        return 0;
    }

    /* Reject built-ins inside pipes before starting any child processes. */
    for (int i = 0; i < count; i++) {
        if (strcmp(commands[i][0], "cd") == 0 ||
            strcmp(commands[i][0], "exit") == 0) {
            fprintf(stderr,
                    "RemoteShell: built-ins in pipelines are unsupported\n");
            return 0;
        }
    }

    execute_pipeline(commands, count);
    return 0;
}