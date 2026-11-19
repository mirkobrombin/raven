"""Writes AVX, AVX2 and FMA3 cases for the translator's oracle test: one or a few VEX instructions each, with inputs around the edges
where x86 and Arm differ (as x86_sse.py) and every YMM register's upper half set, so that clearing it (VEX.128) and keeping it
(legacy SSE) give different results.

usage: x86_avx.py OUTPUT_ASSEMBLY OUTPUT_HEADER

The assembly holds one function per case and run_case, which loads a case's registers, flags, YMM registers and MXCSR, calls it and
stores what it left. An x86-64 build of x86_avx_oracle.c runs them on an x86 CPU or under Rosetta; the translator runs the same bytes
and must agree on every general and YMM register, the flags, MXCSR (but DE) and the 256-byte buffer r15 points at. Each family has a
seed of its own, so a family added later leaves the cases of the others, and their silicon results, as they were.
"""

import os
import random
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import x86_sse as sse  # noqa: E402

ALL_FLAGS = sse.ALL_FLAGS


def reg(rng, avoid=()):
    return rng.choice([i for i in range(16) if i not in avoid])


def three(rng):
    """Destination and two sources, a third of them aliased: destination = first source, destination = second source (which
    needs a scratch register), or both sources the same."""
    d, a, b = rng.randrange(16), rng.randrange(16), rng.randrange(16)
    roll = rng.random()
    if roll < 0.12:
        a = d
    elif roll < 0.24:
        b = d
    elif roll < 0.33:
        b = a
    return d, a, b


def name(number, width):
    return f"{'ymm' if width == 32 else 'xmm'}{number}"


def mem(rng, size, aligned=False):
    word = {1: "byte", 2: "word", 4: "dword", 8: "qword", 16: "xmmword", 32: "ymmword"}[size]
    offset = rng.randrange(0, 192 - size + 1)
    if aligned:
        offset -= offset % size
    return f"{word} ptr [r15 + {offset}]"


def binary(rng, names, width, memory_chance=0.4):
    d, a, b = three(rng)
    source = mem(rng, width) if rng.random() < memory_chance else name(b, width)
    return f"{rng.choice(names)} {name(d, width)}, {name(a, width)}, {source}"


def moves(rng):
    roll = rng.randrange(8)
    width = rng.choice([16, 32])
    whole = ["vmovaps", "vmovapd", "vmovdqa", "vmovups", "vmovupd", "vmovdqu"]
    if roll < 3:
        op = rng.choice(whole)
        aligned = op in ("vmovaps", "vmovapd", "vmovdqa")
        form = rng.randrange(3)
        if form == 0:
            return [f"{op} {name(reg(rng), width)}, {name(reg(rng), width)}"]
        if form == 1:
            return [f"{op} {name(reg(rng), width)}, {mem(rng, width, aligned)}"]
        return [f"{op} {mem(rng, width, aligned)}, {name(reg(rng), width)}"]
    if roll == 3:
        op, size = rng.choice([("vmovss", 4), ("vmovsd", 8)])
        form = rng.randrange(3)
        if form == 0:
            d, a, b = three(rng)
            return [f"{op} xmm{d}, xmm{a}, xmm{b}"]
        if form == 1:
            return [f"{op} xmm{reg(rng)}, {mem(rng, size)}"]
        return [f"{op} {mem(rng, size)}, xmm{reg(rng)}"]
    if roll == 4:
        size = rng.choice([32, 64])
        gpr = sse.gpr(rng, size)
        op = "vmovd" if size == 32 else "vmovq"
        form = rng.randrange(4)
        if form == 0:
            return [f"{op} xmm{reg(rng)}, {gpr}"]
        if form == 1:
            return [f"{op} {gpr}, xmm{reg(rng)}"]
        if form == 2:
            return [f"{op} xmm{reg(rng)}, {mem(rng, size // 8)}"]
        return [f"{op} {mem(rng, size // 8)}, xmm{reg(rng)}"]
    if roll == 5:
        return [f"vmovq xmm{reg(rng)}, xmm{reg(rng)}"]
    if roll == 6:
        return [f"vmovntdq {mem(rng, width, True)}, {name(reg(rng), width)}"]
    op = rng.choice(["vmovlps", "vmovhps", "vmovlpd", "vmovhpd"])
    if rng.random() < 0.5:
        return [f"{op} xmm{reg(rng)}, xmm{reg(rng)}, {mem(rng, 8)}"]
    return [f"{op} {mem(rng, 8)}, xmm{reg(rng)}"]


def transitions(rng):
    """What happens to the upper halves: a 256-bit write, then a legacy SSE instruction (keeps them), a VEX.128 one (clears them),
    vzeroupper or vzeroall."""
    a, b, c = rng.randrange(16), rng.randrange(16), rng.randrange(16)
    first = f"vaddps ymm{a}, ymm{b}, ymm{c}" if rng.random() < 0.5 else f"vmovups ymm{a}, {mem(rng, 32)}"
    roll = rng.randrange(6)
    if roll == 0:
        return [first, f"addps xmm{a}, xmm{rng.randrange(16)}"]
    if roll == 1:
        return [first, f"movaps xmm{a}, xmm{rng.randrange(16)}"]
    if roll == 2:
        return [first, f"vaddps xmm{a}, xmm{rng.randrange(16)}, xmm{rng.randrange(16)}"]
    if roll == 3:
        return [first, "vzeroupper"]
    if roll == 4:
        return [first, "vzeroall"]
    d, x, y = three(rng)
    return [first, f"{rng.choice(['vaddsd', 'vmulss', 'vsqrtsd', 'vmaxss'])} xmm{d}, xmm{x}, xmm{y}"]


def arithmetic(rng):
    width = rng.choice([16, 32])
    roll = rng.random()
    if roll < 0.55:
        return [binary(rng, [f"v{op}{t}" for op in ("add", "sub", "mul", "div", "min", "max") for t in ("ps", "pd")], width)]
    if roll < 0.85:
        d, a, b = three(rng)
        op = rng.choice([f"v{op}{t}" for op in ("add", "sub", "mul", "div", "min", "max", "sqrt") for t in ("ss", "sd")])
        source = mem(rng, 4 if op.endswith("ss") else 8) if rng.random() < 0.4 else f"xmm{b}"
        return [f"{op} xmm{d}, xmm{a}, {source}"]
    op = rng.choice(["vsqrtps", "vsqrtpd"])
    source = mem(rng, width) if rng.random() < 0.4 else name(reg(rng), width)
    return [f"{op} {name(reg(rng), width)}, {source}"]


def logic(rng):
    width = rng.choice([16, 32])
    names = ["vandps", "vandpd", "vandnps", "vandnpd", "vorps", "vorpd", "vxorps", "vxorpd", "vpand", "vpandn", "vpor", "vpxor"]
    return [binary(rng, names, width)]


def compare(rng):
    width = rng.choice([16, 32])
    roll = rng.random()
    if roll < 0.6:
        d, a, b = three(rng)
        t = rng.choice(["ps", "pd"])
        source = mem(rng, width) if rng.random() < 0.4 else name(b, width)
        return [f"vcmp{t} {name(d, width)}, {name(a, width)}, {source}, {rng.randrange(8)}"]
    if roll < 0.8:
        d, a, b = three(rng)
        t = rng.choice(["ss", "sd"])
        return [f"vcmp{t} xmm{d}, xmm{a}, xmm{b}, {rng.randrange(8)}"]
    op = rng.choice(["vcomiss", "vcomisd", "vucomiss", "vucomisd"])
    cond = rng.choice(["a", "ae", "b", "be", "e", "ne", "p", "np"])
    return [f"{op} xmm{reg(rng)}, xmm{reg(rng)}", f"set{cond} al"]


def integer(rng):
    width = rng.choice([16, 32])
    roll = rng.random()
    if roll < 0.6:
        names = ["vpaddb", "vpaddw", "vpaddd", "vpaddq", "vpsubb", "vpsubw", "vpsubd", "vpsubq", "vpaddsb", "vpaddsw", "vpaddusb",
                 "vpaddusw", "vpsubsb", "vpsubsw", "vpsubusb", "vpsubusw", "vpmullw", "vpmulhw", "vpmulhuw", "vpmuludq", "vpmaddwd",
                 "vpcmpeqb", "vpcmpeqw", "vpcmpeqd", "vpcmpgtb", "vpcmpgtw", "vpcmpgtd", "vpavgb", "vpavgw", "vpminub", "vpmaxub",
                 "vpminsw", "vpmaxsw", "vpsadbw"]
        return [binary(rng, names, width)]
    if roll < 0.8:
        op = rng.choice(["vpmovmskb", "vmovmskps", "vmovmskpd"])
        return [f"{op} {sse.gpr(rng, 32)}, {name(reg(rng), width)}"]
    d, a = reg(rng), reg(rng)
    op = rng.choice(["vpsllw", "vpslld", "vpsllq", "vpsrlw", "vpsrld", "vpsrlq", "vpsraw", "vpsrad"])
    if rng.random() < 0.5:
        return [f"{op} {name(d, width)}, {name(a, width)}, {rng.choice([0, 1, 7, 15, 16, 31, 32, 63, 64, 200])}"]
    return [f"{op} {name(d, width)}, {name(a, width)}, xmm{reg(rng)}"]


def shuffle(rng):
    width = rng.choice([16, 32])
    roll = rng.random()
    if roll < 0.5:
        names = ["vpacksswb", "vpackssdw", "vpackuswb", "vpunpcklbw", "vpunpcklwd", "vpunpckldq", "vpunpcklqdq", "vpunpckhbw",
                 "vpunpckhwd", "vpunpckhdq", "vpunpckhqdq", "vunpcklps", "vunpcklpd", "vunpckhps", "vunpckhpd"]
        return [binary(rng, names, width)]
    if roll < 0.75:
        op = rng.choice(["vpshufd", "vpshuflw", "vpshufhw"])
        return [f"{op} {name(reg(rng), width)}, {name(reg(rng), width)}, {rng.randrange(256)}"]
    if roll < 0.9:
        d, a, b = three(rng)
        op = rng.choice(["vshufps", "vshufpd"])
        return [f"{op} {name(d, width)}, {name(a, width)}, {name(b, width)}, {rng.randrange(256)}"]
    op = rng.choice(["vpslldq", "vpsrldq"])
    return [f"{op} {name(reg(rng), width)}, {name(reg(rng), width)}, {rng.choice([0, 1, 4, 8, 15, 16, 20])}"]


def lanes(rng):
    roll = rng.randrange(6)
    d, a, b = three(rng)
    if roll == 0:
        source = mem(rng, 16) if rng.random() < 0.4 else f"xmm{b}"
        return [f"{rng.choice(['vinsertf128', 'vinserti128'])} ymm{d}, ymm{a}, {source}, {rng.randrange(4)}"]
    if roll == 1:
        target = mem(rng, 16) if rng.random() < 0.4 else f"xmm{d}"
        return [f"{rng.choice(['vextractf128', 'vextracti128'])} {target}, ymm{a}, {rng.randrange(4)}"]
    if roll == 2:
        source = mem(rng, 32) if rng.random() < 0.4 else f"ymm{b}"
        return [f"{rng.choice(['vperm2f128', 'vperm2i128'])} ymm{d}, ymm{a}, {source}, {rng.randrange(256)}"]
    if roll == 3:
        op, size = rng.choice([("vbroadcastss", 4), ("vbroadcastsd", 8), ("vbroadcastf128", 16)])
        width = 32 if size != 4 else rng.choice([16, 32])
        source = mem(rng, size) if size == 16 or rng.random() < 0.5 else f"xmm{a}"
        return [f"{op} {name(d, width)}, {source}"]
    op, size = rng.choice([("vpbroadcastb", 1), ("vpbroadcastw", 2), ("vpbroadcastd", 4), ("vpbroadcastq", 8)])
    width = rng.choice([16, 32])
    source = mem(rng, size) if rng.random() < 0.5 else f"xmm{a}"
    return [f"{op} {name(d, width)}, {source}"]


FMA_NAMES = [f"vf{op}{order}" for op in ("madd", "msub", "nmadd", "nmsub") for order in ("132", "213", "231")]


def fma(rng):
    d, a, b = three(rng)
    t = rng.choice(["ps", "pd", "ss", "sd"])
    width = 16 if t in ("ss", "sd") else rng.choice([16, 32])
    size = {"ss": 4, "sd": 8}.get(t, width)
    source = mem(rng, size) if rng.random() < 0.3 else name(b, width)
    return [f"{rng.choice(FMA_NAMES)}{t} {name(d, width)}, {name(a, width)}, {source}"]


def convert(rng):
    width = rng.choice([16, 32])
    roll = rng.random()
    if roll < 0.4:
        op = rng.choice(["vcvtdq2ps", "vcvtps2dq", "vcvttps2dq"])
        return [f"{op} {name(reg(rng), width)}, {name(reg(rng), width)}"]
    if roll < 0.7:
        d, a = reg(rng), reg(rng)
        size = rng.choice([32, 64])
        return [f"{rng.choice(['vcvtsi2ss', 'vcvtsi2sd'])} xmm{d}, xmm{a}, {sse.gpr(rng, size)}"]
    if roll < 0.85:
        d, a, b = three(rng)
        return [f"{rng.choice(['vcvtss2sd', 'vcvtsd2ss'])} xmm{d}, xmm{a}, xmm{b}"]
    op = rng.choice(["vcvttss2si", "vcvttsd2si", "vcvtss2si", "vcvtsd2si"])
    return [f"{op} {sse.gpr(rng, rng.choice([32, 64]))}, xmm{reg(rng)}"]


def sse3(rng):
    """SSE3 in its legacy form, and its VEX forms at 128 and 256 bits, which run the same code."""
    roll = rng.random()
    if roll < 0.6:
        op = rng.choice(["addsubps", "addsubpd", "haddps", "haddpd", "hsubps", "hsubpd"])
        if rng.random() < 0.6:
            source = mem(rng, 16, True) if rng.random() < 0.3 else f"xmm{reg(rng)}"
            return [f"{op} xmm{reg(rng)}, {source}"]
        return [binary(rng, ["v" + op], rng.choice([16, 32]))]
    op = rng.choice(["movddup", "movsldup", "movshdup", "lddqu"])
    size = 8 if op == "movddup" else 16
    if rng.random() < 0.6:
        source = mem(rng, size) if op == "lddqu" or rng.random() < 0.3 else f"xmm{reg(rng)}"
        return [f"{op} xmm{reg(rng)}, {source}"]
    width = rng.choice([16, 32])
    vsize = width if op != "movddup" or width == 32 else 8
    source = mem(rng, vsize) if op == "lddqu" or rng.random() < 0.3 else name(reg(rng), width)
    return [f"v{op} {name(reg(rng), width)}, {source}"]


def ssse3(rng):
    """SSSE3 in its legacy form."""
    d = reg(rng)
    source = mem(rng, 16, True) if rng.random() < 0.3 else f"xmm{reg(rng)}"
    if rng.random() < 0.15:
        return [f"palignr xmm{d}, {source}, {rng.choice([0, 1, 4, 8, 15, 16, 17, 24, 31, 32, 200, rng.randrange(256)])}"]
    op = rng.choice(["pshufb", "phaddw", "phaddd", "phaddsw", "pmaddubsw", "phsubw", "phsubd", "phsubsw", "psignb", "psignw",
                     "psignd", "pmulhrsw", "pabsb", "pabsw", "pabsd"])
    return [f"{op} xmm{d}, {source}"]


# Each family: how its cases are made, what its lanes hold and how many there are.
FAMILIES = [
    ("moves", moves, "mixed", 500),
    ("transitions", transitions, "f32", 300),
    ("arithmetic", arithmetic, "mixed", 900),
    ("logic", logic, "mixed", 200),
    ("compare", compare, "mixed", 500),
    ("integer", integer, "int", 700),
    ("shuffle", shuffle, "int", 400),
    ("lanes", lanes, "mixed", 400),
    ("fma", fma, "mixed", 1200),
    ("convert", convert, "mixed", 400),
]
# Families added after the first run on x86 silicon come after the FMA3 edges, so the earlier cases keep their numbers.
LATER_FAMILIES = [
    ("sse3", sse3, "mixed", 500),
    ("ssse3", ssse3, "int", 500),
]


def lanes_of(body, kind):
    text = body[-1]
    op = text.split()[0]
    if kind != "mixed":
        return kind
    if op.startswith("vf"):
        return "f32" if op.endswith(("ps", "ss")) else "f64"
    if op.lstrip("v").startswith(("addsub", "hadd", "hsub")):
        return "f32" if op.endswith("ps") else "f64"
    return sse.lanes_of([op[1:] + " " + " ".join(text.split()[1:])], kind)


def fma_edges():
    """FMA3 on the values where a fused operation and x86 can disagree: a NaN (quiet or signaling) in each of the three positions,
    infinity times zero plus a quiet NaN, products that cancel the addend exactly, results at the smallest normal, in every rounding
    mode with and without FZ and DAZ."""
    cases = []
    q32, s32, q64, s64 = 0x7FC12345, 0x7F812345, 0x7FF8000012345678, 0x7FF0000012345678
    values32 = [0x3F800000, 0x00000000, 0x80000000, 0x7F800000, 0xFF800000, q32, s32, 0xFFC00001, 0x00800000, 0x80800000,
                0x00000001, 0x3F000000, 0x00400000, 0x7F7FFFFF, 0x34000000, 0xBF800001]
    values64 = [0x3FF0000000000000, 0, 0x8000000000000000, 0x7FF0000000000000, 0xFFF0000000000000, q64, s64,
                0xFFF8000000000001, 0x0010000000000000, 0x8010000000000000, 1, 0x3FE0000000000000, 0x0008000000000000,
                0x7FEFFFFFFFFFFFFF, 0x3CB0000000000000, 0xBFF0000000000001]
    rng = random.Random(20261219)
    for op in FMA_NAMES:
        for t in ("ss", "sd"):
            pool = values32 if t == "ss" else values64
            for _ in range(12):
                x, y, z = (rng.choice(pool) for _ in range(3))
                control = 0x1F80 | rng.randrange(4) << 13 | (0x8000 if rng.random() < 0.2 else 0) | (0x40 if rng.random() < 0.2 else 0)
                cases.append((f"{op}{t} xmm1, xmm2, xmm3", t, x, y, z, control))
    return cases


def main():
    assembly, header = sys.argv[1:3]
    lines = [".intel_syntax noprefix", ".text"]
    # run_case(io = rdi, code = rsi). io: 16 registers, the flags, MXCSR, then YMM0-YMM15 at 144 (32 bytes each) and the buffer r15
    # points at, at 672.
    lines += [
        ".globl _run_case", "_run_case:",
        "push rbx", "push rbp", "push r12", "push r13", "push r14", "push r15", "sub rsp, 8",
        "mov qword ptr [rip + saved_io], rdi", "mov qword ptr [rip + saved_code], rsi", "stmxcsr dword ptr [rip + host_mxcsr]",
    ]
    lines += [f"vmovdqu ymm{i}, ymmword ptr [rdi + {144 + 32 * i}]" for i in range(16)]
    lines += ["ldmxcsr dword ptr [rdi + 136]", "push qword ptr [rdi + 128]", "popfq"]
    for index, reg_name in enumerate(sse.GPR[64]):
        if reg_name not in ("rsp", "rdi", "r15"):
            lines.append(f"mov {reg_name}, qword ptr [rdi + {8 * index}]")
    lines += ["lea r15, [rdi + 672]", "mov rdi, qword ptr [rdi + 56]", "call qword ptr [rip + saved_code]",
              "pushfq", "push rdi", "mov rdi, qword ptr [rip + saved_io]", "pop qword ptr [rdi + 56]", "pop qword ptr [rdi + 128]"]
    for index, reg_name in enumerate(sse.GPR[64]):
        if reg_name not in ("rsp", "rdi", "r15"):
            lines.append(f"mov qword ptr [rdi + {8 * index}], {reg_name}")
    lines += ["stmxcsr dword ptr [rdi + 136]"]
    lines += [f"vmovdqu ymmword ptr [rdi + {144 + 32 * i}], ymm{i}" for i in range(16)]
    lines += ["vzeroupper", "ldmxcsr dword ptr [rip + host_mxcsr]", "add rsp, 8", "pop r15", "pop r14", "pop r13", "pop r12",
              "pop rbp", "pop rbx", "ret"]
    cases = []

    def add(family, registers, flags, control, vectors, memory, body):
        number = len(cases)
        lines.extend([f".globl _case_{number}", f".globl _case_{number}_end", f"_case_{number}:"] + body + ["ret",
                                                                                                         f"_case_{number}_end:"])
        cases.append((family, registers, flags, control, vectors, memory, body))

    def generate(families):
        for family, make, kind, count in families:
            make_family(family, make, kind, count)

    def make_family(family, make, kind, count):
        rng = random.Random(zlib.crc32(family.encode()) ^ 20261219)
        floating = kind != "int"
        for _ in range(count):
            registers = [rng.choice([rng.getrandbits(64), rng.choice(sse.INTEGERS), rng.getrandbits(8)]) for _ in range(16)]
            body = make(rng)
            lane_kind = lanes_of(body, kind)
            vectors = [[sse.quadword(rng, lane_kind) for _ in range(4)] for _ in range(16)]
            memory = [sse.quadword(rng, lane_kind) for _ in range(24)]
            add(family, registers, rng.getrandbits(12) & ALL_FLAGS, sse.mxcsr(rng, floating), vectors, memory, body)
    generate(FAMILIES)
    for text, t, x, y, z, control in fma_edges():
        vectors = [[0x1111111111111111 * (i % 15 + 1)] * 4 for i in range(16)]
        for index, value in ((1, x), (2, y), (3, z)):
            # Element 1 of a single holds pi: the result's must come from the first source.
            low = value | (0x40490FDB << 32) if t == "ss" else value
            vectors[index] = [low, 0x0123456789ABCDEF, 0xFEDCBA9876543210, 0x5555AAAA5555AAAA]
        add("fma edges", [0] * 16, 0, control, vectors, [0] * 24, [text])
    generate(LATER_FAMILIES)
    lines += [".data", "saved_io: .quad 0", "saved_code: .quad 0", "host_mxcsr: .long 0", ""]
    with open(assembly, "w") as out:
        out.write("\n".join(lines))
    with open(header, "w") as out:
        out.write("// Written by x86_avx.py.\n")
        for number in range(len(cases)):
            out.write(f"extern char case_{number}[], case_{number}_end[];\n")
        out.write("static const struct Case cases[] = {\n")
        for number, (family, registers, flags, control, vectors, memory, body) in enumerate(cases):
            values = ", ".join(f"{value:#x}ull" for value in registers)
            ymm = ", ".join("{" + ", ".join(f"{q:#x}ull" for q in quads) + "}" for quads in vectors)
            words = ", ".join(f"{value:#x}ull" for value in memory)
            text = "; ".join(body).replace('"', '\\"')
            out.write(f"\t{{{{{values}}}, {flags:#x}, {control:#x}, {{{ymm}}}, {{{words}}}, \"{family}\", case_{number}, "
                      f"case_{number}_end, \"{text}\"}},\n")
        out.write("};\n")


if __name__ == "__main__":
    main()
