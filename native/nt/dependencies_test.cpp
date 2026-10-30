#include <string>
#include "loader.h"
#include "providers.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void check(bool value,const std::string &message)
{
	if (!value) throw std::runtime_error(message);
}

static std::vector<uint8_t> read_file(const std::filesystem::path &path)
{
	std::ifstream file(path,std::ios::binary|std::ios::ate);
	check(bool(file) && file.tellg()>0 && file.tellg()<1024*1024,"invalid startup fixture");
	std::vector<uint8_t> bytes(size_t(file.tellg()));file.seekg(0);
	check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short startup fixture");return bytes;
}

int main(int argc,char **argv)
{
	try {
		check(argc==3 || argc==4,"bootstrap, lifecycle directory and optional PE32 fixture required");
		const std::filesystem::path directory=argv[2];
		const auto client=read_file(directory/"client.exe"),base=read_file(directory/"base.dll"),dependent=read_file(directory/"dependent.dll");
		const std::array<std::string_view,1> native{"kernel32.dll"};
		for (const bool supplied:{false,true}) for (const bool mapped:{false,true}) {
			raven::ModuleSet modules;auto &entry=modules.add("client.exe",client);
			raven::PEModule *existing=nullptr;if (supplied) existing=&modules.add("base.dll",base);
			std::vector<std::string> queries;
			const auto source=[&](std::string_view name)->std::optional<raven::ModuleBytes> {
				queries.emplace_back(name);
				if (mapped && (name=="base.dll" || name=="dependent.dll")) return raven::ModuleBytes::map(directory/name);
				if (name=="base.dll") return base;
				if (name=="dependent.dll") return dependent;
				throw std::runtime_error("startup queried a native or unrelated module");
			};
			const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(3),source);
			check(result.stop==raven::NativeStop::returned && !result.value,"implicit startup or dependency initialization order differs");
			check(queries.size()==(supplied ? 1 : 2) && modules.all().contains("base.dll") && modules.all().contains("dependent.dll"),"startup queried a dependency more than once");
			if (supplied) check(modules.all().at("base.dll").get()==existing && std::ranges::equal(std::span<const uint8_t>(existing->bytes),base),"startup replaced an explicitly supplied module");
		}
		for (const std::string_view mode:{"missing","malformed","executable","clr","symbol","pe32"}) {
			if (mode=="pe32" && argc!=4) continue;
			raven::ModuleSet modules;auto &entry=modules.add("client.exe",client);const auto original=std::vector<uint8_t>(entry.loaded.data().begin(),entry.loaded.data().end());
			auto bad=dependent;
			if (mode=="malformed") bad.resize(10);
			if (mode=="executable") bad=client;
			if (mode=="pe32") bad=read_file(argv[3]);
			if (mode=="clr") {
				uint32_t header=0;for(unsigned i=0;i<4;++i)header|=uint32_t(bad[0x3c+i])<<(i*8);
				bad[header+24+112+14*8]=1;
			}
			if (mode=="symbol") {
				entry.imports.push_back({"dependent.dll","UnavailableExport",0,false,0});
			}
			std::string error;
			try {
				raven::load_initial_dependencies(modules,[&](std::string_view name)->std::optional<std::vector<uint8_t>> {
					if (name=="base.dll") return base;
					if (name=="dependent.dll" && mode!="missing") return bad;
					return std::nullopt;
				},native);
			} catch (const std::runtime_error &caught) { error=caught.what(); }
			check(!error.empty() && modules.all().size()==1 && std::equal(original.begin(),original.end(),entry.loaded.data().begin()),"failed initial acquisition retained images or changed the entry ("+std::string(mode)+": "+(error.empty() ? "no error" : error)+")");
			if (mode=="missing") check(error=="missing PE module: dependent.dll","missing startup failed for another reason");
			if (mode=="symbol") check(error.find("UnavailableExport")!=error.npos,"symbol startup failed for another reason");
			if (mode=="pe32") check(error.find("PE32")!=error.npos,"PE32 startup failed for another reason");
			if (mode=="clr") check(error.find("runtime header")!=error.npos,"CLR startup failed for another reason");
		}
		{
			raven::ModuleSet modules;auto &entry=modules.add("forward-client.exe",read_file(directory/"forward-client.exe"));
			std::vector<std::string> queries;
			const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(3),
				[&](std::string_view name)->std::optional<std::vector<uint8_t>> {
					queries.emplace_back(name);
					if (name=="forward.dll") return read_file(directory/"forward.dll");
					if (name=="base.dll") return base;
					throw std::runtime_error("inactive startup forwarder was loaded");
				});
			check(result.stop==raven::NativeStop::returned && !result.value && queries==std::vector<std::string>({"forward.dll","base.dll"}),"active forwarder acquisition differs");
		}
		{
			raven::ModuleSet modules;auto &entry=modules.add("forward-client.exe",read_file(directory/"forward-client.exe"));
			for (auto &symbol : entry.imports)
				if (symbol.module=="forward.dll") symbol.module="winmm.dll";
			std::vector<std::string> queries;
			const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(3),
				[&](std::string_view name)->std::optional<std::vector<uint8_t>> {
					queries.emplace_back(name);
					if (name=="winmm.dll") return read_file(directory/"forward.dll");
					if (name=="base.dll") return base;
					return std::nullopt;
				});
			check(result.stop==raven::NativeStop::returned && !result.value && queries==std::vector<std::string>({"winmm.dll","base.dll"}) &&
				modules.all().contains("winmm.dll") && !modules.all().at("winmm.dll")->runtime_api &&
				modules.all().contains("system32-winmm.dll") && modules.all().at("system32-winmm.dll")->runtime_api,
				"program's own winmm.dll or the system32 fallback failed");
		}
		// A system library off the list stays the runtime's whatever copy the program ships, unless RAVEN_PREFER_LOCAL_DLLS=1 asks for the program's.
		{
			const char *prefer=std::getenv("RAVEN_PREFER_LOCAL_DLLS");
			const bool prefer_local=prefer && std::string_view(prefer)=="1";
			raven::ModuleSet modules;auto &entry=modules.add("forward-client.exe",read_file(directory/"forward-client.exe"));
			for (auto &symbol : entry.imports)
				if (symbol.module=="forward.dll") symbol.module="winhttp.dll";
			std::vector<std::string> queries;
			bool returned=false;
			try {
				const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(3),
					[&](std::string_view name)->std::optional<std::vector<uint8_t>> {
						queries.emplace_back(name);
						if (name=="winhttp.dll") return read_file(directory/"forward.dll");
						if (name=="base.dll") return base;
						return std::nullopt;
					});
				returned=result.stop==raven::NativeStop::returned && !result.value;
			} catch (const std::exception &) {}
			if (prefer_local)
				check(returned && queries==std::vector<std::string>({"winhttp.dll","base.dll"}) &&
					modules.all().contains("winhttp.dll") && !modules.all().at("winhttp.dll")->runtime_api &&
					modules.all().contains("system32-winhttp.dll") && modules.all().at("system32-winhttp.dll")->runtime_api,
					"RAVEN_PREFER_LOCAL_DLLS=1 did not load the program's own winhttp.dll");
			else
				check(!returned && std::find(queries.begin(),queries.end(),"winhttp.dll")==queries.end(),
					"the program's own winhttp.dll replaced the runtime's");
		}
		std::cout<<"Implicit initial dependencies, explicit-module preservation, active forwarders and pre-execution rollback passed\n";
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n';return 1; }
	return 0;
}
