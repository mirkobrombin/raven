#!/usr/bin/env python3
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request


URL = "https://download.sourceforge.net/libpng/libpng-1.6.59.tar.xz"
SHA256 = "d80dd2a38a37f803cb9b6ac7b14bd6e74ddc3b654780a8380bdf93523fdb4389"


def main():
    parser = argparse.ArgumentParser(description="Build the pinned libpng release as a static library")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(output), TMP=str(output), TEMP=str(output))
    archive = output / "libpng.tar.xz"
    if not archive.exists():
        urllib.request.urlretrieve(URL, archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError("libpng archive checksum differs")
    with tarfile.open(archive) as source:
        source.extractall(output, filter="data")
    build = output / "build"
    install = output / "install"
    build.mkdir(exist_ok=True)
    # GNUInstallDirs picks lib64 on RPM-based distributions; the path printed below names lib.
    subprocess.run(["cmake", "-DCMAKE_BUILD_TYPE=Release", "-DPNG_SHARED=OFF", "-DPNG_TESTS=OFF", "-DPNG_TOOLS=OFF",
                    f"-DCMAKE_INSTALL_PREFIX={install}", "-DCMAKE_INSTALL_LIBDIR=lib", str(output / "libpng-1.6.59")],
                   cwd=build, env=env, check=True, capture_output=True)
    subprocess.run(["cmake", "--build", ".", "--target", "install", "-j2"], cwd=build, env=env, check=True, capture_output=True)
    print(f"configure Raven with -DPNG_LIBRARY_RELEASE={install}/lib/libpng16.a -DPNG_PNG_INCLUDE_DIR={install}/include")


if __name__ == "__main__":
    main()
