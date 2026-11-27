// A Wayland host that opens a bridged plugin's editor (plugin.cpp) on a surface of its own, as VST 3.8 has a Wayland DAW do: a blue
// window of 400 by 300 pixels, larger than the editor, whose surface the editor's view is attached to, on the connection the host gives the plugin
// (IWaylandHost), here its own. Says "editor: attached" once it did, keeps its window and the plugin's run loop going, and waits for the
// edit a click on the test plugin's editor makes (testdata/native_vst/gain_plugin.cpp). The screen and the click are the business of
// scripts/native-wayland-vst-editor.py, which runs this on an isolated compositor, finds the editor's green where the blue window is and
// clicks it.
//
// usage: wayland_editor_test MODULE

#include "vst/test_host.h"

#include "pluginterfaces/gui/iwaylandframe.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "xdg-shell-client-protocol.h"
#include <wayland-client.h>

#include <cstring>

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace raven_vst;
using namespace raven_vst::test;

namespace {

constexpr int32_t width = 400, height = 300;

// The host's connection to the compositor, and its window.
struct Window {
	wl_display *display = nullptr;
	wl_compositor *compositor = nullptr;
	wl_shm *shm = nullptr;
	xdg_wm_base *shell = nullptr;
	wl_surface *surface = nullptr;
	xdg_surface *frame = nullptr;
	xdg_toplevel *toplevel = nullptr;
	wl_buffer *buffer = nullptr;
	bool configured = false;

	// Reads what the compositor sent within `slice`, with the run loop's descriptors watched too.
	void pump(RunLoop &loop, std::chrono::milliseconds slice)
	{
		while (wl_display_prepare_read(display) != 0)
			wl_display_dispatch_pending(display);
		wl_display_flush(display);
		pollfd watched{wl_display_get_fd(display), POLLIN, 0};
		::poll(&watched, 1, 0);
		if (watched.revents & POLLIN)
			wl_display_read_events(display);
		else
			wl_display_cancel_read(display);
		wl_display_dispatch_pending(display);
		loop.run(slice, wl_display_get_fd(display));
	}

	void open()
	{
		display = wl_display_connect(nullptr);
		check(display, "no Wayland compositor");
		wl_registry *const registry = wl_display_get_registry(display);
		static const wl_registry_listener listener{
			[](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t) {
				auto &window = *static_cast<Window *>(data);
				if (!std::strcmp(interface, "wl_compositor"))
					window.compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
				else if (!std::strcmp(interface, "wl_shm"))
					window.shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
				else if (!std::strcmp(interface, "xdg_wm_base"))
					window.shell = static_cast<xdg_wm_base *>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
			},
			[](void *, wl_registry *, uint32_t) {}};
		wl_registry_add_listener(registry, &listener, this);
		wl_display_roundtrip(display);
		check(compositor && shm && shell, "the compositor lacks a compositor, shared memory or a shell");
		static const xdg_wm_base_listener ping{[](void *, xdg_wm_base *shell, uint32_t serial) { xdg_wm_base_pong(shell, serial); }};
		xdg_wm_base_add_listener(shell, &ping, nullptr);

		surface = wl_compositor_create_surface(compositor);
		frame = xdg_wm_base_get_xdg_surface(shell, surface);
		static const xdg_surface_listener configure{[](void *data, xdg_surface *frame, uint32_t serial) {
			xdg_surface_ack_configure(frame, serial);
			static_cast<Window *>(data)->configured = true;
		}};
		xdg_surface_add_listener(frame, &configure, this);
		toplevel = xdg_surface_get_toplevel(frame);
		xdg_toplevel_set_title(toplevel, "VST 3 host");
		xdg_toplevel_set_min_size(toplevel, width, height);
		xdg_toplevel_set_max_size(toplevel, width, height);
		wl_surface_commit(surface);
		while (!configured && wl_display_dispatch(display) >= 0) {}
		check(configured, "the window is not configured");

		const int memory = ::memfd_create("host-window", MFD_CLOEXEC);
		const size_t size = size_t(width) * height * 4;
		check(memory >= 0 && !::ftruncate(memory, off_t(size)), "no memory for the window");
		auto *const pixels = static_cast<uint32_t *>(::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0));
		check(pixels != MAP_FAILED, "the window's memory does not map");
		std::fill(pixels, pixels + size / 4, 0xff0000ffU);
		::munmap(pixels, size);
		wl_shm_pool *const pool = wl_shm_create_pool(shm, memory, int32_t(size));
		buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
		wl_shm_pool_destroy(pool);
		::close(memory);
		show();
	}

	// The window shows its blue again, which also applies what the plugin's subsurface asked of its parent.
	void show()
	{
		wl_surface_attach(surface, buffer, 0, 0);
		wl_surface_damage(surface, 0, 0, width, height);
		wl_surface_commit(surface);
		wl_display_flush(display);
	}
};

// The connection the host gives the plugin is its own.
class WaylandHost : public Counted<IWaylandHost> {
public:
	explicit WaylandHost(wl_display *display) : display(display) {}
	wl_display *PLUGIN_API openWaylandConnection() override
	{
		++opened;
		return display;
	}
	tresult PLUGIN_API closeWaylandConnection(wl_display *closed) override
	{
		if (closed != display)
			return kInvalidArgument;
		++this->closed;
		return kResultOk;
	}
	int opened = 0, closed = 0;

private:
	wl_display *display;
};

class Host : public Counted<IHostApplication> {
public:
	explicit Host(WaylandHost &wayland) : wayland(wayland) {}
	tresult PLUGIN_API getName(String128 name) override
	{
		const char16 own[] = u"Raven Wayland editor test";
		std::memcpy(name, own, sizeof own);
		return kResultOk;
	}
	tresult PLUGIN_API createInstance(TUID cid, TUID iid, void **object) override
	{
		*object = nullptr;
		if (!same(cid, IWaylandHost::iid) || !same(iid, IWaylandHost::iid))
			return kNoInterface;
		wayland.addRef();
		*object = static_cast<IWaylandHost *>(&wayland);
		return kResultOk;
	}

private:
	WaylandHost &wayland;
};

}

int main(int count, char **arguments)
{
	if (count != 2) {
		std::fputs("usage: wayland_editor_test MODULE\n", stderr);
		return 2;
	}
	try {
		Window window;
		window.open();
		auto *const wayland = new WaylandHost(window.display);
		auto *const host = new Host(*wayland);

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
		check(controller->initialize(host) == kResultOk, "the controller does not initialize");
		auto *const handler = new Handler;
		check(controller->setComponentHandler(handler) == kResultOk, "the controller takes no handler");
		IPlugView *const view = controller->createView(ViewType::kEditor);
		check(view, "the controller has no editor");
		check(view->isPlatformTypeSupported(kPlatformTypeWaylandSurfaceID) == kResultTrue, "the editor cannot show on a Wayland surface");
		check(view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue, "the editor claims an X11 window without X11");

		RunLoop loop;
		auto *const frame = new Frame(loop);
		check(view->setFrame(frame) == kResultOk && loop.count() == 1, "the editor registers nothing with the run loop");
		check(view->attached(window.surface, kPlatformTypeWaylandSurfaceID) == kResultOk, "the editor does not attach");
		check(wayland->opened == 1, "the plugin did not open the host's connection");
		std::puts("editor: attached");
		std::fflush(stdout);

		// The window and the run loop go on until the click's edit comes, as a host's do while the user works in the editor.
		const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		auto shown = std::chrono::steady_clock::now();
		while (handler->ended == 0) {
			check(std::chrono::steady_clock::now() < limit, "the editor's edit does not reach the host");
			window.pump(loop, std::chrono::milliseconds(20));
			if (std::chrono::steady_clock::now() - shown > std::chrono::milliseconds(200)) {
				window.show();
				shown = std::chrono::steady_clock::now();
			}
		}
		std::printf("edit: parameter %u to %.2f, %d begun, %d ended\n", handler->edited, handler->value, handler->begun, handler->ended);
		check(handler->edited == 0 && std::fabs(handler->value - 0.75) < 1e-9, "the edit is not the gain to 0.75");

		check(view->removed() == kResultOk, "the editor does not detach");
		check(wayland->closed == 1, "the plugin did not give the host's connection back");
		view->setFrame(nullptr);
		check(loop.count() == 0, "the editor leaves a handler in the run loop");
		view->release();
		frame->release();
		controller->setComponentHandler(nullptr);
		controller->terminate();
		controller->release();
		handler->release();
		using Exit = bool (*)();
		if (const auto exit = reinterpret_cast<Exit>(::dlsym(module, "ModuleExit")))
			exit();
		factory->release();
		host->release();
		wayland->release();
		std::puts("editor: done");
		std::fflush(stdout);
		// The module stays loaded: its bridge ends with the process, after the host's connection.
		wl_display_disconnect(window.display);
	} catch (const std::exception &error) {
		std::fprintf(stderr, "wayland_editor_test: %s\n", error.what());
		return 1;
	}
	return 0;
}
