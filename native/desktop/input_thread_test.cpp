#include "nt/providers.h"
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and input thread fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate); check(bool(file) && file.tellg()>0,"cannot read input thread fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short input thread fixture");
		raven::ModuleSet modules; auto &entry=modules.add("input-thread.exe",bytes);
		std::vector<uint64_t> windows;
		unsigned step=0;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view,uint32_t,uint32_t,std::span<const uint8_t>) {
			if (std::find(windows.begin(),windows.end(),handle)==windows.end()) windows.push_back(handle);
		};
		presentation.remove=[](uint64_t) {};
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (windows.size()<2) return events;
			// The second window shown is the worker's.
			if (step==0) {
				events.push_back({windows[1],0,0,0,raven::DesktopEventKind::pointer_motion,10,10});
				events.push_back({windows[1],0x110,1,0,raven::DesktopEventKind::pointer_button,10,10});
				events.push_back({windows[1],0x110,0,0,raven::DesktopEventKind::pointer_button,10,10});
			} else if (step==40) {
				events.push_back({windows[0],0x10});
			}
			++step;
			return events;
		};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(10),{},{},presentation);
		if (result.stop!=raven::NativeStop::returned || result.value) std::cerr<<"fixture answered "<<result.value<<" stop "<<int(result.stop)<<'\n';
		check(result.stop==raven::NativeStop::returned && !result.value,"input for another thread's window failed");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
