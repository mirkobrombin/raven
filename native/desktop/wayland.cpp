#include "wayland.h"
#include "xdg-shell-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#ifdef RAVEN_WAYLAND_CURSOR_SHAPE
#include "cursor-shape-v1-client-protocol.h"
#endif

#include <wayland-client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <optional>
#include <set>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <poll.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace raven {

#ifdef RAVEN_WAYLAND_CURSOR_SHAPE
// The named shape that looks like a system cursor of Windows; the ones without a counterpart show the default pointer.
static uint32_t cursor_shape(uint32_t cursor)
{
	switch (cursor) {
	case 32513: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
	case 32514: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT;
	case 32515: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
	case 32642: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE;
	case 32643: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE;
	case 32644: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
	case 32645: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
	case 32640: case 32646: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE;
	case 32648: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED;
	case 32649: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
	case 32650: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS;
	case 32651: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP;
	default: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
	}
}
#endif

struct WaylandPresentation::State {
	struct Output {
		wl_output *proxy=nullptr;
		zxdg_output_v1 *logical=nullptr;
		struct Geometry { int32_t x=0,y=0,width=0,height=0; } mode, pending, current;
		int32_t scale=1,transform=0;
		void commit() {
			current=pending;
			if (current.width>0 && current.height>0) return;
			current=mode;
			if (transform & 1) std::swap(current.width,current.height);
			current.width/=scale; current.height/=scale;
		}
		~Output() {
			if (logical) zxdg_output_v1_destroy(logical);
			if (proxy) {
				if (wl_output_get_version(proxy)>=3) wl_output_release(proxy);
				else wl_output_destroy(proxy);
			}
		}
	};
	struct Buffer {
		wl_buffer *proxy=nullptr;
		void *pixels=MAP_FAILED;
		size_t size=0;
		uint32_t width=0,height=0;
		// What changed since this buffer last received a frame (left, top, right, bottom): only that part is copied into it again.
		std::array<int32_t,4> stale{0,0,0,0};
		bool released=false;
		~Buffer() { if (proxy) wl_buffer_destroy(proxy); if (pixels!=MAP_FAILED) munmap(pixels,size); }
	};
	struct Surface {
		wl_surface *proxy=nullptr;
		xdg_surface *shell=nullptr;
		xdg_toplevel *role=nullptr;
		xdg_popup *popup=nullptr;
		bool is_popup=false;
		// Where a popup sits on its parent, and whether it took a grab.
		int32_t popup_x=0,popup_y=0;
		bool grabbed=false;
		bool configured=false, closed=false, resized=false, close_posted=false, first_frame=false, maximized=false;
		uint32_t width=0,height=0,pending_width=0,pending_height=0;
		std::vector<std::unique_ptr<Buffer>> buffers;
		// The buffer of the last commit, whose pixels are what the compositor shows, and the hash of each of its rows.
		Buffer *shown=nullptr;
		std::vector<uint64_t> row_hashes;
		// A surface a graphics driver renders to, stacked over the client area.
		wl_surface *child=nullptr;
		wl_subsurface *child_role=nullptr;
		void drop_child() {
			if (child_role) wl_subsurface_destroy(child_role);
			if (child) wl_surface_destroy(child);
			child_role=nullptr; child=nullptr;
		}
		~Surface() {
			drop_child();
			if (popup) xdg_popup_destroy(popup);
			if (role) xdg_toplevel_destroy(role);
			if (shell) xdg_surface_destroy(shell);
			if (proxy) wl_surface_destroy(proxy);
		}
	};
	wl_display *display=nullptr;
	wl_registry *registry=nullptr;
	wl_compositor *compositor=nullptr;
	wl_subcompositor *subcompositor=nullptr;
	wl_shm *shm=nullptr;
	zxdg_output_manager_v1 *output_manager=nullptr;
	std::map<uint32_t,std::unique_ptr<Output>> outputs;
	xdg_wm_base *shell=nullptr;
	wl_seat *seat=nullptr;
	wl_pointer *pointer=nullptr;
	wl_keyboard *keyboard=nullptr;
	// Relative motion of the mouse and the constraint that keeps the pointer in (or holds it on) one window.
	zwp_relative_pointer_manager_v1 *relative_manager=nullptr;
	zwp_relative_pointer_v1 *relative=nullptr;
	zwp_pointer_constraints_v1 *constraints=nullptr;
	zwp_confined_pointer_v1 *confined=nullptr;
	zwp_locked_pointer_v1 *locked=nullptr;
	int64_t relative_x=0,relative_y=0;
	uint64_t constrained_window=0;
#ifdef RAVEN_WAYLAND_CURSOR_SHAPE
	wp_cursor_shape_manager_v1 *shape_manager=nullptr;
	wp_cursor_shape_device_v1 *shape_device=nullptr;
#endif
	// The cursor the program shows and the serial of the pointer's arrival on one of its surfaces, which setting the cursor quotes.
	uint32_t cursor=32512,enter_serial=0;
	bool pointer_inside=false;
	uint64_t pointer_window=0;
	uint64_t keyboard_window=0;
	int32_t pointer_x=0,pointer_y=0;
	bool input_failed=false;
	// Serial of the newest input event; popup grabs must quote one.
	uint32_t last_serial=0;
	// Pointer buttons held down, and a window whose move the window menu asked for while none was: the compositor starts a move only
	// from a press, so the move begins with the next press on that window, as a click ends the move on Windows; a key cancels it.
	uint32_t buttons_down=0;
	uint64_t pending_move=0;
	const char *input_error="unsupported native input event or input queue exhaustion";
	std::vector<DesktopEvent> pending;
	// Host selection: text offered by other clients is fetched on demand, text from the guest is served to whoever asks.
	wl_data_device_manager *data_manager=nullptr;
	wl_data_device *data_device=nullptr;
	wl_data_source *data_source=nullptr;
	std::string source_text;
	std::map<wl_data_offer *,std::set<std::string>> offer_mimes;
	wl_data_offer *selection_offer=nullptr;
	wl_data_offer *drag_offer=nullptr;
	uint64_t selection_counter=0;
	std::map<uint64_t,std::unique_ptr<Surface>> surfaces;
	std::set<uint64_t> maximized;

	~State() {
		release_constraint();
		if (relative) zwp_relative_pointer_v1_destroy(relative);
		if (relative_manager) zwp_relative_pointer_manager_v1_destroy(relative_manager);
		if (constraints) zwp_pointer_constraints_v1_destroy(constraints);
#ifdef RAVEN_WAYLAND_CURSOR_SHAPE
		if (shape_device) wp_cursor_shape_device_v1_destroy(shape_device);
		if (shape_manager) wp_cursor_shape_manager_v1_destroy(shape_manager);
#endif
		surfaces.clear();
		outputs.clear();
		if (output_manager) zxdg_output_manager_v1_destroy(output_manager);
		if (data_source) wl_data_source_destroy(data_source);
		if (selection_offer) wl_data_offer_destroy(selection_offer);
		if (drag_offer) wl_data_offer_destroy(drag_offer);
		if (data_device) wl_data_device_destroy(data_device);
		if (data_manager) wl_data_device_manager_destroy(data_manager);
		if (pointer) wl_pointer_destroy(pointer);
		if (keyboard) wl_keyboard_destroy(keyboard);
		if (seat) wl_seat_destroy(seat);
		if (shell) xdg_wm_base_destroy(shell);
		if (shm) wl_shm_destroy(shm);
		if (subcompositor) wl_subcompositor_destroy(subcompositor);
		if (compositor) wl_compositor_destroy(compositor);
		if (registry) wl_registry_destroy(registry);
		if (display) { wl_display_flush(display); wl_display_disconnect(display); }
	}

	int64_t wheel_remainder=0,horizontal_remainder=0;

	void release_constraint() {
		if (confined) { zwp_confined_pointer_v1_destroy(confined); confined=nullptr; }
		if (locked) { zwp_locked_pointer_v1_destroy(locked); locked=nullptr; }
		constrained_window=0;
	}

	void confine(uint64_t handle,std::optional<std::array<int32_t,4>> rectangle) {
		release_constraint();
		const auto found=surfaces.find(handle);
		if (rectangle && constraints && pointer && found!=surfaces.end() && !found->second->is_popup) {
			const auto &value=*rectangle;
			if (value[2]-value[0]<=2 && value[3]-value[1]<=2) {
				locked=zwp_pointer_constraints_v1_lock_pointer(constraints,found->second->proxy,pointer,nullptr,ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
			} else {
				wl_region *region=wl_compositor_create_region(compositor);
				wl_region_add(region,value[0],value[1],std::max(value[2]-value[0],1),std::max(value[3]-value[1],1));
				confined=zwp_pointer_constraints_v1_confine_pointer(constraints,found->second->proxy,pointer,region,ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
				wl_region_destroy(region);
			}
			constrained_window=handle;
		}
		if (display) wl_display_flush(display);
	}

	void watch_relative_motion() {
		if (relative || !relative_manager || !pointer) return;
		relative=zwp_relative_pointer_manager_v1_get_relative_pointer(relative_manager,pointer);
		static const zwp_relative_pointer_v1_listener listener{
			[](void *data,zwp_relative_pointer_v1 *,uint32_t,uint32_t,wl_fixed_t,wl_fixed_t,wl_fixed_t dx,wl_fixed_t dy) {
				// The unaccelerated movement in whole units; the fractions add up across reports.
				auto &state=*static_cast<State *>(data);
				state.relative_x+=dx; state.relative_y+=dy;
				const auto x=int32_t(state.relative_x/256),y=int32_t(state.relative_y/256);
				state.relative_x-=int64_t(x)*256; state.relative_y-=int64_t(y)*256;
				if ((!x && !y) || state.pending.size()>=10000) return;
				try { state.pending.push_back({state.pointer_window,0,0,0,DesktopEventKind::pointer_relative,x,y}); }
				catch (...) { state.input_failed=true; }
			}};
		zwp_relative_pointer_v1_add_listener(relative,&listener,this);
	}

	void watch_cursor_shape() {
#ifdef RAVEN_WAYLAND_CURSOR_SHAPE
		if (shape_device || !shape_manager || !pointer) return;
		shape_device=wp_cursor_shape_manager_v1_get_pointer(shape_manager,pointer);
#endif
	}

	void show_cursor() {
		if (!pointer || !pointer_inside) return;
		if (!cursor) wl_pointer_set_cursor(pointer,enter_serial,nullptr,0,0);
#ifdef RAVEN_WAYLAND_CURSOR_SHAPE
		else if (shape_device) wp_cursor_shape_device_v1_set_shape(shape_device,enter_serial,cursor_shape(cursor));
#endif
		if (display) wl_display_flush(display);
	}

	void pointer_event(DesktopEventKind kind,uint32_t button=0,uint32_t pressed=0) {
		if (!pointer_window && kind==DesktopEventKind::pointer_motion) return;
		if (pending.size()>=10000) { input_failed=true; return; }
		try { pending.push_back({pointer_window,button,pressed,0,kind,pointer_x,pointer_y}); }
		catch (...) { input_failed=true; }
	}

	void keyboard_event(DesktopEvent event) {
		if (pending.size()>=10000) { input_failed=true; return; }
		try { pending.push_back(std::move(event)); } catch (...) { input_failed=true; }
	}

	void keymap_event(uint32_t format,int descriptor,uint32_t size) {
		struct stat info{};
		if (format!=WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || !size || size>1024*1024 || fstat(descriptor,&info)<0 || info.st_size<size) {
			close(descriptor); input_failed=true; return;
		}
		const auto *mapping=static_cast<const char *>(mmap(nullptr,size,PROT_READ,MAP_PRIVATE,descriptor,0)); close(descriptor);
		if (mapping==MAP_FAILED) { input_failed=true; return; }
		try {
			size_t bytes=0; for (const auto &event : pending) bytes+=event.keymap.size();
			// The protocol does not require the terminating null: some compositors (niri) end the text at its last character.
			const uint32_t length=mapping[size-1] ? size : size-1;
			if (bytes+length>4*1024*1024) input_failed=true;
			else { DesktopEvent event{0,0}; event.kind=DesktopEventKind::keymap; event.keymap.assign(mapping,length); keyboard_event(std::move(event)); }
		} catch (...) { input_failed=true; }
		munmap(const_cast<char *>(mapping),size);
	}

	static int32_t coordinate(wl_fixed_t value) {
		return value>=0 ? value/256 : int32_t(-((-int64_t(value)+255)/256));
	}

	static void capabilities(void *data,wl_seat *seat,uint32_t flags) {
		auto &state=*static_cast<State *>(data);
		if ((flags & WL_SEAT_CAPABILITY_POINTER) && !state.pointer) {
			state.pointer=wl_seat_get_pointer(seat);
			static const auto listener=[] {
				wl_pointer_listener value{};
				value.enter=[](void *data,wl_pointer *,uint32_t serial,wl_surface *surface,wl_fixed_t x,wl_fixed_t y) {
					auto &state=*static_cast<State *>(data); state.pointer_window=0; state.last_serial=serial;
					for (const auto &[handle,window] : state.surfaces) if (window->proxy==surface) state.pointer_window=handle;
					state.enter_serial=serial; state.pointer_inside=true; state.show_cursor();
					state.pointer_x=coordinate(x); state.pointer_y=coordinate(y); state.pointer_event(DesktopEventKind::pointer_motion);
				};
				value.leave=[](void *data,wl_pointer *,uint32_t,wl_surface *) {
					auto &state=*static_cast<State *>(data); state.pointer_window=0; state.pointer_inside=false;
				};
				value.motion=[](void *data,wl_pointer *,uint32_t,wl_fixed_t x,wl_fixed_t y) {
					auto &state=*static_cast<State *>(data); state.pointer_x=coordinate(x); state.pointer_y=coordinate(y);
					state.pointer_event(DesktopEventKind::pointer_motion);
				};
				value.button=[](void *data,wl_pointer *,uint32_t serial,uint32_t,uint32_t button,uint32_t pressed) {
					auto &state=*static_cast<State *>(data);
					state.last_serial=serial;
					state.buttons_down=pressed ? state.buttons_down+1 : state.buttons_down ? state.buttons_down-1 : 0;
					if (pressed && state.pending_move) {
						const auto found=state.surfaces.find(state.pending_move);
						const bool here=state.pending_move==state.pointer_window;
						state.pending_move=0;
						if (here && found!=state.surfaces.end() && found->second->role) {
							xdg_toplevel_move(found->second->role,state.seat,serial);
							state.buttons_down=0;
							return;
						}
					}
					state.pointer_event(DesktopEventKind::pointer_button,button,pressed);
				};
				value.axis=[](void *data,wl_pointer *,uint32_t,uint32_t axis,wl_fixed_t amount) {
					auto &state=*static_cast<State *>(data);
					// A notch reports about ten units. Scrolling down is positive and becomes a negative wheel delta; scrolling right keeps its sign.
					const bool horizontal=axis==WL_POINTER_AXIS_HORIZONTAL_SCROLL;
					if ((!horizontal && axis!=WL_POINTER_AXIS_VERTICAL_SCROLL) || !state.pointer_window) return;
					auto &remainder=horizontal ? state.horizontal_remainder : state.wheel_remainder;
					remainder+=(horizontal ? 1 : -1)*int64_t(amount)*120/(10*256);
					const auto delta=int32_t(remainder/120)*120;
					remainder-=delta;
					if (delta) state.pointer_event(DesktopEventKind::pointer_wheel,uint32_t(delta),horizontal ? 1 : 0);
				};
				return value;
			}();
			wl_pointer_add_listener(state.pointer,&listener,&state);
			state.watch_relative_motion();
			state.watch_cursor_shape();
		} else if (!(flags & WL_SEAT_CAPABILITY_POINTER) && state.pointer) {
			state.release_constraint();
			if (state.relative) { zwp_relative_pointer_v1_destroy(state.relative); state.relative=nullptr; }
#ifdef RAVEN_WAYLAND_CURSOR_SHAPE
			if (state.shape_device) { wp_cursor_shape_device_v1_destroy(state.shape_device); state.shape_device=nullptr; }
#endif
			wl_pointer_destroy(state.pointer); state.pointer=nullptr; state.pointer_window=0; state.pointer_inside=false;
		}
		if ((flags & WL_SEAT_CAPABILITY_KEYBOARD) && !state.keyboard) {
			state.keyboard=wl_seat_get_keyboard(seat);
			static const auto listener=[] {
				wl_keyboard_listener value{};
				value.keymap=[](void *data,wl_keyboard *,uint32_t format,int32_t descriptor,uint32_t size) {
					static_cast<State *>(data)->keymap_event(format,descriptor,size);
				};
				value.enter=[](void *data,wl_keyboard *,uint32_t,wl_surface *surface,wl_array *keys) {
					auto &state=*static_cast<State *>(data);
					if (keys->size%sizeof(uint32_t) || keys->size>256*sizeof(uint32_t) || (keys->size && !keys->data)) {
						state.input_error="invalid native keyboard focus key array"; state.input_failed=true; return;
					}
					state.keyboard_window=0;
					for (const auto &[handle,window] : state.surfaces) if (window->proxy==surface) state.keyboard_window=handle;
					if (std::getenv("RAVEN_TRACE_KEYS")) fprintf(stderr,"keyboard enter %p -> %llx\n",(void *)surface,(unsigned long long)state.keyboard_window);
					if (!state.keyboard_window) { state.input_error="keyboard entered an unknown native surface"; state.input_failed=true; return; }
					DesktopEvent event{state.keyboard_window,0,0,0,DesktopEventKind::keyboard_focus};
					try {
						event.held_keys.resize(keys->size/sizeof(uint32_t));
						if (keys->size) std::memcpy(event.held_keys.data(),keys->data,keys->size);
					} catch (...) { state.input_error="cannot retain native keyboard focus keys"; state.input_failed=true; return; }
					state.keyboard_event(std::move(event));
				};
				value.leave=[](void *data,wl_keyboard *,uint32_t,wl_surface *surface) {
					auto &state=*static_cast<State *>(data); state.keyboard_window=0;
					if (std::getenv("RAVEN_TRACE_KEYS")) fprintf(stderr,"keyboard leave %p\n",(void *)surface);
					state.keyboard_event({0,0,0,0,DesktopEventKind::keyboard_focus});
				};
				value.key=[](void *data,wl_keyboard *,uint32_t serial,uint32_t,uint32_t key,uint32_t pressed) {
					auto &state=*static_cast<State *>(data);
					state.last_serial=serial;
					if (pressed) state.pending_move=0;
					if (std::getenv("RAVEN_TRACE_KEYS")) {
						timespec now{}; clock_gettime(CLOCK_REALTIME,&now);
						fprintf(stderr,"key %u %s at %lld ns\n",key,pressed ? "down" : "up",(long long)now.tv_sec*1000000000LL+now.tv_nsec);
					}
					state.keyboard_event({state.keyboard_window,key,pressed,0,DesktopEventKind::key});
				};
				value.modifiers=[](void *data,wl_keyboard *,uint32_t,uint32_t depressed,uint32_t latched,uint32_t locked,uint32_t group) {
					static_cast<State *>(data)->keyboard_event({0,depressed,latched,uint64_t(locked)|(uint64_t(group)<<32),DesktopEventKind::modifiers});
				};
				return value;
			}();
			wl_keyboard_add_listener(state.keyboard,&listener,&state);
		} else if (!(flags & WL_SEAT_CAPABILITY_KEYBOARD) && state.keyboard) {
			wl_keyboard_destroy(state.keyboard); state.keyboard=nullptr;
			state.keyboard_window=0;
			state.keyboard_event({0,0,0,0,DesktopEventKind::keyboard_focus});
			state.keyboard_event({0,0,0,0,DesktopEventKind::modifiers});
		}
	}

	static constexpr const char *text_mimes[]={"text/plain;charset=utf-8","text/plain;charset=UTF-8","UTF8_STRING","text/plain"};

	void release_drag_offer() {
		if (!drag_offer) return;
		offer_mimes.erase(drag_offer);
		wl_data_offer_destroy(drag_offer);
		drag_offer=nullptr;
	}

	void make_data_device() {
		if (data_device || !data_manager || !seat) return;
		data_device=wl_data_device_manager_get_data_device(data_manager,seat);
		static const wl_data_device_listener listener{
			[](void *data,wl_data_device *,wl_data_offer *offer) {
				auto &state=*static_cast<State *>(data);
				state.offer_mimes[offer].clear();
				static const wl_data_offer_listener offers{[](void *data,wl_data_offer *offer,const char *mime) {
					static_cast<State *>(data)->offer_mimes[offer].insert(mime);
				},[](void *,wl_data_offer *,uint32_t) {},[](void *,wl_data_offer *,uint32_t) {}};
				wl_data_offer_add_listener(offer,&offers,&state);
			},
			// libwayland aborts on an event without a handler. Windows are no drop targets yet, so a dragged offer is only released.
			[](void *data,wl_data_device *,uint32_t,wl_surface *,wl_fixed_t,wl_fixed_t,wl_data_offer *offer) {
				auto &state=*static_cast<State *>(data);
				state.release_drag_offer();
				state.drag_offer=offer;
			},
			[](void *data,wl_data_device *) { static_cast<State *>(data)->release_drag_offer(); },
			[](void *,wl_data_device *,uint32_t,wl_fixed_t,wl_fixed_t) {},
			[](void *data,wl_data_device *) { static_cast<State *>(data)->release_drag_offer(); },
			[](void *data,wl_data_device *,wl_data_offer *offer) {
				auto &state=*static_cast<State *>(data);
				if (state.selection_offer && state.selection_offer!=offer) { state.offer_mimes.erase(state.selection_offer); wl_data_offer_destroy(state.selection_offer); }
				state.selection_offer=offer; ++state.selection_counter;
			}};
		wl_data_device_add_listener(data_device,&listener,this);
	}

	void publish_text(std::string_view text) {
		if (!data_device || !data_manager) return;
		if (data_source) { wl_data_source_destroy(data_source); data_source=nullptr; }
		source_text.assign(text);
		data_source=wl_data_device_manager_create_data_source(data_manager);
		static const wl_data_source_listener listener{
			[](void *,wl_data_source *,const char *) {},
			[](void *data,wl_data_source *,const char *,int32_t descriptor) {
				auto &state=*static_cast<State *>(data);
				size_t done=0;
				while (done<state.source_text.size()) {
					const auto wrote=write(descriptor,state.source_text.data()+done,state.source_text.size()-done);
					if (wrote<=0) break;
					done+=size_t(wrote);
				}
				close(descriptor);
			},
			[](void *data,wl_data_source *source) {
				auto &state=*static_cast<State *>(data);
				if (state.data_source==source) state.data_source=nullptr;
				wl_data_source_destroy(source);
			},[](void *,wl_data_source *) {},[](void *,wl_data_source *) {},[](void *,wl_data_source *,uint32_t) {}};
		wl_data_source_add_listener(data_source,&listener,this);
		for (const auto *mime : text_mimes) wl_data_source_offer(data_source,mime);
		wl_data_device_set_selection(data_device,data_source,last_serial);
		wl_display_flush(display);
	}

	std::optional<std::pair<uint64_t,std::string>> fetch_text() {
		if (!data_device || !selection_offer || data_source) return std::nullopt;
		const auto known=offer_mimes.find(selection_offer);
		if (known==offer_mimes.end()) return std::nullopt;
		const char *mime=nullptr;
		for (const auto *wanted : text_mimes) if (known->second.contains(wanted)) { mime=wanted; break; }
		if (!mime) return std::nullopt;
		int pipes[2];
		if (pipe2(pipes,O_CLOEXEC)<0) return std::nullopt;
		wl_data_offer_receive(selection_offer,mime,pipes[1]);
		close(pipes[1]);
		wl_display_flush(display);
		std::string text;
		const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
		for (;;) {
			pollfd entry{pipes[0],POLLIN,0};
			const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(limit-std::chrono::steady_clock::now()).count();
			if (left<=0 || ::poll(&entry,1,int(left))<=0) break;
			char buffer[4096];
			const auto count=read(pipes[0],buffer,sizeof buffer);
			if (count<=0) break;
			text.append(buffer,size_t(count));
			if (text.size()>(16U<<20)) break;
		}
		close(pipes[0]);
		return std::make_pair(selection_counter,std::move(text));
	}

	void watch_output(Output &output) {
		if (!output_manager || output.logical) return;
		output.logical=zxdg_output_manager_v1_get_xdg_output(output_manager,output.proxy);
		static const zxdg_output_v1_listener listener{
			[](void *data,zxdg_output_v1 *,int32_t x,int32_t y) { auto &o=*static_cast<Output *>(data); o.pending.x=x; o.pending.y=y; },
			[](void *data,zxdg_output_v1 *,int32_t width,int32_t height) { auto &o=*static_cast<Output *>(data); o.pending.width=width; o.pending.height=height; },
			[](void *data,zxdg_output_v1 *) { static_cast<Output *>(data)->commit(); },
			[](void *,zxdg_output_v1 *,const char *) {}, [](void *,zxdg_output_v1 *,const char *) {}};
		zxdg_output_v1_add_listener(output.logical,&listener,&output);
	}

	std::optional<std::pair<int32_t,int32_t>> screen_size() const {
		const Output *selected=nullptr;
		for (const auto &[name,output] : outputs) {
			(void)name;
			if (output->current.width<=0 || output->current.height<=0) continue;
			if (!selected) selected=output.get();
			if (!output->current.x && !output->current.y) { selected=output.get(); break; }
		}
		if (!selected) return std::nullopt;
		return std::pair{selected->current.width,selected->current.height};
	}

	static void global(void *data,wl_registry *registry,uint32_t name,const char *interface,uint32_t version) {
		auto &state=*static_cast<State *>(data);
		if (!std::strcmp(interface,"wl_compositor")) state.compositor=static_cast<wl_compositor *>(wl_registry_bind(registry,name,&wl_compositor_interface,std::min(version,4U)));
		else if (!std::strcmp(interface,"wl_subcompositor")) state.subcompositor=static_cast<wl_subcompositor *>(wl_registry_bind(registry,name,&wl_subcompositor_interface,1));
		else if (!std::strcmp(interface,"wl_shm")) state.shm=static_cast<wl_shm *>(wl_registry_bind(registry,name,&wl_shm_interface,1));
		else if (!std::strcmp(interface,"wl_output")) {
			auto output=std::make_unique<Output>();
			output->proxy=static_cast<wl_output *>(wl_registry_bind(registry,name,&wl_output_interface,std::min(version,4U)));
			static const wl_output_listener listener{
				[](void *data,wl_output *proxy,int32_t x,int32_t y,int32_t,int32_t,int32_t,const char *,const char *,int32_t transform) {
					auto &o=*static_cast<Output *>(data); o.mode.x=x; o.mode.y=y; o.transform=transform;
					if (wl_output_get_version(proxy)<2) o.commit();
				},
				[](void *data,wl_output *proxy,uint32_t flags,int32_t width,int32_t height,int32_t) {
					auto &o=*static_cast<Output *>(data);
					if (flags & WL_OUTPUT_MODE_CURRENT) { o.mode.width=width; o.mode.height=height; }
					if (wl_output_get_version(proxy)<2) o.commit();
				},
				[](void *data,wl_output *) { static_cast<Output *>(data)->commit(); },
				[](void *data,wl_output *,int32_t scale) { static_cast<Output *>(data)->scale=std::max(scale,1); },
				[](void *,wl_output *,const char *) {}, [](void *,wl_output *,const char *) {}};
			wl_output_add_listener(output->proxy,&listener,output.get());
			state.watch_output(*output);
			state.outputs.emplace(name,std::move(output));
		}
		else if (!std::strcmp(interface,"zxdg_output_manager_v1")) {
			// Version two supplies its own done event, so logical changes are committed together on every wl_output version.
			state.output_manager=static_cast<zxdg_output_manager_v1 *>(wl_registry_bind(registry,name,&zxdg_output_manager_v1_interface,std::min(version,2U)));
			for (auto &[id,output] : state.outputs) { (void)id; state.watch_output(*output); }
		}
		else if (!std::strcmp(interface,"wl_seat")) {
			if (state.seat) { state.input_failed=true; return; }
			state.seat=static_cast<wl_seat *>(wl_registry_bind(registry,name,&wl_seat_interface,1));
			static const wl_seat_listener listener{capabilities,nullptr};
			wl_seat_add_listener(state.seat,&listener,&state);
			state.make_data_device();
		}
		else if (!std::strcmp(interface,"wl_data_device_manager")) {
			state.data_manager=static_cast<wl_data_device_manager *>(wl_registry_bind(registry,name,&wl_data_device_manager_interface,std::min(version,3U)));
			state.make_data_device();
		}
		else if (!std::strcmp(interface,"zwp_relative_pointer_manager_v1")) {
			state.relative_manager=static_cast<zwp_relative_pointer_manager_v1 *>(wl_registry_bind(registry,name,&zwp_relative_pointer_manager_v1_interface,1));
			state.watch_relative_motion();
		}
#ifdef RAVEN_WAYLAND_CURSOR_SHAPE
		else if (!std::strcmp(interface,"wp_cursor_shape_manager_v1")) {
			state.shape_manager=static_cast<wp_cursor_shape_manager_v1 *>(wl_registry_bind(registry,name,&wp_cursor_shape_manager_v1_interface,1));
			state.watch_cursor_shape();
		}
#endif
		else if (!std::strcmp(interface,"zwp_pointer_constraints_v1"))
			state.constraints=static_cast<zwp_pointer_constraints_v1 *>(wl_registry_bind(registry,name,&zwp_pointer_constraints_v1_interface,1));
		else if (!std::strcmp(interface,"xdg_wm_base")) {
			state.shell=static_cast<xdg_wm_base *>(wl_registry_bind(registry,name,&xdg_wm_base_interface,1));
			static const xdg_wm_base_listener listener{[](void *,xdg_wm_base *base,uint32_t serial) { xdg_wm_base_pong(base,serial); }};
			xdg_wm_base_add_listener(state.shell,&listener,nullptr);
		}
	}

	void events(std::chrono::milliseconds budget) {
		if (wl_display_dispatch_pending(display)<0) throw std::runtime_error("Wayland event dispatch failed");
		while (wl_display_prepare_read(display)!=0)
			if (wl_display_dispatch_pending(display)<0) throw std::runtime_error("Wayland pending events failed");
		const auto flushed=wl_display_flush(display);
		pollfd socket{wl_display_get_fd(display),short(POLLIN | (flushed<0 ? POLLOUT : 0)),0};
		const auto ready=poll(&socket,1,int(budget.count()));
		const auto polling_error=ready<0 ? errno : 0;
		if (ready<=0 || !(socket.revents & POLLIN)) wl_display_cancel_read(display);
		else if (wl_display_read_events(display)<0) throw std::runtime_error("Wayland socket read failed");
		if ((ready<0 && polling_error!=EINTR) || (socket.revents & (POLLERR|POLLHUP|POLLNVAL)) || wl_display_dispatch_pending(display)<0)
			throw std::runtime_error("Wayland connection failed");
		if (input_failed) throw std::runtime_error(input_error);
	}

	void sync() {
		bool complete=false;
		wl_callback *callback=wl_display_sync(display);
		static const wl_callback_listener listener{[](void *data,wl_callback *callback,uint32_t) {
			*static_cast<bool *>(data)=true; wl_callback_destroy(callback);
		}};
		wl_callback_add_listener(callback,&listener,&complete);
		const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
		try {
			while (!complete && std::chrono::steady_clock::now()<end) events(std::chrono::milliseconds(20));
		} catch (...) { if (!complete) wl_callback_destroy(callback); throw; }
		if (!complete) { wl_callback_destroy(callback); throw std::runtime_error("Wayland synchronization timed out"); }
	}

	void connect() {
		if (display) {
			if (!compositor || !shm || !shell) throw std::runtime_error("Wayland presentation connection is incomplete");
			return;
		}
		display=wl_display_connect(nullptr);
		if (!display) throw std::runtime_error("cannot connect native Wayland presentation");
		registry=wl_display_get_registry(display);
		static const wl_registry_listener listener{global,[](void *data,wl_registry *,uint32_t name) { static_cast<State *>(data)->outputs.erase(name); }};
		wl_registry_add_listener(registry,&listener,this);
		sync();
		// Registry callbacks bind the outputs; their mode and logical-size events follow those requests.
		sync();
		if (!compositor || !shm || !shell) throw std::runtime_error("Wayland compositor lacks required presentation protocols");
	}

	// The rectangle of a frame that differs from the one shown, empty when they match.
	// A row's 64-bit hash in four independent lanes, so that a frame is read at the speed of memory.
	static uint64_t row_hash(const uint8_t *row,size_t length) {
		constexpr uint64_t factor=0x9e3779b97f4a7c15ULL;
		uint64_t lanes[4]={1,2,3,4};
		size_t at=0;
		for (;at+32<=length;at+=32)
			for (int lane=0;lane<4;++lane) {
				uint64_t word=0;
				std::memcpy(&word,row+at+size_t(lane)*8,8);
				lanes[lane]=(lanes[lane]^word)*factor;
			}
		for (;at<length;++at) lanes[0]=(lanes[0]^row[at])*factor;
		return lanes[0]^(lanes[1]<<1)^(lanes[2]<<2)^(lanes[3]<<3)^length;
	}

	// The rectangle of a frame that differs from the one shown, empty when they match. The rows whose hash changed since the shown
	// frame are the only ones compared, so the shown frame is read only where something changed; the hashes then describe the new frame.
	static std::array<int32_t,4> changed_area(Surface &window,const Buffer &shown,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
		const size_t stride=size_t(width)*4;
		const auto *old=static_cast<const uint8_t *>(shown.pixels);
		const bool known=window.row_hashes.size()==height;
		window.row_hashes.resize(height);
		uint32_t top=height,bottom=0,left=width,right=0;
		for (uint32_t y=0;y<height;++y) {
			const auto *row=pixels.data()+y*stride;
			const auto hash=row_hash(row,stride);
			const bool same=known && window.row_hashes[y]==hash;
			window.row_hashes[y]=hash;
			if (same) continue;
			const auto *previous=old+y*stride;
			if (!std::memcmp(previous,row,stride)) continue;
			top=std::min(top,y); bottom=y+1;
			uint32_t x=0;
			while (x<left && !std::memcmp(previous+x*4,row+x*4,4)) ++x;
			left=std::min(left,x);
			x=width;
			while (x>right && !std::memcmp(previous+(x-1)*4,row+(x-1)*4,4)) --x;
			right=std::max(right,x);
		}
		if (top>=bottom) return {0,0,0,0};
		return {int32_t(left),int32_t(top),int32_t(right-left),int32_t(bottom-top)};
	}

	void attach_frame(Surface &window,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
		events(std::chrono::milliseconds(0));
		// Released buffers of this size are reused; the compositor only repaints what changed since the shown frame.
		std::array<int32_t,4> damage{0,0,int32_t(width),int32_t(height)};
		const bool same_size=window.shown && window.shown->width==width && window.shown->height==height;
		// Hashes describe the shown frame only after a comparison made them; a frame presented whole leaves none.
		if (!same_size) window.row_hashes.clear();
		if (same_size) {
			damage=changed_area(window,*window.shown,width,height,pixels);
			if (!damage[2]) {
				wl_surface_commit(window.proxy);
				return;
			}
		}
		std::erase_if(window.buffers,[&](const auto &buffer) {
			return buffer->released && buffer.get()!=window.shown && (buffer->width!=width || buffer->height!=height);
		});
		Buffer *target=nullptr;
		size_t pending=0;
		for (const auto &buffer : window.buffers) {
			if (buffer->released && buffer.get()!=window.shown && !target) target=buffer.get();
			else if (!buffer->released) ++pending;
		}
		if (!target) {
			if (pending>=3) throw std::runtime_error("Wayland presentation buffer limit reached");
			size_t used=0;
			for (const auto &[key,surface] : surfaces)
				for (const auto &buffer : surface->buffers) used+=buffer->size;
			if (used+pixels.size()>256*1024*1024) throw std::runtime_error("Wayland presentation memory limit reached");
			auto buffer=std::make_unique<Buffer>(); buffer->size=pixels.size(); buffer->width=width; buffer->height=height;
			buffer->stale={0,0,int32_t(width),int32_t(height)};
			const int descriptor=int(syscall(SYS_memfd_create,"raven-frame",MFD_CLOEXEC));
			if (descriptor<0) throw std::runtime_error("Wayland frame descriptor failed");
			if (ftruncate(descriptor,off_t(buffer->size))<0) { close(descriptor); throw std::runtime_error("Wayland frame sizing failed"); }
			buffer->pixels=mmap(nullptr,buffer->size,PROT_READ|PROT_WRITE,MAP_SHARED,descriptor,0);
			if (buffer->pixels==MAP_FAILED) { close(descriptor); throw std::runtime_error("Wayland frame mapping failed"); }
			wl_shm_pool *pool=wl_shm_create_pool(shm,descriptor,int32_t(buffer->size)); close(descriptor);
			buffer->proxy=wl_shm_pool_create_buffer(pool,0,int32_t(width),int32_t(height),int32_t(width*4),WL_SHM_FORMAT_XRGB8888);
			wl_shm_pool_destroy(pool);
			static const wl_buffer_listener buffer_listener{[](void *data,wl_buffer *) { static_cast<Buffer *>(data)->released=true; }};
			wl_buffer_add_listener(buffer->proxy,&buffer_listener,buffer.get());
			target=buffer.get();
			window.buffers.push_back(std::move(buffer));
		}
		for (const auto &buffer : window.buffers) {
			if (buffer->width!=width || buffer->height!=height) continue;
			auto &stale=buffer->stale;
			const std::array<int32_t,4> changed{damage[0],damage[1],damage[0]+damage[2],damage[1]+damage[3]};
			stale=stale[2]<=stale[0] ? changed : std::array<int32_t,4>{std::min(stale[0],changed[0]),std::min(stale[1],changed[1]),
				std::max(stale[2],changed[2]),std::max(stale[3],changed[3])};
		}
		{
			const auto &stale=target->stale;
			const size_t stride=size_t(width)*4,from=size_t(stale[0])*4,length=size_t(stale[2]-stale[0])*4;
			if (stale[0]==0 && stale[2]==int32_t(width))
				std::memcpy(static_cast<uint8_t *>(target->pixels)+size_t(stale[1])*stride,pixels.data()+size_t(stale[1])*stride,size_t(stale[3]-stale[1])*stride);
			else
				for (int32_t y=stale[1];y<stale[3];++y)
					std::memcpy(static_cast<uint8_t *>(target->pixels)+size_t(y)*stride+from,pixels.data()+size_t(y)*stride+from,length);
			target->stale={0,0,0,0};
		}
		target->released=false;
		wl_surface_attach(window.proxy,target->proxy,0,0);
		wl_surface_damage(window.proxy,damage[0],damage[1],damage[2],damage[3]);
		wl_surface_commit(window.proxy); window.shown=target; sync();
		if (window.closed) throw std::runtime_error("Wayland surface was closed during presentation");
	}

	void popup_surface(uint64_t handle,uint64_t parent,int32_t x,int32_t y,uint32_t width,uint32_t height,std::span<const uint8_t> pixels,
		bool grab=true) {
		if (!handle || !width || !height || width>4096 || height>4096 || pixels.size()!=size_t(width)*height*4)
			throw std::runtime_error("invalid native Wayland popup frame");
		connect();
		auto found=surfaces.find(handle);
		// A popup that moved or changed size is made again where it now is; one that was a window before becomes a popup.
		if (found!=surfaces.end() && !grab && (!found->second->is_popup || found->second->popup_x!=x || found->second->popup_y!=y ||
			found->second->width!=width || found->second->height!=height || found->second->closed)) {
			surfaces.erase(found);
			found=surfaces.end();
		}
		if (found==surfaces.end()) {
			const auto owner=surfaces.find(parent);
			if (owner==surfaces.end()) throw std::runtime_error("Wayland popup has no presented parent surface");
			auto window=std::make_unique<Surface>();
			window->width=width; window->height=height; window->is_popup=true;
			window->popup_x=x; window->popup_y=y; window->grabbed=grab;
			window->proxy=wl_compositor_create_surface(compositor);
			window->shell=xdg_wm_base_get_xdg_surface(shell,window->proxy);
			xdg_positioner *positioner=xdg_wm_base_create_positioner(shell);
			xdg_positioner_set_size(positioner,int32_t(width),int32_t(height));
			xdg_positioner_set_anchor_rect(positioner,x,y,1,1);
			xdg_positioner_set_anchor(positioner,XDG_POSITIONER_ANCHOR_TOP_LEFT);
			xdg_positioner_set_gravity(positioner,XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
			window->popup=xdg_surface_get_popup(window->shell,owner->second->shell,positioner);
			xdg_positioner_destroy(positioner);
			static const xdg_surface_listener surface_listener{[](void *data,xdg_surface *surface,uint32_t serial) {
				xdg_surface_ack_configure(surface,serial); static_cast<Surface *>(data)->configured=true;
			}};
			static const xdg_popup_listener popup_listener{
				[](void *,xdg_popup *,int32_t,int32_t,int32_t,int32_t) {},
				[](void *data,xdg_popup *) { static_cast<Surface *>(data)->closed=true; },
				nullptr};
			xdg_surface_add_listener(window->shell,&surface_listener,window.get());
			xdg_popup_add_listener(window->popup,&popup_listener,window.get());
			if (seat && grab) xdg_popup_grab(window->popup,seat,last_serial);
			wl_surface_commit(window->proxy);
			const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
			while (!window->configured && !window->closed && std::chrono::steady_clock::now()<end) events(std::chrono::milliseconds(20));
			if (!window->configured || window->closed) throw std::runtime_error("Wayland popup configuration failed");
			found=surfaces.emplace(handle,std::move(window)).first;
		}
		auto &window=*found->second;
		if (!window.is_popup || window.closed || window.width!=width || window.height!=height)
			throw std::runtime_error("Wayland popup cannot accept this frame");
		attach_frame(window,width,height,pixels);
	}

	std::optional<HostSurface> host_surface(uint64_t handle,int32_t x,int32_t y) {
		const auto found=surfaces.find(handle);
		if (found==surfaces.end() || found->second->is_popup || !subcompositor) return std::nullopt;
		auto &window=*found->second;
		if (!window.child) {
			window.child=wl_compositor_create_surface(compositor);
			window.child_role=wl_subcompositor_get_subsurface(subcompositor,window.child,window.proxy);
			wl_subsurface_set_desync(window.child_role);
			// Input stays with the window: the driver's surface takes none of it.
			wl_region *none=wl_compositor_create_region(compositor);
			wl_surface_set_input_region(window.child,none);
			wl_region_destroy(none);
		}
		wl_subsurface_set_position(window.child_role,x,y);
		wl_surface_commit(window.proxy);
		wl_display_flush(display);
		return HostSurface{display,window.child};
	}

	void maximize(uint64_t handle,bool wanted) {
		if (wanted) maximized.insert(handle); else maximized.erase(handle);
		const auto found=surfaces.find(handle);
		if (found==surfaces.end() || !found->second->role || found->second->maximized==wanted) return;
		auto &window=*found->second;
		window.maximized=wanted;
		window.configured=false;
		if (wanted) xdg_toplevel_set_maximized(window.role);
		else xdg_toplevel_unset_maximized(window.role);
		const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
		while (!window.configured && !window.closed && std::chrono::steady_clock::now()<end) events(std::chrono::milliseconds(20));
		if (!window.configured || window.closed) throw std::runtime_error("Wayland maximization configuration failed");
	}

	void present(uint64_t handle,std::string_view title,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
		if (!handle || !width || !height || width>8192 || height>8192 || uint64_t(width)*height>16*1024*1024 || pixels.size()!=size_t(width)*height*4)
			throw std::runtime_error("invalid native Wayland frame");
		connect();
		auto found=surfaces.find(handle);
		// A window shown as a popup until now becomes a window of its own.
		if (found!=surfaces.end() && found->second->is_popup) {
			surfaces.erase(found);
			found=surfaces.end();
		}
		if (found==surfaces.end()) {
			auto window=std::make_unique<Surface>();
			window->width=width; window->height=height;
			window->maximized=maximized.contains(handle);
			window->proxy=wl_compositor_create_surface(compositor);
			window->shell=xdg_wm_base_get_xdg_surface(shell,window->proxy);
			window->role=xdg_surface_get_toplevel(window->shell);
			static const xdg_surface_listener surface_listener{[](void *data,xdg_surface *surface,uint32_t serial) {
				xdg_surface_ack_configure(surface,serial); static_cast<Surface *>(data)->configured=true;
			}};
			static const xdg_toplevel_listener role_listener{
				[](void *data,xdg_toplevel *,int32_t width,int32_t height,wl_array *) {
					auto &window=*static_cast<Surface *>(data);
					if (width>0 && height>0 && (uint32_t(width)!=window.width || uint32_t(height)!=window.height)) {
						window.pending_width=uint32_t(width); window.pending_height=uint32_t(height); window.resized=true;
					}
				}, [](void *data,xdg_toplevel *) { static_cast<Surface *>(data)->closed=true; },nullptr,nullptr};
			xdg_surface_add_listener(window->shell,&surface_listener,window.get());
			xdg_toplevel_add_listener(window->role,&role_listener,window.get());
			xdg_toplevel_set_title(window->role,std::string(title).c_str());
			xdg_toplevel_set_app_id(window->role,"raven.native");
			// A floating window starts at its requested size; a maximized one follows the compositor's work area.
			if (window->maximized) xdg_toplevel_set_maximized(window->role);
			else {
				xdg_toplevel_set_min_size(window->role,int32_t(width),int32_t(height));
				xdg_toplevel_set_max_size(window->role,int32_t(width),int32_t(height));
			}
			wl_surface_commit(window->proxy);
			const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
			while (!window->configured && !window->closed && !window->resized && std::chrono::steady_clock::now()<end) events(std::chrono::milliseconds(20));
			if (!window->maximized) window->resized=false;
			window->first_frame=!window->maximized;
			if (!window->configured || window->closed) throw std::runtime_error("Wayland window configuration failed");
			found=surfaces.emplace(handle,std::move(window)).first;
		}
		auto &window=*found->second;
		xdg_toplevel_set_title(window.role,std::string(title).c_str());
		// A size the compositor refused to configure (a window larger than the output) is shown clipped rather than failing the frame.
		// A frame of another size is the guest's answer to a resize: the surface follows it.
		// The compositor's close is a request: once the program has it (WM_CLOSE), a window it keeps, asking to save its work for
		// instance, goes on showing its frames.
		if (window.closed && window.close_posted) {
			window.closed=false;
			window.close_posted=false;
		}
		if (window.closed) throw std::runtime_error("Wayland surface cannot accept this frame");
		if (window.maximized && window.pending_width && window.pending_height &&
			(window.pending_width!=width || window.pending_height!=height)) return;
		if (window.pending_width==width && window.pending_height==height) window.resized=false;
		window.width=width; window.height=height;
		xdg_surface_set_window_geometry(window.shell,0,0,int32_t(width),int32_t(height));
		attach_frame(window,width,height,pixels);
		if (window.first_frame) {
			window.first_frame=false;
			xdg_toplevel_set_min_size(window.role,0,0);
			xdg_toplevel_set_max_size(window.role,0,0);
			wl_surface_commit(window.proxy);
		}
	}
};

WaylandPresentation::WaylandPresentation() : state(std::make_unique<State>()) {}
WaylandPresentation::~WaylandPresentation()=default;

DesktopPresentation WaylandPresentation::callbacks()
{
	return {[this](uint64_t handle,std::string_view title,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
		state->present(handle,title,width,height,pixels);
	},[this](uint64_t handle) {
		state->maximized.erase(handle);
		if (state->pointer_window==handle) state->pointer_window=0;
		if (state->keyboard_window==handle) state->keyboard_window=0;
		std::erase_if(state->pending,[&](const DesktopEvent &event) { return event.window==handle; });
		if (state->constrained_window==handle) state->release_constraint();
		if (state->surfaces.erase(handle) && state->display) state->sync();
	},
	[this](std::chrono::milliseconds budget) {
		std::vector<DesktopEvent> events;
		if (!state->display) { if (budget.count()>0) ::poll(nullptr,0,int(budget.count())); return events; }
		state->events(budget);
		events.swap(state->pending);
		for (auto &[handle,surface] : state->surfaces) {
			if (surface->resized && !surface->is_popup) {
				events.push_back({handle,0,0,0,DesktopEventKind::resize,int32_t(surface->pending_width),int32_t(surface->pending_height)});
				surface->resized=false;
			}
			if (surface->closed && !surface->close_posted) {
				if (surface->is_popup) events.push_back({handle,0,0,0,DesktopEventKind::popup_done});
				else events.push_back({handle,0x10});
				surface->close_posted=true;
			}
		}
		return events;
	},[this](uint64_t handle,uint64_t parent,int32_t x,int32_t y,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
		state->popup_surface(handle,parent,x,y,width,height,pixels);
	},[this](uint64_t handle,uint32_t action) {
		const auto found=state->surfaces.find(handle);
		if (found==state->surfaces.end() || !state->seat || found->second->is_popup || !found->second->role) return;
		if (action==0 && !state->buttons_down) state->pending_move=handle;
		else if (action==0) xdg_toplevel_move(found->second->role,state->seat,state->last_serial);
		else xdg_toplevel_resize(found->second->role,state->seat,state->last_serial,action);
		wl_display_flush(state->display);
	},[this](std::string_view text) { state->publish_text(text); },
	[this]() { return state->fetch_text(); },
	[this](uint64_t handle,int32_t x,int32_t y,uint32_t,uint32_t) { return state->host_surface(handle,x,y); },
	[this](uint64_t handle) {
		const auto found=state->surfaces.find(handle);
		if (found==state->surfaces.end()) return;
		found->second->drop_child();
		if (state->display) wl_display_flush(state->display);
	},
	[this](uint64_t handle,std::optional<std::array<int32_t,4>> rectangle) { state->confine(handle,rectangle); },
	[this](uint64_t handle,uint64_t owner,int32_t x,int32_t y,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
		state->popup_surface(handle,owner,x,y,width,height,pixels,false);
	},
	[this] { return state->display ? wl_display_get_fd(state->display) : -1; },
	[this]() -> void * { state->connect(); return state->display; },
	[this] { state->connect(); return state->screen_size(); },
	[this](uint64_t handle,bool maximized) { state->maximize(handle,maximized); },
	[this](uint32_t shape) {
		if (state->cursor==shape) return;
		state->cursor=shape;
		state->show_cursor();
	}};
}

}
