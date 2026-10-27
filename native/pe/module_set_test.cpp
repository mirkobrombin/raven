#include "module_set.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static void put(std::span<uint8_t> bytes, size_t offset, uint64_t value, size_t width = 4)
{
	for (size_t i = 0; i < width; ++i)
		bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}

static uint64_t read(std::span<const uint8_t> bytes, size_t offset, size_t width)
{
	uint64_t value = 0;
	for (size_t i = 0; i < width; ++i)
		value |= uint64_t(bytes[offset + i]) << (i * 8);
	return value;
}

static void text(std::span<uint8_t> bytes, size_t offset, const std::string &value)
{
	std::copy(value.begin(), value.end(), bytes.begin() + static_cast<ptrdiff_t>(offset));
	bytes[offset + value.size()] = 0;
}

static std::vector<uint8_t> fixture(bool is_64, uint64_t base)
{
	std::vector<uint8_t> bytes(0x800);
	put(bytes, 0, 0x5a4d, 2);
	put(bytes, 0x3c, 0x80);
	put(bytes, 0x80, 0x4550);
	put(bytes, 0x84, is_64 ? 0x8664 : 0x14c, 2);
	put(bytes, 0x86, 1, 2);
	put(bytes, 0x94, is_64 ? 240 : 224, 2);
	put(bytes, 0x96, 0x2002, 2);
	put(bytes, 0x98, is_64 ? 0x20b : 0x10b, 2);
	put(bytes, 0xa8, 0x1000);
	put(bytes, is_64 ? 0xb0 : 0xb4, base, is_64 ? 8 : 4);
	put(bytes, 0xb8, 0x1000);
	put(bytes, 0xbc, 0x200);
	put(bytes, 0xd0, 0x2000);
	put(bytes, 0xd4, 0x200);
	put(bytes, is_64 ? 0x104 : 0xf4, 16);
	const size_t section = is_64 ? 0x188 : 0x178;
	text(bytes, section, ".text");
	put(bytes, section + 8, 0x600);
	put(bytes, section + 12, 0x1000);
	put(bytes, section + 16, 0x600);
	put(bytes, section + 20, 0x200);
	put(bytes, section + 36, 0xe0000020);
	bytes[0x200] = 0xc3;
	return bytes;
}

static void exports(std::vector<uint8_t> &bytes, bool is_64, const std::string &forwarder)
{
	const size_t directory = is_64 ? 0x108 : 0xf8;
	put(bytes, directory, 0x1100);
	put(bytes, directory + 4, 0x100);
	put(bytes, 0x310, 7);
	put(bytes, 0x314, 3);
	put(bytes, 0x318, 3);
	put(bytes, 0x31c, 0x1140);
	put(bytes, 0x320, 0x1150);
	put(bytes, 0x324, 0x1160);
	put(bytes, 0x340, 0x1000);
	put(bytes, 0x348, forwarder.empty() ? 0x1010 : 0x11c0);
	put(bytes, 0x350, 0x1180);
	put(bytes, 0x354, 0x1190);
	put(bytes, 0x358, 0x11a0);
	put(bytes, 0x360, 0, 2);
	put(bytes, 0x362, 2, 2);
	put(bytes, 0x364, 0, 2);
	text(bytes, 0x380, "Alias");
	text(bytes, 0x390, "Forward");
	text(bytes, 0x3a0, "Function");
	if (!forwarder.empty())
		text(bytes, 0x3c0, forwarder);
}

static void imports(std::vector<uint8_t> &bytes, bool is_64)
{
	const size_t directory = is_64 ? 0x110 : 0x100;
	const size_t width = is_64 ? 8 : 4;
	put(bytes, directory, 0x1300);
	put(bytes, directory + 4, 40);
	put(bytes, 0x500, 0x1340);
	put(bytes, 0x50c, 0x13c0);
	put(bytes, 0x510, 0x1380);
	put(bytes, 0x540, 0x13e0, width);
	put(bytes, 0x540 + width, (uint64_t(1) << (width * 8 - 1)) | 9, width);
	text(bytes, 0x5c0, "Facade.DLL");
	text(bytes, 0x5e2, "Function");
}

static void rejected(const std::function<void()> &operation)
{
	try {
		operation();
	} catch (const std::runtime_error &) {
		return;
	}
	throw std::runtime_error("invalid export or module graph was accepted");
}

int main()
{
	try {
		for (bool is_64 : {false, true}) {
			const uint64_t root_base = is_64 ? 0x140000000ULL : 0x400000;
			const uint64_t facade_base = root_base + 0x100000;
			const uint64_t target_base = root_base + 0x200000;
			const size_t width = is_64 ? 8 : 4;
			auto root = fixture(is_64, root_base);
			auto facade = fixture(is_64, facade_base);
			auto target = fixture(is_64, target_base);
			imports(root, is_64);
			exports(facade, is_64, "TARGET.#7");
			exports(target, is_64, "");
			const auto decoded = raven::PEImage(facade).exports();
			check(decoded.size() == 3 && decoded[0].ordinal == 7 && decoded[0].address == 0x1000 &&
				decoded[0].names == std::vector<std::string>{"Alias", "Function"} &&
				decoded[1].address == 0 && decoded[2].forwarder == "TARGET.#7", "export table identities differ");
			raven::ModuleSet modules;
			auto &main = modules.add("program.exe", root, root_base);
			modules.add("FaCaDe", facade, facade_base);
			const auto original = std::vector<uint8_t>(main.loaded.data().begin(), main.loaded.data().end());
			rejected([&] { modules.bind_imports(); });
			check(std::equal(original.begin(), original.end(), main.loaded.data().begin()), "failed binding changed an image");
			modules.add("TARGET.dll", target, target_base);
			check(modules.bind_imports() == 2 && read(main.loaded.data(), 0x1380, width) == facade_base + 0x1000 &&
				read(main.loaded.data(), 0x1380 + width, width) == target_base + 0x1000, "IAT name/ordinal or forwarded address differs");
			check(modules.bind_imports() == 2, "rebinding did not use immutable original import identities");
			check(modules.resolve({"facade", "Alias", 0, false, 0}) == facade_base + 0x1000, "export alias did not resolve");
			{
				auto relocated = target;
				const size_t relocation = is_64 ? 0x130 : 0x120;
				put(relocated, relocation, 0x1500);
				put(relocated, relocation + 4, 12);
				put(relocated, 0x700, 0x1000);
				put(relocated, 0x704, 12);
				put(relocated, 0x708, ((is_64 ? 10 : 3) << 12) | 0x520, 2);
				put(relocated, 0x720, target_base + 0x1000, width);
				raven::ModuleSet rebased;
				auto &client = rebased.add("main.exe", root, root_base);
				rebased.add("facade", facade, facade_base);
				auto &dependency = rebased.add("target", relocated, target_base + 0x10000);
				check(rebased.bind_imports() == 2 && read(client.loaded.data(), 0x1380 + width, width) == target_base + 0x11000 &&
					read(dependency.loaded.data(), 0x1520, width) == target_base + 0x11000,
					"forwarded IAT or relocation retained the dependency's preferred base");
				raven::ModuleSet automatic;
				auto &automatic_client = automatic.add("main.exe", root);
				automatic.add("facade", facade);
				automatic.add("occupied", target);
				const uint64_t fallback = is_64 ? 0x180000000ULL : 0x10000000ULL;
				automatic.add("reserved", fixture(is_64, fallback));
				auto &moved = automatic.add("target", relocated);
				check(automatic_client.loaded.base() == root_base && moved.loaded.base() == fallback + 65536 &&
					read(moved.loaded.data(), 0x1520, width) == moved.loaded.base() + 0x1000,
					"automatic placement changed a free preference or failed to skip occupied ranges");
				check(automatic.bind_imports() == 2 && read(automatic_client.loaded.data(), 0x1380 + width, width) == moved.loaded.base() + 0x1000,
					"automatic placement left a forwarded IAT at the preferred base");
				const auto count = automatic.all().size();
				rejected([&] { automatic.add("fixed", target); });
				check(automatic.all().size() == count, "failed automatic relocation published a module");
				rejected([&] { automatic.add("requested", relocated, target_base); });
				check(automatic.all().size() == count, "explicit colliding base changed the module graph");
				raven::ModuleSet constrained;
				const auto image_size = raven::PEImage(relocated).image_size;
				const auto available = [&](uint64_t base, uint64_t size) {
					check(size == image_size, "placement queried the wrong image size");
					return base != target_base && (base < fallback || base >= fallback + 2 * 65536);
				};
				auto &placed = constrained.add("target", relocated, available);
				check(placed.loaded.base() == fallback + 2 * 65536 &&
					read(placed.loaded.data(), 0x1520, width) == placed.loaded.base() + 0x1000,
					"placement ignored occupied process memory or failed to relocate");
				auto &data = constrained.add_data("resource", relocated, available);
				check(data.loaded.base() == fallback + 3 * 65536 && read(data.loaded.data(), 0x1520, width) == target_base + 0x1000,
					"data placement overlapped a process range or applied code relocations");
				raven::ModuleSet displaced;
				auto &displaced_first = displaced.add("first", relocated);
				check(displaced_first.loaded.base() == target_base, "initial relocatable placement ignored free preferred base");
				auto &displaced_fixed = displaced.add("fixed", target);
				check(displaced_fixed.loaded.base() == target_base && displaced_first.loaded.base() == fallback &&
					read(displaced_first.loaded.data(), 0x1520, width) == fallback + 0x1000,
					"fixed-base module did not displace an unbound relocatable module");
			}
			rejected([&] { modules.resolve({"facade", "function", 0, false, 0}); });
			rejected([&] { modules.resolve({"facade", "", 8, true, 0}); });
			rejected([&] { modules.add("facade.dll", facade, facade_base); });
			rejected([&] { modules.add("overlap.dll", root, root_base); });
			rejected([&] { modules.add("../target.dll", target, target_base); });
			for (const auto &forwarder : {"target.Function", "target.dll.Function", "target.#7"}) {
				auto bytes = facade;
				exports(bytes, is_64, forwarder);
				raven::ModuleSet named;
				named.add("facade", bytes, facade_base);
				named.add("target", target, target_base);
				check(named.resolve({"facade", "Forward", 0, false, 0}) == target_base + 0x1000, "forwarder name or extension resolution differs");
			}
			for (const auto &forwarder : {"facade.#9", "target.Forward", "target.#no", "target.#", "target.#4294967296", "target.", "NoSeparator"}) {
				auto bytes = facade;
				exports(bytes, is_64, forwarder);
				auto other = target;
				exports(other, is_64, "facade.Forward");
				raven::ModuleSet cyclic;
				cyclic.add("facade", bytes, facade_base);
				cyclic.add("target", other, target_base);
				rejected([&] { cyclic.resolve({"facade", "Forward", 0, false, 0}); });
			}
			const size_t directory = is_64 ? 0x108 : 0xf8;
			for (const auto &[offset, value] : std::vector<std::pair<size_t, uint64_t>>{
				{directory + 4, 39}, {0x310, UINT32_MAX}, {0x314, 1000001}, {0x31c, 0},
				{0x320, 0x1fff}, {0x324, 0x1fff}, {0x360, 3}, {0x354, 0x1180},
				{0x340, 0x1f00}, {0x348, 0x11ff}}) {
				auto bytes = facade;
				put(bytes, offset, value, offset == 0x360 ? 2 : 4);
				if (offset == 0x348)
					bytes[0x3ff] = 'x';
				rejected([&] { raven::PEImage(bytes).exports(); });
			}
			auto hole = root;
			put(hole, 0x510, 0x1800);
			raven::ModuleSet bad_iat;
			bad_iat.add("main.exe", hole, root_base);
			bad_iat.add("facade", facade, facade_base);
			bad_iat.add("target", target, target_base);
			rejected([&] { bad_iat.bind_imports(); });
			raven::ModuleSet mixed;
			mixed.add("target", target, target_base);
			rejected([&] { mixed.add("other", fixture(!is_64, 0x700000), 0x700000); });
		}
		std::cout << "PE32/PE64 exports, aliases, holes, forwarding, transactional IAT binding and negative module contracts passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
