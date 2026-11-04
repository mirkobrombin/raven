#include "windows.h"
#include <unistd.h>
#include "nt/system_exports.h"
#include "nt/child_process.h"
#include "nt/text.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <cstring>
#include <sys/mman.h>
#include <utility>

namespace raven {

namespace {

constexpr uint32_t ws_minimize = 0x20000000U, ws_maximize = 0x01000000U, ws_disabled = 0x08000000U, ws_visible = 0x10000000U;
constexpr uint32_t swp_nosize = 1, swp_nomove = 2, swp_nozorder = 4, swp_noredraw = 8, swp_noactivate = 0x10, swp_showwindow = 0x40,
	swp_hidewindow = 0x80, swp_nosendchanging = 0x400;

struct Layout {
	size_t size, pointer, flags;
};

// WINDOWPOS: two handles, the position and size as four ints, then the flags; 64-bit programs pad it to 40 bytes.
Layout position_layout(bool narrow)
{
	return narrow ? Layout{28, 4, 24} : Layout{40, 8, 32};
}

}

std::vector<uint64_t> NativeWindows::top_level_windows() const
{
	std::vector<uint64_t> result;
	for (const auto handle : objects.child_windows(0))
		if (handle != desktop)
			result.push_back(handle);
	return result;
}

uint64_t NativeWindows::window_at(int32_t x, int32_t y)
{
	auto roots = top_level_windows();
	for (auto root = roots.rbegin(); root != roots.rend(); ++root) {
		const auto *record = std::as_const(objects).window(*root);
		if (record && record->visible && x >= record->outer.left && y >= record->outer.top && x < record->outer.right && y < record->outer.bottom)
			return pointer_child(*root, x, y);
	}
	return 0;
}

void NativeWindows::shift_window(uint64_t handle, int32_t dx, int32_t dy)
{
	auto *window = objects.window(handle);
	if (!window)
		return;
	window->x += dx;
	window->y += dy;
	window->outer.left += dx;
	window->outer.right += dx;
	window->outer.top += dy;
	window->outer.bottom += dy;
	for (const auto child : objects.child_windows(handle))
		shift_window(child, dx, dy);
}

void NativeWindows::invalidate_tree(uint64_t handle, bool erase)
{
	const auto *window = std::as_const(objects).window(handle);
	if (!window)
		return;
	objects.invalidate(handle, {0, 0, window->width, window->height}, erase);
	for (const auto child : objects.child_windows(handle))
		invalidate_tree(child, erase);
}

bool NativeWindows::enable_window(uint64_t handle, bool enable)
{
	auto *window = objects.window(handle);
	if (!window)
		throw uint32_t(1400);
	const bool was_disabled = window->style & ws_disabled;
	if (was_disabled == !enable)
		return was_disabled;
	window->style = enable ? window->style & ~ws_disabled : window->style | ws_disabled;
	if (!enable) {
		// A disabled window cannot keep the keyboard or the mouse.
		if (focus == handle || objects.is_child(handle, focus))
			set_focus(0);
		if (capture == handle) {
			capture = 0;
			send(handle, 0x215, 0, 0);
		}
	}
	if (std::as_const(objects).window(handle))
		send(handle, 0x0a, enable, 0);
	return was_disabled;
}

bool NativeWindows::show_window(uint64_t handle, uint32_t command)
{
	auto *window = objects.window(handle);
	if (!window)
		throw uint32_t(1400);
	const bool previous = window->visible;
	if (command > 11)
		throw uint32_t(87);
	const bool hide = command == 0;
	const bool minimize = command == 2 || command == 6 || command == 7 || command == 11;
	const bool maximize = command == 3;
	const bool activate_it = command == 1 || command == 3 || command == 5 || command == 9 || command == 10 || command == 2;
	if (!window->parent && (minimize || maximize || command == 9 || command == 1 || command == 10)) {
		// Maximizing fills the work area; restoring brings back the rectangle remembered when the state first changed.
		const auto &screen = objects.nonclient_metrics();
		const bool is_max = window->style & ws_maximize, is_min = window->style & ws_minimize;
		if (maximize && !is_max) {
			if (!is_min)
				window->restore = window->outer;
			window->style = (window->style & ~ws_minimize) | ws_maximize;
			const auto frame = adjust_window_rect(objects.nonclient_metrics(), {0, 0, 0, 0}, window->style & ~ws_visible, window->menu != 0, window->ex_style);
			(void)frame;
			reshape(handle, {0, 0, screen.screen_x, screen.screen_y});
		} else if (minimize && !is_min) {
			if (!is_max)
				window->restore = window->outer;
			window->style = (window->style & ~ws_maximize) | ws_minimize;
		} else if ((command == 9 || command == 1 || command == 10) && (is_max || is_min)) {
			window->style &= ~(ws_maximize | ws_minimize);
			const auto target = window->restore.right > window->restore.left ? window->restore : window->outer;
			if (is_max)
				reshape(handle, target);
		}
		window = objects.window(handle);
	}
	const bool show = !hide && !(window->style & ws_minimize && !window->parent);
	if (show && (!window->parent || objects.window_visible(window->parent)))
		present(handle);
	else if (!hide && (window->style & ws_minimize) && previous && !window->parent && presentation.minimize)
		presentation.minimize(handle);
	else if ((!show || (window->style & ws_minimize)) && previous && !window->parent && presentation.remove)
		remove_host_window(handle);
	window = objects.window(handle);
	window->visible = !hide;
	window->style = (window->style & ~ws_visible) | (window->visible ? ws_visible : 0);
	objects.update_window_dcs(window->parent ? window->parent : handle);
	if (!window->parent) {
		publish_traits(handle);
		window = objects.window(handle);
	}
	if (show) {
		// Showing a window paints its whole subtree, so the children's update regions are invalidated with it.
		if (!objects.invalidate(handle, {0, 0, window->width, window->height}, true))
			throw uint32_t(8);
		for (const auto child : objects.child_windows(handle))
			invalidate_tree(child, true);
		window = objects.window(handle);
	}
	if (window->visible && !show)
		objects.update_window_dcs(handle);
	if (show && !window->parent && activate_it && std::as_const(objects).window(handle) && !(window->ex_style & 0x08000000U))
		set_focus(handle);
	window = objects.window(handle);
	if (!window)
		throw uint32_t(1400);
	if (!window->visible && previous && window->parent) {
		const auto *parent = std::as_const(objects).window(window->parent);
		objects.invalidate(window->parent, {window->outer.left - parent->x, window->outer.top - parent->y, window->outer.right - parent->x,
			window->outer.bottom - parent->y}, true);
	}
	send(handle, 0x18, window->visible, 0);
	// The size and position messages an overlapped window did not get while it was created come with its first showing.
	window = objects.window(handle);
	if (window && show && window->need_size) {
		window->need_size = false;
		const uint64_t kind = (window->style & ws_maximize) ? 2 : (window->style & ws_minimize) ? 1 : 0;
		send(handle, 5, kind, uint16_t(window->width) | (uint64_t(uint16_t(window->height)) << 16));
		window = objects.window(handle);
		if (window)
			send(handle, 3, 0, uint16_t(window->x) | (uint64_t(uint16_t(window->y)) << 16));
	}
	return previous;
}

void NativeWindows::place_window(uint64_t handle, DesktopRect outer, uint64_t insert_after, uint32_t flags)
{
	auto *window = objects.window(handle);
	if (!window)
		throw uint32_t(1400);
	const auto layout = position_layout(process.mode() == NativeMode::x86);
	const auto relative = [&](const DesktopRect &rect) {
		const auto *parent = window->parent ? objects.window(window->parent) : nullptr;
		const int32_t ox = parent ? parent->x : 0, oy = parent ? parent->y : 0;
		return DesktopRect{rect.left - ox, rect.top - oy, rect.right - ox, rect.bottom - oy};
	};
	// WM_WINDOWPOSCHANGING lets the window veto or adjust the change before it happens.
	auto requested = relative(outer);
	uint32_t effective = flags;
	uint64_t after = insert_after;
	if (!(flags & swp_nosendchanging)) {
		const auto scratch = process.map_memory(4096, PROT_READ | PROT_WRITE);
		try {
			std::vector<uint8_t> bytes(layout.size);
			const auto store = [&](size_t at, uint64_t value, size_t width) { std::memcpy(bytes.data() + at, &value, width); };
			store(0, handle, layout.pointer);
			store(layout.pointer, insert_after, layout.pointer);
			store(layout.pointer * 2, uint32_t(requested.left), 4);
			store(layout.pointer * 2 + 4, uint32_t(requested.top), 4);
			store(layout.pointer * 2 + 8, uint32_t(requested.right - requested.left), 4);
			store(layout.pointer * 2 + 12, uint32_t(requested.bottom - requested.top), 4);
			store(layout.flags, flags, 4);
			process.write_memory(scratch, bytes);
			send(handle, 0x46, 0, scratch);
			if (!std::as_const(objects).window(handle)) {
				process.unmap_memory(scratch);
				return;
			}
			const auto changed = process.read_memory(scratch, layout.size);
			const auto load = [&](size_t at, size_t width) { uint64_t value = 0; std::memcpy(&value, changed.data() + at, width); return value; };
			after = load(layout.pointer, layout.pointer);
			const int32_t x = int32_t(load(layout.pointer * 2, 4)), y = int32_t(load(layout.pointer * 2 + 4, 4));
			const int32_t width = int32_t(load(layout.pointer * 2 + 8, 4)), height = int32_t(load(layout.pointer * 2 + 12, 4));
			effective = uint32_t(load(layout.flags, 4));
			requested = {x, y, x + width, y + height};
		} catch (...) {
			process.unmap_memory(scratch);
			throw;
		}
		process.unmap_memory(scratch);
		window = objects.window(handle);
		if (!window)
			return;
		const auto *parent = window->parent ? objects.window(window->parent) : nullptr;
		const int32_t ox = parent ? parent->x : 0, oy = parent ? parent->y : 0;
		outer = {requested.left + ox, requested.top + oy, requested.right + ox, requested.bottom + oy};
	}
	if (!(effective & swp_nozorder) && after != UINT64_MAX - 1 + 0) {
		const bool top = after == 0 || after == UINT64_MAX || after == UINT64_MAX - 1 + 0;
		if (after == 1)
			objects.restack(handle, false);
		else if (top || !std::as_const(objects).window(after))
			objects.restack(handle, true);
		else {
			objects.restack(handle, true);
			// Insert directly above the named sibling: every sibling above it climbs over this window.
			const auto *sibling = std::as_const(objects).window(after);
			auto *self = objects.window(handle);
			if (sibling && self) {
				self->order = sibling->order;
				for (const auto other : objects.child_windows(window->parent))
					if (other != handle && std::as_const(objects).window(other)->order >= sibling->order && other != after)
						++objects.window(other)->order;
				++self->order;
			}
		}
		window = objects.window(handle);
	}
	const bool moving = !(effective & swp_nomove), sizing = !(effective & swp_nosize);
	const DesktopRect old = window->outer;
	const bool was_visible = objects.window_visible(handle);
	DesktopRect target = old;
	if (moving) {
		target.right = target.left + (old.right - old.left);
		target.bottom = target.top + (old.bottom - old.top);
		target.left = outer.left;
		target.top = outer.top;
		target.right = target.left + (old.right - old.left);
		target.bottom = target.top + (old.bottom - old.top);
	}
	if (sizing) {
		target.right = target.left + (outer.right - outer.left);
		target.bottom = target.top + (outer.bottom - outer.top);
	}
	const bool shifted = target.left != old.left || target.top != old.top;
	const bool resized = target.right - target.left != old.right - old.left || target.bottom - target.top != old.bottom - old.top;
	if (target.right - target.left < 0 || target.bottom - target.top < 0 || target.right - target.left > 8192 || target.bottom - target.top > 8192)
		throw uint32_t(87);
	if (shifted && !resized && !window->parent) {
		shift_window(handle, target.left - old.left, target.top - old.top);
	} else if (shifted || resized) {
		const auto old_client_x = window->x, old_client_y = window->y;
		if (window->parent && was_visible) {
			const auto *parent = std::as_const(objects).window(window->parent);
			objects.invalidate(window->parent, {old.left - parent->x, old.top - parent->y, old.right - parent->x, old.bottom - parent->y}, true);
		}
		if (window->parent || !resized) {
			if (shifted)
				shift_window(handle, target.left - old.left, target.top - old.top);
			if (resized) {
				window = objects.window(handle);
				const int32_t client_width = window->width, client_height = window->height;
				const std::pair<int32_t, int32_t> laid_out{target.left + window->x - window->outer.left, target.top + window->y - window->outer.top};
				window->outer = target;
				window->x = target.left;
				window->y = target.top;
				window->width = target.right - target.left;
				window->height = target.bottom - target.top;
				recalculate_client(handle, client_width, client_height, laid_out);
			}
		} else {
			reshape(handle, target);
		}
		window = objects.window(handle);
		if (!window)
			return;
		const bool client_moved = window->x != old_client_x || window->y != old_client_y;
		if (window->parent && client_moved && !resized) {
			// Moving a child in its parent changes where its whole subtree draws.
			objects.update_window_dcs(handle);
			const auto *parent = std::as_const(objects).window(window->parent);
			send(handle, 3, 0, uint16_t(window->x - parent->x) | (uint64_t(uint16_t(window->y - parent->y)) << 16));
		}
		window = objects.window(handle);
		if (window && window->parent && objects.window_visible(handle) && !(effective & swp_noredraw)) {
			objects.update_window_dcs(window->parent);
			invalidate_tree(handle, true);
		}
	}
	window = objects.window(handle);
	if (!window)
		return;
	// SWP_FRAMECHANGED asks the window for its client area again, as a program that draws its own frame does after it changes.
	if ((effective & 0x20) && !(shifted || resized)) {
		recalculate_client(handle);
		window = objects.window(handle);
		if (!window)
			return;
	}
	if (effective & swp_showwindow)
		show_window(handle, 5);
	else if (effective & swp_hidewindow)
		show_window(handle, 0);
	if (!std::as_const(objects).window(handle))
		return;
	// WM_WINDOWPOSCHANGED tells the window what actually happened; the default procedure turns it into WM_MOVE and WM_SIZE.
	const auto scratch = process.map_memory(4096, PROT_READ | PROT_WRITE);
	try {
		window = objects.window(handle);
		const auto rel = relative(window->outer);
		std::vector<uint8_t> bytes(layout.size);
		const auto store = [&](size_t at, uint64_t value, size_t width) { std::memcpy(bytes.data() + at, &value, width); };
		store(0, handle, layout.pointer);
		store(layout.pointer, after, layout.pointer);
		store(layout.pointer * 2, uint32_t(rel.left), 4);
		store(layout.pointer * 2 + 4, uint32_t(rel.top), 4);
		store(layout.pointer * 2 + 8, uint32_t(rel.right - rel.left), 4);
		store(layout.pointer * 2 + 12, uint32_t(rel.bottom - rel.top), 4);
		store(layout.flags, (effective & ~(swp_nomove | swp_nosize)) | (shifted ? 0 : swp_nomove) | (resized ? 0 : swp_nosize), 4);
		process.write_memory(scratch, bytes);
		send(handle, 0x47, 0, scratch);
	} catch (...) {
		process.unmap_memory(scratch);
		throw;
	}
	process.unmap_memory(scratch);
}

uint64_t NativeWindows::foreign_top_level(const std::u16string &class_name, bool titled, const std::u16string &title) const
{
	for (const auto handle : foreign_windows()) {
		const auto traits = foreign_window_traits(handle);
		if (!traits || traits->parent)
			continue;
		if (!class_name.empty() && !equal_fold(from_utf8(traits->class_name), class_name))
			continue;
		if (titled && from_utf8(traits->title) != title)
			continue;
		return handle;
	}
	return 0;
}

void NativeWindows::register_manager_api(ApiRegistry &registry)
{
	registry.add("FindWindowW", 2, [this](ApiCall &call) {
		const auto name = call[0] && call[0] > 0xffff ? call.wide(call[0], 256) : std::u16string();
		const auto title = call[1] ? call.wide(call[1], 4096) : std::u16string();
		auto roots = top_level_windows();
		for (auto root = roots.rbegin(); root != roots.rend(); ++root) {
			const auto *window = std::as_const(objects).window(*root);
			const auto *definition = window ? objects.window_class(window->class_id) : nullptr;
			if (!definition || window->destroying)
				continue;
			if (!name.empty() && !equal_fold(from_utf8(definition->name), name))
				continue;
			if (call[1] && window->title != title)
				continue;
			call.ret(*root);
			return;
		}
		if (const auto found = foreign_top_level(name, call[1] != 0, title)) {
			call.ret(found);
			return;
		}
		call.fail(1400);
	});
	registry.add("FindWindowA", 2, [this](ApiCall &call) {
		const auto narrow_name = call[0] && call[0] > 0xffff ? call.narrow(call[0], 256) : std::string();
		const auto narrow_title = call[1] ? call.narrow(call[1], 4096) : std::string();
		const std::u16string name(narrow_name.begin(), narrow_name.end()), title(narrow_title.begin(), narrow_title.end());
		auto roots = top_level_windows();
		for (auto root = roots.rbegin(); root != roots.rend(); ++root) {
			const auto *window = std::as_const(objects).window(*root);
			const auto *definition = window ? objects.window_class(window->class_id) : nullptr;
			if (!definition || window->destroying)
				continue;
			if (!name.empty() && !equal_fold(from_utf8(definition->name), name))
				continue;
			if (call[1] && window->title != title)
				continue;
			call.ret(*root);
			return;
		}
		if (const auto found = foreign_top_level(name, call[1] != 0, title)) {
			call.ret(found);
			return;
		}
		call.fail(1400);
	});
	registry.add("FindWindowExW", 4, [this](ApiCall &call) {
		const auto name = call[2] && call[2] > 0xffff ? call.wide(call[2], 256) : std::u16string();
		const auto title = call[3] ? call.wide(call[3], 4096) : std::u16string();
		const auto siblings = objects.child_windows(call[0]);
		bool started = call[1] == 0;
		for (auto candidate = siblings.rbegin(); candidate != siblings.rend(); ++candidate) {
			if (!started) {
				started = *candidate == call[1];
				continue;
			}
			const auto *window = std::as_const(objects).window(*candidate);
			const auto *definition = window ? objects.window_class(window->class_id) : nullptr;
			if (!definition || (!name.empty() && !equal_fold(from_utf8(definition->name), name)))
				continue;
			if (call[3] && window->title != title)
				continue;
			call.ret(*candidate);
			return;
		}
		call.fail(1400);
	});
	registry.add("GetAncestor", 2, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		switch (call.u32(1)) {
		case 1: call.ret(window->parent); break;
		case 2: call.ret(objects.window_root(call[0])); break;
		case 3: {
			auto root = objects.window_root(call[0]);
			while (objects.window(root) && std::as_const(objects).window(root)->owner && std::as_const(objects).window(objects.window(root)->owner))
				root = objects.window_root(objects.window(root)->owner);
			call.ret(root);
			break;
		}
		default: call.fail(87);
		}
	});
	registry.add("GetWindow", 2, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		const auto siblings = objects.child_windows(window->parent);
		const auto position = std::find(siblings.begin(), siblings.end(), call[0]);
		switch (call.u32(1)) {
		case 0: call.ret(siblings.empty() ? 0 : siblings.back()); break;
		case 1: call.ret(siblings.empty() ? 0 : siblings.front()); break;
		case 2: call.ret(position == siblings.begin() ? 0 : *(position - 1)); break;
		case 3: call.ret(position == siblings.end() || position + 1 == siblings.end() ? 0 : *(position + 1)); break;
		case 4: call.ret(window->owner); break;
		case 5: {
			const auto children = objects.child_windows(call[0]);
			call.ret(children.empty() ? 0 : children.back());
			break;
		}
		default: call.fail(87);
		}
	});
	registry.add("SetParent", 2, [this](ApiCall &call) {
		auto *window = objects.window(call[0]);
		// HWND_MESSAGE takes the window off the desktop: it stays a top-level window that only receives messages.
		const bool message_only = call[1] == UINT64_MAX - 2 || call[1] == 0xfffffffdULL;
		const uint64_t target = message_only ? 0 : call[1];
		if (!window || (target && !std::as_const(objects).window(target))) {
			call.fail(1400);
			return;
		}
		if (target == call[0] || objects.is_child(call[0], target)) {
			call.fail(87);
			return;
		}
		const auto previous = window->parent;
		const auto *old_parent = previous ? objects.window(previous) : nullptr;
		const int32_t old_x = old_parent ? old_parent->x : 0, old_y = old_parent ? old_parent->y : 0;
		const auto *new_parent = target ? objects.window(target) : nullptr;
		const int32_t new_x = new_parent ? new_parent->x : 0, new_y = new_parent ? new_parent->y : 0;
		// Surfaces belong to the root a window draws on, so a move between roots drops every surface of the subtree.
		const auto drop_surfaces = [&](auto &&self, uint64_t node) -> void {
			for (const auto child : objects.child_windows(node))
				self(self, child);
			release_surface(node);
			if (auto *record = objects.window(node))
				record->bitmap = record->dc = 0;
		};
		if (previous != target || message_only)
			drop_surfaces(drop_surfaces, call[0]);
		window = objects.window(call[0]);
		window->parent = target;
		window->shown_on = !target && previous ? objects.window_root(previous) : 0;
		if (target) window->style = (window->style & ~0x80000000U) | 0x40000000U;
		else window->style = (window->style & ~0x40000000U) | 0x80000000U;
		if (message_only) window->style &= ~0x10000000U;
		shift_window(call[0], new_x - old_x, new_y - old_y);
		objects.restack(call[0], true);
		objects.update_window_dcs(call[0]);
		call.ret(previous);
	});
	registry.add("BringWindowToTop", 1, [this](ApiCall &call) {
		if (!std::as_const(objects).window(call[0])) {
			call.fail(1400);
			return;
		}
		objects.restack(call[0], true);
		const auto *window = std::as_const(objects).window(call[0]);
		if (window->parent && objects.window_visible(call[0]))
			invalidate_tree(call[0], true);
		call.ret(1);
	});
	registry.add("SetWindowDisplayAffinity", 2, [this](ApiCall &call) {
		auto *window = objects.window(call[0]);
		if (!window || window->parent) {
			call.fail(window ? 87 : 1400);
			return;
		}
		if (call.u32(1) != 0 && call.u32(1) != 1 && call.u32(1) != 0x11) {
			call.fail(87);
			return;
		}
		window->display_affinity = call.u32(1);
		call.ret(1);
	});
	registry.add("GetWindowDisplayAffinity", 2, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window || !call[1]) {
			call.fail(window ? 87 : 1400);
			return;
		}
		call.store<uint32_t>(call[1], window->display_affinity);
		call.ret(1);
	});
	// Windows here are never snapped into an arrangement.
	registry.add("IsWindowArranged", 1, [](ApiCall &call) { call.ret(0); });
	registry.add("IsIconic", 1, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		call.ret(window && (window->style & ws_minimize));
	});
	registry.add("IsZoomed", 1, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		call.ret(window && (window->style & ws_maximize));
	});
	registry.add("IsWindowEnabled", 1, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		call.ret(window && !(window->style & ws_disabled));
	});
	registry.add("EnableWindow", 2, [this](ApiCall &call) {
		try {
			call.ret(enable_window(call[0], call.u32(1)));
		} catch (const uint32_t &error) {
			call.fail(error);
		}
	});
	registry.add("GetForegroundWindow", 0, [this](ApiCall &call) { call.ret(active); });
	// Native windows do not impose a foreground lock; every live runtime process can activate its own windows.
	registry.add("AllowSetForegroundWindow", 1, [](ApiCall &call) {
		if (call.u32(0) != UINT32_MAX && !runtime_process_alive(pid_t(call.u32(0)))) {
			call.fail(87);
			return;
		}
		call.ret(1);
	});
	registry.add("SetForegroundWindow", 1, [this](ApiCall &call) {
		if (!std::as_const(objects).window(call[0])) {
			call.fail(1400);
			return;
		}
		set_focus(objects.window_root(call[0]));
		call.ret(1);
	});
	registry.add("GetLastActivePopup", 1, [](ApiCall &call) { call.ret(call[0]); });
	registry.add("SetActiveWindow", 1, [this](ApiCall &call) {
		if (!std::as_const(objects).window(call[0])) {
			call.fail(1400);
			return;
		}
		const auto previous = active;
		activate(call[0]);
		call.ret(previous);
	});
	registry.add("AttachThreadInput", 3, [](ApiCall &call) { call.ret(1); });
	registry.add("GetWindowThreadProcessId", 2, [this](ApiCall &call) {
		if (!std::as_const(objects).window(call[0])) {
			// A window of another process: its creating thread stays with the owner, whose first thread stands for it.
			if (const auto owner = foreign_window_owner(call[0])) {
				if (call[1]) call.store<uint32_t>(call[1], *owner);
				call.ret(*owner);
				return;
			}
			call.fail(1400);
			return;
		}
		if (call[1]) call.store<uint32_t>(call[1], uint32_t(::getpid()));
		const auto *window = std::as_const(objects).window(call[0]);
		call.ret(window->thread ? window->thread : environment.thread_id());
	});
	registry.add("ChildWindowFromPoint", 2, [this](ApiCall &call) {
		const auto *parent = std::as_const(objects).window(call[0]);
		if (!parent) {
			call.fail(1400);
			return;
		}
		const int32_t x = int32_t(call[1]) + parent->x, y = int32_t(call[1] >> 32) + parent->y;
		uint64_t found = call[0];
		const auto children = objects.child_windows(call[0]);
		for (auto child = children.rbegin(); child != children.rend(); ++child) {
			const auto *record = std::as_const(objects).window(*child);
			if (record && x >= record->outer.left && y >= record->outer.top && x < record->outer.right && y < record->outer.bottom) {
				found = *child;
				break;
			}
		}
		call.ret(found);
	});
	registry.add("ChildWindowFromPointEx", 3, [this](ApiCall &call) {
		const auto *parent = std::as_const(objects).window(call[0]);
		if (!parent) {
			call.fail(1400);
			return;
		}
		const int32_t x = int32_t(call[1]) + parent->x, y = int32_t(call[1] >> 32) + parent->y;
		uint64_t found = call[0];
		const uint32_t skip = call.u32(2);
		const auto children = objects.child_windows(call[0]);
		for (auto child = children.rbegin(); child != children.rend(); ++child) {
			const auto *record = std::as_const(objects).window(*child);
			if (!record || x < record->outer.left || y < record->outer.top || x >= record->outer.right || y >= record->outer.bottom)
				continue;
			if (((skip & 1) && !record->visible) || ((skip & 2) && (record->style & ws_disabled)) || ((skip & 4) && (record->ex_style & 0x20)))
				continue;
			found = *child;
			break;
		}
		call.ret(found);
	});
	// RealChildWindowFromPoint looks through what is transparent to the mouse, as a group box is: a child that answers WM_NCHITTEST with
	// HTTRANSPARENT gives way to the one below it.
	registry.add("RealChildWindowFromPoint", 2, [this](ApiCall &call) {
		const auto *parent = std::as_const(objects).window(call[0]);
		if (!parent) {
			call.fail(1400);
			return;
		}
		const int32_t x = int32_t(call[1]) + parent->x, y = int32_t(call[1] >> 32) + parent->y;
		uint64_t found = call[0];
		const auto children = objects.child_windows(call[0]);
		for (auto child = children.rbegin(); child != children.rend(); ++child) {
			const auto *record = std::as_const(objects).window(*child);
			if (!record || !record->visible || x < record->outer.left || y < record->outer.top || x >= record->outer.right || y >= record->outer.bottom)
				continue;
			if (int32_t(send(*child, 0x84, 0, uint32_t(uint16_t(x)) | uint32_t(uint16_t(y)) << 16)) == -1)
				continue;
			found = *child;
			break;
		}
		call.ret(found);
	});
	registry.add("WindowFromPoint", 1, [this](ApiCall &call) { call.ret(window_at(int32_t(call[0]), int32_t(call[0] >> 32))); });
	const auto enumerate = [this](ApiCall &call, const std::vector<uint64_t> &handles, uint64_t procedure, uint64_t parameter) {
		for (const auto handle : handles) {
			if (!std::as_const(objects).window(handle) && !foreign_window_size(handle))
				continue;
			const uint64_t arguments[] = {handle, parameter};
			const auto result = invoke(procedure, arguments);
			if (result.stop != NativeStop::returned)
				throw result;
			if (!uint32_t(result.value))
				break;
		}
		call.ret(1);
	};
	registry.add("EnumWindows", 2, [this, enumerate](ApiCall &call) {
		auto roots = top_level_windows();
		std::reverse(roots.begin(), roots.end());
		// The top-level windows of the other processes of the prefix follow, as one desktop holds them all on Windows.
		for (const auto handle : foreign_windows())
			if (const auto traits = foreign_window_traits(handle); traits && !traits->parent)
				roots.push_back(handle);
		enumerate(call, roots, call[0], call[1]);
	});
	registry.add("EnumThreadWindows", 3, [this, enumerate](ApiCall &call) {
		auto roots = top_level_windows();
		std::reverse(roots.begin(), roots.end());
		enumerate(call, roots, call[1], call[2]);
	});
	registry.add("EnumChildWindows", 3, [this, enumerate](ApiCall &call) {
		std::vector<uint64_t> all;
		const auto collect = [&](auto &&self, uint64_t parent) -> void {
			auto children = objects.child_windows(parent);
			std::reverse(children.begin(), children.end());
			for (const auto child : children) {
				all.push_back(child);
				self(self, child);
			}
		};
		collect(collect, call[0]);
		enumerate(call, all, call[1], call[2]);
	});

	// A property is named by an atom or by text, wide or ANSI, compared without case.
	const auto property_key = [](ApiCall &call, bool wide) {
		std::u16string key = call[1] <= 0xffff ? u"#" + widen_ascii(std::to_string(call[1])) :
			wide ? call.wide(call[1], 255) : from_ansi(call.narrow(call[1], 255), 1252);
		for (auto &character : key)
			character = upper_case(character);
		return key;
	};
	for (const bool wide : {true, false}) {
		const std::string suffix = wide ? "W" : "A";
		registry.add("SetProp" + suffix, 3, [this, property_key, wide](ApiCall &call) {
			auto *window = objects.window(call[0]);
			if (!window) {
				call.fail(1400);
				return;
			}
			window->properties[property_key(call, wide)] = call[2];
			call.ret(1);
		});
		registry.add("GetProp" + suffix, 2, [this, property_key, wide](ApiCall &call) {
			const auto *window = std::as_const(objects).window(call[0]);
			if (!window) {
				call.ret(0);
				return;
			}
			const auto found = window->properties.find(property_key(call, wide));
			call.ret(found == window->properties.end() ? 0 : found->second);
		});
		registry.add("RemoveProp" + suffix, 2, [this, property_key, wide](ApiCall &call) {
			auto *window = objects.window(call[0]);
			if (!window) {
				call.fail(1400);
				return;
			}
			const auto found = window->properties.find(property_key(call, wide));
			if (found == window->properties.end()) {
				call.ret(0);
				return;
			}
			const auto value = found->second;
			window->properties.erase(found);
			call.ret(value);
		});
	}

	registry.add("SetWindowPos", 7, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		const uint32_t flags = call.u32(6);
		const auto *parent = window->parent ? objects.window(window->parent) : nullptr;
		const int32_t ox = parent ? parent->x : 0, oy = parent ? parent->y : 0;
		const int32_t x = call.i32(2), y = call.i32(3), width = std::max(call.i32(4), 0), height = std::max(call.i32(5), 0);
		DesktopRect outer{x + ox, y + oy, x + ox + width, y + oy + height};
		try {
			place_window(call[0], outer, call[1], flags);
			call.ret(1);
		} catch (const uint32_t &error) {
			call.fail(error);
		}
	});
	registry.add("MoveWindow", 6, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		const auto *parent = window->parent ? objects.window(window->parent) : nullptr;
		const int32_t ox = parent ? parent->x : 0, oy = parent ? parent->y : 0;
		const int32_t x = call.i32(1), y = call.i32(2), width = std::max(call.i32(3), 0), height = std::max(call.i32(4), 0);
		try {
			place_window(call[0], {x + ox, y + oy, x + ox + width, y + oy + height}, 0, call.u32(5) ? 0x4 : 0x4 | swp_noredraw);
			call.ret(1);
		} catch (const uint32_t &error) {
			call.fail(error);
		}
	});
	// Deferred positioning collects window changes and applies them in order when the batch ends.
	struct Deferred {
		uint64_t window, insert_after;
		DesktopRect outer;
		uint32_t flags;
	};
	auto batches = std::make_shared<std::map<uint64_t, std::vector<Deferred>>>();
	auto next_batch = std::make_shared<uint64_t>(0x7d000000);
	registry.add("BeginDeferWindowPos", 1, [batches, next_batch](ApiCall &call) {
		const auto handle = ++*next_batch;
		(*batches)[handle] = {};
		call.ret(handle);
	});
	registry.add("DeferWindowPos", 8, [this, batches](ApiCall &call) {
		const auto found = batches->find(call[0]);
		const auto *window = std::as_const(objects).window(call[1]);
		if (found == batches->end() || !window) {
			call.fail(window ? 6 : 1400, 0);
			return;
		}
		const auto *parent = window->parent ? objects.window(window->parent) : nullptr;
		const int32_t ox = parent ? parent->x : 0, oy = parent ? parent->y : 0;
		found->second.push_back({call[1], call[2], {call.i32(3) + ox, call.i32(4) + oy, call.i32(3) + ox + call.i32(5), call.i32(4) + oy + call.i32(6)}, call.u32(7)});
		call.ret(call[0]);
	});
	registry.add("EndDeferWindowPos", 1, [this, batches](ApiCall &call) {
		const auto found = batches->find(call[0]);
		if (found == batches->end()) {
			call.fail(6);
			return;
		}
		const auto operations = std::move(found->second);
		batches->erase(found);
		try {
			for (const auto &operation : operations)
				if (std::as_const(objects).window(operation.window))
					place_window(operation.window, operation.outer, operation.insert_after, operation.flags);
			call.ret(1);
		} catch (const uint32_t &error) {
			call.fail(error);
		}
	});

	registry.add("GetWindowPlacement", 2, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		const bool minimized = window->style & ws_minimize, maximized = window->style & ws_maximize;
		const DesktopRect normal = (minimized || maximized) && window->restore.right > window->restore.left ? window->restore : window->outer;
		std::vector<uint8_t> bytes(44);
		const uint32_t fields[11] = {44, 0, uint32_t(minimized ? 2 : maximized ? 3 : window->visible ? 1 : 0), UINT32_MAX, UINT32_MAX, UINT32_MAX,
			UINT32_MAX, uint32_t(normal.left), uint32_t(normal.top), uint32_t(normal.right), uint32_t(normal.bottom)};
		std::memcpy(bytes.data(), fields, 44);
		call.write(call[1], bytes);
		call.ret(1);
	});
	registry.add("SetWindowPlacement", 2, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		const auto bytes = call.read(call[1], 44);
		uint32_t fields[11];
		std::memcpy(fields, bytes.data(), 44);
		if (fields[0] != 44) {
			call.fail(87);
			return;
		}
		try {
			const DesktopRect normal{int32_t(fields[7]), int32_t(fields[8]), int32_t(fields[9]), int32_t(fields[10])};
			objects.window(call[0])->restore = normal;
			// The window becomes visible before it is sized, so its size notifications reach a window that is already shown.
			if (fields[2] != 0)
				show_window(call[0], fields[2]);
			place_window(call[0], normal, 0, 0x4 | swp_noactivate);
			if (fields[2] == 0)
				show_window(call[0], 0);
			call.ret(1);
		} catch (const uint32_t &error) {
			call.fail(error);
		}
	});
	registry.add("DrawMenuBar", 1, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		if (objects.window_visible(call[0]))
			present(call[0]);
		call.ret(1);
	});
	registry.add("GetUpdateRgn", 3, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window || !objects.region(call[1])) {
			call.fail(window ? 6 : 1400);
			return;
		}
		objects.replace_region(call[1], window->update);
		const auto bounds = window->update.bounds();
		call.ret(window->update.empty() ? 1 : bounds.right - bounds.left == 0 ? 1 : window->update.complexity());
	});
	// RedrawWindow (RDW_*): invalidate or validate the window, optionally its children, and optionally paint right away.
	registry.add("RedrawWindow", 4, [this](ApiCall &call) {
		const uint64_t handle = call[0] ? call[0] : desktop_window();
		const auto *window = std::as_const(objects).window(handle);
		if (!window) {
			call.fail(1400);
			return;
		}
		const uint32_t flags = call.u32(3);
		DesktopRect rect{0, 0, window->width, window->height};
		if (call[1]) {
			const auto bytes = call.read(call[1], 16);
			std::memcpy(&rect, bytes.data(), 16);
		}
		try {
			const bool all = flags & 0x80;
			const auto apply = [&](auto &&self, uint64_t target, DesktopRect area, bool whole) -> void {
				const auto *record = std::as_const(objects).window(target);
				if (!record)
					return;
				if (whole)
					area = {0, 0, record->width, record->height};
				if (flags & 1)
					objects.invalidate(target, area, flags & 4);
				else if (flags & 8)
					objects.validate(target, area);
				if (all && !(flags & 0x40))
					for (const auto child : objects.child_windows(target))
						self(self, child, area, true);
			};
			apply(apply, handle, rect, false);
			// A null window is the desktop: every top-level window and what it contains is redrawn, owned dialogs included.
			if (!call[0] && !(flags & 0x40))
				for (const auto root : top_level_windows())
					apply(apply, root, rect, true);
			if (flags & 0x100) {
				if (std::as_const(objects).window(handle) && !std::as_const(objects).window(handle)->update.empty())
					send(handle, 0x0f, 0, 0);
			}
			call.ret(1);
		} catch (const uint32_t &error) {
			call.fail(error);
		}
	});
	registry.add("ScrollWindow", 5, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		// Scrolling is a copy plus the exposed strip; without a pixel-level copy the whole area is repainted.
		objects.invalidate(call[0], {0, 0, window->width, window->height}, true);
		call.ret(1);
	});
	// Layered window attributes are kept per window for GetLayeredWindowAttributes; the compositor draws windows opaque.
	auto layered = std::make_shared<std::map<uint64_t, std::array<uint32_t, 3>>>();
	registry.add("UpdateLayeredWindow", 9, [this](ApiCall &call) {
		if (!std::as_const(objects).window(call[0])) {
			call.fail(1400);
			return;
		}
		call.ret(1);
	});
	registry.add("SetLayeredWindowAttributes", 4, [this, layered](ApiCall &call) {
		if (!std::as_const(objects).window(call[0])) {
			call.fail(1400);
			return;
		}
		(*layered)[call[0]] = {call.u32(1), call.u32(2) & 0xff, call.u32(3)};
		call.ret(1);
	});
	registry.add("GetLayeredWindowAttributes", 4, [this, layered](ApiCall &call) {
		const auto found = layered->find(call[0]);
		if (!std::as_const(objects).window(call[0]) || found == layered->end()) {
			call.fail(objects.window(call[0]) ? 87 : 1400);
			return;
		}
		if (call[1]) call.store<uint32_t>(call[1], found->second[0]);
		if (call[2]) call.store<uint8_t>(call[2], uint8_t(found->second[1]));
		if (call[3]) call.store<uint32_t>(call[3], found->second[2]);
		call.ret(1);
	});
	// Of the attributes the compositor keeps for a window, the dark frame changes what is drawn; the others are accepted and ignored.
	// The attributes a program reads to place its own caption: the composition is on, nothing is cloaked, the frame is the window
	// and the caption buttons sit in its top right corner, in window coordinates. Others are refused, as for an unknown attribute.
	registry.add("DwmGetWindowAttribute", 4, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window) {
			call.ret(0x80070006U);
			return;
		}
		const uint32_t attribute = call.u32(1), size = call.u32(3);
		const auto rect = [&](int32_t left, int32_t top, int32_t right, int32_t bottom) {
			const int32_t values[] = {left, top, right, bottom};
			call.write(call[2], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(values), sizeof(values)));
			call.ret(0);
		};
		if (attribute == 1 && size >= 4) {
			call.store<uint32_t>(call[2], 1);
			call.ret(0);
		} else if (attribute == 14 && size >= 4) {
			call.store<uint32_t>(call[2], 0);
			call.ret(0);
		} else if (attribute == 5 && size >= 16) {
			constexpr int32_t buttons_width = 138, buttons_height = 30;
			rect(std::max(window->width - buttons_width, 0), 0, window->width, buttons_height);
		} else if (attribute == 9 && size >= 16) {
			rect(window->x, window->y, window->x + window->width, window->y + window->height);
		} else {
			call.ret(0x80070057U);
		}
	});
	registry.add("DwmSetWindowAttribute", 4, [this](ApiCall &call) {
		auto *window = objects.window(call[0]);
		if (!window) {
			call.ret(0x80070006U);
			return;
		}
		// DWMWA_USE_IMMERSIVE_DARK_MODE is 20, and 19 on Windows 10 before build 18985.
		if ((call.u32(1) == 20 || call.u32(1) == 19) && call[2] && call.u32(3) == 4) {
			const bool dark = call.load<uint32_t>(call[2]) != 0;
			if (window->dark_frame != dark) {
				window->dark_frame = dark;
				drawing(call[0]);
			}
		}
		call.ret(0);
	});
	// The compositor shows the frame through the client area within these margins, wherever the program left it black (see present).
	registry.add("DwmExtendFrameIntoClientArea", 2, [this](ApiCall &call) {
		auto *window = objects.window(call[0]);
		if (!window || !call[1]) {
			call.ret(0x80070057U);
			return;
		}
		std::array<int32_t, 4> margins{};
		for (size_t index = 0; index < 4; ++index)
			margins[index] = int32_t(call.load<uint32_t>(call[1] + index * 4));
		if (std::getenv("RAVEN_TRACE_NC"))
			std::fprintf(stderr, "glass %llx margins %d %d %d %d\n", (unsigned long long)call[0], margins[0], margins[1], margins[2], margins[3]);
		if (margins == std::array<int32_t, 4>{})
			window->glass.reset();
		else
			window->glass = margins;
		drawing(call[0]);
		call.ret(0);
	});
	// The compositor answers the hit test of the caption buttons it draws over an extended frame; every other message is the program's.
	registry.add("DwmDefWindowProc", 5, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window || !window->glass || call.u32(1) != 0x84 || !themed()) {
			call.ret(0);
			return;
		}
		const auto layout = frame_layout(*window);
		const int32_t x = int16_t(call[3] & 0xffff) - window->outer.left, y = int16_t((call[3] >> 16) & 0xffff) - window->outer.top;
		if (window->y - window->outer.top > layout.close.top) {
			call.ret(0);
			return;
		}
		const auto inside = [&](DesktopRect rect) { return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom; };
		const uint64_t hit = inside(layout.close) ? 20 : inside(layout.maximize) ? 9 : inside(layout.minimize) ? 8 : 0;
		if (!hit) {
			call.ret(0);
			return;
		}
		if (call[4])
			call.store_pointer(call[4], hit);
		call.ret(1);
	});
	registry.add("EnumDisplayMonitors", 4, [this](ApiCall &call) {
		const auto &metrics = objects.nonclient_metrics();
		const auto rect = process.map_memory(4096, PROT_READ | PROT_WRITE);
		try {
			std::vector<uint8_t> bytes(16);
			const int32_t values[4] = {0, 0, metrics.screen_x, metrics.screen_y};
			std::memcpy(bytes.data(), values, 16);
			process.write_memory(rect, bytes);
			const uint64_t arguments[] = {1, call[0], rect, call[3]};
			const auto result = invoke(call[2], arguments);
			if (result.stop != NativeStop::returned)
				throw result;
		} catch (...) {
			process.unmap_memory(rect);
			throw;
		}
		process.unmap_memory(rect);
		call.ret(1);
	});
	// MONITORINFO, or MONITORINFOEX with the device name in the caller's character width.
	for (const bool wide : {true, false})
		registry.add(wide ? "GetMonitorInfoW" : "GetMonitorInfoA", 2, [this, wide](ApiCall &call) {
			const auto &metrics = objects.nonclient_metrics();
			const uint32_t size = call.load<uint32_t>(call[1]);
			if (size != 40 && size != (wide ? 104 : 72)) {
				call.fail(87);
				return;
			}
			std::vector<uint8_t> bytes(size);
			const int32_t values[10] = {int32_t(size), 0, 0, metrics.screen_x, metrics.screen_y, 0, 0, metrics.screen_x, metrics.screen_y, 1};
			std::memcpy(bytes.data(), values, 40);
			if (size > 40) {
				const std::string_view device = "\\\\.\\DISPLAY1";
				for (size_t index = 0; index < device.size(); ++index)
					bytes[40 + index * (wide ? 2 : 1)] = uint8_t(device[index]);
			}
			call.write(call[1], bytes);
			call.ret(1);
		});
	registry.add("MonitorFromPoint", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("MonitorFromRect", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("MonitorFromWindow", 2, [](ApiCall &call) { call.ret(1); });
	// Where a popup of the given size goes next to an anchor point: the TPM_ alignment flags place it, the screen keeps it whole, and
	// the rectangle to keep clear pushes it aside, to the right or left of it, or below or above it with TPM_VERTICAL.
	registry.add("CalculatePopupWindowPosition", 5, [this](ApiCall &call) {
		const auto &metrics = objects.nonclient_metrics();
		const int32_t x = call.load<int32_t>(call[0]), y = call.load<int32_t>(call[0] + 4);
		const int32_t width = call.load<int32_t>(call[1]), height = call.load<int32_t>(call[1] + 4);
		const uint32_t flags = call.u32(2);
		if (width < 0 || height < 0 || !call[4]) {
			call.fail(87);
			return;
		}
		int32_t left = (flags & 8) ? x - width : (flags & 4) ? x - width / 2 : x;
		int32_t top = (flags & 0x20) ? y - height : (flags & 0x10) ? y - height / 2 : y;
		const auto fit = [](int32_t start, int32_t length, int32_t limit) { return std::max(0, std::min(start, limit - length)); };
		if (call[3]) {
			const int32_t ex_left = call.load<int32_t>(call[3]), ex_top = call.load<int32_t>(call[3] + 4);
			const int32_t ex_right = call.load<int32_t>(call[3] + 8), ex_bottom = call.load<int32_t>(call[3] + 12);
			const bool overlaps = left < ex_right && left + width > ex_left && top < ex_bottom && top + height > ex_top;
			if (overlaps && (flags & 0x40))
				top = ex_bottom + height <= metrics.screen_y || ex_top - height < 0 ? ex_bottom : ex_top - height;
			else if (overlaps)
				left = ex_right + width <= metrics.screen_x || ex_left - width < 0 ? ex_right : ex_left - width;
			if (flags & 0x40)
				left = fit(left, width, metrics.screen_x);
			else
				top = fit(top, height, metrics.screen_y);
		} else {
			left = fit(left, width, metrics.screen_x);
			top = fit(top, height, metrics.screen_y);
		}
		const int32_t rect[4] = {left, top, left + width, top + height};
		call.write(call[4], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(rect), sizeof(rect)));
		call.ret(1);
	});
	// The one display has a single mode, which is the current one; mode numbers above 0 end the enumeration.
	const auto display_mode = [this](ApiCall &call, size_t mode_index, size_t device_index, size_t output_index) {
		const auto &metrics = objects.nonclient_metrics();
		const int32_t mode = call.i32(mode_index);
		if (call[device_index]) {
			const auto name = call.wide(call[device_index], 64);
			if (name != u"\\\\.\\DISPLAY1") {
				call.fail(0);
				return;
			}
		}
		if (mode > 0 || mode < -2) {
			call.fail(0);
			return;
		}
		const uint32_t size = call.load<uint16_t>(call[output_index] + 68);
		if (size < 188) {
			call.fail(87);
			return;
		}
		std::vector<uint8_t> bytes(size, 0);
		const std::u16string device = u"\\\\.\\DISPLAY1";
		std::memcpy(bytes.data(), device.data(), device.size() * 2);
		const auto put = [&](size_t at, uint64_t value, size_t width) { for (size_t byte = 0; byte < width; ++byte) bytes[at + byte] = uint8_t(value >> (byte * 8)); };
		put(66, 0x0401, 2);
		put(68, size, 2);
		// Position, orientation, bits per pixel, width, height, display flags and refresh rate.
		put(72, 0x20 | 0x80 | 0x40000 | 0x80000 | 0x100000 | 0x200000 | 0x400000, 4);
		put(168, 32, 4);
		put(172, uint32_t(metrics.screen_x), 4);
		put(176, uint32_t(metrics.screen_y), 4);
		put(184, 60, 4);
		call.write(call[output_index], bytes);
		call.ret(1);
	};
	registry.add("EnumDisplaySettingsW", 3, [=](ApiCall &call) { display_mode(call, 1, 0, 2); });
	registry.add("EnumDisplaySettingsExW", 4, [=](ApiCall &call) { display_mode(call, 1, 0, 2); });
	// DEVMODEA holds its two names as 32 bytes each instead of 32 characters, so every later field sits 32, then 64 bytes earlier.
	const auto display_mode_ansi = [this](ApiCall &call) {
		const auto &metrics = objects.nonclient_metrics();
		const int32_t mode = call.i32(1);
		if ((call[0] && call.narrow(call[0], 32) != "\\\\.\\DISPLAY1") || mode > 0 || mode < -2) {
			call.fail(0);
			return;
		}
		const uint32_t size = call.load<uint16_t>(call[2] + 36);
		if (size < 124) {
			call.fail(87);
			return;
		}
		std::vector<uint8_t> bytes(size, 0);
		const std::string device = "\\\\.\\DISPLAY1";
		std::memcpy(bytes.data(), device.data(), device.size());
		const auto put = [&](size_t at, uint64_t value, size_t width) { for (size_t byte = 0; byte < width; ++byte) bytes[at + byte] = uint8_t(value >> (byte * 8)); };
		put(34, 0x0401, 2);
		put(36, size, 2);
		put(40, 0x20 | 0x80 | 0x40000 | 0x80000 | 0x100000 | 0x200000 | 0x400000, 4);
		put(104, 32, 4);
		put(108, uint32_t(metrics.screen_x), 4);
		put(112, uint32_t(metrics.screen_y), 4);
		put(120, 60, 4);
		call.write(call[2], bytes);
		call.ret(1);
	};
	registry.add("EnumDisplaySettingsA", 3, [=](ApiCall &call) { display_mode_ansi(call); });
	registry.add("EnumDisplaySettingsExA", 4, [=](ApiCall &call) { display_mode_ansi(call); });
	// There is no kernel graphics subsystem behind the host's display: the adapter thunks report that the request is not supported, and
	// programs that probe them (to size video memory, for instance) fall back to what the graphics API reports.
	for (const auto *thunk : {"D3DKMTOpenAdapterFromLuid", "D3DKMTOpenAdapterFromGdiDisplayName", "D3DKMTOpenAdapterFromHdc", "D3DKMTCloseAdapter",
			"D3DKMTQueryAdapterInfo", "D3DKMTQueryVideoMemoryInfo", "D3DKMTEnumAdapters", "D3DKMTEnumAdapters2", "D3DKMTGetDeviceState",
			"D3DKMTCreateDevice", "D3DKMTDestroyDevice", "D3DKMTWaitForVerticalBlankEvent", "D3DKMTCheckVidPnExclusiveOwnership"})
		registry.add(thunk, 1, [](ApiCall &call) { call.ret(0xC00000BBU); });
	// The same answer for every other thunk the library exports: each takes one pointer to its parameter structure.
	for (const auto name : system_export_names("gdi32.dll"))
		if (name.starts_with("D3DKMT") && !registry.has(name))
			registry.add(name, 1, [](ApiCall &call) { call.ret(0xC00000BBU); });
	// One adapter, and one monitor on it, for programs that ask which display they are on.
	const auto display_device = [](ApiCall &call, bool wide) {
		const size_t size = wide ? 840 : 424;
		if (call.u32(1) != 0 || !call[2] || call.load<uint32_t>(call[2]) != size) {
			call.ret(0);
			return;
		}
		const bool monitor = call[0] != 0;
		std::vector<uint8_t> record(size);
		const auto put = [&](size_t offset, std::string_view text) {
			for (size_t index = 0; index < text.size(); ++index) {
				record[offset + index * (wide ? 2 : 1)] = uint8_t(text[index]);
			}
		};
		record[0] = uint8_t(size & 0xff);
		record[1] = uint8_t(size >> 8);
		const size_t name = 4, description = wide ? 68 : 36, flags = wide ? 324 : 164, identifier = wide ? 328 : 168;
		put(name, monitor ? "\\\\.\\DISPLAY1\\Monitor0" : "\\\\.\\DISPLAY1");
		put(description, monitor ? "Raven Monitor" : "Raven Display");
		record[flags] = monitor ? 3 : 5;
		put(identifier, monitor ? "MONITOR\\RAVEN0000" : "PCI\\VEN_1AF4&DEV_1050");
		call.write(call[2], record);
		call.ret(1);
	};
	registry.add("EnumDisplayDevicesA", 4, [=](ApiCall &call) { display_device(call, false); });
	registry.add("EnumDisplayDevicesW", 4, [=](ApiCall &call) { display_device(call, true); });
	// Programs declare how they handle scaling; the host's windows are not scaled by a second layer, so every declaration is accepted.
	registry.add("SetProcessDPIAware", 0, [](ApiCall &call) { call.ret(1); });
	registry.add("IsProcessDPIAware", 0, [](ApiCall &call) { call.ret(1); });
	registry.add("SetProcessDpiAwarenessContext", 1, [](ApiCall &call) { call.ret(1); });
	// There is no display topology to report: callers fall back to their single-monitor behavior.
	registry.add("GetDisplayConfigBufferSizes", 3, [](ApiCall &call) {
		if (call[1]) call.store<uint32_t>(call[1], 0);
		if (call[2]) call.store<uint32_t>(call[2], 0);
		call.ret(50);
	});
	registry.add("QueryDisplayConfig", 6, [](ApiCall &call) {
		if (call[1]) call.store<uint32_t>(call[1], 0);
		if (call[3]) call.store<uint32_t>(call[3], 0);
		call.ret(50);
	});
}

}
