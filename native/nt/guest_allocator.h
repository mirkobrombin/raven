#pragma once

#include "heap.h"
#include "memory.h"

#include <array>
#include <map>
#include <set>
#include <vector>

namespace raven {

// Guest memory for blocks that host handlers hand to the program (LocalAlloc, GlobalAlloc, message buffers).
// Blocks come from the process heap when the RTL component provides one. Otherwise blocks up to 4 KB share 64 KB chunks of private pages,
// cut in sizes that are powers of two from 16 bytes, the freed ones of each size kept for the next request of that size; a larger block
// has pages of its own. A reservation per small block would spend 64 KB of address space on each, which a 32-bit program runs out of.
class GuestAllocator {
public:
	GuestAllocator(NativeMemory &memory, NativeHeap &heap) : memory(memory), heap(heap) {}
	// Returns zero on exhaustion; zeroed blocks match HEAP_ZERO_MEMORY.
	uint64_t allocate(uint64_t size, bool zero = true);
	bool release(uint64_t address);
	// Usable size of a block, or UINT64_MAX when the address is not a block of this allocator.
	uint64_t size(uint64_t address);
	bool owns(uint64_t address);

private:
	static constexpr size_t size_classes = 9;
	NativeMemory &memory;
	NativeHeap &heap;
	std::map<uint64_t, uint64_t> pages;
	// The blocks the process heap gave out here: what a program may free again, and the only addresses whose size is answered.
	std::set<uint64_t> heap_blocks;
	// The small blocks given out, by address, with the size asked for; the free blocks of each size; the chunk being cut for each size.
	std::map<uint64_t, uint64_t> shared;
	std::array<std::vector<uint64_t>, size_classes> free_blocks;
	std::array<std::pair<uint64_t, uint64_t>, size_classes> cutting{};
};

}
