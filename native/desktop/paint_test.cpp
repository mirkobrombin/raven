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
		check(argc==5,"bootstrap, paint, close and pointer fixtures required");
		for (int fixture=2;fixture<argc;++fixture) {
			std::ifstream file(argv[fixture],std::ios::binary | std::ios::ate);
			check(bool(file) && file.tellg()>0,"cannot read paint fixture");
			std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
			check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short paint fixture");
			raven::ModuleSet modules;
			auto &entry=modules.add("paint.exe",bytes);
			unsigned frames=0,removed=0;
			uint64_t window=0;
			raven::DesktopPresentation presentation;
			presentation.present=[&](uint64_t handle,std::string_view title,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
				check(handle && (!window || window==handle) && title=="Raven native paint" && width==32 && height==32 && pixels.size()==4096,"paint surface identity differs");
				window=handle;
				if (frames) {
					for (unsigned y=0;y<32;++y) for (unsigned x=0;x<32;++x) {
						const auto at=(y*32+x)*4;
						check(pixels[at]==x*7 && pixels[at+1]==(31-y)*7 && pixels[at+2]==192,"guest paint pixel differs");
					}
				} else
					// A class without a background shows the window color until the program paints, never black.
					for (size_t at=0;at<pixels.size();at+=4) check(pixels[at]==255 && pixels[at+1]==255 && pixels[at+2]==255 && !pixels[at+3],
						"initial window surface does not show the window color");
				++frames;
			};
			presentation.remove=[&](uint64_t handle) { check(handle==window,"removed wrong surface"); ++removed; };
			// The pointer over the window shows the hand its class names.
			std::vector<uint32_t> cursors;
			presentation.cursor=[&](uint32_t shape) { cursors.push_back(shape); };
			unsigned waits=0;
			bool closed=false;
			// A blocked GetMessage looks at the host without a budget of its own, so the event comes on the first look after the frame.
			presentation.poll=[&](std::chrono::milliseconds) {
				std::vector<raven::DesktopEvent> events;
				if (fixture>=3 && frames==2 && !closed) {
					++waits; closed=true;
					if (fixture==3) events.push_back({window,0x10});
					else {
						events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,-2,3});
						events.push_back({window,0x110,1,0,raven::DesktopEventKind::pointer_button,16,16});
						events.push_back({window,0x110,0,0,raven::DesktopEventKind::pointer_button,16,16});
					}
				}
				return events;
			};
			const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(5),{},{},presentation);
			check(result.stop==raven::NativeStop::returned && !result.value && frames==2 && removed==1,"SDK paint lifecycle failed");
			check(fixture==2 || waits==1,"close workflow did not wait for host event");
			check(fixture!=4 || (!cursors.empty() && cursors.front()==32649),"class cursor did not reach the host pointer");
		}
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
