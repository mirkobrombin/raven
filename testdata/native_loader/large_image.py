#!/usr/bin/env python3
"""Copies a program and extends its file past a gigabyte with a hole and a marker at the end (large_image_client.c), as an installer
carries its payload after the image. The hole takes no disk space where the file system supports sparse files."""
from pathlib import Path
import sys

SIZE = 0x48000000
MARKER = b"raven large end\0"

source, output = Path(sys.argv[1]), Path(sys.argv[2])
output.write_bytes(source.read_bytes())
with output.open("r+b") as file:
    file.seek(SIZE - len(MARKER))
    file.write(MARKER)
