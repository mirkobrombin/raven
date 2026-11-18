#!/usr/bin/env python3
"""Provision the pinned Chromium Embedded Framework build the page engine draws pages with.

Only the files the engine needs are taken from the official binary distribution: the runtime, its resources and locales, the C API
headers and the license texts. The distribution's Direct3D shader compiler and Vulkan loader are left out: the first is a Windows
component, the second would hide Raven's own Vulkan bridge."""

import argparse
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile

import downloads


def wanted(name, package):
    """Where an archive entry goes in the output, or None for entries the engine does not use."""
    path = PurePosixPath(name)
    if not path.parts or path.parts[0] != package["archive_root"] or ".." in path.parts:
        return None
    relative = PurePosixPath(*path.parts[1:]).as_posix()
    for entry in package["runtime"]:
        if relative == entry or (entry.endswith("/") and relative.startswith(entry)):
            return PurePosixPath("runtime") / relative.split("/", 1)[1]
    if relative.startswith(package["headers"]):
        return PurePosixPath(relative)
    if relative in package["licenses"]:
        return PurePosixPath("licenses") / relative
    return None


def provision(destination, package):
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination / package["archive"]
    downloads.fetch(package["url"], archive, package["sha256"], "CEF archive", size=package["size"])
    output = destination / "cef"
    if output.exists():
        shutil.rmtree(output)
    found = set()
    with tarfile.open(archive, mode="r:bz2") as source:
        for member in source:
            target = wanted(member.name, package)
            if target is None or not member.isfile():
                continue
            path = output / target
            path.parent.mkdir(parents=True, exist_ok=True)
            with source.extractfile(member) as data, path.open("xb") as written:
                shutil.copyfileobj(data, written)
            found.add(target.as_posix())
    missing = [entry for entry in package["runtime"] if not entry.endswith("/") and ("runtime/" + entry.split("/", 1)[1]) not in found]
    missing += [entry for entry in package["licenses"] if ("licenses/" + entry) not in found]
    if missing or not any(name.startswith("runtime/locales/") for name in found) or not any(name.startswith("include/") for name in found):
        raise RuntimeError(f"files missing from the distribution: {missing}")
    (output / "version.txt").write_text(package["version"] + "\n")
    print(f'cef {package["version"]}: {len(found)} files')


def main():
    parser = argparse.ArgumentParser(description="Provision the pinned CEF runtime for the page engine")
    parser.add_argument("--destination", type=Path, required=True)
    arguments = parser.parse_args()
    manifest = Path(__file__).resolve().parent.parent / "native/guest/raven-webengine/cef.json"
    try:
        provision(arguments.destination.resolve(), json.loads(manifest.read_text()))
    except (OSError, RuntimeError, tarfile.TarError) as error:
        parser.exit(1, f"CEF provisioning failed: {error}\n")


if __name__ == "__main__":
    main()
