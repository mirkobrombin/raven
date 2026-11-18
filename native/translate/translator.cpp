#include "translator.h"

#include "a64_emitter.h"
#include "cpu_state.h"
#include "x86_decoder.h"

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

constexpr std::array<uint8_t, 16> host_register = {8, 0, 1, 27, 28, 24, 25, 26, 2, 3, 4, 5, 19, 20, 21, 22};
constexpr uint8_t state_register = 23;
constexpr uint8_t parity_register = 13;
constexpr uint8_t adjust_register = 14;
constexpr uint8_t s0 = 9, s1 = 10, s2 = 11, s3 = 12, s4 = 15, address_register = 16, s5 = 17;
// Holds the last RIP-relative address the block computed, so a loop over one global computes it once.
constexpr uint8_t constant_register = 7;

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

bool all_registers_or_memory(const Instruction &instruction)
{
	for (uint8_t i = 0; i < instruction.count; ++i)
		if (instruction.operands[i].kind == OperandKind::mem && instruction.operands[i].segment != Segment::none &&
			instruction.operands[i].segment != Segment::gs && instruction.operands[i].segment != Segment::fs)
			return false;
	return true;
}

// Whether the translator has code for the instruction; the block ends before any it has not.
bool supported(const Instruction &instruction)
{
	if (instruction.rep || instruction.repne || !all_registers_or_memory(instruction))
		return false;
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
	case Mnemonic::cmpxchg: case Mnemonic::xadd: return first.kind == OperandKind::mem;
	case Mnemonic::imul: return instruction.count >= 2 && size >= 4;
	case Mnemonic::shl: case Mnemonic::shr: case Mnemonic::sar:
		// Narrow shifts only by a count inside their width: beyond it CF is undefined and the result needs its own code.
		return size >= 4 || (instruction.operands[1].kind == OperandKind::imm && shift_count(instruction) < size * 8u);
	default: return false;
	}
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
		if (operand.size == 8) {
			if (may_alias)
				return reg;
			a.mov(target, reg);
		} else if (operand.size == 4)
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
	const uint8_t right = read(source, s1);
	// The result goes straight to a full-width destination register.
	const bool direct = writes && target.kind == OperandKind::reg && size >= 4 && target.reg < high_byte_base;
	const uint8_t result = direct ? host_register[target.reg] : s2;
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
		switch (mnemonic) {
		case Mnemonic::shl: a.lsl_imm(s2, value, static_cast<uint8_t>(count), !w); break;
		case Mnemonic::shr: a.lsr_imm(s2, value, static_cast<uint8_t>(count), !w); break;
		default: a.asr_imm(s2, value, static_cast<uint8_t>(count), !w); break;
		}
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
		// The accumulator takes the old value, which equals it when the exchange happened: written either way, so a 32-bit
		// accumulator clears the upper half of RAX either way, as Rosetta does.
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

void BlockTranslator::translate(const Instruction &instruction)
{
	const Operand *operands = instruction.operands.data();
	switch (instruction.mnemonic) {
	case Mnemonic::add: case Mnemonic::sub: case Mnemonic::cmp: case Mnemonic::and_: case Mnemonic::or_: case Mnemonic::xor_:
	case Mnemonic::test: case Mnemonic::adc: case Mnemonic::sbb:
		arithmetic(instruction);
		return;
	case Mnemonic::inc: case Mnemonic::dec: case Mnemonic::neg: case Mnemonic::not_: unary(instruction); return;
	case Mnemonic::shl: case Mnemonic::shr: case Mnemonic::sar: shift(instruction); return;
	case Mnemonic::imul: multiply(instruction); return;
	case Mnemonic::xchg: case Mnemonic::cmpxchg: case Mnemonic::xadd: exchange(instruction); return;
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
	case Mnemonic::movzx: write(operands[0], read(operands[1], s0, false)); return;
	case Mnemonic::movsx: case Mnemonic::movsxd: read_signed(operands[1], s0, operands[0].size); write(operands[0], s0); return;
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
		// The next instruction is not translated: unsupported or invalid stops here, a full block falls through to the next.
		const Instruction next = at < code.size() ? decode(code.subspan(at), rip) : Instruction{};
		if (instructions.size() >= options.max_instructions && next.mnemonic != Mnemonic::invalid && supported(next))
			exit_to(rip);
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

TranslatedBlock translate_block(std::span<const uint8_t> code, uint64_t address, const TranslatorOptions &options,
	TranslationStats &stats)
{
	BlockTranslator translator(options, stats);
	return translator.run(code, address);
}

std::vector<uint32_t> enter_trampoline()
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

std::vector<uint32_t> exit_trampoline()
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
	a.mov_imm(address_register, function);
	a.blr(address_register);
	a.mov(host_register[rax], 0);
	// A native function that ended the guest (ExitProcess) asked for a stop; otherwise return as ret does.
	const Assembler::Label stop = a.label();
	a.ldr(s0, state_register, state_offset::stop_requested, 4);
	a.cbnz(s0, stop, false);
	a.ldr_post(address_register, host_register[rsp], 8);
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
	a.bind(stop);
	a.ldr_post(address_register, host_register[rsp], 8);
	a.str(address_register, state_register, state_offset::rip, 8);
	a.mov_imm(s0, static_cast<uint32_t>(ExitReason::stopped), false);
	a.ldr(address_register, state_register, state_offset::exit_trampoline, 8);
	a.br(address_register);
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
