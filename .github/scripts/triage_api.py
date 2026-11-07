#!/usr/bin/env python3
"""Tell an issue reporter which of the Windows functions they name the main branch already answers.

Reports often list the functions a program stopped at. A function counts as answered when the host registers a handler
for it, or when a Raven guest DLL exports it; a name that only appears in the export tables is not, because the call still
stops. Only functions answered after the issue was opened are listed, each with the commit that added it, so the reporter
knows which build to retest.

The issue text arrives in ISSUE_TITLE and ISSUE_BODY, its creation time in ISSUE_CREATED; the text is only ever matched
against, never run. The comment, if any, is
written to the path given; nothing is written when no named function is answered.

usage: triage_api.py OUTPUT
"""

import os
import re
import subprocess
import sys
from datetime import datetime

MARKER = "<!-- answered-functions -->"
# Names in code spans, and names after the "api" prefix of a traced call.
CANDIDATES = (re.compile(r"`(_{0,2}[A-Za-z][A-Za-z0-9_]{2,63})`"), re.compile(r"\bapi\s+(_{0,2}[A-Za-z][A-Za-z0-9_]{2,63})\s*\("))
LIMIT = 60


def git(*arguments):
    return subprocess.run(["git", *arguments], text=True, capture_output=True).stdout


def names(text):
    found = []
    for pattern in CANDIDATES:
        for name in pattern.findall(text):
            # Windows functions are CamelCase or CRT names with an underscore; plain words in code spans are left out.
            if (re.search(r"[a-z][A-Z]|^[A-Z][a-z]+[A-Z]", name) or name.startswith("_")) and name not in found:
                found.append(name)
    return found[:LIMIT]


def answer(name):
    """The file and commit that answer a function, or None."""
    handler = f'registry.add("{name}"'
    files = git("grep", "-l", "-F", handler, "--", "native").split()
    if files:
        return files[0], handler
    exported = git("grep", "-l", "-w", "-E", rf"^\s*{name}(\s|=|@|$)", "--", "native/guest/*.def", "native/guest/*exports.txt").split()
    defined = git("grep", "-l", "-E", rf"^(CRT|EXPORT|__declspec\(dllexport\))[^(]*\b{name}\s*\(", "--", "native/guest").split()
    if exported or defined:
        return (exported or defined)[0], name
    return None


def main():
    text = os.environ.get("ISSUE_TITLE", "") + "\n" + os.environ.get("ISSUE_BODY", "")
    opened = datetime.fromisoformat(os.environ.get("ISSUE_CREATED", "1970-01-01T00:00:00Z").replace("Z", "+00:00")).timestamp()
    rows = []
    for name in names(text):
        found = answer(name)
        if not found:
            continue
        path, needle = found
        commit = git("log", "--reverse", "--format=%ct %h %as", "-S", needle, "--", path).split("\n", 1)[0].split()
        if len(commit) == 3 and int(commit[0]) > opened:
            rows.append(f"| `{name}` | `{path}` | {commit[1]} {commit[2]} |")
    if not rows:
        return 0
    with open(sys.argv[1], "w") as output:
        output.write(MARKER + "\n")
        output.write("Some of the functions named here were answered on the main branch after this report. "
                     "If you tested an older build, please try again with one that includes these commits:\n\n")
        output.write("| Function | Answered in | Since |\n| --- | --- | --- |\n")
        output.write("\n".join(rows) + "\n\n")
        output.write("This is an automatic check of the names in the report; it does not mean the program works yet.\n")
    print(f"{len(rows)} answered functions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
