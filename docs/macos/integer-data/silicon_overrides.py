"""Compares two runs of x86_semantics_oracle (x86_semantics.py's cases), first Rosetta's and then an x86 CPU's, field by field as
x86_semantics_check.h compares them (flags only where the case defines them), prints each run's corpus fingerprints (the `corpus`
line), prints what differs by family and field, and with OUTPUT writes the silicon results file the tests apply: one line per
differing case, its number, its text, rosetta=FNV-1a 64 of Rosetta's compared output (x86_semantics_check.h's fingerprint) and
FIELD=HEX for each field the x86 CPU left otherwise (rN, flags, bOFFSET).

usage: silicon_overrides.py ROSETTA.bin SILICON.bin [OUTPUT.txt "HEADER LINE"]
"""

import struct
import sys
from collections import Counter

IO = 16 * 8 + 8 + 64
FLAGS = 1 | 4 | 16 | 64 | 128 | 2048


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
        defined = struct.unpack_from("<Q", data, at)[0]
        at += 8
        cases.append((fields[0], fields[1].decode(), fields[2].decode(), data[at + IO:at + 2 * IO], defined))
        at += 2 * IO
    return cases


def compared(io):
    data = bytearray(io)
    data[4 * 8:5 * 8] = bytes(8)
    data[15 * 8:16 * 8] = bytes(8)
    data[128:136] = struct.pack("<Q", struct.unpack_from("<Q", data, 128)[0] & FLAGS)
    return bytes(data)


def fingerprint(io):
    value = 0xCBF29CE484222325
    for byte in compared(io):
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def fields(io, defined):
    data = compared(io)
    result = {}
    for r in range(16):
        if r not in (4, 15):
            result[f"r{r}"] = data[8 * r:8 * r + 8]
    result["flags"] = data[128:136]
    result["defined flags"] = struct.pack("<Q", struct.unpack_from("<Q", data, 128)[0] & defined)
    for b in range(0, 64, 16):
        result[f"b{b}"] = data[136 + b:152 + b]
    return result


def corpus(cases, family):
    """x86_semantics_check.h's corpus_fingerprints for one family: FNV-1a 64 over its cases' code and text, and over their fingerprints."""
    generated = results = 0xCBF29CE484222325
    for code, text, case_family, io, _ in cases:
        if case_family != family:
            continue
        for byte in code + text.encode():
            generated = ((generated ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        for byte in struct.pack("<Q", fingerprint(io)):
            results = ((results ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return generated, results


def main():
    rosetta, silicon = read(sys.argv[1]), read(sys.argv[2])
    # The x86 run may hold fewer cases (families added since): those compare as far as it goes, and only families it holds whole
    # get a corpus line.
    complete = {case[2] for case in rosetta} - {case[2] for case in rosetta[len(silicon):]}
    families = sorted(complete)
    for family in families:
        for name, cases in (("first", rosetta), ("second", silicon)):
            generated, results = corpus(cases[:len(silicon)], family)
            print(f"{name}: corpus\t{family}\tgenerated={generated:016x}\trosetta={results:016x}")
    lines = []
    by_field = Counter()
    by_family = Counter()
    examples = {}
    # Both runs must have executed the same bytes: an assembler may encode the same text otherwise (GNU as and clang choose
    # different forms and source orders), and then a difference says nothing about the CPU.
    encodings = [number for number, (a, b) in enumerate(zip(rosetta, silicon)) if a[0] != b[0]]
    print(f"{len(encodings)} cases encoded differently in the two runs (left out)")
    undefined = Counter()
    for number, ((code, text, family, ours, defined), (code2, text2, _, theirs, _)) in enumerate(zip(rosetta, silicon)):
        if code != code2:
            continue
        assert text == text2, f"case {number} differs in text"
        a, b = fields(ours, defined), fields(theirs, defined)
        # Flags x86 leaves undefined may differ between CPUs: counted, not written.
        if a["flags"] != b["flags"] and a["defined flags"] == b["defined flags"]:
            undefined[family] += 1
        differ = [name for name in a if a[name] != b[name] and name not in ("flags", "defined flags")]
        if a["defined flags"] != b["defined flags"]:
            differ.append("flags")
        if not differ:
            continue
        by_family[family] += 1
        for name in differ:
            key = name if not name.startswith(("b", "r")) or name == "flags" else name.rstrip("0123456789")
            by_field[key] += 1
        examples.setdefault(family, []).append((number, text, [(n, a[n].hex(), b[n].hex()) for n in differ]))
        lines.append("\t".join([str(number), text, f"rosetta={fingerprint(ours):016x}"] + [f"{n}={b[n].hex()}" for n in differ]))
    print(f"{len(rosetta)} cases, {len(lines)} differ in a compared field")
    for family, count in sorted(undefined.items()):
        print(f"  {family}: {count} differ only in flags x86 leaves undefined (not written)")
    for family, count in sorted(by_family.items()):
        print(f"  {family}: {count}")
    for name, count in by_field.most_common():
        print(f"  field {name}: {count}")
    for family, items in sorted(examples.items()):
        for number, text, diffs in items[:4]:
            print(f"  e.g. #{number} {text}: " + "; ".join(f"{n} rosetta {x} x86 {y}" for n, x, y in diffs))
    if len(sys.argv) > 4:
        with open(sys.argv[3], "w") as out:
            out.write(sys.argv[4] + "\n")
            for family in families:
                generated, results = corpus(rosetta, family)
                count = sum(1 for case in rosetta if case[2] == family)
                out.write(f"corpus\t{family}\tcases={count}\tgenerated={generated:016x}\trosetta={results:016x}\n")
            out.write("".join(line + "\n" for line in lines))


if __name__ == "__main__":
    main()
