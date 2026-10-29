#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace raven {

// Public export names of a Windows system library, empty when Raven knows no such library.
std::span<const std::string_view> system_export_names(std::string_view dll);
// Names that Windows also publishes under a fixed ordinal, which programs may import without a name.
std::span<const std::pair<std::string_view, uint16_t>> system_export_ordinals(std::string_view dll);
// The stdcall argument words of a function of a 32-bit system library, from the library's own decorated symbol; UINT32_MAX when it has none
// (a function that is not stdcall, or that the library does not export).
uint32_t system_export_words(std::string_view dll, std::string_view name);
// Every library with a name list, in lower case with its ".dll" suffix.
std::span<const std::string_view> system_library_names();
// The system libraries the runtime ships as guest components beside itself (in x86/ for 32-bit programs), which are loaded from there
// whatever copies a program ships or an installer leaves in the system directory.
inline constexpr std::string_view runtime_components[]{"msi.dll", "cabinet.dll", "sxs.dll", "rpcrt4.dll", "msxml3.dll", "libxslt.dll", "atl100.dll",
	"libpng.dll", "libjpeg.dll", "libtiff.dll", "propsys.dll", "mlang.dll", "windowscodecs.dll", "gdiplus.dll", "urlmon.dll", "riched20.dll", "msftedit.dll", "msvfw32.dll", "advpack.dll",
	"mscoree.dll", "taskschd.dll", "webservices.dll", "raven-accprop.dll", "raven-deployment.dll", "raven-netlist.dll", "raven-uisettings.dll", "xmllite.dll",
	"avrt.dll", "mfplat.dll"};

}
