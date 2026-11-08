#include "nt/providers.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

// The presented frame of a child with a modal dialog frame: a raised edge, then the face colour up to the client area.
int main(int argc,char **argv)
{
	try {
		check(argc==3 || argc==4,"bootstrap and dialog frame fixture required");
		const bool destroyed=argc==4 && std::string_view(argv[3])=="destroyed";
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg()>0,"cannot read dialog frame fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short dialog frame fixture");
		raven::ModuleSet modules; auto &entry=modules.add("dialog-frame.exe",bytes);
		std::vector<uint8_t> final; std::string colors;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t,std::string_view title,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
			check(width==120 && height==80 && pixels.size()==120*80*4,"dialog frame surface differs");
			colors=std::string(title); final.assign(pixels.begin(),pixels.end());
		};
		presentation.remove=[](uint64_t) {};
		presentation.poll=[](std::chrono::milliseconds) { return std::vector<raven::DesktopEvent>{}; };
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(10),{},{},presentation);
		check(result.stop==raven::NativeStop::returned && !result.value && !final.empty(),"dialog frame fixture failed");
		unsigned light=0,face=0,dark=0;
		check(std::sscanf(colors.c_str(),"%x %x %x",&light,&face,&dark)==3,"dialog frame colours missing");
		const auto at=[&](unsigned x,unsigned y) {
			const auto *pixel=&final[(y*120+x)*4];
			return unsigned(pixel[2]) | unsigned(pixel[1])<<8 | unsigned(pixel[0])<<16;
		};
		const auto expect=[&](unsigned x,unsigned y,unsigned color,const char *what) {
			if (at(x,y)!=color) { std::fprintf(stderr,"%s at %u,%u is %06x, not %06x\n",what,x,y,at(x,y),color); check(false,"dialog frame differs"); }
		};
		// A destroyed frame leaves what its parent, which clips its children, draws there.
		if (destroyed) {
			expect(10,30,0xffffff,"destroyed outer edge");
			expect(12,30,0xffffff,"destroyed frame band");
			expect(30,30,0xffffff,"destroyed client area");
			return 0;
		}
		expect(10,30,light,"outer left edge");
		expect(30,10,light,"outer top edge");
		expect(69,30,dark,"outer right edge");
		expect(30,49,dark,"outer bottom edge");
		expect(12,30,face,"frame band");
		expect(30,47,face,"frame band");
		expect(30,30,0xffffff,"client area");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
