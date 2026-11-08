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
		check(argc==3,"bootstrap and wheel fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate); check(bool(file) && file.tellg()>0,"cannot read wheel fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short wheel fixture");
		raven::ModuleSet modules; auto &entry=modules.add("wheel.exe",bytes);
		uint64_t window=0;
		bool scrolled=false;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view,uint32_t,uint32_t,std::span<const uint8_t>) { if (!window) window=handle; };
		presentation.remove=[](uint64_t) {};
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (!window || scrolled) return events;
			scrolled=true;
			// One notch down over the right pane.
			events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,48,16});
			events.push_back({window,uint32_t(-120),0,0,raven::DesktopEventKind::pointer_wheel,48,16});
			events.push_back({window,0x10});
			return events;
		};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(10),{},{},presentation);
		if (result.stop!=raven::NativeStop::returned || result.value) std::cerr<<"fixture answered "<<result.value<<'\n';
		check(result.stop==raven::NativeStop::returned && !result.value && scrolled,"the wheel did not reach the window under the pointer");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
