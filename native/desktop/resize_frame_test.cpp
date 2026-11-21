#include "nt/providers.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and resize frame fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg()>0,"cannot read resize frame fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short resize frame fixture");
		raven::ModuleSet modules; auto &entry=modules.add("resize-frame.exe",bytes);
		uint64_t window=0; bool grown=false,ready=false,closed=false;
		int left=0,top=0,right=0,bottom=0;
		uint32_t empty=0;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view title,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
			check(handle && pixels.size()==size_t(width)*height*4,"resize frame surface differs");
			const auto colour=[&](int x,int y) { const auto at=(size_t(y)*width+size_t(x))*4; return uint32_t(pixels[at+2])<<16 | pixels[at+1]<<8 | pixels[at]; };
			if (grown && !ready && title=="Popup") {
				// The client area moves to the corner of the frameless window and keeps what it showed, with nothing of the old frame.
				check(width==360 && height==240,"the frameless window has another size");
				for (int y=0;y<240;++y) for (int x=0;x<360;++x)
					check(colour(x,y)==(x<240-left-right && y<160-top-bottom ? 0xc81414 : empty),"the client area did not move with the frame");
				ready=true;
				return;
			}
			if (grown || std::sscanf(std::string(title).c_str(),"Grow: %d %d %d %d",&left,&top,&right,&bottom)!=4) return;
			check(width==320 && height==220 && left>0 && top>0 && right>0 && bottom>0,"the grown window has no frame");
			window=handle;
			// The client area it had keeps its red; the new part of the client area shows one colour, with nothing of the old frame.
			for (int y=top;y<160-bottom;++y) for (int x=left;x<240-right;++x) check(colour(x,y)==0xc81414,"the old client area is not kept");
			empty=colour(320-right-1,220-bottom-1);
			check(empty!=0xc81414,"the new part of the client area is red");
			for (int y=top;y<220-bottom;++y) for (int x=left;x<320-right;++x)
				if (x>=240-right || y>=160-bottom) check(colour(x,y)==empty,"the old frame is kept inside the grown window");
			grown=true;
		};
		presentation.remove=[](uint64_t) {};
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (ready && !closed) { closed=true; events.push_back({window,0x10}); }
			return events;
		};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(10),{},{},presentation);
		check(result.stop==raven::NativeStop::returned,"resize frame fixture did not return");
		check(!result.value,"resize frame fixture failed");
		check(ready && closed,"the resized windows were not presented");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
