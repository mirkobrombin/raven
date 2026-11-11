#include "nt/providers.h"
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
		check(argc==3,"bootstrap and pointer leave fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate); check(bool(file) && file.tellg()>0,"cannot read pointer leave fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short pointer leave fixture");
		raven::ModuleSet modules; auto &entry=modules.add("leave.exe",bytes);
		uint64_t window=0;
		unsigned step=0;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view,uint32_t,uint32_t,std::span<const uint8_t>) { if (!window) window=handle; };
		presentation.remove=[](uint64_t) {};
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (!window) return events;
			// The pointer comes over the window, then goes to another program's, which the compositor tells as a leave with no window.
			if (step==0) events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,20,10});
			else if (step==20) events.push_back({0,0,0,0,raven::DesktopEventKind::pointer_leave,20,10});
			else if (step==40) events.push_back({window,0x10});
			++step;
			return events;
		};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(10),{},{},presentation);
		if (result.stop!=raven::NativeStop::returned || result.value) std::cerr<<"fixture answered "<<result.value<<'\n';
		check(result.stop==raven::NativeStop::returned && !result.value,"the pointer leaving for another program's window was not seen");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
