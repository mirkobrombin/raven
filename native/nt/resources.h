#pragma once

#include "environment.h"
#include "pe/resources.h"

#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace raven {

struct NativeResourceView {
	uint64_t address;
	uint32_t size;
	uint32_t language;
};

class NativeResources {
public:
	NativeResources(NativeProcess &process, NativeEnvironment &environment,
		const ModuleSet &modules, const PEModule &entry);
	bool dispatch(std::string_view name);
	std::optional<NativeResourceView> view(uint64_t handle, uint32_t type, uint64_t name, uint16_t language = 0);

private:
	NativeProcess &process;
	NativeEnvironment &environment;
	const ModuleSet &modules;
	const PEModule &entry;
	const PEModule *module(uint64_t handle);
	// The last parse of each module's resource directory, with the parts of the directory it read and their bytes: a lookup compares those
	// parts of the live image and parses again only when one changed. Programs look up hundreds of resources while they start, and the
	// directory holds the resource payloads too, which the parse never reads.
	struct Parsed {
		std::vector<std::pair<size_t, size_t>> read;
		std::vector<uint8_t> bytes;
		std::vector<PEResource> resources;
		bool valid = false;
		// A directory the guest can only read, with no mapping changed since it was compared, cannot have changed: Office looks up
		// resources while it draws, and its directories hold megabytes of parsed parts.
		bool read_only = false;
		uint64_t mapping_changes = 0;
		// The resources by type and name, with names folded to upper case, by entry and by payload address: Office looks up its
		// resources by the thousand while it draws, in directories of thousands of entries.
		std::map<std::pair<ResourceKey, ResourceKey>, std::vector<uint32_t>> by_name;
		std::set<ResourceKey> types;
		std::unordered_map<uint32_t, uint32_t> by_entry, by_data;
	};
	mutable std::map<const PEModule *, Parsed> parsed;
	const Parsed &parse(const PEModule &image) const;
	std::optional<ResourceKey> key(uint64_t address, bool wide);
	uint64_t find(uint64_t handle, uint64_t type, uint64_t name, uint16_t language, bool wide);
	uint64_t access(uint64_t handle, uint64_t resource, bool size);
	uint64_t lock(uint64_t data);
	void readable(uint64_t address, size_t size);
};

}
