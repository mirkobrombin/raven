#include "nt/providers.h"
#include <algorithm>
#include <cstring>
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
		check(argc==3,"bootstrap and wait present fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate); check(bool(file) && file.tellg()>0,"cannot read wait present fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short wait present fixture");
		raven::ModuleSet modules; auto &entry=modules.add("wait-present.exe",bytes);
		// The colour in the middle of each frame, in the order the frames were presented.
		std::vector<uint32_t> frames;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t,std::string_view,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
			uint32_t colour=0;
			std::memcpy(&colour,pixels.data()+(size_t(height/2)*width+width/2)*4,4);
			frames.push_back(colour & 0xffffffU);
		};
		presentation.remove=[](uint64_t) {};
		presentation.poll=[](std::chrono::milliseconds) { return std::vector<raven::DesktopEvent>{}; };
		const auto result=entry.image.is_64 ?
			raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(10),{},{},presentation) :
			raven::run_native_process_x86(modules,entry,argv[1],std::chrono::seconds(10),{},presentation);
		if (result.stop!=raven::NativeStop::returned || result.value) std::cerr<<"fixture answered "<<result.value<<" stop "<<int(result.stop)<<'\n';
		check(result.stop==raven::NativeStop::returned && !result.value,"waiting for messages while another thread draws failed");
		// The process shows its last frame, the green one, when it ends; the red and the blue one show only if the wait presented them.
		const auto red=std::find(frames.begin(),frames.end(),0xff0000U);
		check(red!=frames.end(),"the first frame drawn during the wait was not presented");
		const auto blue=std::find(red,frames.end(),0x0000ffU);
		check(blue!=frames.end(),"the second frame drawn during the wait was not presented");
		check(std::find(blue,frames.end(),0x00ff00U)!=frames.end(),"the last frame was not presented");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
