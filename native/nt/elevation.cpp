#include "elevation.h"

#include "pe/resources.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace raven {

namespace {

bool elevated = false;
bool noninteractive = false;
bool child_elevation_requested = false;

}

bool manifest_requests_elevation(const PEImage &image, std::string_view file_name)
{
	for (const auto &resource : image.resources()) {
		if (resource.type != ResourceKey{uint32_t(24)} || !resource.size)
			continue;
		const auto bytes = image.file_span(resource.data_address, resource.size);
		const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
		const auto element = text.find("requestedExecutionLevel");
		if (element == text.npos)
			continue;
		const auto attribute = text.find("level=", element);
		if (attribute == text.npos || attribute + 7 >= text.size())
			continue;
		const char quote = text[attribute + 6];
		const auto end = text.find(quote, attribute + 7);
		if (end == text.npos)
			continue;
		const auto level = text.substr(attribute + 7, end - attribute - 7);
		return level == "requireAdministrator" || level == "highestAvailable";
	}
	// Installer detection: a 32-bit program that states no execution level and is named like a setup program is taken for an installer
	// and starts elevated.
	if (image.is_64)
		return false;
	std::string name(file_name);
	for (auto &character : name)
		if (character >= 'A' && character <= 'Z')
			character = char(character + ('a' - 'A'));
	for (const std::string_view word : {"setup", "install", "update", "patch", "upgrad"})
		if (name.find(word) != name.npos)
			return true;
	return false;
}

bool installer_like(const PEImage &image, std::string_view file_name)
{
	if (manifest_requests_elevation(image, file_name))
		return true;
	std::string name(file_name);
	for (auto &character : name)
		if (character >= 'A' && character <= 'Z')
			character = char(character + ('a' - 'A'));
	for (const std::string_view word : {"setup", "install", "uninst", "update", "patch", "redist"})
		if (name.find(word) != name.npos)
			return true;
	if (name == "msiexec.exe")
		return true;
	// A program that installs, changes or removes Windows Installer packages itself does what msiexec does.
	for (const auto &import : image.imports()) {
		const std::string_view function = import.name;
		for (const std::string_view installs : {"MsiInstallProduct", "MsiConfigureProduct", "MsiConfigureFeature", "MsiReinstallProduct", "MsiApplyPatch"})
			if (function.starts_with(installs))
				return true;
	}
	// A WiX bundle keeps its manifest in a section of its own; an IExpress package keeps its cabinet as a resource.
	if (std::any_of(image.sections.begin(), image.sections.end(), [](const auto &section) { return section.name == ".wixburn"; }))
		return true;
	for (const auto &resource : image.resources())
		if (resource.type == ResourceKey{uint32_t(10)} && resource.name == ResourceKey{std::u16string(u"CABINET")})
			return true;
	return false;
}

void set_process_elevated(bool value)
{
	elevated = value;
}

bool process_elevated()
{
	return elevated;
}

void set_process_noninteractive(bool value)
{
	noninteractive = value;
}

bool process_noninteractive()
{
	return noninteractive;
}

void request_child_elevation(bool value)
{
	child_elevation_requested = value;
}

bool child_elevation_requested_now()
{
	return child_elevation_requested;
}

}
