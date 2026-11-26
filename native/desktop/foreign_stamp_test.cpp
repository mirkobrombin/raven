#include "desktop/foreign_surface.h"

#include <chrono>
#include <csignal>
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
		// A window of the program's own opening, changing and closing is no news to it.
		raven::publish_window(window,4,2);
		raven::publish_window_traits(window,{});
		raven::withdraw_window(window);
		check(settled_stamp()==before,"a window of the program's own moved the stamp");
		// A message posted to a window is.
		const bool posted=raven::post_foreign_message(foreign,0x400,0,0);
		stop_other();
		check(posted,"cannot post to the other program's window");
		check(settled_stamp()!=before,"a posted message left the stamp as it was");
	} catch (const std::exception &error) {
		stop_other();
		std::cerr<<error.what()<<'\n';
		if (!runtime.empty()) std::filesystem::remove_all(runtime);
		return 1;
	}
	std::filesystem::remove_all(runtime);
	return 0;
}
