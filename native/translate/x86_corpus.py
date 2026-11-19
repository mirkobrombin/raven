"""Builds the reference corpus the x86-64 decoder is checked against: LLVM's disassembler is the oracle.

usage: x86_corpus.py CLANG OBJDUMP OUTPUT [PE_OR_OBJECT...]

Writes one line per instruction, "hex bytes<TAB>address<TAB>Intel text", from four sources: random instances of the instructions the
translator handles, of SSE and SSE2 and of x87 (assembled by CLANG), a stream of random bytes, and the code sections of every PE given. The
seeds are fixed, so the corpus is the same on every build.
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
PTR = {8: "byte", 16: "word", 32: "dword", 64: "qword", 80: "tbyte", 128: "xmmword"}
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
        if rng.random() < 0.25:
            xmm = f"xmm{rng.randrange(16)}"
            return f"movq {xmm}, {register(rng, 64)}" if rng.random() < 0.5 else f"movq {register(rng, 64)}, {xmm}"
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


# SSE and SSE2 on XMM registers, by operand form: V an XMM register, W an XMM register or memory of the size given (in bits), U an XMM
# register alone, M memory alone, E a general register or memory, G a general register, I an immediate.
SSE_FORMS = {
    "V,W128": "unpcklps unpcklpd unpckhps unpckhpd sqrtps sqrtpd andps andpd andnps andnpd orps orpd xorps xorpd addps addpd mulps "
              "mulpd subps subpd minps minpd divps divpd maxps maxpd cvtpd2ps cvtdq2ps cvtps2dq cvttps2dq punpcklbw punpcklwd "
              "punpckldq packsswb pcmpgtb pcmpgtw pcmpgtd packuswb punpckhbw punpckhwd punpckhdq packssdw punpcklqdq punpckhqdq "
              "psrlw psraw psllw psrld psrad pslld psrlq psllq pcmpeqb pcmpeqw pcmpeqd paddq pmullw psubusb psubusw pminub pand "
              "paddusb paddusw pmaxub pandn pavgb pavgw pmulhuw pmulhw cvttpd2dq cvtpd2dq psubsb psubsw pminsw por paddsb paddsw "
              "pmaxsw pxor pmuludq pmaddwd psadbw psubb psubw psubd psubq paddb paddw paddd movups movupd movaps movapd movdqa "
              "movdqu",
    "W128,V": "movups movupd movaps movapd movdqa movdqu",
    "M128,V": "movntps movntpd movntdq",
    "V,W32": "movss sqrtss addss mulss subss minss divss maxss ucomiss comiss cvtss2sd",
    "V,W64": "movsd sqrtsd addsd mulsd subsd minsd divsd maxsd ucomisd comisd cvtps2pd cvtsd2ss cvtdq2pd movq",
    "M32,V": "movss movd",
    "M64,V": "movsd movlps movhps movlpd movhpd movq",
    "V,M64": "movlps movhps movlpd movhpd",
    "V,U": "movhlps movlhps",
    "V,E32": "cvtsi2ss cvtsi2sd movd",
    "V,E64": "cvtsi2ss cvtsi2sd movq",
    "E32,V": "movd",
    "E64,V": "movq",
    "G32,W32": "cvttss2si cvtss2si",
    "G64,W32": "cvttss2si cvtss2si",
    "G32,W64": "cvttsd2si cvtsd2si",
    "G64,W64": "cvttsd2si cvtsd2si",
    "G32,U": "movmskps movmskpd pmovmskb",
    "V,W128,I": "pshufd pshufhw pshuflw shufps shufpd",
    "U,I": "psrlw psraw psllw psrld psrad pslld psrlq psllq psrldq pslldq",
    "M32": "ldmxcsr stmxcsr",
    "C128": "ps pd",
    "C32": "ss",
    "C64": "sd",
}
SSE_PREDICATES = ["eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"]


def sse_instance(rng):
    form, names = rng.choice(list(SSE_FORMS.items()))
    name = rng.choice(names.split())
    xmm = lambda: f"xmm{rng.randrange(16)}"
    if form.startswith("C"):
        source = xmm() if rng.random() < 0.5 else memory(rng, int(form[1:]))
        return f"cmp{rng.choice(SSE_PREDICATES)}{name} {xmm()}, {source}"
    operands = []
    for part in form.split(","):
        kind, bits = part[0], int(part[1:]) if len(part) > 1 else 0
        if kind == "V" or kind == "U" or (kind == "W" and rng.random() < 0.5):
            operands.append(xmm())
        elif kind in "WM":
            operands.append(memory(rng, bits))
        elif kind == "E":
            operands.append(rm(rng, bits))
        elif kind == "G":
            operands.append(register(rng, bits))
        else:
            operands.append(str(rng.randrange(256)))
    return f"{name} {', '.join(operands)}"


X87_MEMORY = {
    4: "fadd fmul fcom fcomp fsub fsubr fdiv fdivr fld fst fstp fiadd fimul ficom ficomp fisub fisubr fidiv fidivr fild fisttp fist "
       "fistp",
    8: "fadd fmul fcom fcomp fsub fsubr fdiv fdivr fld fst fstp fild fisttp fistp",
    2: "fiadd fimul ficom ficomp fisub fisubr fidiv fidivr fild fisttp fist fistp fldcw fnstcw fnstsw",
    10: "fld fstp fbld fbstp",
    0: "fldenv fnstenv frstor fnsave",
}
X87_REGISTER = {
    "st, st(i)": "fadd fmul fsub fsubr fdiv fdivr fcmovb fcmove fcmovbe fcmovu fcmovnb fcmovne fcmovnbe fcmovnu fucomi fcomi fucompi "
                 "fcompi",
    "st(i), st": "fadd fmul fsub fsubr fdiv fdivr faddp fmulp fsubp fsubrp fdivp fdivrp",
    "st(i)": "fcom fcomp fld fxch ffree fst fstp fucom fucomp ffreep",
}
X87_NULLARY = ("fnop fchs fabs ftst fxam fld1 fldl2t fldl2e fldpi fldlg2 fldln2 fldz f2xm1 fyl2x fptan fpatan fxtract fprem1 fdecstp "
               "fincstp fprem fyl2xp1 fsqrt fsincos frndint fscale fsin fcos fucompp fnclex fninit fcompp wait").split() + ["fnstsw ax"]


# VEX encodings, as bytes, in the opcodes the decoder gives VEX semantics to and their neighbours: map 1 (all of it), the FMA3 and
# broadcast rows of map 2, and the 128-bit lane moves of map 3, with every VEX field random. LLVM decides which are valid.
VEX_MAP2 = list(range(0x96, 0xc0)) + [0x18, 0x19, 0x1a, 0x58, 0x59, 0x5a, 0x78, 0x79]
VEX_MAP3 = [0x06, 0x18, 0x19, 0x38, 0x39, 0x46, 0x00, 0x01]


def vex_instance(rng):
    vex_map = rng.choice([1, 1, 1, 2, 3])
    opcode = rng.randrange(256) if vex_map == 1 else rng.choice(VEX_MAP2 if vex_map == 2 else VEX_MAP3)
    w, vvvv, l, pp = rng.randrange(2), rng.randrange(16), rng.randrange(2), rng.choice([0, 1, 1, 2, 3]) if vex_map == 1 else 1
    if rng.random() < 0.4:
        vvvv = 15
    r, x, b = rng.randrange(2), rng.randrange(2), rng.randrange(2)
    if vex_map == 1 and w == 0 and x == 1 and b == 1 and rng.random() < 0.5:
        prefix = [0xc5, r << 7 | vvvv << 3 | l << 2 | pp]
    else:
        prefix = [0xc4, r << 7 | x << 6 | b << 5 | vex_map, w << 7 | vvvv << 3 | l << 2 | pp]
    modrm = rng.randrange(256) if rng.random() < 0.5 else 0xc0 | rng.randrange(64)
    mod, rm = modrm >> 6, modrm & 7
    rest = [modrm]
    if mod != 3 and rm == 4:
        sib = rng.randrange(256)
        rest.append(sib)
        if mod == 0 and sib & 7 == 5:
            rest += [rng.randrange(256) for _ in range(4)]
    if mod == 1:
        rest.append(rng.randrange(256))
    elif mod == 2 or (mod == 0 and rm == 5):
        rest += [rng.randrange(256) for _ in range(4)]
    if vex_map == 3 or (vex_map == 1 and opcode in (0xc2, 0xc4, 0xc5, 0xc6, 0x70, 0x71, 0x72, 0x73)):
        rest.append(rng.randrange(256))
    return ".byte " + ", ".join(str(value) for value in prefix + [opcode] + rest)


def x87_instance(rng):
    roll = rng.random()
    if roll < 0.45:
        size = rng.choice(list(X87_MEMORY))
        name = rng.choice(X87_MEMORY[size].split())
        return f"{name} {memory(rng, size * 8, sized=size != 0)}"
    if roll < 0.85:
        form = rng.choice(list(X87_REGISTER))
        return f"{rng.choice(X87_REGISTER[form].split())} {form.replace('i', str(rng.randrange(8)))}"
    return rng.choice(X87_NULLARY)


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
        sse_rng = random.Random(20261119)
        sse_source = Path(scratch) / "sse_instances.s"
        sse_source.write_text(".intel_syntax noprefix\n.text\n" + "".join(sse_instance(sse_rng) + "\n" for _ in range(8000)))
        sse_obj = Path(scratch) / "sse_instances.o"
        subprocess.run([clang, "-c", "-x", "assembler", "-o", str(sse_obj), str(sse_source)], check=True)
        x87_rng = random.Random(20261125)
        x87_source = Path(scratch) / "x87_instances.s"
        x87_source.write_text(".intel_syntax noprefix\n.text\n" + "".join(x87_instance(x87_rng) + "\n" for _ in range(4000)))
        x87_obj = Path(scratch) / "x87_instances.o"
        subprocess.run([clang, "-c", "-x", "assembler", "-o", str(x87_obj), str(x87_source)], check=True)
        vex_rng = random.Random(20261119)
        vex_source = Path(scratch) / "vex_instances.s"
        vex_source.write_text(".text\n" + "".join(vex_instance(vex_rng) + "\n" for _ in range(20000)))
        vex_obj = Path(scratch) / "vex_instances.o"
        subprocess.run([clang, "-c", "-x", "assembler", "-o", str(vex_obj), str(vex_source)], check=True)
        noise = Path(scratch) / "noise.s"
        noise.write_text(".text\n" + "".join(f".byte {', '.join(str(rng.randrange(256)) for _ in range(16))}\n" for _ in range(8192)))
        noise_obj = Path(scratch) / "noise.o"
        subprocess.run([clang, "-c", "-x", "assembler", "-o", str(noise_obj), str(noise)], check=True)
        for path in [obj, sse_obj, x87_obj, vex_obj, noise_obj] + [Path(image) for image in images]:
            source_name = path.name
            lines.append(f"# {source_name}")
            for raw, address, text in disassemble(objdump, str(path)):
                lines.append(f"{raw}\t{address:x}\t{text}")
    Path(output).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
