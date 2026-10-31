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
	if (heap.available())
		return heap.allocate(zero ? 8 : 0, std::max<uint64_t>(size, 1));
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

uint64_t GuestAllocator::allocate_movable(uint64_t size, bool zero)
{
	const auto address = allocate(size, zero);
	if (!address)
		return 0;
	empty.erase(address);
	moved_blocks.erase(address);
	movable_blocks[address] = address;
	if (!size)
		empty.insert(address);
	return address;
}

void GuestAllocator::make_movable(uint64_t address)
{
	movable_blocks.emplace(address, address);
}

uint64_t GuestAllocator::data(uint64_t handle) const
{
	const auto found = movable_blocks.find(handle);
	return found == movable_blocks.end() ? handle : found->second;
}

uint64_t GuestAllocator::handle(uint64_t block) const
{
	const auto found = moved_blocks.find(block);
	return found == moved_blocks.end() ? block : found->second;
}

void GuestAllocator::move(uint64_t handle, uint64_t block)
{
	auto &current = movable_blocks.at(handle);
	if (current != handle) {
		moved_blocks.erase(current);
		release_block(current);
	}
	current = block;
	moved_blocks[block] = handle;
}

bool GuestAllocator::release(uint64_t address)
{
	const auto block = handle(address);
	if (const auto found = movable_blocks.find(block); found != movable_blocks.end()) {
		if (found->second != block) {
			moved_blocks.erase(found->second);
			release_block(found->second);
		}
		movable_blocks.erase(found);
	}
	return release_block(block);
}

bool GuestAllocator::release_block(uint64_t address)
{
	empty.erase(address);
	if (heap.available())
		return heap.release(address);
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
	if (empty.contains(address))
		return 0;
	if (heap.available())
		return heap.block_size(address);
	if (const auto block = shared.find(address); block != shared.end())
		return block->second;
	const auto found = pages.find(address);
	return found == pages.end() ? UINT64_MAX : found->second;
}

bool GuestAllocator::owns(uint64_t address)
{
	return heap.available() ? heap.block_size(address) != UINT64_MAX : pages.contains(address) || shared.contains(address);
}

}
