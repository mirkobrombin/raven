#include "nt/providers.h"
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

// A window that covers the screen asks the host for full screen, and leaves it once moved to a part of the screen; a window maximized
// with its caption is maximized, not shown full screen.
int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and full screen fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg()>0,"cannot read full screen fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short full screen fixture");
		raven::ModuleSet modules; auto &entry=modules.add("full-screen.exe",bytes);
		std::map<uint64_t,std::string> titles;
		std::vector<std::pair<uint64_t,bool>> asked;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view title,uint32_t,uint32_t,std::span<const uint8_t>) { titles[handle]=title; };
		presentation.remove=[&](uint64_t) {};
		presentation.screen_size=[&] { return std::optional<std::pair<int32_t,int32_t>>{{800,600}}; };
		presentation.maximize=[&](uint64_t,bool) {};
		presentation.full_screen=[&](uint64_t handle,bool wanted) { asked.emplace_back(handle,wanted); };
		presentation.poll=[&](std::chrono::milliseconds) { return std::vector<raven::DesktopEvent>{}; };
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(15),{},{},presentation);
		if (result.stop!=raven::NativeStop::returned || result.value) std::cerr<<"fixture returned "<<result.value<<'\n';
		check(result.stop==raven::NativeStop::returned && !result.value,"full screen fixture failed");
		const auto named=[&](const std::string &title) {
			for (const auto &[handle,name] : titles) if (name==title) return handle;
			return uint64_t(0);
		};
		const auto full=named("full screen"),maximized=named("maximized");
		check(full && maximized,"the fixture's windows were not shown");
		size_t entered=asked.size(),left=asked.size();
		for (size_t index=0;index<asked.size();++index) {
			if (asked[index].first==full && asked[index].second && entered==asked.size()) entered=index;
			if (asked[index].first==full && !asked[index].second && entered<index) left=index;
			check(asked[index].first!=maximized || !asked[index].second,"a window maximized with its caption asked for full screen");
		}
		check(entered<asked.size(),"the window that covers the screen did not ask for full screen");
		check(left<asked.size(),"the window moved to a part of the screen stayed full screen");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
