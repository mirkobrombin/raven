#pragma once

#include "cpu_state.h"
#include "guest_memory.h"
#include "x86_decoder.h"

#include <cstdint>

namespace raven::translate {

// A locked read-modify-write whose memory operand crosses a 16-byte boundary, which no LSE atomic covers: translated code leaves with
// ExitReason::cpuid at the instruction and the runtime runs it here, while every other guest thread of the VM is stopped
// (CodeRunner::exclusive). xchg with memory, cmpxchg, xadd, and lock add, or, adc, sbb, and, sub, xor, inc, dec, not and neg; the
// flags as x86 leaves them. next is the address of the instruction after it.
bool locked_supported(const Instruction &instruction);
void locked_execute(CpuState &state, GuestMemory &memory, const Instruction &instruction, uint64_t next);

}
