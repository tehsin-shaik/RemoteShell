#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

/* Maximum input length is 1023 characters plus the null terminator. */
#define INPUT_SIZE 1024

#define MAX_ARGS 64
#define MAX_COMMANDS 4

/*
 * Split input in place into whitespace-separated arguments.
 * argv points into input, so no memory allocation is needed.
 * Returns the argument count, or -1 if there are too many.
 */
static int parse_command(char *input, char *argv[], size_t capacity)
{
    size_t count = 0;
    char *token = strtok(input, " \t\r");

    while (token != NULL) {
        /* Reserve the final array entry for NULL. */
        if (count >= capacity - 1) {
            argv[count] = NULL;
            return -1;
        }

        argv[count++] = token;
        token = strtok(NULL, " \t\r");
    }

    /* Required by execvp when we add execution next. */
    argv[count] = NULL;
    return (int)count;
}

/*
 * Run one external command.
 * The child executes the program; the parent waits and stays alive.
 */
static void execute_command(char *argv[])
{
    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        /* Search PATH and replace this child with the requested program. */
        execvp(argv[0], argv);

        /* Successful execvp never returns. */
        int saved_errno = errno;
        perror(argv[0]);

        /* Avoid flushing copies of the parent's stdio buffers. */
        _exit(saved_errno == ENOENT ? 127 : 126);
    }

    /* Reap this specific child to prevent a zombie process. */
    int status;

    while (waitpid(pid, &status, 0) == -1) {
        /* Retry if a signal interrupted the wait. */
        if (errno == EINTR) {
            continue;
        }

        perror("waitpid");
        return;
    }
}

/*
 * Handle built-ins in the shell process.
 * Return 1 if handled, or 0 if this is an external command.
 */
static int handle_builtin(int argc, char *argv[])
{
    if (strcmp(argv[0], "exit") == 0) {
        /* This version supports exit without arguments. */
        if (argc != 1) {
            fprintf(stderr, "Usage: exit\n");
            return 1;
        }

        exit(EXIT_SUCCESS);
    }

    if (strcmp(argv[0], "cd") == 0) {
        if (argc > 2) {
            fprintf(stderr, "Usage: cd [directory]\n");
            return 1;
        }

        /* With no argument, cd uses the user's home directory. */
        const char *directory;

        if (argc == 1) {
            directory = getenv("HOME");

            if (directory == NULL || directory[0] == '\0') {
                fprintf(stderr, "cd: HOME is not set\n");
                return 1;
            }
        } else {
            directory = argv[1];
        }

        /* Change this shell's current working directory. */
        if (chdir(directory) == -1) {
            perror("cd");
        }

        return 1;
    }

    return 0;
}

/*
 * Split a line into commands, then parse each command's arguments.
 * Return the command count, or -1 for invalid input.
 * Quotes and escapes are still unsupported.
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

        /* Split at the next pipe, preserving empty stages for validation. */
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

        if (separator == NULL)
            return count;

        /* Continue with the text after this pipe. */
        input = separator + 1;
    }
}

/* Close both ends of every successfully created pipe. */
static void close_pipes(int pipes[][2], int count)
{
    for (int i = 0; i < count; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

/* Reap every child, retrying waits interrupted by signals. */
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
 * Run two to four external commands.
 * Command i reads from pipe i-1 and writes to pipe i,
 * except at the first and last stages.
 */
static void execute_pipeline(char *commands[][MAX_ARGS + 1], int count)
{
    int pipes[MAX_COMMANDS - 1][2];
    pid_t children[MAX_COMMANDS];
    int pipe_count = count - 1;

    /* Create all pipes before starting children. */
    for (int i = 0; i < pipe_count; i++) {
        if (pipe(pipes[i]) == -1) {
            perror("pipe");

            /* Only the first i pipes were successfully created. */
            close_pipes(pipes, i);
            return;
        }
    }

    /* Start every command before waiting for any command. */
    for (int i = 0; i < count; i++) {
        children[i] = fork();

        if (children[i] == -1) {
            perror("fork");
            close_pipes(pipes, pipe_count);

            /* Stop any children already started after partial failure. */
            for (int j = 0; j < i; j++) {
                if (kill(children[j], SIGKILL) == -1 && errno != ESRCH)
                    perror("kill");
            }

            wait_for_children(children, i);
            return;
        }

        if (children[i] == 0) {
            /* Every command except the first reads the previous pipe. */
            if (i > 0 &&
                dup2(pipes[i - 1][0], STDIN_FILENO) == -1) {
                perror("dup2 stdin");
                _exit(EXIT_FAILURE);
            }

            /* Every command except the last writes to the next pipe. */
            if (i < count - 1 &&
                dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                perror("dup2 stdout");
                _exit(EXIT_FAILURE);
            }

            /*
             * Redirected stdin/stdout remain open after these originals
             * close. Unused copies must close so EOF works correctly.
             */
            close_pipes(pipes, pipe_count);

            execvp(commands[i][0], commands[i]);

            int saved_errno = errno;
            perror(commands[i][0]);
            _exit(saved_errno == ENOENT ? 127 : 126);
        }
    }

    /* The parent owns no pipeline data connections while waiting. */
    close_pipes(pipes, pipe_count);
    wait_for_children(children, count);
}

int main(void)
{
    char input[INPUT_SIZE];

    while (1) {
        /* Display the prompt before waiting for input. */
        printf("RemoteShell> ");

        /* The prompt has no newline, so flush it immediately. */
        if (fflush(stdout) == EOF) {
            perror("stdout");
            return EXIT_FAILURE;
        }

        /* Read one line, leaving space for the null terminator. */
        if (fgets(input, sizeof(input), stdin) == NULL) {
            if (ferror(stdin)) {
                perror("stdin");
                return EXIT_FAILURE;
            }

            /* EOF: Ctrl+D at an empty prompt ends the shell. */
            printf("\n");
            break;
        }

        /* Find the newline that fgets may have stored. */
        char *newline = strchr(input, '\n');

        if (newline != NULL) {
            *newline = '\0';
        } else {
            /*
             * Check whether the line fits exactly or is too long.
             * getchar returns int so it can represent EOF as well.
             */
            int next = getchar();

            if (next != '\n' && next != EOF) {
                /* Discard the remainder of an oversized line. */
                do {
                    next = getchar();
                } while (next != '\n' && next != EOF);

                if (ferror(stdin)) {
                    perror("stdin");
                    return EXIT_FAILURE;
                }

                fprintf(stderr,
                        "RemoteShell: input exceeds %d characters\n",
                        INPUT_SIZE - 1);
                continue;
            }

            if (ferror(stdin)) {
                perror("stdin");
                return EXIT_FAILURE;
            }
        }

        /* Ignore empty lines and lines containing only whitespace. */
        if (input[strspn(input, " \t\r")] == '\0') {
            continue;
        }

        char *commands[MAX_COMMANDS][MAX_ARGS + 1];
        int arg_counts[MAX_COMMANDS];

        /* Validate the whole line before executing anything. */
        int count = parse_pipeline(input, commands, arg_counts);

        if (count < 0)
            continue;

        if (count == 1) {
            /* Keep the existing single-command and built-in behavior. */
            if (!handle_builtin(arg_counts[0], commands[0]))
                execute_command(commands[0]);

            continue;
        }

        /* Built-ins in pipelines remain unsupported. */
        int contains_builtin = 0;

        for (int i = 0; i < count; i++) {
            if (strcmp(commands[i][0], "cd") == 0 ||
                strcmp(commands[i][0], "exit") == 0) {
                contains_builtin = 1;
                break;
            }
        }

        if (contains_builtin) {
            fprintf(stderr,
                    "RemoteShell: built-ins in pipelines are unsupported\n");
            continue;
        }

        execute_pipeline(commands, count);
    }

    return EXIT_SUCCESS;
}