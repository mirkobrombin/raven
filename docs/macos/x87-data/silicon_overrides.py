"""Compares two runs of x87_oracle (x87.py's cases) and writes, for every case where they differ, what the second left: the file
x87_check.h applies to the build's own oracle run, so the tests hold the translator to x86 silicon where Rosetta disagrees with it.

usage: silicon_overrides.py ROSETTA.bin SILICON.bin OUTPUT.txt "HEADER LINE"

One line per case: its number, its text, then field=hex for each field that differs: rN (general registers but RSP and R15), flags
(CF PF AF ZF SF OF), signal, fcw, fsw, ftw (abridged), st0-st7 (80 bits each, ST order as FXSAVE stores them, little-endian bytes)
and bOFFSET (a run of buffer bytes). The x87 instruction and data pointers and the opcode (FXSAVE bytes 6-23, and those fields of
an fnstenv or fnsave image) are left out: they are not compared.
"""

import re
import struct
import sys


def read(path):
    data = open(path, "rb").read()
    count = struct.unpack_from("<I", data)[0]
    at = 4
    cases = []
    for _ in range(count):
        fields = []
        for _ in range(3):
            length = struct.unpack_from("<I", data, at)[0]
            fields.append(data[at + 4:at + 4 + length])
            at += 4 + length
        cases.append((fields[1].decode(), data[at + 912:at + 1824]))
        at += 1824
    return cases


def fields(io, text):
    result = {}
    for r in range(16):
        if r not in (4, 15):
            result[f"r{r}"] = io[8 * r:8 * r + 8]
    result["flags"] = struct.pack("<Q", struct.unpack_from("<Q", io, 128)[0] & 0x8D5)
    result["signal"] = io[136:144]
    fx = io[144:656]
    result["fcw"], result["fsw"], result["ftw"] = fx[0:2], fx[2:4], fx[4:5]
    for i in range(8):
        result[f"st{i}"] = fx[32 + 16 * i:42 + 16 * i]
    return result


def buffer(io, text):
    data = bytearray(io[656:912])
    match = re.search(r"(fnstenv|fnsave) \[r15(?: \+ (\d+))?\]", text)
    if match:
        at = int(match.group(2) or 0)
        data[at + 12:at + 24] = bytes(12)
        data[at + 24:at + 26] = bytes(2)
    return bytes(data)


def main():
    rosetta, silicon, output, header = sys.argv[1:5]
    lines = [f"# {header}"]
    for number, ((text, a), (other, b)) in enumerate(zip(read(rosetta), read(silicon))):
        assert text == other
        parts = [f"{name}={b_value.hex()}" for (name, a_value), b_value in zip(fields(a, text).items(), fields(b, text).values())
                 if a_value != b_value]
        ba, bb = buffer(a, text), buffer(b, text)
        at = 0
        while at < len(ba):
            if ba[at] == bb[at]:
                at += 1
                continue
            end = at
            while end < len(ba) and ba[end] != bb[end]:
                end += 1
            parts.append(f"b{at}={bb[at:end].hex()}")
            at = end
        if parts:
            lines.append("\t".join([str(number), text] + parts))
    open(output, "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
