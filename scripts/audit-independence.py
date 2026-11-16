#!/usr/bin/env python3
"""Show what actually runs when Raven executes a Windows program, from the live process table.

While the program runs the script samples /proc every 20 ms and records every process in Raven's tree, the files each one has
mapped or open, its environment and the Unix sockets it holds. It fails when any process, mapped file, descriptor or environment
variable belongs to a Wine or Proton runtime, when a process other than Raven and its bootstrap appears, or when a wineserver
directory for the user changes while the program runs.

usage: audit-independence.py RAVEN PROGRAM [QUERY]
"""

from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
import time

FORBIDDEN = re.compile(r"wine|proton|wineserver|ntdll\.so|winebus|/\.wine", re.IGNORECASE)
ALLOWED_COMM = {"raven", "raven-runtime", "raven-bootstrap"}
QUERY = "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<12000000) SELECT count(*) FROM c;"


def children(pid):
    found = []
    for task in Path(f"/proc/{pid}/task").iterdir():
        try:
            found += [int(value) for value in (task / "children").read_text().split()]
        except OSError:
            pass
    return found


def tree(root):
    pids, pending = [], [root]
    while pending:
        pid = pending.pop()
        pids.append(pid)
        pending += children(pid)
    return pids


def read(path):
    try:
        return Path(path).read_bytes()
    except OSError:
        return b""


def snapshot(pid, seen):
    comm = read(f"/proc/{pid}/comm").decode().strip()
    entry = seen.setdefault(pid, {"comm": comm, "files": set(), "env": set(), "sockets": set(), "exe": ""})
    try:
        entry["exe"] = os.readlink(f"/proc/{pid}/exe")
    except OSError:
        pass
    for line in read(f"/proc/{pid}/maps").decode(errors="replace").splitlines():
        parts = line.split(None, 5)
        if len(parts) == 6 and parts[5].startswith(("/", "[", "memfd")):
            entry["files"].add(parts[5])
    try:
        for fd in Path(f"/proc/{pid}/fd").iterdir():
            target = os.readlink(fd)
            (entry["sockets"] if target.startswith("socket:") else entry["files"]).add(target)
    except OSError:
        pass
    for item in read(f"/proc/{pid}/environ").split(b"\0"):
        name = item.split(b"=", 1)[0].decode(errors="replace")
        if name:
            entry["env"].add(name + "=" + item.split(b"=", 1)[1].decode(errors="replace") if b"=" in item else name)


def wine_state():
    root = Path(f"/tmp/.wine-{os.getuid()}")
    return sorted((str(path), path.stat().st_mtime_ns) for path in root.rglob("*")) if root.exists() else []


def main():
    raven, program = sys.argv[1], sys.argv[2]
    query = sys.argv[3] if len(sys.argv) > 3 else QUERY
    scratch = Path(tempfile.mkdtemp(dir=os.environ.get("TMPDIR")))
    before = wine_state()
    process = subprocess.Popen(
        [raven, "--run-process", "--execution=native", "--timeout-ms=120000", "--stdio=inherit", program, "--", ":memory:"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=os.environ | {"RAVEN_PREFIX": str(scratch)})
    process.stdin.write((query + "\n").encode())
    process.stdin.close()
    seen, deadline = {}, time.time() + 120
    while process.poll() is None and time.time() < deadline:
        for pid in tree(process.pid):
            snapshot(pid, seen)
        time.sleep(0.02)
    output = process.stdout.read().decode(errors="replace").strip()
    after = wine_state()
    problems = []
    if process.returncode != 0:
        problems.append(f"program exited {process.returncode}")
    comms = {entry["comm"] for entry in seen.values()}
    for entry in seen.values():
        if entry["comm"] not in ALLOWED_COMM:
            problems.append(f"unexpected process {entry['comm']}")
        for value in sorted(entry["files"] | entry["env"] | {entry["exe"], entry["comm"]}):
            if FORBIDDEN.search(value):
                problems.append(f"{entry['comm']}: {value}")
    if before != after:
        problems.append("a wineserver directory changed while the program ran")
    print(f"output: {output}")
    print(f"processes seen: {sorted(comms)} ({len(seen)} pids)")
    for pid, entry in sorted(seen.items()):
        libraries = sorted(path for path in entry["files"] if re.search(r"\.so(\.|$)", path))
        print(f"  {entry['comm']}: exe {entry['exe']}, {len(entry['files'])} mapped or open files, {len(libraries)} shared libraries, "
              f"{len(entry['sockets'])} sockets")
        for path in libraries:
            print(f"      {path}")
    print(f"wineserver directory unchanged: {before == after}")
    print("independence check:", "FAILED" if problems else "passed")
    for problem in problems:
        print("  problem:", problem)
    subprocess.run(["rm", "-r", "--", str(scratch)], check=False)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
