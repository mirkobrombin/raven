#!/usr/bin/env python3
"""Check what RAVEN_CYCLES reports for threads that call at once: the calls answered beside the turn, and the turn waited for by the rest."""

import os
import re
import subprocess
import sys


def main():
    runtime, fixture = sys.argv[1:3]
    # No report comes during the run at this interval: the one the run ends with holds every call.
    result = subprocess.run([runtime, "--run-process", "--execution=native", "--timeout-ms=20000", fixture],
                            env=os.environ | {"RAVEN_CYCLES": "600"}, capture_output=True, text=True, timeout=60)
    output = result.stdout + result.stderr
    if result.returncode != 0 or "Native entry returned 0 " not in output:
        raise SystemExit(f"the program failed: exit {result.returncode}\n{output}")
    if not re.search(r"^cycles: \d+ calls, \d+ beside the turn, per call boundary \d+ turn \d+ service \d+ return \d+$", output, re.M):
        raise SystemExit(f"no summary of the calls\n{output}")
    rows = {}
    for match in re.finditer(r"^cycles:\s+(\S+)\s+(\d+) calls\s+(\d+) beside\s+boundary\s+(\d+)\s+turn\s+(\d+)\s+service\s+(\d+)", output, re.M):
        rows[match[1]] = {"calls": int(match[2]), "beside": int(match[3]), "turn": int(match[5])}
    directories, events = rows.get("GetCurrentDirectoryW"), rows.get("SetEvent")
    if not directories or directories["calls"] < 1600:
        raise SystemExit(f"GetCurrentDirectoryW is not reported for every call\n{output}")
    # GetCurrentDirectoryW takes the turn: four threads calling it at once wait for it.
    if directories["beside"] or not directories["turn"]:
        raise SystemExit(f"GetCurrentDirectoryW shows no wait for the turn\n{output}")
    for name in ("VirtualAlloc", "VirtualProtect", "VirtualFree", "SetEvent", "CreateEventW", "CloseHandle"):
        row = rows.get(name)
        if not row or row["beside"] < row["calls"] // 2:
            raise SystemExit(f"{name} is not answered beside the turn\n{output}")
    allocations = rows["VirtualAlloc"]
    print(f"GetCurrentDirectoryW waits {directories['turn']} cycles for the turn; VirtualAlloc beside it {allocations['beside']} of "
          f"{allocations['calls']} times, waiting {allocations['turn']} cycles for it; SetEvent {events['beside']} of {events['calls']}")


main()
