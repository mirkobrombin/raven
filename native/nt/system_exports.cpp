#include "system_exports.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <vector>

namespace raven {

namespace {

#include "system_exports.inc"
#include "system_arguments.inc"
#include "system_ordinals.inc"

constexpr std::array<std::pair<std::string_view, uint16_t>, 6> comctl32_ordinals{{
	{"DefSubclassProc", 413}, {"GetWindowSubclass", 411}, {"InitCommonControls", 17}, {"LoadIconWithScaleDown", 381},
	{"RemoveWindowSubclass", 412}, {"SetWindowSubclass", 410},
}};
constexpr std::array<std::pair<std::string_view, uint16_t>, 4> shell32_ordinals{{
	{"IsUserAnAdmin", 680}, {"OpenRegStream", 85}, {"SHCloneSpecialIDList", 89}, {"SHCreateDirectory", 165},
}};
constexpr std::array<std::pair<std::string_view, uint16_t>, 9> oleaut32_ordinals{{
	{"SysAllocString", 2}, {"SysAllocStringLen", 4}, {"SysFreeString", 6}, {"SysReAllocString", 3}, {"SysReAllocStringLen", 5},
	{"SysStringLen", 7}, {"VariantClear", 9}, {"VariantCopy", 10}, {"VariantInit", 8},
}};
constexpr std::array<std::pair<std::string_view, uint16_t>, 28> ws2_32_ordinals{{
	{"__WSAFDIsSet", 151}, {"accept", 1}, {"bind", 2}, {"closesocket", 3}, {"connect", 4}, {"getpeername", 5}, {"getsockname", 6},
	{"getsockopt", 7}, {"htonl", 8}, {"htons", 9}, {"inet_addr", 11}, {"inet_ntoa", 12}, {"ioctlsocket", 10}, {"listen", 13},
	{"ntohl", 14}, {"ntohs", 15}, {"recv", 16}, {"recvfrom", 17}, {"select", 18}, {"send", 19}, {"sendto", 20}, {"setsockopt", 21},
	{"shutdown", 22}, {"socket", 23}, {"WSACleanup", 116}, {"WSAGetLastError", 111}, {"WSASetLastError", 112}, {"WSAStartup", 115},
}};
// The same sockets, plus the two extension calls that wsock32.dll alone publishes by number.
constexpr std::array<std::pair<std::string_view, uint16_t>, 30> wsock32_ordinals{{
	{"__WSAFDIsSet", 151}, {"accept", 1}, {"AcceptEx", 1141}, {"bind", 2}, {"closesocket", 3}, {"connect", 4}, {"GetAcceptExSockaddrs", 1142},
	{"getpeername", 5}, {"getsockname", 6}, {"getsockopt", 7}, {"htonl", 8}, {"htons", 9}, {"inet_addr", 11}, {"inet_ntoa", 12},
	{"ioctlsocket", 10}, {"listen", 13}, {"ntohl", 14}, {"ntohs", 15}, {"recv", 16}, {"recvfrom", 17}, {"select", 18}, {"send", 19},
	{"sendto", 20}, {"setsockopt", 21}, {"shutdown", 22}, {"socket", 23}, {"WSACleanup", 116}, {"WSAGetLastError", 111},
	{"WSASetLastError", 112}, {"WSAStartup", 115},
}};
constexpr std::array<std::pair<std::string_view, uint16_t>, 2> shlwapi_ordinals{{{"IUnknown_QueryService", 176}, {"IsOS", 437}}};
// DirectSound's functions keep the numbers of its first release, which programs import them by.
constexpr std::array<std::pair<std::string_view, uint16_t>, 12> dsound_ordinals{{
	{"DirectSoundCreate", 1}, {"DirectSoundEnumerateA", 2}, {"DirectSoundEnumerateW", 3}, {"DllCanUnloadNow", 4}, {"DllGetClassObject", 5},
	{"DirectSoundCaptureCreate", 6}, {"DirectSoundCaptureEnumerateA", 7}, {"DirectSoundCaptureEnumerateW", 8}, {"GetDeviceID", 9},
	{"DirectSoundFullDuplexCreate", 10}, {"DirectSoundCreate8", 11}, {"DirectSoundCaptureCreate8", 12},
}};
// The system file checker's catalog functions have numbers and no names.
constexpr std::array<std::pair<std::string_view, uint16_t>, 2> sfc_ordinals{{{"SfpInstallCatalog", 8}, {"SfpDeleteCatalog", 9}}};
// uxtheme.dll's dark mode functions, which programs take by these numbers.
constexpr std::array<std::pair<std::string_view, uint16_t>, 9> uxtheme_ordinals{{
	{"AllowDarkModeForWindow", 133}, {"FlushMenuThemes", 136}, {"GetIsImmersiveColorUsingHighContrast", 106}, {"IsDarkModeAllowedForApp", 139},
	{"IsDarkModeAllowedForWindow", 137}, {"RefreshImmersiveColorPolicyState", 104}, {"SetPreferredAppMode", 135}, {"ShouldAppsUseDarkMode", 132},
	{"ShouldSystemUseDarkMode", 138},
}};

}

std::span<const std::string_view> system_export_names(std::string_view dll)
{
	for (const auto &[name, exports] : libraries)
		if (name == dll)
			return exports;
	return {};
}

// The Windows numbers of a library's exports, as its specification records them, with the pins above added for names it lacks.
template <const auto &spec, const auto &pinned>
std::span<const std::pair<std::string_view, uint16_t>> merged_ordinals()
{
	static const auto merged = [] {
		std::vector<std::pair<std::string_view, uint16_t>> list(std::begin(spec), std::end(spec));
		for (const auto &entry : pinned)
			if (std::none_of(list.begin(), list.end(), [&](const auto &item) { return item.first == entry.first || item.second == entry.second; }))
				list.push_back(entry);
		std::sort(list.begin(), list.end());
		return list;
	}();
	return merged;
}

std::span<const std::pair<std::string_view, uint16_t>> system_export_ordinals(std::string_view dll)
{
	if (dll == "comctl32.dll")
		return merged_ordinals<comctl32_spec_ordinals, comctl32_ordinals>();
	if (dll == "shell32.dll")
		return merged_ordinals<shell32_spec_ordinals, shell32_ordinals>();
	if (dll == "oleaut32.dll")
		return merged_ordinals<oleaut32_spec_ordinals, oleaut32_ordinals>();
	if (dll == "shlwapi.dll")
		return merged_ordinals<shlwapi_spec_ordinals, shlwapi_ordinals>();
	if (dll == "ws2_32.dll")
		return merged_ordinals<ws2_32_spec_ordinals, ws2_32_ordinals>();
	if (dll == "wsock32.dll")
		return merged_ordinals<wsock32_spec_ordinals, wsock32_ordinals>();
	if (dll == "dsound.dll")
		return dsound_ordinals;
	if (dll == "sfc.dll")
		return sfc_ordinals;
	if (dll == "uxtheme.dll")
		return uxtheme_ordinals;
	if (dll == "wldap32.dll")
		return wldap32_spec_ordinals;
	return {};
}

uint32_t system_export_words(std::string_view dll, std::string_view name)
{
	for (const auto &[library, table] : argument_tables) {
		if (library != dll)
			continue;
		const auto found = std::lower_bound(table.begin(), table.end(), name, [](const auto &item, std::string_view wanted) { return item.first < wanted; });
		if (found != table.end() && found->first == name)
			return found->second;
		break;
	}
	return std::numeric_limits<uint32_t>::max();
}

std::span<const std::string_view> system_library_names()
{
	static const auto names = [] {
		std::array<std::string_view, std::size(libraries)> list{};
		std::transform(std::begin(libraries), std::end(libraries), list.begin(), [](const auto &item) { return item.first; });
		return list;
	}();
	return names;
}

bool program_may_replace(std::string_view dll)
{
	static const bool prefer_local = [] {
		const char *value = std::getenv("RAVEN_PREFER_LOCAL_DLLS");
		return value && std::string_view(value) == "1";
	}();
	if (std::find(std::begin(program_replaceable_libraries), std::end(program_replaceable_libraries), dll) != std::end(program_replaceable_libraries))
		return true;
	const auto names = system_library_names();
	return prefer_local && std::find(names.begin(), names.end(), dll) != names.end() &&
		std::find(std::begin(core_system_libraries), std::end(core_system_libraries), dll) == std::end(core_system_libraries);
}

}
