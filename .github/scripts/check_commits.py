#!/usr/bin/env python3
"""Check the commits of a push or a pull request against CONTRIBUTING.md.

Each subject is `type: describe the change` with a lowercase type from CONTRIBUTING.md. AI tools are never credited with
Co-Authored-By, and a commit that declares assistance carries both Assisted-by and AI-Scope.

usage: check_commits.py BASE HEAD
"""

import re
import subprocess
import sys

TYPES = ("feat", "fix", "docs", "test", "perf", "refactor", "build", "ci", "chore")
SUBJECT = re.compile(r"(?:%s): \S.*" % "|".join(TYPES))
# Tools that are declared with Assisted-by rather than credited as co-authors.
TOOLS = re.compile(r"anthropic|claude|openai|chatgpt|codex|copilot|cursor|gemini|aider|devin|windsurf|tabnine", re.IGNORECASE)
LONG_SUBJECT = 100


def git(*arguments, stdin=None):
    return subprocess.run(["git", *arguments], input=stdin, text=True, capture_output=True, check=True).stdout


def problems(revision):
    parents = git("rev-list", "--parents", "-n", "1", revision).split()[1:]
    message = git("log", "-1", "--format=%B", revision)
    subject = message.split("\n", 1)[0]
    if len(parents) > 1:
        # Merges come from the maintainer or from the merge button; both name what was merged.
        if not subject.startswith(("chore: merge ", "Merge ")):
            yield f"merge subject does not name what was merged: {subject!r}"
    elif not SUBJECT.fullmatch(subject):
        yield f"subject must be `type: describe the change` with one of {', '.join(TYPES)}: {subject!r}"
    elif len(subject) > LONG_SUBJECT:
        print(f"::warning title=Commit {revision[:8]}::subject is longer than {LONG_SUBJECT} characters")

    keys = set()
    for line in git("interpret-trailers", "--parse", stdin=message).splitlines():
        key, _, value = line.partition(":")
        key = key.strip().lower()
        keys.add(key)
        if key == "co-authored-by" and TOOLS.search(value):
            yield f"AI tools are declared with Assisted-by and AI-Scope, not credited as co-authors: {line.strip()!r}"
    if ("assisted-by" in keys) != ("ai-scope" in keys):
        yield "Assisted-by and AI-Scope go together"


def main():
    base, head = sys.argv[1:3]
    # A new branch has no base: only its head is checked.
    if not base.strip("0"):
        revisions = [head]
    else:
        revisions = git("rev-list", "--reverse", f"{base}..{head}").split()
    failed = 0
    for revision in revisions:
        for text in problems(revision):
            failed += 1
            print(f"::error title=Commit {revision[:8]}::{text}")
    print(f"{len(revisions)} commits checked, {failed} problems")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
