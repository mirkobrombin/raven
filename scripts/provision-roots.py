#!/usr/bin/env python3
"""Download the root certificates Windows trusts for code signing that host certificate bundles leave out.

Every Windows installation carries the Microsoft code signing roots in its root store, and installers check the signatures of the
packages they unpack against them. They come from Microsoft's public certificate repository, each checked against its pinned digest.

usage: provision-roots.py --output DIRECTORY
"""

import argparse
import hashlib
from pathlib import Path
import urllib.request

# The certificate, where Microsoft publishes it, and the SHA-256 of its DER bytes.
ROOTS = (
    ("MicrosoftRootAuthority.crt", "http://www.microsoft.com/pki/certs/MicrosoftRootAuthority.crt",
     "f38406e540d7a9d90cb4a9479299640ffb6df9e224ecc7a01c0d9558d8dad77d"),
    ("MicrosoftRootCert.crt", "http://www.microsoft.com/pki/certs/MicrosoftRootCert.crt",
     "885de64c340e3ea70658f01e1145f957fcda27aabeea1ab9faa9fdb0102d4077"),
    ("MicRooCerAut_2010-06-23.crt", "http://www.microsoft.com/pki/certs/MicRooCerAut_2010-06-23.crt",
     "df545bf919a2439c36983b54cdfc903dfa4f37d3996d8d84b4c31eec6f3c163e"),
    ("MicRooCerAut2011_2011_03_22.crt", "http://www.microsoft.com/pki/certs/MicRooCerAut2011_2011_03_22.crt",
     "847df6a78497943f27fc72eb93f9a637320a02b561d0a91b09e87a7807ed7c61"),
    # The root of Microsoft's signing service for other publishers, whose signatures paint.net's setup checks on its own files.
    ("MicrosoftIdentityVerificationRootCA2020.crt",
     "http://www.microsoft.com/pkiops/certs/Microsoft%20Identity%20Verification%20Root%20Certificate%20Authority%202020.crt",
     "5367f20c7ade0e2bca790915056d086b720c33c1fa2a2661acf787e3292e1270"),
)


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
