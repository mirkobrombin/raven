#include "xcb.h"

#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include <xcb/xinput.h>
// The keyboard extension names a structure member after a C++ keyword.
#define explicit explicit_components
#include <xcb/xkb.h>
#undef explicit
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/eventfd.h>
#include <unistd.h>
#include <vector>

namespace raven {

namespace {

template <typename T> struct Reply {
	T *value=nullptr;
	explicit Reply(T *reply) : value(reply) {}
	~Reply() { std::free(value); }
	Reply(const Reply &)=delete;
	Reply &operator=(const Reply &)=delete;
	T *operator->() const { return value; }
	explicit operator bool() const { return value; }
};

constexpr const char *atom_names[]={"WM_PROTOCOLS","WM_DELETE_WINDOW","WM_NAME","WM_CLASS","_NET_WM_NAME","UTF8_STRING","STRING","TEXT",
	"CLIPBOARD","TARGETS","text/plain;charset=utf-8","text/plain","RAVEN_SELECTION","INCR","_NET_WM_STATE","_NET_WM_STATE_MAXIMIZED_VERT",
	"_NET_WM_STATE_MAXIMIZED_HORZ","_NET_WM_MOVERESIZE","_MOTIF_WM_HINTS","_NET_WM_WINDOW_TYPE","_NET_WM_WINDOW_TYPE_POPUP_MENU","ATOM"};
enum Atom { wm_protocols, wm_delete_window, wm_name, wm_class, net_wm_name, utf8_string, string_type, text_type, clipboard, targets,
	text_utf8, text_plain, raven_selection, incr, net_wm_state, maximized_vert, maximized_horz, net_wm_moveresize, motif_hints,
	window_type, window_type_popup, atom_type, atom_count };

}

struct XcbPresentation::State {
	struct Window {
		xcb_window_t id=0;
		bool is_popup=false, grabbed=false;
		// A popup's place on its parent, and its rectangle on the screen for telling a click outside it.
		int32_t popup_x=0,popup_y=0;
		std::array<int32_t,4> screen{0,0,0,0};
		uint64_t parent=0;
		bool closed=false, close_posted=false, resized=false, maximized=false, mapped=false;
		uint32_t width=0,height=0,pending_width=0,pending_height=0;
		std::string title;
		// The frame the window shows, kept to repaint what the server exposes and to send only what changed.
		std::vector<uint8_t> shown;
		uint32_t shown_width=0,shown_height=0;
		std::array<int32_t,4> exposed{0,0,0,0};
	};
	// A window a graphics driver renders to for one window of the program: a child of the host window of its root, over the client area,
	// mapped while it shows, and kept unmapped under the screen's root otherwise. It selects no input, so input reaches the host window.
	struct Hosted {
		xcb_window_t id=0,parent=0;
		std::array<uint32_t,4> place{};
		bool shown=false;
	};
	std::map<uint64_t,Hosted> hosted;
	xcb_connection_t *connection=nullptr;
	xcb_screen_t *screen=nullptr;
	xcb_gcontext_t gc=0;
	// Owns the clipboard selection and receives transfers.
	xcb_window_t utility=0;
	std::array<xcb_atom_t,atom_count> atoms{};
	size_t request_bytes=0;
	std::vector<uint8_t> band;
	uint8_t xkb_event=0,xfixes_event=0,xinput_opcode=0;
	int32_t keyboard_device=-1;
	xkb_context *keyboard_context=nullptr;
	std::map<uint64_t,std::unique_ptr<Window>> windows;
	std::set<uint64_t> maximized;
	std::vector<DesktopEvent> pending;
	uint64_t pointer_window=0,keyboard_window=0;
	int32_t pointer_x=0,pointer_y=0,root_x=0,root_y=0;
	xcb_timestamp_t last_time=XCB_CURRENT_TIME;
	bool input_failed=false;
	const char *input_error="unsupported native input event or input queue exhaustion";
	// Raw motion in 1/256 units; the fractions add up across reports.
	int64_t relative_x=0,relative_y=0;
	// An input-only window over the rectangle the pointer is kept in while it is held to a window.
	xcb_window_t confinement=0;
	uint64_t constrained_window=0;
	std::string source_text;
	bool owns_selection=false, selection_present=false, selection_arrived=false;
	xcb_atom_t selection_property=XCB_NONE;
	uint64_t selection_counter=0;
	// Readable for as long as events wait in the queue, for a caller that sleeps on the descriptor while the socket has nothing to read.
	int ready=-1;
	std::array<bool,256> keys_down{};
	// The cursor the program shows, by its IDC_ identifier or 0 for none, and the server cursors made for each so far.
	uint32_t cursor=32512;
	std::map<uint32_t,xcb_cursor_t> cursors;
	xcb_font_t cursor_font=0;

	~State() {
		for (auto &[handle,window] : windows) destroy(*window);
		windows.clear();
		if (keyboard_context) xkb_context_unref(keyboard_context);
		if (ready>=0) close(ready);
		if (connection) {
			for (const auto &[id,made] : cursors) xcb_free_cursor(connection,made);
			if (cursor_font) xcb_close_font(connection,cursor_font);
			if (confinement) xcb_destroy_window(connection,confinement);
			if (utility) xcb_destroy_window(connection,utility);
			if (gc) xcb_free_gc(connection,gc);
			xcb_flush(connection);
			xcb_disconnect(connection);
		}
	}

	static bool tracing() {
		static const bool enabled=std::getenv("RAVEN_TRACE_X11")!=nullptr;
		return enabled;
	}

	void destroy(Window &window) {
		if (!connection) return;
		if (tracing()) std::fprintf(stderr,"x11: destroy window %x\n",window.id);
		// The driver's windows outlive the host window, which would take its children along.
		for (auto &[handle,entry] : hosted)
			if (window.id && entry.parent==window.id) {
				xcb_unmap_window(connection,entry.id);
				xcb_reparent_window(connection,entry.id,screen->root,0,0);
				entry.parent=screen->root; entry.shown=false;
			}
		if (window.id) xcb_destroy_window(connection,window.id);
		window.id=0;
	}

	uint64_t handle_of(xcb_window_t id) const {
		for (const auto &[handle,window] : windows) if (window->id==id) return handle;
		return 0;
	}

	Window *window_of(xcb_window_t id) {
		for (const auto &[handle,window] : windows) if (window->id==id) return window.get();
		return nullptr;
	}

	void queue(DesktopEvent event) {
		if (pending.size()>=10000) { input_failed=true; return; }
		try { pending.push_back(std::move(event)); } catch (...) { input_failed=true; }
	}

	void pointer_event(DesktopEventKind kind,uint32_t button=0,uint32_t pressed=0) {
		if (!pointer_window && kind==DesktopEventKind::pointer_motion) return;
		queue({pointer_window,button,pressed,0,kind,pointer_x,pointer_y});
	}

	void property(xcb_window_t window,xcb_atom_t name,xcb_atom_t type,uint8_t format,uint32_t count,const void *data) {
		xcb_change_property(connection,XCB_PROP_MODE_REPLACE,window,name,type,format,count,data);
	}

	void send_keymap() {
		if (keyboard_device<0 || !keyboard_context) return;
		xkb_keymap *keymap=xkb_x11_keymap_new_from_device(keyboard_context,connection,keyboard_device,XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (!keymap) { input_error="cannot read the X11 keyboard map"; input_failed=true; return; }
		char *text=xkb_keymap_get_as_string(keymap,XKB_KEYMAP_FORMAT_TEXT_V1);
		xkb_state *state=xkb_x11_state_new_from_device(keymap,connection,keyboard_device);
		if (text && std::strlen(text)<=1024*1024) {
			DesktopEvent event{0,0}; event.kind=DesktopEventKind::keymap;
			try { event.keymap=text; queue(std::move(event)); } catch (...) { input_failed=true; }
			if (state) queue({0,xkb_state_serialize_mods(state,XKB_STATE_MODS_DEPRESSED),xkb_state_serialize_mods(state,XKB_STATE_MODS_LATCHED),
				uint64_t(xkb_state_serialize_mods(state,XKB_STATE_MODS_LOCKED))|
				(uint64_t(xkb_state_serialize_layout(state,XKB_STATE_LAYOUT_LOCKED))<<32),DesktopEventKind::modifiers});
		} else input_failed=true;
		if (state) xkb_state_unref(state);
		std::free(text);
		xkb_keymap_unref(keymap);
	}

	void watch_keyboard() {
		uint8_t base=0;
		if (!xkb_x11_setup_xkb_extension(connection,XKB_X11_MIN_MAJOR_XKB_VERSION,XKB_X11_MIN_MINOR_XKB_VERSION,
			XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,nullptr,nullptr,&base,nullptr))
			throw std::runtime_error("X11 server lacks the keyboard extension");
		xkb_event=base;
		keyboard_device=xkb_x11_get_core_keyboard_device_id(connection);
		keyboard_context=xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		if (keyboard_device<0 || !keyboard_context) throw std::runtime_error("cannot open the X11 keyboard");
		constexpr uint16_t events=XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY|XCB_XKB_EVENT_TYPE_MAP_NOTIFY|XCB_XKB_EVENT_TYPE_STATE_NOTIFY;
		constexpr uint16_t parts=XCB_XKB_MAP_PART_KEY_TYPES|XCB_XKB_MAP_PART_KEY_SYMS|XCB_XKB_MAP_PART_MODIFIER_MAP|
			XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS|XCB_XKB_MAP_PART_KEY_ACTIONS|XCB_XKB_MAP_PART_VIRTUAL_MODS|XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP;
		constexpr uint16_t keyboards=XCB_XKB_NKN_DETAIL_KEYCODES;
		constexpr uint16_t state=XCB_XKB_STATE_PART_MODIFIER_BASE|XCB_XKB_STATE_PART_MODIFIER_LATCH|XCB_XKB_STATE_PART_MODIFIER_LOCK|
			XCB_XKB_STATE_PART_GROUP_BASE|XCB_XKB_STATE_PART_GROUP_LATCH|XCB_XKB_STATE_PART_GROUP_LOCK;
		xcb_xkb_select_events_details_t details{};
		details.affectNewKeyboard=keyboards; details.newKeyboardDetails=keyboards;
		details.affectState=state; details.stateDetails=state;
		xcb_xkb_select_events_aux(connection,uint16_t(keyboard_device),events,0,0,parts,parts,&details);
		// Windows repeats a held key with further presses and no releases between them.
		xcb_discard_reply(connection,xcb_xkb_per_client_flags(connection,XCB_XKB_ID_USE_CORE_KBD,XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT,
			XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT,0,0,0).sequence);
		send_keymap();
	}

	void watch_selection() {
		const auto *extension=xcb_get_extension_data(connection,&xcb_xfixes_id);
		if (!extension || !extension->present) return;
		Reply version(xcb_xfixes_query_version_reply(connection,xcb_xfixes_query_version(connection,5,0),nullptr));
		if (!version) return;
		xfixes_event=extension->first_event;
		xcb_xfixes_select_selection_input(connection,utility,atoms[clipboard],XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER|
			XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY|XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
		Reply owner(xcb_get_selection_owner_reply(connection,xcb_get_selection_owner(connection,atoms[clipboard]),nullptr));
		if (owner && owner->owner) { selection_present=true; selection_counter=1; }
	}

	void watch_relative_motion() {
		const auto *extension=xcb_get_extension_data(connection,&xcb_input_id);
		if (!extension || !extension->present) return;
		Reply version(xcb_input_xi_query_version_reply(connection,xcb_input_xi_query_version(connection,2,0),nullptr));
		if (!version || version->major_version<2) return;
		xinput_opcode=extension->major_opcode;
		struct { xcb_input_event_mask_t head; uint32_t mask; } selection{{XCB_INPUT_DEVICE_ALL_MASTER,1},XCB_INPUT_XI_EVENT_MASK_RAW_MOTION};
		xcb_input_xi_select_events(connection,screen->root,1,&selection.head);
	}

	void connect() {
		if (connection) {
			if (xcb_connection_has_error(connection)) throw std::runtime_error("X11 connection failed");
			return;
		}
		if (!std::getenv("DISPLAY")) throw std::runtime_error("X11 presentation needs DISPLAY");
		int number=0;
		connection=xcb_connect(nullptr,&number);
		if (xcb_connection_has_error(connection)) throw std::runtime_error("cannot connect native X11 presentation");
		const auto *setup=xcb_get_setup(connection);
		auto roots=xcb_setup_roots_iterator(setup);
		for (int index=0;index<number && roots.rem;++index) xcb_screen_next(&roots);
		if (!roots.rem) throw std::runtime_error("X11 display has no such screen");
		screen=roots.data;
		// Frames are 32-bit pixels, blue first; the root visual must take them as they are.
		bool pixels=false;
		for (auto format=xcb_setup_pixmap_formats_iterator(setup);format.rem;xcb_format_next(&format))
			if (format.data->depth==screen->root_depth) pixels=format.data->bits_per_pixel==32;
		const xcb_visualtype_t *visual=nullptr;
		for (auto depth=xcb_screen_allowed_depths_iterator(screen);depth.rem && !visual;xcb_depth_next(&depth))
			for (auto type=xcb_depth_visuals_iterator(depth.data);type.rem;xcb_visualtype_next(&type))
				if (type.data->visual_id==screen->root_visual) { visual=type.data; break; }
		if (!pixels || setup->image_byte_order!=XCB_IMAGE_ORDER_LSB_FIRST || (screen->root_depth!=24 && screen->root_depth!=32) || !visual ||
			visual->_class!=XCB_VISUAL_CLASS_TRUE_COLOR || visual->red_mask!=0xff0000 || visual->green_mask!=0xff00 || visual->blue_mask!=0xff)
			throw std::runtime_error("X11 display needs a 24-bit TrueColor visual with 32-bit pixels");
		request_bytes=std::min<size_t>(size_t(xcb_get_maximum_request_length(connection))*4,4U<<20);
		if (request_bytes<64*1024) throw std::runtime_error("X11 server request limit is too small");
		std::array<xcb_intern_atom_cookie_t,atom_count> cookies;
		for (size_t index=0;index<atom_count;++index)
			cookies[index]=xcb_intern_atom(connection,0,uint16_t(std::strlen(atom_names[index])),atom_names[index]);
		for (size_t index=0;index<atom_count;++index) {
			Reply atom(xcb_intern_atom_reply(connection,cookies[index],nullptr));
			if (!atom) throw std::runtime_error("cannot name X11 atoms");
			atoms[index]=atom->atom;
		}
		gc=xcb_generate_id(connection);
		const uint32_t no_exposures=0;
		xcb_create_gc(connection,gc,screen->root,XCB_GC_GRAPHICS_EXPOSURES,&no_exposures);
		utility=xcb_generate_id(connection);
		const uint32_t changes=XCB_EVENT_MASK_PROPERTY_CHANGE;
		xcb_create_window(connection,XCB_COPY_FROM_PARENT,utility,screen->root,-1,-1,1,1,0,XCB_WINDOW_CLASS_INPUT_ONLY,XCB_COPY_FROM_PARENT,
			XCB_CW_EVENT_MASK,&changes);
		ready=eventfd(1,EFD_CLOEXEC|EFD_NONBLOCK);
		if (ready<0) throw std::runtime_error("cannot make the X11 event signal");
		watch_keyboard();
		watch_selection();
		watch_relative_motion();
		xcb_flush(connection);
		if (xcb_connection_has_error(connection)) throw std::runtime_error("X11 connection failed");
	}

	void expose(Window &window,int32_t left,int32_t top,int32_t right,int32_t bottom) {
		auto &area=window.exposed;
		area=area[2]<=area[0] ? std::array<int32_t,4>{left,top,right,bottom} :
			std::array<int32_t,4>{std::min(area[0],left),std::min(area[1],top),std::max(area[2],right),std::max(area[3],bottom)};
	}

	// Sends the rectangle (left, top, right, bottom) of the shown frame in bands that fit a request.
	void upload(Window &window,int32_t left,int32_t top,int32_t right,int32_t bottom) {
		right=std::min(right,int32_t(window.shown_width)); bottom=std::min(bottom,int32_t(window.shown_height));
		left=std::max(left,0); top=std::max(top,0);
		if (right<=left || bottom<=top || !window.id) return;
		const size_t stride=size_t(window.shown_width)*4,span=size_t(right-left)*4;
		const auto rows=int32_t(std::max<size_t>(1,(request_bytes-64)/span));
		for (int32_t y=top;y<bottom;y+=rows) {
			const auto count=std::min(rows,bottom-y);
			const uint8_t *data=window.shown.data()+size_t(y)*stride+size_t(left)*4;
			if (span!=stride) {
				band.resize(span*size_t(count));
				for (int32_t row=0;row<count;++row) std::memcpy(band.data()+size_t(row)*span,data+size_t(row)*stride,span);
				data=band.data();
			}
			xcb_put_image(connection,XCB_IMAGE_FORMAT_Z_PIXMAP,window.id,gc,uint16_t(right-left),uint16_t(count),int16_t(left),int16_t(y),0,
				screen->root_depth,uint32_t(span*size_t(count)),data);
		}
	}

	void attach_frame(Window &window,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
		const size_t stride=size_t(width)*4;
		int32_t top=0,bottom=int32_t(height),left=0,right=int32_t(width);
		if (window.shown_width==width && window.shown_height==height) {
			top=int32_t(height); bottom=0; left=int32_t(width); right=0;
			for (uint32_t y=0;y<height;++y) {
				const auto *row=pixels.data()+y*stride,*previous=window.shown.data()+y*stride;
				if (!std::memcmp(previous,row,stride)) continue;
				top=std::min(top,int32_t(y)); bottom=int32_t(y)+1;
				int32_t x=0;
				while (x<left && !std::memcmp(previous+x*4,row+x*4,4)) ++x;
				left=std::min(left,x);
				x=int32_t(width);
				while (x>right && !std::memcmp(previous+(x-1)*4,row+(x-1)*4,4)) --x;
				right=std::max(right,x);
			}
			if (top>=bottom) return;
			for (int32_t y=top;y<bottom;++y) std::memcpy(window.shown.data()+size_t(y)*stride,pixels.data()+size_t(y)*stride,stride);
		} else {
			size_t used=0;
			for (const auto &[handle,other] : windows) if (other.get()!=&window) used+=other->shown.size();
			if (used+pixels.size()>256*1024*1024) throw std::runtime_error("X11 presentation memory limit reached");
			window.shown.assign(pixels.begin(),pixels.end());
			window.shown_width=width; window.shown_height=height;
		}
		upload(window,left,top,right,bottom);
	}

	// A glyph of the standard cursor font for each system cursor that has one; none of them is an empty pixmap.
	xcb_cursor_t server_cursor(uint32_t id) {
		if (const auto found=cursors.find(id); found!=cursors.end()) return found->second;
		const xcb_cursor_t made=xcb_generate_id(connection);
		if (!id) {
			const xcb_pixmap_t empty=xcb_generate_id(connection);
			xcb_create_pixmap(connection,1,empty,screen->root,1,1);
			xcb_create_cursor(connection,made,empty,empty,0,0,0,0,0,0,0,0);
			xcb_free_pixmap(connection,empty);
		} else {
			uint16_t glyph=68;
			switch (id) {
			case 32513: glyph=152; break;
			case 32514: case 32650: glyph=150; break;
			case 32515: glyph=34; break;
			case 32642: glyph=14; break;
			case 32643: glyph=12; break;
			case 32644: glyph=108; break;
			case 32645: glyph=116; break;
			case 32640: case 32646: glyph=52; break;
			case 32648: glyph=24; break;
			case 32649: glyph=60; break;
			case 32651: glyph=92; break;
			default: break;
			}
			if (!cursor_font) {
				cursor_font=xcb_generate_id(connection);
				xcb_open_font(connection,cursor_font,6,"cursor");
			}
			xcb_create_glyph_cursor(connection,made,cursor_font,cursor_font,glyph,uint16_t(glyph+1),0,0,0,0xffff,0xffff,0xffff);
		}
		cursors.emplace(id,made);
		return made;
	}

	void show_cursor() {
		if (!connection) return;
		const uint32_t value=server_cursor(cursor);
		for (const auto &[handle,window] : windows) xcb_change_window_attributes(connection,window->id,XCB_CW_CURSOR,&value);
		xcb_flush(connection);
	}

	xcb_window_t make_window(bool popup,int32_t x,int32_t y,uint32_t width,uint32_t height) {
		const xcb_window_t id=xcb_generate_id(connection);
		uint32_t mask=XCB_EVENT_MASK_EXPOSURE|XCB_EVENT_MASK_BUTTON_PRESS|XCB_EVENT_MASK_BUTTON_RELEASE|XCB_EVENT_MASK_POINTER_MOTION|
			XCB_EVENT_MASK_ENTER_WINDOW|XCB_EVENT_MASK_LEAVE_WINDOW|XCB_EVENT_MASK_KEY_PRESS|XCB_EVENT_MASK_KEY_RELEASE;
		if (!popup) mask|=XCB_EVENT_MASK_STRUCTURE_NOTIFY|XCB_EVENT_MASK_FOCUS_CHANGE;
		// No background: what the server would clear to shows only until the frame arrives.
		const uint32_t values[]={XCB_BACK_PIXMAP_NONE,0,XCB_GRAVITY_NORTH_WEST,popup ? 1U : 0U,mask,server_cursor(cursor)};
		xcb_create_window(connection,XCB_COPY_FROM_PARENT,id,screen->root,int16_t(x),int16_t(y),uint16_t(width),uint16_t(height),0,
			XCB_WINDOW_CLASS_INPUT_OUTPUT,XCB_COPY_FROM_PARENT,
			XCB_CW_BACK_PIXMAP|XCB_CW_BORDER_PIXEL|XCB_CW_BIT_GRAVITY|XCB_CW_OVERRIDE_REDIRECT|XCB_CW_EVENT_MASK|XCB_CW_CURSOR,values);
		static constexpr char name[]="raven.native\0raven.native";
		property(id,atoms[wm_class],atoms[string_type],8,sizeof name,name);
		if (popup) property(id,atoms[window_type],atoms[atom_type],32,1,&atoms[window_type_popup]);
		return id;
	}

	void set_title(Window &window,std::string_view title) {
		if (window.title==title) return;
		window.title.assign(title);
		property(window.id,atoms[net_wm_name],atoms[utf8_string],8,uint32_t(title.size()),title.data());
		property(window.id,atoms[wm_name],atoms[utf8_string],8,uint32_t(title.size()),title.data());
	}

	void ungrab_if_unused() {
		if (std::any_of(windows.begin(),windows.end(),[](const auto &entry) { return entry.second->grabbed && !entry.second->closed; })) return;
		xcb_ungrab_keyboard(connection,XCB_CURRENT_TIME);
		if (!confinement) xcb_ungrab_pointer(connection,XCB_CURRENT_TIME);
	}

	void popup_surface(uint64_t handle,uint64_t parent,int32_t x,int32_t y,uint32_t width,uint32_t height,std::span<const uint8_t> pixels,
		bool grab=true) {
		if (!handle || !width || !height || width>4096 || height>4096 || pixels.size()!=size_t(width)*height*4)
			throw std::runtime_error("invalid native X11 popup frame");
		connect();
		auto found=windows.find(handle);
		if (found!=windows.end() && (!found->second->is_popup || found->second->closed)) {
			destroy(*found->second);
			windows.erase(found);
			found=windows.end();
		}
		const auto owner=windows.find(parent);
		if (owner==windows.end() || !owner->second->id) throw std::runtime_error("X11 popup has no presented parent window");
		Reply place(xcb_translate_coordinates_reply(connection,xcb_translate_coordinates(connection,owner->second->id,screen->root,
			int16_t(x),int16_t(y)),nullptr));
		if (!place) throw std::runtime_error("cannot place X11 popup");
		const std::array<int32_t,4> area{place->dst_x,place->dst_y,place->dst_x+int32_t(width),place->dst_y+int32_t(height)};
		if (found==windows.end()) {
			auto window=std::make_unique<Window>();
			window->is_popup=true; window->parent=parent;
			window->id=make_window(true,area[0],area[1],width,height);
			window->width=width; window->height=height;
			xcb_map_window(connection,window->id);
			found=windows.emplace(handle,std::move(window)).first;
		} else if (found->second->screen!=area || found->second->width!=width || found->second->height!=height) {
			const uint32_t values[]={uint32_t(area[0]),uint32_t(area[1]),width,height};
			xcb_configure_window(connection,found->second->id,XCB_CONFIG_WINDOW_X|XCB_CONFIG_WINDOW_Y|XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT,values);
			found->second->width=width; found->second->height=height;
		}
		auto &window=*found->second;
		window.popup_x=x; window.popup_y=y; window.screen=area;
		if (grab && !window.grabbed) {
			// The pointer and keyboard follow the popup until a click outside it dismisses it, as a compositor's popup grab does.
			Reply pointer(xcb_grab_pointer_reply(connection,xcb_grab_pointer(connection,1,window.id,XCB_EVENT_MASK_BUTTON_PRESS|
				XCB_EVENT_MASK_BUTTON_RELEASE|XCB_EVENT_MASK_POINTER_MOTION,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_NONE,XCB_NONE,
				XCB_CURRENT_TIME),nullptr));
			Reply keyboard(xcb_grab_keyboard_reply(connection,xcb_grab_keyboard(connection,1,window.id,XCB_CURRENT_TIME,XCB_GRAB_MODE_ASYNC,
				XCB_GRAB_MODE_ASYNC),nullptr));
			window.grabbed=true;
			if (keyboard && keyboard->status==XCB_GRAB_STATUS_SUCCESS) {
				keyboard_window=handle;
				queue({handle,0,0,0,DesktopEventKind::keyboard_focus});
			}
		}
		attach_frame(window,width,height,pixels);
		xcb_flush(connection);
	}

	void set_maximized(Window &window,bool wanted,bool mapped) {
		const xcb_atom_t states[]={atoms[maximized_vert],atoms[maximized_horz]};
		if (!mapped) {
			if (wanted) property(window.id,atoms[net_wm_state],atoms[atom_type],32,2,states);
			return;
		}
		xcb_client_message_event_t message{};
		message.response_type=XCB_CLIENT_MESSAGE; message.format=32; message.window=window.id; message.type=atoms[net_wm_state];
		message.data.data32[0]=wanted ? 1 : 0; message.data.data32[1]=states[0]; message.data.data32[2]=states[1]; message.data.data32[3]=1;
		xcb_send_event(connection,0,screen->root,XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
			reinterpret_cast<const char *>(&message));
	}

	void maximize(uint64_t handle,bool wanted) {
		if (wanted) maximized.insert(handle); else maximized.erase(handle);
		const auto found=windows.find(handle);
		if (found==windows.end() || found->second->is_popup || found->second->maximized==wanted) return;
		found->second->maximized=wanted;
		set_maximized(*found->second,wanted,true);
		xcb_flush(connection);
	}

	void present(uint64_t handle,std::string_view title,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
		if (!handle || !width || !height || width>8192 || height>8192 || uint64_t(width)*height>16*1024*1024 || pixels.size()!=size_t(width)*height*4)
			throw std::runtime_error("invalid native X11 frame");
		connect();
		auto found=windows.find(handle);
		// A window shown as a popup until now becomes a window of its own.
		if (found!=windows.end() && found->second->is_popup) {
			destroy(*found->second);
			windows.erase(found);
			found=windows.end();
		}
		if (found==windows.end()) {
			auto window=std::make_unique<Window>();
			window->width=width; window->height=height;
			window->maximized=maximized.contains(handle);
			window->id=make_window(false,0,0,width,height);
			property(window->id,atoms[wm_protocols],atoms[atom_type],32,1,&atoms[wm_delete_window]);
			// Raven draws the frame itself, as on Wayland; the window manager adds none.
			const uint32_t hints[]={2,0,0,0,0};
			property(window->id,atoms[motif_hints],atoms[motif_hints],32,5,hints);
			set_title(*window,title);
			set_maximized(*window,window->maximized,false);
			xcb_map_window(connection,window->id);
			if (tracing()) std::fprintf(stderr,"x11: window %llx -> %x %ux%u\n",(unsigned long long)handle,window->id,width,height);
			found=windows.emplace(handle,std::move(window)).first;
			// X keeps no content for a window that is not shown yet: frames, a graphics driver's included, wait until the window manager
			// has mapped it.
			const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
			while (!found->second->mapped && !found->second->closed && std::chrono::steady_clock::now()<end) events(std::chrono::milliseconds(20),true);
		}
		auto &window=*found->second;
		set_title(window,title);
		// The window manager's close is a request: once the program has it (WM_CLOSE), a window it keeps goes on showing its frames.
		if (window.closed && window.close_posted) {
			window.closed=false;
			window.close_posted=false;
		}
		if (window.closed) throw std::runtime_error("X11 window cannot accept this frame");
		if (window.maximized && window.pending_width && window.pending_height &&
			(window.pending_width!=width || window.pending_height!=height)) return;
		if (window.pending_width==width && window.pending_height==height) window.resized=false;
		// A frame of another size is the program resizing its window, unless the window manager's own resize is still being answered.
		if ((window.width!=width || window.height!=height) && !window.resized) {
			const uint32_t values[]={width,height};
			xcb_configure_window(connection,window.id,XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT,values);
		}
		window.width=width; window.height=height;
		attach_frame(window,width,height,pixels);
		xcb_flush(connection);
		if (xcb_connection_has_error(connection)) throw std::runtime_error("X11 connection failed during presentation");
	}

	void focus_in(uint64_t handle) {
		keyboard_window=handle;
		DesktopEvent event{handle,0,0,0,DesktopEventKind::keyboard_focus};
		Reply keys(xcb_query_keymap_reply(connection,xcb_query_keymap(connection),nullptr));
		if (keys)
			for (uint32_t code=8;code<256;++code)
				if (keys->keys[code/8] & (1U<<(code%8))) {
					try { event.held_keys.push_back(code-8); } catch (...) { input_failed=true; return; }
				}
		keys_down.fill(false);
		for (const auto key : event.held_keys) keys_down[key]=true;
		queue(std::move(event));
	}

	// A press anywhere but on a grabbing popup dismisses the popups, and the press goes to none of the program's windows.
	bool dismisses(int32_t x,int32_t y) {
		bool grabbing=false,inside=false;
		for (const auto &[handle,window] : windows) {
			if (!window->grabbed || window->closed) continue;
			grabbing=true;
			const auto &area=window->screen;
			if (x>=area[0] && x<area[2] && y>=area[1] && y<area[3]) inside=true;
		}
		if (!grabbing || inside) return false;
		for (auto &[handle,window] : windows) if (window->grabbed) window->closed=true;
		ungrab_if_unused();
		return true;
	}

	void button(const xcb_button_press_event_t &event,bool pressed) {
		last_time=event.time;
		root_x=event.root_x; root_y=event.root_y;
		if (pressed && dismisses(event.root_x,event.root_y)) return;
		pointer_window=handle_of(event.event);
		pointer_x=event.event_x; pointer_y=event.event_y;
		if (!pointer_window) return;
		// Buttons four to seven are the wheel: up and right are positive notches.
		if (event.detail>=4 && event.detail<=7) {
			if (pressed) pointer_event(DesktopEventKind::pointer_wheel,event.detail==4 || event.detail==7 ? 120U : uint32_t(-120),event.detail>=6 ? 1 : 0);
			return;
		}
		static constexpr uint32_t codes[]={0,0x110,0x112,0x111,0,0,0,0,0x113,0x114};
		if (event.detail<std::size(codes) && codes[event.detail]) pointer_event(DesktopEventKind::pointer_button,codes[event.detail],pressed ? 1 : 0);
	}

	void raw_motion(const xcb_input_raw_motion_event_t *event) {
		const auto target=constrained_window ? constrained_window : pointer_window;
		if (!target) return;
		const auto *mask=xcb_input_raw_button_press_valuator_mask(event);
		const auto *values=xcb_input_raw_button_press_axisvalues_raw(event);
		int64_t moved[2]={0,0};
		int index=0;
		for (uint32_t axis=0;axis<uint32_t(event->valuators_len)*32;++axis) {
			if (!(mask[axis/32] & (1U<<(axis%32)))) continue;
			if (axis<2) moved[axis]=int64_t(values[index].integral)*256+int64_t(values[index].frac>>24);
			++index;
		}
		relative_x+=moved[0]; relative_y+=moved[1];
		const auto x=int32_t(relative_x/256),y=int32_t(relative_y/256);
		relative_x-=int64_t(x)*256; relative_y-=int64_t(y)*256;
		if (x || y) queue({target,0,0,0,DesktopEventKind::pointer_relative,x,y});
	}

	void selection_request(const xcb_selection_request_event_t &request) {
		xcb_selection_notify_event_t reply{};
		reply.response_type=XCB_SELECTION_NOTIFY; reply.time=request.time; reply.requestor=request.requestor;
		reply.selection=request.selection; reply.target=request.target;
		const auto target=request.property ? request.property : request.target;
		if (owns_selection && request.selection==atoms[clipboard]) {
			if (request.target==atoms[targets]) {
				const xcb_atom_t offered[]={atoms[targets],atoms[utf8_string],atoms[string_type],atoms[text_type],atoms[text_utf8],atoms[text_plain]};
				property(request.requestor,target,atoms[atom_type],32,uint32_t(std::size(offered)),offered);
				reply.property=target;
			} else if ((request.target==atoms[utf8_string] || request.target==atoms[string_type] || request.target==atoms[text_type] ||
				request.target==atoms[text_utf8] || request.target==atoms[text_plain]) && source_text.size()+256<request_bytes) {
				property(request.requestor,target,request.target==atoms[text_type] ? atoms[utf8_string] : request.target,8,
					uint32_t(source_text.size()),source_text.data());
				reply.property=target;
			}
		}
		xcb_send_event(connection,0,request.requestor,XCB_EVENT_MASK_NO_EVENT,reinterpret_cast<const char *>(&reply));
		xcb_flush(connection);
	}

	void handle(xcb_generic_event_t *generic) {
		const auto type=uint8_t(generic->response_type & 0x7f);
		if (xkb_event && type==xkb_event) {
			const auto kind=reinterpret_cast<const uint8_t *>(generic)[1];
			if (kind==XCB_XKB_NEW_KEYBOARD_NOTIFY || kind==XCB_XKB_MAP_NOTIFY) send_keymap();
			else if (kind==XCB_XKB_STATE_NOTIFY) {
				const auto &state=*reinterpret_cast<const xcb_xkb_state_notify_event_t *>(generic);
				queue({0,state.baseMods,state.latchedMods,uint64_t(state.lockedMods)|(uint64_t(state.lockedGroup)<<32),DesktopEventKind::modifiers});
			}
			return;
		}
		if (xfixes_event && type==xfixes_event+XCB_XFIXES_SELECTION_NOTIFY) {
			const auto &notice=*reinterpret_cast<const xcb_xfixes_selection_notify_event_t *>(generic);
			if (notice.selection!=atoms[clipboard]) return;
			selection_present=notice.owner!=XCB_NONE;
			if (notice.owner!=utility) { owns_selection=false; ++selection_counter; }
			return;
		}
		switch (type) {
		case XCB_GE_GENERIC: {
			const auto *event=reinterpret_cast<const xcb_ge_generic_event_t *>(generic);
			if (xinput_opcode && event->extension==xinput_opcode && event->event_type==XCB_INPUT_RAW_MOTION)
				raw_motion(reinterpret_cast<const xcb_input_raw_motion_event_t *>(generic));
			return;
		}
		case XCB_EXPOSE: {
			const auto &event=*reinterpret_cast<const xcb_expose_event_t *>(generic);
			if (auto *window=window_of(event.window)) expose(*window,event.x,event.y,event.x+event.width,event.y+event.height);
			return;
		}
		case XCB_CONFIGURE_NOTIFY: {
			const auto &event=*reinterpret_cast<const xcb_configure_notify_event_t *>(generic);
			auto *window=window_of(event.window);
			if (!window || window->is_popup || event.window!=event.event) return;
			if (event.width==window->width && event.height==window->height) { window->resized=false; window->pending_width=window->pending_height=0; return; }
			window->pending_width=event.width; window->pending_height=event.height; window->resized=true;
			return;
		}
		case XCB_MAP_NOTIFY: case XCB_UNMAP_NOTIFY: {
			const auto &event=*reinterpret_cast<const xcb_map_notify_event_t *>(generic);
			if (event.window!=event.event) return;
			if (auto *window=window_of(event.window)) window->mapped=type==XCB_MAP_NOTIFY;
			return;
		}
		case XCB_CLIENT_MESSAGE: {
			const auto &event=*reinterpret_cast<const xcb_client_message_event_t *>(generic);
			if (event.type==atoms[wm_protocols] && event.data.data32[0]==atoms[wm_delete_window])
				if (auto *window=window_of(event.window)) window->closed=true;
			return;
		}
		case XCB_FOCUS_IN: case XCB_FOCUS_OUT: {
			const auto &event=*reinterpret_cast<const xcb_focus_in_event_t *>(generic);
			if (event.mode==XCB_NOTIFY_MODE_GRAB || event.mode==XCB_NOTIFY_MODE_UNGRAB || event.detail==XCB_NOTIFY_DETAIL_POINTER ||
				event.detail==XCB_NOTIFY_DETAIL_INFERIOR) return;
			const auto handle=handle_of(event.event);
			if (!handle) return;
			if (type==XCB_FOCUS_IN) focus_in(handle);
			else if (keyboard_window==handle) { keyboard_window=0; queue({0,0,0,0,DesktopEventKind::keyboard_focus}); }
			return;
		}
		case XCB_KEY_PRESS: case XCB_KEY_RELEASE: {
			const auto &event=*reinterpret_cast<const xcb_key_press_event_t *>(generic);
			last_time=event.time;
			if (event.detail<8) return;
			const uint32_t code=event.detail-8U;
			keys_down[code]=type==XCB_KEY_PRESS;
			const auto handle=handle_of(event.event);
			queue({handle ? handle : keyboard_window,code,type==XCB_KEY_PRESS ? 1U : 0U,0,DesktopEventKind::key});
			return;
		}
		case XCB_BUTTON_PRESS: case XCB_BUTTON_RELEASE:
			button(*reinterpret_cast<const xcb_button_press_event_t *>(generic),type==XCB_BUTTON_PRESS);
			return;
		case XCB_MOTION_NOTIFY: {
			const auto &event=*reinterpret_cast<const xcb_motion_notify_event_t *>(generic);
			root_x=event.root_x; root_y=event.root_y;
			const auto *window=window_of(event.event);
			// Under a popup grab the pointer outside the program's windows is reported to the popup; it is over no window then.
			if (!window || event.event_x<0 || event.event_y<0 || event.event_x>=int32_t(window->width) || event.event_y>=int32_t(window->height)) return;
			pointer_window=handle_of(event.event);
			pointer_x=event.event_x; pointer_y=event.event_y;
			pointer_event(DesktopEventKind::pointer_motion);
			return;
		}
		case XCB_ENTER_NOTIFY: case XCB_LEAVE_NOTIFY: {
			const auto &event=*reinterpret_cast<const xcb_enter_notify_event_t *>(generic);
			// Crossing into the driver's child window keeps the pointer on the window itself.
			if (event.mode!=XCB_NOTIFY_MODE_NORMAL || event.detail==XCB_NOTIFY_DETAIL_INFERIOR) return;
			const auto handle=handle_of(event.event);
			if (type==XCB_LEAVE_NOTIFY) { if (pointer_window==handle) pointer_window=0; return; }
			pointer_window=handle;
			pointer_x=event.event_x; pointer_y=event.event_y;
			pointer_event(DesktopEventKind::pointer_motion);
			return;
		}
		case XCB_SELECTION_REQUEST:
			selection_request(*reinterpret_cast<const xcb_selection_request_event_t *>(generic));
			return;
		case XCB_SELECTION_CLEAR:
			if (reinterpret_cast<const xcb_selection_clear_event_t *>(generic)->selection==atoms[clipboard]) owns_selection=false;
			return;
		case XCB_SELECTION_NOTIFY: {
			const auto &event=*reinterpret_cast<const xcb_selection_notify_event_t *>(generic);
			if (event.requestor==utility) { selection_arrived=true; selection_property=event.property; }
			return;
		}
		default:
			return;
		}
	}

	void drain() {
		while (auto *event=xcb_poll_for_event(connection)) {
			handle(event);
			std::free(event);
		}
		for (auto &[handle,window] : windows) {
			const auto area=window->exposed;
			window->exposed={0,0,0,0};
			upload(*window,area[0],area[1],area[2],area[3]);
		}
		xcb_flush(connection);
		if (xcb_connection_has_error(connection)) throw std::runtime_error("X11 connection failed");
		if (input_failed) throw std::runtime_error(input_error);
	}

	// Takes what the server sent, waiting up to the budget for something to arrive; a caller with input to deliver waits only when it is
	// itself waiting for a reply from the server (patient).
	void events(std::chrono::milliseconds budget,bool patient=false) {
		drain();
		if ((!patient && !pending.empty()) || budget.count()<=0) return;
		pollfd socket{xcb_get_file_descriptor(connection),POLLIN,0};
		const auto ready_count=::poll(&socket,1,int(budget.count()));
		if (ready_count>0 && (socket.revents & (POLLERR|POLLHUP|POLLNVAL))) throw std::runtime_error("X11 connection failed");
		drain();
	}

	void publish_text(std::string_view text) {
		connect();
		source_text.assign(text);
		xcb_set_selection_owner(connection,utility,atoms[clipboard],last_time);
		Reply owner(xcb_get_selection_owner_reply(connection,xcb_get_selection_owner(connection,atoms[clipboard]),nullptr));
		owns_selection=owner && owner->owner==utility;
		if (owns_selection) selection_present=true;
		xcb_flush(connection);
	}

	std::optional<std::pair<uint64_t,std::string>> fetch_text() {
		if (!connection || owns_selection || !selection_present) return std::nullopt;
		for (const auto target : {atoms[utf8_string],atoms[string_type]}) {
			selection_arrived=false; selection_property=XCB_NONE;
			xcb_delete_property(connection,utility,atoms[raven_selection]);
			xcb_convert_selection(connection,utility,atoms[clipboard],target,atoms[raven_selection],last_time);
			xcb_flush(connection);
			const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
			while (!selection_arrived) {
				const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(limit-std::chrono::steady_clock::now());
				if (left.count()<=0) break;
				events(std::min(left,std::chrono::milliseconds(20)),true);
			}
			if (!selection_arrived) break;
			if (selection_property==XCB_NONE) continue;
			Reply value(xcb_get_property_reply(connection,xcb_get_property(connection,1,utility,atoms[raven_selection],XCB_GET_PROPERTY_TYPE_ANY,0,
				(16U<<20)/4),nullptr));
			// A transfer in increments is too large to be taken here.
			if (!value || value->type==atoms[incr] || value->format!=8) return std::make_pair(selection_counter,std::string());
			const auto *data=static_cast<const char *>(xcb_get_property_value(value.value));
			return std::make_pair(selection_counter,std::string(data,size_t(xcb_get_property_value_length(value.value))));
		}
		return std::make_pair(selection_counter,std::string());
	}

	std::optional<HostSurface> host_surface(uint64_t handle,uint64_t window,int32_t x,int32_t y,uint32_t width,uint32_t height,bool shown) {
		if (!connection) return std::nullopt;
		const auto found=windows.find(handle);
		const xcb_window_t parent=found!=windows.end() && found->second->id ? found->second->id : screen->root;
		shown=shown && parent!=screen->root;
		width=std::clamp(width,1U,8192U); height=std::clamp(height,1U,8192U);
		auto &entry=hosted[window];
		if (!entry.id) {
			entry.id=xcb_generate_id(connection);
			entry.parent=parent;
			const uint32_t values[]={XCB_BACK_PIXMAP_NONE,0,XCB_GRAVITY_NORTH_WEST};
			xcb_create_window(connection,screen->root_depth,entry.id,parent,int16_t(x),int16_t(y),uint16_t(width),uint16_t(height),0,
				XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,XCB_CW_BACK_PIXMAP|XCB_CW_BORDER_PIXEL|XCB_CW_BIT_GRAVITY,values);
			entry.place={uint32_t(x),uint32_t(y),width,height};
		} else if (entry.parent!=parent) {
			xcb_reparent_window(connection,entry.id,parent,int16_t(x),int16_t(y));
			entry.parent=parent;
		}
		const std::array<uint32_t,4> place{uint32_t(x),uint32_t(y),width,height};
		if (place!=entry.place) {
			xcb_configure_window(connection,entry.id,XCB_CONFIG_WINDOW_X|XCB_CONFIG_WINDOW_Y|XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT,place.data());
			entry.place=place;
		}
		if (shown!=entry.shown) {
			if (shown) xcb_map_window(connection,entry.id); else xcb_unmap_window(connection,entry.id);
			entry.shown=shown;
		}
		xcb_flush(connection);
		if (tracing()) std::fprintf(stderr,"x11: surface of %x is %x at %d,%d %ux%u%s\n",parent,entry.id,x,y,width,height,shown ? "" : " hidden");
		HostSurface surface{connection,nullptr,HostPlatform::xcb,entry.id,screen->root_visual};
		return surface;
	}

	void release_constraint() {
		if (!confinement) return;
		xcb_destroy_window(connection,confinement);
		confinement=0; constrained_window=0;
		ungrab_if_unused();
	}

	void confine(uint64_t handle,std::optional<std::array<int32_t,4>> rectangle) {
		if (!connection) return;
		release_constraint();
		const auto found=windows.find(handle);
		if (rectangle && found!=windows.end() && !found->second->is_popup && found->second->id) {
			const auto &value=*rectangle;
			// A rectangle of a point or two holds the pointer still: raw motion keeps being reported.
			const bool locked=value[2]-value[0]<=2 && value[3]-value[1]<=2;
			confinement=xcb_generate_id(connection);
			xcb_create_window(connection,0,confinement,found->second->id,int16_t(value[0]),int16_t(value[1]),uint16_t(std::max(value[2]-value[0],1)),
				uint16_t(std::max(value[3]-value[1],1)),0,XCB_WINDOW_CLASS_INPUT_ONLY,XCB_COPY_FROM_PARENT,0,nullptr);
			xcb_map_window(connection,confinement);
			if (locked) xcb_warp_pointer(connection,XCB_NONE,confinement,0,0,0,0,0,0);
			Reply grab(xcb_grab_pointer_reply(connection,xcb_grab_pointer(connection,1,found->second->id,XCB_EVENT_MASK_BUTTON_PRESS|
				XCB_EVENT_MASK_BUTTON_RELEASE|XCB_EVENT_MASK_POINTER_MOTION,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,confinement,XCB_NONE,
				XCB_CURRENT_TIME),nullptr));
			if (grab && grab->status==XCB_GRAB_STATUS_SUCCESS) constrained_window=handle;
			else { xcb_destroy_window(connection,confinement); confinement=0; }
		}
		xcb_flush(connection);
	}

	void interactive(uint64_t handle,uint32_t action) {
		const auto found=windows.find(handle);
		if (found==windows.end() || found->second->is_popup || !found->second->id) return;
		// Edges as the frame names them (top 1, bottom 2, left 4, right 8 and their sums) to the window manager's directions.
		static constexpr std::array<int32_t,11> directions{8,1,5,-1,7,0,6,-1,3,2,4};
		if (action>=directions.size() || directions[action]<0) return;
		xcb_ungrab_pointer(connection,XCB_CURRENT_TIME);
		xcb_client_message_event_t message{};
		message.response_type=XCB_CLIENT_MESSAGE; message.format=32; message.window=found->second->id; message.type=atoms[net_wm_moveresize];
		message.data.data32[0]=uint32_t(root_x); message.data.data32[1]=uint32_t(root_y); message.data.data32[2]=uint32_t(directions[action]);
		message.data.data32[3]=1; message.data.data32[4]=1;
		xcb_send_event(connection,0,screen->root,XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
			reinterpret_cast<const char *>(&message));
		xcb_flush(connection);
	}

	void remove(uint64_t handle) {
		maximized.erase(handle);
		if (pointer_window==handle) pointer_window=0;
		std::erase_if(pending,[&](const DesktopEvent &event) { return event.window==handle; });
		if (constrained_window==handle) release_constraint();
		const auto found=windows.find(handle);
		if (found==windows.end()) return;
		const bool grabbed=found->second->grabbed;
		const auto parent=found->second->parent;
		destroy(*found->second);
		windows.erase(found);
		if (grabbed) {
			ungrab_if_unused();
			// The keyboard goes back to the window the popup came from, as when a compositor's popup grab ends.
			if (keyboard_window==handle && windows.contains(parent)) { keyboard_window=parent; queue({parent,0,0,0,DesktopEventKind::keyboard_focus}); }
		}
		if (keyboard_window==handle) keyboard_window=0;
		if (connection) xcb_flush(connection);
	}

	std::vector<DesktopEvent> poll(std::chrono::milliseconds budget) {
		std::vector<DesktopEvent> result;
		if (!connection) { if (budget.count()>0) ::poll(nullptr,0,int(budget.count())); return result; }
		events(budget);
		result.swap(pending);
		for (auto &[handle,window] : windows) {
			if (window->resized && !window->is_popup) {
				result.push_back({handle,0,0,0,DesktopEventKind::resize,int32_t(window->pending_width),int32_t(window->pending_height)});
				window->resized=false;
			}
			if (window->closed && !window->close_posted) {
				if (window->is_popup) result.push_back({handle,0,0,0,DesktopEventKind::popup_done});
				else result.push_back({handle,0x10});
				window->close_posted=true;
			}
		}
		return result;
	}

	int descriptor() {
		if (!connection) return -1;
		// Events a reply wait already read leave the socket quiet: the signal, always readable, stands in for it while they wait.
		while (auto *event=xcb_poll_for_queued_event(connection)) { handle(event); std::free(event); }
		const bool due=!pending.empty() || std::any_of(windows.begin(),windows.end(),[](const auto &entry) {
			return entry.second->resized || (entry.second->closed && !entry.second->close_posted) || entry.second->exposed[2]>entry.second->exposed[0];
		});
		return due ? ready : xcb_get_file_descriptor(connection);
	}
};

XcbPresentation::XcbPresentation() : state(std::make_unique<State>()) {}
XcbPresentation::~XcbPresentation()=default;

DesktopPresentation XcbPresentation::callbacks()
{
	DesktopPresentation result{[this](uint64_t handle,std::string_view title,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
		state->present(handle,title,width,height,pixels);
	},[this](uint64_t handle) { state->remove(handle); },
	[this](std::chrono::milliseconds budget) { return state->poll(budget); },
	[this](uint64_t handle,uint64_t parent,int32_t x,int32_t y,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
		state->popup_surface(handle,parent,x,y,width,height,pixels);
	},[this](uint64_t handle,uint32_t action) { state->interactive(handle,action); },
	[this](std::string_view text) { state->publish_text(text); },
	[this]() { return state->fetch_text(); },
	[this](uint64_t handle,uint64_t window,int32_t x,int32_t y,uint32_t width,uint32_t height,bool shown) {
		return state->host_surface(handle,window,x,y,width,height,shown);
	},
	[this](uint64_t window) {
		const auto found=state->hosted.find(window);
		if (found==state->hosted.end()) return;
		if (state->connection) {
			xcb_destroy_window(state->connection,found->second.id);
			xcb_flush(state->connection);
		}
		state->hosted.erase(found);
	},
	[this](uint64_t handle,std::optional<std::array<int32_t,4>> rectangle) { state->confine(handle,rectangle); },
	[this](uint64_t handle,uint64_t owner,int32_t x,int32_t y,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
		state->popup_surface(handle,owner,x,y,width,height,pixels,false);
	},
	[this] { return state->descriptor(); },
	[this]() -> void * { state->connect(); return state->connection; },
	[this]() -> std::optional<std::pair<int32_t,int32_t>> {
		state->connect();
		return std::pair{int32_t(state->screen->width_in_pixels),int32_t(state->screen->height_in_pixels)};
	},
	[this](uint64_t handle,bool maximized) { state->maximize(handle,maximized); },
	[this](uint32_t shape) {
		if (state->cursor==shape) return;
		state->cursor=shape;
		state->show_cursor();
	}};
	result.platform=HostPlatform::xcb;
	return result;
}

}
