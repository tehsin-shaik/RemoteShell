#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>

#include "net.h"

/*
 * Mark a file descriptor to close automatically after a successful exec.
 * This prevents executed programs from retaining sockets or saved descriptors.
 *
 * Returns 0 on success, or -1 on failure.
 */
int set_cloexec(int fd)
{
    /* Read the existing descriptor flags before adding FD_CLOEXEC. */
    int flags = fcntl(fd, F_GETFD);

    if (flags == -1)
        return -1;

    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/*
 * Convert a decimal port argument into a 16-bit port number.
 * Reject conversion errors, missing digits, trailing characters,
 * and values outside the supported range of 1 to 65535.
 *
 * Store the result through port only when validation succeeds.
 * Returns 0 on success, or -1 on invalid input.
 */
int parse_port(const char *text, uint16_t *port)
{
    char *end;

    /* Clear errno so a conversion error can be detected. */
    errno = 0;
    long value = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' ||
        value < 1 || value > 65535)
        return -1;

    *port = (uint16_t)value;
    return 0;
}

/*
 * Send every requested byte.
 * A single send call may transfer fewer bytes than requested.
 *
 * Returns 0 when all bytes have been sent, or -1 on failure.
 */
int send_all(int fd, const void *data, size_t length)
{
    /* Use a byte pointer so offsets represent individual bytes. */
    const unsigned char *bytes = data;
    size_t sent = 0;

    while (sent < length) {
        /*
         * Send only the bytes still outstanding.
         * MSG_NOSIGNAL prevents SIGPIPE from terminating this process
         * if the peer has disconnected; send reports an error instead.
         */
        ssize_t n = send(fd, bytes + sent, length - sent, MSG_NOSIGNAL);

        if (n < 0) {
            /* Retry if a signal interrupted the operation. */
            if (errno == EINTR)
                continue;

            return -1;
        }

        /* Treat zero progress as a connection failure. */
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }

        sent += (size_t)n;
    }

    return 0;
}

/*
 * Receive exactly the requested number of bytes.
 * TCP may deliver those bytes across several recv calls.
 *
 * Returns:
 *   1: All requested bytes were received.
 *   0: The peer closed its sending side before any bytes were received.
 *  -1: A receive error occurred or the peer closed partway through the data.
 *
 * A request for zero bytes succeeds immediately.
 */
int recv_exact(int fd, void *data, size_t length)
{
    unsigned char *bytes = data;
    size_t received = 0;

    while (received < length) {
        /* Append incoming bytes after those already received. */
        ssize_t n = recv(fd, bytes + received, length - received, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (n == 0) {
            /* EOF before any data can indicate a normal disconnection. */
            if (received == 0)
                return 0;

            /* EOF midway through a field means the message is incomplete. */
            errno = ECONNRESET;
            return -1;
        }

        received += (size_t)n;
    }

    return 1;
}

/*
 * Send a 32-bit unsigned integer in network byte order.
 * Returns 0 on success, or -1 on failure.
 */
int send_u32(int fd, uint32_t value)
{
    uint32_t network_value = htonl(value);

    return send_all(fd, &network_value, sizeof(network_value));
}

/*
 * Receive a complete 32-bit integer and convert it to host byte order.
 * Update value only after the entire integer has been received.
 * Returns the same 1, 0, or -1 result as recv_exact.
 */
int recv_u32(int fd, uint32_t *value)
{
    uint32_t network_value;
    int result = recv_exact(fd, &network_value, sizeof(network_value));

    if (result == 1)
        *value = ntohl(network_value);

    return result;
}

/*
 * Send one frame: a 32-bit byte count followed by that many data bytes.
 * The count lets the receiver identify boundaries within TCP's byte stream.
 * A terminating '\0' is not added or transmitted automatically.
 *
 * Returns 0 on success, or -1 on failure.
 */
int send_frame(int fd, const void *data, uint32_t length)
{
    if (send_u32(fd, length) == -1)
        return -1;

    return send_all(fd, data, length);
}

/*
 * Mark the end of one command's response.
 * First send a zero-length output marker, then the session-control flag:
 *   0: The client may send another command.
 *   1: The current session should end.
 *
 * This flag is separate from the executed program's exit status.
 * Returns 0 on success, or -1 on failure.
 */
int send_response_end(int fd, uint32_t session_ended)
{
    if (send_u32(fd, 0) == -1)
        return -1;

    return send_u32(fd, session_ended);
}