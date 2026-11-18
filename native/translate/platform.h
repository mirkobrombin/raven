#pragma once

#include <cstddef>
#include <cstdint>

// The host services the translator needs, one implementation per host OS: platform_darwin.cpp (MAP_JIT and the per-thread write
// switch) and platform_linux.cpp (a memfd mapped twice, writable and executable). Nothing above this header names an OS.
namespace raven::translate::host {

// Executable memory seen at two addresses that may be the same: code runs at execute and is written through write.
struct CodeRegion {
	uint8_t *execute = nullptr;
	uint8_t *write = nullptr;
	size_t size = 0;
};

CodeRegion map_code(size_t size);
void unmap_code(CodeRegion &region);
// Lets the calling thread write code until close_code_writes; on macOS the switch is per thread, so other threads keep running code.
void open_code_writes();
void close_code_writes();
void flush_instruction_cache(void *start, size_t size);
size_t page_size();
// Whether the CPU orders memory as x86 does for this thread (TSO). False on macOS for native processes; true inside a VM whose vCPU has
// ACTLR_EL1.TSOEN set.
bool hardware_tso();
// Whether the CPU has FEAT_AFP: FPCR.AH and FIZ, on which translated SSE code relies for x86's NaNs, min and max and DAZ.
bool alternate_floating_point();

}
