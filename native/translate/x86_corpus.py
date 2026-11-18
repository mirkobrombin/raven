"""Builds the reference corpus the x86-64 decoder is checked against: LLVM's disassembler is the oracle.

usage: x86_corpus.py CLANG OBJDUMP OUTPUT [PE_OR_OBJECT...]

Writes one line per instruction, "hex bytes<TAB>address<TAB>Intel text", from three sources: random instances of the instructions the
translator handles (assembled by CLANG), a stream of random bytes, and the code sections of every PE given. The seed is fixed, so the
corpus is the same on every build.
"""

import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

GPR = {
    8: ["al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"] + [f"r{i}b" for i in range(8, 16)],
    16: ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"] + [f"r{i}w" for i in range(8, 16)],
    32: ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"] + [f"r{i}d" for i in range(8, 16)],
    64: ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"] + [f"r{i}" for i in range(8, 16)],
}
HIGH = ["ah", "ch", "dh", "bh"]
PTR = {8: "byte", 16: "word", 32: "dword", 64: "qword"}
CONDITIONS = ["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"]
ARITHMETIC = ["add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"]
SHIFTS = ["rol", "ror", "rcl", "rcr", "shl", "shr", "sar"]


def disp(rng):
    return rng.choice([0, rng.randint(-128, 127), rng.randint(-2**31, 2**31 - 1)])


def memory(rng, size, sized=True):
    base = rng.choice(GPR[64] + ["rip", None])
    index = rng.choice([r for r in GPR[64] if r != "rsp"] + [None, None])
    parts = []
    if base == "rip":
        parts.append("rip")
        index = None
    elif base:
        parts.append(base)
    if index:
        parts.append(f"{rng.choice([1, 2, 4, 8])}*{index}")
    offset = disp(rng)
    if not parts:
        offset = rng.randint(0, 2**31 - 1)
    text = " + ".join(parts) if parts else ""
    if offset or not parts:
        text = f"{text} + {offset}" if text else str(offset)
    segment = rng.choice(["", "", "", "", "fs:", "gs:"])
    prefix = f"{PTR[size]} ptr " if sized else ""
    return f"{prefix}{segment}[{text}]"


def register(rng, size):
    return rng.choice(GPR[size])


def rm(rng, size):
    return register(rng, size) if rng.random() < 0.5 else memory(rng, size)


def immediate(rng, bits, signed=True):
    if signed:
        return rng.randint(-(2 ** (bits - 1)), 2 ** (bits - 1) - 1)
    return rng.randint(0, 2**bits - 1)


def instance(rng):
    size = rng.choice([8, 16, 32, 64])
    vsize = rng.choice([16, 32, 64])
    kind = rng.randrange(24)
    if kind < 5:
        op = rng.choice(ARITHMETIC)
        form = rng.randrange(4)
        if form == 0:
            return f"{op} {rm(rng, size)}, {register(rng, size)}"
        if form == 1:
            return f"{op} {register(rng, size)}, {memory(rng, size)}"
        if form == 2:
            bits = 8 if size == 8 or rng.random() < 0.5 else min(size, 32)
            return f"{op} {rm(rng, size)}, {immediate(rng, bits)}"
        return f"{op} {GPR[size][0]}, {immediate(rng, min(size, 32))}"
    if kind < 9:
        form = rng.randrange(5)
        if form == 0:
            return f"mov {rm(rng, size)}, {register(rng, size)}"
        if form == 1:
            return f"mov {register(rng, size)}, {memory(rng, size)}"
        if form == 2:
            return f"mov {register(rng, size)}, {immediate(rng, size if size < 64 else rng.choice([32, 64]))}"
        if form == 3:
            return f"mov {memory(rng, size)}, {immediate(rng, min(size, 32))}"
        return f"lea {register(rng, vsize)}, {memory(rng, 64, sized=False)}"
    if kind == 9:
        op = rng.choice(["movzx", "movsx"])
        source = rng.choice([8, 16])
        target = rng.choice([s for s in (16, 32, 64) if s > source])
        return f"{op} {register(rng, target)}, {rm(rng, source)}"
    if kind == 10:
        return f"movsxd {register(rng, 64)}, {rm(rng, 32)}"
    if kind == 11:
        op = rng.choice(["inc", "dec", "neg", "not", "mul", "imul", "div", "idiv"])
        return f"{op} {rm(rng, size)}"
    if kind == 12:
        form = rng.randrange(3)
        if form == 0:
            return f"test {rm(rng, size)}, {register(rng, size)}"
        if form == 1:
            return f"test {rm(rng, size)}, {immediate(rng, min(size, 32))}"
        return f"xchg {rm(rng, size)}, {register(rng, size)}"
    if kind == 13:
        op = rng.choice(SHIFTS)
        count = rng.choice(["1", "cl", str(rng.randint(2, 63 if size == 64 else 31))])
        return f"{op} {rm(rng, size)}, {count}"
    if kind == 14:
        size = rng.choice([16, 64])
        op = rng.choice(["push", "pop"])
        if rng.random() < 0.5:
            return f"{op} {register(rng, size)}"
        return f"{op} {memory(rng, size)}"
    if kind == 15:
        return f"push {immediate(rng, rng.choice([8, 32]))}"
    if kind == 16:
        return f"set{rng.choice(CONDITIONS)} {rm(rng, 8)}"
    if kind == 17:
        return f"cmov{rng.choice(CONDITIONS)} {register(rng, vsize)}, {rm(rng, vsize)}"
    if kind == 18:
        form = rng.randrange(3)
        if form == 0:
            return f"imul {register(rng, vsize)}, {rm(rng, vsize)}"
        bits = 8 if form == 1 else min(vsize, 32)
        return f"imul {register(rng, vsize)}, {rm(rng, vsize)}, {immediate(rng, bits)}"
    if kind == 19:
        return rng.choice(["ret", "int3", "nop", "syscall", "leave", "cdq", "cqo", "cdqe", "cwde", "ud2", "hlt", f"ret {rng.randint(0, 65535)}"])
    if kind == 20:
        op = rng.choice(["call", "jmp"])
        return f"{op} {rm(rng, 64)}"
    if kind == 21:
        return f"nop {rm(rng, rng.choice([16, 32]))}"
    if kind == 22:
        op = rng.choice(["cmpxchg", "xadd"])
        lock = rng.choice(["", "lock "])
        return f"{lock}{op} {memory(rng, size)}, {register(rng, size)}"
    if kind == 23 and rng.random() < 0.5:
        # The high byte registers exist only without REX: pair them with registers and addresses that need none.
        legacy = GPR[64][:8]
        address = f"byte ptr [{rng.choice(legacy)} + {rng.randint(-128, 127)}]"
        other = rng.choice(HIGH + GPR[8][:4] + [address])
        return f"{rng.choice(ARITHMETIC + ['mov', 'test', 'xchg'])} {rng.choice(HIGH)}, {other}"
    return f"{rng.choice(['j' + c for c in CONDITIONS] + ['jmp', 'call'])} label{rng.randrange(100)}"


def disassemble(objdump, path):
    """Yields (bytes, address, text) for every instruction LLVM decodes in the code sections of path."""
    output =subprocess.run([objdump, "-d", "-M", "intel", path], check=True, capture_output=True, text=True).stdout
    line_pattern = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s+)+)(.*)$")
    for line in output.splitlines():
        match = line_pattern.match(line)
        if not match:
            continue
        address, raw, text = match.groups()
        text = re.sub(r"\s*(#.*|<.*>)$", "", text.strip())
        text = re.sub(r"\s+", " ", text)
        yield "".join(raw.split()), int(address, 16), text


def main():
    clang, objdump, output = sys.argv[1:4]
    images = sys.argv[4:]
    rng = random.Random(20261118)
    lines = []
    with tempfile.TemporaryDirectory() as scratch:
        source = Path(scratch) / "instances.s"
        source.write_text(".intel_syntax noprefix\n.text\n" + "".join(
            instance(rng) + "\n" + (f"label{n // 200}:\n" if n % 200 == 0 else "") for n in range(20000)))
        obj = Path(scratch) / "instances.o"
        subprocess.run([clang, "-c", "-x", "assembler", "-o", str(obj), str(source)], check=True)
        noise = Path(scratch) / "noise.s"
        noise.write_text(".text\n" + "".join(f".byte {', '.join(str(rng.randrange(256)) for _ in range(16))}\n" for _ in range(8192)))
        noise_obj = Path(scratch) / "noise.o"
        subprocess.run([clang, "-c", "-x", "assembler", "-o", str(noise_obj), str(noise)], check=True)
        for path in [obj, noise_obj] + [Path(image) for image in images]:
            source_name = path.name
            lines.append(f"# {source_name}")
            for raw, address, text in disassemble(objdump, str(path)):
                lines.append(f"{raw}\t{address:x}\t{text}")
    Path(output).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
