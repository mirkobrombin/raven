#include <iostream>
#include "windows.h"
#include "nt/actctx.h"
#include "nt/text.h"
#include "brush.h"
#include "nt/nls.h"
#include "gdi.h"
#include "canvas.h"

#include <array>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <sys/mman.h>
#include <thread>
#include <utility>

namespace raven {

static uint64_t read(std::span<const uint8_t> bytes,size_t at,size_t width);

NativeWindows::NativeWindows(NativeProcess &process, NativeEnvironment &environment, DesktopObjects &objects)
	: process(process), environment(environment), objects(objects), deadline(std::chrono::steady_clock::now()) {}

void NativeWindows::dump_windows()
{
	if (std::getenv("RAVEN_DUMP_WINDOWS")) {
		for (const auto handle : objects.window_handles()) {
			const auto *window=objects.window(handle);
			const auto *definition=objects.window_class(window->class_id);
			std::cerr << "window " << std::hex << handle << std::dec << " " << (definition ? definition->name : "?") << " parent=" << std::hex << window->parent << std::dec
				<< " style=" << std::hex << window->style << " ex=" << window->ex_style << std::dec << " visible=" << window->visible
				<< " outer=" << window->outer.left << "," << window->outer.top << "," << window->outer.right << "," << window->outer.bottom
				<< " client=" << window->x << "," << window->y << "," << window->width << "x" << window->height
				<< " title=" << to_utf8(window->title) << "\n";
		}
	}
}

void NativeWindows::shutdown()
{
	dump_windows();
	try { flush_dirty(true); } catch (...) {}
	for (const auto handle : objects.window_handles()) {
		const auto *window=objects.window(handle);
		if (handle!=desktop && !window->parent && window->bitmap && presentation.remove) remove_host_window(handle);
	}
}

void NativeWindows::remove_host_window(uint64_t handle)
{
	if (surface_cleanup) surface_cleanup(handle);
	if (presentation.remove) presentation.remove(handle);
}

void NativeWindows::set_deadline(std::chrono::steady_clock::time_point end)
{
	deadline=end;
}

void NativeWindows::set_call(std::function<NativeResult(uint64_t,std::span<const uint64_t>)> caller)
{
	invoke=std::move(caller);
}

void NativeWindows::set_presentation(DesktopPresentation backend)
{
	presentation=std::move(backend);
	refresh_screen();
	if (presentation.poll) {
		auto poll=presentation.poll;
		presentation.poll=[this,poll=std::move(poll)](std::chrono::milliseconds budget) {
			auto events=poll(budget);
			refresh_screen();
			return events;
		};
	}
}

void NativeWindows::refresh_screen()
{
	if (!presentation.screen_size) return;
	const auto size=presentation.screen_size();
	if (!size || !objects.set_screen_size(size->first,size->second)) return;
	if (system_table) set_system_table(system_table);
	if (auto *window=objects.window(desktop)) {
		window->outer={0,0,size->first,size->second};
		window->x=window->y=0;
		window->width=size->first; window->height=size->second;
		// Existing screen DCs remain valid when a mode changes, including their guest-side capability table.
		if (screen_block && objects.bitmap(window->bitmap)) {
			const DesktopDC device{window->bitmap,window->bitmap,desktop};
			for (int32_t index=0;index<128;++index) {
				const uint32_t value=device_caps(objects,device,index);
				std::memcpy(screen_block+screen_caps+size_t(index)*4,&value,4);
			}
		}
	}
	for (const auto handle : top_level_windows()) {
		const auto *window=objects.window(handle);
		if ((window->style & 0x21000000U)==0x01000000U) reshape(handle,{0,0,size->first,size->second});
		if (objects.window(handle)) post(handle,0x7e,32,uint16_t(size->first) | (uint64_t(uint16_t(size->second))<<16));
	}
}

uint64_t NativeWindows::surface_dc(uint64_t handle)
{
	auto *window=objects.window(handle);
	if (!window) throw uint32_t(1400);
	if (window->dc) return window->dc;
	if (window->parent) {
		const auto root=objects.window_root(handle);surface_dc(root);
		window=objects.window(handle);const auto bitmap=objects.window(root)->bitmap;
		DesktopDC device{bitmap,bitmap,handle};objects.configure_window_dc(handle,device,false);
		const auto dc=objects.add(device);if (!dc) throw uint32_t(8);
		window=objects.window(handle);window->bitmap=bitmap;window->dc=dc;return dc;
	}

	// An empty window still owns a surface, one pixel large; its DCs clip every drawing operation away.
	const auto width=std::max<int64_t>(int64_t(window->outer.right)-window->outer.left,1),height=std::max<int64_t>(int64_t(window->outer.bottom)-window->outer.top,1);
	if (width>8192 || height>8192 || uint64_t(width)*uint64_t(height)>16*1024*1024)
		throw uint32_t(87);
	const auto size=(size_t(width)*size_t(height)*4+4095)&~size_t(4095);
	std::shared_ptr<SharedPixels> shared;
	uint64_t address=0;
	if (const auto spare=spare_surfaces.find(handle); spare!=spare_surfaces.end() && spare->second.size>=size) {
		shared=spare->second.pixels;
		address=spare->second.address;
		spare_surfaces.erase(spare);
	} else {
		drop_spare_surface(handle);
		shared=std::make_shared<SharedPixels>(size);
		address=process.map_shared(shared->descriptor(),size);
	}
	uint64_t bitmap=0,dc=0;
	try {
		bitmap=objects.add(DesktopBitmap{uint32_t(width),uint32_t(height),32,{},address,true,true,false,shared});
		if (!bitmap) throw uint32_t(8);
		DesktopDC device{bitmap,bitmap,handle};
		objects.configure_window_dc(handle,device,false);
		dc=objects.add(device);
		if (!dc) throw uint32_t(8);
		window=objects.window(handle); window->bitmap=bitmap; window->dc=dc;
		objects.attach_window_dcs(handle,bitmap);
		return dc;
	} catch (...) { if (dc) objects.erase(dc); if (bitmap) objects.erase(bitmap); process.unmap_memory(address); throw; }
}

bool NativeWindows::blank_client(uint64_t handle) const
{
	const auto *window=objects.window(handle);
	const auto *bitmap=window ? objects.bitmap(window->bitmap) : nullptr;
	if (!window) return false;
	if (!bitmap) return true;
	// Only erased so far: no WM_PAINT finished on the window and no other process has drawn anything into it or one of its children.
	if (!window->painted) {
		bool drawn=false;
		for (const auto drawn_window : foreign_content)
			if (drawn_window==handle || objects.is_child(handle,drawn_window)) { drawn=true; break; }
		if (!drawn) return true;
	}
	const auto pixels=bitmap->pixels();
	const int64_t left=std::max<int64_t>(int64_t(window->x)-window->outer.left,0),top=std::max<int64_t>(int64_t(window->y)-window->outer.top,0);
	const int64_t right=std::min<int64_t>(left+window->width,bitmap->width),bottom=std::min<int64_t>(top+window->height,bitmap->height);
	if (pixels.size()<size_t(bitmap->width)*bitmap->height*4) return false;
	for (int64_t y=top;y<bottom;++y) {
		const auto *row=pixels.data()+(size_t(y)*bitmap->width+size_t(left))*4;
		for (int64_t x=left;x<right;++x,row+=4)
			if (row[0] | row[1] | row[2] | row[3]) return false;
	}
	return true;
}

void NativeWindows::present(uint64_t handle, bool force)
{
	// The desktop is the screen itself; guest drawing on it never becomes a host window.
	if (handle==desktop) return;
	handle=objects.window_root(handle);
	if (!handle) throw uint32_t(1400);
	if (!presentation.present) throw uint32_t(120);
	dirty_roots.erase(handle);
	// The compositor does not show a window or its controls as empty rectangles: while the window or a visible child of the same thread
	// still waits for its first WM_PAINT, the frame is put off, for at most a quarter of a second (two seconds before the window first
	// shows, as a program can take that long to paint it), so that everything appears painted together. A window that has painted once
	// already shows its old content, so a program redrawing continuously, as while the user draws, is never held back. A surface about to
	// go, as the process ends or the window loses it, shows what was drawn on it at once (force).
	if (auto *waiting=force ? nullptr : objects.window(handle)) {
		const auto me=environment.thread_id();
		// The window itself waiting for its first WM_PAINT is held back the same way: it would show as an unpainted, black surface.
		bool pending=!waiting->painted && !waiting->update.empty() && (!waiting->thread || waiting->thread==me);
		for (const auto other : objects.window_handles()) {
			if (pending) break;
			const auto *candidate=std::as_const(objects).window(other);
			if (candidate && other!=handle && !candidate->painted && !candidate->update.empty() && objects.is_child(handle,other) &&
				objects.window_visible(other) && (!candidate->thread || candidate->thread==me)) {
				pending=true;
				break;
			}
		}
		// A window whose client area nothing reached yet would show as a black rectangle too: a program that draws it from another
		// process, as a browser's compositor does, ends WM_PAINT without drawing. In a program drawn that way its first frame waits a
		// moment for that drawing. A window that hands its client area to a graphics driver shows that surface instead and is not held.
		const bool blank=!pending && !waiting->presented && !foreign_content.empty() && !hosts_surface(handle) && blank_client(handle);
		const auto now=std::chrono::steady_clock::now();
		if (pending || blank) {
			if (!waiting->present_waiting) waiting->present_waiting=now;
			if (now-*waiting->present_waiting<std::chrono::milliseconds(blank ? 1000 : waiting->presented ? 250 : 2000)) {
				dirty_roots.insert(handle);
				return;
			}
		}
		waiting->present_waiting.reset();
		waiting->presented=true;
	}
	surface_dc(handle);
	paint_nonclient(handle);
	paint_child_frames(handle);
	paint_scrollbars(handle);
	// Painting sends messages, and a handler may have destroyed the window.
	const auto *window=objects.window(handle);
	if (!window) return;
	const auto *bitmap=objects.bitmap(window->bitmap);
	if (!bitmap) return;
	auto pixels=std::span<const uint8_t>(bitmap->pixels().data(),size_t(bitmap->width)*bitmap->height*4);
	// The frame extended into the client area shows through what the program painted black there, black with no alpha to GDI, as the
	// Windows compositor shows its frame, and the compositor draws the caption buttons over it when the client area covers the caption.
	std::vector<uint8_t> glazed;
	if (window->glass) {
		glazed.assign(pixels.begin(),pixels.end());
		const int32_t width=int32_t(bitmap->width),height=int32_t(bitmap->height);
		const int32_t left=std::max(window->x-window->outer.left,0),top=std::max(window->y-window->outer.top,0);
		const int32_t right=std::min(width,left+window->width),bottom=std::min(height,top+window->height);
		const auto &margins=*window->glass;
		const bool whole=margins[0]<0 || margins[1]<0 || margins[2]<0 || margins[3]<0;
		paint::Canvas canvas{glazed.data(),width,height};
		const uint32_t title=caption_color(*window,active==handle);
		for (int32_t y=top;y<bottom;++y)
			for (int32_t x=left;x<right;++x) {
				if (!whole && x>=left+margins[0] && x<right-margins[1] && y>=top+margins[2] && y<bottom-margins[3])
					continue;
				const auto *pixel=glazed.data()+(size_t(y)*width+x)*4;
				if (!pixel[0] && !pixel[1] && !pixel[2])
					canvas.set(x,y,title);
			}
		const auto layout=frame_layout(*window);
		if (themed() && layout.close.right>layout.close.left && top<=layout.close.top)
			paint_caption_buttons(handle,*window,layout,canvas,active==handle);
		pixels=glazed;
	}
	// A popup without a caption that another window owns (a menu, a drop-down list), or an owned tool window (a palette), opens where the
	// program placed it: as a popup of the owner's host window, since a host window of its own would be placed by the compositor.
	const auto owner=host_owner(*window);
	const auto *owning=owner && owner!=handle ? objects.window(owner) : nullptr;
	if (presentation.owned_popup && owning && shown_on_owner(*window)) {
		try {
			presentation.owned_popup(handle,owner,window->outer.left-owning->outer.left,window->outer.top-owning->outer.top,bitmap->width,bitmap->height,pixels);
			place_host_surface(handle);
			return;
		} catch (const std::exception &) {}
	}
	// Host titles use replacement characters; the guest retains every UTF-16 code unit.
	if (presentation.maximize) presentation.maximize(handle,(window->style & 0x01000000U)!=0);
	presentation.present(handle,encode_utf8(window->title,true),bitmap->width,bitmap->height,pixels);
	place_host_surface(handle);
}

std::optional<HostSurface> NativeWindows::host_surface(uint64_t handle,bool hidden)
{
	if (!presentation.host_surface) return std::nullopt;
	const auto root=objects.window_root(handle);
	if (!objects.window(root) || !objects.window(handle) || (!hidden && !objects.window_visible(handle))) return std::nullopt;
	// GPU drawing supplies the client pixels itself, so it cannot wait for WM_PAINT. A hidden window may get a surface nothing shows yet,
	// as a program makes its swap chain before it shows the window.
	if (objects.window_visible(root)) {
		try { present(root,true); } catch (...) { return std::nullopt; }
	}
	if (!objects.window(handle)) return std::nullopt;
	hosted_windows[handle]=root;
	auto surface=place_hosted(handle);
	if (!surface) hosted_windows.erase(handle);
	return surface;
}

bool NativeWindows::hosts_surface(uint64_t root) const
{
	for (const auto &[window,placed] : hosted_windows) if (placed==root) return true;
	return false;
}

std::optional<HostSurface> NativeWindows::place_hosted(uint64_t handle)
{
	const auto root=objects.window_root(handle);
	const auto *top=objects.window(root);
	const auto *window=objects.window(handle);
	if (!top || !window) return std::nullopt;
	hosted_windows[handle]=root;
	// Window positions are on the screen, children's included, so the client origin inside the frame the host window shows is the
	// window's position less the frame's.
	return presentation.host_surface(root,handle,window->x-top->outer.left,window->y-top->outer.top,uint32_t(std::max(window->width,1)),
		uint32_t(std::max(window->height,1)),objects.window_visible(handle));
}

void NativeWindows::place_host_surface(uint64_t root)
{
	if (!presentation.host_surface) return;
	std::vector<uint64_t> placed;
	for (const auto &[window,last] : hosted_windows)
		if (last==root || objects.window_root(window)==root) placed.push_back(window);
	for (const auto window:placed) place_hosted(window);
}

std::optional<std::pair<uint32_t,uint32_t>> NativeWindows::client_size(uint64_t handle)
{
	const auto *window=objects.window(handle);
	if (!window) return std::nullopt;
	return std::pair<uint32_t,uint32_t>{uint32_t(std::max(window->width,1)),uint32_t(std::max(window->height,1))};
}

void NativeWindows::release_host_surface(uint64_t handle)
{
	if (!presentation.release_host_surface) return;
	hosted_windows.erase(handle);
	presentation.release_host_surface(handle);
}

void NativeWindows::drawing(uint64_t handle)
{
	if (handle==desktop) return;
	const auto *window=objects.window(handle);
	if (window && objects.window_visible(handle)) dirty_roots.insert(objects.window_root(handle));
}

bool NativeWindows::input_ready()
{
	flush_dirty();
	caret_idle();
	scroll_idle();
	deliver_pending();
	if (presentation.poll)
		for (const auto &event : presentation.poll(std::chrono::milliseconds(0)))
			receive(event);
	serve_sent();
	const auto me=environment.thread_id();
	if (queued_for(me) || quits.contains(me))
		return true;
	post_due_timers();
	return queued_for(me) || invalid_owned(0);
}

void NativeWindows::deliver_pending()
{
	while (!pending_events.empty()) {
		const auto event = pending_events.front();
		pending_events.pop_front();
		// The window an event was collected for may have been hidden or destroyed before the event could be delivered: such an
		// event no longer has a target, as one for a surface the host no longer shows. A menu popup is no guest window; the menu
		// loop decides about its events.
		const bool popup=event.window>=0x7b000000ULL && event.window<next_popup;
		if (event.window && !popup && (!objects.window(event.window) || !objects.window(event.window)->visible))
			continue;
		receive(event);
	}
}

static uint32_t event_kinds(const DesktopEvent &event)
{
	switch (event.kind) {
	case DesktopEventKind::key: case DesktopEventKind::modifiers: return 1;
	case DesktopEventKind::pointer_motion: return 2;
	case DesktopEventKind::pointer_button: case DesktopEventKind::pointer_wheel: return 4;
	case DesktopEventKind::pointer_relative: return 0x400;
	case DesktopEventKind::message: return 0x108;
	case DesktopEventKind::keymap: return 0;
	default: return 0x40;
	}
}

uint32_t NativeWindows::queue_status(uint32_t thread,uint32_t mask,bool clear)
{
	const auto event_thread=[&](const DesktopEvent &event) { return owner(event.window ? event.window : focus); };
	if (presentation.poll)
		for (const auto &event : presentation.poll(std::chrono::milliseconds(0))) {
			pending_events.push_back(event);
			queues[event_thread(event)].changed|=event_kinds(event);
		}
	post_due_timers();
	uint32_t current=0;
	for (const auto &message : messages)
		if (!message.thread || message.thread==thread) current|=message.kinds;
	if (quits.contains(thread)) current|=0x108;
	if (sent_for(thread)) current|=0x40;
	for (const auto &event : pending_events)
		if (!event_thread(event) || event_thread(event)==thread) current|=event_kinds(event);
	auto &state=queues[thread];
	std::map<uint64_t,uint64_t> paints;
	for (const auto handle : objects.window_handles()) {
		const auto *window=objects.window(handle);
		if (!window || window->update.empty() || !objects.window_visible(handle) || (window->thread && window->thread!=thread)) continue;
		current|=0x20;
		paints[handle]=window->update_generation;
		const auto previous=state.paints.find(handle);
		if (previous==state.paints.end() || previous->second!=window->update_generation) state.changed|=0x20;
	}
	state.paints=std::move(paints);
	state.changed=(state.changed | (current & ~state.current)) & current;
	state.current=current;
	const uint32_t result=((current<<16) | state.changed) & ((mask<<16) | mask);
	if (clear) state.changed&=~mask;
	return result;
}

uint64_t NativeWindows::foreign_dc(uint64_t handle)
{
	auto size=foreign_window_size(handle);
	if (!size) throw uint32_t(1400);
	size->first=std::max<uint32_t>(size->first,1);
	size->second=std::max<uint32_t>(size->second,1);
	auto &target=foreign_targets[handle];
	if (!target.surface || target.surface->width!=size->first || target.surface->height!=size->second) {
		if (target.bitmap) {
			if (const auto *bitmap=objects.bitmap(target.bitmap); bitmap && bitmap->address) process.unmap_memory(bitmap->address);
			objects.erase(target.bitmap);
			target.bitmap=0;
		}
		target.surface=ForeignSurface::open_for_drawing(handle,size->first,size->second);
		if (!target.surface) throw uint32_t(8);
		const auto &pixels=*target.surface->pixels;
		const auto address=process.map_shared(pixels.descriptor(),pixels.size(),pixels.offset());
		DesktopBitmap record{size->first,size->second,32,{},address,true,false,true,target.surface->pixels};
		record.stride=size->first*4;
		target.bitmap=objects.add(std::move(record));
		if (!target.bitmap) { process.unmap_memory(address); throw uint32_t(8); }
	}
	DesktopDC device{target.bitmap,target.bitmap,0};
	device.leased=true; device.foreign=handle;
	const auto dc=objects.add(device);
	if (!dc) throw uint32_t(8);
	return dc;
}

void NativeWindows::import_foreign_frames()
{
	// New frames are looked for at most every 8 ms; a window whose root was drawn over since is given its last frame again, since the
	// owner's painting of the parent does not leave out the area another process draws.
	const auto now=std::chrono::steady_clock::now();
	const bool poll=now-foreign_checked>=std::chrono::milliseconds(8);
	if (poll) foreign_checked=now;
	if (!poll && dirty_roots.empty()) return;
	// Only windows another process has drawn into are looked at on every poll; all of them only when the set of drawn windows changed.
	bool every=false;
	if (poll) {
		const auto stamp=foreign_frames_stamp();
		every=stamp!=foreign_stamp;
		foreign_stamp=stamp;
	}
	for (const auto handle : objects.window_handles()) {
		if (handle<window_handle_base() || handle>window_handle_base()+0xffff) continue;
		const auto known=foreign_frames.find(handle);
		const bool drawn=known!=foreign_frames.end() && known->second;
		const bool redraw=drawn && dirty_roots.contains(objects.window_root(handle));
		if (!poll && !redraw) continue;
		if (poll && !redraw && !every && !drawn) continue;
		auto &seen=foreign_frames[handle];
		if (redraw) seen=0;
		take_foreign_frame(handle,seen,[&](const uint8_t *pixels,uint32_t width,uint32_t height) {
			const auto root=objects.window_root(handle);
			if (!root) return;
			// A browser's compositor hands a window frames of plain black before it has anything to show: first, and again while a page
			// it navigates to loads. Only a frame with something on it counts as drawn. Black frames are left out, so the old page stays
			// until the new one paints, as the browser keeps it, however long the network takes; a window never drawn waits four seconds.
			bool black=true;
			for (size_t at=0;at<size_t(width)*height*4 && black;at+=4)
				black=!(pixels[at] | pixels[at+1] | pixels[at+2]);
			if (!black) { foreign_content.insert(handle); foreign_black.erase(handle); }
			else if (foreign_content.contains(handle) || now-foreign_black.try_emplace(handle,now).first->second<std::chrono::seconds(4)) return;
			surface_dc(root);
			const auto *top=objects.window(root);
			const auto *window=objects.window(handle);
			const auto *bitmap=top ? objects.bitmap(top->bitmap) : nullptr;
			if (!window || !bitmap || !bitmap->shared) return;
			// The client origin inside the root's surface: window positions are on the screen, children's included.
			const int64_t x=int64_t(window->x)-top->outer.left,y=int64_t(window->y)-top->outer.top;
			const auto stride=size_t(bitmap->width)*4;
			auto target=bitmap->shared->data();
			const auto columns=std::min<int64_t>({int64_t(width),int64_t(window->width),int64_t(bitmap->width)-x});
			for (int64_t row=0; row<int64_t(height) && row<window->height && y+row<int64_t(bitmap->height); ++row) {
				if (y+row<0 || x<0 || columns<=0) continue;
				auto *line=target+size_t(y+row)*stride+size_t(x)*4;
				std::memcpy(line,pixels+size_t(row)*width*4,size_t(columns)*4);
				for (int64_t column=0; column<columns; ++column) line[column*4+3]=0xff;
			}
			dirty_roots.insert(root);
		});
	}
}

void NativeWindows::update_warp_lock(bool warped)
{
	const bool hidden=cursor_count<0;
	if (warp_lock_window && (!hidden || !objects.window(warp_lock_window) || warp_lock_window!=active)) {
		if (presentation.confine && clip_window!=warp_lock_window) presentation.confine(warp_lock_window,std::nullopt);
		warp_lock_window=0;
	}
	if (!warped || !hidden || warp_lock_window || !active || !presentation.confine || clip_window==active) return;
	const auto *window=objects.window(active);
	if (!window || cursor_x<window->outer.left || cursor_x>=window->outer.right || cursor_y<window->outer.top || cursor_y>=window->outer.bottom) return;
	const int32_t x=cursor_x-window->outer.left,y=cursor_y-window->outer.top;
	presentation.confine(active,std::array<int32_t,4>{x,y,x+1,y+1});
	warp_lock_window=active;
}

void NativeWindows::show_cursor()
{
	if (!presentation.cursor) return;
	// A cursor that is not a system one (made from a bitmap, or a resource of a module) shows as the arrow.
	uint32_t shape=32512;
	if (cursor_count<0 || !current_cursor) shape=0;
	else if (current_cursor>=desktop_stock_cursor && current_cursor<desktop_stock_cursor+desktop_stock_cursor_count)
		shape=uint32_t(32512+current_cursor-desktop_stock_cursor);
	static const bool trace=std::getenv("RAVEN_TRACE_CURSOR")!=nullptr;
	if (trace) fprintf(stderr,"cursor %llx count %d shape %u\n",(unsigned long long)current_cursor,cursor_count,shape);
	presentation.cursor(shape);
}

void NativeWindows::flush_dirty(bool force)
{
	settle_focus_leave(false);
	import_foreign_frames();
	const auto roots=std::move(dirty_roots);
	dirty_roots.clear();
	if (!presentation.present) return;
	for (const auto root:roots)
		if (objects.window(root) && objects.window_visible(root)) present(root, force);
}

void NativeWindows::release_surface(uint64_t handle)
{
	const auto *window=objects.window(handle);
	if (!window) return;
	// A surface about to disappear still shows what the guest last drew.
	if (!window->parent && dirty_roots.contains(handle) && presentation.present && window->bitmap) {
		try { present(handle, true); } catch (...) {}
	}
	objects.detach_window_dcs(handle);
	if (!window->parent) {
		// Children draw on the root's bitmap through their own surface contexts; those follow it out.
		const auto drop=[&](auto &&self,uint64_t node) -> void {
			for (const auto child:objects.child_windows(node)) {
				self(self,child);
				objects.detach_window_dcs(child);
				if (auto *record=objects.window(child)) {
					if (record->dc) objects.erase(record->dc);
					record->dc=record->bitmap=0;
				}
			}
		};
		drop(drop,handle);
		window=objects.window(handle);
	}
	if (focus==handle) focus=0;
	if (active==handle) active=0;
	// A resize keeps the host window: the next frame has the new size.
	if (!window->parent && window->bitmap && presentation.remove && !keep_host_window) remove_host_window(handle);
	const auto bitmap=window->bitmap,dc=window->dc;
	if (dc) objects.erase(dc);
	if (bitmap && !window->parent) {
		const auto *record=objects.bitmap(bitmap);
		const auto address=record->address;
		if (resizing && record->shared) {
			drop_spare_surface(handle);
			spare_surfaces[handle]={record->shared,address,record->shared->size()};
		} else {
			process.unmap_memory(address);
		}
		objects.erase(bitmap);
	}
	if (!resizing) drop_spare_surface(handle);
}

void NativeWindows::drop_spare_surface(uint64_t handle)
{
	const auto found=spare_surfaces.find(handle);
	if (found==spare_surfaces.end()) return;
	process.unmap_memory(found->second.address);
	spare_surfaces.erase(found);
}

DesktopRect NativeWindows::update_rect(uint64_t handle,uint64_t address)
{
	const auto *window=objects.window(handle); if (!window) throw uint32_t(1400);
	DesktopRect rect{0,0,window->width,window->height};
	if (address) {
		const auto bytes=process.read_memory(address,16);
		rect={int32_t(read(bytes,0,4)),int32_t(read(bytes,4,4)),int32_t(read(bytes,8,4)),int32_t(read(bytes,12,4))};
		rect.left=std::max(rect.left,0); rect.top=std::max(rect.top,0);
		rect.right=std::min(rect.right,window->width); rect.bottom=std::min(rect.bottom,window->height);
	}
	return rect;
}

// A child with a border, or with room it reserved in WM_NCCALCSIZE, hears WM_NCPAINT as BeginPaint starts, as on Windows. A program that draws that border itself, answering
// without the default handling, keeps what it drew: the frame the surface draws for children leaves that window out.
void NativeWindows::paint_frame(uint64_t handle)
{
	const auto *window=objects.window(handle);
	const bool reserved=window && (window->outer.right-window->outer.left!=window->width || window->outer.bottom-window->outer.top!=window->height);
	if (!window || !window->parent || !((window->ex_style & 0x20200U) || (window->style & 0x800000U) || reserved)) return;
	const auto saved=framing; const bool saved_default=framed_by_default;
	framing=handle; framed_by_default=false;
	send(handle,0x85,1,0);
	const bool by_default=framed_by_default;
	framing=saved; framed_by_default=saved_default;
	if (auto *record=objects.window(handle)) record->own_frame=!by_default;
}

NativeWindows::PaintStart NativeWindows::start_paint(uint64_t handle)
{
	paint_frame(handle);
	if (!objects.window(handle)) throw std::runtime_error("frame callback destroyed its painting window");
	const auto dc=surface_dc(handle);
	const bool erase=objects.start_paint(handle);
	const auto rect=objects.dc(dc)->clip->bounds();
	if (std::getenv("RAVEN_TRACE_PAINT")) {
		const auto *device=objects.dc(dc);
		std::cerr << "start " << std::hex << handle << std::dec << " clip " << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom;
		if (device->bounds) std::cerr << " bounds " << device->bounds->left << ',' << device->bounds->top << ',' << device->bounds->right << ',' << device->bounds->bottom;
		if (device->visible) { const auto box = device->visible->bounds(); std::cerr << " visible " << box.left << ',' << box.top << ',' << box.right << ',' << box.bottom; }
		std::cerr << " erase " << erase << '\n';
	}
	const bool pending=erase && !send(handle,0x14,dc,0);
	if (!objects.window(handle)) throw std::runtime_error("background callback destroyed its painting window");
	return {dc,rect,pending};
}

void NativeWindows::finish_paint(uint64_t handle)
{
	const auto *window=objects.window(handle);
	// A parent without WS_CLIPCHILDREN, or with WS_EX_COMPOSITED, may have drawn over its children: they draw again on top, as the
	// children on Windows show.
	std::optional<DesktopRect> painted;
	if (!(window->style & 0x02000000U) || (window->ex_style & 0x02000000U))
		if (const auto *device=objects.dc(window->dc); device && device->clip) painted=device->clip->bounds();
	objects.finish_paint(handle);
	if (painted)
		if (const auto *owner=objects.window(handle))
			for (const auto child : objects.child_windows(handle)) {
				const auto *inner=objects.window(child);
				if (!inner || !inner->visible) continue;
				const DesktopRect inside{std::max(painted->left+owner->x,inner->x)-inner->x,std::max(painted->top+owner->y,inner->y)-inner->y,
					std::min(painted->right+owner->x,inner->x+inner->width)-inner->x,std::min(painted->bottom+owner->y,inner->y+inner->height)-inner->y};
				if (inside.left<inside.right && inside.top<inside.bottom) objects.invalidate(child,inside,true);
			}
	// WS_EX_COMPOSITED paints the whole tree in one pass, from the bottom up: the children it covered paint now, before the frame
	// is shown, so that no frame shows the parent over unpainted children.
	if (painted && (window=objects.window(handle)) && (window->ex_style & 0x02000000U)) {
		const auto me=environment.thread_id();
		for (const auto child : objects.child_windows(handle)) {
			const auto *inner=objects.window(child);
			if (inner && inner->visible && !inner->update.empty() && !inner->painting && (!inner->thread || inner->thread==me))
				send(child,0x0f,0,0);
		}
	}
}

static uint64_t read(std::span<const uint8_t> bytes, size_t at, size_t width)
{
	uint64_t value=0;
	for (size_t byte=0; byte<width; ++byte) value|=uint64_t(bytes[at+byte])<<(byte*8);
	return value;
}

static void put(std::span<uint8_t> bytes, size_t at, uint64_t value, size_t width)
{
	for (size_t byte=0; byte<width; ++byte) bytes[at+byte]=uint8_t(value>>(byte*8));
}

// Guest MSG: hwnd, message, wParam, lParam, time, pt. Offsets differ between the 32-bit and 64-bit Windows ABIs.
struct MessageLayout { size_t size, message, wparam, lparam, time, x, y, pointer; };
static MessageLayout message_layout(bool narrow)
{
	return narrow ? MessageLayout{28,4,8,12,16,20,24,4} : MessageLayout{48,8,16,24,32,36,40,8};
}

static uint32_t message_time()
{
	return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void NativeWindows::raw_mouse_input(int64_t dx,int64_t dy,uint32_t button_flags,int16_t wheel)
{
	const auto target=raw_mouse.target && objects.window(raw_mouse.target) ? raw_mouse.target : focus;
	if (!target || !objects.window(target) || messages.size()>=10000) return;
	// Reports nobody read are dropped oldest first, as the system discards raw input a program leaves in its queue.
	while (raw_reports.size()>=1024) {
		const auto expired=raw_reports.begin()->first;
		std::erase_if(messages,[&](const Message &message) { return message.message==0xff && (message.kinds & 0x400) && message.lparam==expired; });
		raw_reports.erase(raw_reports.begin());
	}
	const auto handle=next_raw_report++;
	raw_reports[handle]={int32_t(std::clamp<int64_t>(dx,INT32_MIN,INT32_MAX)),int32_t(std::clamp<int64_t>(dy,INT32_MIN,INT32_MAX)),
		int32_t(button_flags),int32_t(wheel),0,0};
	queue({target,0,handle,0xffU,message_time(),cursor_x,cursor_y},0x400);
}

void NativeWindows::post(uint64_t window,uint32_t message,uint64_t wparam,uint64_t lparam)
{
	// A broadcast goes to every top-level window, each in its own thread's queue.
	if (window==0xffff) {
		for (const auto handle : top_level_windows()) post(handle,message,wparam,lparam);
		return;
	}
	if (window && !objects.window(window)) throw uint32_t(1400);
	if (messages.size()>=10000) throw uint32_t(1816);
	queue({window,wparam,lparam,message,message_time(),cursor_x,cursor_y});
}

uint32_t NativeWindows::owner(uint64_t window) const
{
	const auto *record=objects.window(window);
	return record ? record->thread : 0;
}

void NativeWindows::queue(Message message,uint32_t kinds)
{
	if (!message.thread) message.thread=message.window ? owner(message.window) : environment.thread_id();
	message.kinds=kinds;
	queues[message.thread].changed|=kinds;
	messages.push_back(message);
}

bool NativeWindows::queued_for(uint32_t thread) const
{
	return std::any_of(messages.begin(),messages.end(),[&](const Message &message) { return !message.thread || message.thread==thread; });
}

bool NativeWindows::sent_for(uint32_t thread) const
{
	return std::any_of(sent.begin(),sent.end(),[&](const auto &item) { return item->thread==thread && !item->taken; });
}

void NativeWindows::serve_sent()
{
	const auto me=environment.thread_id();
	for (;;) {
		const auto found=std::find_if(sent.begin(),sent.end(),[&](const auto &item) { return item->thread==me && !item->taken; });
		if (found==sent.end()) return;
		const auto item=*found;
		item->taken=true;
		try {
			item->result=objects.window(item->window) ? send(item->window,item->message,item->wparam,item->lparam) : 0;
		} catch (...) {
			item->done=true;
			std::erase(sent,item);
			throw;
		}
		item->done=true;
		std::erase(sent,item);
	}
}

uint64_t NativeWindows::invalid_owned(uint64_t filter,uint32_t thread) const
{
	const auto me=thread ? thread : environment.thread_id();
	// The search starts after the window painted last and goes round: a window invalidated with every pointer move (a status bar
	// showing the position) would otherwise be found first each time, and the others, the canvas the pointer draws on among them,
	// would wait for as long as the moves go on.
	auto handles=objects.window_handles();
	std::stable_partition(handles.begin(),handles.end(),[&](uint64_t handle) { return handle>paint_cursor; });
	for (const auto handle : handles) {
		const auto *window=std::as_const(objects).window(handle);
		if (!window || window->update.empty() || !objects.window_visible(handle) || (window->thread && window->thread!=me)) continue;
		if (filter && filter!=handle && !objects.is_child(filter,handle)) continue;
		// Parents paint before their children, as Windows orders WM_PAINT: a child may lie under what its parent draws, and handles
		// come back in any order once windows are destroyed and made again.
		uint64_t chosen=handle;
		for (auto parent=window->parent;parent && parent!=filter;) {
			const auto *above=std::as_const(objects).window(parent);
			if (!above) break;
			if (!above->update.empty() && objects.window_visible(parent) && (!above->thread || above->thread==me)) chosen=parent;
			parent=above->parent;
		}
		if (filter && chosen!=handle && filter!=chosen && !objects.is_child(filter,chosen)) chosen=handle;
		return chosen;
	}
	return 0;
}

// A compositor that moves the keyboard between two windows of the program reports a leave first. The leave waits a moment, so the
// arrival that follows can be read as the move it is: a popup that never activates does not take the focus from where it was.
void NativeWindows::settle_focus_leave(bool now)
{
	if (!focus_leaving || (!now && std::chrono::steady_clock::now()-focus_left<std::chrono::milliseconds(150))) return;
	focus_leaving=false;
	key_down={}; key_translation.reset();
	set_focus(0);
	activate(0);
}

void NativeWindows::receive(const DesktopEvent &event)
{
	if (placing_press && (event.kind==DesktopEventKind::pointer_motion || event.kind==DesktopEventKind::pointer_button ||
		event.kind==DesktopEventKind::pointer_wheel || event.kind==DesktopEventKind::pointer_relative || event.kind==DesktopEventKind::key)) {
		held_input.push_back(event);
		return;
	}
	// A key means one of the program's windows still has the keyboard, so a pending leave did not happen after all.
	if (event.kind==DesktopEventKind::key) focus_leaving=false;
	else if (event.kind!=DesktopEventKind::keyboard_focus) settle_focus_leave(false);
	if (menu_event(event)) return;
	if (event.kind==DesktopEventKind::message) { post(event.window,event.message,event.wparam,event.lparam); return; }
	if (event.kind==DesktopEventKind::keymap) { keyboard_map.load(event.keymap); key_translation.reset(); return; }
	if (event.kind==DesktopEventKind::modifiers) {
		keyboard_map.modifiers(event.message,uint32_t(event.wparam),uint32_t(event.lparam),uint32_t(event.lparam>>32));
		// A virtual keyboard reports modifiers without key events; the guest still sees them go down and up.
		if (focus && objects.window(focus) && messages.size()<9990) {
			static constexpr uint32_t scans[3]{0x2a,0x1d,0x38};
			for (uint32_t index=0;index<3;++index) {
				const uint32_t code=0x10+index;
				const bool active=keyboard_map.modifier_active(code),down=(vk_async[code] & 0x80)!=0;
				if (active==down) continue;
				const bool system=code==0x12 || (vk_async[0x12] & 0x80);
				vk_async[code]=active ? (vk_async[code] | 0x80) : (vk_async[code] & 0x7f);
				queue({focus,code,1 | (uint64_t(scans[index])<<16) | (uint64_t(!active)<<30) | (uint64_t(!active)<<31) | (code==0x12 ? uint64_t(1)<<29 : 0),
					active ? (system ? 0x104U : 0x100U) : (system ? 0x105U : 0x101U),message_time(),cursor_x,cursor_y,0},1);
			}
		}
		return;
	}
	if (event.kind==DesktopEventKind::keyboard_focus) {
		std::array<bool,256> initial{};
		for (const auto code : event.held_keys) {
			if (code>=initial.size() || initial[code]) throw std::runtime_error("invalid initial keyboard key state");
			initial[code]=true;
		}
		if (!event.window && !event.held_keys.empty())
			throw std::runtime_error("keyboard focus requires its visible guest window");
		// The keyboard moving between two windows of the program (a leave of one came just before) can carry a key pressed while it moved:
		// the compositor reports it as held on arrival and never sends its press. That press is the program's, so it is delivered.
		const bool moving=focus_leaving;
		const auto before=key_down;
		const auto deliver_moving_keys=[&] {
			if (!moving || !focus || !objects.window(focus)) return;
			for (size_t code=0;code<initial.size();++code)
				if (initial[code] && !before[code]) {
					key_down[code]=false;
					receive({event.window ? event.window : focus,uint32_t(code),1,0,DesktopEventKind::key});
				}
		};
		// The host can give the focus to a window the program hides or destroys before the event is read; there is nothing to focus then.
		if (event.window && (!objects.window(event.window) || !objects.window(event.window)->visible)) {
			// A popup gone before the compositor focused it does not take the focus either: the program's focus stays.
			if (focus && objects.window(focus) && objects.window_visible(focus)) {
				focus_leaving=false;
				key_down=initial; key_translation.reset();
				deliver_moving_keys();
			} else {
				settle_focus_leave(true);
			}
			return;
		}
		if (!event.window && event.held_keys.empty() && focus) {
			// The keys the program saw pressed stay recorded until the leave turns out to be real.
			focus_leaving=true;
			focus_left=std::chrono::steady_clock::now();
			return;
		}
		// Windows never gives the keyboard to a window that does not activate (WS_EX_NOACTIVATE) or to an owned popup without a caption
		// (a completion list, a menu, a dropdown): the compositor focusing one as it appears leaves the program's focus where it is.
		if (event.window && focus && objects.window(focus) && objects.window_visible(focus)) {
			const auto *target=objects.window(event.window);
			const bool passive=(target->ex_style & 0x08000000U) || ((target->style & 0x80000000U) && target->owner && (target->style & 0x00c00000U)!=0x00c00000U);
			if (passive && objects.window_root(focus)!=event.window) {
				focus_leaving=false;
				key_down=initial; key_translation.reset();
				deliver_moving_keys();
				return;
			}
		}
		focus_leaving=false;
		key_down=initial; key_translation.reset();
		if (!event.window) { set_focus(0); activate(0); }
		else if (!focus || objects.window_root(focus)!=event.window) set_focus(event.window);
		deliver_moving_keys();
		return;
	}
	if (event.kind==DesktopEventKind::key) {
		// Keys go to the focus window whichever of the program's host windows the compositor reports them on. The compositor sends keys
		// only to the program that has the keyboard, so a key that comes without a window (the keyboard appeared after the compositor
		// focused the window and no enter followed) is the program's too. With no focus left (a dialog ended by the previous key), the
		// key goes nowhere.
		if (!focus || !objects.window(focus) || !objects.window_visible(focus)) {
			if (event.message<key_down.size()) key_down[event.message]=event.wparam!=0;
			return;
		}
		if (event.wparam>1 || event.message>=key_down.size() || messages.size()>=10000)
			throw std::runtime_error("invalid native keyboard event or queue exhaustion");
		const bool pressed=event.wparam!=0,previous=key_down[event.message];
		if (!pressed && !previous) return;
		const auto key=pressed || !key_pressed[event.message].code ? keyboard_map.decode(event.message) : key_pressed[event.message];
		key_down[event.message]=pressed;
		key_pressed[event.message]=pressed ? key : KeyboardKey{};
		if (!key.code) return;
		const bool modifier=key.code>=0x10 && key.code<=0x12;
		// A modifier already reported through the keymap state is not announced twice.
		if (modifier && ((vk_async[key.code] & 0x80)!=0)==pressed) return;
		const bool system=key.code==0x12 || (vk_async[0x12] & 0x80);
		const uint64_t flags=1 | (uint64_t(key.scan)<<16) | (uint64_t(key.extended)<<24) | (system && key.code!=0x12 ? uint64_t(1)<<29 : 0) |
			(key.code==0x12 ? uint64_t(1)<<29 : 0) | (uint64_t(previous)<<30) | (uint64_t(!pressed)<<31);
		queue({focus,key.code,flags,system ? (pressed ? 0x104U : 0x105U) : (pressed ? 0x100U : 0x101U),message_time(),cursor_x,cursor_y,pressed ? key.character : 0},1);
		vk_async[key.code&255]=pressed ? (vk_async[key.code&255] | 0x80) : (vk_async[key.code&255] & 0x7f);
		if (pressed && !previous && (key.code==0x14 || key.code==0x90 || key.code==0x91)) vk_async[key.code]^=1;
		return;
	}
	if (event.kind==DesktopEventKind::resize) {
		// The host's configured size applies to resizable windows and to a requested maximization.
		const auto *window=objects.window(event.window);
		if (!window || window->parent || !(window->style & (0x40000U | 0x01000000U)) || event.x<=0 || event.y<=0) return;
		const DesktopRect outer{window->outer.left,window->outer.top,window->outer.left+event.x,window->outer.top+event.y};
		if (outer.right-outer.left==window->outer.right-window->outer.left && outer.bottom-outer.top==window->outer.bottom-window->outer.top) return;
		// The compositor already resized the host window, so it stays and receives the next frame at the new size.
		keep_host_window=true;
		try { place_window(event.window,outer,0,0x14); } catch (...) { keep_host_window=false; throw; }
		keep_host_window=false;
		return;
	}
	if (event.kind==DesktopEventKind::pointer_relative) {
		// The mouse's own movement: once the host reports it, raw input uses it rather than the change of the pointer position.
		raw_mouse.relative=true;
		if (raw_mouse.registered) raw_mouse_input(event.x,event.y,0,0);
		return;
	}
	if (event.kind==DesktopEventKind::pointer_wheel) {
		// The wheel goes to the focus window with the screen position of the pointer.
		const auto *source=objects.window(event.window);
		if (!source || !focus || !objects.window(focus) || messages.size()>=10000) return;
		const auto x=int64_t(source->outer.left)+event.x,y=int64_t(source->outer.top)+event.y;
		if (x<INT16_MIN || x>INT16_MAX || y<INT16_MIN || y>INT16_MAX) return;
		const auto delta=uint64_t(uint16_t(int16_t(int32_t(event.message))));
		if (raw_mouse.registered) raw_mouse_input(0,0,event.wparam ? 0x800 : 0x400,int16_t(int32_t(event.message)));
		queue({focus,(delta<<16)|mouse_buttons|keyboard_map.mouse_modifiers(),uint16_t(x)|(uint64_t(uint16_t(y))<<16),event.wparam ? 0x20eU : 0x20aU,message_time(),int32_t(x),int32_t(y)},4);
		return;
	}
	if (event.x<INT16_MIN || event.x>INT16_MAX || event.y<INT16_MIN || event.y>INT16_MAX)
		throw std::runtime_error("pointer coordinates exceed the guest message contract");
	uint32_t message=0x200,buttons=mouse_buttons;
	if (event.kind==DesktopEventKind::pointer_button) {
		uint32_t mask=0;
		if (event.message==0x110) { mask=1; message=0x201; }
		else if (event.message==0x111) { mask=2; message=0x204; }
		else if (event.message==0x112) { mask=16; message=0x207; }
		else throw std::runtime_error("unsupported pointer button");
		if (event.wparam>1) throw std::runtime_error("invalid pointer button state");
		if (event.wparam) buttons|=mask; else { buttons&=~mask; ++message; }
	}
	static const bool trace_buttons=std::getenv("RAVEN_TRACE_MSG")!=nullptr;
	if (trace_buttons && event.kind==DesktopEventKind::pointer_button)
		fprintf(stderr,"button %x on %llx at %d,%d capture %llx\n",message,(unsigned long long)event.window,event.x,event.y,(unsigned long long)capture);
	if (!event.window && event.kind==DesktopEventKind::pointer_button) { mouse_buttons=buttons; mouse_keys(vk_async,buttons); return; }
	const auto *window=objects.window(event.window);
	if (!window || !window->visible) throw std::runtime_error("pointer event targets an unavailable guest window");
	if (messages.size()>=10000) throw std::runtime_error("guest input queue limit reached");
	const auto x=int64_t(window->outer.left)+event.x,y=int64_t(window->outer.top)+event.y;
	if (x<INT32_MIN || x>INT32_MAX || y<INT32_MIN || y>INT32_MAX)
		throw std::runtime_error("guest cursor coordinates overflow");
	if (raw_mouse.registered) {
		// The raw report carries the movement of the host pointer and the buttons that changed: down and up flags for left, right, middle.
		const int64_t dx=raw_mouse.have_position && !raw_mouse.relative ? x-raw_mouse.x : 0,dy=raw_mouse.have_position && !raw_mouse.relative ? y-raw_mouse.y : 0;
		raw_mouse.x=x; raw_mouse.y=y; raw_mouse.have_position=true;
		uint32_t button_flags=0;
		const uint32_t changed=buttons^mouse_buttons;
		if (changed & 1) button_flags|=(buttons & 1) ? 0x1 : 0x2;
		if (changed & 2) button_flags|=(buttons & 2) ? 0x4 : 0x8;
		if (changed & 16) button_flags|=(buttons & 16) ? 0x10 : 0x20;
		if (dx || dy || button_flags) raw_mouse_input(dx,dy,button_flags,0);
	}
	mouse_buttons=buttons; cursor_x=int32_t(x); cursor_y=int32_t(y);
	mouse_keys(vk_async,buttons);
	if (raw_mouse.registered && raw_mouse.no_legacy) return;
	if (frame_drag.window) {
		// The drag of a tool window's caption follows the pointer until the button goes up, wherever the pointer is over the program.
		if (!objects.window(frame_drag.window) || !(buttons & 1)) {
			frame_drag.window=0;
		} else if (message==0x200) {
			const int32_t dx=int32_t(x)-frame_drag.start_x,dy=int32_t(y)-frame_drag.start_y;
			const auto &origin=frame_drag.origin;
			place_window(frame_drag.window,{origin.left+dx,origin.top+dy,origin.right+dx,origin.bottom+dy},0,0x1 | 0x4 | 0x10);
			return;
		}
	}
	if (scroll_drag.window && (message==0x200 || message==0x202 || message==0x201) && scroll_motion(message,int32_t(x),int32_t(y))) return;
	if (capture && !objects.window(capture)) capture=0;
	// The press is placed before any input read while the program handles the messages sent for it.
	const bool press=message==0x201 || message==0x204 || message==0x207;
	if (press && !placing_press) {
		placing_press=true;
		try {
			place_pointer(event,message,buttons,x,y);
		} catch (...) {
			placing_press=false;
			held_input.clear();
			throw;
		}
		release_held_input();
		return;
	}
	place_pointer(event,message,buttons,x,y);
}

static bool trace_buttons_enabled()
{
	static const bool enabled=std::getenv("RAVEN_TRACE_MSG")!=nullptr;
	return enabled;
}

uint64_t NativeWindows::host_owner(const DesktopWindow &window) const
{
	return window.owner ? objects.window_root(window.owner) : window.shown_on ? objects.window_root(window.shown_on) : 0;
}

bool NativeWindows::shown_on_owner(const DesktopWindow &window) const
{
	const auto owner=host_owner(window);
	if (!owner || window.parent || !objects.window_visible(owner))
		return false;
	return ((window.style & 0x80000000U) && (window.style & 0x00c00000U)!=0x00c00000U) || (window.ex_style & 0x80U);
}

void NativeWindows::release_held_input()
{
	placing_press=false;
	while (!held_input.empty()) {
		const auto next=held_input.front();
		held_input.pop_front();
		receive(next);
	}
}

void NativeWindows::place_pointer(const DesktopEvent &event,uint32_t message,uint32_t buttons,int64_t x,int64_t y)
{
	const auto target=capture ? capture : pointer_child(event.window,int32_t(x),int32_t(y));auto *window=objects.window(target);
	if (!window || !objects.window_visible(target)) return;
	track_pointer(target);
	const bool inset=window->x!=window->outer.left || window->y!=window->outer.top ||
		int64_t(window->width)!=int64_t(window->outer.right)-window->outer.left ||
		int64_t(window->height)!=int64_t(window->outer.bottom)-window->outer.top;
	const bool inside=x>=window->outer.left && y>=window->outer.top && x<window->outer.right && y<window->outer.bottom;
	const auto hit=!capture && inset && inside ? send(target,0x84,0,uint16_t(x)|(uint64_t(uint16_t(y))<<16)) : 1;
	window=objects.window(target);
	if (trace_buttons_enabled() && message!=0x200)
		fprintf(stderr,"button %x target %llx hit %llx visible %d\n",message,(unsigned long long)target,(unsigned long long)hit,
			window && objects.window_visible(target) ? 1 : 0);
	if (!window || !objects.window_visible(target) || !hit) return;
	// Unless the mouse is captured, the window under the pointer chooses the cursor before the mouse message arrives.
	if (!capture && (message==0x200 || message==0x201 || message==0x204 || message==0x207)) {
		send(target,0x20,target,uint16_t(hit)|(uint64_t(message)<<16));
		window=objects.window(target);
		if (!window || !objects.window_visible(target)) return;
	}
	// Scroll bars and the frame may run a loop of their own that waits for the release, so what was held goes in first.
	if (hit!=1 && placing_press) release_held_input();
	if ((hit==6 || hit==7) && scroll_pointer(target,hit,message,int32_t(x),int32_t(y))) return;
	if (hit!=1) { nonclient_pointer(target,hit,message,int32_t(event.x),int32_t(event.y)); return; }
	// A second press of the same button on the same window within the double-click time (500 ms) and rectangle (SM_CXDOUBLECLK, 4 by 4)
	// is a double click when the window's class asks for them (CS_DBLCLKS); the press after a double click starts over.
	if (message==0x201 || message==0x204 || message==0x207) {
		const auto now=message_time();
		const auto *type=objects.window_class(window->class_id);
		const bool again=last_press.window==target && last_press.message==message && now-last_press.time<=500 &&
			std::abs(int64_t(x)-last_press.x)<=2 && std::abs(int64_t(y)-last_press.y)<=2;
		if (again && type && (type->style & 8)) { message+=2; last_press={}; }
		else last_press={target,message,now,int32_t(x),int32_t(y)};
	}
	if ((message==0x201 || message==0x204 || message==0x207) && window->parent && !(window->ex_style & 4)) {
		for (auto parent=window->parent;parent;) {
			const auto *ancestor=objects.window(parent);if (!ancestor) break;
			const auto next=ancestor->parent;
			send(parent,0x210,message,uint16_t(x-ancestor->x)|(uint64_t(uint16_t(y-ancestor->y))<<16));
			parent=next;
		}
		window=objects.window(target);if (!window || !objects.window_visible(target)) return;
	}
	if (messages.size()>=10000) throw std::runtime_error("guest input queue limit reached");
	const Message input{target,buttons|keyboard_map.mouse_modifiers(),uint16_t(x-window->x)|(uint64_t(uint16_t(y-window->y))<<16),message,message_time(),
		int32_t(x),int32_t(y)};
	// As on Windows, a move the program has not retrieved yet is replaced by the newer one rather than queued after it: a program slow
	// to handle each move (a paint program drawing along the pointer) would otherwise fall ever further behind and never reach WM_PAINT.
	if (message==0x200 && !messages.empty() && messages.back().message==0x200 && messages.back().window==target &&
		messages.back().wparam==input.wparam) {
		const auto thread=messages.back().thread;
		messages.back()=input;
		messages.back().thread=thread;
		messages.back().kinds=2;
		queues[thread].changed|=2;
		return;
	}
	queue(input,message==0x200 || message==0xa0 ? 2 : 4);
}

NativeWindows::Modifiers NativeWindows::modifier_state() const
{
	return Modifiers{(vk_state[0x10] & 0x80)!=0,(vk_state[0x11] & 0x80)!=0,(vk_state[0x12] & 0x80)!=0};
}

uint64_t NativeWindows::retrieve(std::span<const uint64_t> args,bool blocking)
{
	const auto layout=message_layout(process.mode()==NativeMode::x86);
	// A 32-bit (HWND)-1 selects thread messages exactly like its 64-bit sign extension.
	const auto window=process.mode()==NativeMode::x86 && args[1]==UINT32_MAX ? UINT64_MAX : args[1];
	const auto minimum=uint32_t(args[2]),maximum=uint32_t(args[3]);
	if (window && window!=UINT64_MAX && !objects.window(window)) throw uint32_t(1400);
	if ((minimum || maximum) && (minimum>maximum || maximum>0xffff)) throw uint32_t(87);
	const auto flags=blocking ? 1U : uint32_t(args[4]);
	if (flags & ~0x1dff0003U) throw uint32_t(87);
	const auto kinds=flags>>16 ? flags>>16 : 0x1dffU;
	const auto seen=(kinds | 8U) & ((minimum || maximum || (flags>>16)) ? ~0x100U : UINT32_MAX);
	flush_dirty();
	const auto me=environment.thread_id();
	const auto matches=[&](const Message &message) {
		return (!message.thread || message.thread==me) && (message.kinds & kinds) &&
			(!window || (window==UINT64_MAX ? !message.window : message.window==window || objects.is_child(window,message.window))) &&
			(message.message==0x12 || (!minimum && !maximum) || (message.message>=minimum && message.message<=maximum));
	};
	for (;;) {
		deliver_pending();
		if (presentation.poll)
			for (const auto &event : presentation.poll(std::chrono::milliseconds(0)))
				receive(event);
		// Messages sent from other threads run before anything is retrieved, as they do while a Windows thread pumps its queue.
		if (kinds & 0x40) serve_sent();
		auto found=std::find_if(messages.begin(),messages.end(),matches);
		const auto quit=(kinds & 0x108) ? quits.find(me) : quits.end();
		if (found!=messages.end() || quit!=quits.end()) {
			const auto value=found!=messages.end() ? *found : Message{0,quit->second,0,0x12,message_time(),cursor_x,cursor_y};
			std::vector<uint8_t> bytes(layout.size);
			put(bytes,0,value.window,layout.pointer); put(bytes,layout.message,value.message,4);
			put(bytes,layout.wparam,value.wparam,layout.pointer); put(bytes,layout.lparam,value.lparam,layout.pointer);
			put(bytes,layout.time,value.time,4);
			put(bytes,layout.x,uint32_t(value.x),4); put(bytes,layout.y,uint32_t(value.y),4);
			process.write_memory(args[0],bytes);
			// RAVEN_TRACE_MSG also follows what each retrieval returns, with the filter it was asked with.
			static const char *const trace_retrieval=std::getenv("RAVEN_TRACE_MSG");
			if (trace_retrieval)
				fprintf(stderr,"get %llx %x filter %llx %x-%x %s\n",(unsigned long long)value.window,value.message,(unsigned long long)window,minimum,maximum,
					(flags & 1U) ? "remove" : "peek");
			if (value.message>=0x100 && value.message<=0x105 && value.message!=0x102 && value.message!=0x103) {
				key_translation=value;
				// Key state seen by GetKeyState follows the messages the thread has retrieved, not the physical keys.
				if (flags & 1U) {
					const auto code=uint32_t(value.wparam & 255);
					const bool down=value.message==0x100 || value.message==0x104;
					const bool repeat=(value.lparam>>30) & 1;
					vk_state[code]=down ? (vk_state[code] | 0x80) : (vk_state[code] & 0x7f);
					if (down && !repeat && (code==0x14 || code==0x90 || code==0x91)) vk_state[code]^=1;
				}
			}
			// The buttons seen by GetKeyState likewise follow the pointer messages retrieved.
			if ((flags & 1U) && ((value.message>=0x200 && value.message<=0x20e) || (value.message>=0x2a0 && value.message<=0x2a3)))
				mouse_keys(vk_state,value.wparam);
			if (flags & 1U) { if (found!=messages.end()) messages.erase(found); else quits.erase(me); }
			if (value.message!=0x12) {
				// Keyboard and mouse hooks may swallow the message; the get-message hook only observes it.
				if (has_hook(2) && (value.message>=0x100 && value.message<=0x105) && (flags & 1U) &&
					call_hooks(2,0,value.wparam,value.lparam)) continue;
				if (has_hook(7) && value.message>=0x200 && value.message<=0x20e && (flags & 1U)) {
					std::vector<uint8_t> hook(32);
					put(hook,0,uint32_t(value.x),4); put(hook,4,uint32_t(value.y),4); put(hook,8,value.window,8); put(hook,16,1,4);
					const auto at=scratch_allocate(hook.size()); process.write_memory(at,hook);
					if (call_hooks(7,0,value.message,at)) continue;
				}
				if (has_hook(3)) call_hooks(3,0,(flags & 1U) ? 1 : 0,args[0]);
			}
			queue_status(me,seen,true);
			return blocking && value.message==0x12 ? 0 : 1;
		}
		// WM_TIMER ranks below input and paint, so it is only generated when neither is waiting.
		if (found==messages.end() && quit==quits.end()) {
			const auto before=messages.size();
			// A paint waiting holds the timers back only from a retrieval that would take it.
			const bool paint_eligible=(kinds & 0x20) && ((!minimum && !maximum) || (minimum<=0x0f && maximum>=0x0f));
			if ((kinds & 0x10) && (!paint_eligible || !invalid_owned(window))) post_due_timers();
			if (messages.size()!=before) continue;
		}
		const auto paint=(kinds & 0x20) && ((!minimum && !maximum) || (minimum<=0x0f && maximum>=0x0f)) ? invalid_owned(window) : 0;
		if (paint) {
			if (std::getenv("RAVEN_TRACE_PAINT")) {
				std::cerr << "paint " << std::hex << paint << " pending";
				for (const auto handle : objects.window_handles())
					if (const auto *other = objects.window(handle); other && !other->update.empty())
						std::cerr << ' ' << handle << (objects.window_visible(handle) ? "" : "(hidden)");
				std::cerr << std::dec << "\n";
			}
			paint_cursor=paint;
			std::vector<uint8_t> bytes(layout.size);
			put(bytes,0,paint,layout.pointer); put(bytes,layout.message,0x0f,4); put(bytes,layout.time,message_time(),4);
			process.write_memory(args[0],bytes);
			queue_status(me,seen,true);
			return 1;
		}
		if (!blocking) { queue_status(me,seen,true); return 0; }
		if (std::chrono::steady_clock::now()>=deadline) throw NativeResult{NativeStop::timeout,0,0,0};
		// RAVEN_DUMP_WINDOWS lists the windows once, after nine seconds or after as many seconds as its value names.
		if (const char *dump=std::getenv("RAVEN_DUMP_WINDOWS"); dump && !dumped && std::chrono::steady_clock::now()-started>std::chrono::seconds(std::max(9,std::atoi(dump)))) { dumped=true; dump_windows(); }
		caret_idle();
		scroll_idle();
		flush_dirty();
		deliver_pending();
		if (message_wait) {
			auto until=std::min(deadline,std::chrono::steady_clock::now()+std::chrono::milliseconds(20));
			for (const auto &timer : timers) until=std::min(until,timer.due);
			if (caret && caret->hidden<=0 && !caret->blank) until=std::min(until,caret->next);
			if (scroll_drag.window) until=std::min(until,scroll_drag.next);
			if (std::any_of(foreign_frames.begin(),foreign_frames.end(),[](const auto &frame) { return frame.second!=0; }))
				until=std::min(until,foreign_checked+std::chrono::milliseconds(8));
			message_wait([&] {
				// Collect host input without invoking a guest procedure while the scheduler checks another thread.
				queue_status(me,0,false);
				return !pending_events.empty() || ((kinds & 0x40) && sent_for(me)) ||
					((kinds & 0x108) && quits.contains(me)) || std::any_of(messages.begin(),messages.end(),matches) ||
					((kinds & 0x20) && ((!minimum && !maximum) || (minimum<=0x0f && maximum>=0x0f)) && invalid_owned(window,me));
			},until);
			continue;
		}
		if (presentation.poll && presentation.descriptor && idle_until && presentation.descriptor()>=0) {
			wait_idle(deadline);
		} else if (presentation.poll) {
			const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now());
			for (const auto &event : presentation.poll(std::min(remaining,std::chrono::milliseconds(20))))
				receive(event);
		} else std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void NativeWindows::wait_idle(std::chrono::steady_clock::time_point deadline)
{
	const auto now=std::chrono::steady_clock::now();
	auto until=std::min(deadline,now+std::chrono::seconds(1));
	for (const auto &timer : timers) until=std::min(until,timer.due);
	if (caret && caret->hidden<=0 && !caret->blank) until=std::min(until,caret->next);
	if (scroll_drag.window) until=std::min(until,scroll_drag.next);
	// Windows another process draws into are looked at as often as before; one starting to draw shows in the windows directory.
	if (std::any_of(foreign_frames.begin(),foreign_frames.end(),[](const auto &frame) { return frame.second!=0; }))
		until=std::min(until,foreign_checked+std::chrono::milliseconds(8));
	if (!pending_events.empty() || !dirty_roots.empty()) until=now;
	until=idle_until(until);
	if (until>now) {
		std::vector<pollfd> descriptors{{presentation.descriptor(),POLLIN,0},{foreign_frames_descriptor(),POLLIN,0}};
		idle_descriptors(descriptors);
		const auto left=std::chrono::duration_cast<std::chrono::nanoseconds>(until-now).count();
		const timespec timeout{left/1000000000,left%1000000000};
		::ppoll(descriptors.data(),descriptors.size(),&timeout,nullptr);
		idle_drain();
		if (descriptors[1].revents & POLLIN) {
			drain_foreign_frames_descriptor();
			foreign_checked={};
		}
	}
	for (const auto &event : presentation.poll(std::chrono::milliseconds(0)))
		receive(event);
}

std::string NativeWindows::text(uint64_t address)
{
	std::string result;
	for (size_t index=0; index<256; ++index) {
		const auto bytes=process.read_memory(address+index*2,2);
		const auto character=read(bytes,0,2);
		if (!character) return result;
		if (character>127) throw uint32_t(120);
		result.push_back(char(character));
	}
	throw uint32_t(87);
}

std::u16string NativeWindows::window_text(uint64_t address)
{
	std::u16string result;
	if (!address) return result;
	for (size_t index=0;index<=32766;++index) {
		if (address>UINT64_MAX-index*2) throw uint32_t(87);
		const auto character=char16_t(read(process.read_memory(address+index*2,2),0,2));
		if (!character) return result;
		if (index==32766) throw uint32_t(120);
		result.push_back(character);
	}
	throw uint32_t(120);
}

uint64_t NativeWindows::copy_window_text(uint64_t handle,uint64_t output,uint64_t capacity)
{
	const auto *window=objects.window(handle);if (!window) throw uint32_t(1400);
	if (!capacity) return 0;
	const auto length=std::min(uint64_t(window->title.size()),capacity-1);
	std::vector<uint8_t> bytes(size_t(length+1)*2);
	for (size_t index=0;index<length;++index) put(bytes,index*2,window->title[index],2);
	process.write_memory(output,bytes);return length;
}

uint16_t NativeWindows::atom(uint64_t name, uint64_t instance)
{
	// The predefined system class atoms 0x8000..0x8004 name the classes "#32768".."#32772".
	if (name>=0x8000 && name<=0x8004) return objects.find_class(instance,"#"+std::to_string(name));
	if (name && name<=UINT16_MAX) {
		return objects.find_class(instance,uint16_t(name));
	}
	auto value=text(name);
	// A class an assembly of the thread's activation context declares is first looked for under its versioned name.
	auto &contexts=activation_contexts();
	if (contexts.current_thread)
		if (const auto versioned=contexts.lookup_name(contexts.current_thread(),value))
			if (const auto found=objects.find_class(instance,*versioned)) return found;
	for (auto &character : value)
		if (character>='A' && character<='Z') character=char(character+('a'-'A'));
	return objects.find_class(instance,value);
}

uint64_t NativeWindows::send(uint64_t handle, uint32_t message, uint64_t wparam, uint64_t lparam)
{
	const auto *window=objects.window(handle);
	if (!window) throw uint32_t(1400);
	if (!invoke) throw std::runtime_error("native window callbacks are not connected");
	// A window of another thread runs its procedure on that thread: the message waits in its queue of sent messages, and this thread
	// answers the messages sent to its own windows while it waits for the result.
	if (const auto me=environment.thread_id(); window->thread && me && window->thread!=me && block_hook) {
		auto item=std::make_shared<SentMessage>(SentMessage{handle,message,wparam,lparam,0,window->thread});
		sent.push_back(item);
		queues[item->thread].changed|=0x40;
		while (!item->done) {
			serve_sent();
			if (item->done) break;
			block_hook([&] { return item->done || sent_for(me); });
		}
		return item->result;
	}
	const auto procedure=window->procedure;
	const uint64_t arguments[]{handle,message,wparam,lparam};
	// The desktop window belongs to the system and has no guest procedure.
	if (!procedure) return default_message(arguments);
	// RAVEN_TRACE_MSG follows every message delivered to a guest procedure; a value limits it to one window handle.
	static const char *const trace_message=std::getenv("RAVEN_TRACE_MSG");
	const bool traced=trace_message && (std::string_view(trace_message)=="1" || std::strtoull(trace_message,nullptr,16)==handle);
	if (traced) fprintf(stderr,"msg %llx %x %llx %llx\n",(unsigned long long)handle,message,(unsigned long long)wparam,(unsigned long long)lparam);
	const auto result=invoke(procedure,arguments);
	if (result.stop!=NativeStop::returned) throw result;
	if (traced) fprintf(stderr,"msg %llx %x -> %llx\n",(unsigned long long)handle,message,(unsigned long long)result.value);
	return result.value;
}

void NativeWindows::activate(uint64_t handle)
{
	if (handle) handle=objects.window_root(handle);
	// A window that is activated while another activation is being delivered takes its turn when that one is done.
	if (activating) { queued_activation=handle; has_queued_activation=true; return; }
	for (;;) {
		if (active!=handle) {
			activating=true;
			try {
				const auto previous=active;
				if (previous && objects.window(previous)) {
					send(previous,0x86,0,handle);
					send(previous,6,0,handle);
				}
				if (handle && !objects.window(handle)) handle=0;
				active=handle;
				if (handle) { send(handle,0x86,1,previous); send(handle,6,1,previous); }
				activating=false;
			} catch (...) { activating=false; has_queued_activation=false; throw; }
		}
		if (!has_queued_activation) return;
		handle=queued_activation;
		has_queued_activation=false;
		if (handle && !objects.window(handle)) handle=0;
	}
}

uint64_t NativeWindows::set_focus(uint64_t handle)
{
	if (handle && (!objects.window(handle) || objects.window(handle)->destroying)) throw uint32_t(1400);
	if (handle) activate(handle);
	const auto previous=focus;
	if (previous==handle) return previous;
	// A handler may move the focus again; the nested change completes first and the outer one stops when it was superseded.
	++focusing;
	try {
		focus=handle;
		if (previous && objects.window(previous)) send(previous,8,handle,0);
		if (handle && focus==handle && objects.window(handle)) send(handle,7,previous,0);
		--focusing;
		return previous;
	} catch (...) { --focusing; throw; }
}

bool NativeWindows::destroy(uint64_t handle)
{
	auto *window=objects.window(handle);
	if (!window || window->destroying) throw uint32_t(5);
	for (const auto owned:objects.window_handles())
		if (objects.is_child(handle,owned) && objects.window(owned)->destroying) throw uint32_t(120);
	// HCBT_DESTROYWND comes first and may veto the destruction.
	if (has_hook(5) && call_hooks(5,4,handle,0)) return false;
	window=objects.window(handle);
	if (!window || window->destroying) throw uint32_t(5);
	window->destroying=true;window->visible=false;window->style&=~0x10000000U;
	withdraw_window(handle);
	if (active==handle) activate(0);
	if (focus==handle || objects.is_child(handle,focus)) set_focus(0);
	parent_notify(handle,2);
	send(handle,2,0,0);
	for (const auto child:objects.child_windows(handle)) if (objects.window(child)) destroy(child);
	if (objects.window(handle)) send(handle,0x82,0,0);
	window=objects.window(handle);
	if (window && window->parent) {
		const auto *parent=objects.window(window->parent);
		if (parent && !parent->destroying) {
			const auto relative=[](int64_t value) { return int32_t(std::clamp(value,int64_t(INT32_MIN),int64_t(INT32_MAX))); };
			if (!objects.invalidate(window->parent,{relative(int64_t(window->outer.left)-parent->x),relative(int64_t(window->outer.top)-parent->y),
				relative(int64_t(window->outer.right)-parent->x),relative(int64_t(window->outer.bottom)-parent->y)},true)) throw uint32_t(8);
		}
	}
	// A driver may still hold the surface of a window going away, which then only stops showing.
	if (hosted_windows.contains(handle) && presentation.host_surface) {
		place_hosted(handle);
		hosted_windows.erase(handle);
	}
	release_surface(handle);
	painted_frames.erase(handle);
	// The window menu goes with its window.
	if (const auto menu=system_menus.find(handle); menu!=system_menus.end()) {
		if (objects.menu(menu->second)) objects.erase(menu->second);
		system_menus.erase(menu);
	}
	unthemed_windows.erase(handle);
	dark_windows.erase(handle);
	named_windows.erase(handle);
	dark_allowed_windows.erase(handle);
	foreign_content.erase(handle);
	foreign_black.erase(handle);
	return objects.erase(handle);
}

uint64_t NativeWindows::default_message(std::span<const uint64_t> args)
{
	auto *window=objects.window(args[0]);
	if (!window) throw uint32_t(1400);
	const auto message=uint32_t(args[1]);
	if (message==0x200 || message==0x201 || message==0x202 || message==0x204 || message==0x207 || message==0x208) return 0;
	// A child that does not answer passes the menu request up to its parent.
	if (message==0x7b) {
		if ((window->style & 0x40000000U) && window->parent) send(window->parent,0x7b,args[2],args[3]);
		return 0;
	}
	if (message==0x205) {
		const auto x=uint16_t(int64_t(window->x)+int16_t(uint16_t(args[3])));
		const auto y=uint16_t(int64_t(window->y)+int16_t(uint16_t(args[3]>>16)));
		send(args[0],0x7b,args[0],uint32_t(x)|(uint32_t(y)<<16));
		return 0;
	}
	if (message==0x210 && (uint16_t(args[2])==1 || uint16_t(args[2])==2 || uint16_t(args[2])==0x201 || uint16_t(args[2])==0x204 || uint16_t(args[2])==0x207)) return 0;
	if (message==0x86) return 1;
	// WM_NOTIFYFORMAT: a window of DefWindowProcW takes Unicode notifications (NFR_UNICODE); NF_REQUERY asks the parent again.
	if (message==0x55) {
		if (uint32_t(args[3])==3) return 2;
		if (uint32_t(args[3])==4 && window->parent) return send(window->parent,0x55,args[0],3);
		return 0;
	}
	if (message==6) { if (uint16_t(args[2]) && !uint16_t(args[2]>>16)) set_focus(args[0]); return 0; }
	if (message==7 || message==8) return 0;
	if (message==0x81 || message==0x0c) {
		const bool narrow=process.mode()==NativeMode::x86;
		const auto address=message==0x81 ? (narrow ? read(process.read_memory(args[3],48),36,4) :
			read(process.read_memory(args[3],80),56,8)) : args[3];
		if (!objects.set_title(args[0],window_text(address))) throw uint32_t(8);
		if (objects.window_visible(args[0])) present(args[0]);
		return 1;
	}
	if (message==0x0e) return window->title.size();
	if (message==0x0d) return copy_window_text(args[0],args[3],args[2]);
	if (message==0x10) { destroy(args[0]); return 0; }
	if (message>=0x132 && message<=0x138) {
		if (message==0x137) throw uint32_t(120);
		auto *dc=objects.dc(args[2]);if (!dc) throw uint32_t(6);
		const auto background=message==0x133 || message==0x134 ? 5 : 15;
		dc->text_color=objects.system_color(8);dc->background_color=objects.system_color(background);
		return objects.system_brush(background);
	}
	if (message==0x14) {
		const auto background=objects.window_class(window->class_id)->background;
		if (!background) return 0;
		const auto *dc=objects.dc(args[2]);
		const auto *bitmap=dc ? objects.bitmap(dc->bitmap) : nullptr;
		if (!bitmap) throw uint32_t(6);
		const auto target=dc->window; const bool leased=dc->leased;
		const auto changed=fill_rect(process,objects,args[2],{INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX},background);
		if (changed && target && leased) drawing(target);
		return 1;
	}
	if (message==0x85) {
		// The frame is repainted from window state each time the surface is presented; a child's waits for that, as its WM_NCPAINT
		// comes while it starts painting.
		if (args[0]==framing) framed_by_default=true;
		if (window->parent) drawing(args[0]);
		else if (objects.window_visible(args[0])) present(args[0]);
		return 0;
	}
	if (message==0x84 && !window->parent && (frame_layout(*window).border || frame_layout(*window).caption_height || window->menu))
		return nonclient_hit(*window,int16_t(uint16_t(args[3])),int16_t(uint16_t(args[3]>>16)));
	if (message==0x84) {
		const auto x=int16_t(uint16_t(args[3])),y=int16_t(uint16_t(args[3]>>16));
		if (const auto bar=scroll_hit(*window,x,y)) return bar;
		return x>=window->outer.left && y>=window->outer.top && x<window->outer.right && y<window->outer.bottom &&
			x>=window->x && y>=window->y && int64_t(x)<int64_t(window->x)+window->width &&
			int64_t(y)<int64_t(window->y)+window->height ? 1 : 0;
	}
	if (message==0x20) {
		// WM_SETCURSOR: the parent of a child chooses first. The client area shows the class cursor, the frame the cursor of its edge
		// and the rest the arrow.
		if ((window->style & 0x40000000U) && window->parent && objects.window(window->parent) && send(window->parent,0x20,args[2],args[3])) return 1;
		window=objects.window(args[0]);
		if (!window) return 0;
		const auto hit=int16_t(uint16_t(args[3]));
		uint64_t cursor=desktop_stock_cursor;
		if (hit==1) {
			const auto *definition=objects.window_class(window->class_id);
			if (!definition || !definition->cursor) return 0;
			cursor=definition->cursor;
		}
		else if (hit==10 || hit==11) cursor=desktop_stock_cursor+(32644-32512);
		else if (hit==12 || hit==15) cursor=desktop_stock_cursor+(32645-32512);
		else if (hit==13 || hit==17) cursor=desktop_stock_cursor+(32642-32512);
		else if (hit==14 || hit==16) cursor=desktop_stock_cursor+(32643-32512);
		current_cursor=cursor;
		show_cursor();
		return hit==1 || (hit>=10 && hit<=17);
	}
	if (message==0x317) {
		// WM_PRINT: the default procedure replays its flags as the erase and client-paint requests.
		if (args[3] & 0x8) send(args[0],0x14,args[2],0);
		if (objects.window(args[0]) && (args[3] & 0x4)) send(args[0],0x318,args[2],args[3]);
		return 0;
	}
	if (message==0x318) {
		// WM_PRINTCLIENT: the default procedure paints the window into the supplied device context.
		if (args[2]) send(args[0],0x0f,args[2],0);
		return 0;
	}
	if (message==0x0f && args[2]) return 0;
	if (message==0x0f) {
		if (window->painting) throw uint32_t(170);
		start_paint(args[0]); finish_paint(args[0]);
		if (objects.window(args[0])->visible) present(args[0]);
		return 0;
	}
	if (message==0x83) {
		// The extended form (NCCALCSIZE_PARAMS) starts with the proposed window rectangle, which becomes the client rectangle.
		auto rect=process.read_memory(args[3],16);
		// The client area is the window rectangle minus what AdjustWindowRectEx adds for this style.
		const auto frame=adjust_window_rect(objects.nonclient_metrics(),{0,0,0,0},window->style & ~0x10000000U,window->menu!=0,window->ex_style);
		const int32_t edges[4]={frame.left,frame.top,frame.right,frame.bottom};
		for (size_t index=0;index<4;++index)
			put(rect,index*4,uint32_t(int32_t(read(rect,index*4,4))-edges[index]),4);
		// Scroll bars sit between the edge and the client area on the right and bottom.
		if (window->style & 0x200000U) put(rect,8,uint32_t(int32_t(read(rect,8,4))-17),4);
		if (window->style & 0x100000U) put(rect,12,uint32_t(int32_t(read(rect,12,4))-17),4);
		process.write_memory(args[3],rect);
		return 0;
	}
	if (message==0x106) {
		// A system character reaches the menu through the top-level window, whichever child had the focus.
		const auto root=objects.window_root(args[0]);
		if (root) send(root,0x112,0xf100,args[2]);
		return 0;
	}
	if (message==0x112) {
		if ((uint32_t(args[2]) & 0xfff0U)==0xf100U && args[3]==' ' && !window->parent) {
			// SC_KEYMENU with a space is Alt+Space: the window menu opens under the caption icon.
			const auto layout=frame_layout(*window);
			system_menu_open(args[0],layout.icon.left,layout.caption.bottom+1);
			return 0;
		}
		if ((uint32_t(args[2]) & 0xfff0U)==0xf100U && window->menu) {
			// SC_KEYMENU: Alt plus a menu bar mnemonic opens that menu.
			if (const auto *bar=objects.menu(window->menu)) {
				const auto fold=[](uint64_t c) { return c>='A' && c<='Z' ? c+32 : c; };
				const auto wanted=fold(args[3]);
				for (size_t index=0;index<bar->items.size();++index) {
					const auto &text=bar->items[index].text;
					for (size_t at=0;at+1<text.size();++at)
						if (text[at]==u'&' && text[at+1]!=u'&') {
							if (fold(text[at+1])==wanted) { menu_open_bar(args[0],index); return 0; }
							break;
						}
				}
			}
			return 0;
		}
		// SC_CLOSE becomes WM_CLOSE; the other system commands need window state transitions that are not modeled.
		if ((uint32_t(args[2]) & 0xfff0U)==0xf060U) { send(args[0],0x10,0,0); return 0; }
		// Size commands change the window state: maximize, minimize and restore.
		if ((uint32_t(args[2]) & 0xfff0U)==0xf030U) { show_window(args[0],3); return 0; }
		if ((uint32_t(args[2]) & 0xfff0U)==0xf020U) { show_window(args[0],6); return 0; }
		if ((uint32_t(args[2]) & 0xfff0U)==0xf120U) { show_window(args[0],9); return 0; }
		// SC_MOVE and SC_SIZE hand the window to the compositor, which moves or sizes host windows itself.
		if (((uint32_t(args[2]) & 0xfff0U)==0xf010U || (uint32_t(args[2]) & 0xfff0U)==0xf000U) && presentation.interactive && !window->parent &&
			!(window->style & 0x01000000U)) {
			presentation.interactive(args[0],(uint32_t(args[2]) & 0xfff0U)==0xf010U ? 0 : 10);
			return 0;
		}
		return 0;
	}
	if (message==0x21) return 1;
	if (message==0x11) return 1;
	if (!message || message==1 || message==2 || message==3 || message==5 || message==0x82 || message==0x18) return 0;
	// Messages whose Windows default is "no action, result 0": activation notices, window-position notices,
	// non-client mouse notices, capture loss, keyboard and timer/command traffic nobody consumed.
	if (message==0x16 || message==0x1c || message==0x24 || message==0x46 || message==0x47 || message==0x4e ||
		message==0x7f || message==0x80 || (message>=0xa0 && message<=0xa9) || message==0x215 || (message>=0x100 && message<=0x109) ||
		message==0x111 || message==0x113 || (message>=0x116 && message<=0x120) || message==0x211 || message==0x212) return 0;
	// Every other message has no default action in DefWindowProc and answers zero.
	return 0;
}

uint64_t NativeWindows::create(std::span<const uint64_t> args)
{
	const auto style=uint32_t(args[3]),ex_style=uint32_t(args[0]);
	const bool child=(style & 0x40000000U)!=0;
	// A message-only window (parent HWND_MESSAGE) only receives messages: it is a top-level window that is never shown.
	const bool message_only=!child && (args[8]==UINT64_MAX-2 || args[8]==0xfffffffdULL);
	const uint64_t owner_handle=message_only ? 0 : args[8];
	const auto effective_style=message_only ? style & ~0x10000000U : style;
	// A visible window needs somewhere to appear; headless runs can still create hidden windows.
	if ((effective_style & 0x10000000U) && !presentation.present) throw uint32_t(120);
	const auto *parent=child ? objects.window(args[8]) : nullptr;
	if (child && (!parent || parent->destroying)) throw uint32_t(1400);
	// An owner of a top-level window must be a top-level window; message-only parents (HWND_MESSAGE) are not modelled.
	if (!child && owner_handle && !objects.window(owner_handle)) throw uint32_t(1400);
	const auto class_atom=atom(args[1],args[10]);
	const auto *definition=objects.window_class(class_atom);
	if (!definition) {
		if (std::getenv("RAVEN_TRACE_API")) {
			std::string name;
			try { if (args[1]>0xffff) { const auto text=process.read_memory(args[1],64); for (size_t i=0;i<32 && (text[i*2]|text[i*2+1]);++i) name.push_back(char(text[i*2])); } else name="#"+std::to_string(args[1]); } catch (...) {}
			std::cerr << "raven: window class not found: " << name << "\n";
		}
		throw uint32_t(1407);
	}
	auto x=int32_t(args[4]),y=int32_t(args[5]);
	auto width=int32_t(args[6]),height=int32_t(args[7]);
	// CW_USEDEFAULT (0x80000000): top-level windows get a cascading position and three quarters of the screen; children get zero.
	if (x==INT32_MIN || y==INT32_MIN || width==INT32_MIN || height==INT32_MIN) {
		const auto &screen=objects.nonclient_metrics();
		if (x==INT32_MIN) x=child ? 0 : 0;
		if (y==INT32_MIN) y=child ? 0 : 0;
		if (width==INT32_MIN) width=child ? 0 : screen.screen_x*3/4;
		if (height==INT32_MIN) height=child ? 0 : screen.screen_y*3/4;
	}
	if (width<0 || height<0 || width>8192 || height>8192 || int64_t(x)+width>INT32_MAX || int64_t(y)+height>INT32_MAX)
		throw uint32_t(87);
	// An overlapped window, and any top-level one with a sizing border, starts at least at the minimum tracking size (SM_CXMINTRACK and
	// SM_CYMINTRACK), as CreateWindowEx makes it: programs that create a window empty and size it later measure its frame from it.
	if (!child && !message_only && (!(style & 0x80000000U) || (style & 0x40000U))) {
		width=std::max(width,136);
		height=std::max(height,39);
	}
	const auto relative_x=x,relative_y=y;
	if (parent) {
		const auto absolute_x=int64_t(parent->x)+x,absolute_y=int64_t(parent->y)+y;
		if (absolute_x<INT32_MIN || absolute_y<INT32_MIN || absolute_x+width>INT32_MAX || absolute_y+height>INT32_MAX) throw uint32_t(87);
		x=int32_t(absolute_x);y=int32_t(absolute_y);
	}
	if (args[2]) window_text(args[2]);
	DesktopWindow record{class_atom,definition->procedure,{},x,y,width,height};
	record.thread=environment.thread_id();
	record.instance=args[10]; record.style=effective_style | (child ? 0U : 0x04000000U);record.ex_style=ex_style;
	record.parent=child ? args[8] : 0;record.child_id=child ? args[9] : 0;record.client_ready=false;
	record.owner=child ? 0 : owner_handle;
	if (!child) record.menu=args[9];
	// A window created without a menu takes the default menu of its class.
	if (!child && !args[9] && definition->menu_id) {
		if (!menu_loader) throw uint32_t(120);
		record.menu=menu_loader(definition->instance,definition->menu_id);
		if (std::getenv("RAVEN_TRACE_API")) std::cerr << "class menu " << definition->menu_id << " instance " << std::hex << definition->instance << " -> " << record.menu << std::dec << "\n";
	} else if (!child && !args[9] && !definition->menu_name.empty()) {
		// A class menu named by a string loads by that name; a name the module has no menu for leaves the window without one.
		if (!menu_loader) throw uint32_t(120);
		std::vector<uint8_t> name((definition->menu_name.size()+1)*2);
		std::memcpy(name.data(),definition->menu_name.data(),definition->menu_name.size()*2);
		const auto at=process.map_memory(4096,PROT_READ | PROT_WRITE);
		try {
			process.write_memory(at,std::span<const uint8_t>(name.data(),std::min<size_t>(name.size(),4096)));
			record.menu=menu_loader(definition->instance,at);
		} catch (...) { process.unmap_memory(at); throw; }
		process.unmap_memory(at);
	}
	record.extra.resize(definition->window_extra);
	const auto handle=objects.add(std::move(record));
	if (!handle) throw uint32_t(8);
	// A new child sits below its siblings, so controls created first stay on top of those created later.
	if (child) objects.restack(handle,false);
	uint64_t scratch=0;
	try {
		scratch=process.map_memory(4096,PROT_READ | PROT_WRITE);
		// CREATESTRUCTW followed by the 16-byte window rectangle used for WM_NCCALCSIZE.
		const bool narrow=process.mode()==NativeMode::x86;
		const size_t rect_at=narrow ? 48 : 80;
		std::vector<uint8_t> bytes(rect_at+16);
		if (narrow) {
			put(bytes,0,args[11],4); put(bytes,4,args[10],4);put(bytes,8,args[9],4);put(bytes,12,args[8],4);
			put(bytes,16,uint32_t(height),4); put(bytes,20,uint32_t(width),4);
			put(bytes,24,uint32_t(relative_y),4); put(bytes,28,uint32_t(relative_x),4); put(bytes,32,uint32_t(args[3]),4);
			put(bytes,36,args[2],4); put(bytes,40,args[1],4);put(bytes,44,ex_style,4);
		} else {
			put(bytes,0,args[11],8); put(bytes,8,args[10],8);put(bytes,16,args[9],8);put(bytes,24,args[8],8);
			put(bytes,32,uint32_t(height),4); put(bytes,36,uint32_t(width),4);
			put(bytes,40,uint32_t(relative_y),4); put(bytes,44,uint32_t(relative_x),4); put(bytes,48,uint32_t(args[3]),4);
			put(bytes,56,args[2],8); put(bytes,64,args[1],8);put(bytes,72,ex_style,4);
		}
		put(bytes,rect_at,uint32_t(x),4); put(bytes,rect_at+4,uint32_t(y),4);
		put(bytes,rect_at+8,uint32_t(x+width),4); put(bytes,rect_at+12,uint32_t(y+height),4);
		process.write_memory(scratch,bytes);
		// A CBT hook sees the window before WM_NCCREATE (HCBT_CREATEWND with a CBT_CREATEWND of the create structure and the window to
		// insert after); class libraries subclass their windows there, and a nonzero answer cancels the creation.
		if (has_hook(5)) {
			std::vector<uint8_t> cbt(narrow ? 8 : 16);
			put(cbt,0,scratch,narrow ? 4 : 8);
			const auto at=scratch+rect_at+16;
			process.write_memory(at,cbt);
			if (call_hooks(5,3,handle,at) || !objects.window(handle)) {
				if (objects.window(handle)) { release_surface(handle); objects.erase(handle); }
				process.unmap_memory(scratch); return 0;
			}
		}
		const auto accepted=send(handle,0x81,0,scratch);
		if (!objects.window(handle)) { process.unmap_memory(scratch); return 0; }
		if (!accepted) {
			send(handle,0x82,0,0);
			release_surface(handle);
			objects.erase(handle); process.unmap_memory(scratch); return 0;
		}
		// WM_NCCREATE may already have moved or sized the window, so the frame calculation starts from its current rectangle.
		{
			const auto &outer=objects.window(handle)->outer;
			put(bytes,rect_at,uint32_t(outer.left),4); put(bytes,rect_at+4,uint32_t(outer.top),4);
			put(bytes,rect_at+8,uint32_t(outer.right),4); put(bytes,rect_at+12,uint32_t(outer.bottom),4);
			process.write_memory(scratch+rect_at,std::span<const uint8_t>(bytes.data()+rect_at,16));
		}
		send(handle,0x83,0,scratch+rect_at);
		if (!objects.window(handle)) { process.unmap_memory(scratch); return 0; }
		const auto client=process.read_memory(scratch+rect_at,16);
		const auto left=int32_t(uint32_t(read(client,0,4))),top=int32_t(uint32_t(read(client,4,4)));
		// A window smaller than its frame has an empty client area, not a negative one.
		const auto right=uint32_t(std::max(int32_t(uint32_t(read(client,8,4))),left)),bottom=uint32_t(std::max(int32_t(uint32_t(read(client,12,4))),top));
		if (int64_t(left)-x<INT32_MIN || int64_t(left)-x>INT32_MAX || int64_t(top)-y<INT32_MIN || int64_t(top)-y>INT32_MAX) {
			send(handle,0x82,0,0);
			throw uint32_t(120);
		}
		auto *geometry=objects.window(handle);
		// Children the window made during WM_NCCREATE were laid out in its window rectangle; they follow the client origin.
		const std::pair<int32_t,int32_t> laid_out{geometry->x,geometry->y};
		geometry->x=left; geometry->y=top;
		if (left!=laid_out.first || top!=laid_out.second)
			for (const auto inner : objects.child_windows(handle))
				shift_window(inner,left-laid_out.first,top-laid_out.second);
		geometry=objects.window(handle);
		geometry->width=int32_t(right-uint32_t(left)); geometry->height=int32_t(bottom-uint32_t(top));
		objects.update_window_dcs(handle);geometry->client_ready=true;
		const auto created=send(handle,1,0,scratch);
		process.unmap_memory(scratch); scratch=0;
		if (!objects.window(handle)) return 0;
		if (created==UINT64_MAX) { reject_tree(handle);return 0; }
		auto *window=objects.window(handle);
		if (!(window->style & 0xc0000000U)) {
			window->need_size=true;
		} else {
			send(handle,5,0,uint16_t(window->width) | (uint64_t(uint16_t(window->height))<<16));
			window=objects.window(handle);
			if (!window) throw uint32_t(1400);
			send(handle,3,0,uint16_t(child ? int64_t(window->x)-objects.window(args[8])->x : window->x) |
				(uint64_t(uint16_t(child ? int64_t(window->y)-objects.window(args[8])->y : window->y))<<16));
			if (!objects.window(handle)) throw uint32_t(1400);
		}
		if (child) parent_notify(handle,1);
		if (!objects.window(handle)) return 0;
		if (const auto *created=objects.window(handle)) publish_window(handle,uint32_t(std::max(created->width,0)),uint32_t(std::max(created->height,0)));
		if (effective_style & 0x10000000U) {
			auto *window=objects.window(handle);window->visible=true;objects.update_window_dcs(child ? window->parent : handle);
			if (objects.window_visible(handle)) present(handle);
			if (!objects.invalidate(handle,{0,0,window->width,window->height},true)) throw uint32_t(8);
		}
		return handle;
	} catch (...) {
		discard_tree(handle);
		if (scratch) process.unmap_memory(scratch);
		throw;
	}
}

uint64_t NativeWindows::map_points(uint64_t from,uint64_t to,uint64_t points,uint32_t count)
{
	const auto origin = [&](uint64_t handle) {
		if (!handle) return std::pair<int32_t,int32_t>{0,0};
		const auto *window=objects.window(handle);
		if (!window) throw uint32_t(1400);
		return std::pair<int32_t,int32_t>{window->x,window->y};
	};
	const auto source=origin(from),destination=origin(to);
	const auto dx=uint32_t(int64_t(source.first)-destination.first);
	const auto dy=uint32_t(int64_t(source.second)-destination.second);
	if (count>65536) throw uint32_t(120);
	if (count) {
		auto bytes=process.read_memory(points,size_t(count)*8);
		for (size_t offset=0;offset<bytes.size();offset+=8) {
			put(bytes,offset,uint32_t(read(bytes,offset,4))+dx,4);
			put(bytes,offset+4,uint32_t(read(bytes,offset+4,4))+dy,4);
		}
		process.write_memory(points,bytes);
	}
	return uint16_t(dx) | (uint32_t(uint16_t(dy))<<16);
}

void NativeWindows::publish_screen_dc(uint64_t desktop)
{
	if (!screen_block)
		return;
	auto &published=*reinterpret_cast<uint64_t *>(screen_block+screen_dc);
	// A published context that is gone (deleted rather than released) is withdrawn, and so is one a caller holds: it stays that
	// caller's, an ordinary context its ReleaseDC brings to the runtime, and the next GetDC(NULL) in the guest takes a new one.
	if (const uint64_t current=__atomic_load_n(&published,__ATOMIC_ACQUIRE); current &&
		(!objects.dc(current) || __atomic_load_n(reinterpret_cast<uint32_t *>(screen_block+screen_dc_state),__ATOMIC_ACQUIRE)))
		withdraw_screen_dc(current);
	if (__atomic_load_n(&published,__ATOMIC_ACQUIRE))
		return;
	// A context of its own, free: the caller keeps the one it asked for, which a program may hold for its whole life.
	const auto *window=objects.window(desktop);
	DesktopDC device{window->bitmap,window->bitmap,desktop}; device.leased=true;
	objects.configure_window_dc(desktop,device,false);
	uint64_t handle=0;
	try { handle=objects.add(device); } catch (...) { return; }
	if (!handle)
		return;
	for (int32_t index=0;index<128;++index) {
		const uint32_t value=device_caps(objects,*objects.dc(handle),index);
		std::memcpy(screen_block+screen_caps+size_t(index)*4,&value,4);
	}
	__atomic_store_n(reinterpret_cast<uint32_t *>(screen_block+screen_dc_state),uint32_t(0),__ATOMIC_RELEASE);
	__atomic_store_n(&published,handle,__ATOMIC_RELEASE);
}

bool NativeWindows::withdraw_screen_dc(uint64_t handle)
{
	if (!screen_block || !handle)
		return false;
	auto &published=*reinterpret_cast<uint64_t *>(screen_block+screen_dc);
	if (__atomic_load_n(&published,__ATOMIC_ACQUIRE)!=handle)
		return false;
	// The context goes first, so a GetDC that takes the freed state finds none and goes to the runtime.
	__atomic_store_n(&published,uint64_t(0),__ATOMIC_RELEASE);
	__atomic_store_n(reinterpret_cast<uint32_t *>(screen_block+screen_dc_state),uint32_t(0),__ATOMIC_RELEASE);
	return true;
}

void NativeWindows::set_window_table(uint8_t *table)
{
	window_table=table;
	objects.touched_windows().overflow=true;
	publish_windows();
}

// The fixed part of a window's record: everything but its handle and properties.
static void window_fields(const DesktopWindow &window,uint8_t *record)
{
	const auto put=[&](size_t offset,auto value) { std::memcpy(record+offset,&value,sizeof(value)); };
	put(8,window.userdata); put(16,window.procedure); put(24,window.instance);
	put(32,window.parent ? window.parent : window.owner); put(40,window.child_id);
	put(48,window.style); put(52,window.ex_style);
	put(56,window.width); put(60,window.height);
	put(64,window.outer.left); put(68,window.outer.top); put(72,window.outer.right); put(76,window.outer.bottom);
	put(88,window.parent);
	// Extra bytes beyond the record are read by the runtime.
	const bool extra=window.extra.size()<=NativeWindows::window_extra_bytes;
	put(80,uint32_t(extra ? window.extra.size() : 0));
	if (extra && !window.extra.empty()) std::memcpy(record+160,window.extra.data(),window.extra.size());
}

void NativeWindows::publish_window_record(uint64_t handle,const DesktopWindow *window)
{
	auto *record=window_table+(handle%window_records)*window_record;
	const auto put=[&](size_t offset,auto value) { std::memcpy(record+offset,&value,sizeof(value)); };
	uint64_t occupant=0;
	std::memcpy(&occupant,record,8);
	if (!window) {
		if (occupant==handle) put(0,uint64_t(0));
		return;
	}
	// Two windows whose handles share the low bits: the first keeps the record and the other is answered by the runtime.
	if (occupant && occupant!=handle && std::as_const(objects).window(occupant))
		return;
	put(0,uint64_t(0));
	window_fields(*window,record);
	// Properties named by an atom are keyed "#" and its number in decimal; up to four of them travel with the record.
	uint32_t count=0;
	for (const auto &[key,value] : window->properties) {
		if (key.size()<2 || key.size()>6 || key[0]!=u'#' || (key[1]==u'0' && key.size()>2)) continue;
		uint64_t atom=0;
		bool digits=true;
		for (size_t index=1;index<key.size() && digits;++index) {
			digits=key[index]>=u'0' && key[index]<=u'9';
			atom=atom*10+uint64_t(key[index]-u'0');
		}
		if (!digits || atom>0xffff) continue;
		if (count==4) { count=UINT32_MAX; break; }
		put(96+size_t(count)*16,atom); put(104+size_t(count)*16,value);
		++count;
	}
	put(84,count);
	put(0,handle);
}

void NativeWindows::publish_windows()
{
	if (!window_table)
		return;
	// Nothing touched, watched or released since the last time: only the few published words can have changed.
	uint32_t logged=0;
	if (deferred_log) std::memcpy(&logged,deferred_log,4);
	const auto idle=[](auto &touched) { return touched.handles.empty() && !touched.overflow; };
	if (!logged && deferred_pending.empty() && idle(objects.touched_windows()) && idle(objects.touched_menus()) &&
		idle(objects.touched_bitmaps()) && idle(objects.touched_dcs()) && idle(objects.touched_icons()) && watched.empty() &&
		watched_bitmaps.empty() && watched_dcs.empty()) {
		publish_words();
		return;
	}
	// Handles the guest released in a call still in progress keep out of its tables until the runtime carries the release out.
	deferred_pending.clear();
	if (deferred_log) {
		uint32_t count=0;
		std::memcpy(&count,deferred_log,4);
		for (uint32_t index=0;index<std::min<uint32_t>(count,deferred_entries);++index) {
			uint64_t handle=0;
			std::memcpy(&handle,deferred_log+16+size_t(index)*16,8);
			deferred_pending.push_back(handle);
		}
	}
	auto &touched=objects.touched_windows().handles;
	if (objects.touched_windows().overflow) {
		std::memset(window_table,0,window_records*window_record);
		watched.clear();
		for (const auto handle : objects.window_handles()) {
			const auto *window=std::as_const(objects).window(handle);
			publish_window_record(handle,window);
			watched.push_back({handle,window,api_depth});
		}
		objects.touched_windows().overflow=false;
		touched.clear();
	}
	// Windows handed out since the guest last ran are written whole; they stay watched while the call that touched them goes on.
	for (const auto handle : objects.touched_windows().settled()) {
		const auto *window=std::as_const(objects).window(handle);
		publish_window_record(handle,window);
		const auto found=std::find_if(watched.begin(),watched.end(),[&](const auto &item) { return item.first==handle; });
		if (found!=watched.end()) {
			if (window) { found->second=window; found->depth=std::min(found->depth,api_depth); }
			else watched.erase(found);
		} else if (window && watching) {
			watched.push_back({handle,window,api_depth});
		}
	}
	touched.clear();
	// A watched window may have been changed through a pointer taken before the guest last ran: its fields are compared again.
	for ([[maybe_unused]] const auto &[handle,window,depth] : watched) {
		auto *record=window_table+(handle%window_records)*window_record;
		uint64_t occupant=0;
		std::memcpy(&occupant,record,8);
		if (occupant!=handle) continue;
		alignas(8) uint8_t fields[window_record];
		window_fields(*window,fields);
		const size_t extra=window->extra.size()<=window_extra_bytes ? window->extra.size() : 0;
		if (std::memcmp(fields+8,record+8,76) || std::memcmp(fields+88,record+88,8) || (extra && std::memcmp(fields+160,record+160,extra)))
			publish_window_record(handle,window);
	}
	publish_menus();
	publish_bitmaps();
	publish_dcs();
	publish_icons();
	if (!api_depth) watched.clear();
	publish_words();
}

void NativeWindows::set_menu_table(uint8_t *table)
{
	menu_table=table;
	objects.touched_menus().overflow=true;
	publish_menus();
}

static size_t menu_block_size(const DesktopMenu &menu)
{
	size_t size=menu.items.size()*32;
	for (const auto &item : menu.items) size+=item.text.size()*2;
	return (size+7)&~size_t(7);
}

void NativeWindows::publish_menu(uint64_t handle,const DesktopMenu *menu)
{
	auto *slot=menu_table+(handle%menu_slots)*16;
	uint64_t occupant=0;
	std::memcpy(&occupant,slot,8);
	if (!menu) {
		if (occupant==handle) std::memset(slot,0,8);
		return;
	}
	// Two menus whose handles share the low bits: the first keeps the slot and the other is answered by the runtime.
	if (occupant && occupant!=handle && objects.menu(occupant))
		return;
	// An unchanged menu is left as it is, and one whose new block fits where its old one is written there again.
	if (occupant==handle && menu_matches(handle,*menu))
		return;
	const size_t size=menu_block_size(*menu);
	size_t at=menu_used;
	if (occupant==handle) {
		uint32_t place=0;
		std::memcpy(&place,slot+8,4);
		if (const auto found=menu_capacity.find(handle); found!=menu_capacity.end() && found->second.first==place && size<=found->second.second)
			at=place;
	}
	std::memset(slot,0,8);
	if (menu->items.size()>4096 || size>menu_area-menu_blocks)
		return;
	if (at==menu_used && menu_used+size>menu_area) {
		// The blocks are full of old copies: every menu is written again from the start.
		std::memset(menu_table,0,menu_blocks);
		menu_used=menu_blocks;
		menu_capacity.clear();
		for (const auto other : objects.menu_handles())
			if (const auto *record=objects.menu(other); record && menu_used+menu_block_size(*record)<=menu_area) publish_menu(other,record);
		return;
	}
	auto *block=menu_table+at;
	uint32_t text=uint32_t(at+menu->items.size()*32);
	for (const auto &item : menu->items) {
		const uint32_t fields[4]={item.type,item.state,item.id,uint32_t(item.text.size())};
		std::memcpy(block,fields,16);
		std::memcpy(block+16,&item.submenu,8);
		std::memcpy(block+24,&text,4);
		std::memset(block+28,0,4);
		std::memcpy(menu_table+text,item.text.data(),item.text.size()*2);
		text+=uint32_t(item.text.size()*2);
		block+=32;
	}
	const uint32_t place[2]={uint32_t(at),uint32_t(menu->items.size())};
	std::memcpy(slot+8,place,8);
	if (at==menu_used) {
		menu_capacity[handle]={uint32_t(at),size};
		menu_used+=size;
	}
	std::memcpy(slot,&handle,8);
}

bool NativeWindows::menu_matches(uint64_t handle,const DesktopMenu &menu) const
{
	const auto *slot=menu_table+(handle%menu_slots)*16;
	uint64_t occupant=0;
	uint32_t place[2]={};
	std::memcpy(&occupant,slot,8);
	std::memcpy(place,slot+8,8);
	if (occupant!=handle) return true;
	if (place[1]!=menu.items.size()) return false;
	const auto *block=menu_table+place[0];
	for (const auto &item : menu.items) {
		uint32_t fields[4]={},text=0;
		uint64_t submenu=0;
		std::memcpy(fields,block,16);
		std::memcpy(&submenu,block+16,8);
		std::memcpy(&text,block+24,4);
		if (fields[0]!=item.type || fields[1]!=item.state || fields[2]!=item.id || fields[3]!=item.text.size() || submenu!=item.submenu ||
			std::memcmp(menu_table+text,item.text.data(),item.text.size()*2))
			return false;
		block+=32;
	}
	return true;
}

void NativeWindows::publish_menus()
{
	if (!menu_table)
		return;
	auto &touched=objects.touched_menus().handles;
	if (objects.touched_menus().overflow) {
		std::memset(menu_table,0,menu_blocks);
		menu_used=menu_blocks;
		menu_capacity.clear();
		for (const auto handle : objects.menu_handles()) publish_menu(handle,objects.menu(handle));
		objects.touched_menus().overflow=false;
		touched.clear();
	}
	// Menus handed out since the guest last ran are written again. None is watched as windows are: the calls that change menus never run
	// the guest while they hold one.
	for (const auto handle : objects.touched_menus().settled())
		publish_menu(handle,objects.menu(handle));
	touched.clear();
}

void NativeWindows::set_bitmap_table(uint8_t *table)
{
	bitmap_table=table;
	objects.touched_bitmaps().overflow=true;
	publish_bitmaps();
}

// What GetObject answers for a bitmap in 32 bytes (see NativeDesktop::get_object).
static std::array<uint8_t,32> bitmap_fields_of(const DesktopBitmap &bitmap)
{
	std::array<uint8_t,32> bytes{};
	const auto put=[&](size_t offset,auto value) { std::memcpy(bytes.data()+offset,&value,sizeof(value)); };
	put(4,int32_t(bitmap.width));
	put(8,int32_t(bitmap.height));
	put(12,uint32_t(bitmap.stride ? bitmap.stride : bitmap.address ? size_t(bitmap.width)*4 : bitmap.bits.size()/bitmap.height));
	put(16,uint16_t(1));
	put(18,uint16_t(bitmap.depth));
	put(24,uint64_t(bitmap.dib_section ? bitmap.address : 0));
	return bytes;
}

void NativeWindows::publish_bitmap(uint64_t handle,const DesktopBitmap *bitmap,bool touched)
{
	auto *record=bitmap_table+(handle%bitmap_records)*bitmap_record;
	uint64_t occupant=0;
	std::memcpy(&occupant,record,8);
	if (!bitmap || !bitmap->height) {
		if (occupant==handle) std::memset(record,0,8);
		return;
	}
	// Two bitmaps whose handles share the low bits: the first keeps the record and the other is answered by the runtime.
	if (occupant && occupant!=handle && objects.bitmap(occupant))
		return;
	if (deferred(handle))
		return;
	const auto fields=bitmap_fields_of(*bitmap);
	// Bit 1: DeleteObject may release it in the guest, as it is neither a stock bitmap, nor a window's surface, nor selected anywhere.
	// Only a bitmap handed out since the guest last ran can have become selected (SelectObject looks it up), so only its selection
	// is looked for again; a bitmap a context let go keeps the bit clear and is released by the runtime, as before.
	uint64_t previous=0;
	if (occupant==handle) std::memcpy(&previous,record+8,8);
	const bool deletable=touched ? !bitmap->stock && !bitmap->shared && !objects.selected(handle) : (previous & 2)!=0;
	const uint64_t flags=(bitmap->dib_section ? 1 : 0) | (deletable ? 2 : 0);
	if (occupant==handle && !std::memcmp(record+8,&flags,8) && !std::memcmp(record+16,fields.data(),fields.size()))
		return;
	std::memset(record,0,8);
	std::memcpy(record+8,&flags,8);
	std::memcpy(record+16,fields.data(),fields.size());
	std::memcpy(record,&handle,8);
}

void NativeWindows::publish_bitmaps()
{
	if (!bitmap_table)
		return;
	auto &touched=objects.touched_bitmaps();
	if (touched.overflow) {
		std::memset(bitmap_table,0,bitmap_records*bitmap_record);
		watched_bitmaps.clear();
		for (const auto handle : objects.bitmap_handles()) publish_bitmap(handle,std::as_const(objects).bitmap(handle),true);
		touched.overflow=false;
		touched.handles.clear();
	}
	// As with windows: bitmaps handed out since the guest last ran are written again, and stay watched while the call goes on.
	for (const auto handle : touched.settled()) {
		const auto *bitmap=std::as_const(objects).bitmap(handle);
		publish_bitmap(handle,bitmap,true);
		const auto found=std::find_if(watched_bitmaps.begin(),watched_bitmaps.end(),[&](const auto &item) { return item.first==handle; });
		if (found!=watched_bitmaps.end()) {
			if (bitmap) { found->second=bitmap; found->depth=std::min(found->depth,api_depth); }
			else watched_bitmaps.erase(found);
		} else if (bitmap && watching) {
			watched_bitmaps.push_back({handle,bitmap,api_depth});
		}
	}
	touched.handles.clear();
	for ([[maybe_unused]] const auto &[handle,bitmap,depth] : watched_bitmaps) publish_bitmap(handle,bitmap,false);
	if (!api_depth) watched_bitmaps.clear();
}

void NativeWindows::set_system_table(uint8_t *table)
{
	system_table=table;
	for (int32_t index=0;index<128;++index) {
		const uint32_t value=system_metric(objects.nonclient_metrics(),index);
		std::memcpy(table+size_t(index)*4,&value,4);
	}
	for (int32_t index=0;index<64;++index) {
		const uint32_t value=objects.system_color(index);
		std::memcpy(table+512+size_t(index)*4,&value,4);
	}
}

void NativeWindows::set_dc_table(uint8_t *table)
{
	dc_table=table;
	objects.touched_dcs().overflow=true;
	publish_dcs();
}

void NativeWindows::apply_dc_log()
{
	if (!dc_table)
		return;
	uint32_t count=0;
	std::memcpy(&count,dc_table,4);
	for (uint32_t index=0;index<std::min<uint32_t>(count,64);++index) {
		uint64_t handle=0;
		std::memcpy(&handle,dc_table+8+size_t(index)*8,8);
		const auto *slot=dc_table+dc_slots_at+(handle%dc_slots)*dc_slot;
		uint64_t occupant=0;
		std::memcpy(&occupant,slot,8);
		auto *device=occupant==handle ? objects.dc(handle) : nullptr;
		if (!device)
			continue;
		std::memcpy(&device->text_color,slot+8,4);
		std::memcpy(&device->background_color,slot+12,4);
		std::memcpy(&device->background_mode,slot+16,4);
		std::memcpy(&device->text_align,slot+20,4);
	}
	std::memset(dc_table,0,4);
}

void NativeWindows::publish_dc(uint64_t handle,const DesktopDC *device)
{
	auto *slot=dc_table+dc_slots_at+(handle%dc_slots)*dc_slot;
	uint64_t occupant=0;
	std::memcpy(&occupant,slot,8);
	if (!device) {
		if (occupant==handle) std::memset(slot,0,8);
		return;
	}
	// Two contexts whose handles share the low bits: the first keeps the slot and the other is answered by the runtime.
	if (occupant && occupant!=handle && objects.dc(occupant))
		return;
	if (deferred(handle))
		return;
	const uint32_t fields[5]={device->text_color,device->background_color,device->background_mode,device->text_align,device->window ? 1U : 0U};
	if (occupant==handle && !std::memcmp(slot+8,fields,20))
		return;
	std::memset(slot,0,8);
	std::memcpy(slot+8,fields,20);
	std::memcpy(slot,&handle,8);
}

void NativeWindows::publish_dcs()
{
	if (!dc_table)
		return;
	auto &touched=objects.touched_dcs();
	if (touched.overflow) {
		std::memset(dc_table+dc_slots_at,0,dc_slots*dc_slot);
		watched_dcs.clear();
		for (const auto handle : objects.dc_handles()) publish_dc(handle,std::as_const(objects).dc(handle));
		touched.overflow=false;
		touched.handles.clear();
	}
	// As with windows: contexts handed out since the guest last ran are written again, and stay watched while the call goes on.
	for (const auto handle : touched.settled()) {
		const auto *device=std::as_const(objects).dc(handle);
		publish_dc(handle,device);
		const auto found=std::find_if(watched_dcs.begin(),watched_dcs.end(),[&](const auto &item) { return item.first==handle; });
		if (found!=watched_dcs.end()) {
			if (device) { found->second=device; found->depth=std::min(found->depth,api_depth); }
			else watched_dcs.erase(found);
		} else if (device && watching) {
			watched_dcs.push_back({handle,device,api_depth});
		}
	}
	touched.handles.clear();
	for ([[maybe_unused]] const auto &[handle,device,depth] : watched_dcs) publish_dc(handle,device);
	if (!api_depth) watched_dcs.clear();
}

void NativeWindows::set_deferred_tables(uint8_t *log,uint8_t *icons)
{
	deferred_log=log;
	icon_table=icons;
	objects.touched_icons().overflow=true;
	publish_icons();
}

void NativeWindows::take_deferred(const std::function<void(uint32_t,uint64_t)> &apply)
{
	if (!deferred_log)
		return;
	uint32_t count=0;
	std::memcpy(&count,deferred_log,4);
	if (!count)
		return;
	std::memset(deferred_log,0,4);
	deferred_pending.clear();
	for (uint32_t index=0;index<std::min<uint32_t>(count,deferred_entries);++index) {
		uint64_t entry[2]={};
		std::memcpy(entry,deferred_log+16+size_t(index)*16,16);
		apply(uint32_t(entry[1]),entry[0]);
	}
}

void NativeWindows::publish_icons()
{
	if (!icon_table)
		return;
	auto &touched=objects.touched_icons();
	const auto publish=[&](uint64_t handle) {
		auto *slot=icon_table+(handle%icon_slots)*8;
		uint64_t occupant=0;
		std::memcpy(&occupant,slot,8);
		if (deferred(handle))
			return;
		if (std::as_const(objects).icon(handle)) {
			// Two icons whose handles share the low bits: the first keeps the slot and the other is answered by the runtime.
			if (!occupant || occupant==handle || !std::as_const(objects).icon(occupant)) std::memcpy(slot,&handle,8);
		} else if (occupant==handle) {
			std::memset(slot,0,8);
		}
	};
	if (touched.overflow) {
		std::memset(icon_table,0,icon_slots*8);
		for (const auto handle : objects.icon_handles()) publish(handle);
		touched.overflow=false;
	}
	for (const auto handle : touched.settled()) publish(handle);
	touched.handles.clear();
}

void NativeWindows::publish_words()
{
	if (system_table && published_accelerators!=objects.accelerators_version()) {
		published_accelerators=objects.accelerators_version();
		const auto tables=objects.accelerator_handles();
		const uint32_t count=tables.size()<=64 ? uint32_t(tables.size()) : UINT32_MAX;
		std::memset(system_table+1024,0,4);
		if (count!=UINT32_MAX) std::memcpy(system_table+1032,tables.data(),tables.size()*8);
		std::memcpy(system_table+1024,&count,4);
	}
	if (screen_block) {
		const uint32_t flags=1U | (has_hook(-1) ? 2U : 0U);
		std::memcpy(screen_block+loop_flags,&flags,4);
		const uint64_t shown_desktop=desktop && std::as_const(objects).window(desktop) ? desktop : 0;
		std::memcpy(screen_block+desktop_handle,&shown_desktop,8);
		__atomic_store_n(reinterpret_cast<uint64_t *>(screen_block+focus_window),focus,__ATOMIC_RELEASE);
		__atomic_store_n(reinterpret_cast<uint64_t *>(screen_block+focus_published),uint64_t(1),__ATOMIC_RELEASE);
	}
}

void NativeWindows::api_left()
{
	// What the returning call touched is written now: the call is over, so no pointer of its can change it later.
	watching=false;
	publish_windows();
	watching=true;
	--api_depth;
	// What the returning call touched no longer needs watching for it.
	const auto outlived=[&](const auto &item) { return item.depth>api_depth; };
	std::erase_if(watched,outlived);
	std::erase_if(watched_bitmaps,outlived);
	std::erase_if(watched_dcs,outlived);
}

void NativeWindows::screen_dc_use(std::string_view name)
{
	if (!screen_block)
		return;
	const uint64_t published=__atomic_load_n(reinterpret_cast<uint64_t *>(screen_block+screen_dc),__ATOMIC_ACQUIRE);
	if (!published || name=="GetDeviceCaps" || name=="ReleaseDC")
		return;
	// Only the first argument: the calls that change a context take it there, and the other registers may hold anything.
	if (process.api_arguments(1)[0]==published)
		__atomic_store_n(reinterpret_cast<uint32_t *>(screen_block+screen_dc_state),uint32_t(2),__ATOMIC_RELEASE);
}

bool NativeWindows::dispatch(std::string_view name)
{
	if (tree_dispatch(name) || system_dispatch(name)) return true;
	// A 32-bit program's window data accessors are served by the shared handlers, which apply style changes.
	if (process.mode()==NativeMode::x86 && (name=="GetWindowLongW" || name=="SetWindowLongW")) return false;
	const bool data=(name.starts_with("GetWindowLong") || name.starts_with("SetWindowLong") || name.starts_with("GetClassLong") || name.starts_with("SetClassLong"));
	const bool info=name=="GetClassInfoW" || name=="GetClassInfoExW" || name=="GetClassNameW" || name=="GetClassNameA";
	const bool registration=name=="RegisterClassExW" || name=="RegisterClassW";
	const bool creation=name=="CreateWindowExW";
	const bool coordinate=name=="ClientToScreen" || name=="ScreenToClient";
	const bool adjustment=name=="AdjustWindowRect" || name=="AdjustWindowRectEx";
	if (!adjustment && name!="GetSystemMetrics" && !coordinate && name!="MapWindowPoints" && !info && !data && name!="CallWindowProcW" && !registration && !creation && name!="DestroyWindow" && name!="SendMessageW" && name!="SendMessageTimeoutW" && name!="IsWindow" &&
		name!="IsWindowUnicode" && name!="UnregisterClassW" && name!="ShowWindow" && name!="GetDC" && name!="GetWindowDC" && name!="WindowFromDC" && name!="ReleaseDC" &&
		name!="UpdateWindow" && name!="IsWindowVisible" && name!="BeginPaint" && name!="EndPaint" &&
		name!="PostMessageW" && name!="SendNotifyMessageW" && name!="PostThreadMessageW" && name!="PeekMessageW" && name!="GetMessageW" && name!="DispatchMessageW" &&
		name!="TranslateMessage" && name!="PostQuitMessage" && name!="DefWindowProcW" &&
		name!="GetWindowTextW" && name!="InternalGetWindowText" && name!="GetWindowTextLengthW" && name!="SetWindowTextW" &&
		name!="GetFocus" && name!="SetFocus" && name!="GetActiveWindow" && name!="GetClientRect" && name!="GetWindowRect" &&
		name!="InvalidateRect" && name!="ValidateRect" && name!="GetUpdateRect") return false;
	const auto args=process.api_arguments(adjustment ? (name=="AdjustWindowRectEx" ? 4 : 3) : coordinate ? 2 : name=="MapWindowPoints" ? 4 : info ? 3 : data ? (name.starts_with("Set") ? 3 : 2) : name=="CallWindowProcW" ? 5 : registration ? 1 : creation ? 12 : name=="SendMessageW" ? 4 : name=="SendMessageTimeoutW" ? 7 :
		name=="PostMessageW" || name=="SendNotifyMessageW" || name=="PostThreadMessageW" || name=="GetMessageW" || name=="DefWindowProcW" ? 4 : name=="PeekMessageW" ? 5 :
		name=="GetWindowTextW" || name=="InternalGetWindowText" || name=="InvalidateRect" || name=="GetUpdateRect" ? 3 :
		name=="SetWindowTextW" || name=="GetClientRect" || name=="GetWindowRect" || name=="ValidateRect" ? 2 :
		name=="UnregisterClassW" || name=="ShowWindow" || name=="ReleaseDC" || name=="BeginPaint" || name=="EndPaint" ? 2 : 1);
	uint64_t result=name=="GetMessageW" ? UINT64_MAX : 0;
	try {
		if (name=="GetSystemMetrics") {
			// The icon size belongs to the display the icons are drawn for; every other metric comes from the shared table.
			const auto index=int32_t(uint32_t(args[0]));
			if (index==11 || index==12) {
				const auto display=objects.display();result=index==11 ? display.width : display.height;
			} else result=system_metric(objects.nonclient_metrics(),index);
		}
		else if (adjustment) {
			const auto bytes=process.read_memory(args[0],16);
			const DesktopRect rect{int32_t(read(bytes,0,4)),int32_t(read(bytes,4,4)),int32_t(read(bytes,8,4)),int32_t(read(bytes,12,4))};
			const auto adjusted=adjust_window_rect(objects.nonclient_metrics(),rect,uint32_t(args[1]),uint32_t(args[2])!=0,name=="AdjustWindowRectEx" ? uint32_t(args[3]) : 0);
			std::array<uint8_t,16> output{};
			put(output,0,uint32_t(adjusted.left),4); put(output,4,uint32_t(adjusted.top),4);
			put(output,8,uint32_t(adjusted.right),4); put(output,12,uint32_t(adjusted.bottom),4);
			process.write_memory(args[0],output); result=1;
		} else if (coordinate) {
			if (!objects.window(args[0])) throw uint32_t(1400);
			map_points(name=="ClientToScreen" ? args[0] : 0,name=="ScreenToClient" ? args[0] : 0,args[1],1);
			result=1;
		} else if (name=="MapWindowPoints") result=map_points(args[0],args[1],args[2],uint32_t(args[3]));
		else if (info) result=class_info(name,args);
		else if (data) result=long_value(name,args);
		else if (name=="CallWindowProcW") {
			if (!args[0] || !invoke) throw uint32_t(120);
			const auto called=invoke(args[0],std::span<const uint64_t>(args).subspan(1));
			if (called.stop!=NativeStop::returned) throw called;
			result=called.value;
		}
		else if (name=="GetFocus") result=focus;
		else if (name=="GetActiveWindow") result=active;
		else if (name=="SetFocus") result=set_focus(args[0]);
		else if (name=="InvalidateRect" || name=="ValidateRect") {
			if (!args[0]) throw uint32_t(120);
			const auto rect=update_rect(args[0],args[1]);
			if (!(name=="InvalidateRect" ? objects.invalidate(args[0],rect,args[2]!=0) : objects.validate(args[0],rect))) throw uint32_t(8);
			result=1;
		} else if (name=="GetUpdateRect") {
			const auto *window=objects.window(args[0]); if (!window) throw uint32_t(1400);
			if (args[2]) throw uint32_t(120);
			const auto rect=window->update.bounds();
			if (args[1]) {
				std::array<uint8_t,16> bytes{};
				put(bytes,0,uint32_t(rect.left),4); put(bytes,4,uint32_t(rect.top),4);
				put(bytes,8,uint32_t(rect.right),4); put(bytes,12,uint32_t(rect.bottom),4);
				process.write_memory(args[1],bytes);
			}
			result=!window->update.empty();
		}
		else if (name=="GetClientRect" || name=="GetWindowRect") {
			const auto *window=objects.window(args[0]);
			DesktopRect bounds{};
			if (window) bounds=name=="GetClientRect" ? DesktopRect{0,0,window->width,window->height} : window->outer;
			else {
				const auto size=name=="GetClientRect" ? foreign_window_size(args[0]) : std::nullopt;
				if (!size || size->first>INT32_MAX || size->second>INT32_MAX) throw uint32_t(1400);
				bounds={0,0,int32_t(size->first),int32_t(size->second)};
			}
			std::array<uint8_t,16> rect{};
			put(rect,0,uint32_t(bounds.left),4); put(rect,4,uint32_t(bounds.top),4);
			put(rect,8,uint32_t(bounds.right),4); put(rect,12,uint32_t(bounds.bottom),4);
			process.write_memory(args[1],rect); result=1;
		}
		else if (name=="DefWindowProcW") result=default_message(args);
		else if (name=="GetWindowTextLengthW") result=send(args[0],0x0e,0,0);
		else if (name=="SetWindowTextW") result=send(args[0],0x0c,0,args[1]);
		else if (name=="GetWindowTextW") {
			if (int32_t(args[2])>0) result=send(args[0],0x0d,uint32_t(args[2]),args[1]);
		}
		else if (name=="InternalGetWindowText") {
			if (int32_t(args[2])>0) result=copy_window_text(args[0],args[1],uint32_t(args[2]));
		}
		else if (name=="PostMessageW") { post(args[0],uint32_t(args[1]),args[2],args[3]); result=1; }
		// A notification does not wait for the receiver: a broadcast, such as a changed setting announced to every window, needs no
		// delivery here since no other program's windows listen; one window gets it in its queue.
		else if (name=="SendNotifyMessageW") {
			if (args[0]!=0xffff && args[0]!=0xffffffffffffffffULL) post(args[0],uint32_t(args[1]),args[2],args[3]);
			result=1;
		}
		// A message for a thread has no window and goes to that thread's queue.
		else if (name=="PostThreadMessageW") {
			if (messages.size()>=10000) throw uint32_t(1816);
			queue({0,args[2],args[3],uint32_t(args[1]),message_time(),cursor_x,cursor_y,0,uint32_t(args[0])});
			result=1;
		}
		else if (name=="PostQuitMessage") { const auto me=environment.thread_id(); quits[me]=uint32_t(args[0]); queues[me].changed|=0x108; }
		else if (name=="PeekMessageW" || name=="GetMessageW") result=retrieve(args,name=="GetMessageW");
		else if (name=="DispatchMessageW" || name=="TranslateMessage") {
			const auto layout=message_layout(process.mode()==NativeMode::x86);
			const auto bytes=process.read_memory(args[0],layout.size);
			const auto message=uint32_t(read(bytes,layout.message,4));
			if (name=="TranslateMessage") {
				if (message==0x100 || message==0x101 || message==0x104 || message==0x105) {
					// The character comes with the key message the thread retrieved last. Another key message (one the program made, or an
					// earlier one it kept) is accepted as Windows accepts it, without a character to post.
					const bool current=key_translation && key_translation->window==read(bytes,0,layout.pointer) && key_translation->message==message &&
						key_translation->wparam==read(bytes,layout.wparam,layout.pointer) && key_translation->lparam==read(bytes,layout.lparam,layout.pointer) &&
						key_translation->time==read(bytes,layout.time,4);
					const auto key=current ? *key_translation : Message{};
					if (current && (message==0x100 || message==0x104) && key.character) {
						// Posted character messages are retrieved before input still waiting in the queue.
						const auto units=key.character>0xffff ? 2U : 1U;
						const uint32_t kind=message==0x104 ? 0x106U : 0x102U;
						if (messages.size()>10000-units) throw std::runtime_error("guest character queue limit reached");
						queues[environment.thread_id()].changed|=0x108;
						if (units==2) {
							const auto value=key.character-0x10000;
							messages.insert(messages.begin(),Message{key.window,0xd800+(value>>10),key.lparam,kind,key.time,key.x,key.y,0,environment.thread_id()});
							messages.insert(messages.begin()+1,Message{key.window,0xdc00+(value & 0x3ff),key.lparam,kind,key.time,key.x,key.y,0,environment.thread_id()});
						} else messages.insert(messages.begin(),Message{key.window,key.character,key.lparam,kind,key.time,key.x,key.y,0,environment.thread_id()});
					}
					result=1;
				} else if (message>=0x100 && message<=0x109) throw uint32_t(120);
			} else if (read(bytes,0,layout.pointer)) {
				const auto target=read(bytes,0,layout.pointer);
				if (message==0x113 && read(bytes,layout.lparam,layout.pointer)) {
					// A timer with its own callback bypasses the window procedure.
					if (!invoke) throw std::runtime_error("native window callbacks are not connected");
					const uint64_t arguments[]{target,0x113,read(bytes,layout.wparam,layout.pointer),read(bytes,layout.time,4)};
					const auto called=invoke(read(bytes,layout.lparam,layout.pointer),arguments);
					if (called.stop!=NativeStop::returned) throw called;
					result=0;
				} else
				result=send(target,message,read(bytes,layout.wparam,layout.pointer),read(bytes,layout.lparam,layout.pointer));
			}
		}
		else if (registration) {
			const bool extended=name=="RegisterClassExW";
			const bool narrow=process.mode()==NativeMode::x86;
			const size_t P=narrow ? 4 : 8;
			// WNDCLASSW fields follow style at 0; the 32-bit WNDCLASSEXW shifts all of them by its cbSize.
			const size_t shift=narrow && extended ? 4 : 0;
			const size_t size=narrow ? (extended ? 48 : 40) : (extended ? 80 : 72);
			const auto bytes=process.read_memory(args[0],size);
			if (extended && read(bytes,0,4)!=size) throw uint32_t(87);
			const auto style=uint32_t(read(bytes,extended ? 4 : 0,4));
			const auto icon=read(bytes,3*P+8+shift,P),cursor=read(bytes,4*P+8+shift,P),menu_name=read(bytes,6*P+8+shift,P);
			const bool stock_cursor=cursor>=desktop_stock_cursor && cursor<desktop_stock_cursor+desktop_stock_cursor_count;
			if ((icon && !objects.icon(icon)) || (cursor && !stock_cursor)) throw uint32_t(1402);
			const auto class_extra=int32_t(uint32_t(read(bytes,2*P+shift,4))),window_extra=int32_t(uint32_t(read(bytes,2*P+4+shift,4)));
			if (class_extra<0 || window_extra<0) throw uint32_t(87);
			if (class_extra>4096 || window_extra>4096) throw uint32_t(120);
			const auto background=read(bytes,5*P+8+shift,P);
			if (background && background>31 && !objects.brush(background)) throw uint32_t(background<=32 ? 120 : 6);
			const auto procedure=read(bytes,P+shift,P), instance=read(bytes,2*P+8+shift,P);
			auto class_name=text(read(bytes,7*P+8+shift,P));
			if (!procedure || class_name.empty()) throw uint32_t(87);
			// Inside an activation context whose assembly declares the class, it registers under its versioned name.
			if (auto &contexts=activation_contexts(); contexts.current_thread)
				if (auto versioned=contexts.registered_name(contexts.current_thread(),class_name)) class_name=std::move(*versioned);
			if (objects.class_registered(instance,class_name,(style & 0x4000)!=0)) throw uint32_t(1410);
			DesktopClass definition{std::move(class_name),instance,procedure,background,uint32_t(window_extra),std::vector<uint8_t>(size_t(class_extra)),style};
			definition.icon=icon; definition.cursor=cursor;
			if (menu_name && menu_name<=UINT16_MAX) definition.menu_id=uint32_t(menu_name);
			else if (menu_name) definition.menu_name=window_text(menu_name);
			const auto id=objects.register_class(std::move(definition));
			if (!id) throw uint32_t(8);
			result=objects.window_class(id)->atom;
		} else if (creation) result=create(args);
		else if (name=="SendMessageW") result=send(args[0],uint32_t(args[1]),args[2],args[3]);
		// A broadcast, such as a changed setting announced to every window, needs no delivery as no other program's windows listen; a
		// window of the process answers at once, so the time limit never runs out.
		else if (name=="SendMessageTimeoutW") {
			uint64_t answer=0;
			if (args[0]!=0xffff && args[0]!=0xffffffffffffffffULL) {
				if (!objects.window(args[0])) throw uint32_t(1400);
				answer=send(args[0],uint32_t(args[1]),args[2],args[3]);
			}
			if (args[6]) {
				const auto width=process.mode()==NativeMode::x86 ? 4 : 8;
				std::vector<uint8_t> bytes(width);
				for (size_t at=0;at<bytes.size();++at) bytes[at]=uint8_t(answer>>(8*at));
				process.write_memory(args[6],bytes);
			}
			result=1;
		}
		else if (name=="DestroyWindow") result=destroy(args[0]);
		else if (name=="WindowFromDC") {
			// The window a device context belongs to: a window's own context names it, the screen's names the desktop, any other has none.
			const auto *device=objects.dc(args[0]);
			result=device && device->window && objects.window(device->window) ? device->window : 0;
		}
		else if (name=="GetDC" || name=="GetWindowDC") {
			// Parallel guests trap here: the host owns the cache until the guest preludes can publish atomically.
			if (process.parallel() && screen_block && !args[0] && name=="GetDC") {
				auto &state=*reinterpret_cast<uint32_t *>(screen_block+screen_dc_state);
				const auto cached=*reinterpret_cast<uint64_t *>(screen_block+screen_dc);
				if (cached && !state && objects.dc(cached)) {
					state=1;
					process.return_api(cached);
					return true;
				}
			}
			// A null window selects the screen, which is the desktop window's surface.
			const auto target=args[0] ? args[0] : desktop_window();
			if (!objects.window(target)) { result=foreign_dc(target); process.return_api(result); return true; }
			surface_dc(target);
			const auto *window=objects.window(target);
			DesktopDC device{window->bitmap,window->bitmap,target}; device.leased=true;
			objects.configure_window_dc(target,device,name=="GetWindowDC");
			result=objects.add(device);
			if (!result) throw uint32_t(8);
			if (!args[0] && name=="GetDC") publish_screen_dc(target);
		}
		else if (name=="BeginPaint") {
			auto *window=objects.window(args[0]); if (!window) throw uint32_t(1400);
			if (window->painting) throw uint32_t(170);
			const bool narrow=process.mode()==NativeMode::x86;
			const size_t P=narrow ? 4 : 8;
			const auto old=process.read_memory(args[1],narrow ? 64 : 72); process.write_memory(args[1],old);
			const auto paint=start_paint(args[0]);
			std::vector<uint8_t> output(old.size());
			put(output,0,paint.dc,P); put(output,P,paint.erase,4);
			put(output,P+4,uint32_t(paint.rect.left),4); put(output,P+8,uint32_t(paint.rect.top),4);
			put(output,P+12,uint32_t(paint.rect.right),4); put(output,P+16,uint32_t(paint.rect.bottom),4);
			process.write_memory(args[1],output); result=paint.dc;
		} else if (name=="EndPaint") {
			auto *window=objects.window(args[0]); if (!window) throw uint32_t(1400);
			const bool narrow=process.mode()==NativeMode::x86;
			const auto paint=process.read_memory(args[1],narrow ? 64 : 72);
			if (!window->painting || read(paint,0,narrow ? 4 : 8)!=window->dc) throw uint32_t(87);
			finish_paint(args[0]);
			if (objects.window_visible(args[0])) present(args[0]);
			result=1;
		}
		else if (name=="ReleaseDC") {
			const auto owner=args[0] ? args[0] : desktop_window();
			const auto *window=objects.window(owner);
			const auto *device=objects.dc(args[1]);
			// Releasing the context of another process's window hands the frame drawn into it to that process.
			if (!window && device && device->foreign==owner) {
				if (const auto found=foreign_targets.find(owner); found!=foreign_targets.end() && found->second.surface) found->second.surface->publish_frame();
				result=objects.erase(args[1]);
			}
			if (window) {
				if (!device || !device->leased || device->window!=owner) throw uint32_t(1400);
				if (process.parallel() && screen_block && !args[0] &&
					*reinterpret_cast<uint64_t *>(screen_block+screen_dc)==args[1]) {
					auto &state=*reinterpret_cast<uint32_t *>(screen_block+screen_dc_state);
					if (state==1) {
						state=0;
						process.return_api(1);
						return true;
					}
				}
				result=objects.erase(args[1]);
				// The published context comes back here once a call changed it; a fresh one takes its place.
				if (withdraw_screen_dc(args[1])) publish_screen_dc(owner);
			}
		} else if (name=="IsWindowVisible") {
			result=objects.window_visible(args[0]);
		} else if (name=="ShowWindow") {
			result=show_window(args[0],uint32_t(args[1]));
		} else if (name=="UpdateWindow") {
			if (!objects.window(args[0])) throw uint32_t(1400);
			if (!objects.window(args[0])->update.empty()) send(args[0],0x0f,0,0);
			result=1;
		}
		else if (name=="UnregisterClassW") {
			const auto error=objects.unregister_class(atom(args[0],args[1]));
			if (error) throw error;
			result=1;
		} else result=objects.window(args[0])!=nullptr || (name=="IsWindow" && foreign_window_size(args[0]).has_value());
	} catch (uint32_t error) { environment.set_last_error(error); }
	catch (const std::bad_alloc &) { environment.set_last_error(8); }
	catch (const std::runtime_error &failure) {
		const std::string_view message=failure.what();
		if (std::getenv("RAVEN_TRACE_API")) std::cerr << "raven: " << message << "\n";
		if (message.find("readable")==message.npos && message.find("writable")==message.npos) throw;
		environment.set_last_error(998);
	}
	process.return_api(result);
	return true;
}

}
