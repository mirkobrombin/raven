// A Linux host that opens a bridged plugin's editor (plugin.cpp) in an X11 window of its own, as a DAW does: makes the controller,
// creates its editor view, attaches it to a blue window with a frame that brings a run loop, and waits until a child of the window shows
// the test plugin's green (testdata/native_vst/gain_plugin.cpp). Then clicks the editor, which edits the gain, and waits until the edit
// reaches the host's component handler through the run loop, with nothing else calling the plugin meanwhile.
//
// usage: editor_test MODULE

#include "vst/test_host.h"

#include <xcb/xcb.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>

#include <dlfcn.h>

using namespace raven_vst;
using namespace raven_vst::test;

namespace {

// The colour at a point of a window, as the server shows it.
uint32_t pixel(xcb_connection_t *connection, xcb_window_t window, int16_t x, int16_t y)
{
	xcb_get_image_reply_t *const image = xcb_get_image_reply(connection,
		xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window, x, y, 1, 1, UINT32_MAX), nullptr);
	uint32_t value = UINT32_MAX;
	if (image && xcb_get_image_data_length(image) >= 4) {
		std::memcpy(&value, xcb_get_image_data(image), 4);
		value &= 0xffffffU;
	}
	std::free(image);
	return value;
}

// Runs the loop until `done` holds or ten seconds pass.
bool wait(RunLoop &loop, const std::function<bool()> &done)
{
	const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!done()) {
		if (std::chrono::steady_clock::now() >= limit)
			return false;
		loop.run(std::chrono::milliseconds(20));
	}
	return true;
}

}

int main(int count, char **arguments)
{
	if (count != 2) {
		std::fputs("usage: editor_test MODULE\n", stderr);
		return 2;
	}
	try {
		void *const module = ::dlopen(arguments[1], RTLD_NOW | RTLD_LOCAL);
		check(module, "the module does not load");
		using Get = IPluginFactory *(PLUGIN_API *)();
		const auto get = reinterpret_cast<Get>(::dlsym(module, "GetPluginFactory"));
		IPluginFactory *const factory = get ? get() : nullptr;
		check(factory, "the module has no factory");
		TUID controller_id{};
		check(controller_class(*factory, controller_id), "no controller class");
		IEditController *controller = nullptr;
		check(factory->createInstance(controller_id, IEditController::iid, reinterpret_cast<void **>(&controller)) == kResultOk && controller,
			"the controller is not made");
		check(controller->initialize(nullptr) == kResultOk, "the controller does not initialize");
		auto *const handler = new Handler;
		check(controller->setComponentHandler(handler) == kResultOk, "the controller takes no handler");
		IPlugView *const view = controller->createView(ViewType::kEditor);
		check(view, "the controller has no editor");
		check(view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) == kResultTrue, "the editor cannot show in an X11 window");
		ViewRect size{};
		check(view->getSize(&size) == kResultOk && size.getWidth() == 300 && size.getHeight() == 200, "the editor is not 300 by 200");

		xcb_connection_t *const connection = xcb_connect(nullptr, nullptr);
		check(!xcb_connection_has_error(connection), "no X server");
		xcb_screen_t *const screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
		const xcb_window_t parent = xcb_generate_id(connection);
		const uint32_t blue[] = {0x0000ffU};
		xcb_create_window(connection, screen->root_depth, parent, screen->root, 0, 0, uint16_t(size.getWidth()), uint16_t(size.getHeight()), 0,
			XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL, blue);
		xcb_map_window(connection, parent);
		xcb_flush(connection);

		RunLoop loop;
		auto *const frame = new Frame(loop);
		check(view->setFrame(frame) == kResultOk && loop.count() == 1, "the editor registers nothing with the run loop");
		check(view->attached(reinterpret_cast<void *>(uintptr_t(parent)), kPlatformTypeX11EmbedWindowID) == kResultOk, "the editor does not attach");
		xcb_window_t editor = 0;
		check(wait(loop, [&] {
			xcb_query_tree_reply_t *const tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, parent), nullptr);
			if (tree && xcb_query_tree_children_length(tree) > 0)
				editor = xcb_query_tree_children(tree)[0];
			std::free(tree);
			return editor && pixel(connection, parent, 150, 100) == 0x00ff00U;
		}), "the editor's green does not show in the host's window");
		std::puts("editor: shown in the host's window");

		// A click inside the editor: the plugin edits the gain, which comes to the host through its run loop alone.
		for (const uint8_t type : {uint8_t(XCB_BUTTON_PRESS), uint8_t(XCB_BUTTON_RELEASE)}) {
			xcb_button_press_event_t click{};
			click.response_type = type;
			click.detail = 1;
			click.time = XCB_CURRENT_TIME;
			click.root = screen->root;
			click.event = editor;
			click.child = XCB_NONE;
			click.root_x = click.event_x = 150;
			click.root_y = click.event_y = 100;
			click.same_screen = 1;
			xcb_send_event(connection, 0, editor, type == XCB_BUTTON_PRESS ? XCB_EVENT_MASK_BUTTON_PRESS : XCB_EVENT_MASK_BUTTON_RELEASE,
				reinterpret_cast<const char *>(&click));
		}
		xcb_flush(connection);
		check(wait(loop, [&] { return handler->ended > 0; }), "the editor's edit does not reach the host");
		std::printf("edit: parameter %u to %.2f, %d begun, %d ended\n", handler->edited, handler->value, handler->begun, handler->ended);
		check(handler->edited == 0 && std::fabs(handler->value - 0.75) < 1e-9, "the edit is not the gain to 0.75");
		check(std::fabs(controller->getParamNormalized(0) - 0.75) < 1e-9, "the controller does not have the edited gain");

		check(view->removed() == kResultOk, "the editor does not detach");
		view->setFrame(nullptr);
		check(loop.count() == 0, "the editor leaves a handler in the run loop");
		view->release();
		frame->release();
		controller->setComponentHandler(nullptr);
		controller->terminate();
		controller->release();
		handler->release();
		factory->release();
		xcb_disconnect(connection);
		::dlclose(module);
		std::puts("editor: done");
	} catch (const std::exception &error) {
		std::fprintf(stderr, "editor_test: %s\n", error.what());
		return 1;
	}
	return 0;
}
