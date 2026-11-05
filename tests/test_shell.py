#!/usr/bin/env python3
"""Check Phase 1 input handling and pipeline cleanup on Linux.

Build the shell first, then run from any directory:
python3 /path/to/RemoteShell/tests/test_shell.py

Uses only Python's standard library and Linux /proc.
"""

import os
from pathlib import Path
import select
import signal
import subprocess
import time


PROJECT_ROOT = Path(__file__).resolve().parent.parent
PROMPT = b"RemoteShell> "


def main():
    executable = PROJECT_ROOT / "remoteshell"
    if not executable.is_file():
        raise SystemExit("Build RemoteShell with make before running this test.")

    # A separate process group lets a failed test clean up pipeline children.
    shell = subprocess.Popen(
        [str(executable)],
        cwd=PROJECT_ROOT,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    completed = False

    def check(condition, message):
        # Explicit checks also work if Python is invoked with optimization.
        if not condition:
            raise AssertionError(message)

    def read_prompt():
        """Read to the next prompt; fail instead of waiting indefinitely."""
        output = bytearray()
        deadline = time.monotonic() + 5

        while not output.endswith(PROMPT):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("Shell did not return to the prompt")

            ready, _, _ = select.select([shell.stdout], [], [], remaining)
            if not ready:
                raise RuntimeError("Shell timed out")

            # Read from the descriptor directly to avoid buffered-read issues.
            byte = os.read(shell.stdout.fileno(), 1)
            if not byte:
                raise RuntimeError("Shell exited unexpectedly")
            output.extend(byte)

        return output.decode(errors="replace")

    def run(command):
        shell.stdin.write((command + "\n").encode())
        shell.stdin.flush()
        return read_prompt()

    try:
        read_prompt()

        fd_path = Path(f"/proc/{shell.pid}/fd")
        child_path = Path(f"/proc/{shell.pid}/task/{shell.pid}/children")
        original_fds = set(os.listdir(fd_path))
        original_directory = run("pwd")

        check(run("   \t") == PROMPT.decode(), "Whitespace was not ignored")
        check("too many arguments" in run("echo " + "x " * 65),
              "Excessive arguments were not rejected")
        check("input exceeds" in run("x" * 1024),
              "Oversized input was not rejected")
        check("recovered\n" in run("echo recovered"),
              "Shell did not recover after invalid input")

        # Invalid built-ins must leave the shell alive and directory unchanged.
        check("Usage: exit" in run("exit extra"),
              "exit accepted an unexpected argument")
        check("Usage: cd" in run("cd /tmp extra"),
              "cd accepted excessive arguments")
        check(run("pwd") == original_directory,
              "Invalid built-in changed the working directory")

        # Repeated pipelines exercise descriptor closure and child reaping.
        for iteration in range(50):
            result = run("seq 1000 | cat | cat | wc -l")
            check(result.strip() == "1000\nRemoteShell>",
                  f"Unexpected output on pipeline {iteration + 1}: {result!r}")

        check(set(os.listdir(fd_path)) == original_fds,
              "Shell descriptor set changed after repeated pipelines")
        check(child_path.read_text().strip() == "",
              "Child processes remain after the shell returned to its prompt")

        # Closing the input stream checks EOF handling without the exit built-in.
        shell.stdin.close()
        check(shell.wait(timeout=5) == 0, "EOF termination was unsuccessful")
        completed = True

        print("PASS: input checks, 50 pipelines, stable descriptors,")
        print("no remaining children, and successful EOF termination.")
    finally:
        if not completed:
            try:
                os.killpg(shell.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

        shell.wait()
        shell.stdin.close()
        shell.stdout.close()


if __name__ == "__main__":
    main()
