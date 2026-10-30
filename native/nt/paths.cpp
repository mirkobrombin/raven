#include "paths.h"
#include "actctx.h"
#include "text.h"
#include "system_exports.h"

#include <algorithm>
#include <cstdlib>
#include <dirent.h>
#include <vector>

namespace raven {

namespace {

constexpr std::u16string_view volume_prefix = u"\\\\?\\Volume{52415645-0000-0000-0000-0000000000";

std::u16string strip_namespace(std::u16string_view path)
{
	// A volume's name stands for its drive: two hex digits of the letter, then the brace that closes the GUID.
	if (path.size() >= volume_prefix.size() + 3 && equal_fold(path.substr(0, volume_prefix.size()), volume_prefix) && path[volume_prefix.size() + 2] == u'}') {
		const auto digit = [](char16_t c) { return c >= u'0' && c <= u'9' ? c - u'0' : (c | 0x20) >= u'a' && (c | 0x20) <= u'f' ? (c | 0x20) - u'a' + 10 : -1; };
		const int high = digit(path[volume_prefix.size()]), low = digit(path[volume_prefix.size() + 1]);
		if (high >= 0 && low >= 0 && high * 16 + low >= u'A' && high * 16 + low <= u'Z')
			return std::u16string{char16_t(high * 16 + low), u':'} + std::u16string(path.substr(volume_prefix.size() + 3));
	}
	for (const std::u16string_view prefix : {u"\\\\?\\", u"\\\\.\\", u"\\??\\"})
		if (path.starts_with(prefix) && path.size() > prefix.size() + 1 && path[prefix.size() + 1] == u':')
			return std::u16string(path.substr(prefix.size()));
	return std::u16string(path);
}

// Looks for an entry that differs from name only by case, so Windows spellings find Linux files.
std::optional<std::string> case_match(const std::filesystem::path &directory, const std::string &name)
{
	std::error_code ignored;
	if (std::filesystem::exists(directory / name, ignored))
		return name;
	const auto wanted = from_utf8(name);
	DIR *handle = ::opendir(directory.c_str());
	if (!handle)
		return std::nullopt;
	std::optional<std::string> found;
	while (const auto *entry = ::readdir(handle))
		if (equal_fold(from_utf8(entry->d_name), wanted)) {
			found = entry->d_name;
			break;
		}
	::closedir(handle);
	return found;
}

}

std::u16string NativePaths::volume_name(char16_t letter)
{
	constexpr char16_t hex[] = u"0123456789abcdef";
	return std::u16string(volume_prefix) + hex[(letter >> 4) & 15] + hex[letter & 15] + u"}\\";
}

std::vector<std::u16string> NativePaths::path_folders(std::u16string_view value)
{
	std::vector<std::u16string> folders;
	for (size_t at = 0; at <= value.size();) {
		const auto end = std::min(value.find(u';', at), value.size());
		auto folder = std::u16string(value.substr(at, end - at));
		if (folder.size() >= 2 && folder.front() == u'"' && folder.back() == u'"')
			folder = folder.substr(1, folder.size() - 2);
		if (!folder.empty())
			folders.push_back(std::move(folder));
		at = end + 1;
	}
	return folders;
}

std::optional<std::u16string> NativePaths::environment_value(std::u16string_view block, std::u16string_view name)
{
	for (size_t at = 0; at < block.size();) {
		const auto end = std::min(block.find(u'\0', at), block.size());
		const auto entry = block.substr(at, end - at);
		if (entry.empty())
			break;
		if (entry.size() > name.size() && entry[name.size()] == u'=' && equal_fold(entry.substr(0, name.size()), name))
			return std::u16string(entry.substr(name.size() + 1));
		at = end + 1;
	}
	return std::nullopt;
}

NativePaths::NativePaths(const NativePrefix &prefix) : prefix(prefix), directory(u"C:\\")
{
}

std::optional<std::filesystem::path> NativePaths::drive_root(char16_t letter) const
{
	if (letter >= u'A' && letter <= u'Z')
		letter = char16_t(letter + 32);
	if (letter < u'a' || letter > u'z')
		return std::nullopt;
	if (letter == u'z')
		return std::filesystem::path("/");
	auto root = prefix.root() / (std::string("drive_") + char(letter));
	std::error_code ignored;
	if (letter != u'c' && !std::filesystem::is_directory(root, ignored))
		return std::nullopt;
	return root;
}

bool NativePaths::device(std::u16string_view path)
{
	auto name = strip_namespace(path);
	if (name.starts_with(u"\\\\.\\"))
		name.erase(0, 4);
	const auto slash = name.find_last_of(u"\\/");
	if (slash != name.npos)
		return false;
	const auto dot = name.find(u'.');
	if (dot != name.npos)
		name.erase(dot);
	for (const std::u16string_view candidate : {u"NUL", u"CON", u"PRN", u"AUX", u"CONIN$", u"CONOUT$"})
		if (equal_fold(name, candidate))
			return true;
	if (name.size() == 4 && (equal_fold(name.substr(0, 3), u"COM") || equal_fold(name.substr(0, 3), u"LPT")) && name[3] >= u'1' && name[3] <= u'9')
		return true;
	return false;
}

std::u16string NativePaths::full(std::u16string_view input) const
{
	auto text = strip_namespace(input);
	if (text.empty())
		return {};
	for (auto &character : text)
		if (character == u'/')
			character = u'\\';
	std::u16string base;
	std::u16string_view rest = text;
	if (text.size() >= 2 && text[1] == u':') {
		const char16_t letter = upper_case(text[0]);
		if (letter < u'A' || letter > u'Z')
			return {};
		rest = rest.substr(2);
		if (rest.starts_with(u"\\"))
			base = std::u16string(1, letter) + u":";
		else if (upper_case(directory[0]) == letter)
			base = directory;
		else
			base = std::u16string(1, letter) + u":\\";
	} else if (text.starts_with(u"\\\\")) {
		return {};
	} else if (text.starts_with(u"\\")) {
		base = directory.substr(0, 2);
	} else {
		base = directory;
	}
	// base is "X:" (rooted rest), "X:\" or "X:\dir..." (relative rest).
	std::vector<std::u16string> parts;
	const auto split = [&](std::u16string_view value) {
		for (size_t start = 0; start <= value.size();) {
			const auto end = value.find(u'\\', start);
			const auto part = value.substr(start, end == value.npos ? value.npos : end - start);
			if (part == u"..") {
				if (!parts.empty())
					parts.pop_back();
			} else if (!part.empty() && part != u".") {
				parts.emplace_back(part);
			}
			if (end == value.npos)
				break;
			start = end + 1;
		}
	};
	split(base.substr(base.size() > 2 ? 3 : base.size()));
	split(rest);
	std::u16string result = base.substr(0, 2);
	result[0] = upper_case(result[0]);
	for (const auto &part : parts)
		result += u"\\" + part;
	if (parts.empty() || (!rest.empty() && rest.back() == u'\\' && !parts.empty()))
		result += u"\\";
	return result;
}

std::u16string NativePaths::redirected(std::u16string path) const
{
	if (!wow64 || !redirecting)
		return path;
	const auto lower = [](std::u16string_view text) {
		std::u16string result(text);
		for (auto &character : result)
			if (character >= u'A' && character <= u'Z')
				character = char16_t(character + (u'a' - u'A'));
		return result;
	};
	const auto under = [&](std::u16string_view folder) {
		return path.size() >= folder.size() && lower(std::u16string_view(path).substr(0, folder.size())) == folder &&
			(path.size() == folder.size() || path[folder.size()] == u'\\');
	};
	if (under(u"c:\\windows\\sysnative"))
		return u"C:\\Windows\\System32" + path.substr(20);
	if (!under(u"c:\\windows\\system32"))
		return path;
	// The folders Windows leaves shared between the two widths.
	for (const auto folder : {u"c:\\windows\\system32\\catroot", u"c:\\windows\\system32\\catroot2", u"c:\\windows\\system32\\driverstore",
			u"c:\\windows\\system32\\drivers\\etc", u"c:\\windows\\system32\\logfiles", u"c:\\windows\\system32\\spool"})
		if (under(folder))
			return path;
	return u"C:\\Windows\\SysWOW64" + path.substr(19);
}

std::optional<NativePaths::Resolved> NativePaths::resolve(std::u16string_view input) const
{
	const auto path = redirected(full(input));
	auto result = resolve_direct(path);
	if (result && !result->exists && package_roots)
		if (auto virtual_file = resolve_virtual(path))
			return virtual_file;
	return result;
}

// The folders a package's vfs folder stands for, by the tokens App-V names them with; the longer folder comes first so that Common
// Files is not taken for a program folder.
std::optional<NativePaths::Resolved> NativePaths::resolve_virtual(const std::u16string &path) const
{
	static constexpr std::pair<std::u16string_view, std::u16string_view> tokens[] = {
		{u"C:\\Program Files (x86)\\Common Files", u"ProgramFilesCommonX86"}, {u"C:\\Program Files\\Common Files", u"ProgramFilesCommonX64"},
		{u"C:\\Program Files (x86)", u"ProgramFilesX86"}, {u"C:\\Program Files", u"ProgramFilesX64"}, {u"C:\\Windows\\SysWOW64", u"SystemX86"},
		{u"C:\\Windows\\System32", u"SystemX64"}, {u"C:\\Windows\\Fonts", u"Fonts"}, {u"C:\\Windows", u"Windows"},
		{u"C:\\ProgramData", u"Common AppData"}};
	for (const auto &[folder, token] : tokens) {
		if (path.size() <= folder.size() || path[folder.size()] != u'\\' || !equal_fold(std::u16string_view(path).substr(0, folder.size()), folder))
			continue;
		for (const auto &root : package_roots()) {
			const auto candidate = full(root + u"\\Root\\VFS\\" + std::u16string(token) + path.substr(folder.size()));
			// Looked up directly: a package's files never stand for files of another package.
			if (auto found = resolve_direct(candidate); found && found->exists)
				return found;
		}
		return std::nullopt;
	}
	return std::nullopt;
}

std::optional<NativePaths::Resolved> NativePaths::resolve_direct(const std::u16string &path) const
{
	if (path.size() < 3)
		return std::nullopt;
	auto host = drive_root(path[0]);
	if (!host)
		return std::nullopt;
	Resolved result{*host, true, true};
	std::u16string_view rest = std::u16string_view(path).substr(3);
	while (!rest.empty()) {
		const auto end = rest.find(u'\\');
		const auto part = rest.substr(0, end);
		if (!part.empty()) {
			const auto name = to_utf8(part);
			const bool last = end == rest.npos || end + 1 == rest.size();
			if (const auto match = result.exists ? case_match(result.host, name) : std::nullopt)
				result.host /= *match;
			else {
				result.host /= name;
				if (result.exists)
					result.exists = false;
				else
					result.parent_exists = false;
				if (!last)
					result.parent_exists = false;
			}
		}
		if (end == rest.npos)
			break;
		rest = rest.substr(end + 1);
	}
	return result;
}

bool NativePaths::set_dll_directory(std::optional<std::u16string> path)
{
	if (path && !path->empty()) {
		*path = full(*path);
		if (path->empty()) return false;
	}
	dll_folder = std::move(path);
	return true;
}

std::optional<NativePaths::Resolved> NativePaths::find_library(std::u16string_view image_path, std::u16string_view name,
	bool wide, uint32_t flags) const
{
	if (name.find_first_of(u"/\\:") != name.npos)
		return resolve(name);
	const auto program = full(image_path);
	const auto application = program.substr(0, program.find_last_of(u'\\'));
	uint32_t search = flags & 0x1f00;
	if (search & 0x1000) search |= 0xe00;
	std::vector<std::u16string> folders;
	if (!search || (search & 0x200)) folders.push_back(application);
	if ((!search || (search & 0x400)) && dll_folder && !dll_folder->empty()) folders.push_back(*dll_folder);
	if (!search || (search & 0x800)) folders.push_back(system_directory(wide));
	if (!search) {
		folders.push_back(u"C:\\Windows\\System");
		folders.push_back(u"C:\\Windows");
		if (!dll_folder) folders.push_back(directory);
		// The standard search order ends with the folders of the PATH variable.
		if (search_path)
			for (auto &folder : path_folders(search_path()))
				folders.push_back(std::move(folder));
	}
	for (const auto &folder : folders) {
		const auto found = resolve(folder + u"\\" + std::u16string(name));
		std::error_code error;
		if (found && found->exists && std::filesystem::is_regular_file(found->host, error)) return found;
	}
	return std::nullopt;
}

std::u16string NativePaths::library_location(std::u16string_view image_path, std::u16string_view name, bool wide) const
{
	// The runtime's components stand where Windows keeps them, in the system directory, whatever folder serves them here.
	std::string lower;
	for (const auto character : name)
		lower.push_back(character >= u'A' && character <= u'Z' ? char(character + 32) : char(character));
	if (std::find(std::begin(runtime_components), std::end(runtime_components), lower) != std::end(runtime_components))
		return system_directory(wide) + u"\\" + std::u16string(name);
	// A library a side-by-side assembly of the program's context declares comes from the assembly's folder.
	if (const auto folder = activation_contexts().library_folder(lower))
		return dos(*folder) + u"\\" + std::u16string(name);
	if (const auto found = find_library(image_path, name, wide))
		return dos(found->host);
	const auto program = full(image_path);
	auto beside = program.substr(0, program.find_last_of(u'\\') + 1);
	beside += name;
	if (const auto found = resolve(beside); found && found->exists)
		return beside;
	auto system = system_directory(wide) + u"\\";
	system += name;
	if (const auto found = resolve(system); found && found->exists)
		return system;
	return beside;
}

std::u16string NativePaths::user_name() const
{
	const char *login = std::getenv("USER");
	return from_utf8(login && *login ? login : "user");
}

std::u16string NativePaths::user_profile() const
{
	const auto profile = u"C:\\Users\\" + user_name();
	for (const auto *part : {u"", u"\\AppData\\Roaming", u"\\AppData\\Local\\Temp", u"\\Documents", u"\\Desktop"})
		if (const auto resolved = resolve(profile + part))
			std::filesystem::create_directories(resolved->host);
	return profile;
}

std::u16string NativePaths::temporary() const
{
	return user_profile() + u"\\AppData\\Local\\Temp\\";
}

bool NativePaths::set_current(std::u16string_view path)
{
	const auto resolved = resolve(path);
	if (!resolved || !resolved->exists || !std::filesystem::is_directory(resolved->host))
		return false;
	directory = full(path);
	return true;
}

std::u16string NativePaths::dos(const std::filesystem::path &host) const
{
	const auto relative = [&](const std::filesystem::path &root) -> std::optional<std::u16string> {
		const auto text = host.lexically_normal().string();
		const auto base = root.lexically_normal().string();
		const auto stripped = base.size() > 1 && base.back() == '/' ? base.substr(0, base.size() - 1) : base;
		if (stripped == "/" ? text.starts_with("/") : text == stripped || text.starts_with(stripped + "/")) {
			auto tail = from_utf8(text.substr(stripped == "/" ? 0 : stripped.size()));
			for (auto &character : tail)
				if (character == u'/')
					character = u'\\';
			return tail.empty() ? u"\\" : tail;
		}
		return std::nullopt;
	};
	for (char16_t letter = u'a'; letter < u'z'; ++letter)
		if (const auto root = drive_root(letter))
			if (const auto tail = relative(*root))
				return std::u16string(1, char16_t(letter - 32)) + u":" + *tail;
	const auto tail = relative("/");
	return u"Z:" + tail.value_or(u"\\");
}

}
