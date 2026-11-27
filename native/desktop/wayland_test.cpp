#include "wayland.h"
#include "relative-pointer-unstable-v1-server-protocol.h"
#include "xdg-shell-server-protocol.h"
#include <wayland-server.h>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
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
// the popup's parent when the popup goes. Like every compositor, it releases a surface's buffer once a newer one is committed. It accepts
// and ignores every other request.
struct Compositor {
	struct Shell { wl_resource *shell=nullptr,*role=nullptr,*parent=nullptr; bool popup=false,grab=false,denied=false,configured=false; uint32_t adjustment=0; };
	wl_display *display=wl_display_create();
	wl_resource *keyboard=nullptr,*focused=nullptr,*pointer=nullptr,*relative=nullptr;
	std::map<wl_resource *,Shell> surfaces;
	// How each positioner lets the compositor move a popup that would not fit, and what the newest popup was given.
	std::map<wl_resource *,uint32_t> adjustments;
	std::atomic<uint32_t> popup_adjustment{0};
	// The subsurfaces made, how many of them lie on a window of their own, where the newest was last placed, how many popups were made
	// and whether the newest opened on a window.
	std::atomic<int> subsurfaces{0},subsurfaces_on_windows{0},placed_x{0},placed_y{0},popups{0};
	std::atomic<bool> popup_on_window{false};
	// The shared memory pools the client made, each surface's buffer attached and not yet committed, and committed; the width, format and
	// first byte of the buffer committed last.
	std::atomic<int> pools{0};
	std::map<wl_resource *,wl_resource *> attached,shown;
	std::atomic<int32_t> committed_width{0};
	std::atomic<uint32_t> committed_format{0},committed_byte{0};
	uint32_t serial=0,pressed=0;
	// Set by the test: a key is pressed and released; the next frame committed to a window is answered with a request to close it.
	std::atomic<bool> type{false},stop{false},close_on_commit{false};
	// Set by the test: the next step of the pointer (see point).
	std::atomic<int> pointer_step{0};
	// The application id the newest toplevel was given.
	std::string app_id;
	std::mutex app_id_lock;

	void focus(wl_resource *surface) {
		if (!keyboard) return;
		wl_array keys; wl_array_init(&keys);
		wl_keyboard_send_enter(keyboard,++serial,surface,&keys);
		wl_array_release(&keys);
		focused=surface;
	}

	// The pointer's steps, each a frame of its own: 1 enters the first window at 10,10; 2 moves 5,3 by relative motion alone, as labwc
	// reports a mouse over a full screen game for a while; 3 moves relatively and to 40,30; 4 moves far beyond the window, relatively.
	void point(int step) {
		if (!pointer || !relative) return;
		const auto moved=[&](double x,double y) {
			zwp_relative_pointer_v1_send_relative_motion(relative,0,0,wl_fixed_from_double(x),wl_fixed_from_double(y),
				wl_fixed_from_double(x),wl_fixed_from_double(y));
		};
		if (step==1) {
			for (const auto &[surface,shell] : surfaces)
				if (shell.role && !shell.popup) { wl_pointer_send_enter(pointer,++serial,surface,wl_fixed_from_int(10),wl_fixed_from_int(10)); break; }
		} else if (step==2) moved(5,3);
		else if (step==3) { moved(7,7); wl_pointer_send_motion(pointer,0,wl_fixed_from_int(40),wl_fixed_from_int(30)); }
		else if (step==4) moved(1000,1000);
		if (wl_resource_get_version(pointer)>=WL_POINTER_FRAME_SINCE_VERSION) wl_pointer_send_frame(pointer);
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
	if (interface==&wl_seat_interface) wl_seat_send_capabilities(resource,WL_SEAT_CAPABILITY_POINTER|WL_SEAT_CAPABILITY_KEYBOARD);
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
	else if (request=="wl_seat.get_pointer") compositor.pointer=made;
	else if (request=="zwp_relative_pointer_manager_v1.get_relative_pointer") compositor.relative=made;
	else if (request=="xdg_wm_base.get_xdg_surface") {
		wl_resource_set_user_data(made,object(1));
		compositor.surfaces[object(1)].shell=made;
	} else if (request=="xdg_surface.get_toplevel" || request=="xdg_surface.get_popup") {
		auto &shell=compositor.surfaces[surface_of(resource)];
		wl_resource_set_user_data(made,surface_of(resource));
		shell.role=made;
		shell.popup=request=="xdg_surface.get_popup";
		if (shell.popup && object(1)) shell.parent=surface_of(object(1));
		if (shell.popup) {
			const auto parent=compositor.surfaces.find(shell.parent);
			++compositor.popups;
			compositor.popup_on_window=parent!=compositor.surfaces.end() && parent->second.role && !parent->second.popup;
		}
		if (shell.popup) compositor.popup_adjustment=shell.adjustment=compositor.adjustments[object(2)];
	} else if (request=="wl_subcompositor.get_subsurface") {
		const auto parent=compositor.surfaces.find(object(2));
		++compositor.subsurfaces;
		if (parent!=compositor.surfaces.end() && parent->second.role && !parent->second.popup) ++compositor.subsurfaces_on_windows;
	} else if (request=="wl_subsurface.set_position") {
		compositor.placed_x=arguments[0].i;
		compositor.placed_y=arguments[1].i;
	} else if (request=="xdg_positioner.set_constraint_adjustment") {
		compositor.adjustments[resource]=arguments[0].u;
	} else if (request=="xdg_toplevel.set_app_id") {
		const std::lock_guard guard(compositor.app_id_lock);
		compositor.app_id=arguments[0].s ? arguments[0].s : "";
	} else if (request=="xdg_popup.grab") {
		auto &shell=compositor.surfaces[surface_of(resource)];
		shell.grab=true;
		shell.denied=!compositor.pressed || arguments[1].u!=compositor.pressed;
	} else if (request=="xdg_popup.destroy") {
		const auto &shell=compositor.surfaces[surface_of(resource)];
		if (compositor.focused==surface_of(resource) && shell.parent) compositor.focus(shell.parent);
	} else if (request=="wl_surface.attach") {
		compositor.attached[resource]=object(0);
	} else if (request=="wl_surface.destroy") {
		compositor.surfaces.erase(resource);
		compositor.attached.erase(resource);
		compositor.shown.erase(resource);
		if (compositor.focused==resource) compositor.focused=nullptr;
	} else if (request=="wl_surface.commit") {
		if (const auto next=compositor.attached.find(resource); next!=compositor.attached.end()) {
			auto &shown=compositor.shown[resource];
			if (shown && shown!=next->second) wl_buffer_send_release(shown);
			shown=next->second;
			compositor.attached.erase(next);
			if (auto *buffer=shown ? wl_shm_buffer_get(shown) : nullptr) {
				wl_shm_buffer_begin_access(buffer);
				compositor.committed_width=wl_shm_buffer_get_width(buffer);
				compositor.committed_format=wl_shm_buffer_get_format(buffer);
				compositor.committed_byte=*static_cast<const uint8_t *>(wl_shm_buffer_get_data(buffer));
				wl_shm_buffer_end_access(buffer);
			}
		}
		const auto found=compositor.surfaces.find(resource);
		if (found!=compositor.surfaces.end() && found->second.configured && !found->second.popup && compositor.close_on_commit.exchange(false))
			xdg_toplevel_send_close(found->second.role);
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
	// A mouse the compositor reports by relative motion alone moves the cursor by it, as it moves on Windows, kept to the window; motion
	// to a place puts it there.
	const auto pointed=[&](int step) {
		compositor.pointer_step=step;
		std::optional<std::pair<int32_t,int32_t>> place;
		for (int attempt=0;attempt<50 && !place;++attempt)
			for (const auto &event : desktop.poll(std::chrono::milliseconds(10)))
				if (event.kind==raven::DesktopEventKind::pointer_motion && event.window==1) place=std::pair{event.x,event.y};
		return place.value_or(std::pair{-1,-1});
	};
	check(pointed(1)==std::pair{10,10},"the pointer did not enter the window");
	check(pointed(2)==std::pair{15,13},"relative motion alone did not move the cursor");
	check(pointed(3)==std::pair{40,30},"motion to a place did not put the cursor there");
	check(pointed(4)==std::pair{63,47},"relative motion took the cursor out of the window");
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
	// A menu that would leave the screen opens on the other side of where it was asked for, as Windows opens one near the bottom upwards,
	// and slides along an edge when neither side has room.
	const uint32_t moves=XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X|XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y|
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X|XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y;
	check((compositor.popup_adjustment&moves)==moves,"a menu cannot be moved back onto the screen");
	desktop.remove(2);
	check(focused(desktop)==1,"the keyboard did not return to the window");
	// The next menu of the menu bar, opened as the pointer moves over it, quotes the same press.
	desktop.popup(3,1,8,4,16,16,menu);
	check(focused(desktop)==3,"the keyboard did not follow the next menu");
	desktop.remove(3);
	check(focused(desktop)==1,"the keyboard did not return to the window");
	// A window the first owns that takes no grab, as a palette, lies on it as a subsurface: no popup of the shell, which a compositor
	// keeps over every window. Moved, it is placed again and not made again; a menu it opens is a popup of the window it lies on.
	const int popups_before=compositor.popups;
	desktop.owned_popup(5,1,10,12,16,16,menu);
	desktop.poll(std::chrono::milliseconds(50));
	check(compositor.subsurfaces==1 && compositor.subsurfaces_on_windows==1 && compositor.popups==popups_before,
		"an owned window without a grab was not attached to its owner's window");
	check(compositor.placed_x==10 && compositor.placed_y==12,"the owned window is not where it was put on its owner");
	desktop.owned_popup(5,1,30,40,16,16,menu);
	desktop.poll(std::chrono::milliseconds(50));
	check(compositor.subsurfaces==1 && compositor.placed_x==30 && compositor.placed_y==40,"a moved owned window was not placed again");
	desktop.popup(6,5,2,3,16,16,menu);
	check(compositor.popups==popups_before+1 && compositor.popup_on_window,"a menu of an owned window did not open on its owner's window");
	desktop.remove(6);
	desktop.remove(5);
	// The compositor asks to close the window while a frame of it is being shown, as labwc does for Alt+F4: the frame is shown, the
	// program gets WM_CLOSE, and the window it keeps goes on showing its frames.
	const std::vector<uint8_t> changed(64*48*4,0x40);
	compositor.close_on_commit=true;
	desktop.present(1,"owner",64,48,changed);
	bool asked=false;
	for (int attempt=0;attempt<50 && !asked;++attempt)
		for (const auto &event : desktop.poll(std::chrono::milliseconds(10)))
			asked=asked || (event.window==1 && event.message==0x10);
	check(asked,"the compositor's close did not reach the window");
	desktop.present(1,"owner",64,48,frame);
	// A window that changes size at each step, as while the user drags its edge, cuts its new buffers from the pools it has. Each buffer
	// shows its own frame, at its own width.
	const int pools_before=compositor.pools;
	for (uint32_t width=200;width<220;++width) {
		const std::vector<uint8_t> sized(size_t(width)*80*4,uint8_t(width));
		desktop.present(9,"sizes",width,80,sized);
		check(compositor.committed_width==int32_t(width) && compositor.committed_byte==uint8_t(width) &&
			compositor.committed_format==WL_SHM_FORMAT_XRGB8888,"a buffer cut again for another size shows another frame");
	}
	std::cout<<"buffer pools for 20 sizes: "<<compositor.pools-pools_before<<'\n';
	check(compositor.pools-pools_before<=3,"a window made a new buffer pool for each of its sizes");
	// A buffer cut again for a frame with alpha says so; a frame larger than every pool gets a new one.
	desktop.translucent(9,true);
	desktop.present(9,"sizes",219,80,std::vector<uint8_t>(219*80*4,0x30));
	check(compositor.committed_format==WL_SHM_FORMAT_ARGB8888 && compositor.committed_byte==0x30,"a frame with alpha is shown without it");
	const int pools_small=compositor.pools;
	desktop.present(9,"sizes",600,80,std::vector<uint8_t>(600*80*4,0x50));
	check(compositor.committed_width==600 && compositor.committed_byte==0x50 && compositor.pools==pools_small+1,"a larger frame is not shown whole");
	// A window shown inside a window of another program names it in its application id, for that program's compositor (native/vst);
	// shown on its own again, it is the runtime's window as before.
	const auto app_id=[&] { const std::lock_guard guard(compositor.app_id_lock); return compositor.app_id; };
	desktop.embed(10,42);
	desktop.present(10,"editor",32,24,std::vector<uint8_t>(32*24*4,0x60));
	check(app_id()=="raven-embed-42","an embedded window does not name the window it shows inside");
	desktop.embed(10,0);
	desktop.present(10,"editor",32,24,std::vector<uint8_t>(32*24*4,0x60));
	check(app_id()=="raven.native","a window shown on its own again still names another program's window");
}

int main()
{
	Compositor compositor;
	int sockets[2];
	if (!compositor.display || wl_display_init_shm(compositor.display) || socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets) ||
		!wl_global_create(compositor.display,&wl_compositor_interface,4,&compositor,bind<&wl_compositor_interface>) ||
		!wl_global_create(compositor.display,&wl_subcompositor_interface,1,&compositor,bind<&wl_subcompositor_interface>) ||
		!wl_global_create(compositor.display,&xdg_wm_base_interface,1,&compositor,bind<&xdg_wm_base_interface>) ||
		!wl_global_create(compositor.display,&wl_seat_interface,5,&compositor,bind<&wl_seat_interface>) ||
		!wl_global_create(compositor.display,&zwp_relative_pointer_manager_v1_interface,1,&compositor,
			bind<&zwp_relative_pointer_manager_v1_interface>) ||
		!wl_client_create(compositor.display,sockets[0])) {
		std::cerr<<"cannot start the test compositor\n";
		return 1;
	}
	auto *logger=wl_display_add_protocol_logger(compositor.display,[](void *data,wl_protocol_logger_type type,const wl_protocol_logger_message *message) {
		if (type==WL_PROTOCOL_LOGGER_REQUEST && std::string(wl_resource_get_class(message->resource))=="wl_shm" &&
			std::string(message->message->name)=="create_pool")
			++static_cast<Compositor *>(data)->pools;
	},&compositor);
	// The presentation connects through the inherited socket instead of a compositor named by WAYLAND_DISPLAY.
	setenv("WAYLAND_SOCKET",std::to_string(sockets[1]).c_str(),1);
	std::thread server([&] {
		auto *loop=wl_display_get_event_loop(compositor.display);
		while (!compositor.stop) {
			if (compositor.type.exchange(false)) compositor.press();
			if (const int step=compositor.pointer_step.exchange(0)) compositor.point(step);
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
	wl_protocol_logger_destroy(logger);
	wl_display_destroy(compositor.display);
	if (!failure.empty()) { std::cerr<<failure<<'\n'; return 1; }
	return 0;
}
