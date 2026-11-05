#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Maximum input length is 1023 characters plus the null terminator. */
#define INPUT_SIZE 1024

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

        /*
         * Temporary output to verify input handling.
         * Parsing and command execution will replace this later.
         */
        printf("Received: %s\n", input);
    }

    return EXIT_SUCCESS;
}
