#!/usr/bin/env python3
"""Check that RAVEN_CYCLES reports the NT system calls a program makes itself, as it does the calls through served stubs, and that one
whose arguments and results lie on the caller's stack has the kernel copy guest memory once at most. So does a call that reads a
name and sets the thread's last error: the thread's own block is reached as its stack is. Run again with the stubs off, so that the
calls are real system calls, the signal that takes each to the agent and its way back to the guest are counted too."""

import os
import re
import subprocess
import sys


def cycles(runtime, fixture, environment):
    result = subprocess.run([runtime, "--run-process", "--execution=native", "--stdio=inherit", "--timeout-ms=120000", fixture],
                            env=os.environ | {"RAVEN_CYCLES": "600"} | environment, capture_output=True, text=True, timeout=140)
    output = result.stdout + result.stderr
    if result.returncode != 0 or "hotpath done" not in output:
        raise SystemExit(f"the program failed: exit {result.returncode}\n{output}")
    return output


def main():
    runtime, fixture = sys.argv[1:3]
    output = cycles(runtime, fixture, {})
    row = re.search(r"^cycles:\s+NtQueryEvent\s+(\d+) calls\s+(\d+) beside\s+boundary\s+(\d+)\s+turn\s+\d+\s+service\s+(\d+)\s+return\s+(\d+)"
                    r"\s+lock\s+\d+\s+copies\s+(\d+)\s+signal\s+(\d+)\s+sigreturn\s+(\d+)",
                    output, re.M)
    if not row:
        raise SystemExit(f"NtQueryEvent is not reported\n{output}")
    calls, beside, boundary, service, back, copies, _, _ = map(int, row.groups())
    if calls < 20000 or not service:
        raise SystemExit(f"NtQueryEvent is not reported for every call\n{output}")
    # The state of an event is under the object's own lock, as what SetEvent and ResetEvent change: it needs no turn.
    if beside < calls * 0.99:
        raise SystemExit(f"NtQueryEvent is answered beside the turn {beside} times in {calls} calls")
    if copies > calls * 1.1:
        raise SystemExit(f"NtQueryEvent has guest memory copied {copies} times in {calls} calls")
    # Each process reports for itself, those that make the prefix as well: the program's row is the one with all its calls.
    rows = re.findall(r"^cycles:\s+GetFileAttributesW\s+(\d+) calls\s.*\scopies\s+(\d+)", output, re.M)
    named_calls, named_copies = max(((int(made), int(copied)) for made, copied in rows), default=(0, 0))
    if named_calls < 20000:
        raise SystemExit(f"GetFileAttributesW is not reported for every call\n{output}")
    if named_copies > named_calls * 1.1:
        raise SystemExit(f"GetFileAttributesW has guest memory copied {named_copies} times in {named_calls} calls")
    trapped = cycles(runtime, fixture, {"RAVEN_TRAPPED_CALLS": "1"})
    row = re.search(r"^cycles:\s+NtQueryEvent\s+(\d+) calls\s.*\ssignal\s+(\d+)\s+sigreturn\s+(\d+)", trapped, re.M)
    if not row or int(row.group(1)) < 20000:
        raise SystemExit(f"NtQueryEvent is not reported for every system call\n{trapped}")
    signal, sigreturn = int(row.group(2)), int(row.group(3))
    if not signal or not sigreturn:
        raise SystemExit(f"NtQueryEvent's signal {signal} and its return {sigreturn} cycles are not counted\n{trapped}")
    print(f"NtQueryEvent {calls} calls: boundary {boundary}, service {service}, return {back} cycles; as system calls: signal {signal}, "
          f"sigreturn {sigreturn} cycles")


main()
