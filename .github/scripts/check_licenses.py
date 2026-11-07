#!/usr/bin/env python3
"""Check that LICENSES.json still describes the tree.

Every tracked file of a Raven guest component under native/guest/ is listed by a component, because those files are MIT
while the rest of native/ is GPL. Every listed path exists, every license is an SPDX identifier or a declared reference,
and each third-party directory under native/upstream/ carries its own notices.

usage: check_licenses.py
"""

import json
import pathlib
import subprocess
import sys

GUEST = "native/guest/"
UPSTREAM = "native/upstream/"
NOTICES = ("LICENSE", "LICENSE.md", "COPYING", "README.md", "provenance.json")
KNOWN = {"GPL-2.0-only", "GPL-3.0-or-later", "MIT", "Unicode-3.0", "blessing", "BSD-3-Clause", "Apache-2.0", "Zlib", "libpng-2.0"}


def main():
    inventory = json.loads(pathlib.Path("LICENSES.json").read_text())
    tracked = set(filter(None, subprocess.check_output(["git", "ls-files", "-z"], text=True).split("\0")))
    references = set(inventory.get("license_references", {}))
    failed = 0

    def fail(text, name="LICENSES.json"):
        nonlocal failed
        failed += 1
        print(f"::error file={name}::{text}")

    listed = set()
    for component in inventory["components"]:
        for path in component.get("paths", ()):
            if path not in tracked and not any(name.startswith(path.rstrip("/") + "/") for name in tracked):
                fail(f"{component['name']} lists {path}, which is not tracked")
            if path.startswith(GUEST):
                listed.add(path.rstrip("/"))
        for key in ("license", "distribution_license", "source_terms"):
            value = component.get(key)
            if value and value not in KNOWN and value not in references:
                fail(f"{component['name']} uses {value}, which is neither a known SPDX identifier nor a license reference")

    def covered(name):
        parts = name.split("/")
        return any("/".join(parts[:end]) in listed for end in range(3, len(parts) + 1))

    for name in sorted(tracked):
        if name.startswith(GUEST):
            if not covered(name):
                fail("not listed by any component of LICENSES.json", name)
        elif name.startswith(UPSTREAM):
            directory = name[len(UPSTREAM):].split("/", 1)[0]
            if not any(f"{UPSTREAM}{directory}/{notice}" in tracked for notice in NOTICES):
                fail(f"imported source has no {', '.join(NOTICES)}", f"{UPSTREAM}{directory}")
    print(f"{len(tracked)} tracked files, {len(listed)} listed paths, {failed} problems")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
