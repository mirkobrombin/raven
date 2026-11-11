#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raven {

// The programs installed in a prefix and what each one owns: what its installer made or changed, found by comparing the prefix with the
// snapshot taken before the installation. Every program of the prefix reads all of it; a program installed this way writes straight into
// what it owns, and what it writes anywhere else lands in its own layer, beside the prefix, which only it sees and which can be thrown
// away. A program nothing installed this way, and the system's own, write into the prefix as they always did.
//
// The records live in <prefix>.apps, which a confined runtime only reads; the layers in <prefix>.layers, one folder each.
struct InstalledApp {
	std::string id;
	// Paths relative to the prefix, separated by '/': a folder the installer made is owned with everything later put in it, a file it made
	// or changed is owned alone.
	std::vector<std::string> folders, files;
	// Registry keys by their full path in upper case: a key the installer made is owned with everything below it, a key whose values it
	// changed is owned alone.
	std::vector<std::u16string> keys, changed_keys;
};

std::filesystem::path apps_directory(const std::filesystem::path &prefix);
std::filesystem::path layers_directory(const std::filesystem::path &prefix);
std::vector<InstalledApp> installed_apps(const std::filesystem::path &prefix);
// The installed program an executable belongs to; none for one under C:\Windows, which belongs to the system.
std::optional<InstalledApp> app_of(const std::filesystem::path &prefix, const std::filesystem::path &executable);
// Records what an installation made: the difference between the prefix now and the snapshot taken before it.
InstalledApp record_installation(const std::filesystem::path &prefix, const std::filesystem::path &snapshot, std::string_view name);
// Throws an installed program's layer away: the program sees the prefix as its installer left it again.
bool discard_layer(const std::filesystem::path &prefix, std::string_view id);
bool remove_app(const std::filesystem::path &prefix, std::string_view id);

// The identity the program this runtime runs takes from its executable.
class AppLayer {
public:
	void set(const std::filesystem::path &prefix, InstalledApp app);
	bool active() const { return !owner.id.empty(); }
	const InstalledApp &app() const { return owner; }
	// The folder that stands for the prefix in the layer.
	const std::filesystem::path &root() const { return layer; }
	const std::filesystem::path &prefix_root() const { return prefix; }
	// Whether the program writes a prefix path, given relative to the prefix, in place: one it owns or the person's own documents and
	// temporary files, which every program shares.
	bool owns(std::string_view relative) const;
	bool owns_key(std::u16string_view folded) const;
	// Paths deleted in the layer that the prefix still holds, relative to the prefix.
	bool deleted(std::string_view relative) const;
	void mark_deleted(std::string_view relative, bool value) const;
	// A file of the layer went away, deleted or moved: the prefix file it stood for stays hidden from the program.
	void hide(const std::filesystem::path &host) const;
	std::filesystem::path registry() const { return layer.parent_path() / (layer.filename().string() + ".reg"); }

private:
	void read_deleted() const;
	InstalledApp owner;
	std::filesystem::path prefix, layer;
	mutable std::vector<std::string> removed;
	mutable long long removed_stamp = -2;
};

AppLayer &app_layer();

}
