#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace raven {

// Drawing surfaces for the 2D components a program uses (Direct2D): premultiplied BGRA pixels held by the host, drawn into with Skia from
// commands the guest sends. The state of a surface (clips and layers pushed between BeginDraw and EndDraw) stays with it between calls.
namespace canvas {

// What a command draws with: brush kinds and the layout of the command record, which is the guest's own (32-bit words and floats).
enum class Op : uint32_t { clear = 1, fill_path = 2, stroke_path = 3, draw_bitmap = 4, fill_mask = 5, push_clip = 6, push_layer = 7, pop = 8, copy = 9 };
enum class Brush : uint32_t { none = 0, solid = 1, linear = 2, radial = 3, bitmap = 4 };

// A path is a run of floats: a verb (0 move, 1 line, 2 quadratic, 3 cubic, 4 close) and its points.
struct Command {
	uint32_t op, antialias;
	float transform[6];
	uint32_t brush;
	float opacity;
	float color[4];
	float brush_transform[6];
	float brush_points[6];
	uint32_t stops, extend_x, extend_y, interpolation, bitmap;
	float source[4];
	float stroke_width;
	uint32_t start_cap, end_cap, dash_cap, line_join;
	float miter_limit;
	uint32_t dashes;
	float dash_offset;
	float rect[4];
	uint32_t path_floats, fill_mode, mask_width, mask_height;
	// Followed by `stops` stops of five floats (position, red, green, blue, alpha), `dashes` floats, `path_floats` floats, then a mask
	// of mask_width * mask_height coverage bytes.
};
static_assert(sizeof(Command) == 204);

uint32_t create(uint32_t width, uint32_t height);
void destroy(uint32_t surface);
bool size(uint32_t surface, uint32_t &width, uint32_t &height);
// Copies a rectangle of pixels out of the surface (`write` false) or into it, `pitch` bytes apart.
bool pixels(uint32_t surface, int32_t x, int32_t y, uint32_t width, uint32_t height, uint8_t *data, uint32_t pitch, bool write);
// The rectangle of the surface that differs from what the caller last took of it, as left, top, right and bottom, which the caller
// then holds as taken: a frame is shown by its changes alone. `whole` forgets what was taken, so the whole surface differs. False when
// nothing differs.
bool changed(uint32_t surface, bool whole, int32_t rect[4]);
// Runs one command; the bytes after the record are the variable parts it names. False when the command or its data is not valid.
bool draw(uint32_t surface, const Command &command, std::span<const uint8_t> data);
// Combines two paths of the command's float form (each with its fill mode, 0 alternate and 1 winding) by a Direct2D combine mode:
// 0 union, 1 intersect, 2 xor, 3 exclude. The result is in the same form with its fill mode; false when the paths cannot be combined.
bool combine(std::span<const float> one, uint32_t one_fill, std::span<const float> two, uint32_t two_fill, uint32_t mode, std::vector<float> &result,
	uint32_t &result_fill);
// The area a stroke of a path covers, as a path to fill (Direct2D's Widen): `stroke` holds the width, the start cap, the end cap, the line
// join, the miter limit, the dash offset, the dash count and the dash lengths, every one a float.
bool widen(std::span<const float> path, uint32_t fill, std::span<const float> stroke, std::vector<float> &result, uint32_t &result_fill);

}

}
