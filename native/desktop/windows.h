#pragma once

#include "objects.h"
#include "foreign_surface.h"
#include "presentation.h"
#include "keyboard.h"
#include <poll.h>
#include <array>
#include <chrono>
#include <map>
#include <set>
#include "nt/api.h"
#include "nt/environment.h"
#include <functional>
#include <deque>
#include <memory>
#include <optional>

namespace raven {

namespace paint { struct Canvas; }


class NativeWindows {
public:
	NativeWindows(NativeProcess &process, NativeEnvironment &environment, DesktopObjects &objects);
	void set_call(std::function<NativeResult(uint64_t,std::span<const uint64_t>)> invoke);
	void set_presentation(DesktopPresentation presentation);
	// A host surface over the client area of a window for a graphics driver to present into, and its release; null when the host shows no such window.
	// The surface a graphics driver renders to for the window; a hidden window gets one only when asked for (hidden), as an OpenGL
	// surface nothing shows would block the program's buffer swaps.
	std::optional<HostSurface> host_surface(uint64_t window,bool hidden=false);
	void release_host_surface(uint64_t window);
	void *host_display() const { return presentation.host_display ? presentation.host_display() : nullptr; }
	HostPlatform host_platform() const { return presentation.platform; }
	void set_surface_cleanup(std::function<void(uint64_t)> cleanup) { surface_cleanup = std::move(cleanup); }
	// The client area of a window as its swap chains see it; empty for a window that does not exist.
	std::optional<std::pair<uint32_t,uint32_t>> client_size(uint64_t window);
	void set_deadline(std::chrono::steady_clock::time_point deadline);
	// Parks a message waiter while other guest threads run, until input arrives or the next timer is due.
	void set_message_wait(std::function<void(std::function<bool()>,std::chrono::steady_clock::time_point)> wait) { message_wait = std::move(wait); }
	// How a thread waiting for messages sleeps when every other thread waits too: until the time `until` allows for the other threads,
	// on their descriptors besides the display's, then `drain` takes back what woke it (see NativeThreads::idle_until).
	void set_idle_sources(std::function<std::chrono::steady_clock::time_point(std::chrono::steady_clock::time_point)> until,
		std::function<void(std::vector<pollfd> &)> descriptors, std::function<void()> drain)
	{
		idle_until = std::move(until);
		idle_descriptors = std::move(descriptors);
		idle_drain = std::move(drain);
	}
	// Blocks the calling guest thread until the condition holds while the others run (a message sent to another thread's window).
	void set_block(std::function<void(std::function<bool()>)> block) { block_hook = std::move(block); }
	// Resolves the default menu of a window class (module, resource identifier) to a menu handle.
	// Allocates guest global memory for clipboard data that arrives from the host.
	void set_global_allocator(std::function<uint64_t(uint64_t)> allocator) { global_allocator = std::move(allocator); }
	void set_menu_loader(std::function<uint64_t(uint64_t,uint64_t)> loader) { menu_loader = std::move(loader); }
	bool dispatch(std::string_view name);
	// The served block where the screen context the guest's GetDC(NULL) takes is published (see the screen preludes), and the
	// offsets in it of the capability table, the context and its state.
	static constexpr size_t screen_caps = 0x400, screen_dc = 0x600, screen_dc_state = 0x608;
	void set_screen_block(uint64_t *block) { screen_block = reinterpret_cast<uint8_t *>(block); }
	// A call the runtime serves with the published screen context as its first argument may change it: the guest's ReleaseDC then
	// leaves it to the runtime. Called before every served call.
	void screen_dc_use(std::string_view name);
	// The window table the guest's window preludes read: a record of 256 bytes for each window, at its handle's low nine bits, and
	// the offsets in a record. The runtime brings the records of the windows it touched up to date before the guest runs again; a
	// window changed by a call stays touched until the outermost call in progress returns, as the call may change it after a callback.
	static constexpr size_t window_records=512, window_record=256, window_extra_bytes=96;
	// The focus window, published in the served block with the table: a flag, then the handle.
	static constexpr size_t focus_published=0x710, focus_window=0x718;
	void set_window_table(uint8_t *table);
	// The menu table the guest's menu preludes read: 1024 slots of 16 bytes (handle, block offset, item count) at the handle's low ten
	// bits, then the blocks: an item of 32 bytes each (type, state, identifier, text length, submenu, text offset) and their texts.
	static constexpr size_t menu_slots=1024, menu_blocks=0x4000, menu_area=512*1024;
	void set_menu_table(uint8_t *table);
	// The bitmap table GetObject reads for bitmaps: 1024 records of 48 bytes (handle, flags with 1 for a DIB section, BITMAP) at the
	// handle's low ten bits.
	static constexpr size_t bitmap_records=1024, bitmap_record=48;
	void set_bitmap_table(uint8_t *table);
	// GetSystemMetrics for indexes below 128, then GetSysColor for indexes below 64, as 32-bit values; both are fixed for the process.
	// At 1024 the accelerator tables: a count, then their handles.
	void set_system_table(uint8_t *table);
	// The device context attributes the guest sets and reads itself: a log of the contexts it changed (a count, then up to 64 handles),
	// then from 4096 a slot of 32 bytes for each context at its handle's low ten bits (handle, text color, background color and mode,
	// text alignment). The runtime takes the log before it serves a call and writes the contexts it touched before the guest runs again.
	static constexpr size_t dc_slots=1024, dc_slot=32, dc_slots_at=4096, dc_table_size=dc_slots_at+dc_slots*dc_slot;
	void set_dc_table(uint8_t *table);
	void apply_dc_log();
	// Releases the guest answers itself and leaves for the runtime: a log page (a count, then from 16 up to 255 entries of a handle
	// and an operation) and the set of valid icons (a handle at each handle's low ten bits). The guest takes a handle out of its tables
	// as it logs it; the runtime carries the log out before it serves a call and does not publish logged handles again until then.
	static constexpr size_t deferred_entries=255, icon_slots=1024;
	void set_deferred_tables(uint8_t *log,uint8_t *icons);
	void take_deferred(const std::function<void(uint32_t,uint64_t)> &apply);
	// In the served block: bit 0 when the flags are published, bit 1 while a message filter hook is installed; then the desktop window.
	static constexpr size_t loop_flags=0x738, desktop_handle=0x740;
	void publish_windows();
	void api_entered() { ++api_depth; }
	void api_left();
	// user32 entry points that follow the shared registry conventions; every handler runs on the calling guest thread.
	void register_api(ApiRegistry &registry);
	// Drawing marks the surface dirty; the next message retrieval presents it, so a burst of primitives costs one frame.
	void drawing(uint64_t window);
	void flush_dirty(bool force = false);
	// Whether a message, paint request or due timer waits for the guest; host input is delivered to the windows on the way, so this
	// runs only inside a call of the guest thread.
	bool input_ready();
	// Queue categories in the high word, new categories in the low word; safe in a waiting thread's condition.
	uint32_t queue_status(uint32_t thread,uint32_t mask,bool clear);
	void dump_windows();
	void shutdown();

private:
	void refresh_screen();
	NativeProcess &process;
	NativeEnvironment &environment;
	DesktopObjects &objects;
	std::function<NativeResult(uint64_t,std::span<const uint64_t>)> invoke;
	DesktopPresentation presentation;
	std::function<void(uint64_t)> surface_cleanup;
	void remove_host_window(uint64_t handle);
	struct Message {
		uint64_t window, wparam, lparam;
		uint32_t message, time;
		int32_t x=0,y=0;
		uint32_t character=0;
		// The guest thread whose queue holds the message; zero for one any thread may take.
		uint32_t thread=0;
		uint32_t kinds=0x108;
	};
	// The posted messages of every guest thread, each marked with the thread that owns it; HWNDs and drawing resources stay in
	// DesktopObjects.
	std::deque<Message> messages;
	// Queues a message for the thread that owns its window (the calling thread for a message to no window).
	void queue(Message message,uint32_t kinds=0x108);
	struct QueueState {
		uint32_t current=0,changed=0;
		std::map<uint64_t,uint64_t> paints;
	};
	std::map<uint32_t,QueueState> queues;
	uint32_t owner(uint64_t window) const;
	bool queued_for(uint32_t thread) const;
	// A message sent to a window of another thread waits here until that thread retrieves messages; the sender waits for the result.
	struct SentMessage { uint64_t window; uint32_t message; uint64_t wparam, lparam, result=0; uint32_t thread; bool taken=false, done=false; };
	std::deque<std::shared_ptr<SentMessage>> sent;
	// Runs the messages other threads sent to windows of the calling thread.
	void serve_sent();
	bool sent_for(uint32_t thread) const;
	std::function<void(std::function<bool()>)> block_hook;
	// A window of the calling thread that needs painting, under the filter window when one is given.
	uint64_t invalid_owned(uint64_t filter,uint32_t thread=0) const;
	// The window the last WM_PAINT went to; the next search for one starts after it.
	uint64_t paint_cursor=0;
	// Device contexts over windows of other processes, and the frames other processes drew into this process's windows.
	struct ForeignTarget { std::shared_ptr<ForeignSurface> surface; uint64_t bitmap=0; };
	std::map<uint64_t,ForeignTarget> foreign_targets;
	uint64_t foreign_dc(uint64_t window);
	std::map<uint64_t,uint32_t> foreign_frames;
	// Windows that received a frame from another process with something other than black on it.
	std::set<uint64_t> foreign_content;
	// Since when each window has been receiving frames of plain black.
	std::map<uint64_t,std::chrono::steady_clock::time_point> foreign_black;
	std::chrono::steady_clock::time_point foreign_checked{};
	uint64_t foreign_stamp=UINT64_MAX;
	void import_foreign_frames();
	uint8_t *screen_block=nullptr;
	// A table entry handed out while calls were in progress, with the depth of the outermost of them: it is compared again before the
	// guest runs until that call returns, as the call may change it through a pointer it took before a callback.
	template<class T> struct WatchedItem { uint64_t first; const T *second; unsigned depth; };
	// What each top-level window's frame was last drawn from (see paint_nonclient).
	std::map<uint64_t,std::u16string> painted_frames;
	uint8_t *window_table=nullptr;
	unsigned api_depth=0;
	// Whether what is written now is watched (see WatchedItem): not when written for a call that just returned.
	bool watching=true;
	void publish_window_record(uint64_t handle,const DesktopWindow *window);
	uint8_t *menu_table=nullptr;
	size_t menu_used=menu_blocks;
	// Where each menu's block was written and how much room it has there.
	std::map<uint64_t,std::pair<uint32_t,size_t>> menu_capacity;
	void publish_menu(uint64_t handle,const DesktopMenu *menu);
	bool menu_matches(uint64_t handle,const DesktopMenu &menu) const;
	void publish_menus();
	uint8_t *bitmap_table=nullptr;
	uint8_t *system_table=nullptr;
	uint8_t *dc_table=nullptr;
	uint8_t *deferred_log=nullptr,*icon_table=nullptr;
	std::vector<uint64_t> deferred_pending;
	bool deferred(uint64_t handle) const { return std::find(deferred_pending.begin(),deferred_pending.end(),handle)!=deferred_pending.end(); }
	void publish_icons();
	// The accelerator tables, the loop flags, the desktop and focus windows.
	void publish_words();
	uint64_t published_accelerators=UINT64_MAX;
	std::vector<WatchedItem<DesktopDC>> watched_dcs;
	void publish_dc(uint64_t handle,const DesktopDC *dc);
	void publish_dcs();
	std::vector<WatchedItem<DesktopBitmap>> watched_bitmaps;
	void publish_bitmap(uint64_t handle,const DesktopBitmap *bitmap,bool touched);
	void publish_bitmaps();
	std::vector<WatchedItem<DesktopWindow>> watched;
	// Publishes a free screen context over the desktop window when none is published.
	void publish_screen_dc(uint64_t desktop);
	// Withdraws the published context when the runtime releases it; false for any other context.
	bool withdraw_screen_dc(uint64_t handle);
	// Host input collected while threads waited, delivered by the next input_ready.
	std::deque<DesktopEvent> pending_events;
	void deliver_pending();
	// PostQuitMessage codes by thread.
	std::map<uint32_t,uint32_t> quits;
	std::set<uint64_t> dirty_roots;
	bool dumped=false;
	std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point deadline;
	std::function<void(std::function<bool()>,std::chrono::steady_clock::time_point)> message_wait;
	std::function<std::chrono::steady_clock::time_point(std::chrono::steady_clock::time_point)> idle_until;
	std::function<void(std::vector<pollfd> &)> idle_descriptors;
	std::function<void()> idle_drain;
	// Sleeps until the display, another thread or the next timer, caret blink or scroll repeat needs this thread.
	void wait_idle(std::chrono::steady_clock::time_point deadline);
	std::function<uint64_t(uint64_t,uint64_t)> menu_loader;
	uint32_t mouse_buttons=0;
	int32_t cursor_x=0,cursor_y=0;
	uint64_t focus=0,active=0;
	bool activating=false,has_queued_activation=false;
	uint64_t queued_activation=0;
	int focusing=0;
	bool keep_host_window=false;
	// A top-level window's surface released while it is resized, kept for the new size when it fits: making a surface maps fresh pages
	// into both processes.
	struct SpareSurface { std::shared_ptr<SharedPixels> pixels; uint64_t address=0; size_t size=0; };
	std::map<uint64_t,SpareSurface> spare_surfaces;
	bool resizing=false;
	void drop_spare_surface(uint64_t handle);
	// The compositor took the keyboard from the program's windows; applied once no window of the program receives it right after.
	bool focus_leaving=false;
	std::chrono::steady_clock::time_point focus_left{};
	void settle_focus_leave(bool now);
	// The windows whose surface was handed to a graphics driver, by the root they were last placed in, so that a frame moving or
	// resizing a client area, or showing or hiding a window, takes the surface along.
	std::map<uint64_t,uint64_t> hosted_windows;
	// Whether a window of the root carries a driver's surface: its host window must survive a resize.
	bool hosts_surface(uint64_t root) const;
	std::optional<HostSurface> place_hosted(uint64_t window);
	void place_host_surface(uint64_t root);
	// Whether nothing has been drawn into the window's client area yet: its surface still holds only zeros or what erasing it left,
	// or there is none.
	bool blank_client(uint64_t handle) const;
	KeyboardMap keyboard_map;
	std::array<bool,256> key_down{};
	// The key each held key went down as, so its release matches its press after a keymap change.
	std::array<KeyboardKey,256> key_pressed{};
	std::optional<Message> key_translation;
	void activate(uint64_t window);
	uint64_t set_focus(uint64_t window);
	void receive(const DesktopEvent &event);
	// Input that arrives while a button press is being placed in the queue (the program runs during its hit test and parent notifications
	// and may pump): it is taken in after the press, so the program sees the input in the order it happened.
	bool placing_press=false;
	// A tool window shown as a popup of its owner is moved by the program's side, as the compositor moves only toplevels: the window
	// whose caption is being dragged, where the pointer started and where the window was then.
	struct FrameDrag { uint64_t window=0; int32_t start_x=0,start_y=0; DesktopRect origin{}; } frame_drag;
	// The last press in a client area, which the next press of the same button may turn into a double click.
	struct LastPress { uint64_t window=0; uint32_t message=0,time=0; int32_t x=0,y=0; } last_press;
	// True when the window appears as a popup of its owner's host window rather than as a host window of its own.
	bool shown_on_owner(const DesktopWindow &window) const;
	// The root whose host window shows a window placed by the program: its owner's, or the one a child moved to the desktop came from.
	uint64_t host_owner(const DesktopWindow &window) const;
	void place_pointer(const DesktopEvent &event,uint32_t message,uint32_t buttons,int64_t x,int64_t y);
	void release_held_input();
	std::deque<DesktopEvent> held_input;
	uint64_t retrieve(std::span<const uint64_t> arguments,bool blocking);
	void post(uint64_t window,uint32_t message,uint64_t wparam,uint64_t lparam);
	uint64_t surface_dc(uint64_t window);
	bool tree_dispatch(std::string_view name);
	// Classic frame geometry in window-relative coordinates, derived from AdjustWindowRectEx rules.
	struct FrameLayout {
		int32_t border, caption_height, menu_height;
		bool edge;
		DesktopRect caption, menu_bar, close, maximize, minimize, icon;
	};
	FrameLayout frame_layout(const DesktopWindow &window) const;
	std::vector<DesktopRect> menu_bar_items(const DesktopWindow &window);
	void paint_nonclient(uint64_t window);
	void paint_modern_frame(uint64_t handle,const DesktopWindow &window,const FrameLayout &layout,paint::Canvas &canvas,bool is_active,int32_t width,
		int32_t height);
	// The surface color of the Windows 11 caption, which also fills the frame a program extends into its client area.
	uint32_t caption_color(const DesktopWindow &window,bool is_active) const;
	void paint_caption_buttons(uint64_t handle,const DesktopWindow &window,const FrameLayout &layout,paint::Canvas &canvas,bool is_active);
	uint64_t nonclient_hit(const DesktopWindow &window,int32_t x,int32_t y) const;
	// Menu bar item currently shown as pressed, and the window that owns it.
	std::optional<size_t> open_menu_item;
	uint64_t open_menu_window=0;
	uint64_t pressed_frame_hit=0,pressed_frame_window=0;
	// The caption button under the pointer, which the Windows 11 frame lights up.
	uint64_t hovered_frame_hit=0,hovered_frame_window=0;
	void hover_frame(uint64_t window,uint64_t hit);
	struct MenuPopup {
		uint64_t handle,menu,parent;
		int32_t x,y,width,height;
		std::vector<DesktopRect> items;
		std::optional<size_t> highlight;
		std::vector<uint8_t> pixels;
	};
	// Open drop-downs, outermost first; handles live in their own range so they never collide with window handles.
	std::vector<MenuPopup> popups;
	uint64_t next_popup=0x7b000000;
	bool menu_active() const { return open_menu_item.has_value() || !popups.empty(); }
	bool menu_event(const DesktopEvent &event);
	void menu_open_bar(uint64_t window,size_t index);
	void menu_open_popup(uint64_t menu,uint64_t parent,int32_t x,int32_t y);
	void menu_render(MenuPopup &popup);
	void menu_render_modern(MenuPopup &popup,const DesktopMenu &menu,paint::Canvas &canvas);
	void menu_highlight(size_t level,std::optional<size_t> index);
	void menu_select(size_t level,size_t index);
	void menu_close();
	// A menu TrackPopupMenu shows waits for its answer: the command chosen is kept here instead of being posted.
	bool menu_tracking=false;
	uint64_t tracked_command=0;
	uint64_t track_popup_menu(uint64_t menu,uint32_t flags,int32_t x,int32_t y,uint64_t window);
	void nonclient_pointer(uint64_t window,uint64_t hit,uint32_t message,int32_t x,int32_t y);
	// The window menu GetSystemMenu answers, made on first use; it opens from the caption and Alt+Space, and its choices are WM_SYSCOMMAND.
	uint64_t system_menu(uint64_t window);
	void system_menu_open(uint64_t window,int32_t x,int32_t y);
	bool system_dispatch(std::string_view name);
	void register_message_api(ApiRegistry &registry);
	void register_manager_api(ApiRegistry &registry);
	void register_menu_api(ApiRegistry &registry);
	void register_ext_api(ApiRegistry &registry);
	void register_scroll_api(ApiRegistry &registry);
	void register_long_api(ApiRegistry &registry);
	void register_ansi_api(ApiRegistry &registry);
	void register_theme_api(ApiRegistry &registry);
	// Visual styles: on unless RAVEN_THEME=classic; the open theme handles with their class, those drawn with the dark palette, the
	// theme each window opened last, the windows SetWindowTheme took out of the styles, those it gave a dark variant and those it gave
	// any application name; the windows a program allowed dark mode and the app mode it prefers (0 default, 1 allow dark, 2 force dark,
	// 3 force light).
	bool themed() const;
	std::map<uint64_t, std::string> themes;
	std::set<uint64_t> dark_themes;
	std::map<uint64_t, uint64_t> window_themes;
	std::set<uint64_t> unthemed_windows, dark_windows, named_windows, dark_allowed_windows;
	uint32_t app_mode = 0;
	uint64_t next_theme = 0x7a000001;
	// Layout of one non-client scroll bar in screen coordinates.
	struct ScrollGeometry {
		DesktopRect bar{},up{},down{},track{},thumb{};
		bool present=false,vertical=false,has_thumb=false;
	};
	ScrollGeometry scroll_geometry(const DesktopWindow &window,int bar) const;
	int scroll_part_at(const DesktopWindow &window,int bar,int32_t x,int32_t y) const;
	uint64_t scroll_hit(const DesktopWindow &window,int32_t x,int32_t y) const;
	bool scroll_sync_visibility(uint64_t window,int bar,bool allow_disable);
	void refresh_frame(uint64_t window);
	void paint_scrollbars(uint64_t root);
	void paint_child_frames(uint64_t root);
	void scroll_notify(uint64_t window,int bar,uint32_t code,int32_t position);
	bool scroll_pointer(uint64_t window,uint64_t hit,uint32_t message,int32_t x,int32_t y);
	bool scroll_motion(uint32_t message,int32_t x,int32_t y);
	void scroll_idle();
	// The scroll bar being operated with the pointer: which part, and for the thumb where it was grabbed.
	struct ScrollDrag { uint64_t window=0; int bar=0; int part=0; int32_t grab=0; std::chrono::steady_clock::time_point next{}; };
	ScrollDrag scroll_drag;
	// Text caret of the input queue: one per thread in Windows, one here. Its rectangle is drawn by inverting pixels.
	struct Caret {
		uint64_t window=0;
		int32_t width=0,height=0,x=0,y=0;
		int hidden=1;
		bool drawn=false;
		// Shaped by a monochrome bitmap of zeros, which leaves every pixel as it is when the caret is inverted with it.
		bool blank=false;
		std::chrono::steady_clock::time_point next{};
	};
	std::optional<Caret> caret;
	void caret_sync();
	void caret_idle();
	// Hooks installed with SetWindowsHookEx, newest first; handles live in their own range.
	// A hook for one thread (thread non-zero) runs only for what happens on that thread.
	struct Hook { uint64_t handle; int32_t id; uint64_t procedure; uint32_t thread; };
	bool hook_applies(const Hook &hook) const;
	std::vector<Hook> hooks;
	uint64_t next_hook=0x7e000000;
	bool has_hook(int32_t id) const;
	uint64_t call_hooks(int32_t id,int32_t code,uint64_t wparam,uint64_t lparam,uint64_t after=0);
	// In-process clipboard: each format maps to the global memory handle the owner supplied.
	std::map<uint32_t,uint64_t> clipboard_data;
	uint64_t clipboard_window=0,clipboard_owner=0;
	uint32_t clipboard_sequence=1;
	// Sequence already offered to the host selection and the host selection already imported.
	uint32_t clipboard_published=0;
	uint64_t host_selection=0;
	std::function<uint64_t(uint64_t)> global_allocator;
	std::array<uint8_t,256> vk_state{},vk_async{};
	// The mouse buttons count as keys too (VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1 and 2), from the MK_ flags of a pointer message.
	static void mouse_keys(std::array<uint8_t,256> &state,uint64_t buttons)
	{
		static constexpr std::pair<uint32_t,uint32_t> keys[]={{0x01,0x01},{0x02,0x02},{0x04,0x10},{0x05,0x20},{0x06,0x40}};
		for (const auto &[code,flag] : keys)
			state[code]=(buttons & flag) ? (state[code] | 0x80) : (state[code] & 0x7f);
	}
	std::map<uint64_t,uint64_t> system_menus;
	struct Tracking { uint64_t window=0; uint32_t flags=0; };
	Tracking tracking;
	void track_pointer(uint64_t window);
	// Persistent guest scratch memory for API handlers that need to pass structures to guest callbacks.
	uint64_t scratch_base=0;
	size_t scratch_used=0;
	uint64_t scratch_allocate(size_t size);
	// Shows, hides, minimizes, maximizes or restores a window; returns whether it was visible before.
	bool show_window(uint64_t window,uint32_t command);
	// Moves and resizes a window (screen coordinates for the outer rectangle) with the WM_WINDOWPOSCHANGING/CHANGED protocol.
	void place_window(uint64_t window,DesktopRect outer,uint64_t insert_after,uint32_t flags);
	void shift_window(uint64_t window,int32_t dx,int32_t dy);
	std::vector<uint64_t> top_level_windows() const;
	uint64_t window_at(int32_t x,int32_t y);
	bool enable_window(uint64_t window,bool enable);
	void invalidate_tree(uint64_t window,bool erase);
	// Window messages registered by name (RegisterWindowMessage); ids start at 0xc000 like atoms.
	std::map<std::u16string,uint32_t> registered_messages;
	int32_t cursor_count=0;
	// Raw mouse input (RegisterRawInputDevices): the window that receives WM_INPUT, whether legacy mouse messages are left out, and the
	// reports waiting to be read by GetRawInputData under their handle values. Movement is the change of the host pointer position.
	struct RawMouse { bool registered=false,no_legacy=false,relative=false; uint64_t target=0; bool have_position=false; int64_t x=0,y=0; };
	RawMouse raw_mouse;
	// ClipCursor: the rectangle on the screen and the window whose host surface keeps the pointer in it.
	std::optional<std::array<int32_t,4>> clip_rect;
	uint64_t clip_window=0;
	// A program that hides the cursor and keeps putting it back inside its window steers by mouse movement alone; the host cannot move
	// its pointer, so the pointer is held on that window until the cursor shows again.
	uint64_t warp_lock_window=0;
	void update_warp_lock(bool warped);
	// Hands the cursor the program shows to the host pointer.
	void show_cursor();
	std::map<uint64_t,std::array<int32_t,6>> raw_reports;
	uint64_t next_raw_report=0x1000;
	void raw_mouse_input(int64_t dx,int64_t dy,uint32_t button_flags,int16_t wheel);
	uint64_t current_cursor=desktop_stock_cursor;
	// Mouse capture and timers belong to the initial guest thread's input state.
	uint64_t capture=0;
	struct Timer { uint64_t window,id,procedure; uint32_t interval; std::chrono::steady_clock::time_point due; bool system=false; uint32_t thread=0; };
	std::vector<Timer> timers;
	uint64_t next_timer_id=0x1000;
	void post_due_timers();
	// The previous client size, when the caller has already stored a provisional one, decides whether WM_SIZE is due.
	// Children keep their place in the client area: when its origin moves they move with it. origin is where they were laid out against,
	// the client origin before the call unless the caller already moved the window.
	void recalculate_client(uint64_t window,int32_t previous_width=-1,int32_t previous_height=-1,std::optional<std::pair<int32_t,int32_t>> origin={});
	void reshape(uint64_t window,DesktopRect outer);
	uint64_t desktop_window();
	uint64_t desktop=0;
	struct Modifiers { bool shift,control,alt; };
	Modifiers modifier_state() const;
	void parent_notify(uint64_t window,uint32_t message);
	void discard_tree(uint64_t window);
	void reject_tree(uint64_t window);
	uint64_t pointer_child(uint64_t window,int32_t x,int32_t y);
	void present(uint64_t window, bool force = false);
	void release_surface(uint64_t window);
	DesktopRect update_rect(uint64_t window,uint64_t address);
	struct PaintStart { uint64_t dc; DesktopRect rect; bool erase; };
	PaintStart start_paint(uint64_t window);
	void finish_paint(uint64_t window);
	// WM_NCPAINT for a child with a border, and the window whose WM_NCPAINT is being sent, with whether it reached the default handling.
	void paint_frame(uint64_t window);
	uint64_t framing = 0;
	bool framed_by_default = false;
	std::string text(uint64_t address);
	std::u16string window_text(uint64_t address);
	uint64_t copy_window_text(uint64_t window,uint64_t output,uint64_t capacity);
	uint16_t atom(uint64_t name, uint64_t instance);
	uint64_t fill_class_info(uint16_t id,bool extended,std::span<const uint64_t> args);
	uint64_t send(uint64_t window, uint32_t message, uint64_t wparam, uint64_t lparam);
	uint64_t create(std::span<const uint64_t> arguments);
	uint64_t map_points(uint64_t from,uint64_t to,uint64_t points,uint32_t count);
	bool destroy(uint64_t window);
	uint64_t default_message(std::span<const uint64_t> arguments);
	uint64_t long_value(std::string_view name,std::span<const uint64_t> arguments);
	uint64_t class_info(std::string_view name,std::span<const uint64_t> arguments);
};

}
