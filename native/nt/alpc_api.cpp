#include "kernel_context.h"

#include <array>
#include <bit>

namespace raven {

namespace {

constexpr uint32_t attribute_mask = 0xfe000000u;

size_t attribute_size(uint32_t allocated, size_t width, uint32_t first_excluded = 0)
{
	// The slots follow the descending flag order: security, view, context, handle, token, direct, work on behalf.
	const std::array<size_t, 7> sizes{3 * width, 4 * width, width == 8 ? 32u : 20u, width == 8 ? 24u : 16u, 24, width, 8};
	size_t size = 8;
	for (size_t index = 0; index < sizes.size(); ++index) {
		const uint32_t bit = 0x80000000u >> index;
		if (bit <= first_excluded)
			break;
		if (allocated & bit)
			size += sizes[index];
	}
	return size;
}

}

void register_alpc_api(ApiRegistry &registry)
{
	registry.add("AlpcGetHeaderSize", 1, [](ApiCall &call) {
		call.ret(attribute_size(call.u32(0), call.pointer_width()));
	});
	registry.add("AlpcInitializeMessageAttribute", 4, [](ApiCall &call) {
		const auto flags = call.u32(0);
		const auto needed = attribute_size(flags, call.pointer_width());
		call.store_pointer(call[3], needed);
		if (call[2] < needed) {
			call.ret(0xc0000023u);
			return;
		}
		if (call[1])
			call.store(call[1], std::array<uint32_t, 2>{flags, 0});
		call.ret(0);
	});
	registry.add("AlpcGetMessageAttribute", 2, [](ApiCall &call) {
		const auto flag = call.u32(1);
		if (!call[0] || !(flag & attribute_mask) || !std::has_single_bit(flag))
			return;
		const auto allocated = call.load<uint32_t>(call[0]);
		if (allocated & flag)
			call.ret(call[0] + attribute_size(allocated, call.pointer_width(), flag));
	});
}

}
