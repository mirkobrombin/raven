#pragma once

#include "loaded_image.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

namespace raven {

// The file a module comes from: bytes held in memory, or a read-only mapping of the host file. A mapping shares its pages with every
// process that loads the same library, where reading a library of hundreds of megabytes into each of them cost seconds.
class ModuleBytes {
public:
	ModuleBytes(std::vector<uint8_t> bytes) : owned(std::move(bytes)), view(owned) {}
	// A move keeps the vector's buffer, which the view names; a copy would not.
	ModuleBytes(ModuleBytes &&) = default;
	ModuleBytes &operator=(ModuleBytes &&) = default;
	ModuleBytes(const ModuleBytes &) = delete;
	ModuleBytes &operator=(const ModuleBytes &) = delete;
	static std::optional<ModuleBytes> map(const std::filesystem::path &path);
	operator std::span<const uint8_t>() const { return view; }

private:
	ModuleBytes() = default;
	std::vector<uint8_t> owned;
	std::shared_ptr<const void> mapping;
	std::span<const uint8_t> view;
};

struct PEModule {
	PEModule(std::string name, ModuleBytes bytes, uint64_t base, bool relocated = true);
	// The index of export names views text the module holds, which a copy would not carry along.
	PEModule(const PEModule &) = delete;
	PEModule &operator=(const PEModule &) = delete;
	std::string name;
	ModuleBytes bytes;
	PEImage image;
	LoadedImage loaded;
	std::vector<PEImport> imports;
	std::vector<PEExport> exports;
	// The names of the exports one after another, and the first export that carries each name, viewed in that text: imports are resolved by
	// name, thousands of them for a large library, and the index is made without a copy of each name.
	std::string export_text;
	std::unordered_map<std::string_view, size_t> export_names;
	// How many exports the index covers: a lookup that misses it scans only when exports came after. Exports stand at their ordinal less
	// the first one's, as the address table holds them, and need no index of their own.
	size_t indexed_exports = 0;
	bool runtime_api = false;
	// Whether the library asked not to be told when threads start and end (DisableThreadLibraryCalls).
	bool quiet_threads = false;
	// The host file a library loaded from a folder of the program's came from; empty for the runtime's own and for libraries found by name.
	std::filesystem::path file;
};

// Owns module bytes and shared images. Resolves only explicitly supplied modules.
class ModuleSet {
public:
	// Keep a free preferred base; relocate colliding images into graph-owned space.
	// A process loader also excludes its allocations through available.
	PEModule &add(std::string name, ModuleBytes bytes, std::function<bool(uint64_t, uint64_t)> available = {});
	PEModule &add(std::string name, ModuleBytes bytes, uint64_t base);
	// The process loader must release guest mappings and references before erasing an image.
	void remove(const PEModule &module);
	// An image mapped only to be read, as a data file or for its resources: it may have the other width, and nothing resolves through it.
	PEModule &add_data(std::string name, ModuleBytes bytes, std::function<bool(uint64_t, uint64_t)> available = {});
	const std::map<std::string, std::unique_ptr<PEModule>> &all() const;
	uint64_t resolve(const PEImport &symbol) const;
	uint64_t resolve_import(const PEImport &symbol) const;
	std::vector<const PEModule *> resolution_path(const PEImport &symbol) const;
	// An opt-in diagnostic target for a missing static import. Real exports and explicit lookups keep their usual resolution.
	void set_import_probe(std::function<uint64_t(std::string_view, std::string_view, uint32_t)> probe);
	size_t bind_imports();
	// Lets imports that name an API set contract resolve through the module that implements it.
	void add_alias(std::string name, std::string target);

private:
	std::map<std::string, std::unique_ptr<PEModule>> modules;
	std::map<std::string, std::string> aliases;
	std::set<std::string> data_names;
	std::set<std::string> movable_names;
	bool adding_data = false;
	bool bound_any = false;
	std::function<uint64_t(std::string_view, std::string_view, uint32_t)> import_probe;
	uint64_t resolve(std::string module, std::string name, uint32_t ordinal, bool by_ordinal,
		std::set<std::pair<std::string, uint32_t>> &chain, std::vector<const PEModule *> *path = nullptr, bool static_import = false) const;
};

}
