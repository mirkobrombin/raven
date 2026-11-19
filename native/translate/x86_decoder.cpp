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

// SSE and SSE2 in the 0F map, by mandatory prefix: none, 66, F3, F2 (F2 and F3 take precedence over 66). The form says where the
// operands come from: V is the XMM register of ModRM.reg, W the XMM register or memory of ModRM.rm, U that register alone, M that
// memory alone, E a general register or memory of four bytes (eight with REX.W), G the general register of ModRM.reg, I an imm8.
// ev_ib, ve_ib and gu_ib add an imm8 (pextr*, pinsr*, extractps, pextrw); vw0 has xmm0 as an implicit third operand (blendv).
enum class SseForm : uint8_t { none, vw, wv, vw_ib, vm, mv, vu, ve, ev, gw, gu, ui, m, ev_ib, ve_ib, gu_ib, vw0 };

struct SseEntry {
	Mnemonic mnemonic = Mnemonic::invalid;
	SseForm form = SseForm::none;
	// The size of the memory operand.
	uint8_t size = 0;
};

using SseMap = std::array<std::array<SseEntry, 256>, 4>;

constexpr SseMap make_sse_map()
{
	SseMap map{};
	using enum Mnemonic;
	using F = SseForm;
	auto set = [&](unsigned prefix, unsigned opcode, Mnemonic mnemonic, SseForm form, uint8_t size) {
		map[prefix][opcode] = SseEntry{mnemonic, form, size};
	};
	set(0, 0x10, movups, F::vw, 16); set(1, 0x10, movupd, F::vw, 16); set(2, 0x10, movss, F::vw, 4); set(3, 0x10, movsd, F::vw, 8);
	set(0, 0x11, movups, F::wv, 16); set(1, 0x11, movupd, F::wv, 16); set(2, 0x11, movss, F::wv, 4); set(3, 0x11, movsd, F::wv, 8);
	set(0, 0x12, movlps, F::vm, 8); set(1, 0x12, movlpd, F::vm, 8);
	set(0, 0x13, movlps, F::mv, 8); set(1, 0x13, movlpd, F::mv, 8);
	set(0, 0x14, unpcklps, F::vw, 16); set(1, 0x14, unpcklpd, F::vw, 16);
	set(0, 0x15, unpckhps, F::vw, 16); set(1, 0x15, unpckhpd, F::vw, 16);
	set(0, 0x16, movhps, F::vm, 8); set(1, 0x16, movhpd, F::vm, 8);
	set(0, 0x17, movhps, F::mv, 8); set(1, 0x17, movhpd, F::mv, 8);
	set(0, 0x28, movaps, F::vw, 16); set(1, 0x28, movapd, F::vw, 16);
	set(0, 0x29, movaps, F::wv, 16); set(1, 0x29, movapd, F::wv, 16);
	set(2, 0x2a, cvtsi2ss, F::ve, 4); set(3, 0x2a, cvtsi2sd, F::ve, 4);
	set(0, 0x2b, movntps, F::mv, 16); set(1, 0x2b, movntpd, F::mv, 16);
	set(2, 0x2c, cvttss2si, F::gw, 4); set(3, 0x2c, cvttsd2si, F::gw, 8);
	set(2, 0x2d, cvtss2si, F::gw, 4); set(3, 0x2d, cvtsd2si, F::gw, 8);
	set(0, 0x2e, ucomiss, F::vw, 4); set(1, 0x2e, ucomisd, F::vw, 8);
	set(0, 0x2f, comiss, F::vw, 4); set(1, 0x2f, comisd, F::vw, 8);
	set(0, 0x50, movmskps, F::gu, 16); set(1, 0x50, movmskpd, F::gu, 16);
	// The arithmetic rows 51 and 54-5F: ps, pd, ss, sd.
	const Mnemonic arithmetic[][4] = {{sqrtps, sqrtpd, sqrtss, sqrtsd}, {invalid, invalid, invalid, invalid},
		{invalid, invalid, invalid, invalid}, {andps, andpd, invalid, invalid}, {andnps, andnpd, invalid, invalid},
		{orps, orpd, invalid, invalid}, {xorps, xorpd, invalid, invalid}, {addps, addpd, addss, addsd}, {mulps, mulpd, mulss, mulsd},
		{cvtps2pd, cvtpd2ps, cvtss2sd, cvtsd2ss}, {cvtdq2ps, cvtps2dq, cvttps2dq, invalid}, {subps, subpd, subss, subsd},
		{minps, minpd, minss, minsd}, {divps, divpd, divss, divsd}, {maxps, maxpd, maxss, maxsd}};
	const uint8_t sizes[] = {16, 16, 4, 8};
	for (unsigned row = 0; row < 15; ++row)
		for (unsigned prefix = 0; prefix < 4; ++prefix)
			if (arithmetic[row][prefix] != invalid)
				set(prefix, 0x51 + row, arithmetic[row][prefix], F::vw, sizes[prefix]);
	set(0, 0x5a, cvtps2pd, F::vw, 8);
	set(2, 0x5b, cvttps2dq, F::vw, 16);
	// The integer rows with 66.
	const Mnemonic integer[] = {punpcklbw, punpcklwd, punpckldq, packsswb, pcmpgtb, pcmpgtw, pcmpgtd, packuswb, punpckhbw, punpckhwd,
		punpckhdq, packssdw, punpcklqdq, punpckhqdq};
	for (unsigned i = 0; i < 14; ++i)
		set(1, 0x60 + i, integer[i], F::vw, 16);
	set(1, 0x6e, movd, F::ve, 4);
	set(1, 0x6f, movdqa, F::vw, 16); set(2, 0x6f, movdqu, F::vw, 16);
	set(1, 0x70, pshufd, F::vw_ib, 16); set(2, 0x70, pshufhw, F::vw_ib, 16); set(3, 0x70, pshuflw, F::vw_ib, 16);
	set(1, 0x74, pcmpeqb, F::vw, 16); set(1, 0x75, pcmpeqw, F::vw, 16); set(1, 0x76, pcmpeqd, F::vw, 16);
	set(1, 0x7e, movd, F::ev, 4); set(2, 0x7e, movq, F::vw, 8);
	set(1, 0x7f, movdqa, F::wv, 16); set(2, 0x7f, movdqu, F::wv, 16);
	set(0, 0xc2, cmpps, F::vw_ib, 16); set(1, 0xc2, cmppd, F::vw_ib, 16); set(2, 0xc2, cmpss, F::vw_ib, 4); set(3, 0xc2, cmpsd, F::vw_ib, 8);
	set(0, 0xc6, shufps, F::vw_ib, 16); set(1, 0xc6, shufpd, F::vw_ib, 16);
	set(1, 0xd6, movq, F::wv, 8);
	set(1, 0xd7, pmovmskb, F::gu, 16);
	set(1, 0xe6, cvttpd2dq, F::vw, 16); set(2, 0xe6, cvtdq2pd, F::vw, 8); set(3, 0xe6, cvtpd2dq, F::vw, 16);
	set(1, 0xe7, movntdq, F::mv, 16);
	// SSE3, and pinsrw and pextrw.
	set(3, 0xd0, addsubps, F::vw, 16); set(1, 0xd0, addsubpd, F::vw, 16);
	set(3, 0x7c, haddps, F::vw, 16); set(1, 0x7c, haddpd, F::vw, 16); set(3, 0x7d, hsubps, F::vw, 16); set(1, 0x7d, hsubpd, F::vw, 16);
	set(3, 0x12, movddup, F::vw, 8); set(2, 0x12, movsldup, F::vw, 16); set(2, 0x16, movshdup, F::vw, 16);
	set(3, 0xf0, lddqu, F::vm, 16);
	set(1, 0xc4, pinsrw, F::ve_ib, 2); set(1, 0xc5, pextrw, F::gu_ib, 16);
	const Mnemonic high[] = {invalid, psrlw, psrld, psrlq, paddq, pmullw, invalid, invalid, psubusb, psubusw, pminub, pand, paddusb,
		paddusw, pmaxub, pandn, pavgb, psraw, psrad, pavgw, pmulhuw, pmulhw, invalid, invalid, psubsb, psubsw, pminsw, por, paddsb,
		paddsw, pmaxsw, pxor, invalid, psllw, pslld, psllq, pmuludq, pmaddwd, psadbw, invalid, psubb, psubw, psubd, psubq, paddb,
		paddw, paddd, invalid};
	for (unsigned i = 0; i < 48; ++i)
		if (high[i] != invalid)
			set(1, 0xd0 + i, high[i], F::vw, 16);
	return map;
}

constexpr SseMap sse_map = make_sse_map();

// SSSE3, SSE4.1 and SSE4.2 with 66 in the 0F 38 map (index 0) and the 0F 3A map (index 1).
using EscapeMap = std::array<std::array<SseEntry, 256>, 2>;

constexpr EscapeMap make_escape_map()
{
	EscapeMap map{};
	using enum Mnemonic;
	using F = SseForm;
	auto set = [&](unsigned which, unsigned opcode, Mnemonic mnemonic, SseForm form, uint8_t size) {
		map[which][opcode] = SseEntry{mnemonic, form, size};
	};
	const Mnemonic ssse3[] = {pshufb, phaddw, phaddd, phaddsw, pmaddubsw, phsubw, phsubd, phsubsw, psignb, psignw, psignd, pmulhrsw};
	for (unsigned i = 0; i < 12; ++i)
		set(0, i, ssse3[i], F::vw, 16);
	set(0, 0x10, pblendvb, F::vw0, 16); set(0, 0x14, blendvps, F::vw0, 16); set(0, 0x15, blendvpd, F::vw0, 16);
	set(0, 0x17, ptest, F::vw, 16);
	set(0, 0x1c, pabsb, F::vw, 16); set(0, 0x1d, pabsw, F::vw, 16); set(0, 0x1e, pabsd, F::vw, 16);
	const Mnemonic extend[] = {pmovsxbw, pmovsxbd, pmovsxbq, pmovsxwd, pmovsxwq, pmovsxdq, pmovzxbw, pmovzxbd, pmovzxbq, pmovzxwd,
		pmovzxwq, pmovzxdq};
	const uint8_t extend_sizes[] = {8, 4, 2, 8, 4, 8};
	for (unsigned i = 0; i < 6; ++i) {
		set(0, 0x20 + i, extend[i], F::vw, extend_sizes[i]);
		set(0, 0x30 + i, extend[6 + i], F::vw, extend_sizes[i]);
	}
	set(0, 0x28, pmuldq, F::vw, 16); set(0, 0x29, pcmpeqq, F::vw, 16); set(0, 0x2a, movntdqa, F::vm, 16);
	set(0, 0x2b, packusdw, F::vw, 16); set(0, 0x37, pcmpgtq, F::vw, 16);
	const Mnemonic minmax[] = {pminsb, pminsd, pminuw, pminud, pmaxsb, pmaxsd, pmaxuw, pmaxud, pmulld, phminposuw};
	for (unsigned i = 0; i < 10; ++i)
		set(0, 0x38 + i, minmax[i], F::vw, 16);
	set(1, 0x08, roundps, F::vw_ib, 16); set(1, 0x09, roundpd, F::vw_ib, 16); set(1, 0x0a, roundss, F::vw_ib, 4);
	set(1, 0x0b, roundsd, F::vw_ib, 8); set(1, 0x0c, blendps, F::vw_ib, 16); set(1, 0x0d, blendpd, F::vw_ib, 16);
	set(1, 0x0e, pblendw, F::vw_ib, 16); set(1, 0x0f, palignr, F::vw_ib, 16);
	set(1, 0x14, pextrb, F::ev_ib, 1); set(1, 0x15, pextrw, F::ev_ib, 2); set(1, 0x16, pextrd, F::ev_ib, 4);
	set(1, 0x17, extractps, F::ev_ib, 4);
	set(1, 0x20, pinsrb, F::ve_ib, 1); set(1, 0x21, insertps, F::vw_ib, 4); set(1, 0x22, pinsrd, F::ve_ib, 4);
	set(1, 0x40, dpps, F::vw_ib, 16); set(1, 0x41, dppd, F::vw_ib, 16); set(1, 0x42, mpsadbw, F::vw_ib, 16);
	set(1, 0x60, pcmpestrm, F::vw_ib, 16); set(1, 0x61, pcmpestri, F::vw_ib, 16); set(1, 0x62, pcmpistrm, F::vw_ib, 16);
	set(1, 0x63, pcmpistri, F::vw_ib, 16);
	return map;
}

constexpr EscapeMap escape_map = make_escape_map();

// 66 0F 71, 72 and 73 by ModRM.reg.
constexpr Mnemonic sse_shifts[3][8] = {
	{Mnemonic::invalid, Mnemonic::invalid, Mnemonic::psrlw, Mnemonic::invalid, Mnemonic::psraw, Mnemonic::invalid, Mnemonic::psllw,
		Mnemonic::invalid},
	{Mnemonic::invalid, Mnemonic::invalid, Mnemonic::psrld, Mnemonic::invalid, Mnemonic::psrad, Mnemonic::invalid, Mnemonic::pslld,
		Mnemonic::invalid},
	{Mnemonic::invalid, Mnemonic::invalid, Mnemonic::psrlq, Mnemonic::psrldq, Mnemonic::invalid, Mnemonic::invalid, Mnemonic::psllq,
		Mnemonic::pslldq},
};

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

struct Vex {
	bool present = false;
	bool r = false, x = false, b = false, w = false, l = false;
	uint8_t map = 0, vvvv = 0, pp = 0, opcode = 0;
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

static void decode_x87(Instruction &result, uint8_t index, uint8_t mod, uint8_t reg, uint8_t rm);
static bool decode_vex(Instruction &result, const Vex &vex, const ModRM &modrm, bool rex_w);

Instruction decode(std::span<const uint8_t> bytes, uint64_t address)
{
	Instruction result;
	Reader reader(bytes);
	Prefixes prefixes;
	Vex vex;
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
	// The 0F map without VEX, EVEX or REX2: where SSE and SSE2 live.
	bool legacy_map = false;
	// 0F 38 (1) or 0F 3A (2), and the opcode in it.
	unsigned escape = 0;
	uint8_t escape_opcode = 0;
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
			// VEX (C4, C5) after no 66, F2, F3 or REX prefix: its fields, for the forms the decoder gives semantics to.
			if (opcode != 0x62 && opcode != 0x8f && !prefixes.operand16 && !prefixes.rep && !prefixes.repne && !prefixes.rex &&
				!prefixes.lock) {
				const uint8_t first_byte = reader.peek();
				const uint8_t second_byte = opcode == 0xc4 ? bytes[reader.at + 1] : first_byte;
				vex.present = true;
				vex.map = static_cast<uint8_t>(map);
				vex.r = !(first_byte & 0x80);
				vex.x = opcode == 0xc4 && !(first_byte & 0x40);
				vex.b = opcode == 0xc4 && !(first_byte & 0x20);
				vex.w = opcode == 0xc4 && (second_byte & 0x80);
				vex.vvvv = static_cast<uint8_t>((~second_byte >> 3) & 15);
				vex.l = (second_byte >> 2) & 1;
				vex.pp = second_byte & 3;
			}
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
			prefixes.rex = vex.present ? static_cast<uint8_t>(0x40 | (vex.w ? 8 : 0) | (vex.r ? 4 : 0) | (vex.x ? 2 : 0) | (vex.b ? 1 : 0)) : 0;
			vex.opcode = vex_opcode;
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
			escape = second == 0x38 ? 1 : 2;
			escape_opcode = third;
		} else {
			entry = two_byte[second];
			legacy_map = true;
		}
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
	// SSE4.2 on general registers: crc32 (F2 0F 38 F0 and F1) and popcnt (F3 0F B8).
	if (result.mnemonic == Mnemonic::other && ((escape == 1 && prefixes.repne && !prefixes.rep && (escape_opcode == 0xf0 ||
		escape_opcode == 0xf1)) || (legacy_map && opcode == 0xb8 && prefixes.rep && !prefixes.repne))) {
		const bool byte = escape == 1 && escape_opcode == 0xf0;
		const uint8_t source_size = byte ? 1 : rex_w ? 8 : prefixes.operand16 ? 2 : 4;
		Operand source = result.operands[0];
		if (modrm.mod == 3)
			source = gpr(modrm.rm, source_size, rex_b);
		else
			source.size = source_size;
		const uint8_t target_size = escape == 1 ? (rex_w ? 8 : 4) : source_size;
		result.mnemonic = escape == 1 ? Mnemonic::crc32 : Mnemonic::popcnt;
		result.operands = {gpr(modrm.reg, target_size, rex_r), source};
		result.count = 2;
		result.rep = result.repne = false;
	}
	if ((legacy_map || (escape && prefixes.operand16 && !prefixes.rep && !prefixes.repne)) && result.mnemonic == Mnemonic::other) {
		const unsigned prefix = prefixes.rep ? 2 : prefixes.repne ? 3 : prefixes.operand16 ? 1 : 0;
		SseEntry sse = escape ? escape_map[escape - 1][escape_opcode] : sse_map[prefix][opcode];
		if (escape && rex_w && (escape_opcode == 0x16 || escape_opcode == 0x22) && escape == 2)
			sse = {escape_opcode == 0x16 ? Mnemonic::pextrq : Mnemonic::pinsrq, sse.form, 8};
		const bool memory = modrm.mod != 3;
		if (escape)
			;
		else if (prefix == 0 && (opcode == 0x12 || opcode == 0x16) && !memory)
			sse = {opcode == 0x12 ? Mnemonic::movhlps : Mnemonic::movlhps, SseForm::vu, 16};
		else if (prefix == 1 && opcode >= 0x71 && opcode <= 0x73)
			sse = {sse_shifts[opcode - 0x71][modrm.reg], SseForm::ui, 0};
		else if (prefix == 0 && opcode == 0xae && (modrm.reg == 2 || modrm.reg == 3))
			sse = {modrm.reg == 2 ? Mnemonic::ldmxcsr : Mnemonic::stmxcsr, SseForm::m, 4};
		const SseForm form = sse.form;
		const bool fits = sse.mnemonic != Mnemonic::invalid &&
			!(memory && (form == SseForm::vu || form == SseForm::gu || form == SseForm::ui || form == SseForm::gu_ib)) &&
			!(!memory && (form == SseForm::vm || form == SseForm::mv || form == SseForm::m));
		if (fits) {
			// The generic pass read ModRM.rm into the first operand and the imm8 into the second.
			const Operand rm = result.operands[0];
			const Operand imm = result.operands[1];
			const uint8_t wide = rex_w ? 8 : 4;
			Operand xmm_reg;
			xmm_reg.kind = OperandKind::xmm;
			xmm_reg.reg = static_cast<uint8_t>(modrm.reg | (rex_r ? 8 : 0));
			xmm_reg.size = 16;
			Operand xmm_rm = rm;
			if (memory)
				xmm_rm.size = sse.size;
			else {
				xmm_rm = Operand{};
				xmm_rm.kind = OperandKind::xmm;
				xmm_rm.reg = rm.reg;
				xmm_rm.size = 16;
			}
			Operand general_rm = rm;
			general_rm.size = wide;
			result.mnemonic = sse.mnemonic;
			result.count = 2;
			switch (form) {
			case SseForm::vw: case SseForm::vm: case SseForm::vu: result.operands = {xmm_reg, xmm_rm}; break;
			case SseForm::vw_ib: result.operands = {xmm_reg, xmm_rm, imm}; result.count = 3; break;
			case SseForm::wv: case SseForm::mv: result.operands = {xmm_rm, xmm_reg}; break;
			case SseForm::ve: result.operands = {xmm_reg, general_rm}; break;
			case SseForm::ev: result.operands = {general_rm, xmm_reg}; break;
			case SseForm::gw: result.operands = {gpr(modrm.reg, wide, rex_r), xmm_rm}; break;
			// The masks are 32-bit writes whatever REX.W says, as LLVM prints them.
			case SseForm::gu: result.operands = {gpr(modrm.reg, 4, rex_r), xmm_rm}; break;
			case SseForm::ui: result.operands = {xmm_rm, imm}; break;
			case SseForm::m: result.operands = {xmm_rm}; result.count = 1; break;
			case SseForm::ev_ib: case SseForm::ve_ib: {
				// A general register of 32 bits (64 for pextrq and pinsrq) or memory of the element's size.
				Operand e = memory ? rm : gpr(modrm.rm, sse.size == 8 ? 8 : 4, rex_b);
				if (memory)
					e.size = sse.size;
				if (form == SseForm::ev_ib)
					result.operands = {e, xmm_reg, imm};
				else
					result.operands = {xmm_reg, e, imm};
				result.count = 3;
				break;
			}
			case SseForm::gu_ib: result.operands = {gpr(modrm.reg, 4, rex_r), xmm_rm, imm}; result.count = 3; break;
			case SseForm::vw0: {
				Operand zero;
				zero.kind = OperandKind::xmm;
				zero.reg = 0;
				zero.size = 16;
				result.operands = {xmm_reg, xmm_rm, zero};
				result.count = 3;
				break;
			}
			case SseForm::none: break;
			}
			// movd and its REX.W form, movq.
			if (sse.mnemonic == Mnemonic::movd && rex_w) {
				result.mnemonic = Mnemonic::movq;
				for (Operand &operand : result.operands)
					if (operand.kind == OperandKind::mem)
						operand.size = 8;
			}
			result.rep = result.repne = false;
		}
	}
	if (vex.present && result.mnemonic == Mnemonic::other && !decode_vex(result, vex, modrm, rex_w))
		return {};
	if (full_opcode >= 0xd8 && full_opcode <= 0xdf && !prefixes.lock && !prefixes.rep && !prefixes.repne && !prefixes.operand16 &&
		!rex_w)
		decode_x87(result, static_cast<uint8_t>(full_opcode - 0xd8), modrm.mod, modrm.reg, modrm.rm);
	// wait alone: with a prefix LLVM prints the prefix as an instruction of its own.
	if (full_opcode == 0x9b && reader.at == 1)
		result.mnemonic = Mnemonic::fwait;
	// 90 is xchg eax, eax only with REX.B; without it, and as F3 90 (pause), it does nothing to registers.
	if (full_opcode == 0x90 && !rex_b) {
		result.mnemonic = prefixes.rep ? Mnemonic::other : Mnemonic::nop;
		result.count = 0;
	}
	// cpuid (0F A2) and xgetbv (0F 01 D0), without prefixes: the runtime answers both (cpuid.h).
	if (full_opcode == 0x0fa2 && reader.at == 2) {
		result.mnemonic = Mnemonic::cpuid;
		result.count = 0;
	}
	if (full_opcode == 0x0f01 && reader.at == 3 && bytes[2] == 0xd0) {
		result.mnemonic = Mnemonic::xgetbv;
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

// D8-DF: index is the opcode less D8. The generic pass read ModRM.rm into the first operand; forms LLVM rejects (the reserved
// aliases such as DC D0 and DD C8, and the 287's feni, fdisi and fsetpm) stay other.
// The VEX forms the translator knows: those of SSE and SSE2 in map 1 (sse_map, by VEX.pp), vzeroupper and vzeroall, and the AVX, AVX2
// and FMA3 instructions of avx_first to avx_last. The generic pass left ModRM.rm in operands[0] and an imm8 in operands[1]. Anything
// else stays `other`. Returns false where x86 raises #UD for the encoding (a VEX.vvvv that must be 1111 and is not, VEX.L where the
// form has none), so the instruction does not decode.
static bool decode_vex(Instruction &result, const Vex &vex, const ModRM &modrm, bool)
{
	using enum Mnemonic;
	const bool memory = modrm.mod != 3;
	const Operand rm = result.operands[0];
	const Operand imm = result.operands[1];
	const uint8_t width = vex.l ? 32 : 16;
	auto vector = [](uint8_t number, uint8_t size) {
		Operand operand;
		operand.kind = OperandKind::xmm;
		operand.reg = number;
		operand.size = size;
		return operand;
	};
	auto general = [](uint8_t number, uint8_t size) {
		Operand operand;
		operand.kind = OperandKind::reg;
		operand.reg = number;
		operand.size = size;
		return operand;
	};
	const uint8_t reg_number = static_cast<uint8_t>(modrm.reg | (vex.r ? 8 : 0));
	const uint8_t rm_number = static_cast<uint8_t>(modrm.rm | (vex.b ? 8 : 0));
	auto vrm = [&](uint8_t size, uint8_t memory_size) {
		if (!memory)
			return vector(rm_number, size);
		Operand operand = rm;
		operand.size = memory_size;
		return operand;
	};
	auto set = [&](Mnemonic mnemonic, std::initializer_list<Operand> operands) {
		result.mnemonic = mnemonic;
		result.count = static_cast<uint8_t>(operands.size());
		result.operands = {};
		uint8_t i = 0;
		for (const Operand &operand : operands)
			result.operands[i++] = operand;
		result.vex = true;
		result.rep = result.repne = false;
		return true;
	};
	const auto opcode = vex.opcode;
	if (vex.map == 1 && opcode == 0x77 && vex.pp == 0) {
		result.mnemonic = vex.l ? vzeroall : vzeroupper;
		result.count = 0;
		result.vex = true;
		return vex.vvvv == 0;
	}
	if (vex.map == 2 && vex.pp == 1) {
		if (opcode >= 0x96 && (opcode & 0x0f) >= 8 && opcode <= 0xbf && (opcode >> 4) >= 9 && (opcode >> 4) <= 0xb) {
			const unsigned kind = (opcode & 0x0f) - 8, order = (opcode >> 4) - 9;
			const bool scalar = kind & 1;
			const unsigned type = (scalar ? 2 : 0) + (vex.w ? 1 : 0);
			const auto mnemonic = static_cast<Mnemonic>(static_cast<unsigned>(fma_first) + ((kind / 2) * 3 + order) * 4 + type);
			const uint8_t size = scalar ? 16 : width;
			const uint8_t memory_size = scalar ? (vex.w ? 8 : 4) : width;
			result.vector_size = size;
			return set(mnemonic, {vector(reg_number, size), vector(vex.vvvv, size), vrm(size, memory_size)});
		}
		// The broadcasts: vbroadcastss from memory or (AVX2) a register, vbroadcastsd only to a YMM register.
		const struct {
			uint8_t opcode;
			Mnemonic mnemonic;
			uint8_t element;
			bool needs_256;
			bool memory_only;
		} broadcasts[] = {{0x18, vbroadcastss, 4, false, false}, {0x19, vbroadcastsd, 8, true, false},
			{0x1a, vbroadcastf128, 16, true, true}, {0x78, vpbroadcastb, 1, false, false}, {0x79, vpbroadcastw, 2, false, false},
			{0x58, vpbroadcastd, 4, false, false}, {0x59, vpbroadcastq, 8, false, false}};
		for (const auto &broadcast : broadcasts)
			if (opcode == broadcast.opcode) {
				if (vex.w || vex.vvvv != 0 || (broadcast.needs_256 && !vex.l) || (broadcast.memory_only && !memory))
					return false;
				result.vector_size = width;
				return set(broadcast.mnemonic, {vector(reg_number, width), vrm(16, broadcast.element)});
			}
		return true;
	}
	if (vex.map == 3 && vex.pp == 1 && (opcode == 0x18 || opcode == 0x19 || opcode == 0x38 || opcode == 0x39 || opcode == 0x06 ||
		opcode == 0x46)) {
		if (!vex.l || vex.w)
			return false;
		result.vector_size = 32;
		switch (opcode) {
		case 0x18: case 0x38:
			return set(opcode == 0x18 ? vinsertf128 : vinserti128, {vector(reg_number, 32), vector(vex.vvvv, 32), vrm(16, 16), imm});
		case 0x19: case 0x39:
			if (vex.vvvv != 0)
				return false;
			return set(opcode == 0x19 ? vextractf128 : vextracti128, {vrm(16, 16), vector(reg_number, 32), imm});
		default:
			return set(opcode == 0x06 ? vperm2f128 : vperm2i128, {vector(reg_number, 32), vector(vex.vvvv, 32), vrm(32, 32), imm});
		}
	}
	if (vex.map != 1)
		return true;
	SseEntry sse = sse_map[vex.pp][opcode];
	if (vex.pp == 0 && (opcode == 0x12 || opcode == 0x16) && !memory)
		sse = {opcode == 0x12 ? movhlps : movlhps, SseForm::vu, 16};
	else if (vex.pp == 1 && opcode >= 0x71 && opcode <= 0x73)
		sse = {sse_shifts[opcode - 0x71][modrm.reg], SseForm::ui, 0};
	else if (vex.pp == 0 && opcode == 0xae && (modrm.reg == 2 || modrm.reg == 3))
		sse = {modrm.reg == 2 ? ldmxcsr : stmxcsr, SseForm::m, 4};
	const SseForm form = sse.form;
	if (sse.mnemonic == invalid || (memory && (form == SseForm::vu || form == SseForm::gu || form == SseForm::ui)) ||
		(!memory && (form == SseForm::vm || form == SseForm::mv || form == SseForm::m)))
		return true;
	const Mnemonic mnemonic = sse.mnemonic;
	// The forms with one source: VEX.vvvv is 1111 (0 here) or x86 raises #UD.
	static constexpr Mnemonic unary[] = {movups, movupd, movaps, movapd, movdqa, movdqu, movntps, movntpd, movntdq, sqrtps, sqrtpd,
		cvtdq2ps, cvtps2dq, cvttps2dq, cvtps2pd, cvtpd2ps, cvttpd2dq, cvtdq2pd, cvtpd2dq, pshufd, pshufhw, pshuflw, ucomiss, ucomisd,
		comiss, comisd, movq, movd, movmskps, movmskpd, pmovmskb, cvttss2si, cvttsd2si, cvtss2si, cvtsd2si, ldmxcsr, stmxcsr, movddup,
		movsldup, movshdup, lddqu};
	bool is_unary = false;
	for (const Mnemonic one : unary)
		is_unary |= one == mnemonic;
	const bool scalar_move = (mnemonic == movss || mnemonic == movsd);
	// Loads of a scalar and every store have one source.
	if ((scalar_move && memory) || ((form == SseForm::wv || form == SseForm::mv) && memory))
		is_unary = true;
	// Forms whose elements are no wider than a quadword or a scalar: VEX.L is ignored (scalars) or must be 0.
	static constexpr Mnemonic scalars[] = {movss, movsd, movlps, movlpd, movhps, movhpd, movhlps, movlhps, cvtsi2ss, cvtsi2sd,
		cvttss2si, cvttsd2si, cvtss2si, cvtsd2si, ucomiss, ucomisd, comiss, comisd, sqrtss, sqrtsd, addss, addsd, mulss, mulsd, subss,
		subsd, minss, minsd, divss, divsd, maxss, maxsd, cvtss2sd, cvtsd2ss, cmpss, cmpsd};
	bool scalar = false;
	for (const Mnemonic one : scalars)
		scalar |= one == mnemonic;
	static constexpr Mnemonic l0_only[] = {movq, movd, ldmxcsr, stmxcsr, movlps, movlpd, movhps, movhpd, movhlps, movlhps};
	for (const Mnemonic one : l0_only)
		if (one == mnemonic && vex.l)
			return false;
	// The conversions that change the width between source and destination have their own operand sizes at 256 bits: not given
	// semantics here.
	static constexpr Mnemonic widening[] = {cvtps2pd, cvtpd2ps, cvttpd2dq, cvtdq2pd, cvtpd2dq};
	for (const Mnemonic one : widening)
		if (one == mnemonic && vex.l)
			return true;
	if (is_unary && vex.vvvv != 0)
		return false;
	const uint8_t size = scalar ? 16 : width;
	result.vector_size = size;
	// vmovddup of a YMM register reads 32 bytes, as the other whole-register forms.
	const uint8_t memory_size = sse.size == 16 || (mnemonic == movddup && vex.l) ? size : sse.size;
	const Operand v = vector(reg_number, size);
	const Operand w = vrm(size, memory_size);
	const Operand n = vector(vex.vvvv, size);
	// The shifts by a count in an XMM register or memory read 128 bits of it whatever VEX.L says.
	const bool count_shift = form == SseForm::vw && (mnemonic == psrlw || mnemonic == psrld || mnemonic == psrlq ||
		mnemonic == psraw || mnemonic == psrad || mnemonic == psllw || mnemonic == pslld || mnemonic == psllq);
	const uint8_t wide = vex.w ? 8 : 4;
	switch (form) {
	case SseForm::vw:
		if (scalar_move && !memory)
			return set(mnemonic, {v, n, vector(rm_number, 16)});
		if (count_shift)
			return set(mnemonic, {v, n, vrm(16, 16)});
		if (mnemonic == movq)
			return set(mnemonic, {vector(reg_number, 16), vrm(16, 8)});
		return is_unary ? set(mnemonic, {v, w}) : set(mnemonic, {v, n, w});
	case SseForm::vw_ib: return is_unary ? set(mnemonic, {v, w, imm}) : set(mnemonic, {v, n, w, imm});
	case SseForm::wv: case SseForm::mv:
		if (scalar_move && !memory)
			return set(mnemonic, {vector(rm_number, 16), n, v});
		if (mnemonic == movq)
			return set(mnemonic, {vrm(16, 8), vector(reg_number, 16)});
		return set(mnemonic, {w, v});
	case SseForm::vm: case SseForm::vu: return is_unary ? set(mnemonic, {v, w}) : set(mnemonic, {v, n, w});
	case SseForm::ve: {
		Operand e = memory ? rm : general(rm_number, wide);
		e.size = wide;
		if (mnemonic == movd)
			return set(vex.w ? movq : movd, {vector(reg_number, 16), e});
		return set(mnemonic, {v, n, e});
	}
	case SseForm::ev: {
		Operand e = memory ? rm : general(rm_number, wide);
		e.size = wide;
		return set(vex.w ? movq : movd, {e, vector(reg_number, 16)});
	}
	case SseForm::gw: return set(mnemonic, {general(reg_number, wide), w});
	case SseForm::gu: return set(mnemonic, {general(reg_number, 4), vector(rm_number, width)});
	case SseForm::ui: return set(mnemonic, {vector(vex.vvvv, width), vector(rm_number, width), imm});
	case SseForm::m: return set(mnemonic, {w});
	case SseForm::none: return true;
	}
	return true;
}

static void decode_x87(Instruction &result, uint8_t index, uint8_t mod, uint8_t reg, uint8_t rm)
{
	using enum Mnemonic;
	struct Form {
		Mnemonic mnemonic;
		uint8_t size;
	};
	static constexpr Form memory[8][8] = {
		{{fadd, 4}, {fmul, 4}, {fcom, 4}, {fcomp, 4}, {fsub, 4}, {fsubr, 4}, {fdiv, 4}, {fdivr, 4}},
		{{fld, 4}, {other, 0}, {fst, 4}, {fstp, 4}, {fldenv, 28}, {fldcw, 2}, {fnstenv, 28}, {fnstcw, 2}},
		{{fiadd, 4}, {fimul, 4}, {ficom, 4}, {ficomp, 4}, {fisub, 4}, {fisubr, 4}, {fidiv, 4}, {fidivr, 4}},
		{{fild, 4}, {fisttp, 4}, {fist, 4}, {fistp, 4}, {other, 0}, {fld, 10}, {other, 0}, {fstp, 10}},
		{{fadd, 8}, {fmul, 8}, {fcom, 8}, {fcomp, 8}, {fsub, 8}, {fsubr, 8}, {fdiv, 8}, {fdivr, 8}},
		{{fld, 8}, {fisttp, 8}, {fst, 8}, {fstp, 8}, {frstor, 108}, {other, 0}, {fnsave, 108}, {fnstsw, 2}},
		{{fiadd, 2}, {fimul, 2}, {ficom, 2}, {ficomp, 2}, {fisub, 2}, {fisubr, 2}, {fidiv, 2}, {fidivr, 2}},
		{{fild, 2}, {fisttp, 2}, {fist, 2}, {fistp, 2}, {fbld, 10}, {fild, 8}, {fbstp, 10}, {fistp, 8}},
	};
	if (mod != 3) {
		const Form form = memory[index][reg];
		if (form.mnemonic == other)
			return;
		result.mnemonic = form.mnemonic;
		result.operands[0].size = form.size;
		result.count = 1;
		return;
	}
	// Register forms: st, st(i) (to_top), st(i), st (from_top), st(i) alone (one), or no operand (none).
	enum Shape : uint8_t { none, to_top, from_top, one };
	struct Register {
		Mnemonic mnemonic;
		Shape shape;
	};
	static constexpr Register registers[8][8] = {
		{{fadd, to_top}, {fmul, to_top}, {fcom, one}, {fcomp, one}, {fsub, to_top}, {fsubr, to_top}, {fdiv, to_top}, {fdivr, to_top}},
		{{fld, one}, {fxch, one}, {other, none}, {other, none}, {other, none}, {other, none}, {other, none}, {other, none}},
		{{fcmovb, to_top}, {fcmove, to_top}, {fcmovbe, to_top}, {fcmovu, to_top}, {other, none}, {other, none}, {other, none},
			{other, none}},
		{{fcmovnb, to_top}, {fcmovne, to_top}, {fcmovnbe, to_top}, {fcmovnu, to_top}, {other, none}, {fucomi, to_top},
			{fcomi, to_top}, {other, none}},
		{{fadd, from_top}, {fmul, from_top}, {other, none}, {other, none}, {fsubr, from_top}, {fsub, from_top}, {fdivr, from_top},
			{fdiv, from_top}},
		{{ffree, one}, {other, none}, {fst, one}, {fstp, one}, {fucom, one}, {fucomp, one}, {other, none}, {other, none}},
		{{faddp, from_top}, {fmulp, from_top}, {other, none}, {other, none}, {fsubrp, from_top}, {fsubp, from_top},
			{fdivrp, from_top}, {fdivp, from_top}},
		{{ffreep, one}, {other, none}, {other, none}, {other, none}, {other, none}, {fucompi, to_top}, {fcompi, to_top},
			{other, none}},
	};
	// The forms with no register operand: D9 D0-FF, DA E9, DB E2 and E3, DE D9, DF E0.
	static constexpr Mnemonic d9[48] = {
		fnop, other, other, other, other, other, other, other, other, other, other, other, other, other, other, other,
		fchs, fabs, other, other, ftst, fxam, other, other, fld1, fldl2t, fldl2e, fldpi, fldlg2, fldln2, fldz, other,
		f2xm1, fyl2x, fptan, fpatan, fxtract, fprem1, fdecstp, fincstp, fprem, fyl2xp1, fsqrt, fsincos, frndint, fscale, fsin, fcos};
	Register form = registers[index][reg];
	const uint8_t modrm = static_cast<uint8_t>(0xc0 | reg << 3 | rm);
	if (index == 1 && modrm >= 0xd0)
		form = {d9[modrm - 0xd0], none};
	else if (index == 2 && modrm == 0xe9)
		form = {fucompp, none};
	else if (index == 3 && (modrm == 0xe2 || modrm == 0xe3))
		form = {modrm == 0xe2 ? fnclex : fninit, none};
	else if (index == 6 && modrm == 0xd9)
		form = {fcompp, none};
	if (index == 7 && modrm == 0xe0) {
		result.mnemonic = fnstsw;
		Operand ax;
		ax.kind = OperandKind::reg;
		ax.size = 2;
		ax.reg = 0;
		result.operands = {ax};
		result.count = 1;
		return;
	}
	if (form.mnemonic == other)
		return;
	result.mnemonic = form.mnemonic;
	Operand top, st;
	top.kind = st.kind = OperandKind::st;
	st.reg = rm;
	result.operands = {};
	switch (form.shape) {
	case none: result.count = 0; break;
	case to_top: result.operands = {top, st}; result.count = 2; break;
	case from_top: result.operands = {st, top}; result.count = 2; break;
	case one: result.operands = {st}; result.count = 1; break;
	}
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
	case OperandKind::xmm: return (operand.size == 32 ? "ymm" : "xmm") + std::to_string(operand.reg);
	case OperandKind::st: return operand.reg == no_register ? "st" : "st(" + std::to_string(operand.reg) + ")";
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
		static const char *const sizes[] = {"", "byte ptr ", "word ptr ", "", "dword ptr ", "", "", "", "qword ptr ", "", "tbyte ptr ",
			"", "", "", "", "", "xmmword ptr ", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "ymmword ptr "};
		static const char *const segments[] = {"", "es:", "cs:", "ss:", "ds:", "fs:", "gs:"};
		// Environments and save areas (28 and 108 bytes) print without a size.
		std::string text = operand.size < std::size(sizes) ? sizes[operand.size] : "";
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

static const char *const sse_names[] = {
	"movq", "movd", "movups", "movupd", "movss", "movsd", "movlps", "movlpd", "movhlps", "movhps", "movhpd", "movlhps", "unpcklps",
	"unpcklpd", "unpckhps", "unpckhpd", "movaps", "movapd", "movntps", "movntpd", "cvtsi2ss", "cvtsi2sd", "cvttss2si", "cvttsd2si",
	"cvtss2si", "cvtsd2si", "ucomiss", "ucomisd", "comiss", "comisd", "movmskps", "movmskpd", "sqrtps", "sqrtpd", "sqrtss", "sqrtsd",
	"andps", "andpd", "andnps", "andnpd", "orps", "orpd", "xorps", "xorpd", "addps", "addpd", "addss", "addsd", "mulps", "mulpd",
	"mulss", "mulsd", "cvtps2pd", "cvtpd2ps", "cvtss2sd", "cvtsd2ss", "cvtdq2ps", "cvtps2dq", "cvttps2dq", "subps", "subpd", "subss",
	"subsd", "minps", "minpd", "minss", "minsd", "divps", "divpd", "divss", "divsd", "maxps", "maxpd", "maxss", "maxsd", "punpcklbw",
	"punpcklwd", "punpckldq", "packsswb", "pcmpgtb", "pcmpgtw", "pcmpgtd", "packuswb", "punpckhbw", "punpckhwd", "punpckhdq",
	"packssdw", "punpcklqdq", "punpckhqdq", "movdqa", "movdqu", "pshufd", "pshufhw", "pshuflw", "psrlw", "psraw", "psllw", "psrld",
	"psrad", "pslld", "psrlq", "psrldq", "psllq", "pslldq", "pcmpeqb", "pcmpeqw", "pcmpeqd", "ldmxcsr", "stmxcsr", "cmpps", "cmppd",
	"cmpss", "cmpsd", "shufps", "shufpd", "paddq", "pmullw", "pmovmskb", "psubusb", "psubusw", "pminub", "pand", "paddusb", "paddusw",
	"pmaxub", "pandn", "pavgb", "pavgw", "pmulhuw", "pmulhw", "cvttpd2dq", "cvtdq2pd", "cvtpd2dq", "movntdq", "psubsb", "psubsw",
	"pminsw", "por", "paddsb", "paddsw", "pmaxsw", "pxor", "pmuludq", "pmaddwd", "psadbw", "psubb", "psubw", "psubd", "psubq",
	"paddb", "paddw", "paddd",
	"addsubps", "addsubpd", "haddps", "haddpd", "hsubps", "hsubpd", "movddup", "movsldup", "movshdup", "lddqu", "pinsrw", "pextrw",
	"pshufb", "phaddw", "phaddd", "phaddsw", "pmaddubsw", "phsubw", "phsubd", "phsubsw", "psignb", "psignw", "psignd", "pmulhrsw",
	"pabsb", "pabsw", "pabsd", "palignr", "pblendvb", "blendvps", "blendvpd", "ptest", "pmovsxbw", "pmovsxbd", "pmovsxbq", "pmovsxwd",
	"pmovsxwq", "pmovsxdq", "pmuldq", "pcmpeqq", "movntdqa", "packusdw", "pmovzxbw", "pmovzxbd", "pmovzxbq", "pmovzxwd", "pmovzxwq",
	"pmovzxdq", "pcmpgtq", "pminsb", "pminsd", "pminuw", "pminud", "pmaxsb", "pmaxsd", "pmaxuw", "pmaxud", "pmulld", "phminposuw",
	"roundps", "roundpd", "roundss", "roundsd", "blendps", "blendpd", "pblendw", "pextrb", "pextrd", "pextrq", "extractps", "pinsrb",
	"insertps", "pinsrd", "pinsrq", "dpps", "dppd", "mpsadbw", "pcmpestrm", "pcmpestri", "pcmpistrm", "pcmpistri"};
static_assert(std::size(sse_names) == static_cast<size_t>(sse_last) - static_cast<size_t>(sse_first) + 1);

static const char *const x87_names[] = {
	"fadd", "fmul", "fcom", "fcomp", "fsub", "fsubr", "fdiv", "fdivr", "fld", "fst", "fstp", "fldenv", "fldcw", "fnstenv", "fnstcw",
	"fxch", "fnop", "fchs", "fabs", "ftst", "fxam", "fld1", "fldl2t", "fldl2e", "fldpi", "fldlg2", "fldln2", "fldz", "f2xm1", "fyl2x",
	"fptan", "fpatan", "fxtract", "fprem1", "fdecstp", "fincstp", "fprem", "fyl2xp1", "fsqrt", "fsincos", "frndint", "fscale", "fsin",
	"fcos", "fiadd", "fimul", "ficom", "ficomp", "fisub", "fisubr", "fidiv", "fidivr", "fcmovb", "fcmove", "fcmovbe", "fcmovu",
	"fucompp", "fild", "fisttp", "fist", "fistp", "fcmovnb", "fcmovne", "fcmovnbe", "fcmovnu", "fnclex", "fninit", "fucomi", "fcomi",
	"frstor", "fnsave", "fnstsw", "ffree", "fucom", "fucomp", "faddp", "fmulp", "fcompp", "fsubrp", "fsubp", "fdivrp", "fdivp",
	"ffreep", "fbld", "fbstp", "fucompi", "fcompi", "wait"};
static_assert(std::size(x87_names) == static_cast<size_t>(x87_last) - static_cast<size_t>(x87_first) + 1);

static const char *const avx_names[] = {
	"vzeroupper", "vzeroall", "vbroadcastss", "vbroadcastsd", "vbroadcastf128", "vpbroadcastb", "vpbroadcastw", "vpbroadcastd",
	"vpbroadcastq", "vinsertf128", "vextractf128", "vinserti128", "vextracti128", "vperm2f128", "vperm2i128", "vfmadd132ps",
	"vfmadd132pd", "vfmadd132ss", "vfmadd132sd", "vfmadd213ps", "vfmadd213pd", "vfmadd213ss", "vfmadd213sd", "vfmadd231ps",
	"vfmadd231pd", "vfmadd231ss", "vfmadd231sd", "vfmsub132ps", "vfmsub132pd", "vfmsub132ss", "vfmsub132sd", "vfmsub213ps",
	"vfmsub213pd", "vfmsub213ss", "vfmsub213sd", "vfmsub231ps", "vfmsub231pd", "vfmsub231ss", "vfmsub231sd", "vfnmadd132ps",
	"vfnmadd132pd", "vfnmadd132ss", "vfnmadd132sd", "vfnmadd213ps", "vfnmadd213pd", "vfnmadd213ss", "vfnmadd213sd", "vfnmadd231ps",
	"vfnmadd231pd", "vfnmadd231ss", "vfnmadd231sd", "vfnmsub132ps", "vfnmsub132pd", "vfnmsub132ss", "vfnmsub132sd", "vfnmsub213ps",
	"vfnmsub213pd", "vfnmsub213ss", "vfnmsub213sd", "vfnmsub231ps", "vfnmsub231pd", "vfnmsub231ss", "vfnmsub231sd",
};
static_assert(std::size(avx_names) == static_cast<size_t>(avx_last) - static_cast<size_t>(avx_first) + 1);

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
	case Mnemonic::cpuid: name = "cpuid"; break;
	case Mnemonic::xgetbv: name = "xgetbv"; break;
	case Mnemonic::crc32: name = "crc32"; break;
	case Mnemonic::popcnt: name = "popcnt"; break;
	default: {
		if (instruction.mnemonic >= x87_first && instruction.mnemonic <= x87_last) {
			name = x87_names[static_cast<size_t>(instruction.mnemonic) - static_cast<size_t>(x87_first)];
			break;
		}
		if (instruction.mnemonic >= avx_first && instruction.mnemonic <= avx_last) {
			name = avx_names[static_cast<size_t>(instruction.mnemonic) - static_cast<size_t>(avx_first)];
			break;
		}
		static const char *const predicates[] = {"eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord", "eq_uq", "nge", "ngt", "false",
			"neq_oq", "ge", "gt", "true", "eq_os", "lt_oq", "le_oq", "unord_s", "neq_us", "nlt_uq", "nle_uq", "ord_s", "eq_us", "nge_uq",
			"ngt_uq", "false_os", "neq_os", "ge_oq", "gt_oq", "true_us"};
		static const char *const suffixes[] = {"ps", "pd", "ss", "sd"};
		const auto index = static_cast<size_t>(instruction.mnemonic) - static_cast<size_t>(sse_first);
		name = std::string(instruction.vex ? "v" : "") + sse_names[index];
		// cmpps, cmppd, cmpss and cmpsd with a predicate SSE (0-7) or AVX (0-31) defines print as LLVM's pseudo-instructions.
		const uint8_t last = instruction.vex ? 3 : 2;
		if (instruction.mnemonic >= Mnemonic::cmpps && instruction.mnemonic <= Mnemonic::cmpsd &&
			instruction.operands[last].value < (instruction.vex ? 32 : 8)) {
			const auto kind = static_cast<size_t>(instruction.mnemonic) - static_cast<size_t>(Mnemonic::cmpps);
			name = std::string(instruction.vex ? "vcmp" : "cmp") + predicates[instruction.operands[last].value] + suffixes[kind];
			Instruction shown = instruction;
			shown.count = last;
			shown.mnemonic = Mnemonic::movaps;
			shown.vex = false;
			std::string text = format_intel(shown);
			return name + text.substr(text.find(' '));
		}
		break;
	}
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
