"""Writes SSE and SSE2 cases for the translator's oracle test: one or a few instructions each, with inputs chosen around the edges where
x86 and Arm differ (NaNs, infinities, zeros, denormals, the integer limits of conversions, every rounding mode, FZ and DAZ).

usage: x86_sse.py OUTPUT_ASSEMBLY OUTPUT_HEADER

The assembly holds one function per case and run_case, which loads a case's registers, flags, XMM registers and MXCSR, calls it and
stores what it left. An x86-64 build of x86_sse_oracle.c runs them on an x86 CPU or under Rosetta; the translator runs the same bytes
and must agree on every general and XMM register, the flags, MXCSR and the 256-byte buffer r15 points at.
"""

import random
import struct
import sys

GPR = {
    32: ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"] + [f"r{i}d" for i in range(8, 16)],
    64: ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"] + [f"r{i}" for i in range(8, 16)],
}
# RSP holds the stack and R15 the buffer: no case writes them.
USABLE = [i for i in range(16) if i not in (4, 15)]
ALL_FLAGS = 1 | 4 | 16 | 64 | 128 | 2048

F32 = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3FC00000, 0x40200000, 0xBF000000, 0x40600000, 0x3F000000, 0x00000001,
       0x007FFFFF, 0x80000001, 0x807FFFFF, 0x00800000, 0x00800001, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000,
       0xFFC00000, 0x7FC12345, 0x7F800001, 0x7FA00000, 0xFF812345, 0x4F000000, 0xCF000000, 0x4EFFFFFF, 0xCF000001, 0x5F000000,
       0xDF000000, 0x5EFFFFFF, 0x3DCCCCCD, 0x501502F9, 0xD01502F9, 0x1A000000, 0x0C000000, 0x4B000001, 0x4B7FFFFF]
F64 = [0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x3FF8000000000000, 0x4004000000000000,
       0xBFE0000000000000, 0x400C000000000000, 0x3FE0000000000000, 0x0000000000000001, 0x000FFFFFFFFFFFFF, 0x8000000000000001,
       0x0010000000000000, 0x0010000000000001, 0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF, 0x7FF0000000000000, 0xFFF0000000000000,
       0x7FF8000000000000, 0xFFF8000000000000, 0x7FF8000012345678, 0x7FF0000000000001, 0x7FF4000000000000, 0xFFF0000012345678,
       0x41E0000000000000, 0xC1E0000000000000, 0x41DFFFFFFFC00000, 0x41DFFFFFFFE00000, 0xC1E0000000200000, 0xC1E0000000100000,
       0x43E0000000000000, 0xC3E0000000000000, 0x43DFFFFFFFFFFFFF, 0x3FB999999999999A, 0x7E37E43C8800759C, 0x0000B8157268FDAF,
       0x16687E92154EF7AC, 0x36A0000000000000, 0x3810000000000000, 0x47EFFFFFE0000000, 0x47EFFFFFF0000000, 0x4330000000000001]
INTEGERS = [0, 1, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF80000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000,
            0x1000001, 0x20000000000001, 0xFFFFFFFF, 0x12345678, 0xFFFFFFFFEDCBA988]


def float_bits(rng):
    return struct.unpack("<I", struct.pack("<f", rng.uniform(-1e6, 1e6) * rng.choice([1, 1e-30, 1e30, 1e-6])))[0]


def double_bits(rng):
    return struct.unpack("<Q", struct.pack("<d", rng.uniform(-1e6, 1e6) * rng.choice([1, 1e-300, 1e300, 1e-6])))[0]


def lane32(rng):
    roll = rng.random()
    return rng.choice(F32) if roll < 0.5 else float_bits(rng) if roll < 0.85 else rng.getrandbits(32)


def lane64(rng):
    roll = rng.random()
    return rng.choice(F64) if roll < 0.5 else double_bits(rng) if roll < 0.85 else rng.getrandbits(64)


def lane_int(rng):
    roll = rng.random()
    if roll < 0.3:
        return int.from_bytes(bytes(rng.choice([0, 1, 0x7F, 0x80, 0xFF, 0xFE, 0x81]) for _ in range(8)), "little")
    if roll < 0.4:
        return rng.choice(INTEGERS)
    return rng.getrandbits(64)


def quadword(rng, kind):
    """64 bits of float lanes, double lanes or integers, as the family reads them."""
    if kind == "f32":
        return lane32(rng) | lane32(rng) << 32
    if kind == "f64":
        return lane64(rng)
    if kind == "mixed":
        return quadword(rng, rng.choice(["f32", "f64", "int"]))
    return lane_int(rng)


def xmm(rng):
    return f"xmm{rng.randrange(16)}"


def gpr(rng, size):
    return GPR[size][rng.choice(USABLE)]


def mem(rng, size, aligned):
    """A memory operand of size bytes in the first 128 bytes of the buffer, 16-byte aligned for the packed forms that require it."""
    name = {2: "word", 4: "dword", 8: "qword", 16: "xmmword"}[size]
    offset = rng.randrange(0, 128 - size + 1)
    if aligned:
        offset -= offset % 16
    return f"{name} ptr [r15 + {offset}]"


def packed(rng, names, size=16, unaligned=()):
    name = rng.choice(names)
    source = xmm(rng) if rng.random() < 0.6 else mem(rng, size, name not in unaligned)
    return f"{name} {xmm(rng)}, {source}"


def moves(rng):
    roll = rng.randrange(10)
    whole = ["movaps", "movapd", "movdqa", "movups", "movupd", "movdqu"]
    unaligned = ("movups", "movupd", "movdqu")
    if roll < 3:
        name = rng.choice(whole)
        form = rng.randrange(3)
        if form == 0:
            return [f"{name} {xmm(rng)}, {xmm(rng)}"]
        if form == 1:
            return [f"{name} {xmm(rng)}, {mem(rng, 16, name not in unaligned)}"]
        return [f"{name} {mem(rng, 16, name not in unaligned)}, {xmm(rng)}"]
    if roll == 3:
        name, size = rng.choice([("movss", 4), ("movsd", 8)])
        form = rng.randrange(3)
        if form == 0:
            return [f"{name} {xmm(rng)}, {xmm(rng)}"]
        if form == 1:
            return [f"{name} {xmm(rng)}, {mem(rng, size, False)}"]
        return [f"{name} {mem(rng, size, False)}, {xmm(rng)}"]
    if roll == 4:
        size = rng.choice([32, 64])
        name = "movd" if size == 32 else "movq"
        form = rng.randrange(4)
        if form == 0:
            return [f"{name} {xmm(rng)}, {gpr(rng, size)}"]
        if form == 1:
            return [f"{name} {gpr(rng, size)}, {xmm(rng)}"]
        if form == 2:
            return [f"{name} {xmm(rng)}, {mem(rng, size // 8, False)}"]
        return [f"{name} {mem(rng, size // 8, False)}, {xmm(rng)}"]
    if roll == 5:
        return [f"movq {xmm(rng)}, {xmm(rng)}"]
    if roll == 6:
        name = rng.choice(["movlps", "movhps", "movlpd", "movhpd"])
        if rng.random() < 0.5:
            return [f"{name} {xmm(rng)}, {mem(rng, 8, False)}"]
        return [f"{name} {mem(rng, 8, False)}, {xmm(rng)}"]
    if roll == 7:
        return [f"{rng.choice(['movhlps', 'movlhps'])} {xmm(rng)}, {xmm(rng)}"]
    if roll == 8:
        return [f"{rng.choice(['movmskps', 'movmskpd', 'pmovmskb'])} {gpr(rng, 32)}, {xmm(rng)}"]
    return [f"{rng.choice(['movntps', 'movntpd', 'movntdq'])} {mem(rng, 16, True)}, {xmm(rng)}"]


def logic(rng):
    names = ["andps", "andpd", "andnps", "andnpd", "orps", "orpd", "xorps", "xorpd", "pand", "pandn", "por", "pxor"]
    if rng.random() < 0.15:
        register = xmm(rng)
        return [f"{rng.choice(['xorps', 'pxor', 'xorpd'])} {register}, {register}"]
    return [packed(rng, names)]


def arithmetic(rng):
    operations = ["add", "sub", "mul", "div", "sqrt", "min", "max"]
    name = rng.choice(operations) + rng.choice(["ps", "pd", "ss", "sd"])
    size = {"s": 4, "d": 8}[name[-1]] if name[-2] == "s" else 16
    source = xmm(rng) if rng.random() < 0.6 else mem(rng, size, True)
    body = [f"{name} {xmm(rng)}, {source}"]
    roll = rng.random()
    if roll < 0.1:
        # A new MXCSR from memory (x86_sse.py puts a valid one at r15 + 120) before the operation.
        body.insert(0, "ldmxcsr dword ptr [r15 + 120]")
    elif roll < 0.2:
        body.append(f"stmxcsr dword ptr [r15 + {rng.randrange(0, 32, 4)}]")
    return body


PREDICATES = ["eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"]
CONDITIONS = ["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"]


def compare(rng):
    if rng.random() < 0.5:
        suffix = rng.choice(["ps", "pd", "ss", "sd"])
        size = {"ss": 4, "sd": 8}.get(suffix, 16)
        source = xmm(rng) if rng.random() < 0.6 else mem(rng, size, True)
        return [f"cmp{rng.choice(PREDICATES)}{suffix} {xmm(rng)}, {source}"]
    name = rng.choice(["comiss", "ucomiss", "comisd", "ucomisd"])
    source = xmm(rng) if rng.random() < 0.6 else mem(rng, 4 if name.endswith("ss") else 8, False)
    body = [f"{name} {xmm(rng)}, {source}"]
    # Readers of the flags in the same block, as compilers place them.
    if rng.random() < 0.4:
        body.append(f"set{rng.choice(CONDITIONS)} {rng.choice(['al', 'cl', 'dl', 'bl', 'sil', 'r8b'])}")
    return body


def convert(rng):
    roll = rng.randrange(4)
    if roll == 0:
        name = rng.choice(["cvtsi2ss", "cvtsi2sd"])
        size = rng.choice([32, 64])
        source = gpr(rng, size) if rng.random() < 0.7 else mem(rng, size // 8, False)
        return [f"{name} {xmm(rng)}, {source}"]
    if roll == 1:
        name = rng.choice(["cvtss2si", "cvttss2si", "cvtsd2si", "cvttsd2si"])
        source = xmm(rng) if rng.random() < 0.7 else mem(rng, 4 if "ss" in name else 8, False)
        return [f"{name} {gpr(rng, rng.choice([32, 64]))}, {source}"]
    name, size = rng.choice([("cvtss2sd", 4), ("cvtsd2ss", 8), ("cvtdq2ps", 16), ("cvtdq2pd", 8), ("cvtps2dq", 16), ("cvttps2dq", 16),
                             ("cvtpd2dq", 16), ("cvttpd2dq", 16), ("cvtps2pd", 8), ("cvtpd2ps", 16)])
    source = xmm(rng) if rng.random() < 0.7 else mem(rng, size, True)
    return [f"{name} {xmm(rng)}, {source}"]


def integer(rng):
    roll = rng.randrange(10)
    if roll < 6:
        names = ["paddb", "paddw", "paddd", "paddq", "psubb", "psubw", "psubd", "psubq", "paddsb", "paddsw", "paddusb", "paddusw",
                 "psubsb", "psubsw", "psubusb", "psubusw", "pmullw", "pmulhw", "pmulhuw", "pmuludq", "pmaddwd", "pcmpeqb", "pcmpeqw",
                 "pcmpeqd", "pcmpgtb", "pcmpgtw", "pcmpgtd", "pavgb", "pavgw", "pminub", "pmaxub", "pminsw", "pmaxsw", "psadbw"]
        return [packed(rng, names)]
    if roll < 8:
        name = rng.choice(["psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad", "psrldq", "pslldq"])
        return [f"{name} {xmm(rng)}, {rng.choice([0, 1, rng.randrange(80), rng.randrange(256)])}"]
    # By the count in the low quadword of an XMM register or of memory, which main makes small most of the time.
    return [packed(rng, ["psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"])]


def shuffle(rng):
    if rng.random() < 0.6:
        return [packed(rng, ["packsswb", "packssdw", "packuswb", "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq", "punpckhbw",
                             "punpckhwd", "punpckhdq", "punpckhqdq", "unpcklps", "unpcklpd", "unpckhps", "unpckhpd"])]
    name = rng.choice(["pshufd", "pshuflw", "pshufhw", "shufps", "shufpd"])
    source = xmm(rng) if rng.random() < 0.6 else mem(rng, 16, True)
    return [f"{name} {xmm(rng)}, {source}, {rng.randrange(256)}"]


DATA_LABELS = [0]


def addressing(rng):
    """The address forms compiled code uses: an index register, a displacement too large for one Arm offset, the stack and RIP-relative
    constants (placed after the case's ret; only forms without an alignment requirement read them, as the copy the translator runs
    may sit at another alignment)."""
    load = rng.choice([("movups", 16), ("movdqu", 16), ("movsd", 8), ("movss", 4), ("addsd", 8), ("mulss", 4), ("cvtss2sd", 4),
                       ("movq", 8)])
    store = rng.choice([("movups", 16), ("movdqu", 16), ("movsd", 8), ("movss", 4), ("movq", 8)])
    roll = rng.randrange(4)
    if roll == 0:
        scale = rng.choice([1, 2, 4, 8])
        index = rng.randrange(0, 112 // scale)
        name, size = rng.choice([load, store])
        offset = rng.randrange(0, 128 - size + 1 - scale * index) if scale * index < 128 - size else 0
        address = f"{ {16: 'xmmword', 8: 'qword', 4: 'dword'}[size]} ptr [r15 + {scale}*rcx + {offset}]"
        operands = f"{xmm(rng)}, {address}" if (name, size) == load else f"{address}, {xmm(rng)}"
        return [f"mov ecx, {index}", f"{name} {operands}"]
    if roll == 1:
        far = rng.choice([70000, -5000, 1 << 20, -300])
        name, size = load
        offset = rng.randrange(0, 128 - size + 1)
        word = {16: "xmmword", 8: "qword", 4: "dword"}[size]
        sign = "+" if far >= 0 else "-"
        # RBX holds an address, which differs between the two runs: it is cleared after (mov leaves the flags alone).
        return [f"lea rbx, [r15 {'-' if far >= 0 else '+'} {abs(far)}]",
                f"{name} {xmm(rng)}, {word} ptr [rbx {sign} {abs(far) + offset if far >= 0 else abs(far) - offset}]", "mov ebx, 0"]
    if roll == 2:
        name, size = store
        word = {16: "xmmword", 8: "qword", 4: "dword"}[size]
        slot = rng.choice([16, 32, 64, 128])
        other, _ = {16: ("movups", 16), 8: ("movsd", 8), 4: ("movss", 4)}[size]
        return [f"{name} {word} ptr [rsp - {slot}], {xmm(rng)}", f"{other} {xmm(rng)}, {word} ptr [rsp - {slot}]"]
    DATA_LABELS[0] += 1
    label = f"constant_{DATA_LABELS[0]}"
    name, size = load
    word = {16: "xmmword", 8: "qword", 4: "dword"}[size]
    return [f"{name} {xmm(rng)}, {word} ptr [rip + {label}]", "ret", f"{label}: .quad {quadword(rng, 'mixed'):#x}, {quadword(rng, 'mixed'):#x}"]


# Operand pairs where x86 and Arm differ unless the translator makes them agree: signed zeros and NaNs in either order.
EDGE_PAIRS = {
    "s": [(0x00000000, 0x80000000), (0x80000000, 0x00000000), (0x7FC00001, 0x7F800002), (0x7F800002, 0x7FC00001),
          (0x7FC00001, 0xFFC00003), (0x7F800002, 0xFF800004), (0x7F800002, 0x3F800000), (0x3F800000, 0x7F800002),
          (0x7FC00001, 0x80000000), (0x00000000, 0xFFC00003), (0x7F800000, 0x00000000), (0x00000000, 0x7F800000)],
    "d": [(0x0000000000000000, 0x8000000000000000), (0x8000000000000000, 0x0000000000000000),
          (0x7FF8000000000001, 0x7FF0000000000002), (0x7FF0000000000002, 0x7FF8000000000001),
          (0x7FF8000000000001, 0xFFF8000000000003), (0x7FF0000000000002, 0xFFF0000000000004),
          (0x7FF0000000000002, 0x3FF0000000000000), (0x3FF0000000000000, 0x7FF0000000000002),
          (0x7FF8000000000001, 0x8000000000000000), (0x0000000000000000, 0xFFF8000000000003),
          (0x7FF0000000000000, 0x0000000000000000), (0x0000000000000000, 0x7FF0000000000000)],
}


def edges(rng):
    first, second = rng.sample(range(16), 2)
    name = rng.choice(["min", "max", "add", "sub", "mul", "div", "cmplt", "cmpeq", "cmpunord"]) + rng.choice(["ps", "pd", "ss", "sd"])
    if rng.random() < 0.2:
        name = rng.choice(["comiss", "ucomiss", "comisd", "ucomisd"])
    return [f"{name} xmm{first}, xmm{second}"]


def edge_pairs(rng, body, vectors):
    """Puts EDGE_PAIRS into every element of the two registers of an edges case."""
    name, first, second = body[0].replace(",", "").split()
    kind = "s" if name.endswith(("ps", "ss")) else "d"
    for half in range(2):
        low, high = [], []
        for lane in range(2 if kind == "s" else 1):
            a, b = rng.choice(EDGE_PAIRS[kind])
            low.append(a)
            high.append(b)
        if kind == "s":
            vectors[int(first[3:])][half] = low[0] | low[1] << 32
            vectors[int(second[3:])][half] = high[0] | high[1] << 32
        else:
            vectors[int(first[3:])][half] = low[0]
            vectors[int(second[3:])][half] = high[0]


def small_counts(rng, body, vectors, memory):
    """Shift counts that leave something of the value: an XMM register or 16 bytes of memory whose low quadword is the count."""
    parts = body[-1].replace(",", "").split()
    if len(parts) not in (3, 7) or parts[0][:4] not in ("psll", "psrl", "psra") or parts[0].endswith("dq"):
        return
    count = rng.choice([0, 1, rng.randrange(70), rng.randrange(70), 1 << rng.randrange(64)])
    if parts[2] == "xmmword":
        memory[int(parts[-1].rstrip("]")) // 8] = count
    elif parts[2].startswith("xmm") and parts[2] != parts[1]:
        vectors[int(parts[2][3:])][0] = count


# Each family: how its cases are made and what its lanes hold (by default; an instruction ending in ps or ss reads floats, in pd or
# sd doubles).
FAMILIES = [
    ("moves", moves, "mixed", 600),
    ("logic", logic, "mixed", 300),
    ("arithmetic", arithmetic, "mixed", 1500),
    ("compare", compare, "mixed", 1200),
    ("convert", convert, "mixed", 1500),
    ("integer", integer, "int", 1500),
    ("shuffle", shuffle, "int", 800),
    ("addressing", addressing, "mixed", 600),
    ("edges", edges, "mixed", 600),
]


def lanes_of(body, kind):
    name = body[-1].split()[0] if not body[-1].startswith(("stmxcsr", "set")) else body[0].split()[0]
    if kind != "mixed" or name.startswith(("mov", "pmov")):
        return kind
    if name.startswith("cvt"):
        # The source's lanes: cvtdq2ps reads integers, cvttsd2si doubles.
        return {"ss": "f32", "ps": "f32", "sd": "f64", "pd": "f64", "dq": "int"}.get(name[3:].lstrip("t").split("2")[0], kind)
    if name.endswith(("ps", "ss")):
        return "f32"
    if name.endswith(("pd", "sd")):
        return "f64"
    return kind


def mxcsr(rng, floating):
    value = 0x1F80
    if floating:
        if rng.random() < 0.5:
            value |= rng.randrange(4) << 13
        if rng.random() < 0.15:
            value |= 0x8000
        if rng.random() < 0.15:
            value |= 0x40
    if rng.random() < 0.1:
        value |= rng.getrandbits(6)
    return value


def main():
    assembly, header = sys.argv[1:3]
    rng = random.Random(20261120)
    lines = [".intel_syntax noprefix", ".text"]
    # run_case(io = rdi, code = rsi). io: 16 registers, the flags, MXCSR, then XMM0-XMM15 at 144 and the buffer r15 points at, at 400.
    lines += [
        ".globl _run_case", "_run_case:",
        "push rbx", "push rbp", "push r12", "push r13", "push r14", "push r15", "sub rsp, 8",
        "mov qword ptr [rip + saved_io], rdi", "mov qword ptr [rip + saved_code], rsi", "stmxcsr dword ptr [rip + host_mxcsr]",
    ]
    lines += [f"movdqu xmm{i}, xmmword ptr [rdi + {144 + 16 * i}]" for i in range(16)]
    lines += ["ldmxcsr dword ptr [rdi + 136]", "push qword ptr [rdi + 128]", "popfq"]
    for index, name in enumerate(GPR[64]):
        if name not in ("rsp", "rdi", "r15"):
            lines.append(f"mov {name}, qword ptr [rdi + {8 * index}]")
    lines += ["lea r15, [rdi + 400]", "mov rdi, qword ptr [rdi + 56]", "call qword ptr [rip + saved_code]",
              "pushfq", "push rdi", "mov rdi, qword ptr [rip + saved_io]", "pop qword ptr [rdi + 56]", "pop qword ptr [rdi + 128]"]
    for index, name in enumerate(GPR[64]):
        if name not in ("rsp", "rdi", "r15"):
            lines.append(f"mov qword ptr [rdi + {8 * index}], {name}")
    lines += ["stmxcsr dword ptr [rdi + 136]"]
    lines += [f"movdqu xmmword ptr [rdi + {144 + 16 * i}], xmm{i}" for i in range(16)]
    lines += ["ldmxcsr dword ptr [rip + host_mxcsr]", "add rsp, 8", "pop r15", "pop r14", "pop r13", "pop r12", "pop rbp", "pop rbx",
              "ret"]
    cases = []
    for family, make, kind, count in FAMILIES:
        floating = kind != "int"
        for _ in range(count):
            number = len(cases)
            registers = [rng.choice([rng.getrandbits(64), rng.choice(INTEGERS), rng.getrandbits(8)]) for _ in range(16)]
            body = make(rng)
            lanes = lanes_of(body, kind)
            vectors = [[quadword(rng, lanes), quadword(rng, lanes)] for _ in range(16)]
            memory = [quadword(rng, lanes) for _ in range(16)]
            small_counts(rng, body, vectors, memory)
            if family == "edges":
                edge_pairs(rng, body, vectors)
            if any(line.startswith("ldmxcsr") for line in body):
                memory[15] = mxcsr(rng, True) | rng.getrandbits(32) << 32
            lines += [f".globl _case_{number}", f".globl _case_{number}_end", f"_case_{number}:"] + body + ["ret",
                                                                                                         f"_case_{number}_end:"]
            cases.append((family, registers, rng.getrandbits(12) & ALL_FLAGS, mxcsr(rng, floating), vectors, memory, body))
    # The boundaries of the conversions to integers, each in every rounding mode, with no flag set before: which of IE and PE x86
    # raises where rounding first makes a value inexact and then out of range (2147483647.5 to nearest).
    boundaries = [0x41DFFFFFFFE00000, 0xC1DFFFFFFFE00000, 0x41E0000000100000, 0xC1E0000000100000, 0x41DFFFFFFFC00000,
                  0x41E0000000000000, 0xC1E0000000000000, 0xC1E0000000200000, 0x41DFFFFFFFA00000, 0x3FE0000000000000,
                  0x4004000000000000, 0x43E0000000000000, 0x43DFFFFFFFFFFFFF, 0xC3E0000000000000, 0x7FF0000000000000,
                  0xFFF0000000000000, 0x7FF8000000000000, 0x7FF0000000000001, 0x43E0000000000400, 0xC3E0000000000400]
    singles = [struct.unpack("<I", struct.pack("<f", struct.unpack("<d", struct.pack("<Q", bits))[0]))[0] for bits in boundaries]
    forms = ["cvtsd2si eax, xmm1", "cvtsd2si rax, xmm1", "cvttsd2si eax, xmm1", "cvttsd2si rax, xmm1", "cvtpd2dq xmm2, xmm1",
             "cvttpd2dq xmm2, xmm1", "cvtss2si eax, xmm1", "cvtss2si rax, xmm1", "cvtps2dq xmm2, xmm1", "cvttps2dq xmm2, xmm1"]
    for form in forms:
        for index, bits in enumerate(boundaries):
            for rc in range(4):
                number = len(cases)
                vectors = [[0, 0] for _ in range(16)]
                vectors[1] = [singles[index] if "ss2" in form or "ps2" in form else bits, 0]
                lines += [f".globl _case_{number}", f".globl _case_{number}_end", f"_case_{number}:", form, "ret", f"_case_{number}_end:"]
                cases.append(("boundaries", [0] * 16, 0, 0x1F80 | rc << 13, vectors, [0] * 16, [form]))
    lines += [".data", "saved_io: .quad 0", "saved_code: .quad 0", "host_mxcsr: .long 0", ""]
    with open(assembly, "w") as out:
        out.write("\n".join(lines))
    with open(header, "w") as out:
        out.write("// Written by x86_sse.py.\n")
        for number in range(len(cases)):
            out.write(f"extern char case_{number}[], case_{number}_end[];\n")
        out.write("static const struct Case cases[] = {\n")
        for number, (family, registers, flags, control, vectors, memory, body) in enumerate(cases):
            values = ", ".join(f"{value:#x}ull" for value in registers)
            lanes = ", ".join(f"{{{low:#x}ull, {high:#x}ull}}" for low, high in vectors)
            words = ", ".join(f"{value:#x}ull" for value in memory)
            text = "; ".join(body).replace('"', '\\"')
            out.write(f"\t{{{{{values}}}, {flags:#x}, {control:#x}, {{{lanes}}}, {{{words}}}, \"{family}\", case_{number}, "
                      f"case_{number}_end, \"{text}\"}},\n")
        out.write("};\n")


if __name__ == "__main__":
    main()
