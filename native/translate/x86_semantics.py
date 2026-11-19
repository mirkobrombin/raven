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


def locked(rng):
    """lock add, sub, and, or, xor (register or immediate source), inc, dec, not and neg on aligned memory, the memory first set
    to an edge value. Flags as without lock: and, or and xor leave AF undefined; inc and dec keep CF and not keeps all."""
    cases = []
    for op in ("add", "sub", "and", "or", "xor", "inc", "dec", "not", "neg"):
        for bits in (8, 16, 32, 64):
            size = bits // 8
            for number in range(24):
                registers = registers_for(rng)
                holder, source = rng.sample(USABLE, 2)
                put(registers, holder, 0, width_value(rng, bits), bits)
                put(registers, source, 0, width_value(rng, bits), bits)
                offset = rng.randrange(0, 64 - size + 1)
                offset -= offset % size
                target = f"{PTR[bits]} ptr [r15 + {offset}]"
                body = [f"mov {target}, {GPR[bits][holder]}"]
                if op in ("inc", "dec", "not", "neg"):
                    body.append(f"lock {op} {target}")
                elif number % 3 == 2:
                    body.append(f"lock {op} {target}, {imm(rng, 8 if bits == 8 or rng.random() < 0.5 else min(bits, 32))}")
                else:
                    body.append(f"lock {op} {target}, {GPR[bits][source]}")
                cases.append((registers, body, ALL & ~AF if op in ("and", "or", "xor") else ALL))
    return cases


COUNTS = [0, 1, 2, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 255]


def count_cases(rng, bits):
    """Counts for a shift or rotate: as an imm8 (and the by-one form), and in CL, edge values first."""
    counts = [(str(c), c) for c in COUNTS if c < 256] + [("1", 1)]
    counts += [("cl", c) for c in COUNTS] + [("cl", rng.randrange(256)) for _ in range(8)]
    return counts


def rotate(rng):
    """rol, ror, rcl and rcr by an imm8, by one and by CL. The count is masked to 5 bits (6 for 64-bit operands); a masked count of
    0 changes nothing. rol and ror then rotate by the count modulo the width and set CF even when that is 0; rcl and rcr rotate
    width + 1 bits through CF, modulo 9 and 17 at 8 and 16 bits. OF is defined for a masked count of 1 only; SF, ZF, AF and PF stay."""
    cases = []
    for op in ("rol", "ror", "rcl", "rcr"):
        for bits in (8, 16, 32, 64):
            for text, count in count_cases(rng, bits):
                masked = count & (63 if bits == 64 else 31)
                for memory in (False, True) if text != "cl" or rng.random() < 0.3 else (False,):
                    registers = registers_for(rng)
                    index = rng.choice([i for i in USABLE if i != 1])
                    put(registers, 1, 0, count, 8)
                    put(registers, index, 0, width_value(rng, bits), bits)
                    if memory:
                        offset = rng.randrange(0, 64 - bits // 8 + 1)
                        target = f"{PTR[bits]} ptr [r15 + {offset}]"
                        body = [f"mov {target}, {GPR[bits][index]}", f"{op} {target}, {text}"]
                    else:
                        body = [f"{op} {GPR[bits][index]}, {text}"]
                    cases.append((registers, body, ALL & ~(OF if masked > 1 else 0)))
    return cases


def shift_by_cl(rng):
    """shl, shr and sar by CL at every width. A masked count (CL modulo 32, 64 for 64-bit operands) of 0 changes nothing. Else
    SF, ZF and PF come from the result and AF is undefined; CF is the last bit out, undefined for shl and shr when the count is the
    width or more (8 and 16 bits only); OF is defined for a count of 1."""
    cases = []
    for op in ("shl", "shr", "sar"):
        for bits in (8, 16, 32, 64):
            for count in COUNTS + [rng.randrange(256) for _ in range(10)]:
                masked = count & (63 if bits == 64 else 31)
                registers = registers_for(rng)
                index = rng.choice([i for i in USABLE if i != 1])
                put(registers, 1, 0, count, 8)
                put(registers, index, 0, width_value(rng, bits), bits)
                if rng.random() < 0.25:
                    offset = rng.randrange(0, 64 - bits // 8 + 1)
                    target = f"{PTR[bits]} ptr [r15 + {offset}]"
                    body = [f"mov {target}, {GPR[bits][index]}", f"{op} {target}, cl"]
                else:
                    body = [f"{op} {GPR[bits][index]}, cl"]
                defined = ALL
                if masked:
                    defined &= ~AF
                    if masked > 1:
                        defined &= ~OF
                    if op != "sar" and masked >= bits:
                        defined &= ~CF
                cases.append((registers, body, defined))
    return cases


def carry_narrow(rng):
    """adc and sbb at 8 and 16 bits (register, memory and immediate forms, CF random), and lock adc and lock sbb on memory at every
    width. Every flag is defined."""
    cases = []
    for op in ("adc", "sbb"):
        for bits in (8, 16):
            for a, b in [(a, b) for a in edges(rng, bits)[:8] for b in edges(rng, bits)[:8]] + \
                    [(rng.getrandbits(bits), rng.getrandbits(bits)) for _ in range(20)]:
                registers = registers_for(rng)
                target, source = rng.sample(USABLE, 2)
                put(registers, target, 0, a, bits)
                put(registers, source, 0, b, bits)
                form = rng.randrange(4)
                if form == 0:
                    body = [f"{op} {GPR[bits][target]}, {GPR[bits][source]}"]
                elif form == 1:
                    body = [f"mov {PTR[bits]} ptr [r15 + 8], {GPR[bits][source]}", f"{op} {GPR[bits][target]}, {PTR[bits]} ptr [r15 + 8]"]
                elif form == 2:
                    body = [f"mov {PTR[bits]} ptr [r15 + 8], {GPR[bits][target]}", f"{op} {PTR[bits]} ptr [r15 + 8], {GPR[bits][source]}"]
                else:
                    body = [f"{op} {GPR[bits][target]}, {imm(rng, 8 if bits == 8 or rng.random() < 0.5 else 16)}"]
                cases.append((registers, body, ALL))
        for bits in (8, 16, 32, 64):
            for _ in range(16):
                registers = registers_for(rng)
                holder, source = rng.sample(USABLE, 2)
                put(registers, holder, 0, width_value(rng, bits), bits)
                put(registers, source, 0, width_value(rng, bits), bits)
                offset = rng.randrange(0, 64 - bits // 8 + 1)
                offset -= offset % (bits // 8)
                target = f"{PTR[bits]} ptr [r15 + {offset}]"
                cases.append((registers, [f"mov {target}, {GPR[bits][holder]}", f"lock {op} {target}, {GPR[bits][source]}"], ALL))
    return cases


def flag_readers(rng):
    """The new forms with a reader of their flags later in the same block: setc, seto, cmovc and jc after bt, bts, mul, imul, a
    rotate, a narrow shift by CL and a narrow adc, so that the translator's flag liveness has them produce CF and OF."""
    cases = []
    producers = [("bt eax, 5", CF | ZF), ("bt rbx, rcx", CF | ZF), ("bts dword ptr [r15 + 4], 9", CF | ZF), ("btc ecx, edx", CF | ZF),
                 ("mul rbx", CF | OF), ("imul ecx", CF | OF), ("mul bl", CF | OF), ("rol eax, 1", ALL), ("ror rbx, cl", ALL & ~OF),
                 ("rcl dx, 1", ALL), ("shl bl, cl", CF | ZF | SF | PF), ("adc al, bl", ALL), ("sbb cx, dx", ALL),
                 ("lock bts qword ptr [r15 + 8], rax", CF | ZF)]
    for producer, defined in producers:
        for _ in range(12):
            registers = registers_for(rng)
            put(registers, 1, 0, rng.choice([1, 2, 7, 33, 65]), 8)
            if "[r15 + 8], rax" in producer:
                registers[0] = rng.randrange(-64, 384) & (2 ** 64 - 1)
            reader = rng.choice(["setc sil", "seto sil", "cmovc rdi, rsi", "jc"])
            if reader == "jc":
                body = [producer, "jc 1f", "mov rsi, 1", "1:", "setc dil"]
            else:
                body = [producer, reader]
            reads = OF if "seto" in reader else CF
            if not defined & reads:
                continue
            # The case's flags are those of the producer; which the reader saw shows in RSI or RDI.
            cases.append((registers, body, defined))
    return cases


def unaligned_locked(rng):
    """The locked forms on operands not aligned to their size, inside 16 bytes and across a 16-byte boundary of the buffer (which
    starts 64-byte aligned in the translator's tests), and bts with lock there too."""
    cases = []
    ops = ["add", "sub", "and", "or", "xor", "adc", "sbb", "inc", "dec", "not", "neg", "xadd", "cmpxchg", "xchg", "bts", "btr"]
    for op in ops:
        for bits in (16, 32, 64):
            size = bits // 8
            crossing = list(range(16 - size + 1, 16)) + list(range(48 - size + 1, 48))
            inside = [o for o in range(1, 16 - size + 1) if o % size]
            for offset in crossing + rng.sample(inside, min(3, len(inside))):
                registers = registers_for(rng)
                holder, source = rng.sample(USABLE, 2)
                put(registers, holder, 0, width_value(rng, bits), bits)
                put(registers, source, 0, width_value(rng, bits), bits)
                target = f"{PTR[bits]} ptr [r15 + {offset}]"
                prefix = "" if op == "xchg" else "lock "
                if op in ("inc", "dec", "not", "neg"):
                    text = f"{prefix}{op} {target}"
                elif op in ("bts", "btr"):
                    text = f"{prefix}{op} {target}, {rng.randrange(bits)}"
                else:
                    text = f"{prefix}{op} {target}, {GPR[bits][source]}"
                defined = {"and": ALL & ~AF, "or": ALL & ~AF, "xor": ALL & ~AF, "bts": CF | ZF, "btr": CF | ZF,
                           "cmpxchg": ZF}.get(op, ALL)
                cases.append((registers, [f"mov {target}, {GPR[bits][holder]}", text], defined))
    return cases


def bit_scan(rng):
    """bsf and bsr at 16, 32 and 64 bits from a register or memory, the source 0 among others: ZF is set then and the
    destination, compared whole, shows what the CPU left in it. ZF is the one flag defined."""
    cases = []
    for op in ("bsf", "bsr"):
        for bits in (16, 32, 64):
            values = [0, 0, 0, 1, 2, 3] + [1 << i for i in range(0, bits, 5)] + [(1 << (bits - 1)), (1 << bits) - 1]
            for value in values + [rng.getrandbits(bits) for _ in range(10)]:
                registers = registers_for(rng)
                target, source = rng.sample(USABLE, 2)
                put(registers, source, 0, value, bits)
                if rng.random() < 0.25:
                    body = [f"mov {PTR[bits]} ptr [r15 + 16], {GPR[bits][source]}", f"{op} {GPR[bits][target]}, {PTR[bits]} ptr [r15 + 16]"]
                elif rng.random() < 0.1:
                    put(registers, target, 0, value, bits)
                    body = [f"{op} {GPR[bits][target]}, {GPR[bits][target]}"]
                else:
                    body = [f"{op} {GPR[bits][target]}, {GPR[bits][source]}"]
                cases.append((registers, body, ZF))
    return cases


def strings(rng):
    """rep stos and rep movs at every width, and once without rep, forward and backward (std), with RCX 0 and counts that fill
    the buffer; movs over overlapping ranges both ways. RDI and RSI point into the buffer through lea and are made offsets again
    after, so that the registers compare (R8 holds the flags from before that). Then pushfq and popfq with DF and the six flags, cld, std and pause. No flag changes."""
    cases = []
    for op in ("stos", "movs"):
        for bits in (8, 16, 32, 64):
            size = bits // 8
            suffix = {8: "b", 16: "w", 32: "d", 64: "q"}[bits]
            for _ in range(24):
                backward = rng.random() < 0.4
                rep = rng.random() < 0.85
                limit = 64 // size
                count = rng.choice([0, 1, 2, rng.randrange(limit // 2 + 1), limit // 2]) if rep else 1
                span = max(count, 1) * size
                if op == "stos":
                    start = rng.randrange(0, 64 - span + 1)
                    destination = start + span - size if backward else start
                    source = 0
                else:
                    # Overlapping now and then: the source a few elements before or after the destination.
                    first = rng.randrange(0, 64 - span + 1)
                    second = min(max(first + rng.choice([-2, -1, 1, 2, 3]) * size, 0), 64 - span) if rng.random() < 0.5 \
                        else rng.randrange(0, 64 - span + 1)
                    destination, source = (first + span - size, second + span - size) if backward else (first, second)
                registers = registers_for(rng)
                registers[1] = count
                body = [f"lea rdi, [r15 + {destination}]", f"lea rsi, [r15 + {source}]"]
                if backward:
                    body.append("std")
                body.append(f"{'rep ' if rep else ''}{op}{suffix}")
                if backward:
                    body.append("cld")
                # The flags the string instruction left go to R8 (they must be the input ones): the subs after define them anew,
                # AF from the pointers, which differ between runs.
                body += ["pushfq", "pop r8", "sub rdi, r15", "sub rsi, r15"]
                cases.append((registers, body, ALL & ~AF))
    # The flags through pushfq and popfq: the six status flags and DF in, RFLAGS as pushfq stores it out.
    for _ in range(60):
        registers = registers_for(rng)
        value = (rng.getrandbits(12) & ALL) | 2 | (1024 if rng.random() < 0.5 else 0)
        registers[3] = value
        body = ["push rbx", "popfq", "pushfq", "pop rax", "cld"]
        if value & 1024 and rng.random() < 0.5:
            body = ["push rbx", "popfq", "lea rdi, [r15 + 40]", "mov ecx, 3", "rep stosb", "pushfq", "pop rax", "cld", "sub rdi, r15"]
        cases.append((registers, body, ALL & ~AF if "sub rdi, r15" in body else ALL))
    for _ in range(20):
        registers = registers_for(rng)
        body = rng.choice([["std", "pushfq", "pop rax", "cld"], ["cld", "pushfq", "pop rax"], ["pause"], ["add rax, rbx", "pause", "pushfq", "pop rcx"]])
        cases.append((registers, body, ALL))
    return cases


def shift_by_one(rng):
    """shl, shr and sar by a count of 1 at 8 and 16 bits (the by-one form, an imm8 of 1, CL of 1 and of 33), with every combination of
    the top two bits and the low bit: OF is defined for a count of 1 (shl: the top bit of the result xor CF; shr: the top bit before;
    sar: 0), CF is the bit out."""
    cases = []
    for op in ("shl", "shr", "sar"):
        for bits in (8, 16):
            for top in range(4):
                for low in range(2):
                    for count_text, cl in (("1", 0), ("cl", 1), ("cl", 33)):
                        value = (top << (bits - 2)) | low | (rng.getrandbits(bits - 3) << 1)
                        registers = registers_for(rng)
                        index = rng.choice([i for i in USABLE if i not in (1,)])
                        put(registers, index, 0, value, bits)
                        put(registers, 1, 0, cl if count_text == "cl" else rng.getrandbits(8), 8)
                        cases.append((registers, [f"{op} {GPR[bits][index]}, {count_text}"], ALL & ~AF))
    return cases


def rflags(rng):
    """popfq and pushfq of AC (bit 18) and ID (bit 21): RFLAGS read, a bit or both toggled, written, read back into RBX, and the first
    value put back (AC set would make the oracle's later unaligned accesses fault). TF is left out: x86 would trap on the next
    instruction."""
    cases = []
    for mask in (1 << 21, 1 << 18, (1 << 21) | (1 << 18)):
        for _ in range(8):
            registers = registers_for(rng)
            body = ["pushfq", "pop rax", "mov rcx, rax", f"xor rcx, {mask:#x}", "push rcx", "popfq", "pushfq", "pop rbx", "push rax",
                    "popfq"]
            cases.append((registers, body, ALL))
    return cases


# The directed families: name and generator, which returns a list of (registers, body, defined). Each runs with a seed of its
# name's CRC-32.
FAMILIES = [("multiply", multiply), ("divide", divide), ("bit test", bit_test), ("locked", locked), ("rotate", rotate), ("shift by cl", shift_by_cl),
            ("carry narrow", carry_narrow), ("flag readers", flag_readers), ("unaligned locked", unaligned_locked),
            ("bit scan", bit_scan), ("strings", strings), ("rflags", rflags),
            ("shift by one", shift_by_one)]


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
