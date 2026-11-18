#!/usr/bin/env python3
"""Provision the pinned WebView2 SDK headers the runtime's WebView2 environment is built against.

Only the C headers that define the WebView2 interfaces and the package's license texts are taken from the SDK's NuGet package; its
loader and managed assemblies are left out."""

import argparse
import json
from pathlib import Path, PurePosixPath
import shutil
import zipfile

import downloads


def provision(destination, package):
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination / package["archive"]
    downloads.fetch(package["url"], archive, package["sha256"], "WebView2 SDK package", size=package["size"])
    output = destination / "webview2"
    if output.exists():
        shutil.rmtree(output)
    with zipfile.ZipFile(archive) as source:
        names = set(source.namelist())
        for entry, folder in [(name, "include") for name in package["headers"]] + [(name, "licenses") for name in package["licenses"]]:
            if entry not in names:
                raise RuntimeError(f"{entry} missing from the package")
            path = output / folder / PurePosixPath(entry).name
            path.parent.mkdir(parents=True, exist_ok=True)
            with source.open(entry) as data, path.open("xb") as written:
                shutil.copyfileobj(data, written)
    (output / "version.txt").write_text(package["version"] + "\n")
    print(f'webview2 sdk {package["version"]}: {len(package["headers"]) + len(package["licenses"])} files')


def main():
    parser = argparse.ArgumentParser(description="Provision the pinned WebView2 SDK headers")
    parser.add_argument("--destination", type=Path, required=True)
    arguments = parser.parse_args()
    manifest = Path(__file__).resolve().parent.parent / "native/guest/raven-webview/webview2-sdk.json"
    try:
        provision(arguments.destination.resolve(), json.loads(manifest.read_text()))
    except (OSError, RuntimeError, zipfile.BadZipFile) as error:
        parser.exit(1, f"WebView2 SDK provisioning failed: {error}\n")


if __name__ == "__main__":
    main()
