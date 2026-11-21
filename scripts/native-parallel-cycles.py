#!/usr/bin/env python3
"""Check what RAVEN_CYCLES reports for a program whose threads call the system at once.

--beside names the calls the runtime must answer beside the turn at least half the times they reach it; --guest names those the guest
answers itself, which may reach the runtime only when a change came in between (60 times at most). --lock-under bounds the share of the
calls' service spent waiting for the desktop lock, in percent, over every call reported."""

import argparse
import os
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime")
    parser.add_argument("fixture")
    parser.add_argument("--beside", default="", help="comma separated")
    parser.add_argument("--guest", default="", help="comma separated")
    parser.add_argument("--lock-under", type=float, help="percent of the service")
    args = parser.parse_args()
    # No report comes during the run at this interval: the one the run ends with holds every call.
    result = subprocess.run([args.runtime, "--run-process", "--execution=native", "--timeout-ms=30000", args.fixture],
                            env=os.environ | {"RAVEN_CYCLES": "600"}, capture_output=True, text=True, timeout=90)
    output = result.stdout + result.stderr
    if result.returncode != 0 or "Native entry returned 0 " not in output:
        raise SystemExit(f"the program failed: exit {result.returncode}\n{output}")
    rows = []
    for match in re.finditer(r"^cycles:\s+(\S+)\s+(\d+) calls\s+(\d+) beside\s+boundary\s+(\d+)\s+turn\s+(\d+)\s+service\s+(\d+)"
                             r"\s+return\s+\d+\s+lock\s+(\d+)", output, re.M):
        rows.append((int(match[5]), match[1], int(match[2]), int(match[3]), int(match[6]), int(match[7])))
    if not rows:
        raise SystemExit(f"no calls reported\n{output}")
    for turn, name, calls, beside, service, locked in sorted(rows, reverse=True):
        print(f"{name}: {calls} calls, {beside} beside, turn {turn}, service {service}, lock {locked}")
    reached, beside = {}, {}
    for _, name, calls, answered, _, _ in rows:
        reached[name] = reached.get(name, 0) + calls
        beside[name] = beside.get(name, 0) + answered
    for name in filter(None, args.beside.split(",")):
        if beside.get(name, 0) < reached.get(name, 0) // 2 or not reached.get(name, 0):
            raise SystemExit(f"{name} is answered beside the turn {beside.get(name, 0)} of {reached.get(name, 0)} times\n{output}")
    for name in filter(None, args.guest.split(",")):
        if reached.get(name, 0) > 60:
            raise SystemExit(f"{name} reaches the runtime {reached[name]} times while threads run at once\n{output}")
    service = sum(calls * served for _, _, calls, _, served, _ in rows)
    locked = sum(calls * waited for _, _, calls, _, _, waited in rows)
    share = 100.0 * locked / service if service else 0.0
    print(f"desktop lock: {share:.1f}% of the service")
    if args.lock_under is not None and share >= args.lock_under:
        raise SystemExit(f"the calls waited for the desktop lock {share:.1f}% of their service, {args.lock_under}% at most\n{output}")


main()
