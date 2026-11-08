#include "elevation.h"
#include "child_process.h"
#include "kernel_context.h"
#include "text.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

namespace raven {

namespace {

// Folder locations by CSIDL, relative to the user profile unless the path is absolute.
std::optional<std::u16string> folder_for(int csidl, const std::u16string &profile, bool wow64 = false)
{
	switch (csidl & 0xff) {
	case 0x00: case 0x10: return profile + u"\\Desktop";
	case 0x02: return profile + u"\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs";
	case 0x05: return profile + u"\\Documents";
	case 0x06: return profile + u"\\Favorites";
	case 0x07: return profile + u"\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
	case 0x08: return profile + u"\\AppData\\Roaming\\Microsoft\\Windows\\Recent";
	case 0x09: return profile + u"\\AppData\\Roaming\\Microsoft\\Windows\\SendTo";
	case 0x0b: return profile + u"\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu";
	case 0x0d: return profile + u"\\Music";
	case 0x0e: return profile + u"\\Videos";
	case 0x14: return std::u16string(u"C:\\Windows\\Fonts");
	case 0x15: return profile + u"\\AppData\\Roaming\\Microsoft\\Windows\\Templates";
	case 0x16: return std::u16string(u"C:\\ProgramData\\Microsoft\\Windows\\Start Menu");
	case 0x17: return std::u16string(u"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs");
	case 0x19: return std::u16string(u"C:\\Users\\Public\\Desktop");
	case 0x1a: return profile + u"\\AppData\\Roaming";
	case 0x1c: return profile + u"\\AppData\\Local";
	case 0x20: return profile + u"\\AppData\\Local\\Microsoft\\Windows\\INetCache";
	case 0x21: return profile + u"\\AppData\\Local\\Microsoft\\Windows\\INetCookies";
	case 0x22: return profile + u"\\AppData\\Local\\Microsoft\\Windows\\History";
	case 0x23: return std::u16string(u"C:\\ProgramData");
	case 0x24: return std::u16string(u"C:\\Windows");
	case 0x25: case 0x29: return std::u16string(u"C:\\Windows\\System32");
	// A 32-bit program sees the 32-bit program directories, as on a 64-bit Windows.
	case 0x26: case 0x2b:
		if (wow64)
			return csidl == 0x2b ? std::u16string(u"C:\\Program Files (x86)\\Common Files") : std::u16string(u"C:\\Program Files (x86)");
		return csidl == 0x2b ? std::u16string(u"C:\\Program Files\\Common Files") : std::u16string(u"C:\\Program Files");
	case 0x27: return profile + u"\\Pictures";
	case 0x28: return profile;
	case 0x2a: return std::u16string(u"C:\\Program Files (x86)");
	case 0x2c: return std::u16string(u"C:\\Program Files (x86)\\Common Files");
	default: return std::nullopt;
	}
}

// The folders a known folder id names, by the first word of the id; those that no CSIDL covers carry their own path.
struct KnownFolder {
	uint32_t first;
	int csidl;
	const char16_t *path;
};

constexpr KnownFolder known_folders[] = {
	{0xb4bfcc3a, 0x10, nullptr}, {0xfdd39ad0, 0x05, nullptr}, {0x374de290, -1, u"%PROFILE%\\Downloads"}, {0x4bd8d571, 0x0d, nullptr},
	{0x33e28130, 0x27, nullptr}, {0x18989b1d, 0x0e, nullptr}, {0xf1b32785, 0x1c, nullptr}, {0xa520a1a4, -1, u"%PROFILE%\\AppData\\LocalLow"},
	{0x3eb685db, 0x1a, nullptr}, {0x62ab5d82, 0x23, nullptr}, {0x905e63b6, 0x26, nullptr}, {0x7c5a40ef, 0x2a, nullptr},
	{0x6d809377, -1, u"C:\\Program Files"}, {0xf7f1ed05, 0x2b, nullptr}, {0xde974d24, 0x2c, nullptr}, {0xf38bf404, 0x24, nullptr},
	{0x1ac14e77, 0x25, nullptr}, {0xd65231b0, -1, u"C:\\Windows\\SysWOW64"}, {0x5e6c858f, 0x28, nullptr}, {0xfd228cb7, 0x14, nullptr},
	{0xdfdf76a2, -1, u"C:\\Users\\Public"}, {0x625b53c3, 0x0b, nullptr}, {0xa77f5d77, 0x02, nullptr}, {0xb97d20bb, 0x07, nullptr},
	{0x352481e8, 0x20, nullptr}, {0x2b0f765d, 0x21, nullptr}, {0xd9dc8a3b, 0x22, nullptr}, {0x4c5c32ff, -1, u"%PROFILE%\\Saved Games"},
	{0x1777f761, 0x06, nullptr}, {0xa4115719, 0x16, nullptr}, {0x0139d44e, 0x17, nullptr}, {0xae50c081, 0x27, nullptr},
	{0x6365d5a7, -1, u"C:\\Program Files\\Common Files"}, {0x0762d272, -1, u"C:\\Users"},
	{0x5cd7aee2, -1, u"%PROFILE%\\AppData\\Local\\Programs"}, {0xbcbd3057, -1, u"%PROFILE%\\AppData\\Local\\Programs\\Common"}};

// The double-NUL terminated file lists of SHFileOperation.
// A list of names each ended by a zero and the list by one more, wide or in the ANSI code page.
std::vector<std::u16string> file_list(ApiCall &call, uint64_t address, bool ansi = false)
{
	std::vector<std::u16string> result;
	if (!address)
		return result;
	for (;;) {
		if (ansi) {
			const auto item = call.narrow(address, 32768);
			if (item.empty())
				return result;
			address += item.size() + 1;
			result.push_back(from_ansi(item, 0));
			continue;
		}
		auto item = call.wide(address, 32768);
		if (item.empty())
			return result;
		address += (item.size() + 1) * 2;
		result.push_back(std::move(item));
	}
}

}

namespace {

// The icons the shell gives folders and files: a folder and a sheet of paper, drawn on a grid of 16 units at the size asked for, as
// 0xAARRGGBB pixels from the top row down. Clear pixels are transparent.
std::vector<uint32_t> shell_icon(bool folder, int32_t size)
{
	std::vector<uint32_t> pixels(size_t(size) * size, 0);
	for (int32_t y = 0; y < size; ++y)
		for (int32_t x = 0; x < size; ++x) {
			const int32_t u = x * 16 / size, v = y * 16 / size;
			uint32_t color = 0;
			if (folder) {
				if (v >= 2 && v <= 3 && u >= 1 && u <= 6)
					color = 0xffd9a53aU;
				if (v >= 4 && v <= 13 && u >= 1 && u <= 14)
					color = v == 4 || v == 13 || u == 1 || u == 14 ? 0xffc28a1eU : 0xfff6d26bU;
			} else if (u >= 3 && u <= 12 && v >= 1 && v <= 14) {
				// The top right corner is folded over along its diagonal.
				if (u >= 10 && v <= 3 && u - 10 > v - 1)
					color = 0;
				else if (u >= 10 && v <= 3)
					color = u - 10 == v - 1 ? 0xff8c8c8cU : 0xffd6d6d6U;
				else if (u == 3 || u == 12 || v == 1 || v == 14)
					color = 0xff8c8c8cU;
				else if ((v == 6 || v == 8 || v == 10 || v == 12) && u >= 5 && u <= 10)
					color = 0xffb4b4b4U;
				else
					color = 0xffffffffU;
			}
			pixels[size_t(y) * size + x] = color;
		}
	return pixels;
}

}

void register_shell_api(ApiRegistry &registry, KernelContext &context)
{
	auto &paths = context.paths;
	registry.add("SHGetFolderPathW", 5, [&paths](ApiCall &call) {
		const int csidl = call.i32(1);
		const auto location = folder_for(csidl, paths.user_profile(), call.process.mode() == NativeMode::x86);
		if (!location) {
			call.ret(0x80070057U);
			return;
		}
		if (const auto resolved = paths.resolve(*location); resolved && csidl & 0x8000)
			std::filesystem::create_directories(resolved->host);
		call.put_wide(call[4], *location, 260);
		call.ret(0);
	});
	registry.add("SHGetFolderPathAndSubDirW", 6, [&paths](ApiCall &call) {
		const int csidl = call.i32(1);
		auto location = folder_for(csidl, paths.user_profile(), call.process.mode() == NativeMode::x86);
		if (!location) {
			call.ret(0x80070057U);
			return;
		}
		if (call[4])
			*location += u"\\" + call.wide(call[4], 260);
		if (const auto resolved = paths.resolve(*location); resolved && csidl & 0x8000)
			std::filesystem::create_directories(resolved->host);
		call.put_wide(call[5], *location, 260);
		call.ret(0);
	});
	registry.add("SHGetFolderPathA", 5, [&paths](ApiCall &call) {
		const int csidl = call.i32(1);
		const auto location = folder_for(csidl, paths.user_profile(), call.process.mode() == NativeMode::x86);
		if (!location) {
			call.ret(0x80070057U);
			return;
		}
		if (const auto resolved = paths.resolve(*location); resolved && csidl & 0x8000)
			std::filesystem::create_directories(resolved->host);
		call.put_narrow(call[4], to_ansi(*location, 1252), 260);
		call.ret(0);
	});
	// A known folder id names the folder; the path comes in memory the program frees with CoTaskMemFree.
	registry.add("SHGetKnownFolderPath", 4, [&paths, &context](ApiCall &call) {
		if (!call[0] || !call[3]) {
			call.ret(0x80070057U);
			return;
		}
		const uint32_t first = call.load<uint32_t>(call[0]);
		const bool wow64 = call.process.mode() == NativeMode::x86;
		std::optional<std::u16string> location;
		for (const auto &folder : known_folders) {
			if (folder.first != first)
				continue;
			if (folder.csidl >= 0)
				location = folder_for(folder.csidl, paths.user_profile(), wow64);
			else {
				location = std::u16string(folder.path);
				if (const auto at = location->find(u"%PROFILE%"); at != location->npos)
					location->replace(at, 9, paths.user_profile());
			}
			break;
		}
		if (!location) {
			call.ret(0x80070002U);
			return;
		}
		// KF_FLAG_CREATE makes the directory.
		if (const auto resolved = paths.resolve(*location); resolved && (call.u32(1) & 0x8000))
			std::filesystem::create_directories(resolved->host);
		const auto memory = context.allocator.allocate((location->size() + 1) * 2, true);
		if (!memory) {
			call.ret(0x8007000EU);
			return;
		}
		call.put_wide(memory, *location, location->size() + 1);
		call.store_pointer(call[3], memory);
		call.ret(0);
	});
	// An AppContainer's own folder, named by its SID in text. It exists from the moment a program asks.
	registry.add("GetAppContainerFolderPath", 2, [&paths, &context](ApiCall &call) {
		if (!call[0] || !call[1]) {
			call.ret(0x80070057U);
			return;
		}
		const auto location = paths.user_profile() + u"\\AppData\\Local\\Packages\\" + call.wide(call[0], 256) + u"\\AC";
		// The folders a profile comes with: programs write straight into Temp and the caches.
		if (const auto resolved = paths.resolve(location))
			for (const char *folder : {"Temp", "INetCache", "INetCookies", "INetHistory", "Microsoft"})
				std::filesystem::create_directories(resolved->host / folder);
		const auto memory = context.allocator.allocate((location.size() + 1) * 2, true);
		if (!memory) {
			call.ret(0x8007000EU);
			return;
		}
		call.put_wide(memory, location, location.size() + 1);
		call.store_pointer(call[1], memory);
		call.ret(0);
	});
	// A folder location as an item id list. Programs treat it as opaque: one item that carries the path, then the terminating empty item.
	// The list address, zero when the folder is unknown, or the error the call returns.
	const auto folder_list = [&paths, &context](ApiCall &call, int csidl, bool create) -> std::pair<uint64_t, uint32_t> {
		const auto location = folder_for(csidl, paths.user_profile(), call.process.mode() == NativeMode::x86);
		if (!location)
			return {0, 0x80070002U};
		if (const auto resolved = paths.resolve(*location); resolved && create)
			std::filesystem::create_directories(resolved->host);
		const size_t item = 4 + location->size() * 2;
		const auto address = context.allocator.allocate(item + 2, true);
		if (!address)
			return {0, 0x8007000EU};
		std::vector<uint8_t> bytes(item + 2, 0);
		bytes[0] = uint8_t(item);
		bytes[1] = uint8_t(item >> 8);
		bytes[2] = 'R';
		bytes[3] = 'V';
		std::memcpy(bytes.data() + 4, location->data(), location->size() * 2);
		call.write(address, bytes);
		return {address, 0};
	};
	registry.add("SHGetKnownFolderIDList", 4, [folder_list](ApiCall &call) {
		if (!call[0] || !call[3]) {
			call.ret(0x80070057U);
			return;
		}
		const uint32_t first = call.load<uint32_t>(call[0]);
		for (const auto &folder : known_folders) {
			if (folder.first != first || folder.csidl < 0)
				continue;
			const auto [address, status] = folder_list(call, folder.csidl, call.u32(1) & 0x8000);
			if (address)
				call.store_pointer(call[3], address);
			call.ret(status);
			return;
		}
		call.ret(0x80070002U);
	});
	registry.add("SHGetSpecialFolderLocation", 3, [folder_list](ApiCall &call) {
		if (!call[2]) {
			call.ret(0x80070057U);
			return;
		}
		const auto [address, status] = folder_list(call, call.i32(1), call.i32(1) & 0x8000);
		if (address)
			call.store_pointer(call[2], address);
		call.ret(status);
	});
	// The older form of the same call, which shell32 publishes by number: it answers the list itself, or no list when the folder is unknown.
	registry.add("SHCloneSpecialIDList", 3, [folder_list](ApiCall &call) {
		call.ret(folder_list(call, call.i32(1) & 0xff, call.i32(2) || (call.i32(1) & 0x8000)).first);
	});
	const auto path_from_list = [](ApiCall &call, size_t capacity) {
		if (!call[0] || !call[1]) {
			call.ret(0);
			return;
		}
		// A list made at a folder has one item per level: the path of the first, then a name for each below it.
		std::u16string text;
		uint64_t at = call[0];
		for (;;) {
			const auto header = call.read(at, 2);
			const size_t size = header[0] | header[1] << 8;
			if (!size)
				break;
			const auto item = call.read(at, size < 4 ? 4 : size);
			if (size < 4 || size > 4 + 520 || item[2] != 'R' || item[3] != 'V') {
				call.ret(0);
				return;
			}
			if (!text.empty() && text.back() != u'\\')
				text.push_back(u'\\');
			for (size_t index = 4; index + 1 < size; index += 2)
				text.push_back(char16_t(item[index] | item[index + 1] << 8));
			at += size;
		}
		if (text.empty() || text.size() >= capacity) {
			call.ret(0);
			return;
		}
		call.put_wide(call[1], text, capacity);
		call.ret(1);
	};
	// The recent list only feeds the shell's own menus, which nothing here shows.
	registry.add("SHAddToRecentDocs", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("SHGetPathFromIDListW", 2, [path_from_list](ApiCall &call) { path_from_list(call, 260); });
	registry.add("SHGetPathFromIDListEx", 4, [path_from_list](ApiCall &call) { path_from_list(call, call.u32(2)); });
	for (const bool ansi : {false, true})
		registry.add(ansi ? "SHGetSpecialFolderPathA" : "SHGetSpecialFolderPathW", 4, [&paths, ansi](ApiCall &call) {
			const int csidl = call.i32(2);
			const auto location = folder_for(csidl, paths.user_profile(), call.process.mode() == NativeMode::x86);
			if (!location) {
				call.ret(0);
				return;
			}
			if (call.u32(3)) {
				const auto resolved = paths.resolve(*location);
				std::error_code error;
				if (resolved)
					std::filesystem::create_directories(resolved->host, error);
				if (!resolved || error) {
					call.ret(0);
					return;
				}
			}
			if (ansi)
				call.put_narrow(call[1], to_ansi(*location, 1252), 260);
			else
				call.put_wide(call[1], *location, 260);
			call.ret(1);
		});
	registry.add("SHCreateDirectory", 2, [&paths](ApiCall &call) {
		const auto resolved = paths.resolve(call.wide(call[1]));
		if (!resolved) {
			call.ret(123);
			return;
		}
		std::error_code error;
		std::filesystem::create_directories(resolved->host, error);
		call.ret(error ? 5 : 0);
	});
	registry.add("SHCreateDirectoryExW", 3, [&paths](ApiCall &call) {
		const auto resolved = paths.resolve(call.wide(call[1]));
		if (!resolved) {
			call.ret(123);
			return;
		}
		std::error_code error;
		std::filesystem::create_directories(resolved->host, error);
		call.ret(error ? 5 : 0);
	});
	// The shell's change notifications refresh Explorer windows, which this desktop does not have; announcing a change does nothing.
	registry.add("SHChangeNotify", 4, [](ApiCall &call) { call.ret(0); });
	// A folder on a local disk is ready for writing once it exists; SHPPFW_DIRCREATE makes it first.
	registry.add("SHPathPrepareForWriteW", 4, [&paths](ApiCall &call) {
		const auto resolved = call[2] ? paths.resolve(call.wide(call[2])) : std::nullopt;
		if (!resolved) {
			call.ret(0x80070003U);
			return;
		}
		std::error_code error;
		if (call.u32(3) & 1)
			std::filesystem::create_directories(resolved->host, error);
		call.ret(std::filesystem::is_directory(resolved->host, error) ? 0 : 0x80070003U);
	});
	registry.add("CommandLineToArgvW", 2, [&context](ApiCall &call) {
		const auto line = call.wide(call[0]);
		std::vector<std::u16string> arguments;
		size_t at = 0;
		// Program name: quoted up to the next quote, otherwise up to the first blank.
		if (line.empty()) {
			arguments.push_back(context.launch.image_path);
		} else {
			std::u16string name;
			if (line[0] == u'"') {
				const auto end = line.find(u'"', 1);
				name = line.substr(1, end == line.npos ? line.npos : end - 1);
				at = end == line.npos ? line.size() : end + 1;
			} else {
				const auto end = line.find_first_of(u" \t");
				name = line.substr(0, end);
				at = end == line.npos ? line.size() : end;
			}
			arguments.push_back(name);
			bool quoted = false;
			std::u16string current;
			bool started = false;
			for (; at <= line.size(); ++at) {
				const char16_t character = at < line.size() ? line[at] : u'\0';
				if (!character || (!quoted && (character == u' ' || character == u'\t'))) {
					if (started)
						arguments.push_back(current);
					current.clear();
					started = false;
					continue;
				}
				started = true;
				if (character == u'\\') {
					size_t slashes = 0;
					while (at < line.size() && line[at] == u'\\') {
						++slashes;
						++at;
					}
					if (at < line.size() && line[at] == u'"') {
						current.append(slashes / 2, u'\\');
						if (slashes % 2)
							current.push_back(u'"');
						else
							quoted = !quoted;
					} else {
						current.append(slashes, u'\\');
						--at;
					}
				} else if (character == u'"') {
					quoted = !quoted;
				} else {
					current.push_back(character);
				}
			}
		}
		// The pointer array has the width of the caller's pointers, then the strings follow it in the same block.
		const size_t width = call.pointer_width();
		size_t bytes = arguments.size() * width + width;
		for (const auto &argument : arguments)
			bytes += (argument.size() + 1) * 2;
		const auto block = context.allocator.allocate(bytes);
		if (!block) {
			call.fail(8);
			return;
		}
		uint64_t cursor = block + arguments.size() * width + width;
		for (size_t index = 0; index < arguments.size(); ++index) {
			call.store_pointer(block + index * width, cursor);
			call.put_wide(cursor, arguments[index], arguments[index].size() + 1);
			cursor += (arguments[index].size() + 1) * 2;
		}
		call.store_pointer(block + arguments.size() * width, 0);
		if (call[1]) call.store<int32_t>(call[1], int32_t(arguments.size()));
		call.ret(block);
	});
	// The shell cannot show a tray icon yet; callers see an honest failure.
	registry.add("Shell_NotifyIconW", 2, [](ApiCall &call) { call.fail(5); });
	// One of the shell icons as an icon handle the caller owns, made by the registry's CreateIcon; zero when it is not there.
	const auto make_icon = [&registry, &context](ApiCall &call, bool folder, int32_t size) -> uint64_t {
		const auto create_icon = registry.find("CreateIcon");
		if (!create_icon)
			return 0;
		const auto pixels = shell_icon(folder, size);
		// The color bits, then an AND mask of zeros: the alpha of the colors decides what shows.
		const uint64_t mask_bytes = uint64_t(((size + 15) / 16) * 2) * size;
		const uint64_t block = context.allocator.allocate(pixels.size() * 4 + mask_bytes, true);
		if (!block)
			return 0;
		call.write(block, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(pixels.data()), pixels.size() * 4));
		ApiCall inner(call.process, call.environment, {0, uint64_t(size), uint64_t(size), 1, 32, block + pixels.size() * 4, block});
		create_icon->second(inner);
		context.allocator.release(block);
		return inner.result;
	};
	// SHGetFileInfo answers from the path and its attributes. No file associations are registered, so a file's type is named after its
	// extension as Windows names one that nothing claims ("TXT File"), and every file has the document icon and every folder the folder.
	// Item lists (SHGFI_PIDL) and the system image list (SHGFI_SYSICONINDEX) are not provided.
	const auto file_info = [&paths, make_icon](ApiCall &call, bool wide) {
		const uint32_t flags = call.u32(4);
		if (!call[0] || (flags & (0x8 | 0x4000))) {
			call.ret(0);
			return;
		}
		const auto path = wide ? call.wide(call[0]) : from_utf8(call.narrow(call[0]));
		bool directory = false;
		if (flags & 0x10) {
			directory = call.u32(1) & 0x10;
		} else {
			const auto resolved = paths.resolve(path);
			if (!resolved || !resolved->exists) {
				call.ret(0);
				return;
			}
			std::error_code error;
			directory = std::filesystem::is_directory(resolved->host, error);
		}
		if (!call[2]) {
			call.ret(0);
			return;
		}
		std::u16string name = path;
		while (name.size() > 3 && (name.back() == u'\\' || name.back() == u'/'))
			name.pop_back();
		if (const auto slash = name.find_last_of(u"\\/"); slash != std::u16string::npos && slash + 1 < name.size())
			name.erase(0, slash + 1);
		std::u16string type = u"File folder";
		if (!directory) {
			const auto dot = name.find_last_of(u'.');
			if (dot == std::u16string::npos || dot + 1 == name.size()) {
				type = u"File";
			} else {
				type.clear();
				for (size_t index = dot + 1; index < name.size(); ++index)
					type.push_back(upper_case(name[index]));
				type += u" File";
			}
		}
		const size_t width = call.pointer_width(), unit = wide ? 2 : 1;
		const size_t size = (width + 8 + (260 + 80) * unit + width - 1) / width * width;
		std::vector<uint8_t> bytes(size, 0);
		uint64_t icon = 0;
		if (flags & 0x100)
			icon = make_icon(call, directory, (flags & 1) ? 16 : 32);
		std::memcpy(bytes.data(), &icon, width);
		// SFGAO: part of the file system, can be copied, moved, linked, renamed and deleted; a folder also holds other items.
		const uint32_t attributes = 0x40000000U | 0x37U | (directory ? 0x20000000U : 0U);
		if (flags & 0x800)
			std::memcpy(bytes.data() + width + 4, &attributes, 4);
		const auto put_text = [&](size_t offset, const std::u16string &text, size_t capacity) {
			if (wide) {
				const auto count = std::min(text.size(), capacity - 1);
				std::memcpy(bytes.data() + offset, text.data(), count * 2);
			} else {
				const auto narrow = to_utf8(text);
				std::memcpy(bytes.data() + offset, narrow.data(), std::min(narrow.size(), capacity - 1));
			}
		};
		if (flags & 0x200)
			put_text(width + 8, name, 260);
		if (flags & 0x400)
			put_text(width + 8 + 260 * unit, type, 80);
		call.write(call[2], std::span<const uint8_t>(bytes.data(), std::min<size_t>(bytes.size(), call.u32(3) ? call.u32(3) : bytes.size())));
		call.ret(1);
	};
	registry.add("SHGetFileInfoW", 5, [file_info](ApiCall &call) { file_info(call, true); });
	registry.add("SHGetFileInfoA", 5, [file_info](ApiCall &call) { file_info(call, false); });
	registry.add("SHCreateItemFromParsingName", 4, [](ApiCall &call) {
		if (call[3]) call.store_pointer(call[3], 0);
		call.ret(0x80070002U);
	});
	// The program runs as the host account, which holds administrator rights only when it is root.
	// The application identity only groups taskbar buttons; there is no taskbar to group them on.
	registry.add("SetCurrentProcessExplicitAppUserModelID", 1, [](ApiCall &call) { call.ret(0); });
	// The account of a prefix is an administrator under User Account Control: the check answers yes only in an elevated process.
	// Raven does not stand for the privileges of the host account.
	// There are no default-program associations to ask about: the registration object is not available.
	registry.add("SHCreateAssociationRegistration", 2, [](ApiCall &call) {
		if (call[1])
			call.store_pointer(call[1], 0);
		call.ret(0x80040154U);
	});
	registry.add("IsUserAnAdmin", 0, [](ApiCall &call) { call.ret(process_elevated() ? 1 : 0); });
	registry.add("DragQueryFileW", 4, [](ApiCall &call) { call.ret(0); });
	registry.add("DragQueryPoint", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("DragFinish", 1, [](ApiCall &) {});
	registry.add("DragAcceptFiles", 2, [](ApiCall &) {});
	// Opens a program, a folder, a document or a URL the way the shell does. The result is the value ShellExecute reports: above 32 on
	// success, otherwise the error. A program that is started is handed back through `started`.
	const auto open_target = [&context, &paths](const std::u16string &operation, std::u16string target, const std::u16string &parameters,
		const std::u16string &directory, std::shared_ptr<ProcessObject> &started) -> uint64_t {
		if (!equal_fold(operation, u"open") && !equal_fold(operation, u"edit") && !equal_fold(operation, u"explore") && !equal_fold(operation, u"runas"))
			return 31;
		std::string argument;
		const bool url = target.find(u"://") != target.npos || target.starts_with(u"mailto:");
		if (!url) {
			const auto candidates = target.find_first_of(u"\\/:") != target.npos ? std::vector<std::u16string>{target} :
				std::vector<std::u16string>{directory.empty() ? paths.current() + u"\\" + target : directory + u"\\" + target, target};
			const auto resolved = [&]() -> std::optional<NativePaths::Resolved> {
				for (const auto &candidate : candidates)
					if (const auto found = paths.resolve(candidate); found && found->exists)
						return found;
				return std::nullopt;
			}();
			if (!resolved) {
				// A bare program name is found along the usual program directories, as Windows does for the open verb.
				uint32_t error = 0;
				const int standard[3] = {0, 1, 2};
				started = start_program(context, u"", u"\"" + target + u"\"" + (parameters.empty() ? u"" : u" " + parameters), directory, standard, context.environment_block ? context.environment_block() : std::u16string(), nullptr, error);
				return started ? 42 : error == 2 ? 2 : error == 193 ? 11 : 31;
			}
			if (equal_fold(from_utf8(std::filesystem::path(resolved->host).extension().string()), u".exe") && !std::filesystem::is_directory(resolved->host)) {
				uint32_t error = 0;
				const int standard[3] = {0, 1, 2};
				const auto full = paths.dos(resolved->host);
				request_child_elevation(equal_fold(operation, u"runas"));
				started = start_program(context, full, u"\"" + full + u"\"" + (parameters.empty() ? u"" : u" " + parameters),
					directory.empty() ? full.substr(0, full.find_last_of(u'\\')) : directory, standard, context.environment_block ? context.environment_block() : std::u16string(), nullptr, error);
				request_child_elevation(false);
				return started ? 42 : error == 193 ? 11 : error;
			}
			argument = resolved->host.string();
		} else {
			argument = to_utf8(target);
		}
		const pid_t child = ::fork();
		if (child == 0) {
			::setsid();
			::execlp("xdg-open", "xdg-open", argument.c_str(), static_cast<char *>(nullptr));
			::_exit(127);
		}
		return child < 0 ? 0 : 42;
	};
	registry.add("ShellExecuteW", 6, [open_target](ApiCall &call) {
		std::shared_ptr<ProcessObject> started;
		call.ret(open_target(call[1] ? call.wide(call[1]) : std::u16string(u"open"), call[2] ? call.wide(call[2]) : std::u16string(),
			call[3] ? call.wide(call[3]) : std::u16string(), call[4] ? call.wide(call[4]) : std::u16string(), started));
	});
	// SHELLEXECUTEINFOW: the pointers sit after the size, mask and window, each as wide as the program's.
	registry.add("ShellExecuteExW", 1, [open_target, &context](ApiCall &call) {
		const size_t width = call.pointer_width();
		const auto at = [&](size_t index) { return call[0] + 8 + index * width; };
		if (!call[0] || call.load<uint32_t>(call[0]) < 8 + 13 * width) {
			call.fail(87);
			return;
		}
		const auto text = [&](size_t index) {
			const auto pointer = call.load_pointer(at(index));
			return pointer ? call.wide(pointer) : std::u16string();
		};
		const uint32_t mask = call.load<uint32_t>(call[0] + 4);
		const auto verb = text(1);
		std::shared_ptr<ProcessObject> started;
		const auto result = open_target(verb.empty() ? std::u16string(u"open") : verb, text(2), text(3), text(4), started);
		call.store_pointer(at(6), result);
		if (result <= 32) {
			if (std::getenv("RAVEN_TRACE_FAIL"))
				std::cerr << "raven: ShellExecuteEx " << to_utf8(text(2)) << ' ' << to_utf8(text(3)) << " fails: " << result << '\n';
			call.fail(uint32_t(result == 31 ? 1155 : result));
			return;
		}
		if (started && (mask & 0x40)) {
			const auto handle = context.objects.add_object(started);
			call.store_pointer(at(12), handle.status ? 0 : handle.value);
		} else if (mask & 0x40) {
			call.store_pointer(at(12), 0);
		}
		call.ret(1);
	});
	const auto file_operation = [&paths](ApiCall &call, bool ansi) {
		// SHFILEOPSTRUCTW: owner window, function, source and destination lists, flags, then the aborted flag; the pointers are as wide as the program's.
		const bool narrow = call.process.mode() == NativeMode::x86;
		const size_t function_at = narrow ? 4 : 8, sources_at = narrow ? 8 : 16, destinations_at = narrow ? 12 : 24, flags_at = narrow ? 16 : 32,
			aborted_at = narrow ? 20 : 36;
		const uint32_t function = call.load<uint32_t>(call[0] + function_at);
		const uint64_t sources = narrow ? call.load<uint32_t>(call[0] + sources_at) : call.load<uint64_t>(call[0] + sources_at);
		const uint64_t destinations = narrow ? call.load<uint32_t>(call[0] + destinations_at) : call.load<uint64_t>(call[0] + destinations_at);
		const uint16_t flags = call.load<uint16_t>(call[0] + flags_at);
		const auto from = file_list(call, sources, ansi), to = file_list(call, destinations, ansi);
		call.store<int32_t>(call[0] + aborted_at, 0);
		std::error_code error;
		for (size_t index = 0; index < from.size(); ++index) {
			const auto source = paths.resolve(from[index]);
			if (!source || !source->exists) {
				call.ret(0x402);
				return;
			}
			const auto destination = function == 3 ? std::nullopt : paths.resolve(to.size() == from.size() ? to[index] : to.empty() ? std::u16string() : to[0]);
			if (function == 3) {
				if (flags & 0x40) {
					// FOF_ALLOWUNDO: move into the prefix recycle bin instead of destroying the item.
					const auto bin = paths.resolve(u"C:\\$Recycle.Bin");
					std::filesystem::create_directories(bin->host, error);
					std::filesystem::rename(source->host, bin->host / source->host.filename(), error);
					if (error) {
						std::filesystem::remove_all(source->host, error);
					}
				} else {
					std::filesystem::remove_all(source->host, error);
				}
			} else if (function == 2 && destination) {
				auto target = destination->host;
				if (std::filesystem::is_directory(target) && !(flags & 0x400))
					target /= source->host.filename();
				std::filesystem::copy(source->host, target, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
			} else if ((function == 1 || function == 4) && destination) {
				auto target = destination->host;
				if (function == 1 && std::filesystem::is_directory(target))
					target /= source->host.filename();
				std::filesystem::rename(source->host, target, error);
			}
			if (error) {
				call.ret(0x7c);
				return;
			}
		}
		call.ret(0);
	};
	// The ANSI form lists its names in the program's code page; the structure is the same.
	registry.add("SHFileOperationW", 1, [file_operation](ApiCall &call) { file_operation(call, false); });
	registry.add("SHFileOperationA", 1, [file_operation](ApiCall &call) { file_operation(call, true); });
}

}
