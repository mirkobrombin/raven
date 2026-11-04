#!/usr/bin/env python3
"""Check the tracked files for content that does not belong in the repository.

Windows binaries are never committed: they are built from source or provisioned with their notices, as docs/packaging.md
explains. Personal editor and assistant settings stay in each contributor's own configuration. Large files are kept out so
that clones stay small; test data is generated or stored as text.

usage: check_files.py
"""

import pathlib
import struct
import subprocess
import sys

LIMIT = 2 * 1024 * 1024
PERSONAL = ("CLAUDE.md", "AGENTS.md", "GEMINI.md", ".cursorrules", ".windsurfrules")
PERSONAL_DIRECTORIES = (".claude", ".cursor", ".codex", ".aider", ".continue", ".idea", ".vscode")


def portable_executable(path):
    with path.open("rb") as file:
        header = file.read(64)
        if len(header) < 64 or header[:2] != b"MZ":
            return False
        file.seek(struct.unpack_from("<I", header, 0x3C)[0])
        return file.read(4) == b"PE\0\0"


def problems(name):
    path = pathlib.Path(name)
    if path.name in PERSONAL or path.name.startswith(".aider"):
        yield "personal assistant settings stay in the contributor's own configuration"
    if any(part in PERSONAL_DIRECTORIES for part in path.parts[:-1]):
        yield "personal editor and assistant settings stay in the contributor's own configuration"
    if path.is_symlink() or not path.is_file():
        return
    if path.stat().st_size > LIMIT:
        yield f"file is larger than {LIMIT // (1024 * 1024)} MB"
    if portable_executable(path):
        yield "Windows binaries are built from source or provisioned, never committed"


def main():
    names = subprocess.check_output(["git", "ls-files", "-z"], text=True).split("\0")
    failed = 0
    for name in filter(None, names):
        for text in problems(name):
            failed += 1
            print(f"::error file={name}::{text}")
    print(f"{len(names) - 1} files checked, {failed} problems")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
