# RemoteShell Phase 3: threaded client sessions

## What this update adds

The server accepts multiple clients and creates one detached POSIX thread for
each client. Each thread receives that client's commands, dispatches execution,
and forwards the resulting output/error frames to that client.

A separate, single-threaded shell worker process holds each client's working
directory and command execution state. It runs the existing shared shell
engine. This combines POSIX threads for concurrent session handling with
processes for command execution.

A mutex protects the active-client counter. It is never held while waiting
for a client or a command, so a slow command does not serialize all clients.
The server admits up to 32 active sessions.

## Installation into the existing Phase 2 project

This archive is an UPDATE, not a standalone project. It requires the existing
Phase 2 main.c, shell.c, client.c, and net.c in src/.

1. Stop the old server using Ctrl+C after closing its clients.
2. Copy the current project folder outside the repository as a Phase 2 backup.
3. In Ubuntu, create a branch from the completed Phase 2 work:

```bash
cd /mnt/c/Tehsin/remoteshell/RemoteShell
git status
git switch -c phase3-threads
```

4. Copy the following files from this archive to the corresponding locations:

| File | Action |
| --- | --- |
| src/server.c | Replace the sequential server with the threaded server |
| src/server_worker.c | Add the isolated shell worker executable source |
| src/net.h | Replace; adds the private worker-channel descriptor constant |
| src/shell.h | Replace; updates the shared-engine usage comment |
| Makefile | Replace; builds the worker and links server with -pthread |
| PHASE3_GUIDE.md | Add this guide |

Keep all other source files, reports, README, tests, and .gitignore.

5. Ensure the existing .gitignore includes these generated programs:

```gitignore
/remoteshell
/server
/client
/server_worker
```

6. Build:

```bash
make clean
make
```

The four programs are remoteshell, server, client, and server_worker.
The Makefile also builds server_worker when make server is requested.
Do not start server_worker manually; the server launches it internally.

## First test: three Ubuntu terminals

Open all three terminals in the same Ubuntu-24.04 distribution and enter
the repository directory in each:

```bash
cd /mnt/c/Tehsin/remoteshell/RemoteShell
```

Terminal 1:

```bash
./server
```

Terminal 2:

```bash
./client
```

Terminal 3:

```bash
./client
```

Both clients should connect while the other remains connected.
The server logs connected/disconnected client IDs and worker process IDs.

### Test 1: actual concurrent execution

After BOTH clients are connected, enter in client A:

```text
sleep 15
```

While A is still waiting, enter in client B:

```text
echo CLIENT_B_RESPONDED
pwd
```

Client B must receive output before client A's sleep finishes.
Connecting two clients alone does not prove concurrent execution.

### Test 2: independent working directories

In client A:

```text
cd /tmp
pwd
```

Expected: /tmp.

In client B:

```text
pwd
cd /
pwd
```

Expected: first the server's starting directory, then /.

In client A again:

```text
pwd
```

Expected: still /tmp. Client B must not change client A's directory.

### Test 3: output separation and pipes

In A:

```text
echo CLIENT_A_ONLY
seq 10 | cat | grep 1 | wc -l
```

In B:

```text
echo CLIENT_B_ONLY
echo hello | wc -c
seq 10 | grep 1 | wc -l
```

Expected: A receives its marker and 2; B receives its marker, 6, and 2.
Neither client should receive the other client's marker.

For a larger result on A while B remains usable:

```text
seq 2000
```

For a larger pipeline:

```text
seq 100000 | cat | cat | wc -l
yes | head -n 5
```

### Test 4: independent error recovery

In A:

```text
not_a_real_command
ls |
echo A_RECOVERED
```

In B:

```text
echo B_UNAFFECTED
```

Only A should receive its error messages. Both clients must still work.

### Test 5: disconnect and reconnect

In A:

```text
exit
```

Keep B connected, then run in B:

```text
echo B_STILL_CONNECTED
pwd
```

Start ./client again in A's terminal and enter pwd. The new session should
start in the server's original directory, while B should remain at /.

Ctrl+D at an empty client prompt also disconnects that client.

### Optional thread evidence

From a connected client, run:

```text
ps -T -C server
```

On Ubuntu, this displays the server's main thread and active client threads.
Worker process IDs are shown separately in the server log.

Close clients before stopping the server with Ctrl+C.

## Source design explained

### Why execution is isolated

Threads in a process share its working directory and file descriptor table.
Calling chdir in one thread would change every client's current directory.
Redirecting process-wide stdout/stderr with dup2 would risk mixing output.
The existing strtok parser also holds process-wide parsing state.

Each client instead has one separate worker process, where these operations
are private. The shell engine remains single-threaded inside that worker.
There is no global command-execution mutex serializing client requests.

### Why posix_spawn is used

A forked child of a multithreaded process is restricted in what it can do
before exec. The server uses posix_spawn to start the separate worker
executable, avoiding application-level parsing, stdio, or allocation in a
fork child of the threaded server.

The worker then runs as a normal single-threaded process. Its fork/exec
calls create external programs and pipe stages just as in Phase 2.

### Thread and descriptor ownership

The main server loop accepts a socket and reserves a slot. It allocates one
client_context and transfers ownership to the new detached thread.

The thread owns its client socket, worker-channel socket, worker PID, and
context allocation. At session end it closes descriptors, waits for its own
worker, frees the context, and releases the slot. Detached threads release
their thread resources when they return.

Sockets are created with close-on-exec atomically. This matters because
another thread may spawn a worker concurrently. The listener and other
clients' sockets must not appear in that worker.

### Communication

The client uses the same Phase 2 TCP protocol, so client.c needs no changes.

A private AF_UNIX socketpair connects each thread to its worker. This is
local IPC inside the server machine, separate from the client's TCP socket.
The same length-prefixed request and response format is reused over it.

The worker captures stdout/stderr in a temporary file and sends response
chunks to its owning thread. That thread forwards them to the owning client.
Output is returned after command completion, as in Phase 2.

### Synchronization

clients_mutex protects active_clients during short admission/release updates.
Its lock is released before thread creation, request handling, worker startup,
and command execution. The worker executable path is shared read-only.

### Resource and failure handling

- Startup arguments and request lengths are validated.
- A separate allocation is used per client; threads do not share the address
  of an accept-loop socket variable.
- Thread/spawn failures close resources and release the client slot.
- Failed sessions terminate their worker process group before waiting for
  the worker; this group includes ordinary command children.
- The server waits for each worker by its specific PID.
- Client errors and malformed requests do not shut down other sessions.
- Excess connections receive a client-limit response.
- The main server never changes its working directory or redirects stdout.

## Validation performed during development

All four programs built with -std=c11, -Wall, -Wextra, -Wpedantic, and -Werror.

Seventeen test groups passed, covering concurrent clients, directory and
output isolation, pipes, binary/multichunk output, fragmented/coalesced TCP
requests, malformed input, reconnects, the 32-client limit, worker startup
failure, and local-shell/client compatibility.

The tests included eight clients running marked output and pipes
simultaneously, twenty reconnect cycles, and thirty repeated three-pipe
commands after filling and releasing all 32 client slots.

An additional test used two actual compiled C clients: B returned its output
while A was still executing sleep 2.

These checks were performed in the development environment. Repeat the manual
checks on your laptop and use your actual screenshots/results in the report.

## Limitations and remaining project questions

The server remains an educational service without authentication/encryption.
Use loopback for initial tests; it supports explicit IPv4 address/port arguments.

Commands are noninteractive, output is returned after completion, and the
parser still lacks quotes, escapes, redirection, background jobs, and
conditional operators. Built-ins inside pipes remain unsupported.

There is no execution timeout or interactive command-cancellation feature.
A client disconnect during command execution may be detected only when the
thread next communicates with that client; other threads remain responsive.
Close client sessions before stopping the server.

The implementation uses Linux accept4, SOCK_CLOEXEC, and /proc/self/exe,
alongside POSIX threads and posix_spawn, and targets Linux/Ubuntu as required.

The project brief also mentions a custom time-based scheduler, without a
policy or interface. This update implements the explicit Phase 3 requirement
for multithreaded, simultaneous client sessions. It does not implement an
additional custom scheduler; confirm that requirement with the instructor
before final project submission.

## Suggested commit after your tests pass

```bash
git add src/server.c src/server_worker.c src/net.h src/shell.h
git add Makefile PHASE3_GUIDE.md .gitignore
git commit -m "feat: serve concurrent clients with POSIX threads"
git push -u origin phase3-threads
```

