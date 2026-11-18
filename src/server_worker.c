#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"
#include "shell.h"

/* Private scheduler-control messages share this worker's socket channel. */
static int control_failed;

static int notify_scheduler(void *context, uint32_t event, pid_t pgid)
{
    int channel = *(int *)context;
    uint32_t acknowledgement;
    if (send_u32(channel, event) == -1 ||
        send_u32(channel, (uint32_t)pgid) == -1) {
        control_failed = 1;
        return -1;
    }
    if (recv_u32(channel, &acknowledgement) != 1 ||
        acknowledgement != WORKER_EVENT_ACK) {
        control_failed = 1;
        errno = EPIPE;
        return -1;
    }
    return 0;
}

static int job_ready(void *context, pid_t pgid)
{
    return notify_scheduler(context, WORKER_JOB_READY, pgid);
}

static int job_finished(void *context, pid_t pgid)
{
    return notify_scheduler(context, WORKER_JOB_DONE, pgid);
}

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

    if (control_failed) {
        fclose(capture);
        return -1;
    }

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
 * This process belongs to one client; other clients have independent workers.
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

/*
 * This executable is launched internally by the threaded server.
 * Its private command/response socket is installed at WORKER_CHANNEL_FD.
 * It has no TCP listener and never accepts clients itself.
 */
int main(int argc, char *argv[])
{
    /* The server supplies its PID so even a startup-time parent loss is detected. */
    if (argc != 2) {
        fputs("server_worker is started internally by ./server\n", stderr);
        return EXIT_FAILURE;
    }
    char *end;
    errno = 0;
    long parent = strtol(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0' ||
        parent <= 1 || parent > INT_MAX ||
        prctl(PR_SET_PDEATHSIG, (unsigned long)SIGKILL, 0UL, 0UL, 0UL) == -1 ||
        getppid() != (pid_t)parent) {
        fputs("worker: invalid or unavailable server parent\n", stderr);
        return EXIT_FAILURE;
    }

    if (set_cloexec(WORKER_CHANNEL_FD) == -1) {
        perror("worker channel");
        return EXIT_FAILURE;
    }

    /* Save output descriptors only inside this single-threaded worker. */
    int console[2];
    console[0] = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 4);
    console[1] = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 4);
    if (console[0] == -1 || console[1] == -1) {
        perror("save worker output");
        if (console[0] != -1) close(console[0]);
        if (console[1] != -1) close(console[1]);
        close(WORKER_CHANNEL_FD);
        return EXIT_FAILURE;
    }

    /* Remote programs cannot read the server operator's keyboard. */
    int null_input = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null_input == -1 || dup2(null_input, STDIN_FILENO) == -1) {
        perror("worker stdin");
        if (null_input != -1) close(null_input);
        close(console[0]);
        close(console[1]);
        close(WORKER_CHANNEL_FD);
        return EXIT_FAILURE;
    }
    if (null_input != STDIN_FILENO)
        close(null_input);

    int channel = WORKER_CHANNEL_FD;
    struct shell_job_hooks hooks = {job_ready, job_finished, &channel, channel};
    shell_set_job_hooks(&hooks);

    /* cd changes only this worker's directory for the current client. */
    serve_client(WORKER_CHANNEL_FD, console);

    close(WORKER_CHANNEL_FD);
    close(console[0]);
    close(console[1]);
    return EXIT_SUCCESS;
}

