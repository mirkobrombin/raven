#!/usr/bin/env python3
"""Runs the custom action fixture with process ids of six hex digits, as on a desktop that has run for a while: the installer names
the pipe of its custom action server after its process id, and the server has to reach it under the same name."""

from pathlib import Path
import subprocess
import sys

# The first process id that takes six hex digits.
FIRST = 0x100000
SKIP = 77
# A user and process id namespace of the caller's own: its root sets the counter the namespace's next process ids follow.
NAMESPACE = ["unshare", "--user", "--map-root-user", "--pid", "--fork", "--mount-proc"]
START = 'echo "$1" > /proc/sys/kernel/ns_last_pid || exit 1; shift; exec "$@"'


def main():
    raven, program = sys.argv[1:]
    if int(Path("/proc/sys/kernel/pid_max").read_text()) <= FIRST + 1000:
        print(f"SKIP: pid_max is at most {FIRST + 1000:#x}, so process ids do not reach six hex digits")
        return SKIP
    try:
        probe = subprocess.run([*NAMESPACE, "sh", "-c", START, "sh", str(FIRST), "sh", "-c", "echo $$"], capture_output=True, text=True, timeout=20)
    except FileNotFoundError:
        print("SKIP: unshare is not installed")
        return SKIP
    if probe.returncode or not probe.stdout.strip().isdigit() or int(probe.stdout) <= FIRST:
        print(f"SKIP: no process id namespace of our own can start at {FIRST:#x}: {probe.stderr.strip()}")
        return SKIP
    try:
        result = subprocess.run([*NAMESPACE, "sh", "-c", START, "sh", str(FIRST), raven, "--run-process", "--execution=native", "--timeout-ms=120000",
                                 program], capture_output=True, text=True, timeout=240)
    except subprocess.TimeoutExpired:
        raise RuntimeError("custom actions with large process ids: the installer did not finish") from None
    output = result.stdout + result.stderr
    if "Native entry returned 0 " not in output:
        raise RuntimeError(f"custom actions with large process ids: status {result.returncode}\n{output}")
    print(f"PASS: custom actions ran with process ids from {FIRST:#x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
