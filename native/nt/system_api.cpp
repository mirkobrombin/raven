#include "kernel_context.h"
#include "actctx.h"
#include "text.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cpuid.h>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

extern char **environ;

namespace raven {

namespace {

constexpr uint64_t current_process = UINT64_MAX, current_thread = UINT64_MAX - 1;

// Civil date from days since 1970-01-01 (proleptic Gregorian), after Howard Hinnant's public-domain algorithm.
struct Civil {
	int year;
	unsigned month, day;
};

Civil civil_from_days(int64_t days)
{
	days += 719468;
	const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
	const unsigned day_of_era = unsigned(days - era * 146097);
	const unsigned year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
	const int64_t year = int64_t(year_of_era) + era * 400;
	const unsigned day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
	const unsigned shifted = (5 * day_of_year + 2) / 153;
	const unsigned day = day_of_year - (153 * shifted + 2) / 5 + 1;
	const unsigned month = shifted < 10 ? shifted + 3 : shifted - 9;
	return {int(year + (month <= 2)), month, day};
}

int64_t days_from_civil(int year, unsigned month, unsigned day)
{
	year -= month <= 2;
	const int64_t era = (year >= 0 ? year : year - 399) / 400;
	const unsigned year_of_era = unsigned(year - era * 400);
	const unsigned day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
	const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
	return era * 146097 + int64_t(day_of_era) - 719468;
}

// SYSTEMTIME: year, month, weekday, day, hour, minute, second, milliseconds as 16-bit words.
std::vector<uint8_t> system_time(uint64_t ticks)
{
	const uint64_t seconds = ticks / 10000000ULL;
	const int64_t days = int64_t(seconds / 86400);
	const auto civil = civil_from_days(days - 134774);
	const uint16_t fields[8] = {uint16_t(civil.year), uint16_t(civil.month), uint16_t((days + 1) % 7), uint16_t(civil.day),
		uint16_t(seconds % 86400 / 3600), uint16_t(seconds % 3600 / 60), uint16_t(seconds % 60), uint16_t(ticks / 10000 % 1000)};
	std::vector<uint8_t> bytes(16);
	std::memcpy(bytes.data(), fields, 16);
	return bytes;
}

std::optional<uint64_t> ticks_from(std::span<const uint8_t> bytes)
{
	uint16_t fields[8];
	std::memcpy(fields, bytes.data(), 16);
	if (fields[1] < 1 || fields[1] > 12 || fields[3] < 1 || fields[3] > 31 || fields[4] > 23 || fields[5] > 59 || fields[6] > 59 || fields[7] > 999 || fields[0] < 1601)
		return std::nullopt;
	static constexpr int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	const bool leap = fields[0] % 4 == 0 && (fields[0] % 100 != 0 || fields[0] % 400 == 0);
	if (fields[3] > month_days[fields[1] - 1] + (fields[1] == 2 && leap))
		return std::nullopt;
	const int64_t days = days_from_civil(fields[0], fields[1], fields[3]) + 134774;
	return (uint64_t(days) * 86400ULL + fields[4] * 3600ULL + fields[5] * 60ULL + fields[6]) * 10000000ULL + fields[7] * 10000ULL;
}

uint64_t now_ticks()
{
	timespec now{};
	::clock_gettime(CLOCK_REALTIME, &now);
	return uint64_t(now.tv_sec) * 10000000ULL + uint64_t(now.tv_nsec) / 100 + filetime_epoch_offset;
}

// Offset of local time from UTC in minutes at a UTC instant, with the zone abbreviation and DST flag.
struct ZoneInfo {
	int offset_minutes;
	bool daylight;
	std::string name;
};

ZoneInfo zone_at(time_t instant)
{
	tm local{};
	::localtime_r(&instant, &local);
	return {int(local.tm_gmtoff / 60), local.tm_isdst > 0, local.tm_zone ? local.tm_zone : "UTC"};
}

std::map<std::u16string, std::pair<std::u16string, std::u16string>> default_environment(const NativePaths &paths)
{
	const auto user = paths.user_name();
	const auto profile = paths.user_profile();
	std::map<std::u16string, std::pair<std::u16string, std::u16string>> result;
	const auto add = [&](std::u16string name, std::u16string value) {
		auto key = name;
		for (auto &character : key)
			character = upper_case(character);
		result[key] = {std::move(name), std::move(value)};
	};
	char host[256]{};
	::gethostname(host, sizeof host - 1);
	std::u16string computer = from_utf8(host);
	for (auto &character : computer)
		character = upper_case(character);
	add(u"ALLUSERSPROFILE", u"C:\\ProgramData");
	add(u"APPDATA", profile + u"\\AppData\\Roaming");
	add(u"CommonProgramFiles", u"C:\\Program Files\\Common Files");
	add(u"CommonProgramFiles(x86)", u"C:\\Program Files (x86)\\Common Files");
	add(u"CommonProgramW6432", u"C:\\Program Files\\Common Files");
	add(u"COMPUTERNAME", computer.empty() ? u"RAVEN" : computer);
	add(u"ComSpec", u"C:\\Windows\\system32\\cmd.exe");
	add(u"HOMEDRIVE", u"C:");
	add(u"HOMEPATH", u"\\Users\\" + user);
	add(u"LOCALAPPDATA", profile + u"\\AppData\\Local");
	add(u"NUMBER_OF_PROCESSORS", widen_ascii(std::to_string(std::max(1U, std::min(64U, std::thread::hardware_concurrency())))));
	add(u"OS", u"Windows_NT");
	add(u"Path", u"C:\\Windows\\system32;C:\\Windows;C:\\Windows\\System32\\Wbem");
	add(u"PATHEXT", u".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC");
	add(u"PROCESSOR_ARCHITECTURE", u"AMD64");
	add(u"PROCESSOR_LEVEL", u"6");
	add(u"ProgramData", u"C:\\ProgramData");
	add(u"ProgramFiles", u"C:\\Program Files");
	add(u"ProgramFiles(x86)", u"C:\\Program Files (x86)");
	add(u"ProgramW6432", u"C:\\Program Files");
	add(u"PUBLIC", u"C:\\Users\\Public");
	add(u"SystemDrive", u"C:");
	add(u"SystemRoot", u"C:\\Windows");
	add(u"TEMP", profile + u"\\AppData\\Local\\Temp");
	add(u"TMP", profile + u"\\AppData\\Local\\Temp");
	add(u"USERDOMAIN", computer.empty() ? u"RAVEN" : computer);
	add(u"USERNAME", user);
	add(u"USERPROFILE", profile);
	add(u"windir", u"C:\\Windows");
	// A variable of the host named RAVEN_GUEST_<NAME> reaches the program as <NAME>, to configure what runs in the prefix.
	for (char **entry = environ; entry && *entry; ++entry) {
		const std::string_view text(*entry);
		constexpr std::string_view prefix = "RAVEN_GUEST_";
		const auto equals = text.find('=');
		if (text.starts_with(prefix) && equals != text.npos && equals > prefix.size())
			add(from_utf8(std::string(text.substr(prefix.size(), equals - prefix.size()))), from_utf8(std::string(text.substr(equals + 1))));
	}
	return result;
}

// The Software Protection Platform's license store: the files programs install, each named after the first license it holds and kept
// where Windows keeps them. A license is an <r:license> element whose licenseId attribute is its SLID.
struct LicenseStore {
	std::mutex lock;
	// Open sessions and the authentication data each was given.
	std::map<uint64_t, std::vector<uint8_t>> sessions;
};

using Slid = std::array<uint8_t, 16>;

std::optional<Slid> parse_slid(std::string_view text)
{
	if (text.size() == 38 && text.front() == '{' && text.back() == '}')
		text = text.substr(1, 36);
	if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-')
		return std::nullopt;
	std::string digits;
	for (const char character : text)
		if (character != '-')
			digits += character;
	uint8_t bytes[16];
	for (size_t index = 0; index < 16; ++index) {
		const auto digit = [](char character) -> int {
			if (character >= '0' && character <= '9')
				return character - '0';
			character = char(character | 0x20);
			return character >= 'a' && character <= 'f' ? character - 'a' + 10 : -1;
		};
		const int high = digit(digits[2 * index]), low = digit(digits[2 * index + 1]);
		if (high < 0 || low < 0)
			return std::nullopt;
		bytes[index] = uint8_t(high << 4 | low);
	}
	// The text spells the first three fields big-endian; the SLID keeps them as little-endian numbers.
	return Slid{bytes[3], bytes[2], bytes[1], bytes[0], bytes[5], bytes[4], bytes[7], bytes[6], bytes[8], bytes[9], bytes[10], bytes[11],
		bytes[12], bytes[13], bytes[14], bytes[15]};
}

std::string slid_text(const Slid &id)
{
	char text[39];
	std::snprintf(text, sizeof(text), "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}", id[3], id[2], id[1], id[0], id[5],
		id[4], id[7], id[6], id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
	return text;
}

// License files are UTF-8 XML; one saved as UTF-16 is read through its UTF-8 spelling.
std::string license_text(std::span<const uint8_t> bytes)
{
	if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
		std::u16string wide((bytes.size() - 2) / 2, u'\0');
		std::memcpy(wide.data(), bytes.data() + 2, wide.size() * 2);
		return to_utf8(wide);
	}
	return std::string(bytes.begin(), bytes.end());
}

struct LicenseElement {
	Slid id;
	// The element's content, between its start and end tags.
	std::string_view body;
};

std::vector<LicenseElement> license_elements(std::string_view text)
{
	std::vector<LicenseElement> result;
	for (size_t start = text.find("<r:license"); start != text.npos; start = text.find("<r:license", start + 1)) {
		const auto tag_end = text.find('>', start);
		if (tag_end == text.npos)
			break;
		const auto tag = text.substr(start, tag_end - start);
		const auto attribute = tag.find("licenseId");
		if (attribute == tag.npos)
			continue;
		size_t cursor = attribute + 9;
		while (cursor < tag.size() && (tag[cursor] == ' ' || tag[cursor] == '\t' || tag[cursor] == '\r' || tag[cursor] == '\n' || tag[cursor] == '='))
			++cursor;
		if (cursor >= tag.size() || (tag[cursor] != '"' && tag[cursor] != '\''))
			continue;
		const auto close = tag.find(tag[cursor], cursor + 1);
		const auto id = close == tag.npos ? std::nullopt : parse_slid(tag.substr(cursor + 1, close - cursor - 1));
		if (!id)
			continue;
		const auto end = text.find("</r:license>", tag_end);
		result.push_back({*id, text.substr(tag_end + 1, end == text.npos ? text.npos : end - tag_end - 1)});
	}
	return result;
}

// The text of the first element under the license whose name attribute is the field, with the XML character references resolved.
std::optional<std::string> license_field(std::string_view body, std::string_view field)
{
	const auto attribute = body.find("name=\"" + std::string(field) + "\"");
	if (attribute == body.npos)
		return std::nullopt;
	const auto start = body.find('>', attribute);
	const auto end = start == body.npos ? body.npos : body.find('<', start);
	if (end == body.npos)
		return std::nullopt;
	std::string value;
	const auto raw = body.substr(start + 1, end - start - 1);
	for (size_t index = 0; index < raw.size(); ++index) {
		constexpr std::pair<std::string_view, char> entities[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
		const auto entity = std::find_if(std::begin(entities), std::end(entities), [&](const auto &item) { return raw.substr(index).starts_with(item.first); });
		if (entity != std::end(entities)) {
			value += entity->second;
			index += entity->first.size() - 1;
		} else {
			value += raw[index];
		}
	}
	return value;
}

std::optional<std::vector<uint8_t>> read_host_file(const std::filesystem::path &path)
{
	std::ifstream stream(path, std::ios::binary);
	if (!stream)
		return std::nullopt;
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

}

uint64_t counter_ticks()
{
	timespec now{};
	::clock_gettime(CLOCK_MONOTONIC, &now);
	return uint64_t(now.tv_sec) * 10000000ULL + uint64_t(now.tv_nsec) / 100;
}

void register_system_api(ApiRegistry &registry, KernelContext &context)
{
	auto &process = context.process;
	auto &environment = context.environment;
	auto &paths = context.paths;
	auto &modules = context.modules;

	registry.add("GetSystemTimeAsFileTime", 1, [](ApiCall &call) { call.store<uint64_t>(call[0], now_ticks()); });
	registry.add("GetSystemTimePreciseAsFileTime", 1, [](ApiCall &call) { call.store<uint64_t>(call[0], now_ticks()); });
	registry.add("GetSystemTime", 1, [](ApiCall &call) { call.write(call[0], system_time(now_ticks())); });
	registry.add("GetLocalTime", 1, [](ApiCall &call) {
		const uint64_t utc = now_ticks();
		const auto zone = zone_at(time_t((utc - filetime_epoch_offset) / 10000000ULL));
		call.write(call[0], system_time(utc + int64_t(zone.offset_minutes) * 600000000LL));
	});
	registry.add("QueryPerformanceCounter", 1, [](ApiCall &call) {
		call.store<uint64_t>(call[0], counter_ticks());
		call.ret(1);
	});
	registry.add("QueryPerformanceFrequency", 1, [](ApiCall &call) {
		call.store<uint64_t>(call[0], 10000000ULL);
		call.ret(1);
	});
	registry.add("GetTickCount64", 0, [](ApiCall &call) { call.ret(counter_ticks() / 10000); });
	registry.add("GetTickCount", 0, [](ApiCall &call) { call.ret(uint32_t(counter_ticks() / 10000)); });
	// winmm timers: the host clock already has millisecond resolution, so periods are accepted and change nothing.
	registry.add("timeGetTime", 0, [](ApiCall &call) { call.ret(uint32_t(counter_ticks() / 10000)); });
	registry.add("timeBeginPeriod", 1, [](ApiCall &call) { call.ret(call.u32(0) ? 0 : 97); });
	registry.add("timeEndPeriod", 1, [](ApiCall &call) { call.ret(call.u32(0) ? 0 : 97); });
	registry.add("timeGetDevCaps", 2, [](ApiCall &call) {
		if (!call[0] || call.u32(1) < 8) {
			call.ret(97);
			return;
		}
		call.store<uint32_t>(call[0], 1);
		call.store<uint32_t>(call[0] + 4, 1000000);
		call.ret(0);
	});
	registry.add("FileTimeToSystemTime", 2, [](ApiCall &call) {
		const auto ticks = call.load<uint64_t>(call[0]);
		if (ticks > 0x7fffffffffffffffULL) {
			call.fail(87);
			return;
		}
		call.write(call[1], system_time(ticks));
		call.ret(1);
	});
	registry.add("SystemTimeToFileTime", 2, [](ApiCall &call) {
		const auto ticks = ticks_from(call.read(call[0], 16));
		if (!ticks) {
			call.fail(87);
			return;
		}
		call.store<uint64_t>(call[1], *ticks);
		call.ret(1);
	});
	// Local file times differ from UTC by the zone's offset now, whatever the date converted, as Windows does.
	registry.add("FileTimeToLocalFileTime", 2, [](ApiCall &call) {
		const int64_t offset = int64_t(zone_at(::time(nullptr)).offset_minutes) * 600000000LL;
		call.store<uint64_t>(call[1], uint64_t(int64_t(call.load<uint64_t>(call[0])) + offset));
		call.ret(1);
	});
	registry.add("LocalFileTimeToFileTime", 2, [](ApiCall &call) {
		const int64_t offset = int64_t(zone_at(::time(nullptr)).offset_minutes) * 600000000LL;
		call.store<uint64_t>(call[1], uint64_t(int64_t(call.load<uint64_t>(call[0])) - offset));
		call.ret(1);
	});
	// MS-DOS dates count years from 1980 in bits 9-15, then the month and the day; times hold hours, minutes and seconds halved.
	registry.add("DosDateTimeToFileTime", 3, [](ApiCall &call) {
		const uint16_t date = uint16_t(call.u32(0)), time = uint16_t(call.u32(1));
		const uint16_t fields[8] = {uint16_t(1980 + (date >> 9)), uint16_t((date >> 5) & 15), 0, uint16_t(date & 31), uint16_t(time >> 11),
			uint16_t((time >> 5) & 63), uint16_t((time & 31) * 2), 0};
		std::vector<uint8_t> bytes(16);
		std::memcpy(bytes.data(), fields, 16);
		const auto ticks = ticks_from(bytes);
		if (!ticks) {
			call.fail(87);
			return;
		}
		call.store<uint64_t>(call[2], *ticks);
		call.ret(1);
	});
	registry.add("FileTimeToDosDateTime", 3, [](ApiCall &call) {
		const auto ticks = call.load<uint64_t>(call[0]);
		if (ticks > 0x7fffffffffffffffULL) {
			call.fail(87);
			return;
		}
		const auto bytes = system_time(ticks);
		uint16_t fields[8];
		std::memcpy(fields, bytes.data(), 16);
		if (fields[0] < 1980 || fields[0] > 2107) {
			call.fail(87);
			return;
		}
		if (call[1])
			call.store<uint16_t>(call[1], uint16_t((fields[0] - 1980) << 9 | fields[1] << 5 | fields[3]));
		if (call[2])
			call.store<uint16_t>(call[2], uint16_t(fields[4] << 11 | fields[5] << 5 | fields[6] / 2));
		call.ret(1);
	});
	registry.add("CompareFileTime", 2, [](ApiCall &call) {
		const auto a = call.load<uint64_t>(call[0]), b = call.load<uint64_t>(call[1]);
		call.ret(uint32_t(a < b ? -1 : a > b ? 1 : 0));
	});
	// The host zone's TIME_ZONE_INFORMATION for a year (172 bytes; the dynamic form adds a 128-character key name and a flag): the
	// standard bias, the names, and the transitions, found by stepping through the year's offsets. Each transition is the recurring
	// "nth weekday of the month" rule (wYear zero, wDay 1 to 4, or 5 for the last), at the local time on the clock it leaves: daylight
	// time starts at a standard-time hour, standard time at a daylight-time hour. The result says whether the zone has daylight time.
	const auto zone_for_year = [](int year, bool dynamic, std::vector<uint8_t> &bytes) {
		tm start{};
		start.tm_year = year - 1900;
		start.tm_mday = 1;
		const time_t year_start = ::timegm(&start);
		struct Change { time_t at; ZoneInfo before, after; };
		std::vector<Change> changes;
		auto previous = zone_at(year_start);
		for (int day = 1; day <= 366; ++day) {
			const time_t at = year_start + time_t(day) * 86400;
			const auto zone = zone_at(at);
			if (zone.offset_minutes != previous.offset_minutes) {
				// The first second of the new offset, between the two days.
				time_t low = at - 86400, high = at;
				while (high - low > 1) {
					const time_t middle = low + (high - low) / 2;
					(zone_at(middle).offset_minutes == previous.offset_minutes ? low : high) = middle;
				}
				changes.push_back({high, previous, zone});
			}
			previous = zone;
		}
		const auto first = zone_at(year_start), second = zone_at(year_start + 182 * 86400);
		const int standard = std::min(first.offset_minutes, second.offset_minutes), daylight = std::max(first.offset_minutes, second.offset_minutes);
		bytes.assign(dynamic ? 432 : 172, 0);
		const auto put_rule = [&](size_t at, const Change &change) {
			const time_t local = change.at + time_t(change.before.offset_minutes) * 60;
			tm clock{};
			::gmtime_r(&local, &clock);
			const int month_days = clock.tm_mon == 1 ? ((year % 4 == 0 && year % 100) || year % 400 == 0 ? 29 : 28) :
				(clock.tm_mon == 3 || clock.tm_mon == 5 || clock.tm_mon == 8 || clock.tm_mon == 10) ? 30 : 31;
			const uint16_t occurrence = clock.tm_mday + 7 > month_days ? 5 : uint16_t((clock.tm_mday - 1) / 7 + 1);
			const uint16_t fields[8] = {0, uint16_t(clock.tm_mon + 1), uint16_t(clock.tm_wday), occurrence, uint16_t(clock.tm_hour),
				uint16_t(clock.tm_min), uint16_t(clock.tm_sec), 0};
			std::memcpy(bytes.data() + at, fields, sizeof fields);
		};
		const int32_t bias = -standard;
		std::memcpy(bytes.data(), &bias, 4);
		const auto standard_name = from_utf8(first.offset_minutes <= second.offset_minutes ? first.name : second.name).substr(0, 31);
		const auto daylight_name = from_utf8(first.offset_minutes > second.offset_minutes ? first.name : second.name).substr(0, 31);
		std::memcpy(bytes.data() + 4, standard_name.data(), standard_name.size() * 2);
		std::memcpy(bytes.data() + 88, daylight_name.data(), daylight_name.size() * 2);
		if (standard != daylight)
			for (const auto &change : changes)
				put_rule(change.after.offset_minutes == daylight ? 152 : 68, change);
		const int32_t daylight_bias = standard - daylight;
		std::memcpy(bytes.data() + 168, &daylight_bias, 4);
		if (dynamic) {
			const auto key_name = standard_name.substr(0, 127);
			std::memcpy(bytes.data() + 172, key_name.data(), key_name.size() * 2);
		}
		return standard != daylight;
	};
	const auto current_year = [] {
		const time_t now = ::time(nullptr);
		tm local{};
		::localtime_r(&now, &local);
		return local.tm_year + 1900;
	};
	const auto zone_information = [zone_for_year, current_year](ApiCall &call, bool dynamic) {
		std::vector<uint8_t> bytes;
		const bool has_daylight = zone_for_year(current_year(), dynamic, bytes);
		call.write(call[0], bytes);
		call.ret(!has_daylight ? 0 : zone_at(::time(nullptr)).daylight ? 2 : 1);
	};
	// A year's rules for the zone named, or for the current zone when none is. The host has one zone: a dynamic zone with another key
	// name is not found.
	registry.add("GetTimeZoneInformationForYear", 3, [zone_for_year, current_year](ApiCall &call) {
		if (!call[2]) {
			call.fail(87);
			return;
		}
		const int year = uint16_t(call[0]) ? int(uint16_t(call[0])) : current_year();
		std::vector<uint8_t> bytes;
		zone_for_year(year, true, bytes);
		if (call[1] && call.read(call[1] + 172, 256) != std::vector<uint8_t>(bytes.begin() + 172, bytes.begin() + 428)) {
			call.fail(2);
			return;
		}
		bytes.resize(172);
		call.write(call[2], bytes);
		call.ret(1);
	});
	registry.add("GetTimeZoneInformation", 1, [zone_information](ApiCall &call) { zone_information(call, false); });
	registry.add("GetDynamicTimeZoneInformation", 1, [zone_information](ApiCall &call) { zone_information(call, true); });
	// The reverse of the call below: a local time becomes UTC by taking the zone's offset off.
	registry.add("TzSpecificLocalTimeToSystemTime", 3, [](ApiCall &call) {
		const auto ticks = ticks_from(call.read(call[1], 16));
		if (!ticks) {
			call.fail(87);
			return;
		}
		int offset;
		if (call[0])
			offset = -call.load<int32_t>(call[0]);
		else
			offset = zone_at(time_t((*ticks - filetime_epoch_offset) / 10000000ULL)).offset_minutes;
		call.write(call[2], system_time(*ticks - int64_t(offset) * 600000000LL));
		call.ret(1);
	});
	registry.add("SystemTimeToTzSpecificLocalTime", 3, [](ApiCall &call) {
		const auto ticks = ticks_from(call.read(call[1], 16));
		if (!ticks) {
			call.fail(87);
			return;
		}
		int offset;
		if (call[0])
			offset = -call.load<int32_t>(call[0]);
		else
			offset = zone_at(time_t((*ticks - filetime_epoch_offset) / 10000000ULL)).offset_minutes;
		call.write(call[2], system_time(*ticks + int64_t(offset) * 600000000LL));
		call.ret(1);
	});

	registry.add("GetCurrentProcess", 0, [](ApiCall &call) { call.ret(current_process); });
	registry.add("GetCurrentThread", 0, [](ApiCall &call) { call.ret(current_thread); });
	// Locally unique identifiers are handed out from a counter, as Windows does within one boot.
	registry.add("AllocateLocallyUniqueId", 1, [](ApiCall &call) {
		static uint64_t next = 0x3e8;
		const uint64_t value = next++;
		call.store<uint32_t>(call[0], uint32_t(value));
		call.store<int32_t>(call[0] + 4, int32_t(value >> 32));
		call.ret(1);
	});
	registry.add("IsDebuggerPresent", 0, [](ApiCall &call) { call.ret(0); });
	registry.add("CheckRemoteDebuggerPresent", 2, [](ApiCall &call) {
		call.store<uint32_t>(call[1], 0);
		call.ret(1);
	});
	registry.add("IsProcessorFeaturePresent", 1, [](ApiCall &call) {
		unsigned a = 0, b = 0, c = 0, d = 0;
		__get_cpuid(1, &a, &b, &c, &d);
		const unsigned leaf1_ecx = c, leaf1_edx = d;
		unsigned b7 = 0;
		if (__get_cpuid_max(0, nullptr) >= 7) {
			__cpuid_count(7, 0, a, b7, c, d);
		}
		bool present;
		switch (call.u32(0)) {
		case 2: case 14: present = leaf1_ecx >> 13 & 1; break;
		case 3: present = leaf1_edx >> 23 & 1; break;
		case 6: present = leaf1_edx >> 25 & 1; break;
		case 8: case 9: case 12: case 20: case 22: case 23: present = true; break;
		case 10: present = leaf1_edx >> 26 & 1; break;
		case 13: present = leaf1_ecx & 1; break;
		case 17: present = leaf1_ecx >> 26 & 1; break;
		case 36: present = leaf1_ecx >> 9 & 1; break;
		case 37: present = leaf1_ecx >> 19 & 1; break;
		case 38: present = leaf1_ecx >> 20 & 1; break;
		case 39: present = leaf1_ecx >> 28 & 1; break;
		case 40: present = b7 >> 5 & 1; break;
		default: present = false;
		}
		call.ret(present);
	});
	const auto system_information = [](ApiCall &call) {
		// A 32-bit program sees the 36-byte layout and its own address range; GetNativeSystemInfo still names the x64 processor.
		if (call.pointer_width() == 4) {
			const uint32_t processors = std::max(1U, std::min(32U, std::thread::hardware_concurrency()));
			const uint32_t mask = processors >= 32 ? ~0U : (1U << processors) - 1;
			const uint32_t fields[9] = {9, 4096, 0x10000, 0x7ffeffff, mask, processors, 8664, 65536, 6};
			std::vector<uint8_t> bytes(36);
			std::memcpy(bytes.data(), fields, sizeof fields);
			call.write(call[0], bytes);
			return;
		}
		std::vector<uint8_t> bytes(48);
		const uint16_t architecture = 9;
		std::memcpy(bytes.data(), &architecture, 2);
		const uint32_t page = 4096, granularity = 65536, processors = std::max(1U, std::min(64U, std::thread::hardware_concurrency())), type = 8664;
		const uint64_t minimum = 0x10000, maximum = native_user_limit - 1, mask = processors >= 64 ? ~0ULL : (1ULL << processors) - 1;
		std::memcpy(bytes.data() + 4, &page, 4);
		std::memcpy(bytes.data() + 8, &minimum, 8);
		std::memcpy(bytes.data() + 16, &maximum, 8);
		std::memcpy(bytes.data() + 24, &mask, 8);
		std::memcpy(bytes.data() + 32, &processors, 4);
		std::memcpy(bytes.data() + 36, &type, 4);
		std::memcpy(bytes.data() + 40, &granularity, 4);
		const uint16_t level = 6;
		std::memcpy(bytes.data() + 44, &level, 2);
		call.write(call[0], bytes);
	};
	// The processor state the system saves for threads: the x87 and SSE registers; programs then take the paths that need no more.
	registry.add("GetEnabledXStateFeatures", 0, [](ApiCall &call) { call.ret(3); });
	// Where a CONTEXT keeps an extended state feature: of the two features enabled above, the legacy floating-point state is the
	// FXSAVE area (FltSave on x64, ExtendedRegisters on x86) and the SSE state its XMM registers. No other feature is in a context.
	registry.add("LocateXStateFeature", 3, [](ApiCall &call) {
		const bool wide = call.pointer_width() == 8;
		const uint64_t save_area = call[0] + (wide ? 0x100 : 0xcc);
		const uint32_t feature = call.u32(1);
		if (!call[0] || feature > 1) {
			call.ret(0);
			return;
		}
		if (call[2])
			call.store<uint32_t>(call[2], feature == 0 ? 512 : wide ? 256 : 128);
		call.ret(feature == 0 ? save_area : save_area + 160);
	});
	// The number of file handles a process may have is no longer limited: the request is answered as asked.
	registry.add("SetHandleCount", 1, [](ApiCall &call) { call.ret(call.u32(0)); });
	registry.add("GetSystemInfo", 1, system_information);
	// Large pages are 2 MiB on x64, as Windows reports; programs use the size only after they hold the lock-pages privilege.
	registry.add("GetLargePageMinimum", 0, [](ApiCall &call) { call.ret(0x200000); });
	registry.add("GetNativeSystemInfo", 1, system_information);
	// GetVersion and GetVersionEx report what the program's manifest declares it supports, as Windows does; RtlGetVersion tells the truth.
	registry.add("GetVersion", 0, [](ApiCall &call) {
		const auto version = activation_contexts().reported_version();
		call.ret(version.major | version.minor << 8 | version.build << 16);
	});
	registry.add("GetVersionExW", 1, [](ApiCall &call) {
		const auto size = call.load<uint32_t>(call[0]);
		if (size != 276 && size != 284) {
			call.fail(122);
			return;
		}
		std::vector<uint8_t> bytes(size);
		const auto version = activation_contexts().reported_version();
		const uint32_t fields[5] = {size, version.major, version.minor, version.build, 2};
		std::memcpy(bytes.data(), fields, 20);
		// The extended block adds the service pack, the suite mask and the product type: a workstation.
		if (size == 284)
			bytes[282] = 1;
		call.write(call[0], bytes);
		call.ret(1);
	});
	// The version as ntdll reports it, Windows 10 unless a compatibility layer says otherwise; the extended block is filled when the
	// caller sized for it.
	registry.add("RtlGetVersion", 1, [](ApiCall &call) {
		const auto size = call.load<uint32_t>(call[0]);
		if (size < 276) {
			call.ret(0xc000000dU);
			return;
		}
		std::vector<uint8_t> bytes(size >= 284 ? 284 : 276);
		const auto version = activation_contexts().system_version();
		const uint32_t fields[5] = {size, version.major, version.minor, version.build, 2};
		std::memcpy(bytes.data(), fields, 20);
		if (bytes.size() == 284)
			bytes[282] = 1;
		call.write(call[0], bytes);
		call.ret(0);
	});
	// The device family and form of a desktop PC whose maker named no form; sizes are in bytes with the terminator.
	registry.add("RtlConvertDeviceFamilyInfoToString", 4, [](ApiCall &call) {
		constexpr std::u16string_view family = u"Windows.Desktop", form = u"Unknown";
		const uint32_t family_size = uint32_t(2 * (family.size() + 1)), form_size = uint32_t(2 * (form.size() + 1));
		const uint32_t family_capacity = call.load<uint32_t>(call[0]), form_capacity = call.load<uint32_t>(call[1]);
		call.store<uint32_t>(call[0], family_size);
		call.store<uint32_t>(call[1], form_size);
		if (family_capacity < family_size || form_capacity < form_size || !call[2] || !call[3]) {
			call.ret(0xc0000023U);
			return;
		}
		call.put_wide(call[2], family, family_size / 2);
		call.put_wide(call[3], form, form_size / 2);
		call.ret(0);
	});
	// The licensing policy store holds no values: every query answers SL_E_VALUE_NOT_FOUND.
	const auto policy_value = [](ApiCall &call) { call.ret(call[0] ? 0xc004f012U : 0x80070057U); };
	registry.add("SLGetWindowsInformation", 4, policy_value);
	registry.add("SLGetWindowsInformationDWORD", 2, policy_value);
	// The Software Protection Platform keeps the licenses programs install and lists and reads them back. It holds no product keys, no
	// policies and no key to sign an answer to a program's authentication challenge, so rights are not granted and the challenge cannot
	// be verified. A session handle names nothing beyond itself.
	auto store = std::make_shared<LicenseStore>();
	constexpr uint32_t invalid_argument = 0x80070057U, value_not_found = 0xc004f012U, file_not_installed = 0xc004f011U,
		invalid_license = 0xc004f01fU;
	const auto open_session = [store](uint64_t handle) {
		std::lock_guard guard(store->lock);
		return handle && store->sessions.contains(handle);
	};
	registry.add("SLOpen", 1, [&context, store](ApiCall &call) {
		const auto handle = call[0] ? context.allocator.allocate(8, true) : 0;
		if (!handle) {
			call.ret(call[0] ? 0x8007000eU : invalid_argument);
			return;
		}
		{
			std::lock_guard guard(store->lock);
			store->sessions[handle];
		}
		call.store_pointer(call[0], handle);
		call.ret(0);
	});
	registry.add("SLClose", 1, [&context, store](ApiCall &call) {
		std::lock_guard guard(store->lock);
		if (!store->sessions.erase(call[0])) {
			call.ret(invalid_argument);
			return;
		}
		context.allocator.release(call[0]);
		call.ret(0);
	});
	// The store directory as the host sees it, created when asked to be; the service's own System32, whatever the caller's width.
	const auto store_directory = [&paths](bool create) -> std::optional<std::filesystem::path> {
		const bool was = paths.redirect(false);
		auto resolved = paths.resolve(u"C:\\Windows\\System32\\spp\\store\\2.0\\licenses");
		auto system = paths.resolve(u"C:\\Windows\\System32");
		paths.redirect(was);
		if (resolved && resolved->exists)
			return resolved->host;
		if (!create || !system || !system->exists)
			return std::nullopt;
		std::error_code error;
		const auto directory = system->host / "spp" / "store" / "2.0" / "licenses";
		std::filesystem::create_directories(directory, error);
		return error ? std::nullopt : std::optional(directory);
	};
	// SLInstallLicense keeps the file under the ID of its first license and gives that ID back as the file's.
	registry.add("SLInstallLicense", 4, [call_store = store_directory, open_session](ApiCall &call) {
		const uint32_t size = call.u32(1);
		if (!open_session(call[0]) || !size || !call[2] || !call[3]) {
			call.ret(invalid_argument);
			return;
		}
		const auto bytes = call.read(call[2], size);
		const auto licenses = license_elements(license_text(bytes));
		if (licenses.empty()) {
			call.ret(invalid_license);
			return;
		}
		const auto directory = call_store(true);
		if (!directory) {
			call.ret(0x80070003U);
			return;
		}
		const auto target = *directory / (slid_text(licenses.front().id) + ".xrm-ms");
		auto partial = target;
		partial += ".partial";
		{
			std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
			stream.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
			if (!stream.flush()) {
				call.ret(0x8007001dU);
				return;
			}
		}
		std::error_code error;
		std::filesystem::rename(partial, target, error);
		if (error) {
			std::filesystem::remove(partial, error);
			call.ret(0x8007001dU);
			return;
		}
		call.write(call[3], licenses.front().id);
		call.ret(0);
	});
	// SLGetSLIDList answers for the licenses of an installed file; other questions find nothing installed and give an empty list.
	registry.add("SLGetSLIDList", 6, [&context, call_store = store_directory, open_session](ApiCall &call) {
		constexpr uint32_t license_file = 2, license = 3, last = 8;
		const uint32_t query = call.u32(1), returned = call.u32(3);
		if (!open_session(call[0]) || query >= last || returned >= last || !call[4] || !call[5]) {
			call.ret(invalid_argument);
			return;
		}
		call.store<uint32_t>(call[4], 0);
		call.store_pointer(call[5], 0);
		if (query != license_file || returned != license) {
			call.ret(0);
			return;
		}
		if (!call[2]) {
			call.ret(invalid_argument);
			return;
		}
		const auto file = call.read(call[2], 16);
		const auto directory = call_store(false);
		const auto bytes = directory ? read_host_file(*directory / (slid_text(*reinterpret_cast<const Slid *>(file.data())) + ".xrm-ms"))
					     : std::nullopt;
		if (!bytes) {
			call.ret(file_not_installed);
			return;
		}
		const auto licenses = license_elements(license_text(*bytes));
		if (licenses.empty()) {
			call.ret(value_not_found);
			return;
		}
		const auto list = context.allocator.allocate(16 * licenses.size(), false);
		if (!list) {
			call.ret(0x8007000eU);
			return;
		}
		for (size_t index = 0; index < licenses.size(); ++index)
			call.write(list + 16 * index, licenses[index].id);
		call.store<uint32_t>(call[4], uint32_t(licenses.size()));
		call.store_pointer(call[5], list);
		call.ret(0);
	});
	// SLGetLicenseInformation reads the type, version and description an installed license states about itself, as a string.
	registry.add("SLGetLicenseInformation", 6, [&context, call_store = store_directory, open_session](ApiCall &call) {
		if (!open_session(call[0]) || !call[1] || !call[2] || !call[4] || !call[5]) {
			call.ret(invalid_argument);
			return;
		}
		if (call[3])
			call.store<uint32_t>(call[3], 0);
		call.store<uint32_t>(call[4], 0);
		call.store_pointer(call[5], 0);
		const auto name = call.wide(call[2]);
		const std::pair<std::u16string_view, std::vector<std::string_view>> fields[] = {
			{u"LicenseType", {"licenseType"}}, {u"Version", {"licenseVersion"}}, {u"Description", {"description", "productDescription"}}};
		const auto field = std::find_if(std::begin(fields), std::end(fields), [&](const auto &item) { return item.first == name; });
		const auto directory = call_store(false);
		if (field == std::end(fields) || !directory) {
			call.ret(value_not_found);
			return;
		}
		Slid id;
		std::memcpy(id.data(), call.read(call[1], 16).data(), 16);
		// The file named after the license holds it when it came first in what was installed; any other file may hold it too.
		std::vector<std::filesystem::path> files{*directory / (slid_text(id) + ".xrm-ms")};
		std::error_code error;
		for (const auto &entry : std::filesystem::directory_iterator(*directory, error))
			if (entry.path().extension() == ".xrm-ms" && entry.path() != files.front())
				files.push_back(entry.path());
		for (const auto &file : files) {
			const auto bytes = read_host_file(file);
			if (!bytes)
				continue;
			const auto text = license_text(*bytes);
			for (const auto &license : license_elements(text)) {
				if (license.id != id)
					continue;
				std::optional<std::string> value;
				for (const auto spelling : field->second)
					if (!value)
						value = license_field(license.body, spelling);
				if (!value) {
					call.ret(value_not_found);
					return;
				}
				const auto wide = from_utf8(*value);
				const auto block = context.allocator.allocate(2 * (wide.size() + 1), true);
				if (!block) {
					call.ret(0x8007000eU);
					return;
				}
				call.put_wide(block, wide, wide.size() + 1);
				if (call[3])
					call.store<uint32_t>(call[3], 1);
				call.store<uint32_t>(call[4], uint32_t(2 * (wide.size() + 1)));
				call.store_pointer(call[5], block);
				call.ret(0);
				return;
			}
		}
		call.ret(value_not_found);
	});
	// Empty lists: SLGetLicensingStatusInformation and SLGetInstalledProductKeyIds give a count of zero and no array.
	const auto empty_list = [](size_t count, size_t array) {
		return [count, array](ApiCall &call) {
			if (!call[0] || !call[count] || !call[array]) {
				call.ret(invalid_argument);
				return;
			}
			call.store<uint32_t>(call[count], 0);
			call.store_pointer(call[array], 0);
			call.ret(0);
		};
	};
	registry.add("SLGetLicensingStatusInformation", 6, empty_list(4, 5));
	registry.add("SLGetInstalledProductKeyIds", 4, empty_list(2, 3));
	const auto answer = [](uint32_t result) { return [result](ApiCall &call) { call.ret(call[0] ? result : invalid_argument); }; };
	const std::pair<const char *, int> lookups[] = {{"SLGetApplicationInformation", 6}, {"SLGetProductSkuInformation", 6},
		{"SLGetPKeyInformation", 6}, {"SLGetServiceInformation", 5}, {"SLGetPolicyInformation", 5}, {"SLGetPolicyInformationDWORD", 3},
		{"SLGetApplicationPolicy", 5}};
	for (const auto &[name, words] : lookups)
		registry.add(name, words, answer(value_not_found));
	// No policies are installed for any application: the handle it would load is cleared.
	registry.add("SLLoadApplicationPolicies", 4, [](ApiCall &call) {
		if (!call[0] || !call[3]) {
			call.ret(invalid_argument);
			return;
		}
		call.store_pointer(call[3], 0);
		call.ret(0xc004f072U);
	});
	registry.add("SLConsumeRight", 5, answer(0xc004f013U));
	registry.add("SLGetLicense", 4, answer(file_not_installed));
	registry.add("SLGetLicenseFileId", 4, answer(file_not_installed));
	registry.add("SLUninstallLicense", 2, answer(file_not_installed));
	registry.add("SLUninstallProofOfPurchase", 2, answer(value_not_found));
	// No product key configuration is installed to tell a key's ID, and no SKU to identify offline, confirm or rearm.
	registry.add("SLGetPKeyId", 6, answer(0xc004f016U));
	registry.add("SLGenerateOfflineInstallationId", 3, answer(value_not_found));
	registry.add("SLDepositOfflineConfirmationId", 4, answer(0xc004f016U));
	registry.add("SLReArm", 4, answer(0xc004f016U));
	// The session keeps the challenge it is given; an empty one clears it. Without the platform's signing key no result can be built
	// for it, which SLGetAuthenticationResult reports as SL_E_AUTHN_CANT_VERIFY, or SL_E_AUTHN_CHALLENGE_NOT_SET before a challenge.
	registry.add("SLSetAuthenticationData", 3, [store](ApiCall &call) {
		const uint32_t size = call.u32(1);
		std::lock_guard guard(store->lock);
		const auto session = store->sessions.find(call[0]);
		if (!call[0] || session == store->sessions.end() || (size && !call[2])) {
			call.ret(invalid_argument);
			return;
		}
		session->second = size ? call.read(call[2], size) : std::vector<uint8_t>();
		call.ret(0);
	});
	registry.add("SLGetAuthenticationResult", 3, [store](ApiCall &call) {
		std::lock_guard guard(store->lock);
		const auto session = store->sessions.find(call[0]);
		if (!call[0] || session == store->sessions.end() || !call[1] || !call[2]) {
			call.ret(invalid_argument);
			return;
		}
		call.store<uint32_t>(call[1], 0);
		call.store_pointer(call[2], 0);
		call.ret(session->second.empty() ? 0xc004f079U : 0xc004f07aU);
	});
	registry.add("SLInstallProofOfPurchase", 6, answer(0xc004f016U));
	registry.add("SLPersistApplicationPolicies", 3, answer(0));
	registry.add("SLUnloadApplicationPolicies", 2, answer(0));
	// Undocumented: present because programs look for them, and they refuse every request.
	registry.add("SLDepositMigrationBlob", 4, [](ApiCall &call) { call.ret(0xc004f016U); });
	registry.add("SLRegisterPlugin", 4, [](ApiCall &call) { call.ret(0xc004f016U); });
	registry.add("SLPersistRTSPayloadOverride", 4, [](ApiCall &call) { call.ret(0xc004f016U); });
	// All logical processors form one group of at most 64: the masks of the process and the system are the same.
	const auto processor_mask = [] {
		const long count = std::max<long>(::sysconf(_SC_NPROCESSORS_ONLN), 1);
		return count >= 64 ? ~uint64_t(0) : (uint64_t(1) << count) - 1;
	};
	// The processor the calling thread runs on at this moment: the host processor it runs on, in group 0, within the processors the process may use.
	const auto current_processor = [] {
		const int processor = ::sched_getcpu();
		return uint32_t(processor < 0 ? 0 : processor % std::min<long>(std::max<long>(::sysconf(_SC_NPROCESSORS_ONLN), 1), 64));
	};
	for (const char *name : {"GetCurrentProcessorNumber", "NtGetCurrentProcessorNumber", "RtlGetCurrentProcessorNumber"})
		registry.add(name, 0, [current_processor](ApiCall &call) { call.ret(current_processor()); });
	// PROCESSOR_NUMBER: the group, the number within it, a reserved byte.
	for (const char *name : {"GetCurrentProcessorNumberEx", "NtGetCurrentProcessorNumberEx", "RtlGetCurrentProcessorNumberEx"})
		registry.add(name, 1, [current_processor](ApiCall &call) {
			if (call[0])
				call.write(call[0], std::vector<uint8_t>{0, 0, uint8_t(current_processor()), 0});
		});
	registry.add("GetProcessAffinityMask", 3, [processor_mask](ApiCall &call) {
		if (!call[1] || !call[2]) {
			call.fail(87);
			return;
		}
		call.store_pointer(call[1], processor_mask());
		call.store_pointer(call[2], processor_mask());
		call.ret(1);
	});
	// Affinity is not enforced: the call reports the mask the thread had, which is every processor.
	registry.add("SetProcessAffinityMask", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("SetProcessShutdownParameters", 2, [](ApiCall &call) { call.ret(1); });
	// Processor topology: one package, one core per logical processor, no cache or NUMA records beyond the single node.
	registry.add("GetLogicalProcessorInformationEx", 3, [processor_mask](ApiCall &call) {
		constexpr uint32_t core = 0, node = 1, package = 3, group = 4, all = 0xffff;
		const uint32_t relationship = call.u32(0);
		if (relationship != core && relationship != node && relationship != package && relationship != group && relationship != all) {
			call.fail(87);
			return;
		}
		if (!call[2]) {
			call.fail(87);
			return;
		}
		const size_t width = call.pointer_width();
		// Each record: relationship, size, then flags, efficiency class, 20 reserved bytes, the group count and one group affinity.
		const size_t record = (32 + width + 8 + width - 1) / width * width;
		const long count = std::max<long>(::sysconf(_SC_NPROCESSORS_ONLN), 1);
		const auto mask = processor_mask();
		std::vector<uint8_t> bytes;
		const auto add = [&](uint32_t kind, uint64_t group_mask) {
			std::vector<uint8_t> entry(record);
			std::memcpy(entry.data(), &kind, 4);
			const uint32_t size = uint32_t(record);
			std::memcpy(entry.data() + 4, &size, 4);
			const uint16_t groups = 1;
			std::memcpy(entry.data() + 30, &groups, 2);
			std::memcpy(entry.data() + 32, &group_mask, width);
			bytes.insert(bytes.end(), entry.begin(), entry.end());
		};
		if (relationship == core || relationship == all)
			for (long index = 0; index < std::min<long>(count, 64); ++index)
				add(core, uint64_t(1) << index);
		if (relationship == node || relationship == all)
			add(node, mask);
		if (relationship == package || relationship == all)
			add(package, mask);
		if (relationship == group || relationship == all) {
			std::vector<uint8_t> entry(72 + width);
			const uint32_t size = uint32_t(entry.size());
			std::memcpy(entry.data(), &group, 4);
			std::memcpy(entry.data() + 4, &size, 4);
			const uint16_t groups = 1;
			std::memcpy(entry.data() + 8, &groups, 2);
			std::memcpy(entry.data() + 10, &groups, 2);
			entry[32] = entry[33] = uint8_t(std::min<long>(count, 64));
			std::memcpy(entry.data() + 72, &mask, width);
			bytes.insert(bytes.end(), entry.begin(), entry.end());
		}
		const uint32_t capacity = call[1] ? call.load<uint32_t>(call[2]) : 0;
		call.store<uint32_t>(call[2], uint32_t(bytes.size()));
		if (!call[1] || capacity < bytes.size()) {
			call.fail(122);
			return;
		}
		call.write(call[1], bytes);
		call.ret(1);
	});
	// The same topology as fixed-size records: a processor mask, the relationship, then the 16-byte union (core flags, node number).
	registry.add("GetLogicalProcessorInformation", 2, [processor_mask](ApiCall &call) {
		if (!call[1]) {
			call.fail(87);
			return;
		}
		const size_t width = call.pointer_width(), record = width == 4 ? 24 : 32;
		const long count = std::max<long>(::sysconf(_SC_NPROCESSORS_ONLN), 1);
		std::vector<uint8_t> bytes;
		const auto add = [&](uint32_t kind, uint64_t mask) {
			std::vector<uint8_t> entry(record);
			std::memcpy(entry.data(), &mask, width);
			std::memcpy(entry.data() + width, &kind, 4);
			bytes.insert(bytes.end(), entry.begin(), entry.end());
		};
		for (long index = 0; index < std::min<long>(count, long(width * 8)); ++index)
			add(0, uint64_t(1) << index);
		add(1, processor_mask());
		add(3, processor_mask());
		const uint32_t capacity = call.load<uint32_t>(call[1]);
		call.store<uint32_t>(call[1], uint32_t(bytes.size()));
		if (!call[0] || capacity < bytes.size()) {
			call.fail(122);
			return;
		}
		call.write(call[0], bytes);
		call.ret(1);
	});
	// The interface language: one preferred language, English (United States), as a list of names that ends with an empty one.
	const auto preferred_languages = [](ApiCall &call) {
		constexpr uint32_t by_name = 8;
		const std::u16string text = call.u32(0) & by_name ? u"en-US" : u"0409";
		std::u16string list = text;
		list.push_back(u'\0');
		list.push_back(u'\0');
		if (!call[1] || !call[3]) {
			call.fail(87);
			return;
		}
		const uint32_t capacity = call.load<uint32_t>(call[3]);
		call.store<uint32_t>(call[3], uint32_t(list.size()));
		call.store<uint32_t>(call[1], 1);
		if (call[2]) {
			if (capacity < list.size()) {
				call.fail(122);
				return;
			}
			call.write(call[2], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(list.data()), list.size() * 2));
		}
		call.ret(1);
	};
	registry.add("GetThreadPreferredUILanguages", 4, preferred_languages);
	registry.add("GetUserPreferredUILanguages", 4, preferred_languages);
	registry.add("GetSystemPreferredUILanguages", 4, preferred_languages);
	registry.add("GetProcessPreferredUILanguages", 4, preferred_languages);
	// Power: always on mains power, no battery, nothing to suspend.
	registry.add("GetSystemPowerStatus", 1, [](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		// SYSTEM_POWER_STATUS: line status online, battery flag "no battery", unknown life percent and times.
		std::vector<uint8_t> status(12);
		status[0] = 1;
		status[1] = 128;
		status[2] = 255;
		const uint32_t unknown = UINT32_MAX;
		std::memcpy(status.data() + 4, &unknown, 4);
		std::memcpy(status.data() + 8, &unknown, 4);
		call.write(call[0], status);
		call.ret(1);
	});
	// What a thread asks of power management, which has nothing to keep awake here: the previous state comes back, and a continuous request
	// stays until the next one replaces it.
	registry.add("SetThreadExecutionState", 1, [state = std::make_shared<uint32_t>(0x80000000U)](ApiCall &call) {
		const uint32_t flags = call.u32(0), previous = *state;
		if (flags & 0x80000000U)
			*state = flags;
		call.ret(previous);
	});
	registry.add("RegisterSuspendResumeNotification", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("UnregisterSuspendResumeNotification", 1, [](ApiCall &call) { call.ret(1); });
	// Giving pages back changes nothing the program can see here.
	registry.add("DiscardVirtualMemory", 2, [](ApiCall &call) { call.ret(0); });
	// The display is 96 dpi and every process counts as aware of it (IsProcessDPIAware): the awareness a process asks for is kept and
	// read back, PROCESS_SYSTEM_DPI_AWARE until it asks.
	auto dpi_awareness = std::make_shared<uint32_t>(1);
	registry.add("SetProcessDpiAwareness", 1, [dpi_awareness](ApiCall &call) {
		if (call.u32(0) > 2) {
			call.ret(0x80070057U);
			return;
		}
		*dpi_awareness = call.u32(0);
		call.ret(0);
	});
	registry.add("GetProcessDpiAwareness", 2, [dpi_awareness](ApiCall &call) {
		if (!call[1]) {
			call.ret(0x80070057U);
			return;
		}
		call.store<uint32_t>(call[1], *dpi_awareness);
		call.ret(0);
	});
	registry.add("GetVersionExA", 1, [](ApiCall &call) {
		const auto size = call.load<uint32_t>(call[0]);
		if (size != 148 && size != 156) {
			call.fail(122);
			return;
		}
		std::vector<uint8_t> bytes(size);
		const auto version = activation_contexts().reported_version();
		const uint32_t fields[5] = {size, version.major, version.minor, version.build, 2};
		std::memcpy(bytes.data(), fields, 20);
		if (size == 156)
			bytes[154] = 1;
		call.write(call[0], bytes);
		call.ret(1);
	});
	// Windows 10 Pro (PRODUCT_PROFESSIONAL), whatever version the program asks about.
	// A program in the prefix runs outside any app package: the package queries answer APPMODEL_ERROR_NO_PACKAGE, with no length.
	for (const char *name : {"GetCurrentPackageFullName", "GetCurrentPackageFamilyName", "GetCurrentPackageId", "GetCurrentPackagePath",
			"GetCurrentApplicationUserModelId"})
		registry.add(name, 2, [](ApiCall &call) {
			if (call[0])
				call.store<uint32_t>(call[0], 0);
			call.ret(15700);
		});
	// No app package is installed: a family has no packages, and the counts say so.
	registry.add("GetPackagesByPackageFamily", 5, [](ApiCall &call) {
		if (!call[0] || !call[1] || !call[3]) {
			call.ret(87);
			return;
		}
		call.store<uint32_t>(call[1], 0);
		call.store<uint32_t>(call[3], 0);
		call.ret(0);
	});
	registry.add("GetProductInfo", 5, [](ApiCall &call) {
		if (!call[4]) {
			call.fail(87);
			return;
		}
		call.store<uint32_t>(call[4], 0x30);
		call.ret(1);
	});
	// One display scale: every monitor reports the standard 96 dots per inch.
	registry.add("GetDpiForMonitor", 4, [](ApiCall &call) {
		if (!call[2] || !call[3]) {
			call.ret(0x80070057U);
			return;
		}
		call.store<uint32_t>(call[2], 96);
		call.store<uint32_t>(call[3], 96);
		call.ret(0);
	});
	registry.add("VerSetConditionMask", 3, [](ApiCall &call) {
		uint64_t mask = call[0];
		const uint32_t types = call.u32(1);
		const uint64_t condition = call[2] & 7;
		// Each version field owns three bits of the mask; the field index is the position of its flag bit.
		for (unsigned bit = 0; bit < 8; ++bit)
			if (types & (1U << bit))
				mask = (mask & ~(7ULL << (bit * 3))) | (condition << (bit * 3));
		call.ret(mask);
	});
	const auto verify_version = [](ApiCall &call, bool native) -> bool {
		const auto info = call.read(call[0], 28);
		uint32_t requested[5];
		std::memcpy(requested, info.data(), 20);
		const uint32_t types = call.u32(1);
		const uint64_t mask = call[2];
		// The version the program's manifest makes Windows report, with no service pack; the native call compares the real one.
		const auto version = native ? activation_contexts().system_version() : activation_contexts().reported_version();
		const auto compare = [&](unsigned bit, uint64_t have, uint64_t want) {
			switch ((mask >> (bit * 3)) & 7) {
			case 1: return have == want;
			case 2: return have > want;
			case 3: return have >= want;
			case 4: return have < want;
			case 5: return have <= want;
			default: return have == want;
			}
		};
		bool good = true;
		const bool hierarchical = (types & 3) == 3 && ((mask & 7) == 3 || ((mask >> 3) & 7) == 3);
		if (hierarchical) {
			// Major, minor and service pack compare as one ordered tuple when they ask for "at least".
			const uint64_t have = uint64_t(version.major) << 32 | version.minor, want = uint64_t(requested[1]) << 32 | requested[2];
			good = have >= want;
		} else {
			if (types & 2) good = good && compare(1, version.major, requested[1]);
			if (types & 1) good = good && compare(0, version.minor, requested[2]);
		}
		if (types & 4) good = good && compare(2, version.build, requested[3]);
		if (types & 8) good = good && compare(3, 2, requested[4]);
		return good;
	};
	registry.add("VerifyVersionInfoW", 3, [verify_version](ApiCall &call) {
		if (!verify_version(call, false)) {
			call.fail(1150);
			return;
		}
		call.ret(1);
	});
	// The native form answers STATUS_REVISION_MISMATCH where the Win32 one fails with ERROR_OLD_WIN_VERSION.
	registry.add("RtlVerifyVersionInfo", 3, [verify_version](ApiCall &call) { call.ret(verify_version(call, true) ? 0 : 0xc0000059U); });
	// kernel32's memory exports: moving, filling and clearing a block of the program's memory.
	registry.add("RtlMoveMemory", 3, [](ApiCall &call) {
		if (call[2])
			call.write(call[0], call.read(call[1], size_t(call[2])));
	});
	registry.add("RtlFillMemory", 3, [](ApiCall &call) {
		if (call[1])
			call.write(call[0], std::vector<uint8_t>(size_t(call[1]), uint8_t(call[2])));
	});
	registry.add("RtlZeroMemory", 2, [](ApiCall &call) {
		if (call[1])
			call.write(call[0], std::vector<uint8_t>(size_t(call[1]), 0));
	});
	// ntdll's C runtime copies and fills, which some programs import from it rather than from a C runtime; they answer the destination.
	const auto copy = [](ApiCall &call) {
		if (call[2])
			call.write(call[0], call.read(call[1], size_t(call[2])));
		call.ret(call[0]);
	};
	registry.add("memcpy", 3, copy);
	registry.add("memmove", 3, copy);
	registry.add("memset", 3, [](ApiCall &call) {
		if (call[2])
			call.write(call[0], std::vector<uint8_t>(size_t(call[2]), uint8_t(call[1])));
		call.ret(call[0]);
	});
	// ntdll's C runtime string lengths and comparisons, with the bytes compared as unsigned characters.
	registry.add("strlen", 1, [](ApiCall &call) { call.ret(call.narrow(call[0], SIZE_MAX - 1).size()); });
	// A comparison reads no byte past the first difference, the terminator or the limit, so a bounded one may run up to the end of a
	// buffer that has no terminator.
	const auto compare_text = [](ApiCall &call, size_t limit) {
		for (size_t at = 0; at < limit; ++at) {
			const uint8_t left = call.load<uint8_t>(call[0] + at), right = call.load<uint8_t>(call[1] + at);
			if (left != right || !left) {
				call.ret(uint64_t(int64_t(left < right ? -1 : left > right ? 1 : 0)));
				return;
			}
		}
		call.ret(0);
	};
	registry.add("strcmp", 2, [compare_text](ApiCall &call) { compare_text(call, SIZE_MAX - 1); });
	registry.add("strncmp", 3, [compare_text](ApiCall &call) {
		if (!call[2]) {
			call.ret(0);
			return;
		}
		compare_text(call, size_t(call[2]));
	});
	registry.add("memcmp", 3, [](ApiCall &call) {
		if (!call[2]) {
			call.ret(0);
			return;
		}
		const auto left = call.read(call[0], size_t(call[2])), right = call.read(call[1], size_t(call[2]));
		const int order = std::memcmp(left.data(), right.data(), left.size());
		call.ret(uint64_t(int64_t(order < 0 ? -1 : order > 0 ? 1 : 0)));
	});
	registry.add("MulDiv", 3, [](ApiCall &call) {
		const int64_t a = call.i32(0), b = call.i32(1), c = call.i32(2);
		if (!c) {
			call.ret(uint32_t(-1));
			return;
		}
		const int64_t product = a * b, half = (c < 0 ? -c : c) / 2;
		const bool negative = (product < 0) != (c < 0);
		const int64_t magnitude = ((product < 0 ? -product : product) + half) / (c < 0 ? -c : c);
		const int64_t value = negative ? -magnitude : magnitude;
		call.ret(value > INT32_MAX || value < INT32_MIN ? uint32_t(-1) : uint32_t(int32_t(value)));
	});
	registry.add("EncodePointer", 1, [](ApiCall &call) { call.ret(call[0]); });
	registry.add("DecodePointer", 1, [](ApiCall &call) { call.ret(call[0]); });
	registry.add("EncodeSystemPointer", 1, [](ApiCall &call) { call.ret(call[0]); });
	registry.add("DecodeSystemPointer", 1, [](ApiCall &call) { call.ret(call[0]); });
	// Singly linked list: the head pointer sits in the first word and the depth in the low 16 bits of the next one, under a sequence that
	// every change advances. Both words are as wide as the program's pointers, so a 32-bit header is the 8-byte form.
	registry.add("InitializeSListHead", 1, [](ApiCall &call) { call.write(call[0], std::vector<uint8_t>(call.pointer_width() * 2)); });
	const auto count = [](ApiCall &call) {
		const auto width = call.pointer_width();
		return width == 4 ? uint64_t(call.load<uint32_t>(call[0] + 4)) : call.load<uint64_t>(call[0] + 8);
	};
	const auto update = [](ApiCall &call, uint64_t head, uint64_t value) {
		call.store_pointer(call[0], head);
		if (call.pointer_width() == 4)
			call.store<uint32_t>(call[0] + 4, uint32_t(value));
		else
			call.store<uint64_t>(call[0] + 8, value);
	};
	registry.add("InterlockedPushEntrySList", 2, [count, update](ApiCall &call) {
		const auto head = call.load_pointer(call[0]);
		const auto depth = count(call);
		call.store_pointer(call[1], head);
		update(call, call[1], (depth & ~uint64_t(0xffff)) + 0x10000 + ((depth & 0xffff) + 1) % 0xffff);
		call.ret(head);
	});
	registry.add("InterlockedPopEntrySList", 1, [count, update](ApiCall &call) {
		const auto head = call.load_pointer(call[0]);
		if (!head) {
			call.ret(0);
			return;
		}
		const auto depth = count(call);
		update(call, call.load_pointer(head), (depth & ~uint64_t(0xffff)) + 0x10000 + (((depth & 0xffff) + 0xfffe) % 0xffff));
		call.ret(head);
	});
	registry.add("InterlockedFlushSList", 1, [count, update](ApiCall &call) {
		const auto head = call.load_pointer(call[0]);
		update(call, 0, (count(call) & ~uint64_t(0xffff)) + 0x10000);
		call.ret(head);
	});
	registry.add("QueryDepthSList", 1, [count](ApiCall &call) { call.ret(count(call) & 0xffff); });
	// ntdll's names for the same lists.
	for (const auto &[native, win32] : {std::pair{"RtlInitializeSListHead", "InitializeSListHead"},
			{"RtlInterlockedPushEntrySList", "InterlockedPushEntrySList"}, {"RtlInterlockedPopEntrySList", "InterlockedPopEntrySList"},
			{"RtlInterlockedFlushSList", "InterlockedFlushSList"}, {"RtlQueryDepthSList", "QueryDepthSList"}})
		if (const auto entry = registry.find(win32); entry && !registry.has(native))
			registry.add(native, entry->first, entry->second);
	// RTL_BITMAP: the number of bits, then a pointer of the program's width to the words that hold them, lowest bit first.
	const auto bitmap = [](ApiCall &call) {
		const uint32_t size = call.load<uint32_t>(call[0]);
		const uint64_t buffer = call.load_pointer(call[0] + call.pointer_width());
		return std::pair{size, buffer};
	};
	const auto get_bit = [](ApiCall &call, uint64_t buffer, uint32_t index) {
		return (call.load<uint32_t>(buffer + (index / 32) * 4) >> (index % 32)) & 1;
	};
	const auto put_bits = [](ApiCall &call, uint64_t buffer, uint32_t start, uint32_t count, bool value) {
		for (uint32_t index = start; index < start + count; ++index) {
			const uint64_t word = buffer + (index / 32) * 4;
			const uint32_t bits = call.load<uint32_t>(word), mask = 1U << (index % 32);
			call.store<uint32_t>(word, value ? bits | mask : bits & ~mask);
		}
	};
	registry.add("RtlInitializeBitMap", 3, [](ApiCall &call) {
		call.store<uint32_t>(call[0], call.u32(2));
		call.store_pointer(call[0] + call.pointer_width(), call[1]);
	});
	registry.add("RtlSetBits", 3, [bitmap, put_bits](ApiCall &call) {
		const auto [size, buffer] = bitmap(call);
		if (uint64_t(call.u32(1)) + call.u32(2) <= size)
			put_bits(call, buffer, call.u32(1), call.u32(2), true);
	});
	registry.add("RtlClearBits", 3, [bitmap, put_bits](ApiCall &call) {
		const auto [size, buffer] = bitmap(call);
		if (uint64_t(call.u32(1)) + call.u32(2) <= size)
			put_bits(call, buffer, call.u32(1), call.u32(2), false);
	});
	registry.add("RtlSetAllBits", 1, [bitmap, put_bits](ApiCall &call) {
		const auto [size, buffer] = bitmap(call);
		put_bits(call, buffer, 0, size, true);
	});
	registry.add("RtlClearAllBits", 1, [bitmap, put_bits](ApiCall &call) {
		const auto [size, buffer] = bitmap(call);
		put_bits(call, buffer, 0, size, false);
	});
	const auto run_of = [get_bit](ApiCall &call, uint64_t buffer, uint32_t start, uint32_t count, uint32_t value) {
		for (uint32_t index = start; index < start + count; ++index)
			if (get_bit(call, buffer, index) != value)
				return false;
		return true;
	};
	registry.add("RtlAreBitsClear", 3, [bitmap, run_of](ApiCall &call) {
		const auto [size, buffer] = bitmap(call);
		call.ret(uint64_t(call.u32(1)) + call.u32(2) <= size && run_of(call, buffer, call.u32(1), call.u32(2), 0));
	});
	registry.add("RtlAreBitsSet", 3, [bitmap, run_of](ApiCall &call) {
		const auto [size, buffer] = bitmap(call);
		call.ret(uint64_t(call.u32(1)) + call.u32(2) <= size && run_of(call, buffer, call.u32(1), call.u32(2), 1));
	});
	// The first run of clear bits of the length asked for, looked for from the hint on and then from the start; ~0 when there is none.
	const auto find_clear = [bitmap, run_of](ApiCall &call) -> uint32_t {
		const auto [size, buffer] = bitmap(call);
		const uint32_t count = call.u32(1), hint = call.u32(2) < size ? call.u32(2) : 0;
		if (!count || count > size)
			return count ? UINT32_MAX : hint;
		for (const auto [from, to] : {std::pair{hint, size}, std::pair{0U, hint + count < size ? hint + count : size}})
			for (uint32_t start = from; start + count <= to; ++start)
				if (run_of(call, buffer, start, count, 0))
					return start;
		return UINT32_MAX;
	};
	registry.add("RtlFindClearBits", 3, [find_clear](ApiCall &call) { call.ret(find_clear(call)); });
	registry.add("RtlFindClearBitsAndSet", 3, [bitmap, find_clear, put_bits](ApiCall &call) {
		const uint32_t found = find_clear(call);
		if (found != UINT32_MAX && call.u32(1))
			put_bits(call, bitmap(call).second, found, call.u32(1), true);
		call.ret(found);
	});
	registry.add("RtlNumberOfSetBits", 1, [bitmap, get_bit](ApiCall &call) {
		const auto [size, buffer] = bitmap(call);
		uint32_t count = 0;
		for (uint32_t index = 0; index < size; ++index)
			count += get_bit(call, buffer, index);
		call.ret(count);
	});
	registry.add("SetUnhandledExceptionFilter", 1, [&context](ApiCall &call) {
		const auto previous = context.unhandled_filter;
		context.unhandled_filter = call[0];
		call.ret(previous);
	});
	registry.add("SetErrorMode", 1, [](ApiCall &call) { call.ret(0); });
	// Each thread keeps the error mode it sets, SEM_FAILCRITICALERRORS, SEM_NOGPFAULTERRORBOX and SEM_NOOPENFILEERRORBOX, and starts with none.
	const auto thread_modes = std::make_shared<std::map<uint64_t, uint32_t>>();
	registry.add("SetThreadErrorMode", 2, [thread_modes](ApiCall &call) {
		if (call.u32(0) & ~0x8003U) {
			call.fail(87);
			return;
		}
		auto &mode = (*thread_modes)[call.environment.teb()];
		if (call[1])
			call.store<uint32_t>(call[1], mode);
		mode = call.u32(0);
		call.ret(1);
	});
	registry.add("GetThreadErrorMode", 0, [thread_modes](ApiCall &call) {
		const auto found = thread_modes->find(call.environment.teb());
		call.ret(found == thread_modes->end() ? 0 : found->second);
	});
	registry.add("GetApplicationRestartSettings", 4, [](ApiCall &call) { call.fail(0x80070490U, 0x80070490U); });
	registry.add("RegisterApplicationRestart", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("UnregisterApplicationRestart", 0, [](ApiCall &call) { call.ret(0); });
	// Recovery callbacks run only when Windows Error Reporting catches a hang or crash, which never happens here.
	registry.add("RegisterApplicationRecoveryCallback", 4, [](ApiCall &call) { call.ret(call[0] ? 0 : 0x80070057U); });
	registry.add("UnregisterApplicationRecoveryCallback", 0, [](ApiCall &call) { call.ret(0); });
	// Out-of-box setup finished long ago.
	registry.add("OOBEComplete", 1, [](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		call.store<uint32_t>(call[0], 1);
		call.ret(1);
	});

	registry.add("GetModuleFileNameW", 3, [&](ApiCall &call) {
		std::u16string text;
		const auto base = call[0];
		if (!base || base == context.entry.loaded.base()) {
			text = context.launch.image_path;
		} else {
			const auto found = std::find_if(modules.all().begin(), modules.all().end(), [&](const auto &item) { return item.second->loaded.base() == base; });
			if (found == modules.all().end()) {
				call.fail(6);
				return;
			}
			text = found->second->runtime_api ? u"C:\\Windows\\System32\\" + widen_ascii(found->first) :
				!found->second->file.empty() ? paths.dos(found->second->file) : paths.library_location(context.launch.image_path, widen_ascii(found->first), context.entry.image.is_64);
		}
		const size_t capacity = call.u32(2);
		if (!capacity) {
			call.fail(122);
			return;
		}
		if (text.size() >= capacity) {
			call.put_wide(call[1], text.substr(0, capacity - 1), capacity);
			call.fail(122, capacity);
			return;
		}
		call.put_wide(call[1], text, capacity);
		call.set_last_error(0);
		call.ret(text.size());
	});
	registry.add("GetModuleHandleExW", 3, [&](ApiCall &call) {
		const uint32_t flags = call.u32(0);
		uint64_t handle = 0;
		if (flags & 4) {
			for (const auto &[name, module] : modules.all())
				if (call[1] >= module->loaded.base() && call[1] < module->loaded.base() + module->loaded.data().size())
					handle = module->loaded.base();
		} else if (!call[1]) {
			handle = context.entry.loaded.base();
		} else if (context.module_named) {
			handle = context.module_named(call[1], true);
			if (!handle) {
				call.ret(0);
				return;
			}
		} else {
			auto name = to_utf8(call.wide(call[1]));
			for (auto &character : name)
				character = char(std::tolower(static_cast<unsigned char>(character)));
			if (name.find('.') == name.npos)
				name += ".dll";
			const auto found = modules.all().find(name);
			if (found != modules.all().end())
				handle = found->second->loaded.base();
		}
		if (!handle) {
			call.fail(126);
			return;
		}
		// Unless the caller asks for the count to stay as it is, the handle is a reference that FreeLibrary gives back.
		if (!(flags & 2) && context.reference_module)
			context.reference_module(handle, flags & 1);
		// The handle is a pointer of the program's width: a 32-bit program keeps four bytes there.
		if (call[2]) call.store_pointer(call[2], handle);
		call.ret(1);
	});

	// Environment variables live in the host process; names compare case-insensitively like Windows.
	struct Variables {
		std::map<std::u16string, std::pair<std::u16string, std::u16string>> values;
	};
	auto variables = std::make_shared<Variables>();
	variables->values = default_environment(paths);
	// A program started by another one begins with the environment its parent chose.
	if (context.launch.environment) {
		variables->values.clear();
		const auto &block = *context.launch.environment;
		for (size_t at = 0; at < block.size();) {
			const auto end = block.find(u'\0', at);
			const auto entry = block.substr(at, end == block.npos ? block.npos : end - at);
			if (entry.empty())
				break;
			// A name may begin with '=' (the per-drive directories), so the separator is looked for after the first character.
			const auto equals = entry.find(u'=', 1);
			if (equals != entry.npos) {
				auto name = entry.substr(0, equals);
				auto key = name;
				for (auto &character : key)
					character = upper_case(character);
				variables->values[key] = {name, entry.substr(equals + 1)};
			}
			at = end == block.npos ? block.size() : end + 1;
		}
	}
	// The compatibility layer a program was started with changes the version Windows reports to it.
	if (const auto layer = variables->values.find(u"__COMPAT_LAYER"); layer != variables->values.end())
		activation_contexts().set_compatibility_layer(layer->second.second);
	paths.set_search_path([variables] {
		const auto found = variables->values.find(u"PATH");
		return found == variables->values.end() ? std::u16string() : found->second.second;
	});
	context.environment_block = [variables]() {
		std::u16string block;
		for (const auto &[key, entry] : variables->values) {
			block += entry.first + u"=" + entry.second;
			block.push_back(u'\0');
		}
		block.push_back(u'\0');
		return block;
	};
	const auto key_of = [](std::u16string name) {
		for (auto &character : name)
			character = upper_case(character);
		return name;
	};
	registry.add("GetEnvironmentVariableW", 3, [variables, key_of](ApiCall &call) {
		const auto found = variables->values.find(key_of(call.wide(call[0])));
		if (found == variables->values.end()) {
			call.fail(203);
			return;
		}
		const auto &value = found->second.second;
		const size_t capacity = call.u32(2);
		call.put_wide(call[1], value, capacity);
		call.ret(capacity > value.size() ? value.size() : value.size() + 1);
	});
	registry.add("SetEnvironmentVariableW", 2, [variables, key_of](ApiCall &call) {
		const auto name = call.wide(call[0]);
		// A name may start with '=' (the per-drive current directories such as "=C:") but holds no other '='.
		if (name.empty() || name.find(u'=', 1) != name.npos) {
			call.fail(87);
			return;
		}
		// Removing a variable that is not set succeeds: Rust's standard library, for one, removes variables without looking first and
		// stops when that fails.
		if (!call[1]) {
			variables->values.erase(key_of(name));
		} else {
			variables->values[key_of(name)] = {name, call.wide(call[1])};
		}
		call.ret(1);
	});
	registry.add("ExpandEnvironmentStringsW", 3, [variables, key_of](ApiCall &call) {
		const auto text = call.wide(call[0]);
		std::u16string result;
		for (size_t at = 0; at < text.size();) {
			if (text[at] == u'%') {
				const auto close = text.find(u'%', at + 1);
				if (close != text.npos && close > at + 1) {
					const auto found = variables->values.find(key_of(text.substr(at + 1, close - at - 1)));
					if (found != variables->values.end()) {
						result += found->second.second;
						at = close + 1;
						continue;
					}
					result += text.substr(at, close - at);
					at = close;
					continue;
				}
			}
			result.push_back(text[at++]);
		}
		const size_t capacity = call.u32(2);
		call.put_wide(call[1], result, capacity);
		call.ret(result.size() + 1);
	});
	// The environment of any token is this user's: the expansion is ExpandEnvironmentStrings', answered as TRUE or FALSE.
	registry.add("ExpandEnvironmentStringsForUserW", 4, [&registry](ApiCall &call) {
		ApiCall inner(call.process, call.environment, {call[1], call[2], call[3]});
		registry.find("ExpandEnvironmentStringsW")->second(inner);
		if (inner.result > call.u32(3)) {
			call.fail(122, 0);
			return;
		}
		call.ret(1);
	});
	registry.add("GetEnvironmentStringsW", 0, [&, variables](ApiCall &call) {
		std::u16string block;
		for (const auto &[key, entry] : variables->values) {
			block += entry.first + u"=" + entry.second;
			block.push_back(0);
		}
		block.push_back(0);
		const auto address = context.allocator.allocate(block.size() * 2);
		if (!address) {
			call.fail(8);
			return;
		}
		std::vector<uint8_t> bytes(block.size() * 2);
		std::memcpy(bytes.data(), block.data(), bytes.size());
		call.write(address, bytes);
		call.ret(address);
	});
	registry.add("FreeEnvironmentStringsW", 1, [&](ApiCall &call) {
		call.ret(context.allocator.release(call[0]));
	});

	// TLS and FLS indexes are process-wide; values belong to the thread whose TEB asks.
	struct Slots {
		std::vector<bool> used;
		std::vector<uint64_t> callbacks;
		std::map<uint64_t, std::vector<uint64_t>> values;
	};
	const auto make_slots = [](size_t count) {
		auto slots = std::make_shared<Slots>();
		slots->used.assign(count, false);
		slots->callbacks.assign(count, 0);
		return slots;
	};
	const auto tls = make_slots(1088), fls = make_slots(4080);
	// The thread block mirrors values for the guest-side fast paths of Tls/FlsGetValue (and TlsSetValue).
	const auto bind = [&](const char *prefix, std::shared_ptr<Slots> slots, bool callback) {
		const std::string base = prefix;
		const bool is_tls = base == "Tls";
		const auto mirror_clear = [&context, is_tls](ApiCall &call, size_t index) {
			const uint64_t zero = 0;
			for (const auto block : context.threads.thread_blocks()) {
				if (is_tls && index < 64) {
					call.process.write_memory(block + 0x1480 + index * 8, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&zero), 8));
				} else if (!is_tls && index < 4080) {
					const auto array = call.load<uint64_t>(block + 0x17c8);
					if (array)
						call.process.write_memory(array + index * 8, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&zero), 8));
				}
			}
		};
		const auto mirror_set = [is_tls](ApiCall &call, uint64_t block, size_t index, uint64_t value) {
			if (is_tls) {
				if (index < 64)
					call.store<uint64_t>(block + 0x1480 + index * 8, value);
				return;
			}
			if (index >= 4080)
				return;
			auto array = call.load<uint64_t>(block + 0x17c8);
			if (!array) {
				array = call.process.map_memory(0x8000, PROT_READ | PROT_WRITE);
				call.store<uint64_t>(block + 0x17c8, array);
			}
			call.store<uint64_t>(array + index * 8, value);
		};
		registry.add(base + "Alloc", callback ? 1 : 0, [&environment, slots, callback, mirror_clear](ApiCall &call) {
			const auto free = std::find(slots->used.begin(), slots->used.end(), false);
			if (free == slots->used.end()) {
				call.fail(callback ? 1450 : 8, UINT32_MAX);
				return;
			}
			*free = true;
			const size_t index = size_t(free - slots->used.begin());
			slots->callbacks[index] = callback ? call[0] : 0;
			for (auto &[thread, values] : slots->values)
				if (index < values.size()) values[index] = 0;
			mirror_clear(call, index);
			call.ret(index);
			call.set_last_error(0);
		});
		registry.add(base + "Free", 1, [slots, mirror_clear](ApiCall &call) {
			const auto index = size_t(call.u32(0));
			if (index >= slots->used.size() || !slots->used[index]) {
				call.fail(87);
				return;
			}
			slots->used[index] = false;
			for (auto &[thread, values] : slots->values)
				if (index < values.size()) values[index] = 0;
			mirror_clear(call, index);
			call.ret(1);
		});
		const auto get_value = [&environment, slots](ApiCall &call) {
			const auto index = size_t(call.u32(0));
			if (index >= slots->used.size() || !slots->used[index]) {
				call.fail(87);
				return;
			}
			auto &values = slots->values[environment.teb()];
			call.set_last_error(0);
			call.ret(index < values.size() ? values[index] : 0);
		};
		registry.add(base + "GetValue", 1, get_value);
		// Newer Windows builds publish the fiber-local read under a second name that the C runtime calls.
		if (!is_tls)
			registry.add("FlsGetValue2", 1, get_value);
		registry.add(base + "SetValue", 2, [&environment, slots, mirror_set](ApiCall &call) {
			const auto index = size_t(call.u32(0));
			if (index >= slots->used.size() || !slots->used[index]) {
				call.fail(87);
				return;
			}
			auto &values = slots->values[environment.teb()];
			if (values.size() < slots->used.size())
				values.resize(slots->used.size());
			values[index] = call[1];
			mirror_set(call, environment.teb(), index, call[1]);
			call.ret(1);
		});
	};
	bind("Tls", tls, false);
	bind("Fls", fls, true);
	(void)process;
}

}
