#include "kernel_context.h"
#include "nls.h"
#include "registry.h"
#include "text.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <tuple>

namespace raven {

namespace {

constexpr uint32_t error_more_data = 234, error_no_more_items = 259, error_invalid_parameter = 87, error_file_not_found = 2;
constexpr uint32_t type_sz = 1, type_expand_sz = 2, type_multi_sz = 7, type_dword = 4, type_qword = 11, type_binary = 3;

bool is_text(uint32_t type)
{
	return type == type_sz || type == type_expand_sz || type == type_multi_sz;
}

// Registry text is stored as UTF-16; ANSI callers receive the process code page, always terminated.
std::vector<uint8_t> to_ansi_bytes(const std::vector<uint8_t> &wide, uint32_t code_page)
{
	std::u16string text;
	for (size_t index = 0; index + 1 < wide.size(); index += 2)
		text.push_back(char16_t(wide[index] | wide[index + 1] << 8));
	auto ansi = encode_ansi(text, code_page == 1252 ? 1252 : 1252);
	if (ansi.empty() || ansi.back() != '\0')
		ansi.push_back('\0');
	return std::vector<uint8_t>(ansi.begin(), ansi.end());
}

std::vector<uint8_t> from_ansi_bytes(std::span<const uint8_t> ansi, uint32_t code_page)
{
	const auto text = from_ansi(std::string_view(reinterpret_cast<const char *>(ansi.data()), ansi.size()), code_page);
	std::vector<uint8_t> bytes(text.size() * 2);
	std::memcpy(bytes.data(), text.data(), bytes.size());
	return bytes;
}

}

// The settings the control panel keeps in the prefix registry take effect as the process starts: the runtime options under
// HKCU\Software\Raven\Environment, and the visual styles switched off as Windows records it (ThemeManager ThemeActive "0"). What the
// environment already sets wins over the registry.
void apply_registry_settings(NativeRegistry &hive)
{
	const auto text = [](const std::vector<uint8_t> &data) {
		std::u16string value(data.size() / 2, u'\0');
		std::memcpy(value.data(), data.data(), value.size() * 2);
		while (!value.empty() && !value.back())
			value.pop_back();
		return to_utf8(value);
	};
	uint64_t key = 0;
	if (!hive.open(0x80000001, u"Software\\Raven\\Environment", key)) {
		std::u16string name;
		uint32_t type = 0;
		std::vector<uint8_t> data;
		for (uint32_t index = 0; !hive.enum_value(key, index, name, type, data); ++index) {
			const auto variable = to_utf8(name);
			if ((type == 1 || type == 2) && variable.starts_with("RAVEN_"))
				::setenv(variable.c_str(), text(data).c_str(), 0);
		}
		hive.close(key);
	}
	if (!hive.open(0x80000001, u"Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager", key)) {
		uint32_t type = 0;
		std::vector<uint8_t> data;
		if (!hive.query(key, u"ThemeActive", type, data) && type == 1 && text(data) == "0")
			::setenv("RAVEN_THEME", "classic", 0);
		hive.close(key);
	}
}

void register_registry_api(ApiRegistry &registry, KernelContext &context, NativeRegistry &hive)
{
	const uint32_t ansi_page = context.launch.ansi_code_page;
	// Key handles come from the same space as every other handle, so CloseHandle and NtClose close them too.
	hive.set_allocator([&objects = context.objects, &hive] { return objects.reserve([&hive](uint64_t key) { hive.close(key); }); },
		[&objects = context.objects](uint64_t key) { objects.release_reserved(key); });
	const size_t handle_width = context.process.mode() == NativeMode::x86 ? 4 : 8;
	const auto text_of = [](ApiCall &call, uint64_t address, bool wide) {
		if (!address)
			return std::u16string();
		return wide ? call.wide(address, 16384) : from_ansi(call.narrow(address, 16384), 1252);
	};
	const auto put_handle = [handle_width](ApiCall &call, uint64_t address, uint64_t value) {
		std::vector<uint8_t> bytes(handle_width);
		std::memcpy(bytes.data(), &value, handle_width);
		call.write(address, bytes);
	};
	// A 32-bit program sees HKEY_LOCAL_MACHINE\SOFTWARE as the Wow6432Node subkey of it, except for the keys both views share; the key
	// SOFTWARE itself opens as that subkey, so that what is opened below it stays in the view. KEY_WOW64_64KEY (0x100) asks for the
	// 64-bit view from either width and KEY_WOW64_32KEY (0x200) for the 32-bit one.
	const bool wow64 = context.process.mode() == NativeMode::x86;
	const auto view = [wow64](uint64_t parent, std::u16string path, uint32_t access = 0) {
		const bool redirect = (access & 0x200) || (wow64 && !(access & 0x100));
		if (!redirect || uint32_t(parent) != 0x80000002U)
			return path;
		const auto fold = [](std::u16string text) {
			for (auto &character : text)
				if (character >= u'a' && character <= u'z')
					character = char16_t(character - (u'a' - u'A'));
			return text;
		};
		while (!path.empty() && path.front() == u'\\')
			path.erase(0, 1);
		const auto first = path.find(u'\\');
		if (fold(path.substr(0, first)) != u"SOFTWARE")
			return path;
		if (first == std::u16string::npos || first + 1 == path.size())
			return path.substr(0, first) + u"\\Wow6432Node";
		const auto second_end = path.find(u'\\', first + 1);
		const auto second = fold(path.substr(first + 1, second_end == std::u16string::npos ? std::u16string::npos : second_end - first - 1));
		if (second == u"CLASSES" || second == u"WOW6432NODE")
			return path;
		return path.substr(0, first + 1) + u"Wow6432Node\\" + path.substr(first + 1);
	};
	for (const bool wide : {true, false}) {
		const std::string suffix = wide ? "W" : "A";
		registry.add("RegOpenKeyEx" + suffix, 5, [&hive, text_of, put_handle, wide, view](ApiCall &call) {
			if (!call[4]) {
				call.ret(error_invalid_parameter);
				return;
			}
			uint64_t key = 0;
			const auto status = hive.open(call[0], view(call[0], text_of(call, call[1], wide), call.u32(3)), key);
			if (!status)
				put_handle(call, call[4], key);
			call.ret(status);
		});
		// SHDeleteEmptyKey removes a key only while it holds neither subkeys nor values.
		registry.add("SHDeleteEmptyKey" + suffix, 2, [&hive, text_of, wide, view](ApiCall &call) {
			const auto path = view(call[0], text_of(call, call[1], wide), 0);
			uint64_t key = 0;
			if (const auto status = hive.open(call[0], path, key)) {
				call.ret(status);
				return;
			}
			NativeRegistry::Info info;
			const auto status = hive.info(key, info);
			hive.close(key);
			if (status || info.subkeys || info.values) {
				call.ret(status ? status : 1020);
				return;
			}
			call.ret(hive.remove_key(call[0], path));
		});
		registry.add("RegOpenKey" + suffix, 3, [&hive, text_of, put_handle, wide, view](ApiCall &call) {
			if (!call[2]) {
				call.ret(error_invalid_parameter);
				return;
			}
			uint64_t key = 0;
			const auto status = hive.open(call[0], view(call[0], text_of(call, call[1], wide)), key);
			if (!status)
				put_handle(call, call[2], key);
			call.ret(status);
		});
		registry.add("RegCreateKey" + suffix, 3, [&hive, text_of, put_handle, wide, view](ApiCall &call) {
			if (!call[2]) {
				call.ret(error_invalid_parameter);
				return;
			}
			uint64_t key = 0;
			uint32_t disposition = 0;
			const auto status = hive.create(call[0], view(call[0], text_of(call, call[1], wide)), key, disposition);
			if (!status)
				put_handle(call, call[2], key);
			call.ret(status);
		});
		registry.add("RegCreateKeyEx" + suffix, 9, [&hive, text_of, put_handle, wide, view](ApiCall &call) {
			if (!call[7]) {
				call.ret(error_invalid_parameter);
				return;
			}
			uint64_t key = 0;
			uint32_t disposition = 0;
			const auto status = hive.create(call[0], view(call[0], text_of(call, call[1], wide), call.u32(5)), key, disposition);
			if (!status) {
				put_handle(call, call[7], key);
				if (call[8]) call.store<uint32_t>(call[8], disposition);
			}
			call.ret(status);
		});
		registry.add("RegSetValueEx" + suffix, 6, [&hive, text_of, wide, ansi_page](ApiCall &call) {
			const auto name = text_of(call, call[1], wide);
			const uint32_t type = call.u32(3), size = call.u32(5);
			if (!call[4] && size) {
				call.ret(error_invalid_parameter);
				return;
			}
			auto data = size ? call.read(call[4], size) : std::vector<uint8_t>{};
			if (!wide && is_text(type))
				data = from_ansi_bytes(data, ansi_page);
			call.ret(hive.set(call[0], name, type, data));
		});
		registry.add("RegQueryValueEx" + suffix, 6, [&hive, text_of, wide, ansi_page](ApiCall &call) {
			const auto name = text_of(call, call[1], wide);
			if (call[4] && !call[5]) {
				call.ret(error_invalid_parameter);
				return;
			}
			uint32_t type = 0;
			std::vector<uint8_t> data;
			const auto status = hive.query(call[0], name, type, data);
			if (status) {
				call.ret(status);
				return;
			}
			if (!wide && is_text(type))
				data = to_ansi_bytes(data, ansi_page);
			if (call[3]) call.store<uint32_t>(call[3], type);
			if (!call[4]) {
				if (call[5]) call.store<uint32_t>(call[5], uint32_t(data.size()));
				call.ret(0);
				return;
			}
			const uint32_t capacity = call.load<uint32_t>(call[5]);
			call.store<uint32_t>(call[5], uint32_t(data.size()));
			if (data.size() > capacity) {
				call.ret(error_more_data);
				return;
			}
			call.write(call[4], data);
			call.ret(0);
		});
		registry.add("RegDeleteValue" + suffix, 2, [&hive, text_of, wide](ApiCall &call) { call.ret(hive.remove_value(call[0], text_of(call, call[1], wide))); });
		registry.add("RegDeleteKey" + suffix, 2, [&hive, text_of, wide, view](ApiCall &call) { call.ret(hive.remove_key(call[0], view(call[0], text_of(call, call[1], wide)))); });
		// The older form: the buffer size is a plain count of characters, not a pointer to one.
		registry.add("RegEnumKey" + suffix, 4, [&hive, wide](ApiCall &call) {
			std::u16string name;
			const auto status = hive.enum_key(call[0], call.u32(1), name);
			if (status) {
				call.ret(status);
				return;
			}
			const uint32_t capacity = call.u32(3);
			const auto narrow = wide ? std::string() : encode_ansi(name, 1252);
			const size_t length = wide ? name.size() : narrow.size();
			if (!call[2] || capacity <= length) {
				call.ret(call[2] ? error_more_data : error_invalid_parameter);
				return;
			}
			if (wide) call.put_wide(call[2], name, capacity);
			else call.put_narrow(call[2], narrow, capacity);
			call.ret(0);
		});
		registry.add("RegEnumKeyEx" + suffix, 8, [&hive, wide](ApiCall &call) {
			std::u16string name;
			const auto status = hive.enum_key(call[0], call.u32(1), name);
			if (status) {
				call.ret(status);
				return;
			}
			const uint32_t capacity = call.load<uint32_t>(call[3]);
			const auto narrow = wide ? std::string() : encode_ansi(name, 1252);
			const size_t length = wide ? name.size() : narrow.size();
			if (capacity <= length) {
				call.ret(error_more_data);
				return;
			}
			if (wide) call.put_wide(call[2], name, capacity);
			else call.put_narrow(call[2], narrow, capacity);
			call.store<uint32_t>(call[3], uint32_t(length));
			if (call[5] && call[6]) {
				if (call.load<uint32_t>(call[6]) > 0)
					call.store<uint16_t>(call[5], 0);
				call.store<uint32_t>(call[6], 0);
			}
			if (call[7]) call.store<uint64_t>(call[7], 0);
			call.ret(0);
		});
		registry.add("RegEnumValue" + suffix, 8, [&hive, wide, ansi_page](ApiCall &call) {
			std::u16string name;
			uint32_t type = 0;
			std::vector<uint8_t> data;
			const auto status = hive.enum_value(call[0], call.u32(1), name, type, data);
			if (status) {
				call.ret(status);
				return;
			}
			if (!wide && is_text(type))
				data = to_ansi_bytes(data, ansi_page);
			const uint32_t name_capacity = call.load<uint32_t>(call[3]);
			const auto narrow = wide ? std::string() : encode_ansi(name, 1252);
			const size_t length = wide ? name.size() : narrow.size();
			if (name_capacity <= length) {
				call.ret(error_more_data);
				return;
			}
			if (call[4]) call.ret(error_invalid_parameter);
			if (call[5]) call.store<uint32_t>(call[5], type);
			if (call[6]) {
				if (!call[7]) {
					call.ret(error_invalid_parameter);
					return;
				}
				const uint32_t capacity = call.load<uint32_t>(call[7]);
				call.store<uint32_t>(call[7], uint32_t(data.size()));
				if (data.size() > capacity) {
					call.ret(error_more_data);
					return;
				}
				call.write(call[6], data);
			} else if (call[7]) {
				call.store<uint32_t>(call[7], uint32_t(data.size()));
			}
			if (wide) call.put_wide(call[2], name, name_capacity);
			else call.put_narrow(call[2], narrow, name_capacity);
			call.store<uint32_t>(call[3], uint32_t(length));
			call.ret(0);
		});
	}
	// The native registry calls name keys by object path: \Registry\Machine, or \Registry\User\<account> for the current user, then the key path;
	// a key handle in the attributes makes the path relative to it.
	constexpr uint32_t status_not_found = 0xc0000034U, status_denied = 0xc0000022U, status_invalid = 0xc000000dU, status_failure = 0xc0000001U,
		status_overflow = 0x80000005U, status_too_small = 0xc0000023U;
	const auto status_of = [](uint32_t win32) -> uint32_t {
		switch (win32) {
		case 0: return 0;
		case 2: case 3: return status_not_found;
		case 5: return status_denied;
		case 87: return status_invalid;
		default: return status_failure;
		}
	};
	struct ObjectKey {
		uint64_t root = 0;
		std::u16string path;
	};
	const auto object_key = [handle_width](ApiCall &call, uint64_t attributes) -> std::optional<ObjectKey> {
		if (!attributes)
			return std::nullopt;
		const auto root = call.load_pointer(attributes + 8);
		const auto name = call.load_pointer(attributes + 8 + handle_width);
		if (!name)
			return std::nullopt;
		const auto length = call.load<uint16_t>(name);
		const auto buffer = call.load_pointer(name + (handle_width == 4 ? 4 : 8));
		std::u16string text;
		if (length) {
			const auto bytes = call.read(buffer, length);
			text.resize(length / 2);
			std::memcpy(text.data(), bytes.data(), text.size() * 2);
		}
		ObjectKey result{root, text};
		if (root)
			return result;
		const auto fold = [](std::u16string value) {
			for (auto &character : value)
				if (character >= u'a' && character <= u'z')
					character = char16_t(character - (u'a' - u'A'));
			return value;
		};
		auto rest = text;
		const auto strip = [&](std::u16string_view prefix) {
			if (fold(rest.substr(0, prefix.size())) != prefix || (rest.size() > prefix.size() && rest[prefix.size()] != u'\\'))
				return false;
			rest.erase(0, std::min(rest.size(), prefix.size() + 1));
			return true;
		};
		if (strip(u"\\REGISTRY\\MACHINE")) {
			result.root = 0x80000002ULL;
		} else if (strip(u"\\REGISTRY\\USER")) {
			result.root = 0x80000001ULL;
			// The first component names the account; the current one is the only one.
			const auto end = rest.find(u'\\');
			rest = end == rest.npos ? std::u16string() : rest.substr(end + 1);
		} else {
			return std::nullopt;
		}
		result.path = rest;
		return result;
	};
	const auto create_key = [&hive, object_key, put_handle, status_of](ApiCall &call, uint64_t attributes, uint64_t out, uint64_t disposition_out) {
		const auto where = object_key(call, attributes);
		if (!where || !out) {
			call.ret(status_invalid);
			return;
		}
		uint64_t key = 0;
		uint32_t disposition = 0;
		const auto status = hive.create(where->root, where->path, key, disposition);
		if (!status) {
			put_handle(call, out, key);
			if (disposition_out)
				call.store<uint32_t>(disposition_out, disposition);
		}
		call.ret(status_of(status));
	};
	registry.add("NtCreateKey", 7, [create_key](ApiCall &call) { create_key(call, call[2], call[0], call[6]); });
	const auto open_key = [&hive, object_key, put_handle, status_of](ApiCall &call) {
		const auto where = object_key(call, call[2]);
		if (!where || !call[0]) {
			call.ret(status_invalid);
			return;
		}
		uint64_t key = 0;
		const auto status = hive.open(where->root, where->path, key);
		if (!status)
			put_handle(call, call[0], key);
		call.ret(status_of(status));
	};
	registry.add("NtOpenKey", 3, open_key);
	registry.add("NtOpenKeyEx", 4, open_key);
	registry.add("NtDeleteKey", 1, [&hive, status_of](ApiCall &call) { call.ret(status_of(hive.remove_key(call[0], u""))); });
	const auto name_of = [handle_width](ApiCall &call, uint64_t address) {
		if (!address)
			return std::u16string();
		const auto length = call.load<uint16_t>(address);
		const auto buffer = call.load_pointer(address + (handle_width == 4 ? 4 : 8));
		std::u16string text(length / 2, u'\0');
		if (length)
			std::memcpy(text.data(), call.read(buffer, length).data(), length / 2 * 2);
		return text;
	};
	// The last write time (0), the flags of a WOW64 key (1) and the control flags (2) are the classes a key takes; the time keeps the
	// hive's own record, so a valid key and class only succeed.
	registry.add("NtSetInformationKey", 4, [&hive, status_of](ApiCall &call) {
		NativeRegistry::Info info;
		if (const auto status = hive.info(call[0], info)) {
			call.ret(status_of(status));
			return;
		}
		static constexpr uint32_t sizes[] = {8, 4, 4};
		const uint32_t information = call.u32(1);
		if (information >= std::size(sizes)) {
			call.ret(0xc0000003);
			return;
		}
		call.ret(call.u32(3) != sizes[information] ? 0xc0000004 : 0);
	});
	// What the KEY_* and KEY_VALUE_* information forms share: little-endian words and UTF-16 text, and a buffer that takes the fixed part or
	// fails, then takes what fits of the rest.
	const auto put32 = [](std::vector<uint8_t> &out, uint32_t value) { for (int shift = 0; shift < 32; shift += 8) out.push_back(uint8_t(value >> shift)); };
	const auto put_text = [](std::vector<uint8_t> &out, std::u16string_view text) {
		for (const auto unit : text) {
			out.push_back(uint8_t(unit));
			out.push_back(uint8_t(unit >> 8));
		}
	};
	const auto deliver = [status_overflow, status_too_small](ApiCall &call, const std::vector<uint8_t> &out, size_t header, uint64_t buffer,
		uint32_t capacity, uint64_t result) {
		if (result)
			call.store<uint32_t>(result, uint32_t(out.size()));
		if (capacity < header) {
			call.ret(status_too_small);
			return;
		}
		call.write(buffer, std::span<const uint8_t>(out.data(), std::min<size_t>(out.size(), capacity)));
		call.ret(out.size() > capacity ? status_overflow : 0);
	};
	// KEY_*_INFORMATION: the basic and node forms carry the last name of the key, the name form its full path in the native namespace,
	// the full and cached forms the counts. The hive keeps no write times or classes, so both read as zero. The header size is 0 for a class
	// the hive does not answer.
	const auto key_information = [put32, put_text](uint32_t information, const NativeRegistry::Info &info, std::vector<uint8_t> &out) -> size_t {
		static constexpr std::pair<std::u16string_view, std::u16string_view> roots[] = {
			{u"HKEY_LOCAL_MACHINE", u"\\REGISTRY\\MACHINE"}, {u"HKEY_USERS", u"\\REGISTRY\\USER"},
			{u"HKEY_CURRENT_USER", u"\\REGISTRY\\USER\\S-1-5-21-1000-1000-1000-1000"}, {u"HKEY_CLASSES_ROOT", u"\\REGISTRY\\MACHINE\\SOFTWARE\\Classes"}};
		std::u16string native = info.path;
		for (const auto &[root, replacement] : roots)
			if (native.size() >= root.size() && equal_fold(native.substr(0, root.size()), root) &&
				(native.size() == root.size() || native[root.size()] == u'\\')) {
				native = std::u16string(replacement) + native.substr(root.size());
				break;
			}
		const std::u16string last = native.substr(native.find_last_of(u'\\') + 1);
		const uint32_t name_size = uint32_t(last.size() * 2);
		switch (information) {
		case 0: // basic
			for (const uint32_t word : {0u, 0u, 0u, name_size}) put32(out, word);
			put_text(out, last);
			return 16;
		case 1: // node: the class would follow the name
			for (const uint32_t word : {0u, 0u, 0u, 24 + name_size, 0u, name_size}) put32(out, word);
			put_text(out, last);
			return 24;
		case 2: // full
			for (const uint32_t word : {0u, 0u, 0u, 44u, 0u, info.subkeys, info.longest_subkey * 2, 0u, info.values, info.longest_name * 2, info.longest_data})
				put32(out, word);
			return 44;
		case 3: // name
			put32(out, uint32_t(native.size() * 2));
			put_text(out, native);
			return 4;
		case 4: // cached
			for (const uint32_t word : {0u, 0u, 0u, info.subkeys, info.longest_subkey * 2, info.values, info.longest_name * 2, info.longest_data, name_size})
				put32(out, word);
			return 36;
		default:
			return 0;
		}
	};
	registry.add("NtQueryKey", 5, [&hive, status_of, key_information, deliver](ApiCall &call) {
		NativeRegistry::Info info;
		if (const auto status = hive.info(call[0], info)) {
			call.ret(status_of(status));
			return;
		}
		std::vector<uint8_t> out;
		const size_t header = key_information(call.u32(1), info, out);
		if (!header) {
			call.ret(0xc0000003U);
			return;
		}
		deliver(call, out, header, call[2], call.u32(3), call[4]);
	});
	// A subkey by its position: its own information, read through a handle opened for the moment. STATUS_NO_MORE_ENTRIES past the last.
	registry.add("NtEnumerateKey", 6, [&hive, status_of, key_information, deliver](ApiCall &call) {
		std::u16string name;
		if (const auto status = hive.enum_key(call[0], call.u32(1), name)) {
			call.ret(status == 259 ? 0x8000001aU : status_of(status));
			return;
		}
		uint64_t child = 0;
		NativeRegistry::Info info;
		auto status = hive.open(call[0], name, child);
		if (!status) {
			status = hive.info(child, info);
			hive.close(child);
		}
		if (status) {
			call.ret(status_of(status));
			return;
		}
		std::vector<uint8_t> out;
		const size_t header = call.u32(2) == 3 ? 0 : key_information(call.u32(2), info, out);
		if (!header) {
			call.ret(0xc000000dU);
			return;
		}
		deliver(call, out, header, call[3], call.u32(4), call[5]);
	});
	// The hive is written as it changes, so a flush has nothing left to do for a key that exists.
	registry.add("NtFlushKey", 1, [&hive, status_of](ApiCall &call) {
		NativeRegistry::Info info;
		call.ret(status_of(hive.info(call[0], info)));
	});
	// Change notification is not delivered: an asynchronous request stays pending, as one would until a change, and a synchronous one returns
	// at once rather than block its thread for ever, as RegNotifyChangeKeyValue does here.
	registry.add("NtNotifyChangeKey", 10, [&hive, status_of](ApiCall &call) {
		NativeRegistry::Info info;
		const auto status = hive.info(call[0], info);
		call.ret(status ? status_of(status) : call.u32(9) ? 0x103U : 0);
	});
	registry.add("NtNotifyChangeMultipleKeys", 12, [&hive, status_of](ApiCall &call) {
		NativeRegistry::Info info;
		const auto status = hive.info(call[0], info);
		call.ret(status ? status_of(status) : call.u32(11) ? 0x103U : 0);
	});
	// The hive has no rename; a program sees the failure a key that cannot be renamed gives.
	registry.add("NtRenameKey", 2, [](ApiCall &call) { call.ret(0xc0000002U); });
	registry.add("NtDeleteValueKey", 2,[&hive, name_of, status_of](ApiCall &call) { call.ret(status_of(hive.remove_value(call[0], name_of(call, call[1])))); });
	registry.add("NtSetValueKey", 6, [&hive, name_of, status_of](ApiCall &call) {
		const uint32_t size = call.u32(5);
		if (!call[4] && size) {
			call.ret(status_invalid);
			return;
		}
		call.ret(status_of(hive.set(call[0], name_of(call, call[1]), call.u32(3), size ? call.read(call[4], size) : std::vector<uint8_t>{})));
	});
	// KEY_VALUE_*_INFORMATION: the type and sizes come first; the basic form carries the name, the full form the name and the data, the partial one
	// the data. The header size is 0 for a class the hive does not answer.
	const auto value_information = [put32, put_text](uint32_t information, std::u16string_view name, uint32_t type, const std::vector<uint8_t> &data,
		std::vector<uint8_t> &out) -> size_t {
		const uint32_t name_size = uint32_t(name.size() * 2);
		if (information == 0) {
			for (const uint32_t word : {0u, type, name_size}) put32(out, word);
			put_text(out, name);
			return 12;
		}
		if (information == 1) {
			const uint32_t data_at = (20 + name_size + 3) / 4 * 4;
			for (const uint32_t word : {0u, type, data_at, uint32_t(data.size()), name_size}) put32(out, word);
			put_text(out, name);
			out.resize(data_at);
			out.insert(out.end(), data.begin(), data.end());
			return 20;
		}
		if (information == 2) {
			for (const uint32_t word : {0u, type, uint32_t(data.size())}) put32(out, word);
			out.insert(out.end(), data.begin(), data.end());
			return 12;
		}
		return 0;
	};
	registry.add("NtQueryValueKey", 6, [&hive, name_of, status_of, value_information, deliver](ApiCall &call) {
		const auto name = name_of(call, call[1]);
		uint32_t type = 0;
		std::vector<uint8_t> data;
		if (const auto status = hive.query(call[0], name, type, data)) {
			call.ret(status_of(status));
			return;
		}
		std::vector<uint8_t> out;
		const size_t header = value_information(call.u32(2), name, type, data, out);
		if (!header) {
			call.ret(status_invalid);
			return;
		}
		deliver(call, out, header, call[3], call.u32(4), call[5]);
	});
	registry.add("NtEnumerateValueKey", 6, [&hive, status_of, value_information, deliver](ApiCall &call) {
		std::u16string name;
		uint32_t type = 0;
		std::vector<uint8_t> data;
		if (const auto status = hive.enum_value(call[0], call.u32(1), name, type, data)) {
			call.ret(status == 259 ? 0x8000001aU : status_of(status));
			return;
		}
		std::vector<uint8_t> out;
		const size_t header = value_information(call.u32(2), name, type, data, out);
		if (!header) {
			call.ret(status_invalid);
			return;
		}
		deliver(call, out, header, call[3], call.u32(4), call[5]);
	});
	// The key path of the current user in the native namespace; the string belongs to the program, which frees it with RtlFreeUnicodeString.
	registry.add("RtlFormatCurrentUserKeyPath", 1, [&context](ApiCall &call) {
		if (!call[0]) {
			call.ret(status_invalid);
			return;
		}
		const std::u16string path = u"\\REGISTRY\\USER\\S-1-5-21-1000-1000-1000-1000";
		const auto block = context.allocator.allocate(path.size() * 2 + 2, true);
		if (!block) {
			call.ret(0xc0000017U);
			return;
		}
		call.write(block, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(path.data()), path.size() * 2));
		const size_t width = call.pointer_width();
		std::vector<uint8_t> text(width == 4 ? 8 : 16);
		const uint16_t length = uint16_t(path.size() * 2), maximum = uint16_t(length + 2);
		std::memcpy(text.data(), &length, 2);
		std::memcpy(text.data() + 2, &maximum, 2);
		std::memcpy(text.data() + (width == 4 ? 4 : 8), &block, width);
		call.write(call[0], text);
		call.ret(0);
	});
	registry.add("RtlFreeUnicodeString", 1, [&context](ApiCall &call) {
		if (!call[0])
			return;
		const auto buffer = call.load_pointer(call[0] + (call.pointer_width() == 4 ? 4 : 8));
		if (buffer)
			context.allocator.release(buffer);
	});
	// A counted string of its own, its buffer from the process heap that RtlFreeUnicodeString returns it to.
	registry.add("RtlCreateUnicodeStringFromAsciiz", 2, [&context, ansi_page](ApiCall &call) {
		const auto text = from_ansi(call.narrow(call[1]), ansi_page);
		const auto block = text.size() < 32767 ? context.allocator.allocate(text.size() * 2 + 2, true) : 0;
		if (!block) {
			call.ret(0);
			return;
		}
		call.write(block, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size() * 2));
		const size_t width = call.pointer_width();
		std::vector<uint8_t> counted(width == 4 ? 8 : 16);
		const uint16_t length = uint16_t(text.size() * 2), maximum = uint16_t(length + 2);
		std::memcpy(counted.data(), &length, 2);
		std::memcpy(counted.data() + 2, &maximum, 2);
		std::memcpy(counted.data() + (width == 4 ? 4 : 8), &block, width);
		call.write(call[0], counted);
		call.ret(1);
	});
	// UTF-16 to the ANSI code page, as many bytes as fit; and the bytes the whole text needs.
	registry.add("RtlUnicodeToMultiByteN", 5, [ansi_page](ApiCall &call) {
		const auto bytes = call.read(call[3], call.u32(4) & ~1U);
		std::u16string text(bytes.size() / 2, u'\0');
		std::memcpy(text.data(), bytes.data(), text.size() * 2);
		auto narrow = to_ansi(text, ansi_page);
		narrow.resize(std::min<size_t>(narrow.size(), call.u32(1)));
		call.write(call[0], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(narrow.data()), narrow.size()));
		if (call[2])
			call.store<uint32_t>(call[2], uint32_t(narrow.size()));
		call.ret(0);
	});
	registry.add("RtlUnicodeToMultiByteSize", 3, [ansi_page](ApiCall &call) {
		const auto bytes = call.read(call[1], call.u32(2) & ~1U);
		std::u16string text(bytes.size() / 2, u'\0');
		std::memcpy(text.data(), bytes.data(), text.size() * 2);
		call.store<uint32_t>(call[0], uint32_t(to_ansi(text, ansi_page).size()));
		call.ret(0);
	});
	// Whether a buffer reads as UTF-16: a byte order mark says so, otherwise text whose odd bytes are mostly zero, as Latin text in
	// UTF-16 is. The tests asked for are answered with the ones that held.
	registry.add("RtlIsTextUnicode", 3, [](ApiCall &call) {
		const auto bytes = call.read(call[0], std::min<uint32_t>(call.u32(1), 65536));
		constexpr uint32_t statistics = 0x0002, signature = 0x0008, reverse_signature = 0x0080, odd_length = 0x0200;
		uint32_t held = 0;
		if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe)
			held |= signature;
		if (bytes.size() >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff)
			held |= reverse_signature;
		if (bytes.size() % 2)
			held |= odd_length;
		size_t zero_high = 0;
		for (size_t at = 1; at < bytes.size(); at += 2)
			zero_high += bytes[at] == 0;
		if (bytes.size() >= 2 && zero_high * 2 > bytes.size() / 2)
			held |= statistics;
		const uint32_t asked = call[2] ? call.load<uint32_t>(call[2]) : 0xffffffffU;
		if (call[2])
			call.store<uint32_t>(call[2], held & asked);
		call.ret((held & (signature | statistics) & asked) && !(held & (reverse_signature | odd_length)) ? 1 : 0);
	});
	registry.add("RtlImageNtHeader", 1, [](ApiCall &call) {
		if (!call[0] || call.load<uint16_t>(call[0]) != 0x5a4d) {
			call.ret(0);
			return;
		}
		const uint64_t headers = call[0] + call.load<uint32_t>(call[0] + 0x3c);
		call.ret(call.load<uint32_t>(headers) == 0x4550 ? headers : 0);
	});
	registry.add("RtlInitUnicodeString", 2, [](ApiCall &call) {
		if (!call[0])
			return;
		size_t length = 0;
		if (call[1]) {
			const auto text = call.wide(call[1], 32767);
			length = text.size() * 2;
		}
		const size_t width = call.pointer_width();
		std::vector<uint8_t> text(width == 4 ? 8 : 16);
		const uint16_t used = uint16_t(length), maximum = uint16_t(call[1] ? length + 2 : 0);
		std::memcpy(text.data(), &used, 2);
		std::memcpy(text.data() + 2, &maximum, 2);
		const uint64_t source = call[1];
		std::memcpy(text.data() + (width == 4 ? 4 : 8), &source, width);
		call.write(call[0], text);
	});
	// Counted strings compared, copied and written. Case-insensitive comparison goes through upper case, as RtlUpcaseUnicodeChar would.
	const auto upper_text = [](std::u16string text) {
		for (auto &unit : text)
			unit = upper_case(unit);
		return text;
	};
	registry.add("RtlUpcaseUnicodeChar", 1, [](ApiCall &call) { call.ret(upper_case(char16_t(call[0]))); });
	registry.add("RtlCompareUnicodeString", 3, [name_of, upper_text](ApiCall &call) {
		auto left = name_of(call, call[0]), right = name_of(call, call[1]);
		if (call.u32(2) & 0xff) {
			left = upper_text(left);
			right = upper_text(right);
		}
		const size_t shared = std::min(left.size(), right.size());
		for (size_t index = 0; index < shared; ++index)
			if (left[index] != right[index]) {
				call.ret(uint64_t(int64_t(int32_t(left[index]) - int32_t(right[index]))));
				return;
			}
		call.ret(uint64_t(int64_t(int32_t(left.size()) - int32_t(right.size()))));
	});
	registry.add("RtlEqualUnicodeString", 3, [name_of, upper_text](ApiCall &call) {
		const auto left = name_of(call, call[0]), right = name_of(call, call[1]);
		call.ret(call.u32(2) & 0xff ? upper_text(left) == upper_text(right) : left == right);
	});
	registry.add("RtlPrefixUnicodeString", 3, [name_of, upper_text](ApiCall &call) {
		auto prefix = name_of(call, call[0]), text = name_of(call, call[1]);
		if (call.u32(2) & 0xff) {
			prefix = upper_text(prefix);
			text = upper_text(text);
		}
		call.ret(text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0);
	});
	// UNICODE_STRING: the length and capacity in bytes, then the buffer pointer after alignment.
	const auto string_fields = [](ApiCall &call, uint64_t address) {
		return std::tuple{call.load<uint16_t>(address + 2), call.load_pointer(address + (call.pointer_width() == 4 ? 4 : 8))};
	};
	registry.add("RtlCopyUnicodeString", 2, [name_of, string_fields](ApiCall &call) {
		const auto [capacity, buffer] = string_fields(call, call[0]);
		const auto source = call[1] ? name_of(call, call[1]) : std::u16string();
		const size_t units = std::min<size_t>(source.size(), capacity / 2);
		std::vector<uint8_t> bytes(units * 2);
		std::memcpy(bytes.data(), source.data(), bytes.size());
		if (units)
			call.write(buffer, bytes);
		if (units * 2 + 2 <= capacity)
			call.store<uint16_t>(buffer + units * 2, 0);
		call.store<uint16_t>(call[0], uint16_t(units * 2));
	});
	// The digits of a value in base 2, 8, 10 (also for 0) or 16, upper case, ending with a NUL when it fits.
	registry.add("RtlIntegerToUnicodeString", 3, [string_fields](ApiCall &call) {
		uint32_t value = call.u32(0), base = call.u32(1) ? call.u32(1) : 10;
		if (base != 2 && base != 8 && base != 10 && base != 16) {
			call.ret(0xc000000dU);
			return;
		}
		std::u16string digits;
		do {
			digits.insert(digits.begin(), u"0123456789ABCDEF"[value % base]);
			value /= base;
		} while (value);
		const auto [capacity, buffer] = string_fields(call, call[2]);
		if (digits.size() * 2 > capacity) {
			call.ret(0x80000005U);
			return;
		}
		if (digits.size() * 2 + 2 <= capacity)
			digits.push_back(u'\0');
		std::vector<uint8_t> bytes(digits.size() * 2);
		std::memcpy(bytes.data(), digits.data(), bytes.size());
		call.write(buffer, bytes);
		call.store<uint16_t>(call[2], uint16_t((digits.back() ? digits.size() : digits.size() - 1) * 2));
		call.ret(0);
	});
	// A name against an expression with the file system's wildcards: * and ? as usual, and the DOS forms < (any run up to the last dot),
	// > (one character, or none at a dot or the end) and " (a dot, or none at the end). With case ignored the expression is already upper case.
	registry.add("RtlIsNameInExpression", 4, [name_of, upper_text](ApiCall &call) {
		const auto expression = name_of(call, call[0]);
		const auto name = call.u32(2) & 0xff ? upper_text(name_of(call, call[1])) : name_of(call, call[1]);
		const size_t length = name.size(), last_dot = name.rfind(u'.');
		std::vector<int8_t> memo((expression.size() + 1) * (length + 1), -1);
		std::function<bool(size_t, size_t)> match = [&](size_t at, size_t position) -> bool {
			auto &known = memo[at * (length + 1) + position];
			if (known >= 0)
				return known;
			bool result = false;
			if (at == expression.size()) {
				result = position == length;
			} else {
				const char16_t wanted = expression[at];
				const bool more = position < length;
				switch (wanted) {
				case u'*': result = match(at + 1, position) || (more && match(at, position + 1)); break;
				case u'<': result = match(at + 1, position) || (more && (name[position] != u'.' || position != last_dot) && match(at, position + 1)); break;
				case u'>': result = more && name[position] != u'.' ? match(at + 1, position + 1) : match(at + 1, position); break;
				case u'"': result = more && name[position] == u'.' ? match(at + 1, position + 1) : !more && match(at + 1, position); break;
				case u'?': result = more && match(at + 1, position + 1); break;
				default: result = more && name[position] == wanted && match(at + 1, position + 1); break;
				}
			}
			known = result;
			return result;
		};
		call.ret(match(0, 0));
	});
	registry.add("RtlGetLastNtStatus", 0, [](ApiCall &call) { call.ret(0); });
	registry.add("RtlNtStatusToDosError", 1, [](ApiCall &call) {
		switch (call.u32(0)) {
		case 0: call.ret(0); break;
		case 0xc0000034U: case 0xc000003aU: call.ret(2); break;
		case 0xc0000022U: call.ret(5); break;
		case 0xc000000dU: call.ret(87); break;
		case 0xc0000017U: call.ret(8); break;
		case 0xc0000023U: call.ret(122); break;
		case 0x80000005U: call.ret(234); break;
		case 0xc0000008U: call.ret(6); break;
		case 0x80000006U: call.ret(18); break;
		case 0xc000000fU: call.ret(2); break;
		case 0xc0000103U: call.ret(267); break;
		case 0xc0000003U: call.ret(87); break;
		case 0xc00000bbU: call.ret(50); break;
		case 0xc0000004U: call.ret(24); break;
		case 0xc0000005U: call.ret(998); break;
		default: call.ret(317); break;
		}
	});
	registry.add("RegDeleteTreeW", 2, [&hive, text_of, view](ApiCall &call) {
		call.ret(hive.remove_tree(call[0], call[1] ? view(call[0], text_of(call, call[1], true)) : std::u16string()));
	});
	registry.add("RegDeleteTreeA", 2, [&hive, text_of, view](ApiCall &call) {
		call.ret(hive.remove_tree(call[0], call[1] ? view(call[0], text_of(call, call[1], false)) : std::u16string()));
	});
	registry.add("RegDeleteKeyExW", 4, [&hive, text_of, view](ApiCall &call) { call.ret(hive.remove_key(call[0], view(call[0], text_of(call, call[1], true), call.u32(2)))); });
	registry.add("RegCloseKey", 1, [&hive](ApiCall &call) { call.ret(hive.close(call[0])); });
	registry.add("RegFlushKey", 1, [](ApiCall &call) { call.ret(0); });
	// Predefined keys are resolved for each operation; no cached handles need invalidating.
	registry.add("RegDisablePredefinedCache", 0, [](ApiCall &call) { call.ret(0); });
	registry.add("RegDisablePredefinedCacheEx", 0, [](ApiCall &call) { call.ret(0); });
	registry.add("RegNotifyChangeKeyValue", 5, [](ApiCall &call) { call.ret(0); });
	registry.add("RegOpenCurrentUser", 2, [put_handle](ApiCall &call) {
		put_handle(call, call[1], 0x80000001ULL);
		call.ret(0);
	});
	// The counts are the same in characters for the wide and the ANSI form; the class of a key is always empty here.
	const auto query_info = [&hive](ApiCall &call) {
		NativeRegistry::Info info;
		const auto status = hive.info(call[0], info);
		if (status) {
			call.ret(status);
			return;
		}
		if (call[2]) call.store<uint32_t>(call[2], 0);
		if (call[4]) call.store<uint32_t>(call[4], info.subkeys);
		if (call[5]) call.store<uint32_t>(call[5], info.longest_subkey);
		if (call[6]) call.store<uint32_t>(call[6], 0);
		if (call[7]) call.store<uint32_t>(call[7], info.values);
		if (call[8]) call.store<uint32_t>(call[8], info.longest_name);
		if (call[9]) call.store<uint32_t>(call[9], info.longest_data);
		if (call[10]) call.store<uint32_t>(call[10], 0);
		if (call[11]) call.store<uint64_t>(call[11], 0);
		call.ret(0);
	};
	registry.add("RegQueryInfoKeyW", 12, query_info);
	registry.add("RegQueryInfoKeyA", 12, query_info);
	// RegGetValue filters by type (RRF_RT_*), opens the subkey itself in the caller's registry view (RRF_SUBKEY_WOW6464KEY and
	// RRF_SUBKEY_WOW6432KEY choose one) and expands REG_EXPAND_SZ unless told not to.
	const auto get_value = [&hive, &context, text_of, view, ansi_page](ApiCall &call, bool wide) {
		const uint32_t flags = call.u32(3);
		uint64_t key = call[0];
		uint64_t opened = 0;
		const auto subkey = text_of(call, call[1], wide);
		if (!subkey.empty()) {
			const uint32_t access = (flags & 0x10000 ? 0x100 : 0) | (flags & 0x20000 ? 0x200 : 0);
			const auto status = hive.open(key, view(key, subkey, access), opened);
			if (status) {
				call.ret(status);
				return;
			}
			key = opened;
		}
		uint32_t type = 0;
		std::vector<uint8_t> data;
		auto status = hive.query(key, text_of(call, call[2], wide), type, data);
		if (opened) {
			hive.close(opened);
			context.objects.release_reserved(opened);
		}
		if (status) {
			call.ret(status);
			return;
		}
		const auto allowed = [&](uint32_t value) {
			switch (value) {
			case type_sz: return flags & 0x2; case type_expand_sz: return flags & 0x4; case type_multi_sz: return flags & 0x20;
			case type_dword: return flags & 0x10; case type_qword: return flags & 0x40; case type_binary: return flags & 0x8;
			default: return 0U;
			}
		};
		if ((flags & 0xffff) && !allowed(type) && !(flags & 1)) {
			call.ret(1629);
			return;
		}
		if (!wide && is_text(type))
			data = to_ansi_bytes(data, ansi_page);
		if (call[4]) call.store<uint32_t>(call[4], type);
		if (!call[5]) {
			if (call[6]) call.store<uint32_t>(call[6], uint32_t(data.size()));
			call.ret(0);
			return;
		}
		const uint32_t capacity = call.load<uint32_t>(call[6]);
		call.store<uint32_t>(call[6], uint32_t(data.size()));
		if (data.size() > capacity) {
			call.ret(error_more_data);
			return;
		}
		call.write(call[5], data);
		call.ret(0);
	};
	registry.add("RegGetValueW", 7, [get_value](ApiCall &call) { get_value(call, true); });
	registry.add("RegGetValueA", 7, [get_value](ApiCall &call) { get_value(call, false); });
	// RegSetKeyValue creates the subkey when it is missing, then writes the value as RegSetValueEx does.
	for (const bool wide : {true, false})
		registry.add(wide ? "RegSetKeyValueW" : "RegSetKeyValueA", 6, [&hive, &context, text_of, view, ansi_page, wide](ApiCall &call) {
			const uint32_t type = call.u32(3), size = call.u32(5);
			if (!call[4] && size) {
				call.ret(error_invalid_parameter);
				return;
			}
			uint64_t key = call[0];
			uint64_t created = 0;
			const auto subkey = text_of(call, call[1], wide);
			if (!subkey.empty()) {
				uint32_t disposition = 0;
				const auto status = hive.create(key, view(key, subkey, 0), created, disposition);
				if (status) {
					call.ret(status);
					return;
				}
				key = created;
			}
			auto data = size ? call.read(call[4], size) : std::vector<uint8_t>{};
			if (!wide && is_text(type))
				data = from_ansi_bytes(data, ansi_page);
			const auto status = hive.set(key, text_of(call, call[2], wide), type, data);
			if (created) {
				hive.close(created);
				context.objects.release_reserved(created);
			}
			call.ret(status);
		});
	// Whether .NET programs run as 64-bit ones on 64-bit Windows: the system reads it from Enable64Bit of the framework's key, and the
	// framework's setup turns it on through SetComPlusPackageInstallStatus. Only bit 0 has a meaning.
	registry.add("GetComPlusPackageInstallStatus", 0, [&hive](ApiCall &call) {
		uint64_t key = 0;
		uint32_t type = 0, status = 1;
		std::vector<uint8_t> data;
		if (!hive.open(0x80000002ULL, u"SOFTWARE\\Microsoft\\.NETFramework", key)) {
			if (!hive.query(key, u"Enable64Bit", type, data) && type == 4 && data.size() == 4)
				std::memcpy(&status, data.data(), 4);
			hive.close(key);
		}
		call.ret(status);
	});
	registry.add("SetComPlusPackageInstallStatus", 1, [&hive](ApiCall &call) {
		const uint32_t status = call.u32(0);
		uint64_t key = 0;
		uint32_t disposition = 0;
		if (status & ~1U || hive.create(0x80000002ULL, u"SOFTWARE\\Microsoft\\.NETFramework", key, disposition)) {
			call.fail(87);
			return;
		}
		hive.set(key, u"Enable64Bit", 4, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&status), 4));
		hive.close(key);
		call.ret(1);
	});
	// Transactions take effect at once (see CreateTransaction), so each transacted key call is its plain form: (name, plain form, its
	// arguments, the ones the plain form takes from the first).
	struct Transacted {
		const char *name, *plain;
		size_t arguments, taken;
	};
	const Transacted transacted[] = {{"RegOpenKeyTransactedW", "RegOpenKeyExW", 7, 5}, {"RegOpenKeyTransactedA", "RegOpenKeyExA", 7, 5},
		{"RegCreateKeyTransactedW", "RegCreateKeyExW", 11, 9}, {"RegCreateKeyTransactedA", "RegCreateKeyExA", 11, 9},
		{"RegDeleteKeyTransactedW", "RegDeleteKeyExW", 6, 4}};
	for (const auto &entry : transacted) {
		const auto plain = registry.find(entry.plain);
		if (!plain)
			continue;
		registry.add(entry.name, entry.arguments, [handler = plain->second, taken = entry.taken](ApiCall &call) {
			std::vector<uint64_t> words;
			for (size_t index = 0; index < taken; ++index)
				words.push_back(call[index]);
			ApiCall inner(call.process, call.environment, words);
			handler(inner);
			call.ret(inner.result);
		});
	}
	(void)error_no_more_items;
	(void)error_file_not_found;
}

}
