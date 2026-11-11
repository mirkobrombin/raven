#pragma once

#include <functional>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace raven {

// Win32 registry state shared by every guest handle. Status values are Win32 error codes.
class NativeRegistry {
public:
	// A registry that never touches the host file system.
	NativeRegistry() = default;
	// Loads the hive file when present. Each change is appended to it as a record, which every process of the prefix reads after what it
	// read before; the file is written again whole, atomically, once the records outgrow the hive they change.
	explicit NativeRegistry(std::filesystem::path file);
	uint32_t create(uint64_t parent, std::u16string_view path, uint64_t &key, uint32_t &disposition);
	uint32_t open(uint64_t parent, std::u16string_view path, uint64_t &key);
	uint32_t close(uint64_t key);
	// Where key handle values come from: a program's registry keys and kernel objects share one handle space when this names its source.
	// The release gives a closed key's value back to that space.
	void set_allocator(std::function<uint64_t()> allocator, std::function<void(uint64_t)> releaser = {})
	{
		allocate = std::move(allocator);
		release = std::move(releaser);
	}
	uint32_t query(uint64_t key, std::u16string_view name, uint32_t &type, std::vector<uint8_t> &data) const;
	uint32_t set(uint64_t key, std::u16string_view name, uint32_t type, std::span<const uint8_t> data);
	size_t handles() const { return open_keys.size(); }
	// Enumeration, deletion and statistics for the advapi32 surface; names sort case-insensitively like the real hive.
	uint32_t remove_key(uint64_t parent, std::u16string_view path);
	uint32_t remove_value(uint64_t key, std::u16string_view name);
	// A key with everything below it; an empty path empties the open key of its subkeys and values but keeps it.
	uint32_t remove_tree(uint64_t parent, std::u16string_view path);
	uint32_t enum_key(uint64_t key, uint32_t index, std::u16string &name) const;
	uint32_t enum_value(uint64_t key, uint32_t index, std::u16string &name, uint32_t &type, std::vector<uint8_t> &data) const;
	struct Info {
		uint32_t subkeys = 0, longest_subkey = 0, values = 0, longest_name = 0, longest_data = 0;
		// The key's full path as the hive shows it, starting with its predefined root name.
		std::u16string path;
	};
	uint32_t info(uint64_t key, Info &result) const;
	// RegOverridePredefKey: a predefined root of this process stands for an open key until the override is removed with key 0.
	uint32_t override_root(uint64_t root, uint64_t key);
	// Every key of a hive file by its full path in upper case, with its values written out, to compare two hives by.
	static std::map<std::u16string, std::string> digest(const std::filesystem::path &file);
	// A layer over the hive: a change to a key the program does not own goes to a hive file of the program's own, read over the shared
	// one, so that the program sees it and no other program does. The program an installation recorded gets its layer by itself.
	void set_layer(std::filesystem::path layer, std::function<bool(std::u16string_view)> owns, std::function<void(std::u16string_view)> claim);

private:
	struct Value { std::u16string name; uint32_t type; std::vector<uint8_t> data; };
	struct Key { std::u16string path; std::map<std::u16string, Value> values; };
	std::filesystem::path file, layer_file;
	std::function<bool(std::u16string_view)> layer_owns;
	std::function<void(std::u16string_view)> layer_claim;
	// What the hive file looked like when it was last read or written; another process that changed it makes the next call read it again.
	struct Stamp {
		long long seconds = -1, nanoseconds = 0;
		long long size = -1, inode = -1;
		bool operator==(const Stamp &other) const = default;
	};
	mutable Stamp stamp, layer_stamp;
	static Stamp stamp_of(const std::filesystem::path &path);
	Stamp current_stamp() const { return stamp_of(file); }
	void replay_layer();
	void refresh() const;
	std::map<std::u16string, Key> keys;
	std::map<uint64_t, std::u16string> open_keys;
	std::map<uint64_t, std::u16string> overrides;
	uint64_t next = 0x300;
	std::function<uint64_t()> allocate;
	std::function<void(uint64_t)> release;
	uint64_t new_key_handle()
	{
		if (allocate)
			return allocate();
		if (next > UINT32_MAX - 4)
			return 0;
		const auto value = next;
		next += 4;
		return value;
	}
	bool resolve(uint64_t parent, std::u16string_view path, std::u16string &folded, std::u16string &display) const;
	// Folded paths of the immediate subkeys of a folded key path, in key order.
	std::vector<std::u16string> children(const std::u16string &folded) const;
	void save() const;
	void load();
	// The hive file's records from `from` on: each applies to the keys in the order the file holds them. The size read is returned.
	long long replay(long long from);
	void apply(const std::string &record);
	// Appends the records of one change under the prefix's lock, after reading what other processes appended first.
	// The key the change is to decides which file holds it, when the hive has a layer.
	void journal(const std::string &records, std::u16string_view folded);
	mutable long long loaded = 0, compacted = 0;
	// The values Windows ships in its hive that programs read to learn where they are and what they run on; added when a hive lacks them.
	void seed();
	void register_runtime_classes();
	void register_animation_classes();
	void register_event_logs();
	void register_system_environment();
	bool seeding = false;
};

// The roots of the App-V packages that Click-to-Run registers, one value named after each root under HKLM\Software\Microsoft\AppVISV.
std::vector<std::u16string> virtual_package_roots(NativeRegistry &registry);

}
