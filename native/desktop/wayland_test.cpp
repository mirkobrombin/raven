#include "wayland.h"
#include "xdg-shell-server-protocol.h"
#include <wayland-server.h>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

// A compositor in the test's own process. Like GNOME's, it grants a popup grab only for the serial of the newest key press, dismissing the
// popup otherwise; it gives a grabbing popup the keyboard in the same burst of events as the popup's first configure, and gives it back to
// the popup's parent when the popup goes. It accepts and ignores every other request.
struct Compositor {
	struct Shell { wl_resource *shell=nullptr,*role=nullptr,*parent=nullptr; bool popup=false,grab=false,denied=false,configured=false; };
	wl_display *display=wl_display_create();
	wl_resource *keyboard=nullptr,*focused=nullptr;
	std::map<wl_resource *,Shell> surfaces;
	uint32_t serial=0,pressed=0;
	// Set by the test: a key is pressed and released.
	std::atomic<bool> type{false},stop{false};

	void focus(wl_resource *surface) {
		if (!keyboard) return;
		wl_array keys; wl_array_init(&keys);
		wl_keyboard_send_enter(keyboard,++serial,surface,&keys);
		wl_array_release(&keys);
		focused=surface;
	}

	void press() {
		if (!keyboard) return;
		pressed=++serial;
		wl_keyboard_send_key(keyboard,pressed,0,30,WL_KEYBOARD_KEY_STATE_PRESSED);
		wl_keyboard_send_key(keyboard,++serial,0,30,WL_KEYBOARD_KEY_STATE_RELEASED);
	}
};

static int dispatch(const void *,void *,uint32_t,const wl_message *,wl_argument *);

static wl_resource *make(Compositor &compositor,wl_client *client,const wl_interface *interface,int version,uint32_t id)
{
	auto *resource=wl_resource_create(client,interface,version,id);
	wl_resource_set_dispatcher(resource,dispatch,&compositor,nullptr,nullptr);
	return resource;
}

template<const wl_interface *interface> static void bind(wl_client *client,void *data,uint32_t version,uint32_t id)
{
	auto *resource=make(*static_cast<Compositor *>(data),client,interface,int(version),id);
	if (interface==&wl_seat_interface) wl_seat_send_capabilities(resource,WL_SEAT_CAPABILITY_KEYBOARD);
}

static int dispatch(const void *data,void *target,uint32_t,const wl_message *message,wl_argument *arguments)
{
	auto &compositor=*static_cast<Compositor *>(const_cast<void *>(data));
	auto *resource=static_cast<wl_resource *>(target);
	const auto request=std::string(wl_resource_get_class(resource))+"."+message->name;
	const auto object=[&](size_t index) { return reinterpret_cast<wl_resource *>(arguments[index].o); };
	wl_resource *made=nullptr;
	size_t index=0;
	for (const char *type=message->signature;*type;++type) {
		if ((*type>='0' && *type<='9') || *type=='?') continue;
		if (*type=='n') made=make(compositor,wl_resource_get_client(resource),message->types[index],wl_resource_get_version(resource),arguments[index].n);
		else if (*type=='h') close(arguments[index].h);
		++index;
	}
	const auto surface_of=[](wl_resource *role) { return static_cast<wl_resource *>(wl_resource_get_user_data(role)); };
	if (request=="wl_seat.get_keyboard") compositor.keyboard=made;
	else if (request=="xdg_wm_base.get_xdg_surface") {
		wl_resource_set_user_data(made,object(1));
		compositor.surfaces[object(1)].shell=made;
	} else if (request=="xdg_surface.get_toplevel" || request=="xdg_surface.get_popup") {
		auto &shell=compositor.surfaces[surface_of(resource)];
		wl_resource_set_user_data(made,surface_of(resource));
		shell.role=made;
		shell.popup=request=="xdg_surface.get_popup";
		if (shell.popup && object(1)) shell.parent=surface_of(object(1));
	} else if (request=="xdg_popup.grab") {
		auto &shell=compositor.surfaces[surface_of(resource)];
		shell.grab=true;
		shell.denied=!compositor.pressed || arguments[1].u!=compositor.pressed;
	} else if (request=="xdg_popup.destroy") {
		const auto &shell=compositor.surfaces[surface_of(resource)];
		if (compositor.focused==surface_of(resource) && shell.parent) compositor.focus(shell.parent);
	} else if (request=="wl_surface.destroy") {
		compositor.surfaces.erase(resource);
		if (compositor.focused==resource) compositor.focused=nullptr;
	} else if (request=="wl_surface.commit") {
		const auto found=compositor.surfaces.find(resource);
		if (found!=compositor.surfaces.end() && found->second.role && !found->second.configured) {
			auto &shell=found->second;
			shell.configured=true;
			if (!shell.popup) {
				wl_array states; wl_array_init(&states);
				xdg_toplevel_send_configure(shell.role,0,0,&states);
				wl_array_release(&states);
			} else if (shell.denied) {
				xdg_popup_send_popup_done(shell.role);
				return 0;
			} else xdg_popup_send_configure(shell.role,0,0,16,16);
			xdg_surface_send_configure(shell.shell,++compositor.serial);
			if (shell.popup && shell.grab) compositor.focus(resource);
		}
	}
	if (std::string(message->name)=="destroy") wl_resource_destroy(resource);
	return 0;
}

// The window the keyboard went to last among the events that arrived, or 0 when it went nowhere.
static uint64_t focused(raven::DesktopPresentation &desktop)
{
	uint64_t window=0;
	for (const auto &event : desktop.poll(std::chrono::milliseconds(0)))
		if (event.kind==raven::DesktopEventKind::keyboard_focus) window=event.window;
	return window;
}

static void exercise(raven::DesktopPresentation &desktop,Compositor &compositor)
{
	const std::vector<uint8_t> frame(64*48*4,0xff),menu(16*16*4,0x80);
	desktop.present(1,"owner",64,48,frame);
	// Before any press the compositor refuses the grab; the popup it dismissed is forgotten, so that the same window can show it later.
	bool refused=false;
	try { desktop.popup(2,1,4,4,16,16,menu); } catch (const std::runtime_error &) { refused=true; }
	check(refused,"a popup the compositor dismissed was shown");
	// A key goes down and up; the grab quotes the press, not the newer release.
	compositor.type=true;
	bool released=false;
	for (int tries=0;tries<200 && !released;++tries)
		for (const auto &event : desktop.poll(std::chrono::milliseconds(10)))
			if (event.kind==raven::DesktopEventKind::key && !event.wparam) released=true;
	check(released,"the key did not arrive");
	// A menu opened from the window takes the keyboard as it appears.
	desktop.popup(2,1,4,4,16,16,menu);
	check(focused(desktop)==2,"the keyboard did not follow the grabbing popup");
	desktop.remove(2);
	check(focused(desktop)==1,"the keyboard did not return to the window");
	// The next menu of the menu bar, opened as the pointer moves over it, quotes the same press.
	desktop.popup(3,1,8,4,16,16,menu);
	check(focused(desktop)==3,"the keyboard did not follow the next menu");
	desktop.remove(3);
	check(focused(desktop)==1,"the keyboard did not return to the window");
}

int main()
{
	Compositor compositor;
	int sockets[2];
	if (!compositor.display || wl_display_init_shm(compositor.display) || socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets) ||
		!wl_global_create(compositor.display,&wl_compositor_interface,4,&compositor,bind<&wl_compositor_interface>) ||
		!wl_global_create(compositor.display,&xdg_wm_base_interface,1,&compositor,bind<&xdg_wm_base_interface>) ||
		!wl_global_create(compositor.display,&wl_seat_interface,1,&compositor,bind<&wl_seat_interface>) ||
		!wl_client_create(compositor.display,sockets[0])) {
		std::cerr<<"cannot start the test compositor\n";
		return 1;
	}
	// The presentation connects through the inherited socket instead of a compositor named by WAYLAND_DISPLAY.
	setenv("WAYLAND_SOCKET",std::to_string(sockets[1]).c_str(),1);
	std::thread server([&] {
		auto *loop=wl_display_get_event_loop(compositor.display);
		while (!compositor.stop) {
			if (compositor.type.exchange(false)) compositor.press();
			wl_display_flush_clients(compositor.display);
			wl_event_loop_dispatch(loop,10);
		}
	});
	std::string failure;
	try {
		raven::WaylandPresentation presentation;
		auto desktop=presentation.callbacks();
		exercise(desktop,compositor);
	} catch (const std::exception &error) { failure=error.what(); }
	compositor.stop=true;
	server.join();
	wl_display_destroy(compositor.display);
	if (!failure.empty()) { std::cerr<<failure<<'\n'; return 1; }
	return 0;
}
