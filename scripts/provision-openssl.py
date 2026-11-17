#!/usr/bin/env python3
"""Build the pinned OpenSSL release as static libraries, for host binaries that start on any distribution.

Distributions name their OpenSSL libraries by major version (libssl.so.3, libssl.so.4), so a runtime linked against one does not start
where only the other is installed; the portable host links these instead. They read no configuration file: the system's may be written
for another OpenSSL. Trust comes from the runtime's own roots (trust_anchors), not from this build's certificate directory.
"""
import argparse
import atexit
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request


VERSION = "3.5.9"
URL = f"https://github.com/openssl/openssl/releases/download/openssl-{VERSION}/openssl-{VERSION}.tar.gz"
SHA256 = "603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a"


def main():
    parser = argparse.ArgumentParser(description="Build the pinned OpenSSL release as static libraries")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # The tools' temporary files go to a directory of their own, removed on exit, so none of them stays in the output.
    scratch = tempfile.mkdtemp(prefix=".tmp-", dir=output)
    atexit.register(shutil.rmtree, scratch, True)
    env = dict(os.environ, TMPDIR=scratch, TMP=scratch, TEMP=scratch)
    archive = output / f"openssl-{VERSION}-source.tar.gz"
    if not archive.exists():
        urllib.request.urlretrieve(URL, archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError("OpenSSL archive checksum differs")
    with tarfile.open(archive) as source:
        source.extractall(scratch, filter="data")
    tree = Path(scratch) / f"openssl-{VERSION}"
    install = output / "install"
    subprocess.run(["./Configure", "linux-x86_64", "no-shared", "no-module", "no-tests", "no-docs", "no-autoload-config",
                    f"--prefix={install}", "--libdir=lib", f"--openssldir={install}/ssl"],
                   cwd=tree, env=env, check=True, capture_output=True)
    subprocess.run(["make", f"-j{args.jobs}", "build_libs"], cwd=tree, env=env, check=True, capture_output=True)
    subprocess.run(["make", "install_dev"], cwd=tree, env=env, check=True, capture_output=True)
    licenses = output / "licenses" / "openssl"
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copy(tree / "LICENSE.txt", licenses / "LICENSE.txt")
    print(f"configure Raven with -DOPENSSL_USE_STATIC_LIBS=TRUE -DOPENSSL_ROOT_DIR={install}")


if __name__ == "__main__":
    main()
