#include "nt/providers.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

// The pointer crosses the caption, lights the close button and leaves it again: the frame shows the lit button and then the plain one,
// and the client area and its control are never painted again (the fixture reports what it was sent as its exit code).
int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and frame hover fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg()>0,"cannot read frame hover fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short frame hover fixture");
		raven::ModuleSet modules; auto &entry=modules.add("frame-hover.exe",bytes);
		constexpr uint32_t width=300,height=200;
		// The close button is the last 46 pixels of the caption; the caption and the client area start well inside these rows.
		constexpr uint32_t close_x=width-20,close_y=12,client_top=45;
		uint64_t window=0;
		unsigned frames=0,lit_frames=0,step=0,frames_at_step=0;
		std::vector<uint8_t> first,last;
		const auto pixel=[](const std::vector<uint8_t> &pixels,uint32_t x,uint32_t y) {
			const auto at=(size_t(y)*width+x)*4;
			return uint32_t(pixels[at]) | uint32_t(pixels[at+1])<<8 | uint32_t(pixels[at+2])<<16;
		};
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view,uint32_t frame_width,uint32_t frame_height,std::span<const uint8_t> pixels) {
			check(handle && (!window || window==handle) && frame_width==width && frame_height==height,"frame hover surface differs");
			window=handle; last.assign(pixels.begin(),pixels.end()); ++frames;
			if (first.empty()) return;
			if (pixel(last,close_x,close_y)!=pixel(first,close_x,close_y)) ++lit_frames;
			check(std::equal(last.begin()+size_t(client_top)*width*4,last.end(),first.begin()+size_t(client_top)*width*4),
				"the client area changed while the pointer crossed the frame");
		};
		presentation.remove=[&](uint64_t) {};
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (!frames) return events;
			// One step at a time; the button that lights up waits to be shown before the pointer leaves it.
			if (step==3 && frames==frames_at_step) return events;
			frames_at_step=frames;
			// The painted window as shown before the pointer comes is what every later frame is compared with.
			if (!step) first=last;
			switch (step++) {
			case 0: events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,60,12}); break;
			case 1: events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,90,14}); break;
			case 2: events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,int32_t(close_x),int32_t(close_y)}); break;
			case 3: events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,60,12}); break;
			case 4: events.push_back({window,0x8000}); break;
			default: break;
			}
			return events;
		};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(10),{},{},presentation);
		if (result.stop!=raven::NativeStop::returned || result.value) std::cerr<<"fixture returned "<<result.value<<'\n';
		check(result.stop==raven::NativeStop::returned,"frame hover fixture did not return");
		check(result.value!=10,"crossing the frame asked for the client area again (WM_NCCALCSIZE)");
		check(result.value!=11 && result.value!=12,"crossing the frame erased and painted the client area again");
		check(result.value!=13,"crossing the frame painted the control again");
		check(!result.value,"frame hover workflow failed");
		check(step>=5,"the pointer did not cross the frame");
		check(lit_frames>=1,"the close button under the pointer was not shown lit");
		check(pixel(last,close_x,close_y)==pixel(first,close_x,close_y),"the close button stayed lit after the pointer left it");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
