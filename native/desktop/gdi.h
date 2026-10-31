#pragma once

#include "objects.h"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace raven {

constexpr uint32_t rop_srccopy = 0x00cc0020, rop_patcopy = 0x00f00021;

// COLORREF 0x00bbggrr.
inline uint32_t color_red(uint32_t color) { return color & 0xff; }
inline uint32_t color_green(uint32_t color) { return (color >> 8) & 0xff; }
inline uint32_t color_blue(uint32_t color) { return (color >> 16) & 0xff; }

// Combines two 8-bit channels with a ternary raster operation (pattern, source, destination).
uint8_t combine_ternary(uint32_t operation, uint8_t pattern, uint8_t source, uint8_t destination);

// Drawing on one device context. Coordinates are logical; the painter adds the DC origin and honors every clip.
class Painter {
public:
	// Throws a Windows error code when the DC has no drawable 32-bit surface.
	Painter(DesktopObjects &objects, DesktopDC &dc);
	DesktopObjects &objects;
	DesktopDC &dc;
	DesktopBitmap &bitmap;

	bool visible(int64_t device_x, int64_t device_y) const;
	// A device-space pixel with raster operation applied; returns false when it was clipped away.
	bool plot(int64_t device_x, int64_t device_y, uint32_t color, uint32_t rop2 = 13);
	uint32_t read(int64_t device_x, int64_t device_y) const;
	// Fills logical pixels [left,right) x [top,bottom) with a brush, aligned to the brush origin.
	void fill(int64_t left, int64_t top, int64_t right, int64_t bottom, const DesktopBrush &brush);
	// Draws a pen stroke between logical points, optionally including the last point.
	struct Stroke { int64_t distance = 0; };
	void line(int64_t x0, int64_t y0, int64_t x1, int64_t y1, const DesktopPen &pen, bool include_end, Stroke &stroke);
	void polyline(std::span<const std::pair<int32_t, int32_t>> points, const DesktopPen &pen, bool closed);
	void ellipse(int64_t left, int64_t top, int64_t right, int64_t bottom, int64_t round_width, int64_t round_height, bool rounded);
	void polygon(std::span<const std::pair<int32_t, int32_t>> points);
	bool changed() const { return dirty; }
	// Raw BGRA bytes of a 32-bit surface pixel (no clip test); mark() records that the caller modified it.
	uint8_t *pixel_pointer(int64_t device_x, int64_t device_y) const;
	void mark() { dirty = true; }

private:
	uint8_t *row_pointer(int64_t device_y) const;
	void stamp(int64_t x, int64_t y, const DesktopPen &pen);
	bool dirty = false;
};

// The color a brush paints at a pattern position (second value false when the pixel is left alone).
std::pair<uint32_t, bool> brush_color_at(const DesktopBrush &brush, const DesktopDC &dc, int64_t pattern_x, int64_t pattern_y);

// What GetDeviceCaps answers for a device context; throws a Windows error code when it has no bitmap.
uint32_t device_caps(const DesktopObjects &objects, const DesktopDC &dc, int32_t index);

// Raster-operation transfer between device contexts (source may be null for pattern operations); true when pixels changed.
// Bytes per row, and the COLORREF of one pixel of a row, for a bitmap of any depth.
size_t bitmap_row_bytes(const DesktopBitmap &bitmap);
uint32_t bitmap_color(const DesktopBitmap &bitmap, const uint8_t *row, int64_t x);
bool blit(DesktopObjects &objects, DesktopDC &destination, int32_t dx, int32_t dy, int32_t dw, int32_t dh, DesktopDC *source_dc,
	int32_t sx, int32_t sy, int32_t sw, int32_t sh, uint32_t rop);

// Resolves the brush and pen a DC has selected, including the stock DC brush and DC pen colors.
const DesktopBrush &selected_brush(DesktopObjects &objects, const DesktopDC &dc);
const DesktopPen &selected_pen(DesktopObjects &objects, const DesktopDC &dc);

}
