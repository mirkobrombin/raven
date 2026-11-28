#include "divide.h"

namespace raven::translate {

namespace {

uint64_t source_value(const CpuState &state, const GuestMemory &memory, const Instruction &instruction, uint64_t next)
{
	const Operand &operand = instruction.operands[0];
	const unsigned size = operand.size;
	uint64_t value = 0;
	if (operand.kind == OperandKind::reg) {
		value = operand.reg >= high_byte_base ? state.gpr[operand.reg - high_byte_base] >> 8 : state.gpr[operand.reg];
	} else {
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
		memory.read(at, &value, size);
	}
	return size == 8 ? value : value & ((uint64_t(1) << (size * 8)) - 1);
}

void put(uint64_t &reg, uint64_t value, unsigned size, unsigned shift = 0)
{
	if (size == 4) {
		reg = static_cast<uint32_t>(value);
		return;
	}
	if (size == 8) {
		reg = value;
		return;
	}
	const uint64_t mask = ((uint64_t(1) << (size * 8)) - 1) << shift;
	reg = (reg & ~mask) | ((value << shift) & mask);
}

}

bool divide_supported(const Instruction &instruction)
{
	return instruction.mnemonic == Mnemonic::div || instruction.mnemonic == Mnemonic::idiv;
}

bool divide(CpuState &state, const GuestMemory &memory, const Instruction &instruction, uint64_t next)
{
	using u128 = unsigned __int128;
	const unsigned size = instruction.operands[0].size, bits = size * 8;
	const uint64_t divisor = source_value(state, memory, instruction, next);
	// The dividend, twice the width: AX for 8 bits, else RDX:RAX of the width.
	const uint64_t mask = size == 8 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
	const u128 dividend = size == 1 ? u128(state.gpr[rax] & 0xffff) : (u128(state.gpr[rdx] & mask) << bits) | (state.gpr[rax] & mask);
	if (divisor == 0)
		return false;
	u128 quotient, remainder;
	if (instruction.mnemonic == Mnemonic::div) {
		quotient = dividend / divisor;
		remainder = dividend % divisor;
		if (quotient > mask)
			return false;
	} else {
		// Magnitudes, then the signs: the quotient's is the xor of both, the remainder's the dividend's.
		const bool dividend_negative = (dividend >> (2 * bits - 1)) & 1, divisor_negative = (divisor >> (bits - 1)) & 1;
		const u128 wide_mask = bits == 64 ? ~u128(0) : (u128(1) << (2 * bits)) - 1;
		const u128 magnitude = dividend_negative ? (~dividend + 1) & wide_mask : dividend;
		const uint64_t divisor_magnitude = divisor_negative ? (~divisor + 1) & mask : divisor;
		quotient = magnitude / divisor_magnitude;
		remainder = magnitude % divisor_magnitude;
		const u128 limit = u128(1) << (bits - 1);
		if (dividend_negative != divisor_negative ? quotient > limit : quotient >= limit)
			return false;
		if (dividend_negative != divisor_negative)
			quotient = ~quotient + 1;
		if (dividend_negative)
			remainder = ~remainder + 1;
	}
	if (size == 1) {
		put(state.gpr[rax], uint64_t(quotient), 1);
		put(state.gpr[rax], uint64_t(remainder), 1, 8);
	} else {
		put(state.gpr[rax], uint64_t(quotient), size);
		put(state.gpr[rdx], uint64_t(remainder), size);
	}
	return true;
}

}
