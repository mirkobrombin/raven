"""VEX-encoded (and, apart, EVEX-encoded) instructions in a PE image's code, by family, reachable from its .pdata function starts, entry point and exports by
direct branches and fall-through (as x87_reach.py), and the cpuid and xgetbv instructions among them. Indirect jumps are not
followed, so the reachable count is a lower bound; the count over the .pdata ranges is printed as an upper bound. An object file or
static library has no .pdata to start from: every instruction counts, which is exact there.

usage: avx_reach.py OBJDUMP READOBJ FILE [--list N]
"""
import bisect
import re
import subprocess
import sys
from collections import Counter

objdump, readobj, image = sys.argv[1:4]
listed = int(sys.argv[5]) if len(sys.argv) > 5 and sys.argv[4] == "--list" else 0
listing = subprocess.run([objdump, "-d", "-M", "intel", image], capture_output=True, text=True).stdout
insns = {}
order = []
line_re = re.compile(r"^\s*([0-9a-f]+):\s((?:[0-9a-f]{2} )+)\s*\t(\S+)\s*(.*)$")
# The encoding: the first byte after the legacy and REX prefixes (c4 or c5 is VEX, 62 EVEX).
PREFIXES = {0x66, 0x67, 0xf0, 0xf2, 0xf3, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65} | set(range(0x40, 0x50))
encoding = {}
member = ""
for line in listing.splitlines():
    if "file format" in line:
        # An archive member: "LIBRARY(MEMBER):\tfile format ...", each starting at offset 0.
        name = line.split("file format")[0].strip().rstrip(":")
        member = name if name.endswith(")") else ""
    m = line_re.match(line)
    if m:
        a = (member, int(m.group(1), 16))
        insns[a] = (m.group(3), m.group(4))
        lead = [int(x, 16) for x in m.group(2).split()]
        lead = [x for x in lead if x not in PREFIXES][:1]
        encoding[a] = "vex" if lead and lead[0] in (0xc4, 0xc5) else "evex" if lead and lead[0] == 0x62 else ""
        order.append(a)

BMI = {"andn", "bextr", "blsi", "blsmsk", "blsr", "bzhi", "mulx", "pdep", "pext", "rorx", "sarx", "shlx", "shrx"}
AVX2_ONLY = re.compile(r"^v(pbroadcast|perm2i128|inserti128|extracti128|permq|permd|permps|permpd|pgather|gather|pmaskmov|psllv|psrlv|"
                       r"psrav|pblendd|broadcasti128)")


def family(a):
    mnemonic, operands = insns[a]
    if encoding[a] == "evex":
        return "EVEX (AVX-512)"
    if encoding[a] != "vex":
        return None
    if mnemonic in BMI:
        return "BMI"
    if re.match(r"^vf(n?)m(add|sub)", mnemonic):
        return "FMA3"
    if mnemonic in ("vcvtph2ps", "vcvtps2ph"):
        return "F16C"
    if AVX2_ONLY.match(mnemonic) or (mnemonic.startswith("vp") and "ymm" in operands and not mnemonic.startswith("vperm2f128")
                                     and not mnemonic.startswith(("vpermil",))):
        return "AVX2 integer"
    return "AVX"


reach_from = list(order)
ranges = []
if image.lower().endswith((".exe", ".dll")):
    unwind = subprocess.run([readobj, "--unwind", image], capture_output=True, text=True).stdout
    ranges = list(zip([int(x, 16) for x in re.findall(r"StartAddress: (?:\S+ )?\(0x([0-9A-F]+)\)", unwind)],
                      [int(x, 16) for x in re.findall(r"EndAddress: (?:\S+ )?\(0x([0-9A-F]+)\)", unwind)]))
    headers = subprocess.run([readobj, "--file-headers", "--coff-exports", image], capture_output=True, text=True).stdout
    base = int(re.search(r"ImageBase: 0x([0-9A-F]+)", headers).group(1), 16)
    reach_from = [("", s) for s, _ in ranges] + [("", base + int(re.search(r"AddressOfEntryPoint: 0x([0-9A-F]+)", headers).group(1), 16))]
    reach_from += [("", base + int(x, 16)) for x in re.findall(r"RVA: 0x([0-9A-F]+)", headers)]
nexts = {a: b for a, b in zip(order, order[1:])}
seen = set()
missing = set()
work = list(reach_from)
while work:
    a = work.pop()
    while a not in seen:
        if a not in insns:
            missing.add(a)
            break
        seen.add(a)
        m, ops = insns[a]
        target = re.match(r"^(0x)?([0-9a-f]+)\b", ops)
        if m.startswith("j") or m == "call" or m.startswith("loop"):
            if target and not ops.startswith("qword") and "ptr" not in ops.split("<")[0]:
                try:
                    work.append((a[0], int(target.group(2), 16)))
                except ValueError:
                    pass
        if m in ("ret", "jmp", "int3", "ud2", "hlt") or m.startswith("rep") and "ret" in ops:
            break
        a = nexts.get(a, ("", -1))


def tally(addresses):
    families = Counter()
    mnemonics = {}
    widths = Counter()
    for a in addresses:
        m, ops = insns[a]
        f = family(a)
        if f:
            families[f] += 1
            mnemonics.setdefault(f, Counter())[m] += 1
            if f not in ("BMI", "EVEX (AVX-512)"):
                widths["ymm" if "ymm" in ops else "xmm"] += 1
    return families, mnemonics, widths


in_range = set()
keys = [a[1] for a in order]
for s, e in ranges:
    i = bisect.bisect_left(keys, s)
    while i < len(order) and order[i][1] < e:
        in_range.add(order[i])
        i += 1
families, mnemonics, widths = tally(seen)
if image.lower().endswith((".exe", ".dll")):
    print(f"{image.split('/')[-1]}: {len(order)} insns, {len(seen)} reachable ({len(missing)} desynced starts), {len(ranges)} pdata ranges")
else:
    print(f"{image.split('/')[-1]}: {len(order)} insns")
if not image.lower().endswith((".exe", ".dll")):
    seen = set(order)
    families, mnemonics, widths = tally(seen)
    print("  (no .pdata: every instruction counts)")
vector = sum(v for k, v in families.items() if k not in ("BMI", "EVEX (AVX-512)"))
print(f"  reachable: vector VEX {vector}, BMI {families['BMI']}, EVEX {families['EVEX (AVX-512)']};", dict(families.most_common()),
      "registers:", dict(widths))
for f, counter in sorted(mnemonics.items()):
    print(f"    {f}: {dict(counter.most_common(40))}")
if ranges:
    upper, _, _ = tally(in_range)
    print("  VEX in pdata ranges:", sum(upper.values()), dict(upper.most_common()))
whole, _, _ = tally(order)
print("  VEX whole linear:", sum(whole.values()), dict(whole.most_common()))
for name in ("cpuid", "xgetbv"):
    sites = sorted(a for a in seen if insns[a][0] == name)
    print(f"  {name} reachable: {len(sites)}", " ".join(f"{a[1]:x}" for a in sites[:20]))
for a in sorted(a for a in seen if family(a))[:listed]:
    print(f"  {a[0]} {a[1]:x} {insns[a][0]} {insns[a][1]}")
