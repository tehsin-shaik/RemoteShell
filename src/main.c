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
 * Execute two external commands connected by one pipe.
 * left and right are NULL-terminated argument arrays.
 */
static void execute_pipeline(char *left[], char *right[])
{
    int fd[2];
    pid_t children[2];

    if (pipe(fd) == -1) {
        perror("pipe");
        return;
    }

    /* Start BOTH children before waiting: pipes have limited capacity. */
    for (int i = 0; i < 2; i++) {
        children[i] = fork();

        if (children[i] == -1) {
            perror("fork");
            close(fd[0]);
            close(fd[1]);

            /* If the second fork failed, stop and reap the first child. */
            if (i == 1) {
                if (kill(children[0], SIGKILL) == -1 && errno != ESRCH)
                    perror("kill");

                while (waitpid(children[0], NULL, 0) == -1) {
                    if (errno != EINTR) {
                        perror("waitpid");
                        break;
                    }
                }
            }

            return;
        }

        if (children[i] == 0) {
            /*
             * First command: stdout -> pipe write end.
             * Second command: stdin <- pipe read end.
             */
            int source = (i == 0) ? fd[1] : fd[0];
            int target = (i == 0) ? STDOUT_FILENO : STDIN_FILENO;

            if (dup2(source, target) == -1) {
                perror("dup2");
                _exit(EXIT_FAILURE);
            }

            /* Close original descriptors; keep the redirected standard FD. */
            if (fd[0] != target)
                close(fd[0]);
            if (fd[1] != target)
                close(fd[1]);

            char **args = (i == 0) ? left : right;
            execvp(args[0], args);

            int saved_errno = errno;
            perror(args[0]);
            _exit(saved_errno == ENOENT ? 127 : 126);
        }
    }

    /*
     * The shell does not read or write pipeline data.
     * Closing its copies lets readers receive EOF when writers finish.
     */
    close(fd[0]);
    close(fd[1]);

    /* Reap both children, even if one command failed. */
    for (int i = 0; i < 2; i++) {
        while (waitpid(children[i], NULL, 0) == -1) {
            if (errno != EINTR) {
                perror("waitpid");
                break;
            }
        }
    }
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
        
        /* Detect a pipeline before strtok modifies the input. */
        char *separator = strchr(input, '|');

        if (separator != NULL) {
            /* This milestone supports exactly one pipe. */
            if (strchr(separator + 1, '|') != NULL) {
                fprintf(stderr,
                        "RemoteShell: only one pipe is supported yet\n");
                continue;
            }

            /* Split the input into two separate C strings. */
            *separator = '\0';

            char *left[MAX_ARGS + 1];
            char *right[MAX_ARGS + 1];

            /* Parse each side separately. */
            int left_count = parse_command(input, left, MAX_ARGS + 1);
            int right_count = parse_command(separator + 1, right,
                                            MAX_ARGS + 1);

            if (left_count < 0 || right_count < 0) {
                fprintf(stderr, "RemoteShell: too many arguments\n");
                continue;
            }

            if (left_count == 0 || right_count == 0) {
                fprintf(stderr,
                        "RemoteShell: missing command beside pipe\n");
                continue;
            }

            /* Reject built-ins inside pipelines for now. */
            if (strcmp(left[0], "cd") == 0 ||
                strcmp(left[0], "exit") == 0 ||
                strcmp(right[0], "cd") == 0 ||
                strcmp(right[0], "exit") == 0) {
                fprintf(stderr,
                        "RemoteShell: built-ins in pipelines are unsupported\n");
                continue;
            }

            execute_pipeline(left, right);
            continue;
        }

        /* Allow MAX_ARGS arguments plus the terminating NULL pointer. */
        char *argv[MAX_ARGS + 1];

        int argc = parse_command(input, argv, MAX_ARGS + 1);

        if (argc == -1) {
            fprintf(stderr,
                    "RemoteShell: too many arguments (maximum %d)\n",
                    MAX_ARGS);
            continue;
        }

        /* Built-ins run in the parent; external commands run in children. */
        if (argc > 0 && !handle_builtin(argc, argv)) {
            execute_command(argv);
        }
    }

    return EXIT_SUCCESS;
}