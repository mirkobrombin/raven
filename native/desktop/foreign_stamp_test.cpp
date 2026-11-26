#include "desktop/foreign_surface.h"
#include "desktop/shared_pixels.h"

#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

// The stamp once the watch read every event: the same for 50 ms, with no read pending (the top bit clear).
static uint64_t settled_stamp()
{
	const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
	uint64_t last=raven::foreign_frames_stamp();
	auto since=std::chrono::steady_clock::now();
	while (std::chrono::steady_clock::now()<limit) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		const auto stamp=raven::foreign_frames_stamp();
		if (stamp!=last || stamp>>63) {
			last=stamp;
			since=std::chrono::steady_clock::now();
		} else if (std::chrono::steady_clock::now()-since>=std::chrono::milliseconds(50))
			return stamp;
	}
	throw std::runtime_error("the stamp does not settle");
}

int main()
{
	std::filesystem::path runtime;
	pid_t other=-1;
	const auto stop_other=[&other] {
		if (other>0) {
			::kill(other,SIGKILL);
			::waitpid(other,nullptr,0);
			other=-1;
		}
	};
	try {
		auto pattern=(std::filesystem::temp_directory_path()/"raven-foreign-stamp-XXXXXX").string();
		check(::mkdtemp(pattern.data()),"cannot make a runtime directory");
		runtime=pattern;
		::setenv("XDG_RUNTIME_DIR",pattern.c_str(),1);
		// Another program with a window of its own, which this one posts to.
		other=::fork();
		check(other>=0,"cannot start another program");
		if (!other) {
			raven::publish_window(raven::window_handle_base()+1,4,2);
			::pause();
			::_exit(0);
		}
		const uint64_t foreign=((uint64_t(uint32_t(other)%0x7ef0)+0x100)<<16)+1;
		const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
		while (!raven::foreign_window_size(foreign) && std::chrono::steady_clock::now()<limit)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		const auto window=raven::window_handle_base()+1;
		const auto before=settled_stamp();
		// A window of the program's own opening, changing and closing is no news to it, not even for the moment the watch has not read
		// the change yet: each thread looking at its queue then would take the desktop whole.
		raven::publish_window(window,4,2);
		raven::publish_window_traits(window,{});
		check(raven::foreign_frames_stamp()==before,"a window of the program's own unsettled the stamp");
		raven::withdraw_window(window);
		check(raven::foreign_frames_stamp()==before && settled_stamp()==before,"a window of the program's own moved the stamp");
		// A message posted to a window is.
		const bool posted=raven::post_foreign_message(foreign,0x400,0,0);
		stop_other();
		check(posted,"cannot post to the other program's window");
		check(settled_stamp()!=before,"a posted message left the stamp as it was");
		// Another program draws into a window of this one, as a browser's GPU process does: it opens the pixels, makes its swap chain and
		// only then finishes a first frame. The owner looks at a window not drawn yet only when the stamp moved, so the first frame must
		// move it, or the window shows nothing for good.
		raven::publish_window(window,4,2);
		int ready[2],go[2];
		check(::pipe(ready)==0 && ::pipe(go)==0,"cannot make pipes");
		other=::fork();
		check(other>=0,"cannot start a drawing program");
		if (!other) {
			const auto surface=raven::ForeignSurface::open_for_drawing(window,4,2);
			char byte=surface ? 1 : 0;
			if (::write(ready[1],&byte,1)!=1 || ::read(go[0],&byte,1)!=1 || !surface) ::_exit(1);
			std::memset(surface->pixels->data(),9,4*2*4);
			surface->publish_frame();
			byte=1;
			if (::write(ready[1],&byte,1)!=1) ::_exit(1);
			::pause();
			::_exit(0);
		}
		char byte=0;
		check(::read(ready[0],&byte,1)==1 && byte,"the drawing program cannot open the pixels");
		const auto opened=settled_stamp();
		uint32_t seen=0;
		const auto take=[&] {
			return raven::take_foreign_frame(window,seen,[](const uint8_t *pixels,uint32_t width,uint32_t height) {
				check(width==4 && height==2 && pixels[0]==9,"the first frame differs");
			});
		};
		check(!take(),"a frame was taken before any was drawn");
		byte=1;
		check(::write(go[1],&byte,1)==1 && ::read(ready[0],&byte,1)==1,"the drawing program did not draw");
		const auto drawn=settled_stamp();
		stop_other();
		check(drawn!=opened,"a window's first frame left the stamp as it was: its owner never looks at it");
		check(take(),"the first frame is not taken");
	} catch (const std::exception &error) {
		stop_other();
		std::cerr<<error.what()<<'\n';
		if (!runtime.empty()) std::filesystem::remove_all(runtime);
		return 1;
	}
	std::filesystem::remove_all(runtime);
	return 0;
}
