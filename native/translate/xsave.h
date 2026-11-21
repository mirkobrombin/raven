#pragma once

#include "cpu_state.h"
#include "guest_memory.h"
#include "x86_decoder.h"

#include <cstdint>

namespace raven::translate {

// xsave and xrstor (and their REX.W forms) run in the host, as cpuid does: a block ends before one with ExitReason::cpuid and the
// runtime runs it here on CpuState, in the standard (not compacted) format with XCR0 as cpuid.h reports it. next is the address of
// the instruction after it. False: x86 raises #GP (an address not aligned to 64 bytes, a bad header or MXCSR) or #UD (XSAVE not
// reported); nothing has been written then.
bool xsave_supported(const Instruction &instruction);
bool xsave(CpuState &state, GuestMemory &memory, const Instruction &instruction, uint64_t next);

}
