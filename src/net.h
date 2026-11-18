#ifndef REMOTESHELL_NET_H
#define REMOTESHELL_NET_H

#include <stddef.h>  /* size_t */
#include <stdint.h>  /* uint16_t and uint32_t */

/* TCP port used when no port argument is provided. */
#define DEFAULT_PORT 5555

/* Private socket descriptor installed in each spawned shell worker. */
#define WORKER_CHANNEL_FD 3

/* Private worker events; these values never appear as client output lengths. */
#define WORKER_JOB_READY UINT32_MAX
#define WORKER_JOB_DONE (UINT32_MAX - 1U)
#define WORKER_EVENT_ACK 1U

/* Maximum number of output bytes sent in one response chunk. */
#define OUTPUT_CHUNK_SIZE 4096

/*
 * Close this descriptor automatically after a successful exec.
 * Prevents executed programs from retaining sockets or saved descriptors.
 * Returns 0 on success, or -1 on failure.
 */
int set_cloexec(int fd);

/*
 * Convert a decimal string to a port number between 1 and 65535.
 * Store the result through port.
 * Returns 0 on success, or -1 if validation fails.
 */
int parse_port(const char *text, uint16_t *port);

/*
 * Send all length bytes, retrying partial sends and interrupted calls.
 * Returns 0 on success, or -1 on failure.
 */
int send_all(int fd, const void *data, size_t length);

/*
 * Receive exactly length bytes, using multiple reads if necessary.
 * Returns:
 *   1: All requested bytes were received.
 *   0: EOF occurred before any bytes were received.
 *  -1: An error occurred or EOF interrupted an incomplete transfer.
 * A zero-byte request returns 1 immediately.
 */
int recv_exact(int fd, void *data, size_t length);

/*
 * Send a 32-bit unsigned integer in network byte order.
 * Returns 0 on success, or -1 on failure.
 */
int send_u32(int fd, uint32_t value);

/*
 * Receive a 32-bit unsigned integer and convert it to host byte order.
 * Returns 1 on success, 0 on EOF before any bytes, or -1 on failure.
 */
int recv_u32(int fd, uint32_t *value);

/*
 * Send a frame containing a 32-bit length followed by the data bytes.
 * Used for client commands and server output chunks.
 * Returns 0 on success, or -1 on failure.
 */
int send_frame(int fd, const void *data, uint32_t length);

/*
 * Finish a response by sending a zero-length output marker,
 * followed by a session-control flag:
 *   0: Continue accepting commands.
 *   1: End this client session.
 * Returns 0 on success, or -1 on failure.
 */
int send_response_end(int fd, uint32_t session_ended);

#endif /* REMOTESHELL_NET_H */
