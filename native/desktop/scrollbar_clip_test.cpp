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
		check(argc==3,"bootstrap and scroll bar clip fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg()>0,"cannot read scroll bar clip fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short scroll bar clip fixture");
		raven::ModuleSet modules; auto &entry=modules.add("scrollbar-clip.exe",bytes);
		uint64_t window=0; bool ready=false,closed=false;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view title,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
			check(handle && width==64 && height==48 && pixels.size()==64*48*4,"scroll bar clip surface differs");
			if (title!="Scroll clip: ready" || ready) return;
			window=handle;
			const auto colour=[&](int x,int y) { const auto at=size_t(y*64+x)*4; return uint32_t(pixels[at+2])<<16 | pixels[at+1]<<8 | pixels[at]; };
			for (int y=0;y<16;++y) for (int x=0;x<24;++x) check(colour(x,y)==0x0ac81e,"the child that keeps its whole rectangle is not drawn");
			// Its bars lie right of it and below it, outside the window: the parent shows there.
			for (int y=0;y<16;++y) for (int x=24;x<40;++x) check(colour(x,y)==0x255bad,"a vertical scroll bar is drawn outside its window");
			for (int y=16;y<33;++y) for (int x=0;x<24;++x) check(colour(x,y)==0x255bad,"a horizontal scroll bar is drawn outside its window");
			// A bar past the client area of the window's parent shows only inside that client area.
			for (int y=34;y<48;++y) for (int x=32;x<40;++x) check(colour(x,y)==0x255bad,"a scroll bar is drawn outside its parent");
			for (int y=34;y<48;++y) for (int x=23;x<32;++x) {
				const auto shown=colour(x,y);
				check(shown!=0x0ac81e && shown!=0x255bad,"the part of a scroll bar inside its parent is not drawn");
			}
			// The other child keeps its bars inside it.
			for (int y=0;y<23;++y) for (int x=47;x<64;++x) {
				const auto shown=colour(x,y);
				check(shown!=0x0ac81e && shown!=0x255bad,"a vertical scroll bar inside its window is not drawn");
			}
			ready=true;
		};
		presentation.remove=[&](uint64_t handle) { check(handle==window,"removed another scroll bar clip window"); };
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (ready && !closed) { closed=true; events.push_back({window,0x10}); }
			return events;
		};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(10),{},{},presentation);
		check(result.stop==raven::NativeStop::returned,"scroll bar clip fixture did not return");
		check(!result.value,"scroll bar clip fixture failed");
		check(ready && closed,"scroll bar clip frame was not presented");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
