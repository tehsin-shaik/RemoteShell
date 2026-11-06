#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"
#include "shell.h"

/*
 * Receive and display the response to one command.
 *
 * The server sends output in chunks. Each chunk starts with its byte count.
 * A zero-length chunk marks the end of the command's output, followed by
 * a flag indicating whether the client session should end.
 *
 * Returns:
 *   0: The response is complete; accept another command.
 *   1: The server has ended this session.
 *  -1: A communication, protocol, or output error occurred.
 */
static int receive_response(int server)
{
    unsigned char buffer[OUTPUT_CHUNK_SIZE];

    for (;;) {
        /* Read the next chunk's length using the shared network helper. */
        uint32_t length;
        if (recv_u32(server, &length) != 1) {
            fprintf(stderr, "client: connection ended before response completed\n");
            return -1;
        }
        /* Zero length means there are no more output chunks. */
        if (length == 0) {
            uint32_t ended;
            /* Only 0 (continue) and 1 (end session) are valid flags. */
            if (recv_u32(server, &ended) != 1 || ended > 1) {
                fprintf(stderr, "client: invalid response ending\n");
                return -1;
            }
            /* Display any buffered output before accepting another command. */
            if (fflush(stdout) == EOF) {
                perror("stdout");
                return -1;
            }
            return (int)ended;
        }
        /*
         * Check the length before reading into the fixed-size buffer.
         * recv_exact handles output that arrives across multiple TCP reads.
         */
        if (length > sizeof(buffer) ||
            recv_exact(server, buffer, length) != 1) {
            fprintf(stderr, "client: invalid or incomplete output frame\n");
            return -1;
        }

        /*
         * Write exactly the received bytes.
         * Unlike printf("%s"), fwrite does not require a terminating '\0'
         * and can display output containing embedded null bytes.
         */
        if (fwrite(buffer, 1, length, stdout) != length) {
            perror("stdout");
            return -1;
        }
    }
}

int main(int argc, char *argv[])
{
    const char *server_ip = "127.0.0.1";
    uint16_t port = DEFAULT_PORT;

    if (argc > 3) {
        fprintf(stderr, "Usage: %s [server-ipv4] [port]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc >= 2)
        server_ip = argv[1];

    /* Validate an explicitly supplied port before attempting a connection. */
    if (argc == 3 && parse_port(argv[2], &port) == -1) {
        fprintf(stderr, "client: invalid port\n");
        return EXIT_FAILURE;
    }

    /* Build the server address, storing the port in network byte order. */
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    /* Convert the IPv4 address from text to its binary representation. */
    if (inet_pton(AF_INET, server_ip, &address.sin_addr) != 1) {
        fprintf(stderr, "client: invalid IPv4 address\n");
        return EXIT_FAILURE;
    }

    /* Create an IPv4 TCP socket. */
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /*
     * Mark the socket to close automatically if this process executes
     * another program, then establish the connection to the server.
     */
    if (set_cloexec(server) == -1 ||
        connect(server, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("connect");
        close(server);
        return EXIT_FAILURE;
    }

    /* Check if the input is coming from a terminal. */
    int interactive = isatty(STDIN_FILENO);
    int failed = 0;
    char input[INPUT_SIZE];

    for (;;) {
        if (interactive) {
            fputs("RemoteShell(remote)> ", stdout);
            /* The prompt has no newline, so display it immediately. */
            if (fflush(stdout) == EOF) {
                perror("stdout");
                failed = 1;
                break;
            }
        }
        /* Read one command, reserving space for the terminating '\0'. */
        if (fgets(input, sizeof(input), stdin) == NULL) {
            if (ferror(stdin)) {
                perror("stdin");
                failed = 1;
            } else if (interactive) {
                putchar('\n');
            }
            break;
        }
        /* Remove the newline before transmitting the command. */
        char *newline = strchr(input, '\n');
        if (newline != NULL) {
            *newline = '\0';
        } else {
            /*
             * If fgets did not read a newline, inspect the next character.
             * A newline or EOF means the line fits; any other character
             * means the input exceeds the buffer's limit.
             */
            int next = getchar();

            if (next != '\n' && next != EOF) {
                /* Discard the entire remainder; never send a partial command. */
                do {
                    next = getchar();
                } while (next != '\n' && next != EOF);

                if (ferror(stdin)) {
                    perror("stdin");
                    failed = 1;
                    break;
                }

                fprintf(stderr, "client: input exceeds 1023 characters\n");
                continue;
            }

            if (ferror(stdin)) {
                perror("stdin");
                failed = 1;
                break;
            }
        }

        /* Empty and whitespace-only input does not need a server request. */
        if (input[strspn(input, " \t\r")] == '\0')
            continue;

        /*
         * Send the command length followed by its bytes.
         * The terminating '\0' is not transmitted.
         */
        uint32_t length = (uint32_t)strlen(input);

        if (send_frame(server, input, length) == -1) {
            perror("send command");
            failed = 1;
            break;
        }

        /* Finish receiving this response before sending another command. */
        int result = receive_response(server);

        if (result != 0) {
            if (result < 0)
                failed = 1;

            break;
        }
    }

    /* Release the socket after normal disconnection or an error. */
    close(server);

    /* This status describes the client, not the remote command's exit status. */
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}