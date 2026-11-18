#include "x86_decoder.h"

#include <cstdio>

// Opcode maps: Intel SDM volume 2, appendix A. Operand specifiers follow its notation (E, G, I, J, Z for a register in the opcode).
namespace raven::translate {

namespace {

enum Spec : uint8_t {
	None,
	Eb, // r/m8
	Ev, // r/m of the operand size
	Ew, // r/m16 (source of movzx and movsx)
	Ed, // r/m32 (source of movsxd)
	Eq, // r/m of the operand size, 64 by default (push, pop, call, jmp)
	Gb,
	Gv,
	M, // memory of no size (lea)
	Ib, // imm8
	Ibs, // imm8 sign-extended to the operand size
	Iw, // imm16
	Iz, // imm16 or imm32, sign-extended to the operand size
	Iv, // imm16, imm32 or imm64 (mov r, imm)
	Jb,
	Jz,
	Zb, // register in the low three bits of the opcode, 8 bits
	Zv,
	Zq, // the same, 64 bits by default (push, pop)
	AL,
	AX, // rAX of the operand size
	CL,
	One, // the implicit count of a shift by one
	// Operands that only give the length of an instruction without semantics.
	Ob, // moffs: an address of the address size
	Ibw, // enter: imm16 then imm8
	Id, // imm32 regardless of operand size (XOP map 10)
};

enum Flag : uint8_t {
	with_modrm = 1,
	default64 = 2,
	invalid64 = 4,
	group = 8, // the reg field of ModRM selects the entry in a group table
	register_only = 16, // mod is ignored: always a register (mov to and from control registers)
	conditional = 32,
};

struct Entry {
	Mnemonic mnemonic = Mnemonic::invalid;
	Spec a = None, b = None, c = None;
	uint8_t flags = 0;
	uint8_t group_index = 0;
};

constexpr Entry op(Mnemonic mnemonic, Spec a = None, Spec b = None, Spec c = None, uint8_t flags = 0)
{
	uint8_t all = flags;
	for (Spec spec : {a, b, c})
		if (spec == Eb || spec == Ev || spec == Ew || spec == Ed || spec == Eq || spec == Gb || spec == Gv || spec == M)
			all |= with_modrm;
	return Entry{mnemonic, a, b, c, all, 0};
}

constexpr Entry grp(uint8_t index, Spec a = None, Spec b = None, uint8_t flags = 0)
{
	Entry entry = op(Mnemonic::other, a, b, None, flags);
	entry.flags |= with_modrm | group;
	entry.group_index = index;
	return entry;
}

constexpr Entry other(Spec a = None, Spec b = None, uint8_t flags = 0)
{
	return op(Mnemonic::other, a, b, None, flags);
}

constexpr Entry other_modrm(Spec imm = None)
{
	return op(Mnemonic::other, Ev, imm);
}

constexpr Entry bad()
{
	return Entry{Mnemonic::invalid, None, None, None, invalid64, 0};
}

using Map = std::array<Entry, 256>;

constexpr Mnemonic arithmetic[8] = {Mnemonic::add, Mnemonic::or_, Mnemonic::adc, Mnemonic::sbb, Mnemonic::and_, Mnemonic::sub,
	Mnemonic::xor_, Mnemonic::cmp};

// Groups, indexed by ModRM.reg. Their operands come from the opcode's entry unless the group entry names its own.
enum GroupIndex : uint8_t { group1, group2, group3b, group3v, group4, group5, group11b, group11v, group1a, nop_group };

struct Group {
	std::array<Entry, 8> entries;
};

constexpr std::array<Group, 10> make_groups()
{
	std::array<Group, 10> groups{};
	for (size_t i = 0; i < 8; ++i)
		groups[group1].entries[i] = Entry{arithmetic[i]};
	// /6 is an undocumented alias of shl and F6 /1, F7 /1 of test: they decode, but nothing translates them.
	const Mnemonic shifts[8] = {Mnemonic::rol, Mnemonic::ror, Mnemonic::rcl, Mnemonic::rcr, Mnemonic::shl, Mnemonic::shr,
		Mnemonic::other, Mnemonic::sar};
	for (size_t i = 0; i < 8; ++i)
		groups[group2].entries[i] = Entry{shifts[i]};
	const Mnemonic unary[8] = {Mnemonic::test, Mnemonic::other, Mnemonic::not_, Mnemonic::neg, Mnemonic::mul, Mnemonic::imul,
		Mnemonic::div, Mnemonic::idiv};
	for (size_t i = 0; i < 8; ++i) {
		groups[group3b].entries[i] = i < 2 ? op(unary[i], Eb, Ib) : op(unary[i], Eb);
		groups[group3v].entries[i] = i < 2 ? op(unary[i], Ev, Iz) : op(unary[i], Ev);
	}
	groups[group4].entries = {op(Mnemonic::inc, Eb), op(Mnemonic::dec, Eb), bad(), bad(), bad(), bad(), bad(), bad()};
	groups[group5].entries = {op(Mnemonic::inc, Ev), op(Mnemonic::dec, Ev), op(Mnemonic::call, Eq), other_modrm(),
		op(Mnemonic::jmp, Eq), other_modrm(), op(Mnemonic::push, Eq), bad()};
	groups[group11b].entries = {op(Mnemonic::mov, Eb, Ib), bad(), bad(), bad(), bad(), bad(), bad(), other_modrm(Ib)};
	groups[group11v].entries = {op(Mnemonic::mov, Ev, Iz), bad(), bad(), bad(), bad(), bad(), bad(), other_modrm(Jz)};
	groups[group1a].entries = {op(Mnemonic::pop, Eq), bad(), bad(), bad(), bad(), bad(), bad(), bad()};
	groups[nop_group].entries = {op(Mnemonic::nop, Ev), other_modrm(), other_modrm(), other_modrm(), other_modrm(), other_modrm(),
		other_modrm(), other_modrm()};
	return groups;
}

constexpr std::array<Group, 10> groups = make_groups();

constexpr Map make_one_byte()
{
	Map map{};
	for (size_t i = 0; i < 8; ++i) {
		const size_t base = i * 8;
		map[base + 0] = op(arithmetic[i], Eb, Gb);
		map[base + 1] = op(arithmetic[i], Ev, Gv);
		map[base + 2] = op(arithmetic[i], Gb, Eb);
		map[base + 3] = op(arithmetic[i], Gv, Ev);
		map[base + 4] = op(arithmetic[i], AL, Ib);
		map[base + 5] = op(arithmetic[i], AX, Iz);
		map[base + 6] = bad();
		map[base + 7] = bad();
	}
	// 0f is the escape and 26, 2e, 36, 3e, 64-67, f0, f2, f3 and 40-4f are prefixes: the decoder takes them before the table.
	for (size_t i = 0; i < 8; ++i) {
		map[0x50 + i] = op(Mnemonic::push, Zq);
		map[0x58 + i] = op(Mnemonic::pop, Zq);
		map[0x90 + i] = op(Mnemonic::xchg, AX, Zv);
		map[0xb0 + i] = op(Mnemonic::mov, Zb, Ib);
		map[0xb8 + i] = op(Mnemonic::mov, Zv, Iv);
	}
	map[0x60] = bad();
	map[0x61] = bad();
	map[0x62] = bad();
	map[0x63] = op(Mnemonic::movsxd, Gv, Ed);
	map[0x68] = op(Mnemonic::push, Iz, None, None, default64);
	map[0x69] = op(Mnemonic::imul, Gv, Ev, Iz);
	map[0x6a] = op(Mnemonic::push, Ibs, None, None, default64);
	map[0x6b] = op(Mnemonic::imul, Gv, Ev, Ibs);
	for (size_t i = 0x6c; i <= 0x6f; ++i)
		map[i] = other();
	for (size_t i = 0; i < 16; ++i)
		map[0x70 + i] = op(Mnemonic::jcc, Jb, None, None, conditional);
	map[0x80] = grp(group1, Eb, Ib);
	map[0x81] = grp(group1, Ev, Iz);
	map[0x82] = bad();
	map[0x83] = grp(group1, Ev, Ibs);
	map[0x84] = op(Mnemonic::test, Eb, Gb);
	map[0x85] = op(Mnemonic::test, Ev, Gv);
	map[0x86] = op(Mnemonic::xchg, Eb, Gb);
	map[0x87] = op(Mnemonic::xchg, Ev, Gv);
	map[0x88] = op(Mnemonic::mov, Eb, Gb);
	map[0x89] = op(Mnemonic::mov, Ev, Gv);
	map[0x8a] = op(Mnemonic::mov, Gb, Eb);
	map[0x8b] = op(Mnemonic::mov, Gv, Ev);
	map[0x8c] = other_modrm();
	map[0x8d] = op(Mnemonic::lea, Gv, M);
	map[0x8e] = other_modrm();
	map[0x8f] = grp(group1a);
	map[0x98] = op(Mnemonic::cbw);
	map[0x99] = op(Mnemonic::cwd);
	map[0x9a] = bad();
	for (size_t i = 0x9b; i <= 0x9f; ++i)
		map[i] = other();
	for (size_t i = 0xa0; i <= 0xa3; ++i)
		map[i] = other(Ob);
	for (size_t i = 0xa4; i <= 0xa7; ++i)
		map[i] = other();
	map[0xa8] = op(Mnemonic::test, AL, Ib);
	map[0xa9] = op(Mnemonic::test, AX, Iz);
	for (size_t i = 0xaa; i <= 0xaf; ++i)
		map[i] = other();
	map[0xc0] = grp(group2, Eb, Ib);
	map[0xc1] = grp(group2, Ev, Ib);
	map[0xc2] = op(Mnemonic::ret, Iw);
	map[0xc3] = op(Mnemonic::ret);
	map[0xc6] = grp(group11b);
	map[0xc7] = grp(group11v);
	map[0xc8] = other(Ibw);
	map[0xc9] = op(Mnemonic::leave);
	map[0xca] = other(Iw);
	map[0xcb] = other();
	map[0xcc] = op(Mnemonic::int3);
	map[0xcd] = other(Ib);
	map[0xce] = bad();
	map[0xcf] = other();
	map[0xd0] = grp(group2, Eb, One);
	map[0xd1] = grp(group2, Ev, One);
	map[0xd2] = grp(group2, Eb, CL);
	map[0xd3] = grp(group2, Ev, CL);
	map[0xd4] = bad();
	map[0xd5] = bad();
	map[0xd6] = bad();
	map[0xd7] = other();
	for (size_t i = 0xd8; i <= 0xdf; ++i)
		map[i] = other_modrm();
	for (size_t i = 0xe0; i <= 0xe3; ++i)
		map[i] = other(Jb);
	for (size_t i = 0xe4; i <= 0xe7; ++i)
		map[i] = other(Ib);
	map[0xe8] = op(Mnemonic::call, Jz, None, None, default64);
	map[0xe9] = op(Mnemonic::jmp, Jz, None, None, default64);
	map[0xea] = bad();
	map[0xeb] = op(Mnemonic::jmp, Jb, None, None, default64);
	for (size_t i = 0xec; i <= 0xef; ++i)
		map[i] = other();
	map[0xf1] = other();
	map[0xf4] = op(Mnemonic::hlt);
	map[0xf5] = other();
	map[0xf6] = grp(group3b);
	map[0xf7] = grp(group3v);
	for (size_t i = 0xf8; i <= 0xfd; ++i)
		map[i] = other();
	map[0xfe] = grp(group4);
	map[0xff] = grp(group5);
	return map;
}

constexpr Map make_two_byte()
{
	Map map{};
	for (auto &entry : map)
		entry = other_modrm();
	map[0x04] = bad();
	map[0x05] = op(Mnemonic::syscall);
	map[0x06] = other();
	map[0x07] = other();
	map[0x08] = other();
	map[0x09] = other();
	map[0x0a] = bad();
	map[0x0b] = op(Mnemonic::ud2);
	map[0x0c] = bad();
	map[0x0e] = other();
	map[0x0f] = other_modrm(Ib);
	map[0x1f] = grp(nop_group);
	for (size_t i = 0x20; i <= 0x23; ++i)
		map[i] = other(None, None, with_modrm | register_only);
	for (size_t i = 0x24; i <= 0x27; ++i)
		map[i] = bad();
	for (size_t i = 0x30; i <= 0x37; ++i)
		map[i] = other();
	map[0x36] = bad();
	map[0x38] = bad();
	map[0x39] = bad();
	map[0x3a] = bad();
	for (size_t i = 0x3b; i <= 0x3f; ++i)
		map[i] = bad();
	for (size_t i = 0; i < 16; ++i) {
		map[0x40 + i] = op(Mnemonic::cmovcc, Gv, Ev, None, conditional);
		map[0x80 + i] = op(Mnemonic::jcc, Jz, None, None, conditional | default64);
		map[0x90 + i] = op(Mnemonic::setcc, Eb, None, None, conditional);
	}
	for (size_t i = 0x70; i <= 0x73; ++i)
		map[i] = other_modrm(Ib);
	map[0x77] = other();
	for (size_t i : {0xa0u, 0xa1u, 0xa2u, 0xa8u, 0xa9u, 0xaau})
		map[i] = other();
	map[0xa6] = bad();
	map[0xa7] = bad();
	map[0xa4] = other_modrm(Ib);
	map[0xac] = other_modrm(Ib);
	map[0xaf] = op(Mnemonic::imul, Gv, Ev);
	map[0xb0] = op(Mnemonic::cmpxchg, Eb, Gb);
	map[0xb1] = op(Mnemonic::cmpxchg, Ev, Gv);
	map[0xb6] = op(Mnemonic::movzx, Gv, Eb);
	map[0xb7] = op(Mnemonic::movzx, Gv, Ew);
	map[0xba] = other_modrm(Ib);
	map[0xbe] = op(Mnemonic::movsx, Gv, Eb);
	map[0xbf] = op(Mnemonic::movsx, Gv, Ew);
	map[0xc0] = op(Mnemonic::xadd, Eb, Gb);
	map[0xc1] = op(Mnemonic::xadd, Ev, Gv);
	map[0xc2] = other_modrm(Ib);
	for (size_t i = 0xc4; i <= 0xc6; ++i)
		map[i] = other_modrm(Ib);
	for (size_t i = 0xc8; i <= 0xcf; ++i)
		map[i] = other();
	return map;
}

constexpr Map one_byte = make_one_byte();
constexpr Map two_byte = make_two_byte();

class Reader {
public:
	Reader(std::span<const uint8_t> bytes) : bytes(bytes) {}
	bool has(size_t count) const { return at + count <= bytes.size() && at + count <= 15; }
	uint8_t peek() const { return bytes[at]; }
	uint64_t take(size_t count)
	{
		uint64_t value = 0;
		for (size_t i = 0; i < count; ++i)
			value |= uint64_t(bytes[at + i]) << (8 * i);
		at += count;
		return value;
	}
	size_t at = 0;

private:
	std::span<const uint8_t> bytes;
};

int64_t sign_extend(uint64_t value, size_t bytes)
{
	const unsigned shift = unsigned(64 - 8 * bytes);
	return static_cast<int64_t>(value << shift) >> shift;
}

struct Prefixes {
	bool operand16 = false;
	bool address32 = false;
	bool lock = false;
	bool rep = false;
	bool repne = false;
	Segment segment = Segment::none;
	uint8_t rex = 0;
};

struct ModRM {
	uint8_t mod = 0, reg = 0, rm = 0;
};

bool read_memory(Reader &reader, const Prefixes &prefixes, ModRM modrm, Operand &operand)
{
	operand.kind = OperandKind::mem;
	operand.segment = prefixes.segment;
	const bool rex_b = prefixes.rex & 1, rex_x = prefixes.rex & 2;
	size_t displacement = modrm.mod == 1 ? 1 : modrm.mod == 2 ? 4 : 0;
	if (modrm.rm == 4) {
		if (!reader.has(1))
			return false;
		const uint8_t sib = static_cast<uint8_t>(reader.take(1));
		const uint8_t scale = sib >> 6, index = (sib >> 3) & 7, base = sib & 7;
		const uint8_t full_index = static_cast<uint8_t>(index | (rex_x ? 8 : 0));
		operand.scale = static_cast<uint8_t>(1 << scale);
		if (full_index != 4)
			operand.index = full_index;
		else {
			const bool has_base = !(base == 5 && modrm.mod == 0);
			const uint8_t full_base = static_cast<uint8_t>(base | (rex_b ? 8 : 0));
			operand.redundant_sib = scale != 0 || (has_base && full_base != 4 && full_base != 12);
		}
		if (base == 5 && modrm.mod == 0)
			displacement = 4;
		else
			operand.base = static_cast<uint8_t>(base | (rex_b ? 8 : 0));
	} else if (modrm.rm == 5 && modrm.mod == 0) {
		operand.rip_relative = true;
		displacement = 4;
	} else
		operand.base = static_cast<uint8_t>(modrm.rm | (rex_b ? 8 : 0));
	if (!reader.has(displacement))
		return false;
	operand.value = displacement ? sign_extend(reader.take(displacement), displacement) : 0;
	return true;
}

}

Instruction decode(std::span<const uint8_t> bytes, uint64_t address)
{
	Instruction result;
	Reader reader(bytes);
	Prefixes prefixes;
	// Legacy prefixes in any order; a REX prefix counts only right before the opcode.
	for (;;) {
		if (!reader.has(1))
			return {};
		const uint8_t byte = reader.peek();
		if (byte >= 0x40 && byte <= 0x4f) {
			prefixes.rex = byte;
			reader.take(1);
			continue;
		}
		bool legacy = true;
		switch (byte) {
		case 0x66: prefixes.operand16 = true; break;
		case 0x67: prefixes.address32 = true; break;
		case 0xf0: prefixes.lock = true; break;
		case 0xf2: prefixes.repne = true; prefixes.rep = false; break;
		case 0xf3: prefixes.rep = true; prefixes.repne = false; break;
		case 0x26: prefixes.segment = Segment::es; break;
		case 0x2e: prefixes.segment = Segment::cs; break;
		case 0x36: prefixes.segment = Segment::ss; break;
		case 0x3e: prefixes.segment = Segment::ds; break;
		case 0x64: prefixes.segment = Segment::fs; break;
		case 0x65: prefixes.segment = Segment::gs; break;
		default: legacy = false;
		}
		if (!legacy)
			break;
		prefixes.rex = 0;
		reader.take(1);
	}
	const bool rex_w = prefixes.rex & 8, rex_r = prefixes.rex & 4, rex_b = prefixes.rex & 1;
	uint8_t opcode = static_cast<uint8_t>(reader.take(1));
	Entry entry;
	uint32_t full_opcode = opcode;
	// VEX, EVEX and XOP carry their own map: the length of what follows is all the decoder needs from them.
	if (opcode == 0xc4 || opcode == 0xc5 || opcode == 0x62 || opcode == 0x8f) {
		size_t payload = opcode == 0xc5 ? 1 : opcode == 0x62 ? 3 : 2;
		if (!reader.has(payload + 1))
			return {};
		if (opcode == 0x8f && ((reader.peek() >> 3) & 7) == 0) {
			entry = groups[group1a].entries[0];
		} else {
			const uint8_t first = reader.peek();
			unsigned map = opcode == 0xc5 ? 1 : opcode == 0x62 ? (first & 7) : (first & 0x1f);
			if (opcode == 0x8f && map < 8)
				return {};
			if (opcode == 0x62 && (map == 0 || map == 4 || map == 7))
				return {};
			reader.take(payload);
			if (!reader.has(1))
				return {};
			const uint8_t vex_opcode = static_cast<uint8_t>(reader.take(1));
			Spec imm = None;
			if (map == 3 || map == 8 || (map == 1 && (vex_opcode == 0xc2 || (vex_opcode >= 0x70 && vex_opcode <= 0x73) ||
					(vex_opcode >= 0xc4 && vex_opcode <= 0xc6))))
				imm = Ib;
			else if (map == 10)
				imm = Id;
			const bool has_modrm = !(map == 1 && vex_opcode == 0x77 && opcode != 0x62);
			entry = has_modrm ? other_modrm(imm) : other();
			full_opcode = 0x10000u | vex_opcode;
			prefixes.rex = 0;
		}
	} else if (opcode == 0xd5) {
		// REX2 (APX): one payload byte whose top bit selects the 0F map. Nothing translates APX code.
		if (!reader.has(2))
			return {};
		const uint8_t payload = static_cast<uint8_t>(reader.take(1));
		const uint8_t next = static_cast<uint8_t>(reader.take(1));
		entry = (payload & 0x80) ? two_byte[next] : one_byte[next];
		if (entry.mnemonic == Mnemonic::invalid)
			return {};
		entry.mnemonic = Mnemonic::other;
		if (entry.flags & group)
			entry.flags = static_cast<uint8_t>(entry.flags & ~group);
		full_opcode = (payload & 0x80) ? 0x0f00u | next : next;
		opcode = next;
		prefixes.rex = static_cast<uint8_t>(0x40 | (payload & 0x0f));
	} else if (opcode == 0x0f) {
		if (!reader.has(1))
			return {};
		const uint8_t second = static_cast<uint8_t>(reader.take(1));
		full_opcode = 0x0f00u | second;
		if (second == 0x38 || second == 0x3a) {
			if (!reader.has(1))
				return {};
			const uint8_t third = static_cast<uint8_t>(reader.take(1));
			full_opcode = (second == 0x38 ? 0x0f3800u : 0x0f3a00u) | third;
			entry = other_modrm(second == 0x3a ? Ib : None);
		} else
			entry = two_byte[second];
		opcode = second;
	} else
		entry = one_byte[opcode];
	if (entry.mnemonic == Mnemonic::invalid)
		return {};

	ModRM modrm;
	if (entry.flags & with_modrm) {
		if (!reader.has(1))
			return {};
		const uint8_t byte = static_cast<uint8_t>(reader.take(1));
		modrm = {static_cast<uint8_t>(byte >> 6), static_cast<uint8_t>((byte >> 3) & 7), static_cast<uint8_t>(byte & 7)};
		if (entry.flags & register_only)
			modrm.mod = 3;
		if (entry.flags & group) {
			const Entry &member = groups[entry.group_index].entries[modrm.reg];
			if (member.mnemonic == Mnemonic::invalid)
				return {};
			Entry merged = member;
			if (merged.a == None) {
				merged.a = entry.a;
				merged.b = entry.b;
			}
			merged.flags |= entry.flags & ~group;
			entry = merged;
		}
		// C6 /7 and C7 /7 are xabort and xbegin only as C6 F8 and C7 F8.
		if ((full_opcode == 0xc6 || full_opcode == 0xc7) && modrm.reg == 7 && (modrm.mod != 3 || modrm.rm != 0))
			return {};
	}
	const bool default_64 = (entry.flags & default64) || entry.a == Eq || entry.a == Zq;
	result.operand_size = rex_w ? 8 : prefixes.operand16 ? 2 : default_64 ? 8 : 4;
	result.address_size = prefixes.address32 ? 4 : 8;
	result.mnemonic = entry.mnemonic;
	result.opcode = full_opcode;
	result.lock = prefixes.lock;
	result.rep = prefixes.rep;
	result.repne = prefixes.repne;
	if (entry.flags & conditional)
		result.condition = opcode & 0xf;

	auto gpr = [&](uint8_t number, uint8_t size, bool extended) {
		Operand operand;
		operand.kind = OperandKind::reg;
		operand.size = size;
		operand.reg = static_cast<uint8_t>(number | (extended ? 8 : 0));
		if (size == 1 && !prefixes.rex && number >= 4 && number <= 7)
			operand.reg = static_cast<uint8_t>(high_byte_base + number - 4);
		return operand;
	};
	auto rm_operand = [&](uint8_t size, Operand &operand) {
		if (modrm.mod == 3) {
			operand = gpr(modrm.rm, size, rex_b);
			return true;
		}
		operand.size = size;
		return read_memory(reader, prefixes, modrm, operand);
	};
	auto immediate = [&](size_t size, Operand &operand, bool sign) {
		if (!reader.has(size))
			return false;
		operand.kind = OperandKind::imm;
		const uint64_t raw = reader.take(size);
		operand.value = sign ? sign_extend(raw, size) : int64_t(raw);
		operand.size = static_cast<uint8_t>(size);
		operand.encoded_size = static_cast<uint8_t>(size);
		return true;
	};
	const uint8_t v = result.operand_size;
	uint8_t count = 0;
	for (Spec spec : {entry.a, entry.b, entry.c}) {
		if (spec == None)
			break;
		Operand operand;
		bool ok = true;
		switch (spec) {
		case Eb: ok = rm_operand(1, operand); break;
		case Ev: case Eq: ok = rm_operand(v, operand); break;
		case Ew: ok = rm_operand(2, operand); break;
		case Ed: ok = rm_operand(4, operand); break;
		case M:
			if (modrm.mod == 3)
				return {};
			ok = rm_operand(0, operand);
			break;
		case Gb: operand = gpr(modrm.reg, 1, rex_r); break;
		case Gv: operand = gpr(modrm.reg, v, rex_r); break;
		case Zb: operand = gpr(opcode & 7, 1, rex_b); break;
		case Zv: case Zq: operand = gpr(opcode & 7, v, rex_b); break;
		case AL: operand = gpr(0, 1, false); break;
		case AX: operand = gpr(0, v, false); break;
		case CL: operand = gpr(1, 1, false); break;
		case One:
			operand.kind = OperandKind::imm;
			operand.size = 1;
			operand.value = 1;
			break;
		case Ib: {
			const bool shift = entry.mnemonic >= Mnemonic::rol && entry.mnemonic <= Mnemonic::sar;
			ok = immediate(1, operand, !shift && (entry.a == Eb || entry.a == AL || entry.a == Zb));
			break;
		}
		case Ibs: ok = immediate(1, operand, true); operand.size = v; break;
		case Iw: ok = immediate(2, operand, false); break;
		case Iz: ok = immediate(v == 2 ? 2 : 4, operand, true); operand.size = v; break;
		case Iv: ok = immediate(v, operand, true); break;
		case Id: ok = immediate(4, operand, false); break;
		case Ibw: ok = immediate(2, operand, false) && immediate(1, operand, false); break;
		case Ob: ok = immediate(result.address_size, operand, false); break;
		case Jb: case Jz: {
			const size_t size = spec == Jb ? 1 : 4;
			if (!reader.has(size))
				return {};
			operand.kind = OperandKind::rel;
			const int64_t displacement = sign_extend(reader.take(size), size);
			operand.value = int64_t(address + reader.at) + displacement;
			operand.size = 8;
			break;
		}
		default: break;
		}
		if (!ok)
			return {};
		if (count < result.operands.size())
			result.operands[count++] = operand;
	}
	result.count = count;
	// 90 is xchg eax, eax only with REX.B; without it, and as F3 90 (pause), it does nothing to registers.
	if (full_opcode == 0x90 && !rex_b) {
		result.mnemonic = prefixes.rep ? Mnemonic::other : Mnemonic::nop;
		result.count = 0;
	}
	// LOCK applies only to read-modify-write forms with a memory destination; anything else raises #UD.
	if (prefixes.lock && result.mnemonic != Mnemonic::other) {
		static constexpr Mnemonic lockable[] = {Mnemonic::add, Mnemonic::or_, Mnemonic::adc, Mnemonic::sbb, Mnemonic::and_, Mnemonic::sub,
			Mnemonic::xor_, Mnemonic::inc, Mnemonic::dec, Mnemonic::neg, Mnemonic::not_, Mnemonic::xchg, Mnemonic::cmpxchg, Mnemonic::xadd};
		bool allowed = false;
		for (Mnemonic mnemonic : lockable)
			allowed |= mnemonic == result.mnemonic;
		if (!allowed || result.count == 0 || result.operands[0].kind != OperandKind::mem)
			return {};
	}
	result.length = static_cast<uint8_t>(reader.at);
	return result;
}

static const char *register_name(uint8_t reg, uint8_t size, bool address32 = false)
{
	static const char *const names64[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12",
		"r13", "r14", "r15"};
	static const char *const names32[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d",
		"r12d", "r13d", "r14d", "r15d"};
	static const char *const names16[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w",
		"r13w", "r14w", "r15w"};
	static const char *const names8[] = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b", "r10b", "r11b", "r12b",
		"r13b", "r14b", "r15b"};
	static const char *const high[] = {"ah", "ch", "dh", "bh"};
	if (reg >= high_byte_base)
		return high[reg - high_byte_base];
	if (address32)
		return names32[reg];
	switch (size) {
	case 1: return names8[reg];
	case 2: return names16[reg];
	case 4: return names32[reg];
	default: return names64[reg];
	}
}

static std::string hex(int64_t value)
{
	char buffer[32];
	if (value < 0)
		std::snprintf(buffer, sizeof buffer, "-0x%llx", static_cast<unsigned long long>(-static_cast<uint64_t>(value)));
	else
		std::snprintf(buffer, sizeof buffer, "0x%llx", static_cast<unsigned long long>(value));
	return buffer;
}

static std::string format_operand(const Instruction &instruction, const Operand &operand)
{
	switch (operand.kind) {
	case OperandKind::reg: return register_name(operand.reg, operand.size);
	case OperandKind::imm: {
		// LLVM sign-extends an imm8 and any immediate of a 64-bit operation; a wider immediate of a narrower one prints unsigned.
		int64_t value = operand.value;
		if (instruction.mnemonic == Mnemonic::ret)
			value = static_cast<int16_t>(value);
		else if (operand.encoded_size == 2 && operand.size != 8)
			value = static_cast<uint16_t>(value);
		else if (operand.encoded_size == 4 && operand.size != 8)
			value = static_cast<uint32_t>(value);
		return hex(value);
	}
	case OperandKind::rel: {
		char buffer[32];
		std::snprintf(buffer, sizeof buffer, "0x%llx", static_cast<unsigned long long>(operand.value));
		return buffer;
	}
	case OperandKind::mem: {
		static const char *const sizes[] = {"", "byte ptr ", "word ptr ", "", "dword ptr ", "", "", "", "qword ptr "};
		static const char *const segments[] = {"", "es:", "cs:", "ss:", "ds:", "fs:", "gs:"};
		std::string text = sizes[operand.size];
		text += segments[static_cast<int>(operand.segment)];
		text += '[';
		const bool address32 = instruction.address_size == 4;
		bool first = true;
		if (operand.rip_relative) {
			text += address32 ? "eip" : "rip";
			first = false;
		} else if (operand.base != no_register) {
			text += register_name(operand.base, 8, address32);
			first = false;
		}
		if (operand.index != no_register || operand.redundant_sib) {
			if (!first)
				text += " + ";
			if (operand.scale != 1 || first)
				text += std::to_string(operand.scale) + "*";
			if (operand.index != no_register)
				text += register_name(operand.index, 8, address32);
			else
				text += address32 ? "eiz" : "riz";
			first = false;
		}
		int64_t displacement = operand.value;
		if (address32 && first)
			displacement = static_cast<uint32_t>(displacement);
		if (first)
			text += hex(displacement);
		else if (displacement < 0)
			text += " - " + hex(-displacement);
		else if (displacement > 0)
			text += " + " + hex(displacement);
		return text + ']';
	}
	default: return {};
	}
}

std::string format_intel(const Instruction &instruction)
{
	static const char *const conditions[] = {"o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"};
	std::string name;
	switch (instruction.mnemonic) {
	case Mnemonic::invalid: case Mnemonic::other: return {};
	case Mnemonic::add: name = "add"; break;
	case Mnemonic::or_: name = "or"; break;
	case Mnemonic::adc: name = "adc"; break;
	case Mnemonic::sbb: name = "sbb"; break;
	case Mnemonic::and_: name = "and"; break;
	case Mnemonic::sub: name = "sub"; break;
	case Mnemonic::xor_: name = "xor"; break;
	case Mnemonic::cmp: name = "cmp"; break;
	case Mnemonic::inc: name = "inc"; break;
	case Mnemonic::dec: name = "dec"; break;
	case Mnemonic::neg: name = "neg"; break;
	case Mnemonic::not_: name = "not"; break;
	case Mnemonic::mul: name = "mul"; break;
	case Mnemonic::imul: name = "imul"; break;
	case Mnemonic::div: name = "div"; break;
	case Mnemonic::idiv: name = "idiv"; break;
	case Mnemonic::test: name = "test"; break;
	case Mnemonic::mov: name = "mov"; break;
	case Mnemonic::movzx: name = "movzx"; break;
	case Mnemonic::movsx: name = "movsx"; break;
	case Mnemonic::movsxd: name = "movsxd"; break;
	case Mnemonic::lea: name = "lea"; break;
	case Mnemonic::xchg: name = "xchg"; break;
	case Mnemonic::cmpxchg: name = "cmpxchg"; break;
	case Mnemonic::xadd: name = "xadd"; break;
	case Mnemonic::push: name = "push"; break;
	case Mnemonic::pop: name = "pop"; break;
	case Mnemonic::rol: name = "rol"; break;
	case Mnemonic::ror: name = "ror"; break;
	case Mnemonic::rcl: name = "rcl"; break;
	case Mnemonic::rcr: name = "rcr"; break;
	case Mnemonic::shl: name = "shl"; break;
	case Mnemonic::shr: name = "shr"; break;
	case Mnemonic::sar: name = "sar"; break;
	case Mnemonic::jmp: name = "jmp"; break;
	case Mnemonic::jcc: name = std::string("j") + conditions[instruction.condition]; break;
	case Mnemonic::call: name = "call"; break;
	case Mnemonic::ret: name = "ret"; break;
	case Mnemonic::setcc: name = std::string("set") + conditions[instruction.condition]; break;
	case Mnemonic::cmovcc: name = std::string("cmov") + conditions[instruction.condition]; break;
	case Mnemonic::cbw: name = instruction.operand_size == 8 ? "cdqe" : instruction.operand_size == 4 ? "cwde" : "cbw"; break;
	case Mnemonic::cwd: name = instruction.operand_size == 8 ? "cqo" : instruction.operand_size == 4 ? "cdq" : "cwd"; break;
	case Mnemonic::leave: name = "leave"; break;
	case Mnemonic::nop: name = "nop"; break;
	case Mnemonic::int3: name = "int3"; break;
	case Mnemonic::syscall: name = "syscall"; break;
	case Mnemonic::ud2: name = "ud2"; break;
	case Mnemonic::hlt: name = "hlt"; break;
	}
	std::string text;
	if (instruction.lock)
		text += "lock ";
	if (instruction.rep)
		text += "rep ";
	if (instruction.repne)
		text += "repne ";
	bool memory = false;
	for (uint8_t i = 0; i < instruction.count; ++i)
		memory |= instruction.operands[i].kind == OperandKind::mem;
	if (instruction.address_size == 4 && !memory)
		text += "addr32 ";
	if (instruction.mnemonic == Mnemonic::mov && instruction.opcode >= 0xb8 && instruction.opcode <= 0xbf && instruction.operand_size == 8)
		name = "movabs";
	text += name;
	// LLVM prints the register of ModRM.reg first when xchg names two registers.
	const bool swap = instruction.mnemonic == Mnemonic::xchg && (instruction.opcode == 0x86 || instruction.opcode == 0x87) &&
		instruction.operands[0].kind == OperandKind::reg;
	for (uint8_t i = 0; i < instruction.count; ++i) {
		const Operand &operand = instruction.operands[swap ? 1 - i : i];
		// The count of a shift by one is implicit in its encoding and LLVM prints none.
		if (operand.kind == OperandKind::imm && i == 1 && instruction.opcode >= 0xd0 && instruction.opcode <= 0xd1)
			break;
		text += i == 0 ? " " : ", ";
		text += format_operand(instruction, operand);
	}
	return text;
}

}
