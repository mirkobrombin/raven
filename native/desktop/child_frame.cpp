#include "windows.h"
#include "canvas.h"

#include <utility>

namespace raven {

// Draws the border and edge of every visible child of one top-level surface, as the default non-client paint does.
void NativeWindows::paint_child_frames(uint64_t root)
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
	const auto ring = [&](DesktopRect rect, uint32_t top_left, uint32_t bottom_right) {
		canvas.fill({rect.left, rect.top, rect.right, rect.top + 1}, top_left);
		canvas.fill({rect.left, rect.top, rect.left + 1, rect.bottom}, top_left);
		canvas.fill({rect.left, rect.bottom - 1, rect.right, rect.bottom}, bottom_right);
		canvas.fill({rect.right - 1, rect.top, rect.right, rect.bottom}, bottom_right);
	};
	const auto walk = [&](auto &&self, uint64_t parent) -> void {
		for (const auto handle : objects.child_windows(parent)) {
			const auto *window = std::as_const(objects).window(handle);
			if (!window || !window->visible)
				continue;
			const DesktopRect rect{window->outer.left - top->outer.left, window->outer.top - top->outer.top,
				window->outer.right - top->outer.left, window->outer.bottom - top->outer.top};
			if (window->own_frame) {
				self(self, handle);
				continue;
			}
			// A dialog frame or a sizing border without a caption is a raised edge with the rest of the border in the face colour.
			if ((window->ex_style & 0x1U || window->style & 0x440000U) && (window->style & 0xc00000U) != 0xc00000U) {
				const DesktopRect client{window->x - top->outer.left, window->y - top->outer.top,
					window->x - top->outer.left + window->width, window->y - top->outer.top + window->height};
				const auto face = color(color_button_face);
				canvas.fill({rect.left, rect.top, rect.right, client.top}, face);
				canvas.fill({rect.left, client.bottom, rect.right, rect.bottom}, face);
				canvas.fill({rect.left, client.top, client.left, client.bottom}, face);
				canvas.fill({client.right, client.top, rect.right, client.bottom}, face);
				canvas.edge(rect, color(color_light), color(color_dark_shadow), color(color_button_highlight), color(color_button_shadow));
				self(self, handle);
				continue;
			}
			// With visual styles the sunken edges become the thin light borders of Windows 11 fields and lists, or dark ones for a window
			// given a dark theme variant.
			if (themed() && (window->ex_style & 0x20200U || window->style & 0x800000U)) {
				const bool dark = dark_windows.contains(handle);
				ring(rect, dark ? 0x5a5a5a : 0xd5d5d5, dark ? 0x5a5a5a : 0xd5d5d5);
				if (window->ex_style & 0x200U)
					ring({rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1}, dark ? 0x2b2b2b : 0xffffff, dark ? 0x2b2b2b : 0xffffff);
			} else if (window->ex_style & 0x200U)
				canvas.edge(rect, color(color_button_shadow), color(color_button_highlight), color(color_dark_shadow), color(color_light));
			else if (window->ex_style & 0x20000U)
				ring(rect, color(color_button_shadow), color(color_button_highlight));
			else if (window->style & 0x800000U)
				ring(rect, color(color_window_frame), color(color_window_frame));
			self(self, handle);
		}
	};
	walk(walk, root);
}

}
