#pragma once

#include "kernel_context.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace raven {

// Side-by-side activation contexts: the assemblies a manifest names, the libraries and window classes each one declares, and the stack of
// contexts every thread has activated. The process starts with the context of the program's own manifest, which is in effect whenever a
// thread has activated nothing else.
class ActivationContexts {
public:
	struct Assembly {
		std::string name, version, architecture, token;
		// Window classes the assembly's libraries register, by lower-case name; true when the class name carries the version.
		std::map<std::string, bool> classes;
		std::vector<std::string> files;
		std::vector<Assembly> dependencies;
		// The operating systems the manifest's compatibility section names, as lower-case GUIDs.
		std::vector<std::string> supported_os;
		// The assembly's folder in the side-by-side store, named like its manifest there, or the absolute host folder of a private
		// assembly; empty for a program's own manifest.
		std::string folder;
	};
	struct Context {
		Assembly root;
		std::u16string source;
		uint32_t references = 1;
	};

	// The runtime services the contexts need: guest memory, the prefix's paths and which thread is calling.
	void attach(KernelContext &kernel);
	// Before the process exists, as its first libraries are found: the host folder of the side-by-side store's manifests, the
	// program's width and its own manifest.
	// Private assemblies, which carry no public key token, are looked for in the program's folder when it is given.
	void prepare(const std::filesystem::path &manifests, bool wide, std::string_view program_manifest,
		const std::filesystem::path &application_folder = {});
	// The side-by-side folder that serves a library named alone: an assembly the context in effect depends on, directly or through
	// another, declares the file. The loader looks there before any other place, as Windows redirects it.
	std::optional<std::filesystem::path> library_folder(std::string_view name) const;
	// The manifest a library carries for the libraries it imports itself, resource 2 of type RT_MANIFEST, and the side-by-side folder
	// an assembly it depends on serves a library from: the loader looks there for that library's imports, as Windows does.
	std::optional<Assembly> module_manifest(const PEImage &image) const;
	std::optional<std::filesystem::path> assembly_folder(const Assembly &root, std::string_view name) const;
	// The place in a context of the assembly that declares a library: 1 is the context's own manifest, its dependencies follow.
	std::optional<uint32_t> library_roster(uint64_t handle, std::string_view name) const;
	// Parses a manifest's text; dependencies resolve through the WinSxS manifests of the prefix for the process's architecture.
	std::optional<Assembly> parse(std::string_view text, bool wide) const;
	uint64_t create(Context context);
	Context *find(uint64_t handle);
	void release(uint64_t handle);
	// The context in effect on a thread: the last one it activated, else the program's own.
	uint64_t current(uint32_t thread) const;
	uint64_t activate(uint32_t thread, uint64_t handle);
	bool deactivate(uint32_t thread, uint64_t cookie);
	void set_process_default(uint64_t handle) { process_default = handle; }
	uint64_t process_default_context() const { return process_default; }
	// The name a class registers under while a thread has a context active: its version and an exclamation mark come first when the
	// context's assembly declares the class. Lookups ask for the name an assembly the current context depends on gives it.
	std::optional<std::string> registered_name(uint32_t thread, std::string_view name) const;
	std::optional<std::string> lookup_name(uint32_t thread, std::string_view name) const;
	// The version GetVersionEx and VerifyVersionInfo report to the process, as Windows 10 and later do: 10.0 for a program whose manifest
	// supports Windows 10, 6.3 for one that supports Windows 8.1, and 6.2 for every other program. A compatibility layer the process was
	// started with (__COMPAT_LAYER, as the compatibility engine reads it) reports the version of the Windows it names instead.
	struct Version { uint32_t major, minor, build; };
	Version reported_version() const;
	// The real version, which RtlGetVersion reports whatever the manifest says; a compatibility layer changes this one too, as it
	// rewrites the version fields of the process environment block that the call reads.
	Version system_version() const { return layer_version ? *layer_version : Version{10, 0, 19045}; }
	void set_compatibility_layer(std::u16string_view layers);
	std::function<uint32_t()> current_thread;
	bool wide_process = true;

private:
	KernelContext *kernel = nullptr;
	std::filesystem::path manifests, application;
	std::optional<Assembly> program;
	std::map<uint64_t, Context> contexts;
	std::map<uint32_t, std::vector<std::pair<uint64_t, uint64_t>>> stacks;
	uint64_t next_handle = 0x7e000001, next_cookie = 1, process_default = 0;
	std::optional<Version> layer_version;
	std::optional<Assembly> resolve(const Assembly &identity) const;
	// The host folder an assembly's files are in.
	std::filesystem::path folder_of(const Assembly &assembly) const;
	const Assembly *declaring(const Assembly &root, std::string_view name, uint32_t &index) const;
};

ActivationContexts &activation_contexts();
void register_actctx_api(ApiRegistry &registry, KernelContext &context);

}
