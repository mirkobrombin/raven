#include "windows.h"
#include "canvas.h"
#include "shapes.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace raven {

namespace {

constexpr uint32_t ws_hscroll = 0x100000, ws_vscroll = 0x200000;
constexpr int32_t arrow_size = 17, minimum_thumb = 8;

// Parts of a scroll bar a pointer can hit.
enum Part { none = 0, line_up = 1, line_down = 2, page_up = 3, page_down = 4, thumb = 5 };

int32_t maximum_position(const DesktopScroll &scroll)
{
	return scroll.max - (scroll.page ? int32_t(scroll.page) - 1 : 0);
}

bool scrollable(const DesktopScroll &scroll)
{
	return scroll.page ? int64_t(scroll.max) - scroll.min + 1 > int64_t(scroll.page) : scroll.max > scroll.min;
}

}

NativeWindows::ScrollGeometry NativeWindows::scroll_geometry(const DesktopWindow &window, int bar) const
{
	ScrollGeometry geometry{};
	const uint32_t flag = bar == 1 ? ws_vscroll : ws_hscroll;
	if (!(window.style & flag))
		return geometry;
	geometry.present = true;
	geometry.vertical = bar == 1;
	const int32_t left = window.x, top = window.y, right = window.x + window.width, bottom = window.y + window.height;
	if (geometry.vertical)
		geometry.bar = {right, top, right + arrow_size, bottom};
	else
		geometry.bar = {left, bottom, right, bottom + arrow_size};
	const auto &box = geometry.bar;
	const int32_t length = geometry.vertical ? box.bottom - box.top : box.right - box.left;
	const int32_t track_length = std::max(0, length - 2 * arrow_size);
	if (geometry.vertical) {
		geometry.up = {box.left, box.top, box.right, box.top + std::min(arrow_size, length / 2)};
		geometry.down = {box.left, box.bottom - std::min(arrow_size, length / 2), box.right, box.bottom};
		geometry.track = {box.left, geometry.up.bottom, box.right, geometry.down.top};
	} else {
		geometry.up = {box.left, box.top, box.left + std::min(arrow_size, length / 2), box.bottom};
		geometry.down = {box.right - std::min(arrow_size, length / 2), box.top, box.right, box.bottom};
		geometry.track = {geometry.up.right, box.top, geometry.down.left, box.bottom};
	}
	const auto &scroll = window.scroll[size_t(bar)];
	if (track_length > 0 && scrollable(scroll)) {
		const int64_t range = int64_t(scroll.max) - scroll.min + 1;
		int32_t thumb_length = scroll.page ? int32_t(int64_t(track_length) * scroll.page / range) : arrow_size;
		thumb_length = std::clamp(thumb_length, minimum_thumb, track_length);
		const int64_t positions = int64_t(maximum_position(scroll)) - scroll.min;
		const int32_t shown = scroll.track != scroll.pos || scroll_drag.window ? scroll.track : scroll.pos;
		const int64_t offset = positions > 0 ? (int64_t(std::clamp(shown, scroll.min, maximum_position(scroll))) - scroll.min) * (track_length - thumb_length) / positions : 0;
		if (geometry.vertical)
			geometry.thumb = {geometry.track.left, geometry.track.top + int32_t(offset), geometry.track.right, geometry.track.top + int32_t(offset) + thumb_length};
		else
			geometry.thumb = {geometry.track.left + int32_t(offset), geometry.track.top, geometry.track.left + int32_t(offset) + thumb_length, geometry.track.bottom};
		geometry.has_thumb = true;
	}
	return geometry;
}

// Re-lays out a window after its scroll bars or frame changed: recompute the client area and repaint.
void NativeWindows::refresh_frame(uint64_t handle)
{
	auto *window = objects.window(handle);
	if (!window)
		return;
	const auto old_width = window->width, old_height = window->height;
	recalculate_client(handle);
	window = objects.window(handle);
	if (!window)
		return;
	if (window->parent)
		objects.update_window_dcs(handle);
	if (window->width != old_width || window->height != old_height)
		send(handle, 5, 0, uint16_t(window->width) | (uint64_t(uint16_t(window->height)) << 16));
	if (objects.window_visible(handle)) {
		invalidate_tree(handle, true);
		drawing(handle);
	}
}

bool NativeWindows::scroll_sync_visibility(uint64_t handle, int bar, bool allow_disable)
{
	auto *window = objects.window(handle);
	const uint32_t flag = bar == 1 ? ws_vscroll : ws_hscroll;
	const bool shown = (window->style & flag) != 0;
	const bool want = scrollable(window->scroll[size_t(bar)]) || allow_disable;
	if (shown == want)
		return false;
	window->style = want ? window->style | flag : window->style & ~flag;
	return true;
}

void NativeWindows::scroll_notify(uint64_t handle, int bar, uint32_t code, int32_t position)
{
	if (!std::as_const(objects).window(handle))
		return;
	send(handle, bar == 1 ? 0x115 : 0x114, uint64_t(code) | (uint64_t(uint16_t(position)) << 16), 0);
}

void NativeWindows::register_scroll_api(ApiRegistry &registry)
{
	const auto bar_of = [](int32_t bar) { return bar == 0 ? 0 : bar == 1 ? 1 : -1; };
	registry.add("SetScrollInfo", 4, [this, bar_of](ApiCall &call) {
		auto *window = objects.window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		const int32_t kind = call.i32(1);
		if (kind == 2) {
			call.ret(send(call[0], 0xe9, call.u32(3), call[2]));
			return;
		}
		const int bar = bar_of(kind);
		if (bar < 0 || !call[2] || call.load<uint32_t>(call[2]) < 28) {
			call.fail(87);
			return;
		}
		const uint32_t mask = call.load<uint32_t>(call[2] + 4);
		auto &scroll = window->scroll[size_t(bar)];
		bool range_changed = false;
		if (mask & 1) {
			scroll.min = call.load<int32_t>(call[2] + 8);
			scroll.max = call.load<int32_t>(call[2] + 12);
			if (scroll.max < scroll.min)
				scroll.max = scroll.min;
			range_changed = true;
		}
		if (mask & 2) {
			scroll.page = call.load<uint32_t>(call[2] + 16);
			range_changed = true;
		}
		if (mask & 4)
			scroll.pos = call.load<int32_t>(call[2] + 20);
		scroll.pos = std::clamp(scroll.pos, scroll.min, std::max(scroll.min, maximum_position(scroll)));
		scroll.track = scroll.pos;
		bool relayout = false;
		if (range_changed || (mask & 8)) {
			const bool keep_disabled = (mask & 8) != 0;
			relayout = scroll_sync_visibility(call[0], bar, keep_disabled && (window->style & (bar == 1 ? ws_vscroll : ws_hscroll)));
			scroll.disabled = !scrollable(scroll) && keep_disabled ? 3 : 0;
		}
		if (relayout)
			refresh_frame(call[0]);
		else if (call.u32(3) && objects.window_visible(call[0]))
			drawing(call[0]);
		call.ret(uint32_t(scroll.pos));
	});
	registry.add("GetScrollInfo", 3, [this, bar_of](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		const int32_t kind = call.i32(1);
		if (kind == 2) {
			call.ret(send(call[0], 0xea, 0, call[2]) != 0);
			return;
		}
		const int bar = bar_of(kind);
		if (bar < 0 || !call[2] || call.load<uint32_t>(call[2]) < 28) {
			call.fail(87);
			return;
		}
		const auto &scroll = window->scroll[size_t(bar)];
		const uint32_t mask = call.load<uint32_t>(call[2] + 4);
		if (mask & 1) {
			call.store<int32_t>(call[2] + 8, scroll.min);
			call.store<int32_t>(call[2] + 12, scroll.max);
		}
		if (mask & 2)
			call.store<uint32_t>(call[2] + 16, scroll.page);
		if (mask & 4)
			call.store<int32_t>(call[2] + 20, scroll.pos);
		if (mask & 0x10)
			call.store<int32_t>(call[2] + 24, scroll.track);
		call.ret(1);
	});
	registry.add("SetScrollPos", 4, [this, bar_of](ApiCall &call) {
		auto *window = objects.window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		if (call.i32(1) == 2) {
			call.ret(send(call[0], 0xe0, uint64_t(call.i32(2)), call.u32(3)));
			return;
		}
		const int bar = bar_of(call.i32(1));
		if (bar < 0) {
			call.fail(87);
			return;
		}
		auto &scroll = window->scroll[size_t(bar)];
		const auto previous = scroll.pos;
		scroll.pos = std::clamp(call.i32(2), scroll.min, std::max(scroll.min, maximum_position(scroll)));
		scroll.track = scroll.pos;
		if (call.u32(3) && objects.window_visible(call[0]))
			drawing(call[0]);
		call.ret(uint32_t(previous));
	});
	registry.add("GetScrollPos", 2, [this, bar_of](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		if (call.i32(1) == 2) {
			call.ret(send(call[0], 0xe1, 0, 0));
			return;
		}
		const int bar = bar_of(call.i32(1));
		if (bar < 0) {
			call.fail(87);
			return;
		}
		call.ret(uint32_t(window->scroll[size_t(bar)].pos));
	});
	registry.add("SetScrollRange", 5, [this, bar_of](ApiCall &call) {
		auto *window = objects.window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		if (call.i32(1) == 2) {
			call.ret(send(call[0], 0xe2, uint64_t(call.i32(2)), uint64_t(call.i32(3))));
			return;
		}
		const int bar = bar_of(call.i32(1));
		if (bar < 0 || call.i32(2) > call.i32(3)) {
			call.fail(87);
			return;
		}
		auto &scroll = window->scroll[size_t(bar)];
		scroll.min = call.i32(2);
		scroll.max = call.i32(3);
		scroll.pos = std::clamp(scroll.pos, scroll.min, std::max(scroll.min, maximum_position(scroll)));
		scroll.track = scroll.pos;
		if (scroll_sync_visibility(call[0], bar, false))
			refresh_frame(call[0]);
		else if (call.u32(4) && objects.window_visible(call[0]))
			drawing(call[0]);
		call.ret(1);
	});
	registry.add("GetScrollRange", 4, [this, bar_of](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		const int bar = bar_of(call.i32(1));
		if (bar < 0 || !call[2] || !call[3]) {
			call.fail(87);
			return;
		}
		call.store<int32_t>(call[2], window->scroll[size_t(bar)].min);
		call.store<int32_t>(call[3], window->scroll[size_t(bar)].max);
		call.ret(1);
	});
	registry.add("ShowScrollBar", 3, [this, bar_of](ApiCall &call) {
		auto *window = objects.window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		const int32_t kind = call.i32(1);
		if (kind == 2) {
			show_window(call[0], call.u32(2) ? 5 : 0);
			call.ret(1);
			return;
		}
		bool changed = false;
		for (int bar = 0; bar < 2; ++bar) {
			if (kind != 3 && bar != bar_of(kind))
				continue;
			const uint32_t flag = bar == 1 ? ws_vscroll : ws_hscroll;
			const bool shown = (window->style & flag) != 0;
			if (shown != (call.u32(2) != 0)) {
				window->style = call.u32(2) ? window->style | flag : window->style & ~flag;
				changed = true;
			}
		}
		if (changed)
			refresh_frame(call[0]);
		call.ret(1);
	});
	registry.add("EnableScrollBar", 3, [this, bar_of](ApiCall &call) {
		auto *window = objects.window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		const int32_t kind = call.i32(1);
		if (kind == 2) {
			call.ret(send(call[0], 0xe4, call.u32(2), 0));
			return;
		}
		bool changed = false;
		for (int bar = 0; bar < 2; ++bar) {
			if (kind != 3 && bar != bar_of(kind))
				continue;
			auto &scroll = window->scroll[size_t(bar)];
			changed = changed || scroll.disabled != (call.u32(2) & 3);
			scroll.disabled = call.u32(2) & 3;
		}
		if (changed && objects.window_visible(call[0]))
			drawing(call[0]);
		call.ret(changed);
	});
	registry.add("GetScrollBarInfo", 3, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		const int32_t object = call.i32(1);
		if (object != -5 && object != -6) {
			call.fail(87);
			return;
		}
		const int bar = object == -5 ? 1 : 0;
		const auto geometry = scroll_geometry(*window, bar);
		std::vector<uint8_t> bytes(60, 0);
		const uint32_t size = 60;
		std::memcpy(bytes.data(), &size, 4);
		const int32_t box[4] = {geometry.bar.left, geometry.bar.top, geometry.bar.right, geometry.bar.bottom};
		std::memcpy(bytes.data() + 4, box, 16);
		const int32_t line_button = arrow_size;
		std::memcpy(bytes.data() + 20, &line_button, 4);
		const int32_t thumb_start = geometry.has_thumb ? (geometry.vertical ? geometry.thumb.top - geometry.bar.top : geometry.thumb.left - geometry.bar.left) : 0;
		const int32_t thumb_end = geometry.has_thumb ? (geometry.vertical ? geometry.thumb.bottom - geometry.bar.top : geometry.thumb.right - geometry.bar.left) : 0;
		std::memcpy(bytes.data() + 24, &thumb_start, 4);
		std::memcpy(bytes.data() + 28, &thumb_end, 4);
		// rgstate: the bar itself, then up, page up, thumb, page down, down; STATE_SYSTEM_INVISIBLE is 0x8000.
		const uint32_t invisible = geometry.present ? 0 : 0x8000;
		for (size_t i = 0; i < 6; ++i)
			std::memcpy(bytes.data() + 36 + i * 4, &invisible, 4);
		call.write(call[2], bytes);
		call.ret(1);
	});
}

// Draws the scroll bars of every window of one top-level surface into its bitmap.
void NativeWindows::paint_scrollbars(uint64_t root)
{
	const auto *top = std::as_const(objects).window(root);
	if (!top || !top->bitmap)
		return;
	auto *bitmap = objects.bitmap(top->bitmap);
	if (!bitmap || bitmap->depth != 32)
		return;
	using namespace paint;
	Canvas canvas{bitmap->pixels().data(), int32_t(bitmap->width), int32_t(bitmap->height)};
	const auto color = [&](int index) { return rgb(objects.system_color(index)); };
	const int32_t origin_x = top->outer.left, origin_y = top->outer.top;
	const auto shifted = [&](DesktopRect rect) { return DesktopRect{rect.left - origin_x, rect.top - origin_y, rect.right - origin_x, rect.bottom - origin_y}; };
	for (const auto handle : objects.window_handles()) {
		const auto *window = std::as_const(objects).window(handle);
		if (!window || !(window->style & (ws_hscroll | ws_vscroll)) || objects.window_root(handle) != root || !objects.window_visible(handle))
			continue;
		// Drawing is clipped to the window's own outer rectangle and the ancestors' client areas.
		for (int bar = 0; bar < 2; ++bar) {
			const auto geometry = scroll_geometry(*window, bar);
			if (!geometry.present)
				continue;
			const auto &scroll = window->scroll[size_t(bar)];
			const bool dead = !scrollable(scroll) || scroll.disabled == 3;
			if (themed()) {
				// Windows 11: a light bar, small solid arrows and a rounded thumb narrower than the bar.
				struct Surface {
					Canvas &canvas;
					void blend(int64_t x, int64_t y, uint32_t ink, double alpha)
					{
						if (x < 0 || y < 0 || x >= canvas.width || y >= canvas.height)
							return;
						const auto *pixel = canvas.pixels + (size_t(y) * size_t(canvas.width) + size_t(x)) * 4;
						const uint32_t below = uint32_t(pixel[2]) | uint32_t(pixel[1]) << 8 | uint32_t(pixel[0]) << 16;
						canvas.set(int32_t(x), int32_t(y), alpha >= 0.998 ? ink : shapes::mix(below, ink, alpha));
					}
				} surface{canvas};
				shapes::Shapes<Surface> shapes{surface};
				const uint32_t ground = 0xf9f9f9, ink = 0x868686, faint = 0xc5c5c5;
				canvas.fill(shifted(geometry.bar), ground);
				const auto arrow = [&](DesktopRect rect, char direction, bool disabled) {
					const auto r = shifted(rect);
					if (r.right > r.left && r.bottom > r.top)
						shapes.triangle((r.left + r.right) / 2.0, (r.top + r.bottom) / 2.0, 7, direction, disabled ? faint : ink);
				};
				arrow(geometry.up, geometry.vertical ? 'u' : 'l', dead || (scroll.disabled & 1));
				arrow(geometry.down, geometry.vertical ? 'd' : 'r', dead || (scroll.disabled & 2));
				if (geometry.has_thumb && !dead) {
					const auto r = shifted(geometry.thumb);
					const bool held = scroll_drag.window == handle && scroll_drag.bar == bar && scroll_drag.part == Part::thumb;
					const uint32_t pill = held ? 0x5a5a5a : 0x868686;
					if (geometry.vertical)
						shapes.rounded((r.left + r.right) / 2.0 - 3, r.top + 2, (r.left + r.right) / 2.0 + 3, r.bottom - 2, 3, pill, std::nullopt);
					else
						shapes.rounded(r.left + 2, (r.top + r.bottom) / 2.0 - 3, r.right - 2, (r.top + r.bottom) / 2.0 + 3, 3, pill, std::nullopt);
				}
				continue;
			}
			// Track: checkerboard of face and highlight, like the classic dithered scroll bar background.
			const auto track = shifted(geometry.track);
			for (int32_t y = track.top; y < track.bottom; ++y)
				for (int32_t x = track.left; x < track.right; ++x)
					canvas.set(x, y, (x + y) % 2 ? color(color_button_highlight) : color(color_button_face));
			const auto button = [&](DesktopRect rect, int direction, bool disabled) {
				const auto r = shifted(rect);
				if (r.right <= r.left || r.bottom <= r.top)
					return;
				canvas.fill(r, color(color_button_face));
				canvas.edge(r, color(color_button_highlight), color(color_dark_shadow), color(color_light), color(color_button_shadow));
				const int32_t cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
				const uint32_t ink = disabled ? color(color_button_shadow) : color(18);
				for (int32_t d = 0; d < 4; ++d)
					for (int32_t w = -(3 - d); w <= 3 - d; ++w)
						switch (direction) {
						case 0: canvas.set(cx + w, cy + 1 - d, ink); break;
						case 1: canvas.set(cx + w, cy - 2 + d, ink); break;
						case 2: canvas.set(cx + 1 - d, cy + w, ink); break;
						default: canvas.set(cx - 2 + d, cy + w, ink); break;
						}
			};
			button(geometry.up, geometry.vertical ? 0 : 2, dead || (scroll.disabled & 1));
			button(geometry.down, geometry.vertical ? 1 : 3, dead || (scroll.disabled & 2));
			if (geometry.has_thumb && !dead) {
				const auto r = shifted(geometry.thumb);
				canvas.fill(r, color(color_button_face));
				canvas.edge(r, color(color_button_highlight), color(color_dark_shadow), color(color_light), color(color_button_shadow));
			}
		}
		// Where both bars meet, the corner box takes the face color.
		if ((window->style & ws_hscroll) && (window->style & ws_vscroll))
			canvas.fill(shifted({window->x + window->width, window->y + window->height, window->x + window->width + arrow_size, window->y + window->height + arrow_size}),
				themed() ? 0xf9f9f9 : color(color_button_face));
	}
}

int NativeWindows::scroll_part_at(const DesktopWindow &window, int bar, int32_t x, int32_t y) const
{
	const auto geometry = scroll_geometry(window, bar);
	const auto inside = [&](const DesktopRect &r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
	if (!geometry.present || !inside(geometry.bar))
		return none;
	if (inside(geometry.up))
		return line_up;
	if (inside(geometry.down))
		return line_down;
	if (geometry.has_thumb && inside(geometry.thumb))
		return thumb;
	if (!inside(geometry.track) || !scrollable(window.scroll[size_t(bar)]))
		return none;
	const bool before = geometry.vertical ? y < geometry.thumb.top : x < geometry.thumb.left;
	return before ? page_up : page_down;
}

// Hit test for the non-client scroll bars: HTHSCROLL (6) or HTVSCROLL (7), zero elsewhere.
uint64_t NativeWindows::scroll_hit(const DesktopWindow &window, int32_t x, int32_t y) const
{
	for (int bar = 0; bar < 2; ++bar) {
		const auto geometry = scroll_geometry(window, bar);
		if (geometry.present && x >= geometry.bar.left && x < geometry.bar.right && y >= geometry.bar.top && y < geometry.bar.bottom)
			return bar == 1 ? 7 : 6;
	}
	return 0;
}

bool NativeWindows::scroll_pointer(uint64_t handle, uint64_t hit, uint32_t message, int32_t x, int32_t y)
{
	auto *window = objects.window(handle);
	if (!window || (hit != 6 && hit != 7))
		return false;
	if (message != 0x201)
		return true;
	const int bar = hit == 7 ? 1 : 0;
	const int part = scroll_part_at(*window, bar, x, y);
	if (part == none)
		return true;
	auto &scroll = window->scroll[size_t(bar)];
	if (!scrollable(scroll) || scroll.disabled == 3)
		return true;
	const auto geometry = scroll_geometry(*window, bar);
	scroll_drag = {handle, bar, part, 0, std::chrono::steady_clock::now() + std::chrono::milliseconds(400)};
	if (part == thumb) {
		scroll_drag.grab = geometry.vertical ? y - geometry.thumb.top : x - geometry.thumb.left;
		scroll.track = scroll.pos;
	} else {
		static constexpr uint32_t codes[] = {0, 0, 1, 2, 3};
		scroll_notify(handle, bar, codes[part] , scroll.pos);
	}
	return true;
}

// Pointer motion and release while a scroll bar is being operated; returns true when the event was consumed.
bool NativeWindows::scroll_motion(uint32_t message, int32_t x, int32_t y)
{
	if (!scroll_drag.window)
		return false;
	auto *window = objects.window(scroll_drag.window);
	if (!window) {
		scroll_drag = {};
		return true;
	}
	const auto handle = scroll_drag.window;
	const int bar = scroll_drag.bar;
	auto &scroll = window->scroll[size_t(bar)];
	if (message == 0x202) {
		const bool was_thumb = scroll_drag.part == thumb;
		scroll_drag = {};
		if (was_thumb)
			scroll_notify(handle, bar, 4, scroll.track);
		scroll_notify(handle, bar, 8, scroll.pos);
		scroll.track = scroll.pos;
		if (objects.window_visible(handle))
			drawing(handle);
		return true;
	}
	if (scroll_drag.part == thumb) {
		const auto geometry = scroll_geometry(*window, bar);
		const int32_t thumb_length = geometry.vertical ? geometry.thumb.bottom - geometry.thumb.top : geometry.thumb.right - geometry.thumb.left;
		const int32_t track_length = geometry.vertical ? geometry.track.bottom - geometry.track.top : geometry.track.right - geometry.track.left;
		const int32_t start = geometry.vertical ? geometry.track.top : geometry.track.left;
		const int32_t cursor = geometry.vertical ? y : x;
		const int64_t slack = std::max(1, track_length - thumb_length);
		const int64_t offset = std::clamp<int64_t>(cursor - scroll_drag.grab - start, 0, slack);
		const int64_t positions = int64_t(maximum_position(scroll)) - scroll.min;
		scroll.track = scroll.min + int32_t(offset * positions / slack);
		scroll_notify(handle, bar, 5, scroll.track);
		if (objects.window_visible(handle))
			drawing(handle);
	}
	return true;
}

// Holding an arrow or the page area repeats its scroll code.
void NativeWindows::scroll_idle()
{
	if (!scroll_drag.window || scroll_drag.part == thumb)
		return;
	const auto now = std::chrono::steady_clock::now();
	if (now < scroll_drag.next)
		return;
	scroll_drag.next = now + std::chrono::milliseconds(50);
	const auto *window = std::as_const(objects).window(scroll_drag.window);
	if (!window) {
		scroll_drag = {};
		return;
	}
	static constexpr uint32_t codes[] = {0, 0, 1, 2, 3};
	scroll_notify(scroll_drag.window, scroll_drag.bar, codes[scroll_drag.part], window->scroll[size_t(scroll_drag.bar)].pos);
}

}
