"""The instructions the translator refuses in code reachable in PE images, grouped by mnemonic and operand form, with the number of
.pdata functions each group appears in: the code paths each gap blocks.

usage: integer_reach.py READOBJ DECODER_TEST CORPUS OUTPUT_DIRECTORY IMAGE...

CORPUS is x86_corpus.py's output for the images (llvm-objdump's text of their code sections). Reachability is as in x87_reach.py and
avx_reach.py: from the .pdata function starts, the entry point and the exports, through direct branches, calls and fall-through
(indirect jumps are not followed, so it is a lower bound). The reachable instructions go to OUTPUT_DIRECTORY/reachable.txt, a corpus
of their own; x86_decoder_test runs on it and on CORPUS with RAVEN_REFUSED set, which lists what translates() refuses. Prints the
linear and reachable totals per image, then the groups ranked by functions. A function runs from a .pdata start, a call target, the
entry point or an export to the next one (leaf functions have no .pdata entry), so funclets and tail-merged code may split or join
functions: the function counts are approximate.
"""

import bisect
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

readobj, decoder_test, corpus_path, out_dir = sys.argv[1:5]
images = sys.argv[5:]
out = Path(out_dir)
out.mkdir(parents=True, exist_ok=True)

# The corpus by image: (address, bytes, text) in address order.
corpus = defaultdict(list)
source = None
for line in open(corpus_path):
    line = line.rstrip("\n")
    if line.startswith("#"):
        source = line[2:]
        continue
    parts = line.split("\t")
    if len(parts) < 2 or not source:
        continue
    corpus[source].append((int(parts[1], 16), parts[0], parts[2] if len(parts) > 2 else ""))

functions = {}
reachable_lines = []
for image in images:
    name = os.path.basename(image)
    insns = corpus[name]
    unwind = subprocess.run([readobj, "--unwind", image], capture_output=True, text=True).stdout
    ranges = sorted(set(zip([int(x, 16) for x in re.findall(r"StartAddress: (?:\S+ )?\(0x([0-9A-F]+)\)", unwind)],
                            [int(x, 16) for x in re.findall(r"EndAddress: (?:\S+ )?\(0x([0-9A-F]+)\)", unwind)])))
    headers = subprocess.run([readobj, "--file-headers", "--coff-exports", image], capture_output=True, text=True).stdout
    base = int(re.search(r"ImageBase: 0x([0-9A-F]+)", headers).group(1), 16)
    starts = [s for s, _ in ranges] + [base + int(re.search(r"AddressOfEntryPoint: 0x([0-9A-F]+)", headers).group(1), 16)]
    starts += [base + int(x, 16) for x in re.findall(r"RVA: 0x([0-9A-F]+)", headers)]
    index = {a: i for i, (a, _, _) in enumerate(insns)}
    seen = set()
    called = set()
    work = list(starts)
    while work:
        a = work.pop()
        while a in index and a not in seen:
            seen.add(a)
            i = index[a]
            text = insns[i][2]
            m = text.split(" ")[0]
            ops = text[len(m):].strip()
            if m.startswith("j") or m == "call" or m.startswith("loop"):
                target = re.match(r"^0x([0-9a-f]+)\b", ops)
                if target:
                    work.append(int(target.group(1), 16))
                    if m == "call":
                        called.add(int(target.group(1), 16))
            if m in ("ret", "jmp", "int3", "ud2", "hlt") or text.startswith("rep ret"):
                break
            a = insns[i + 1][0] if i + 1 < len(insns) else -1
    # A function: from a .pdata start, a call target, the entry point or an export to the next one. Leaf functions have no .pdata
    # entry, so call targets count too.
    keys = sorted(set(starts) | called)
    def function(address, keys=keys):
        i = bisect.bisect_right(keys, address) - 1
        return keys[i] if i >= 0 else None
    functions[name] = function
    reachable_lines.append(f"# {name}")
    reachable_lines += [f"{b}\t{a:x}\t{t}" for a, b, t in insns if a in seen]
    print(f"{name}: {len(insns)} instructions linearly, {len(seen)} reachable, {len(ranges)} .pdata ranges")
(out / "reachable.txt").write_text("\n".join(reachable_lines) + "\n")


def refused(corpus_file, listing):
    env = dict(os.environ, RAVEN_REFUSED=str(listing))
    subprocess.run([decoder_test, str(corpus_file)], env=env, capture_output=True, text=True)
    rows = []
    for line in open(listing):
        image, address, code, text = line.rstrip("\n").split("\t", 3)
        rows.append((image, int(address, 16), code, text))
    return rows


linear = refused(corpus_path, out / "refused_linear.txt")
reached = refused(out / "reachable.txt", out / "refused_reachable.txt")

SIZES = {}
for size, names in ((8, "al cl dl bl spl bpl sil dil ah ch dh bh r8b r9b r10b r11b r12b r13b r14b r15b"),
                    (16, "ax cx dx bx sp bp si di r8w r9w r10w r11w r12w r13w r14w r15w"),
                    (32, "eax ecx edx ebx esp ebp esi edi r8d r9d r10d r11d r12d r13d r14d r15d"),
                    (64, "rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15")):
    for n in names.split():
        SIZES[n] = size
PTR = {"byte": 8, "word": 16, "dword": 32, "qword": 64, "xmmword": 128, "tbyte": 80}


def form(text):
    """The mnemonic with its prefixes and the kind and size of each operand: r64, m32, imm, cl, a segment override."""
    words = text.split(" ")
    prefixes = []
    while words and words[0] in ("lock", "rep", "repne", "repe", "data16", "addr32", "cs", "ds", "es", "ss", "fs", "gs"):
        prefixes.append(words.pop(0))
    mnemonic = words[0] if words else ""
    rest = " ".join(words[1:])
    kinds = []
    for operand in [o.strip() for o in re.split(r",(?![^\[]*\])", rest) if o.strip()]:
        operand = re.sub(r"\s*#.*$", "", operand)
        segment = re.search(r"\b([cdefgs]s):", operand)
        m = re.match(r"^(byte|word|dword|qword|xmmword|tbyte) ptr", operand)
        if "[" in operand:
            kind = f"m{PTR[m.group(1)]}" if m else "m"
            if segment and segment.group(1) not in ("ds",):
                kind = f"{segment.group(1)}:{kind}"
        elif operand in SIZES:
            kind = "cl" if operand == "cl" and kinds else f"r{SIZES[operand]}"
        elif re.match(r"^-?(0x)?[0-9a-f]+$", operand):
            kind = "imm"
        else:
            kind = operand
        kinds.append(kind)
    return " ".join(prefixes + [mnemonic]) + (" " + ", ".join(kinds) if kinds else "")


def groups(rows):
    count = Counter()
    blocked = defaultdict(set)
    for image, address, code, text in rows:
        g = form(text)
        count[g] += 1
        f = functions[image](address)
        blocked[g].add((image, f))
    return count, blocked


for label, rows in (("linear", linear), ("reachable", reached)):
    per_image = Counter(r[0] for r in rows)
    print(f"{label}: {len(rows)} refused;", ", ".join(f"{k} {v}" for k, v in per_image.items()))
count, blocked = groups(reached)
all_functions = {(r[0], functions[r[0]](r[1])) for r in reached}
print(f"reachable refused instructions in {len(all_functions)} distinct functions")
print("group\tinstructions\tfunctions")
for g, n in sorted(count.items(), key=lambda item: (-len(blocked[item[0]]), -item[1])):
    print(f"{g}\t{n}\t{len(blocked[g])}")
