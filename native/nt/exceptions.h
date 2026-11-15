#pragma once

#include "cpu/native_cpu.h"

#include <vector>

namespace raven {

// Captures a software exception at the API boundary; guest handler search is not implemented.
NativeResult native_raise_exception(NativeProcess &process);
NativeResult native_raise_rtl_exception(NativeProcess &process, bool status_only);

// The exception Windows raises for a processor fault: its code and parameters, and the instruction both the record and the context
// name. That is the faulting instruction, an int3 included, so a handler that skips a breakpoint moves the context past it itself.
struct ProcessorException {
	uint32_t status = 0xc0000005U;
	std::vector<uint64_t> parameters;
	uint64_t address = 0;
};
ProcessorException processor_exception(NativeProcess &process, const NativeResult &fault, uint64_t ip);

}
