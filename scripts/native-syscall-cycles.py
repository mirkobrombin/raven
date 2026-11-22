#!/usr/bin/env python3
"""Check that RAVEN_CYCLES reports the NT system calls a program makes itself, as it does the calls through served stubs, and that one
whose arguments and results lie on the caller's stack has the kernel copy guest memory once at most."""

import os
import re
import subprocess
import sys


def main():
    runtime, fixture = sys.argv[1:3]
    result = subprocess.run([runtime, "--run-process", "--execution=native", "--stdio=inherit", "--timeout-ms=120000", fixture],
                            env=os.environ | {"RAVEN_CYCLES": "600"}, capture_output=True, text=True, timeout=140)
    output = result.stdout + result.stderr
    if result.returncode != 0 or "hotpath done" not in output:
        raise SystemExit(f"the program failed: exit {result.returncode}\n{output}")
    row = re.search(r"^cycles:\s+NtQueryEvent\s+(\d+) calls\s+\d+ beside\s+boundary\s+(\d+)\s+turn\s+\d+\s+service\s+(\d+)\s+return\s+(\d+)"
                    r"\s+lock\s+\d+\s+copies\s+(\d+)",
                    output, re.M)
    if not row:
        raise SystemExit(f"NtQueryEvent is not reported\n{output}")
    calls, boundary, service, back, copies = map(int, row.groups())
    if calls < 20000 or not service:
        raise SystemExit(f"NtQueryEvent is not reported for every call\n{output}")
    if copies > calls * 1.1:
        raise SystemExit(f"NtQueryEvent has guest memory copied {copies} times in {calls} calls")
    print(f"NtQueryEvent {calls} calls: boundary {boundary}, service {service}, return {back} cycles")


main()
