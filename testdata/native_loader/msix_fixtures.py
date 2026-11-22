#!/usr/bin/env python3
"""Builds a signed full-trust package from the test executable, using a key made for this test run."""
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib

from appx_fixtures import Writer, block_map


PUBLISHER = "CN=Raven MSIX Test"
INDIRECT_DATA = "1.3.6.1.4.1.311.2.1.4"
MANIFEST = """<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
         xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
         IgnorableNamespaces="uap rescap">
  <Identity Name="Raven.MsixTest" Publisher="{publisher}" Version="1.2.3.4" ProcessorArchitecture="{arch}" />
  <Properties>
    <DisplayName>Raven MSIX test</DisplayName><PublisherDisplayName>Raven</PublisherDisplayName><Logo>Store.png</Logo>
  </Properties>
  <Dependencies><TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0" MaxVersionTested="10.0.19041.0" /></Dependencies>
  <Resources><Resource Language="en-us" /></Resources>
  <Applications>
    <Application Id="Main" Executable="app.exe" EntryPoint="Windows.FullTrustApplication">
      <uap:VisualElements DisplayName="Raven MSIX test" Description="Raven MSIX test" BackgroundColor="transparent"
                          Square150x150Logo="Tile.png" Square44x44Logo="Small.png" />
    </Application>
  </Applications>
  <Capabilities><rescap:Capability Name="runFullTrust" /></Capabilities>
</Package>
"""
TYPES = b"""<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="exe" ContentType="application/octet-stream"/>
  <Default Extension="png" ContentType="image/png"/>
  <Override PartName="/AppxManifest.xml" ContentType="application/vnd.ms-appx.manifest+xml"/>
  <Override PartName="/AppxBlockMap.xml" ContentType="application/vnd.ms-appx.blockmap+xml"/>
  <Override PartName="/AppxSignature.p7x" ContentType="application/vnd.ms-appx.signature"/>
</Types>
"""


def png(size):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    header = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    pixels = (b"\0" + b"\x18\x33\x50" * size) * size
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b"")


def der(tag, value):
    size = len(value)
    if size < 128:
        length = bytes([size])
    else:
        encoded = size.to_bytes((size.bit_length() + 7) // 8, "big")
        length = bytes([128 + len(encoded)]) + encoded
    return bytes([tag]) + length + value


def split_der(data, tag):
    if data[0] != tag:
        raise ValueError(f"unexpected DER tag {data[0]:#x}")
    length = data[1]
    start = 2
    if length & 128:
        start += length & 127
        length = int.from_bytes(data[2:start], "big")
    end = start + length
    if end > len(data):
        raise ValueError("truncated DER value")
    return data[start:end], data[end:]


def appx_signature(cms, indirect):
    # CMS wraps the same signed bytes in an octet string. Authenticode's
    # PKCS #7 ContentInfo carries those bytes as SpcIndirectDataContent.
    outer, _ = split_der(cms, 0x30)
    content_type, outer = split_der(outer, 6)
    signed, _ = split_der(outer, 0xa0)
    signed, _ = split_der(signed, 0x30)
    version, signed = split_der(signed, 2)
    algorithms, signed = split_der(signed, 0x31)
    encapsulated, certificates_and_signers = split_der(signed, 0x30)
    indirect_type, wrapped = split_der(encapsulated, 6)
    octets, _ = split_der(wrapped, 0xa0)
    content, _ = split_der(octets, 4)
    if content != indirect:
        raise ValueError("CMS signed the wrong content")
    content_info = der(0x30, der(6, indirect_type) + der(0xa0, der(0x30, content)))
    signed = der(2, version) + der(0x31, algorithms) + content_info + certificates_and_signers
    return b"PKCX" + der(0x30, der(6, content_type) + der(0xa0, der(0x30, signed)))


def sign(openssl, directory, hashes):
    key = directory / "signer.key"
    certificate = directory / "signer.pem"
    try:
        subprocess.run([openssl, "req", "-new", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "7",
                        "-subj", "/CN=Raven MSIX Test", "-addext", "basicConstraints=critical,CA:FALSE",
                        "-addext", "keyUsage=critical,digitalSignature", "-addext", "extendedKeyUsage=codeSigning",
                        "-keyout", key, "-out", certificate], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        subprocess.run([openssl, "x509", "-in", certificate, "-outform", "DER", "-out", directory / "signer.cer"], check=True)
        sip_id = uuid.UUID("0ac5df4b-ce07-4de2-b76e-23c839a09fd1").bytes_le
        sip = der(0x30, der(2, b"\x01\x01\0\0") + der(4, sip_id) + der(2, b"\0") * 5)
        subject = der(0x30, der(6, bytes.fromhex("2b06010401823702011e")) + sip)
        sha256 = der(0x30, der(6, bytes.fromhex("608648016503040201")) + der(5, b""))
        indirect = subject + der(0x30, sha256 + der(4, hashes))
        signed = subprocess.run([openssl, "cms", "-sign", "-binary", "-nodetach", "-outform", "DER", "-md", "sha256",
                                 "-econtent_type", INDIRECT_DATA, "-signer", certificate, "-inkey", key],
                                input=indirect, check=True, stdout=subprocess.PIPE).stdout
        return appx_signature(signed, indirect)
    finally:
        key.unlink(missing_ok=True)
        certificate.unlink(missing_ok=True)


def package(executable, arch, directory, openssl):
    writer = Writer()
    files = []
    manifest = MANIFEST.format(publisher=PUBLISHER, arch=arch).encode()
    for name, payload in (("app.exe", executable), ("Store.png", png(50)), ("Tile.png", png(150)),
                          ("Small.png", png(44)), ("AppxManifest.xml", manifest)):
        header, sizes = writer.add(name, payload, False)
        files.append((name, payload, header, sizes))
    blocks = block_map(files)
    writer.add("AppxBlockMap.xml", blocks, False)
    writer.add("[Content_Types].xml", TYPES, False)
    records = bytes(writer.data)
    unsigned = writer.finish()
    hashes = b"APPX"
    for tag, data in ((b"AXPC", records), (b"AXCD", unsigned[len(records):]), (b"AXCT", TYPES), (b"AXBM", blocks)):
        hashes += tag + hashlib.sha256(data).digest()
    writer.data = bytearray(records)
    writer.add("AppxSignature.p7x", sign(openssl, directory, hashes), False)
    signed = writer.finish()
    (directory / "app.msix").write_bytes(signed)
    tampered = bytearray(signed)
    tampered[files[0][2]] ^= 1
    (directory / "app-tampered.msix").write_bytes(tampered)


def run_test(raven, client, directory):
    with tempfile.TemporaryDirectory(prefix="prefix-", dir=directory) as prefix:
        env = dict(os.environ, RAVEN_PREFIX=prefix)
        command = [raven, "--run-process", "--execution=native", "--stdio=inherit", "--timeout-ms=40000", client]
        for trusted in (False, True):
            if trusted:
                certificate = (directory / "signer.cer").read_bytes()
                trust = Path(prefix) / "msix-trusted-publishers"
                trust.mkdir()
                (trust / (hashlib.sha256(certificate).hexdigest() + ".cer")).write_bytes(certificate)
            arguments = command if trusted else [*command, "--", "--untrusted"]
            result = subprocess.run(arguments, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            print(result.stdout, end="")
            entries = re.findall(r"Native entry returned (\d+) ", result.stdout)
            if result.returncode or not entries or entries[-1] != "0":
                return 1
    return 0


if __name__ == "__main__":
    if sys.argv[1] == "--run":
        raven, client, directory = sys.argv[2:]
        sys.exit(run_test(raven, client, Path(directory)))
    executable, architecture, output, openssl = sys.argv[1:]
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    package(Path(executable).read_bytes(), architecture, output, openssl)
