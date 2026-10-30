#include "apiset.h"
#include "loader.h"
#include "memory.h"
#include "system_exports.h"

#include <algorithm>
#include <stdexcept>

namespace raven {

void validate_native_image(const PEImage &image,std::string_view name,bool is_64)
{
	if (image.is_64!=is_64) throw std::runtime_error(is_64 ? "native PE32 execution is not implemented" : "native x86 process cannot mix PE64 modules: "+std::string(name));
	// Managed code is the runtime's to start; a runtime header that does not hold IMAGE_COR20_HEADER is no image Windows would load.
	const auto &runtime=image.directories[14];
	if ((runtime.address || runtime.size) && (!runtime.address || runtime.size<72))
		throw std::runtime_error("native image has a malformed runtime header: "+std::string(name));
	if (image.directories[13].address || image.directories[13].size)
		for (const auto &descriptor:image.delay_imports())
			if (!(image.file_span(descriptor.descriptor_address,4)[0] & 1))
				throw std::runtime_error("native legacy-VA delay helper execution is not implemented: "+std::string(name));
}

uint64_t program_start(const ModuleSet &modules, const PEModule &entry)
{
	if (entry.image.directories[14].address && entry.image.directories[14].size)
		return modules.resolve(PEImport{"mscoree.dll", "_CorExeMain", 0, false, 0});
	return entry.loaded.base() + entry.image.entry;
}

void load_initial_dependencies(ModuleSet &modules,
	const std::function<std::optional<ModuleBytes>(std::string_view)> &source,
	std::span<const std::string_view> native_modules,bool is_64,bool tolerate_missing)
{
	if (!source) return;
	std::vector<PEModule *> pending,staged;
	for (const auto &[name,module]:modules.all()) pending.push_back(module.get());
	try {
		for (size_t index=0;index<pending.size();++index) {
			// A .NET image starts in the runtime's shim, which the loader brings in whether or not the image imports it, as Windows does
			// for programs whose native imports it leaves out.
			auto symbols=pending[index]->imports;
			if (pending[index]->image.directories[14].address && pending[index]->image.directories[14].size)
				symbols.push_back(PEImport{"mscoree.dll","_CorExeMain",0,false,0});
			for (const auto &symbol:symbols)
				for (;;) {
					try { modules.resolve(symbol);break; }
					catch (const std::runtime_error &error) {
						const std::string_view message=error.what();
						if (!message.starts_with("missing PE module: ")) throw;
						const std::string identity(message.substr(19));
						if (std::find(native_modules.begin(),native_modules.end(),identity)!=native_modules.end()) break;
						// An API set name stands for the system library that implements it; that library is served natively, and alias_api_sets
						// ties the name to it once the runtime has made it.
						if (const auto host=apiset_host(identity); !host.empty() && host!="ucrtbase.dll") {
							const auto system_names=system_library_names();
							if (std::find(native_modules.begin(),native_modules.end(),host)!=native_modules.end() ||
								std::find(system_names.begin(),system_names.end(),host)!=system_names.end()) break;
						}
						// A C runtime contract resolves through the runtime component, whichever contract version the module names.
						if (apiset_host(identity)=="ucrtbase.dll") {
							if (!modules.all().contains("ucrtbase.dll")) {
								auto bytes=source("ucrtbase.dll");
								if (!bytes) throw;
								const PEImage image(*bytes);validate_native_image(image,"ucrtbase.dll",is_64);
								staged.reserve(staged.size()+1);pending.reserve(pending.size()+1);
								auto &module=modules.add("ucrtbase.dll",std::move(*bytes));
								staged.push_back(&module);pending.push_back(&module);
							}
							modules.add_alias(identity,"ucrtbase.dll");
							continue;
						}
						if (modules.all().size()>=10000) throw std::runtime_error("native DLL graph exceeds module limit");
						auto bytes=source(identity);
						if (!bytes) {
							const auto system_names=system_library_names();
							if (tolerate_missing || std::find(system_names.begin(),system_names.end(),identity)!=system_names.end()) break;
							throw;
						}
						const PEImage image(*bytes);validate_native_image(image,identity,is_64);
						if (!(image.characteristics & 0x2000)) throw std::runtime_error("native startup dependency must be a DLL: "+identity);
						staged.reserve(staged.size()+1);pending.reserve(pending.size()+1);
						auto &module=modules.add(identity,std::move(*bytes));
						staged.push_back(&module);pending.push_back(&module);
					}
				}
		}
	} catch (...) {
		for (auto item=staged.rbegin();item!=staged.rend();++item) modules.remove(**item);
		throw;
	}
}

void alias_api_sets(ModuleSet &modules)
{
	std::vector<std::string> names;
	for (const auto &[name,module]:modules.all())
		for (const auto &symbol:module->imports) {
			auto identity=symbol.module;
			for (auto &character : identity) if (character>='A' && character<='Z') character=char(character+('a'-'A'));
			if (identity.find('.')==identity.npos) identity+=".dll";
			if (!modules.all().contains(identity)) names.push_back(identity);
		}
	for (const auto &identity:names) {
		const auto host=apiset_host(identity);
		if (!host.empty() && modules.all().contains(host) && !modules.all().contains(identity))
			modules.add_alias(identity,host);
	}
}

void forward_opengl_formats(ModuleSet &modules, PEModule &provider)
{
	if (provider.name != "gdi32.dll" || !provider.runtime_api)
		return;
	const auto backend = modules.all().find("opengl32.dll");
	if (backend == modules.all().end() || backend->second->runtime_api)
		return;
	for (const char *name : {"ChoosePixelFormat", "DescribePixelFormat", "GetPixelFormat", "SetPixelFormat", "SwapBuffers"}) {
		const std::string target = std::string("wgl") + name;
		const auto symbol = provider.export_names.find(name);
		if (symbol != provider.export_names.end() && backend->second->export_names.contains(target))
			provider.exports.at(symbol->second).forwarder = "opengl32." + target;
	}
}

void register_image(NativeMemory &memory, const PEModule &module)
{
	std::vector<std::pair<std::pair<uint64_t,uint64_t>,uint32_t>> sections;
	sections.push_back({{0,module.image.headers_size},2});
	for (const auto &section:module.image.sections) {
		const bool execute=section.characteristics & 0x20000000U, write=section.characteristics & 0x80000000U;
		sections.push_back({{section.address,std::max<uint32_t>(section.virtual_size,section.file_size)},execute ? (write ? 0x40U : 0x20U) : write ? 0x04U : 0x02U});
	}
	memory.add_image(module.loaded.base(),module.image.image_size,sections);
}

void register_images(NativeMemory &memory, const ModuleSet &modules)
{
	for (const auto &[name,module]:modules.all())
		register_image(memory,*module);
}

}
