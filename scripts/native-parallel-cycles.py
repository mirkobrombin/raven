#!/usr/bin/env python3
"""Check what RAVEN_CYCLES reports for a program whose threads call the system at once.

--beside names the calls the runtime must answer beside the turn at least half the times they reach it, --always those it must answer
there at least nine times in ten; --guest names those the guest answers itself, which may reach the runtime only when a change came in
between (60 times at most). --lock-under bounds the share of the service of the calls --beside names spent waiting for the desktop lock,
in percent: calls that sleep or wait would hide it. The share swings between runs with how the machine schedules the threads, so it is
the median of --runs runs that is bounded; the other checks hold for every run. --whole-at-most bounds how many times in a run a call
(NAME:COUNT) answered beside the turn takes the desktop lock whole, stopping every other thread's call on the desktop meanwhile: a
count, not a share, since the times it must (the process's first look, the first after a window appears) do not grow with the run."""

import argparse
import os
import re
import statistics
import subprocess


def run(args):
    # No report comes during the run at this interval: the one the run ends with holds every call.
    result = subprocess.run([args.runtime, "--run-process", "--execution=native", "--timeout-ms=30000", args.fixture],
                            env=os.environ | {"RAVEN_CYCLES": "600"}, capture_output=True, text=True, timeout=90)
    output = result.stdout + result.stderr
    if result.returncode != 0 or "Native entry returned 0 " not in output:
        raise SystemExit(f"the program failed: exit {result.returncode}\n{output}")
    rows = []
    for match in re.finditer(r"^cycles:\s+(\S+)\s+(\d+) calls\s+(\d+) beside\s+boundary\s+(\d+)\s+turn\s+(\d+)\s+service\s+(\d+)"
                             r"\s+return\s+\d+\s+lock\s+(\d+).*\swhole\s+(\d+)$", output, re.M):
        rows.append((int(match[5]), match[1], int(match[2]), int(match[3]), int(match[6]), int(match[7]), int(match[8])))
    if not rows:
        raise SystemExit(f"no calls reported\n{output}")
    for turn, name, calls, beside, service, locked, whole in sorted(rows, reverse=True):
        print(f"{name}: {calls} calls, {beside} beside, turn {turn}, service {service}, lock {locked}, whole {whole}")
    reached, beside, whole = {}, {}, {}
    for _, name, calls, answered, _, _, taken in rows:
        reached[name] = reached.get(name, 0) + calls
        beside[name] = beside.get(name, 0) + answered
        whole[name] = whole.get(name, 0) + taken
    for name in filter(None, args.beside.split(",")):
        if beside.get(name, 0) < reached.get(name, 0) // 2 or not reached.get(name, 0):
            raise SystemExit(f"{name} is answered beside the turn {beside.get(name, 0)} of {reached.get(name, 0)} times\n{output}")
    for name in filter(None, args.always.split(",")):
        if beside.get(name, 0) * 10 < reached.get(name, 0) * 9 or not reached.get(name, 0):
            raise SystemExit(f"{name} is answered beside the turn {beside.get(name, 0)} of {reached.get(name, 0)} times\n{output}")
    for name in filter(None, args.guest.split(",")):
        if reached.get(name, 0) > 60:
            raise SystemExit(f"{name} reaches the runtime {reached[name]} times while threads run at once\n{output}")
    for bound in filter(None, args.whole_at_most.split(",")):
        name, most = bound.rsplit(":", 1)
        if whole.get(name, 0) > int(most):
            raise SystemExit(f"{name} took the desktop lock whole {whole.get(name, 0)} of the {beside.get(name, 0)} times it was answered "
                             f"beside the turn, {most} at most\n{output}")
    measured = [row for row in rows if row[1] in set(filter(None, args.beside.split(",")))]
    service = sum(calls * served for _, _, calls, _, served, _, _ in measured)
    locked = sum(calls * waited for _, _, calls, _, _, waited, _ in measured)
    share = 100.0 * locked / service if service else 0.0
    print(f"desktop lock: {share:.1f}% of the service")
    return share


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime")
    parser.add_argument("fixture")
    parser.add_argument("--beside", default="", help="comma separated")
    parser.add_argument("--always", default="", help="comma separated")
    parser.add_argument("--guest", default="", help="comma separated")
    parser.add_argument("--lock-under", type=float, help="percent of the service")
    parser.add_argument("--whole-at-most", default="", help="comma separated NAME:COUNT")
    parser.add_argument("--runs", type=int, default=1)
    args = parser.parse_args()
    shares = [run(args) for _ in range(args.runs)]
    share = statistics.median(shares)
    print(f"desktop lock: median {share:.1f}% of the service over {len(shares)} runs: " + ", ".join(f"{one:.1f}" for one in shares))
    if args.lock_under is not None and share >= args.lock_under:
        raise SystemExit(f"the calls waited for the desktop lock {share:.1f}% of their service in the median run, {args.lock_under}% at most")


main()
