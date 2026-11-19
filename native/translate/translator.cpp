#include "translator.h"

#include "a64_emitter.h"
#include "cpu_state.h"
#include "x86_decoder.h"
#include "string_compare.h"
#include "x87.h"

#include <array>
#include <stdexcept>

// x64 to AArch64, one basic block at a time.
//
// Registers. The x64 registers stay in host registers for the whole block, following Microsoft's ARM64EC mapping
// (https://learn.microsoft.com/en-us/windows/arm/arm64ec-abi) so that a call between translated and native code needs no shuffling:
// RCX, RDX, R8 and R9 are already the first four AAPCS64 argument registers, and every x64 callee-saved register sits in an AAPCS64
// callee-saved one. Two registers differ from ARM64EC. RSP lives in x28, not sp: x64 code leaves RSP at 8 modulo 16 and the host faults
// on any access through a misaligned sp. RBP lives in x24, not x29: macOS requires x29 to point at a valid frame record at all times.
// Both are among the registers ARM64EC forbids to native code (x13, x14, x23, x24, x28), which leaves them to the emulator.
//
//   rax x8   rcx x0   rdx x1   rbx x27  rsp x28  rbp x24  rsi x25  rdi x26
//   r8  x2   r9  x3   r10 x4   r11 x5   r12 x19  r13 x20  r14 x21  r15 x22
//
// x23 points at the CpuState. x13 and x14 hold the sources of PF and AF. x7 holds the block's last RIP-relative address. x6, x9-x12
// and x15-x17 are scratch. x18 is never touched: it
// belongs to macOS, and translated code has no use for it since gs: and fs: come from CpuState. x29 and x30 stay the host's.
//
// Flags. SF, ZF and OF are N, Z and V. CF is C, but whether as is or inverted depends on the last operation (SUBS leaves C as the
// inverse of a borrow): the translator tracks it per instruction and keeps C inverted at every block boundary, the form compares
// leave. PF and AF are computed only when read, from x13 and x14, which are written only when a later reader may exist. Flags no later
// instruction reads are not computed at all; at the end of a block every flag counts as read.
namespace raven::translate {

namespace {

using a64::Assembler;
using a64::Cond;
using a64::Shift;
namespace simd = a64::simd;

constexpr std::array<uint8_t, 16> host_register = {8, 0, 1, 27, 28, 24, 25, 26, 2, 3, 4, 5, 19, 20, 21, 22};
constexpr uint8_t state_register = 23;
constexpr uint8_t parity_register = 13;
constexpr uint8_t adjust_register = 14;
constexpr uint8_t s0 = 9, s1 = 10, s2 = 11, s3 = 12, s4 = 15, address_register = 16, s5 = 17;
// Holds the last RIP-relative address the block computed, so a loop over one global computes it once.
constexpr uint8_t constant_register = 7;
// XMMn is vn; v16-v31 are scratch.
constexpr uint8_t vs0 = 16, vs1 = 17, vs2 = 18, vs3 = 19;
// For AVX, around the SSE handlers (which use vs0-vs3): upper halves and the merging forms' working copy.
constexpr uint8_t vh0 = 20, vh1 = 21, vh2 = 22, vh3 = 23;
// Scratch outside s0-s5, for a value kept across code that uses those.
constexpr uint8_t s6 = 6;

constexpr uint32_t CF = 1, PF = 4, AF = 16, ZF = 64, SF = 128, OF = 2048;
constexpr uint32_t all_flags = CF | PF | AF | ZF | SF | OF;

uint32_t condition_flags(uint8_t condition)
{
	switch (condition >> 1) {
	case 0: return OF;
	case 1: return CF;
	case 2: return ZF;
	case 3: return CF | ZF;
	case 4: return SF;
	case 5: return PF;
	case 6: return SF | OF;
	default: return ZF | SF | OF;
	}
}

unsigned shift_count(const Instruction &instruction)
{
	const unsigned mask = instruction.operands[0].size == 8 ? 63 : 31;
	return static_cast<unsigned>(instruction.operands[1].value) & mask;
}

struct FlagEffect {
	uint32_t defines = 0;
	uint32_t uses = 0;
};

FlagEffect flag_effect(const Instruction &instruction)
{
	switch (instruction.mnemonic) {
	case Mnemonic::add: case Mnemonic::sub: case Mnemonic::cmp: case Mnemonic::neg: case Mnemonic::and_: case Mnemonic::or_:
	case Mnemonic::xor_: case Mnemonic::test: case Mnemonic::cmpxchg: case Mnemonic::xadd: case Mnemonic::imul:
		return {all_flags, 0};
	case Mnemonic::adc: case Mnemonic::sbb: return {all_flags, CF};
	case Mnemonic::inc: case Mnemonic::dec: return {all_flags & ~CF, 0};
	case Mnemonic::shl: case Mnemonic::shr: case Mnemonic::sar:
		if (instruction.operands[1].kind == OperandKind::imm)
			return {shift_count(instruction) ? all_flags : 0, 0};
		// A count of zero leaves every flag as it was.
		return {0, all_flags};
	case Mnemonic::jcc: case Mnemonic::setcc: case Mnemonic::cmovcc: return {0, condition_flags(instruction.condition)};
	case Mnemonic::comiss: case Mnemonic::comisd: case Mnemonic::ucomiss: case Mnemonic::ucomisd: case Mnemonic::ptest:
	case Mnemonic::popcnt:
		return {all_flags, 0};
	default: return {};
	}
}

bool ends_block(Mnemonic mnemonic)
{
	switch (mnemonic) {
	case Mnemonic::jmp: case Mnemonic::jcc: case Mnemonic::call: case Mnemonic::ret: case Mnemonic::int3: case Mnemonic::syscall:
	case Mnemonic::hlt: case Mnemonic::ud2:
		return true;
	default: return false;
	}
}

// Whether the translator has code for the instruction; the block ends before any it has not.
bool supported(const Instruction &instruction);

// VEX forms of SSE to SSE4.2. Unary: the destination is not a source (and pshufd, pshufhw, pshuflw, roundps, roundpd, pextr* and
// extractps, whose imm8 follows the one source). Otherwise operands[1] is the VEX.vvvv source the destination starts from and the rest are the legacy form's sources.
bool vex_unary(const Instruction &instruction)
{
	switch (instruction.mnemonic) {
	case Mnemonic::pshufd: case Mnemonic::pshufhw: case Mnemonic::pshuflw: case Mnemonic::roundps: case Mnemonic::roundpd:
	case Mnemonic::pextrb: case Mnemonic::pextrw: case Mnemonic::pextrd: case Mnemonic::pextrq: case Mnemonic::extractps:
		return true;
	default: return instruction.count == 2;
	}
}

// The legacy SSE form a VEX one runs as, with target as its destination (and first source, for the merging forms).
Instruction legacy_form(const Instruction &instruction, uint8_t target)
{
	Instruction legacy = instruction;
	legacy.vex = false;
	legacy.vector_size = 16;
	for (Operand &operand : legacy.operands)
		if (operand.kind == OperandKind::xmm || (operand.kind == OperandKind::mem && operand.size == 32))
			operand.size = 16;
	if (!vex_unary(instruction)) {
		legacy.operands = {instruction.operands[0], instruction.operands[2], instruction.operands[3], Operand{}};
		legacy.count = static_cast<uint8_t>(instruction.count - 1);
		for (Operand &operand : legacy.operands)
			if (operand.kind == OperandKind::xmm || (operand.kind == OperandKind::mem && operand.size == 32))
				operand.size = 16;
	}
	if (legacy.operands[0].kind == OperandKind::xmm)
		legacy.operands[0].reg = target;
	return legacy;
}

bool avx_supported(const Instruction &instruction)
{
	const Mnemonic mnemonic = instruction.mnemonic;
	if (mnemonic >= avx_first && mnemonic <= avx_last)
		return true;
	if (mnemonic < sse_first || mnemonic > sse_last)
		return false;
	// The AVX predicates 8-31 of vcmpps and the others are not translated yet.
	if (mnemonic >= Mnemonic::cmpps && mnemonic <= Mnemonic::cmpsd && instruction.operands[instruction.count - 1].value >= 8)
		return false;
	const Instruction legacy = legacy_form(instruction, instruction.operands[0].reg);
	return supported(legacy);
}

bool supported(const Instruction &instruction)
{
	// cs:, ds:, es: and ss: change nothing in 64-bit mode (compilers pad with cs: nops); fs: and gs: add their base.
	if (instruction.rep || instruction.repne)
		return false;
	if (instruction.vex)
		return avx_supported(instruction);
	const Operand &first = instruction.operands[0];
	const uint8_t size = first.size;
	switch (instruction.mnemonic) {
	case Mnemonic::add: case Mnemonic::sub: case Mnemonic::cmp: case Mnemonic::and_: case Mnemonic::or_: case Mnemonic::xor_:
	case Mnemonic::test: case Mnemonic::inc: case Mnemonic::dec: case Mnemonic::neg:
		return !instruction.lock;
	case Mnemonic::adc: case Mnemonic::sbb: return !instruction.lock && size >= 4;
	case Mnemonic::not_: case Mnemonic::mov: case Mnemonic::movzx: case Mnemonic::movsx: case Mnemonic::movsxd: case Mnemonic::lea:
	case Mnemonic::setcc: case Mnemonic::nop: case Mnemonic::cbw: case Mnemonic::cwd: case Mnemonic::leave: case Mnemonic::int3:
	case Mnemonic::syscall: case Mnemonic::hlt: case Mnemonic::ud2: case Mnemonic::jcc: case Mnemonic::jmp: case Mnemonic::call:
	case Mnemonic::ret:
		return !instruction.lock;
	case Mnemonic::cmovcc: return (instruction.condition >> 1) != 5;
	case Mnemonic::push: case Mnemonic::pop: return instruction.operand_size == 8;
	case Mnemonic::xchg: return true;
	case Mnemonic::movq: return !instruction.lock;
	case Mnemonic::movd: case Mnemonic::movups: case Mnemonic::movupd: case Mnemonic::movaps: case Mnemonic::movapd:
	case Mnemonic::movdqa: case Mnemonic::movdqu: case Mnemonic::movntps: case Mnemonic::movntpd: case Mnemonic::movntdq:
	case Mnemonic::movss: case Mnemonic::movsd: case Mnemonic::movlps: case Mnemonic::movlpd: case Mnemonic::movhps:
	case Mnemonic::movhpd: case Mnemonic::movhlps: case Mnemonic::movlhps: case Mnemonic::movmskps: case Mnemonic::movmskpd:
	case Mnemonic::pmovmskb: case Mnemonic::andps: case Mnemonic::andpd: case Mnemonic::andnps: case Mnemonic::andnpd:
	case Mnemonic::orps: case Mnemonic::orpd: case Mnemonic::xorps: case Mnemonic::xorpd: case Mnemonic::pand: case Mnemonic::pandn:
	case Mnemonic::por: case Mnemonic::pxor: case Mnemonic::addps: case Mnemonic::addpd: case Mnemonic::addss: case Mnemonic::addsd:
	case Mnemonic::subps: case Mnemonic::subpd: case Mnemonic::subss: case Mnemonic::subsd: case Mnemonic::mulps: case Mnemonic::mulpd:
	case Mnemonic::mulss: case Mnemonic::mulsd: case Mnemonic::divps: case Mnemonic::divpd: case Mnemonic::divss: case Mnemonic::divsd:
	case Mnemonic::sqrtps: case Mnemonic::sqrtpd: case Mnemonic::sqrtss: case Mnemonic::sqrtsd: case Mnemonic::minps:
	case Mnemonic::minpd: case Mnemonic::minss: case Mnemonic::minsd: case Mnemonic::maxps: case Mnemonic::maxpd: case Mnemonic::maxss:
	case Mnemonic::maxsd: case Mnemonic::ldmxcsr: case Mnemonic::stmxcsr: case Mnemonic::cmpps: case Mnemonic::cmppd:
	case Mnemonic::cmpss: case Mnemonic::cmpsd: case Mnemonic::comiss: case Mnemonic::comisd: case Mnemonic::ucomiss:
	case Mnemonic::ucomisd: case Mnemonic::cvtsi2ss: case Mnemonic::cvtsi2sd: case Mnemonic::cvtss2si: case Mnemonic::cvttss2si:
	case Mnemonic::cvtsd2si: case Mnemonic::cvttsd2si: case Mnemonic::cvtss2sd: case Mnemonic::cvtsd2ss: case Mnemonic::cvtdq2ps:
	case Mnemonic::cvtdq2pd: case Mnemonic::cvtps2dq: case Mnemonic::cvttps2dq: case Mnemonic::cvtpd2dq: case Mnemonic::cvttpd2dq:
	case Mnemonic::cvtps2pd: case Mnemonic::cvtpd2ps: case Mnemonic::paddb: case Mnemonic::paddw: case Mnemonic::paddd:
	case Mnemonic::paddq: case Mnemonic::psubb: case Mnemonic::psubw: case Mnemonic::psubd: case Mnemonic::psubq: case Mnemonic::paddsb:
	case Mnemonic::paddsw: case Mnemonic::paddusb: case Mnemonic::paddusw: case Mnemonic::psubsb: case Mnemonic::psubsw:
	case Mnemonic::psubusb: case Mnemonic::psubusw: case Mnemonic::pmullw: case Mnemonic::pmulhw: case Mnemonic::pmulhuw:
	case Mnemonic::pmuludq: case Mnemonic::pmaddwd: case Mnemonic::pcmpeqb: case Mnemonic::pcmpeqw: case Mnemonic::pcmpeqd:
	case Mnemonic::pcmpgtb: case Mnemonic::pcmpgtw: case Mnemonic::pcmpgtd: case Mnemonic::pavgb: case Mnemonic::pavgw:
	case Mnemonic::pminub: case Mnemonic::pmaxub: case Mnemonic::pminsw: case Mnemonic::pmaxsw: case Mnemonic::psadbw:
	case Mnemonic::psllw: case Mnemonic::pslld: case Mnemonic::psllq: case Mnemonic::psrlw: case Mnemonic::psrld: case Mnemonic::psrlq:
	case Mnemonic::psraw: case Mnemonic::psrad: case Mnemonic::psrldq: case Mnemonic::pslldq: case Mnemonic::packsswb:
	case Mnemonic::packssdw: case Mnemonic::packuswb: case Mnemonic::punpcklbw: case Mnemonic::punpcklwd: case Mnemonic::punpckldq:
	case Mnemonic::punpcklqdq: case Mnemonic::punpckhbw: case Mnemonic::punpckhwd: case Mnemonic::punpckhdq: case Mnemonic::punpckhqdq:
	case Mnemonic::unpcklps: case Mnemonic::unpcklpd: case Mnemonic::unpckhps: case Mnemonic::unpckhpd: case Mnemonic::pshufd:
	case Mnemonic::pshuflw: case Mnemonic::pshufhw: case Mnemonic::shufps: case Mnemonic::shufpd:
	case Mnemonic::addsubps: case Mnemonic::addsubpd: case Mnemonic::haddps: case Mnemonic::haddpd: case Mnemonic::hsubps:
	case Mnemonic::hsubpd: case Mnemonic::movddup: case Mnemonic::movsldup: case Mnemonic::movshdup: case Mnemonic::lddqu:
	case Mnemonic::pshufb: case Mnemonic::phaddw: case Mnemonic::phaddd: case Mnemonic::phaddsw: case Mnemonic::pmaddubsw:
	case Mnemonic::phsubw: case Mnemonic::phsubd: case Mnemonic::phsubsw: case Mnemonic::psignb: case Mnemonic::psignw:
	case Mnemonic::psignd: case Mnemonic::pmulhrsw: case Mnemonic::pabsb: case Mnemonic::pabsw: case Mnemonic::pabsd:
	case Mnemonic::palignr:
	case Mnemonic::pblendvb: case Mnemonic::blendvps: case Mnemonic::blendvpd: case Mnemonic::ptest: case Mnemonic::pmovsxbw:
	case Mnemonic::pmovsxbd: case Mnemonic::pmovsxbq: case Mnemonic::pmovsxwd: case Mnemonic::pmovsxwq: case Mnemonic::pmovsxdq:
	case Mnemonic::pmovzxbw: case Mnemonic::pmovzxbd: case Mnemonic::pmovzxbq: case Mnemonic::pmovzxwd: case Mnemonic::pmovzxwq:
	case Mnemonic::pmovzxdq: case Mnemonic::pmuldq: case Mnemonic::pcmpeqq: case Mnemonic::movntdqa: case Mnemonic::packusdw:
	case Mnemonic::pminsb: case Mnemonic::pminsd: case Mnemonic::pminuw: case Mnemonic::pminud: case Mnemonic::pmaxsb:
	case Mnemonic::pmaxsd: case Mnemonic::pmaxuw: case Mnemonic::pmaxud: case Mnemonic::pmulld: case Mnemonic::phminposuw:
	case Mnemonic::roundps: case Mnemonic::roundpd: case Mnemonic::roundss: case Mnemonic::roundsd: case Mnemonic::blendps:
	case Mnemonic::blendpd: case Mnemonic::pblendw: case Mnemonic::pextrb: case Mnemonic::pextrw: case Mnemonic::pextrd:
	case Mnemonic::pextrq: case Mnemonic::extractps: case Mnemonic::pinsrb: case Mnemonic::pinsrw: case Mnemonic::pinsrd:
	case Mnemonic::pinsrq: case Mnemonic::insertps: case Mnemonic::dpps: case Mnemonic::dppd: case Mnemonic::mpsadbw:
	case Mnemonic::pcmpgtq: case Mnemonic::crc32: case Mnemonic::popcnt:
		return !instruction.lock;
	case Mnemonic::cmpxchg: case Mnemonic::xadd: return first.kind == OperandKind::mem;
	case Mnemonic::imul: return instruction.count >= 2 && size >= 4;
	case Mnemonic::shl: case Mnemonic::shr: case Mnemonic::sar:
		// Narrow shifts only by a count inside their width: beyond it CF is undefined and the result needs its own code.
		return size >= 4 || (instruction.operands[1].kind == OperandKind::imm && shift_count(instruction) < size * 8u);
	default: return false;
	}
}

// MXCSR and the Arm floating-point registers. FPCR: RC to RMode (x86's 01, toward -inf, is Arm's 10, and 10 is 01), FZ to FZ, DAZ to
// FIZ, and AH (FEAT_AFP), with which NaN propagation, min and max and the default NaN follow x86 instead of Arm. FPSR: IE, ZE, OE, UE
// and PE are IOC, DZC, OFC, UFC and IXC.
void fpcr_from_mxcsr(Assembler &a, uint8_t fpcr, uint8_t mxcsr, uint8_t scratch)
{
	a.mov_imm(fpcr, 2, false);
	a.ubfx(scratch, mxcsr, 13, 1, false);
	a.bfi(fpcr, scratch, 23, 1, false);
	a.ubfx(scratch, mxcsr, 14, 1, false);
	a.bfi(fpcr, scratch, 22, 1, false);
	a.ubfx(scratch, mxcsr, 15, 1, false);
	a.bfi(fpcr, scratch, 24, 1, false);
	a.ubfx(scratch, mxcsr, 6, 1, false);
	a.bfi(fpcr, scratch, 0, 1, false);
}

void fpsr_from_mxcsr(Assembler &a, uint8_t fpsr, uint8_t mxcsr, uint8_t scratch)
{
	a.and_imm(fpsr, mxcsr, 1, false);
	a.ubfx(scratch, mxcsr, 2, 4, false);
	a.bfi(fpsr, scratch, 1, 4, false);
}

// The flags of mxcsr from fpsr; DE and the control bits stay.
void mxcsr_flags_from_fpsr(Assembler &a, uint8_t mxcsr, uint8_t fpsr, uint8_t scratch)
{
	a.bfi(mxcsr, fpsr, 0, 1, false);
	a.ubfx(scratch, fpsr, 1, 4, false);
	a.bfi(mxcsr, scratch, 2, 4, false);
}

// FPCR and FPSR from CpuState::mxcsr.
void load_mxcsr(Assembler &a, uint8_t t0, uint8_t t1, uint8_t t2)
{
	a.ldr(t0, state_register, state_offset::mxcsr, 4);
	fpcr_from_mxcsr(a, t1, t0, t2);
	a.msr_fpcr(t1);
	fpsr_from_mxcsr(a, t1, t0, t2);
	a.msr_fpsr(t1);
}

// FPSR's flags into CpuState::mxcsr.
void store_mxcsr_flags(Assembler &a, uint8_t t0, uint8_t t1, uint8_t t2)
{
	a.ldr(t0, state_register, state_offset::mxcsr, 4);
	a.mrs_fpsr(t1);
	mxcsr_flags_from_fpsr(a, t0, t1, t2);
	a.str(t0, state_register, state_offset::mxcsr, 4);
}

void store_vectors(Assembler &a, uint8_t first = 0)
{
	for (uint8_t reg = first; reg < 16; reg += 2)
		a.stp_q(reg, uint8_t(reg + 1), state_register, static_cast<int16_t>(state_offset::xmm + 16 * reg));
}

void load_vectors(Assembler &a, uint8_t first = 0)
{
	for (uint8_t reg = first; reg < 16; reg += 2)
		a.ldp_q(reg, uint8_t(reg + 1), state_register, static_cast<int16_t>(state_offset::xmm + 16 * reg));
}

struct Address {
	uint8_t base;
	int64_t offset;
};

bool fits_offset(int64_t offset, unsigned size)
{
	if (offset >= -256 && offset < 256)
		return true;
	return offset >= 0 && (offset % size) == 0 && offset / size < 4096;
}

class BlockTranslator {
public:
	BlockTranslator(const TranslatorOptions &options, TranslationStats &stats) : options(options), stats(stats) {}

	TranslatedBlock run(std::span<const uint8_t> code, uint64_t address);

private:
	const TranslatorOptions &options;
	TranslationStats &stats;
	Assembler a;
	TranslatedBlock block;
	// Whether host C holds the inverse of CF.
	bool carry_inverted = true;
	const Instruction *current = nullptr;
	uint64_t next_address = 0;
	uint32_t live_after = all_flags;
	bool constant_valid = false;
	uint64_t constant_value = 0;
	// Set by address(): the access goes to a constant address, whose alignment the translator knows.
	bool constant_address = false;
	uint64_t constant_address_value = 0;

	struct PendingExit {
		Assembler::Label stub;
		Assembler::Label site;
		uint64_t target;
		size_t word;
	};
	std::vector<PendingExit> pending;
	// Out-of-line code for an ordered access whose address is not aligned: LDAPR and STLR fault on those, x86 does not.
	struct Unaligned {
		Assembler::Label start;
		Assembler::Label back;
		uint8_t reg;
		uint8_t base;
		unsigned size;
		bool store;
	};
	std::vector<Unaligned> unaligned;

	void translate(const Instruction &instruction);
	bool flags_wanted(uint32_t flags) const { return (live_after & flags) != 0; }

	// Operands.
	Address address(const Operand &memory, bool plain_base);
	void effective_address(uint8_t target, const Operand &memory, bool is64);
	bool ordered(const Operand &memory) const;
	void load_memory(uint8_t target, const Operand &memory, unsigned size);
	void store_memory(uint8_t source, const Operand &memory, unsigned size);
	// The operand's value, zero-extended, in target (or, for a register of 4 or 8 bytes, the register itself when allowed).
	uint8_t read(const Operand &operand, uint8_t target, bool may_alias = true);
	void write(const Operand &operand, uint8_t source);
	void read_signed(const Operand &operand, uint8_t target, unsigned to_size);

	// Flags.
	void normalize_carry();
	Cond condition(uint8_t condition);

	// Instructions.
	void arithmetic(const Instruction &instruction);
	void unary(const Instruction &instruction);
	void shift(const Instruction &instruction);
	void multiply(const Instruction &instruction);
	void exchange(const Instruction &instruction);
	void push_value(uint8_t value);
	void pop_value(uint8_t target);

	// SSE: XMMn is vn.
	void load_vector(uint8_t target, const Operand &memory, unsigned size);
	void store_vector(uint8_t source, const Operand &memory, unsigned size);
	// An XMM operand's register, or the register its memory was loaded into (scratch).
	uint8_t vector_source(const Operand &operand, uint8_t scratch);
	void sse_move(const Instruction &instruction);
	void sse_mask(const Instruction &instruction);
	void sse_logic(const Instruction &instruction);
	void sse_arithmetic(const Instruction &instruction);
	void sse_compare(const Instruction &instruction);
	void sse_ordered_compare(const Instruction &instruction);
	// Element 0 of source (a double or a single) to a signed integer of size bytes in result (s0), truncated or rounded as MXCSR says.
	void convert_to_integer(uint8_t source, bool doubles, bool truncate, unsigned size);
	void sse_convert(const Instruction &instruction);
	void sse_integer(const Instruction &instruction);
	void sse_shift(const Instruction &instruction);
	void sse_shuffle(const Instruction &instruction);
	void sse3(const Instruction &instruction);
	void ssse3(const Instruction &instruction);
	void sse41(const Instruction &instruction);

	// AVX: the low 128 bits of YMMn are vn, the upper halves stay in CpuState::ymm_high (v20-v23 hold them while an instruction
	// works on them; the SSE handlers use v16-v19).
	void load_high(uint8_t target, uint8_t ymm) { a.ldr_v(target, state_register, int64_t(state_offset::ymm_high + 16 * ymm), 16); }
	void store_high(uint8_t source, uint8_t ymm) { a.str_v(source, state_register, int64_t(state_offset::ymm_high + 16 * ymm), 16); }
	void clear_high(uint8_t ymm);
	// The upper half of an operand of 32 bytes into target: from CpuState for a register, from 16 bytes further on for memory.
	uint8_t high_source(const Operand &operand, uint8_t target);
	void avx(const Instruction &instruction);
	void avx_mask(const Instruction &instruction);
	void avx_test(const Instruction &instruction);
	void test_flags();
	void avx_lanes(const Instruction &instruction);
	void avx_fma(const Instruction &instruction);

	// Exits.
	void exit_with(ExitReason reason, uint64_t rip);
	void exit_to(uint64_t target);
	void exit_indirect();
	void finish_exits();
	void ordered_access(uint8_t reg, uint8_t base, unsigned size, bool store);
};

bool BlockTranslator::ordered(const Operand &memory) const
{
	if (options.ordering == MemoryOrdering::hardware)
		return false;
	const bool stack = memory.base == rsp && memory.index == no_register && !memory.rip_relative && memory.segment == Segment::none;
	return options.order_stack || !stack;
}

Address BlockTranslator::address(const Operand &memory, bool plain_base)
{
	const bool truncate = current->address_size == 4;
	uint8_t base = a64::zr;
	int64_t offset = memory.value;
	constant_address = false;
	if (memory.rip_relative) {
		const uint64_t value = next_address + static_cast<uint64_t>(memory.value);
		if (!constant_valid || constant_value != value) {
			a.mov_imm(constant_register, value);
			constant_valid = true;
			constant_value = value;
		}
		constant_address = !truncate && memory.index == no_register && memory.segment == Segment::none;
		constant_address_value = value;
		base = constant_register;
		offset = 0;
	} else if (memory.base != no_register)
		base = host_register[memory.base];
	if (memory.index != no_register) {
		const uint8_t shift_amount = static_cast<uint8_t>(__builtin_ctz(memory.scale));
		a.add(address_register, base, host_register[memory.index], Shift::lsl, shift_amount);
		base = address_register;
	}
	if (memory.segment == Segment::gs || memory.segment == Segment::fs) {
		a.ldr(s5, state_register, memory.segment == Segment::gs ? state_offset::gs_base : state_offset::fs_base, 8);
		a.add(address_register, base == a64::zr ? s5 : base, base == a64::zr ? a64::zr : s5);
		base = address_register;
	}
	if (base == a64::zr || truncate || (plain_base && offset != 0)) {
		// Everything into x16: an absolute address, a 32-bit address or an access that takes a base register alone.
		if (base == a64::zr) {
			a.mov_imm(address_register, static_cast<uint64_t>(offset));
		} else if (offset > 0 && offset < 4096) {
			a.add_imm(address_register, base, static_cast<uint32_t>(offset));
		} else if (offset < 0 && offset > -4096) {
			a.sub_imm(address_register, base, static_cast<uint32_t>(-offset));
		} else if (offset != 0) {
			a.mov_imm(s5, static_cast<uint64_t>(offset));
			a.add(address_register, base, s5);
		} else if (base != address_register)
			a.mov(address_register, base);
		if (truncate)
			a.mov(address_register, address_register, false);
		return {address_register, 0};
	}
	return {base, offset};
}

void BlockTranslator::effective_address(uint8_t target, const Operand &memory, bool is64)
{
	// lea: the address without its segment base.
	Operand plain = memory;
	plain.segment = Segment::none;
	const Address computed = address(plain, true);
	a.mov(target, computed.base, is64);
}

void BlockTranslator::load_memory(uint8_t target, const Operand &memory, unsigned size)
{
	const bool order = ordered(memory);
	if (order && options.ordering == MemoryOrdering::rcpc) {
		const Address at = address(memory, true);
		ordered_access(target, at.base, size, false);
		return;
	}
	Address at = address(memory, false);
	if (!fits_offset(at.offset, size)) {
		a.mov_imm(s5, static_cast<uint64_t>(at.offset));
		a.add(address_register, at.base, s5);
		at = {address_register, 0};
	}
	a.ldr(target, at.base, at.offset, size);
	if (order) {
		a.dmb(a64::Barrier::ishld);
		++stats.barriers;
	} else
		++stats.plain_accesses;
}

void BlockTranslator::store_memory(uint8_t source, const Operand &memory, unsigned size)
{
	const bool order = ordered(memory);
	if (order && options.ordering == MemoryOrdering::rcpc) {
		const Address at = address(memory, true);
		ordered_access(source, at.base, size, true);
		return;
	}
	Address at = address(memory, false);
	if (!fits_offset(at.offset, size)) {
		a.mov_imm(s5, static_cast<uint64_t>(at.offset));
		a.add(address_register, at.base, s5);
		at = {address_register, 0};
	}
	if (order) {
		a.dmb(a64::Barrier::ishst);
		++stats.barriers;
	} else
		++stats.plain_accesses;
	a.str(source, at.base, at.offset, size);
}

void BlockTranslator::ordered_access(uint8_t reg, uint8_t base, unsigned size, bool store)
{
	const bool check = size > 1 && !(constant_address && base == constant_register && constant_address_value % size == 0);
	if (check) {
		// An address that is not a multiple of the size takes the out-of-line path: a plain access and a barrier. The test leaves
		// NZCV alone.
		Unaligned path{a.label(), a.label(), reg, base, size, store};
		a.and_imm(s5, base, size - 1);
		a.cbnz(s5, path.start);
		unaligned.push_back(path);
		++stats.alignment_checks;
	}
	if (store) {
		a.stlr(reg, base, size);
		++stats.ordered_stores;
	} else {
		a.ldapr(reg, base, size);
		++stats.ordered_loads;
	}
	if (check)
		a.bind(unaligned.back().back);
}

uint8_t BlockTranslator::read(const Operand &operand, uint8_t target, bool may_alias)
{
	switch (operand.kind) {
	case OperandKind::reg: {
		if (operand.reg >= high_byte_base) {
			a.ubfx(target, host_register[operand.reg - high_byte_base], 8, 8);
			return target;
		}
		const uint8_t reg = host_register[operand.reg];
		// A 4-byte value is used only by W-form instructions, which ignore the upper half: no copy is needed for those either.
		if (operand.size >= 4 && may_alias)
			return reg;
		if (operand.size == 8)
			a.mov(target, reg);
		else if (operand.size == 4)
			a.mov(target, reg, false);
		else
			a.ubfx(target, reg, 0, static_cast<uint8_t>(operand.size * 8), false);
		return target;
	}
	case OperandKind::mem: load_memory(target, operand, operand.size); return target;
	case OperandKind::imm: {
		uint64_t value = static_cast<uint64_t>(operand.value);
		if (operand.size < 8)
			value &= (1ull << (operand.size * 8)) - 1;
		a.mov_imm(target, value, operand.size == 8);
		return target;
	}
	default: throw std::logic_error("operand has no value");
	}
}

void BlockTranslator::write(const Operand &operand, uint8_t source)
{
	if (operand.kind == OperandKind::mem) {
		store_memory(source, operand, operand.size);
		return;
	}
	if (operand.reg >= high_byte_base) {
		a.bfi(host_register[operand.reg - high_byte_base], source, 8, 8);
		return;
	}
	const uint8_t reg = host_register[operand.reg];
	switch (operand.size) {
	case 8: if (reg != source) a.mov(reg, source); break;
	// A 32-bit write clears the upper half, as a W-register write does.
	case 4: a.mov(reg, source, false); break;
	default: a.bfi(reg, source, 0, static_cast<uint8_t>(operand.size * 8)); break;
	}
}

void BlockTranslator::read_signed(const Operand &operand, uint8_t target, unsigned to_size)
{
	const uint8_t value = read(operand, target, false);
	const bool is64 = to_size == 8;
	a.sbfx(target, value, 0, static_cast<uint8_t>(operand.size * 8), is64);
}

void BlockTranslator::normalize_carry()
{
	if (!carry_inverted) {
		a.cfinv();
		carry_inverted = true;
	}
}

Cond BlockTranslator::condition(uint8_t x86)
{
	const bool negate = x86 & 1;
	Cond cond;
	switch (x86 >> 1) {
	case 0: cond = Cond::vs; break;
	case 1: cond = carry_inverted ? Cond::cc : Cond::cs; break;
	case 2: cond = Cond::eq; break;
	case 3:
		// CF or ZF: only LS tests that, and only with C inverted.
		normalize_carry();
		cond = Cond::ls;
		break;
	case 4: cond = Cond::mi; break;
	case 6: cond = Cond::lt; break;
	case 7: cond = Cond::le; break;
	default: throw std::logic_error("parity has no host condition");
	}
	return negate ? a64::invert(cond) : cond;
}

void BlockTranslator::arithmetic(const Instruction &instruction)
{
	const Operand &target = instruction.operands[0];
	const Operand &source = instruction.operands[1];
	const Mnemonic mnemonic = instruction.mnemonic;
	const unsigned size = target.size;
	const bool is64 = size == 8;
	const bool flags = flags_wanted(all_flags);
	const bool writes = mnemonic != Mnemonic::cmp && mnemonic != Mnemonic::test;
	const bool logical = mnemonic == Mnemonic::and_ || mnemonic == Mnemonic::or_ || mnemonic == Mnemonic::xor_ ||
		mnemonic == Mnemonic::test;
	const uint8_t left = read(target, s0);
	// The result goes straight to a full-width destination register.
	const bool direct = writes && target.kind == OperandKind::reg && size >= 4 && target.reg < high_byte_base;
	const uint8_t result = direct ? host_register[target.reg] : s2;
	if (size >= 4 && source.kind == OperandKind::imm && source.value > -4096 && source.value < 4096 &&
		(mnemonic == Mnemonic::add || mnemonic == Mnemonic::sub || mnemonic == Mnemonic::cmp)) {
		// add, sub or cmp with a small immediate: one instruction, the opposite one for a negative immediate. Adding -n and
		// subtracting n give the same result, N, Z and V, and C keeps the sense of the x86 instruction either way: CF for add,
		// its inverse for sub and cmp (adding 2^w - n carries exactly when a >= n).
		const bool subtract = (mnemonic != Mnemonic::add) != (source.value < 0);
		const uint32_t amount = static_cast<uint32_t>(source.value < 0 ? -source.value : source.value);
		if (flags && flags_wanted(AF)) {
			const uint64_t operand = static_cast<uint64_t>(source.value);
			if (!a.eor_imm(adjust_register, left, operand)) {
				a.mov_imm(s1, operand);
				a.eor(adjust_register, left, s1);
			}
		}
		const uint8_t destination = writes ? result : a64::zr;
		if (subtract)
			a.sub_imm(destination, left, amount, false, is64, flags);
		else
			a.add_imm(destination, left, amount, false, is64, flags);
		if (flags) {
			carry_inverted = mnemonic != Mnemonic::add;
			if (flags_wanted(PF)) {
				if (!writes)
					subtract ? a.sub_imm(parity_register, left, amount, false, is64) : a.add_imm(parity_register, left, amount, false, is64);
				else
					a.mov(parity_register, result);
			}
		}
		if (writes && !direct)
			write(target, result);
		return;
	}
	const uint8_t right = read(source, s1);
	if (size >= 4) {
		if (flags && !logical && flags_wanted(AF))
			a.eor(adjust_register, left, right);
		switch (mnemonic) {
		case Mnemonic::add: a.add(result, left, right, Shift::lsl, 0, is64, flags); carry_inverted = false; break;
		case Mnemonic::sub: case Mnemonic::cmp:
			a.sub(writes ? result : a64::zr, left, right, Shift::lsl, 0, is64, flags);
			carry_inverted = true;
			break;
		case Mnemonic::adc:
			if (carry_inverted)
				a.cfinv();
			a.adc(result, left, right, is64, flags);
			carry_inverted = false;
			break;
		case Mnemonic::sbb:
			if (!carry_inverted)
				a.cfinv();
			a.sbc(result, left, right, is64, flags);
			carry_inverted = true;
			break;
		case Mnemonic::and_: case Mnemonic::test:
			a.and_(writes ? result : a64::zr, left, right, is64, flags);
			carry_inverted = false;
			break;
		case Mnemonic::or_: case Mnemonic::xor_:
			if (mnemonic == Mnemonic::or_)
				a.orr(result, left, right, Shift::lsl, 0, is64);
			else
				a.eor(result, left, right, Shift::lsl, 0, is64);
			if (flags) {
				a.and_(a64::zr, result, result, is64, true);
				carry_inverted = false;
			}
			break;
		default: throw std::logic_error("not an arithmetic instruction");
		}
		// The parity source must be the result; for cmp and test it was not kept, so compute it.
		if (flags && flags_wanted(PF)) {
			if (!writes) {
				if (mnemonic == Mnemonic::cmp)
					a.sub(parity_register, left, right, Shift::lsl, 0, is64);
				else
					a.and_(parity_register, left, right, is64);
			} else
				a.mov(parity_register, result);
		}
		if (writes && !direct)
			write(target, result);
		return;
	}
	// 8 and 16 bits: operate on values moved to the top of a W register, so N, Z, C and V come out for the narrow width.
	const uint8_t top = static_cast<uint8_t>(32 - size * 8);
	if (flags && flags_wanted(AF))
		a.eor(adjust_register, left, right);
	switch (mnemonic) {
	case Mnemonic::add: case Mnemonic::sub: case Mnemonic::cmp:
		if (flags) {
			a.lsl_imm(s3, left, top, false);
			a.lsl_imm(s4, right, top, false);
			if (mnemonic == Mnemonic::add) {
				a.add(s2, s3, s4, Shift::lsl, 0, false, true);
				carry_inverted = false;
			} else {
				a.sub(s2, s3, s4, Shift::lsl, 0, false, true);
				carry_inverted = true;
			}
			a.lsr_imm(s2, s2, top, false);
		} else if (mnemonic == Mnemonic::add)
			a.add(s2, left, right, Shift::lsl, 0, false);
		else
			a.sub(s2, left, right, Shift::lsl, 0, false);
		break;
	default:
		if (mnemonic == Mnemonic::and_ || mnemonic == Mnemonic::test)
			a.and_(s2, left, right, false);
		else if (mnemonic == Mnemonic::or_)
			a.orr(s2, left, right, Shift::lsl, 0, false);
		else
			a.eor(s2, left, right, Shift::lsl, 0, false);
		if (flags) {
			a.lsl_imm(s3, s2, top, false);
			a.and_(a64::zr, s3, s3, false, true);
			carry_inverted = false;
		}
		break;
	}
	if (flags && flags_wanted(PF))
		a.mov(parity_register, s2);
	if (writes)
		write(target, s2);
}

void BlockTranslator::unary(const Instruction &instruction)
{
	const Operand &target = instruction.operands[0];
	const unsigned size = target.size;
	const bool is64 = size == 8;
	const Mnemonic mnemonic = instruction.mnemonic;
	const uint8_t value = read(target, s0);
	if (mnemonic == Mnemonic::not_) {
		a.orn(s2, a64::zr, value, is64 || size < 4 ? true : false);
		write(target, s2);
		return;
	}
	const bool flags = flags_wanted(all_flags & ~(mnemonic == Mnemonic::neg ? 0 : CF));
	const bool keep_carry = mnemonic != Mnemonic::neg && flags_wanted(CF);
	if (keep_carry)
		a.mrs_nzcv(s3);
	if (flags && flags_wanted(AF)) {
		// inc and dec: the other operand is 1; neg: 0 - value.
		if (mnemonic == Mnemonic::neg)
			a.mov(adjust_register, value);
		else
			a.eor_imm(adjust_register, value, 1);
	}
	const uint8_t top = size >= 4 ? 0 : static_cast<uint8_t>(32 - size * 8);
	uint8_t operand = value;
	if (top) {
		a.lsl_imm(s4, value, top, false);
		operand = s4;
	}
	const bool w = !(size == 8);
	if (top) {
		a.mov_imm(s1, uint64_t(1) << top, false);
	}
	switch (mnemonic) {
	case Mnemonic::inc:
		if (top)
			a.add(s2, operand, s1, Shift::lsl, 0, false, flags);
		else
			a.add_imm(s2, operand, 1, false, !w, flags);
		break;
	case Mnemonic::dec:
		if (top)
			a.sub(s2, operand, s1, Shift::lsl, 0, false, flags);
		else
			a.sub_imm(s2, operand, 1, false, !w, flags);
		break;
	default: a.sub(s2, a64::zr, operand, Shift::lsl, 0, !w, flags); break;
	}
	if (top)
		a.lsr_imm(s2, s2, top, false);
	if (keep_carry)
		a.rmif(s3, 28, 0b0010);
	else if (flags)
		carry_inverted = mnemonic != Mnemonic::inc;
	if (flags && flags_wanted(PF))
		a.mov(parity_register, s2);
	write(target, s2);
}

void BlockTranslator::shift(const Instruction &instruction)
{
	const Operand &target = instruction.operands[0];
	const Operand &count_operand = instruction.operands[1];
	const unsigned size = target.size;
	const bool is64 = size == 8;
	const unsigned bits = size * 8;
	const Mnemonic mnemonic = instruction.mnemonic;
	if (count_operand.kind == OperandKind::imm) {
		const unsigned count = shift_count(instruction);
		if (count == 0) {
			// Nothing shifts and no flag changes, but a 32-bit register is still written, so its upper half clears.
			if (target.kind == OperandKind::reg && size == 4)
				a.mov(host_register[target.reg], host_register[target.reg], false);
			return;
		}
		if (count >= bits)
			throw std::logic_error("narrow shift beyond its width");
		uint8_t value = read(target, s0);
		if (mnemonic == Mnemonic::sar && size < 4) {
			a.sbfx(s0, value, 0, static_cast<uint8_t>(bits), false);
			value = s0;
		}
		const bool w = !is64;
		// Without flags to compute from the operand afterwards, a full-width register takes the result directly.
		const bool direct = !flags_wanted(all_flags) && target.kind == OperandKind::reg && size >= 4 && target.reg < high_byte_base;
		const uint8_t result = direct ? host_register[target.reg] : s2;
		switch (mnemonic) {
		case Mnemonic::shl: a.lsl_imm(result, value, static_cast<uint8_t>(count), !w); break;
		case Mnemonic::shr: a.lsr_imm(result, value, static_cast<uint8_t>(count), !w); break;
		default: a.asr_imm(result, value, static_cast<uint8_t>(count), !w); break;
		}
		if (direct)
			return;
		if (flags_wanted(all_flags)) {
			// N and Z from the result at the top of the register; C is the last bit shifted out, V is defined for a count of one.
			const uint8_t top = static_cast<uint8_t>((is64 ? 64 : 32) - bits);
			const unsigned out = mnemonic == Mnemonic::shl ? bits - count : count - 1;
			a.ubfx(s3, value, static_cast<uint8_t>(out), 1, true);
			if (count == 1 && mnemonic != Mnemonic::sar) {
				// shl: OF = top bit of the result xor CF; shr: OF = top bit of the operand.
				a.ubfx(s4, mnemonic == Mnemonic::shl ? s2 : value, static_cast<uint8_t>(bits - 1), 1, true);
				if (mnemonic == Mnemonic::shl)
					a.eor(s4, s4, s3);
				a.orr(s3, s4, s3, Shift::lsl, 1);
			} else
				a.lsl_imm(s3, s3, 1);
			if (top) {
				a.lsl_imm(s4, s2, top, false);
				a.and_(a64::zr, s4, s4, false, true);
			} else
				a.and_(a64::zr, s2, s2, is64, true);
			a.rmif(s3, 0, 0b0011);
			carry_inverted = false;
			if (flags_wanted(PF))
				a.mov(parity_register, s2);
		}
		write(target, s2);
		return;
	}
	// By CL: a count of zero changes nothing at all, not even the flags. Both ways leave C inverted.
	if (flags_wanted(all_flags))
		normalize_carry();
	const Assembler::Label skip = a.label();
	if (target.kind == OperandKind::reg && size == 4)
		a.mov(host_register[target.reg], host_register[target.reg], false);
	a.and_imm(s1, host_register[rcx], is64 ? 63 : 31, false);
	a.cbz(s1, skip, false);
	const uint8_t value = read(target, s0);
	switch (mnemonic) {
	case Mnemonic::shl: a.lslv(s2, value, s1, is64); break;
	case Mnemonic::shr: a.lsrv(s2, value, s1, is64); break;
	default: a.asrv(s2, value, s1, is64); break;
	}
	if (flags_wanted(all_flags)) {
		// C: the last bit out. V: undefined beyond a count of one, and computed as for one.
		if (mnemonic == Mnemonic::shl) {
			a.mov_imm(s3, bits, false);
			a.sub(s3, s3, s1, Shift::lsl, 0, false);
			a.lsrv(s3, value, s3, true);
		} else {
			a.sub_imm(s3, s1, 1, false, false);
			a.lsrv(s3, value, s3, true);
		}
		a.and_imm(s3, s3, 1);
		// V as for a count of one, the only count that defines it: shl, top bit of the result xor CF; shr, top bit of the operand.
		if (mnemonic != Mnemonic::sar) {
			a.ubfx(s4, mnemonic == Mnemonic::shl ? s2 : value, static_cast<uint8_t>(bits - 1), 1, true);
			if (mnemonic == Mnemonic::shl)
				a.eor(s4, s4, s3);
			a.orr(s3, s4, s3, Shift::lsl, 1);
		} else
			a.lsl_imm(s3, s3, 1);
		a.and_(a64::zr, s2, s2, is64, true);
		a.rmif(s3, 0, 0b0011);
		carry_inverted = false;
		if (flags_wanted(PF))
			a.mov(parity_register, s2);
	}
	write(target, s2);
	if (flags_wanted(all_flags))
		a.cfinv();
	a.bind(skip);
	if (flags_wanted(all_flags))
		carry_inverted = true;
}

void BlockTranslator::multiply(const Instruction &instruction)
{
	const Operand &target = instruction.operands[0];
	const bool is64 = target.size == 8;
	const Operand &left_operand = instruction.count == 3 ? instruction.operands[1] : target;
	const Operand &right_operand = instruction.count == 3 ? instruction.operands[2] : instruction.operands[1];
	uint8_t left, right;
	if (is64) {
		left = read(left_operand, s0);
		right = read(right_operand, s1);
		a.mul(s2, left, right);
	} else {
		read_signed(left_operand, s0, 8);
		Operand right_value = right_operand;
		if (right_value.kind == OperandKind::imm)
			right_value.size = 8;
		if (right_value.kind == OperandKind::imm)
			read(right_value, s1);
		else
			read_signed(right_value, s1, 8);
		left = s0;
		right = s1;
		a.mul(s2, left, right);
	}
	if (flags_wanted(all_flags)) {
		// CF and OF: the product does not fit the destination.
		if (is64) {
			a.smulh(s3, left, right);
			a.eor(s3, s3, s2, Shift::asr, 63);
		} else {
			a.sbfx(s3, s2, 0, 32);
			a.eor(s3, s3, s2);
		}
		a.and_(a64::zr, s2, s2, is64, true);
		a.cset(s4, Cond::ne);
		a.and_(a64::zr, s3, s3, true, true);
		a.cset(s3, Cond::ne);
		a.orr(s3, s3, s3, Shift::lsl, 1);
		// Z and N from the product, as most CPUs leave them.
		a.and_(a64::zr, s2, s2, is64, true);
		a.rmif(s3, 0, 0b0011);
		carry_inverted = false;
		if (flags_wanted(PF))
			a.mov(parity_register, s2);
	}
	write(target, s2);
}

void BlockTranslator::exchange(const Instruction &instruction)
{
	const Operand &first = instruction.operands[0];
	const Operand &second = instruction.operands[1];
	const unsigned size = first.size;
	if (instruction.mnemonic == Mnemonic::xchg) {
		if (first.kind == OperandKind::mem || second.kind == OperandKind::mem) {
			// xchg with memory is atomic whether or not it says lock.
			const Operand &memory = first.kind == OperandKind::mem ? first : second;
			const Operand &reg = first.kind == OperandKind::mem ? second : first;
			const uint8_t value = read(reg, s0, false);
			const Address at = address(memory, true);
			a.swpal(value, s1, at.base, size);
			write(reg, s1);
			return;
		}
		read(first, s0, false);
		read(second, s1, false);
		write(first, s1);
		write(second, s0);
		return;
	}
	const Address at = address(first, true);
	if (at.base != address_register)
		a.mov(address_register, at.base);
	const bool is64 = size == 8;
	if (instruction.mnemonic == Mnemonic::cmpxchg) {
		// casal compares with s0 and leaves the old value there; the flags are those of cmp accumulator, old value.
		const Operand accumulator{OperandKind::reg, static_cast<uint8_t>(size), rax};
		const uint8_t expected = read(accumulator, s0, false);
		const uint8_t replacement = read(second, s1, false);
		a.mov(s3, expected);
		a.casal(s0, replacement, address_register, size);
		const uint8_t top = static_cast<uint8_t>(size >= 4 ? 0 : 32 - size * 8);
		if (top) {
			a.lsl_imm(s4, s3, top, false);
			a.lsl_imm(s2, s0, top, false);
			a.sub(a64::zr, s4, s2, Shift::lsl, 0, false, true);
		} else
			a.sub(a64::zr, s3, s0, Shift::lsl, 0, is64, true);
		carry_inverted = true;
		if (flags_wanted(PF))
			a.sub(parity_register, s3, s0);
		if (flags_wanted(AF))
			a.eor(adjust_register, s3, s0);
		// The accumulator takes the old value only when the compare failed. A 32-bit exchange that succeeds leaves all of RAX as it
		// was, as x86 silicon does (Rosetta clears the upper half), so pick the whole register on equal before the flags change.
		if (size == 4) {
			const Operand whole{OperandKind::reg, 8, rax};
			const uint8_t before = read(whole, s2, false);
			a.mov(s4, s0, false);
			a.csel(s4, before, s4, Cond::eq);
			write(whole, s4);
		} else
			write(accumulator, s0);
		return;
	}
	// xadd: memory gets the sum, the register the old value; flags as for add.
	const uint8_t addend = read(second, s1, false);
	a.ldaddal(addend, s0, address_register, size);
	const uint8_t top = static_cast<uint8_t>(size >= 4 ? 0 : 32 - size * 8);
	if (flags_wanted(all_flags)) {
		if (top) {
			a.lsl_imm(s3, s0, top, false);
			a.lsl_imm(s4, addend, top, false);
			a.add(s2, s3, s4, Shift::lsl, 0, false, true);
			a.lsr_imm(s2, s2, top, false);
		} else
			a.add(s2, s0, addend, Shift::lsl, 0, is64, true);
		carry_inverted = false;
		if (flags_wanted(PF))
			a.mov(parity_register, s2);
		if (flags_wanted(AF))
			a.eor(adjust_register, s0, addend);
	}
	write(second, s0);
}

void BlockTranslator::push_value(uint8_t value)
{
	const Operand top{OperandKind::mem, 8, no_register, rsp};
	if (ordered(top)) {
		a.sub_imm(host_register[rsp], host_register[rsp], 8);
		if (options.ordering == MemoryOrdering::rcpc) {
			a.stlr(value, host_register[rsp], 8);
			++stats.ordered_stores;
		} else {
			a.dmb(a64::Barrier::ishst);
			a.str(value, host_register[rsp], 0, 8);
			++stats.barriers;
		}
	} else {
		a.str_pre(value, host_register[rsp], -8);
		++stats.plain_accesses;
	}
}

void BlockTranslator::pop_value(uint8_t target)
{
	const Operand top{OperandKind::mem, 8, no_register, rsp};
	if (ordered(top)) {
		if (options.ordering == MemoryOrdering::rcpc) {
			a.ldapr(target, host_register[rsp], 8);
			++stats.ordered_loads;
		} else {
			a.ldr(target, host_register[rsp], 0, 8);
			a.dmb(a64::Barrier::ishld);
			++stats.barriers;
		}
		a.add_imm(host_register[rsp], host_register[rsp], 8);
	} else {
		a.ldr_post(target, host_register[rsp], 8);
		++stats.plain_accesses;
	}
}

void BlockTranslator::load_vector(uint8_t target, const Operand &memory, unsigned size)
{
	// LDAPR has no SIMD&FP form: an ordered load is a plain one followed by DMB ISHLD, a store DMB ISHST and a plain one, as with
	// MemoryOrdering::barriers. A misaligned address needs no other path.
	const bool order = ordered(memory);
	Address at = address(memory, false);
	if (!fits_offset(at.offset, size)) {
		a.mov_imm(s5, static_cast<uint64_t>(at.offset));
		a.add(address_register, at.base, s5);
		at = {address_register, 0};
	}
	a.ldr_v(target, at.base, at.offset, size);
	if (order) {
		a.dmb(a64::Barrier::ishld);
		++stats.barriers;
	} else
		++stats.plain_accesses;
}

void BlockTranslator::store_vector(uint8_t source, const Operand &memory, unsigned size)
{
	const bool order = ordered(memory);
	Address at = address(memory, false);
	if (!fits_offset(at.offset, size)) {
		a.mov_imm(s5, static_cast<uint64_t>(at.offset));
		a.add(address_register, at.base, s5);
		at = {address_register, 0};
	}
	if (order) {
		a.dmb(a64::Barrier::ishst);
		++stats.barriers;
	} else
		++stats.plain_accesses;
	a.str_v(source, at.base, at.offset, size);
}

uint8_t BlockTranslator::vector_source(const Operand &operand, uint8_t scratch)
{
	if (operand.kind == OperandKind::xmm)
		return operand.reg;
	load_vector(scratch, operand, operand.size);
	return scratch;
}

void BlockTranslator::sse_move(const Instruction &instruction)
{
	const Operand &target = instruction.operands[0], &source = instruction.operands[1];
	const auto kind = [](const Operand &operand) { return operand.kind; };
	switch (instruction.mnemonic) {
	case Mnemonic::movss: case Mnemonic::movsd: {
		const unsigned size = instruction.mnemonic == Mnemonic::movss ? 4 : 8;
		// Between registers only the low element moves; from memory the rest of the register clears, as ldr s and ldr d do.
		if (kind(target) == OperandKind::xmm && kind(source) == OperandKind::xmm)
			a.ins(target.reg, 0, source.reg, 0, size);
		else if (kind(target) == OperandKind::xmm)
			load_vector(target.reg, source, size);
		else
			store_vector(source.reg, target, size);
		return;
	}
	case Mnemonic::movd: case Mnemonic::movq: {
		const bool quad = instruction.mnemonic == Mnemonic::movq;
		if (kind(target) == OperandKind::xmm && kind(source) == OperandKind::xmm)
			a.simd(simd::fmov_d_d, target.reg, source.reg);
		else if (kind(target) == OperandKind::xmm && kind(source) == OperandKind::reg)
			a.simd(quad ? simd::fmov_d_x : simd::fmov_s_w, target.reg, host_register[source.reg]);
		else if (kind(target) == OperandKind::reg)
			a.simd(quad ? simd::fmov_x_d : simd::fmov_w_s, host_register[target.reg], source.reg);
		else if (kind(target) == OperandKind::xmm)
			load_vector(target.reg, source, quad ? 8 : 4);
		else
			store_vector(source.reg, target, quad ? 8 : 4);
		return;
	}
	case Mnemonic::movlps: case Mnemonic::movlpd: case Mnemonic::movhps: case Mnemonic::movhpd: {
		const unsigned half = instruction.mnemonic == Mnemonic::movhps || instruction.mnemonic == Mnemonic::movhpd ? 1 : 0;
		if (kind(target) == OperandKind::xmm) {
			load_vector(vs0, source, 8);
			a.ins(target.reg, half, vs0, 0, 8);
		} else if (half) {
			a.ins(vs0, 0, source.reg, 1, 8);
			store_vector(vs0, target, 8);
		} else
			store_vector(source.reg, target, 8);
		return;
	}
	case Mnemonic::movhlps: a.ins(target.reg, 0, source.reg, 1, 8); return;
	case Mnemonic::movlhps: a.ins(target.reg, 1, source.reg, 0, 8); return;
	default:
		// The whole register: movaps, movups, movapd, movupd, movdqa, movdqu and the non-temporal stores.
		if (kind(target) == OperandKind::xmm && kind(source) == OperandKind::xmm) {
			if (target.reg != source.reg)
				a.simd(simd::orr_16b, target.reg, source.reg, source.reg);
		} else if (kind(target) == OperandKind::xmm)
			load_vector(target.reg, source, 16);
		else
			store_vector(source.reg, target, 16);
		return;
	}
}

void BlockTranslator::sse_mask(const Instruction &instruction)
{
	// The sign bit of each element, gathered into the low byte of each doubleword with shifts and accumulates, then both bytes.
	const uint8_t source = instruction.operands[1].reg;
	switch (instruction.mnemonic) {
	case Mnemonic::movmskps:
		a.shift_imm(simd::ushr, vs0, source, 32, 31);
		a.shift_imm(simd::usra, vs0, vs0, 64, 31);
		a.umov(s0, vs0, 0, 1);
		a.umov(s1, vs0, 8, 1);
		a.orr(s0, s0, s1, Shift::lsl, 2, false);
		break;
	case Mnemonic::movmskpd:
		a.shift_imm(simd::ushr, vs0, source, 64, 63);
		a.umov(s0, vs0, 0, 1);
		a.umov(s1, vs0, 8, 1);
		a.orr(s0, s0, s1, Shift::lsl, 1, false);
		break;
	default:
		a.shift_imm(simd::ushr, vs0, source, 8, 7);
		a.shift_imm(simd::usra, vs0, vs0, 16, 7);
		a.shift_imm(simd::usra, vs0, vs0, 32, 14);
		a.shift_imm(simd::usra, vs0, vs0, 64, 28);
		a.umov(s0, vs0, 0, 1);
		a.umov(s1, vs0, 8, 1);
		a.orr(s0, s0, s1, Shift::lsl, 8, false);
		break;
	}
	write(instruction.operands[0], s0);
}

void BlockTranslator::sse_logic(const Instruction &instruction)
{
	const uint8_t target = instruction.operands[0].reg;
	const Operand &source_operand = instruction.operands[1];
	const Mnemonic mnemonic = instruction.mnemonic;
	const bool exclusive = mnemonic == Mnemonic::xorps || mnemonic == Mnemonic::xorpd || mnemonic == Mnemonic::pxor;
	if (exclusive && source_operand.kind == OperandKind::xmm && source_operand.reg == target) {
		a.simd(simd::movi_zero_2d, target, 0);
		return;
	}
	const uint8_t source = vector_source(source_operand, vs0);
	switch (mnemonic) {
	case Mnemonic::andps: case Mnemonic::andpd: case Mnemonic::pand: a.simd(simd::and_16b, target, target, source); break;
	// andn: the source and not the destination.
	case Mnemonic::andnps: case Mnemonic::andnpd: case Mnemonic::pandn: a.simd(simd::bic_16b, target, source, target); break;
	case Mnemonic::orps: case Mnemonic::orpd: case Mnemonic::por: a.simd(simd::orr_16b, target, target, source); break;
	default: a.simd(simd::eor_16b, target, target, source); break;
	}
}

void BlockTranslator::sse_arithmetic(const Instruction &instruction)
{
	// With FPCR.AH, Arm's NaN propagation (the first operand's NaN, quieted), default NaN (negative) and min and max (the second
	// operand when either is a NaN or both are zeros) are x86's, so each operation is one instruction: the destination is the first
	// operand, the source the second.
	struct Operation {
		Mnemonic ps, pd, ss, sd;
		uint32_t vector, scalar;
	};
	static constexpr Operation operations[] = {
		{Mnemonic::addps, Mnemonic::addpd, Mnemonic::addss, Mnemonic::addsd, simd::fadd_4s, simd::fadd_s},
		{Mnemonic::subps, Mnemonic::subpd, Mnemonic::subss, Mnemonic::subsd, simd::fsub_4s, simd::fsub_s},
		{Mnemonic::mulps, Mnemonic::mulpd, Mnemonic::mulss, Mnemonic::mulsd, simd::fmul_4s, simd::fmul_s},
		{Mnemonic::divps, Mnemonic::divpd, Mnemonic::divss, Mnemonic::divsd, simd::fdiv_4s, simd::fdiv_s},
		{Mnemonic::minps, Mnemonic::minpd, Mnemonic::minss, Mnemonic::minsd, simd::fmin_4s, simd::fmin_s},
		{Mnemonic::maxps, Mnemonic::maxpd, Mnemonic::maxss, Mnemonic::maxsd, simd::fmax_4s, simd::fmax_s},
		{Mnemonic::sqrtps, Mnemonic::sqrtpd, Mnemonic::sqrtss, Mnemonic::sqrtsd, simd::fsqrt_4s, simd::fsqrt_s},
	};
	const Mnemonic mnemonic = instruction.mnemonic;
	const uint8_t target = instruction.operands[0].reg;
	for (const Operation &operation : operations) {
		const bool packed = mnemonic == operation.ps || mnemonic == operation.pd;
		const bool scalar = mnemonic == operation.ss || mnemonic == operation.sd;
		if (!packed && !scalar)
			continue;
		const bool doubles = mnemonic == operation.pd || mnemonic == operation.sd;
		const bool unary = operation.vector == simd::fsqrt_4s;
		const uint8_t source = vector_source(instruction.operands[1], vs0);
		if (packed) {
			a.simd(operation.vector | (doubles ? simd::sz : 0), target, unary ? source : target, unary ? 0 : source);
			return;
		}
		// A scalar result clears the rest of an Arm register; x86 keeps the destination's upper elements.
		a.simd(operation.scalar | (doubles ? simd::sz : 0), vs1, unary ? source : target, unary ? 0 : source);
		a.ins(target, 0, vs1, 0, doubles ? 8 : 4);
		return;
	}
	throw std::logic_error("not an SSE arithmetic instruction");
}

void BlockTranslator::sse_compare(const Instruction &instruction)
{
	// cmpps, cmppd, cmpss and cmpsd: all ones where the predicate holds. lt, le, nlt and nle signal on a quiet NaN as fcmgt and fcmge do;
	// eq, neq, unord and ord only on a signaling one, as fcmeq.
	const Mnemonic mnemonic = instruction.mnemonic;
	const bool doubles = mnemonic == Mnemonic::cmppd || mnemonic == Mnemonic::cmpsd;
	const bool scalar = mnemonic == Mnemonic::cmpss || mnemonic == Mnemonic::cmpsd;
	const uint32_t sz = doubles ? simd::sz : 0;
	const uint32_t eq = (scalar ? simd::fcmeq_s : simd::fcmeq_4s) | sz, ge = (scalar ? simd::fcmge_s : simd::fcmge_4s) | sz,
		gt = (scalar ? simd::fcmgt_s : simd::fcmgt_4s) | sz;
	const uint8_t target = instruction.operands[0].reg;
	const uint8_t source = vector_source(instruction.operands[1], vs0);
	const unsigned predicate = static_cast<unsigned>(instruction.operands[2].value) & 7;
	switch (predicate & 3) {
	case 0: a.simd(eq, vs1, target, source); break;
	case 1: a.simd(gt, vs1, source, target); break;
	case 2: a.simd(ge, vs1, source, target); break;
	default:
		// ord: both are numbers; unord: not.
		a.simd(eq, vs1, target, target);
		a.simd(eq, vs2, source, source);
		a.simd(simd::and_16b, vs1, vs1, vs2);
		if (predicate == 3)
			a.simd(simd::not_16b, vs1, vs1);
		break;
	}
	// Predicates 4 to 6 are the negations of 0 to 2.
	if (predicate >= 4 && predicate <= 6)
		a.simd(simd::not_16b, vs1, vs1);
	if (scalar)
		a.ins(target, 0, vs1, 0, doubles ? 8 : 4);
	else
		a.simd(simd::orr_16b, target, vs1, vs1);
}

void BlockTranslator::sse_ordered_compare(const Instruction &instruction)
{
	// comis and ucomis: ZF, PF and CF are 111 unordered, 000 greater, 001 less, 100 equal; OF, SF and AF are cleared. fcmp leaves
	// unordered as V, so ZF is EQ or VS and CF is LT (N != V, which unordered also satisfies). comis signals on a quiet NaN, as fcmpe.
	const Mnemonic mnemonic = instruction.mnemonic;
	const bool doubles = mnemonic == Mnemonic::comisd || mnemonic == Mnemonic::ucomisd;
	const bool signaling = mnemonic == Mnemonic::comiss || mnemonic == Mnemonic::comisd;
	const uint8_t target = instruction.operands[0].reg;
	const uint8_t source = vector_source(instruction.operands[1], vs0);
	a.simd(simd::fcmp_s | (doubles ? simd::sz : 0) | (signaling ? simd::signal_any : 0), 0, target, source);
	if (!flags_wanted(all_flags))
		return;
	a.cset(s0, Cond::vs);
	a.cset(s1, Cond::eq);
	a.orr(s1, s1, s0, Shift::lsl, 0, false);
	// C holds the inverse of CF, the form block boundaries keep.
	a.cset(s2, Cond::ge);
	a.lsl_imm(s1, s1, 30, false);
	a.orr(s1, s1, s2, Shift::lsl, 29, false);
	a.msr_nzcv(s1);
	carry_inverted = true;
	if (flags_wanted(PF | AF)) {
		// An even low byte (0) when unordered, an odd one otherwise; the same value for AF's source leaves AF clear.
		a.eor_imm(parity_register, s0, 1, false);
		a.mov(adjust_register, parity_register);
	}
}

void BlockTranslator::convert_to_integer(uint8_t source, bool doubles, bool truncate, unsigned size)
{
	// Out of range or NaN, x86 gives the integer indefinite (the lowest integer of the width) where fcvtzs saturates and gives 0 for
	// NaN. frintx rounds as FPCR says (MXCSR.RC) and reports inexact, so fcvtzs then converts an integer. The fix-up compares, so NZCV
	// is kept when a later instruction reads it.
	const uint32_t sz = doubles ? simd::sz : 0;
	uint8_t value = source;
	if (!truncate) {
		// frintx reports inexact for 2147483647.5, which then does not fit: x86 raises only invalid there. FPSR is cleared around
		// the pair and merged back below without inexact when invalid was raised.
		a.mrs_fpsr(s3);
		a.msr_fpsr(a64::zr);
		a.simd(simd::frintx_s | sz, vs1, source);
		value = vs1;
	}
	const bool keep = flags_wanted(all_flags);
	if (keep)
		a.mrs_nzcv(s4);
	const bool is64 = size == 8;
	a.simd(simd::fcvtzs_w_s | (is64 ? simd::sf : 0) | sz, s0, value);
	a.simd(simd::fcmp_s | sz, 0, value, value);
	a.mov_imm(s1, is64 ? 0x8000000000000000ull : 0x80000000ull, is64);
	a.csel(s0, s1, s0, Cond::vs, is64);
	if (doubles && !is64) {
		// 2147483647.5 truncates to the highest int32, which saturation also gives: compare with 2^31 instead.
		a.mov_imm(s2, 0x41e0000000000000ull);
		a.simd(simd::fmov_d_x, vs2, s2);
		a.simd(simd::fcmp_s | sz, 0, value, vs2);
		a.csel(s0, s1, s0, Cond::ge, false);
	} else {
		// No single (nor double, for int64) converts to the highest integer exactly: only saturation gives it. V: it plus one overflows.
		a.add_imm(a64::zr, s0, 1, false, is64, true);
		a.csel(s0, s1, s0, Cond::vs, is64);
	}
	if (!truncate) {
		a.mrs_fpsr(s1);
		a.and_imm(s2, s1, 1, false);
		a.lsl_imm(s2, s2, 4, false);
		a.orn(s2, a64::zr, s2, false);
		a.and_(s1, s1, s2, false);
		a.orr(s1, s1, s3, Shift::lsl, 0, false);
		a.msr_fpsr(s1);
	}
	if (keep)
		a.msr_nzcv(s4);
}

void BlockTranslator::sse_convert(const Instruction &instruction)
{
	const Mnemonic mnemonic = instruction.mnemonic;
	const Operand &target_operand = instruction.operands[0], &source_operand = instruction.operands[1];
	const uint8_t target = target_operand.reg;
	switch (mnemonic) {
	case Mnemonic::cvtsi2ss: case Mnemonic::cvtsi2sd: {
		const bool doubles = mnemonic == Mnemonic::cvtsi2sd;
		const uint8_t value = read(source_operand, s0);
		a.simd(simd::scvtf_s_w | (source_operand.size == 8 ? simd::sf : 0) | (doubles ? simd::sz : 0), vs1, value);
		a.ins(target, 0, vs1, 0, doubles ? 8 : 4);
		return;
	}
	case Mnemonic::cvtss2si: case Mnemonic::cvttss2si: case Mnemonic::cvtsd2si: case Mnemonic::cvttsd2si: {
		const bool doubles = mnemonic == Mnemonic::cvtsd2si || mnemonic == Mnemonic::cvttsd2si;
		const bool truncate = mnemonic == Mnemonic::cvttss2si || mnemonic == Mnemonic::cvttsd2si;
		convert_to_integer(vector_source(source_operand, vs0), doubles, truncate, target_operand.size);
		write(target_operand, s0);
		return;
	}
	case Mnemonic::cvtpd2dq: case Mnemonic::cvttpd2dq: {
		// Two doubles to two int32 in the low quadword, the rest cleared: element by element, for the indefinite and the flags.
		const bool truncate = mnemonic == Mnemonic::cvttpd2dq;
		const uint8_t source = vector_source(source_operand, vs0);
		a.ins(vs3, 0, source, 1, 8);
		convert_to_integer(source, true, truncate, 4);
		a.mov(s6, s0, false);
		convert_to_integer(vs3, true, truncate, 4);
		a.orr(s0, s6, s0, Shift::lsl, 32);
		a.simd(simd::fmov_d_x, target, s0);
		return;
	}
	case Mnemonic::cvtps2dq: case Mnemonic::cvttps2dq: {
		// fcvtzs saturates; where the value is not below 2^31 (a NaN is not either) x86 gives 0x80000000, as fcvtzs already does below
		// -2^31.
		uint8_t value = vector_source(source_operand, vs0);
		if (mnemonic == Mnemonic::cvtps2dq) {
			a.simd(simd::frintx_4s, vs1, value);
			value = vs1;
		}
		a.simd(simd::fcvtzs_4s, vs2, value);
		a.mov_imm(s0, 0x4f000000, false);
		a.simd(simd::dup_4s_w, vs3, s0);
		a.simd(simd::fcmgt_4s, vs3, vs3, value);
		a.simd(simd::and_16b, vs2, vs2, vs3);
		a.simd(simd::not_16b, vs3, vs3);
		a.shift_imm(simd::shl, vs3, vs3, 32, 31);
		a.simd(simd::orr_16b, target, vs2, vs3);
		return;
	}
	default: break;
	}
	const uint8_t source = vector_source(source_operand, vs0);
	switch (mnemonic) {
	case Mnemonic::cvtss2sd: a.simd(simd::fcvt_d_s, vs1, source); a.ins(target, 0, vs1, 0, 8); return;
	case Mnemonic::cvtsd2ss: a.simd(simd::fcvt_s_d, vs1, source); a.ins(target, 0, vs1, 0, 4); return;
	case Mnemonic::cvtps2pd: a.simd(simd::fcvtl_2d, target, source); return;
	// The two singles go to the low quadword and the rest clears, as a 64-bit vector write does.
	case Mnemonic::cvtpd2ps: a.simd(simd::fcvtn_2s, target, source); return;
	case Mnemonic::cvtdq2ps: a.simd(simd::scvtf_4s, target, source); return;
	case Mnemonic::cvtdq2pd:
		a.simd(simd::sxtl_2d, vs1, source);
		a.simd(simd::scvtf_4s | simd::sz, target, vs1);
		return;
	default: throw std::logic_error("not an SSE conversion");
	}
}

void BlockTranslator::sse_integer(const Instruction &instruction)
{
	struct Operation {
		Mnemonic mnemonic;
		uint32_t opcode;
	};
	using enum Mnemonic;
	static constexpr Operation elementwise[] = {
		{paddb, simd::add_16b}, {paddw, simd::add_16b | simd::size_h}, {paddd, simd::add_16b | simd::size_s},
		{paddq, simd::add_16b | simd::size_d}, {psubb, simd::sub_16b}, {psubw, simd::sub_16b | simd::size_h},
		{psubd, simd::sub_16b | simd::size_s}, {psubq, simd::sub_16b | simd::size_d}, {paddsb, simd::sqadd_16b},
		{paddsw, simd::sqadd_16b | simd::size_h}, {paddusb, simd::uqadd_16b}, {paddusw, simd::uqadd_16b | simd::size_h},
		{psubsb, simd::sqsub_16b}, {psubsw, simd::sqsub_16b | simd::size_h}, {psubusb, simd::uqsub_16b},
		{psubusw, simd::uqsub_16b | simd::size_h}, {pmullw, simd::mul_16b | simd::size_h}, {pcmpeqb, simd::cmeq_16b},
		{pcmpeqw, simd::cmeq_16b | simd::size_h}, {pcmpeqd, simd::cmeq_16b | simd::size_s}, {pcmpgtb, simd::cmgt_16b},
		{pcmpgtw, simd::cmgt_16b | simd::size_h}, {pcmpgtd, simd::cmgt_16b | simd::size_s}, {pavgb, simd::urhadd_16b},
		{pavgw, simd::urhadd_16b | simd::size_h}, {pminub, simd::umin_16b}, {pmaxub, simd::umax_16b},
		{pminsw, simd::smin_16b | simd::size_h}, {pmaxsw, simd::smax_16b | simd::size_h},
	};
	const uint8_t target = instruction.operands[0].reg;
	const uint8_t source = vector_source(instruction.operands[1], vs0);
	for (const Operation &operation : elementwise)
		if (operation.mnemonic == instruction.mnemonic) {
			a.simd(operation.opcode, target, target, source);
			return;
		}
	switch (instruction.mnemonic) {
	case pmulhw: case pmulhuw: {
		// The high halves of the 32-bit products.
		const uint32_t multiply = (instruction.mnemonic == pmulhw ? simd::smull_8h : simd::umull_8h) | simd::size_h;
		a.simd(multiply, vs1, target, source);
		a.simd(multiply | simd::q, vs2, target, source);
		a.simd(simd::uzp2_16b | simd::size_h, target, vs1, vs2);
		return;
	}
	case pmuludq:
		// The low doublewords of both quadwords, multiplied to quadwords.
		a.simd(simd::xtn_8b | simd::size_s, vs1, target);
		a.simd(simd::xtn_8b | simd::size_s, vs2, source);
		a.simd(simd::umull_8h | simd::size_s, target, vs1, vs2);
		return;
	case pmaddwd:
		a.simd(simd::smull_8h | simd::size_h, vs1, target, source);
		a.simd(simd::smull_8h | simd::size_h | simd::q, vs2, target, source);
		a.simd(simd::addp_16b | simd::size_s, target, vs1, vs2);
		return;
	case psadbw:
		// The sum of the absolute differences of each eight bytes, in the low word of their quadword.
		a.simd(simd::uabd_16b, vs1, target, source);
		a.simd(simd::uaddlp_16b, vs1, vs1);
		a.simd(simd::uaddlp_16b | simd::size_h, vs1, vs1);
		a.simd(simd::uaddlp_16b | simd::size_s, target, vs1);
		return;
	default: throw std::logic_error("not an SSE integer instruction");
	}
}

void BlockTranslator::sse_shift(const Instruction &instruction)
{
	using enum Mnemonic;
	const Mnemonic mnemonic = instruction.mnemonic;
	const uint8_t target = instruction.operands[0].reg;
	const Operand &count_operand = instruction.operands[1];
	if (mnemonic == psrldq || mnemonic == pslldq) {
		// Whole bytes, zeros shifted in: ext with a zero register.
		const unsigned count = static_cast<unsigned>(count_operand.value) & 0xff;
		if (count == 0)
			return;
		a.simd(simd::movi_zero_2d, count > 15 ? target : vs1, 0);
		if (count > 15)
			return;
		if (mnemonic == psrldq)
			a.ext(target, target, vs1, count);
		else
			a.ext(target, vs1, target, 16 - count);
		return;
	}
	const bool left = mnemonic == psllw || mnemonic == pslld || mnemonic == psllq;
	const bool arithmetic = mnemonic == psraw || mnemonic == psrad;
	const unsigned bits = mnemonic == psllw || mnemonic == psrlw || mnemonic == psraw ? 16 :
		mnemonic == pslld || mnemonic == psrld || mnemonic == psrad ? 32 : 64;
	const uint32_t size = bits == 16 ? simd::size_h : bits == 32 ? simd::size_s : simd::size_d;
	if (count_operand.kind == OperandKind::imm) {
		// A count beyond the width clears the elements, or fills them with their sign.
		unsigned count = static_cast<unsigned>(count_operand.value) & 0xff;
		if (count >= bits) {
			if (!arithmetic) {
				a.simd(simd::movi_zero_2d, target, 0);
				return;
			}
			count = bits - 1;
		}
		if (count)
			a.shift_imm(left ? simd::shl : arithmetic ? simd::sshr : simd::ushr, target, target, bits, count);
		return;
	}
	// By the unsigned low quadword of an XMM register or of memory: ushl and sshl take a signed byte per element, and clear or fill
	// for any count from the width on, so the count is limited to the width.
	const uint8_t source = vector_source(count_operand, vs0);
	a.simd(simd::fmov_x_d, s0, source);
	const bool keep = flags_wanted(all_flags);
	if (keep)
		a.mrs_nzcv(s4);
	a.mov_imm(s1, bits);
	a.sub(a64::zr, s0, s1, Shift::lsl, 0, true, true);
	a.csel(s0, s1, s0, Cond::hi);
	if (keep)
		a.msr_nzcv(s4);
	if (!left)
		a.sub(s0, a64::zr, s0);
	a.simd(simd::dup_w | (bits / 8) << 16, vs1, s0);
	a.simd((arithmetic ? simd::sshl_16b : simd::ushl_16b) | size, target, target, vs1);
}

void BlockTranslator::sse3(const Instruction &instruction)
{
	const Mnemonic mnemonic = instruction.mnemonic;
	const uint8_t target = instruction.operands[0].reg;
	const Operand &source_operand = instruction.operands[1];
	const bool doubles = mnemonic == Mnemonic::addsubpd || mnemonic == Mnemonic::haddpd || mnemonic == Mnemonic::hsubpd;
	const uint32_t sz = doubles ? simd::sz : 0;
	const uint32_t elements = doubles ? simd::size_d : simd::size_s;
	switch (mnemonic) {
	case Mnemonic::movddup:
		if (source_operand.kind == OperandKind::mem) {
			load_vector(vs0, source_operand, 8);
			a.dup(target, vs0, 8);
		} else
			a.dup(target, source_operand.reg, 8);
		return;
	// From memory; the upper half of vlddqu ymm comes here as a register (avx()).
	case Mnemonic::lddqu:
		if (source_operand.kind == OperandKind::xmm)
			a.simd(simd::orr_16b, target, source_operand.reg, source_operand.reg);
		else
			load_vector(target, source_operand, 16);
		return;
	default: break;
	}
	const uint8_t source = vector_source(source_operand, vs0);
	switch (mnemonic) {
	case Mnemonic::movsldup: a.simd(simd::trn1_16b | simd::size_s, target, source, source); return;
	case Mnemonic::movshdup: a.simd(simd::trn2_16b | simd::size_s, target, source, source); return;
	// The pairs of each operand in turn: faddp adds the first element of a pair to the second, as x86 does.
	case Mnemonic::haddps: case Mnemonic::haddpd: a.simd(simd::faddp_4s | sz, target, target, source); return;
	case Mnemonic::hsubps: case Mnemonic::hsubpd:
		a.simd(simd::uzp1_16b | elements, vs1, target, source);
		a.simd(simd::uzp2_16b | elements, vs2, target, source);
		a.simd(simd::fsub_4s | sz, target, vs1, vs2);
		return;
	default:
		// addsub: the even elements subtract, the odd ones add. The source's even elements are negated first (exact; fneg leaves a
		// NaN's sign alone under FPCR.AH), then one fadd.
		a.simd(simd::fneg_4s | sz, vs1, source);
		if (doubles) {
			a.ins(vs1, 1, source, 1, 8);
		} else {
			a.simd(simd::movi_low_singles, vs2, 0);
			a.simd(simd::bsl_16b, vs2, vs1, source);
			a.simd(simd::orr_16b, vs1, vs2, vs2);
		}
		a.simd(simd::fadd_4s | sz, target, target, vs1);
		return;
	}
}

void BlockTranslator::ssse3(const Instruction &instruction)
{
	using enum Mnemonic;
	const Mnemonic mnemonic = instruction.mnemonic;
	const uint8_t target = instruction.operands[0].reg;
	const uint8_t source = vector_source(instruction.operands[1], vs0);
	const uint32_t size = mnemonic == phaddw || mnemonic == phaddsw || mnemonic == phsubw || mnemonic == phsubsw || mnemonic == psignw ||
			mnemonic == pabsw ? simd::size_h : mnemonic == phaddd || mnemonic == phsubd || mnemonic == psignd || mnemonic == pabsd ?
		simd::size_s : 0;
	switch (mnemonic) {
	case pshufb:
		// A byte with bit 7 set selects 0; tbl gives 0 for any index from 16 on, so bits 6-4 are cleared and bit 7 kept.
		a.simd(simd::movi_8f, vs2, 0);
		a.simd(simd::and_16b, vs1, source, vs2);
		a.simd(simd::tbl_16b, target, target, vs1);
		return;
	case phaddw: case phaddd: a.simd(simd::addp_16b | size, target, target, source); return;
	case phaddsw: case phsubw: case phsubd: case phsubsw:
		a.simd(simd::uzp1_16b | size, vs1, target, source);
		a.simd(simd::uzp2_16b | size, vs2, target, source);
		a.simd((mnemonic == phaddsw ? simd::sqadd_16b : mnemonic == phsubsw ? simd::sqsub_16b : simd::sub_16b) | size, target, vs1, vs2);
		return;
	case pmaddubsw:
		// Unsigned bytes of the destination times signed bytes of the source fit a halfword exactly; adjacent pairs add with signed
		// saturation.
		a.simd(simd::uxtl_8h, vs1, target, 0);
		a.simd(simd::uxtl_8h | simd::q, vs2, target, 0);
		a.simd(simd::sxtl_8h, vs3, source, 0);
		a.simd(simd::mul_16b | simd::size_h, vs1, vs1, vs3);
		a.simd(simd::sxtl_8h | simd::q, vs3, source, 0);
		a.simd(simd::mul_16b | simd::size_h, vs2, vs2, vs3);
		a.simd(simd::uzp1_16b | simd::size_h, vs3, vs1, vs2);
		a.simd(simd::uzp2_16b | simd::size_h, vs1, vs1, vs2);
		a.simd(simd::sqadd_16b | simd::size_h, target, vs3, vs1);
		return;
	case pmulhrsw:
		// ((a * b >> 14) + 1) >> 1 is (a * b + 2^14) >> 15, truncated to 16 bits: -32768 * -32768 gives 0x8000, where sqrdmulh would
		// saturate.
		a.simd(simd::smull_4s, vs1, target, source);
		a.simd(simd::smull_4s | simd::q, vs2, target, source);
		a.simd(simd::rshrn_4h_15, target, vs1, 0);
		a.simd(simd::rshrn_4h_15 | simd::q, target, vs2, 0);
		return;
	case psignb: case psignw: case psignd:
		a.simd(simd::neg_16b | size, vs1, target);
		a.simd(simd::cmlt_zero_16b | size, vs2, source);
		a.simd(simd::bsl_16b, vs2, vs1, target);
		a.simd(simd::cmeq_zero_16b | size, vs3, source);
		a.simd(simd::bic_16b, target, vs2, vs3);
		return;
	case pabsb: case pabsw: case pabsd: a.simd(simd::abs_16b | size, target, source); return;
	default: {
		// palignr: bytes imm8 on of the destination above the source, zeros beyond.
		const auto shift = static_cast<unsigned>(instruction.operands[2].value & 0xff);
		if (shift < 16)
			a.ext(target, source, target, shift);
		else if (shift < 32) {
			a.simd(simd::movi_zero_2d, vs1, 0);
			a.ext(target, target, vs1, shift - 16);
		} else
			a.simd(simd::movi_zero_2d, target, 0);
		return;
	}
	}
}

namespace {

// imm5 of ins (general) and dup (element): element index of size bytes.
uint32_t element_imm5(unsigned index, unsigned size)
{
	const unsigned log = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
	return uint32_t(((index << (log + 1)) | (1u << log)) & 31) << 16;
}

}

void BlockTranslator::sse41(const Instruction &instruction)
{
	using enum Mnemonic;
	const Mnemonic mnemonic = instruction.mnemonic;
	const Operand *operands = instruction.operands.data();
	const auto imm = [&](unsigned at) { return static_cast<unsigned>(operands[at].value & 0xff); };
	switch (mnemonic) {
	case pextrb: case pextrw: case pextrd: case pextrq: case extractps: {
		// An element of the XMM register into a general register (zero-extended) or memory of its size.
		const unsigned size = mnemonic == pextrb ? 1 : mnemonic == pextrw ? 2 : mnemonic == pextrq ? 8 : 4;
		a.umov(s0, operands[1].reg, imm(2) % (16 / size), size);
		if (operands[0].kind == OperandKind::mem)
			store_memory(s0, operands[0], size);
		else
			write(operands[0], s0);
		return;
	}
	case pinsrb: case pinsrw: case pinsrd: case pinsrq: {
		const unsigned size = mnemonic == pinsrb ? 1 : mnemonic == pinsrw ? 2 : mnemonic == pinsrq ? 8 : 4;
		const uint8_t value = operands[1].kind == OperandKind::mem ? (load_memory(s0, operands[1], size), s0) : read(operands[1], s0);
		a.simd(simd::ins_general | element_imm5(imm(2) % (16 / size), size), operands[0].reg, value);
		return;
	}
	default: break;
	}
	const uint8_t target = operands[0].reg;
	const Operand &source_operand = operands[1];
	switch (mnemonic) {
	case pmovsxbw: case pmovsxbd: case pmovsxbq: case pmovsxwd: case pmovsxwq: case pmovsxdq: case pmovzxbw: case pmovzxbd:
	case pmovzxbq: case pmovzxwd: case pmovzxwq: case pmovzxdq: {
		// Widen step by step: bytes to halfwords, halfwords to words, words to doublewords.
		const bool sign = mnemonic <= pmovsxdq;
		const unsigned from = mnemonic == pmovsxbw || mnemonic == pmovsxbd || mnemonic == pmovsxbq || mnemonic == pmovzxbw ||
				mnemonic == pmovzxbd || mnemonic == pmovzxbq ? 1 : mnemonic == pmovsxdq || mnemonic == pmovzxdq ? 4 : 2;
		const unsigned to = mnemonic == pmovsxbw || mnemonic == pmovzxbw ? 2 : mnemonic == pmovsxbd || mnemonic == pmovzxbd ||
				mnemonic == pmovsxwd || mnemonic == pmovzxwd ? 4 : 8;
		uint8_t value = source_operand.reg;
		if (source_operand.kind == OperandKind::mem) {
			load_vector(vs0, source_operand, source_operand.size);
			value = vs0;
		}
		for (unsigned size = from; size < to; size *= 2) {
			const uint32_t opcode = size == 1 ? (sign ? simd::sxtl_8h : simd::uxtl_8h) : size == 2 ? (sign ? simd::sxtl_4s : simd::uxtl_4s) :
				(sign ? simd::sxtl_2d : simd::uxtl_2d);
			a.simd(opcode, size * 2 == to ? target : vs1, value, 0);
			value = vs1;
		}
		return;
	}
	case movntdqa:
		if (source_operand.kind == OperandKind::xmm)
			a.simd(simd::orr_16b, target, source_operand.reg, source_operand.reg);
		else
			load_vector(target, source_operand, 16);
		return;
	case insertps: {
		// The source element (imm8 bits 7:6, or the 4 bytes of memory) into element bits 5:4, then the elements of bits 3:0 cleared.
		if (source_operand.kind == OperandKind::mem) {
			load_vector(vs0, source_operand, 4);
			a.ins(target, (imm(2) >> 4) & 3, vs0, 0, 4);
		} else
			a.ins(target, (imm(2) >> 4) & 3, source_operand.reg, imm(2) >> 6, 4);
		a.simd(simd::movi_zero_2d, vs1, 0);
		for (unsigned i = 0; i < 4; ++i)
			if (imm(2) >> i & 1)
				a.ins(target, i, vs1, 0, 4);
		return;
	}
	case roundss: case roundsd: case roundps: case roundpd: {
		const bool scalar = mnemonic == roundss || mnemonic == roundsd;
		const bool doubles = mnemonic == roundsd || mnemonic == roundpd;
		const uint8_t source = source_operand.kind == OperandKind::mem ? (load_vector(vs0, source_operand, source_operand.size), vs0) :
			source_operand.reg;
		const unsigned control = imm(2);
		const uint32_t sz = doubles ? simd::sz : 0;
		const uint8_t result = scalar ? vs1 : target;
		// Bit 2: MXCSR's mode (frinti, or frintx when bit 3 lets PE be raised). Otherwise bits 1:0 name it: without bit 3 PE must be
		// raised too, which only frintx does, so FPCR's RMode is set to that mode around it.
		if (control & 4)
			a.simd((scalar ? (control & 8 ? simd::frinti_s : simd::frintx_s) : (control & 8 ? simd::frinti_4s : simd::frintx_4s)) | sz,
				result, source);
		else if (control & 8) {
			static constexpr uint32_t vector[] = {simd::frintn_4s, simd::frintm_4s, simd::frintp_4s, simd::frintz_4s};
			static constexpr uint32_t one[] = {simd::frintn_s, simd::frintm_s, simd::frintp_s, simd::frintz_s};
			a.simd((scalar ? one[control & 3] : vector[control & 3]) | sz, result, source);
		} else {
			// x86's 0 nearest, 1 down, 2 up, 3 zero are RMode 0, 2, 1, 3.
			static constexpr uint8_t rmode[] = {0, 2, 1, 3};
			a.mrs_fpcr(s1);
			a.mov(s2, s1);
			a.mov_imm(s3, rmode[control & 3], false);
			a.bfi(s2, s3, 22, 2);
			a.msr_fpcr(s2);
			a.simd((scalar ? simd::frintx_s : simd::frintx_4s) | sz, result, source);
			a.msr_fpcr(s1);
		}
		if (scalar)
			a.ins(target, 0, vs1, 0, doubles ? 8 : 4);
		return;
	}
	default: break;
	}
	const uint8_t source = vector_source(source_operand, vs0);
	switch (mnemonic) {
	case pblendvb: case blendvps: case blendvpd: {
		const uint32_t size = mnemonic == blendvps ? simd::size_s : mnemonic == blendvpd ? simd::size_d : 0;
		a.simd(simd::cmlt_zero_16b | size, vs1, 0);
		a.simd(simd::bsl_16b, vs1, source, target);
		a.simd(simd::orr_16b, target, vs1, vs1);
		return;
	}
	case ptest: {
		// ZF: the destination and the source share no bit; CF: the source has no bit the destination lacks. AF, OF, PF and SF clear.
		a.simd(simd::and_16b, vs1, target, source);
		a.simd(simd::bic_16b, vs2, source, target);
		test_flags();
		return;
	}
	case pmuldq:
		a.simd(simd::uzp1_16b | simd::size_s, vs1, target, target);
		a.simd(simd::uzp1_16b | simd::size_s, vs2, source, source);
		a.simd(simd::smull_2d, target, vs1, vs2);
		return;
	case pcmpeqq: a.simd(simd::cmeq_16b | simd::size_d, target, target, source); return;
	case packusdw:
		a.simd(simd::sqxtun_4h, vs1, target, 0);
		a.simd(simd::sqxtun_4h | simd::q, vs1, source, 0);
		a.simd(simd::orr_16b, target, vs1, vs1);
		return;
	case pminsb: a.simd(simd::smin_16b, target, target, source); return;
	case pminsd: a.simd(simd::smin_16b | simd::size_s, target, target, source); return;
	case pminuw: a.simd(simd::umin_16b | simd::size_h, target, target, source); return;
	case pminud: a.simd(simd::umin_16b | simd::size_s, target, target, source); return;
	case pmaxsb: a.simd(simd::smax_16b, target, target, source); return;
	case pmaxsd: a.simd(simd::smax_16b | simd::size_s, target, target, source); return;
	case pmaxuw: a.simd(simd::umax_16b | simd::size_h, target, target, source); return;
	case pmaxud: a.simd(simd::umax_16b | simd::size_s, target, target, source); return;
	case pmulld: a.simd(simd::mul_16b | simd::size_s, target, target, source); return;
	case phminposuw: {
		// The lowest halfword and the index of its first occurrence; the rest cleared.
		a.simd(simd::uminv_h, vs1, source);
		a.simd(simd::dup_element | element_imm5(0, 2), vs2, vs1);
		a.simd(simd::cmeq_16b | simd::size_h, vs2, source, vs2);
		a.mov_imm(s0, 0x0003000200010000ull);
		a.mov_imm(s1, 0x0007000600050004ull);
		a.simd(simd::fmov_d_x, vs3, s0);
		a.simd(simd::ins_general | element_imm5(1, 8), vs3, s1);
		a.simd(simd::orn_16b, vs3, vs3, vs2);
		a.simd(simd::uminv_h, vs3, vs3);
		a.umov(s0, vs1, 0, 2);
		a.umov(s1, vs3, 0, 2);
		a.orr(s0, s0, s1, Shift::lsl, 16, false);
		a.simd(simd::fmov_s_w, target, s0);
		return;
	}
	case blendps: case blendpd: case pblendw: {
		const unsigned size = mnemonic == blendps ? 4 : mnemonic == blendpd ? 8 : 2;
		for (unsigned i = 0; i < 16 / size; ++i)
			if (imm(2) >> i & 1)
				a.ins(target, i, source, i, size);
		return;
	}
	case dpps: case dppd: {
		// Products of the elements imm8 bits 7:4 (5:4) select, +0.0 elsewhere so they raise nothing, summed as x86 does: (p0 + p1) +
		// (p2 + p3); the sum goes to the elements bits 3:0 (1:0) select, +0.0 to the others.
		const bool doubles = mnemonic == dppd;
		const unsigned count = doubles ? 2 : 4, size = doubles ? 8 : 4;
		const uint32_t sz = doubles ? simd::sz : 0;
		auto mask = [&](unsigned bits, uint8_t into) {
			uint64_t words[2] = {};
			for (unsigned i = 0; i < count; ++i)
				if (bits >> i & 1)
					words[i * size / 8] |= (size == 8 ? ~0ull : 0xffffffffull) << (i * size % 8 * 8);
			a.mov_imm(s0, words[0]);
			a.mov_imm(s1, words[1]);
			a.simd(simd::fmov_d_x, into, s0);
			a.simd(simd::ins_general | element_imm5(1, 8), into, s1);
		};
		mask(imm(2) >> 4, vs3);
		a.simd(simd::and_16b, vs1, target, vs3);
		a.simd(simd::and_16b, vs2, source, vs3);
		a.simd(simd::fmul_4s | sz, vs1, vs1, vs2);
		a.simd(simd::faddp_4s | sz, vs1, vs1, vs1);
		if (!doubles)
			a.simd(simd::faddp_4s, vs1, vs1, vs1);
		mask(imm(2) & 15, vs3);
		a.simd(simd::and_16b, target, vs1, vs3);
		return;
	}
	default: {
		// mpsadbw: eight sums of four absolute byte differences, a block of the destination (imm8 bit 2: bytes 0 or 4 on) sliding
		// against four bytes of the source (bits 1:0: from byte 0, 4, 8 or 12).
		const unsigned from = (imm(2) >> 2 & 1) * 4, block = (imm(2) & 3) * 4;
		for (unsigned j = 0; j < 4; ++j) {
			a.ext(vs1, target, target, from + j);
			a.simd(simd::dup_element | element_imm5(block + j, 1), vs2, source);
			a.simd((j == 0 ? simd::uabdl_8h : simd::uabal_8h), vs3, vs1, vs2);
		}
		a.simd(simd::orr_16b, target, vs3, vs3);
		return;
	}
	}
}

void BlockTranslator::sse_shuffle(const Instruction &instruction)
{
	using enum Mnemonic;
	const Mnemonic mnemonic = instruction.mnemonic;
	const uint8_t target = instruction.operands[0].reg;
	const uint8_t source = vector_source(instruction.operands[1], vs0);
	struct Interleave {
		Mnemonic mnemonic;
		uint32_t opcode;
	};
	static constexpr Interleave interleaves[] = {
		{punpcklbw, simd::zip1_16b}, {punpcklwd, simd::zip1_16b | simd::size_h}, {punpckldq, simd::zip1_16b | simd::size_s},
		{punpcklqdq, simd::zip1_16b | simd::size_d}, {punpckhbw, simd::zip2_16b}, {punpckhwd, simd::zip2_16b | simd::size_h},
		{punpckhdq, simd::zip2_16b | simd::size_s}, {punpckhqdq, simd::zip2_16b | simd::size_d}, {unpcklps, simd::zip1_16b | simd::size_s},
		{unpcklpd, simd::zip1_16b | simd::size_d}, {unpckhps, simd::zip2_16b | simd::size_s}, {unpckhpd, simd::zip2_16b | simd::size_d},
	};
	for (const Interleave &interleave : interleaves)
		if (interleave.mnemonic == mnemonic) {
			a.simd(interleave.opcode, target, target, source);
			return;
		}
	const unsigned order = static_cast<unsigned>(instruction.count == 3 ? instruction.operands[2].value : 0) & 0xff;
	switch (mnemonic) {
	case packsswb: case packssdw: case packuswb: {
		// The destination's elements narrowed to the low half, the source's to the high half.
		const uint32_t narrow = (mnemonic == packuswb ? simd::sqxtun_8b : simd::sqxtn_8b) | (mnemonic == packssdw ? simd::size_h : 0);
		a.simd(narrow, vs1, target);
		a.simd(narrow | simd::q, vs1, source);
		break;
	}
	case pshufd:
		for (unsigned i = 0; i < 4; ++i)
			a.ins(vs1, i, source, order >> (2 * i) & 3, 4);
		break;
	case pshuflw: case pshufhw: {
		// Four words of one half chosen from that half, the other half as it is.
		const unsigned half = mnemonic == pshufhw ? 4 : 0;
		a.simd(simd::orr_16b, vs1, source, source);
		for (unsigned i = 0; i < 4; ++i)
			a.ins(vs1, half + i, source, half + (order >> (2 * i) & 3), 2);
		break;
	}
	case shufps:
		// Two singles from the destination, then two from the source.
		for (unsigned i = 0; i < 4; ++i)
			a.ins(vs1, i, i < 2 ? target : source, order >> (2 * i) & 3, 4);
		break;
	case shufpd:
		a.ins(vs1, 0, target, order & 1, 8);
		a.ins(vs1, 1, source, order >> 1 & 1, 8);
		break;
	default: throw std::logic_error("not an SSE shuffle");
	}
	a.simd(simd::orr_16b, target, vs1, vs1);
}

void BlockTranslator::exit_with(ExitReason reason, uint64_t rip)
{
	a.mov_imm(s4, rip);
	a.str(s4, state_register, state_offset::rip, 8);
	a.mov_imm(s0, static_cast<uint32_t>(reason), false);
	a.ldr(address_register, state_register, state_offset::exit_trampoline, 8);
	a.br(address_register);
}

void BlockTranslator::exit_to(uint64_t target)
{
	// A b the runtime can point at the target's translation; until then it reaches a stub that asks for one.
	PendingExit exit{a.label(), a.label(), target, a.size()};
	a.bind(exit.site);
	a.b(exit.stub);
	pending.push_back(exit);
}

void BlockTranslator::exit_indirect()
{
	// The target is in x16. Look it up without touching NZCV, which may be live into the target.
	const Assembler::Label miss = a.label();
	a.ldr(s5, state_register, state_offset::lookup_table, 8);
	a.ubfx(s0, address_register, 0, lookup_bits);
	a.add(s5, s5, s0, Shift::lsl, 4);
	a.ldp(s1, s2, s5, 0);
	a.eor(s1, s1, address_register);
	a.cbnz(s1, miss);
	a.br(s2);
	a.bind(miss);
	a.str(address_register, state_register, state_offset::rip, 8);
	a.mov_imm(s0, static_cast<uint32_t>(ExitReason::lookup_miss), false);
	a.ldr(address_register, state_register, state_offset::exit_trampoline, 8);
	a.br(address_register);
}

void BlockTranslator::finish_exits()
{
	for (const Unaligned &path : unaligned) {
		a.bind(path.start);
		if (path.store) {
			a.dmb(a64::Barrier::ish);
			a.str(path.reg, path.base, 0, path.size);
		} else {
			a.ldr(path.reg, path.base, 0, path.size);
			a.dmb(a64::Barrier::ishld);
		}
		a.b(path.back);
	}
	for (const PendingExit &exit : pending) {
		a.bind(exit.stub);
		a.mov_imm(s4, exit.target);
		a.str(s4, state_register, state_offset::rip, 8);
		// x10: the execute address of the b to patch, which only the code knows once placed.
		a.adr(s1, exit.site);
		a.mov_imm(s0, static_cast<uint32_t>(ExitReason::chain), false);
		a.ldr(address_register, state_register, state_offset::exit_trampoline, 8);
		a.br(address_register);
		block.exits.push_back({static_cast<uint32_t>(exit.word), exit.target});
	}
}

void BlockTranslator::clear_high(uint8_t ymm)
{
	a.simd(simd::movi_zero_2d, vh3, 0);
	store_high(vh3, ymm);
}

uint8_t BlockTranslator::high_source(const Operand &operand, uint8_t target)
{
	if (operand.kind == OperandKind::xmm) {
		load_high(target, operand.reg);
		return target;
	}
	Operand high = operand;
	high.value += 16;
	high.size = 16;
	load_vector(target, high, 16);
	return target;
}

void BlockTranslator::avx(const Instruction &instruction)
{
	const Mnemonic mnemonic = instruction.mnemonic;
	const Operand *operands = instruction.operands.data();
	if (mnemonic == Mnemonic::vzeroupper || mnemonic == Mnemonic::vzeroall) {
		a.simd(simd::movi_zero_2d, vh3, 0);
		for (uint8_t r = 0; r < 16; ++r) {
			store_high(vh3, r);
			if (mnemonic == Mnemonic::vzeroall)
				a.simd(simd::movi_zero_2d, r, 0);
		}
		return;
	}
	if (mnemonic >= Mnemonic::vfmadd132ps)
		return avx_fma(instruction);
	if (mnemonic >= avx_first)
		return avx_lanes(instruction);
	const bool wide = instruction.vector_size == 32;
	if (mnemonic == Mnemonic::ptest)
		return avx_test(instruction);
	if (wide && (mnemonic == Mnemonic::movmskps || mnemonic == Mnemonic::movmskpd || mnemonic == Mnemonic::pmovmskb))
		return avx_mask(instruction);
	const Operand &target = operands[0];
	const bool writes_vector = target.kind == OperandKind::xmm && mnemonic != Mnemonic::comiss && mnemonic != Mnemonic::comisd &&
		mnemonic != Mnemonic::ucomiss && mnemonic != Mnemonic::ucomisd;
	// The shifts by a count in an XMM register or memory use its low 128 bits in both halves.
	const bool count_shift = !vex_unary(instruction) && instruction.count == 3 && operands[2].kind != OperandKind::imm &&
		(mnemonic == Mnemonic::psllw || mnemonic == Mnemonic::pslld || mnemonic == Mnemonic::psllq || mnemonic == Mnemonic::psrlw ||
			mnemonic == Mnemonic::psrld || mnemonic == Mnemonic::psrlq || mnemonic == Mnemonic::psraw || mnemonic == Mnemonic::psrad);
	if (target.kind == OperandKind::mem) {
		// Stores: the low half as SSE stores it, then the upper half 16 bytes on. x86 writes no byte of a store that faults; two
		// stores of 16 bytes would leave the first written when the second faults. So the first and the last byte are probed first
		// with an atomic add of zero (staddb wzr), which needs write access and changes nothing, even with another thread writing:
		// 32 bytes span at most those two pages.
		if (wide) {
			for (const int64_t offset : {int64_t(0), int64_t(31)}) {
				Operand probe = target;
				probe.value += offset;
				probe.size = 1;
				effective_address(s1, probe, true);
				a.simd(simd::staddb_zero, a64::zr, s1, a64::zr);
			}
		}
		translate(legacy_form(instruction, 0));
		if (wide) {
			load_high(vh0, operands[1].reg);
			Operand high = target;
			high.value += 16;
			high.size = 16;
			store_vector(vh0, high, 16);
		}
		return;
	}
	if (!writes_vector) {
		translate(legacy_form(instruction, target.reg));
		return;
	}
	const uint8_t destination = target.reg;
	// x86's exceptions are precise: an instruction whose memory operand faults changes no register. So every load comes before the
	// first write to the destination: the upper half of a memory operand into vh1, the low half's result into vh2, the upper half's
	// into vh0, and only then the destination's two halves.
	if (vex_unary(instruction)) {
		if (wide) {
			const uint8_t high_input = high_source(operands[1], vh1);
			translate(legacy_form(instruction, vh2));
			Instruction high = legacy_form(instruction, vh0);
			high.operands[1] = Operand{};
			high.operands[1].kind = OperandKind::xmm;
			high.operands[1].size = 16;
			high.operands[1].reg = high_input;
			translate(high);
			store_high(vh0, destination);
			a.simd(simd::orr_16b, destination, vh2, vh2);
			return;
		}
		translate(legacy_form(instruction, destination));
		clear_high(destination);
		return;
	}
	const Operand &first = operands[1], &second = operands[2];
	const bool high_from_second = wide && !count_shift && second.kind != OperandKind::imm;
	const uint8_t high_input = high_from_second ? high_source(second, vh1) : 0;
	// The low half: the destination starts as the first source, in vh2 when the instruction has an upper half or a memory operand
	// (so nothing is written before the loads) or when the destination is the second source (which must be read first).
	const bool second_is_destination = second.kind == OperandKind::xmm && second.reg == destination && first.reg != destination;
	const uint8_t working = wide || second.kind == OperandKind::mem || second_is_destination ? vh2 : destination;
	if (first.reg != working)
		a.simd(simd::orr_16b, working, first.reg, first.reg);
	translate(legacy_form(instruction, working));
	if (wide) {
		load_high(vh0, first.reg);
		Instruction high = legacy_form(instruction, vh0);
		if (high_from_second) {
			high.operands[1] = Operand{};
			high.operands[1].kind = OperandKind::xmm;
			high.operands[1].size = 16;
			high.operands[1].reg = high_input;
		}
		// vshufpd takes bits 3:2 of its imm8 for the upper half (vshufps the same four fields for both).
		if (mnemonic == Mnemonic::shufpd)
			high.operands[2].value = (operands[3].value >> 2) & 3;
		// vblendps and vblendpd: one imm8 bit per element across both halves.
		if (mnemonic == Mnemonic::blendps)
			high.operands[2].value = (operands[3].value >> 4) & 15;
		if (mnemonic == Mnemonic::blendpd)
			high.operands[2].value = (operands[3].value >> 2) & 3;
		translate(high);
		store_high(vh0, destination);
	}
	if (working != destination)
		a.simd(simd::orr_16b, destination, working, working);
	if (!wide)
		clear_high(destination);
}

void BlockTranslator::test_flags()
{
	// ptest and the others: vs1 holds the destination and the source, vs2 the source and not the destination.
	a.umov(s0, vs1, 0, 8);
	a.umov(s1, vs1, 1, 8);
	a.orr(s0, s0, s1);
	a.umov(s1, vs2, 0, 8);
	a.umov(s2, vs2, 1, 8);
	a.orr(s1, s1, s2);
	if (flags_wanted(all_flags)) {
		// Z = (s0 == 0); C, the inverse of CF, = (s1 != 0); N = V = 0.
		a.sub_imm(s2, s0, 0, false, true, true);
		a.cset(s2, Cond::eq);
		a.sub_imm(s3, s1, 0, false, true, true);
		a.cset(s3, Cond::ne);
		a.lsl_imm(s2, s2, 30, false);
		a.orr(s2, s2, s3, Shift::lsl, 29, false);
		a.msr_nzcv(s2);
		carry_inverted = true;
		if (flags_wanted(PF | AF)) {
			// An odd low byte (PF clear), the same for AF's source (AF clear).
			a.mov_imm(parity_register, 1);
			a.mov(adjust_register, parity_register);
		}
	}
}

void BlockTranslator::avx_test(const Instruction &instruction)
{
	// vptest at either width: ZF and CF over all 256 bits. Every load comes first; nothing but the flags is written.
	const Operand &first = instruction.operands[0], &second = instruction.operands[1];
	uint8_t low = second.reg;
	if (second.kind == OperandKind::mem) {
		Operand half = second;
		half.size = 16;
		load_vector(vs0, half, 16);
		low = vs0;
	}
	if (instruction.vector_size == 32) {
		const uint8_t high = high_source(second, vh1);
		load_high(vh0, first.reg);
		a.simd(simd::and_16b, vh2, vh0, high);
		a.simd(simd::bic_16b, vh3, high, vh0);
	}
	a.simd(simd::and_16b, vs1, first.reg, low);
	a.simd(simd::bic_16b, vs2, low, first.reg);
	if (instruction.vector_size == 32) {
		a.simd(simd::orr_16b, vs1, vs1, vh2);
		a.simd(simd::orr_16b, vs2, vs2, vh3);
	}
	test_flags();
}

void BlockTranslator::avx_mask(const Instruction &instruction)
{
	// The mask of each half as SSE computes it, the upper one shifted above the lower: 16 bits each for bytes, 4 for singles, 2 for
	// doubles.
	const Operand &source = instruction.operands[1];
	const unsigned bits = instruction.mnemonic == Mnemonic::pmovmskb ? 16 : instruction.mnemonic == Mnemonic::movmskps ? 4 : 2;
	Instruction half = legacy_form(instruction, 0);
	half.operands[0].reg = rax;
	half.operands[0].size = 4;
	Operand into = instruction.operands[0];
	load_high(vh0, source.reg);
	half.operands[1].reg = vh0;
	// The SSE handler writes its general register; s6 keeps the upper mask across the second run.
	const uint8_t saved = host_register[into.reg];
	half.operands[0].reg = into.reg;
	translate(half);
	a.lsl_imm(s6, saved, static_cast<uint8_t>(bits), false);
	half.operands[1].reg = source.reg;
	translate(half);
	a.orr(saved, saved, s6, Shift::lsl, 0, false);
}

void BlockTranslator::avx_lanes(const Instruction &instruction)
{
	const Operand *operands = instruction.operands.data();
	const Mnemonic mnemonic = instruction.mnemonic;
	auto low_source = [&](const Operand &operand, uint8_t scratch) {
		if (operand.kind == OperandKind::xmm) {
			a.simd(simd::orr_16b, scratch, operand.reg, operand.reg);
			return scratch;
		}
		Operand low = operand;
		low.size = 16;
		load_vector(scratch, low, 16);
		return scratch;
	};
	switch (mnemonic) {
	case Mnemonic::vinsertf128: case Mnemonic::vinserti128: {
		const uint8_t destination = operands[0].reg, first = operands[1].reg;
		const bool upper = operands[3].value & 1;
		low_source(operands[2], vh1);
		if (upper) {
			if (destination != first)
				a.simd(simd::orr_16b, destination, first, first);
			store_high(vh1, destination);
		} else {
			load_high(vh0, first);
			a.simd(simd::orr_16b, destination, vh1, vh1);
			store_high(vh0, destination);
		}
		return;
	}
	case Mnemonic::vextractf128: case Mnemonic::vextracti128: {
		const uint8_t source = operands[1].reg;
		const bool upper = operands[2].value & 1;
		uint8_t value = source;
		if (upper) {
			load_high(vh0, source);
			value = vh0;
		}
		if (operands[0].kind == OperandKind::mem) {
			Operand to = operands[0];
			to.size = 16;
			store_vector(value, to, 16);
		} else {
			if (operands[0].reg != value)
				a.simd(simd::orr_16b, operands[0].reg, value, value);
			clear_high(operands[0].reg);
		}
		return;
	}
	case Mnemonic::vperm2f128: case Mnemonic::vperm2i128: {
		const uint8_t destination = operands[0].reg;
		// The four lanes to choose from: the first source's low and high, the second's low and high.
		low_source(operands[1], vh0);
		load_high(vh1, operands[1].reg);
		low_source(operands[2], vh2);
		high_source(operands[2], vh3);
		const auto control = static_cast<unsigned>(operands[3].value);
		const uint8_t lanes[] = {vh0, vh1, vh2, vh3};
		// The upper half first, while all four lanes are still there; then the lower one into the destination.
		const unsigned upper = control >> 4;
		if (upper & 8)
			a.simd(simd::movi_zero_2d, vs0, 0);
		else
			a.simd(simd::orr_16b, vs0, lanes[upper & 3], lanes[upper & 3]);
		store_high(vs0, destination);
		if (control & 8)
			a.simd(simd::movi_zero_2d, destination, 0);
		else
			a.simd(simd::orr_16b, destination, lanes[control & 3], lanes[control & 3]);
		return;
	}
	default: {
		// The broadcasts: element 0 of the source, from memory of the element's size or a register, into every element.
		const uint8_t destination = operands[0].reg;
		const bool wide = instruction.vector_size == 32;
		const unsigned element = mnemonic == Mnemonic::vbroadcastss || mnemonic == Mnemonic::vpbroadcastd ? 4 :
			mnemonic == Mnemonic::vbroadcastsd || mnemonic == Mnemonic::vpbroadcastq ? 8 : mnemonic == Mnemonic::vpbroadcastb ? 1 :
			mnemonic == Mnemonic::vpbroadcastw ? 2 : 16;
		uint8_t source = operands[1].reg;
		if (operands[1].kind == OperandKind::mem) {
			Operand from = operands[1];
			from.size = static_cast<uint8_t>(element);
			load_vector(vh0, from, element);
			source = vh0;
		}
		if (element == 16) {
			if (destination != source)
				a.simd(simd::orr_16b, destination, source, source);
		} else
			a.dup(destination, source, element);
		if (wide)
			store_high(destination, destination);
		else
			clear_high(destination);
		return;
	}
	}
}

void BlockTranslator::avx_fma(const Instruction &instruction)
{
	// x86: 132 is op1 * op3 + op2, 213 op2 * op1 + op3, 231 op2 * op3 + op1; fmsub subtracts the addend, fnmadd negates the
	// product, fnmsub both. Arm's four-operand scalar forms compute a + n * m (fmadd), a - n * m (fmsub), -a - n * m (fnmadd) and
	// n * m - a (fnmsub), each rounded once; the vector forms only a + n * m (fmla) and a - n * m (fmls), so for fmsub and fnmsub
	// the addend is negated first, which is exact.
	const auto index = static_cast<unsigned>(instruction.mnemonic) - static_cast<unsigned>(Mnemonic::vfmadd132ps);
	const unsigned type = index % 4, order = (index / 4) % 3, operation = index / 12;
	const bool scalar = type >= 2, doubles = type & 1;
	const bool wide = instruction.vector_size == 32;
	const Operand *operands = instruction.operands.data();
	const uint8_t destination = operands[0].reg;
	// The three operands as vector registers: op1 and op2 are registers; op3 is a register or memory.
	auto run = [&](uint8_t op1, uint8_t op2, uint8_t op3, uint8_t result) {
		uint8_t n, m, addend;
		switch (order) {
		case 0: n = op1; m = op3; addend = op2; break;
		case 1: n = op2; m = op1; addend = op3; break;
		default: n = op2; m = op3; addend = op1; break;
		}
		if (scalar) {
			static constexpr uint32_t forms[] = {0x1f000000, 0x1f208000, 0x1f008000, 0x1f200000};
			const uint32_t opcode = forms[operation] | (doubles ? simd::sz : 0) | uint32_t(addend) << 10;
			a.simd(opcode, vs1, n, m);
			// x86 keeps the rest of the destination (op1).
			if (result != op1)
				a.simd(simd::orr_16b, result, op1, op1);
			a.ins(result, 0, vs1, 0, doubles ? 8 : 4);
			return;
		}
		const uint32_t size = doubles ? simd::sz : 0;
		constexpr uint32_t fmla = 0x4e20cc00, fmls = 0x4ea0cc00, fneg = 0x6ea0f800;
		if (operation == 1 || operation == 3)
			a.simd(fneg | size, vs1, addend);
		else
			a.simd(simd::orr_16b, vs1, addend, addend);
		a.simd((operation >= 2 ? fmls : fmla) | size, vs1, n, m);
		a.simd(simd::orr_16b, result, vs1, vs1);
	};
	auto third = [&](const Operand &operand, uint8_t scratch, bool high) {
		if (high)
			return high_source(operand, scratch);
		if (operand.kind == OperandKind::xmm)
			return operand.reg;
		Operand low = operand;
		if (low.size == 32)
			low.size = 16;
		load_vector(scratch, low, low.size);
		return scratch;
	};
	// Both halves of a memory operand are loaded before the destination changes (precise exceptions, as in avx()).
	if (wide) {
		const uint8_t high_third = third(operands[2], vh2, true);
		const uint8_t low_third = third(operands[2], vh3, false);
		load_high(vh0, destination);
		load_high(vh1, operands[1].reg);
		run(vh0, vh1, high_third, vh0);
		run(destination, operands[1].reg, low_third, destination);
		store_high(vh0, destination);
		return;
	}
	run(destination, operands[1].reg, third(operands[2], vh2, false), destination);
	clear_high(destination);
}

void BlockTranslator::translate(const Instruction &instruction)
{
	const Operand *operands = instruction.operands.data();
	if (instruction.vex) {
		avx(instruction);
		return;
	}
	switch (instruction.mnemonic) {
	case Mnemonic::add: case Mnemonic::sub: case Mnemonic::cmp: case Mnemonic::and_: case Mnemonic::or_: case Mnemonic::xor_:
	case Mnemonic::test: case Mnemonic::adc: case Mnemonic::sbb:
		arithmetic(instruction);
		return;
	case Mnemonic::inc: case Mnemonic::dec: case Mnemonic::neg: case Mnemonic::not_: unary(instruction); return;
	case Mnemonic::shl: case Mnemonic::shr: case Mnemonic::sar: shift(instruction); return;
	case Mnemonic::imul: multiply(instruction); return;
	case Mnemonic::xchg: case Mnemonic::cmpxchg: case Mnemonic::xadd: exchange(instruction); return;
	case Mnemonic::movq: case Mnemonic::movd: case Mnemonic::movups: case Mnemonic::movupd: case Mnemonic::movaps:
	case Mnemonic::movapd: case Mnemonic::movdqa: case Mnemonic::movdqu: case Mnemonic::movntps: case Mnemonic::movntpd:
	case Mnemonic::movntdq: case Mnemonic::movss: case Mnemonic::movsd: case Mnemonic::movlps: case Mnemonic::movlpd:
	case Mnemonic::movhps: case Mnemonic::movhpd: case Mnemonic::movhlps: case Mnemonic::movlhps:
		sse_move(instruction);
		return;
	case Mnemonic::movmskps: case Mnemonic::movmskpd: case Mnemonic::pmovmskb: sse_mask(instruction); return;
	case Mnemonic::andps: case Mnemonic::andpd: case Mnemonic::andnps: case Mnemonic::andnpd: case Mnemonic::orps: case Mnemonic::orpd:
	case Mnemonic::xorps: case Mnemonic::xorpd: case Mnemonic::pand: case Mnemonic::pandn: case Mnemonic::por: case Mnemonic::pxor:
		sse_logic(instruction);
		return;
	case Mnemonic::addps: case Mnemonic::addpd: case Mnemonic::addss: case Mnemonic::addsd: case Mnemonic::subps: case Mnemonic::subpd:
	case Mnemonic::subss: case Mnemonic::subsd: case Mnemonic::mulps: case Mnemonic::mulpd: case Mnemonic::mulss: case Mnemonic::mulsd:
	case Mnemonic::divps: case Mnemonic::divpd: case Mnemonic::divss: case Mnemonic::divsd: case Mnemonic::sqrtps: case Mnemonic::sqrtpd:
	case Mnemonic::sqrtss: case Mnemonic::sqrtsd: case Mnemonic::minps: case Mnemonic::minpd: case Mnemonic::minss: case Mnemonic::minsd:
	case Mnemonic::maxps: case Mnemonic::maxpd: case Mnemonic::maxss: case Mnemonic::maxsd:
		sse_arithmetic(instruction);
		return;
	case Mnemonic::cmpps: case Mnemonic::cmppd: case Mnemonic::cmpss: case Mnemonic::cmpsd: sse_compare(instruction); return;
	case Mnemonic::comiss: case Mnemonic::comisd: case Mnemonic::ucomiss: case Mnemonic::ucomisd: sse_ordered_compare(instruction); return;
	case Mnemonic::cvtsi2ss: case Mnemonic::cvtsi2sd: case Mnemonic::cvtss2si: case Mnemonic::cvttss2si: case Mnemonic::cvtsd2si:
	case Mnemonic::cvttsd2si: case Mnemonic::cvtss2sd: case Mnemonic::cvtsd2ss: case Mnemonic::cvtdq2ps: case Mnemonic::cvtdq2pd:
	case Mnemonic::cvtps2dq: case Mnemonic::cvttps2dq: case Mnemonic::cvtpd2dq: case Mnemonic::cvttpd2dq: case Mnemonic::cvtps2pd:
	case Mnemonic::cvtpd2ps:
		sse_convert(instruction);
		return;
	case Mnemonic::paddb: case Mnemonic::paddw: case Mnemonic::paddd: case Mnemonic::paddq: case Mnemonic::psubb: case Mnemonic::psubw:
	case Mnemonic::psubd: case Mnemonic::psubq: case Mnemonic::paddsb: case Mnemonic::paddsw: case Mnemonic::paddusb:
	case Mnemonic::paddusw: case Mnemonic::psubsb: case Mnemonic::psubsw: case Mnemonic::psubusb: case Mnemonic::psubusw:
	case Mnemonic::pmullw: case Mnemonic::pmulhw: case Mnemonic::pmulhuw: case Mnemonic::pmuludq: case Mnemonic::pmaddwd:
	case Mnemonic::pcmpeqb: case Mnemonic::pcmpeqw: case Mnemonic::pcmpeqd: case Mnemonic::pcmpgtb: case Mnemonic::pcmpgtw:
	case Mnemonic::pcmpgtd: case Mnemonic::pavgb: case Mnemonic::pavgw: case Mnemonic::pminub: case Mnemonic::pmaxub:
	case Mnemonic::pminsw: case Mnemonic::pmaxsw: case Mnemonic::psadbw:
		sse_integer(instruction);
		return;
	case Mnemonic::psllw: case Mnemonic::pslld: case Mnemonic::psllq: case Mnemonic::psrlw: case Mnemonic::psrld: case Mnemonic::psrlq:
	case Mnemonic::psraw: case Mnemonic::psrad: case Mnemonic::psrldq: case Mnemonic::pslldq:
		sse_shift(instruction);
		return;
	case Mnemonic::addsubps: case Mnemonic::addsubpd: case Mnemonic::haddps: case Mnemonic::haddpd: case Mnemonic::hsubps:
	case Mnemonic::hsubpd: case Mnemonic::movddup: case Mnemonic::movsldup: case Mnemonic::movshdup: case Mnemonic::lddqu:
		sse3(instruction);
		return;
	case Mnemonic::pshufb: case Mnemonic::phaddw: case Mnemonic::phaddd: case Mnemonic::phaddsw: case Mnemonic::pmaddubsw:
	case Mnemonic::phsubw: case Mnemonic::phsubd: case Mnemonic::phsubsw: case Mnemonic::psignb: case Mnemonic::psignw:
	case Mnemonic::psignd: case Mnemonic::pmulhrsw: case Mnemonic::pabsb: case Mnemonic::pabsw: case Mnemonic::pabsd:
	case Mnemonic::palignr:
		ssse3(instruction);
		return;
	case Mnemonic::pblendvb: case Mnemonic::blendvps: case Mnemonic::blendvpd: case Mnemonic::ptest: case Mnemonic::pmovsxbw:
	case Mnemonic::pmovsxbd: case Mnemonic::pmovsxbq: case Mnemonic::pmovsxwd: case Mnemonic::pmovsxwq: case Mnemonic::pmovsxdq:
	case Mnemonic::pmovzxbw: case Mnemonic::pmovzxbd: case Mnemonic::pmovzxbq: case Mnemonic::pmovzxwd: case Mnemonic::pmovzxwq:
	case Mnemonic::pmovzxdq: case Mnemonic::pmuldq: case Mnemonic::pcmpeqq: case Mnemonic::movntdqa: case Mnemonic::packusdw:
	case Mnemonic::pminsb: case Mnemonic::pminsd: case Mnemonic::pminuw: case Mnemonic::pminud: case Mnemonic::pmaxsb:
	case Mnemonic::pmaxsd: case Mnemonic::pmaxuw: case Mnemonic::pmaxud: case Mnemonic::pmulld: case Mnemonic::phminposuw:
	case Mnemonic::roundps: case Mnemonic::roundpd: case Mnemonic::roundss: case Mnemonic::roundsd: case Mnemonic::blendps:
	case Mnemonic::blendpd: case Mnemonic::pblendw: case Mnemonic::pextrb: case Mnemonic::pextrw: case Mnemonic::pextrd:
	case Mnemonic::pextrq: case Mnemonic::extractps: case Mnemonic::pinsrb: case Mnemonic::pinsrw: case Mnemonic::pinsrd:
	case Mnemonic::pinsrq: case Mnemonic::insertps: case Mnemonic::dpps: case Mnemonic::dppd: case Mnemonic::mpsadbw:
		sse41(instruction);
		return;
	case Mnemonic::pcmpgtq: {
		const uint8_t source = vector_source(operands[1], vs0);
		a.simd(simd::cmgt_16b | simd::size_d, operands[0].reg, operands[0].reg, source);
		return;
	}
	case Mnemonic::crc32: {
		// x86's crc32 is CRC-32C, bit-reflected, as Arm's crc32c: the accumulator in the low 32 bits, the result zero-extended.
		const unsigned size = operands[1].size;
		const uint8_t data = read(operands[1], s1);
		const uint8_t accumulator = host_register[operands[0].reg];
		static constexpr uint32_t forms[] = {0x1ac05000, 0x1ac05400, 0x1ac05800, 0x9ac05c00};
		const uint32_t opcode = forms[size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3];
		a.simd(opcode, s0, accumulator, data);
		a.mov(accumulator, s0, false);
		return;
	}
	case Mnemonic::popcnt: {
		// cnt on the bytes and addv; ZF from the source, CF, OF, SF, AF and PF clear.
		const unsigned size = operands[1].size;
		const uint8_t value = read(operands[1], s1, size == 8);
		a.simd(simd::fmov_d_x, vs0, value);
		a.simd(simd::cnt_8b, vs0, vs0);
		a.simd(simd::addv_8b, vs0, vs0);
		a.umov(s0, vs0, 0, 1);
		if (flags_wanted(all_flags)) {
			a.sub_imm(s2, value, 0, false, size == 8, true);
			a.cset(s2, Cond::eq);
			a.lsl_imm(s2, s2, 30, false);
			// C set: the inverse of a clear CF.
			a.orr_imm(s2, s2, uint64_t(1) << 29, false);
			a.msr_nzcv(s2);
			carry_inverted = true;
			if (flags_wanted(PF | AF)) {
				a.mov_imm(parity_register, 1);
				a.mov(adjust_register, parity_register);
			}
		}
		write(operands[0], s0);
		return;
	}
	case Mnemonic::packsswb: case Mnemonic::packssdw: case Mnemonic::packuswb: case Mnemonic::punpcklbw: case Mnemonic::punpcklwd:
	case Mnemonic::punpckldq: case Mnemonic::punpcklqdq: case Mnemonic::punpckhbw: case Mnemonic::punpckhwd: case Mnemonic::punpckhdq:
	case Mnemonic::punpckhqdq: case Mnemonic::unpcklps: case Mnemonic::unpcklpd: case Mnemonic::unpckhps: case Mnemonic::unpckhpd:
	case Mnemonic::pshufd: case Mnemonic::pshuflw: case Mnemonic::pshufhw: case Mnemonic::shufps: case Mnemonic::shufpd:
		sse_shuffle(instruction);
		return;
	case Mnemonic::ldmxcsr:
		// The whole value is MXCSR; FPCR and FPSR follow it.
		load_memory(s0, operands[0], 4);
		a.str(s0, state_register, state_offset::mxcsr, 4);
		load_mxcsr(a, s0, s1, s2);
		return;
	case Mnemonic::stmxcsr:
		store_mxcsr_flags(a, s0, s1, s2);
		store_memory(s0, operands[0], 4);
		return;
	case Mnemonic::mov: {
		const Operand &target = operands[0], &source = operands[1];
		if (target.kind == OperandKind::reg && target.reg < high_byte_base && target.size >= 4) {
			const uint8_t reg = host_register[target.reg];
			if (source.kind == OperandKind::mem) {
				load_memory(reg, source, target.size);
				return;
			}
			const uint8_t value = read(source, reg);
			if (value != reg)
				a.mov(reg, value, target.size == 8);
			else if (target.size == 4 && source.kind == OperandKind::reg)
				a.mov(reg, value, false);
			return;
		}
		write(target, read(source, s0));
		return;
	}
	case Mnemonic::movzx: case Mnemonic::movsx: case Mnemonic::movsxd: {
		// Into a full-width register directly: the W or X form of the extension writes all of it.
		const Operand &target = operands[0];
		const bool direct = target.size >= 4 && target.reg < high_byte_base;
		const uint8_t value = direct ? host_register[target.reg] : s0;
		if (instruction.mnemonic == Mnemonic::movzx) {
			const uint8_t read_into = read(operands[1], value, false);
			if (direct && read_into != value)
				a.mov(value, read_into, false);
		} else
			read_signed(operands[1], value, target.size);
		if (!direct)
			write(target, value);
		return;
	}
	case Mnemonic::lea: effective_address(s0, operands[1], operands[0].size == 8); write(operands[0], s0); return;
	case Mnemonic::push: push_value(read(operands[0], s0)); return;
	case Mnemonic::pop: pop_value(s0); write(operands[0], s0); return;
	case Mnemonic::nop: return;
	case Mnemonic::cbw: {
		const uint8_t size = instruction.operand_size;
		const uint8_t reg = host_register[rax];
		if (size == 8)
			a.sbfx(reg, reg, 0, 32);
		else if (size == 4)
			a.sbfx(reg, reg, 0, 16, false);
		else {
			a.sbfx(s0, reg, 0, 8, false);
			a.bfi(reg, s0, 0, 16);
		}
		return;
	}
	case Mnemonic::cwd: {
		const uint8_t size = instruction.operand_size;
		if (size == 8)
			a.asr_imm(host_register[rdx], host_register[rax], 63);
		else if (size == 4)
			a.asr_imm(host_register[rdx], host_register[rax], 31, false);
		else {
			a.sbfx(s0, host_register[rax], 15, 1, false);
			a.bfi(host_register[rdx], s0, 0, 16);
		}
		return;
	}
	case Mnemonic::leave:
		a.mov(host_register[rsp], host_register[rbp]);
		pop_value(host_register[rbp]);
		return;
	case Mnemonic::setcc:
		if ((instruction.condition >> 1) == 5) {
			// PF: even parity of the low byte of the last result.
			a.eor(s0, parity_register, parity_register, Shift::lsr, 4, false);
			a.eor(s0, s0, s0, Shift::lsr, 2, false);
			a.eor(s0, s0, s0, Shift::lsr, 1, false);
			a.and_imm(s0, s0, 1, false);
			if (!(instruction.condition & 1))
				a.eor_imm(s0, s0, 1, false);
		} else
			a.cset(s0, condition(instruction.condition));
		write(operands[0], s0);
		return;
	case Mnemonic::cmovcc: {
		const Operand &target = operands[0];
		const uint8_t value = read(operands[1], s1);
		const uint8_t current_value = read(target, s0);
		const Cond cond = condition(instruction.condition);
		if (target.size >= 4)
			a.csel(host_register[target.reg], value, current_value, cond, target.size == 8);
		else {
			a.csel(s2, value, current_value, cond, false);
			write(target, s2);
		}
		return;
	}
	case Mnemonic::jcc: {
		normalize_carry();
		const uint64_t target = static_cast<uint64_t>(operands[0].value);
		if ((instruction.condition >> 1) == 5) {
			a.eor(s0, parity_register, parity_register, Shift::lsr, 4, false);
			a.eor(s0, s0, s0, Shift::lsr, 2, false);
			a.eor(s0, s0, s0, Shift::lsr, 1, false);
			const Assembler::Label not_taken = a.label();
			// Bit 0 clear: even parity, PF set.
			if (instruction.condition & 1)
				a.tbz(s0, 0, not_taken);
			else
				a.tbnz(s0, 0, not_taken);
			exit_to(target);
			a.bind(not_taken);
			exit_to(next_address);
			return;
		}
		const Assembler::Label taken = a.label();
		a.b(condition(instruction.condition), taken);
		exit_to(next_address);
		a.bind(taken);
		exit_to(target);
		return;
	}
	case Mnemonic::jmp:
		normalize_carry();
		if (operands[0].kind == OperandKind::rel) {
			exit_to(static_cast<uint64_t>(operands[0].value));
			return;
		}
		a.mov(address_register, read(operands[0], address_register));
		exit_indirect();
		return;
	case Mnemonic::call: {
		normalize_carry();
		if (operands[0].kind == OperandKind::rel) {
			a.mov_imm(s0, next_address);
			push_value(s0);
			exit_to(static_cast<uint64_t>(operands[0].value));
			return;
		}
		// The target first: its operand may name RSP.
		const uint8_t target = read(operands[0], s3);
		a.mov(s3, target);
		a.mov_imm(s0, next_address);
		push_value(s0);
		a.mov(address_register, s3);
		exit_indirect();
		return;
	}
	case Mnemonic::ret:
		normalize_carry();
		pop_value(address_register);
		if (instruction.count == 1 && operands[0].value)
			a.add_imm(host_register[rsp], host_register[rsp], static_cast<uint32_t>(operands[0].value & 0xffff));
		exit_indirect();
		return;
	case Mnemonic::int3: normalize_carry(); exit_with(ExitReason::breakpoint, next_address - instruction.length); return;
	case Mnemonic::syscall: normalize_carry(); exit_with(ExitReason::syscall, next_address); return;
	case Mnemonic::hlt: case Mnemonic::ud2:
		normalize_carry();
		exit_with(ExitReason::fault, next_address - instruction.length);
		return;
	default: throw std::logic_error("no translation for a supported instruction");
	}
}

TranslatedBlock BlockTranslator::run(std::span<const uint8_t> code, uint64_t address)
{
	std::vector<Instruction> instructions;
	size_t at = 0;
	bool ended = false;
	while (at < code.size() && instructions.size() < options.max_instructions) {
		const Instruction instruction = decode(code.subspan(at), address + at);
		if (instruction.mnemonic == Mnemonic::invalid || !supported(instruction))
			break;
		instructions.push_back(instruction);
		at += instruction.length;
		if (ends_block(instruction.mnemonic)) {
			ended = true;
			break;
		}
	}
	// Which flags each instruction must produce: those a later one reads before another defines them, all at the end.
	std::vector<uint32_t> live(instructions.size());
	uint32_t needed = all_flags;
	for (size_t i = instructions.size(); i-- > 0;) {
		live[i] = needed;
		const FlagEffect effect = flag_effect(instructions[i]);
		needed = (needed & ~effect.defines) | effect.uses;
	}
	block.guest_start = address;
	uint64_t rip = address;
	for (size_t i = 0; i < instructions.size(); ++i) {
		current = &instructions[i];
		next_address = rip + current->length;
		live_after = live[i];
		translate(*current);
		rip = next_address;
	}
	if (!ended) {
		normalize_carry();
		// The next instruction is not translated: x87 goes to the runtime's x87 core, cpuid and xgetbv to the runtime's answers,
		// unsupported or invalid stops here, a full block falls through to the next.
		const Instruction next = at < code.size() ? decode(code.subspan(at), rip) : Instruction{};
		if (instructions.size() >= options.max_instructions && next.mnemonic != Mnemonic::invalid && supported(next))
			exit_to(rip);
		else if (next.mnemonic != Mnemonic::invalid && x87_supported(next))
			exit_with(ExitReason::x87, rip);
		else if (next.mnemonic == Mnemonic::cpuid || next.mnemonic == Mnemonic::xgetbv || string_compare_supported(next))
			exit_with(ExitReason::cpuid, rip);
		else
			exit_with(ExitReason::unsupported, rip);
	}
	finish_exits();
	a.finish();
	block.code = std::move(a.words);
	block.guest_size = static_cast<uint32_t>(rip - address);
	block.instructions = static_cast<uint32_t>(instructions.size());
	++stats.blocks;
	stats.guest_instructions += instructions.size();
	stats.host_words += block.code.size();
	return std::move(block);
}

}

bool translates(const Instruction &instruction)
{
	return supported(instruction) || x87_supported(instruction) || instruction.mnemonic == Mnemonic::cpuid ||
		instruction.mnemonic == Mnemonic::xgetbv || string_compare_supported(instruction);
}

TranslatedBlock translate_block(std::span<const uint8_t> code, uint64_t address, const TranslatorOptions &options,
	TranslationStats &stats)
{
	BlockTranslator translator(options, stats);
	return translator.run(code, address);
}

std::vector<uint32_t> enter_trampoline(bool restore_host_fpcr)
{
	Assembler a;
	// enter(x0 = state, x1 = code). The frame keeps the host's callee-saved registers: x19-x28, x29, x30 and the low halves of v8-v15
	// AAPCS64 asks for, kept whole here since x64 code expects all of XMM6-XMM15 preserved across calls.
	a.stp_pre(29, 30, a64::sp, -224);
	a.mov_sp(29, a64::sp);
	a.stp(19, 20, a64::sp, 16);
	a.stp(21, 22, a64::sp, 32);
	a.stp(23, 24, a64::sp, 48);
	a.stp(25, 26, a64::sp, 64);
	a.stp(27, 28, a64::sp, 80);
	a.stp_q(8, 9, a64::sp, 96);
	a.stp_q(10, 11, a64::sp, 128);
	a.stp_q(12, 13, a64::sp, 160);
	a.stp_q(14, 15, a64::sp, 192);
	a.mov(state_register, 0);
	// MSR FPCR costs more than the rest of the entry: it is skipped when FPCR already holds what MXCSR asks for, as it does in a vCPU
	// from the second run on. NZCV is free here: it is loaded below.
	a.mrs_fpcr(s0);
	if (restore_host_fpcr)
		a.str(s0, state_register, state_offset::host_fpcr, 8);
	a.ldr(s1, state_register, state_offset::mxcsr, 4);
	fpcr_from_mxcsr(a, s2, s1, s3);
	const Assembler::Label same_fpcr = a.label(), same_fpsr = a.label();
	a.sub(a64::zr, s0, s2, Shift::lsl, 0, true, true);
	a.b(Cond::eq, same_fpcr);
	a.msr_fpcr(s2);
	a.bind(same_fpcr);
	fpsr_from_mxcsr(a, s2, s1, s3);
	a.mrs_fpsr(s0);
	a.sub(a64::zr, s0, s2, Shift::lsl, 0, true, true);
	a.b(Cond::eq, same_fpsr);
	a.msr_fpsr(s2);
	a.bind(same_fpsr);
	load_vectors(a);
	a.mov_sp(s0, a64::sp);
	a.str(s0, state_register, state_offset::host_frame, 8);
	a.mov(address_register, 1);
	a.ldr(s0, state_register, state_offset::nzcv, 8);
	a.msr_nzcv(s0);
	a.ldr(parity_register, state_register, state_offset::parity_source, 8);
	a.ldr(adjust_register, state_register, state_offset::adjust_source, 8);
	for (uint8_t reg = 0; reg < 16; ++reg)
		a.ldr(host_register[reg], state_register, static_cast<int64_t>(state_offset::gpr + 8 * reg), 8);
	a.br(address_register);
	a.finish();
	return a.words;
}

std::vector<uint32_t> exit_trampoline(bool restore_host_fpcr)
{
	Assembler a;
	// w9: the reason; x10: the exit's detail.
	a.str(s0, state_register, state_offset::exit_reason, 4);
	a.str(s1, state_register, state_offset::exit_info, 8);
	for (uint8_t reg = 0; reg < 16; ++reg)
		a.str(host_register[reg], state_register, static_cast<int64_t>(state_offset::gpr + 8 * reg), 8);
	a.mrs_nzcv(s0);
	a.str(s0, state_register, state_offset::nzcv, 8);
	a.str(parity_register, state_register, state_offset::parity_source, 8);
	a.str(adjust_register, state_register, state_offset::adjust_source, 8);
	store_vectors(a);
	store_mxcsr_flags(a, s0, s2, s3);
	if (restore_host_fpcr) {
		a.ldr(s0, state_register, state_offset::host_fpcr, 8);
		a.msr_fpcr(s0);
	}
	a.ldr(s0, state_register, state_offset::host_frame, 8);
	a.mov_sp(a64::sp, s0);
	a.ldr(0, state_register, state_offset::exit_reason, 4);
	a.ldp(19, 20, a64::sp, 16);
	a.ldp(21, 22, a64::sp, 32);
	a.ldp(23, 24, a64::sp, 48);
	a.ldp(25, 26, a64::sp, 64);
	a.ldp(27, 28, a64::sp, 80);
	a.ldp_q(8, 9, a64::sp, 96);
	a.ldp_q(10, 11, a64::sp, 128);
	a.ldp_q(12, 13, a64::sp, 160);
	a.ldp_q(14, 15, a64::sp, 192);
	a.ldp_post(29, 30, a64::sp, 224);
	a.ret();
	a.finish();
	return a.words;
}

namespace {

// Goes on with translated code at the guest address in address_register through the lookup table, or exits with a lookup miss.
void branch_to_guest(Assembler &a)
{
	const Assembler::Label miss = a.label();
	a.ldr(s5, state_register, state_offset::lookup_table, 8);
	a.ubfx(s0, address_register, 0, lookup_bits);
	a.add(s5, s5, s0, Shift::lsl, 4);
	a.ldp(s1, s2, s5, 0);
	a.eor(s1, s1, address_register);
	a.cbnz(s1, miss);
	a.br(s2);
	a.bind(miss);
	a.str(address_register, state_register, state_offset::rip, 8);
	a.mov_imm(s0, static_cast<uint32_t>(ExitReason::lookup_miss), false);
	a.ldr(address_register, state_register, state_offset::exit_trampoline, 8);
	a.br(address_register);
}

}

std::vector<uint32_t> native_thunk(uint64_t function, unsigned integer_arguments)
{
	if (integer_arguments > 8)
		throw std::invalid_argument("native thunks take up to eight integer arguments");
	Assembler a;
	// RSP for a native function that calls back into the guest.
	a.str(host_register[rsp], state_register, static_cast<int64_t>(state_offset::gpr + 8 * rsp), 8);
	// Arguments five to eight: on the x64 stack above the return address and the 32-byte home area; AAPCS64 takes them in x4-x7.
	for (unsigned i = 4; i < integer_arguments; ++i)
		a.ldr(static_cast<uint8_t>(i), host_register[rsp], static_cast<int64_t>(8 + 32 + 8 * (i - 4)), 8);
	// The function sees the guest's XMM registers and MXCSR in CpuState and runs with the host's FPCR. XMM6-XMM15 are callee-saved for
	// x64 and only the low halves of v8-v15 are for AAPCS64: they come back from CpuState, where a guest callback leaves them as it
	// found them. XMM0 is the function's floating-point result.
	store_vectors(a);
	store_mxcsr_flags(a, s0, s1, s2);
	a.ldr(s0, state_register, state_offset::host_fpcr, 8);
	a.msr_fpcr(s0);
	a.mov_imm(address_register, function);
	a.blr(address_register);
	a.mov(host_register[rax], 0);
	load_vectors(a, 6);
	load_mxcsr(a, s0, s1, s2);
	// A native function that ended the guest (ExitProcess) asked for a stop; otherwise return as ret does.
	const Assembler::Label stop = a.label();
	a.ldr(s0, state_register, state_offset::stop_requested, 4);
	a.cbnz(s0, stop, false);
	a.ldr_post(address_register, host_register[rsp], 8);
	branch_to_guest(a);
	a.bind(stop);
	a.ldr_post(address_register, host_register[rsp], 8);
	a.str(address_register, state_register, state_offset::rip, 8);
	a.mov_imm(s0, static_cast<uint32_t>(ExitReason::stopped), false);
	a.ldr(address_register, state_register, state_offset::exit_trampoline, 8);
	a.br(address_register);
	a.finish();
	return a.words;
}

// Native ARM64EC code shares FPCR with the x64 code around it: the rounding mode and FZ are the one state that MXCSR and FPCR are on
// Windows. AH and FIZ are this translator's own, for x86's NaNs, min and max and DAZ; Arm code expects them clear, so they are cleared
// on the way into native code and set again (FIZ from MXCSR.DAZ) on the way back.
namespace {

void clear_x86_fpcr(Assembler &a)
{
	a.mrs_fpcr(s5);
	a.and_imm(s5, s5, ~uint64_t(3));
	a.msr_fpcr(s5);
}

void set_x86_fpcr(Assembler &a)
{
	a.mrs_fpcr(s5);
	a.orr_imm(s5, s5, 2);
	a.ldr(s1, state_register, state_offset::mxcsr, 4);
	a.ubfx(s1, s1, 6, 1, false);
	a.bfi(s5, s1, 0, 1);
	a.msr_fpcr(s5);
}

}

std::vector<uint32_t> arm64ec_entry(uint64_t function, uint64_t thunk)
{
	Assembler a;
	clear_x86_fpcr(a);
	// As Windows enters an entry thunk: the x64 return address in x30, where the thunk keeps it for __os_arm64x_dispatch_ret, RSP past
	// it in sp and in x4, from which the thunk reads stack arguments, and the function in x9.
	a.ldr_post(30, host_register[rsp], 8);
	a.mov_sp(a64::sp, host_register[rsp]);
	a.mov(4, host_register[rsp]);
	a.mov_imm(9, function);
	a.mov_imm(address_register, thunk);
	a.br(address_register);
	a.finish();
	return a.words;
}

std::vector<uint32_t> arm64ec_resume(uint64_t address)
{
	Assembler a;
	// The x64 ret popped the address; native code finds its stack where it left it, RSP past the address, and RAX in x8.
	clear_x86_fpcr(a);
	a.mov_sp(a64::sp, host_register[rsp]);
	a.mov_imm(address_register, address);
	a.br(address_register);
	a.finish();
	return a.words;
}

std::vector<uint32_t> arm64ec_dispatch_ret()
{
	Assembler a;
	// sp, not x28: a callback in between moved RSP, and only sp comes back through native code.
	a.mov_sp(host_register[rsp], a64::sp);
	a.mov(address_register, 30);
	set_x86_fpcr(a);
	branch_to_guest(a);
	a.finish();
	return a.words;
}

std::vector<uint32_t> arm64ec_dispatch_call()
{
	Assembler a;
	// One word, the native return address, pushed below sp: the exit thunk's home area and stack arguments start right above it.
	a.sub_imm(host_register[rsp], a64::sp, 8);
	a.str(30, host_register[rsp], 0, 8);
	a.mov(address_register, 9);
	set_x86_fpcr(a);
	branch_to_guest(a);
	a.finish();
	return a.words;
}

std::vector<uint32_t> arm64ec_check_icall(std::span<const std::pair<uint64_t, uint64_t>> native)
{
	Assembler a;
	const Assembler::Label is_native = a.label();
	for (const auto &[start, end] : native) {
		const Assembler::Label next = a.label();
		a.mov_imm(16, start);
		a.sub(a64::zr, 11, 16, Shift::lsl, 0, true, true);
		a.b(Cond::cc, next);
		a.mov_imm(16, end);
		a.sub(a64::zr, 11, 16, Shift::lsl, 0, true, true);
		a.b(Cond::cc, is_native);
		a.bind(next);
	}
	// x64 code: the caller calls the exit thunk, which finds the target in x9.
	a.mov(9, 11);
	a.mov(11, 10);
	a.bind(is_native);
	a.ret();
	a.finish();
	return a.words;
}

std::vector<uint32_t> arm64ec_unsupported()
{
	Assembler a;
	a.brk(0xec);
	a.finish();
	return a.words;
}

std::vector<uint32_t> return_stub()
{
	Assembler a;
	a.mov_imm(s0, static_cast<uint32_t>(ExitReason::returned), false);
	a.ldr(address_register, state_register, state_offset::exit_trampoline, 8);
	a.br(address_register);
	a.finish();
	return a.words;
}

}
