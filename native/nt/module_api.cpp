#include <cstring>
#include "module_api.h"
#include "apiset.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>

namespace raven {

NativeModuleAPI::NativeModuleAPI(NativeProcess &process, NativeEnvironment &environment,
	const ModuleSet &modules, const PEModule &entry)
	: process(process), environment(environment), modules(modules), entry(entry) {}

std::optional<std::string> NativeModuleAPI::guest_name(uint64_t address, bool wide) const
{
	std::string result;
	const size_t width = wide ? 2 : 1;
	for (size_t index = 0; index < 32768; ++index) {
		if (address > UINT64_MAX - index * width)
			throw std::runtime_error("native module API string address overflows");
		const auto bytes = process.read_memory(address + index * width, width);
		const uint32_t character = uint32_t(bytes[0]) | (wide ? uint32_t(bytes[1]) << 8 : 0);
		if (!character)
			return result;
		if (wide && character > 127)
			return std::nullopt;
		result += static_cast<char>(character);
	}
	throw std::runtime_error("native module API string exceeds initial limit");
}

uint64_t NativeModuleAPI::module_handle(uint64_t address, bool wide)
{
	if (!address)
		return entry.loaded.base();
	const auto decoded = guest_name(address, wide);
	if (decoded && path_lookup && decoded->find_first_of("/\\:") != std::string::npos) {
		const auto base = path_lookup(*decoded);
		if (!base)
			environment.set_last_error(126);
		return base;
	}
	if (!decoded || std::any_of(decoded->begin(), decoded->end(), [](unsigned char c) { return c > 127; }) ||
		decoded->find_first_of("/\\:") != std::string::npos) {
		environment.set_last_error(120);
		return 0;
	}
	std::string name = *decoded;
	if (name.empty()) {
		environment.set_last_error(126);
		return 0;
	}
	for (auto &character : name)
		if (character >= 'A' && character <= 'Z')
			character = static_cast<char>(character + ('a' - 'A'));
	if (!name.empty() && name.back() == '.')
		name.pop_back();
	else if (name.find('.') == std::string::npos)
		name += ".dll";
	if (const auto host = apiset_host(name); !host.empty())
		name = host;
	const auto found = modules.all().find(name);
	if (found == modules.all().end()) {
		environment.set_last_error(126);
		return 0;
	}
	return found->second->loaded.base();
}

uint64_t NativeModuleAPI::procedure(uint64_t handle, uint64_t name)
{
	const auto found = std::find_if(modules.all().begin(), modules.all().end(), [&](const auto &item) {
		return item.second->loaded.base() == handle;
	});
	if (found == modules.all().end()) {
		environment.set_last_error(126);
		return 0;
	}
	const bool ordinal = name <= UINT16_MAX;
	const auto symbol = ordinal ? std::string{} : guest_name(name, false).value();
	// Loading a forwarder's target adds to the modules, so the module's name is kept apart from the entry that found it.
	const std::string module = found->first;
	// A forwarder names the library its target lives in; the lookup loads that library when no one has yet, as the loader does for an
	// import, and goes on. Each library is tried once, so a chain that cannot be completed ends.
	std::set<std::string> loaded;
	for (;;) {
		try {
			return modules.resolve({module, symbol, static_cast<uint16_t>(ordinal ? name : 0), ordinal, 0});
		} catch (const std::runtime_error &error) {
			const std::string_view message = error.what();
			if (message.starts_with("missing PE module: ") && load) {
				std::string target(message.substr(19));
				if (loaded.insert(target).second && load(target))
					continue;
			}
			if (std::getenv("RAVEN_TRACE_FAIL"))
				std::cerr << "raven: GetProcAddress " << module << ' ' << (ordinal ? "#" + std::to_string(name) : symbol) << " fails: " << message << '\n';
			if (message.starts_with("missing PE export:"))
				environment.set_last_error(127);
			else if (message.starts_with("missing PE module:"))
				environment.set_last_error(126);
			else
				throw;
			return 0;
		}
	}
}

bool NativeModuleAPI::dispatch(std::string_view name)
{
	if (name == "LoadLibraryA" || name == "LoadLibraryW" || name == "LoadLibraryExA" || name == "LoadLibraryExW") {
		if (!load)
			return false;
		const bool extended = name.starts_with("LoadLibraryEx");
		const auto arguments = process.api_arguments(extended ? 3 : 1);
		const auto address = arguments.at(0);
		if (!address) {
			environment.set_last_error(87);
			process.return_api(0);
			return true;
		}
		const auto decoded = guest_name(address, name.ends_with("W"));
		if (!decoded) {
			environment.set_last_error(120);
			process.return_api(0);
		} else {
			flags = extended ? uint32_t(arguments.at(2)) : 0;
			const auto loaded = load(*decoded);
			flags = 0;
			process.return_api(loaded);
		}
		return true;
	}
	// ResolveDelayLoadedAPI(base, descriptor, dll hook, system hook, thunk, flags): the library of a delay-load descriptor is loaded on
	// first use and its handle kept where the descriptor says; the function the thunk stands for is resolved by name or ordinal from
	// the matching entry of the name table and written into the thunk.
	if (name == "ResolveDelayLoadedAPI") {
		if (!load)
			return false;
		const auto arguments = process.api_arguments(6);
		const uint64_t base = arguments[0], thunk = arguments[4];
		const auto read32 = [&](uint64_t address) {
			const auto bytes = process.read_memory(address, 4);
			return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
		};
		const auto read64 = [&](uint64_t address) {
			const auto bytes = process.read_memory(address, 8);
			uint64_t value = 0;
			std::memcpy(&value, bytes.data(), 8);
			return value;
		};
		const auto descriptor = arguments[1];
		const uint32_t name_rva = read32(descriptor + 4), handle_rva = read32(descriptor + 8), table_rva = read32(descriptor + 12),
			names_rva = read32(descriptor + 16);
		uint64_t handle = handle_rva ? read64(base + handle_rva) : 0;
		if (!handle) {
			const auto library = guest_name(base + name_rva, false);
			handle = library ? load(*library) : 0;
			if (handle && handle_rva) {
				std::vector<uint8_t> bytes(8);
				std::memcpy(bytes.data(), &handle, 8);
				process.write_memory(base + handle_rva, bytes);
			}
		}
		uint64_t result = 0;
		if (handle) {
			const uint64_t entry = read64(base + names_rva + (thunk - (base + table_rva)));
			result = procedure(handle, entry >> 63 ? (entry & 0xffff) : base + uint32_t(entry) + 2);
		}
		if (result) {
			std::vector<uint8_t> bytes(8);
			std::memcpy(bytes.data(), &result, 8);
			process.write_memory(thunk, bytes);
		} else if (!handle) {
			environment.set_last_error(126);
		}
		process.return_api(result);
		return true;
	}
	if (name == "FreeLibrary") {
		if (!free)
			return false;
		const auto handle = process.api_arguments(1).at(0);
		process.return_api(free(handle));
		return true;
	}
	if (name != "GetModuleHandleA" && name != "GetModuleHandleW" && name != "GetProcAddress")
		return false;
	const auto arguments = process.api_arguments(name == "GetProcAddress" ? 2 : 1);
	const auto result = name == "GetProcAddress" ? procedure(arguments[0], arguments[1]) :
		module_handle(arguments[0], name == "GetModuleHandleW");
	process.return_api(result);
	return true;
}

void NativeModuleAPI::set_loader(std::function<uint64_t(std::string)> load,
	std::function<bool(uint64_t)> free)
{
	this->load = std::move(load);
	this->free = std::move(free);
}

}
