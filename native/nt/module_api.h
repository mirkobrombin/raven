#pragma once

#include "environment.h"

#include <optional>
#include <functional>

namespace raven {

class NativeModuleAPI {
public:
	NativeModuleAPI(NativeProcess &process, NativeEnvironment &environment,
		const ModuleSet &modules, const PEModule &entry);
	bool dispatch(std::string_view name);
	void set_loader(std::function<uint64_t(std::string)> load, std::function<bool(uint64_t)> free);
	// The flags of the LoadLibraryEx call being served, zero for LoadLibrary.
	uint32_t load_flags() const { return flags; }
	// A library asked for by a path is a particular file, which only the loader that read it can tell is already in the process.
	void set_path_lookup(std::function<uint64_t(const std::string &)> lookup) { path_lookup = std::move(lookup); }
	// GetModuleHandle's lookup: the program for a null name, else the loaded module the guest string names.
	uint64_t module_handle(uint64_t address, bool wide);

private:
	NativeProcess &process;
	NativeEnvironment &environment;
	const ModuleSet &modules;
	const PEModule &entry;
	std::function<uint64_t(std::string)> load;
	std::function<bool(uint64_t)> free;
	std::function<uint64_t(const std::string &)> path_lookup;
	uint32_t flags = 0;
	std::optional<std::string> guest_name(uint64_t address, bool wide) const;
	uint64_t procedure(uint64_t handle, uint64_t name);
};

}
