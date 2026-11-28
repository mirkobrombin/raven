#!/usr/bin/env python3
"""Download the JUnit reports of a run's test shards for merge_junit.py, each from the attempt that uploaded it last.

A shard that is run again uploads a second artifact of the same name, and the artifact of the attempt before it stays. The run's result
is the one of the last attempt, so its report is the one to bundle. actions/download-artifact keeps the artifact with the highest ID
instead, and IDs do not follow the order of uploads: it can hand back the report of the attempt that failed.

usage: GH_REPO=OWNER/REPO shard_reports.py RUN DIRECTORY
"""

from datetime import datetime
import io
import json
from pathlib import Path
import subprocess
import sys
import zipfile


def gh(*arguments):
    return subprocess.run(["gh", "api", *arguments], check=True, stdout=subprocess.PIPE).stdout


def latest(artifacts):
    chosen = {}
    for artifact in artifacts:
        name = artifact["name"]
        if name.startswith("tests-") and (name not in chosen or uploaded(artifact) > uploaded(chosen[name])):
            chosen[name] = artifact
    return chosen


def uploaded(artifact):
    return datetime.fromisoformat(artifact["created_at"])


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    run, directory = sys.argv[1], Path(sys.argv[2])
    listed = gh("--paginate", f"repos/{{owner}}/{{repo}}/actions/runs/{run}/artifacts", "--jq", ".artifacts[]")
    chosen = latest(json.loads(line) for line in listed.decode().splitlines() if line)
    if not chosen:
        print(f"run {run} has no test results", file=sys.stderr)
        return 1
    for name, artifact in sorted(chosen.items()):
        data = gh(f"repos/{{owner}}/{{repo}}/actions/artifacts/{artifact['id']}/zip")
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            archive.extractall(directory / name)
        print(f"{name}: artifact {artifact['id']}, uploaded {artifact['created_at']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
