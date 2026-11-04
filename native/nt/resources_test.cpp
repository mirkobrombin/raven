#include "resources.h"
#include "providers.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static std::vector<uint8_t> read_file(const std::filesystem::path &path)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file || file.tellg() < 0 || file.tellg() > 1024 * 1024 * 1024)
		throw std::runtime_error("invalid native resource fixture");
	std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg()));
	file.seekg(0);
	if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
		throw std::runtime_error("cannot read native resource fixture");
	return bytes;
}

int main(int argc, char **argv)
{
	try {
		check(argc == 3, "bootstrap and resource fixture directory required");
		const std::filesystem::path directory(argv[2]);
		{
			raven::ModuleSet modules;
			auto &entry = modules.add("client.exe", read_file(directory / "client.exe"));
			unsigned requests = 0;
			const auto source = [&](std::string_view name) -> std::optional<std::vector<uint8_t>> {
				check(name == "resources.dll", "resource workflow requested an unexpected DLL");
				++requests;
				return read_file(directory / "resources.dll");
			};
			const auto result = raven::run_native_process(modules, entry, argv[1], std::chrono::seconds(2), source);
			check(result.stop == raven::NativeStop::returned && result.value == 0 && requests == 1,
				"compiled resource API workflow failed");
			check(!modules.all().contains("resources.dll"), "resource DLL outlived guest FreeLibrary");
			const auto phase = modules.resolve({"client.exe", "phase", 0, false, 0}) - entry.loaded.base();
			check(entry.loaded.data()[static_cast<size_t>(phase)] == 3, "resource workflow did not finish its unload phase");
		}
		raven::ModuleSet modules;
		auto &entry = modules.add("client.exe", read_file(directory / "client.exe"));
		auto &provider = modules.add("kernel32.dll", raven::environment_provider("kernel32.dll", 0x180000000ULL));
		raven::LoadedImage *initial[] = {&entry.loaded, &provider.loaded};
		raven::NativeProcess process(initial, argv[1], std::chrono::seconds(2));
		for (const auto &symbol : provider.exports)
			process.add_api_trap(raven::api_trap_address(provider, "kernel32.dll", symbol.names.at(0), symbol.address));
		raven::NativeEnvironment environment(process, entry.loaded.base(), 1, 1);
		raven::NativeResources resources(process, environment, modules, entry);
		const auto api = [&](const char *name, std::initializer_list<uint64_t> args) {
			const auto address = modules.resolve({"kernel32.dll", name, 0, false, 0});
			const auto pending = process.call(address, std::span(args.begin(), args.size()), std::chrono::seconds(1));
			check(pending.stop == raven::NativeStop::api && resources.dispatch(name), "resource API trap was not dispatched");
			const auto result = process.resume(std::chrono::seconds(1));
			check(result.stop == raven::NativeStop::returned, "resource API did not return");
			return result.value;
		};
		const auto api_rejected = [&](const char *name, std::initializer_list<uint64_t> args, std::string_view reason) {
			const auto address = modules.resolve({"kernel32.dll", name, 0, false, 0});
			check(process.call(address, std::span(args.begin(), args.size()), std::chrono::seconds(1)).stop == raven::NativeStop::api,
				"resource negative did not reach its API");
			std::string failure;
			try {
				resources.dispatch(name);
			} catch (const std::runtime_error &error) {
				failure = error.what();
			}
			check(!failure.empty() && failure.find(reason) != failure.npos, "resource negative failed for a different reason");
			process.return_api(0);
			check(process.resume(std::chrono::seconds(1)).stop == raven::NativeStop::returned,
				"resource negative lost its pending API context");
		};
		api_rejected("FindResourceW", {0, 0x400000000000ULL, 10}, "readable");
		const auto entry_info = api("FindResourceExW", {0, 10, 101, 0x409});
		check(entry_info != 0, "resource entry handle missing");
		const auto entry_data = api("LoadResource", {0, entry_info});
		check(entry_data && api("LockResource", {entry_data}) == entry_data, "resource data identity differs");
		check(api("SizeofResource", {0, entry_info + 1}) == 0 && environment.last_error() == 6,
			"forged resource info was accepted");
		check(api("LockResource", {entry_data + 1}) == 0 && environment.last_error() == 6,
			"forged resource data was accepted");
		// A neutral lookup over several languages follows the guest locale (en-US), like the thread language of a real system.
		check(api("FindResourceW", {0, 101, 10}) == entry_info, "neutral lookup did not take the locale language");
		// A program changes its resources only after it makes their read-only pages writable, which the resource cache notices.
		{
			const auto directory = entry.image.directories[2];
			const uint64_t first = (entry.loaded.base() + directory.address) & ~uint64_t(4095);
			const uint64_t end = (entry.loaded.base() + directory.address + directory.size + 4095) & ~uint64_t(4095);
			process.protect_image(first, end - first, PROT_READ | PROT_WRITE);
		}
		const auto field = static_cast<size_t>(entry_info - entry.loaded.base());
		entry.loaded.data()[field + 4] = 2;
		check(api("SizeofResource", {0, entry_info}) == 2, "live resource metadata was cached");
		entry.loaded.data()[field + 4] = 4;
		const auto saved = std::vector<uint8_t>(entry.loaded.data().begin() + static_cast<ptrdiff_t>(field),
			entry.loaded.data().begin() + static_cast<ptrdiff_t>(field + 4));
		for (size_t index = 0; index < 4; ++index)
			entry.loaded.data()[field + index] = static_cast<uint8_t>((entry.image.image_size + 1) >> (index * 8));
		api_rejected("LoadResource", {0, entry_info}, "payload exceeds image");
		std::copy(saved.begin(), saved.end(), entry.loaded.data().begin() + static_cast<ptrdiff_t>(field));
		const auto strings = process.map_memory(4096, PROT_READ | PROT_WRITE);
		const uint8_t overflow[] = {'#', '9', '9', '9', '9', '9', '9', '9', '9', '9', '9', 0};
		process.write_memory(strings, overflow);
		check(api("FindResourceA", {0, strings, 10}) == 0 && environment.last_error() == 87,
			"overflowed decimal resource ID was accepted");
		const uint8_t ansi[] = {0xe9, 0};
		process.write_memory(strings, ansi);
		check(api("FindResourceA", {0, strings, 10}) == 0 && environment.last_error() == 120,
			"unsupported ANSI code page was guessed");
		auto &late = modules.add("resources.dll", read_file(directory / "resources.dll"));
		process.map_image(late.loaded);
		const auto late_base = late.loaded.base();
		const auto late_info = api("FindResourceExW", {late_base, 10, 101, 0x409});
		const auto late_data = api("LoadResource", {late_base, late_info});
		check(late_info && late_data && api("LockResource", {late_data}) == late_data,
			"late image resources are unavailable");
		check(api("LoadResource", {late_base, entry_info}) == 0 && environment.last_error() == 6,
			"resource info crossed module ownership");
		process.unmap_image(late_base);
		modules.remove(late);
		check(api("FindResourceExW", {late_base, 10, 101, 0x409}) == 0 && environment.last_error() == 6,
			"unloaded resource module was accepted");
		check(api("LoadResource", {0, late_info}) == 0 && environment.last_error() == 6,
			"unloaded resource info survived its module");
		check(api("LockResource", {late_data}) == 0 && environment.last_error() == 6,
			"unloaded resource data survived its module");
		std::cout << "SDK resource workflow, live image state, negative ownership and late unload contracts passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
