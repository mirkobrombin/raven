#include "providers.h"

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
		throw std::runtime_error("invalid dynamic fixture file");
	std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg()));
	file.seekg(0);
	if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
		throw std::runtime_error("cannot read dynamic fixture");
	return bytes;
}

int main(int argc, char **argv)
{
	try {
		if (argc != 3)
			throw std::runtime_error("bootstrap and dynamic fixture directory required");
		const std::filesystem::path directory(argv[2]);
		for (const auto *mode : {"success", "missing", "dependency", "reject", "fault", "loop", "detach_fault", "detach_loop", "executable", "inert"}) {
			raven::ModuleSet modules;
			auto &entry = modules.add("client.exe", read_file(directory / "failure.exe"));
			const std::string selected(mode);
			const auto source = [&](std::string_view name) -> std::optional<std::vector<uint8_t>> {
				if (name == "tlsprobe.dll" && selected != "dependency")
					return read_file(directory / "tlsprobe.dll");
				if (name != "probe.dll" || selected == "missing")
					return std::nullopt;
				const auto filename = selected == "success" || selected == "executable" || selected == "inert" ? "tlsprobe.dll" :
					selected == "dependency" ? "parent.dll" : selected + ".dll";
				auto bytes = read_file(directory / filename);
				if (selected == "executable" || selected == "inert") {
					// A valid executable image is not a LoadLibrary DLL, unless it has no entry point to run.
					size_t offset = 22;
					for (size_t index = 0; index < 4; ++index)
						offset += size_t(bytes.at(0x3c + index)) << (index * 8);
					bytes.at(offset + 1) &= static_cast<uint8_t>(~0x20U);
					if (selected == "inert")
						for (size_t index = 0; index < 4; ++index)
							bytes.at(offset + 18 + index) = 0;
				}
				return bytes;
			};
			// Process startup alone can take most of a second on a loaded host: only the looping DLLs need a short limit.
			const auto timeout = std::chrono::milliseconds(selected.ends_with("loop") ? 2000 : 10000);
			const auto result = raven::run_native_process(modules, entry, argv[1], timeout, source);
			const auto address = modules.resolve({"client.exe", "entered", 0, false, 0}) - entry.loaded.base();
			check(address < entry.loaded.data().size(), "fixture state is outside image");
			const auto entered = entry.loaded.data()[static_cast<size_t>(address)];
			if (selected == "fault" || selected == "loop" || selected == "detach_fault" || selected == "detach_loop") {
				check(result.stop == (selected.ends_with("fault") ? raven::NativeStop::fault : raven::NativeStop::timeout), "dynamic DLL did not propagate terminal stop");
				check(entered == (selected.starts_with("detach_") ? 1 : 0), "dynamic DLL stop resumed the guest caller");
			} else {
				const uint64_t error = selected == "success" || selected == "inert" ? 0 : selected == "reject" ? 1114 : selected == "executable" ? 120 : 126;
				check(result.stop == raven::NativeStop::returned && result.value == error && entered == 1, "dynamic loader failure contract differs");
				check(!modules.all().contains("probe.dll") && !modules.all().contains("tlsprobe.dll"), "dynamic rollback or release retained graph images");
			}
		}
		std::cout << "Dynamic guest loading, dependency rollback, unsupported executable loading, entry-less executable loading and terminal attach stops passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
