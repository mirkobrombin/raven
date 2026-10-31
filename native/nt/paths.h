#pragma once

#include "prefix.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace raven {

// Windows path semantics over a prefix: C: is the prefix drive, Z: is the host root, other letters map to drive_<letter>.
class NativePaths {
public:
	explicit NativePaths(const NativePrefix &prefix);
	const std::u16string &current() const { return directory; }
	// The directory must exist; the stored form is a full path without a trailing separator (except for roots).
	bool set_current(std::u16string_view path);
	bool set_dll_directory(std::optional<std::u16string> path);
	const std::optional<std::u16string> &dll_directory() const { return dll_folder; }
	// Full drive-qualified path without "." or ".." parts and with backslashes; empty when the text is not a path.
	std::u16string full(std::u16string_view path) const;
	struct Resolved {
		std::filesystem::path host;
		bool exists = false;
		// Every directory above the final component exists, so a new file can be created.
		bool parent_exists = false;
	};
	std::optional<Resolved> resolve(std::u16string_view path) const;
	// DOS spelling of a host path inside a mapped drive.
	std::u16string dos(const std::filesystem::path &host) const;
	// User name shown to the guest; the host login name keeps profiles of different accounts apart.
	std::u16string user_name() const;
	// C:\Users\<user>, created on first use together with AppData\Roaming and AppData\Local\Temp.
	std::u16string user_profile() const;
	std::u16string temporary() const;
	// Where a program of the given width finds the system's libraries: System32, or SysWOW64 for a 32-bit program.
	static std::u16string system_directory(bool wide) { return wide ? u"C:\\Windows\\System32" : u"C:\\Windows\\SysWOW64"; }
	// The file a library loaded by name came from, in the loader's search order: beside the program, then in the system directory.
	std::u16string library_location(std::u16string_view image_path, std::u16string_view name, bool wide) const;
	std::optional<Resolved> find_library(std::u16string_view image_path, std::u16string_view name, bool wide, uint32_t flags = 0) const;
	// A 32-bit program sees SysWOW64 where it names System32, and System32 where it names Sysnative, while redirection is on.
	void set_wow64(bool enabled) { wow64 = enabled; }
	// Turns redirection on or off and tells whether it was on.
	bool redirect(bool enabled) { return std::exchange(redirecting, enabled); }
	// The roots of the virtual application packages installed in the prefix, asked for when a path is missing: a file such a package
	// keeps under its Root\VFS folder appears at the place it stands for, as the App-V file system shows it to the package's programs.
	void set_package_roots(std::function<std::vector<std::u16string>()> source) { package_roots = std::move(source); }
	// The program's PATH as it is now, which the standard library search ends with.
	void set_search_path(std::function<std::u16string()> source) { search_path = std::move(source); }
	// The folders of the program's PATH as it is now, where the standard searches for programs and libraries end.
	std::vector<std::u16string> search_folders() const { return search_path ? path_folders(search_path()) : std::vector<std::u16string>(); }
	// The folders of a PATH value: separated by semicolons, empty entries skipped, surrounding quotes removed.
	static std::vector<std::u16string> path_folders(std::u16string_view value);
	// The value of a variable in an environment block ("NAME=VALUE" entries each ended by a zero), the name compared without case.
	static std::optional<std::u16string> environment_value(std::u16string_view block, std::u16string_view name);
	// True for the DOS device names that are not files (NUL, CON, ...), optionally behind \\.\ or \\?\.
	static bool device(std::u16string_view path);
	// The volume a drive is, by its \\?\Volume{GUID}\ name: the GUID is made from the drive letter, so a drive is the same volume
	// every time. A path may start with that name instead of the drive.
	static std::u16string volume_name(char16_t letter);

private:
	const NativePrefix &prefix;
	std::u16string directory;
	std::optional<std::u16string> dll_folder;
	bool wow64 = false, redirecting = true;
	std::function<std::vector<std::u16string>()> package_roots;
	std::function<std::u16string()> search_path;
	std::optional<std::filesystem::path> drive_root(char16_t letter) const;
	std::optional<Resolved> resolve_direct(const std::u16string &path) const;
	std::optional<Resolved> resolve_virtual(const std::u16string &path) const;
	std::u16string redirected(std::u16string path) const;
};

}
