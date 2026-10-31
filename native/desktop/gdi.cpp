#include "gdi.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace raven {

uint8_t combine_raster(uint32_t mode, uint8_t pen, uint8_t destination);

namespace {

// 8x8 hatch masks, one byte per row with the leftmost pixel in the high bit.
constexpr uint8_t hatch_masks[6][8] = {
	{0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00},
	{0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08},
	{0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01},
	{0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80},
	{0x08, 0x08, 0x08, 0xff, 0x08, 0x08, 0x08, 0x08},
	{0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81},
};

// Dash patterns in pixels, alternating on and off, for the cosmetic dashed pen styles.
struct Dashes { size_t count; int pattern[6]; };
constexpr Dashes dash_patterns[5] = {
	{0, {}}, {2, {18, 6}}, {2, {3, 3}}, {4, {9, 6, 3, 6}}, {6, {9, 3, 3, 3, 3, 3}},
};

DesktopBitmap &require_bitmap(DesktopObjects &objects, DesktopDC &dc)
{
	auto *bitmap = objects.bitmap(dc.bitmap);
	if (!bitmap)
		throw uint32_t(6);
	const auto depth = bitmap->depth;
	if (depth != 32 && depth != 1 && depth != 4 && depth != 8 && depth != 16 && depth != 24)
		throw uint32_t(120);
	return *bitmap;
}

size_t row_bytes(const DesktopBitmap &bitmap)
{
	if (bitmap.stride)
		return bitmap.stride;
	return bitmap.depth == 1 ? ((size_t(bitmap.width) + 15) / 16) * 2 : size_t(bitmap.width) * 4;
}

uint32_t nearest_entry(const std::vector<uint32_t> &palette, uint32_t color)
{
	uint32_t best = 0;
	int64_t best_distance = INT64_MAX;
	for (uint32_t i = 0; i < palette.size(); ++i) {
		const int64_t dr = int64_t(color_red(color)) - color_red(palette[i]), dg = int64_t(color_green(color)) - color_green(palette[i]),
			db = int64_t(color_blue(color)) - color_blue(palette[i]);
		const int64_t distance = dr * dr + dg * dg + db * db;
		if (distance < best_distance) {
			best_distance = distance;
			best = i;
		}
	}
	return best;
}

// COLORREF of one pixel of any supported depth.
uint32_t get_color(const DesktopBitmap &bitmap, const uint8_t *row, int64_t x)
{
	switch (bitmap.depth) {
	case 1: {
		const unsigned bit = (row[x / 8] >> (7 - x % 8)) & 1;
		return bitmap.palette.empty() ? (bit ? 0xffffff : 0) : bitmap.palette[bit];
	}
	case 4: return bitmap.palette[(row[x / 2] >> (x % 2 ? 0 : 4)) & 15];
	case 8: return bitmap.palette[row[x]];
	case 16: {
		const uint32_t word = row[x * 2] | (uint32_t(row[x * 2 + 1]) << 8);
		return (((word >> 10) & 31) * 255 / 31) | (((word >> 5) & 31) * 255 / 31) << 8 | ((word & 31) * 255 / 31) << 16;
	}
	case 24: return uint32_t(row[x * 3 + 2]) | uint32_t(row[x * 3 + 1]) << 8 | uint32_t(row[x * 3]) << 16;
	default: return uint32_t(row[x * 4 + 2]) | uint32_t(row[x * 4 + 1]) << 8 | uint32_t(row[x * 4]) << 16;
	}
}

void put_color(const DesktopBitmap &bitmap, uint8_t *row, int64_t x, uint32_t color)
{
	switch (bitmap.depth) {
	case 1: {
		bool bit;
		if (bitmap.palette.empty())
			bit = (color_red(color) * 30 + color_green(color) * 59 + color_blue(color) * 11) >= 128 * 100;
		else
			bit = nearest_entry(bitmap.palette, color) != 0;
		const uint8_t mask = uint8_t(0x80 >> (x % 8));
		row[x / 8] = uint8_t(bit ? (row[x / 8] | mask) : (row[x / 8] & ~mask));
		return;
	}
	case 4: {
		const uint8_t index = uint8_t(nearest_entry(bitmap.palette, color));
		row[x / 2] = x % 2 ? uint8_t((row[x / 2] & 0xf0) | index) : uint8_t((row[x / 2] & 0x0f) | index << 4);
		return;
	}
	case 8: row[x] = uint8_t(nearest_entry(bitmap.palette, color)); return;
	case 16: {
		const uint32_t word = ((color_red(color) >> 3) << 10) | ((color_green(color) >> 3) << 5) | (color_blue(color) >> 3);
		row[x * 2] = uint8_t(word); row[x * 2 + 1] = uint8_t(word >> 8);
		return;
	}
	case 24: row[x * 3] = uint8_t(color_blue(color)); row[x * 3 + 1] = uint8_t(color_green(color)); row[x * 3 + 2] = uint8_t(color_red(color)); return;
	default:
		row[x * 4] = uint8_t(color_blue(color)); row[x * 4 + 1] = uint8_t(color_green(color)); row[x * 4 + 2] = uint8_t(color_red(color)); row[x * 4 + 3] = 0;
	}
}

struct BrushPixel {
	bool paint;
	uint32_t color;
};

BrushPixel brush_pixel(const DesktopBrush &brush, const DesktopDC &dc, int64_t pattern_x, int64_t pattern_y)
{
	if (brush.hollow)
		return {false, 0};
	if (brush.style == 2) {
		const auto row = hatch_masks[std::min<uint32_t>(brush.hatch, 5)][size_t(((pattern_y % 8) + 8) % 8)];
		if (row & (0x80 >> (((pattern_x % 8) + 8) % 8)))
			return {true, brush.color};
		return dc.background_mode == 2 ? BrushPixel{true, dc.background_color} : BrushPixel{false, 0};
	}
	if (!brush.pattern.empty() && brush.pattern_width && brush.pattern_height) {
		const auto x = size_t(((pattern_x % brush.pattern_width) + brush.pattern_width) % brush.pattern_width);
		const auto y = size_t(((pattern_y % brush.pattern_height) + brush.pattern_height) % brush.pattern_height);
		const auto value = brush.pattern[y * brush.pattern_width + x];
		if (brush.pattern_mono)
			return {true, value ? dc.background_color : dc.text_color};
		return {true, value};
	}
	return {true, brush.color};
}

}

std::pair<uint32_t, bool> brush_color_at(const DesktopBrush &brush, const DesktopDC &dc, int64_t pattern_x, int64_t pattern_y)
{
	const auto pixel = brush_pixel(brush, dc, pattern_x, pattern_y);
	return {pixel.color, pixel.paint};
}

uint8_t combine_ternary(uint32_t operation, uint8_t pattern, uint8_t source, uint8_t destination)
{
	// Bit n of the operation index (pattern << 2 | source << 1 | destination) selects the result bit.
	const uint8_t table = uint8_t(operation >> 16);
	uint8_t value = 0;
	for (unsigned term = 0; term < 8; ++term)
		if (table & (1U << term))
			value |= uint8_t((term & 4 ? pattern : ~pattern) & (term & 2 ? source : ~source) & (term & 1 ? destination : ~destination));
	return value;
}

Painter::Painter(DesktopObjects &objects, DesktopDC &dc) : objects(objects), dc(dc), bitmap(require_bitmap(objects, dc)) {}

bool Painter::visible(int64_t device_x, int64_t device_y) const
{
	return device_x >= 0 && device_y >= 0 && device_x < bitmap.width && device_y < bitmap.height && dc.contains(device_x, device_y);
}

uint8_t *Painter::pixel_pointer(int64_t device_x, int64_t device_y) const
{
	return row_pointer(device_y) + size_t(device_x) * 4;
}

uint8_t *Painter::row_pointer(int64_t device_y) const
{
	const size_t row = bitmap.top_down ? size_t(device_y) : bitmap.height - 1 - size_t(device_y);
	return bitmap.pixels().data() + row * row_bytes(bitmap);
}

uint32_t Painter::read(int64_t x, int64_t y) const
{
	if (x < 0 || y < 0 || x >= bitmap.width || y >= bitmap.height)
		return 0;
	return get_color(bitmap, row_pointer(y), x);
}

bool Painter::plot(int64_t x, int64_t y, uint32_t color, uint32_t rop2)
{
	if (!visible(x, y))
		return false;
	auto *row = row_pointer(y);
	if (bitmap.depth == 32 && rop2 == 13) {
		auto *pixel = row + size_t(x) * 4;
		pixel[0] = uint8_t(color_blue(color));
		pixel[1] = uint8_t(color_green(color));
		pixel[2] = uint8_t(color_red(color));
		pixel[3] = 0;
		dirty = true;
		return true;
	}
	if (rop2 != 13) {
		const uint32_t below = get_color(bitmap, row, x);
		color = combine_raster(rop2, uint8_t(color_red(color)), uint8_t(color_red(below))) |
			combine_raster(rop2, uint8_t(color_green(color)), uint8_t(color_green(below))) << 8 |
			combine_raster(rop2, uint8_t(color_blue(color)), uint8_t(color_blue(below))) << 16;
	}
	put_color(bitmap, row, x, color);
	dirty = true;
	return true;
}

void Painter::fill(int64_t left, int64_t top, int64_t right, int64_t bottom, const DesktopBrush &brush)
{
	if (brush.hollow)
		return;
	const int64_t x0 = std::max<int64_t>(left + dc.origin_x, 0), y0 = std::max<int64_t>(top + dc.origin_y, 0);
	const int64_t x1 = std::min<int64_t>(right + dc.origin_x, bitmap.width), y1 = std::min<int64_t>(bottom + dc.origin_y, bitmap.height);
	if (x0 >= x1 || y0 >= y1)
		return;
	const int64_t base_x = int64_t(dc.origin_x) - dc.user_dx + dc.brush_org_x, base_y = int64_t(dc.origin_y) - dc.user_dy + dc.brush_org_y;
	const bool solid = brush.style == 0 && brush.pattern.empty();
	for (const auto &area : dc.paintable(x0, y0, x1, y1)) {
		if (solid && dc.rop2 == 13 && bitmap.depth == 32) {
			// The common solid copy writes the first row of each visible rectangle and repeats it.
			const size_t bytes = size_t(area.right - area.left) * 4;
			auto *first = row_pointer(area.top) + size_t(area.left) * 4;
			for (size_t i = 0; i < bytes; i += 4) {
				first[i] = uint8_t(color_blue(brush.color));
				first[i + 1] = uint8_t(color_green(brush.color));
				first[i + 2] = uint8_t(color_red(brush.color));
				first[i + 3] = 0;
			}
			for (int64_t y = area.top + 1; y < area.bottom; ++y)
				std::memcpy(row_pointer(y) + size_t(area.left) * 4, first, bytes);
			dirty = true;
			continue;
		}
		for (int64_t y = area.top; y < area.bottom; ++y)
			for (int64_t x = area.left; x < area.right; ++x) {
				if (solid) {
					plot(x, y, brush.color, dc.rop2);
					continue;
				}
				const auto pixel = brush_pixel(brush, dc, x - base_x, y - base_y);
				if (pixel.paint)
					plot(x, y, pixel.color, dc.rop2);
			}
	}
}

void Painter::stamp(int64_t x, int64_t y, const DesktopPen &pen)
{
	const int64_t px = x + dc.origin_x, py = y + dc.origin_y;
	if (pen.width <= 1) {
		plot(px, py, pen.color, dc.rop2);
		return;
	}
	// Wide pens stamp a disc.
	const double radius = pen.width / 2.0;
	const int64_t extent = int64_t(std::ceil(radius));
	for (int64_t dy = -extent; dy <= extent; ++dy)
		for (int64_t dx = -extent; dx <= extent; ++dx)
			if (double(dx) * dx + double(dy) * dy <= radius * radius + 0.25 && !(pen.width % 2 == 0 && (dx == extent || dy == extent)))
				plot(px + dx, py + dy, pen.color, dc.rop2);
}

void Painter::line(int64_t x0, int64_t y0, int64_t x1, int64_t y1, const DesktopPen &pen, bool include_end, Stroke &stroke)
{
	if (pen.hollow)
		return;
	const int64_t dx = std::llabs(x1 - x0), dy = std::llabs(y1 - y0);
	if (std::max(dx, dy) > 1048576)
		throw uint32_t(120);
	const int64_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
	int64_t error = dx - dy, x = x0, y = y0;
	const bool dashed = pen.style >= 1 && pen.style <= 4 && pen.width <= 1 && !pen.geometric;
	const auto &dash = dash_patterns[dashed ? pen.style : 0];
	int64_t total = 0;
	if (dashed)
		for (size_t i = 0; i < dash.count; ++i)
			total += dash.pattern[i];
	for (;;) {
		const bool last = x == x1 && y == y1;
		if (!last || include_end) {
			bool on = true;
			if (dashed) {
				int64_t position = stroke.distance % total;
				for (size_t i = 0; i < dash.count; ++i) {
					if (position < dash.pattern[i]) {
						on = i % 2 == 0;
						break;
					}
					position -= dash.pattern[i];
				}
			}
			if (on)
				stamp(x, y, pen);
			++stroke.distance;
		}
		if (last)
			break;
		const int64_t twice = 2 * error;
		if (twice > -dy || (twice == -dy && sx < 0)) {
			error -= dy;
			x += sx;
		}
		if (twice < dx || (twice == dx && sy < 0)) {
			error += dx;
			y += sy;
		}
	}
}

void Painter::polyline(std::span<const std::pair<int32_t, int32_t>> points, const DesktopPen &pen, bool closed)
{
	if (points.size() < 2 || pen.hollow)
		return;
	Stroke stroke;
	for (size_t i = 0; i + 1 < points.size(); ++i)
		line(points[i].first, points[i].second, points[i + 1].first, points[i + 1].second, pen, i + 2 == points.size() && !closed, stroke);
	if (closed)
		line(points.back().first, points.back().second, points.front().first, points.front().second, pen, false, stroke);
}

const DesktopBrush &selected_brush(DesktopObjects &objects, const DesktopDC &dc)
{
	static DesktopBrush stock;
	if (const auto *brush = objects.brush(dc.brush))
		return *brush;
	return stock;
}

const DesktopPen &selected_pen(DesktopObjects &objects, const DesktopDC &dc)
{
	static DesktopPen stock;
	if (const auto *pen = objects.pen(dc.pen))
		return *pen;
	return stock;
}

namespace {

// Spans of an ellipse or rounded rectangle for every row, as [first,last] inclusive logical columns.
struct Span { int64_t first, last; bool valid; };

std::vector<Span> outline_rows(int64_t left, int64_t top, int64_t right, int64_t bottom, int64_t round_w, int64_t round_h, bool rounded)
{
	const int64_t width = right - left, height = bottom - top;
	std::vector<Span> rows(static_cast<size_t>(height));
	const double rx = rounded ? std::min<double>(round_w, double(width)) / 2.0 : width / 2.0;
	const double ry = rounded ? std::min<double>(round_h, double(height)) / 2.0 : height / 2.0;
	for (int64_t row = 0; row < height; ++row) {
		const double center_y = row + 0.5;
		double inset = 0;
		if (rounded) {
			double dy = 0;
			if (center_y < ry)
				dy = ry - center_y;
			else if (center_y > height - ry)
				dy = center_y - (height - ry);
			if (dy > 0 && ry > 0)
				inset = rx - rx * std::sqrt(std::max(0.0, 1.0 - (dy / ry) * (dy / ry)));
			rows[size_t(row)] = {left + int64_t(std::floor(inset + 0.5)), right - 1 - int64_t(std::floor(inset + 0.5)), true};
		} else {
			const double dy = (center_y - height / 2.0) / (height / 2.0);
			const double half = (width / 2.0) * std::sqrt(std::max(0.0, 1.0 - dy * dy));
			const double center_x = left + width / 2.0;
			const int64_t first = int64_t(std::ceil(center_x - half - 0.5)), last = int64_t(std::floor(center_x + half - 0.5));
			rows[size_t(row)] = {first, std::max(first, last), true};
		}
	}
	return rows;
}

}

void Painter::ellipse(int64_t left, int64_t top, int64_t right, int64_t bottom, int64_t round_width, int64_t round_height, bool rounded)
{
	if (right <= left || bottom <= top || right - left > 65536 || bottom - top > 65536)
		return;
	const auto &brush = selected_brush(objects, dc);
	const auto &pen = selected_pen(objects, dc);
	const auto rows = outline_rows(left, top, right, bottom, round_width, round_height, rounded);
	const bool outlined = !pen.hollow;
	for (size_t i = 0; i < rows.size(); ++i) {
		const auto &span = rows[i];
		const int64_t y = top + int64_t(i);
		const bool edge_row = i == 0 || i + 1 == rows.size();
		int64_t from = span.first + (outlined ? 1 : 0), to = span.last - (outlined ? 1 : 0);
		if (edge_row && outlined)
			from = to + 1;
		if (!brush.hollow && from <= to)
			fill(from, y, to + 1, y + 1, brush);
	}
	if (!outlined)
		return;
	// The pen follows both flanks and spans the top and bottom rows, joining consecutive rows without gaps.
	Stroke stroke;
	int64_t previous_first = 0, previous_last = 0;
	for (size_t i = 0; i < rows.size(); ++i) {
		const auto &span = rows[i];
		const int64_t y = top + int64_t(i);
		if (i == 0 || i + 1 == rows.size()) {
			line(span.first, y, span.last, y, pen, true, stroke);
		} else {
			line(previous_first, y - 1, span.first, y, pen, true, stroke);
			line(previous_last, y - 1, span.last, y, pen, true, stroke);
		}
		previous_first = span.first;
		previous_last = span.last;
	}
}

void Painter::polygon(std::span<const std::pair<int32_t, int32_t>> points)
{
	if (points.size() < 2)
		return;
	const auto &brush = selected_brush(objects, dc);
	const auto &pen = selected_pen(objects, dc);
	if (!brush.hollow && points.size() >= 3) {
		int64_t min_y = INT64_MAX, max_y = INT64_MIN;
		for (const auto &point : points) {
			min_y = std::min<int64_t>(min_y, point.second);
			max_y = std::max<int64_t>(max_y, point.second);
		}
		if (max_y - min_y > 65536)
			throw uint32_t(120);
		const bool even_odd = dc.poly_fill != 2;
		for (int64_t y = min_y; y < max_y; ++y) {
			// Crossings of the scan line through the pixel centers, with their winding direction.
			std::vector<std::pair<double, int>> crossings;
			for (size_t i = 0; i < points.size(); ++i) {
				const auto &a = points[i], &b = points[(i + 1) % points.size()];
				if (a.second == b.second)
					continue;
				const double center = y + 0.5;
				const double low = std::min(a.second, b.second), high = std::max(a.second, b.second);
				if (center < low || center >= high)
					continue;
				const double t = (center - a.second) / (double(b.second) - a.second);
				crossings.push_back({a.first + t * (double(b.first) - a.first), b.second > a.second ? 1 : -1});
			}
			std::sort(crossings.begin(), crossings.end());
			int winding = 0;
			for (size_t i = 0; i + 1 < crossings.size(); ++i) {
				winding += crossings[i].second;
				const bool inside = even_odd ? (i % 2 == 0) : winding != 0;
				if (!inside)
					continue;
				const int64_t from = int64_t(std::ceil(crossings[i].first - 0.5)), to = int64_t(std::ceil(crossings[i + 1].first - 0.5));
				if (to > from)
					fill(from, y, to, y + 1, brush);
			}
		}
	}
	polyline(points, pen, true);
}


namespace {

struct Source {
	DesktopBitmap *bitmap = nullptr;
	DesktopDC *dc = nullptr;
	uint32_t pixel(int64_t x, int64_t y, const DesktopDC &destination, bool destination_mono) const
	{
		const size_t row = bitmap->top_down ? size_t(y) : bitmap->height - 1 - size_t(y);
		const uint8_t *line = bitmap->pixels().data() + row * row_bytes(*bitmap);
		const uint32_t color = get_color(*bitmap, line, x);
		// A monochrome device-dependent source takes the destination's colors; a color source copied to a monochrome
		// surface turns its background color white and everything else black.
		if (bitmap->depth == 1 && bitmap->palette.empty()) {
			if (destination_mono)
				return color;
			return color ? destination.background_color : destination.text_color;
		}
		if (destination_mono)
			return color == dc->background_color ? 0xffffff : 0;
		return color;
	}
	const uint8_t *raw(int64_t x, int64_t y) const
	{
		const size_t row = bitmap->top_down ? size_t(y) : bitmap->height - 1 - size_t(y);
		return &bitmap->pixels()[row * row_bytes(*bitmap) + size_t(x) * 4];
	}
};

}

size_t bitmap_row_bytes(const DesktopBitmap &bitmap)
{
	return row_bytes(bitmap);
}

uint32_t bitmap_color(const DesktopBitmap &bitmap, const uint8_t *row, int64_t x)
{
	return get_color(bitmap, row, x);
}

bool blit(DesktopObjects &objects, DesktopDC &destination, int32_t dx, int32_t dy, int32_t dw, int32_t dh, DesktopDC *source_dc,
	int32_t sx, int32_t sy, int32_t sw, int32_t sh, uint32_t rop)
{
	Painter painter(objects, destination);
	const uint32_t table = (rop >> 16) & 0xff;
	const auto table_bit = [&](unsigned pattern, unsigned source, unsigned destination) { return (table >> (pattern << 2 | source << 1 | destination)) & 1; };
	bool uses_source = false, uses_pattern = false;
	for (unsigned pattern = 0; pattern < 2; ++pattern)
		for (unsigned destination = 0; destination < 2; ++destination)
			uses_source = uses_source || table_bit(pattern, 0, destination) != table_bit(pattern, 1, destination);
	for (unsigned source = 0; source < 2; ++source)
		for (unsigned destination = 0; destination < 2; ++destination)
			uses_pattern = uses_pattern || table_bit(0, source, destination) != table_bit(1, source, destination);
	Source source;
	if (uses_source) {
		if (!source_dc)
			throw uint32_t(6);
		source.dc = source_dc;
		source.bitmap = objects.bitmap(source_dc->bitmap);
		if (!source.bitmap || !(source.bitmap->depth == 32 || source.bitmap->depth == 1 || source.bitmap->depth == 4 || source.bitmap->depth == 8 || source.bitmap->depth == 16 || source.bitmap->depth == 24))
			throw uint32_t(120);
	}
	if (!dw || !dh)
		return false;
	const bool flip_x = (dw < 0) != (sw < 0), flip_y = (dh < 0) != (sh < 0);
	const int64_t abs_dw = std::abs(int64_t(dw)), abs_dh = std::abs(int64_t(dh));
	const int64_t abs_sw = std::abs(int64_t(sw)), abs_sh = std::abs(int64_t(sh));
	const int64_t dest_left = dw < 0 ? int64_t(dx) + dw + 1 : dx, dest_top = dh < 0 ? int64_t(dy) + dh + 1 : dy;
	const int64_t src_left = (sw < 0 ? int64_t(sx) + sw + 1 : sx) + (source_dc ? source_dc->origin_x : 0),
		src_top = (sh < 0 ? int64_t(sy) + sh + 1 : sy) + (source_dc ? source_dc->origin_y : 0);
	if (abs_dw > 32768 || abs_dh > 32768)
		throw uint32_t(87);
	const bool same_bitmap = uses_source && source.bitmap == &painter.bitmap;
	std::vector<uint8_t> snapshot;
	DesktopBitmap snapshot_bitmap;
	if (same_bitmap) {
		const auto pixels = source.bitmap->pixels();
		snapshot.assign(pixels.begin(), pixels.end());
		snapshot_bitmap = *source.bitmap;
		snapshot_bitmap.shared.reset();
		snapshot_bitmap.bits = std::move(snapshot);
		source.bitmap = &snapshot_bitmap;
	}
	const DesktopBrush *brush = uses_pattern ? &selected_brush(objects, destination) : nullptr;
	const int64_t brush_base_x = int64_t(destination.origin_x) - destination.user_dx + destination.brush_org_x;
	const int64_t brush_base_y = int64_t(destination.origin_y) - destination.user_dy + destination.brush_org_y;
	const bool destination_mono = painter.bitmap.depth == 1 && painter.bitmap.palette.empty();
	const bool fast_copy = rop == rop_srccopy && uses_source && abs_dw == abs_sw && abs_dh == abs_sh && !flip_x && !flip_y &&
		source.bitmap->depth == 32 && painter.bitmap.depth == 32;
	std::vector<DesktopRect> copy_areas;
	if (fast_copy)
		copy_areas = destination.paintable(dest_left + destination.origin_x, dest_top + destination.origin_y,
			dest_left + destination.origin_x + abs_dw, dest_top + destination.origin_y + abs_dh);
	for (int64_t row = 0; row < abs_dh; ++row) {
		const int64_t device_y = dest_top + row + destination.origin_y;
		if (device_y < 0 || device_y >= painter.bitmap.height)
			continue;
		const int64_t source_row_index = abs_dh == abs_sh ? row : row * abs_sh / abs_dh;
		const int64_t source_y = src_top + (flip_y ? abs_sh - 1 - source_row_index : source_row_index);
		if (uses_source && (source_y < 0 || source_y >= source.bitmap->height))
			continue;
		if (fast_copy) {
			// A straight copy moves the visible part of each row at once.
			const int64_t device_left = dest_left + destination.origin_x;
			const int64_t first = std::max<int64_t>({0, -device_left, -src_left});
			const int64_t last = std::min<int64_t>({abs_dw, painter.bitmap.width - device_left, source.bitmap->width - src_left});
			for (const auto &area : copy_areas) {
				if (device_y < area.top || device_y >= area.bottom)
					continue;
				const int64_t from = std::max<int64_t>(device_left + first, area.left), to = std::min<int64_t>(device_left + last, area.right);
				if (from >= to)
					continue;
				std::memcpy(painter.pixel_pointer(from, device_y), source.raw(src_left + (from - device_left), source_y), size_t(to - from) * 4);
				painter.mark();
			}
			continue;
		}
		for (int64_t column = 0; column < abs_dw; ++column) {
			const int64_t device_x = dest_left + column + destination.origin_x;
			if (device_x < 0 || device_x >= painter.bitmap.width || !painter.visible(device_x, device_y))
				continue;
			int64_t source_x = 0;
			if (uses_source) {
				const int64_t source_col_index = abs_dw == abs_sw ? column : column * abs_sw / abs_dw;
				source_x = src_left + (flip_x ? abs_sw - 1 - source_col_index : source_col_index);
				if (source_x < 0 || source_x >= source.bitmap->width)
					continue;
			}
			// Between 32-bit surfaces every byte, alpha included, goes through the raster operation.
			if (painter.bitmap.depth == 32 && (!uses_source || source.bitmap->depth == 32)) {
				uint8_t *target = painter.pixel_pointer(device_x, device_y);
				uint8_t from[4] = {0, 0, 0, 0}, pattern[4] = {0, 0, 0, 0};
				if (uses_source)
					std::memcpy(from, source.raw(source_x, source_y), 4);
				if (brush) {
					const auto pixel = brush_color_at(*brush, destination, device_x - brush_base_x, device_y - brush_base_y);
					const uint32_t color = pixel.second ? pixel.first : painter.read(device_x, device_y);
					pattern[0] = uint8_t(color_blue(color)); pattern[1] = uint8_t(color_green(color)); pattern[2] = uint8_t(color_red(color));
				}
				for (int channel = 0; channel < 4; ++channel)
					target[channel] = combine_ternary(rop, pattern[channel], from[channel], target[channel]);
				painter.mark();
				continue;
			}
			const uint32_t destination_color = painter.read(device_x, device_y);
			uint32_t source_color = uses_source ? source.pixel(source_x, source_y, destination, destination_mono) : 0;
			uint32_t pattern_color = 0;
			if (brush) {
				const auto pixel = brush_color_at(*brush, destination, device_x - brush_base_x, device_y - brush_base_y);
				// A transparent hatch pixel leaves the destination unchanged for pattern operations.
				pattern_color = pixel.second ? pixel.first : destination_color;
			}
			uint32_t result = 0;
			for (int channel = 0; channel < 3; ++channel) {
				const int shift = channel * 8;
				const uint8_t value = combine_ternary(rop, uint8_t(pattern_color >> shift), uint8_t(source_color >> shift), uint8_t(destination_color >> shift));
				result |= uint32_t(value) << shift;
			}
			painter.plot(device_x, device_y, result, 13);
		}
	}
	return painter.changed();
}

}
