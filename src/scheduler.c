#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "scheduler.h"

/*
 * A single scheduler thread controls runnable job groups.
 * All queue/state changes and group signals occur under this mutex.
 */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed;
static pthread_t scheduler_thread;
static struct scheduler_job *head;
static struct scheduler_job *tail;
static struct scheduler_job *current;
static unsigned quantum;
static int stopping;
static int started;
static struct timespec origin;

/* Unexpected synchronization failures indicate an internal server error. */
static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        abort();
    }
}

static void log_job(const struct scheduler_job *job, const char *event)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        perror("scheduler clock");
        abort();
    }
    double elapsed = (double)(now.tv_sec - origin.tv_sec) +
                     (double)(now.tv_nsec - origin.tv_nsec) / 1000000000.0;
    printf("[scheduler +%.3fs] client %lu group %ld: %s\n",
           elapsed, job->client_id, (long)job->pgid, event);
    fflush(stdout);
}

static struct timespec deadline_after_quantum(void)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) == -1) {
        perror("scheduler clock");
        abort();
    }
    deadline.tv_sec += (time_t)(quantum / 1000);
    deadline.tv_nsec += (long)(quantum % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static void enqueue(struct scheduler_job *job)
{
    job->next = NULL;
    if (tail != NULL)
        tail->next = job;
    else
        head = job;
    tail = job;
}

static struct scheduler_job *dequeue(void)
{
    struct scheduler_job *job = head;
    head = job->next;
    if (head == NULL)
        tail = NULL;
    job->next = NULL;
    return job;
}

static void remove_ready(struct scheduler_job *job)
{
    struct scheduler_job *previous = NULL;
    struct scheduler_job *entry = head;
    while (entry != NULL && entry != job) {
        previous = entry;
        entry = entry->next;
    }
    if (entry == NULL)
        return;
    if (previous != NULL)
        previous->next = entry->next;
    else
        head = entry->next;
    if (tail == entry)
        tail = previous;
    entry->next = NULL;
}

/* The lock is held, so the owner cannot free this job during removal. */
static void make_idle(struct scheduler_job *job)
{
    if (current == job)
        current = NULL;
    else if (job->state == JOB_READY)
        remove_ready(job);
    job->state = JOB_IDLE;
    job->next = NULL;
    job->pgid = 0;
}

static void *schedule_jobs(void *unused)
{
    (void)unused;
    struct timespec deadline = {0};
    check_pthread(pthread_mutex_lock(&lock), "scheduler lock");

    while (!stopping) {
        while (!stopping && current == NULL && head == NULL)
            check_pthread(pthread_cond_wait(&changed, &lock), "scheduler wait");
        if (stopping)
            break;

        if (current == NULL) {
            current = dequeue();
            current->state = JOB_RUNNING;
            if (kill(-current->pgid, SIGCONT) == -1) {
                perror("resume job group");
                log_job(current, "RESUME FAILED");
                /* Unblock the worker's exit wait if any members remain. */
                (void)kill(-current->pgid, SIGKILL);
                make_idle(current);
                continue;
            }
            log_job(current, "RUNNING");
            deadline = deadline_after_quantum();
        }

        /*
         * New arrivals wake us but do not restart the current time slice.
         * Completion wakes us and clears current, allowing an early switch.
         */
        int error = pthread_cond_timedwait(&changed, &lock, &deadline);
        if (error != 0 && error != ETIMEDOUT)
            check_pthread(error, "scheduler timed wait");
        if (stopping)
            break;
        if (current == NULL)
            continue;
        if (error != ETIMEDOUT)
            continue;

        if (head == NULL) {
            /* A lone job keeps running instead of being paused pointlessly. */
            deadline = deadline_after_quantum();
            continue;
        }

        struct scheduler_job *expired = current;
        if (kill(-expired->pgid, SIGSTOP) == -1) {
            perror("pause job group");
            log_job(expired, "PAUSE FAILED");
            (void)kill(-expired->pgid, SIGKILL);
            make_idle(expired);
            continue;
        }

        log_job(expired, "QUANTUM EXPIRED");
        expired->state = JOB_READY;
        current = NULL;
        enqueue(expired);
    }

    check_pthread(pthread_mutex_unlock(&lock), "scheduler unlock");
    return NULL;
}

int scheduler_start(unsigned quantum_ms)
{
    if (started || quantum_ms < 10 || quantum_ms > 5000) {
        errno = EINVAL;
        return -1;
    }

    pthread_condattr_t attributes;
    int error = pthread_condattr_init(&attributes);
    if (error != 0) {
        errno = error;
        return -1;
    }
    error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (error == 0)
        error = pthread_cond_init(&changed, &attributes);
    pthread_condattr_destroy(&attributes);
    if (error != 0) {
        errno = error;
        return -1;
    }

    quantum = quantum_ms;
    stopping = 0;
    head = tail = current = NULL;
    if (clock_gettime(CLOCK_MONOTONIC, &origin) == -1) {
        pthread_cond_destroy(&changed);
        return -1;
    }

    error = pthread_create(&scheduler_thread, NULL, schedule_jobs, NULL);
    if (error != 0) {
        pthread_cond_destroy(&changed);
        errno = error;
        return -1;
    }
    started = 1;
    return 0;
}

int scheduler_submit(struct scheduler_job *job, pid_t pgid,
                     unsigned long client_id)
{
    check_pthread(pthread_mutex_lock(&lock), "scheduler submit lock");
    if (stopping || job->state != JOB_IDLE || pgid <= 1) {
        check_pthread(pthread_mutex_unlock(&lock), "scheduler submit unlock");
        errno = EINVAL;
        return -1;
    }

    job->pgid = pgid;
    job->client_id = client_id;
    job->state = JOB_READY;
    enqueue(job);
    log_job(job, "READY");
    check_pthread(pthread_cond_signal(&changed), "scheduler notify");
    check_pthread(pthread_mutex_unlock(&lock), "scheduler submit unlock");
    return 0;
}

int scheduler_finish(struct scheduler_job *job, pid_t pgid)
{
    check_pthread(pthread_mutex_lock(&lock), "scheduler finish lock");
    if (job->state == JOB_IDLE || job->pgid != pgid) {
        check_pthread(pthread_mutex_unlock(&lock), "scheduler finish unlock");
        errno = EINVAL;
        return -1;
    }

    log_job(job, "COMPLETED");
    make_idle(job);
    check_pthread(pthread_cond_signal(&changed), "scheduler notify");
    check_pthread(pthread_mutex_unlock(&lock), "scheduler finish unlock");
    return 0;
}

void scheduler_cancel(struct scheduler_job *job)
{
    check_pthread(pthread_mutex_lock(&lock), "scheduler cancel lock");
    if (job->state != JOB_IDLE) {
        log_job(job, "CANCELLED");
        if (kill(-job->pgid, SIGKILL) == -1 && errno != ESRCH)
            perror("cancel job group");
        make_idle(job);
        check_pthread(pthread_cond_signal(&changed), "scheduler notify");
    }
    check_pthread(pthread_mutex_unlock(&lock), "scheduler cancel unlock");
}

void scheduler_stop(void)
{
    if (!started)
        return;
    check_pthread(pthread_mutex_lock(&lock), "scheduler stop lock");
    stopping = 1;
    if (current != NULL) {
        (void)kill(-current->pgid, SIGKILL);
        make_idle(current);
    }
    while (head != NULL) {
        struct scheduler_job *job = head;
        (void)kill(-job->pgid, SIGKILL);
        make_idle(job);
    }
    check_pthread(pthread_cond_signal(&changed), "scheduler stop notify");
    check_pthread(pthread_mutex_unlock(&lock), "scheduler stop unlock");
    check_pthread(pthread_join(scheduler_thread, NULL), "join scheduler");
    check_pthread(pthread_cond_destroy(&changed), "destroy scheduler condition");
    started = 0;
}

