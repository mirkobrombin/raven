#include "desktop/foreign_surface.h"
#include "desktop/shared_pixels.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main()
{
	std::filesystem::path runtime;
	try {
		auto pattern=(std::filesystem::temp_directory_path()/"raven-foreign-surface-XXXXXX").string();
		check(::mkdtemp(pattern.data()),"cannot make a runtime directory");
		runtime=pattern;
		::setenv("XDG_RUNTIME_DIR",pattern.c_str(),1);
		const auto window=raven::window_handle_base()+1;
		const auto first=raven::ForeignSurface::open_for_drawing(window,4,2);
		check(first!=nullptr,"cannot open the pixels of a 4x2 window");
		for (size_t at=0;at<4*2*4;++at) first->pixels->data()[at]=uint8_t(at);
		first->publish_frame();
		// The window grows and its drawer makes a new swap chain, which opens the same pixels before it draws.
		const auto second=raven::ForeignSurface::open_for_drawing(window,5,3);
		check(second!=nullptr,"cannot open the pixels of a 5x3 window");
		uint32_t seen=0;
		bool taken=raven::take_foreign_frame(window,seen,[](const uint8_t *pixels,uint32_t width,uint32_t height) {
			check(width==4 && height==2,"the last frame is taken with the size of a swap chain that has not drawn yet");
			for (size_t at=0;at<4*2*4;++at) check(pixels[at]==uint8_t(at),"the last frame changed");
		});
		check(taken,"the last frame is not taken");
		std::memset(second->pixels->data(),7,5*3*4);
		second->publish_frame();
		taken=raven::take_foreign_frame(window,seen,[](const uint8_t *pixels,uint32_t width,uint32_t height) {
			check(width==5 && height==3,"the new frame is not taken with its own size");
			for (size_t at=0;at<5*3*4;++at) check(pixels[at]==7,"the new frame differs");
		});
		check(taken,"the new frame is not taken");
	} catch (const std::exception &error) {
		std::cerr<<error.what()<<'\n';
		if (!runtime.empty()) std::filesystem::remove_all(runtime);
		return 1;
	}
	std::filesystem::remove_all(runtime);
	return 0;
}
