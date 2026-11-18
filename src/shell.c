#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>

#include "shell.h"

/* Limits include the program name and allow up to three pipes. */
#define MAX_ARGS 64
#define MAX_COMMANDS 4

/*
 * Split a command into arguments separated by spaces, tabs, or carriage returns.
 *
 * strtok modifies input by replacing separators with '\0'.
 * The argument pointers refer to this same buffer; no allocation is needed.
 *
 * capacity includes space for the final NULL pointer.
 * Returns the argument count, or -1 if the array would overflow.
 */
static int parse_command(char *input, char *argv[], size_t capacity)
{
    size_t count = 0;
    char *token = strtok(input, " \t\r");

    while (token != NULL) {
        /* Always reserve the last position for NULL. */
        if (count >= capacity - 1) {
            argv[count] = NULL;
            return -1;
        }

        argv[count++] = token;
        token = strtok(NULL, " \t\r");
    }

    /* execvp uses NULL to identify the end of the argument array. */
    argv[count] = NULL;
    return (int)count;
}

/* Single-command execution is implemented through the common executor below. */
static void execute_command(char *argv[]);

/*
 * Handle commands that must run in the calling shell process.
 *
 * Returns:
 *   0: This is not a built-in; try external execution.
 *   1: The built-in was handled, including any reported error.
 *   2: A valid exit command requested the end of this session.
 */
static int handle_builtin(int argc, char *argv[])
{
    if (strcmp(argv[0], "exit") == 0) {
        /* Numeric exit arguments are not supported in this version. */
        if (argc != 1) {
            fprintf(stderr, "Usage: exit\n");
            return 1;
        }

        /* Let the caller close the session without stopping the server. */
        return 2;
    }

    if (strcmp(argv[0], "cd") == 0) {
        if (argc > 2) {
            fprintf(stderr, "Usage: cd [directory]\n");
            return 1;
        }

        const char *directory;

        if (argc == 1) {
            /* Without a directory argument, use the user's home directory. */
            directory = getenv("HOME");

            if (directory == NULL || directory[0] == '\0') {
                fprintf(stderr, "cd: HOME is not set\n");
                return 1;
            }
        } else {
            directory = argv[1];
        }

        /*
         * Change the calling process's directory so later commands use it.
         * Performing this only in a child would not update the shell.
         */
        if (chdir(directory) == -1) {
            perror("cd");
        }

        return 1;
    }

    return 0;
}

/*
 * Separate the input into commands at each pipe character.
 * Parse each command's arguments before executing any part of the line.
 *
 * commands stores an argument array for each command.
 * arg_counts stores the corresponding argument counts.
 *
 * Returns the command count, or -1 for invalid input.
 * Quotes and escape sequences are not interpreted.
 */
static int parse_pipeline(char *input,
                          char *commands[][MAX_ARGS + 1],
                          int arg_counts[])
{
    int count = 0;

    for (;;) {
        if (count == MAX_COMMANDS) {
            fprintf(stderr, "RemoteShell: maximum 3 pipes supported\n");
            return -1;
        }

        /*
         * End the current command at the next pipe.
         * Empty sections remain detectable, unlike splitting on pipes
         * with strtok, which would skip consecutive delimiters.
         */
        char *separator = strchr(input, '|');

        if (separator != NULL)
            *separator = '\0';

        int argc = parse_command(input, commands[count], MAX_ARGS + 1);

        if (argc < 0) {
            fprintf(stderr, "RemoteShell: too many arguments\n");
            return -1;
        }

        if (argc == 0) {
            fprintf(stderr, "RemoteShell: missing command beside pipe\n");
            return -1;
        }

        arg_counts[count++] = argc;

        /* No further pipe means the entire line has been parsed. */
        if (separator == NULL)
            return count;

        input = separator + 1;
    }
}

/* Close original pipe descriptors after redirection or setup failure. */
static void close_pipes(int pipes[][2], int count)
{
    for (int i = 0; i < count; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

static struct shell_job_hooks job_hooks;

void shell_set_job_hooks(const struct shell_job_hooks *hooks)
{
    if (hooks != NULL)
        job_hooks = *hooks;
    else
        memset(&job_hooks, 0, sizeof(job_hooks));
}

static int observe_child(pid_t child, int options, siginfo_t *information)
{
    for (;;) {
        if (waitid(P_PID, (id_t)child, information, options) == 0)
            return 0;
        if (errno != EINTR)
            return -1;
    }
}

/* Release every child process entry, retrying interrupted waits. */
static void wait_for_children(pid_t children[], int count)
{
    for (int i = 0; i < count; i++) {
        while (waitpid(children[i], NULL, 0) == -1) {
            if (errno == EINTR)
                continue;
            if (errno != ECHILD)
                perror("waitpid");
            break;
        }
    }
}

/* Also signal individual children if group setup was only partially finished. */
static void terminate_children(pid_t children[], int count, pid_t pgid)
{
    if (pgid > 1 && kill(-pgid, SIGKILL) == -1 && errno != ESRCH)
        perror("terminate command group");
    for (int i = 0; i < count; i++) {
        if (kill(children[i], SIGKILL) == -1 && errno != ESRCH)
            perror("terminate command child");
    }
    wait_for_children(children, count);
}

/*
 * Common executor for one external command or a pipeline of up to four stages.
 * In scheduled workers, all stages join one new process group and stop BEFORE
 * exec. The ready hook admits the group only after all stages are confirmed
 * stopped, so no command runs ahead of its first turn.
 */
static void execute_pipeline(char *commands[][MAX_ARGS + 1], int count)
{
    int pipes[MAX_COMMANDS - 1][2];
    pid_t children[MAX_COMMANDS];
    int pipe_count = count - 1;
    int scheduled = job_hooks.ready != NULL && job_hooks.finished != NULL;
    pid_t pgid = 0;
    pid_t owner = getpid();

    for (int i = 0; i < pipe_count; i++) {
        if (pipe(pipes[i]) == -1) {
            perror("pipe");
            close_pipes(pipes, i);
            return;
        }
    }

    for (int i = 0; i < count; i++) {
        children[i] = fork();
        if (children[i] == -1) {
            perror("fork");
            close_pipes(pipes, pipe_count);
            terminate_children(children, i, pgid);
            return;
        }

        if (children[i] == 0) {
            if (scheduled) {
                /* A stopped stage must not outlive an unexpectedly lost worker. */
                if (prctl(PR_SET_PDEATHSIG, (unsigned long)SIGKILL,
                          0UL, 0UL, 0UL) == -1) {
                    perror("command parent-death protection");
                    _exit(EXIT_FAILURE);
                }
                if (getppid() != owner)
                    _exit(EXIT_FAILURE);
                close(job_hooks.control_fd);
            }
            /* The first child creates the group; later stages join it. */
            if (scheduled && setpgid(0, pgid) == -1) {
                perror("child setpgid");
                _exit(EXIT_FAILURE);
            }
            if (i > 0 && dup2(pipes[i - 1][0], STDIN_FILENO) == -1) {
                perror("dup2 stdin");
                _exit(EXIT_FAILURE);
            }
            if (i < count - 1 && dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                perror("dup2 stdout");
                _exit(EXIT_FAILURE);
            }
            close_pipes(pipes, pipe_count);

            if (scheduled && raise(SIGSTOP) != 0)
                _exit(EXIT_FAILURE);

            execvp(commands[i][0], commands[i]);
            int saved_errno = errno;
            perror(commands[i][0]);
            _exit(saved_errno == ENOENT ? 127 : 126);
        }

        if (scheduled) {
            if (pgid == 0)
                pgid = children[i];
            /* Parent and child both set the group to avoid a startup race. */
            if (setpgid(children[i], pgid) == -1) {
                perror("parent setpgid");
                close_pipes(pipes, pipe_count);
                terminate_children(children, i + 1, pgid);
                return;
            }
        }
    }
    close_pipes(pipes, pipe_count);

    if (scheduled) {
        for (int i = 0; i < count; i++) {
            siginfo_t information;
            if (observe_child(children[i], WSTOPPED | WEXITED | WNOWAIT,
                              &information) == -1 ||
                information.si_code != CLD_STOPPED) {
                fputs("shell: command setup failed before scheduling\n", stderr);
                terminate_children(children, count, pgid);
                return;
            }
        }

        if (job_hooks.ready(job_hooks.context, pgid) == -1) {
            perror("scheduler ready handshake");
            terminate_children(children, count, pgid);
            return;
        }

        /*
         * Observe completion without releasing PIDs. The group identity stays
         * reserved until the server removes it from the scheduler and ACKs.
         */
        for (int i = 0; i < count; i++) {
            siginfo_t information;
            if (observe_child(children[i], WEXITED | WNOWAIT,
                              &information) == -1) {
                perror("observe command exit");
                terminate_children(children, count, pgid);
                return;
            }
        }

        if (job_hooks.finished(job_hooks.context, pgid) == -1) {
            perror("scheduler completion handshake");
            terminate_children(children, count, pgid);
            return;
        }
    }

    wait_for_children(children, count);
}

static void execute_command(char *argv[])
{
    char *commands[1][MAX_ARGS + 1];
    size_t i = 0;
    do {
        commands[0][i] = argv[i];
    } while (argv[i++] != NULL);
    execute_pipeline(commands, 1);
}

/*
 * Shared entry point used by the local interface and remote server.
 *
 * input must be writable and have its trailing newline removed.
 * Parsing modifies the buffer. Output goes to stdout and stderr.
 *
 * Returns 1 to end the session, or 0 to continue.
 * This return value does not represent an external program's exit status.
 */
int shell_run_line(char *input)
{
    /* Empty or whitespace-only input requires no execution. */
    if (input[strspn(input, " \t\r")] == '\0')
        return 0;

    char *commands[MAX_COMMANDS][MAX_ARGS + 1];
    int arg_counts[MAX_COMMANDS];

    /* Validate the entire line before launching any command. */
    int count = parse_pipeline(input, commands, arg_counts);

    if (count < 0)
        return 0;

    if (count == 1) {
        /* Built-ins run in the calling process; other commands use a child. */
        int builtin = handle_builtin(arg_counts[0], commands[0]);

        if (builtin == 2)
            return 1;

        if (builtin == 0)
            execute_command(commands[0]);

        return 0;
    }

    /* Reject built-ins inside pipes before starting any child processes. */
    for (int i = 0; i < count; i++) {
        if (strcmp(commands[i][0], "cd") == 0 ||
            strcmp(commands[i][0], "exit") == 0) {
            fprintf(stderr,
                    "RemoteShell: built-ins in pipelines are unsupported\n");
            return 0;
        }
    }

    execute_pipeline(commands, count);
    return 0;
}
