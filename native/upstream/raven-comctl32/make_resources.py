#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mirko Brombin
# See LICENSES/MIT.txt.
"""Draws the toolbar images, cursors and the drag arrow of the common controls component, and writes its resource script."""
import struct
import sys
from pathlib import Path

# The color the toolbar makes transparent in a built-in bitmap.
KEY = (192, 192, 192)
INK = (32, 32, 32)
SHADE = (128, 128, 128)
PAPER = (255, 255, 255)
BLUE = (32, 96, 200)
GOLD = (232, 184, 56)
GREEN = (48, 148, 72)
RED = (200, 48, 48)


def box(color, x0, y0, x1, y1):
    return ("box", color, x0, y0, x1, y1)


def line(color, *points):
    return ("line", color, points)


def page(x0=3, y0=1, x1=13, y1=15):
    return [box(INK, x0, y0, x1, y1), box(PAPER, x0 + 1, y0 + 1, x1 - 1, y1 - 1)]


def folder():
    return [box(INK, 1, 4, 15, 14), box(GOLD, 2, 6, 14, 13), box(INK, 2, 3, 7, 5), box(GOLD, 3, 4, 6, 5)]


def writing(y0=5, rows=3):
    return [box(SHADE, 5, y0 + row * 2, 11, y0 + row * 2 + 1) for row in range(rows)]


def arrow_left(color):
    return [line(color, (13, 8), (3, 8)), line(color, (7, 4), (3, 8), (7, 12))]


def arrow_right(color):
    return [line(color, (3, 8), (13, 8)), line(color, (9, 4), (13, 8), (9, 12))]


def lens(x, y):
    return [box(INK, x, y, x + 6, y + 6), box(PAPER, x + 1, y + 1, x + 5, y + 5), line(INK, (x + 5, y + 5), (x + 9, y + 9))]


def star(color):
    return [line(color, (8, 2), (11, 13), (2, 6), (14, 6), (5, 13), (8, 2))]


def tiles(size, gap):
    shapes = []
    for row in range(2):
        for column in range(2):
            x, y = 2 + column * (size + gap), 2 + row * (size + gap)
            shapes.append(box(BLUE, x, y, x + size, y + size))
    return shapes


def rows_with(mark=None):
    shapes = [box(INK, 2, 2 + row * 4, 14, 3 + row * 4) for row in range(4)]
    return shapes + (mark or [])


def sort_mark(color):
    return [line(color, (12, 2), (12, 13)), line(color, (9, 10), (12, 13), (15, 10))]


def plug(color):
    return [box(INK, 2, 6, 7, 11), box(color, 9, 6, 14, 11), line(INK, (7, 8), (9, 8))]


# The images of a bitmap, in the order of the STD_, VIEW_ and HIST_ constants, on a sixteen pixel grid.
STANDARD = [
    [line(INK, (4, 1), (10, 9)), line(INK, (12, 1), (6, 9)), box(RED, 3, 9, 7, 14), box(RED, 9, 9, 13, 14)],
    page(2, 1, 10, 11) + page(6, 5, 14, 15),
    [box(INK, 2, 2, 12, 15), box(GOLD, 3, 3, 11, 14)] + page(7, 6, 15, 15),
    [line(BLUE, (13, 12), (13, 6), (3, 6)), line(BLUE, (7, 2), (3, 6), (7, 10))],
    [line(BLUE, (3, 12), (3, 6), (13, 6)), line(BLUE, (9, 2), (13, 6), (9, 10))],
    [line(RED, (3, 3), (13, 13)), line(RED, (13, 3), (3, 13))],
    page() + writing(),
    folder(),
    [box(INK, 2, 2, 14, 14), box(BLUE, 3, 3, 13, 13), box(PAPER, 5, 3, 11, 7), box(SHADE, 5, 10, 11, 13)],
    page(3, 1, 11, 15) + lens(6, 5),
    [box(INK, 2, 3, 14, 13), box(PAPER, 3, 4, 13, 12), box(SHADE, 4, 6, 12, 7), box(SHADE, 4, 9, 12, 10)],
    [box(BLUE, 5, 2, 11, 4), box(BLUE, 10, 3, 12, 7), box(BLUE, 7, 7, 11, 9), box(BLUE, 7, 11, 9, 13)],
    lens(2, 2),
    page(2, 1, 10, 11) + [line(BLUE, (9, 13), (14, 13)), line(BLUE, (12, 11), (14, 13), (12, 15))],
    [box(INK, 4, 1, 12, 6), box(PAPER, 5, 2, 11, 6), box(INK, 1, 6, 15, 12), box(SHADE, 2, 7, 14, 11), box(INK, 4, 10, 12, 15), box(PAPER, 5, 11, 11, 14)],
]
VIEW = [
    tiles(5, 2),
    tiles(3, 4) + [box(INK, 6, 3, 8, 4), box(INK, 12, 3, 14, 4), box(INK, 6, 10, 8, 11), box(INK, 12, 10, 14, 11)],
    [box(BLUE, 2, 2 + row * 4, 4, 4 + row * 4) for row in range(3)] + [box(INK, 6, 3 + row * 4, 14, 4 + row * 4) for row in range(3)],
    rows_with() + [box(SHADE, 8, 1, 9, 15)],
    rows_with(sort_mark(RED)),
    rows_with(sort_mark(BLUE)),
    rows_with(sort_mark(GREEN)),
    rows_with(sort_mark(GOLD)),
    folder() + [line(BLUE, (8, 12), (8, 7)), line(BLUE, (6, 9), (8, 7), (10, 9))],
    plug(GREEN),
    plug(RED) + [line(RED, (3, 3), (13, 13))],
    folder() + [box(GREEN, 10, 1, 14, 2), box(GREEN, 11, 0, 13, 4)],
]
HISTORY = [
    arrow_left(BLUE),
    arrow_right(BLUE),
    star(GOLD),
    star(GOLD) + [box(GREEN, 11, 11, 15, 12), box(GREEN, 12, 10, 14, 15)],
    [box(INK, 3, 2, 4, 14), box(INK, 3, 5, 9, 6), box(INK, 3, 11, 9, 12), box(GOLD, 9, 3, 14, 8), box(GOLD, 9, 9, 14, 14)],
]


def put(pixels, x, y, color):
    if 0 <= y < len(pixels) and 0 <= x < len(pixels[0]):
        pixels[y][x] = color


def draw_stroke(pixels, left, color, start, end, scale):
    x0, y0 = start[0] * scale, start[1] * scale
    x1, y1 = end[0] * scale, end[1] * scale
    steps = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
    for step in range(steps + 1):
        x = x0 + (x1 - x0) * step / steps
        y = y0 + (y1 - y0) * step / steps
        put(pixels, left + int(x), int(y), color)


def draw_image(pixels, left, shapes, size):
    scale = size / 16
    for shape in shapes:
        if shape[0] == "box":
            _, color, x0, y0, x1, y1 = shape
            for y in range(int(y0 * scale), max(int(y1 * scale), int(y0 * scale) + 1)):
                for x in range(int(x0 * scale), max(int(x1 * scale), int(x0 * scale) + 1)):
                    put(pixels, left + x, y, color)
        else:
            _, color, points = shape
            for start, end in zip(points, points[1:]):
                draw_stroke(pixels, left, color, start, end, scale)


def write_strip(path, images, size):
    width = size * len(images)
    pixels = [[KEY] * width for _ in range(size)]
    for index, shapes in enumerate(images):
        draw_image(pixels, index * size, shapes, size)
    stride = (width * 3 + 3) // 4 * 4
    rows = b""
    for row in reversed(pixels):
        data = b"".join(bytes((b, g, r)) for r, g, b in row)
        rows += data + b"\0" * (stride - len(data))
    header = struct.pack("<IiiHHIIiiII", 40, width, size, 1, 24, 0, len(rows), 0, 0, 0, 0)
    path.write_bytes(b"BM" + struct.pack("<IHHI", 14 + len(header) + len(rows), 0, 0, 14 + len(header)) + header + rows)


def monochrome_image(rows):
    """A 32 by 32 cursor image, where X is black, . is white and the rest is see-through."""
    color = bytearray(128)
    mask = bytearray(b"\xff" * 128)
    for y, row in enumerate(rows):
        for x, cell in enumerate(row):
            bit = 0x80 >> (x % 8)
            at = (31 - y) * 4 + x // 8
            if cell in "X.":
                mask[at] &= ~bit & 0xff
            if cell == ".":
                color[at] |= bit
    header = struct.pack("<IiiHHIIiiII", 40, 32, 64, 1, 1, 0, 256, 0, 0, 2, 2)
    return header + bytes((0, 0, 0, 0, 255, 255, 255, 0)) + bytes(color) + bytes(mask)


ARROW = [
    "X",
    "XX",
    "X.X",
    "X..X",
    "X...X",
    "X....X",
    "X.....X",
    "X......X",
    "X.......X",
    "X........X",
    "X.....XXXXX",
    "X..X..X",
    "X.X X..X",
    "XX  X..X",
    "X    X..X",
    "     X..X",
    "      XX",
]


def with_badge(badge):
    rows = [row.ljust(32) for row in ARROW] + [" " * 32] * (32 - len(ARROW))
    for offset, part in enumerate(badge):
        target = rows[17 + offset]
        rows[17 + offset] = target[:10] + part + target[10 + len(part):]
    return rows


def write_cursor(path, rows):
    image = monochrome_image(rows)
    header = struct.pack("<HHH", 0, 2, 1) + struct.pack("<BBBBHHII", 32, 32, 2, 0, 0, 0, len(image), 22)
    path.write_bytes(header + image)


def write_arrow_icon(path):
    """The insertion arrow a drag list draws: a small black arrow on nothing."""
    size = 32
    bitmap = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            inside = 12 <= y <= 24 and abs(x - 15.5) <= (6 if y < 18 else 24 - y)
            bitmap += bytes((0, 0, 0, 255 if inside else 0))
    mask = b"\0" * (4 * size)
    image = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(bitmap) + len(mask), 0, 0, 0, 0) + bytes(bitmap) + mask
    path.write_bytes(struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", size, size, 0, 0, 1, 32, len(image), 22) + image)


STRIPS = {
    120: ("standard_small", STANDARD, 16),
    121: ("standard_large", STANDARD, 24),
    124: ("view_small", VIEW, 16),
    125: ("view_large", VIEW, 24),
    130: ("history_small", HISTORY, 16),
    131: ("history_large", HISTORY, 24),
}
BADGE_BOX = ["XXXXXXXXXXX", "X.........X", "X.........X", "X.........X", "XXXXXXXXXXX"]
BADGE_COPY = ["XXXXXXXXXXX", "X....X....X", "X..XXXXX..X", "X....X....X", "XXXXXXXXXXX"]
BADGE_STOP = ["  XXXXXXX", " X.......X", " X.XXXXX.X", " X.......X", "  XXXXXXX"]
CURSORS = {102: ("move", BADGE_BOX), 103: ("stop", BADGE_STOP), 104: ("copy", BADGE_COPY)}
VERSION_SCRIPT = """1 VERSIONINFO
FILEVERSION 6,10,7601,0
PRODUCTVERSION 6,10,7601,0
FILEOS 0x40004
FILETYPE 2
BEGIN
 BLOCK "StringFileInfo"
 BEGIN
  BLOCK "040904B0"
  BEGIN
   VALUE "FileDescription", "Common Controls"
   VALUE "FileVersion", "6.10.7601.0"
   VALUE "OriginalFilename", "comctl32.dll"
   VALUE "ProductName", "Raven"
  END
 END
 BLOCK "VarFileInfo"
 BEGIN
  VALUE "Translation", 0x409, 1200
 END
END
"""


def main():
    output = Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=True)
    script = []
    for identifier, (name, images, size) in STRIPS.items():
        write_strip(output / f"{name}.bmp", images, size)
        script.append(f'{identifier} BITMAP "{name}.bmp"')
    for identifier, (name, badge) in CURSORS.items():
        write_cursor(output / f"{name}.cur", with_badge(badge))
        script.append(f'{identifier} CURSOR "{name}.cur"')
    write_arrow_icon(output / "insert.ico")
    script.append('501 ICON "insert.ico"')
    (output / "comctl32.rc").write_text("\n".join(script) + "\n" + VERSION_SCRIPT)


if __name__ == "__main__":
    main()
