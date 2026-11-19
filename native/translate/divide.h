#pragma once

#include "cpu_state.h"
#include "guest_memory.h"
#include "x86_decoder.h"

#include <cstdint>

namespace raven::translate {

// div and idiv in the host, for what translated code does not finish inline (translator.cpp's divide): it leaves with
// ExitReason::cpuid at the instruction and the runtime runs it here on CpuState. next is the address of the instruction after it.
// False: x86 raises #DE (a zero divisor or a quotient that does not fit); nothing has changed then.
bool divide_supported(const Instruction &instruction);
bool divide(CpuState &state, const GuestMemory &memory, const Instruction &instruction, uint64_t next);

}
