#include "nt/providers.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

// The host gives a window five sizes in one go, each narrower than the last, as a compositor does while the user drags an edge and the
// program is busy. The window takes the last of them alone, and its first frame at that size still shows what it had painted, red,
// where the old and the new size share the surface, not an empty window.
int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and host resize fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg()>0,"cannot read host resize fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short host resize fixture");
		raven::ModuleSet modules; auto &entry=modules.add("host-resize.exe",bytes);
		constexpr uint32_t first_width=500,last_width=400,height=300;
		uint64_t window=0;
		bool painted=false,sized=false,kept=false,closed=false;
		unsigned other_sizes=0,step=0;
		auto waited=std::chrono::steady_clock::now();
		const auto red_at=[](std::span<const uint8_t> pixels,uint32_t width,uint32_t x,uint32_t y) {
			const auto *pixel=pixels.data()+(size_t(y)*width+x)*4;
			return pixel[2]>200 && pixel[1]<60 && pixel[0]<60;
		};
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view,uint32_t width,uint32_t rows,std::span<const uint8_t> pixels) {
			window=handle;
			if (width==first_width && red_at(pixels,width,width/2,rows/2)) painted=true;
			if (width!=first_width && width!=last_width) ++other_sizes;
			if (width==last_width && !sized) {
				sized=true;
				kept=red_at(pixels,width,width/2,rows/2);
			}
		};
		presentation.remove=[&](uint64_t) {};
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (!window) return events;
			const bool done=step==0 ? painted : step==1 ? sized : true;
			if (!done && std::chrono::steady_clock::now()-waited<std::chrono::seconds(3)) return events;
			waited=std::chrono::steady_clock::now();
			switch (step++) {
			case 0:
				for (int32_t width=480;width>=int32_t(last_width);width-=20)
					events.push_back({window,0,0,0,raven::DesktopEventKind::resize,width,int32_t(height)});
				break;
			case 1: break;
			case 2: events.push_back({window,0x10}); closed=true; break;
			default: break;
			}
			return events;
		};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(20),{},{},presentation);
		if (result.stop!=raven::NativeStop::returned || result.value) std::cerr<<"fixture returned "<<result.value<<'\n';
		check(result.stop==raven::NativeStop::returned && closed,"host resize fixture did not return");
		check(painted,"the window did not paint itself before it was sized");
		check(sized,"the window did not take the last size the host gave it");
		check(result.value<10,"the window heard of sizes the host had already replaced");
		check(!other_sizes,"the window showed a frame at a size the host had already replaced");
		check(!result.value,"the window is not at the last size the host gave it");
		check(kept,"the window sized by the host showed an empty surface before it painted");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
