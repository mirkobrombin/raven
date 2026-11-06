#include "windows.h"
#include "canvas.h"
#include "shapes.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace raven {

using namespace paint;

NativeWindows::FrameLayout NativeWindows::frame_layout(const DesktopWindow &window) const
{
	const auto &metrics = objects.nonclient_metrics();
	const uint32_t style = window.style & ~0x10000000U;
	FrameLayout layout{};
	const int32_t width = window.outer.right - window.outer.left, height = window.outer.bottom - window.outer.top;
	// AdjustWindowRect already encodes the border and caption rules; its deltas are the frame thickness.
	const auto adjusted = adjust_window_rect(metrics, {0, 0, 0, 0}, style, false, window.ex_style);
	layout.border = -adjusted.left;
	layout.caption_height = (style & 0xc00000U) == 0xc00000U ? metrics.caption : 0;
	layout.menu_height = window.menu ? metrics.menu : 0;
	layout.edge = (style & 0x40000U) || (style & 0xc00000U) == 0x400000U || (style & 0xc00000U) == 0xc00000U;
	const int32_t border = layout.border;
	layout.caption = {border, border, width - border, border + std::max(layout.caption_height - 1, 0)};
	layout.menu_bar = {border, border + layout.caption_height, width - border, border + layout.caption_height + layout.menu_height};
	// Windows 11 caption buttons are 46 pixels wide and fill the caption; the classic ones are bevelled boxes inside it.
	const bool modern = themed();
	const int32_t inset = modern ? 0 : 2;
	const int32_t button_height = std::max(layout.caption.bottom - layout.caption.top - 2 * inset + (modern ? 1 : 0), 0);
	const int32_t button_width = std::min(modern ? 46 : metrics.button_x, std::max(layout.caption.right - layout.caption.left, 0));
	int32_t cursor = layout.caption.right - inset;
	layout.close = layout.maximize = layout.minimize = {0, 0, 0, 0};
	if (layout.caption_height && (style & 0x80000U)) {
		layout.close = {cursor - button_width, layout.caption.top + inset, cursor, layout.caption.top + inset + button_height};
		cursor = layout.close.left - inset;
		if (style & 0x30000U) {
			layout.maximize = {cursor - button_width, layout.caption.top + inset, cursor, layout.caption.top + inset + button_height};
			layout.minimize = {layout.maximize.left - button_width, layout.maximize.top, layout.maximize.left, layout.maximize.bottom};
			// WS_MINIMIZEBOX or WS_MAXIMIZEBOX alone leaves the other button visible but disabled in Windows.
			if (!(style & 0x20000U) && !(style & 0x10000U))
				layout.minimize = layout.maximize = {0, 0, 0, 0};
		}
	}
	layout.icon = {layout.caption.left + 2, layout.caption.top + (layout.caption.bottom - layout.caption.top - 16) / 2, layout.caption.left + 18,
		layout.caption.top + (layout.caption.bottom - layout.caption.top - 16) / 2 + 16};
	return layout;
}

uint64_t NativeWindows::nonclient_hit(const DesktopWindow &window,int32_t x,int32_t y) const
{
	constexpr uint64_t nowhere=0,client=1,caption=2,system_menu=3,menu=5,minimize=8,maximize=9,left=10,right=11,top=12,top_left=13,
		top_right=14,bottom=15,bottom_left=16,bottom_right=17,border=18,close=20;
	const auto &outer=window.outer;
	if (x<outer.left || y<outer.top || x>=outer.right || y>=outer.bottom) return nowhere;
	if (x>=window.x && y>=window.y && int64_t(x)<int64_t(window.x)+window.width && int64_t(y)<int64_t(window.y)+window.height) return client;
	const auto layout=frame_layout(window);
	const int32_t px=x-outer.left,py=y-outer.top;
	const auto inside=[&](DesktopRect rect) { return px>=rect.left && px<rect.right && py>=rect.top && py<rect.bottom; };
	if (inside(layout.close)) return close;
	if (inside(layout.maximize)) return maximize;
	if (inside(layout.minimize)) return minimize;
	if (layout.caption_height && (window.style & 0x80000U) && inside(layout.icon)) return system_menu;
	if (layout.caption_height && inside({layout.caption.left,layout.caption.top,layout.caption.right,layout.caption.bottom+1})) return caption;
	if (layout.menu_height && inside(layout.menu_bar)) return menu;
	if (window.style & 0x40000U) {
		const int32_t width=outer.right-outer.left,height=outer.bottom-outer.top,corner=layout.border*2+8;
		const bool west=px<layout.border,east=px>=width-layout.border,north=py<layout.border,south=py>=height-layout.border;
		if (north && px<corner) return top_left;
		if (north && px>=width-corner) return top_right;
		if (south && px<corner) return bottom_left;
		if (south && px>=width-corner) return bottom_right;
		if (west && py<corner) return top_left;
		if (west && py>=height-corner) return bottom_left;
		if (east && py<corner) return top_right;
		if (east && py>=height-corner) return bottom_right;
		if (north) return top;
		if (south) return bottom;
		if (west) return left;
		if (east) return right;
	}
	return border;
}

void NativeWindows::hover_frame(uint64_t handle,uint64_t hit)
{
	// Only a caption button lights up: a move over the caption or a border with none lit before changes nothing.
	const uint64_t window=hit ? handle : 0;
	if (hovered_frame_window==window && hovered_frame_hit==hit)
		return;
	const auto previous=hovered_frame_window;
	hovered_frame_window=window;
	hovered_frame_hit=hit;
	if (!themed())
		return;
	// The frame is drawn from the hover state each time the window is presented. As on Windows, only the frame changes: the client
	// area and the controls in it are not painted again, so they never show erased while the pointer crosses the frame.
	if (previous && previous!=window)
		drawing(previous);
	if (window)
		drawing(window);
}

void NativeWindows::nonclient_pointer(uint64_t handle,uint64_t hit,uint32_t message,int32_t x,int32_t y)
{
	const auto *window=std::as_const(objects).window(handle);
	if (!window) return;
	constexpr uint64_t caption_hit=2,system_menu_hit=3,menu_hit=5,minimize_hit=8,maximize_hit=9,close_hit=20;
	// The window menu opens where the secondary button is released over the caption, or under the icon when the icon is pressed.
	if (message==0x205 && (hit==caption_hit || hit==system_menu_hit)) { system_menu_open(handle,x,y); return; }
	if (message==0x201 && hit==system_menu_hit) {
		const auto layout=frame_layout(*window);
		system_menu_open(handle,layout.icon.left,layout.caption.bottom+1);
		return;
	}
	if (message==0x200) {
		// The caption button under the pointer lights up in the Windows 11 frame.
		hover_frame(handle,hit==minimize_hit || hit==maximize_hit || hit==close_hit ? hit : 0);
		return;
	}
	if (message==0x201) {
		pressed_frame_hit=hit; pressed_frame_window=handle;
		// A drag that starts on the caption or a sizing border continues in the compositor (maximized windows do not move).
		// A tool window shown on its owner is moved here: the compositor moves only the host windows it places itself.
		if (hit==2 && shown_on_owner(*window) && !(window->style & 0x01000000U)) {
			frame_drag={handle,cursor_x,cursor_y,window->outer};
			return;
		}
		if (presentation.interactive && !(window->style & 0x01000000U)) {
			// Hit codes 10..17 are the sizing borders: left, right, top, top-left, top-right, bottom, bottom-left, bottom-right.
			static constexpr uint32_t edges[]{4,8,1,5,9,2,6,10};
			if (hit==2) { presentation.interactive(handle,0); return; }
			if (hit>=10 && hit<=17) { presentation.interactive(handle,edges[hit-10]); return; }
		}
		if (hit==menu_hit) {
			const auto rects=menu_bar_items(*window);
			for (size_t index=0;index<rects.size();++index)
				if (x>=rects[index].left && x<rects[index].right) { menu_open_bar(handle,index); break; }
		}
		return;
	}
	if (message==0x202 && pressed_frame_window==handle && pressed_frame_hit==hit) {
		pressed_frame_hit=0; pressed_frame_window=0;
		if (hit==close_hit) send(handle,0x112,0xf060,0);
		else if (hit==minimize_hit && (window->style & 0x20000U)) send(handle,0x112,0xf020,0);
		// The maximize button of a maximized window is its restore button.
		else if (hit==maximize_hit && (window->style & 0x10000U)) send(handle,0x112,(window->style & 0x01000000U) ? 0xf120 : 0xf030,0);
	}
}

#ifdef RAVEN_SKIA
std::vector<DesktopRect> NativeWindows::menu_bar_items(const DesktopWindow &window)
{
	std::vector<DesktopRect> rects;
	const auto *menu = objects.menu(window.menu);
	if (!menu)
		return rects;
	const auto layout = frame_layout(window);
	auto &renderer = objects.text();
	const auto font = interface_font();
	(void)font;
	std::vector<int32_t> widths;
	for (const auto &item : menu->items)
		widths.push_back(text_width(renderer, menu_label(item.text).first) + 2 * 8);
	// The first right-justified item and every item after it sit against the right edge of the bar.
	size_t split = menu->items.size();
	int32_t trailing = 0;
	for (size_t index = 0; index < menu->items.size(); ++index)
		if (menu->items[index].type & 0x4000U) { split = index; break; }
	for (size_t index = split; index < widths.size(); ++index)
		trailing += widths[index];
	int32_t x = layout.menu_bar.left;
	for (size_t index = 0; index < widths.size(); ++index) {
		if (index == split)
			x = std::max(x, layout.menu_bar.right - trailing);
		rects.push_back({x, layout.menu_bar.top, x + widths[index], layout.menu_bar.bottom});
		x += widths[index];
	}
	return rects;
}

void NativeWindows::paint_nonclient(uint64_t handle)
{
	const auto *window = std::as_const(objects).window(handle);
	if (!window || window->parent || !window->bitmap)
		return;
	const auto layout = frame_layout(*window);
	if (!layout.border && !layout.caption_height && !layout.menu_height)
		return;
	const auto *bitmap = objects.bitmap(window->bitmap);
	if (!bitmap || !bitmap->shared || bitmap->depth != 32)
		return;
	const int32_t width = window->outer.right - window->outer.left, height = window->outer.bottom - window->outer.top;
	const bool is_active = active == handle;
	// The frame is drawn again only when something it shows changed; guest drawing stays inside the client area.
	{
		std::u16string key;
		const auto add = [&](uint64_t value) { key.append(reinterpret_cast<const char16_t *>(&value), 4); };
		add(window->bitmap); add(uint64_t(bitmap->width) << 32 | bitmap->height); add(uint64_t(uint32_t(width)) << 32 | uint32_t(height));
		add(uint64_t(window->style) << 32 | window->ex_style); add(is_active); add(window->menu);
		add(open_menu_item && open_menu_window == handle ? *open_menu_item + 1 : 0);
		add(hovered_frame_window == handle ? hovered_frame_hit : 0);
		add(themed());
		add(uint64_t(uint32_t(window->x - window->outer.left)) << 32 | uint32_t(window->y - window->outer.top));
		key += window->title;
		key += u'\0';
		if (const auto *menu = objects.menu(window->menu))
			for (const auto &item : menu->items) {
				add(uint64_t(item.type) << 32 | item.state);
				key += item.text;
				key += u'\0';
			}
		auto &painted = painted_frames[handle];
		if (painted == key)
			return;
		painted = std::move(key);
	}
	auto *pixels = objects.bitmap(window->bitmap)->pixels().data();
	Canvas canvas{pixels, int32_t(bitmap->width), int32_t(bitmap->height)};
	const auto color = [&](int index) { return rgb(objects.system_color(index)); };
	// A program that draws its own frame answers WM_NCCALCSIZE with a client area over part of the frame: what it drew there stays, and
	// the frame shows only around it.
	const int32_t client_left = std::max(window->x - window->outer.left, 0), client_top = std::max(window->y - window->outer.top, 0);
	const int32_t client_right = std::min(client_left + window->width, int32_t(bitmap->width));
	const int32_t client_bottom = std::min(client_top + window->height, int32_t(bitmap->height));
	std::vector<uint8_t> kept;
	const bool over_frame = client_top < layout.border + layout.caption_height + layout.menu_height && client_right > client_left &&
		client_bottom > client_top;
	const size_t row_bytes = size_t(std::max(client_right - client_left, 0)) * 4;
	if (over_frame)
		for (int32_t y = client_top; y < client_bottom; ++y) {
			const auto *row = pixels + (size_t(y) * bitmap->width + client_left) * 4;
			kept.insert(kept.end(), row, row + row_bytes);
		}
	const auto restore = [&] {
		for (int32_t y = client_top; over_frame && y < client_bottom; ++y)
			std::memcpy(pixels + (size_t(y) * bitmap->width + client_left) * 4, kept.data() + size_t(y - client_top) * row_bytes, row_bytes);
	};
	if (themed()) {
		paint_modern_frame(handle, *window, layout, canvas, is_active, width, height);
		restore();
		return;
	}
	if (layout.border) {
		// The ring first takes the frame color, then the bevel is laid over its outer two pixels.
		const DesktopRect all{0, 0, width, height};
		const DesktopRect inner{layout.border, layout.border, width - layout.border, height - layout.border};
		canvas.fill({all.left, all.top, all.right, inner.top}, color(color_button_face));
		canvas.fill({all.left, inner.bottom, all.right, all.bottom}, color(color_button_face));
		canvas.fill({all.left, inner.top, inner.left, inner.bottom}, color(color_button_face));
		canvas.fill({inner.right, inner.top, all.right, inner.bottom}, color(color_button_face));
		if (layout.edge && layout.border >= 2)
			canvas.edge(all, color(color_light), color(color_dark_shadow), color(color_button_highlight), color(color_button_shadow));
	}
	if (layout.caption_height) {
		const auto from = color(is_active ? color_active_caption : color_inactive_caption);
		const auto to = color(is_active ? color_gradient_active : color_gradient_inactive);
		const auto span = layout.caption.right - layout.caption.left;
		for (int32_t x = layout.caption.left; x < layout.caption.right; ++x)
			canvas.fill({x, layout.caption.top, x + 1, layout.caption.bottom}, mix(from, to, x - layout.caption.left, span - 1));
		canvas.fill({layout.caption.left, layout.caption.bottom, layout.caption.right, layout.caption.bottom + 1}, color(color_button_face));
		auto &renderer = objects.text();
		const auto text_color = color(is_active ? color_caption_text : color_inactive_text);
		const int32_t text_left = layout.caption.left + (window->style & 0x80000U ? 20 : 4);
		int32_t text_right = layout.caption.right - 2;
		for (const auto &button : {layout.minimize, layout.maximize, layout.close})
			if (button.right > button.left)
				text_right = std::min(text_right, button.left - 4);
		const auto metrics = renderer.metrics(interface_font());
		draw_text(renderer, canvas, window->title, text_left, layout.caption.top + (layout.caption.bottom - layout.caption.top - metrics.height) / 2,
			text_color, std::nullopt, text_right - text_left);
		const auto button = [&](DesktopRect rect, int glyph) {
			if (rect.right <= rect.left)
				return;
			canvas.fill(rect, color(color_button_face));
			canvas.edge(rect, color(color_button_highlight), color(color_dark_shadow), color(color_light), color(color_button_shadow));
			const int32_t cx = (rect.left + rect.right) / 2, cy = (rect.top + rect.bottom) / 2;
			const auto ink = color(color_menu_text);
			if (glyph == 0) {
				for (int32_t d = -3; d <= 3; ++d) { canvas.set(cx + d, cy + d, ink); canvas.set(cx + d, cy - d, ink); }
			} else if (glyph == 1) {
				for (int32_t d = -4; d <= 4; ++d) { canvas.set(cx + d, cy - 4, ink); canvas.set(cx + d, cy + 4, ink); canvas.set(cx - 4, cy + d, ink); canvas.set(cx + 4, cy + d, ink); }
			} else {
				for (int32_t d = -3; d <= 3; ++d) canvas.set(cx + d, cy + 4, ink);
			}
		};
		button(layout.close, 0);
		button(layout.maximize, 1);
		button(layout.minimize, 2);
	}
	if (layout.menu_height) {
		canvas.fill(layout.menu_bar, color(color_menu_bar));
		auto &renderer = objects.text();
		const auto metrics = renderer.metrics(interface_font());
		const auto *menu = objects.menu(window->menu);
		const auto rects = menu_bar_items(*window);
		for (size_t index = 0; menu && index < rects.size() && index < menu->items.size(); ++index) {
			const auto &rect = rects[index];
			if (rect.right > layout.menu_bar.right)
				break;
			const auto [label, mnemonic] = menu_label(menu->items[index].text);
			const bool open = open_menu_item && open_menu_window == handle && *open_menu_item == index;
			if (open) {
				canvas.fill(rect, color(13));
				draw_text(renderer, canvas, label, rect.left + 8, rect.top + (rect.bottom - rect.top - metrics.height) / 2, color(14), mnemonic);
			} else {
				draw_text(renderer, canvas, label, rect.left + 8, rect.top + (rect.bottom - rect.top - metrics.height) / 2, color(color_menu_text), mnemonic);
			}
		}
	}
	restore();
}
namespace {

// The frame's pixels as a surface for the antialiased shapes.
struct CanvasSurface {
	Canvas &canvas;
	void blend(int64_t x, int64_t y, uint32_t color, double alpha)
	{
		if (x < 0 || y < 0 || x >= canvas.width || y >= canvas.height)
			return;
		const auto *pixel = canvas.pixels + (size_t(y) * size_t(canvas.width) + size_t(x)) * 4;
		const uint32_t below = uint32_t(pixel[2]) | uint32_t(pixel[1]) << 8 | uint32_t(pixel[0]) << 16;
		canvas.set(int32_t(x), int32_t(y), alpha >= 0.998 ? color : shapes::mix(below, color, alpha));
	}
};

constexpr uint32_t shade(uint32_t r, uint32_t g, uint32_t b) { return r | g << 8 | b << 16; }

}

uint32_t NativeWindows::caption_color(const DesktopWindow &window, bool is_active) const
{
	if (window.dark_frame)
		return is_active ? shade(0x20, 0x20, 0x20) : shade(0x2b, 0x2b, 0x2b);
	return is_active ? shade(0xef, 0xf4, 0xf9) : shade(0xf3, 0xf3, 0xf3);
}

// The minimize, maximize and close buttons of the Windows 11 caption, which also appear over the frame a program extends into its client area.
void NativeWindows::paint_caption_buttons(uint64_t handle, const DesktopWindow &window, const FrameLayout &layout, Canvas &canvas, bool is_active)
{
	CanvasSurface surface{canvas};
	shapes::Shapes<CanvasSurface> shapes{surface};
	const bool dark = window.dark_frame;
	const uint32_t ink = dark ? (is_active ? shade(0xff, 0xff, 0xff) : shade(0x9a, 0x9a, 0x9a)) : is_active ? shade(0x1a, 0x1a, 0x1a) : shade(0x8a, 0x8a, 0x8a);
	const bool maximized = (window.style & 0x01000000U) != 0;
	const auto button = [&](DesktopRect rect, uint64_t hit) {
		if (rect.right <= rect.left)
			return;
		// A button the window's style leaves out is shown greyed beside the one it has, and does nothing.
		const bool enabled = hit == 20 || (window.style & (hit == 9 ? 0x10000U : 0x20000U));
		const bool hot = enabled && hovered_frame_window == handle && hovered_frame_hit == hit;
		const bool pressed = hot && pressed_frame_window == handle && pressed_frame_hit == hit;
		uint32_t glyph = enabled ? ink : dark ? shade(0x5c, 0x5c, 0x5c) : shade(0xc4, 0xc4, 0xc4);
		if (hot && hit == 20) {
			canvas.fill(rect, pressed ? shade(0xc8, 0x3c, 0x30) : shade(0xc4, 0x2b, 0x1c));
			glyph = shade(0xff, 0xff, 0xff);
		} else if (hot) {
			canvas.fill(rect, dark ? (pressed ? shade(0x3a, 0x3a, 0x3a) : shade(0x2d, 0x2d, 0x2d)) : pressed ? shade(0xdc, 0xe0, 0xe5) : shade(0xe3, 0xe7, 0xec));
		}
		const double cx = (rect.left + rect.right) / 2.0, cy = (rect.top + rect.bottom) / 2.0;
		if (hit == 20) {
			shapes.segment(cx - 5, cy - 5, cx + 5, cy + 5, 1, glyph);
			shapes.segment(cx + 5, cy - 5, cx - 5, cy + 5, 1, glyph);
		} else if (hit == 9 && maximized) {
			shapes.rounded(cx - 5, cy - 3, cx + 3, cy + 5, 1, std::nullopt, glyph);
			shapes.segment(cx - 3, cy - 5, cx + 5, cy - 5, 1, glyph);
			shapes.segment(cx + 5, cy - 5, cx + 5, cy + 3, 1, glyph);
		} else if (hit == 9) {
			shapes.rounded(cx - 5, cy - 5, cx + 5, cy + 5, 1, std::nullopt, glyph);
		} else {
			shapes.segment(cx - 5, cy, cx + 5, cy, 1, glyph);
		}
	};
	button(layout.close, 20);
	button(layout.maximize, 9);
	button(layout.minimize, 8);
}

// The Windows 11 frame: a light caption that continues into the sizing border and the menu bar, a thin outline, the title in dark text,
// flat caption buttons with line glyphs that light up under the pointer, red for close, and the open menu item on a rounded highlight.
void NativeWindows::paint_modern_frame(uint64_t handle, const DesktopWindow &window, const FrameLayout &layout, Canvas &canvas, bool is_active, int32_t width,
	int32_t height)
{
	CanvasSurface surface{canvas};
	shapes::Shapes<CanvasSurface> shapes{surface};
	const bool dark = window.dark_frame;
	const uint32_t title = caption_color(window, is_active);
	const uint32_t ink = dark ? (is_active ? shade(0xff, 0xff, 0xff) : shade(0x9a, 0x9a, 0x9a)) : is_active ? shade(0x1a, 0x1a, 0x1a) : shade(0x8a, 0x8a, 0x8a);
	const int32_t client_top = layout.border + layout.caption_height + layout.menu_height;
	// The ring, the caption and the menu bar share one surface color.
	canvas.fill({0, 0, width, client_top}, title);
	canvas.fill({0, client_top, layout.border, height}, title);
	canvas.fill({width - layout.border, client_top, width, height}, title);
	canvas.fill({0, height - layout.border, width, height}, title);
	if (layout.border)
		shapes.rounded(0, 0, width, height, 0, std::nullopt, dark ? (is_active ? shade(0x4a, 0x4a, 0x4a) : shade(0x3c, 0x3c, 0x3c))
			: is_active ? shade(0xb9, 0xbf, 0xc7) : shade(0xd2, 0xd2, 0xd2));
	if (layout.caption_height) {
		auto &renderer = objects.text();
		const int32_t text_left = layout.caption.left + (window.style & 0x80000U ? 22 : 8);
		int32_t text_right = layout.caption.right - 8;
		for (const auto &button : {layout.minimize, layout.maximize, layout.close})
			if (button.right > button.left)
				text_right = std::min(text_right, button.left - 8);
		const auto metrics = renderer.metrics(interface_font());
		draw_text(renderer, canvas, window.title, text_left, layout.caption.top + (layout.caption.bottom - layout.caption.top - metrics.height) / 2,
			ink, std::nullopt, text_right - text_left);
		paint_caption_buttons(handle, window, layout, canvas, is_active);
	}
	if (layout.menu_height) {
		auto &renderer = objects.text();
		const auto metrics = renderer.metrics(interface_font());
		const auto *menu = objects.menu(window.menu);
		const auto rects = menu_bar_items(window);
		for (size_t index = 0; menu && index < rects.size() && index < menu->items.size(); ++index) {
			const auto &rect = rects[index];
			if (rect.right > layout.menu_bar.right)
				break;
			const auto [label, mnemonic] = menu_label(menu->items[index].text);
			if (open_menu_item && open_menu_window == handle && *open_menu_item == index)
				shapes.rounded(rect.left + 1, rect.top + 2, rect.right - 1, rect.bottom - 2, 4, dark ? shade(0x3a, 0x3a, 0x3a) : shade(0xe0, 0xe4, 0xea),
					std::nullopt);
			draw_text(renderer, canvas, label, rect.left + 8, rect.top + (rect.bottom - rect.top - metrics.height) / 2,
				(menu->items[index].state & 3) ? (dark ? shade(0x6e, 0x6e, 0x6e) : shade(0xa0, 0xa0, 0xa0)) : dark ? shade(0xff, 0xff, 0xff)
				: shade(0x1a, 0x1a, 0x1a), mnemonic);
		}
	}
}
#else
void NativeWindows::paint_nonclient(uint64_t)
{
	// Without the text engine, nonclient text cannot be drawn; the window keeps its unpainted frame.
}

std::vector<DesktopRect> NativeWindows::menu_bar_items(const DesktopWindow &)
{
	return {};
}

uint32_t NativeWindows::caption_color(const DesktopWindow &window, bool is_active) const
{
	if (window.dark_frame)
		return is_active ? 0x202020U : 0x2b2b2bU;
	return is_active ? 0xf9f4efU : 0xf3f3f3U;
}

void NativeWindows::paint_caption_buttons(uint64_t, const DesktopWindow &, const FrameLayout &, Canvas &, bool)
{
}
#endif

}
