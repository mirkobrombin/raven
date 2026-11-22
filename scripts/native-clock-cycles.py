#!/usr/bin/env python3
"""Check that the guest reads its clocks itself from the start of the process: of a million GetTickCount calls few reach the runtime."""

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
    if not re.search(r"^cycles: \d+ calls", output, re.M):
        raise SystemExit(f"no report of the calls\n{output}")
    trapped = sum(int(count) for count in re.findall(r"^cycles:\s+GetTickCount\s+(\d+) calls", output, re.M))
    if trapped >= 10000:
        raise SystemExit(f"{trapped} of 1000000 GetTickCount calls left the guest\n{output}")
    print(f"{trapped} of 1000000 GetTickCount calls left the guest")


main()
