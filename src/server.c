#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"
#include "shell.h"

/*
 * Send an error message as a complete response.
 * end_session is 0 to continue the session, or 1 to close it.
 * Returns 0 on success, or -1 if sending fails.
 */
static int send_error(int client, const char *message, uint32_t end_session)
{
    if (send_frame(client, message, (uint32_t)strlen(message)) == -1)
        return -1;

    return send_response_end(client, end_session);
}

/*
 * Execute one command and return its output to the client.
 *
 * stdout and stderr are redirected to a temporary file during execution.
 * A file avoids the limited-capacity problem of an unread output pipe
 * while the shell engine waits for child processes.
 *
 * Output is sent after execution finishes, rather than streamed live.
 *
 * console[0] holds the original stdout descriptor.
 * console[1] holds the original stderr descriptor.
 *
 * Returns:
 *   0: Continue the client session.
 *   1: A valid exit command ended the session.
 *  -1: A capture or communication error occurred.
 */
static int execute_and_reply(int client, char *command, int console[2])
{
    /* The temporary file is automatically removed when closed. */
    FILE *capture = tmpfile();

    if (capture == NULL) {
        perror("tmpfile");
        return send_error(client, "server: cannot capture output\n", 0);
    }

    /*
     * Obtain the file descriptor for dup2.
     * Mark the original descriptor close-on-exec and flush existing stdio
     * output before changing where stdout and stderr point.
     */
    int capture_fd = fileno(capture);

    if (set_cloexec(capture_fd) == -1 || fflush(NULL) == EOF) {
        perror("prepare capture");
        fclose(capture);
        return -1;
    }

    /* Send both normal output and error messages to the capture file. */
    int setup_ok = 1;

    if (dup2(capture_fd, STDOUT_FILENO) == -1 ||
        dup2(capture_fd, STDERR_FILENO) == -1)
        setup_ok = 0;

    /*
     * Execute only if both redirections succeeded.
     * The engine returns 1 for exit instead of terminating the server.
     */
    int ended = 0;

    if (setup_ok)
        ended = shell_run_line(command);

    /*
     * Flush output generated through stdio before restoring the console.
     * Attempt both restorations even if execution setup failed.
     */
    int flush_failed = (fflush(NULL) == EOF);
    int restore_out = dup2(console[0], STDOUT_FILENO);
    int restore_err = dup2(console[1], STDERR_FILENO);

    if (!setup_ok || flush_failed || restore_out == -1 || restore_err == -1) {
        perror("capture or restore output");
        fclose(capture);
        return -1;
    }

    /* Return to the beginning of the file to read the captured output. */
    if (fseeko(capture, 0, SEEK_SET) == -1) {
        perror("rewind capture");
        fclose(capture);
        return -1;
    }

    /* Transfer output in fixed-size chunks rather than loading it all at once. */
    unsigned char buffer[OUTPUT_CHUNK_SIZE];
    size_t length;

    while ((length = fread(buffer, 1, sizeof(buffer), capture)) > 0) {
        if (send_frame(client, buffer, (uint32_t)length) == -1) {
            fclose(capture);
            return -1;
        }
    }

    if (ferror(capture)) {
        perror("read captured output");
        fclose(capture);
        return -1;
    }

    fclose(capture);

    /*
     * Explicitly finish the response, even if the command produced no output.
     * The client can then continue or disconnect according to ended.
     */
    if (send_response_end(client, (uint32_t)ended) == -1)
        return -1;

    return ended;
}

/*
 * Receive and execute commands from one connected client.
 * This function returns after exit, disconnection, or a session error.
 * Another client is served only after this session finishes.
 */
static void serve_client(int client, int console[2])
{
    for (;;) {
        /* Each request begins with a 32-bit command length. */
        uint32_t length;
        int result = recv_u32(client, &length);

        if (result == 0)
            return; /* Normal disconnection between requests. */

        if (result == -1) {
            perror("receive command length");
            return;
        }

        /*
         * Reserve one buffer position for the terminating '\0'.
         * Reject oversized requests and close the session rather than
         * attempting to receive an arbitrarily large command.
         */
        if (length >= INPUT_SIZE) {
            (void)send_error(
                client, "server: command exceeds 1023 characters\n", 1);
            return;
        }

        /* Receive exactly the number of command bytes specified. */
        char command[INPUT_SIZE];

        if (recv_exact(client, command, length) != 1) {
            fprintf(stderr, "server: incomplete command\n");
            return;
        }

        /*
         * A request must contain one command line, without embedded null
         * characters or newlines. The server adds its own string terminator.
         */
        if (memchr(command, '\0', length) != NULL ||
            memchr(command, '\n', length) != NULL) {
            if (send_error(
                    client, "server: invalid command frame\n", 0) == -1)
                return;

            continue;
        }

        command[length] = '\0';

        /* Execute the command and send its complete response. */
        result = execute_and_reply(client, command, console);

        if (result != 0)
            return; /* End the session on valid exit or execution-path failure. */
    }
}

int main(int argc, char *argv[])
{
    /* Listen on loopback by default, allowing local TCP testing. */
    const char *bind_ip = "127.0.0.1";
    uint16_t port = DEFAULT_PORT;

    /* Optional arguments: ./server [bind-ipv4] [port] */
    if (argc > 3) {
        fprintf(stderr, "Usage: %s [bind-ipv4] [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc >= 2)
        bind_ip = argv[1];

    if (argc == 3 && parse_port(argv[2], &port) == -1) {
        fprintf(stderr, "server: invalid port\n");
        return EXIT_FAILURE;
    }

    /* Prepare the IPv4 listening address and network-order port number. */
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, bind_ip, &address.sin_addr) != 1) {
        fprintf(stderr, "server: invalid IPv4 address\n");
        return EXIT_FAILURE;
    }

    /*
     * Save the starting directory so each new client begins there.
     * Keeping a directory descriptor allows restoration using fchdir.
     */
    int base_directory = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    /*
     * Save the server's original stdout and stderr for restoration after
     * command capture. Request descriptor numbers of at least 3 so the
     * saved copies do not occupy standard input, output, or error.
     *
     * Close-on-exec keeps these saved copies out of executed programs.
     */
    int console[2];
    console[0] = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    console[1] = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);

    if (base_directory == -1 || console[0] == -1 || console[1] == -1) {
        perror("save server state");

        if (base_directory != -1) close(base_directory);
        if (console[0] != -1) close(console[0]);
        if (console[1] != -1) close(console[1]);

        return EXIT_FAILURE;
    }

    /*
     * Remote commands receive EOF when reading standard input.
     * They cannot read the server operator's keyboard or protocol bytes.
     * This version therefore supports noninteractive remote commands.
     */
    int null_input = open("/dev/null", O_RDONLY | O_CLOEXEC);

    if (null_input == -1 || dup2(null_input, STDIN_FILENO) == -1) {
        perror("prepare command stdin");

        if (null_input != -1) close(null_input);

        close(base_directory);
        close(console[0]);
        close(console[1]);
        return EXIT_FAILURE;
    }

    /* Standard input now refers to /dev/null; close the extra descriptor. */
    if (null_input != STDIN_FILENO)
        close(null_input);

    /*
     * Create the TCP listening socket.
     * SO_REUSEADDR helps with restarting the server after a previous run.
     * The listen backlog allows connections to queue; sessions are still
     * processed one at a time.
     */
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;

    if (listener == -1 || set_cloexec(listener) == -1 ||
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) == -1 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1 ||
        listen(listener, 4) == -1) {
        perror("start server");

        if (listener != -1) close(listener);

        close(base_directory);
        close(console[0]);
        close(console[1]);
        return EXIT_FAILURE;
    }

    printf("RemoteShell server listening on %s:%u (one client at a time)\n",
           bind_ip, (unsigned)port);
    fflush(stdout);

    int failed = 0;

    for (;;) {
        /* Wait for a client and obtain its connected socket. */
        int client = accept(listener, NULL, NULL);

        if (client == -1) {
            if (errno == EINTR)
                continue;

            perror("accept");
            failed = 1;
            break;
        }

        /* Executed commands must not retain the connected client socket. */
        if (set_cloexec(client) == -1) {
            perror("client descriptor");
        } else {
            serve_client(client, console);
        }

        close(client);

        /*
         * cd persists during a session, but its effect must not carry over
         * to the next client. Restore the server's starting directory.
         */
        if (fchdir(base_directory) == -1) {
            perror("restore server directory");
            failed = 1;
            break;
        }
    }

    /* Release saved state and the listening socket before termination. */
    close(listener);
    close(base_directory);
    close(console[0]);
    close(console[1]);

    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}