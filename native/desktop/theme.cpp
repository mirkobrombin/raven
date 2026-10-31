#include "windows.h"
#include "gdi.h"
#include "shapes.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sys/mman.h>

// Visual styles: the uxtheme interface the common controls draw through, answered with the look of Windows 11 (rounded controls,
// light fills, thin borders and the accent color) drawn with antialiasing into the caller's device context. The classic look stays
// available: RAVEN_THEME=classic turns the styles off, as switching off the theme service does on Windows, and every control then
// draws itself the classic way.

namespace raven {

namespace {

using shapes::mix;

// A device context as a surface for the shapes: its clip and visible region decide which pixels change.
struct PainterSurface {
	Painter &painter;
	void blend(int64_t x, int64_t y, uint32_t color, double alpha)
	{
		if (!painter.visible(x, y))
			return;
		painter.plot(x, y, alpha >= 0.998 ? color : mix(painter.read(x, y), color, alpha));
	}
};

using Shapes = shapes::Shapes<PainterSurface>;

constexpr uint32_t s_ok = 0, e_fail = 0x80004005U, e_invalidarg = 0x80070057U, e_handle = 0x80070006U, e_notimpl = 0x80004001U,
	e_prop = 0x80070490U;

constexpr uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return r | g << 8 | b << 16; }

// The colors controls are drawn with: the Windows 11 light palette, and the dark one the controls of a window take when the program
// gives it a "DarkMode_" theme name, as programs do to follow the system's dark mode.
struct Palette {
	uint32_t accent, accent_hot, accent_pressed, accent_dark;
	uint32_t text, text_secondary, text_disabled, text_on_accent;
	uint32_t control, control_hot, control_pressed, control_disabled, white;
	uint32_t stroke, stroke_bottom, stroke_strong, stroke_disabled, divider;
	uint32_t surface, subtle_hot, subtle_pressed, glyph, thumb, thumb_hot, thumb_pressed, selection, selection_hot;
	uint32_t box, box_hot, box_pressed;
};

constexpr Palette light{
	rgb(0x00, 0x5f, 0xb8), rgb(0x19, 0x6e, 0xbf), rgb(0x31, 0x7e, 0xc6), rgb(0x00, 0x3e, 0x92),
	rgb(0x1a, 0x1a, 0x1a), rgb(0x60, 0x60, 0x60), rgb(0xa0, 0xa0, 0xa0), rgb(0xff, 0xff, 0xff),
	rgb(0xfb, 0xfb, 0xfb), rgb(0xf6, 0xf6, 0xf6), rgb(0xf3, 0xf3, 0xf3), rgb(0xf5, 0xf5, 0xf5), rgb(0xff, 0xff, 0xff),
	rgb(0xe5, 0xe5, 0xe5), rgb(0xc4, 0xc4, 0xc4), rgb(0x8a, 0x8a, 0x8a), rgb(0xc5, 0xc5, 0xc5), rgb(0xe0, 0xe0, 0xe0),
	rgb(0xf3, 0xf3, 0xf3), rgb(0xea, 0xea, 0xea), rgb(0xe0, 0xe0, 0xe0), rgb(0x5c, 0x5c, 0x5c), rgb(0x8a, 0x8a, 0x8a), rgb(0x6e, 0x6e, 0x6e),
	rgb(0x58, 0x58, 0x58), rgb(0xcc, 0xe8, 0xff), rgb(0xe5, 0xf3, 0xff),
	rgb(0xf6, 0xf6, 0xf6), rgb(0xed, 0xed, 0xed), rgb(0xe5, 0xe5, 0xe5)};

constexpr Palette dark{
	rgb(0x4c, 0xc2, 0xff), rgb(0x47, 0xb1, 0xe8), rgb(0x42, 0xa1, 0xd2), rgb(0x3a, 0x96, 0xc7),
	rgb(0xff, 0xff, 0xff), rgb(0xc5, 0xc5, 0xc5), rgb(0x6e, 0x6e, 0x6e), rgb(0x00, 0x00, 0x00),
	rgb(0x2d, 0x2d, 0x2d), rgb(0x32, 0x32, 0x32), rgb(0x27, 0x27, 0x27), rgb(0x2a, 0x2a, 0x2a), rgb(0x1f, 0x1f, 0x1f),
	rgb(0x3a, 0x3a, 0x3a), rgb(0x4a, 0x4a, 0x4a), rgb(0x9a, 0x9a, 0x9a), rgb(0x4a, 0x4a, 0x4a), rgb(0x3a, 0x3a, 0x3a),
	rgb(0x20, 0x20, 0x20), rgb(0x2d, 0x2d, 0x2d), rgb(0x29, 0x29, 0x29), rgb(0xc5, 0xc5, 0xc5), rgb(0x9f, 0x9f, 0x9f), rgb(0xb0, 0xb0, 0xb0),
	rgb(0x8a, 0x8a, 0x8a), rgb(0x26, 0x4f, 0x78), rgb(0x2d, 0x2d, 0x2d),
	rgb(0x27, 0x27, 0x27), rgb(0x2f, 0x2f, 0x2f), rgb(0x35, 0x35, 0x35)};

struct Box {
	double left, top, right, bottom;
	double width() const { return right - left; }
	double height() const { return bottom - top; }
	double cx() const { return (left + right) / 2; }
	double cy() const { return (top + bottom) / 2; }
};

// A control surface: rounded, filled, with a light border whose bottom edge is a shade darker, as Windows 11 buttons and fields.
void control_surface(Shapes &shapes, const Box &box, uint32_t fill, uint32_t border, std::optional<uint32_t> bottom, double radius = 4)
{
	shapes.rounded(box.left, box.top, box.right, box.bottom, radius, fill, border);
	if (bottom)
		shapes.segment(box.left + radius, box.bottom - 0.5, box.right - radius, box.bottom - 0.5, 1, *bottom);
}

void draw_button(Shapes &shapes, const Palette &tone, int part, int state, const Box &box)
{
	switch (part) {
	case 1: { // BP_PUSHBUTTON: normal, hot, pressed, disabled, defaulted, defaulted animating
		if (state == 5 || state == 6) {
			control_surface(shapes, box, tone.accent, tone.accent, tone.accent_dark);
			return;
		}
		const uint32_t fill = state == 2 ? tone.control_hot : state == 3 ? tone.control_pressed : state == 4 ? tone.control_disabled : tone.control;
		control_surface(shapes, box, fill, tone.stroke, state == 3 || state == 4 ? std::nullopt : std::optional(tone.stroke_bottom));
		return;
	}
	case 2: { // BP_RADIOBUTTON: unchecked and checked, each normal, hot, pressed, disabled
		const double size = std::min(box.width(), box.height()), radius = size / 2, cx = box.left + radius, cy = box.cy();
		const int phase = (state - 1) % 4;
		if (state <= 4) {
			shapes.circle(cx, cy, radius, phase == 1 ? tone.box_hot : phase == 2 ? tone.box_pressed : tone.box,
				phase == 3 ? tone.stroke_disabled : tone.stroke_strong);
		} else {
			shapes.circle(cx, cy, radius, phase == 3 ? tone.stroke_disabled : phase == 1 ? tone.accent_hot : phase == 2 ? tone.accent_pressed : tone.accent,
				std::nullopt);
			shapes.circle(cx, cy, radius * (phase == 1 ? 0.5 : phase == 2 ? 0.36 : 0.43), tone.text_on_accent, std::nullopt);
		}
		return;
	}
	case 3: { // BP_CHECKBOX: unchecked, checked, mixed, implicit and excluded, each normal, hot, pressed, disabled
		const double size = std::min(box.width(), box.height());
		const Box square{box.left, box.cy() - size / 2, box.left + size, box.cy() + size / 2};
		const int kind = (state - 1) / 4, phase = (state - 1) % 4;
		if (kind == 0 || kind > 2) {
			shapes.rounded(square.left, square.top, square.right, square.bottom, 3,
				phase == 1 ? tone.box_hot : phase == 2 ? tone.box_pressed : tone.box,
				phase == 3 ? tone.stroke_disabled : tone.stroke_strong);
			return;
		}
		shapes.rounded(square.left, square.top, square.right, square.bottom, 3,
			phase == 3 ? tone.stroke_disabled : phase == 1 ? tone.accent_hot : phase == 2 ? tone.accent_pressed : tone.accent, std::nullopt);
		const double s = size;
		if (kind == 1) {
			shapes.segment(square.left + s * 0.24, square.top + s * 0.52, square.left + s * 0.42, square.top + s * 0.70, s * 0.11, tone.text_on_accent);
			shapes.segment(square.left + s * 0.42, square.top + s * 0.70, square.left + s * 0.78, square.top + s * 0.32, s * 0.11, tone.text_on_accent);
		} else {
			shapes.segment(square.left + s * 0.28, square.cy(), square.right - s * 0.28, square.cy(), s * 0.11, tone.text_on_accent);
		}
		return;
	}
	case 4: // BP_GROUPBOX
		shapes.rounded(box.left, box.top, box.right, box.bottom, 4, std::nullopt, tone.stroke);
		return;
	default:
		control_surface(shapes, box, state == 2 ? tone.control_hot : tone.control, tone.stroke, tone.stroke_bottom);
	}
}

// A text field: white when it has the keyboard, with a strong bottom line that turns into the accent.
void draw_field(Shapes &shapes, const Palette &tone, int state, const Box &box, bool focused)
{
	const bool disabled = state == 4, hot = state == 2;
	shapes.rounded(box.left, box.top, box.right, box.bottom, 4, disabled ? tone.control_disabled : focused ? tone.white : hot ? tone.control_hot : tone.control,
		tone.stroke);
	if (focused)
		shapes.rounded(box.left + 1, box.bottom - 2, box.right - 1, box.bottom, 1, tone.accent, std::nullopt);
	else if (!disabled)
		shapes.segment(box.left + 3, box.bottom - 0.5, box.right - 3, box.bottom - 0.5, 1, tone.stroke_strong);
}

void draw_scrollbar(Shapes &shapes, const Palette &tone, int part, int state, const Box &box)
{
	switch (part) {
	case 1: { // SBP_ARROWBTN: up, down, left, right; each normal, hot, pressed, disabled, then the hover states
		const int direction = state <= 16 ? (state - 1) / 4 : (state - 17) % 4;
		const int phase = state <= 16 ? (state - 1) % 4 : 1;
		const uint32_t color = phase == 3 ? tone.stroke_disabled : phase == 2 ? tone.thumb_pressed : phase == 1 ? tone.thumb_hot : tone.thumb;
		shapes.fill(box.left, box.top, box.right, box.bottom, tone.surface);
		shapes.triangle(box.cx(), box.cy(), std::min(box.width(), box.height()) * 0.42, "udlr"[direction], color);
		return;
	}
	case 2: case 3: { // SBP_THUMBBTNHORZ, SBP_THUMBBTNVERT: a pill narrower than the bar
		const bool vertical = part == 3;
		const uint32_t color = state == 3 ? tone.thumb_pressed : state == 2 ? tone.thumb_hot : tone.thumb;
		shapes.fill(box.left, box.top, box.right, box.bottom, tone.surface);
		const double thickness = std::max(3.0, (vertical ? box.width() : box.height()) * 0.4);
		if (vertical)
			shapes.rounded(box.cx() - thickness / 2, box.top + 2, box.cx() + thickness / 2, box.bottom - 2, thickness / 2, color, std::nullopt);
		else
			shapes.rounded(box.left + 2, box.cy() - thickness / 2, box.right - 2, box.cy() + thickness / 2, thickness / 2, color, std::nullopt);
		return;
	}
	case 4: case 5: case 6: case 7: // the tracks above, below and beside the thumb
		shapes.fill(box.left, box.top, box.right, box.bottom, tone.surface);
		return;
	case 10: // SBP_SIZEBOX: three dots in a corner
		shapes.fill(box.left, box.top, box.right, box.bottom, tone.surface);
		for (int dot = 0; dot < 3; ++dot)
			shapes.circle(box.right - 4 - dot * 4, box.bottom - 4, 1, tone.thumb, std::nullopt), shapes.circle(box.right - 4, box.bottom - 4 - dot * 4, 1, tone.thumb, std::nullopt);
		return;
	default:
		return;
	}
}

void draw_header(Shapes &shapes, const Palette &tone, int part, int state, const Box &box)
{
	if (part == 4) { // HP_HEADERSORTARROW: sorted up, sorted down
		shapes.chevron(box.cx(), box.cy(), std::min(box.width(), 8.0), state == 2 ? 'd' : 'u', tone.text_secondary);
		return;
	}
	const uint32_t fill = state == 3 || state == 6 || state == 9 ? tone.subtle_pressed : state == 2 || state == 5 || state == 8 ? rgb(0xf5, 0xf5, 0xf5)
		: tone.white;
	shapes.fill(box.left, box.top, box.right, box.bottom, fill);
	if (part != 0)
		shapes.fill(box.right - 1, box.top + 4, box.right, box.bottom - 4, tone.divider);
	shapes.fill(box.left, box.bottom - 1, box.right, box.bottom, tone.divider);
}

void draw_tab(Shapes &shapes, const Palette &tone, int part, int state, const Box &box)
{
	if (part == 9 || part == 10) { // TABP_PANE, TABP_BODY
		shapes.rounded(box.left, box.top, box.right, box.bottom, 4, tone.white, tone.stroke);
		return;
	}
	if (state == 3 || state == 5) // selected or focused: joined to the pane
		shapes.rounded(box.left, box.top, box.right, box.bottom + 4, 4, tone.white, tone.stroke);
	else if (state == 2)
		shapes.rounded(box.left + 1, box.top + 2, box.right - 1, box.bottom - 1, 4, tone.subtle_hot, std::nullopt);
}

void draw_toolbar(Shapes &shapes, const Palette &tone, int part, int state, const Box &box)
{
	if (part == 5 || part == 6) { // TP_SEPARATOR, TP_SEPARATORVERT
		if (part == 5)
			shapes.fill(std::floor(box.cx()), box.top + 3, std::floor(box.cx()) + 1, box.bottom - 3, tone.divider);
		else
			shapes.fill(box.left + 3, std::floor(box.cy()), box.right - 3, std::floor(box.cy()) + 1, tone.divider);
		return;
	}
	const bool checked = state == 5 || state == 6;
	if (state == 2 || state == 3 || checked)
		shapes.rounded(box.left, box.top, box.right, box.bottom, 4, state == 3 ? tone.subtle_pressed : checked && state != 6 ? rgb(0xe5, 0xe5, 0xe5)
			: tone.subtle_hot, checked ? std::optional(rgb(0xd0, 0xd0, 0xd0)) : std::nullopt);
	if (part == 2 || part == 4) // the dropdown arrow
		shapes.chevron(part == 4 ? box.cx() : box.right - 6, box.cy(), 6, 'd', state == 4 ? tone.text_disabled : tone.glyph);
}

void draw_part(Shapes &shapes, const Palette &tone, const std::string &klass, int part, int state, const Box &box, bool focused)
{
	if (klass == "button")
		draw_button(shapes, tone, part, state, box);
	else if (klass == "edit")
		draw_field(shapes, tone, state, box, focused || state == 5);
	else if (klass == "combobox") {
		if (part == 1 || part == 6) { // CP_DROPDOWNBUTTON, CP_DROPDOWNBUTTONRIGHT
			if (state == 2 || state == 3)
				shapes.rounded(box.left + 2, box.top + 2, box.right - 2, box.bottom - 2, 3, state == 3 ? tone.subtle_pressed : tone.subtle_hot, std::nullopt);
			shapes.chevron(box.cx(), box.cy(), 8, 'd', state == 4 ? tone.text_disabled : tone.glyph);
		} else {
			draw_field(shapes, tone, state, box, focused);
		}
	} else if (klass == "listbox")
		shapes.rounded(box.left, box.top, box.right, box.bottom, 4, std::nullopt, tone.stroke);
	else if (klass == "scrollbar")
		draw_scrollbar(shapes, tone, part, state, box);
	else if (klass == "header")
		draw_header(shapes, tone, part, state, box);
	else if (klass == "treeview") {
		if (part == 2) // TVP_GLYPH: closed, opened
			shapes.chevron(box.cx(), box.cy(), std::min(box.width(), box.height()) * 0.5, state == 2 ? 'd' : 'r', tone.glyph);
		else if (part == 1 && state >= 2 && state != 4)
			shapes.rounded(box.left, box.top, box.right, box.bottom, 4, state == 2 ? tone.selection_hot : tone.selection, std::nullopt);
	} else if (klass == "listview") {
		if (part == 1 && state >= 2 && state != 4)
			shapes.rounded(box.left, box.top, box.right, box.bottom, 4, state == 2 ? tone.selection_hot : tone.selection, std::nullopt);
	} else if (klass == "tab")
		draw_tab(shapes, tone, part, state, box);
	else if (klass == "toolbar")
		draw_toolbar(shapes, tone, part, state, box);
	else if (klass == "rebar") {
		if (part == 4) // RP_CHEVRON
			shapes.chevron(box.cx(), box.cy(), 6, 'r', tone.glyph);
		else if (part == 0 || part == 3)
			shapes.fill(box.left, box.top, box.right, box.bottom, tone.surface);
	} else if (klass == "status") {
		if (part == 0)
			shapes.fill(box.left, box.top, box.right, box.bottom, tone.surface);
		else if (part == 1 || part == 2) // SP_PANE, SP_GRIPPERPANE: a divider at the right
			shapes.fill(box.right - 1, box.top + 3, box.right, box.bottom - 3, tone.divider);
		else if (part == 3)
			for (int row = 0; row < 3; ++row)
				for (int column = 0; column <= row; ++column)
					shapes.circle(box.right - 3 - column * 4, box.bottom - 3 - (2 - row) * 4, 1, tone.thumb, std::nullopt);
	} else if (klass == "progress") {
		if (part == 1 || part == 2) // the track: a thin rounded groove
			shapes.rounded(box.left, box.cy() - 2, box.right, box.cy() + 2, 2, rgb(0xd6, 0xd6, 0xd6), std::nullopt);
		else if (part == 3 || part == 4 || part == 5 || part == 6)
			shapes.fill(box.left, std::max(box.top, box.cy() - 2), box.right, std::min(box.bottom, box.cy() + 2), tone.accent);
	} else if (klass == "trackbar") {
		if (part == 1)
			shapes.rounded(box.left, box.cy() - 2, box.right, box.cy() + 2, 2, tone.thumb, std::nullopt);
		else if (part == 2)
			shapes.rounded(box.cx() - 2, box.top, box.cx() + 2, box.bottom, 2, tone.thumb, std::nullopt);
		else if (part >= 3 && part <= 8) {
			const double radius = std::min(box.width(), box.height()) / 2;
			shapes.circle(box.cx(), box.cy(), radius, tone.white, tone.stroke);
			shapes.circle(box.cx(), box.cy(), radius * (state == 2 ? 0.6 : state == 3 ? 0.4 : 0.5), state == 5 ? tone.stroke_disabled : tone.accent,
				std::nullopt);
		}
	} else if (klass == "spin") {
		if (state == 2 || state == 3)
			shapes.rounded(box.left, box.top, box.right, box.bottom, 3, state == 3 ? tone.subtle_pressed : tone.subtle_hot, std::nullopt);
		const char direction = part == 1 ? 'u' : part == 2 ? 'd' : part == 3 ? 'r' : 'l';
		shapes.chevron(box.cx(), box.cy(), std::min(box.width(), box.height()) * 0.45, direction, state == 4 ? tone.text_disabled : tone.glyph);
	}
}

// The color of text on a part, which the controls ask for and which DrawThemeText uses.
uint32_t text_color(const Palette &tone, const std::string &klass, int part, int state)
{
	if (klass == "button") {
		if (part == 1 && (state == 5 || state == 6))
			return tone.text_on_accent;
		if ((part == 1 && state == 4) || ((part == 2 || part == 3) && (state - 1) % 4 == 3))
			return tone.text_disabled;
		return tone.text;
	}
	if ((klass == "tab" || klass == "toolbar" || klass == "edit" || klass == "combobox") && state == 4)
		return tone.text_disabled;
	return tone.text;
}

// A clip region narrowed to a rectangle.
std::optional<DesktopRegion> clipped(const std::optional<DesktopRegion> &clip, const Box &box)
{
	DesktopRegion region;
	region.add({int32_t(box.left), int32_t(box.top), int32_t(box.right), int32_t(box.bottom)});
	if (!clip)
		return region;
	DesktopRegion both;
	both.combine(*clip, region, 1);
	return both;
}

bool rounded_part(const std::string &klass, int part)
{
	return klass == "button" || (klass == "tab" && part != 9 && part != 10) || klass == "combobox" || klass == "edit" || klass == "spin";
}

}

bool NativeWindows::themed() const
{
	static const bool classic = [] {
		const char *chosen = std::getenv("RAVEN_THEME");
		return chosen && std::string_view(chosen) == "classic";
	}();
	return !classic;
}

void NativeWindows::register_theme_api(ApiRegistry &registry)
{
	const auto palette = [this](uint64_t theme) -> const Palette & { return dark_themes.contains(theme) ? dark : light; };
	// A window a program allowed dark mode takes the dark variant of the application names it opens, as the system does while the app
	// mode forces dark; the system itself is light, so allowing dark alone changes nothing.
	const auto dark_allowed = [this](uint64_t window) { return app_mode == 2 && dark_allowed_windows.contains(window); };
	const auto open = [this, dark_allowed](ApiCall &call, uint64_t window, uint64_t list) {
		if (!themed() || (window && unthemed_windows.contains(window)) || !list) {
			call.fail(1168, 0);
			return;
		}
		// The first class of the list this engine draws; a prefix such as "Explorer::" names a variant of the same class, and a
		// "DarkMode_" one, or that application name given to the window, its dark variant.
		static const std::set<std::string> known{"button", "edit", "combobox", "listbox", "scrollbar", "header", "listview", "treeview", "tab", "toolbar",
			"rebar", "status", "progress", "trackbar", "spin", "window", "menu", "tooltip", "explorerbar", "datepicker", "taskdialog", "textstyle"};
		const auto text = call.wide(list, 512);
		size_t start = 0;
		while (start <= text.size()) {
			auto end = text.find(u';', start);
			if (end == text.npos)
				end = text.size();
			std::string name;
			for (size_t at = start; at < end; ++at)
				name.push_back(char(text[at] < 128 ? std::tolower(int(text[at])) : '?'));
			bool dark = window && dark_windows.contains(window);
			if (const auto colon = name.rfind("::"); colon != name.npos) {
				dark = dark || name.find("darkmode_") != name.npos || (window && dark_allowed(window));
				name = name.substr(colon + 2);
			}
			dark = dark || (window && named_windows.contains(window) && dark_allowed(window));
			while (!name.empty() && name.front() == ' ')
				name.erase(name.begin());
			if (known.contains(name)) {
				const uint64_t handle = next_theme++;
				themes[handle] = name;
				if (dark)
					dark_themes.insert(handle);
				if (window)
					window_themes[window] = handle;
				call.ret(handle);
				return;
			}
			start = end + 1;
		}
		call.fail(1168, 0);
	};
	registry.add("OpenThemeData", 2, [open](ApiCall &call) { open(call, call[0], call[1]); });
	registry.add("OpenThemeDataEx", 3, [open](ApiCall &call) { open(call, call[0], call[1]); });
	registry.add("OpenThemeDataForDpi", 3, [open](ApiCall &call) { open(call, call[0], call[1]); });
	registry.add("CloseThemeData", 1, [this](ApiCall &call) {
		dark_themes.erase(call[0]);
		if (!themes.erase(call[0])) {
			call.ret(call[0] ? e_handle : s_ok);
			return;
		}
		std::erase_if(window_themes, [&](const auto &entry) { return entry.second == call[0]; });
		call.ret(s_ok);
	});
	registry.add("GetWindowTheme", 1, [this](ApiCall &call) {
		const auto found = window_themes.find(call[0]);
		call.ret(found != window_themes.end() && themes.contains(found->second) ? found->second : 0);
	});
	// SetWindowTheme with empty names takes a window out of visual styles, as programs do for controls they draw the classic way; a
	// "DarkMode_" application name gives its controls the dark palette.
	registry.add("SetWindowTheme", 3, [this](ApiCall &call) {
		const bool off = call[1] && call[2] && call.wide(call[1], 4).empty() && call.wide(call[2], 4).empty();
		if (off)
			unthemed_windows.insert(call[0]);
		else
			unthemed_windows.erase(call[0]);
		std::u16string application = call[1] ? call.wide(call[1], 64) : std::u16string();
		for (auto &character : application)
			if (character < 128)
				character = char16_t(std::tolower(int(character)));
		if (application.starts_with(u"darkmode_"))
			dark_windows.insert(call[0]);
		else
			dark_windows.erase(call[0]);
		if (!application.empty())
			named_windows.insert(call[0]);
		else
			named_windows.erase(call[0]);
		// The window hears that its theme changed, as controls reopen their theme data then.
		if (objects.window(call[0]))
			send(call[0], 0x31a, 0, 0);
		call.ret(s_ok);
	});
	// The dark mode functions programs take by number: the window and app permissions the theme data follows, and the system's own
	// preference, which is light.
	registry.add("AllowDarkModeForWindow", 2, [this](ApiCall &call) {
		if (!objects.window(call[0])) {
			call.ret(0);
			return;
		}
		if (call.u32(1) & 0xff)
			dark_allowed_windows.insert(call[0]);
		else
			dark_allowed_windows.erase(call[0]);
		call.ret(1);
	});
	registry.add("IsDarkModeAllowedForWindow", 1, [this](ApiCall &call) { call.ret(dark_allowed_windows.contains(call[0])); });
	registry.add("SetPreferredAppMode", 1, [this](ApiCall &call) {
		const uint32_t previous = app_mode;
		if (call.u32(0) <= 3)
			app_mode = call.u32(0);
		call.ret(previous);
	});
	registry.add("IsDarkModeAllowedForApp", 0, [this](ApiCall &call) { call.ret(app_mode == 1 || app_mode == 2); });
	registry.add("ShouldAppsUseDarkMode", 0, [](ApiCall &call) { call.ret(0); });
	registry.add("ShouldSystemUseDarkMode", 0, [](ApiCall &call) { call.ret(0); });
	registry.add("GetIsImmersiveColorUsingHighContrast", 1, [](ApiCall &call) { call.ret(0); });
	registry.add("RefreshImmersiveColorPolicyState", 0, [](ApiCall &call) { call.ret(0); });
	registry.add("FlushMenuThemes", 0, [](ApiCall &call) { call.ret(0); });
	registry.add("IsThemeActive", 0, [this](ApiCall &call) { call.ret(themed()); });
	registry.add("IsAppThemed", 0, [this](ApiCall &call) { call.ret(themed()); });
	registry.add("IsCompositionActive", 0, [this](ApiCall &call) { call.ret(themed()); });
	registry.add("GetThemeAppProperties", 0, [this](ApiCall &call) { call.ret(themed() ? 7 : 0); });
	registry.add("SetThemeAppProperties", 1, [](ApiCall &call) { call.ret(0); });
	registry.add("EnableThemeDialogTexture", 2, [](ApiCall &call) { call.ret(s_ok); });
	registry.add("IsThemeDialogTextureEnabled", 1, [](ApiCall &call) { call.ret(0); });
	registry.add("IsThemePartDefined", 3, [this](ApiCall &call) { call.ret(themes.contains(call[0])); });
	registry.add("IsThemeBackgroundPartiallyTransparent", 3, [this](ApiCall &call) {
		const auto found = themes.find(call[0]);
		call.ret(found != themes.end() && rounded_part(found->second, call.i32(1)));
	});
	const auto read_box = [](ApiCall &call, uint64_t address) {
		const auto bytes = call.read(address, 16);
		int32_t values[4];
		std::memcpy(values, bytes.data(), 16);
		return Box{double(values[0]), double(values[1]), double(values[2]), double(values[3])};
	};
	// DrawThemeBackground(theme, dc, part, state, rect, clip rect) and the Ex form, whose options may carry the clip.
	const auto background = [this, read_box, palette](ApiCall &call, bool extended) {
		const auto found = themes.find(call[0]);
		auto *dc = objects.dc(call[1]);
		if (found == themes.end() || !dc || !call[4]) {
			call.ret(found == themes.end() ? e_handle : e_invalidarg);
			return;
		}
		auto box = read_box(call, call[4]);
		std::optional<Box> clip;
		if (!extended && call[5])
			clip = read_box(call, call[5]);
		else if (extended && call[5]) {
			// DTBGOPTS: size, flags, then the clip rectangle when DTBG_CLIPRECT (1) is set.
			const auto options = call.read(call[5], 24);
			uint32_t flags;
			std::memcpy(&flags, options.data() + 4, 4);
			if (flags & 1)
				clip = read_box(call, call[5] + 8);
		}
		const auto saved_clip = dc->application_clip;
		if (clip)
			dc->application_clip = clipped(dc->application_clip, *clip);
		try {
			Painter painter(objects, *dc);
			PainterSurface surface{painter};
			Shapes shapes{surface, double(dc->origin_x), double(dc->origin_y)};
			draw_part(shapes, palette(call[0]), found->second, call.i32(2), call.i32(3), box, dc->window && dc->window == focus);
			dc->application_clip = saved_clip;
			if (painter.changed())
				if (dc->window && dc->leased)
					drawing(dc->window);
		} catch (uint32_t) {
			dc->application_clip = saved_clip;
		}
		call.ret(s_ok);
	};
	registry.add("DrawThemeBackground", 6, [background](ApiCall &call) { background(call, false); });
	registry.add("DrawThemeBackgroundEx", 6, [background](ApiCall &call) { background(call, true); });
	// The parent paints itself behind a control with rounded corners: its background and client drawing go to the control's context,
	// shifted so that its pixels land where they show through.
	registry.add("DrawThemeParentBackground", 3, [this, read_box](ApiCall &call) {
		auto *dc = objects.dc(call[1]);
		const auto *window = objects.window(call[0]);
		const auto *parent = window && window->parent ? objects.window(window->parent) : nullptr;
		if (!dc || !parent) {
			call.ret(e_fail);
			return;
		}
		const int32_t dx = window->x - parent->x, dy = window->y - parent->y;
		const uint64_t parent_handle = window->parent;
		const auto saved_x = dc->origin_x, saved_y = dc->origin_y;
		const auto saved_clip = dc->application_clip;
		if (call[2])
			dc->application_clip = clipped(dc->application_clip, read_box(call, call[2]));
		// The clip is kept in the control's coordinates, so it moves with the origin.
		if (dc->application_clip)
			dc->application_clip->offset(dx, dy);
		dc->origin_x -= dx;
		dc->origin_y -= dy;
		send(parent_handle, 0x14, call[1], 0);
		send(parent_handle, 0x318, call[1], 4);
		if (auto *again = objects.dc(call[1])) {
			again->origin_x = saved_x;
			again->origin_y = saved_y;
			again->application_clip = saved_clip;
		}
		call.ret(s_ok);
	});
	// The content of a part sits inside its border and padding; the extent is the part around a given content.
	const auto padding = [](const std::string &klass, int part) {
		if (klass == "button" && part == 1)
			return 4;
		if (klass == "edit" || klass == "combobox" || klass == "listbox")
			return 2;
		if (klass == "tab" || klass == "header")
			return 3;
		return 0;
	};
	registry.add("GetThemeBackgroundContentRect", 6, [this, read_box, padding](ApiCall &call) {
		const auto found = themes.find(call[0]);
		if (found == themes.end() || !call[4] || !call[5]) {
			call.ret(found == themes.end() ? e_handle : e_invalidarg);
			return;
		}
		const auto box = read_box(call, call[4]);
		const int inset = padding(found->second, call.i32(2));
		const int32_t values[4]{int32_t(box.left) + inset, int32_t(box.top) + inset, int32_t(box.right) - inset, int32_t(box.bottom) - inset};
		call.write(call[5], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(values), 16));
		call.ret(s_ok);
	});
	registry.add("GetThemeBackgroundExtent", 6, [this, read_box, padding](ApiCall &call) {
		const auto found = themes.find(call[0]);
		if (found == themes.end() || !call[4] || !call[5]) {
			call.ret(found == themes.end() ? e_handle : e_invalidarg);
			return;
		}
		const auto box = read_box(call, call[4]);
		const int inset = padding(found->second, call.i32(2));
		const int32_t values[4]{int32_t(box.left) - inset, int32_t(box.top) - inset, int32_t(box.right) + inset, int32_t(box.bottom) + inset};
		call.write(call[5], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(values), 16));
		call.ret(s_ok);
	});
	// GetThemePartSize(theme, dc, part, state, rect, size kind, SIZE): the glyphs the controls lay out around.
	registry.add("GetThemePartSize", 7, [this](ApiCall &call) {
		const auto found = themes.find(call[0]);
		if (found == themes.end() || !call[6]) {
			call.ret(found == themes.end() ? e_handle : e_invalidarg);
			return;
		}
		const auto &klass = found->second;
		const int part = call.i32(2);
		int32_t size[2]{0, 0};
		if (klass == "button" && (part == 2 || part == 3))
			size[0] = size[1] = 13;
		else if (klass == "treeview" && part == 2)
			size[0] = size[1] = 12;
		else if (klass == "header" && part == 4)
			size[0] = 9, size[1] = 5;
		else if (klass == "trackbar" && part >= 3 && part <= 8)
			size[0] = size[1] = 18;
		else if (klass == "toolbar" && (part == 2 || part == 4))
			size[0] = 13, size[1] = 13;
		else if (klass == "rebar" && part == 4)
			size[0] = size[1] = 16;
		else if (klass == "scrollbar")
			size[0] = size[1] = 17;
		else {
			call.ret(e_notimpl);
			return;
		}
		call.write(call[6], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(size), 8));
		call.ret(s_ok);
	});
	registry.add("GetThemeMargins", 7, [](ApiCall &call) {
		if (call[6]) {
			const int32_t zero[4]{0, 0, 0, 0};
			call.write(call[6], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(zero), 16));
		}
		call.ret(s_ok);
	});
	// GetThemeColor(theme, part, state, property, COLORREF): text, fill and border colors.
	registry.add("GetThemeColor", 5, [this, palette](ApiCall &call) {
		const auto found = themes.find(call[0]);
		if (found == themes.end() || !call[4]) {
			call.ret(found == themes.end() ? e_handle : e_invalidarg);
			return;
		}
		const auto &tone = palette(call[0]);
		uint32_t color;
		switch (call.u32(3)) {
		case 3803: // TMT_TEXTCOLOR
			color = text_color(palette(call[0]), found->second, call.i32(1), call.i32(2));
			break;
		case 3802: // TMT_FILLCOLOR
		case 3821: // TMT_FILLCOLORHINT
			color = found->second == "status" || found->second == "rebar" || found->second == "toolbar" ? tone.surface : tone.white;
			break;
		case 3801: // TMT_BORDERCOLOR
		case 3822: // TMT_BORDERCOLORHINT
			color = tone.stroke;
			break;
		case 3810: // TMT_EDGEHIGHLIGHTCOLOR
		case 3808: // TMT_EDGELIGHTCOLOR
			color = tone.white;
			break;
		case 3811: // TMT_EDGESHADOWCOLOR
		case 3812: // TMT_EDGEDKSHADOWCOLOR
			color = tone.divider;
			break;
		case 3823: // TMT_ACCENTCOLORHINT
			color = tone.accent;
			break;
		default:
			call.ret(e_prop);
			return;
		}
		call.store<uint32_t>(call[4], color);
		call.ret(s_ok);
	});
	// Progress bars fill smoothly: one-pixel chunks with no space between them.
	registry.add("GetThemeInt", 5, [this](ApiCall &call) {
		const auto found = themes.find(call[0]);
		if (found == themes.end() || !call[4]) {
			call.ret(found == themes.end() ? e_handle : e_invalidarg);
			return;
		}
		const uint32_t property = call.u32(3);
		if (found->second == "progress" && (property == 2411 || property == 2412)) {
			call.store<int32_t>(call[4], property == 2411 ? 1 : 0);
			call.ret(s_ok);
			return;
		}
		call.ret(e_prop);
	});
	for (const char *name : {"GetThemeFont", "GetThemeBool", "GetThemeEnumValue", "GetThemeRect", "GetThemePosition", "GetThemeString",
			"GetThemeFilename", "GetThemeMetric", "GetThemeIntList", "GetThemePropertyOrigin", "GetThemeStream"})
		registry.add(name, std::string_view(name) == "GetThemeFont" || std::string_view(name) == "GetThemeMetric" ? 6 : 5,
			[](ApiCall &call) { call.ret(e_prop); });
	registry.add("GetThemeTransitionDuration", 6, [](ApiCall &call) { call.ret(e_prop); });
	registry.add("GetThemeSysColor", 2, [this](ApiCall &call) { call.ret(objects.system_color(call.i32(1))); });
	registry.add("GetThemeSysSize", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("GetThemeSysBool", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("GetThemeSysFont", 3, [](ApiCall &call) { call.ret(e_prop); });
	registry.add("GetThemeSysInt", 3, [](ApiCall &call) { call.ret(e_prop); });
	registry.add("GetThemeSysString", 4, [](ApiCall &call) { call.ret(e_prop); });
	registry.add("GetThemeDocumentationProperty", 4, [](ApiCall &call) { call.ret(e_prop); });
	registry.add("GetCurrentThemeName", 6, [this](ApiCall &call) {
		if (!themed()) {
			call.ret(e_fail);
			return;
		}
		if (call[0] && call.u32(1))
			call.put_wide(call[0], u"C:\\Windows\\resources\\Themes\\Aero\\Aero.msstyles", call.u32(1));
		if (call[2] && call.u32(3))
			call.put_wide(call[2], u"NormalColor", call.u32(3));
		if (call[4] && call.u32(5))
			call.put_wide(call[4], u"NormalSize", call.u32(5));
		call.ret(s_ok);
	});
	// HitTestThemeBackground(theme, dc, part, state, options, rect, region, POINT, code): the point arrives as one 64-bit argument.
	registry.add("HitTestThemeBackground", 9, [read_box](ApiCall &call) {
		if (!call[5] || !call[8]) {
			call.ret(e_invalidarg);
			return;
		}
		// HTCLIENT inside the rectangle, HTNOWHERE outside: the parts have no transparent regions to test.
		const auto box = read_box(call, call[5]);
		const int32_t x = int32_t(call[7] & 0xffffffff), y = int32_t(call[7] >> 32);
		call.store<uint16_t>(call[8], x >= box.left && x < box.right && y >= box.top && y < box.bottom ? 1 : 0);
		call.ret(s_ok);
	});
	// DrawThemeEdge draws the light Windows 11 border on the sides asked for, and answers the content inside it.
	registry.add("DrawThemeEdge", 8, [this, read_box, palette](ApiCall &call) {
		auto *dc = objects.dc(call[1]);
		if (!themes.contains(call[0]) || !dc || !call[4]) {
			call.ret(e_invalidarg);
			return;
		}
		const auto box = read_box(call, call[4]);
		const uint32_t sides = call.u32(6);
		const auto &tone = palette(call[0]);
		try {
			Painter painter(objects, *dc);
			PainterSurface surface{painter};
			Shapes shapes{surface, double(dc->origin_x), double(dc->origin_y)};
			if (sides & 1)
				shapes.fill(box.left, box.top, box.left + 1, box.bottom, tone.stroke);
			if (sides & 2)
				shapes.fill(box.left, box.top, box.right, box.top + 1, tone.stroke);
			if (sides & 4)
				shapes.fill(box.right - 1, box.top, box.right, box.bottom, tone.stroke);
			if (sides & 8)
				shapes.fill(box.left, box.bottom - 1, box.right, box.bottom, tone.stroke);
			if (painter.changed())
				if (dc->window && dc->leased)
					drawing(dc->window);
		} catch (uint32_t) {
		}
		if (call[7]) {
			const int32_t values[4]{int32_t(box.left) + (sides & 1 ? 1 : 0), int32_t(box.top) + (sides & 2 ? 1 : 0), int32_t(box.right) - (sides & 4 ? 1 : 0),
				int32_t(box.bottom) - (sides & 8 ? 1 : 0)};
			call.write(call[7], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(values), 16));
		}
		call.ret(s_ok);
	});
	// DrawThemeText(theme, dc, part, state, text, count, flags, flags2, rect) and the Ex form: the text in the part's color, placed as
	// DrawText places it, through the device context's own text output.
	const auto draw_text = [this, &registry, read_box, palette](ApiCall &call, bool extended) {
		const auto found = themes.find(call[0]);
		auto *dc = objects.dc(call[1]);
		// DrawThemeText has a second flags word where DrawThemeTextEx goes straight on to the rectangle and the options.
		const uint64_t rectangle = call[extended ? 7 : 8];
		if (found == themes.end() || !dc || !call[4] || !rectangle) {
			call.ret(found == themes.end() ? e_handle : e_invalidarg);
			return;
		}
		const int32_t count = call.i32(5);
		auto text = count < 0 ? call.wide(call[4], 4096) : call.wide(call[4], size_t(std::min(count, 4096)));
		if (count >= 0 && text.size() > size_t(count))
			text.resize(size_t(count));
		const uint32_t flags = call.u32(6);
		const auto box = read_box(call, rectangle);
		uint32_t color = text_color(palette(call[0]), found->second, call.i32(2), call.i32(3));
		if (extended && call[8]) {
			// DTTOPTS: size, flags, then the text color when DTT_TEXTCOLOR (1) is set.
			const auto options = call.read(call[8], 12);
			uint32_t options_flags;
			std::memcpy(&options_flags, options.data() + 4, 4);
			if (options_flags & 1)
				std::memcpy(&color, options.data() + 8, 4);
		}
		// Prefix characters mark mnemonics: the ampersand goes, a doubled one stays as one.
		if (!(flags & 0x800)) {
			std::u16string plain;
			for (size_t at = 0; at < text.size(); ++at) {
				if (text[at] == u'&' && at + 1 < text.size())
					++at;
				else if (text[at] == u'&')
					continue;
				plain.push_back(text[at]);
			}
			text = std::move(plain);
		}
		std::vector<std::u16string> lines;
		if (flags & 0x20)
			lines.push_back(text);
		else
			for (size_t start = 0;;) {
				const auto end = text.find(u'\n', start);
				auto line = text.substr(start, end == text.npos ? text.npos : end - start);
				if (!line.empty() && line.back() == u'\r')
					line.pop_back();
				lines.push_back(line);
				if (end == text.npos)
					break;
				start = end + 1;
			}
		const auto extent = registry.find("GetTextExtentPoint32W");
		const auto output = registry.find("ExtTextOutW");
		if (!extent || !output) {
			call.ret(e_fail);
			return;
		}
		const auto scratch = process.map_memory(65536, PROT_READ | PROT_WRITE);
		try {
			const auto measure = [&](const std::u16string &line) {
				call.put_wide(scratch, line, line.size() + 1);
				ApiCall inner(call.process, call.environment, {call[1], scratch, line.size(), scratch + 32768});
				extent->second(inner);
				const auto bytes = call.read(scratch + 32768, 8);
				int32_t size[2];
				std::memcpy(size, bytes.data(), 8);
				return std::pair{size[0], size[1]};
			};
			int32_t line_height = measure(u"Ag").second;
			const int32_t total = line_height * int32_t(lines.size());
			double y = flags & 0x8 ? box.bottom - total : flags & 0x4 ? box.top + (box.height() - total) / 2 : box.top;
			const auto saved_color = dc->text_color;
			const auto saved_mode = dc->background_mode;
			dc->text_color = color;
			dc->background_mode = 1;
			for (auto line : lines) {
				auto width = measure(line).first;
				// DT_END_ELLIPSIS shortens a line that does not fit and ends it with three dots.
				if ((flags & 0x8000) && width > box.width()) {
					while (!line.empty() && measure(line + u"...").first > box.width())
						line.pop_back();
					line += u"...";
					width = measure(line).first;
				}
				const double x = flags & 0x2 ? box.right - width : flags & 0x1 ? box.left + (box.width() - width) / 2 : box.left;
				const int32_t clip[4]{int32_t(box.left), int32_t(box.top), int32_t(box.right), int32_t(box.bottom)};
				call.write(scratch + 32768, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(clip), 16));
				call.put_wide(scratch, line, line.size() + 1);
				ApiCall inner(call.process, call.environment, {call[1], uint64_t(int64_t(std::lround(x))), uint64_t(int64_t(std::lround(y))),
					uint64_t((flags & 0x100) ? 0 : 4), scratch + 32768, scratch, line.size(), 0});
				output->second(inner);
				y += line_height;
			}
			if (auto *again = objects.dc(call[1])) {
				again->text_color = saved_color;
				again->background_mode = saved_mode;
			}
		} catch (...) {
			process.unmap_memory(scratch);
			throw;
		}
		process.unmap_memory(scratch);
		call.ret(s_ok);
	};
	registry.add("DrawThemeText", 9, [draw_text](ApiCall &call) { draw_text(call, false); });
	registry.add("DrawThemeTextEx", 9, [draw_text](ApiCall &call) { draw_text(call, true); });
	// GetThemeTextExtent(theme, dc, part, state, text, count, flags, bounding rect, extent rect): the size the text takes in the DC.
	registry.add("GetThemeTextExtent", 9, [this, &registry](ApiCall &call) {
		if (!themes.contains(call[0]) || !call[4] || !call[8]) {
			call.ret(e_invalidarg);
			return;
		}
		const auto extent = registry.find("GetTextExtentPoint32W");
		const int32_t count = call.i32(5);
		auto text = count < 0 ? call.wide(call[4], 4096) : call.wide(call[4], size_t(std::min(count, 4096)));
		if (count >= 0 && text.size() > size_t(count))
			text.resize(size_t(count));
		const auto scratch = process.map_memory(65536, PROT_READ | PROT_WRITE);
		int32_t size[2]{0, 0};
		try {
			call.put_wide(scratch, text, text.size() + 1);
			ApiCall inner(call.process, call.environment, {call[1], scratch, text.size(), scratch + 32768});
			if (extent)
				extent->second(inner);
			const auto bytes = call.read(scratch + 32768, 8);
			std::memcpy(size, bytes.data(), 8);
		} catch (...) {
			process.unmap_memory(scratch);
			throw;
		}
		process.unmap_memory(scratch);
		int32_t left = 0, top = 0;
		if (call[7]) {
			const auto bytes = call.read(call[7], 8);
			std::memcpy(&left, bytes.data(), 4);
			std::memcpy(&top, bytes.data() + 4, 4);
		}
		const int32_t values[4]{left, top, left + size[0], top + size[1]};
		call.write(call[8], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(values), 16));
		call.ret(s_ok);
	});
	registry.add("DrawThemeIcon", 7, [](ApiCall &call) { call.ret(e_notimpl); });
	registry.add("GetThemeTextMetrics", 5, [](ApiCall &call) { call.ret(e_notimpl); });
	registry.add("GetThemeBackgroundRegion", 5, [](ApiCall &call) { call.ret(e_notimpl); });
	registry.add("BufferedPaintInit", 0, [](ApiCall &call) { call.ret(s_ok); });
	registry.add("BufferedPaintUnInit", 0, [](ApiCall &call) { call.ret(s_ok); });
}

}
