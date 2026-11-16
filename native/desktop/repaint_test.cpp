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
		check(argc==3,"bootstrap and repaint fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate); check(bool(file) && file.tellg()>0,"cannot read repaint fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short repaint fixture");
		raven::ModuleSet modules; auto &entry=modules.add("repaint.exe",bytes);
		unsigned frames=0,removed=0; uint64_t window=0; bool closed=false;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view title,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
			check(handle && (!window || window==handle) && width==32 && height==32 && pixels.size()==4096,"repaint surface identity differs"); window=handle;
			check(title==(frames<6 ? "Raven native paint" : "Repaint: ready"),"repaint caption differs");
			for (unsigned y=0;y<32;++y) for (unsigned x=0;x<32;++x) {
				const auto at=(y*32+x)*4;
				if (!frames) { check(pixels[at]==255 && pixels[at+1]==255 && pixels[at+2]==255,"initial repaint surface does not show the window color"); continue; }
				const bool a=x>=2 && x<10 && y>=3 && y<11,b=x>=20 && x<28 && y>=21 && y<29,hole=x>=4 && x<6 && y>=5 && y<7;
				bool red=frames>=2 && ((a || b) && !hole);
				red|=frames>=3 && x>=12 && x<14 && y>=13 && y<15;
				red|=frames>=4 && !x && !y;
				red|=frames>=5 && x>=30 && y>=30;
				check(pixels[at]==x*7 && pixels[at+1]==(31-y)*7 && pixels[at+2]==(red ? 64 : 192),"update clip damaged an expected pixel");
			}
			++frames;
		};
		presentation.remove=[&](uint64_t handle) { check(handle==window,"removed wrong repaint surface"); ++removed; };
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (frames==7 && !closed) { closed=true; events.push_back({window,0x10}); }
			return events;
		};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(5),{},{},presentation);
		check(result.stop==raven::NativeStop::returned && !result.value && frames==7 && removed==1 && closed,"SDK repaint lifecycle failed");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
