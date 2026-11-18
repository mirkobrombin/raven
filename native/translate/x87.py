"""Writes x87 cases for the translator's oracle test: a few instructions each, run from a whole x87 state (the eight registers, TOP,
the tags, the control and status words) chosen around the edges of the 80-bit format and of each precision and rounding control.

usage: x87.py OUTPUT_ASSEMBLY OUTPUT_HEADER

The assembly holds one function per case and run_case, which loads a case's registers, flags and FXSAVE image (fxrstor64), calls it
and stores what it left (fxsave64), then gives the caller its own x87 state back. x87_oracle.c runs them on an x86 CPU or under
Rosetta; the translator runs the same bytes and must agree on the general registers, the flags, the x87 state and the buffer r15
points at. The x87 instructions are written as bytes, encoded here, so every assembler builds the same code.
"""

import random
import struct
import sys

GPR64 = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"] + [f"r{i}" for i in range(8, 16)]
ALL_FLAGS = 1 | 4 | 16 | 64 | 128 | 2048
ONE = 1 << 63
QUIET = 1 << 62

# 80-bit values as (sign and exponent, significand).
E80 = [
    (0, 0), (0x8000, 0), (0x3FFF, ONE), (0xBFFF, ONE), (0x3FFF, 0xC000000000000000), (0x4000, ONE), (0x4000, 0xC000000000000000),
    (0x3FFE, ONE), (0xBFFD, 0xA000000000000000), (0x4002, 0xA000000000000000), (0x401D, 0xFFFFFFFE00000000),
    (0x7FFF, ONE), (0xFFFF, ONE),
    (0x7FFF, 0xC000000000000000), (0xFFFF, 0xC000000000000000), (0x7FFF, 0xC000000012345678), (0xFFFF, 0xE000000000000001),
    (0x7FFF, 0x8000000000000001), (0xFFFF, 0xA000000000000000), (0x7FFF, 0x8000000087654321),
    (0, 1), (0, 0x7FFFFFFFFFFFFFFF), (0x8000, 0x0000000100000000), (0, 0x4000000000000000),
    (0, 0x8000000000000001), (0x8000, ONE),
    (0x3FFF, 0x4000000000000000), (0x1234, 0), (0xC000, 0x0000000000000001),
    (0x7FFF, 0), (0xFFFF, 0), (0x7FFF, 0x4000000000000001), (0xFFFF, 0x0000000000000001),
    (0x7FFE, 0xFFFFFFFFFFFFFFFF), (0xFFFE, 0xFFFFFFFFFFFFFFFF), (0x0001, ONE), (0x8001, ONE), (0x0001, 0xFFFFFFFFFFFFFFFF),
    (0x43FF, ONE), (0x43FE, 0xFFFFFFFFFFFFF800), (0x43FE, 0xFFFFFFFFFFFFFC00), (0x3C00, ONE), (0x3BCD, 0xC000000000000000),
    (0x407E, 0xFFFFFF8000000000), (0x407E, 0xFFFFFFC000000000), (0x3F80, ONE), (0x3F6A, ONE),
    (0x7000, 0xD555555555555555), (0x0100, 0xAAAAAAAAAAAAAAAB), (0x7FFD, ONE),
    (0x3FFF, ONE | 1 << 10), (0x3FFF, ONE | 1 << 10 | 1), (0x3FFF, ONE | 3 << 10), (0xBFFF, ONE | 1 << 10),
    (0x3FFF, ONE | 1 << 39), (0x3FFF, ONE | 3 << 39), (0x3FFF, ONE | 1 << 39 | 1), (0x3FFF, 0xFFFFFFFFFFFFFFFF),
    (0x3FFF, 0xFFFFFFFFFFFFF800), (0x3FFF, 0xFFFFFF8000000000), (0x4000, 0xC90FDAA22168C235), (0x3FFE, 0xB17217F7D1CF79AC),
    (0x403E, ONE), (0xC03E, ONE), (0x403D, 0xFFFFFFFFFFFFFFFF), (0x401E, ONE), (0xC01E, ONE), (0x401D, 0xFFFFFFFF00000000),
    (0x400E, ONE), (0xC00E, ONE), (0x400D, 0xFFFE000000000000), (0x3FFE, 0xC000000000000000), (0x4000, 0xA000000000000000),
    (0xBFFE, 0xC000000000000000), (0x3FFD, ONE), (0x403E, 0xC000000000000000),
]
F32 = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3FC00000, 0x40200000, 0xBF000000, 0x00000001, 0x007FFFFF, 0x80000001,
       0x00800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7FC12345, 0x7F800001, 0x7FA00000,
       0xFF812345, 0x4F000000, 0xCF000000, 0x3DCCCCCD, 0x501502F9]
F64 = [0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x3FF8000000000000, 0x4004000000000000,
       0x0000000000000001, 0x000FFFFFFFFFFFFF, 0x8000000000000001, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF,
       0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0xFFF8000000000000, 0x7FF8000012345678, 0x7FF0000000000001,
       0x7FF4000000000000, 0xFFF0000012345678, 0x41E0000000000000, 0xC1E0000000000000, 0x43E0000000000000, 0x3FB999999999999A,
       0x400921FB54442D18, 0x3FE0000000000000]
INT16 = [0, 1, 0xFFFF, 0x7FFF, 0x8000, 0x8001, 0x1234, 0xFFFE]
INT32 = [0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x80000001, 0x01000001, 0x12345678, 0xFFFF8000]
INT64 = [0, 1, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0x8000000000000001, 0x0020000000000001,
         0x123456789ABCDEF1, 0xFFFFFFFF80000000]

# Memory forms: name -> (opcode, ModRM.reg, size in bytes).
MEMORY = {}
for index, name in enumerate(["fadd", "fmul", "fcom", "fcomp", "fsub", "fsubr", "fdiv", "fdivr"]):
    MEMORY[(name, 4)] = (0xD8, index)
    MEMORY[(name, 8)] = (0xDC, index)
for index, name in enumerate(["fiadd", "fimul", "ficom", "ficomp", "fisub", "fisubr", "fidiv", "fidivr"]):
    MEMORY[(name, 4)] = (0xDA, index)
    MEMORY[(name, 2)] = (0xDE, index)
MEMORY.update({
    ("fld", 4): (0xD9, 0), ("fst", 4): (0xD9, 2), ("fstp", 4): (0xD9, 3), ("fldenv", 28): (0xD9, 4), ("fldcw", 2): (0xD9, 5),
    ("fnstenv", 28): (0xD9, 6), ("fnstcw", 2): (0xD9, 7),
    ("fild", 4): (0xDB, 0), ("fisttp", 4): (0xDB, 1), ("fist", 4): (0xDB, 2), ("fistp", 4): (0xDB, 3), ("fld", 10): (0xDB, 5),
    ("fstp", 10): (0xDB, 7),
    ("fld", 8): (0xDD, 0), ("fisttp", 8): (0xDD, 1), ("fst", 8): (0xDD, 2), ("fstp", 8): (0xDD, 3), ("frstor", 108): (0xDD, 4),
    ("fnsave", 108): (0xDD, 6), ("fnstsw", 2): (0xDD, 7),
    ("fild", 2): (0xDF, 0), ("fisttp", 2): (0xDF, 1), ("fist", 2): (0xDF, 2), ("fistp", 2): (0xDF, 3), ("fild", 8): (0xDF, 5),
    ("fistp", 8): (0xDF, 7),
})
PTR = {2: "word ptr ", 4: "dword ptr ", 8: "qword ptr ", 10: "tbyte ptr ", 28: "", 108: ""}
# Register forms: name -> (opcode, ModRM base, form): "st,i" for st, st(i); "i,st" for st(i), st; "i" for st(i); "" for none.
REGISTER = {}
for index, name in enumerate(["fadd", "fmul", "fcom", "fcomp", "fsub", "fsubr", "fdiv", "fdivr"]):
    REGISTER[name] = (0xD8, 0xC0 + 8 * index, "i" if name in ("fcom", "fcomp") else "st,i")
for name, base in [("fadd", 0xC0), ("fmul", 0xC8), ("fsubr", 0xE0), ("fsub", 0xE8), ("fdivr", 0xF0), ("fdiv", 0xF8)]:
    REGISTER[name + "/r"] = (0xDC, base, "i,st")
for name, base in [("faddp", 0xC0), ("fmulp", 0xC8), ("fsubrp", 0xE0), ("fsubp", 0xE8), ("fdivrp", 0xF0), ("fdivp", 0xF8)]:
    REGISTER[name] = (0xDE, base, "i,st")
REGISTER.update({
    "fld": (0xD9, 0xC0, "i"), "fxch": (0xD9, 0xC8, "i"), "ffree": (0xDD, 0xC0, "i"), "fst": (0xDD, 0xD0, "i"),
    "fstp": (0xDD, 0xD8, "i"), "fucom": (0xDD, 0xE0, "i"), "fucomp": (0xDD, 0xE8, "i"), "ffreep": (0xDF, 0xC0, "i"),
    "fcmovb": (0xDA, 0xC0, "st,i"), "fcmove": (0xDA, 0xC8, "st,i"), "fcmovbe": (0xDA, 0xD0, "st,i"), "fcmovu": (0xDA, 0xD8, "st,i"),
    "fcmovnb": (0xDB, 0xC0, "st,i"), "fcmovne": (0xDB, 0xC8, "st,i"), "fcmovnbe": (0xDB, 0xD0, "st,i"),
    "fcmovnu": (0xDB, 0xD8, "st,i"), "fucomi": (0xDB, 0xE8, "st,i"), "fcomi": (0xDB, 0xF0, "st,i"),
    "fucompi": (0xDF, 0xE8, "st,i"), "fcompi": (0xDF, 0xF0, "st,i"),
})
NULLARY = {
    "fnop": (0xD9, 0xD0), "fchs": (0xD9, 0xE0), "fabs": (0xD9, 0xE1), "ftst": (0xD9, 0xE4), "fxam": (0xD9, 0xE5), "fld1": (0xD9, 0xE8),
    "fldl2t": (0xD9, 0xE9), "fldl2e": (0xD9, 0xEA), "fldpi": (0xD9, 0xEB), "fldlg2": (0xD9, 0xEC), "fldln2": (0xD9, 0xED),
    "fldz": (0xD9, 0xEE), "f2xm1": (0xD9, 0xF0), "fyl2x": (0xD9, 0xF1), "fptan": (0xD9, 0xF2), "fpatan": (0xD9, 0xF3),
    "fxtract": (0xD9, 0xF4), "fprem1": (0xD9, 0xF5), "fdecstp": (0xD9, 0xF6), "fincstp": (0xD9, 0xF7), "fprem": (0xD9, 0xF8),
    "fyl2xp1": (0xD9, 0xF9), "fsqrt": (0xD9, 0xFA), "fsincos": (0xD9, 0xFB), "frndint": (0xD9, 0xFC), "fscale": (0xD9, 0xFD),
    "fsin": (0xD9, 0xFE), "fcos": (0xD9, 0xFF), "fucompp": (0xDA, 0xE9), "fnclex": (0xDB, 0xE2), "fninit": (0xDB, 0xE3),
    "fcompp": (0xDE, 0xD9), "fnstsw ax": (0xDF, 0xE0), "wait": (0x9B, None),
}
CONDITIONS = ["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"]


class Insn:
    def __init__(self, text, code):
        self.text = text
        self.code = code


def memory_insn(name, size, offset):
    opcode, reg = MEMORY[(name, size)]
    # [r15 + disp8]: REX.B, ModRM mod 01, rm 7.
    return Insn(f"{name} {PTR[size]}[r15 + {offset}]" if offset else f"{name} {PTR[size]}[r15]",
                bytes([0x41, opcode, 0x40 | reg << 3 | 7, offset]))


def register_insn(name, i):
    opcode, base, form = REGISTER[name]
    shown = name.split("/")[0]
    operands = {"st,i": f"st, st({i})", "i,st": f"st({i}), st", "i": f"st({i})"}[form]
    return Insn(f"{shown} {operands}", bytes([opcode, base + i]))


def nullary(name):
    opcode, modrm = NULLARY[name]
    return Insn(name, bytes([opcode]) if modrm is None else bytes([opcode, modrm]))


def setcc(condition):
    return Insn(f"set{CONDITIONS[condition]} al", bytes([0x0F, 0x90 + condition, 0xC0]))


def value80(rng):
    roll = rng.random()
    if roll < 0.55:
        return rng.choice(E80)
    if roll < 0.9:
        sign = rng.choice([0, 0x8000])
        exponent = 0x3FFF + rng.randint(-70, 70) if rng.random() < 0.8 else rng.randint(1, 0x7FFE)
        return (sign | exponent, ONE | rng.getrandbits(63) & rng.choice([(1 << 63) - 1, ~((1 << 11) - 1) & ((1 << 63) - 1)]))
    if roll < 0.95:
        return (rng.choice([0, 0x8000]) | 0x3FFF + rng.randint(-3, 3), ONE | rng.randrange(8) << 60)
    return (rng.getrandbits(16), rng.getrandbits(64))


def integer_value(rng, size):
    pool = {2: INT16, 4: INT32, 8: INT64}[size]
    if rng.random() < 0.5:
        return rng.choice(pool)
    return rng.getrandbits(8 * size) >> rng.choice([0, 0, 4, 8 * size - 12])


class State:
    """An x87 state as fxrstor64 loads it, with physical registers R0-R7."""

    def __init__(self, rng, single):
        self.top = rng.randrange(8)
        roll = rng.random()
        self.depth = 8 if roll < 0.1 else 0 if roll < 0.15 else rng.randint(1, 7)
        self.regs = [value80(rng) for _ in range(8)]
        pc = rng.choices([3, 2, 0, 1], [45, 40, 10, 5])[0]
        rc = rng.randrange(4)
        masks = 0x3F
        if single and rng.random() < 0.12:
            masks = rng.getrandbits(6)
        self.fcw = 0x40 | masks | pc << 8 | rc << 10
        codes = rng.getrandbits(4) if rng.random() < 0.5 else 0
        self.fsw = (codes & 7) << 8 | (codes >> 3) << 14
        if masks == 0x3F and rng.random() < 0.3:
            self.fsw |= rng.getrandbits(7)
        self.fsw |= self.top << 11

    def stack(self, i):
        return (self.top + i) % 8

    def ftw(self):
        return sum(1 << self.stack(j) for j in range(self.depth))

    def image(self):
        """The 512-byte FXSAVE image: FCW, FSW, abridged FTW, FOP, FIP, FDP, MXCSR, MXCSR_MASK, ST0-ST7, XMM0-XMM15."""
        data = bytearray(512)
        struct.pack_into("<HHBBHQQII", data, 0, self.fcw, self.fsw, self.ftw(), 0, 0, 0, 0, 0x1F80, 0xFFFF)
        for i in range(8):
            sign_exponent, significand = self.regs[self.stack(i)]
            struct.pack_into("<QH", data, 32 + 16 * i, significand, sign_exponent)
        return bytes(data)


def operand_size(rng, sizes):
    return rng.choice(sizes)


def offset(rng, size):
    return rng.randrange(0, 128 - min(size, 108) + 1) if size < 108 else rng.randrange(0, 20)


def fill_memory(rng, memory, at, size, kind):
    """Writes a value of the operand's kind at buffer offset at."""
    if kind == "f32":
        value = rng.choice(F32) if rng.random() < 0.6 else rng.getrandbits(32)
        memory[at:at + 4] = struct.pack("<I", value)
    elif kind == "f64":
        value = rng.choice(F64) if rng.random() < 0.6 else rng.getrandbits(64)
        memory[at:at + 8] = struct.pack("<Q", value)
    elif kind == "f80":
        sign_exponent, significand = value80(rng)
        memory[at:at + 10] = struct.pack("<QH", significand, sign_exponent)
    elif kind == "int":
        memory[at:at + size] = integer_value(rng, size).to_bytes(size, "little")


def float_kind(size):
    return {4: "f32", 8: "f64", 10: "f80"}[size]


ARITHMETIC = ["fadd", "fmul", "fsub", "fsubr", "fdiv", "fdivr"]


def moves(rng, state, memory):
    roll = rng.randrange(6)
    if roll == 0:
        size = rng.choice([4, 8, 10])
        at = offset(rng, size)
        fill_memory(rng, memory, at, size, float_kind(size))
        return [memory_insn("fld", size, at)]
    if roll == 1:
        return [register_insn("fld", rng.randrange(8))]
    if roll == 2:
        size = rng.choice([4, 8, 10])
        name = "fstp" if size == 10 else rng.choice(["fst", "fstp"])
        return [memory_insn(name, size, offset(rng, size))]
    if roll == 3:
        return [register_insn(rng.choice(["fst", "fstp"]), rng.randrange(8))]
    # A load and a store of a narrower format: the conversions' rounding with an exact source.
    size = rng.choice([4, 8, 10])
    at = offset(rng, size)
    fill_memory(rng, memory, at, size, float_kind(size))
    store = rng.choice([4, 8])
    return [memory_insn("fld", size, at), memory_insn("fstp", store, offset(rng, store))]


def constants(rng, state, memory):
    names = ["fldz", "fld1", "fldpi", "fldl2e", "fldl2t", "fldlg2", "fldln2"]
    body = [nullary(rng.choice(names))]
    if rng.random() < 0.5:
        body.append(memory_insn("fstp", 10, offset(rng, 10)))
    return body


def arithmetic(rng, state, memory):
    name = rng.choice(ARITHMETIC)
    roll = rng.randrange(7)
    if roll == 0:
        return [register_insn(name, rng.randrange(8))]
    if roll == 1:
        return [register_insn(name + "/r", rng.randrange(8))]
    if roll == 2:
        return [register_insn(name + "p", rng.randrange(8))]
    if roll in (3, 4):
        size = rng.choice([4, 8])
        at = offset(rng, size)
        fill_memory(rng, memory, at, size, float_kind(size))
        return [memory_insn(name, size, at)]
    if roll == 5:
        size = rng.choice([2, 4])
        at = offset(rng, size)
        fill_memory(rng, memory, at, size, "int")
        return [memory_insn("fi" + name[1:], size, at)]
    return [nullary(rng.choice(["fsqrt", "fabs", "fchs", "frndint", "fsqrt"]))]


def compare(rng, state, memory):
    roll = rng.randrange(9)
    tail = []
    if roll == 0:
        body = [register_insn(rng.choice(["fcom", "fcomp", "fucom", "fucomp"]), rng.randrange(8))]
    elif roll == 1:
        size = rng.choice([4, 8])
        at = offset(rng, size)
        fill_memory(rng, memory, at, size, float_kind(size))
        body = [memory_insn(rng.choice(["fcom", "fcomp"]), size, at)]
    elif roll == 2:
        size = rng.choice([2, 4])
        at = offset(rng, size)
        fill_memory(rng, memory, at, size, "int")
        body = [memory_insn(rng.choice(["ficom", "ficomp"]), size, at)]
    elif roll == 3:
        body = [nullary(rng.choice(["fcompp", "fucompp", "ftst", "fxam"]))]
    else:
        body = [register_insn(rng.choice(["fcomi", "fucomi", "fcompi", "fucompi"]), rng.randrange(8))]
        tail = [setcc(rng.randrange(16))] if rng.random() < 0.5 else []
    if not tail and rng.random() < 0.5:
        tail = [nullary("fnstsw ax")]
    return body + tail


def stack(rng, state, memory):
    roll = rng.randrange(6)
    if roll == 0:
        return [register_insn("fxch", rng.randrange(8))]
    if roll == 1:
        return [register_insn(rng.choice(["ffree", "ffreep"]), rng.randrange(8))]
    if roll == 2:
        return [nullary(rng.choice(["fincstp", "fdecstp"]))]
    if roll == 3:
        # Pushes past a full stack, or pops past an empty one: the stack fault and C1.
        return [nullary(rng.choice(["fld1", "fldz"])) for _ in range(rng.randint(1, 9))]
    if roll == 4:
        return [memory_insn("fstp", 8, offset(rng, 8)) for _ in range(rng.randint(1, 3))]
    return [nullary("fnop")]


def env_image(rng, size):
    """A random environment (28 bytes) or save area (108 bytes) as fldenv and frstor read them in 64-bit mode."""
    other = State(rng, False)
    tags = 0
    for physical in range(8):
        tags |= rng.choice([0, 1, 2, 3, 3]) << 2 * physical
    data = bytearray(struct.pack("<IIIIIII", other.fcw | 0xFFFF0000 * rng.randrange(2), other.fsw, tags, rng.getrandbits(32),
                                 rng.getrandbits(32), rng.getrandbits(32), rng.getrandbits(32)))
    if size == 108:
        for i in range(8):
            sign_exponent, significand = value80(rng)
            data += struct.pack("<QH", significand, sign_exponent)
    return bytes(data)


def control(rng, state, memory):
    roll = rng.randrange(9)
    if roll == 0:
        at = offset(rng, 2)
        word = rng.choice([0x037F, 0x027F, 0x007F, 0x0F7F, 0x0B7F, 0x077F, 0x1372]) if rng.random() < 0.6 else rng.getrandbits(16)
        # Leave every exception masked (bits 0-5) unless no flag is set: an unmasked pending flag makes the next x87 instruction fault.
        if state.fsw & 0x3F:
            word |= 0x3F
        memory[at:at + 2] = struct.pack("<H", word)
        return [memory_insn("fldcw", 2, at), memory_insn("fnstcw", 2, offset(rng, 2))] + (
            [register_insn("fadd", 1)] if rng.random() < 0.5 and not state.fsw & 0x3F else [])
    if roll == 1:
        return [memory_insn("fnstcw", 2, offset(rng, 2))]
    if roll == 2:
        return [memory_insn("fnstsw", 2, offset(rng, 2))] if rng.random() < 0.5 else [nullary("fnstsw ax")]
    if roll == 3:
        return [nullary("fnclex")]
    if roll == 4:
        return [nullary("fninit")] + ([memory_insn("fnstcw", 2, offset(rng, 2))] if rng.random() < 0.5 else [])
    if roll == 5:
        return [memory_insn("fnstenv", 28, offset(rng, 28))]
    if roll == 6:
        return [memory_insn("fnsave", 108, offset(rng, 108))]
    if roll == 7:
        size = rng.choice([28, 108])
        at = offset(rng, size)
        image = env_image(rng, size)
        memory[at:at + size] = image
        # fldenv and frstor may load unmasked flags; nothing x87 runs after them in the case.
        return [memory_insn("fldenv" if size == 28 else "frstor", size, at)]
    return [nullary("wait")]


def cmov(rng, state, memory):
    names = ["fcmovb", "fcmove", "fcmovbe", "fcmovu", "fcmovnb", "fcmovne", "fcmovnbe", "fcmovnu"]
    return [register_insn(rng.choice(names), rng.randrange(8))]


def integer(rng, state, memory):
    roll = rng.randrange(4)
    if roll == 0:
        size = rng.choice([2, 4, 8])
        at = offset(rng, size)
        fill_memory(rng, memory, at, size, "int")
        return [memory_insn("fild", size, at)]
    if roll == 1:
        size = rng.choice([2, 4])
        return [memory_insn(rng.choice(["fist", "fistp"]), size, offset(rng, size))]
    if roll == 2:
        size = rng.choice([2, 4, 8])
        return [memory_insn("fistp", size, offset(rng, size))] if size == 8 else [memory_insn("fisttp", size, offset(rng, size))]
    return [memory_insn("fisttp", 8, offset(rng, 8))]


def scale(rng, state, memory):
    return [nullary(rng.choice(["fscale", "fxtract", "fprem", "fprem1"]))]


def transcendental(rng, state, memory):
    return [nullary(rng.choice(["fsin", "fcos", "fsincos", "fptan", "fpatan", "f2xm1", "fyl2x", "fyl2xp1"]))]


def sequences(rng, state, memory):
    """What compiled long double code does: loads, arithmetic on the stack, a compare, stores."""
    body = []
    for _ in range(rng.randint(2, 3)):
        size = rng.choice([8, 8, 10, 4])
        at = offset(rng, size)
        fill_memory(rng, memory, at, size, float_kind(size))
        body.append(memory_insn("fld", size, at))
    for _ in range(rng.randint(1, 2)):
        body.append(register_insn(rng.choice(ARITHMETIC) + "p", 1))
    if rng.random() < 0.4:
        body += [register_insn(rng.choice(["fucomi", "fcomi"]), 1), setcc(rng.randrange(16))]
    name = rng.choice(["fstp", "fstp", "fistp"])
    body.append(memory_insn(name, rng.choice([8, 10]) if name == "fstp" else 8, offset(rng, 10)))
    return body


FAMILIES = [
    ("moves", moves, 900),
    ("constants", constants, 300),
    ("arithmetic", arithmetic, 2400),
    ("compare", compare, 1000),
    ("stack", stack, 400),
    ("control", control, 500),
    ("cmov", cmov, 300),
    ("integer", integer, 900),
    ("scale", scale, 500),
    ("transcendental", transcendental, 400),
    ("sequences", sequences, 400),
]


def main():
    assembly, header = sys.argv[1:3]
    rng = random.Random(20261125)
    lines = [".intel_syntax noprefix", ".text"]
    # run_case(io = rdi, code = rsi). io: 16 registers, the flags, then the FXSAVE image at 144 and the buffer r15 points at, at 656.
    lines += [
        ".globl _run_case", "_run_case:",
        "push rbx", "push rbp", "push r12", "push r13", "push r14", "push r15", "sub rsp, 8",
        "mov qword ptr [rip + saved_io], rdi", "mov qword ptr [rip + saved_code], rsi", "fxsave64 [rip + _host_fx]",
        "fxrstor64 [rdi + 144]", "push qword ptr [rdi + 128]", "popfq",
    ]
    for index, name in enumerate(GPR64):
        if name not in ("rsp", "rdi", "r15"):
            lines.append(f"mov {name}, qword ptr [rdi + {8 * index}]")
    lines += ["lea r15, [rdi + 656]", "mov rdi, qword ptr [rdi + 56]", "call qword ptr [rip + saved_code]",
              "pushfq", "push rdi", "mov rdi, qword ptr [rip + saved_io]", "pop qword ptr [rdi + 56]", "pop qword ptr [rdi + 128]"]
    for index, name in enumerate(GPR64):
        if name not in ("rsp", "rdi", "r15"):
            lines.append(f"mov qword ptr [rdi + {8 * index}], {name}")
    lines += ["fxsave64 [rdi + 144]", "fxrstor64 [rip + _host_fx]", "add rsp, 8", "pop r15", "pop r14", "pop r13", "pop r12", "pop rbp",
              "pop rbx", "ret"]
    cases = []
    for family, make, count in FAMILIES:
        for _ in range(count):
            number = len(cases)
            registers = [rng.choice([rng.getrandbits(64), rng.getrandbits(8), 0]) for _ in range(16)]
            memory = bytearray(rng.getrandbits(8) for _ in range(256))
            state = State(rng, True)
            body = make(rng, state, memory)
            # Unmasked exceptions only in single-instruction cases: a pending one makes the next waiting instruction fault.
            if len(body) > 1 or body[0].text.startswith(("fldenv", "frstor", "fldcw")):
                state.fcw |= 0x3F
            code = b"".join(insn.code for insn in body)
            lines += [f".globl _case_{number}", f".globl _case_{number}_end", f"_case_{number}:",
                      ".byte " + ", ".join(str(byte) for byte in code), "ret", f"_case_{number}_end:"]
            cases.append((family, registers, rng.getrandbits(12) & ALL_FLAGS, state.image(), bytes(memory),
                          "; ".join(insn.text for insn in body)))
    lines += [".data", ".balign 16", ".globl _host_fx", "_host_fx: .zero 512", "saved_io: .quad 0", "saved_code: .quad 0", ""]
    with open(assembly, "w") as out:
        out.write("\n".join(lines))
    with open(header, "w") as out:
        out.write("// Written by x87.py.\n")
        for number in range(len(cases)):
            out.write(f"extern char case_{number}[], case_{number}_end[];\n")
        out.write("static const struct Case cases[] = {\n")
        for number, (family, registers, flags, image, memory, text) in enumerate(cases):
            values = ", ".join(f"{value:#x}ull" for value in registers)
            # The image's first 160 bytes (control, status, tags and the registers) and the buffer, as quadwords.
            fx = ", ".join(f"{value:#x}ull" for value in struct.unpack("<20Q", image[:160]))
            words = ", ".join(f"{value:#x}ull" for value in struct.unpack("<32Q", memory))
            out.write(f"\t{{{{{values}}}, {flags:#x}, {{{fx}}}, {{{words}}}, \"{family}\", case_{number}, case_{number}_end, "
                      f"\"{text}\"}},\n")
        out.write("};\n")


if __name__ == "__main__":
    main()
