#include "pe.h"
#include "loaded_image.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>
#include <tuple>
#include <unistd.h>

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static void put(std::vector<uint8_t> &bytes, size_t offset, uint64_t value, size_t size = 4)
{
	for (size_t i = 0; i < size; ++i)
		bytes.at(offset + i) = static_cast<uint8_t>(value >> (i * 8));
}

static std::vector<uint8_t> fixture(bool is_64)
{
	std::vector<uint8_t> bytes(0x400);
	put(bytes, 0, 0x5a4d, 2);
	put(bytes, 0x3c, 0x80);
	put(bytes, 0x80, 0x4550);
	put(bytes, 0x84, is_64 ? 0x8664 : 0x14c, 2);
	put(bytes, 0x86, 1, 2);
	put(bytes, 0x94, is_64 ? 240 : 224, 2);
	put(bytes, 0x96, 2, 2);
	put(bytes, 0x98, is_64 ? 0x20b : 0x10b, 2);
	put(bytes, 0xa8, 0x1000);
	put(bytes, is_64 ? 0xb0 : 0xb4, is_64 ? 0x140000000ULL : 0x400000, is_64 ? 8 : 4);
	put(bytes, 0xb8, 0x1000);
	put(bytes, 0xbc, 0x200);
	put(bytes, 0xd0, 0x2000);
	put(bytes, 0xd4, 0x200);
	put(bytes, is_64 ? 0x104 : 0xf4, 16);
	const size_t section = is_64 ? 0x188 : 0x178;
	bytes[section] = '.';
	bytes[section + 1] = 't';
	put(bytes, section + 8, 0x300);
	put(bytes, section + 12, 0x1000);
	put(bytes, section + 16, 0x200);
	put(bytes, section + 20, 0x200);
	put(bytes, section + 36, 0x60000020);
	bytes[0x200] = 0xc3;
	return bytes;
}

static void rejected(const std::function<void()> &operation)
{
	try {
		operation();
	} catch (const std::runtime_error &) {
		return;
	}
	throw std::runtime_error("invalid PE input was accepted");
}

static uint64_t read_value(std::span<const uint8_t> bytes, size_t offset, size_t width)
{
	uint64_t value = 0;
	for (size_t i = 0; i < width; ++i)
		value |= uint64_t(bytes[offset + i]) << (i * 8);
	return value;
}

static void delay_import_contracts()
{
	for (bool is_64 : {false, true}) {
		for (bool rva_based : {false, true}) {
			if (is_64 && !rva_based)
				continue;
			auto bytes = fixture(is_64);
			const uint64_t base = is_64 ? 0x140000000ULL : 0x400000;
			const size_t width = is_64 ? 8 : 4;
			const size_t directory = (is_64 ? 0x108 : 0xf8) + 13 * 8;
			check(raven::PEImage(bytes).delay_imports().empty(), "absent delay directory produced imports");
			put(bytes, directory, 0x1040);
			put(bytes, directory + 4, 64);
			put(bytes, 0x240, rva_based ? 1 : 0);
			for (const auto &[offset, address] : std::vector<std::pair<size_t, uint32_t>>{
				{4, 0x10c0}, {8, 0x1200}, {12, 0x10a0}, {16, 0x1080}, {20, 0x1100}, {24, 0x1140}})
				put(bytes, 0x240 + offset, address + (rva_based ? 0 : base));
			put(bytes, 0x25c, 0x12345678);
			put(bytes, 0x280, 0x10e0 + (rva_based ? 0 : base), width);
			put(bytes, 0x280 + width, (uint64_t(1) << (width * 8 - 1)) | 17, width);
			const std::string module = "deferred.dll";
			const std::string symbol = "DelayedFunction";
			std::copy(module.begin(), module.end(), bytes.begin() + 0x2c0);
			std::copy(symbol.begin(), symbol.end(), bytes.begin() + 0x2e2);
			const auto descriptors = raven::PEImage(bytes).delay_imports();
			check(descriptors.size() == 1, "delay descriptor count differs");
			const auto &item = descriptors[0];
			check(item.module == module && item.descriptor_address == 0x1040 &&
				item.module_handle_address == 0x1200 && item.lookup_address == 0x1080 &&
				item.iat_address == 0x10a0 && item.bound_address == 0x1100 &&
				item.unload_address == 0x1140 && item.timestamp == 0x12345678,
				"delay descriptor addresses or timestamp differ");
			check(item.imports.size() == 2 && item.imports[0].name == symbol &&
				!item.imports[0].by_ordinal && item.imports[0].iat_address == 0x10a0 &&
				item.imports[1].by_ordinal && item.imports[1].ordinal == 17 &&
				item.imports[1].iat_address == 0x10a0 + width,
				"delay named or ordinal import differs");
			const auto valid = bytes;
			for (const auto &[offset, value, size] : std::vector<std::tuple<size_t, uint64_t, size_t>>{
				{directory, 0, 4}, {directory + 4, 31, 4}, {directory + 4, 63, 4},
				{0x240, 2, 4}, {0x244, 0, 4}, {0x248, 0, 4}, {0x24c, 0, 4}, {0x250, 0, 4},
				{0x248, 0x1400 + (rva_based ? 0 : base), 4},
				{0x24c, 0x1fff + (rva_based ? 0 : base), 4},
				{0x254, 0x11ff + (rva_based ? 0 : base), 4},
				{0x258, 0x11ff + (rva_based ? 0 : base), 4},
				{0x280, UINT64_MAX - 0xffff, width}, {0x2e2, 0, 1}}) {
				bytes = valid;
				put(bytes, offset, value, size);
				rejected([&] { raven::PEImage(bytes).delay_imports(); });
			}
			bytes = valid;
			std::fill(bytes.begin() + 0x2e2, bytes.end(), uint8_t('x'));
			rejected([&] { raven::PEImage(bytes).delay_imports(); });
			// A table in the part of a section the file does not hold, which a packed program's unpacker writes, is no table yet.
			bytes = valid;
			put(bytes, directory, 0x1200);
			check(raven::PEImage(bytes).delay_imports().empty(), "delay directory the file does not hold produced imports");
			// One that starts in the file and runs past it is malformed.
			put(bytes, directory, 0x11e0);
			rejected([&] { raven::PEImage(bytes).delay_imports(); });
		}
	}
}

static void loaded_image_contracts()
{
	for (bool is_64 : {false, true}) {
		auto bytes = fixture(is_64);
		const size_t width = is_64 ? 8 : 4;
		const size_t relocation = is_64 ? 0x130 : 0x120;
		const uint64_t old_base = is_64 ? 0x140000000ULL : 0x400000;
		put(bytes, relocation, 0x1100);
		put(bytes, relocation + 4, 12);
		put(bytes, 0x300, 0x1000);
		put(bytes, 0x304, 12);
		put(bytes, 0x308, ((is_64 ? 10 : 3) << 12) | 0x20, 2);
		put(bytes, 0x220, old_base + 0x1234, width);
		const raven::PEImage image(bytes);
		for (const uint64_t base : {old_base, old_base + 0x10000, old_base - 0x10000}) {
			raven::LoadedImage loaded(image, base);
			check(loaded.base() == base && loaded.data().size() == 0x2000,
				"loaded image address or size is incorrect");
			check(loaded.data()[0x1000] == 0xc3 && loaded.data()[0x1200] == 0,
				"section content or BSS differs from the image");
			check(read_value(loaded.data(), 0x1020, width) == base + 0x1234,
				"positive or negative relocation delta is incorrect");
			check(loaded.relocation_count() == (base == old_base ? 0 : 1),
				"relocation count includes padding or preferred-base work");
			const auto mappings = loaded.mappings();
			check(mappings.size() == 2 && mappings[0].address == base && mappings[0].file_offset == 0 &&
				mappings[0].protection == PROT_READ && mappings[1].size == 0x1000 &&
				mappings[1].file_offset == 0x1000 && mappings[1].protection == (PROT_READ | PROT_EXEC),
				"guest mapping permissions or offsets differ");
			void *alias = mmap(nullptr, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED, loaded.descriptor(), 0);
			check(alias != MAP_FAILED, "cannot create second shared image view");
			auto *shared = static_cast<uint8_t *>(alias);
			const bool visible = shared[0x1000] == 0xc3;
			shared[0x1200] = 0x52;
			const bool writable = loaded.data()[0x1200] == 0x52;
			munmap(alias, 0x2000);
			check(visible && writable, "shared backing does not propagate image writes");
		}
		rejected([&] { raven::LoadedImage invalid(image, 0); });
		rejected([&] { raven::LoadedImage invalid(image, old_base + 1); });
		put(bytes, 0x304, 7);
		rejected([&] { raven::LoadedImage invalid(raven::PEImage(bytes), old_base + 0x10000); });
		put(bytes, 0x304, 14);
		rejected([&] { raven::LoadedImage invalid(raven::PEImage(bytes), old_base + 0x10000); });
		put(bytes, 0x304, 12);
		put(bytes, 0x308, 0x5020, 2);
		rejected([&] { raven::LoadedImage invalid(raven::PEImage(bytes), old_base + 0x10000); });
		put(bytes, 0x308, ((is_64 ? 10 : 3) << 12) | 0xfff, 2);
		rejected([&] { raven::LoadedImage invalid(raven::PEImage(bytes), old_base + 0x10000); });
		put(bytes, 0x300, 0);
		put(bytes, 0x308, ((is_64 ? 10 : 3) << 12) | 0x500, 2);
		rejected([&] { raven::LoadedImage invalid(raven::PEImage(bytes), old_base + 0x10000); });
		put(bytes, 0x96, 3, 2);
		rejected([&] { raven::LoadedImage invalid(raven::PEImage(bytes), old_base + 0x10000); });
		bytes = fixture(is_64);
		const raven::PEImage fixed(bytes);
		int retired_fd = -1;
		{
			raven::LoadedImage preferred(fixed, old_base);
			retired_fd = preferred.descriptor();
		}
		errno = 0;
		check(fcntl(retired_fd, F_GETFD) == -1 && errno == EBADF,
			"shared image descriptor leaked after destruction");
		rejected([&] { raven::LoadedImage invalid(fixed, old_base + 0x10000); });
		if (!is_64)
			rejected([&] { raven::LoadedImage invalid(fixed, 0x100000000ULL); });
	}
}

int main()
{
	try {
		for (bool is_64 : {false, true}) {
			auto bytes = fixture(is_64);
			const raven::PEImage image(bytes);
			check(image.is_64 == is_64 && image.entry == 0x1000 && image.sections.size() == 1,
				"PE architecture or sections were parsed incorrectly");
			check(image.file_span(0x1000, 1)[0] == 0xc3, "RVA was treated as a file offset");
			check(image.imports().empty(), "absent import directory produced imports");
			auto heap_bytes=bytes;
			const uint64_t reserve=is_64 ? 0x1122334455667788ULL : 0x11223344ULL;
			const uint64_t commit=is_64 ? 0x8877665544332211ULL : 0x44332211ULL;
			put(heap_bytes,0x98+(is_64 ? 88 : 80),reserve,is_64 ? 8 : 4);
			put(heap_bytes,0x98+(is_64 ? 96 : 84),commit,is_64 ? 8 : 4);
			const raven::PEImage heap_image(heap_bytes);
			check(heap_image.heap_reserve==reserve && heap_image.heap_commit==commit,
				"PE32/PE64 optional heap sizes differ in width or offset");
			rejected([&] { image.file_span(0x1200, 1); });
			rejected([&] { image.file_span(0x11ff, 2); });
			for (size_t length = 0; length < 0x400; ++length)
				rejected([&] { raven::PEImage invalid{std::span<const uint8_t>(bytes).first(length)}; });
			put(bytes, 0x3c, 0xfffffff0);
			rejected([&] { raven::PEImage invalid(bytes); });
			bytes = fixture(is_64);
			put(bytes, 0x84, is_64 ? 0x14c : 0x8664, 2);
			rejected([&] { raven::PEImage invalid(bytes); });
			bytes = fixture(is_64);
			put(bytes, is_64 ? 0x104 : 0xf4, 17);
			rejected([&] { raven::PEImage invalid(bytes); });
			bytes = fixture(is_64);
			const size_t directory = is_64 ? 0x110 : 0x100;
			const size_t width = is_64 ? 8 : 4;
			put(bytes, directory, 0x1040);
			put(bytes, directory + 4, 40);
			put(bytes, 0x240, 0x1080);
			put(bytes, 0x24c, 0x10c0);
			put(bytes, 0x250, 0x10a0);
			put(bytes, 0x280, 0x10e0, width);
			put(bytes, 0x280 + width, (uint64_t(1) << (width * 8 - 1)) | 17, width);
			const std::string module = "kernel32.dll";
			const std::string symbol = "ExitProcess";
			std::copy(module.begin(), module.end(), bytes.begin() + 0x2c0);
			std::copy(symbol.begin(), symbol.end(), bytes.begin() + 0x2e2);
			const auto imports = raven::PEImage(bytes).imports();
			check(imports.size() == 2 && imports[0].module == module && imports[0].name == symbol &&
				!imports[0].by_ordinal && imports[0].iat_address == 0x10a0 &&
				imports[1].by_ordinal && imports[1].ordinal == 17 &&
				imports[1].iat_address == 0x10a0 + width, "PE name/ordinal imports were parsed incorrectly");
			put(bytes, 0x240, 0);
			std::copy(bytes.begin() + 0x280, bytes.begin() + 0x280 + width * 3, bytes.begin() + 0x2a0);
			check(raven::PEImage(bytes).imports().size() == 2, "missing lookup table did not use IAT");
			put(bytes, directory + 4, 39);
			rejected([&] { raven::PEImage(bytes).imports(); });
			put(bytes, directory + 4, 40);
			put(bytes, 0x250, 0x1fff);
			rejected([&] { raven::PEImage(bytes).imports(); });
			put(bytes, 0x240, 0x1080);
			put(bytes, 0x250, 0x10a0);
			std::fill(bytes.begin() + 0x2c0, bytes.end(), uint8_t('x'));
			rejected([&] { raven::PEImage(bytes).imports(); });
		}
		delay_import_contracts();
		loaded_image_contracts();
		std::cout << "PE32/PE64 headers, imports, delay imports, shared images, relocations and malformed-input contracts passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
