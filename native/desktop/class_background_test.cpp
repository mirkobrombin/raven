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
		check(argc==4,"bootstrap, class background fixture and output required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg()>0,"cannot read class background fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short class background fixture");
		raven::ModuleSet modules; auto &entry=modules.add("class-background.exe",bytes);
		unsigned frames=0,removed=0; uint64_t window=0; bool ready=false,closed=false;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view title,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
			check(handle && (!window || handle==window) && width==32 && height==24 && pixels.size()==3072,"class background surface identity differs"); window=handle;
			check(title==(frames<4 ? "Raven class background" : "Background: ready"),"class background presentation sequence differs");
			for (unsigned y=0;y<24;++y) for (unsigned x=0;x<32;++x) {
				const auto at=(y*32+x)*4;
				// Before the first paint the surface already shows the class background, never black.
				if (!frames) { check(pixels[at]==173 && pixels[at+1]==91 && pixels[at+2]==37 && !pixels[at+3],"initial surface does not show the class background"); continue; }
				const bool mark=x>=4 && x<12 && y>=4 && y<12;
				const bool hole=frames>=2 && x>=6 && x<10 && y>=6 && y<10;
				const bool red=mark && !hole;
				check(pixels[at]==(red ? 31 : 173) && pixels[at+1]==(red ? 44 : 91) && pixels[at+2]==(red ? 166 : 37) && !pixels[at+3],"class background erased outside the update region or missed a pixel");
			}
			if (title=="Background: ready") {
				std::ofstream output(argv[3],std::ios::binary); output.write(reinterpret_cast<const char *>(pixels.data()),std::streamsize(pixels.size()));
				check(bool(output),"cannot preserve class background buffer"); ready=true;
			}
			++frames;
		};
		presentation.remove=[&](uint64_t handle) { check(handle==window,"removed wrong class background window"); ++removed; };
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (ready && !closed) { closed=true; events.push_back({window,0x10}); }
			return events;
		};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(10),{},{},presentation);
		check(result.stop==raven::NativeStop::returned && !result.value && frames==5 && removed==1 && closed,"class background SDK workflow failed");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
