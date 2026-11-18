#ifndef REMOTESHELL_SHELL_H
#define REMOTESHELL_SHELL_H

#include <sys/types.h>

#define INPUT_SIZE 1024

/*
 * Optional hooks used only inside an isolated, single-threaded worker.
 * ready is called after all child stages are stopped in a new process group.
 * finished is called after all stages exit, before their PIDs are released.
 * Both return 0 on acknowledgement, or -1 if the server channel has failed.
 */
struct shell_job_hooks {
    int (*ready)(void *context, pid_t pgid);
    int (*finished)(void *context, pid_t pgid);
    void *context;
    int control_fd; /* Child programs must not retain the private IPC socket. */
};

/* Passing NULL selects normal local execution without time slicing. */
void shell_set_job_hooks(const struct shell_job_hooks *hooks);

/*
 * Parse/execute a writable line with its trailing newline removed.
 * Returns 1 for a valid exit request, or 0 to continue the session.
 * The return value controls the session, not a program's exit status.
 * This engine runs sequentially in one process; each client has its own worker.
 */
int shell_run_line(char *input);

#endif

