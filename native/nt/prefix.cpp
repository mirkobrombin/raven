#include "prefix.h"
#include "system_exports.h"

#include <cctype>
#include <cstdlib>
#include <system_error>
#include <vector>
#include <stdexcept>
#include <string>

namespace raven {

NativePrefix::NativePrefix(std::filesystem::path root) : directory(std::move(root))
{
	if (directory.empty())
		throw std::runtime_error("native prefix requires a directory");
	// The folders every Windows installation has, so that programs can put files in the places they expect.
	for (const char *folder : {"windows", "windows/system32", "windows/syswow64", "Program Files/Common Files", "Program Files (x86)/Common Files",
			"ProgramData/Microsoft/Windows/Start Menu/Programs",
			"Users/Public/Desktop", "Users/Public/Documents"})
		std::filesystem::create_directories(directory / "drive_c" / folder);
	// The Fonts folder holds the installed fonts, as on Windows: links to the host's font files, made once when the folder is new.
	const auto fonts = directory / "drive_c" / "windows" / "Fonts";
	std::error_code error;
	if (std::filesystem::create_directory(fonts, error)) {
		std::vector<std::filesystem::path> roots{"/usr/share/fonts", "/usr/local/share/fonts"};
		if (const char *home = std::getenv("HOME"); home && *home)
			roots.push_back(std::filesystem::path(home) / ".local" / "share" / "fonts");
		for (const auto &root : roots)
			for (std::filesystem::recursive_directory_iterator walk(root, std::filesystem::directory_options::follow_directory_symlink, error), end;
					walk != end; walk.increment(error)) {
				auto extension = walk->path().extension().string();
				for (auto &character : extension)
					character = char(std::tolower(static_cast<unsigned char>(character)));
				if ((extension == ".ttf" || extension == ".otf" || extension == ".ttc") && walk->is_regular_file(error))
					std::filesystem::create_symlink(walk->path(), fonts / walk->path().filename(), error);
			}
	}
}

void NativePrefix::place_runtime_components(const std::filesystem::path &runtime) const
{
	// The loader takes these libraries from beside the runtime whatever the prefix holds: the copies only answer programs that search for,
	// read or hash them. A newer runtime refreshes them.
	const auto windows = directory / "drive_c" / "windows";
	for (const auto &[source, folder] : {std::pair{runtime, "system32"}, std::pair{runtime / "x86", "syswow64"}})
		for (const auto name : runtime_components) {
			std::error_code error;
			const auto from = source / name;
			if (std::filesystem::is_regular_file(from, error))
				std::filesystem::copy_file(from, windows / folder / name, std::filesystem::copy_options::update_existing, error);
		}
	// The Windows Runtime metadata stands in each system directory's WinMetadata folder, where the type resolution of the Windows Runtime
	// looks for it.
	for (const auto folder : {"system32", "syswow64"}) {
		std::error_code error;
		const auto source = runtime / "WinMetadata", target = windows / folder / "WinMetadata";
		if (!std::filesystem::is_directory(source, error))
			break;
		std::filesystem::create_directories(target, error);
		for (const auto &entry : std::filesystem::directory_iterator(source, error))
			if (entry.path().extension() == ".winmd")
				std::filesystem::copy_file(entry.path(), target / entry.path().filename(), std::filesystem::copy_options::update_existing, error);
	}
	// The page engine of the web controls and the WebView2 runtime over it are too large to copy into every prefix, so the system
	// directory links to the runtime's.
	for (const auto &[folder, name] : {std::pair{"cef", "raven-cef"}, std::pair{"webview", "raven-webview"}}) {
		std::error_code error;
		const auto target = runtime / folder, link = windows / "system32" / name;
		if (!std::filesystem::is_directory(target, error))
			continue;
		if (std::filesystem::is_symlink(link, error) && std::filesystem::read_symlink(link, error) != target)
			std::filesystem::remove(link, error);
		if (!std::filesystem::exists(std::filesystem::symlink_status(link, error)))
			std::filesystem::create_directory_symlink(target, link, error);
	}
}

NativePrefix NativePrefix::from_environment()
{
	if (const char *configured = std::getenv("RAVEN_PREFIX"); configured && *configured)
		return NativePrefix(configured);
	const char *home = std::getenv("HOME");
	if (!home || !*home)
		throw std::runtime_error("native prefix needs RAVEN_PREFIX or HOME");
	return NativePrefix(std::filesystem::path(home) / ".local" / "share" / "raven" / "default");
}

std::optional<std::filesystem::path> NativePrefix::resolve(std::string_view windows_path) const
{
	std::string text(windows_path);
	for (auto &character : text)
		if (character == '\\')
			character = '/';
	std::filesystem::path base = directory / "drive_c" / "windows";
	if (text.size() >= 2 && text[1] == ':') {
		const char drive = char(text[0] | 0x20);
		if (drive < 'a' || drive > 'z')
			return std::nullopt;
		base = directory / (std::string("drive_") + drive);
		text.erase(0, 2);
	}
	if (!text.empty() && text[0] == '/')
		text.erase(0, 1);
	if (text.empty())
		return std::nullopt;
	std::filesystem::path result = base;
	size_t start = 0;
	while (start <= text.size()) {
		const size_t end = text.find('/', start);
		const auto part = text.substr(start, end == text.npos ? text.npos : end - start);
		if (part == "..")
			return std::nullopt;
		if (!part.empty() && part != ".")
			result /= part;
		if (end == text.npos)
			break;
		start = end + 1;
	}
	return result;
}

}
