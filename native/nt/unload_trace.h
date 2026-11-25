#pragma once

#include "guest_allocator.h"

#include <cstdint>
#include <string_view>

namespace raven {

class NativeProcess;

// The libraries the process unloaded last, kept in the guest's memory as ntdll keeps them: a ring of 64 RTL_UNLOAD_EVENT_TRACE records
// (base, size, sequence, time stamp, checksum, the first 31 characters of the name, version) after the record's size and count, which
// RtlGetUnloadEventTraceEx points at. Crash reporters read it to name the library a stray call went to.
class UnloadTrace {
public:
	static constexpr uint32_t records = 64;
	// The block, made the first time it is asked for, its records as wide as the process's pointers; zero when no memory was left.
	uint64_t block(NativeProcess &process, GuestAllocator &allocator, bool wide);
	// The size and count stand first, the ring from this offset.
	static constexpr uint64_t ring = 16;
	static constexpr uint32_t record_size(bool wide) { return wide ? 104 : 92; }
	// Notes a library that was unloaded, with what its headers in the guest's memory say, before its pages go.
	void record(NativeProcess &process, GuestAllocator &allocator, bool wide, uint64_t base, uint64_t size, std::string_view name);

private:
	uint64_t at = 0;
	uint32_t sequence = 0;
};

}
