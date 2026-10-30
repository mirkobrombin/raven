#include "x86_loader.h"
#include "actctx.h"

#include "apiset.h"
#include "text.h"
#include "system_exports.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace raven {

namespace {

std::string lowered(std::string text)
{
	for (auto &character : text)
		if (character >= 'A' && character <= 'Z')
			character = static_cast<char>(character + ('a' - 'A'));
	return text;
}

std::vector<uint8_t> read_all(const std::filesystem::path &path)
{
	std::ifstream stream(path, std::ios::binary | std::ios::ate);
	const auto size = stream ? stream.tellg() : std::streampos(-1);
	if (size <= 0)
		return {};
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	stream.seekg(0);
	if (!stream.read(reinterpret_cast<char *>(bytes.data()), size))
		bytes.resize(static_cast<size_t>(stream.gcount()));
	return bytes;
}

bool trace()
{
	return getenv("RAVEN_TRACE_X86") != nullptr;
}

}

NativeX86Loader::NativeX86Loader(NativeProcess &process, NativeEnvironment &environment, ModuleSet &modules, const NativePaths &paths,
	std::function<NativeResult(uint64_t, std::span<const uint64_t>)> call)
	: process(process), environment(environment), modules(modules), paths(paths), call(std::move(call))
{
}

std::optional<std::vector<uint8_t>> NativeX86Loader::read_library(const std::string &name, std::string &identity, std::filesystem::path &file) const
{
	// The runtime's own components come from beside it, in their 32-bit builds.
	if (std::find(std::begin(runtime_components), std::end(runtime_components), name) != std::end(runtime_components)) {
		const auto component = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "x86" / name;
		if (std::filesystem::exists(component)) {
			identity = name;
			return read_all(component);
		}
	}
	// The C runtime comes from the runtime's own directory, in its 32-bit build.
	if (name == "ucrtbase.dll") {
		const auto component = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "ucrtbase32.dll";
		if (!std::filesystem::exists(component))
			return std::nullopt;
		identity = name;
		return read_all(component);
	}
	// A name alone that an assembly of the context in effect declares comes from that assembly's folder, as Windows redirects it.
	if (name.find_first_of("/\\:") == std::string::npos)
		if (const auto folder = activation_contexts().library_folder(name)) {
			std::error_code error;
			for (const auto &entry : std::filesystem::directory_iterator(*folder, error))
				if (lowered(entry.path().filename().string()) == name && entry.is_regular_file(error)) {
					identity = name;
					file = entry.path();
					return read_all(entry.path());
				}
		}
	const auto resolved = paths.find_library(application_directory + u"\\program.exe", from_utf8(name), false, library_flags);
	if (!resolved || !resolved->exists || !std::filesystem::is_regular_file(resolved->host)) {
		// Direct3D 10.1 is the runtime's own library over d3d10core.dll, unless the Direct3D build the runtime is given carries one.
		if (name == "d3d10_1.dll") {
			std::vector<std::filesystem::path> places;
			if (const char *directory = std::getenv("RAVEN_D3D_DIRECTORY"))
				places.push_back(std::filesystem::path(directory) / name);
			places.push_back(std::filesystem::read_symlink("/proc/self/exe").parent_path() / "x86" / "raven-d3d10_1.dll");
			for (const auto &place : places)
				if (std::filesystem::exists(place)) {
					identity = name;
					file = place;
					return read_all(place);
				}
		}
		return std::nullopt;
	}
	identity = lowered(resolved->host.filename().string());
	file = resolved->host;
	return read_all(resolved->host);
}

// Brings one library into the process, with its dependencies; null when it is not available.
PEModule *NativeX86Loader::bring(const std::string &requested)
{
	auto name = lowered(requested);
	if (name.find('.') == std::string::npos)
		name += ".dll";
	if (const auto host = apiset_host(name); !host.empty()) {
		if (!modules.all().contains(host) && !bring(host))
			return nullptr;
		if (!modules.all().contains(name))
			modules.add_alias(name, host);
		name = host;
	}
	if (const auto found = modules.all().find(name); found != modules.all().end())
		return found->second.get();
	// A system library named by its path is the runtime's, as it is by its name; any library named by its path in the system directory is
	// looked for as by its name, so a program that loads comctl32.dll from System32 gets the one the runtime has.
	if (const auto slash = name.find_last_of("/\\:"); slash != std::string::npos) {
		const auto base = name.substr(slash + 1);
		if (const auto found = modules.all().find(base); found != modules.all().end() && found->second->runtime_api)
			return found->second.get();
		auto folder = name.substr(0, slash);
		for (auto &character : folder)
			if (character == '/')
				character = '\\';
		if (folder == "c:\\windows\\system32" || folder == "c:\\windows\\syswow64") {
			if (const auto fallback = modules.all().find("system32-" + base); fallback != modules.all().end())
				return fallback->second.get();
			return bring(base);
		}
	}
	if ((library_flags & 0x1f00) == 0x800) {
		if (const auto fallback = modules.all().find("system32-" + name); fallback != modules.all().end())
			return fallback->second.get();
	}
	std::string identity;
	std::filesystem::path file;
	auto bytes = read_library(name, identity, file);
	if (!bytes) {
		if (const auto fallback = modules.all().find("system32-" + name); fallback != modules.all().end())
			return fallback->second.get();
		if (trace())
			std::cerr << "raven: library file not found: " << requested << '\n';
		environment.set_last_error(126);
		return nullptr;
	}
	// A file that is no program image at all, a type library tried as a resource module first, is ERROR_BAD_EXE_FORMAT.
	std::optional<PEImage> parsed;
	try {
		parsed.emplace(*bytes);
	} catch (const std::runtime_error &) {
		environment.set_last_error(193);
		return nullptr;
	}
	const PEImage &image = *parsed;
	if (image.is_64 || (!(image.characteristics & 0x2000) && !(mapping_only && staged.empty()))) {
		environment.set_last_error(193);
		return nullptr;
	}
	if (modules.all().contains(identity))
		identity += "#" + std::to_string(modules.all().size());
	auto &added = modules.add(identity, std::move(*bytes), [&](uint64_t base, uint64_t size) { return process.memory_available(base, size); });
	added.file = file;
	staged.push_back(&added);
	if (mapping_only && staged.size() == 1) {
		added.imports.clear();
		return &added;
	}
	std::string missing;
	// A library's own manifest names the side-by-side assemblies its imports come from, the C runtime of its compiler most often.
	// Its context is the one in effect while it and the libraries it brings in start, which the C runtime checks it was found by.
	const auto own = activation_contexts().module_manifest(added.image);
	const uint64_t context = own ? activation_contexts().create({*own, {}}) : importer_context;
	if (context)
		entry_contexts[&added] = context;
	const auto outer = importer_context;
	importer_context = context;
	for (const auto &symbol : added.imports) {
		const auto dependency = lowered(symbol.module);
		if (own && !modules.all().contains(dependency))
			if (const auto folder = activation_contexts().assembly_folder(*own, dependency))
				bring(lowered(to_utf8(paths.dos(*folder))) + "\\" + dependency);
		if (!modules.all().contains(dependency) && !bring(dependency)) {
			missing += " " + symbol.module + "!";
			continue;
		}
		try {
			modules.resolve(symbol);
		} catch (const std::runtime_error &) {
			missing += " " + symbol.module + "!" + (symbol.by_ordinal ? "#" + std::to_string(symbol.ordinal) : symbol.name);
		}
	}
	importer_context = outer;
	if (!missing.empty())
		throw std::runtime_error("unresolved imports in " + identity + ":" + missing);
	return &added;
}

std::optional<std::string> NativeX86Loader::path_key(const std::string &name) const
{
	std::optional<NativePaths::Resolved> resolved;
	if (name.find_first_of("/\\:") != std::string::npos)
		resolved = paths.resolve(std::u16string(name.begin(), name.end()));
	else if (!application_directory.empty())
		resolved = paths.find_library(application_directory + u"\\program.exe", from_utf8(name), false, library_flags);
	if (!resolved || !resolved->exists || !std::filesystem::is_regular_file(resolved->host))
		return std::nullopt;
	return lowered(resolved->host.string());
}

uint64_t NativeX86Loader::find(const std::string &name) const
{
	const auto key = path_key(name);
	const auto found = key ? held.find(*key) : held.end();
	return found == held.end() ? 0 : found->second.module->loaded.base();
}

bool NativeX86Loader::release(uint64_t handle)
{
	const auto found = std::find_if(held.begin(), held.end(), [&](const auto &item) { return item.second.module->loaded.base() == handle; });
	if (found == held.end())
		return false;
	if (--found->second.references)
		return true;
	const auto module = found->second.module;
	const bool mapped_only = found->second.mapped_only;
	held.erase(found);
	if (call && module->image.entry && !mapped_only) {
		const uint64_t arguments[] = {module->loaded.base(), 0, 0};
		const auto result = call(module->loaded.base() + module->image.entry, arguments);
		if (result.stop != NativeStop::returned)
			throw result;
	}
	return true;
}

uint64_t NativeX86Loader::load(std::string name, bool only_map, uint32_t search_flags)
{
	if (!call)
		return 0;
	const auto previous_flags = library_flags;
	library_flags = search_flags;
	struct RestoreFlags { uint32_t &value; uint32_t previous; ~RestoreFlags() { value = previous; } } restore{library_flags, previous_flags};
	const auto key = path_key(name);
	if (key)
		if (const auto found = held.find(*key); found != held.end()) {
			++found->second.references;
			return found->second.module->loaded.base();
		}
	staged.clear();
	entry_contexts.clear();
	importer_context = 0;
	mapping_only = only_map;
	const auto existing = modules.all().size();
	bool mapped = false;
	try {
		auto *root = bring(name);
		if (!root) {
			for (auto item = staged.rbegin(); item != staged.rend(); ++item)
				modules.remove(**item);
			return 0;
		}
		if (staged.empty())
			return root->loaded.base();
		modules.bind_imports();
		// Dependencies start before the library that needs them: the order the imports were followed, reversed.
		for (auto item = staged.rbegin(); item != staged.rend(); ++item) {
			process.map_image((*item)->loaded);
			mapped = true;
			if (mapped_hook)
				mapped_hook(**item);
			if (tls)
				tls->add(**item);
		}
		std::vector<PEModule *> attached;
		for (auto item = staged.rbegin(); item != staged.rend(); ++item) {
			if (tls && !(only_map && *item == root))
				for (const auto callback : tls->callbacks(**item)) {
					const uint64_t arguments[] = {(*item)->loaded.base(), 1, 0};
					const auto result = call(callback, arguments);
					if (result.stop != NativeStop::returned)
						throw result;
				}
			if (!(*item)->image.entry || (only_map && *item == root))
				continue;
			const uint64_t arguments[] = {(*item)->loaded.base(), 1, 0};
			auto &contexts = activation_contexts();
			const auto context = entry_contexts.find(*item);
			const uint32_t thread = contexts.current_thread ? contexts.current_thread() : 0;
			const auto cookie = context == entry_contexts.end() ? 0 : contexts.activate(thread, context->second);
			const auto result = call((*item)->loaded.base() + (*item)->image.entry, arguments);
			if (cookie)
				contexts.deactivate(thread, cookie);
			if (result.stop != NativeStop::returned)
				throw result;
			if (!uint32_t(result.value)) {
				// A library that refuses to start leaves the process with what it brought, as on Windows: the ones that started are told
				// they are detached, then every new image goes, so that the next load of that name starts it again.
				for (auto done = attached.rbegin(); done != attached.rend(); ++done) {
					const uint64_t detach[] = {(*done)->loaded.base(), 0, 0};
					const auto detached = call((*done)->loaded.base() + (*done)->image.entry, detach);
					if (detached.stop != NativeStop::returned)
						throw detached;
				}
				for (auto gone = staged.rbegin(); gone != staged.rend(); ++gone) {
					if (tls)
						tls->remove(**gone);
					if (unmapped_hook)
						unmapped_hook(**gone);
					process.unmap_image((*gone)->loaded.base());
					modules.remove(**gone);
				}
				environment.set_last_error(1114);
				return 0;
			}
			attached.push_back(*item);
		}
		if (key)
			held[*key] = {root, 1, only_map};
		return root->loaded.base();
	} catch (const NativeResult &) {
		throw;
	} catch (const std::exception &failure) {
		if (trace())
			std::cerr << "raven: cannot load " << name << ": " << failure.what() << '\n';
		// An image that never reached the guest must not stay in the set, or every later import binding would trip over it.
		if (!mapped)
			for (auto item = staged.rbegin(); item != staged.rend(); ++item)
				modules.remove(**item);
		(void)existing;
		environment.set_last_error(126);
		return 0;
	}
}

}
