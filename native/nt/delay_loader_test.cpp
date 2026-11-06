#include "providers.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static std::vector<uint8_t> read_file(const std::filesystem::path &path)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file || file.tellg() < 0 || file.tellg() > 1024 * 1024 * 1024)
		throw std::runtime_error("invalid delay fixture file");
	std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg()));
	file.seekg(0);
	if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
		throw std::runtime_error("cannot read delay fixture");
	return bytes;
}

int main(int argc, char **argv)
{
	try {
		if (argc != 3)
			throw std::runtime_error("bootstrap and delay fixture directory required");
		const std::filesystem::path directory(argv[2]);
		for (const auto mode : {0, 1, 2}) {
			raven::ModuleSet modules;
			auto &entry = modules.add("client.exe", read_file(directory / "client.exe"));
			const auto delayed = entry.image.delay_imports();
			check(delayed.size() == 1 && delayed[0].module == "delayed.dll" && delayed[0].imports.size() == 2,
				"fixture is not a real linker-generated delay graph");
			check(std::any_of(delayed[0].imports.begin(), delayed[0].imports.end(), [](const auto &s) { return s.by_ordinal && s.ordinal == 9; }),
				"fixture ordinal delay import differs");
			unsigned requests = 0;
			const auto source = [&](std::string_view name) -> std::optional<std::vector<uint8_t>> {
				check(name == "delayed.dll", "delay load requested an unexpected dependency");
				++requests;
				if (mode == 1)
					return std::nullopt;
				auto bytes = read_file(directory / "delayed.dll");
				if (mode == 2) {
					const std::array<uint8_t, 6> name{'N', 'a', 'm', 'e', 'd', 0};
					const auto found = std::search(bytes.begin(), bytes.end(), name.begin(), name.end());
					check(found != bytes.end(), "cannot alter fixture export name");
					*found = 'M';
				}
				return bytes;
			};
			const auto result = raven::run_native_process(modules, entry, argv[1], std::chrono::seconds(2), source);
			check(requests == 1, "delay helper loaded eagerly or repeatedly");
			const auto field = [&](const char *name) {
				const auto address = modules.resolve({"client.exe", name, 0, false, 0}) - entry.loaded.base();
				check(address <= entry.loaded.data().size() - 4, "delay fixture field is outside image");
				uint32_t value = 0;
				for (size_t index = 0; index < 4; ++index)
					value |= uint32_t(entry.loaded.data()[static_cast<size_t>(address) + index]) << (index * 8);
				return value;
			};
			if (!mode) {
				std::cout << "Delay notifications: loads=" << field("loads") << " procedures=" << field("procedures")
					<< " starts=" << field("starts") << " ends=" << field("ends") << '\n';
				check(result.stop == raven::NativeStop::returned && result.value == 0 && field("entered") == 1,
					"real lazy delay workflow failed");
				check(!modules.all().contains("delayed.dll"), "guest FreeLibrary retained the delayed DLL");
			} else {
				check(result.stop == raven::NativeStop::exception && result.value == (mode == 1 ? 0xc06d007eULL : 0xc06d007fULL),
					"delay helper failure did not raise its software exception");
				check(result.exception_flags == 0 && result.exception_information.size() == 1 && result.exception_information[0] != 0,
					"delay helper exception parameters were lost");
				check(field("entered") == 0 && field("failure_error") == (mode == 1 ? 126U : 127U) && field("failure_reason") == (mode == 1 ? 3U : 4U),
					"delay helper failure resumed or lost its observed error");
			}
		}
		std::cout << "Guest delay helper, lazy name/ordinal binding, cached calls and raised failure records passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
