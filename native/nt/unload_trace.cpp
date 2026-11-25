#include "unload_trace.h"
#include "text.h"
#include "cpu/native_cpu.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

namespace raven {

uint64_t UnloadTrace::block(NativeProcess &process, GuestAllocator &allocator, bool wide)
{
	if (at)
		return at;
	const uint32_t header[] = {record_size(wide), records};
	at = allocator.allocate(ring + uint64_t(header[0]) * records);
	if (at)
		process.write_memory(at, std::span(reinterpret_cast<const uint8_t *>(header), sizeof header));
	return at;
}

void UnloadTrace::record(NativeProcess &process, GuestAllocator &allocator, bool wide, uint64_t base, uint64_t size,
	std::string_view name)
{
	if (!block(process, allocator, wide))
		return;
	const uint32_t width = wide ? 8 : 4, size_of_record = record_size(wide);
	std::vector<uint8_t> bytes(size_of_record);
	std::memcpy(bytes.data(), &base, width);
	std::memcpy(bytes.data() + width, &size, width);
	std::memcpy(bytes.data() + 2 * width, &sequence, 4);
	// The time stamp and checksum of the image, from its file header and optional header (at the same place in both widths).
	uint32_t header = 0, stamp = 0, checksum = 0;
	const auto read32 = [&](uint64_t address) {
		uint32_t value = 0;
		const auto read = process.read_memory(address, 4);
		std::memcpy(&value, read.data(), 4);
		return value;
	};
	try {
		header = read32(base + 0x3c);
		if (header && header < size) {
			stamp = read32(base + header + 8);
			checksum = read32(base + header + 24 + 64);
		}
	} catch (...) {
	}
	std::memcpy(bytes.data() + 2 * width + 4, &stamp, 4);
	std::memcpy(bytes.data() + 2 * width + 8, &checksum, 4);
	const auto slash = name.find_last_of("\\/");
	auto text = from_utf8(slash == name.npos ? name : name.substr(slash + 1));
	text.resize(std::min<size_t>(text.size(), 31));
	std::memcpy(bytes.data() + 2 * width + 12, text.data(), text.size() * 2);
	const auto slot = at + ring + uint64_t(sequence % records) * size_of_record;
	process.write_memory(slot, bytes);
	++sequence;
}

}
