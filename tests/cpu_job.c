#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/*
 * Finite CPU-bound workload for the scheduler demonstration.
 * Process CPU time excludes time spent paused by SIGSTOP.
 * Usage: ./cpu_job milliseconds [label]
 */
int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s milliseconds [label]\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *end;
    errno = 0;
    long target = strtol(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0' ||
        target < 10 || target > 60000) {
        fputs("cpu_job: duration must be 10 to 60000 CPU milliseconds\n", stderr);
        return EXIT_FAILURE;
    }
    const char *label = argc == 3 ? argv[2] : "job";

    struct timespec start, now;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start) == -1) {
        perror("process CPU clock");
        return EXIT_FAILURE;
    }

    volatile unsigned long value = 1;
    double elapsed;
    do {
        /* Unsigned arithmetic has defined wraparound; keep the CPU busy. */
        for (unsigned i = 0; i < 50000; i++)
            value = value * 1664525UL + 1013904223UL;
        if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) == -1) {
            perror("process CPU clock");
            return EXIT_FAILURE;
        }
        elapsed = (double)(now.tv_sec - start.tv_sec) * 1000.0 +
                  (double)(now.tv_nsec - start.tv_nsec) / 1000000.0;
    } while (elapsed < (double)target);

    printf("CPU job %s completed: %.0f ms of process CPU time\n", label, elapsed);
    return EXIT_SUCCESS;
}

