#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mirko Brombin
# See LICENSES/MIT.txt.
"""Draws the system icons and the OEM bitmaps of the user32 component, and writes the resource script that names them."""
import math
import struct
import sys
from pathlib import Path

CLEAR = (0, 0, 0, 0)
FACE = (240, 240, 240)
DARK = (32, 32, 32)
DIM = (160, 160, 160)
LIGHT = (255, 255, 255)
SHADE = (128, 128, 128)


def icon_image(size, draw):
    pixels = [[CLEAR] * size for _ in range(size)]
    draw(pixels, size)
    rows = b"".join(bytes((b, g, r, a)) for row in reversed(pixels) for r, g, b, a in row)
    mask = b"\0" * (((size + 31) // 32 * 4) * size)
    return struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(rows) + len(mask), 0, 0, 0, 0) + rows + mask


def write_icon(path, draw):
    images = [icon_image(size, draw) for size in (16, 32, 48)]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    entries = b""
    for size, image in zip((16, 32, 48), images):
        entries += struct.pack("<BBBBHHII", size, size, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    path.write_bytes(header + entries + b"".join(images))


def blend(pixels, x, y, color, coverage):
    if coverage <= 0 or not (0 <= y < len(pixels) and 0 <= x < len(pixels)):
        return
    alpha = int(round(255 * min(coverage, 1.0)))
    if alpha >= pixels[y][x][3]:
        pixels[y][x] = (*color, alpha)


def disc(pixels, size, color, scale=1.0):
    radius = size / 2 * scale - 0.5
    middle = (size - 1) / 2
    for y in range(size):
        for x in range(size):
            blend(pixels, x, y, color, radius - math.hypot(x - middle, y - middle) + 0.5)


def bar(pixels, size, color, x0, y0, x1, y1):
    for y in range(int(y0 * size), max(int(y1 * size), int(y0 * size) + 1)):
        for x in range(int(x0 * size), max(int(x1 * size), int(x0 * size) + 1)):
            blend(pixels, x, y, color, 1.0)


def information(pixels, size):
    disc(pixels, size, (32, 96, 200))
    bar(pixels, size, LIGHT, 0.44, 0.22, 0.56, 0.34)
    bar(pixels, size, LIGHT, 0.44, 0.42, 0.56, 0.78)


def question(pixels, size):
    disc(pixels, size, (32, 96, 200))
    bar(pixels, size, LIGHT, 0.34, 0.2, 0.66, 0.3)
    bar(pixels, size, LIGHT, 0.58, 0.2, 0.68, 0.46)
    bar(pixels, size, LIGHT, 0.44, 0.42, 0.68, 0.52)
    bar(pixels, size, LIGHT, 0.44, 0.42, 0.56, 0.62)
    bar(pixels, size, LIGHT, 0.44, 0.7, 0.56, 0.82)


def warning(pixels, size):
    for y in range(size):
        half = (y + 0.5) / size * size / 2
        for x in range(size):
            blend(pixels, x, y, (240, 180, 20), half - abs(x + 0.5 - size / 2) + 0.5)
    bar(pixels, size, DARK, 0.44, 0.34, 0.56, 0.68)
    bar(pixels, size, DARK, 0.44, 0.76, 0.56, 0.88)


def error(pixels, size):
    disc(pixels, size, (200, 40, 40))
    middle = (size - 1) / 2
    limit = size * 0.26
    width = max(size * 0.07, 0.9)
    for y in range(size):
        for x in range(size):
            dx, dy = x - middle, y - middle
            if max(abs(dx), abs(dy)) <= limit:
                blend(pixels, x, y, LIGHT, width - min(abs(dx - dy), abs(dx + dy)) / math.sqrt(2) + 0.5)


def application(pixels, size):
    bar(pixels, size, SHADE[:3], 0.06, 0.12, 0.94, 0.88)
    bar(pixels, size, LIGHT, 0.1, 0.28, 0.9, 0.84)
    bar(pixels, size, (32, 96, 200), 0.1, 0.16, 0.9, 0.26)


def logo(pixels, size):
    disc(pixels, size, (40, 44, 60))
    disc(pixels, size, (120, 170, 240), 0.5)


def write_bitmap(path, width, height, pixels):
    stride = (width * 3 + 3) // 4 * 4
    rows = b""
    for row in reversed(pixels):
        line = b"".join(bytes((b, g, r)) for r, g, b in row)
        rows += line + b"\0" * (stride - len(line))
    header = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(rows), 0, 0, 0, 0)
    path.write_bytes(b"BM" + struct.pack("<IHHI", 14 + len(header) + len(rows), 0, 0, 14 + len(header)) + header + rows)


def glyph_bitmap(path, rows, ink=DARK, sunken=False, width=None):
    """A button face with a glyph drawn in `#`, centred; a sunken one moves it a pixel, as a pressed button does."""
    height = 16
    width = width or 16
    pixels = [[FACE] * width for _ in range(height)]
    shift = 1 if sunken else 0
    top = (height - len(rows)) // 2 + shift
    left = (width - len(rows[0])) // 2 + shift
    for y, row in enumerate(rows):
        for x, character in enumerate(row):
            if character == "#":
                pixels[top + y][left + x] = ink
    write_bitmap(path, width, height, pixels)


def checkboxes(path):
    # Four columns by three rows of 13 pixel cells: check boxes off and on, radio off and on, then their pressed and disabled forms.
    cell = 13
    pixels = [[FACE] * (cell * 4) for _ in range(cell * 3)]
    for row in range(3):
        fill = LIGHT if row == 0 else FACE
        ink = DIM if row == 2 else DARK
        for column in range(4):
            ox, oy = column * cell, row * cell
            round_mark = column >= 2
            for y in range(cell):
                for x in range(cell):
                    distance = math.hypot(x - 6, y - 6)
                    inside = distance <= 5.5 if round_mark else 1 <= x <= 11 and 1 <= y <= 11
                    edge = distance > 4.5 if round_mark else x in (1, 11) or y in (1, 11)
                    if inside:
                        pixels[oy + y][ox + x] = SHADE if edge else fill
            if column == 1:
                for x, y in ((3, 6), (4, 7), (5, 8), (6, 7), (7, 6), (8, 5), (9, 4), (3, 5), (4, 6), (5, 7), (6, 6), (7, 5), (8, 4), (9, 3)):
                    pixels[oy + y][ox + x] = ink
            if column == 3:
                for y in range(cell):
                    for x in range(cell):
                        if math.hypot(x - 6, y - 6) <= 2.5:
                            pixels[oy + y][ox + x] = ink
    write_bitmap(path, cell * 4, cell * 3, pixels)


CLOSE = ["#......#", ".#....#.", "..#..#..", "...##...", "...##...", "..#..#..", ".#....#.", "#......#"]
UP = ["...#...", "..###..", ".#####.", "#######"]
DOWN = list(reversed(UP))
LEFT = ["...#", "..##", ".###", "####", ".###", "..##", "...#"]
RIGHT = ["#...", "##..", "###.", "####", "###.", "##..", "#..."]
MINIMIZE = ["........", "........", "........", "........", "........", "........", "########", "########"]
MAXIMIZE = ["########", "########", "#......#", "#......#", "#......#", "#......#", "#......#", "########"]
RESTORE = ["..######", "..#....#", "######.#", "#....#.#", "#....###", "#....#..", "#....#..", "######.."]
CHECK = [".......#", "......##", "#....##.", "##..##..", ".####...", "..##...."]
GRIP = ["......#", ".....##", "....#.#", "...#..#", "..#...#", ".#....#", "#######"]
TRUETYPE = ["#######", "#..#..#", "...#...", "...#...", "...#...", "...#...", "..###.."]

# The identifiers are the public OBM_ values of winuser.h.
BITMAPS = {
    32767: ("old_close", CLOSE, {}), 32766: ("size", GRIP, {}), 32765: ("old_uparrow", UP, {}), 32764: ("old_dnarrow", DOWN, {}),
    32763: ("old_rgarrow", RIGHT, {}), 32762: ("old_lfarrow", LEFT, {}), 32761: ("btsize", GRIP, {}), 32760: ("check", CHECK, {}),
    32758: ("btncorners", GRIP, {}), 32757: ("old_reduce", MINIMIZE, {}), 32756: ("old_zoom", MAXIMIZE, {}), 32755: ("old_restore", RESTORE, {}),
    32754: ("close", CLOSE, {}), 32753: ("uparrow", UP, {}), 32752: ("dnarrow", DOWN, {}), 32751: ("rgarrow", RIGHT, {}), 32750: ("lfarrow", LEFT, {}),
    32749: ("reduce", MINIMIZE, {}), 32748: ("zoom", MAXIMIZE, {}), 32747: ("restore", RESTORE, {}),
    32746: ("reduced", MINIMIZE, {"sunken": True}), 32745: ("zoomd", MAXIMIZE, {"sunken": True}), 32744: ("restored", RESTORE, {"sunken": True}),
    32743: ("uparrowd", UP, {"sunken": True}), 32742: ("dnarrowd", DOWN, {"sunken": True}), 32741: ("rgarrowd", RIGHT, {"sunken": True}),
    32740: ("lfarrowd", LEFT, {"sunken": True}), 32739: ("mnarrow", RIGHT, {}), 32738: ("combo", DOWN, {}),
    32737: ("uparrowi", UP, {"ink": DIM}), 32736: ("dnarrowi", DOWN, {"ink": DIM}), 32735: ("rgarrowi", RIGHT, {"ink": DIM}),
    32734: ("lfarrowi", LEFT, {"ink": DIM}), 32732: ("trtype", TRUETYPE, {}),
}
# The order the runtime asks for them in: application, warning, question, error, information, logo.
ICONS = {100: ("application", application), 101: ("warning", warning), 102: ("question", question), 103: ("error", error),
         104: ("information", information), 105: ("logo", logo)}


def main():
    output = Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=True)
    script = []
    for identifier, (name, draw) in ICONS.items():
        write_icon(output / f"{name}.ico", draw)
        script.append(f'{identifier} ICON "{name}.ico"')
    for identifier, (name, rows, options) in BITMAPS.items():
        glyph_bitmap(output / f"{name}.bmp", rows, **options)
        script.append(f'{identifier} BITMAP "{name}.bmp"')
    checkboxes(output / "checkboxes.bmp")
    script.append('32759 BITMAP "checkboxes.bmp"')
    (output / "user32.rc").write_text("\n".join(script) + "\n")


if __name__ == "__main__":
    main()
