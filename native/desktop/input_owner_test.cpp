#include "nt/providers.h"
#include <algorithm>
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
		check(argc==3,"bootstrap and input owner fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate); check(bool(file) && file.tellg()>0,"cannot read input owner fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short input owner fixture");
		raven::ModuleSet modules; auto &entry=modules.add("input-owner.exe",bytes);
		std::vector<uint64_t> windows;
		std::chrono::steady_clock::time_point began{},moved{};
		bool closed=false;
		int32_t at=0;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view,uint32_t,uint32_t,std::span<const uint8_t>) {
			if (std::find(windows.begin(),windows.end(),handle)==windows.end()) windows.push_back(handle);
		};
		presentation.remove=[](uint64_t) {};
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (windows.size()<2) return events;
			const auto now=std::chrono::steady_clock::now();
			if (began==std::chrono::steady_clock::time_point{}) began=now;
			// The pointer moves over the first window, the main thread's, a step every few milliseconds, as a hand moves it; then the
			// window is closed.
			if (now-began<std::chrono::milliseconds(1500)) {
				if (now-moved>=std::chrono::milliseconds(5)) {
					moved=now;
					events.push_back({windows[0],0,0,0,raven::DesktopEventKind::pointer_motion,4+(at++%20),8});
				}
			} else if (!closed) {
				closed=true;
				events.push_back({windows[0],0x10});
			}
			return events;
		};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(10),{},{},presentation);
		if (result.stop!=raven::NativeStop::returned || result.value) std::cerr<<"fixture answered "<<result.value<<" stop "<<int(result.stop)<<'\n';
		check(result.stop==raven::NativeStop::returned && !result.value,"input for a busy thread's window held up another thread");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
