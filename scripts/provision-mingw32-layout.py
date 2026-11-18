#!/usr/bin/env python3
"""Set up an i686 MinGW-w64 compiler for 32-bit Vulkan structure layouts on a system that packages no MinGW toolchain.

The 32-bit Vulkan bridge lays out structures the way an i686 Windows compiler does, by compiling sizeof/offsetof tables and reading
them back (scripts/generate-vulkan-layout32.py). That needs headers and a compiler, never a linker or runtime libraries, so the
system's clang targets i686-w64-mingw32 with the MinGW-w64 headers of a pinned llvm-mingw release. llvm-mingw's own binaries need a
newer libstdc++ than the oldest supported systems have, so only its headers are taken. The result goes to RAVEN_MINGW32_VULKAN.
"""
import argparse
import atexit
import os
from pathlib import Path
import shutil
import tarfile
import tempfile

import downloads


RELEASE = "20261104"
ARCHIVE = f"llvm-mingw-{RELEASE}-ucrt-ubuntu-22.04-x86_64"
URL = f"https://github.com/mstorsjo/llvm-mingw/releases/download/{RELEASE}/{ARCHIVE}.tar.xz"
SHA256 = "919380403de87dca3c581c2053230cd298af5e891286628ba0613b7fed370926"
TARGET = "i686-w64-mingw32"


def main():
    parser = argparse.ArgumentParser(description="Set up an i686 MinGW-w64 compiler for 32-bit Vulkan structure layouts")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    clang, objcopy = shutil.which("clang"), shutil.which("llvm-objcopy")
    if not clang or not objcopy:
        raise SystemExit("clang and llvm-objcopy are needed")
    scratch = tempfile.mkdtemp(prefix=".tmp-", dir=output)
    atexit.register(shutil.rmtree, scratch, True)
    archive = Path(scratch) / f"{ARCHIVE}.tar.xz"
    downloads.fetch(URL, archive, SHA256, "llvm-mingw archive")
    headers = f"{ARCHIVE}/generic-w64-mingw32/include/"
    with tarfile.open(archive) as source:
        source.extractall(scratch, members=[member for member in source if member.name.startswith(headers)], filter="data")
    include = output / "sysroot" / TARGET / "include"
    shutil.rmtree(include, ignore_errors=True)
    shutil.copytree(Path(scratch) / headers, include, symlinks=True)

    # The layout generator finds objcopy beside the compiler by name, as it finds a MinGW toolchain's.
    tools = output / "bin"
    tools.mkdir(exist_ok=True)
    compiler = tools / f"{TARGET}-gcc"
    compiler.write_text(f'#!/bin/sh\nexec "{clang}" --target={TARGET} --sysroot="{output / "sysroot"}" "$@"\n')
    compiler.chmod(0o755)
    link = tools / f"{TARGET}-objcopy"
    if link.is_symlink() or link.exists():
        link.unlink()
    os.symlink(objcopy, link)
    print(f"configure Raven with -DRAVEN_MINGW32_VULKAN={compiler}")


if __name__ == "__main__":
    main()
