#!/usr/bin/env python3
"""Build the image decoders of windowscodecs.dll on the host with image_decoders.c and the sanitizers, and run them on changed files.

The decoders are compiled from their own sources (native/guest/raven-windowscodecs) beside wic/wic.h, which stands in for the
component's Windows headers, with the Wuffs and zlib releases the component is built with (their versions and checksums are those of
scripts/provision-installer.py). The seeds are small files of each format and each case the decoders tell apart, written here.

usage: image_decoders.py [--rounds N] [--work DIRECTORY]
"""

import argparse
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
COMPONENT = ROOT / "native/guest/raven-windowscodecs"
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "scripts"))
import downloads


# The parts of zlib the component inflates with, built as it builds them, without a C library.
ZLIB = ("adler32.c", "crc32.c", "inflate.c", "inffast.c", "inftrees.c", "zutil.c")


def release(name):
    """The UPSTREAM entry of a library in provision-installer.py, read without running the recipe."""
    spec = importlib.util.spec_from_file_location("provision_installer", ROOT / "scripts/provision-installer.py")
    recipe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(recipe)
    return recipe.UPSTREAM[name]


def fetched(name, work):
    """The source archive of a library, downloaded and checked as the recipe does."""
    entry = release(name)
    version = entry["version"]
    return downloads.fetch([url.format(version=version) for url in entry["urls"]], work / f"{name}-{version}.tar.gz",
                           entry["sha256"], f"{name} {version} source archive")


def bmp(width, height, depth, compression=0, pixels=None, colors=0, masks=b""):
    stride = (width * depth + 31) // 32 * 4
    palette = b"".join(bytes((index * 40 % 256, index * 80 % 256, index * 120 % 256, 0)) for index in range(colors))
    data = pixels if pixels is not None else bytes(index * 7 % 256 for index in range(stride * height))
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, depth, compression, len(data), 2835, 2835, colors, 0)
    offset = 14 + len(info) + len(masks) + len(palette)
    return b"BM" + struct.pack("<IHHI", offset + len(data), 0, 0, offset) + info + masks + palette + data


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def literals(data):
    """A zlib stream of one block of literals alone whose distance code has no codes (RFC 1951, 3.2.7), which Wuffs refuses and zlib
    reads: literals 0 to 254 of 8 bits, 255 and the end of 9, their lengths told in a code of 8 in 1 bit and 0 and 9 in 2."""
    bits, count, out = 0, 0, bytearray(b"\x78\x01")

    def put(value, width):
        nonlocal bits, count
        bits |= value << count
        count += width
        while count >= 8:
            out.append(bits & 255)
            bits >>= 8
            count -= 8

    def code(value, width):
        put(int(format(value, f"0{width}b")[::-1], 2), width)

    def literal(symbol):
        code(symbol, 8) if symbol < 255 else code(510 + symbol - 255, 9)

    for value, width in ((1, 1), (2, 2), (0, 5), (0, 5), (3, 4), *((length, 3) for length in (0, 0, 0, 2, 1, 0, 2))):
        put(value, width)
    for symbol in range(257):
        code(0, 1) if symbol < 255 else code(3, 2)
    code(2, 2)
    for byte in data:
        literal(byte)
    literal(256)
    if count:
        put(0, 8 - count)
    return bytes(out) + struct.pack(">I", zlib.adler32(data))


def png(width, height, kind, depth, rows, interlace=0, extra=b"", packed=zlib.compress):
    header = struct.pack(">IIBBBBB", width, height, depth, kind, 0, 0, interlace)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + extra + chunk(b"IDAT", packed(rows)) + chunk(b"IEND", b"")


def lzw(indices, minimum=2):
    """GIF image data that clears the table before every code, so each code keeps the narrowest width."""
    clear, width, bits, count, out = 1 << minimum, minimum + 1, 0, 0, bytearray()

    def put(code):
        nonlocal bits, count
        bits |= code << count
        count += width
        while count >= 8:
            out.append(bits & 255)
            bits >>= 8
            count -= 8

    for index in indices:
        put(clear)
        put(index)
    put(clear + 1)
    if count:
        out.append(bits & 255)
    blocks = b"".join(bytes((len(out[at:at + 255]),)) + out[at:at + 255] for at in range(0, len(out), 255))
    return bytes((minimum,)) + blocks + b"\0"


def icon(entries, size=4):
    directory, body = struct.pack("<HHH", 0, 1, len(entries)), b""
    offset = 6 + 16 * len(entries)
    for entry in entries:
        directory += struct.pack("<BBBBHHII", size, size, 0, 0, 1, 32, len(entry), offset + len(body))
        body += entry
    return directory + body


def seeds(folder):
    rgba = png(3, 2, 6, 8, b"".join(b"\0" + bytes(range(y, y + 12)) for y in range(2)))
    files = {
        "24.bmp": bmp(5, 3, 24),
        "8.bmp": bmp(7, 4, 8, colors=256),
        "4.bmp": bmp(9, 2, 4, colors=16),
        "1.bmp": bmp(6, 2, 1, colors=2),
        "fields.bmp": bmp(4, 4, 32, 3, masks=struct.pack("<III", 0xFF0000, 0xFF00, 0xFF)),
        "rle8.bmp": bmp(8, 3, 8, 1, pixels=bytes((3, 1, 2, 2, 0, 0, 0, 3, 5, 6, 7, 0, 0, 0, 4, 9, 0, 1)), colors=16),
        "rle4.bmp": bmp(8, 2, 4, 2, pixels=bytes((4, 0x12, 0, 0, 0, 4, 0x34, 0x56, 0, 1)), colors=16),
        "rgba.png": rgba,
        "palette.png": png(5, 3, 3, 4, b"".join(b"\0" + bytes((0x12, 0x30, 0x10)) for _ in range(3)),
                           extra=chunk(b"PLTE", bytes(range(12))) + chunk(b"tRNS", b"\x00\x80") +
                           chunk(b"iCCP", b"p\0\0" + zlib.compress(b"profile" * 20))),
        "literals.png": png(16, 16, 3, 8, b"".join(b"\0" + bytes((x * 3 + y) % 4 for x in range(16)) for y in range(16)),
                            extra=chunk(b"PLTE", bytes(range(12))) + chunk(b"tRNS", b"\x00\x80"), packed=literals),
        "gray16.png": png(3, 3, 0, 16, b"".join(b"\0" + bytes(range(y, y + 6)) for y in range(3)), interlace=1),
        "icon.ico": icon([struct.pack("<IiiHHIIiiII", 40, 4, 8, 1, 32, 0, 0, 0, 0, 0, 0) + bytes(range(64)) + bytes(16),
                          struct.pack("<IiiHHIIiiII", 40, 4, 8, 1, 8, 0, 0, 0, 0, 0, 0) + bytes(1024) + bytes(16) + bytes(16),
                          struct.pack("<IHHHH", 12, 4, 8, 1, 24) + bytes(48) + bytes(16), rgba]),
    }
    table = bytes((0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255)) + bytes(12)
    files["frames.gif"] = (b"GIF89a" + struct.pack("<HH", 4, 3) + bytes((0xF2, 0, 0)) + table + bytes((0x21, 0xF9, 4, 1, 10, 0, 2, 0)) +
                           b"\x2c" + struct.pack("<HHHH", 0, 0, 4, 3) + b"\0" + lzw([index % 4 for index in range(12)]) +
                           b"\x2c" + struct.pack("<HHHH", 1, 1, 2, 2) + b"\x81" + bytes(range(12)) + lzw([0, 1, 2, 3]) +
                           b"\x2c" + struct.pack("<HHHH", 0, 0, 4, 4) + b"\x40" + lzw([index % 3 for index in range(16)]) + b";")
    paths = []
    for name, data in files.items():
        (folder / name).write_bytes(data)
        paths.append(folder / name)
    return paths


def main():
    parser = argparse.ArgumentParser(description="Run the image decoders of windowscodecs.dll on changed files under the sanitizers")
    parser.add_argument("--rounds", type=int, default=20000)
    parser.add_argument("--work", type=Path, help="where to build; a temporary directory when not given")
    args = parser.parse_args()
    work = args.work.resolve() if args.work else Path(tempfile.mkdtemp(prefix="image-decoders-"))
    work.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fetched("wuffs", work)) as source:
        member = next(item for item in source.getmembers() if item.name.endswith("/release/c/wuffs-v0.4.c"))
        with source.extractfile(member) as data:
            (work / "wuffs-v0.4.c").write_bytes(data.read())
    zlib_folder = work / "zlib"
    zlib_folder.mkdir(exist_ok=True)
    with tarfile.open(fetched("zlib", work)) as source:
        for member in source.getmembers():
            name = Path(member.name).name
            if member.isfile() and member.name.count("/") == 1 and (name in ZLIB or name.endswith(".h")):
                with source.extractfile(member) as data:
                    (zlib_folder / name).write_bytes(data.read())
    # The decoders include "wic.h" by quotes, which finds the one beside them first: they are built from copies beside the stand-in.
    for name in ("decode.h", "formats.c", "png.c", "wuffs.c", "zlib.c"):
        shutil.copyfile(COMPONENT / name, work / name)
    shutil.copyfile(HERE / "wic/wic.h", work / "wic.h")
    shutil.copyfile(HERE / "image_decoders.c", work / "image_decoders.c")
    folder = work / "seeds"
    folder.mkdir(exist_ok=True)
    program = work / "image_decoders"
    subprocess.run(["gcc", "-g", "-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-w", "-I", str(work),
                    "-I", str(zlib_folder), "-DZ_SOLO", "-o", str(program),
                    *(str(work / name) for name in ("image_decoders.c", "formats.c", "png.c", "wuffs.c", "zlib.c")),
                    *(str(zlib_folder / name) for name in ZLIB)], check=True)
    subprocess.run([str(program), str(args.rounds), *map(str, seeds(folder))], check=True)
    if not args.work:
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
