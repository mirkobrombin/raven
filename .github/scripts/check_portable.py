#!/usr/bin/env python3
"""Check that host binaries start on the oldest system a release supports.

A binary may ask for no glibc symbol version newer than the one given, and may need only shared libraries whose names every supported
system has: not OpenSSL's, which is libssl.so.3 on one and libssl.so.4 on another.

usage: check_portable.py GLIBC_VERSION BINARY...
"""

import re
import subprocess
import sys

# Shared libraries a supported system may lack under these names: each distribution has OpenSSL under its own major version.
UNAVAILABLE = re.compile(r"^lib(ssl|crypto)\.so")


def version(text):
    return tuple(int(part) for part in text.split("."))


def main():
    limit, binaries = version(sys.argv[1]), sys.argv[2:]
    problems = []
    for binary in binaries:
        symbols = subprocess.run(["objdump", "-T", binary], capture_output=True, text=True, check=True).stdout
        needed = set(re.findall(r"\(GLIBC_([0-9.]+)\)\s+(\S+)", symbols))
        for wanted, name in sorted(needed):
            if version(wanted) > limit:
                problems.append(f"{binary}: {name} needs GLIBC_{wanted}")
        headers = subprocess.run(["objdump", "-p", binary], capture_output=True, text=True, check=True).stdout
        for library in re.findall(r"NEEDED\s+(\S+)", headers):
            if UNAVAILABLE.match(library):
                problems.append(f"{binary}: needs {library}")
    for problem in problems:
        print(problem)
    if problems:
        raise SystemExit(f"{len(problems)} requirements newer than glibc {sys.argv[1]} systems have")
    print(f"{len(binaries)} binaries start on glibc {sys.argv[1]}")


if __name__ == "__main__":
    main()
