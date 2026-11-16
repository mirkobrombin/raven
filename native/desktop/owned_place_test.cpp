#include "nt/providers.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

// A tool window shows on its owner's host window, placed by where it lies from the owner. The fixture moves the tool window and then
// the owner, drawing nothing: each move shows the tool window again in its new place.
int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and owned place fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg()>0,"cannot read owned place fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short owned place fixture");
		raven::ModuleSet modules; auto &entry=modules.add("owned-place.exe",bytes);
		uint64_t owner=0;
		std::vector<std::pair<int32_t,int32_t>> places;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view,uint32_t,uint32_t,std::span<const uint8_t>) { if (!owner) owner=handle; };
		presentation.owned_popup=[&](uint64_t,uint64_t shown_on,int32_t x,int32_t y,uint32_t,uint32_t,std::span<const uint8_t>) {
			check(owner && shown_on==owner,"the tool window showed on another window than its owner");
			if (places.empty() || places.back()!=std::pair{x,y}) places.emplace_back(x,y);
		};
		presentation.remove=[&](uint64_t) {};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(15),{},{},presentation);
		if (result.stop!=raven::NativeStop::returned || result.value) std::cerr<<"fixture returned "<<result.value<<'\n';
		check(result.stop==raven::NativeStop::returned && !result.value,"owned place fixture failed");
		for (const auto &[x,y] : places) std::cerr<<"shown at "<<x<<','<<y<<'\n';
		check(!places.empty() && places.front()==std::pair{50,60},"the tool window did not show where it was made");
		check(places.size()>=2 && places[1]==std::pair{80,100},"the tool window moved did not show in its new place");
		check(places.size()==3 && places[2]==std::pair{120,130},"the tool window did not show where it lies from its owner once the owner moved");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
