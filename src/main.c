#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"

/*
 * Run the local shell interface.
 * This file handles the prompt and input; shell.c parses and executes commands.
 * No network connection is needed to use this version.
 */
int main(void)
{
    /* Fixed-size buffer shared with the shell engine through shell.h. */
    char input[INPUT_SIZE];

    while (1) {
        printf("RemoteShell> ");

        /* Display the prompt immediately because it has no newline. */
        if (fflush(stdout) == EOF) {
            perror("stdout");
            return EXIT_FAILURE;
        }

        /* Read one line, reserving space for the terminating '\0'. */
        if (fgets(input, sizeof(input), stdin) == NULL) {
            /* An input error is a failure; EOF is normal termination. */
            if (ferror(stdin)) {
                perror("stdin");
                return EXIT_FAILURE;
            }

            /* Ctrl+D at an empty terminal prompt normally produces EOF. */
            printf("\n");
            break;
        }

        /* Remove the newline before passing the command to the parser. */
        char *newline = strchr(input, '\n');

        if (newline != NULL) {
            *newline = '\0';
        } else {
            /*
             * No newline was stored. Inspect the next character to distinguish
             * a line that fits exactly from one that exceeds the limit.
             * Use int because getchar can return a character or EOF.
             */
            int next = getchar();

            if (next != '\n' && next != EOF) {
                /*
                 * Discard the rest of an oversized line so its remaining
                 * characters cannot become another command.
                 */
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

            /* A newline or EOF is acceptable unless a read error occurred. */
            if (ferror(stdin)) {
                perror("stdin");
                return EXIT_FAILURE;
            }
        }

        /* Skip lines containing no command, including whitespace-only lines. */
        if (input[strspn(input, " \t\r")] == '\0') {
            continue;
        }

        /*
         * Let the shared engine parse and execute the command.
         * A return value of 1 means a valid "exit" command ended the session.
         */
        if (shell_run_line(input))
            break;
    }

    return EXIT_SUCCESS;
}