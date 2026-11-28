"""Counts where two runs of x87_oracle (x87.py's cases) differ, by what differs: the table of docs/macos/x87.md.

usage: compare_oracles.py ROSETTA.bin SILICON.bin

Only the fields the tests compare count (silicon_overrides.py's fields and buffer). A case counts once under every line it shows,
so the lines add up to more than the cases. Every case counts, transcendental and faulting ones included; the last lines split the
total by family.
"""

import collections
import re
import struct
import sys

sys.path.insert(0, __import__("os").path.dirname(__file__))
from silicon_overrides import buffer, fields, read  # noqa: E402

INDEFINITE = struct.pack("<QH", 0xC000000000000000, 0xFFFF)


def main():
    rosetta, silicon = read(sys.argv[1]), read(sys.argv[2])
    counts = collections.Counter()
    families = collections.Counter()
    for (text, a), (_, b) in zip(rosetta, silicon):
        fa, fb = fields(a, text), fields(b, text)
        differ = [name for name in fa if fa[name] != fb[name]]
        memory = buffer(a, text) != buffer(b, text)
        if not differ and not memory:
            continue
        counts["cases"] += 1
        family = re.sub(r"\d+$", "", text.split()[0])
        families[family] += 1
        fsw_a, fsw_b = struct.unpack("<H", fa["fsw"])[0], struct.unpack("<H", fb["fsw"])[0]
        flipped = fsw_a ^ fsw_b
        top, tags = fsw_b >> 11 & 7, fb["ftw"][0]
        registers = [i for i in range(8) if fa[f"st{i}"] != fb[f"st{i}"]]
        checks = {
            "signal": "signal" in differ,
            "a transcendental instruction (not run by the tests)": re.search(
                r"\b(fsin|fcos|fsincos|fptan|fpatan|f2xm1|fyl2x|fyl2xp1)\b", text),
            "FSW.ES (bit 7)": flipped & 0x80,
            "FSW.B (bit 15)": flipped & 0x8000,
            "FSW.C1 (bit 9)": flipped & 0x200,
            "FSW.C0, C2 or C3": flipped & 0x4500,
            "FSW exception flags (bits 0-6)": flipped & 0x7F,
            "FSW.TOP": flipped & 0x3800,
            "FCW": "fcw" in differ,
            "tag byte": "ftw" in differ,
            "a register (80 bits)": registers,
            "only empty registers, where Rosetta left the indefinite": registers and all(
                not tags >> ((top + i) & 7) & 1 and fa[f"st{i}"] == INDEFINITE for i in registers),
            "general registers or EFLAGS": any(name[0] == "r" or name == "flags" for name in differ),
            "memory": memory,
        }
        for name, hit in checks.items():
            if hit:
                counts[name] += 1
    print(f"{counts.pop('cases', 0)} cases differ in a compared field")
    for name, count in counts.most_common():
        print(f"{count}\t{name}")
    print("by first mnemonic:", ", ".join(f"{name} {count}" for name, count in families.most_common(12)))


if __name__ == "__main__":
    main()
