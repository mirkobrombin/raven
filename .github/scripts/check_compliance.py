#!/usr/bin/env python3
"""Gate an EUProvGuard scan.

EUProvGuard exits 0 even when it reports findings, so the gate reads its output. A scan whose summary cannot be parsed, or
that ran without the live vulnerability databases, fails: an unverifiable scan is not a pass. Every CVE fails. A source
finding fails unless .github/compliance/accepted.json lists it with the reason it was reviewed and kept; findings are matched
by rule, file and the text of the line, so they survive unrelated edits above them but not a change to the line itself.

usage: check_compliance.py SBOM LOG [ACCEPTED]
"""

import json
import os
import pathlib
import re
import sys


def summary(log, pattern):
    found = re.findall(pattern, log)
    return int(found[-1]) if found else None


def findings(sbom, root):
    for item in sbom.get("vulnerabilities") or ():
        detail = dict(line.split(": ", 1) for line in item.get("detail", "").splitlines() if ": " in line)
        location = detail.get("Location", "")
        path, _, number = location.rpartition(":")
        if not path or not number.isdigit() or not item.get("id", "").startswith("SAST-"):
            yield dict(id=item.get("id"), kind="vulnerability", description=item.get("description", ""))
            continue
        path = os.path.relpath(path, root)
        try:
            text = pathlib.Path(root, path).read_text(errors="replace").splitlines()[int(number) - 1].strip()
        except (OSError, IndexError):
            text = ""
        yield dict(id=item["id"], kind="source", rule=detail.get("Rule", ""), path=path, line=int(number), text=text,
                   description=item.get("description", ""))


def main():
    sbom_path, log_path = sys.argv[1:3]
    accepted_path = sys.argv[3] if len(sys.argv) > 3 else ".github/compliance/accepted.json"
    log = pathlib.Path(log_path).read_text(errors="replace")
    sbom = json.loads(pathlib.Path(sbom_path).read_text())
    accepted = json.loads(pathlib.Path(accepted_path).read_text())["accepted"]
    failed = 0

    def fail(text, **location):
        nonlocal failed
        failed += 1
        where = ",".join(f"{key}={value}" for key, value in location.items())
        print(f"::error {where}::{text}" if where else f"::error::{text}")

    cves = summary(log, r"CVE findings: (\d+)")
    sast = summary(log, r"Native SAST completed: found (\d+) issues")
    print(f"cve={cves if cves is not None else 'missing'} sast={sast if sast is not None else 'missing'}")
    if cves is None or sast is None:
        fail("EUProvGuard summary could not be parsed")
    if "OSV/EUVD query failed" in log:
        fail("vulnerability databases were unreachable")

    used = set()
    for finding in findings(sbom, os.getcwd()):
        if finding["kind"] == "vulnerability":
            fail(f"{finding['id']}: {finding['description']}")
            continue
        key = (finding["rule"], finding["path"], finding["text"])
        match = next((index for index, entry in enumerate(accepted) if (entry["rule"], entry["path"], entry["text"]) == key), None)
        if match is None:
            fail(f"{finding['rule']}: {finding['description']}", file=finding["path"], line=finding["line"])
        else:
            used.add(match)
    for index, entry in enumerate(accepted):
        if index not in used:
            print(f"::warning file={accepted_path}::no longer reported, remove it: {entry['rule']} {entry['path']}: {entry['text']}")
    print(f"{len(used)} accepted findings, {failed} problems")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
