#include "apps.h"

#include "registry.h"
#include "text.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <fstream>
#include <map>

namespace raven {

namespace {

std::string lower(std::string_view text)
{
	std::string result(text);
	for (auto &character : result)
		if (character >= 'A' && character <= 'Z')
			character = char(character + ('a' - 'A'));
	return result;
}

// Whether a relative path is the given one or lies under it, without regard to case.
bool within(std::string_view path, std::string_view folder)
{
	if (path.size() < folder.size() || lower(path.substr(0, folder.size())) != lower(folder))
		return false;
	return path.size() == folder.size() || path[folder.size()] == '/';
}

bool within_key(std::u16string_view key, std::u16string_view folder)
{
	return key.starts_with(folder) && (key.size() == folder.size() || key[folder.size()] == u'\\');
}

std::filesystem::path beside(const std::filesystem::path &prefix, std::string_view suffix)
{
	const auto plain = prefix.lexically_normal();
	const auto name = plain.has_filename() ? plain.filename() : plain.parent_path().filename();
	const auto parent = plain.has_filename() ? plain.parent_path() : plain.parent_path().parent_path();
	return parent / (name.string() + std::string(suffix));
}

std::string safe_name(std::string_view text)
{
	std::string name;
	for (const char character : text)
		name.push_back(std::isalnum(static_cast<unsigned char>(character)) || character == '-' || character == '.' || character == '_' ? character : '_');
	if (name.empty() || name.front() == '.')
		name.insert(name.begin(), 'x');
	return name;
}

std::optional<InstalledApp> read_app(const std::filesystem::path &file)
{
	std::ifstream input(file);
	if (!input)
		return std::nullopt;
	InstalledApp app{file.filename().string(), {}, {}, {}, {}};
	std::string line;
	while (std::getline(input, line)) {
		const auto space = line.find(' ');
		if (space == line.npos)
			continue;
		const auto kind = std::string_view(line).substr(0, space);
		const auto value = line.substr(space + 1);
		if (kind == "folder")
			app.folders.push_back(value);
		else if (kind == "file")
			app.files.push_back(value);
		else if (kind == "key")
			app.keys.push_back(from_utf8(value));
		else if (kind == "changed-key")
			app.changed_keys.push_back(from_utf8(value));
	}
	return app;
}

std::string relative_text(const std::filesystem::path &path, const std::filesystem::path &root)
{
	const auto relative = path.lexically_normal().lexically_relative(root.lexically_normal());
	if (relative.empty() || *relative.begin() == "..")
		return {};
	return relative.generic_string();
}

}

std::filesystem::path apps_directory(const std::filesystem::path &prefix)
{
	return beside(prefix, ".apps");
}

std::filesystem::path layers_directory(const std::filesystem::path &prefix)
{
	return beside(prefix, ".layers");
}

std::vector<InstalledApp> installed_apps(const std::filesystem::path &prefix)
{
	std::vector<InstalledApp> apps;
	std::error_code error;
	for (const auto &entry : std::filesystem::directory_iterator(apps_directory(prefix), error))
		if (entry.is_regular_file(error))
			if (auto app = read_app(entry.path()))
				apps.push_back(std::move(*app));
	std::sort(apps.begin(), apps.end(), [](const auto &left, const auto &right) { return left.id < right.id; });
	return apps;
}

std::optional<InstalledApp> app_of(const std::filesystem::path &prefix, const std::filesystem::path &executable)
{
	const auto relative = relative_text(std::filesystem::absolute(executable), std::filesystem::absolute(prefix));
	if (relative.empty() || within(relative, "drive_c/windows"))
		return std::nullopt;
	for (auto &app : installed_apps(prefix)) {
		for (const auto &file : app.files)
			if (lower(file) == lower(relative))
				return std::move(app);
		for (const auto &folder : app.folders)
			if (within(relative, folder))
				return std::move(app);
	}
	return std::nullopt;
}

InstalledApp record_installation(const std::filesystem::path &prefix, const std::filesystem::path &snapshot, std::string_view name)
{
	const auto store = apps_directory(prefix);
	std::filesystem::create_directories(store);
	std::filesystem::create_directories(layers_directory(prefix));
	InstalledApp app{safe_name(name), {}, {}, {}, {}};
	for (int count = 2; std::filesystem::exists(store / app.id); ++count)
		app.id = safe_name(name) + "~" + std::to_string(count);
	// The files: only the drives count, as the prefix's own records beside them change with every program that runs.
	std::error_code error;
	for (const auto &drive : std::filesystem::directory_iterator(prefix, error)) {
		if (!drive.is_directory(error) || !drive.path().filename().string().starts_with("drive_"))
			continue;
		for (auto walk = std::filesystem::recursive_directory_iterator(drive.path(), std::filesystem::directory_options::skip_permission_denied);
			walk != std::filesystem::recursive_directory_iterator(); ++walk) {
			const auto relative = relative_text(walk->path(), prefix);
			const auto before = snapshot / relative;
			std::error_code missing;
			if (walk->is_symlink(missing))
				continue;
			if (walk->is_directory(missing)) {
				if (!std::filesystem::is_directory(before, missing)) {
					app.folders.push_back(relative);
					walk.disable_recursion_pending();
				}
			} else if (walk->is_regular_file(missing)) {
				const bool same = std::filesystem::is_regular_file(before, missing) && std::filesystem::file_size(before, missing) == walk->file_size(missing) &&
					std::filesystem::last_write_time(before, missing) == walk->last_write_time(missing);
				if (!same)
					app.files.push_back(relative);
			}
		}
	}
	// The registry: keys made, the topmost of each tree made, and keys whose values changed.
	const auto earlier = NativeRegistry::digest(snapshot / "registry.dat");
	const auto now = NativeRegistry::digest(prefix / "registry.dat");
	for (const auto &[key, values] : now) {
		const auto found = earlier.find(key);
		if (found == earlier.end()) {
			if (app.keys.empty() || !within_key(key, app.keys.back()))
				app.keys.push_back(key);
		} else if (found->second != values)
			app.changed_keys.push_back(key);
	}
	// An installation that changed nothing installed nothing.
	if (app.folders.empty() && app.files.empty() && app.keys.empty() && app.changed_keys.empty()) {
		app.id.clear();
		return app;
	}
	const auto partial = store / (app.id + ".partial");
	{
		std::ofstream output(partial, std::ios::trunc);
		for (const auto &folder : app.folders)
			output << "folder " << folder << '\n';
		for (const auto &file : app.files)
			output << "file " << file << '\n';
		for (const auto &key : app.keys)
			output << "key " << to_utf8(key) << '\n';
		for (const auto &key : app.changed_keys)
			output << "changed-key " << to_utf8(key) << '\n';
		if (!output.flush())
			throw std::runtime_error("cannot record the installation");
	}
	std::filesystem::rename(partial, store / app.id);
	return app;
}

bool discard_layer(const std::filesystem::path &prefix, std::string_view id)
{
	const auto name = safe_name(id);
	if (name != id || !std::filesystem::exists(apps_directory(prefix) / name))
		return false;
	const auto layers = layers_directory(prefix);
	std::error_code error;
	std::filesystem::remove_all(layers / name, error);
	std::filesystem::remove(layers / (name + ".reg"), error);
	std::filesystem::remove(layers / (name + ".deleted"), error);
	return true;
}

bool remove_app(const std::filesystem::path &prefix, std::string_view id)
{
	if (!discard_layer(prefix, id))
		return false;
	std::error_code error;
	return std::filesystem::remove(apps_directory(prefix) / std::string(id), error);
}

void AppLayer::set(const std::filesystem::path &prefix_root, InstalledApp app)
{
	owner = std::move(app);
	prefix = std::filesystem::absolute(prefix_root).lexically_normal();
	layer = layers_directory(prefix) / owner.id;
	removed.clear();
	removed_stamp = -2;
}

bool AppLayer::owns(std::string_view relative) const
{
	if (!active())
		return true;
	for (const auto &folder : owner.folders)
		if (within(relative, folder))
			return true;
	for (const auto &file : owner.files)
		if (lower(file) == lower(relative))
			return true;
	// The person's documents and the temporary files are everyone's: what one program saves there another opens.
	const auto text = lower(relative);
	if (within(text, "drive_c/windows/temp"))
		return true;
	if (text.starts_with("drive_c/users/")) {
		const auto user = text.find('/', 14);
		if (user != text.npos) {
			const auto rest = std::string_view(text).substr(user + 1);
			for (const std::string_view shared : {"documents", "desktop", "downloads", "music", "pictures", "videos", "appdata/local/temp"})
				if (within(rest, shared))
					return true;
		}
	}
	// Other drives than C: are the person's.
	return !text.starts_with("drive_c/") && text != "drive_c";
}

bool AppLayer::owns_key(std::u16string_view folded) const
{
	if (!active())
		return true;
	for (const auto &key : owner.keys)
		if (within_key(folded, key))
			return true;
	for (const auto &key : owner.changed_keys)
		if (folded == key)
			return true;
	return false;
}

void AppLayer::read_deleted() const
{
	const auto file = layer.parent_path() / (owner.id + ".deleted");
	struct stat status{};
	const long long stamp = ::stat(file.c_str(), &status) ? -1 : status.st_mtim.tv_sec * 1000000000LL + status.st_mtim.tv_nsec + status.st_size;
	if (stamp == removed_stamp)
		return;
	removed_stamp = stamp;
	removed.clear();
	std::ifstream input(file);
	for (std::string line; std::getline(input, line);)
		if (!line.empty())
			removed.push_back(lower(line));
}

bool AppLayer::deleted(std::string_view relative) const
{
	if (!active())
		return false;
	read_deleted();
	if (removed.empty())
		return false;
	for (const auto &path : removed)
		if (within(relative, path))
			return true;
	return false;
}

void AppLayer::mark_deleted(std::string_view relative, bool value) const
{
	read_deleted();
	const auto text = lower(relative);
	const bool present = std::find(removed.begin(), removed.end(), text) != removed.end();
	if (present == value)
		return;
	if (value)
		removed.push_back(text);
	else
		removed.erase(std::remove_if(removed.begin(), removed.end(), [&](const auto &path) { return within(path, text); }), removed.end());
	const auto file = layer.parent_path() / (owner.id + ".deleted");
	std::filesystem::create_directories(file.parent_path());
	const auto partial = file.parent_path() / (owner.id + ".deleted.partial");
	{
		std::ofstream output(partial, std::ios::trunc);
		for (const auto &path : removed)
			output << path << '\n';
	}
	std::error_code error;
	std::filesystem::rename(partial, file, error);
	removed_stamp = -2;
}

void AppLayer::hide(const std::filesystem::path &host) const
{
	if (!active())
		return;
	const auto relative = relative_text(std::filesystem::absolute(host), layer);
	std::error_code error;
	if (!relative.empty() && !owns(relative) && std::filesystem::exists(std::filesystem::symlink_status(prefix / relative, error)))
		mark_deleted(relative, true);
}

AppLayer &app_layer()
{
	static AppLayer layer;
	return layer;
}

}
