"""x87 instructions in a PE image's code reachable from its .pdata function starts, entry point and exports, by direct branches and
fall-through. Indirect jumps are not followed (switch tables), so this is a lower bound; the linear count over the .pdata ranges is
printed as an upper bound.

usage: x87_reach.py OBJDUMP READOBJ IMAGE
"""
import bisect
import re
import subprocess
import sys
from collections import Counter

objdump, readobj, image = sys.argv[1:4]
listing = subprocess.run([objdump, "-d", "--no-show-raw-insn", "-M", "intel", image], capture_output=True, text=True).stdout
insns = {}
order = []
line_re = re.compile(r"^\s*([0-9a-f]+):\s*\t(\S+)\s*(.*)$")
for line in listing.splitlines():
    m = line_re.match(line)
    if m:
        a = int(m.group(1), 16)
        insns[a] = (m.group(2), m.group(3))
        order.append(a)
nexts = {a: b for a, b in zip(order, order[1:])}
unwind = subprocess.run([readobj, "--unwind", image], capture_output=True, text=True).stdout
ranges = list(zip([int(x, 16) for x in re.findall(r"StartAddress: \(0x([0-9A-F]+)\)", unwind)],
                  [int(x, 16) for x in re.findall(r"EndAddress: \(0x([0-9A-F]+)\)", unwind)]))
headers = subprocess.run([readobj, "--file-headers", "--coff-exports", image], capture_output=True, text=True).stdout
base = int(re.search(r"ImageBase: 0x([0-9A-F]+)", headers).group(1), 16)
starts = [s for s, _ in ranges] + [base + int(re.search(r"AddressOfEntryPoint: 0x([0-9A-F]+)", headers).group(1), 16)]
starts += [base + int(x, 16) for x in re.findall(r"RVA: 0x([0-9A-F]+)", headers)]
X87 = re.compile(r"^(f(?!xsave|xrstor|emms)\w+|wait|fwait)$")
seen = set()
missing = set()
work = list(starts)
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
                    work.append(int(target.group(2), 16))
                except ValueError:
                    pass
        if m in ("ret", "jmp", "int3", "ud2", "hlt") or m.startswith("rep") and "ret" in ops:
            break
        a = nexts.get(a, -1)
reach = Counter(insns[a][0] for a in seen if X87.match(insns[a][0]))
in_range = set()
for s, e in ranges:
    i = bisect.bisect_left(order, s)
    while i < len(order) and order[i] < e:
        in_range.add(order[i])
        i += 1
linear = Counter(insns[a][0] for a in in_range if X87.match(insns[a][0]))
whole = Counter(insns[a][0] for a in order if X87.match(insns[a][0]))
print(f"{image.split('/')[-1]}: {len(order)} insns, {len(seen)} reachable ({len(missing)} desynced starts), {len(ranges)} pdata ranges")
print("  x87 reachable:", sum(reach.values()), dict(reach.most_common()))
print("  x87 in pdata ranges:", sum(linear.values()), dict(linear.most_common(12)))
print("  x87 whole linear:", sum(whole.values()))
for a in sorted(a for a in seen if X87.match(insns[a][0]))[:40]:
    print(f"  {a:x} {insns[a][0]} {insns[a][1]}")
