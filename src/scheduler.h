#ifndef REMOTESHELL_SCHEDULER_H
#define REMOTESHELL_SCHEDULER_H

#include <sys/types.h>

#define DEFAULT_QUANTUM_MS 200

/*
 * One external command or entire pipeline is one scheduled job.
 * The owning client thread keeps this object alive until finish/cancel returns.
 * Its fields are managed by scheduler.c under the scheduler mutex.
 */
enum job_state { JOB_IDLE, JOB_READY, JOB_RUNNING };

struct scheduler_job {
    pid_t pgid;
    unsigned long client_id;
    enum job_state state;
    struct scheduler_job *next;
};

/* Start the scheduler thread. Returns 0, or -1 with errno set. */
int scheduler_start(unsigned quantum_ms);

/* All child stages must already be stopped and belong to pgid. */
int scheduler_submit(struct scheduler_job *job, pid_t pgid,
                     unsigned long client_id);

/* Children must have exited but must not yet have been waited for/released. */
int scheduler_finish(struct scheduler_job *job, pid_t pgid);

/* Remove and terminate an unfinished owned process group, if present. */
void scheduler_cancel(struct scheduler_job *job);

/* Stop and join the scheduler after the server has closed its sessions. */
void scheduler_stop(void);

#endif

