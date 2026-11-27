#!/usr/bin/env python3
"""Provision the pinned VST 3 interface headers the plugin bridge is built against.

Only Steinberg's pluginterfaces tree is taken, the ABI a VST 3 host and plugin share, under its MIT license; the SDK's helper classes are left
out. The tree lands at OUTPUT/pluginterfaces, so its own includes ("pluginterfaces/base/funknown.h") resolve with OUTPUT on the path."""

import argparse
from pathlib import Path, PurePosixPath
import shutil
import tarfile

import downloads

VERSION = "3.8.1_build_84"
SHA256 = "19e88e225b6ecb4a3c429d3ff2fd511be9c6c8b8b1c7e86b05949b45c0553e2d"
SIZE = 111430
URL = f"https://github.com/steinbergmedia/vst3_pluginterfaces/archive/refs/tags/v{VERSION}.tar.gz"


def provision(output):
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f"vst3-pluginterfaces-{VERSION}-source.tar.gz"
    downloads.fetch(URL, archive, SHA256, "VST 3 interface headers", size=SIZE)
    tree = output / "pluginterfaces"
    if tree.exists():
        shutil.rmtree(tree)
    count = 0
    with tarfile.open(archive) as source:
        for member in source:
            parts = PurePosixPath(member.name).parts[1:]
            if not member.isfile() or not parts:
                continue
            if ".." in parts or PurePosixPath(member.name).is_absolute():
                raise RuntimeError(f"invalid archive path {member.name}")
            if parts[0] == "test" or not (parts[-1].endswith((".h", ".cpp")) or parts == ("LICENSE.txt",)):
                continue
            target = tree.joinpath(*parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.extractfile(member).read())
            count += 1
    if not (tree / "base/funknown.h").is_file() or not (tree / "LICENSE.txt").is_file():
        raise RuntimeError("the archive lacks base/funknown.h or LICENSE.txt")
    (output / "version.txt").write_text(VERSION + "\n")
    print(f"vst3 pluginterfaces {VERSION}: {count} files")


def main():
    parser = argparse.ArgumentParser(description="Provision the pinned VST 3 interface headers")
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        provision(arguments.output.resolve())
    except (OSError, RuntimeError, tarfile.TarError) as error:
        parser.exit(1, f"VST 3 provisioning failed: {error}\n")


if __name__ == "__main__":
    main()
