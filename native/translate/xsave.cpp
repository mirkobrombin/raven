#include "xsave.h"

#include "cpuid.h"

#include <cstring>

namespace raven::translate {

namespace {

// The standard format: the legacy area as FXSAVE lays it out (x87 state, MXCSR at 24, MXCSR_MASK at 28, ST0-ST7 at 32, XMM0-XMM15 at
// 160), the header at 512 (XSTATE_BV, then XCOMP_BV), the upper halves of YMM0-YMM15 at 576.
constexpr uint64_t mxcsr_at = 24, mxcsr_mask_at = 28, registers_at = 32, xmm_at = 160, header_at = 512, ymm_at = 576;
// The bits of MXCSR that may be set: all 16, DAZ included.
constexpr uint32_t mxcsr_mask = 0xffff;

uint64_t address_of(const CpuState &state, const Instruction &instruction, uint64_t next)
{
	const Operand &operand = instruction.operands[0];
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

}

bool xsave_supported(const Instruction &instruction)
{
	return instruction.mnemonic == Mnemonic::xsave || instruction.mnemonic == Mnemonic::xrstor;
}

bool xsave(CpuState &state, GuestMemory &memory, const Instruction &instruction, uint64_t next)
{
	const auto xcr0 = xgetbv(0);
	if (!xcr0)
		return false;
	const uint64_t at = address_of(state, instruction, next);
	if (at % 64)
		return false;
	const uint64_t requested = ((state.gpr[rdx] & 0xffffffff) << 32 | (state.gpr[rax] & 0xffffffff)) & *xcr0;
	// Without REX.W the x87 instruction and data pointers are 32-bit offsets, each followed by a selector (zero here).
	const bool wide = instruction.operand_size == 8;
	uint8_t legacy[xmm_at];
	if (instruction.mnemonic == Mnemonic::xsave) {
		if (requested & 1) {
			X87State x87 = state.x87;
			if (!wide) {
				x87.fip &= 0xffffffff;
				x87.fdp &= 0xffffffff;
			}
			std::memcpy(legacy, &x87, 24);
			memory.write(at, legacy, 24);
			for (unsigned i = 0; i < 8; ++i) {
				uint8_t value[10];
				std::memcpy(value, &x87.st[i][0], 8);
				const uint16_t top = static_cast<uint16_t>(x87.st[i][1]);
				std::memcpy(value + 8, &top, 2);
				memory.write(at + registers_at + 16 * i, value, 10);
			}
		}
		if (requested & 6) {
			const uint32_t control[2] = {state.mxcsr, mxcsr_mask};
			memory.write(at + mxcsr_at, control, 8);
		}
		if (requested & 2)
			memory.write(at + xmm_at, state.xmm, sizeof state.xmm);
		if (requested & 4)
			memory.write(at + ymm_at, state.ymm_high, sizeof state.ymm_high);
		// XSTATE_BV: the requested components as in use (x86 may report a component in its initial state as in use), the others as
		// they were.
		uint64_t in_use = 0;
		memory.read(at + header_at, &in_use, 8);
		in_use = (in_use & ~requested) | requested;
		memory.write(at + header_at, &in_use, 8);
		return true;
	}
	// xrstor: XSTATE_BV within XCR0 and XCOMP_BV and the next 8 bytes zero (the standard format), MXCSR without reserved bits.
	uint64_t header[3];
	memory.read(at + header_at, header, sizeof header);
	uint32_t mxcsr = 0;
	memory.read(at + mxcsr_at, &mxcsr, 4);
	if ((header[0] & ~*xcr0) || header[1] || header[2] || ((requested & 6) && (mxcsr & ~mxcsr_mask)))
		return false;
	// Every read before the first write to the state: a read that faults (the area may cross into an unmapped page) leaves it as
	// it was.
	const uint64_t present = header[0];
	X87State x87;
	alignas(16) uint64_t xmm[16][2] = {}, ymm_high[16][2] = {};
	if ((requested & 1) && (present & 1)) {
		memory.read(at, legacy, 24);
		std::memcpy(&x87, legacy, 24);
		if (!wide) {
			x87.fip &= 0xffffffff;
			x87.fdp &= 0xffffffff;
		}
		x87.reserved = 0;
		x87.unused = 0;
		for (unsigned i = 0; i < 8; ++i) {
			uint8_t value[10];
			memory.read(at + registers_at + 16 * i, value, 10);
			uint16_t top = 0;
			std::memcpy(&x87.st[i][0], value, 8);
			std::memcpy(&top, value + 8, 2);
			x87.st[i][1] = top;
		}
	} else {
		// The initial x87 state, as finit leaves it.
		x87.fcw = 0x37f;
	}
	if ((requested & 2) && (present & 2))
		memory.read(at + xmm_at, xmm, sizeof xmm);
	if ((requested & 4) && (present & 4))
		memory.read(at + ymm_at, ymm_high, sizeof ymm_high);
	// A component requested but clear in XSTATE_BV gets its initial state: the zeros above.
	if (requested & 1)
		state.x87 = x87;
	if (requested & 6)
		state.mxcsr = mxcsr;
	if (requested & 2)
		std::memcpy(state.xmm, xmm, sizeof xmm);
	if (requested & 4)
		std::memcpy(state.ymm_high, ymm_high, sizeof ymm_high);
	return true;
}

}
