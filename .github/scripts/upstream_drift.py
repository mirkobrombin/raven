#!/usr/bin/env python3
"""Compare the pinned external inputs with their newest upstream releases.

The pins live in native/graphics/backends.json and the provisioning scripts. A newer release is not adopted automatically:
it changes what Raven ships, so it needs a build, the tests and a license review. This only reports it, as a Markdown table
written to the path given, and exits 0 either way; a failed lookup is reported in the table.

usage: upstream_drift.py OUTPUT
"""

import json
import os
import pathlib
import re
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "raven-upstream-drift"})
    token = os.environ.get("GH_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request, timeout=30) as response:
        return response.read().decode()


def release(repository):
    return json.loads(fetch(f"https://api.github.com/repos/{repository}/releases/latest"))["tag_name"].removeprefix("v")


def stable_tag(repository, prefix):
    tags = [tag["name"] for tag in json.loads(fetch(f"https://api.github.com/repos/{repository}/tags?per_page=100"))]
    versions = [tuple(map(int, tag[1:].split("."))) for tag in tags if re.fullmatch(rf"v{prefix}\.\d+", tag)]
    return ".".join(map(str, max(versions))) if versions else "unknown"


def sqlite():
    found = re.findall(r"sqlite-amalgamation-(\d+)\.zip", fetch("https://www.sqlite.org/download.html"))
    return found[0] if found else "unknown"


def pinned(path, pattern):
    return re.search(pattern, (ROOT / path).read_text()).group(1)


def main():
    backends = json.loads((ROOT / "native/graphics/backends.json").read_text())
    checks = (
        ("DXVK", backends["dxvk"]["version"], lambda: release("doitsujin/dxvk")),
        ("vkd3d-proton", backends["vkd3d-proton"]["version"], lambda: release("HansKristian-Work/vkd3d-proton")),
        ("libpng", pinned("scripts/provision-libpng.py", r"libpng-(\d+\.\d+\.\d+)\.tar"), lambda: stable_tag("pnggroup/libpng", r"1\.6")),
        ("SQLite", pinned("scripts/provision-sqlite.py", r'VERSION = "(\d+)"'), sqlite),
    )
    rows, newer = [], 0
    for name, current, latest in checks:
        try:
            upstream = latest()
        except (OSError, ValueError, KeyError) as error:
            upstream = f"lookup failed: {error}"
        state = "current" if upstream == current else ("**newer**" if not upstream.startswith("lookup") else "unknown")
        newer += state == "**newer**"
        rows.append(f"| {name} | {current} | {upstream} | {state} |")
    with open(sys.argv[1], "w") as output:
        output.write("| Input | Pinned | Upstream | State |\n| --- | --- | --- | --- |\n" + "\n".join(rows) + "\n")
    print(f"{newer} inputs have a newer upstream release")
    return 0


if __name__ == "__main__":
    sys.exit(main())
