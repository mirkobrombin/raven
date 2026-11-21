#pragma once

#include "cpu_state.h"
#include "guest_memory.h"
#include "x86_decoder.h"

#include <cstdint>

namespace raven::translate {

// SSE4.2's string compares run in the host, as cpuid does: a block ends before pcmpestri, pcmpestrm, pcmpistri or pcmpistrm (or
// their VEX.128 forms) with ExitReason::cpuid and the runtime runs it here on CpuState, reading a memory operand through memory.
// next is the address of the instruction after it.
bool string_compare_supported(const Instruction &instruction);
void string_compare(CpuState &state, GuestMemory &memory, const Instruction &instruction, uint64_t next);

}
