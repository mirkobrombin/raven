#!/usr/bin/env python3
"""Check process exit, worker faults and deadlines while guest threads compute."""

import os
import subprocess
import sys


def main():
    runtime, fixture = sys.argv[1:3]
    cases = [("exit", 5000, 0, "Native entry returned 0 ", {}),
             ("fault", 5000, 1, "native guest fault signal 11", {}),
             ("timeout", 1000, 1, "native entry timed out", {"RAVEN_TRACE_API": "1"})]
    if "--sample" in sys.argv[3:]:
        cases.append(("timeout", 5500, 1, "samples: ", {"RAVEN_SAMPLE_GUEST": "1"}))
    for mode, limit, expected, message, extra in cases:
        result = subprocess.run([runtime, "--run-process", "--execution=native", f"--timeout-ms={limit}", fixture, "--", mode],
                                env=os.environ | extra, capture_output=True, text=True, timeout=20)
        output = result.stdout + result.stderr
        # A 32-bit program is handed the fault as an exception; with nothing to handle it, it ends with the exception's status as on
        # Windows, which the runtime names.
        unhandled = mode == "fault" and "unhandled exception 0xc0000005" in output and "exited with status 3221225477" in output
        if "native process is not stopped" in output or not (unhandled or (result.returncode == expected and message in output)):
            raise SystemExit(f"{mode}: exit {result.returncode}\n{output}")
        print(f"{mode}: expected exit {expected}")


main()
