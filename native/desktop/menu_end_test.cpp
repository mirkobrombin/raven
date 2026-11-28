#include "nt/providers.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

// A shortcut menu closed from another thread: the host shows the menu and sends no input, so the menu stays open until the second
// thread's message reaches the owner window through the menu's loop and its procedure calls EndMenu.
int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and menu end fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg()>0,"cannot read menu end fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short menu end fixture");
		raven::ModuleSet modules; auto &entry=modules.add("menu-end.exe",bytes);
		bool shown=false;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t,std::string_view,uint32_t,uint32_t,std::span<const uint8_t>) {};
		presentation.popup=[&](uint64_t,uint64_t,int32_t,int32_t,uint32_t,uint32_t,std::span<const uint8_t>) { shown=true; };
		presentation.poll=[&](std::chrono::milliseconds wait) {
			std::this_thread::sleep_for(wait);
			return std::vector<raven::DesktopEvent>{};
		};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(10),{},{},presentation);
		check(shown,"the menu did not show");
		check(result.stop==raven::NativeStop::returned,"the menu was not closed from the other thread");
		if (result.value) {
			std::cerr<<"the fixture failed with "<<result.value<<'\n';
			return 1;
		}
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
