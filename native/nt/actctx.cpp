#include "actctx.h"
#include "text.h"
#include "pe/resources.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace raven {

namespace {

std::string lowered(std::string_view text)
{
	std::string result(text);
	for (auto &character : result)
		if (character >= 'A' && character <= 'Z')
			character = char(character + ('a' - 'A'));
	return result;
}

// The manifest text as bytes of ASCII and UTF-8: UTF-16 manifests, with or without a byte order mark, are narrowed.
std::string manifest_text(std::string_view bytes)
{
	if (bytes.size() >= 3 && uint8_t(bytes[0]) == 0xef && uint8_t(bytes[1]) == 0xbb && uint8_t(bytes[2]) == 0xbf)
		return std::string(bytes.substr(3));
	const bool little = bytes.size() >= 2 && ((uint8_t(bytes[0]) == 0xff && uint8_t(bytes[1]) == 0xfe) || (bytes[0] == '<' && bytes[1] == 0));
	if (!little)
		return std::string(bytes);
	std::string narrow;
	for (size_t at = uint8_t(bytes[0]) == 0xff ? 2 : 0; at + 1 < bytes.size(); at += 2)
		narrow.push_back(bytes[at + 1] ? '?' : bytes[at]);
	return narrow;
}

// Versions compare component by component as numbers.
std::vector<uint32_t> version_parts(std::string_view version)
{
	std::vector<uint32_t> parts;
	uint32_t value = 0;
	for (const char character : version) {
		if (character == '.') {
			parts.push_back(value);
			value = 0;
		} else if (character >= '0' && character <= '9') {
			value = value * 10 + uint32_t(character - '0');
		}
	}
	parts.push_back(value);
	return parts;
}

// The window classes comctl32 6 declares, which every prefix keeps in its side-by-side store as Windows does.
constexpr const char *common_control_classes[] = {"Button", "ButtonListBox", "ComboBoxEx32", "ComboLBox", "Combobox", "Edit", "Listbox",
	"NativeFontCtl", "ReBarWindow32", "ScrollBar", "Static", "SysAnimate32", "SysDateTimePick32", "SysHeader32", "SysIPAddress32", "SysLink",
	"SysListView32", "SysMonthCal32", "SysPager", "SysTabControl32", "SysTreeView32", "ToolbarWindow32", "msctls_hotkey32", "msctls_progress32",
	"msctls_statusbar32", "msctls_trackbar32", "msctls_updown32", "tooltips_class32"};

std::string common_controls_manifest(std::string_view architecture, std::string_view version)
{
	std::string text = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
		"<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
		"  <assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\" version=\"" + std::string(version) +
		"\" processorArchitecture=\"" + std::string(architecture) + "\" publicKeyToken=\"6595b64144ccf1df\"/>\n  <file name=\"comctl32.dll\">\n";
	for (const auto *name : common_control_classes)
		text += "    <windowClass>" + std::string(name) + "</windowClass>\n";
	return text + "  </file>\n</assembly>\n";
}

}

ActivationContexts &activation_contexts()
{
	static ActivationContexts contexts;
	return contexts;
}

void ActivationContexts::attach(KernelContext &context)
{
	kernel = &context;
	wide_process = context.entry.image.is_64;
	current_thread = [&context] { return context.threads.current_id(); };
	// The common controls' manifests: comctl32 creates its contexts from them and registers its version 6 classes inside.
	if (const auto store = context.paths.resolve(u"C:\\Windows\\WinSxS\\Manifests")) {
		manifests = store->host;
		std::error_code error;
		std::filesystem::create_directories(store->host, error);
		for (const auto *architecture : {"amd64", "x86"})
			for (const auto *version : {"6.0.2600.2982", "5.82.2600.2982"}) {
				const auto file = store->host / (std::string(architecture) + "_microsoft.windows.common-controls_6595b64144ccf1df_" + version +
					"_none_deadbeef.manifest");
				if (!std::filesystem::exists(file, error))
					std::ofstream(file) << common_controls_manifest(architecture, version);
			}
	}
	// The program's own manifest, resource 1 of type RT_MANIFEST, is the context in effect when no other is active.
	const auto &image = context.entry;
	const auto directory = image.image.directories[2];
	if (!directory.address || !directory.size || directory.size > image.image.image_size)
		return;
	try {
		const auto resources = read_resources(directory, context.process.read_memory(image.loaded.base() + directory.address, directory.size),
			image.image.image_size);
		for (const auto &resource : resources)
			if (resource.type == ResourceKey(uint32_t(24)) && resource.name == ResourceKey(uint32_t(1)) && resource.size < 1024 * 1024) {
				const auto bytes = context.process.read_memory(image.loaded.base() + resource.data_address, resource.size);
				if (auto parsed = parse(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()), false))
					process_default = create(Context{std::move(*parsed), context.launch.image_path});
				break;
			}
	} catch (...) {
	}
}

std::optional<ActivationContexts::Assembly> ActivationContexts::parse(std::string_view bytes, bool) const
{
	const auto text = manifest_text(bytes);
	Assembly root;
	bool identified = false, in_dependency = false, in_file = false, assembly = false;
	size_t at = 0;
	while ((at = text.find('<', at)) != text.npos) {
		if (text.compare(at, 4, "<!--") == 0) {
			at = text.find("-->", at);
			if (at == text.npos)
				break;
			continue;
		}
		const auto end = text.find('>', at);
		if (end == text.npos)
			break;
		std::string_view tag(text.data() + at + 1, end - at - 1);
		at = end + 1;
		if (tag.empty() || tag[0] == '?' || tag[0] == '!')
			continue;
		const bool closing = tag[0] == '/';
		if (closing)
			tag.remove_prefix(1);
		const bool self_closing = !tag.empty() && tag.back() == '/';
		size_t name_end = 0;
		while (name_end < tag.size() && tag[name_end] != ' ' && tag[name_end] != '\t' && tag[name_end] != '\r' && tag[name_end] != '\n' &&
			tag[name_end] != '/')
			++name_end;
		auto element = lowered(tag.substr(0, name_end));
		if (const auto colon = element.find(':'); colon != element.npos)
			element = element.substr(colon + 1);
		// Attributes are name="value" or name='value'.
		std::map<std::string, std::string> attributes;
		for (size_t scan = name_end; scan < tag.size();) {
			while (scan < tag.size() && (tag[scan] == ' ' || tag[scan] == '\t' || tag[scan] == '\r' || tag[scan] == '\n'))
				++scan;
			const auto equals = tag.find('=', scan);
			if (equals == tag.npos)
				break;
			auto key = lowered(tag.substr(scan, equals - scan));
			while (!key.empty() && key.back() == ' ')
				key.pop_back();
			size_t open = equals + 1;
			while (open < tag.size() && tag[open] == ' ')
				++open;
			if (open >= tag.size() || (tag[open] != '"' && tag[open] != '\''))
				break;
			const auto close = tag.find(tag[open], open + 1);
			if (close == tag.npos)
				break;
			attributes[key] = std::string(tag.substr(open + 1, close - open - 1));
			scan = close + 1;
		}
		if (element == "assembly" && !closing)
			assembly = true;
		if (element == "dependentassembly")
			in_dependency = !closing && !self_closing;
		else if (element == "file") {
			in_file = !closing && !self_closing;
			if (!closing && attributes.contains("name"))
				root.files.push_back(attributes["name"]);
		} else if (element == "assemblyidentity" && !closing) {
			Assembly identity{attributes["name"], attributes["version"], lowered(attributes["processorarchitecture"]), lowered(attributes["publickeytoken"]),
				{}, {}, {}, {}, {}};
			if (in_dependency) {
				if (auto resolved = resolve(identity))
					root.dependencies.push_back(std::move(*resolved));
			} else if (!identified) {
				identified = true;
				root.name = identity.name;
				root.version = identity.version;
				root.architecture = identity.architecture;
				root.token = identity.token;
			}
		} else if (element == "supportedos" && !closing && attributes.contains("id")) {
			root.supported_os.push_back(lowered(attributes["id"]));
		} else if (element == "windowclass" && !closing && !self_closing && in_file && !in_dependency) {
			const auto close = text.find('<', at);
			if (close == text.npos)
				break;
			std::string name = text.substr(at, close - at);
			while (!name.empty() && (name.back() == ' ' || name.back() == '\n' || name.back() == '\r' || name.back() == '\t'))
				name.pop_back();
			while (!name.empty() && (name.front() == ' ' || name.front() == '\n' || name.front() == '\r' || name.front() == '\t'))
				name.erase(name.begin());
			if (!name.empty())
				root.classes[lowered(name)] = lowered(attributes["versioned"]) != "no";
		}
	}
	// A program's manifest may leave out its own identity and still say which versions of Windows it supports.
	if (!assembly || (!identified && root.dependencies.empty() && root.supported_os.empty() && root.classes.empty()))
		return std::nullopt;
	return root;
}

// An assembly a manifest depends on: the manifest in the side-by-side store with its name, key and architecture, at the highest version
// that keeps the major version asked for, as the publisher policies of the system assemblies redirect.
std::optional<ActivationContexts::Assembly> ActivationContexts::resolve(const Assembly &identity) const
{
	std::error_code error;
	// A private assembly (no public key token) lives with the program, as Windows looks for it: <name>.manifest in the program's
	// folder, then <name>\<name>.manifest; its files are beside its manifest.
	if (identity.token.empty() && !identity.name.empty() && !application.empty())
		for (const auto &place : {application / (identity.name + ".manifest"), application / identity.name / (identity.name + ".manifest")}) {
			if (!std::filesystem::is_regular_file(place, error))
				continue;
			std::ifstream input(place, std::ios::binary);
			const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			auto candidate = parse(bytes, false);
			if (!candidate || lowered(candidate->name) != lowered(identity.name))
				continue;
			candidate->folder = place.parent_path().string();
			return candidate;
		}
	if (manifests.empty() || identity.name.empty() || !std::filesystem::is_directory(manifests, error))
		return std::nullopt;
	const std::string architecture = identity.architecture.empty() || identity.architecture == "*" ? (wide_process ? "amd64" : "x86") : identity.architecture;
	const auto wanted = version_parts(identity.version);
	std::optional<Assembly> best;
	for (const auto &entry : std::filesystem::directory_iterator(manifests, error)) {
		const auto file = lowered(entry.path().filename().string());
		if (!file.ends_with(".manifest") || !file.starts_with(architecture + "_" + lowered(identity.name) + "_"))
			continue;
		std::ifstream input(entry.path(), std::ios::binary);
		const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		auto candidate = parse(bytes, false);
		if (!candidate || lowered(candidate->name) != lowered(identity.name) || (!identity.token.empty() && candidate->token != identity.token))
			continue;
		const auto version = version_parts(candidate->version);
		if (!wanted.empty() && wanted[0] && version[0] != wanted[0])
			continue;
		if (!best || version > version_parts(best->version)) {
			candidate->folder = entry.path().stem().string();
			best = std::move(candidate);
		}
	}
	return best;
}

void ActivationContexts::prepare(const std::filesystem::path &store, bool wide, std::string_view program_manifest,
	const std::filesystem::path &application_folder)
{
	manifests = store;
	wide_process = wide;
	application = application_folder;
	program = program_manifest.empty() ? std::nullopt : parse(program_manifest, false);
}

std::filesystem::path ActivationContexts::folder_of(const Assembly &assembly) const
{
	const std::filesystem::path folder(assembly.folder);
	return folder.is_absolute() ? folder : manifests.parent_path() / folder;
}

const ActivationContexts::Assembly *ActivationContexts::declaring(const Assembly &root, std::string_view name, uint32_t &index) const
{
	const auto wanted = lowered(name);
	for (const auto &dependency : root.dependencies) {
		++index;
		if (!dependency.folder.empty())
			for (const auto &file : dependency.files)
				if (lowered(file) == wanted)
					return &dependency;
		if (const auto *found = declaring(dependency, name, index))
			return found;
	}
	return nullptr;
}

std::optional<std::filesystem::path> ActivationContexts::library_folder(std::string_view name) const
{
	const Assembly *root = program ? &*program : nullptr;
	if (kernel && current_thread) {
		const auto context = contexts.find(current(current_thread()));
		root = context == contexts.end() ? nullptr : &context->second.root;
	}
	uint32_t index = 1;
	if (const auto *assembly = root ? declaring(*root, name, index) : nullptr)
		return folder_of(*assembly);
	return std::nullopt;
}

std::optional<ActivationContexts::Assembly> ActivationContexts::module_manifest(const PEImage &image) const
{
	const auto directory = image.directories[2];
	if (!directory.address || !directory.size || directory.size > image.image_size)
		return std::nullopt;
	try {
		for (const auto &resource : read_resources(directory, image.file_span(directory.address, directory.size), image.image_size))
			if (resource.type == ResourceKey(uint32_t(24)) && resource.name == ResourceKey(uint32_t(2)) && resource.size < 1024 * 1024) {
				const auto bytes = image.file_span(resource.data_address, resource.size);
				return parse(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()), false);
			}
	} catch (const std::runtime_error &) {
	}
	return std::nullopt;
}

std::optional<std::filesystem::path> ActivationContexts::assembly_folder(const Assembly &root, std::string_view name) const
{
	uint32_t index = 1;
	if (const auto *assembly = declaring(root, name, index))
		return folder_of(*assembly);
	return std::nullopt;
}

std::optional<uint32_t> ActivationContexts::library_roster(uint64_t handle, std::string_view name) const
{
	const auto context = contexts.find(handle);
	uint32_t index = 1;
	if (context != contexts.end() && declaring(context->second.root, name, index))
		return index;
	return std::nullopt;
}

void ActivationContexts::set_compatibility_layer(std::u16string_view layers)
{
	// The layers are names separated by spaces, after an optional "~" or "!"; the first that names a version of Windows decides.
	static const std::pair<std::string_view, Version> versions[] = {{"WIN2000", {5, 0, 2195}}, {"WINXP", {5, 1, 2600}},
		{"WINXPSP1", {5, 1, 2600}}, {"WINXPSP2", {5, 1, 2600}}, {"WINXPSP3", {5, 1, 2600}}, {"WINSRV03SP1", {5, 2, 3790}},
		{"WINSRV03SP2", {5, 2, 3790}}, {"VISTARTM", {6, 0, 6000}}, {"VISTASP1", {6, 0, 6001}}, {"VISTASP2", {6, 0, 6002}},
		{"WIN7RTM", {6, 1, 7600}}, {"WIN7SP1", {6, 1, 7601}}, {"WIN8RTM", {6, 2, 9200}}, {"WIN81RTM", {6, 3, 9600}}};
	std::string name;
	const auto decide = [&] {
		for (const auto &[layer, version] : versions)
			if (name == layer && !layer_version)
				layer_version = version;
		name.clear();
	};
	for (const auto character : layers) {
		if (character == u' ' || character == u'\t') {
			decide();
			continue;
		}
		if ((character == u'~' || character == u'!') && name.empty())
			continue;
		name.push_back(character >= u'a' && character <= u'z' ? char(character - 32) : char(character < 128 ? character : '?'));
	}
	decide();
}

ActivationContexts::Version ActivationContexts::reported_version() const
{
	if (layer_version)
		return *layer_version;
	const auto context = contexts.find(process_default);
	if (context != contexts.end()) {
		const auto &supported = context->second.root.supported_os;
		const auto names = [&](std::string_view id) { return std::find(supported.begin(), supported.end(), id) != supported.end(); };
		if (names("{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"))
			return {10, 0, 19045};
		if (names("{1f676c76-80e1-4239-95bb-83d0f6d0da78}"))
			return {6, 3, 9600};
	}
	return {6, 2, 9200};
}

uint64_t ActivationContexts::create(Context context)
{
	const auto handle = next_handle++;
	contexts.emplace(handle, std::move(context));
	return handle;
}

ActivationContexts::Context *ActivationContexts::find(uint64_t handle)
{
	const auto found = contexts.find(handle);
	return found == contexts.end() ? nullptr : &found->second;
}

void ActivationContexts::release(uint64_t handle)
{
	if (auto *context = find(handle); context && handle != process_default && !--context->references)
		contexts.erase(handle);
}

uint64_t ActivationContexts::current(uint32_t thread) const
{
	if (const auto found = stacks.find(thread); found != stacks.end() && !found->second.empty())
		return found->second.back().first;
	return process_default;
}

uint64_t ActivationContexts::activate(uint32_t thread, uint64_t handle)
{
	const auto cookie = next_cookie++;
	stacks[thread].emplace_back(handle, cookie);
	return cookie;
}

bool ActivationContexts::deactivate(uint32_t thread, uint64_t cookie)
{
	auto &stack = stacks[thread];
	const auto found = std::find_if(stack.begin(), stack.end(), [&](const auto &frame) { return frame.second == cookie; });
	if (found == stack.end())
		return false;
	stack.erase(found, stack.end());
	return true;
}

std::optional<std::string> ActivationContexts::registered_name(uint32_t thread, std::string_view name) const
{
	const auto found = stacks.find(thread);
	if (found == stacks.end() || found->second.empty())
		return std::nullopt;
	const auto context = contexts.find(found->second.back().first);
	if (context == contexts.end())
		return std::nullopt;
	const auto &root = context->second.root;
	// Only version 6 and later carry their version in class names; the classes of earlier assemblies keep their plain names.
	const auto declared = root.classes.find(lowered(name));
	if (declared == root.classes.end() || !declared->second || version_parts(root.version)[0] < 6)
		return std::nullopt;
	return root.version + "!" + std::string(name);
}

std::optional<std::string> ActivationContexts::lookup_name(uint32_t thread, std::string_view name) const
{
	const auto context = contexts.find(current(thread));
	if (context == contexts.end())
		return std::nullopt;
	const auto key = lowered(name);
	const auto in = [&](const Assembly &assembly) -> std::optional<std::string> {
		const auto declared = assembly.classes.find(key);
		if (declared == assembly.classes.end() || !declared->second || version_parts(assembly.version)[0] < 6)
			return std::nullopt;
		return assembly.version + "!" + std::string(name);
	};
	if (auto own = in(context->second.root))
		return own;
	for (const auto &dependency : context->second.root.dependencies)
		if (auto found = in(dependency))
			return found;
	return std::nullopt;
}

void register_actctx_api(ApiRegistry &registry, KernelContext &context)
{
	auto &contexts = activation_contexts();
	contexts.attach(context);
	const auto thread = [&context] { return context.threads.current_id(); };
	// CreateActCtxW(ACTCTXW): a manifest file, or a manifest resource of a module named by path or handle. CreateActCtxA takes the same
	// structure with its source path in the ANSI code page.
	const auto create = [&context, &contexts](ApiCall &call, bool ansi) {
		const bool narrow = call.pointer_width() == 4;
		const size_t size = narrow ? 32 : 56;
		if (!call[0]) {
			call.fail(87, UINT64_MAX);
			return;
		}
		const auto bytes = call.read(call[0], size);
		const auto word = [&](size_t at, size_t width) {
			uint64_t value = 0;
			std::memcpy(&value, bytes.data() + at, width);
			return value;
		};
		const uint32_t flags = uint32_t(word(4, 4));
		const uint64_t source = word(narrow ? 8 : 8, narrow ? 4 : 8);
		const uint64_t resource_name = word(narrow ? 20 : 32, narrow ? 4 : 8);
		const uint64_t module = word(narrow ? 28 : 48, narrow ? 4 : 8);
		std::string manifest;
		std::u16string path = !source ? std::u16string() : ansi ? from_ansi(call.narrow(source, 1024), 1252) : call.wide(source, 1024);
		const bool from_resource = (flags & 0x8) || (flags & 0x80);
		if (from_resource) {
			// The resource of a loaded module, read from its image; RT_MANIFEST with the name asked for, else the first one.
			const PEModule *image = nullptr;
			if (flags & 0x80) {
				if (module == context.entry.loaded.base())
					image = &context.entry;
				for (const auto &[name, loaded] : context.modules.all())
					if (!image && loaded->loaded.base() == module)
						image = loaded.get();
			} else {
				const auto file = path.substr(path.find_last_of(u"\\/") == path.npos ? 0 : path.find_last_of(u"\\/") + 1);
				std::string key;
				for (const auto character : file)
					key.push_back(char(character < 128 ? std::tolower(int(character)) : '?'));
				if (const auto found = context.modules.all().find(key); found != context.modules.all().end())
					image = found->second.get();
				else if (context.launch.image_path == path)
					image = &context.entry;
			}
			if (image && image->image.directories[2].address) {
				const auto directory = image->image.directories[2];
				const auto resources = read_resources(directory, call.read(image->loaded.base() + directory.address, directory.size), image->image.image_size);
				for (const auto &resource : resources) {
					if (resource.type != ResourceKey(uint32_t(24)))
						continue;
					if ((flags & 0x8) && resource_name <= 0xffff && resource.name != ResourceKey(uint32_t(resource_name)))
						continue;
					const auto data = call.read(image->loaded.base() + resource.data_address, resource.size);
					manifest.assign(reinterpret_cast<const char *>(data.data()), data.size());
					break;
				}
			}
		} else if (const auto resolved = context.paths.resolve(path); resolved && resolved->exists) {
			std::ifstream input(resolved->host, std::ios::binary);
			manifest.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		} else {
			call.fail(2, UINT64_MAX);
			return;
		}
		auto parsed = manifest.empty() ? std::nullopt : contexts.parse(manifest, false);
		if (!parsed) {
			call.fail(14001, UINT64_MAX);
			return;
		}
		const auto handle = contexts.create({std::move(*parsed), path});
		if (flags & 0x10)
			contexts.set_process_default(handle);
		call.ret(handle);
	};
	registry.add("CreateActCtxW", 1, [create](ApiCall &call) { create(call, false); });
	registry.add("CreateActCtxA", 1, [create](ApiCall &call) { create(call, true); });
	registry.add("AddRefActCtx", 1, [&contexts](ApiCall &call) {
		if (auto *found = contexts.find(call[0]))
			++found->references;
	});
	registry.add("ReleaseActCtx", 1, [&contexts](ApiCall &call) { contexts.release(call[0]); });
	registry.add("ZombifyActCtx", 1, [](ApiCall &call) { call.ret(1); });
	registry.add("ActivateActCtx", 2, [&contexts, thread](ApiCall &call) {
		if (call[0] && call[0] != UINT64_MAX && !contexts.find(call[0])) {
			call.fail(6);
			return;
		}
		const auto cookie = contexts.activate(thread(), call[0] == UINT64_MAX ? 0 : call[0]);
		if (call[1])
			call.store_pointer(call[1], cookie);
		call.ret(1);
	});
	registry.add("DeactivateActCtx", 2, [&contexts, thread](ApiCall &call) {
		if (!contexts.deactivate(thread(), call[1]) && call[1]) {
			call.fail(87);
			return;
		}
		call.ret(1);
	});
	registry.add("GetCurrentActCtx", 1, [&contexts, thread](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		const auto handle = contexts.current(thread());
		if (auto *found = contexts.find(handle))
			++found->references;
		call.store_pointer(call[0], handle);
		call.ret(1);
	});
	// QueryActCtxW answers the basic information: the context and its flags. The detailed forms report that there is nothing to describe.
	registry.add("QueryActCtxW", 7, [&contexts, thread](ApiCall &call) {
		const uint32_t flags = call.u32(0), information = call.u32(3);
		uint64_t handle = call[1];
		if (flags & 4)
			handle = contexts.current(thread());
		// A module handle or an address in one asks for the context of that module; modules share the process's here.
		else if (flags & 0x18)
			handle = contexts.process_default_context();
		if (information != 1) {
			call.fail(87);
			return;
		}
		const size_t width = call.pointer_width(), needed = width * 2;
		if (call[6])
			call.store_pointer(call[6], needed);
		if (!call[4] || call[5] < needed) {
			call.fail(122);
			return;
		}
		call.store_pointer(call[4], handle);
		call.store_pointer(call[4] + width, 0);
		call.ret(1);
	});
	// FindActCtxSectionStringW answers the library redirection section: a library an assembly of the context in effect declares, with the
	// context and the assembly's place in it. The other sections have no entries.
	registry.add("FindActCtxSectionStringW", 5, [&context, &contexts, thread, redirection = std::make_shared<uint64_t>(0)](ApiCall &call) {
		constexpr uint32_t library_section = 2, return_context = 1;
		const auto handle = contexts.current(thread());
		const auto index = call.u32(2) == library_section && call[3] && call[4] ? contexts.library_roster(handle, to_utf8(call.wide(call[3], 260)))
			: std::nullopt;
		if (!index) {
			call.fail(14007);
			return;
		}
		// The entry is an ACTIVATION_CONTEXT_DATA_DLL_REDIRECTION without a path: the library is in its assembly's folder.
		if (!*redirection) {
			*redirection = context.allocator.allocate(20);
			call.store<uint32_t>(*redirection, 20);
		}
		const size_t width = call.pointer_width();
		const size_t size = std::min<size_t>(call.load<uint32_t>(call[4]), width == 4 ? 64 : 112);
		if (size > 4)
			call.write(call[4] + 4, std::vector<uint8_t>(size - 4, 0));
		call.store<uint32_t>(call[4] + 4, 1);
		call.store_pointer(call[4] + 8, *redirection);
		call.store<uint32_t>(call[4] + 8 + width, 20);
		if (call.u32(0) & return_context) {
			call.store_pointer(call[4] + 8 + width * 6, handle);
			if (auto *found = contexts.find(handle))
				++found->references;
		}
		call.store<uint32_t>(call[4] + 8 + width * 7, *index);
		call.ret(1);
	});
	registry.add("FindActCtxSectionGuid", 5, [](ApiCall &call) { call.fail(14007); });
}

}
