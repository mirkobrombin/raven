#!/usr/bin/env python3
"""Download the root certificates Windows trusts for code signing that host certificate bundles leave out.

Every Windows installation carries the Microsoft code signing roots in its root store, and installers check the signatures of the
packages they unpack against them. They come from Microsoft's public certificate repository, each checked against its pinned digest.

usage: provision-roots.py --output DIRECTORY
"""

import argparse
import hashlib
import re
from pathlib import Path
import urllib.request

# The certificates, where Microsoft publishes each and the SHA-256 of its DER bytes, from the table the runtime downloads them by.
TABLE = Path(__file__).resolve().parent.parent / "native/nt/code_signing_roots.inc"
ROOTS = tuple(re.findall(r'^\{"([^"]+)", "([^"]+)", "([0-9a-f]{64})"\},$', TABLE.read_text(), re.M))


def main():
    parser = argparse.ArgumentParser(description="Download the Microsoft code signing roots Windows trusts")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, url, digest in ROOTS:
        target = args.output / name
        if not target.exists() or hashlib.sha256(target.read_bytes()).hexdigest() != digest:
            with urllib.request.urlopen(url) as response:
                data = response.read()
            if hashlib.sha256(data).hexdigest() != digest:
                raise RuntimeError(f"{name} differs from the pinned certificate")
            target.write_bytes(data)
    print(f"roots in {args.output}")


if __name__ == "__main__":
    main()
