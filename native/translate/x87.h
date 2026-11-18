#pragma once

#include "cpu_state.h"
#include "guest_memory.h"
#include "x86_decoder.h"

#include <cstdint>

namespace raven::translate {

// x87 runs in the host, not in translated code: a block ends before an x87 instruction with ExitReason::x87, and the runtime runs it
// and the x87 instructions after it here, on CpuState::x87, then goes back to translated code. The arithmetic is done on integers
// with the 80-bit format's 64-bit significand and 15-bit exponent and every precision and rounding control, so results are those of
// an x86 CPU to the bit (docs/macos/x87.md has the measurements); no host floating point is used.

// Whether the core runs instruction.
bool x87_supported(const Instruction &instruction);

enum class X87Result : uint8_t {
	done,
	// A waiting instruction found an unmasked exception pending (FSW.ES): x86 raises #MF before running it. Nothing changed.
	fault,
};

// Runs instruction, which x87_supported accepts, on state; next is the address of the instruction after it (RIP-relative operands).
// Memory operands are read and written through memory.
X87Result x87_execute(CpuState &state, GuestMemory &memory, const Instruction &instruction, uint64_t next);

// For measuring option (b) of docs/macos/x87.md only: add, sub, mul, div and sqrt round their operands to double and compute in the
// host's double arithmetic instead. Never set outside that measurement: results are then not x86's.
extern bool x87_double_arithmetic;

}
