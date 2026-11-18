/*
 * Linux features are used for accept4 and atomic close-on-exec sockets.
 * POSIX threads manage client sessions; posix_spawn starts isolated workers.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "net.h"
#include "shell.h"
#include "scheduler.h"

extern char **environ;

#define MAX_CLIENTS 32

/*
 * Each thread owns this structure and its client socket.
 * worker_path is immutable after server startup and shared read-only.
 */
struct client_context {
    int client;
    int slot;
    unsigned long id;
    const char *worker_path;
};

/* The registry allows shutdown to wake all blocked client threads. */
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clients_changed = PTHREAD_COND_INITIALIZER;
static int client_sockets[MAX_CLIENTS];
static unsigned active_clients;
static int shutting_down;

static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        abort();
    }
}

static int reserve_slot(int client)
{
    check_pthread(pthread_mutex_lock(&clients_mutex), "client registry lock");
    int slot = -1;
    if (!shutting_down) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_sockets[i] == -1) {
                slot = i;
                client_sockets[i] = client;
                active_clients++;
                break;
            }
        }
    }
    check_pthread(pthread_mutex_unlock(&clients_mutex), "client registry unlock");
    return slot;
}

static void release_slot(int slot)
{
    check_pthread(pthread_mutex_lock(&clients_mutex), "client registry lock");
    /* Close and remove atomically so shutdown never targets a reused FD. */
    close(client_sockets[slot]);
    client_sockets[slot] = -1;
    active_clients--;
    check_pthread(pthread_cond_broadcast(&clients_changed), "client notification");
    check_pthread(pthread_mutex_unlock(&clients_mutex), "client registry unlock");
}

static int server_is_stopping(void)
{
    check_pthread(pthread_mutex_lock(&clients_mutex), "client registry lock");
    int result = shutting_down;
    check_pthread(pthread_mutex_unlock(&clients_mutex), "client registry unlock");
    return result;
}

static void shutdown_sessions(int listener)
{
    check_pthread(pthread_mutex_lock(&clients_mutex), "shutdown registry lock");
    shutting_down = 1;
    (void)shutdown(listener, SHUT_RDWR);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_sockets[i] != -1)
            (void)shutdown(client_sockets[i], SHUT_RDWR);
    }
    check_pthread(pthread_mutex_unlock(&clients_mutex), "shutdown registry unlock");
}

struct signal_context {
    int listener;
    sigset_t signals;
};

/* sigwait runs in a normal thread; no mutex/stdio work runs in a signal handler. */
static void *handle_shutdown(void *argument)
{
    struct signal_context *context = argument;
    int number;
    int error = sigwait(&context->signals, &number);
    if (error != 0)
        fprintf(stderr, "sigwait: %s\n", strerror(error));
    shutdown_sessions(context->listener);
    return NULL;
}

static int send_error(int client, const char *message, uint32_t ended)
{
    if (send_frame(client, message, (uint32_t)strlen(message)) == -1)
        return -1;
    return send_response_end(client, ended);
}

/*
 * Find the worker beside the server executable, independently of the
 * directory from which the operator launched ./server.
 */
static int find_worker(char path[PATH_MAX])
{
    ssize_t n = readlink("/proc/self/exe", path, PATH_MAX - 1);
    if (n < 0)
        return -1;
    if (n == PATH_MAX - 1) {
        errno = ENAMETOOLONG;
        return -1;
    }
    path[n] = '\0';

    char *slash = strrchr(path, '/');
    if (slash == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t prefix = (size_t)(slash - path) + 1;
    const char name[] = "server_worker";
    if (prefix + sizeof(name) > PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(path + prefix, name, sizeof(name));
    return 0;
}

/*
 * Launch a separate, single-threaded shell worker for this session.
 * socketpair provides a private two-way channel between the thread and worker.
 *
 * posix_spawn avoids running application code between fork and exec inside
 * this multithreaded server. The existing fork/exec engine runs only after
 * the separate worker executable has started.
 *
 * The worker also gets its own process group for failure cleanup.
 * Returns the server end of the channel, or -1 on failure.
 */
static int start_worker(const char *path, pid_t *pid)
{
    int channel[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channel) == -1)
        return -1;

    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error != 0) {
        close(channel[0]);
        close(channel[1]);
        errno = error;
        return -1;
    }

    posix_spawnattr_t attributes;
    error = posix_spawnattr_init(&attributes);
    if (error != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close(channel[0]);
        close(channel[1]);
        errno = error;
        return -1;
    }

    /* Close our channel end in the worker, then install its end at FD 3. */
    error = posix_spawn_file_actions_addclose(&actions, channel[0]);
    if (error == 0)
        error = posix_spawn_file_actions_adddup2(
            &actions, channel[1], WORKER_CHANNEL_FD);
    if (error == 0 && channel[1] != WORKER_CHANNEL_FD)
        error = posix_spawn_file_actions_addclose(&actions, channel[1]);

    if (error == 0)
        error = posix_spawnattr_setpgroup(&attributes, 0);
    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    if (error == 0)
        error = posix_spawnattr_setsigmask(&attributes, &empty_mask);
    if (error == 0)
        error = posix_spawnattr_setflags(
            &attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);

    if (error == 0) {
        char parent_pid[32];
        (void)snprintf(parent_pid, sizeof(parent_pid), "%ld", (long)getpid());
        char *worker_argv[] = {(char *)path, parent_pid, NULL};
        error = posix_spawn(pid, path, &actions, &attributes,
                            worker_argv, environ);
    }

    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(channel[1]);

    if (error != 0) {
        close(channel[0]);
        errno = error;
        return -1;
    }
    return channel[0];
}

/* Detect a client close even while its command is queued or running. */
static int wait_for_worker(int worker, int client)
{
    struct pollfd descriptors[2] = {
        {worker, POLLIN, 0},
        {client, POLLRDHUP, 0}
    };
    for (;;) {
        int result = poll(descriptors, 2, -1);
        if (result == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (descriptors[1].revents &
            (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) {
            errno = EPIPE;
            return -1;
        }
        if (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR))
            return 0;
        if (descriptors[0].revents & POLLNVAL)
            return -1;
    }
}

/*
 * Worker control events are consumed here, never forwarded as client output.
 * READY is acknowledged after admission; DONE is acknowledged after removal.
 */
static int relay_response(struct client_context *context, int worker,
                          struct scheduler_job *job)
{
    unsigned char buffer[OUTPUT_CHUNK_SIZE];
    int client = context->client;

    for (;;) {
        if (wait_for_worker(worker, client) == -1)
            return -1;
        uint32_t length;
        if (recv_u32(worker, &length) != 1)
            return -1;

        if (length == WORKER_JOB_READY || length == WORKER_JOB_DONE) {
            uint32_t group;
            if (recv_u32(worker, &group) != 1 || group <= 1 || group > INT_MAX)
                return -1;
            int result;
            if (length == WORKER_JOB_READY)
                result = scheduler_submit(job, (pid_t)group, context->id);
            else
                result = scheduler_finish(job, (pid_t)group);
            if (result == -1 || send_u32(worker, WORKER_EVENT_ACK) == -1)
                return -1;
            continue;
        }

        if (length == 0) {
            uint32_t ended;
            if (recv_u32(worker, &ended) != 1 || ended > 1)
                return -1;
            if (send_response_end(client, ended) == -1)
                return -1;
            return (int)ended;
        }

        if (length > sizeof(buffer) ||
            recv_exact(worker, buffer, length) != 1)
            return -1;
        if (send_frame(client, buffer, length) == -1)
            return -1;
    }
}

/*
 * Process one client's requests. Other client threads run independently.
 * Returns 0 for a normal exit/EOF, or -1 for a session failure.
 */
static int serve_requests(struct client_context *context, int worker,
                          struct scheduler_job *job)
{
    int client = context->client;
    for (;;) {
        uint32_t length;
        int result = recv_u32(client, &length);
        if (result == 0)
            return 0;
        if (result == -1)
            return -1;

        if (length >= INPUT_SIZE) {
            (void)send_error(client,
                             "server: command exceeds 1023 characters\n", 1);
            return -1;
        }

        char command[INPUT_SIZE];
        if (recv_exact(client, command, length) != 1)
            return -1;

        if (memchr(command, '\0', length) != NULL ||
            memchr(command, '\n', length) != NULL) {
            if (send_error(client, "server: invalid command frame\n", 0) == -1)
                return -1;
            continue;
        }

        /*
         * Forward command bytes using the existing Phase 2 protocol.
         * The worker validates and terminates its own command buffer.
         */
        if (send_frame(worker, command, length) == -1)
            return -1;

        result = relay_response(context, worker, job);
        if (result == 1)
            return 0;
        if (result == -1)
            return -1;
    }
}

static void wait_for_worker_exit(pid_t worker)
{
    while (waitpid(worker, NULL, 0) == -1) {
        if (errno == EINTR)
            continue;
        perror("waitpid worker");
        break;
    }
}

/*
 * One detached POSIX thread owns one complete client session.
 * No mutex is held while waiting for commands or running programs.
 */
static void *handle_client(void *argument)
{
    struct client_context *context = argument;
    struct scheduler_job job = {0};
    pid_t worker_pid;
    int worker = start_worker(context->worker_path, &worker_pid);

    if (worker == -1) {
        perror("start shell worker");
        (void)send_error(context->client,
                         "server: cannot start shell worker\n", 1);
    } else {
        printf("[client %lu] connected; worker PID %ld\n",
               context->id, (long)worker_pid);
        fflush(stdout);

        (void)serve_requests(context, worker, &job);

        /* Remove the job before freeing its thread-owned queue object. */
        scheduler_cancel(&job);
        close(worker);

        /*
         * Closing the private channel also releases a worker waiting for a
         * startup/completion ACK. It cleans up its children before exiting.
         */
        wait_for_worker_exit(worker_pid);
    }

    printf("[client %lu] disconnected\n", context->id);
    fflush(stdout);
    release_slot(context->slot);
    free(context);
    return NULL;
}

int main(int argc, char *argv[])
{
    const char *bind_ip = "127.0.0.1";
    uint16_t port = DEFAULT_PORT;
    unsigned quantum = DEFAULT_QUANTUM_MS;
    if (argc > 4) {
        fprintf(stderr, "Usage: %s [bind-ipv4] [port] [quantum-ms]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc >= 2)
        bind_ip = argv[1];
    if (argc >= 3 && parse_port(argv[2], &port) == -1) {
        fprintf(stderr, "server: invalid port\n");
        return EXIT_FAILURE;
    }
    if (argc == 4) {
        char *end;
        errno = 0;
        long value = strtol(argv[3], &end, 10);
        if (errno != 0 || end == argv[3] || *end != '\0' ||
            value < 10 || value > 5000) {
            fprintf(stderr, "server: quantum must be 10 to 5000 ms\n");
            return EXIT_FAILURE;
        }
        quantum = (unsigned)value;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip, &address.sin_addr) != 1) {
        fprintf(stderr, "server: invalid IPv4 address\n");
        return EXIT_FAILURE;
    }

    char worker_path[PATH_MAX];
    if (find_worker(worker_path) == -1 || access(worker_path, X_OK) == -1) {
        perror("locate server_worker; build all programs with make");
        return EXIT_FAILURE;
    }

    /* Block shutdown signals before creating any threads. */
    struct signal_context signals;
    sigemptyset(&signals.signals);
    sigaddset(&signals.signals, SIGINT);
    sigaddset(&signals.signals, SIGTERM);
    int error = pthread_sigmask(SIG_BLOCK, &signals.signals, NULL);
    if (error != 0) {
        fprintf(stderr, "block shutdown signals: %s\n", strerror(error));
        return EXIT_FAILURE;
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
        client_sockets[i] = -1;
    if (scheduler_start(quantum) == -1) {
        perror("start scheduler");
        return EXIT_FAILURE;
    }

    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int reuse = 1;
    if (listener == -1 ||
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) == -1 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1 ||
        listen(listener, MAX_CLIENTS) == -1) {
        perror("start server");
        if (listener != -1) close(listener);
        scheduler_stop();
        return EXIT_FAILURE;
    }
    signals.listener = listener;

    pthread_attr_t attributes;
    error = pthread_attr_init(&attributes);
    if (error != 0) {
        fprintf(stderr, "thread attributes: %s\n", strerror(error));
        close(listener);
        scheduler_stop();
        return EXIT_FAILURE;
    }
    error = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (error != 0) {
        fprintf(stderr, "detach attributes: %s\n", strerror(error));
        pthread_attr_destroy(&attributes);
        close(listener);
        scheduler_stop();
        return EXIT_FAILURE;
    }

    pthread_t shutdown_thread;
    error = pthread_create(&shutdown_thread, NULL, handle_shutdown, &signals);
    if (error != 0) {
        fprintf(stderr, "shutdown thread: %s\n", strerror(error));
        pthread_attr_destroy(&attributes);
        close(listener);
        scheduler_stop();
        return EXIT_FAILURE;
    }

    printf("RemoteShell threaded server listening on %s:%u (up to %d clients)\n",
           bind_ip, (unsigned)port, MAX_CLIENTS);
    printf("Round-robin scheduler: %u ms elapsed-time quantum\n", quantum);
    fflush(stdout);

    unsigned long next_id = 1;
    int failed = 0;
    for (;;) {
        int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (client == -1) {
            if (errno == EINTR)
                continue;
            if (!server_is_stopping()) {
                perror("accept");
                failed = 1;
            }
            break;
        }

        int slot = reserve_slot(client);
        if (slot < 0) {
            if (!server_is_stopping())
                (void)send_error(client, "server: client limit reached\n", 1);
            close(client);
            if (server_is_stopping())
                break;
            continue;
        }

        struct client_context *context = malloc(sizeof(*context));
        if (context == NULL) {
            perror("allocate client context");
            (void)send_error(client, "server: cannot allocate session\n", 1);
            release_slot(slot);
            continue;
        }
        context->client = client;
        context->slot = slot;
        context->id = next_id++;
        context->worker_path = worker_path;

        pthread_t thread;
        error = pthread_create(&thread, &attributes, handle_client, context);
        if (error != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            (void)send_error(client, "server: cannot start client thread\n", 1);
            release_slot(slot);
            free(context);
        }
    }

    shutdown_sessions(listener);
    error = pthread_kill(shutdown_thread, SIGTERM);
    if (error != 0 && error != ESRCH)
        check_pthread(error, "wake shutdown thread");
    check_pthread(pthread_join(shutdown_thread, NULL), "join shutdown thread");

    check_pthread(pthread_mutex_lock(&clients_mutex), "wait clients lock");
    while (active_clients != 0)
        check_pthread(pthread_cond_wait(&clients_changed, &clients_mutex),
                      "wait for clients");
    check_pthread(pthread_mutex_unlock(&clients_mutex), "wait clients unlock");

    scheduler_stop();
    pthread_attr_destroy(&attributes);
    close(listener);
    puts("Server stopped; client workers and scheduled jobs cleaned up.");
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
