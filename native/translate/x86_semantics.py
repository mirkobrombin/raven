"""Writes random x64 instruction sequences for the translator's semantics test, with their inputs and the flags they leave defined.

usage: x86_semantics.py OUTPUT_ASSEMBLY OUTPUT_HEADER [COUNT]

The assembly holds one function per case and run_case, which loads a case's registers and flags, calls it and stores what it left.
An x86-64 build of x86_semantics_oracle.c runs them on an x86 CPU or under Rosetta; the translator runs the same bytes and must agree on
every register, every flag the sequence defines and the 64-byte buffer r15 points at.

Each case belongs to a family: `random`, COUNT random sequences of up to six instructions, then the directed families of FAMILIES,
each from its own seed, so that adding one changes no other. A directed case is one instruction (or a few) on registers chosen for
it, edge values first; the flags come in random, so that flags an instruction must leave alone are checked too.
"""

import random
import sys
import zlib

GPR = {
    8: ["al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"] + [f"r{i}b" for i in range(8, 16)],
    16: ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"] + [f"r{i}w" for i in range(8, 16)],
    32: ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"] + [f"r{i}d" for i in range(8, 16)],
    64: ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"] + [f"r{i}" for i in range(8, 16)],
}
PTR = {8: "byte", 16: "word", 32: "dword", 64: "qword"}
CONDITIONS = ["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "l", "ge", "le", "g"]
CF, PF, AF, ZF, SF, OF = 1, 4, 16, 64, 128, 2048
READS = {"o": OF, "no": OF, "b": CF, "ae": CF, "e": ZF, "ne": ZF, "be": CF | ZF, "a": CF | ZF, "s": SF, "ns": SF, "p": PF, "np": PF,
         "l": SF | OF, "ge": SF | OF, "le": ZF | SF | OF, "g": ZF | SF | OF}
ALL = CF | PF | AF | ZF | SF | OF
# RSP holds the stack and R15 the buffer: no case writes them.
USABLE = [i for i in range(16) if i not in (4, 15)]


def reg(rng, size):
    return GPR[size][rng.choice(USABLE)]


def mem(rng, size, aligned=False):
    step = size // 8
    offset = rng.randrange(0, 64 - step + 1)
    if aligned:
        offset -= offset % step
    return f"{PTR[size]} ptr [r15 + {offset}]"


def imm(rng, bits):
    return rng.randint(-(2 ** (bits - 1)), 2 ** (bits - 1) - 1)


def readable(conditions, defined):
    """The conditions whose flags hold defined values: a case never reads a flag x86 leaves undefined."""
    return [c for c in conditions if READS[c] & defined == READS[c]]


def instruction(rng, first, rcx, defined):
    """One instruction and the flags it (defines, makes undefined)."""
    size = rng.choice([8, 16, 32, 64])
    kind = rng.randrange(16)
    if kind < 4:
        op = rng.choice(["add", "sub", "and", "or", "xor", "cmp", "test"] + (["adc", "sbb"] if defined & CF else []))
        if op in ("adc", "sbb"):
            size = rng.choice([32, 64])
        form = rng.randrange(4)
        if op == "test" and form == 1:
            form = 0
        if form == 0:
            text = f"{op} {reg(rng, size)}, {reg(rng, size)}"
        elif form == 1:
            text = f"{op} {reg(rng, size)}, {mem(rng, size)}"
        elif form == 2:
            text = f"{op} {mem(rng, size)}, {reg(rng, size)}"
        else:
            text = f"{op} {reg(rng, size)}, {imm(rng, 8 if size == 8 or rng.random() < 0.5 else min(size, 32))}"
        logical = op in ("and", "or", "xor", "test")
        return text, (ALL & ~AF if logical else ALL), (AF if logical else 0)
    if kind == 4:
        op = rng.choice(["inc", "dec", "neg", "not"])
        target = reg(rng, size) if rng.random() < 0.7 else mem(rng, size)
        defined = {"inc": ALL & ~CF, "dec": ALL & ~CF, "neg": ALL, "not": 0}[op]
        return f"{op} {target}", defined, 0
    if kind == 5:
        form = rng.randrange(3)
        if form == 0:
            return f"mov {reg(rng, size)}, {reg(rng, size)}", 0, 0
        if form == 1:
            return f"mov {reg(rng, size)}, {mem(rng, size)}", 0, 0
        return f"mov {reg(rng, size)}, {imm(rng, size if size < 64 else rng.choice([32, 64]))}", 0, 0
    if kind == 6:
        op = rng.choice(["movzx", "movsx"])
        source = rng.choice([8, 16])
        target = rng.choice([s for s in (16, 32, 64) if s > source])
        operand = reg(rng, source) if rng.random() < 0.6 else mem(rng, source)
        return f"{op} {reg(rng, target)}, {operand}", 0, 0
    if kind == 7:
        if rng.random() < 0.5:
            return f"movsxd {reg(rng, 64)}, {reg(rng, 32)}", 0, 0
        size = rng.choice([32, 64])
        index = GPR[64][rng.choice([i for i in USABLE])]
        return f"lea {reg(rng, size)}, [{reg(rng, 64)} + {rng.choice([1, 2, 4, 8])}*{index} + {imm(rng, 16)}]", 0, 0
    if kind == 8:
        op = rng.choice(["shl", "shr", "sar"])
        count = rng.randrange(0, size)
        target = reg(rng, size)
        if count == 0:
            return f"{op} {target}, 0", 0, 0
        defined = CF | ZF | SF | PF | (OF if count == 1 else 0)
        undefined = AF | (OF if count > 1 else 0)
        return f"{op} {target}, {count}", defined, undefined
    if kind == 9 and first and size >= 32:
        op = rng.choice(["shl", "shr", "sar"])
        count = rcx & (63 if size == 64 else 31)
        target = GPR[size][rng.choice([i for i in USABLE if i != 1])]
        if count == 0:
            return f"{op} {target}, cl", 0, 0
        defined = CF | ZF | SF | PF | (OF if count == 1 else 0)
        undefined = AF | (OF if count > 1 else 0)
        return f"{op} {target}, cl", defined, undefined
    if kind == 10:
        size = rng.choice([32, 64])
        if rng.random() < 0.5:
            return f"imul {reg(rng, size)}, {reg(rng, size)}", CF | OF, SF | ZF | AF | PF
        return f"imul {reg(rng, size)}, {reg(rng, size)}, {imm(rng, rng.choice([8, 32]))}", CF | OF, SF | ZF | AF | PF
    if kind == 11:
        sets = readable(CONDITIONS + ["p", "np"], defined)
        moves = readable(CONDITIONS, defined)
        if rng.random() < 0.7 and sets:
            return f"set{rng.choice(sets)} {reg(rng, 8)}", 0, 0
        if not moves:
            return "nop", 0, 0
        size = rng.choice([16, 32, 64])
        return f"cmov{rng.choice(moves)} {reg(rng, size)}, {reg(rng, size) if rng.random() < 0.6 else mem(rng, size)}", 0, 0
    if kind == 12:
        if rng.random() < 0.5:
            return f"xchg {reg(rng, size)}, {reg(rng, size)}", 0, 0
        return f"xchg {mem(rng, size, aligned=True)}, {reg(rng, size)}", 0, 0
    if kind == 13:
        return rng.choice(["cdq", "cqo", "cdqe", "cwde", "cbw", "cwd"]), 0, 0
    if kind == 14:
        op = rng.choice(["xadd", "cmpxchg"])
        lock = rng.choice(["", "lock "])
        text = f"{lock}{op} {mem(rng, size, aligned=True)}, {reg(rng, size)}"
        if op == "cmpxchg":
            # The SDM, Bochs and QEMU set the flags of accumulator minus destination; Rosetta leaves those of destination minus
            # accumulator. Only ZF agrees, so only ZF is compared.
            return text, ZF, ALL & ~ZF
        return text, ALL, 0
    return f"mov {mem(rng, size)}, {reg(rng, size)}", 0, 0


def edges(rng, bits, signed=False):
    """Values of a width to try first: 0, 1, -1, the minimum and maximum signed values, their neighbours and powers of two."""
    mask = (1 << bits) - 1
    values = [0, 1, 2, mask, mask - 1, 1 << (bits - 1), (1 << (bits - 1)) - 1, (1 << (bits - 1)) + 1, 1 << rng.randrange(bits),
              (1 << rng.randrange(bits)) - 1]
    return [v & mask for v in values]


def width_value(rng, bits):
    return rng.choice(edges(rng, bits) + [rng.getrandbits(bits)] * 4)


def signed(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >> (bits - 1) else value


def registers_for(rng):
    return [rng.getrandbits(64) for _ in range(16)]


def source_register(rng, bits, avoid, high=True):
    """A register operand of the width other than those avoid names (numbers); at 8 bits now and then ah, ch, dh or bh (with high:
    they cannot appear in an instruction with a REX prefix)."""
    if high and bits == 8 and rng.random() < 0.15 and not (avoid & {0, 1, 2, 3}):
        index = rng.randrange(4)
        return GPR[8][index][0] + "h", index, 8
    index = rng.choice([i for i in USABLE if i not in avoid])
    return GPR[bits][index], index, 0


def put(registers, index, shift, value, bits):
    """Writes value (bits wide) into register index at bit shift, keeping the rest."""
    mask = ((1 << bits) - 1) << shift
    registers[index] = (registers[index] & ~mask) | ((value << shift) & mask)


def multiply(rng):
    """mul and imul with one operand: RDX:RAX (AX for 8 bits) from RAX times the source; CF and OF defined."""
    cases = []
    for op in ("mul", "imul"):
        for bits in (8, 16, 32, 64):
            pairs = [(a, b) for a in edges(rng, bits) for b in edges(rng, bits)]
            pairs += [(rng.getrandbits(bits), rng.getrandbits(bits)) for _ in range(40)]
            for number, (a, b) in enumerate(pairs):
                registers = registers_for(rng)
                put(registers, 0, 0, a, bits)
                memory = number % 5 == 4
                name, index, shift = source_register(rng, bits, {4, 15} | ({0} if not memory and rng.random() < 0.8 else set()),
                                                     not memory)
                if index == 0 and shift == 0 and not memory:
                    b = a
                put(registers, index, shift, b, bits)
                if memory:
                    offset = rng.randrange(0, 64 - bits // 8 + 1)
                    target = f"{PTR[bits]} ptr [r15 + {offset}]"
                    body = [f"mov {target}, {name}", f"{op} {target}"]
                    if index == 0 and shift == 0:
                        put(registers, 0, 0, a, bits)
                else:
                    body = [f"{op} {name}"]
                cases.append((registers, body, CF | OF))
    return cases


def divide(rng):
    """div and idiv: RDX:RAX (AX for 8 bits) by the source, quotient to RAX (AL), remainder to RDX (AH). Inputs x86 raises #DE for
    (a zero divisor, a quotient that does not fit) are left out: the oracle would die of SIGFPE; x86_semantics_check.h's fault
    cases check those. Every flag is undefined."""
    cases = []
    for op in ("div", "idiv"):
        for bits in (8, 16, 32, 64):
            mask = (1 << bits) - 1
            tries = [(q, d, rng.random()) for q in edges(rng, bits) for d in edges(rng, bits)]
            tries += [(rng.getrandbits(bits), rng.getrandbits(rng.choice([2, 8, bits // 2, bits])), rng.random()) for _ in range(80)]
            for number, (q, d, fraction) in enumerate(tries):
                if op == "div":
                    if d == 0:
                        continue
                    n = q * d + int(fraction * d)
                    if n >> (2 * bits) or n // d > mask:
                        continue
                    dividend = n
                else:
                    sq, sd = signed(q, bits), signed(d, bits)
                    if sd == 0:
                        continue
                    product = sq * sd
                    magnitude = int(fraction * abs(sd))
                    n = product + (magnitude if product > 0 or (product == 0 and fraction < 0.5) else -magnitude)
                    if not -(1 << (2 * bits - 1)) <= n < (1 << (2 * bits - 1)):
                        continue
                    quotient = abs(n) // abs(sd) * (1 if (n < 0) == (sd < 0) else -1)
                    if not -(1 << (bits - 1)) <= quotient < (1 << (bits - 1)):
                        continue
                    dividend = n & ((1 << (2 * bits)) - 1)
                registers = registers_for(rng)
                if bits == 8:
                    put(registers, 0, 0, dividend, 16)
                else:
                    put(registers, 0, 0, dividend, bits)
                    put(registers, 2, 0, dividend >> bits, bits)
                memory = number % 5 == 4
                name, index, shift = source_register(rng, bits, {0, 2, 4, 15}, not memory)
                put(registers, index, shift, d, bits)
                if memory:
                    offset = rng.randrange(0, 64 - bits // 8 + 1)
                    target = f"{PTR[bits]} ptr [r15 + {offset}]"
                    body = [f"mov {target}, {name}", f"{op} {target}"]
                else:
                    body = [f"{op} {name}"]
                cases.append((registers, body, 0))
    return cases


def bit_index_values(rng, bits):
    """Bit indices: inside the operand, just beyond it, far beyond and negative, as a register of the width holds them."""
    values = [0, 1, bits - 1, bits, bits + 1, 2 * bits - 1, 255, 256, 1 << (bits - 2), (1 << (bits - 1)) - 1, -1, -2, -bits,
              -bits - 1, -(1 << (bits - 1)), rng.randrange(bits), rng.randrange(-4096, 4096), rng.getrandbits(bits)]
    return [v & ((1 << bits) - 1) for v in values]


def bit_test(rng):
    """bt, bts, btr and btc on registers and memory, by a register or an imm8 index, with and without lock. CF is the bit; ZF is
    left alone (defined: it must not change); OF, SF, AF and PF are undefined. An immediate index is taken modulo the width, a
    register one too for a register operand; for memory, a register index addresses a bit string: the signed index over 8 bytes
    from the operand's address, which the cases put back in the buffer with a base register."""
    cases = []
    for op in ("bt", "bts", "btr", "btc"):
        for bits in (16, 32, 64):
            size = bits // 8
            for index in bit_index_values(rng, bits) + [rng.getrandbits(bits) for _ in range(12)]:
                registers = registers_for(rng)
                target = rng.choice(USABLE)
                registers[target] = rng.getrandbits(64)
                source = rng.choice(USABLE)
                if source != target:
                    put(registers, source, 0, index, bits)
                cases.append((registers, [f"{op} {GPR[bits][target]}, {GPR[bits][source]}"], CF | ZF))
            for _ in range(15):
                registers = registers_for(rng)
                target = rng.choice(USABLE)
                cases.append((registers, [f"{op} {GPR[bits][target]}, {rng.randrange(256)}"], CF | ZF))
            locks = [""] + (["lock "] if op != "bt" else [])
            for lock in locks:
                for _ in range(10):
                    offset = rng.randrange(0, 64 - size + 1)
                    if lock:
                        offset -= offset % size
                    cases.append((registers_for(rng), [f"{lock}{op} {PTR[bits]} ptr [r15 + {offset}], {rng.randrange(256)}"], CF | ZF))
                for index in bit_index_values(rng, bits) + ([rng.getrandbits(bits) for _ in range(6)] if not lock else []):
                    registers = registers_for(rng)
                    base, source = rng.sample(USABLE, 2)
                    put(registers, source, 0, index, bits)
                    # The word x86 reads holds the bit: size bytes at the address plus size times the index over the width.
                    word = signed(index, bits) >> (bits.bit_length() - 1) << (size.bit_length() - 1)
                    place = rng.randrange(0, 64 - size + 1)
                    place -= place % size
                    registers[base] = (place - word) & (2 ** 64 - 1)
                    body = [f"{lock}{op} {PTR[bits]} ptr [r15 + {GPR[64][base]}], {GPR[bits][source]}"]
                    cases.append((registers, body, CF | ZF))
    return cases


# The directed families: name and generator, which returns a list of (registers, body, defined). Each runs with a seed of its
# name's CRC-32.
FAMILIES = [("multiply", multiply), ("divide", divide), ("bit test", bit_test)]


def main():
    assembly, header = sys.argv[1:3]
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 10000
    rng = random.Random(20261118)
    lines = [".intel_syntax noprefix", ".text"]
    # run_case(io = rdi, code = rsi): io holds 16 registers, the flags, then the 64-byte buffer r15 points at.
    lines += [
        ".globl _run_case", "_run_case:",
        "push rbx", "push rbp", "push r12", "push r13", "push r14", "push r15", "sub rsp, 8",
        "mov qword ptr [rip + saved_io], rdi", "mov qword ptr [rip + saved_code], rsi",
        "push qword ptr [rdi + 128]", "popfq",
    ]
    for index, name in enumerate(GPR[64]):
        if name not in ("rsp", "rdi", "r15"):
            lines.append(f"mov {name}, qword ptr [rdi + {8 * index}]")
    lines += ["lea r15, [rdi + 136]", "mov rdi, qword ptr [rdi + 56]", "call qword ptr [rip + saved_code]",
              "pushfq", "push rdi", "mov rdi, qword ptr [rip + saved_io]", "pop qword ptr [rdi + 56]", "pop qword ptr [rdi + 128]"]
    for index, name in enumerate(GPR[64]):
        if name not in ("rsp", "rdi", "r15"):
            lines.append(f"mov qword ptr [rdi + {8 * index}], {name}")
    lines += ["add rsp, 8", "pop r15", "pop r14", "pop r13", "pop r12", "pop rbp", "pop rbx", "ret"]
    cases = []
    for number in range(count):
        registers = [rng.choice([rng.getrandbits(64), rng.getrandbits(8), (1 << 63) - rng.getrandbits(4), 0, 1])
                     for _ in range(16)]
        flags = rng.getrandbits(12) & ALL
        body, defined = [], ALL
        for position in range(rng.randint(1, 6)):
            text, defines, undefines = instruction(rng, position == 0, registers[1], defined)
            body.append(text)
            defined = (defined | defines) & ~undefines
        lines += [f".globl _case_{number}", f".globl _case_{number}_end", f"_case_{number}:"] + body + ["ret", f"_case_{number}_end:"]
        cases.append((registers, flags, defined, body, "random"))
    for family, generate in FAMILIES:
        family_rng = random.Random(zlib.crc32(family.encode()))
        for registers, body, defined in generate(family_rng):
            number = len(cases)
            flags = family_rng.getrandbits(12) & ALL
            lines += [f".globl _case_{number}", f".globl _case_{number}_end", f"_case_{number}:"] + body + ["ret",
                                                                                                     f"_case_{number}_end:"]
            cases.append((registers, flags, defined, body, family))
    lines += [".data", "saved_io: .quad 0", "saved_code: .quad 0", ""]
    with open(assembly, "w") as out:
        out.write("\n".join(lines))
    with open(header, "w") as out:
        out.write("// Written by x86_semantics.py.\n")
        for number in range(len(cases)):
            out.write(f"extern char case_{number}[], case_{number}_end[];\n")
        out.write("static const struct Case cases[] = {\n")
        for number, (registers, flags, defined, body, family) in enumerate(cases):
            values = ", ".join(f"{value & (2 ** 64 - 1):#x}ull" for value in registers)
            text = "; ".join(body).replace('"', '\\"')
            out.write(f"\t{{{{{values}}}, {flags:#x}, {defined:#x}, case_{number}, case_{number}_end, \"{text}\", \"{family}\"}},\n")
        out.write("};\n")


if __name__ == "__main__":
    main()
