#!/usr/bin/env python3
"""Check what RAVEN_CYCLES reports for threads that call USER and GDI at once, each on a window of its own."""

import os
import re
import subprocess
import sys


def main():
    runtime, fixture = sys.argv[1:3]
    # No report comes during the run at this interval: the one the run ends with holds every call.
    result = subprocess.run([runtime, "--run-process", "--execution=native", "--timeout-ms=30000", fixture],
                            env=os.environ | {"RAVEN_CYCLES": "600"}, capture_output=True, text=True, timeout=90)
    output = result.stdout + result.stderr
    if result.returncode != 0 or "Native entry returned 0 " not in output:
        raise SystemExit(f"the program failed: exit {result.returncode}\n{output}")
    rows = []
    for match in re.finditer(r"^cycles:\s+(\S+)\s+(\d+) calls\s+(\d+) beside\s+boundary\s+(\d+)\s+turn\s+(\d+)\s+service\s+(\d+)", output, re.M):
        rows.append((int(match[5]), match[1], int(match[2]), int(match[3]), int(match[6])))
    if not rows:
        raise SystemExit(f"no calls reported\n{output}")
    for turn, name, calls, beside, service in sorted(rows, reverse=True):
        print(f"{name}: {calls} calls, {beside} beside, turn {turn}, service {service}")
    # What the guest reads from the runtime's tables reaches the runtime only when a change came in between: 1200 calls a name.
    reached = {}
    for _, name, calls, _, _ in rows:
        reached[name] = reached.get(name, 0) + calls
    # Changes of window data and properties are answered beside the turn, under the desktop lock.
    beside = {}
    for _, name, calls, answered, _ in rows:
        beside[name] = beside.get(name, 0) + answered
    for name in ("SetWindowLongPtrW", "SetPropW", "RemovePropW", "InvalidateRect", "GetDC", "ReleaseDC",
                 "PeekMessageW"):
        if beside.get(name, 0) < reached.get(name, 0) // 2 or not reached.get(name, 0):
            raise SystemExit(f"{name} is answered beside the turn {beside.get(name, 0)} of {reached.get(name, 0)} times\n{output}")
    for name in ("GetAncestor", "GetPropW", "GetFocus", "GetMenuItemCount", "GetMenuItemID", "GetSubMenu", "PtInRegion", "RectInRegion",
                 "GetRgnBox", "GetObjectW", "GetWindowRect", "GetClientRect", "GetWindowLongPtrW", "SendMessageW"):
        if reached.get(name, 0) > 60:
            raise SystemExit(f"{name} reaches the runtime {reached[name]} times of 1200 while threads run at once\n{output}")


main()
