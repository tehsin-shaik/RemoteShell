#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

	/* Allow MAX_ARGS arguments plus the terminating NULL pointer. */
	char *argv[MAX_ARGS + 1];

	int argc = parse_command(input, argv, MAX_ARGS + 1);

	if (argc == -1) {
	    fprintf(stderr, "RemoteShell: too many arguments (maximum %d)\n",
	            MAX_ARGS);
	    continue;
	}

	/* Execute only when parsing produced a command. */
	if (argc > 0) {
		execute_command(argv);
	}
    }

    return EXIT_SUCCESS;
}
