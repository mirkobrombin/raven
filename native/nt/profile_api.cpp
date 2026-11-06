#include "profile_api.h"
#include "nls.h"
#include "paths.h"

#include <fstream>
#include <iterator>
#include <vector>
#include <stdexcept>

namespace raven {

namespace {

std::u16string fold(std::u16string_view value)
{
	std::u16string result(value);
	for (auto &character : result)
		if (character >= u'a' && character <= u'z')
			character = char16_t(character - (u'a' - u'A'));
	return result;
}

std::u16string widen(std::string_view value)
{
	std::u16string result;
	for (const char character : value)
		result.push_back(char16_t(static_cast<unsigned char>(character)));
	return result;
}

std::u16string trim(std::u16string value)
{
	while (!value.empty() && (value.front() == u' ' || value.front() == u'\t'))
		value.erase(0, 1);
	while (!value.empty() && (value.back() == u' ' || value.back() == u'\t' || value.back() == u'\r'))
		value.pop_back();
	return value;
}

// A profile's lines without their line ends, and whether the file is Unicode: UTF-16 after a byte order mark, as Windows writes Unicode
// profiles, otherwise bytes widened to characters.
struct ProfileFile {
	std::vector<std::u16string> lines;
	bool unicode = false;
};

std::optional<ProfileFile> read_file(const std::filesystem::path &file)
{
	std::ifstream input(file, std::ios::binary);
	if (!input)
		return std::nullopt;
	const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	ProfileFile result;
	std::u16string content;
	if (bytes.size() >= 2 && uint8_t(bytes[0]) == 0xff && uint8_t(bytes[1]) == 0xfe) {
		result.unicode = true;
		for (size_t index = 2; index + 1 < bytes.size(); index += 2)
			content.push_back(char16_t(uint8_t(bytes[index]) | uint8_t(bytes[index + 1]) << 8));
	} else {
		content = widen(bytes);
	}
	for (size_t start = 0; start < content.size();) {
		auto end = content.find(u'\n', start);
		if (end == content.npos)
			end = content.size();
		auto line = content.substr(start, end - start);
		if (!line.empty() && line.back() == u'\r')
			line.pop_back();
		result.lines.push_back(std::move(line));
		start = end + 1;
	}
	return result;
}

// The file's lines, trimmed.
std::optional<std::vector<std::u16string>> read_lines(const std::filesystem::path &file)
{
	auto contents = read_file(file);
	if (!contents)
		return std::nullopt;
	for (auto &line : contents->lines)
		line = trim(std::move(line));
	return std::move(contents->lines);
}

// The lines of one section that are not comments, in file order.
std::optional<std::vector<std::u16string>> section_lines(const std::filesystem::path &file, std::u16string_view section)
{
	const auto lines = read_lines(file);
	if (!lines)
		return std::nullopt;
	const auto wanted = fold(section);
	std::optional<std::vector<std::u16string>> result;
	bool inside = false;
	for (const auto &text : *lines) {
		if (text.empty() || text[0] == u';')
			continue;
		if (text[0] == u'[') {
			const auto close = text.find(u']');
			inside = close != text.npos && fold(trim(text.substr(1, close - 1))) == wanted;
			if (inside && !result)
				result.emplace();
			continue;
		}
		if (inside)
			result->push_back(text);
	}
	return result;
}

// First matching value, or nullopt when the file, section or key is absent.
std::optional<std::u16string> lookup(const std::filesystem::path &file, std::u16string_view section, std::u16string_view key)
{
	const auto lines = read_lines(file);
	if (!lines)
		return std::nullopt;
	const auto wanted_section = fold(section), wanted_key = fold(key);
	bool inside = false;
	for (const auto &text : *lines) {
		if (text.empty() || text[0] == u';')
			continue;
		if (text[0] == u'[') {
			const auto close = text.find(u']');
			inside = close != text.npos && fold(trim(text.substr(1, close - 1))) == wanted_section;
			continue;
		}
		const auto equals = text.find(u'=');
		if (inside && equals != text.npos && fold(trim(text.substr(0, equals))) == wanted_key) {
			auto value = trim(text.substr(equals + 1));
			if (value.size() >= 2 && value.front() == value.back() && (value.front() == u'"' || value.front() == u'\''))
				value = value.substr(1, value.size() - 2);
			return value;
		}
	}
	return std::nullopt;
}


std::vector<std::u16string> section_names(const std::filesystem::path &file)
{
	std::vector<std::u16string> names;
	for (const auto &text : read_lines(file).value_or(std::vector<std::u16string>{})) {
		if (text.empty() || text[0] != u'[')
			continue;
		if (const auto close = text.find(u']'); close != text.npos)
			names.push_back(trim(text.substr(1, close - 1)));
	}
	return names;
}


std::string narrow_line(std::u16string_view value)
{
	// A file that is not Unicode is read as bytes widened to characters, so it is written the same way; what does not fit a byte becomes a
	// question mark.
	std::string result;
	for (const auto character : value)
		result.push_back(character <= 0xff ? char(character) : '?');
	return result;
}

// Sets, replaces or removes a value (or a whole section) in an INI file, keeping every other line as it was. The documentation of
// WritePrivateProfileString: a file created with Unicode characters gets Unicode characters, any other file ANSI ones.
bool write_value(const std::filesystem::path &file, std::u16string_view section, const std::u16string *key, const std::u16string *value)
{
	auto contents = read_file(file).value_or(ProfileFile{});
	auto &lines = contents.lines;
	const auto wanted_section = fold(section);
	const auto wanted_key = key ? fold(*key) : std::u16string{};
	size_t section_start = lines.size(), section_end = lines.size();
	bool found_section = false;
	for (size_t index = 0; index < lines.size(); ++index) {
		const auto text = trim(lines[index]);
		if (!text.empty() && text[0] == u'[') {
			const auto close = text.find(u']');
			const bool match = close != text.npos && fold(trim(text.substr(1, close - 1))) == wanted_section;
			if (found_section && !match && section_end == lines.size()) {
				section_end = index;
				break;
			}
			if (match && !found_section) {
				found_section = true;
				section_start = index;
			}
		}
	}
	if (!key) {
		// No key: the whole section goes.
		if (!found_section)
			return true;
		lines.erase(lines.begin() + static_cast<ptrdiff_t>(section_start), lines.begin() + static_cast<ptrdiff_t>(section_end));
	} else {
		bool replaced = false;
		if (found_section)
			for (size_t index = section_start + 1; index < section_end; ++index) {
				const auto text = trim(lines[index]);
				if (text.empty() || text[0] == u';')
					continue;
				const auto equals = text.find(u'=');
				if (equals == text.npos || fold(trim(text.substr(0, equals))) != wanted_key)
					continue;
				if (value) {
					lines[index] = *key + u"=" + *value;
				} else {
					lines.erase(lines.begin() + static_cast<ptrdiff_t>(index));
				}
				replaced = true;
				break;
			}
		if (!replaced && value) {
			const std::u16string entry = *key + u"=" + *value;
			if (found_section) {
				// After the last line of the section that is not blank.
				size_t at = section_end;
				while (at > section_start + 1 && trim(lines[at - 1]).empty())
					--at;
				lines.insert(lines.begin() + static_cast<ptrdiff_t>(at), entry);
			} else {
				if (!lines.empty() && !trim(lines.back()).empty())
					lines.push_back(u"");
				lines.push_back(u"[" + std::u16string(section) + u"]");
				lines.push_back(entry);
			}
		}
	}
	std::string bytes = contents.unicode ? "\xff\xfe" : "";
	for (const auto &line : lines) {
		if (contents.unicode) {
			for (const auto character : line + u"\r\n") {
				bytes.push_back(char(character & 0xff));
				bytes.push_back(char(character >> 8));
			}
		} else {
			bytes += narrow_line(line) + "\r\n";
		}
	}
	std::ofstream output(file, std::ios::binary | std::ios::trunc);
	if (!output)
		return false;
	output << bytes;
	return bool(output);
}

}

NativeProfile::NativeProfile(NativeProcess &process, NativeEnvironment &environment, const NativePrefix &prefix)
	: process(process), environment(environment), prefix(prefix) {}

std::optional<std::u16string> NativeProfile::text(uint64_t address) const
{
	if (!address)
		return std::nullopt;
	std::u16string result;
	for (size_t index = 0; index < 32768; ++index) {
		const auto bytes = process.read_memory(address + index * 2, 2);
		const char16_t character = char16_t(bytes[0] | bytes[1] << 8);
		if (!character)
			return result;
		result.push_back(character);
	}
	throw std::runtime_error("native profile string exceeds policy limit");
}

// Writes strings that each end in a terminator, then one more; a buffer that is too small is filled and ends in two terminators. The
// result counts the characters written without the last terminator.
uint64_t NativeProfile::put_list(uint64_t address, size_t capacity, const std::vector<std::u16string> &entries) const
{
	std::u16string joined;
	for (const auto &entry : entries)
		joined += entry + u'\0';
	if (capacity < 2)
		return 0;
	const bool fits = joined.size() + 1 <= capacity;
	if (!fits)
		joined.resize(capacity - 2);
	joined.push_back(u'\0');
	if (!fits)
		joined.push_back(u'\0');
	std::vector<uint8_t> bytes;
	for (const auto character : joined) {
		bytes.push_back(uint8_t(character));
		bytes.push_back(uint8_t(character >> 8));
	}
	process.write_memory(address, bytes);
	return fits ? (joined.empty() ? 0 : joined.size() - 1) : capacity - 2;
}

std::optional<std::filesystem::path> NativeProfile::locate(const std::u16string &file_name) const
{
	// A name without a full path is a file of the Windows directory. Names match whatever their case, as the file system's do.
	const bool full = (file_name.size() >= 2 && file_name[1] == u':') || file_name.starts_with(u"\\") || file_name.starts_with(u"/");
	const auto resolved = NativePaths(prefix).resolve(full ? file_name : u"C:\\Windows\\" + file_name);
	if (!resolved)
		return std::nullopt;
	return resolved->host;
}

bool NativeProfile::execute(std::string_view name, const std::vector<uint64_t> &args, uint64_t &result)
{
	if (name == "GetPrivateProfileSectionNamesW") {
		std::vector<std::u16string> names;
		if (const auto file_name = text(args[2]))
			if (const auto path = locate(*file_name))
				names = section_names(*path);
		result = put_list(args[0], uint32_t(args[1]), names);
		return true;
	}
	// Every key=value line of a section.
	if (name == "GetPrivateProfileSectionW") {
		std::vector<std::u16string> lines;
		if (const auto section = text(args[0]), file_name = text(args[3]); section && file_name)
			if (const auto path = locate(*file_name))
				lines = section_lines(*path, *section).value_or(std::vector<std::u16string>{});
		result = put_list(args[1], uint32_t(args[2]), lines);
		return true;
	}
	if (name == "WritePrivateProfileStringW") {
		result = 0;
		try {
			const auto section = text(args[0]), key = text(args[1]), value = text(args[2]), file_name = text(args[3]);
			if (!section || !file_name) {
				environment.set_last_error(87);
				return true;
			}
			const auto path = locate(*file_name);
			if (!path) {
				environment.set_last_error(123);
				return true;
			}
			if (!std::filesystem::exists(path->parent_path())) {
				environment.set_last_error(3);
				return true;
			}
			// A missing key removes the section; a missing value removes the key.
			if (write_value(*path, *section, key ? &*key : nullptr, value ? &*value : nullptr))
				result = 1;
			else
				environment.set_last_error(5);
		} catch (const std::runtime_error &failure) {
			const std::string_view message = failure.what();
			if (message.find("readable") == message.npos && message.find("writable") == message.npos)
				throw;
			environment.set_last_error(998);
		}
		return true;
	}
	// A structure as a value: two hexadecimal digits a byte, then two more for the sum of the bytes, which reading checks.
	if (name == "GetPrivateProfileStructW" || name == "WritePrivateProfileStructW") {
		result = 0;
		try {
			const auto section = text(args[0]), key = text(args[1]), file_name = text(args[4]);
			const auto size = uint32_t(args[3]);
			const auto path = file_name ? locate(*file_name) : std::nullopt;
			if (!section || !path || (name == "GetPrivateProfileStructW" && (!key || !args[2]))) {
				environment.set_last_error(87);
				return true;
			}
			if (name == "WritePrivateProfileStructW") {
				std::optional<std::u16string> value;
				if (args[2]) {
					const auto bytes = process.read_memory(args[2], size);
					static constexpr char16_t digits[] = u"0123456789ABCDEF";
					value.emplace();
					uint8_t sum = 0;
					for (const auto byte : bytes) {
						value->push_back(digits[byte >> 4]);
						value->push_back(digits[byte & 15]);
						sum = uint8_t(sum + byte);
					}
					value->push_back(digits[sum >> 4]);
					value->push_back(digits[sum & 15]);
				}
				if (!std::filesystem::exists(path->parent_path()))
					environment.set_last_error(3);
				else if (write_value(*path, *section, key ? &*key : nullptr, value ? &*value : nullptr))
					result = 1;
				else
					environment.set_last_error(5);
				return true;
			}
			const auto value = lookup(*path, *section, *key);
			if (!value) {
				environment.set_last_error(2);
				return true;
			}
			if (value->size() != size_t(size) * 2 + 2) {
				environment.set_last_error(24);
				return true;
			}
			const auto nibble = [](char16_t c) -> int {
				return c >= u'0' && c <= u'9' ? c - u'0' : c >= u'A' && c <= u'F' ? c - u'A' + 10 : c >= u'a' && c <= u'f' ? c - u'a' + 10 : -1;
			};
			std::vector<uint8_t> bytes;
			uint8_t sum = 0;
			for (size_t at = 0; at + 1 < value->size(); at += 2) {
				const int high = nibble((*value)[at]), low = nibble((*value)[at + 1]);
				if (high < 0 || low < 0) {
					environment.set_last_error(13);
					return true;
				}
				bytes.push_back(uint8_t(high << 4 | low));
			}
			const uint8_t stored = bytes.back();
			bytes.pop_back();
			for (const auto byte : bytes)
				sum = uint8_t(sum + byte);
			if (sum != stored) {
				environment.set_last_error(13);
				return true;
			}
			if (size)
				process.write_memory(args[2], bytes);
			result = 1;
		} catch (const std::runtime_error &failure) {
			const std::string_view message = failure.what();
			if (message.find("readable") == message.npos && message.find("writable") == message.npos)
				throw;
			environment.set_last_error(998);
			result = 0;
		}
		return true;
	}
	const bool integer = name == "GetPrivateProfileIntW";
	if (!integer && name != "GetPrivateProfileStringW")
		return false;
	result = 0;
	try {
		const auto section = text(args[0]), key = text(args[1]), file_name = text(integer ? args[3] : args[5]);
		// Without a section the names of the sections come back, and without a key the names of the section's keys.
		if (!integer && (!section || !key)) {
			std::vector<std::u16string> names;
			if (file_name)
				if (const auto path = locate(*file_name)) {
					if (!section)
						names = section_names(*path);
					else
						for (const auto &line : section_lines(*path, *section).value_or(std::vector<std::u16string>{}))
							names.push_back(trim(line.substr(0, line.find(u'='))));
				}
			result = put_list(args[3], uint32_t(args[4]), names);
			return true;
		}
		std::optional<std::u16string> value;
		if (section && key && file_name)
			if (const auto path = locate(*file_name))
				value = lookup(*path, *section, *key);
		if (integer) {
			result = uint32_t(args[2]);
			if (value) {
				// Decimal digits with an optional sign; parsing stops at the first other character.
				size_t at = 0;
				const bool negative = !value->empty() && (*value)[0] == u'-';
				if (negative)
					++at;
				int64_t number = 0;
				for (; at < value->size() && (*value)[at] >= u'0' && (*value)[at] <= u'9'; ++at)
					number = std::min<int64_t>(number * 10 + ((*value)[at] - u'0'), 0xffffffffLL);
				result = uint32_t(negative ? -number : number);
			}
		} else {
			const auto fallback = text(args[2]).value_or(std::u16string{});
			const auto &chosen = value ? *value : fallback;
			const size_t capacity = uint32_t(args[4]);
			if (capacity) {
				const auto length = std::min(chosen.size(), capacity - 1);
				std::vector<uint8_t> bytes((length + 1) * 2);
				for (size_t index = 0; index < length; ++index) {
					bytes[index * 2] = uint8_t(chosen[index]);
					bytes[index * 2 + 1] = uint8_t(chosen[index] >> 8);
				}
				process.write_memory(args[3], bytes);
				result = length;
			}
		}
	} catch (const std::runtime_error &failure) {
		const std::string_view message = failure.what();
		if (message.find("readable") == message.npos && message.find("writable") == message.npos)
			throw;
		environment.set_last_error(998);
		result = 0;
	}
	return true;
}

bool NativeProfile::dispatch(std::string_view name)
{
	const uint32_t count = name == "GetPrivateProfileSectionNamesW" ? 3 : name == "GetPrivateProfileIntW" || name == "WritePrivateProfileStringW" ||
		name == "GetPrivateProfileSectionW" ? 4 : name == "GetPrivateProfileStructW" || name == "WritePrivateProfileStructW" ? 5 : 6;
	if (name != "GetPrivateProfileSectionNamesW" && name != "GetPrivateProfileIntW" && name != "GetPrivateProfileStringW" && name != "WritePrivateProfileStringW" &&
		name != "GetPrivateProfileSectionW" && name != "GetPrivateProfileStructW" && name != "WritePrivateProfileStructW")
		return false;
	uint64_t result = 0;
	execute(name, process.api_arguments(count), result);
	process.return_api(result);
	return true;
}

void register_profile_api(ApiRegistry &registry, NativeProfile &profile)
{
	// The system's win.ini: a new installation keeps none of the values programs read there, so each one reads as the default given.
	for (const char *name : {"GetProfileIntW", "GetProfileIntA"})
		registry.add(name, 3, [](ApiCall &call) { call.ret(call.u32(2)); });
	registry.add("GetPrivateProfileSectionNamesW", 3, [&profile](ApiCall &call) {
		uint64_t result = 0;
		profile.execute("GetPrivateProfileSectionNamesW", {call[0], call[1], call[2]}, result);
		call.ret(result);
	});
	registry.add("GetPrivateProfileSectionW", 4, [&profile](ApiCall &call) {
		uint64_t result = 0;
		profile.execute("GetPrivateProfileSectionW", {call[0], call[1], call[2], call[3]}, result);
		call.ret(result);
	});
	registry.add("GetPrivateProfileIntW", 4, [&profile](ApiCall &call) {
		uint64_t result = 0;
		profile.execute("GetPrivateProfileIntW", {call[0], call[1], call[2], call[3]}, result);
		call.ret(result);
	});
	registry.add("WritePrivateProfileStringW", 4, [&profile](ApiCall &call) {
		uint64_t result = 0;
		profile.execute("WritePrivateProfileStringW", {call[0], call[1], call[2], call[3]}, result);
		call.ret(result);
	});
	for (const char *name : {"GetPrivateProfileStructW", "WritePrivateProfileStructW"})
		registry.add(name, 5, [&profile, name](ApiCall &call) {
			uint64_t result = 0;
			profile.execute(name, {call[0], call[1], call[2], call[3], call[4]}, result);
			call.ret(result);
		});
	registry.add("GetPrivateProfileStringW", 6, [&profile](ApiCall &call) {
		uint64_t result = 0;
		profile.execute("GetPrivateProfileStringW", {call[0], call[1], call[2], call[3], call[4], call[5]}, result);
		call.ret(result);
	});
}

}
