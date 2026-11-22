#include "kernel_context.h"

#include <array>
#include <bit>
#include <cstring>
#include <map>

namespace raven {

namespace {

class AlpcPort : public KernelObject {
public:
	explicit AlpcPort(std::u16string name) : name(std::move(name)) {}
	std::u16string_view type_name() const override { return u"ALPC Port"; }
	std::u16string_view object_name() const override { return name; }
	uint32_t default_access() const override { return 0x1f0001; }

private:
	std::u16string name;
};

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

void register_alpc_api(ApiRegistry &registry, KernelContext &context)
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
	const auto names = std::make_shared<std::map<std::u16string, std::weak_ptr<AlpcPort>>>();
	registry.add("NtAlpcCreatePort", 3, [&objects = context.objects, names](ApiCall &call) {
		try {
			const size_t width = call.pointer_width();
			call.process.validate_write_memory(call[0], width);
			if (call[2] && (call.load<uint32_t>(call[2]) & 0x00100000u)) {
				call.ret(0xc000000du);
				return;
			}
			uint32_t flags = 0;
			std::u16string name;
			if (const auto attributes = call[1]) {
				if (call.load<uint32_t>(attributes) != 6 * width) {
					call.ret(0xc000000du);
					return;
				}
				if (call.load_pointer(attributes + width) || call.load_pointer(attributes + 4 * width)) {
					call.ret(0xc00000bbu);
					return;
				}
				flags = call.load<uint32_t>(attributes + 3 * width);
				if (const auto counted = call.load_pointer(attributes + 2 * width)) {
					const auto size = call.load<uint16_t>(counted);
					if (size > call.load<uint16_t>(counted + 2) || size % 2 || size > 65532) {
						call.ret(0xc0000033u);
						return;
					}
					const auto bytes = call.read(call.load_pointer(counted + width), size);
					name.resize(size / 2);
					if (!bytes.empty())
						std::memcpy(name.data(), bytes.data(), size);
					if (!name.starts_with(u'\\') || name.find(u'\0') != name.npos || name.ends_with(u'\\')) {
						call.ret(0xc0000033u);
						return;
					}
				}
			}
			auto key = name;
			for (auto &character : key)
				if (character >= u'A' && character <= u'Z')
					character += u'a' - u'A';
			std::erase_if(*names, [](const auto &entry) { return entry.second.expired(); });
			if (!key.empty() && names->contains(key)) {
				call.ret(0xc0000035u);
				return;
			}
			const auto port = std::make_shared<AlpcPort>(std::move(name));
			if (!key.empty())
				names->emplace(std::move(key), port);
			const auto made = objects.add_object(port);
			if (!made.status) {
				objects.set_inherit(made.value, (flags & 2) != 0);
				try {
					call.store_pointer(call[0], made.value);
				} catch (...) {
					objects.close(made.value);
					throw;
				}
			}
			call.ret(made.status);
		} catch (const std::bad_alloc &) {
			call.ret(0xc000009au);
		} catch (const ApiFault &) {
			call.ret(0xc0000005u);
		} catch (const std::runtime_error &) {
			call.ret(0xc0000005u);
		}
	});
}

}
