#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import urllib.request
import zipfile


# The SQLite release winsqlite3.dll is built from: the amalgamation, public domain. sqlite.org publishes SHA3-256 hashes.
VERSION = "3540100"
URL = f"https://www.sqlite.org/2026/sqlite-amalgamation-{VERSION}.zip"
SHA3_256 = "92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30"
# Windows' own build of SQLite: the public interface under its unsuffixed names, threads serialized, memory from the Win32 heap.
DEFINES = ("-DSQLITE_API=__declspec(dllexport)", "-DSQLITE_OS_WIN=1", "-DSQLITE_THREADSAFE=1", "-DSQLITE_WIN32_MALLOC",
           "-DSQLITE_OMIT_LOAD_EXTENSION")


def main():
    parser = argparse.ArgumentParser(description="Build the pinned SQLite release as winsqlite3.dll")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--archive", type=Path, help="a downloaded copy of the amalgamation archive")
    args = parser.parse_args()
    # Only the 64-bit library: Windows' 32-bit one takes the standard calling convention for its interface and callbacks, which the
    # published amalgamation does not declare.
    triple = "x86_64-w64-mingw32"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(output), TMP=str(output), TEMP=str(output))
    archive = args.archive.resolve() if args.archive else output / f"sqlite-amalgamation-{VERSION}.zip"
    if not archive.exists():
        urllib.request.urlretrieve(URL, archive)
    if hashlib.sha3_256(archive.read_bytes()).hexdigest() != SHA3_256:
        raise RuntimeError("SQLite amalgamation checksum differs")
    source = output / "source"
    shutil.rmtree(source, ignore_errors=True)
    source.mkdir()
    with zipfile.ZipFile(archive) as upstream:
        for member in upstream.infolist():
            name = Path(member.filename)
            if member.is_dir() or name.name not in ("sqlite3.c", "sqlite3.h", "sqlite3ext.h"):
                continue
            (source / name.name).write_bytes(upstream.read(member))
    if not shutil.which(triple + "-gcc"):
        raise RuntimeError(f"required SQLite build tool is missing: {triple}-gcc")
    component = output / "winsqlite3.dll"
    command = [triple + "-gcc", "-O2", "-DNDEBUG", *DEFINES, "-shared", "-static-libgcc",
               "-Wl,--no-insert-timestamp,--image-base,0x1b8000000",
               str(source / "sqlite3.c"), "-o", str(component)]
    subprocess.run(command, env=env, check=True)
    subprocess.run([triple + "-strip", "--strip-unneeded", str(component)], env=env, check=True)
    imports = subprocess.run([triple + "-objdump", "-p", str(component)], env=env, check=True, capture_output=True, text=True).stdout
    (output / "imports.txt").write_text(imports)
    (output / "provenance.json").write_text(json.dumps(dict(
        project="SQLite", version=VERSION, url=URL, archive_sha3_256=SHA3_256, license="public domain (https://www.sqlite.org/copyright.html)",
        command=command, component_sha256=hashlib.sha256(component.read_bytes()).hexdigest()), indent=2) + "\n")
    print(component)


if __name__ == "__main__":
    main()
