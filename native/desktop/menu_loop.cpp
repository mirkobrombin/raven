#include "windows.h"
#include "canvas.h"
#include "shapes.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace raven {

#ifdef RAVEN_SKIA

using namespace paint;

namespace {

uint32_t message_time()
{
	return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

constexpr uint32_t item_grayed = 3, item_checked = 8, type_separator = 0x800, type_radio = 0x200;
constexpr int32_t popup_border = 2, gutter = 22, arrow_width = 20, separator_height = 9;

bool is_enabled(const DesktopMenuItem &item)
{
	return !(item.state & item_grayed) && !(item.type & type_separator);
}

// Label up to a tab, and the accelerator text after it.
std::pair<std::u16string, std::u16string> split_tab(std::u16string_view text)
{
	const auto tab = text.find(u'\t');
	return {std::u16string(text.substr(0, tab)), tab == text.npos ? std::u16string{} : std::u16string(text.substr(tab + 1))};
}

}

void NativeWindows::menu_render(MenuPopup &popup)
{
	const auto *menu = objects.menu(popup.menu);
	if (!menu)
		return;
	const auto color = [&](int index) { return rgb(objects.system_color(index)); };
	Canvas canvas{std::vector<uint8_t>(size_t(popup.width) * size_t(popup.height) * 4), popup.width, popup.height};
	if (themed()) {
		menu_render_modern(popup, *menu, canvas);
		popup.pixels = std::move(canvas.owned);
		return;
	}
	canvas.fill({0, 0, popup.width, popup.height}, color(4));
	canvas.edge({0, 0, popup.width, popup.height}, color(22), color(21), color(20), color(16));
	auto &renderer = objects.text();
	const auto metrics = renderer.metrics(interface_font());
	for (size_t index = 0; index < menu->items.size() && index < popup.items.size(); ++index) {
		const auto &item = menu->items[index];
		const auto rect = popup.items[index];
		if (item.type & type_separator) {
			const int32_t y = rect.top + (rect.bottom - rect.top) / 2;
			for (int32_t x = rect.left + 2; x < rect.right - 2; ++x) {
				canvas.set(x, y - 1, color(16));
				canvas.set(x, y, color(20));
			}
			continue;
		}
		const bool selected = popup.highlight && *popup.highlight == index;
		if (selected)
			canvas.fill(rect, color(13));
		const bool enabled = is_enabled(item);
		const auto ink = enabled ? color(selected ? 14 : 7) : color(17);
		const auto [text, accelerator] = split_tab(item.text);
		const auto [label, mnemonic] = menu_label(text);
		const int32_t text_top = rect.top + (rect.bottom - rect.top - metrics.height) / 2;
		draw_text(renderer, canvas, label, rect.left + gutter, text_top, ink, mnemonic);
		if (!accelerator.empty()) {
			const auto characters = printable(accelerator);
			const auto width = renderer.extent(interface_font(), characters).width;
			draw_text(renderer, canvas, accelerator, rect.right - arrow_width - width, text_top, ink);
		}
		const int32_t cy = (rect.top + rect.bottom) / 2;
		if (item.state & item_checked) {
			const int32_t cx = rect.left + 10;
			if (item.type & type_radio) {
				for (int32_t dy = -2; dy <= 2; ++dy)
					for (int32_t dx = -2; dx <= 2; ++dx)
						if (dx * dx + dy * dy <= 5)
							canvas.set(cx + dx, cy + dy, ink);
			} else {
				for (int32_t step = 0; step < 3; ++step) {
					canvas.set(cx - 3 + step, cy + step, ink);
					canvas.set(cx - 3 + step, cy + step + 1, ink);
				}
				for (int32_t step = 0; step < 5; ++step) {
					canvas.set(cx - 1 + step, cy + 2 - step, ink);
					canvas.set(cx - 1 + step, cy + 3 - step, ink);
				}
			}
		}
		if (item.submenu) {
			const int32_t ax = rect.right - 10;
			for (int32_t step = 0; step < 4; ++step)
				for (int32_t dy = -3 + step; dy <= 3 - step; ++dy)
					canvas.set(ax + step, cy + dy, ink);
		}
	}
	popup.pixels = std::move(canvas.owned);
}

// A Windows 11 menu: a light surface with a thin border, items highlighted on a rounded shape inset from the edges, thin separators,
// and the check, the radio dot and the submenu chevron drawn with antialiasing.
void NativeWindows::menu_render_modern(MenuPopup &popup, const DesktopMenu &menu, Canvas &canvas)
{
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
	canvas.fill({0, 0, popup.width, popup.height}, 0xf9f9f9);
	shapes.rounded(0, 0, popup.width, popup.height, 0, std::nullopt, 0xd6d6d6);
	auto &renderer = objects.text();
	const auto metrics = renderer.metrics(interface_font());
	for (size_t index = 0; index < menu.items.size() && index < popup.items.size(); ++index) {
		const auto &item = menu.items[index];
		const auto rect = popup.items[index];
		if (item.type & type_separator) {
			const int32_t y = rect.top + (rect.bottom - rect.top) / 2;
			canvas.fill({rect.left + 4, y, rect.right - 4, y + 1}, 0xe5e5e5);
			continue;
		}
		const bool selected = popup.highlight && *popup.highlight == index;
		const bool enabled = is_enabled(item);
		if (selected)
			shapes.rounded(rect.left + 3, rect.top + 1, rect.right - 3, rect.bottom - 1, 4, enabled ? 0xefefef : 0xf4f4f4, std::nullopt);
		const uint32_t ink = enabled ? 0x1a1a1a : 0xa0a0a0;
		const auto [text, accelerator] = split_tab(item.text);
		const auto [label, mnemonic] = menu_label(text);
		const int32_t text_top = rect.top + (rect.bottom - rect.top - metrics.height) / 2;
		draw_text(renderer, canvas, label, rect.left + gutter + 6, text_top, ink, mnemonic);
		if (!accelerator.empty()) {
			const auto width = renderer.extent(interface_font(), printable(accelerator)).width;
			draw_text(renderer, canvas, accelerator, rect.right - arrow_width - width, text_top, enabled ? 0x606060 : 0xa0a0a0);
		}
		const double cy = (rect.top + rect.bottom) / 2.0, cx = rect.left + 14.0;
		if (item.state & item_checked) {
			if (item.type & type_radio) {
				shapes.circle(cx, cy, 3, ink, std::nullopt);
			} else {
				shapes.segment(cx - 4, cy, cx - 1.5, cy + 3, 1.2, ink);
				shapes.segment(cx - 1.5, cy + 3, cx + 4, cy - 3.5, 1.2, ink);
			}
		}
		if (item.submenu)
			shapes.chevron(rect.right - 12.0, cy, 8, 'r', ink);
	}
}

void NativeWindows::menu_open_popup(uint64_t menu_handle, uint64_t parent, int32_t x, int32_t y)
{
	const auto *menu = objects.menu(menu_handle);
	if (!menu || !presentation.popup)
		throw uint32_t(120);
	if (popups.size() >= 8)
		throw uint32_t(120);
	auto &renderer = objects.text();
	const auto font = interface_font();
	const auto metrics = renderer.metrics(font);
	// Windows 11 spaces its menu items wider than the classic menus.
	const int32_t item_height = metrics.height + (themed() ? 13 : 5);
	int32_t label_width = 0, accelerator_width = 0, total = popup_border;
	std::vector<DesktopRect> rects;
	for (const auto &item : menu->items) {
		if (item.type & type_separator) {
			rects.push_back({popup_border, total, 0, total + separator_height});
			total += separator_height;
			continue;
		}
		const auto [text, accelerator] = split_tab(item.text);
		const auto label = menu_label(text).first;
		const auto labels = printable(label);
		label_width = std::max(label_width, labels.empty() ? 0 : renderer.extent(font, labels).width);
		const auto accelerators = printable(accelerator);
		accelerator_width = std::max(accelerator_width, accelerators.empty() ? 0 : renderer.extent(font, accelerators).width);
		rects.push_back({popup_border, total, 0, total + item_height});
		total += item_height;
	}
	const int32_t width = popup_border * 2 + gutter + label_width + (accelerator_width ? 16 + accelerator_width : 0) + arrow_width;
	const int32_t height = total + popup_border;
	for (auto &rect : rects)
		rect.right = width - popup_border;
	MenuPopup popup{next_popup++, menu_handle, parent, x, y, width, height, std::move(rects), std::nullopt, {}};
	menu_render(popup);
	presentation.popup(popup.handle, parent, x, y, uint32_t(width), uint32_t(height), popup.pixels);
	popups.push_back(std::move(popup));
}

void NativeWindows::menu_close()
{
	const bool was_active = menu_active();
	const auto owner = open_menu_window;
	while (!popups.empty()) {
		const auto handle = popups.back().handle;
		popups.pop_back();
		if (presentation.remove)
			presentation.remove(handle);
	}
	open_menu_item.reset();
	if (was_active && owner && std::as_const(objects).window(owner)) {
		send(owner, 0x11f, 0xffff0000ULL, 0);
		send(owner, 0x212, 0, 0);
		if (objects.window_visible(owner))
			present(owner);
	}
	open_menu_window = 0;
}

void NativeWindows::menu_open_bar(uint64_t window, size_t index)
{
	auto *owner = objects.window(window);
	if (!owner || !owner->menu)
		return;
	const bool already = menu_active();
	while (popups.size() > 0) {
		const auto handle = popups.back().handle;
		popups.pop_back();
		if (presentation.remove)
			presentation.remove(handle);
	}
	const auto rects = menu_bar_items(*owner);
	const auto *bar = objects.menu(owner->menu);
	if (!bar || index >= bar->items.size() || index >= rects.size())
		return;
	if (!already) {
		open_menu_window = window;
		send(window, 0x211, 0, 0);
	}
	send(window, 0x116, owner->menu, 0);
	owner = objects.window(window);
	if (!owner)
		return;
	bar = objects.menu(owner->menu);
	if (!bar || index >= bar->items.size())
		return;
	const auto &item = bar->items[index];
	if (!item.submenu) {
		// A menu bar command without a drop-down runs immediately.
		const auto command = item.id;
		menu_close();
		if (messages.size() < 10000)
			queue({window, command, 0, 0x111, message_time(), cursor_x, cursor_y});
		return;
	}
	send(window, 0x117, item.submenu, uint64_t(index));
	if (!std::as_const(objects).window(window))
		return;
	open_menu_item = index;
	open_menu_window = window;
	const auto rect = menu_bar_items(*objects.window(window))[index];
	menu_open_popup(item.submenu, window, rect.left, rect.bottom);
	present(window);
}

void NativeWindows::menu_highlight(size_t level, std::optional<size_t> index)
{
	if (level >= popups.size())
		return;
	// Deeper levels belong to the previously highlighted item.
	while (popups.size() > level + 1) {
		const auto handle = popups.back().handle;
		popups.pop_back();
		if (presentation.remove)
			presentation.remove(handle);
	}
	auto &popup = popups[level];
	if (popup.highlight == index)
		return;
	popup.highlight = index;
	const auto *menu = objects.menu(popup.menu);
	if (menu && index && *index < menu->items.size()) {
		const auto &item = menu->items[*index];
		const uint64_t flags = 0x80 | (item.submenu ? 0x10U : 0U) | ((item.state & item_grayed) ? 0x3U : 0U) | ((item.state & item_checked) ? 0x8U : 0U);
		if (open_menu_window && std::as_const(objects).window(open_menu_window))
			send(open_menu_window, 0x11f, (item.submenu ? *index : item.id) | (flags << 16), popup.menu);
		if (item.submenu && is_enabled(item)) {
			const auto rect = popup.items[*index];
			send(open_menu_window, 0x117, item.submenu, *index);
			menu_render(popup);
			presentation.popup(popup.handle, popup.parent, popup.x, popup.y, uint32_t(popup.width), uint32_t(popup.height), popup.pixels);
			menu_open_popup(item.submenu, popup.handle, popup.width - 4, rect.top - 2);
			return;
		}
	}
	menu_render(popup);
	presentation.popup(popup.handle, popup.parent, popup.x, popup.y, uint32_t(popup.width), uint32_t(popup.height), popup.pixels);
}

void NativeWindows::menu_select(size_t level, size_t index)
{
	if (level >= popups.size())
		return;
	const auto *menu = objects.menu(popups[level].menu);
	if (!menu || index >= menu->items.size())
		return;
	const auto item = menu->items[index];
	if (!is_enabled(item))
		return;
	if (item.submenu) {
		menu_highlight(level, index);
		return;
	}
	const auto owner = open_menu_window;
	const auto found = system_menus.find(owner);
	const bool system = found != system_menus.end() && popups.front().menu == found->second;
	menu_close();
	if (menu_tracking)
		tracked_command = item.id;
	else if (owner && std::as_const(objects).window(owner) && messages.size() < 10000)
		queue({owner, item.id, 0, system ? 0x112U : 0x111U, message_time(), cursor_x, cursor_y});
}

void NativeWindows::system_menu_open(uint64_t window, int32_t x, int32_t y)
{
	if (menu_active() || menu_tracking || !std::as_const(objects).window(window))
		return;
	const auto handle = system_menu(window);
	if (!handle)
		return;
	const auto *owner = std::as_const(objects).window(window);
	// As in Windows, the standard commands are greyed for what the window's state and style rule out; items a program added keep theirs.
	const uint32_t style = owner->style;
	const bool maximized = style & 0x01000000U, minimized = style & 0x20000000U;
	const auto *type = objects.window_class(owner->class_id);
	for (auto &entry : objects.edit_menu(handle)->items) {
		bool grayed;
		switch (entry.id) {
		case 0xf120: grayed = !maximized && !minimized; break;
		case 0xf010: grayed = maximized; break;
		case 0xf000: grayed = maximized || minimized || !(style & 0x40000U); break;
		case 0xf020: grayed = minimized || !(style & 0x20000U); break;
		case 0xf030: grayed = maximized || !(style & 0x10000U); break;
		case 0xf060: grayed = type && (type->style & 0x200U); break;
		default: continue;
		}
		entry.state = grayed ? entry.state | 3U : entry.state & ~3U;
	}
	open_menu_window = window;
	send(window, 0x211, 0, 0);
	send(window, 0x116, handle, 0);
	// The high word of WM_INITMENUPOPUP's lParam says the popup is the window menu.
	send(window, 0x117, handle, 0x10000);
	if (!std::as_const(objects).window(window) || !objects.menu(handle)) {
		open_menu_window = 0;
		if (std::as_const(objects).window(window))
			send(window, 0x212, 0, 0);
		return;
	}
	menu_open_popup(handle, window, x, y);
	present(window);
}

uint64_t NativeWindows::track_popup_menu(uint64_t menu, uint32_t flags, int32_t x, int32_t y, uint64_t window)
{
	const auto root = objects.window_root(window);
	const auto *top = std::as_const(objects).window(root);
	if (!objects.menu(menu) || !top)
		throw uint32_t(1401);
	if (menu_active() || menu_tracking)
		throw uint32_t(1456);
	// TrackPopupMenu runs its own loop until the menu closes, as Windows does: the owner learns the menu loop started and the menu
	// is about to show, and the popup opens at the screen point given, on the owner's host window.
	open_menu_window = window;
	send(window, 0x211, 1, 0);
	if (!(flags & 0x80))
		send(window, 0x117, menu, 0);
	top = objects.window(root);
	if (!top || !objects.menu(menu)) {
		open_menu_window = 0;
		return 0;
	}
	present(root);
	menu_tracking = true;
	tracked_command = 0;
	try {
		menu_open_popup(menu, root, x - top->outer.left, y - top->outer.top);
		while (!popups.empty()) {
			serve_sent();
			flush_dirty();
			deliver_pending();
			if (!presentation.poll)
				break;
			for (const auto &event : presentation.poll(std::chrono::milliseconds(20)))
				receive(event);
		}
	} catch (...) {
		menu_tracking = false;
		menu_close();
		throw;
	}
	menu_tracking = false;
	menu_close();
	const auto command = tracked_command;
	// TPM_RETURNCMD answers the command itself; otherwise it reaches the owner as WM_COMMAND and the answer is only whether one was chosen.
	if (flags & 0x100)
		return command;
	if (command && !(flags & 0x80) && std::as_const(objects).window(window) && messages.size() < 10000)
		queue({window, command, 0, 0x111, message_time(), cursor_x, cursor_y});
	return command ? 1 : 0;
}

bool NativeWindows::menu_event(const DesktopEvent &event)
{
	const bool in_popup = event.window >= 0x7b000000ULL && event.window < next_popup;
	if (!menu_active() && !in_popup)
		return false;
	if (event.kind == DesktopEventKind::popup_done) {
		menu_close();
		return true;
	}
	if (event.kind == DesktopEventKind::keymap || event.kind == DesktopEventKind::modifiers || event.kind == DesktopEventKind::message)
		return false;
	if (event.kind == DesktopEventKind::keyboard_focus)
		return true;
	const auto level_of = [&](uint64_t handle) -> std::optional<size_t> {
		for (size_t index = 0; index < popups.size(); ++index)
			if (popups[index].handle == handle)
				return index;
		return std::nullopt;
	};
	const auto item_at = [&](const MenuPopup &popup, int32_t x, int32_t y) -> std::optional<size_t> {
		for (size_t index = 0; index < popup.items.size(); ++index) {
			const auto &rect = popup.items[index];
			if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom)
				return index;
		}
		return std::nullopt;
	};
	const auto bar_at = [&](int32_t x, int32_t y) -> std::optional<size_t> {
		const auto *owner = std::as_const(objects).window(open_menu_window);
		if (!owner)
			return std::nullopt;
		const auto layout = frame_layout(*owner);
		if (x < layout.menu_bar.left || x >= layout.menu_bar.right || y < layout.menu_bar.top || y >= layout.menu_bar.bottom)
			return std::nullopt;
		const auto rects = menu_bar_items(*owner);
		for (size_t index = 0; index < rects.size(); ++index)
			if (x >= rects[index].left && x < rects[index].right)
				return index;
		return std::nullopt;
	};
	if (event.kind == DesktopEventKind::pointer_motion) {
		if (const auto level = level_of(event.window)) {
			const auto &popup = popups[*level];
			const auto hit = item_at(popup, event.x, event.y);
			const auto *menu = objects.menu(popup.menu);
			if (hit && menu && !(menu->items[*hit].type & type_separator))
				menu_highlight(*level, hit);
			else if (!hit)
				menu_highlight(*level, std::nullopt);
		} else if (event.window == open_menu_window) {
			if (const auto hit = bar_at(event.x, event.y); hit && (!open_menu_item || *hit != *open_menu_item))
				menu_open_bar(open_menu_window, *hit);
		}
		return true;
	}
	if (event.kind == DesktopEventKind::pointer_button) {
		if (event.message != 0x110)
			return true;
		const bool pressed = event.wparam != 0;
		if (const auto level = level_of(event.window)) {
			const auto hit = item_at(popups[*level], event.x, event.y);
			if (hit && !pressed)
				menu_select(*level, *hit);
			else if (hit && pressed)
				menu_highlight(*level, hit);
			return true;
		}
		if (pressed) {
			const auto hit = event.window == open_menu_window ? bar_at(event.x, event.y) : std::nullopt;
			if (hit && open_menu_item && *hit == *open_menu_item)
				menu_close();
			else if (hit)
				menu_open_bar(open_menu_window, *hit);
			else
				menu_close();
		}
		return true;
	}
	if (event.kind == DesktopEventKind::key) {
		if (event.message < key_down.size())
			key_down[event.message] = event.wparam != 0;
		if (event.wparam != 1 || popups.empty())
			return true;
		const auto key = keyboard_map.decode(event.message);
		auto &top = popups.back();
		const auto *menu = objects.menu(top.menu);
		if (!menu)
			return true;
		const size_t level = popups.size() - 1;
		const auto step = [&](int direction) {
			const size_t count = menu->items.size();
			if (!count)
				return;
			size_t at = top.highlight ? *top.highlight : (direction > 0 ? count - 1 : 0);
			for (size_t tried = 0; tried < count; ++tried) {
				at = direction > 0 ? (at + 1) % count : (at + count - 1) % count;
				if (!(menu->items[at].type & type_separator)) {
					menu_highlight(level, at);
					return;
				}
			}
		};
		if (key.code == 0x1b) {
			if (popups.size() > 1) {
				const auto handle = popups.back().handle;
				popups.pop_back();
				if (presentation.remove)
					presentation.remove(handle);
			} else {
				menu_close();
			}
		} else if (key.code == 0x28) {
			step(1);
		} else if (key.code == 0x26) {
			step(-1);
		} else if (key.code == 0x0d) {
			if (top.highlight)
				menu_select(level, *top.highlight);
		} else if (key.code == 0x27) {
			if (top.highlight && menu->items[*top.highlight].submenu && is_enabled(menu->items[*top.highlight])) {
				menu_highlight(level, std::nullopt);
				menu_highlight(level, *top.highlight);
				if (popups.size() > level + 1)
					menu_highlight(level + 1, [&]() -> std::optional<size_t> {
						const auto *child = objects.menu(popups[level + 1].menu);
						for (size_t index = 0; child && index < child->items.size(); ++index)
							if (!(child->items[index].type & type_separator)) return index;
						return std::nullopt;
					}());
			} else if (popups.size() == 1 && open_menu_item) {
				const auto *owner = std::as_const(objects).window(open_menu_window);
				const auto *bar = owner ? objects.menu(owner->menu) : nullptr;
				if (bar && !bar->items.empty())
					menu_open_bar(open_menu_window, (*open_menu_item + 1) % bar->items.size());
			}
		} else if (key.code == 0x25) {
			if (popups.size() > 1) {
				const auto handle = popups.back().handle;
				popups.pop_back();
				if (presentation.remove)
					presentation.remove(handle);
			} else if (open_menu_item) {
				const auto *owner = std::as_const(objects).window(open_menu_window);
				const auto *bar = owner ? objects.menu(owner->menu) : nullptr;
				if (bar && !bar->items.empty())
					menu_open_bar(open_menu_window, (*open_menu_item + bar->items.size() - 1) % bar->items.size());
			}
		} else if (key.character) {
			const auto wanted = char16_t(key.character >= 'A' && key.character <= 'Z' ? key.character + 32 : key.character);
			for (size_t index = 0; index < menu->items.size(); ++index) {
				const auto &text = menu->items[index].text;
				for (size_t at = 0; at + 1 < text.size(); ++at)
					if (text[at] == u'&' && text[at + 1] != u'&') {
						const auto mark = char16_t(text[at + 1] >= u'A' && text[at + 1] <= u'Z' ? text[at + 1] + 32 : text[at + 1]);
						if (mark == wanted && is_enabled(menu->items[index])) {
							menu_select(level, index);
							return true;
						}
						break;
					}
			}
		}
		return true;
	}
	return true;
}

#else

void NativeWindows::menu_render(MenuPopup &) {}
void NativeWindows::menu_open_popup(uint64_t, uint64_t, int32_t, int32_t) { throw uint32_t(120); }
void NativeWindows::menu_close() {}
void NativeWindows::menu_open_bar(uint64_t, size_t) {}
void NativeWindows::menu_highlight(size_t, std::optional<size_t>) {}
void NativeWindows::menu_select(size_t, size_t) {}
bool NativeWindows::menu_event(const DesktopEvent &) { return false; }
uint64_t NativeWindows::track_popup_menu(uint64_t, uint32_t, int32_t, int32_t, uint64_t) { throw uint32_t(120); }
void NativeWindows::system_menu_open(uint64_t, int32_t, int32_t) {}

#endif

uint64_t NativeWindows::system_menu(uint64_t window)
{
	if (!std::as_const(objects).window(window))
		return 0;
	auto found = system_menus.find(window);
	if (found == system_menus.end() || !objects.menu(found->second)) {
		DesktopMenu menu;
		const auto item = [](uint32_t id, const char16_t *text) { return DesktopMenuItem{0, 0, id, text}; };
		menu.items = {item(0xf120, u"&Restore"), item(0xf010, u"&Move"), item(0xf000, u"&Size"), item(0xf020, u"Mi&nimize"),
			item(0xf030, u"Ma&ximize"), DesktopMenuItem{0x800, 0, 0, u""}, item(0xf060, u"&Close\tAlt+F4")};
		const auto handle = objects.add(std::move(menu));
		if (!handle)
			return 0;
		found = system_menus.insert_or_assign(window, handle).first;
	}
	return found->second;
}

}
