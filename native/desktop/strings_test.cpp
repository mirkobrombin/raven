#include "api.h"
#include "nt/providers.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <sys/mman.h>

static void check(bool condition, const char *message)
{
	if (!condition) throw std::runtime_error(message);
}

static std::vector<uint8_t> read_file(const std::filesystem::path &path)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	check(bool(file) && file.tellg() > 0 && file.tellg() < 1024 * 1024 * 1024, "invalid string fixture");
	std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg()));
	file.seekg(0);
	check(bool(file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))), "cannot read string fixture");
	return bytes;
}

int main(int argc, char **argv)
{
	try {
		const bool dump = argc == 4 && std::string_view(argv[3]) == "--dump";
		check(argc == 3 || dump, "bootstrap and string fixture directory, or image with --dump required");
		raven::ModuleSet modules;
		auto &entry = modules.add("client.exe", read_file(dump ? std::filesystem::path(argv[2]) : std::filesystem::path(argv[2]) / "client.exe"));
		auto &user = modules.add("user32.dll", raven::runtime_provider("user32.dll", 0x180000000ULL, raven::NativeDesktop::user_exports()));
		raven::LoadedImage *images[] = {&entry.loaded, &user.loaded};
		raven::NativeProcess process(images, argv[1], std::chrono::seconds(2));
		const auto function = modules.resolve({"user32.dll", "LoadStringW", 0, false, 0});
		process.add_api_trap(raven::api_trap_address(user, "user32.dll", "LoadStringW", uint32_t(function - user.loaded.base())));
		raven::NativeEnvironment environment(process, entry.loaded.base(), 1, 1);
		raven::NativeResources resources(process, environment, modules, entry);
		raven::DesktopObjects objects;
		raven::NativeDesktop desktop(process, environment, objects, resources);
		const auto output = process.map_memory(4096, PROT_READ | PROT_WRITE);
		const auto call = [&](std::initializer_list<uint64_t> args, std::string_view failure = {}) {
			check(process.call(function, std::span(args.begin(), args.size()), std::chrono::seconds(1)).stop == raven::NativeStop::api,
				"string API did not trap");
			if (failure.empty()) check(desktop.dispatch("LoadStringW"), "string API not dispatched");
			else {
				std::string error;
				try { desktop.dispatch("LoadStringW"); } catch (const std::runtime_error &caught) { error = caught.what(); }
				check(error.find(failure) != error.npos, "string negative failed for another reason");
				process.return_api(0);
			}
			const auto result = process.resume(std::chrono::seconds(1));
			check(result.stop == raven::NativeStop::returned, "string API did not return");
			return result.value;
		};
		if (dump) {
			for (unsigned id = 0; id < 16; ++id) {
				const auto length = call({0, id, output, 0});
				const auto pointer = process.read_memory(output, 8);
				uint64_t address = 0;
				for (unsigned i = 0; i < 8; ++i) address |= uint64_t(pointer[i]) << (i * 8);
				const auto bytes = process.read_memory(address, length * 2);
				std::cout << "id=" << std::dec << id << " length=" << length << " data=";
				for (size_t i = 0; i < bytes.size(); i += 2)
					std::cout << std::hex << std::setfill('0') << std::setw(4) << (unsigned(bytes[i]) | unsigned(bytes[i + 1]) << 8);
				std::cout << '\n';
			}
			return 0;
		}
		check(call({0, 1, output, 0}) == 5, "string pointer mode failed");
		const auto table = resources.view(0, 6, 1);
		check(table.has_value(), "string table missing");
		const auto original = process.read_memory(table->address, table->size);
		const auto table_offset = size_t(table->address - entry.loaded.base());
		// A program changes its resources only after it makes their read-only pages writable, which the resource cache notices.
		{
			const auto directory = entry.image.directories[2];
			const uint64_t first = (entry.loaded.base() + directory.address) & ~uint64_t(4095);
			const uint64_t end = (entry.loaded.base() + directory.address + directory.size + 4095) & ~uint64_t(4095);
			process.protect_image(first, end - first, PROT_READ | PROT_WRITE);
		}
		const uint8_t changed[] = {'Z', 0};
		std::copy(std::begin(changed), std::end(changed), entry.loaded.data().begin() + static_cast<ptrdiff_t>(table_offset + 4));
		check(call({0, 1, output, 6}) == 5 && process.read_memory(output, 2) == std::vector<uint8_t>(changed, changed + 2),
			"live string table was cached");
		std::copy(original.begin(), original.end(), entry.loaded.data().begin() + static_cast<ptrdiff_t>(table_offset));
		call({0, 1, output, 0xffffffff}, "negative LoadStringW");
		call({0, 1, 0x400000000000ULL, 0}, "writable");
		call({0, 1, 0x400000000000ULL, 1}, "writable");
		const uint8_t huge[] = {0xff, 0xff};
		std::copy(std::begin(huge), std::end(huge), entry.loaded.data().begin() + static_cast<ptrdiff_t>(table_offset));
		call({0, 1, output, 0}, "exceeds table payload");
		std::copy(original.begin(), original.end(), entry.loaded.data().begin() + static_cast<ptrdiff_t>(table_offset));
		const auto directory = entry.image.directories[2];
		const auto metadata = raven::read_resources(directory,
			std::span(entry.loaded.data()).subspan(directory.address, directory.size), entry.image.image_size);
		for (const auto &resource : metadata)
			if (resource.type == raven::ResourceKey(uint32_t(6))) {
				const uint8_t size[] = {1, 0, 0, 0};
				std::copy(std::begin(size), std::end(size), entry.loaded.data().begin() + resource.entry_address + 4);
				call({0, 1, output, 0}, "truncated string resource length");
			}
		check(objects.count() == 0, "string lookup allocated desktop handles");
		std::cout << "Live string resources, pointer/copy bounds and malformed payload checks passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
