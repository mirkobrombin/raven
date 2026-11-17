#!/usr/bin/env python3
"""Run a native program again and again: a wake lost now and then hangs one run in a few dozen, which a single run rarely shows.

Each run must print "Native entry returned 0 " within the seconds given; the first that does not fails the check.

usage: native-repeat.py RAVEN PROGRAM RUNS SECONDS
"""

import subprocess
import sys


def main():
    raven, program, runs, seconds = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    for run in range(1, runs + 1):
        try:
            result = subprocess.run([raven, "--run-process", "--execution=native", f"--timeout-ms={seconds * 1000}", program],
                                    capture_output=True, text=True, timeout=seconds + 10)
        except subprocess.TimeoutExpired:
            raise SystemExit(f"run {run} of {runs} did not end")
        if "Native entry returned 0 " not in result.stdout + result.stderr:
            raise SystemExit(f"run {run} of {runs} failed: {(result.stdout + result.stderr)[-400:]}")
    print(f"{runs} runs passed")


if __name__ == "__main__":
    main()
