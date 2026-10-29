#include "guest_allocator.h"

namespace raven {

namespace {

constexpr uint64_t chunk_size = 0x10000, largest_shared = 4096;

// The size class of a small block: 0 holds 16 bytes, each next class twice as many.
size_t size_class(uint64_t size)
{
	size_t index = 0;
	for (uint64_t block = 16; block < size; block <<= 1)
		++index;
	return index;
}

}

uint64_t GuestAllocator::allocate(uint64_t size, bool zero)
{
	if (heap.available()) {
		const auto address = heap.allocate(zero ? 8 : 0, std::max<uint64_t>(size, 1));
		if (address)
			heap_blocks.insert(address);
		return address;
	}
	if (size <= largest_shared) {
		const size_t index = size_class(std::max<uint64_t>(size, 1));
		const uint64_t block = uint64_t(16) << index;
		uint64_t address = 0;
		if (!free_blocks[index].empty()) {
			address = free_blocks[index].back();
			free_blocks[index].pop_back();
			if (zero)
				memory.clear(address, block);
		} else {
			auto &[cursor, end] = cutting[index];
			// A new chunk's pages read as zero.
			if (cursor + block > end) {
				const auto range = memory.allocate(0, chunk_size, 0x3000, 4);
				if (range.status)
					return 0;
				cursor = range.address;
				end = range.address + chunk_size;
			}
			address = cursor;
			cursor += block;
		}
		shared.emplace(address, std::max<uint64_t>(size, 1));
		return address;
	}
	const uint64_t rounded = (std::max<uint64_t>(size, 1) + 4095) & ~uint64_t(4095);
	const auto range = memory.allocate(0, rounded, 0x3000, 4);
	if (range.status)
		return 0;
	pages.emplace(range.address, std::max<uint64_t>(size, 1));
	return range.address;
}

bool GuestAllocator::release(uint64_t address)
{
	// Only a block this allocator handed out may go back: Windows refuses any other address and leaves the heap as it was, while the
	// RTL heap here trusts the block header it finds before the address and would take a corrupted free list with it. Programs do free
	// addresses inside a block, such as the block CommandLineToArgvW returns advanced past the program name.
	if (heap.available()) {
		if (!heap_blocks.erase(address))
			return false;
		return heap.release(address);
	}
	if (const auto block = shared.find(address); block != shared.end()) {
		free_blocks[size_class(block->second)].push_back(address);
		shared.erase(block);
		return true;
	}
	const auto found = pages.find(address);
	if (found == pages.end())
		return false;
	pages.erase(found);
	return memory.free(address, 0, 0x8000).status == 0;
}

uint64_t GuestAllocator::size(uint64_t address)
{
	if (heap.available())
		return heap_blocks.contains(address) ? heap.block_size(address) : UINT64_MAX;
	if (const auto block = shared.find(address); block != shared.end())
		return block->second;
	const auto found = pages.find(address);
	return found == pages.end() ? UINT64_MAX : found->second;
}

bool GuestAllocator::owns(uint64_t address)
{
	return heap.available() ? heap_blocks.contains(address) : pages.contains(address) || shared.contains(address);
}

}
