#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace raven::translate {

// The guest address space as the translator and its runtime see it, whatever the process model: the loader that maps images answers.
// Translated code addresses guest memory at guest virtual addresses, so the code it emits runs wherever guest addresses are the
// addresses of the CPU running it: the host process (the in-process test runner) or a VM whose page tables map the guest.
class GuestMemory {
public:
	virtual ~GuestMemory() = default;
	// The x64 code from address to the end of its executable region; empty when address is not x64 code.
	virtual std::span<const uint8_t> code(uint64_t address) const = 0;
	// Whether address is native code, as the code map of an ARM64EC process says (the EC bitmap): a call there takes a native thunk.
	virtual bool is_native(uint64_t address) const = 0;
	// Makes writes to the guest pages holding [address, address + size) reach Runtime::code_written (true), or plain again (false).
	virtual void watch_code(uint64_t address, size_t size, bool watch) = 0;
	// The granule watch_code works in: the host page size in a process, 4 KiB in a VM with a 4 KiB stage-2 granule.
	virtual size_t watch_granule() const = 0;
};

}
