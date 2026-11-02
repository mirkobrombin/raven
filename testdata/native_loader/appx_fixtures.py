#!/usr/bin/env python3
"""Writes the packages the packaging component's test reads (appx_client.c): a package as package writers make them (each file's blocks
deflated with a flush between them, data descriptors after the data, a block map with each block's SHA-256), the same with its ZIP
records in their ZIP64 forms, one whose stored content differs from its block map, one with a part the block map does not list, and a bundle."""
import base64
import hashlib
from pathlib import Path
import struct
import sys
import zlib

BLOCK = 65536
PUBLISHER = "CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US"
MANIFEST = f"""<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
         xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
         IgnorableNamespaces="uap rescap">
  <Identity Name="Raven.Test" Publisher="{PUBLISHER}" Version="1.2.3.4" ProcessorArchitecture="x64" />
  <Properties>
    <DisplayName>Raven test package</DisplayName>
    <PublisherDisplayName>Raven</PublisherDisplayName>
    <Logo>Assets\\Logo.png</Logo>
  </Properties>
  <Dependencies>
    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0" MaxVersionTested="10.0.19041.0" />
    <PackageDependency Name="Microsoft.VCLibs.140.00.UWPDesktop" Publisher="{PUBLISHER}" MinVersion="14.0.24217.0" />
  </Dependencies>
  <Resources><Resource Language="en-us" /></Resources>
  <Capabilities>
    <Capability Name="internetClient" />
    <rescap:Capability Name="runFullTrust" />
  </Capabilities>
  <Applications>
    <Application Id="App" Executable="raven.exe" EntryPoint="Windows.FullTrustApplication">
      <uap:VisualElements DisplayName="Raven" Description="Raven test" BackgroundColor="transparent"
                          Square150x150Logo="Assets\\Logo.png" Square44x44Logo="Assets\\Logo.png" />
    </Application>
  </Applications>
</Package>
"""
TYPES = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
         '<Default Extension="png" ContentType="image/png"/><Default Extension="db" ContentType="application/octet-stream"/>'
         '<Default Extension="txt" ContentType="text/plain"/><Default Extension="xml" ContentType="application/vnd.ms-appx.manifest+xml"/>'
         '<Override PartName="/AppxBlockMap.xml" ContentType="application/vnd.ms-appx.blockmap+xml"/></Types>')


def index_content():
    """Three blocks' worth of bytes the test computes the same way (appx_client.c's pattern())."""
    return bytes(((i % 251) ^ ((i >> 9) & 0xff)) & 0xff for i in range(2 * BLOCK + 12345))


def deflate_blocks(data):
    """The blocks of a file deflated as a package writer does: a full flush after each, so each block starts on its own."""
    compressor = zlib.compressobj(6, zlib.DEFLATED, -15)
    out, sizes = b"", []
    for offset in range(0, len(data), BLOCK):
        piece = compressor.compress(data[offset:offset + BLOCK]) + compressor.flush(zlib.Z_FULL_FLUSH)
        out += piece
        sizes.append(len(piece))
    return out + compressor.flush(zlib.Z_FINISH), sizes


def part_name(name):
    """The ZIP name of a file: '/' separated, percent-encoded as OPC part names are."""
    return "".join(c if c.isalnum() or c in "-._~/[]" else "".join(f"%{b:02X}" for b in c.encode()) for c in name.replace("\\", "/"))


class Writer:
    def __init__(self, zip64=False):
        self.zip64 = zip64
        self.data = bytearray()
        self.central = []

    def add(self, name, content, compress, raw=None):
        """Adds a part; `raw` replaces the stored bytes (a part whose data differs from what the block map says)."""
        zip_name = part_name(name).encode()
        if compress:
            stored, sizes = deflate_blocks(content)
        else:
            stored, sizes = content, []
        if raw is not None:
            stored = raw
        method = 8 if compress else 0
        crc = zlib.crc32(raw if raw is not None and not compress else content) & 0xffffffff
        offset = len(self.data)
        extra = struct.pack("<HHQQ", 1, 16, 0, 0) if self.zip64 else b""
        # Sizes and CRC follow the data in a data descriptor (general purpose bit 3).
        self.data += struct.pack("<IHHHHHIIIHH", 0x04034b50, 45 if self.zip64 else 20, 8, method, 0, 0, 0,
                                 0xffffffff if self.zip64 else 0, 0xffffffff if self.zip64 else 0, len(zip_name), len(extra)) + zip_name + extra
        header = 30 + len(zip_name) + len(extra)
        self.data += stored
        if self.zip64:
            self.data += struct.pack("<IIQQ", 0x08074b50, crc, len(stored), len(content))
        else:
            self.data += struct.pack("<IIII", 0x08074b50, crc, len(stored), len(content))
        self.central.append((zip_name, method, crc, len(stored), len(content), offset))
        return header, sizes

    def finish(self):
        start = len(self.data)
        for zip_name, method, crc, compressed, size, offset in self.central:
            if self.zip64:
                extra = struct.pack("<HHQQQ", 1, 24, size, compressed, offset)
                fields = (0xffffffff, 0xffffffff, 0xffffffff)
            else:
                extra = b""
                fields = (compressed, size, offset)
            self.data += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 45, 45 if self.zip64 else 20, 8, method, 0, 0, crc, fields[0], fields[1],
                                     len(zip_name), len(extra), 0, 0, 0, 0, fields[2]) + zip_name + extra
        size = len(self.data) - start
        count = len(self.central)
        if self.zip64:
            record = len(self.data)
            self.data += struct.pack("<IQHHIIQQQQ", 0x06064b50, 44, 45, 45, 0, 0, count, count, size, start)
            self.data += struct.pack("<IIQI", 0x07064b50, 0, record, 1)
            self.data += struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 0xffff, 0xffff, 0xffffffff, 0xffffffff, 0)
        else:
            self.data += struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, count, count, size, start, 0)
        return bytes(self.data)


def block_map(files):
    out = ['<?xml version="1.0" encoding="UTF-8" standalone="no"?><BlockMap xmlns="http://schemas.microsoft.com/appx/2010/blockmap" '
           'HashMethod="http://www.w3.org/2001/04/xmlenc#sha256">']
    for name, content, header, sizes in files:
        out.append(f'<File Name="{name}" Size="{len(content)}" LfhSize="{header}">')
        for index, offset in enumerate(range(0, len(content), BLOCK)):
            digest = base64.b64encode(hashlib.sha256(content[offset:offset + BLOCK]).digest()).decode()
            out.append(f'<Block Hash="{digest}"' + (f' Size="{sizes[index]}"' if sizes else "") + "/>")
        out.append("</File>")
    out.append("</BlockMap>")
    return "".join(out).encode()


def package(zip64=False, tamper=False, unlisted=False):
    index = index_content()
    writer = Writer(zip64)
    files = []
    content_of = {"Public\\index.db": index, "Assets\\Logo.png": b"\x89PNG\r\n\x1a\n raven", "Docs\\read me.txt": b"hello",
                  "AppxManifest.xml": MANIFEST.encode()}
    for name, compress in (("Public\\index.db", True), ("Assets\\Logo.png", False), ("Docs\\read me.txt", False), ("AppxManifest.xml", True)):
        content = content_of[name]
        raw = None
        if tamper and name in ("Public\\index.db", "Assets\\Logo.png"):
            # A byte of the stored data changes, the block map staying as it was: within the deflated file's second block (its first still
            # reads), and in the stored file, which only the block's hash tells apart.
            stored, sizes = deflate_blocks(content) if compress else (content, [])
            changed = bytearray(stored)
            changed[sizes[0] + 50 if compress else 3] ^= 0x20
            raw = bytes(changed)
        header, sizes = writer.add(name, content, compress, raw)
        files.append((name, content, header, sizes))
    if unlisted:
        writer.add("extra.txt", b"not in the block map", False)
    writer.add("AppxBlockMap.xml", block_map(files), True)
    writer.add("[Content_Types].xml", TYPES.encode(), True)
    return writer.finish()


def bundle():
    writer = Writer()
    manifest = b'<?xml version="1.0" encoding="UTF-8"?><Bundle xmlns="http://schemas.microsoft.com/appx/2013/bundle" SchemaVersion="1.0"/>'
    header, sizes = writer.add("AppxMetadata\\AppxBundleManifest.xml", manifest, True)
    writer.add("AppxBlockMap.xml", block_map([("AppxMetadata\\AppxBundleManifest.xml", manifest, header, sizes)]), True)
    writer.add("[Content_Types].xml", TYPES.encode(), True)
    return writer.finish()


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    (out / "appx-good.msix").write_bytes(package())
    (out / "appx-zip64.msix").write_bytes(package(zip64=True))
    (out / "appx-tampered.msix").write_bytes(package(tamper=True))
    (out / "appx-unlisted.msix").write_bytes(package(unlisted=True))
    (out / "appx-bundle.msixbundle").write_bytes(bundle())
    (out / "appx-manifest.xml").write_bytes(MANIFEST.encode())


if __name__ == "__main__":
    main()
