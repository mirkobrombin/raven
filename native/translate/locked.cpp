#include "locked.h"

namespace raven::translate {

namespace {

constexpr uint64_t CF = 1, ZF = 64, SF = 128, OF = 2048;

uint64_t address_of(const CpuState &state, const Instruction &instruction, const Operand &operand, uint64_t next)
{
	uint64_t at = static_cast<uint64_t>(operand.value);
	if (operand.rip_relative)
		at += next;
	else if (operand.base != no_register)
		at += state.gpr[operand.base];
	if (operand.index != no_register)
		at += state.gpr[operand.index] * operand.scale;
	if (instruction.address_size == 4)
		at = static_cast<uint32_t>(at);
	if (operand.segment == Segment::fs)
		at += state.fs_base;
	else if (operand.segment == Segment::gs)
		at += state.gs_base;
	return at;
}

uint64_t flags_of(const CpuState &state)
{
	uint64_t flags = 0;
	flags |= (state.nzcv >> 31 & 1) ? SF : 0;
	flags |= (state.nzcv >> 30 & 1) ? ZF : 0;
	flags |= (state.nzcv >> 29 & 1) ? 0 : CF;
	flags |= (state.nzcv >> 28 & 1) ? OF : 0;
	return flags;
}

// The lazy form of CpuState from x86 flags; PF and AF from the result and the adjust bit, as translated code keeps them.
void set_flags(CpuState &state, uint64_t flags, uint64_t result, bool af)
{
	state.nzcv = ((flags & SF) ? 1ull << 31 : 0) | ((flags & ZF) ? 1ull << 30 : 0) | ((flags & CF) ? 0 : 1ull << 29) |
		((flags & OF) ? 1ull << 28 : 0);
	state.parity_source = result;
	state.adjust_source = result ^ (af ? 16 : 0);
}

uint64_t read_register(const CpuState &state, uint8_t reg, unsigned size)
{
	const uint64_t value = state.gpr[reg];
	return size == 8 ? value : value & ((uint64_t(1) << (size * 8)) - 1);
}

void write_register(CpuState &state, uint8_t reg, unsigned size, uint64_t value)
{
	uint64_t &target = state.gpr[reg];
	if (size == 8)
		target = value;
	else if (size == 4)
		target = static_cast<uint32_t>(value);
	else {
		const uint64_t mask = (uint64_t(1) << (size * 8)) - 1;
		target = (target & ~mask) | (value & mask);
	}
}

}

bool locked_supported(const Instruction &instruction)
{
	const Operand &first = instruction.operands[0], &second = instruction.operands[1];
	const bool memory = first.kind == OperandKind::mem || (instruction.mnemonic == Mnemonic::xchg && second.kind == OperandKind::mem);
	if (!memory || (first.kind == OperandKind::mem ? first.size : second.size) < 2)
		return false;
	switch (instruction.mnemonic) {
	case Mnemonic::xchg: return true;
	case Mnemonic::cmpxchg: case Mnemonic::xadd: case Mnemonic::add: case Mnemonic::or_: case Mnemonic::adc: case Mnemonic::sbb:
	case Mnemonic::and_: case Mnemonic::sub: case Mnemonic::xor_: case Mnemonic::inc: case Mnemonic::dec: case Mnemonic::not_:
	case Mnemonic::neg:
		return instruction.lock;
	default: return false;
	}
}

void locked_execute(CpuState &state, GuestMemory &memory, const Instruction &instruction, uint64_t next)
{
	const Mnemonic mnemonic = instruction.mnemonic;
	const bool swapped = mnemonic == Mnemonic::xchg && instruction.operands[0].kind != OperandKind::mem;
	const Operand &target = instruction.operands[swapped ? 1 : 0];
	const Operand &source = instruction.operands[swapped ? 0 : 1];
	const unsigned size = target.size, bits = size * 8;
	const uint64_t mask = size == 8 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
	const uint64_t sign = uint64_t(1) << (bits - 1);
	const uint64_t at = address_of(state, instruction, target, next);
	uint64_t old = 0;
	memory.read(at, &old, size);
	uint64_t operand = 0;
	if (source.kind == OperandKind::reg)
		operand = read_register(state, source.reg, size);
	else if (source.kind == OperandKind::imm)
		operand = static_cast<uint64_t>(source.value) & mask;
	const uint64_t input = flags_of(state);
	// The flags of old OP value plus carry, add or subtract.
	auto arithmetic = [&](uint64_t value, bool subtract, uint64_t carry, uint64_t keep) {
		const uint64_t result = (subtract ? old - value - carry : old + value + carry) & mask;
		uint64_t flags = 0;
		const unsigned __int128 wide = subtract ? static_cast<unsigned __int128>(old) - value - carry
			: static_cast<unsigned __int128>(old) + value + carry;
		if (subtract ? (static_cast<unsigned __int128>(value) + carry > old) : (wide >> bits) != 0)
			flags |= CF;
		const uint64_t overflow = subtract ? (old ^ value) & (old ^ result) : ~(old ^ value) & (old ^ result);
		flags |= (overflow & sign) ? OF : 0;
		flags |= result == 0 ? ZF : 0;
		flags |= (result & sign) ? SF : 0;
		flags = (flags & ~keep) | (input & keep);
		set_flags(state, flags, result, ((old ^ value ^ result) & 16) != 0);
		return result;
	};
	auto logical = [&](uint64_t result) {
		set_flags(state, (result == 0 ? ZF : 0) | ((result & sign) ? SF : 0), result, false);
		return result;
	};
	uint64_t result = old;
	bool store = true;
	switch (mnemonic) {
	case Mnemonic::xchg: result = operand; write_register(state, source.reg, size, old); break;
	case Mnemonic::xadd: result = arithmetic(operand, false, 0, 0); write_register(state, source.reg, size, old); break;
	case Mnemonic::cmpxchg: {
		// The flags of cmp accumulator, old; equal: the source is stored, else the accumulator takes old (a 32-bit one zero-extended
		// only then).
		const uint64_t accumulator = read_register(state, rax, size);
		const uint64_t saved = old;
		old = accumulator;
		arithmetic(saved, true, 0, 0);
		old = saved;
		if (accumulator == saved)
			result = operand;
		else {
			store = false;
			write_register(state, rax, size, saved);
		}
		break;
	}
	case Mnemonic::add: result = arithmetic(operand, false, 0, 0); break;
	case Mnemonic::adc: result = arithmetic(operand, false, input & CF, 0); break;
	case Mnemonic::sub: result = arithmetic(operand, true, 0, 0); break;
	case Mnemonic::sbb: result = arithmetic(operand, true, input & CF, 0); break;
	case Mnemonic::inc: result = arithmetic(1, false, 0, CF); break;
	case Mnemonic::dec: result = arithmetic(1, true, 0, CF); break;
	case Mnemonic::and_: result = logical(old & operand); break;
	case Mnemonic::or_: result = logical(old | operand); break;
	case Mnemonic::xor_: result = logical((old ^ operand) & mask); break;
	case Mnemonic::not_: result = ~old & mask; break;
	default: {
		// neg: the flags of 0 - old.
		const uint64_t saved = old;
		old = 0;
		result = arithmetic(saved, true, 0, 0);
		old = saved;
		break;
	}
	}
	if (store)
		memory.write(at, &result, size);
}

}
