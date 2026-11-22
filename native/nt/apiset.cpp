#include "apiset.h"

#include <array>

namespace raven {

namespace {

struct ApiSetContract { std::string_view name, host; };

constexpr ApiSetContract implemented_contracts[]{
	{"api-ms-win-core-apiquery-l2-1-0", "kernel32.dll"},
	{"api-ms-win-core-console-l1-1-0", "kernel32.dll"},
	{"api-ms-win-core-file-l1-1-0", "kernel32.dll"},
	{"api-ms-win-core-libraryloader-l1-2-0", "kernel32.dll"},
	{"api-ms-win-core-path-l1-1-0", "kernel32.dll"},
	{"api-ms-win-core-processthreads-l1-1-0", "kernel32.dll"},
	{"api-ms-win-core-synch-l1-2-0", "kernelbase.dll"},
	{"api-ms-win-core-winrt-l1-1-0", "runtimeobject.dll"},
	{"api-ms-win-dx-d3dkmt-l1-1-0", "gdi32.dll"},
	{"ext-ms-win-gdi-draw-l1-1-1", "gdi32.dll"},
};

std::string lowercase_name(std::string_view name)
{
	std::string lower(name);
	for (auto &character : lower)
		if (character >= 'A' && character <= 'Z')
			character = char(character + ('a' - 'A'));
	return lower;
}

std::string_view declared_host(std::string_view name)
{
	for (const auto &contract : implemented_contracts)
		if (name == contract.name)
			return contract.host;
	return {};
}

}

bool apiset_implemented(std::string_view name)
{
	// The loader's compatibility fallbacks do not establish that a contract is available.
	return !declared_host(lowercase_name(name)).empty();
}

std::string apiset_host(std::string_view name)
{
	auto lower = lowercase_name(name);
	if (lower.ends_with(".dll"))
		lower.resize(lower.size() - 4);
	if (const auto host = declared_host(lower); !host.empty())
		return std::string(host);
	if (!lower.starts_with("api-ms-win-") && !lower.starts_with("ext-ms-win-") && !lower.starts_with("api-ms-onecoreuap-"))
		return {};
	struct Family {
		std::string_view prefix, host;
	};
	static constexpr std::array<Family, 32> families{{
		{"api-ms-win-core-registry-", "advapi32.dll"}, {"api-ms-win-security-", "advapi32.dll"}, {"api-ms-win-core-security-", "advapi32.dll"},
		{"api-ms-win-eventing-", "advapi32.dll"}, {"api-ms-win-core-com-", "ole32.dll"}, {"api-ms-win-core-shlwapi-", "shlwapi.dll"},
		{"api-ms-win-shell-", "shell32.dll"}, {"api-ms-win-core-shell-", "shell32.dll"}, {"ext-ms-win-ntuser-", "user32.dll"},
		{"api-ms-win-ntuser-sysparams-", "user32.dll"}, {"ext-ms-win-uiacore-", "uiautomationcore.dll"},
		{"api-ms-win-core-localization-", "kernel32.dll"}, {"api-ms-win-core-synch-", "kernelbase.dll"}, {"api-ms-win-gdi-", "gdi32.dll"}, {"ext-ms-win-gdi-", "gdi32.dll"},
		{"api-ms-win-dx-d3dkmt-", "gdi32.dll"},
		{"api-ms-win-rtcore-ntuser-private-", "user32.dll"},
		{"api-ms-win-core-ntuser-", "user32.dll"}, {"api-ms-win-service-", "advapi32.dll"}, {"api-ms-win-devices-", "setupapi.dll"},
		{"api-ms-win-core-psapi-", "psapi.dll"}, {"api-ms-win-shcore-", "shcore.dll"}, {"api-ms-win-core-winrt-", "runtimeobject.dll"}, {"api-ms-win-ro-typeresolution-", "wintypes.dll"}, {"api-ms-win-crt-", "ucrtbase.dll"},
		{"api-ms-win-power-", "powrprof.dll"}, {"api-ms-win-downlevel-shell32-", "shell32.dll"}, {"api-ms-win-downlevel-advapi32-", "advapi32.dll"}, {"api-ms-win-downlevel-user32-", "user32.dll"},
		{"api-ms-win-downlevel-ole32-", "ole32.dll"}, {"api-ms-win-downlevel-version-", "version.dll"}, {"api-ms-win-downlevel-shlwapi-", "shlwapi.dll"},
	}};
	for (const auto &family : families)
		if (lower.starts_with(family.prefix))
			return std::string(family.host);
	return "kernel32.dll";
}

}
