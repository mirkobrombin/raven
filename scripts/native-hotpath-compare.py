#!/usr/bin/env python3
"""Time the hot path benchmark in several modes of the runtime on one machine, the modes taking turns, and print each row's medians."""

import os
import re
import statistics
import subprocess
import sys

ROUNDS = 5


def timed(runtime, fixture, name, settings):
    environment = os.environ | dict(item.split("=", 1) for item in settings.split(",") if item)
    environment["RAVEN_PREFIX"] = os.environ["RAVEN_PREFIX"] + "-" + name
    result = subprocess.run([runtime, "--run-process", "--execution=native", "--stdio=inherit", "--timeout-ms=120000", fixture],
                            env=environment, capture_output=True, text=True, timeout=140)
    if result.returncode != 0 or "hotpath done" not in result.stdout:
        raise SystemExit(f"{name} failed: exit {result.returncode}\n{result.stdout}{result.stderr}")
    return re.findall(r"^hotpath (\w+) (\d+)", result.stdout, re.M)


def main():
    runtime, fixture, *modes = sys.argv[1:]
    modes = [mode.split(":", 1) for mode in modes]
    if len(modes) < 2:
        raise SystemExit("two modes at least, each NAME:VARIABLE=VALUE,...")
    samples = {}
    for round_number in range(ROUNDS):
        # Each round starts one mode later, so none always runs first.
        first = round_number % len(modes)
        for name, settings in modes[first:] + modes[:first]:
            for row, value in timed(runtime, fixture, name, settings):
                samples.setdefault(row, {}).setdefault(name, []).append(int(value))
    base = modes[-1][0]
    for row, measured in samples.items():
        reference = statistics.median(measured[base])
        for name, _ in modes:
            values = measured[name]
            median = statistics.median(values)
            ratio = f" x{median / reference:.2f}" if reference else ""
            print(f"compare {row} {name} median {median:.0f} range {min(values)}-{max(values)}{ratio} of {base}")


main()
