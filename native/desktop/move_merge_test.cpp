#include "nt/providers.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and move merge fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate); check(bool(file) && file.tellg()>0,"cannot read move merge fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short move merge fixture");
		raven::ModuleSet modules; auto &entry=modules.add("move-merge.exe",bytes);
		uint64_t window=0;
		std::chrono::steady_clock::time_point moved{};
		int sent=0;
		bool closed=false;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view,uint32_t,uint32_t,std::span<const uint8_t>) { if (!window) window=handle; };
		presentation.remove=[](uint64_t) {};
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (!window) return events;
			const auto now=std::chrono::steady_clock::now();
			// A move for every couple of milliseconds, then the window is closed.
			if (sent<150) {
				if (moved==std::chrono::steady_clock::time_point{}) moved=now;
				// Every move the hand made since the program last looked comes at once, as from a display server.
				for (;sent<150 && now-moved>=std::chrono::milliseconds(2);moved+=std::chrono::milliseconds(2))
					events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,4+(sent++%40),8});
			} else if (!closed && now-moved>=std::chrono::milliseconds(50)) {
				closed=true;
				events.push_back({window,0x10});
			}
			return events;
		};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(15),{},{},presentation);
		if (result.stop!=raven::NativeStop::returned || result.value) std::cerr<<"fixture answered "<<result.value<<" stop "<<int(result.stop)<<'\n';
		check(result.stop==raven::NativeStop::returned && !result.value,"posted messages kept the pointer's moves from giving way to the newer ones");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
