#pragma once

#include "pe/module_set.h"
#include "cpu/native_cpu.h"

#include <filesystem>
#include <map>
#include <functional>
#include <optional>
#include <string>

namespace raven {

void validate_native_image(const PEImage &image,std::string_view name,bool is_64=true);
// Where a program starts: a .NET program, whose image has a runtime header, at mscoree.dll's _CorExeMain, as Windows starts it whatever
// its entry point says; any other at its entry point.
uint64_t program_start(const ModuleSet &modules, const PEModule &entry);
class NativeMemory;
// Tells the memory manager where the program images are and how their sections are protected.
void register_images(NativeMemory &memory, const ModuleSet &modules);
void register_image(NativeMemory &memory, const PEModule &module);
// Ties each API set name the graph imports to the system library that implements it, once that library is in the set.
void alias_api_sets(ModuleSet &modules);
void forward_opengl_formats(ModuleSet &modules, PEModule &provider);
void load_initial_dependencies(ModuleSet &modules,
	const std::function<std::optional<ModuleBytes>(std::string_view)> &source,
	std::span<const std::string_view> native_modules,bool is_64=true,bool tolerate_missing=false);

// The copies of system libraries that a program ships beside its executable and that load instead of the runtime's (program_may_replace).
// Only the names asked about are looked for: the folder is listed once, on the first such name, and only a file that answers a name is
// read, to see that it is a DLL of the program's width.
class ProgramLibraries {
public:
	ProgramLibraries() = default;
	ProgramLibraries(std::u16string_view image_path, bool is_64);
	bool contains(std::string_view dll);

private:
	std::filesystem::path folder;
	bool is_64 = true;
	std::optional<std::map<std::string, std::filesystem::path>> files;
	std::map<std::string, bool, std::less<>> answers;
};

std::vector<const PEModule *> initialization_order(const ModuleSet &modules, const PEModule &entry);
uint64_t initialize_loader_data(NativeProcess &process, uint64_t peb,
	const ModuleSet &modules, const PEModule &entry);

class NativeLoaderData {
public:
	NativeLoaderData(NativeProcess &process, uint64_t peb, const ModuleSet &modules, const PEModule &entry);
	void add(const PEModule &module);
	void initialized(const PEModule &module);
	void remove(const PEModule &module);
	uint64_t address() const { return loader; }

private:
	struct Node { uint64_t address; uint64_t allocation; };
	NativeProcess &process;
	const ModuleSet &modules;
	uint64_t peb;
	uint64_t loader;
	std::map<const PEModule *, Node> nodes;
	std::vector<const PEModule *> loaded;
	std::vector<const PEModule *> started;
	void validate() const;
	void publish(const std::map<const PEModule *, Node> &proposed,
		const std::vector<const PEModule *> &load, const std::vector<const PEModule *> &init);
};

}
