#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raven {

// The programs of a prefix and what each one owns: what its installer made or changed, found by comparing the prefix with the snapshot
// taken before the installation, and whatever the program itself makes that nothing held before, noted as it makes it. A program nothing
// installed is known by its executable. Every program of the prefix reads all of it; a program writes straight into what it owns, and
// what it writes anywhere else lands in its own layer, beside the prefix, which only it sees and which can be thrown away. Installers and
// the system's own programs write into the prefix as they always did.
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
	// A program nothing installed is known by its executable, recorded the first time it runs.
	std::string executable;
};

std::filesystem::path apps_directory(const std::filesystem::path &prefix);
std::filesystem::path layers_directory(const std::filesystem::path &prefix);
std::vector<InstalledApp> installed_apps(const std::filesystem::path &prefix);
// The program an executable belongs to: the one that installed it, or made it where nothing was, as a launcher makes the games it
// downloads; none for one under C:\Windows, which belongs to the system.
std::optional<InstalledApp> app_of(const std::filesystem::path &prefix, const std::filesystem::path &executable);
// The identity every other program runs with: the installed program its executable belongs to, or else the executable itself, recorded
// the first time it runs. A program nothing installed owns the folder it runs from when that folder is in the prefix and its own.
std::optional<InstalledApp> identity_of(const std::filesystem::path &prefix, const std::filesystem::path &executable);
// Records what an installation made: the difference between the prefix now and the snapshot taken before it.
InstalledApp record_installation(const std::filesystem::path &prefix, const std::filesystem::path &snapshot, std::string_view name);
// The name an installation is recorded under: the installer's own, or for the Windows Installer the package it was given to install.
std::string installation_name(std::string_view program, const std::vector<std::string_view> &arguments);
// Throws an installed program's layer away: the program sees the prefix as its installer left it again.
bool discard_layer(const std::filesystem::path &prefix, std::string_view id);
bool remove_app(const std::filesystem::path &prefix, std::string_view id);
// Whether a key, by its full path in upper case, is one through which programs join the system, which every program writes for all.
bool registration_key(std::u16string_view folded);

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
	// Notes a file, folder or key the program made where nothing was: it is the program's from then on, and every program sees it.
	void claim(std::string_view relative) const;
	void claim_key(std::u16string_view folded) const;
	// Paths deleted in the layer that the prefix still holds, relative to the prefix.
	bool deleted(std::string_view relative) const;
	void mark_deleted(std::string_view relative, bool value) const;
	// A file of the layer went away, deleted or moved: the prefix file it stood for stays hidden from the program.
	void hide(const std::filesystem::path &host) const;
	std::filesystem::path registry() const { return layer.parent_path() / (layer.filename().string() + ".reg"); }

private:
	void read_deleted() const;
	void read_made() const;
	InstalledApp owner;
	std::filesystem::path prefix, layer;
	mutable std::vector<std::string> removed, made;
	mutable std::vector<std::u16string> made_keys;
	mutable long long removed_stamp = -2, made_stamp = -2;
};

AppLayer &app_layer();

}
