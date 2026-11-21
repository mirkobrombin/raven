#include "string_compare.h"

#include <cstring>

namespace raven::translate {

bool string_compare_supported(const Instruction &instruction)
{
	const Mnemonic mnemonic = instruction.mnemonic;
	return (mnemonic == Mnemonic::pcmpestri || mnemonic == Mnemonic::pcmpestrm || mnemonic == Mnemonic::pcmpistri ||
			   mnemonic == Mnemonic::pcmpistrm) &&
		(!instruction.vex || instruction.vector_size == 16);
}

void string_compare(CpuState &state, GuestMemory &memory, const Instruction &instruction, uint64_t next)
{
	const Mnemonic mnemonic = instruction.mnemonic;
	const Operand &first = instruction.operands[0], &second = instruction.operands[1];
	const unsigned control = static_cast<unsigned>(instruction.operands[2].value) & 0xff;
	uint8_t a[16], b[16];
	std::memcpy(a, state.xmm[first.reg], 16);
	if (second.kind == OperandKind::mem) {
		uint64_t at = static_cast<uint64_t>(second.value);
		if (second.rip_relative)
			at += next;
		else if (second.base != no_register)
			at += state.gpr[second.base];
		if (second.index != no_register)
			at += state.gpr[second.index] * second.scale;
		if (instruction.address_size == 4)
			at = static_cast<uint32_t>(at);
		if (second.segment == Segment::fs)
			at += state.fs_base;
		else if (second.segment == Segment::gs)
			at += state.gs_base;
		memory.read(at, b, 16);
	} else
		std::memcpy(b, state.xmm[second.reg], 16);

	// imm8 bits 1:0: unsigned bytes, unsigned words, signed bytes, signed words.
	const bool words = control & 1, is_signed = control & 2;
	const unsigned count = words ? 8 : 16;
	auto element = [&](const uint8_t *from, unsigned i) -> int32_t {
		if (words) {
			uint16_t value;
			std::memcpy(&value, from + 2 * i, 2);
			return is_signed ? int16_t(value) : int32_t(value);
		}
		return is_signed ? int8_t(from[i]) : int32_t(from[i]);
	};
	// The lengths: explicit in EAX and EDX (RAX and RDX with REX.W), as absolute values saturated to the element count; implicit,
	// up to the first null element.
	const bool explicit_lengths = mnemonic == Mnemonic::pcmpestri || mnemonic == Mnemonic::pcmpestrm;
	auto length = [&](unsigned reg, const uint8_t *from) -> unsigned {
		if (explicit_lengths) {
			int64_t value = instruction.operand_size == 8 ? int64_t(state.gpr[reg]) : int64_t(int32_t(state.gpr[reg]));
			const uint64_t magnitude = value < 0 ? uint64_t(0) - uint64_t(value) : uint64_t(value);
			return magnitude > count ? count : unsigned(magnitude);
		}
		unsigned i = 0;
		while (i < count && element(from, i) != 0)
			++i;
		return i;
	};
	const unsigned la = length(rax, a), lb = length(rdx, b);

	// imm8 bits 3:2: equal any, ranges, equal each, equal ordered. IntRes1 bit j is about element j of the second operand.
	const unsigned aggregation = control >> 2 & 3;
	uint32_t result = 0;
	for (unsigned j = 0; j < count; ++j) {
		bool bit = false;
		switch (aggregation) {
		case 0:
			for (unsigned i = 0; i < la && j < lb; ++i)
				bit |= element(a, i) == element(b, j);
			break;
		case 1:
			for (unsigned i = 0; i + 1 < count && i + 1 < la + 1 && j < lb; i += 2)
				if (i + 1 < la)
					bit |= element(b, j) >= element(a, i) && element(b, j) <= element(a, i + 1);
			break;
		case 2: {
			const bool valid_a = j < la, valid_b = j < lb;
			bit = valid_a && valid_b ? element(a, j) == element(b, j) : !valid_a && !valid_b;
			break;
		}
		default:
			bit = true;
			for (unsigned k = 0; k + j < count && bit; ++k) {
				const bool valid_a = k < la, valid_b = j + k < lb;
				if (!valid_a)
					break;
				bit = valid_b && element(a, k) == element(b, j + k);
			}
			break;
		}
		result |= uint32_t(bit) << j;
	}
	// imm8 bits 5:4: as is, negated, as is, negated only over the second operand's valid elements.
	const uint32_t all = (1u << count) - 1;
	switch (control >> 4 & 3) {
	case 1: result = ~result & all; break;
	case 3: result ^= (1u << lb) - 1; break;
	default: break;
	}

	if (mnemonic == Mnemonic::pcmpestri || mnemonic == Mnemonic::pcmpistri) {
		// imm8 bit 6: the lowest set bit, or the highest; the element count when none is set.
		unsigned index = count;
		if (result)
			index = (control & 0x40) ? 31u - unsigned(__builtin_clz(result)) : unsigned(__builtin_ctz(result));
		state.gpr[rcx] = index;
	} else {
		// imm8 bit 6: the bits, zero-extended, or each element all ones where its bit is set.
		uint8_t mask[16] = {};
		if (control & 0x40) {
			const unsigned size = words ? 2 : 1;
			for (unsigned j = 0; j < count; ++j)
				if (result >> j & 1)
					std::memset(mask + j * size, 0xff, size);
		} else {
			mask[0] = uint8_t(result);
			mask[1] = uint8_t(result >> 8);
		}
		std::memcpy(state.xmm[0], mask, 16);
		if (instruction.vex)
			state.ymm_high[0][0] = state.ymm_high[0][1] = 0;
	}
	// CF: any bit set; ZF: the second operand is shorter than the register; SF: the first is; OF: bit 0. AF and PF clear.
	const bool cf = result != 0, zf = lb < count, sf = la < count, of = result & 1;
	state.nzcv = (sf ? 1ull << 31 : 0) | (zf ? 1ull << 30 : 0) | (cf ? 0 : 1ull << 29) | (of ? 1ull << 28 : 0);
	state.parity_source = 1;
	state.adjust_source = 1;
}

}
