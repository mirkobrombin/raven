#pragma once

#include "objects.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace raven::paint {

inline constexpr int color_active_caption = 2, color_inactive_caption = 3, color_menu = 4, color_window_frame = 6, color_menu_text = 7,
	color_caption_text = 9, color_button_face = 15, color_button_shadow = 16, color_button_highlight = 20, color_dark_shadow = 21,
	color_light = 22, color_gradient_active = 27, color_gradient_inactive = 28, color_menu_bar = 30, color_inactive_text = 19;

// BGRA pixels of one surface, either owned or viewed in place.
struct Canvas {
	std::vector<uint8_t> owned{};
	uint8_t *pixels;
	int32_t width, height;

	Canvas(std::vector<uint8_t> storage, int32_t width, int32_t height) : owned(std::move(storage)), pixels(owned.data()), width(width), height(height) {}
	Canvas(uint8_t *view, int32_t width, int32_t height) : pixels(view), width(width), height(height) {}
	Canvas(const Canvas &) = delete;
	Canvas &operator=(const Canvas &) = delete;

	void set(int32_t x, int32_t y, uint32_t rgb)
	{
		if (x < 0 || y < 0 || x >= width || y >= height)
			return;
		const auto at = (size_t(y) * size_t(width) + size_t(x)) * 4;
		pixels[at] = uint8_t(rgb >> 16);
		pixels[at + 1] = uint8_t(rgb >> 8);
		pixels[at + 2] = uint8_t(rgb);
		pixels[at + 3] = 0;
	}

	void fill(DesktopRect rect, uint32_t rgb)
	{
		for (int32_t y = std::max(rect.top, 0); y < std::min(rect.bottom, height); ++y)
			for (int32_t x = std::max(rect.left, 0); x < std::min(rect.right, width); ++x)
				set(x, y, rgb);
	}

	// Two-pixel raised or sunken bevel inside rect, using four shades like DrawEdge.
	void edge(DesktopRect rect, uint32_t outer_top_left, uint32_t outer_bottom_right, uint32_t inner_top_left, uint32_t inner_bottom_right)
	{
		for (int32_t x = rect.left; x < rect.right; ++x) {
			set(x, rect.top, outer_top_left);
			set(x, rect.bottom - 1, outer_bottom_right);
		}
		for (int32_t y = rect.top; y < rect.bottom; ++y) {
			set(rect.left, y, outer_top_left);
			set(rect.right - 1, y, outer_bottom_right);
		}
		set(rect.right - 1, rect.top, outer_bottom_right);
		set(rect.left, rect.bottom - 1, outer_bottom_right);
		for (int32_t x = rect.left + 1; x < rect.right - 1; ++x) {
			set(x, rect.top + 1, inner_top_left);
			set(x, rect.bottom - 2, inner_bottom_right);
		}
		for (int32_t y = rect.top + 1; y < rect.bottom - 1; ++y) {
			set(rect.left + 1, y, inner_top_left);
			set(rect.right - 2, y, inner_bottom_right);
		}
		set(rect.right - 2, rect.top + 1, inner_bottom_right);
		set(rect.left + 1, rect.bottom - 2, inner_bottom_right);
	}

	// Coverage of one byte a pixel (0..255) laid over the surface in a color: a full one takes the color, a partial one mixes it with
	// what is below.
	void blend(const std::vector<uint8_t> &coverage, int32_t mask_width, int32_t x, int32_t y, uint32_t rgb)
	{
		const int32_t mask_height = int32_t(coverage.size() / size_t(mask_width));
		for (int32_t row = 0; row < mask_height; ++row)
			for (int32_t column = 0; column < mask_width; ++column) {
				const uint32_t alpha = coverage[size_t(row) * size_t(mask_width) + size_t(column)];
				const int32_t px = x + column, py = y + row;
				if (!alpha || px < 0 || py < 0 || px >= width || py >= height)
					continue;
				if (alpha == 255) {
					set(px, py, rgb);
					continue;
				}
				const auto at = (size_t(py) * size_t(width) + size_t(px)) * 4;
				const auto mixed = [&](uint32_t foreground, uint32_t background) { return (foreground * alpha + background * (255 - alpha) + 127) / 255; };
				set(px, py, mixed(rgb & 0xff, pixels[at + 2]) | mixed(rgb >> 8 & 0xff, pixels[at + 1]) << 8 | mixed(rgb >> 16 & 0xff, pixels[at]) << 16);
			}
	}
};

inline uint32_t mix(uint32_t from, uint32_t to, int64_t step, int64_t steps)
{
	if (steps <= 0)
		return from;
	uint32_t result = 0;
	for (int shift = 0; shift <= 16; shift += 8) {
		const int64_t a = (from >> shift) & 0xff, b = (to >> shift) & 0xff;
		result |= uint32_t(a + (b - a) * step / steps) << shift;
	}
	return result;
}

// System colors are COLORREF values (0x00BBGGRR), which is also what Canvas stores; no channel swap is needed.
inline uint32_t rgb(uint32_t colorref)
{
	return colorref;
}

// Fonts of the nonclient area: the qualified sans face at the classic 8 point size.
inline std::array<uint8_t, 92> interface_font()
{
	std::array<uint8_t, 92> definition{};
	const int32_t height = -11;
	for (size_t byte = 0; byte < 4; ++byte)
		definition[byte] = uint8_t(uint32_t(height) >> (byte * 8));
	definition[16] = 0x90;
	definition[17] = 0x01;
	definition[26] = 3;
	const std::string_view name = "Liberation Sans";
	for (size_t index = 0; index < name.size(); ++index)
		definition[28 + index * 2] = uint8_t(name[index]);
	return definition;
}


#ifdef RAVEN_SKIA
inline std::vector<uint16_t> printable(std::u16string_view text)
{
	std::vector<uint16_t> result;
	for (const auto character : text) {
		const bool supported = (character >= 32 && character <= 126) || (character >= 0x00a0 && character <= 0x024f) ||
			(character >= 0x0370 && character <= 0x03ff) || (character >= 0x1e00 && character <= 0x1eff);
		result.push_back(supported ? uint16_t(character) : uint16_t('?'));
	}
	return result;
}

// True when the interface face cannot draw every character, so the text takes the logical path with its fallback faces.
inline bool needs_fallback(std::u16string_view text)
{
	for (const auto character : text)
		if (!((character >= 32 && character <= 126) || (character >= 0x00a0 && character <= 0x024f) ||
			(character >= 0x0370 && character <= 0x03ff) || (character >= 0x1e00 && character <= 0x1eff)))
			return true;
	return false;
}

// Pixel width of one line of interface text, using fallback faces for characters the interface face lacks.
inline int32_t text_width(TextRenderer &renderer, std::u16string_view text)
{
	if (text.empty())
		return 0;
	if (needs_fallback(text)) {
		const std::vector<uint16_t> units(text.begin(), text.end());
		const auto advances = renderer.logical_advances(interface_font(), units);
		return advances.empty() ? 0 : advances.back();
	}
	return renderer.extent(interface_font(), printable(text)).width;
}

// Draws one line of text and returns its pixel width; underline marks a mnemonic character index.
inline int32_t draw_text(TextRenderer &renderer, Canvas &canvas, std::u16string_view text, int32_t x, int32_t y, uint32_t color,
	std::optional<size_t> underline = std::nullopt, int32_t limit = INT32_MAX)
{
	const auto font = interface_font();
	if (needs_fallback(text) && limit == INT32_MAX) {
		const std::vector<uint16_t> units(text.begin(), text.end());
		const auto width = text_width(renderer, text);
		const auto metrics = renderer.logical_metrics(font);
		const uint32_t box_width = uint32_t(width + 2), box_height = uint32_t(metrics.height);
		const auto coverage = renderer.render(font, units, box_width, box_height, 0, 0);
		canvas.blend(coverage, int32_t(box_width), x, y, color);
		return width;
	}
	auto characters = printable(text);
	if (characters.empty())
		return 0;
	auto extent = renderer.extent(font, characters);
	if (extent.width > limit) {
		// Truncate with an ellipsis rather than overdrawing the neighboring control.
		while (characters.size() > 1 && renderer.extent(font, characters).width + 3 * 3 > limit)
			characters.pop_back();
		characters.insert(characters.end(), 3, uint16_t('.'));
		extent = renderer.extent(font, characters);
	}
	const auto metrics = renderer.metrics(font);
	const int32_t mask_width = extent.width + 2, mask_height = extent.height;
	const auto coverage = renderer.smooth(font, characters, uint32_t(mask_width), uint32_t(mask_height), 0, 0);
	canvas.blend(coverage, mask_width, x, y, color);
	if (underline && *underline < characters.size()) {
		const auto before = renderer.extent(font, std::span<const uint16_t>(characters).first(*underline)).width;
		const auto glyph = renderer.extent(font, std::span<const uint16_t>(characters).subspan(*underline, 1)).width;
		for (int32_t offset = 0; offset < glyph; ++offset)
			canvas.set(x + before + offset, y + metrics.ascent + 1, color);
	}
	return extent.width;
}

// Menu text without ampersand markers; the mnemonic index points into the returned text.
inline std::pair<std::u16string, std::optional<size_t>> menu_label(std::u16string_view text)
{
	std::u16string result;
	std::optional<size_t> mnemonic;
	for (size_t index = 0; index < text.size(); ++index) {
		if (text[index] == u'&' && index + 1 < text.size()) {
			if (text[index + 1] != u'&' && !mnemonic)
				mnemonic = result.size();
			++index;
		}
		if (text[index] == u'\t')
			break;
		result.push_back(text[index]);
	}
	return {result, mnemonic};
}
#endif

}
