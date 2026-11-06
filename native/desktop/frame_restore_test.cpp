#include "nt/providers.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

// The pointer clicks the maximize button, then the restore button the maximized window shows in its place: the window fills the
// screen and comes back to its own size. A press on the caption of the restored window then hands the move to the compositor. The
// fixture reports the system commands it heard and its final state as its exit code.
int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and frame restore fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg()>0,"cannot read frame restore fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short frame restore fixture");
		raven::ModuleSet modules; auto &entry=modules.add("frame-restore.exe",bytes);
		constexpr int32_t screen_width=800,screen_height=600,width=400;
		// The maximize button sits left of the 46-pixel close button at the right of the caption, inside the sizing border.
		constexpr int32_t button_y=15,caption_x=100;
		const auto button_x=[](int32_t frame_width) { return frame_width-75; };
		uint64_t window=0,moved=0;
		uint32_t frame_width=0;
		bool maximized=false,restored=false,maximize_asked=false,restore_asked=false;
		unsigned step=0;
		auto waited=std::chrono::steady_clock::now();
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view,uint32_t frame,uint32_t,std::span<const uint8_t>) {
			check(handle && (!window || window==handle),"frame restore surface differs");
			window=handle; frame_width=frame;
			if (step>=1 && frame==uint32_t(screen_width)) maximized=true;
			if (maximized && frame==uint32_t(width)) restored=true;
		};
		presentation.remove=[&](uint64_t) {};
		presentation.screen_size=[&] { return std::optional<std::pair<int32_t,int32_t>>{{screen_width,screen_height}}; };
		presentation.maximize=[&](uint64_t,bool wanted) { (wanted ? maximize_asked : restore_asked)=true; };
		presentation.interactive=[&](uint64_t handle,uint32_t action) { if (handle==window && !action) moved=handle; };
		const auto click=[&](std::vector<raven::DesktopEvent> &events,int32_t x,int32_t y,bool release) {
			events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,x,y});
			events.push_back({window,0x110,1,0,raven::DesktopEventKind::pointer_button,x,y});
			if (release) events.push_back({window,0x110,0,0,raven::DesktopEventKind::pointer_button,x,y});
		};
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (!window) return events;
			// Each step waits for the frame it changes, for two seconds at most.
			const bool done=step==1 ? maximized : step==2 ? restored : step==3 ? moved!=0 : true;
			if (!done && std::chrono::steady_clock::now()-waited<std::chrono::seconds(2)) return events;
			waited=std::chrono::steady_clock::now();
			switch (step++) {
			case 0: click(events,button_x(int32_t(frame_width)),button_y,true); break;
			case 1: click(events,button_x(int32_t(frame_width)),button_y,true); break;
			case 2: click(events,caption_x,button_y,false); break;
			case 3: events.push_back({window,0x110,0,0,raven::DesktopEventKind::pointer_button,caption_x,button_y}); break;
			case 4: events.push_back({window,0x10}); break;
			default: break;
			}
			return events;
		};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(15),{},{},presentation);
		if (result.stop!=raven::NativeStop::returned || result.value) std::cerr<<"fixture returned "<<result.value<<'\n';
		check(result.stop==raven::NativeStop::returned,"frame restore fixture did not return");
		check(maximized && maximize_asked,"the maximize button did not maximize the window");
		check(result.value!=10,"the maximize button did not send SC_MAXIMIZE");
		check(result.value!=11,"the restore button of the maximized window did not send SC_RESTORE");
		check(result.value!=12,"the window stayed maximized after its restore button");
		check(restored && restore_asked,"the restore button did not bring the window back to its size");
		check(result.value!=13,"the restored window is not at its own rectangle");
		check(!result.value,"frame restore workflow failed");
		check(moved!=0,"a press on the caption of the restored window did not start a move");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
