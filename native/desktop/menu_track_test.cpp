#include "nt/providers.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

// Clicks through a shortcut menu: a press and release on the item that owns a submenu must leave that submenu open, and the
// click on the submenu's item must come back as the chosen command.
int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and menu tracking fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg()>0,"cannot read menu tracking fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short menu tracking fixture");
		raven::ModuleSet modules; auto &entry=modules.add("menu-track.exe",bytes);
		uint64_t window=0,first=0,second=0;
		bool second_removed=false;
		int step=0;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view,uint32_t,uint32_t,std::span<const uint8_t>) { window=handle; };
		presentation.popup=[&](uint64_t handle,uint64_t parent,int32_t,int32_t,uint32_t,uint32_t,std::span<const uint8_t>) {
			if (parent==window && !first) first=handle;
			else if (first && parent==first && !second) second=handle;
		};
		presentation.remove=[&](uint64_t handle) { if (second && handle==second) second_removed=true; };
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			const auto click=[&](uint64_t target,int32_t x,int32_t y) {
				events.push_back({target,0,0,0,raven::DesktopEventKind::pointer_motion,x,y});
				events.push_back({target,0x110,1,0,raven::DesktopEventKind::pointer_button,x,y});
				events.push_back({target,0x110,0,0,raven::DesktopEventKind::pointer_button,x,y});
			};
			if (step==0 && first) { click(first,20,8); step=1; }
			else if (step==1 && second) {
				check(!second_removed,"the click on the submenu's item closed the submenu it opened");
				click(second,20,8); step=2;
			} else if (step==1 && ++step>1000) check(false,"the submenu did not open");
			return events;
		};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(10),{},{},presentation);
		check(step==2,"the submenu was not reached");
		check(result.stop==raven::NativeStop::returned && !result.value,"the submenu's command was not chosen");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
