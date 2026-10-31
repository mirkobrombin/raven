#include "api.h"
#include "gdi.h"
#include "nt/api.h"
#include "nt/text.h"
#include <sys/mman.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace raven {

namespace {

constexpr uint32_t clr_invalid = 0xffffffffU;

struct Point { int32_t x, y; };

Point read_point(const ApiCall &call, uint64_t address)
{
	return {call.load<int32_t>(address), call.load<int32_t>(address + 4)};
}

DesktopRect read_rect(const ApiCall &call, uint64_t address)
{
	return {call.load<int32_t>(address), call.load<int32_t>(address + 4), call.load<int32_t>(address + 8), call.load<int32_t>(address + 12)};
}

void write_point(const ApiCall &call, uint64_t address, int32_t x, int32_t y)
{
	if (!address)
		return;
	call.store<int32_t>(address, x);
	call.store<int32_t>(address + 4, y);
}

// One decoded device-independent bitmap: top-down BGRA rows with no alpha semantics unless the source carried it.
struct DibImage {
	int32_t width = 0, height = 0;
	std::vector<uint32_t> pixels;
	bool has_alpha = false;
};

// Reads BITMAPINFO plus the pixel data and converts any supported depth to 32-bit pixels (0xAARRGGBB), rows top to bottom.
// Fewer rows than the image height may be supplied; they are the first rows in storage order and the others stay zero.
DibImage decode_dib(const ApiCall &call, uint64_t info, uint64_t bits, uint32_t usage, uint32_t rows_available = 0, int32_t *source_height_out = nullptr)
{
	const auto header_size = call.load<uint32_t>(info);
	if (header_size < 40)
		throw uint32_t(87);
	DibImage image;
	const auto width = call.load<int32_t>(info + 4);
	const auto height = call.load<int32_t>(info + 8);
	const auto bit_count = call.load<uint16_t>(info + 14);
	const auto compression = call.load<uint32_t>(info + 16);
	auto colors_used = call.load<uint32_t>(info + 32);
	if (width <= 0 || !height || width > 16384 || std::abs(int64_t(height)) > 16384)
		throw uint32_t(87);
	if (compression != 0 && compression != 3)
		throw uint32_t(120);
	const bool top_down = height < 0;
	const uint32_t rows = uint32_t(std::abs(int64_t(height)));
	if (source_height_out)
		*source_height_out = int32_t(rows);
	if (!(bit_count == 1 || bit_count == 4 || bit_count == 8 || bit_count == 16 || bit_count == 24 || bit_count == 32))
		throw uint32_t(87);
	uint64_t palette = info + header_size;
	uint32_t masks[3] = {0, 0, 0};
	if (compression == 3) {
		for (unsigned i = 0; i < 3; ++i)
			masks[i] = call.load<uint32_t>(palette + i * 4);
		palette += header_size == 40 ? 12 : 0;
	}
	if (bit_count <= 8 && (!colors_used || colors_used > (1U << bit_count)))
		colors_used = 1U << bit_count;
	std::vector<uint32_t> table(bit_count <= 8 ? 256 : 0, 0);
	if (bit_count <= 8 && usage != 1)
		for (uint32_t i = 0; i < colors_used; ++i)
			table[i] = call.load<uint32_t>(palette + i * 4) & 0xffffff;
	const size_t stride = ((size_t(width) * bit_count + 31) / 32) * 4;
	const uint32_t take = rows_available ? std::min(rows_available, rows) : rows;
	image.width = width;
	image.height = int32_t(rows);
	image.pixels.assign(size_t(width) * rows, 0);
	const auto payload = call.read(bits, stride * take);
	for (uint32_t stored = 0; stored < take; ++stored) {
		const uint32_t row = top_down ? stored : rows - 1 - stored;
		const uint8_t *line = payload.data() + size_t(stored) * stride;
		for (int32_t x = 0; x < width; ++x) {
			uint32_t value = 0;
			switch (bit_count) {
			case 1: value = table[(line[x / 8] >> (7 - x % 8)) & 1]; break;
			case 4: value = table[(line[x / 2] >> (x % 2 ? 0 : 4)) & 15]; break;
			case 8: value = table[line[x]]; break;
			case 16: {
				const uint32_t word = line[x * 2] | (uint32_t(line[x * 2 + 1]) << 8);
				if (compression == 3 && masks[1] == 0x7e0)
					value = (((word >> 11) & 31) * 255 / 31) << 16 | (((word >> 5) & 63) * 255 / 63) << 8 | ((word & 31) * 255 / 31);
				else
					value = (((word >> 10) & 31) * 255 / 31) << 16 | (((word >> 5) & 31) * 255 / 31) << 8 | ((word & 31) * 255 / 31);
				break;
			}
			case 24: value = line[x * 3 + 2] << 16 | line[x * 3 + 1] << 8 | line[x * 3]; break;
			default:
				value = uint32_t(line[x * 4 + 2]) << 16 | uint32_t(line[x * 4 + 1]) << 8 | line[x * 4] | uint32_t(line[x * 4 + 3]) << 24;
				if (line[x * 4 + 3])
					image.has_alpha = true;
			}
			image.pixels[size_t(row) * width + x] = value;
		}
	}
	return image;
}

uint32_t colorref(uint32_t rgb) { return ((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff); }

// COLORREF to 0x00RRGGBB.
uint32_t to_rgb(uint32_t color) { return colorref(color); }

}

void NativeDesktop::register_gdi_api(ApiRegistry &registry)
{
	// Marks the window behind a leased DC as changed so the next message retrieval presents it.
	const auto touched = [this](const DesktopDC &dc) {
		if (dc.window && dc.leased && updated)
			updated(dc.window);
	};
	const auto require_dc = [this](const ApiCall &call, size_t index) -> DesktopDC & {
		auto *dc = objects.dc(call[index]);
		if (!dc)
			throw uint32_t(6);
		return *dc;
	};
	// Runs a drawing handler, translating thrown Windows errors into last-error and a zero return.
	const auto guarded = [](ApiCall &call, uint64_t failure, const std::function<void()> &body) {
		try {
			body();
		} catch (const uint32_t &error) {
			call.fail(error, failure);
		} catch (const std::bad_alloc &) {
			call.fail(8, failure);
		}
	};

	// ---- device context state ----
	registry.add("SaveDC", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			DesktopDCState state;
			state.bitmap = dc.bitmap; state.font = dc.font; state.brush = dc.brush; state.pen = dc.pen;
			state.clip = dc.clip; state.application_clip = dc.application_clip; state.bounds = dc.bounds;
			state.text_color = dc.text_color; state.background_color = dc.background_color; state.background_mode = dc.background_mode;
			state.rop2 = dc.rop2; state.text_align = dc.text_align; state.poly_fill = dc.poly_fill;
			state.origin_x = dc.origin_x; state.origin_y = dc.origin_y; state.position_x = dc.position_x; state.position_y = dc.position_y;
			state.brush_org_x = dc.brush_org_x; state.brush_org_y = dc.brush_org_y; state.window_org_x = dc.window_org_x;
			state.window_org_y = dc.window_org_y; state.viewport_org_x = dc.viewport_org_x; state.viewport_org_y = dc.viewport_org_y;
			state.graphics_mode = dc.graphics_mode; state.world = dc.world;
			dc.saved.push_back(std::move(state));
			call.ret(dc.saved.size());
		});
	});
	registry.add("RestoreDC", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const int32_t requested = call.i32(1);
			const int64_t index = requested < 0 ? int64_t(dc.saved.size()) + requested : int64_t(requested) - 1;
			if (index < 0 || index >= int64_t(dc.saved.size()))
				throw uint32_t(87);
			const auto state = dc.saved[size_t(index)];
			dc.saved.resize(size_t(index));
			// Objects deleted while saved cannot be reselected; the DC keeps what it has.
			if (objects.font(state.font)) objects.select_font(call[0], state.font);
			if (objects.brush(state.brush)) objects.select_brush(call[0], state.brush);
			if (objects.pen(state.pen)) objects.select_pen(call[0], state.pen);
			dc.clip = state.clip; dc.application_clip = state.application_clip; dc.bounds = state.bounds;
			dc.text_color = state.text_color; dc.background_color = state.background_color; dc.background_mode = state.background_mode;
			dc.rop2 = state.rop2; dc.text_align = state.text_align; dc.poly_fill = state.poly_fill;
			dc.origin_x = state.origin_x; dc.origin_y = state.origin_y; dc.position_x = state.position_x; dc.position_y = state.position_y;
			dc.brush_org_x = state.brush_org_x; dc.brush_org_y = state.brush_org_y; dc.window_org_x = state.window_org_x;
			dc.window_org_y = state.window_org_y; dc.viewport_org_x = state.viewport_org_x; dc.viewport_org_y = state.viewport_org_y;
			dc.user_dx = state.viewport_org_x - state.window_org_x; dc.user_dy = state.viewport_org_y - state.window_org_y;
			dc.graphics_mode = state.graphics_mode; dc.world = state.world;
			call.ret(1);
		});
	});
	registry.add("SetGraphicsMode", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const uint32_t mode = call.u32(1);
			if (mode != 1 && mode != 2)
				throw uint32_t(87);
			// Leaving the advanced mode drops a world transform that is not the identity, as Windows refuses to keep one.
			if (mode == 1 && dc.world != std::array<float, 6>{1, 0, 0, 1, 0, 0})
				throw uint32_t(87);
			const auto previous = dc.graphics_mode;
			dc.graphics_mode = mode;
			call.ret(previous);
		});
	});
	registry.add("GetGraphicsMode", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] { call.ret(require_dc(call, 0).graphics_mode); });
	});
	// XFORM: eM11, eM12, eM21, eM22, eDx, eDy.
	const auto read_xform = [](ApiCall &call, uint64_t address) {
		std::array<float, 6> value{};
		const auto bytes = call.read(address, 24);
		std::memcpy(value.data(), bytes.data(), 24);
		return value;
	};
	const auto multiply = [](const std::array<float, 6> &a, const std::array<float, 6> &b) {
		return std::array<float, 6>{a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3], a[2] * b[0] + a[3] * b[2], a[2] * b[1] + a[3] * b[3],
			a[4] * b[0] + a[5] * b[2] + b[4], a[4] * b[1] + a[5] * b[3] + b[5]};
	};
	registry.add("SetWorldTransform", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			if (dc.graphics_mode != 2 || !call[1])
				throw uint32_t(87);
			dc.world = read_xform(call, call[1]);
			call.ret(1);
		});
	});
	registry.add("GetWorldTransform", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			if (!call[1])
				throw uint32_t(87);
			call.write(call[1], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(dc.world.data()), 24));
			call.ret(1);
		});
	});
	// GetTransform(dc, which, XFORM): world to page (0x203) is the world transform; page to device (0x304) moves the window origin onto the
	// viewport origin, the whole MM_TEXT mapping these device contexts keep; world to device (0x204) is their product and device to world
	// (0x402) its inverse. GDI+ reads world to device to place what it draws on a context whose origin a program moved.
	registry.add("GetTransform", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const uint32_t which = call.u32(1);
			if (!call[2] || (which != 0x203 && which != 0x304 && which != 0x204 && which != 0x402))
				throw uint32_t(87);
			const float dx = float(dc.viewport_org_x - dc.window_org_x), dy = float(dc.viewport_org_y - dc.window_org_y);
			const auto &w = dc.world;
			std::array<float, 6> result = which == 0x203 ? w : which == 0x304 ? std::array<float, 6>{1, 0, 0, 1, dx, dy} :
				std::array<float, 6>{w[0], w[1], w[2], w[3], w[4] + dx, w[5] + dy};
			if (which == 0x402) {
				const auto t = result;
				const float determinant = t[0] * t[3] - t[1] * t[2];
				if (determinant == 0)
					throw uint32_t(87);
				result = {t[3] / determinant, -t[1] / determinant, -t[2] / determinant, t[0] / determinant,
					(t[2] * t[5] - t[3] * t[4]) / determinant, (t[1] * t[4] - t[0] * t[5]) / determinant};
			}
			call.write(call[2], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(result.data()), 24));
			call.ret(1);
		});
	});
	registry.add("ModifyWorldTransform", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const uint32_t mode = call.u32(2);
			// Resetting to the identity takes no matrix and changes nothing a compatible-mode context could use; the other modes need
			// GM_ADVANCED.
			if (mode < 1 || mode > 3 || (mode != 1 && (dc.graphics_mode != 2 || !call[1])))
				throw uint32_t(87);
			if (mode == 1)
				dc.world = {1, 0, 0, 1, 0, 0};
			else if (mode == 2)
				dc.world = multiply(read_xform(call, call[1]), dc.world);
			else
				dc.world = multiply(dc.world, read_xform(call, call[1]));
			call.ret(1);
		});
	});
	registry.add("SetTextAlign", 2, [=, this](ApiCall &call) {
		guarded(call, clr_invalid, [&] {
			auto &dc = require_dc(call, 0);
			const auto previous = dc.text_align;
			dc.text_align = call.u32(1);
			call.ret(previous);
		});
	});
	registry.add("GetTextAlign", 1, [=, this](ApiCall &call) {
		guarded(call, clr_invalid, [&] { call.ret(require_dc(call, 0).text_align); });
	});
	registry.add("SetPolyFillMode", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			if (call.u32(1) != 1 && call.u32(1) != 2)
				throw uint32_t(87);
			const auto previous = dc.poly_fill;
			dc.poly_fill = call.u32(1);
			call.ret(previous);
		});
	});
	registry.add("GetPolyFillMode", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] { call.ret(require_dc(call, 0).poly_fill); });
	});
	registry.add("GetROP2", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] { call.ret(require_dc(call, 0).rop2); });
	});
	registry.add("GetMapMode", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			require_dc(call, 0);
			call.ret(1);
		});
	});
	registry.add("SetMapMode", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			require_dc(call, 0);
			// Only the default mapping exists; requesting another one fails visibly.
			if (call.u32(1) != 1)
				throw uint32_t(120);
			call.ret(1);
		});
	});
	registry.add("SetStretchBltMode", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			require_dc(call, 0);
			call.ret(1);
		});
	});
	registry.add("SetBrushOrgEx", 4, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			write_point(call, call[3], dc.brush_org_x, dc.brush_org_y);
			dc.brush_org_x = call.i32(1);
			dc.brush_org_y = call.i32(2);
			call.ret(1);
		});
	});
	registry.add("GetBrushOrgEx", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			write_point(call, call[1], dc.brush_org_x, dc.brush_org_y);
			call.ret(1);
		});
	});
	// Window and viewport origins shift logical coordinates; the DC origin carries the sum and clip regions stay in device units.
	const auto shift_origin = [this](DesktopDC &dc, int32_t window_x, int32_t window_y, int32_t viewport_x, int32_t viewport_y) {
		const int32_t dx = (viewport_x - dc.viewport_org_x) - (window_x - dc.window_org_x);
		const int32_t dy = (viewport_y - dc.viewport_org_y) - (window_y - dc.window_org_y);
		dc.window_org_x = window_x; dc.window_org_y = window_y; dc.viewport_org_x = viewport_x; dc.viewport_org_y = viewport_y;
		dc.origin_x += dx; dc.origin_y += dy; dc.user_dx += dx; dc.user_dy += dy;
	};
	registry.add("SetViewportOrgEx", 4, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			write_point(call, call[3], dc.viewport_org_x, dc.viewport_org_y);
			shift_origin(dc, dc.window_org_x, dc.window_org_y, call.i32(1), call.i32(2));
			call.ret(1);
		});
	});
	registry.add("OffsetViewportOrgEx", 4, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			write_point(call, call[3], dc.viewport_org_x, dc.viewport_org_y);
			shift_origin(dc, dc.window_org_x, dc.window_org_y, dc.viewport_org_x + call.i32(1), dc.viewport_org_y + call.i32(2));
			call.ret(1);
		});
	});
	registry.add("SetWindowOrgEx", 4, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			write_point(call, call[3], dc.window_org_x, dc.window_org_y);
			shift_origin(dc, call.i32(1), call.i32(2), dc.viewport_org_x, dc.viewport_org_y);
			call.ret(1);
		});
	});
	registry.add("OffsetWindowOrgEx", 4, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			write_point(call, call[3], dc.window_org_x, dc.window_org_y);
			shift_origin(dc, dc.window_org_x + call.i32(1), dc.window_org_y + call.i32(2), dc.viewport_org_x, dc.viewport_org_y);
			call.ret(1);
		});
	});
	registry.add("GetViewportOrgEx", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			write_point(call, call[1], dc.viewport_org_x, dc.viewport_org_y);
			call.ret(1);
		});
	});
	registry.add("GetWindowOrgEx", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			write_point(call, call[1], dc.window_org_x, dc.window_org_y);
			call.ret(1);
		});
	});
	// Logical and device coordinates differ only by the window and viewport origins.
	registry.add("DPtoLP", 3, [=, this](ApiCall &call) {
		// An unknown device context fails silently, and a non-positive count has nothing to convert.
		auto *dc = objects.dc(call[0]);
		if (!dc) {
			call.ret(0);
			return;
		}
		const int32_t count = call.i32(2);
		if (count > 65536) {
			call.fail(120, 0);
			return;
		}
		for (int32_t i = 0; i < count; ++i) {
			const auto at = call[1] + uint64_t(i) * 8;
			call.store<int32_t>(at, call.load<int32_t>(at) - dc->user_dx);
			call.store<int32_t>(at + 4, call.load<int32_t>(at + 4) - dc->user_dy);
		}
		call.ret(1);
	});
	registry.add("LPtoDP", 3, [=, this](ApiCall &call) {
		// An unknown device context fails silently, and a non-positive count has nothing to convert.
		auto *dc = objects.dc(call[0]);
		if (!dc) {
			call.ret(0);
			return;
		}
		const int32_t count = call.i32(2);
		if (count > 65536) {
			call.fail(120, 0);
			return;
		}
		for (int32_t i = 0; i < count; ++i) {
			const auto at = call[1] + uint64_t(i) * 8;
			call.store<int32_t>(at, call.load<int32_t>(at) + dc->user_dx);
			call.store<int32_t>(at + 4, call.load<int32_t>(at + 4) + dc->user_dy);
		}
		call.ret(1);
	});
	registry.add("GetNearestColor", 2, [=, this](ApiCall &call) {
		guarded(call, clr_invalid, [&] {
			require_dc(call, 0);
			call.ret(call.u32(1) & 0xffffff);
		});
	});
	registry.add("SelectPalette", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			require_dc(call, 0);
			call.ret(0);
		});
	});
	registry.add("GetPixel", 3, [=, this](ApiCall &call) {
		guarded(call, clr_invalid, [&] {
			auto &dc = require_dc(call, 0);
			Painter painter(objects, dc);
			const int64_t x = int64_t(call.i32(1)) + dc.origin_x, y = int64_t(call.i32(2)) + dc.origin_y;
			call.ret(painter.visible(x, y) ? painter.read(x, y) : clr_invalid);
		});
	});

	// ---- stock objects ----
	registry.add("GetStockObject", 1, [=, this](ApiCall &call) {
		const uint32_t index = call.u32(0);
		if (index < 6) call.ret(desktop_stock_brush + index);
		else if (index <= 8) call.ret(desktop_stock_pen + index - 6);
		else if (index == 18) call.ret(desktop_stock_brush + 6);
		else if (index == 19) call.ret(desktop_stock_pen + 3);
		else if (index == 10 || index == 11 || index == 16) call.ret(desktop_fixed_font);
		else if (index == 12 || index == 17 || index == 14) call.ret(desktop_gui_font);
		else if (index == 13) call.ret(desktop_system_font);
		else if (index == 15) call.ret(0x7fff0001);
		else call.fail(87);
	});
	registry.add("SetDCBrushColor", 2, [=, this](ApiCall &call) {
		guarded(call, clr_invalid, [&] {
			require_dc(call, 0);
			call.ret(objects.set_dc_brush_color(call.u32(1) & 0xffffff));
		});
	});
	registry.add("SetDCPenColor", 2, [=, this](ApiCall &call) {
		guarded(call, clr_invalid, [&] {
			require_dc(call, 0);
			call.ret(objects.set_dc_pen_color(call.u32(1) & 0xffffff));
		});
	});

	// ---- brushes and pens ----
	// LOGBRUSH: style, color, then a hatch index or bitmap handle that is as wide as the program's pointers.
	registry.add("CreateBrushIndirect", 1, [=, this, &registry](ApiCall &call) {
		guarded(call, 0, [&] {
			if (!call[0])
				throw uint32_t(87);
			const auto style = call.load<uint32_t>(call[0]);
			const auto color = call.load<uint32_t>(call[0] + 4) & 0xffffff;
			const uint64_t detail = call.process.mode() == NativeMode::x86 ? call.load<uint32_t>(call[0] + 8) : call.load<uint64_t>(call[0] + 8);
			DesktopBrush brush;
			switch (style) {
			case 0:
				brush.color = color;
				break;
			case 1:
				brush.hollow = true;
				break;
			case 2:
				if (detail > 5)
					throw uint32_t(87);
				brush.color = color;
				brush.style = 2;
				brush.hatch = uint32_t(detail);
				break;
			case 3: {
				// A pattern brush takes the bitmap named by the third member.
				const auto entry = registry.find("CreatePatternBrush");
				if (!entry)
					throw uint32_t(120);
				ApiCall inner(call.process, call.environment, {detail});
				entry->second(inner);
				call.ret(inner.result);
				return;
			}
			default:
				throw uint32_t(120);
			}
			const auto handle = objects.add(brush);
			if (!handle)
				throw uint32_t(8);
			call.ret(handle);
		});
	});
	registry.add("CreateHatchBrush", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			if (call.u32(0) > 5)
				throw uint32_t(87);
			DesktopBrush brush;
			brush.color = call.u32(1) & 0xffffff;
			brush.style = 2;
			brush.hatch = call.u32(0);
			const auto handle = objects.add(brush);
			if (!handle)
				throw uint32_t(8);
			call.ret(handle);
		});
	});
	registry.add("CreatePatternBrush", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			const auto *bitmap = objects.bitmap(call[0]);
			if (!bitmap)
				throw uint32_t(6);
			DesktopBrush brush;
			brush.style = 3;
			brush.pattern_width = bitmap->width;
			brush.pattern_height = bitmap->height;
			brush.pattern_mono = bitmap->depth == 1;
			brush.pattern.assign(size_t(bitmap->width) * bitmap->height, 0);
			const auto pixels = bitmap->pixels();
			for (uint32_t y = 0; y < bitmap->height; ++y)
				for (uint32_t x = 0; x < bitmap->width; ++x) {
					const size_t row = bitmap->top_down ? y : bitmap->height - 1 - y;
					uint32_t value;
					if (bitmap->depth == 1) {
						const size_t stride = ((size_t(bitmap->width) + 15) / 16) * 2;
						value = (pixels[row * stride + x / 8] >> (7 - x % 8)) & 1;
					} else {
						const auto *p = &pixels[(row * bitmap->width + x) * 4];
						value = uint32_t(p[2]) | uint32_t(p[1]) << 8 | uint32_t(p[0]) << 16;
					}
					brush.pattern[size_t(y) * bitmap->width + x] = value;
				}
			const auto handle = objects.add(std::move(brush));
			if (!handle)
				throw uint32_t(8);
			call.ret(handle);
		});
	});
	const auto make_pen = [this](uint32_t style, int32_t width, uint32_t color, bool geometric) {
		DesktopPen pen;
		pen.color = color & 0xffffff;
		pen.style = style;
		pen.hollow = style == 5;
		pen.width = width;
		pen.geometric = geometric;
		const auto handle = objects.add(pen);
		if (!handle)
			throw uint32_t(8);
		return handle;
	};
	registry.add("CreatePen", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			if ((call.u32(0) & 0xff) > 6 || call.i32(1) < 0)
				throw uint32_t(87);
			call.ret(make_pen(call.u32(0) & 0xff, call.i32(1), call.u32(2), false));
		});
	});
	registry.add("CreatePenIndirect", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			const auto style = call.load<uint32_t>(call[0]);
			if ((style & 0xff) > 6)
				throw uint32_t(87);
			call.ret(make_pen(style & 0xff, call.load<int32_t>(call[0] + 4), call.load<uint32_t>(call[0] + 12), false));
		});
	});
	registry.add("ExtCreatePen", 5, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			const auto style = call.u32(0);
			const bool geometric = (style & 0xf0000) == 0x10000;
			if ((style & 0xff) > 7 || !call[2])
				throw uint32_t(87);
			call.ret(make_pen(std::min<uint32_t>(style & 0xff, 6), int32_t(call.u32(1)), call.load<uint32_t>(call[2] + 4), geometric));
		});
	});

	// ---- bitmaps ----
	registry.add("CreateBitmap", 5, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			const int32_t width = call.i32(0), height = call.i32(1);
			const uint32_t planes = call.u32(2), bits_per_pixel = call.u32(3);
			if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || planes != 1 ||
				!(bits_per_pixel == 1 || bits_per_pixel == 4 || bits_per_pixel == 8 || bits_per_pixel == 16 || bits_per_pixel == 24 || bits_per_pixel == 32))
				throw uint32_t(87);
			const auto handle = allocate_bitmap(uint32_t(width), uint32_t(height), bits_per_pixel == 1 ? 1 : 32, false, true);
			if (!handle)
				return;
			auto *bitmap = objects.bitmap(handle);
			if (call[4]) {
				// DDB bits are word-aligned rows, top row first.
				const size_t stride = ((size_t(width) * bits_per_pixel + 15) / 16) * 2;
				const auto source = call.read(call[4], stride * size_t(height));
				auto destination = bitmap->pixels();
				for (int32_t y = 0; y < height; ++y)
					for (int32_t x = 0; x < width; ++x) {
						const uint8_t *line = source.data() + size_t(y) * stride;
						if (bits_per_pixel == 1) {
							const size_t target_stride = ((size_t(width) + 15) / 16) * 2;
							const bool set = (line[x / 8] >> (7 - x % 8)) & 1;
							auto &byte = destination[size_t(y) * target_stride + x / 8];
							byte = uint8_t(set ? (byte | (0x80 >> (x % 8))) : (byte & ~(0x80 >> (x % 8))));
							continue;
						}
						uint8_t blue = 0, green = 0, red = 0;
						switch (bits_per_pixel) {
						case 32: blue = line[x * 4]; green = line[x * 4 + 1]; red = line[x * 4 + 2]; break;
						case 24: blue = line[x * 3]; green = line[x * 3 + 1]; red = line[x * 3 + 2]; break;
						case 16: {
							const uint32_t word = line[x * 2] | (uint32_t(line[x * 2 + 1]) << 8);
							red = uint8_t(((word >> 10) & 31) * 255 / 31); green = uint8_t(((word >> 5) & 31) * 255 / 31); blue = uint8_t((word & 31) * 255 / 31);
							break;
						}
						default: {
							const uint32_t shift = bits_per_pixel == 8 ? 0 : (x % 2 ? 0 : 4);
							const uint32_t index = bits_per_pixel == 8 ? line[x] : (line[x / 2] >> shift) & 15;
							blue = green = red = uint8_t(index * 255 / (bits_per_pixel == 8 ? 255 : 15));
						}
						}
						auto *p = &destination[(size_t(y) * width + x) * 4];
						p[0] = blue; p[1] = green; p[2] = red; p[3] = 0;
					}
			} else if (bitmap->depth == 1) {
				// Uninitialized monochrome bitmaps start black.
			}
			call.ret(handle);
		});
	});
	registry.add("SetBitmapDimensionEx", 4, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto *bitmap = objects.bitmap(call[0]);
			if (!bitmap)
				throw uint32_t(6);
			if (call[3]) {
				call.store<int32_t>(call[3], bitmap->dimension_x);
				call.store<int32_t>(call[3] + 4, bitmap->dimension_y);
			}
			bitmap->dimension_x = call.i32(1);
			bitmap->dimension_y = call.i32(2);
			call.ret(1);
		});
	});
	registry.add("GetBitmapDimensionEx", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			const auto *bitmap = objects.bitmap(call[0]);
			if (!bitmap || !call[1])
				throw uint32_t(bitmap ? 87 : 6);
			call.store<int32_t>(call[1], bitmap->dimension_x);
			call.store<int32_t>(call[1] + 4, bitmap->dimension_y);
			call.ret(1);
		});
	});
	registry.add("SetBitmapBits", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto *bitmap = objects.bitmap(call[0]);
			if (!bitmap)
				throw uint32_t(6);
			auto pixels = bitmap->pixels();
			const size_t size = std::min<size_t>(call.u32(1), pixels.size());
			const auto data = call.read(call[2], size);
			if (bitmap->top_down || bitmap->depth == 1) {
				std::copy(data.begin(), data.end(), pixels.begin());
			} else {
				const size_t stride = size_t(bitmap->width) * 4;
				for (size_t offset = 0; offset < size; ++offset) {
					const size_t row = offset / stride;
					pixels[(bitmap->height - 1 - row) * stride + offset % stride] = data[offset];
				}
			}
			call.ret(size);
		});
	});

	// ---- regions ----
	const auto box_region = [](int32_t left, int32_t top, int32_t right, int32_t bottom) {
		DesktopRegion region;
		if (right > left && bottom > top)
			region.add({left, top, right, bottom});
		return region;
	};
	const auto adopt = [this](DesktopRegion region, ApiCall &call) {
		const auto handle = objects.add(std::move(region));
		if (!handle)
			throw uint32_t(8);
		call.ret(handle);
	};
	registry.add("CreateRectRgnIndirect", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			const auto rect = read_rect(call, call[0]);
			adopt(box_region(std::min(rect.left, rect.right), std::min(rect.top, rect.bottom), std::max(rect.left, rect.right), std::max(rect.top, rect.bottom)), call);
		});
	});
	registry.add("CreateRoundRectRgn", 6, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			const int64_t left = call.i32(0), top = call.i32(1), right = call.i32(2), bottom = call.i32(3);
			if (right <= left || bottom <= top || right - left > 16384 || bottom - top > 16384)
				throw uint32_t(87);
			DesktopRegion region;
			const double rx = std::min<double>(std::abs(call.i32(4)), double(right - left)) / 2.0, ry = std::min<double>(std::abs(call.i32(5)), double(bottom - top)) / 2.0;
			for (int64_t y = top; y < bottom; ++y) {
				const double row = double(y - top) + 0.5;
				double inset = 0;
				const double height = double(bottom - top);
				double dy = row < ry ? ry - row : row > height - ry ? row - (height - ry) : 0;
				if (dy > 0 && ry > 0)
					inset = rx - rx * std::sqrt(std::max(0.0, 1.0 - (dy / ry) * (dy / ry)));
				region.add({int32_t(left + std::floor(inset + 0.5)), int32_t(y), int32_t(right - std::floor(inset + 0.5)), int32_t(y + 1)});
			}
			adopt(std::move(region), call);
		});
	});
	// The pixels closed polygons cover, row by row at pixel centres: ALTERNATE (even_odd) or WINDING filling over the edges of all of
	// them together, each polygon closed back to its first point.
	const auto fill_polygons = [](const std::vector<std::vector<std::array<double, 2>>> &polygons, bool even_odd) {
		double min_y = INFINITY, max_y = -INFINITY;
		for (const auto &polygon : polygons)
			for (const auto &point : polygon) {
				min_y = std::min(min_y, point[1]);
				max_y = std::max(max_y, point[1]);
			}
		DesktopRegion region;
		if (!(min_y < max_y))
			return region;
		if (max_y - min_y > 16384)
			throw uint32_t(120);
		for (int32_t y = int32_t(std::floor(min_y)); y < int32_t(std::ceil(max_y)); ++y) {
			std::vector<std::pair<double, int>> crossings;
			const double center = y + 0.5;
			for (const auto &points : polygons)
				for (size_t i = 0; i < points.size(); ++i) {
					const auto &a = points[i], &b = points[(i + 1) % points.size()];
					if (a[1] == b[1] || center < std::min(a[1], b[1]) || center >= std::max(a[1], b[1]))
						continue;
					const double t = (center - a[1]) / (b[1] - a[1]);
					crossings.push_back({a[0] + t * (b[0] - a[0]), b[1] > a[1] ? 1 : -1});
				}
			std::sort(crossings.begin(), crossings.end());
			int winding = 0;
			for (size_t i = 0; i + 1 < crossings.size(); ++i) {
				winding += crossings[i].second;
				if (!(even_odd ? i % 2 == 0 : winding != 0))
					continue;
				const int32_t from = int32_t(std::ceil(crossings[i].first - 0.5)), to = int32_t(std::ceil(crossings[i + 1].first - 0.5));
				if (to > from)
					region.add({from, y, to, y + 1});
			}
		}
		return region;
	};
	registry.add("CreatePolygonRgn", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			const int32_t count = call.i32(1);
			if (count < 3 || count > 65536)
				throw uint32_t(87);
			std::vector<std::array<double, 2>> points;
			for (int32_t i = 0; i < count; ++i) {
				const auto point = read_point(call, call[0] + uint64_t(i) * 8);
				points.push_back({double(point.x), double(point.y)});
			}
			adopt(fill_polygons({points}, call.u32(2) != 2), call);
		});
	});
	// ---- paths ----
	// BeginPath opens a bracket in which PolyDraw records figures instead of drawing; EndPath closes it, AbortPath drops it.
	registry.add("BeginPath", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			require_dc(call, 0).path = DesktopDC::Path{};
			call.ret(1);
		});
	});
	registry.add("EndPath", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			if (!dc.path || !dc.path->open)
				throw uint32_t(1003);
			dc.path->open = false;
			call.ret(1);
		});
	});
	registry.add("AbortPath", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			require_dc(call, 0).path.reset();
			call.ret(1);
		});
	});
	registry.add("CloseFigure", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			if (!dc.path || !dc.path->open || dc.path->figures.empty())
				throw uint32_t(1003);
			// The next point starts a figure of its own.
			dc.path->figures.push_back({});
			call.ret(1);
		});
	});
	// PolyDraw(dc, points, types, count): PT_MOVETO (6) starts a figure, PT_LINETO (2) and three PT_BEZIERTO (4) points extend it, and
	// PT_CLOSEFIGURE (1) on a point closes it. Inside a path bracket the figures are recorded; outside they are stroked with the pen.
	registry.add("PolyDraw", 4, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const int32_t count = call.i32(3);
			if (count < 0 || count > 65536 || (count && (!call[1] || !call[2])))
				throw uint32_t(87);
			const auto types = call.read(call[2], size_t(count));
			std::vector<std::vector<std::array<double, 2>>> figures;
			std::array<double, 2> current{double(dc.position_x), double(dc.position_y)};
			bool closed = true;
			const auto point_at = [&](int32_t index) {
				const auto point = read_point(call, call[1] + uint64_t(index) * 8);
				return std::array<double, 2>{double(point.x), double(point.y)};
			};
			const auto extend = [&](const std::array<double, 2> &point) {
				if (closed)
					figures.push_back({current});
				closed = false;
				figures.back().push_back(point);
				current = point;
			};
			for (int32_t index = 0; index < count; ++index) {
				const uint8_t type = types[size_t(index)] & ~1;
				if (type == 6) {
					current = point_at(index);
					closed = true;
				} else if (type == 2) {
					extend(point_at(index));
				} else if (type == 4 && index + 2 < count && (types[size_t(index) + 1] & ~1) == 4 && (types[size_t(index) + 2] & ~1) == 4) {
					// A cubic Bezier from the current point, flattened into short lines.
					const auto start = current, first = point_at(index), second = point_at(index + 1), end = point_at(index + 2);
					for (int step = 1; step <= 16; ++step) {
						const double t = step / 16.0, u = 1 - t;
						extend({u * u * u * start[0] + 3 * u * u * t * first[0] + 3 * u * t * t * second[0] + t * t * t * end[0],
							u * u * u * start[1] + 3 * u * u * t * first[1] + 3 * u * t * t * second[1] + t * t * t * end[1]});
					}
					index += 2;
				} else {
					throw uint32_t(87);
				}
				if ((types[size_t(index)] & 1) && !closed && !figures.empty()) {
					current = figures.back().front();
					if (!dc.path || !dc.path->open)
						figures.back().push_back(current);
					closed = true;
				}
			}
			dc.position_x = int32_t(std::lround(current[0]));
			dc.position_y = int32_t(std::lround(current[1]));
			if (dc.path && dc.path->open) {
				if (!dc.path->figures.empty() && dc.path->figures.back().empty())
					dc.path->figures.pop_back();
				for (auto &figure : figures)
					dc.path->figures.push_back(std::move(figure));
				call.ret(1);
				return;
			}
			Painter painter(objects, dc);
			for (const auto &figure : figures) {
				std::vector<std::pair<int32_t, int32_t>> points;
				for (const auto &point : figure)
					points.push_back({int32_t(std::lround(point[0])), int32_t(std::lround(point[1]))});
				painter.polyline(points, selected_pen(objects, dc), false);
			}
			if (painter.changed())
				touched(dc);
			call.ret(1);
		});
	});
	// FillPath, StrokePath and StrokeAndFillPath(dc): the closed path filled with the brush as the polygon fill mode says, its figures
	// outlined, closed, with the pen, or both; the path is used up.
	const auto draw_path = [=, this](ApiCall &call, bool fill, bool stroke) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			if (!dc.path || dc.path->open)
				throw uint32_t(1003);
			const auto figures = std::move(dc.path->figures);
			dc.path.reset();
			Painter painter(objects, dc);
			if (fill) {
				const auto &brush = selected_brush(objects, dc);
				for (const auto &box : fill_polygons(figures, dc.poly_fill != 2).rectangles())
					painter.fill(box.left, box.top, box.right, box.bottom, brush);
			}
			if (stroke)
				for (const auto &figure : figures) {
					std::vector<std::pair<int32_t, int32_t>> points;
					for (const auto &point : figure)
						points.push_back({int32_t(std::lround(point[0])), int32_t(std::lround(point[1]))});
					if (points.size() > 1)
						painter.polyline(points, selected_pen(objects, dc), true);
				}
			if (painter.changed())
				touched(dc);
			call.ret(1);
		});
	};
	registry.add("FillPath", 1, [draw_path](ApiCall &call) { draw_path(call, true, false); });
	registry.add("StrokePath", 1, [draw_path](ApiCall &call) { draw_path(call, false, true); });
	registry.add("StrokeAndFillPath", 1, [draw_path](ApiCall &call) { draw_path(call, true, true); });
	// PathToRegion(dc): the region the closed path covers in device coordinates, filled as the polygon fill mode says; the path is used up.
	registry.add("PathToRegion", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			if (!dc.path || dc.path->open)
				throw uint32_t(1003);
			auto figures = std::move(dc.path->figures);
			dc.path.reset();
			const double dx = double(dc.viewport_org_x) - dc.window_org_x, dy = double(dc.viewport_org_y) - dc.window_org_y;
			for (auto &figure : figures)
				for (auto &point : figure) {
					point[0] += dx;
					point[1] += dy;
				}
			adopt(fill_polygons(figures, dc.poly_fill != 2), call);
		});
	});
	// SelectClipPath(dc, mode): the closed path becomes the clipping region, combined with the clipping the program set as
	// ExtSelectClipRgn combines; the path is used up.
	registry.add("SelectClipPath", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const auto mode = call.u32(1);
			if (mode < 1 || mode > 5)
				throw uint32_t(87);
			if (!dc.path || dc.path->open)
				throw uint32_t(1003);
			auto figures = std::move(dc.path->figures);
			dc.path.reset();
			const double dx = double(dc.viewport_org_x) - dc.window_org_x, dy = double(dc.viewport_org_y) - dc.window_org_y;
			for (auto &figure : figures)
				for (auto &point : figure) {
					point[0] += dx;
					point[1] += dy;
				}
			auto region = fill_polygons(figures, dc.poly_fill != 2);
			std::optional<DesktopRegion> value;
			if (mode == 5 || !dc.application_clip) {
				value = std::move(region);
			} else {
				DesktopRegion next;
				next.combine(*dc.application_clip, region, mode);
				value = std::move(next);
			}
			if (!objects.select_clip(call[0], std::move(value)))
				throw uint32_t(8);
			call.ret(1);
		});
	});
	registry.add("ExtCreateRegion", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			if (call.u32(1) < 32 || !call[2])
				throw uint32_t(87);
			// RGNDATA: RGNDATAHEADER (dwSize, iType, nCount, nRgnSize, rcBound) then RECTs.
			const auto header_size = call.load<uint32_t>(call[2]);
			const auto count = call.load<uint32_t>(call[2] + 8);
			if (header_size != 32 || count > 100000 || call.u32(1) < 32 + count * 16)
				throw uint32_t(87);
			DesktopRegion region;
			for (uint32_t i = 0; i < count; ++i) {
				const auto rect = read_rect(call, call[2] + 32 + uint64_t(i) * 16);
				if (rect.right > rect.left && rect.bottom > rect.top)
					region.add(rect);
			}
			adopt(std::move(region), call);
		});
	});
	registry.add("FillRgn", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const auto *region = objects.region(call[1]);
			const auto *brush = objects.brush(call[2]);
			if (!region || !brush)
				throw uint32_t(6);
			Painter painter(objects, dc);
			const auto bounds = region->bounds();
			for (int32_t y = bounds.top; y < bounds.bottom; ++y)
				for (int32_t x = bounds.left; x < bounds.right; ++x)
					if (region->contains(x, y))
						painter.fill(x, y, x + 1, y + 1, *brush);
			if (painter.changed())
				touched(dc);
			call.ret(1);
		});
	});
	registry.add("FrameRgn", 5, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const auto *region = objects.region(call[1]);
			const auto *brush = objects.brush(call[2]);
			if (!region || !brush)
				throw uint32_t(6);
			Painter painter(objects, dc);
			const int32_t w = std::max(1, call.i32(3)), h = std::max(1, call.i32(4));
			const auto bounds = region->bounds();
			for (int32_t y = bounds.top; y < bounds.bottom; ++y)
				for (int32_t x = bounds.left; x < bounds.right; ++x) {
					if (!region->contains(x, y))
						continue;
					const bool edge = !region->contains(x - w, y) || !region->contains(x + w, y) || !region->contains(x, y - h) || !region->contains(x, y + h);
					if (edge)
						painter.fill(x, y, x + 1, y + 1, *brush);
				}
			if (painter.changed())
				touched(dc);
			call.ret(1);
		});
	});
	registry.add("RectVisible", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const auto rect = read_rect(call, call[1]);
			Painter painter(objects, dc);
			bool any = false;
			for (int32_t y = rect.top; y < rect.bottom && !any; ++y)
				for (int32_t x = rect.left; x < rect.right && !any; ++x)
					any = painter.visible(int64_t(x) + dc.origin_x, int64_t(y) + dc.origin_y);
			call.ret(any);
		});
	});

	// ---- shapes ----
	registry.add("Rectangle", 5, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			Painter painter(objects, dc);
			int64_t left = call.i32(1), top = call.i32(2), right = call.i32(3), bottom = call.i32(4);
			if (right < left) std::swap(left, right);
			if (bottom < top) std::swap(top, bottom);
			const auto &brush = selected_brush(objects, dc);
			const auto &pen = selected_pen(objects, dc);
			if (right > left && bottom > top) {
				if (!brush.hollow)
					painter.fill(left + (pen.hollow ? 0 : 1), top + (pen.hollow ? 0 : 1), right - (pen.hollow ? 0 : 1), bottom - (pen.hollow ? 0 : 1), brush);
				if (!pen.hollow) {
					const std::pair<int32_t, int32_t> corners[4] = {{int32_t(left), int32_t(top)}, {int32_t(right - 1), int32_t(top)},
						{int32_t(right - 1), int32_t(bottom - 1)}, {int32_t(left), int32_t(bottom - 1)}};
					painter.polyline(corners, pen, true);
				}
			}
			if (painter.changed())
				touched(dc);
			call.ret(1);
		});
	});
	registry.add("RoundRect", 7, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			Painter painter(objects, dc);
			painter.ellipse(call.i32(1), call.i32(2), call.i32(3), call.i32(4), std::abs(call.i32(5)), std::abs(call.i32(6)), true);
			if (painter.changed())
				touched(dc);
			call.ret(1);
		});
	});
	registry.add("Ellipse", 5, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			Painter painter(objects, dc);
			painter.ellipse(call.i32(1), call.i32(2), call.i32(3), call.i32(4), 0, 0, false);
			if (painter.changed())
				touched(dc);
			call.ret(1);
		});
	});
	const auto read_points = [](const ApiCall &call, uint64_t address, int32_t count) {
		if (count < 0 || count > 65536)
			throw uint32_t(87);
		std::vector<std::pair<int32_t, int32_t>> points;
		points.reserve(size_t(count));
		const auto bytes = call.read(address, size_t(count) * 8);
		for (int32_t i = 0; i < count; ++i) {
			int32_t x, y;
			std::memcpy(&x, bytes.data() + size_t(i) * 8, 4);
			std::memcpy(&y, bytes.data() + size_t(i) * 8 + 4, 4);
			points.emplace_back(x, y);
		}
		return points;
	};
	registry.add("Polygon", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const auto points = read_points(call, call[1], call.i32(2));
			if (points.size() < 2)
				throw uint32_t(87);
			Painter painter(objects, dc);
			painter.polygon(points);
			if (painter.changed())
				touched(dc);
			call.ret(1);
		});
	});
	registry.add("Polyline", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const auto points = read_points(call, call[1], call.i32(2));
			if (points.size() < 2)
				throw uint32_t(87);
			Painter painter(objects, dc);
			painter.polyline(points, selected_pen(objects, dc), false);
			if (painter.changed())
				touched(dc);
			call.ret(1);
		});
	});
	registry.add("PolylineTo", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			auto points = read_points(call, call[1], call.i32(2));
			if (points.empty())
				throw uint32_t(87);
			points.insert(points.begin(), {dc.position_x, dc.position_y});
			Painter painter(objects, dc);
			painter.polyline(points, selected_pen(objects, dc), false);
			dc.position_x = points.back().first;
			dc.position_y = points.back().second;
			if (painter.changed())
				touched(dc);
			call.ret(1);
		});
	});
	registry.add("PolyPolyline", 4, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const uint32_t polylines = call.u32(3);
			if (polylines > 4096)
				throw uint32_t(87);
			const auto counts = call.read(call[2], size_t(polylines) * 4);
			Painter painter(objects, dc);
			uint64_t cursor = call[1];
			for (uint32_t i = 0; i < polylines; ++i) {
				uint32_t count;
				std::memcpy(&count, counts.data() + size_t(i) * 4, 4);
				const auto points = read_points(call, cursor, int32_t(count));
				painter.polyline(points, selected_pen(objects, dc), false);
				cursor += uint64_t(count) * 8;
			}
			if (painter.changed())
				touched(dc);
			call.ret(1);
		});
	});
	registry.add("LineTo", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			Painter painter(objects, dc);
			Painter::Stroke stroke;
			painter.line(dc.position_x, dc.position_y, call.i32(1), call.i32(2), selected_pen(objects, dc), false, stroke);
			dc.position_x = call.i32(1);
			dc.position_y = call.i32(2);
			if (painter.changed())
				touched(dc);
			call.ret(1);
		});
	});

	// ---- block transfer ----
	const auto blit_to=[=, this](DesktopDC &destination, int32_t dx, int32_t dy, int32_t dw, int32_t dh, DesktopDC *source_dc,
		int32_t sx, int32_t sy, int32_t sw, int32_t sh, uint32_t rop) {
		if (blit(objects, destination, dx, dy, dw, dh, source_dc, sx, sy, sw, sh, rop))
			touched(destination);
	};
	registry.add("BitBlt", 9, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &destination = require_dc(call, 0);
			const uint32_t rop = call.u32(8);
			DesktopDC *source = call[5] ? objects.dc(call[5]) : nullptr;
			if (call[5] && !source)
				throw uint32_t(6);
			blit_to(destination, call.i32(1), call.i32(2), call.i32(3), call.i32(4), source, call.i32(6), call.i32(7), call.i32(3), call.i32(4), rop);
			call.ret(1);
		});
	});
	registry.add("StretchBlt", 11, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &destination = require_dc(call, 0);
			DesktopDC *source = call[5] ? objects.dc(call[5]) : nullptr;
			if (call[5] && !source)
				throw uint32_t(6);
			blit_to(destination, call.i32(1), call.i32(2), call.i32(3), call.i32(4), source, call.i32(6), call.i32(7), call.i32(8), call.i32(9), call.u32(10));
			call.ret(1);
		});
	});
	// msimg32's AlphaBlend is gdi32's GdiAlphaBlend under its older name.
	registry.add("AlphaBlend", 11, [&registry](ApiCall &call) { registry.find("GdiAlphaBlend")->second(call); });
	registry.add("PatBlt", 6, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &destination = require_dc(call, 0);
			blit_to(destination, call.i32(1), call.i32(2), call.i32(3), call.i32(4), nullptr, 0, 0, call.i32(3), call.i32(4), call.u32(5));
			call.ret(1);
		});
	});
	registry.add("FillRect", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const auto rect = read_rect(call, call[1]);
			uint64_t handle = call[2];
			// Small integers are system color indices plus one.
			if (handle && handle <= 31)
				handle = objects.system_brush(int32_t(handle - 1));
			const auto *brush = objects.brush(handle);
			if (!brush)
				throw uint32_t(handle > 0 && handle <= 32 ? 120 : 6);
			Painter painter(objects, dc);
			painter.fill(rect.left, rect.top, rect.right, rect.bottom, *brush);
			if (painter.changed())
				touched(dc);
			call.ret(1);
		});
	});
	// GradientFill(dc, vertices, vertex count, mesh, mesh count, mode): rectangles blend from the first vertex's color to the second's along one axis.
	registry.add("GradientFill", 6, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const uint32_t mode = call.u32(5), vertex_count = call.u32(2), mesh_count = call.u32(4);
			if (mode > 1 || !call[1] || !call[3] || vertex_count > 4096 || mesh_count > 4096)
				throw uint32_t(mode > 2 ? 87 : 120);
			// TRIVERTEX: x, y, then red, green, blue and alpha as 16-bit values.
			const auto vertices = call.read(call[1], size_t(vertex_count) * 16);
			const auto mesh = call.read(call[3], size_t(mesh_count) * 8);
			const auto word = [](const std::vector<uint8_t> &bytes, size_t at, size_t width) {
				uint32_t value = 0;
				std::memcpy(&value, bytes.data() + at, width);
				return value;
			};
			Painter painter(objects, dc);
			for (uint32_t index = 0; index < mesh_count; ++index) {
				const uint32_t first = word(mesh, index * 8, 4), second = word(mesh, index * 8 + 4, 4);
				if (first >= vertex_count || second >= vertex_count)
					throw uint32_t(87);
				const auto at = [&](uint32_t vertex, size_t field, size_t width) { return word(vertices, vertex * 16 + field, width); };
				const int32_t left = std::min<int32_t>(int32_t(at(first, 0, 4)), int32_t(at(second, 0, 4))), right = std::max<int32_t>(int32_t(at(first, 0, 4)), int32_t(at(second, 0, 4)));
				const int32_t top = std::min<int32_t>(int32_t(at(first, 4, 4)), int32_t(at(second, 4, 4))), bottom = std::max<int32_t>(int32_t(at(first, 4, 4)), int32_t(at(second, 4, 4)));
				const int32_t steps = mode == 0 ? right - left : bottom - top;
				for (int32_t step = 0; step < steps; ++step) {
					const auto channel = [&](size_t field) {
						const double from = at(first, field, 2) >> 8, to = at(second, field, 2) >> 8;
						return uint32_t(from + (to - from) * (steps > 1 ? double(step) / (steps - 1) : 0.0) + 0.5);
					};
					DesktopBrush brush;
					brush.color = channel(8) | channel(10) << 8 | channel(12) << 16;
					if (mode == 0)
						painter.fill(left + step, top, left + step + 1, bottom, brush);
					else
						painter.fill(left, top + step, right, top + step + 1, brush);
				}
			}
			if (painter.changed())
				touched(dc);
			call.ret(1);
		});
	});
	registry.add("FrameRect", 3, [=, this](ApiCall &call) {
		auto *dc = objects.dc(call[0]);
		const auto *brush = objects.brush(call[2]);
		if (!dc || !brush) {
			call.ret(0);
			return;
		}
		guarded(call, 0, [&] {
			const auto rect = read_rect(call, call[1]);
			if (rect.right <= rect.left || rect.bottom <= rect.top) {
				call.ret(0);
				return;
			}
			Painter painter(objects, *dc);
			painter.fill(rect.left, rect.top, rect.right, rect.top + 1, *brush);
			painter.fill(rect.left, rect.bottom - 1, rect.right, rect.bottom, *brush);
			painter.fill(rect.left, rect.top + 1, rect.left + 1, rect.bottom - 1, *brush);
			painter.fill(rect.right - 1, rect.top + 1, rect.right, rect.bottom - 1, *brush);
			if (painter.changed())
				touched(*dc);
			call.ret(1);
		});
	});
	registry.add("GdiAlphaBlend", 11, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &destination = require_dc(call, 0);
			auto &source_dc = require_dc(call, 5);
			Painter painter(objects, destination);
			auto *source = objects.bitmap(source_dc.bitmap);
			if (!source || source->depth != 32)
				throw uint32_t(120);
			const uint32_t blend = call.u32(10);
			const uint32_t constant = (blend >> 16) & 0xff, format = (blend >> 24) & 0xff;
			const int32_t dx = call.i32(1), dy = call.i32(2), dw = call.i32(3), dh = call.i32(4);
			const int32_t sx = call.i32(6), sy = call.i32(7), sw = call.i32(8), sh = call.i32(9);
			if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
				throw uint32_t(87);
			const auto pixels = source->pixels();
			for (int32_t y = 0; y < dh; ++y)
				for (int32_t x = 0; x < dw; ++x) {
					const int64_t device_x = int64_t(dx) + x + destination.origin_x, device_y = int64_t(dy) + y + destination.origin_y;
					if (!painter.visible(device_x, device_y))
						continue;
					const int64_t source_x = sx + int64_t(x) * sw / dw, source_y = sy + int64_t(y) * sh / dh;
					if (source_x < 0 || source_y < 0 || source_x >= source->width || source_y >= source->height)
						continue;
					const size_t row = source->top_down ? size_t(source_y) : source->height - 1 - size_t(source_y);
					const auto *p = &pixels[(row * source->width + size_t(source_x)) * 4];
					uint32_t alpha = constant;
					uint32_t blue = p[0], green = p[1], red = p[2];
					if (format & 1) {
						// Premultiplied source: scale by the constant alpha, then composite with the pixel's own alpha.
						const uint32_t pixel_alpha = p[3];
						blue = blue * constant / 255; green = green * constant / 255; red = red * constant / 255;
						alpha = pixel_alpha * constant / 255;
						const uint32_t destination_color = painter.read(device_x, device_y);
						const uint32_t keep = 255 - alpha;
						const uint32_t out_blue = blue + color_blue(destination_color) * keep / 255;
						const uint32_t out_green = green + color_green(destination_color) * keep / 255;
						const uint32_t out_red = red + color_red(destination_color) * keep / 255;
						painter.plot(device_x, device_y, std::min(out_red, 255u) | std::min(out_green, 255u) << 8 | std::min(out_blue, 255u) << 16, 13);
						continue;
					}
					const uint32_t destination_color = painter.read(device_x, device_y);
					const uint32_t out_blue = (blue * alpha + color_blue(destination_color) * (255 - alpha)) / 255;
					const uint32_t out_green = (green * alpha + color_green(destination_color) * (255 - alpha)) / 255;
					const uint32_t out_red = (red * alpha + color_red(destination_color) * (255 - alpha)) / 255;
					painter.plot(device_x, device_y, out_red | out_green << 8 | out_blue << 16, 13);
				}
			if (painter.changed())
				touched(destination);
			call.ret(1);
		});
	});

	// ---- device-independent bitmaps ----
	registry.add("StretchDIBits", 13, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const int32_t dx = call.i32(1), dy = call.i32(2), dw = call.i32(3), dh = call.i32(4);
			const int32_t sx = call.i32(5), sy = call.i32(6), sw = call.i32(7), sh = call.i32(8);
			const auto image = decode_dib(call, call[10], call[9], call.u32(11));
			Painter painter(objects, dc);
			if (!dw || !dh || !sw || !sh) {
				call.ret(0);
				return;
			}
			const int64_t abs_dw = std::abs(int64_t(dw)), abs_dh = std::abs(int64_t(dh)), abs_sw = std::abs(int64_t(sw)), abs_sh = std::abs(int64_t(sh));
			const bool flip_x = (dw < 0) != (sw < 0), flip_y = (dh < 0) != (sh < 0);
			const int64_t dest_left = dw < 0 ? int64_t(dx) + dw + 1 : dx, dest_top = dh < 0 ? int64_t(dy) + dh + 1 : dy;
			// DIB source rows count from the bottom: the origin is the lower-left corner for bottom-up images.
			const bool top_down = call.load<int32_t>(call[10] + 8) < 0;
			for (int64_t row = 0; row < abs_dh; ++row)
				for (int64_t column = 0; column < abs_dw; ++column) {
					const int64_t device_x = dest_left + column + dc.origin_x, device_y = dest_top + row + dc.origin_y;
					if (!painter.visible(device_x, device_y))
						continue;
					const int64_t scaled_x = abs_dw == abs_sw ? column : column * abs_sw / abs_dw;
					const int64_t scaled_y = abs_dh == abs_sh ? row : row * abs_sh / abs_dh;
					const int64_t image_x = sx + (flip_x ? abs_sw - 1 - scaled_x : scaled_x);
					int64_t image_row = flip_y ? abs_sh - 1 - scaled_y : scaled_y;
					// Rows of a top-down DIB use sy as the first line; rows of a bottom-up DIB count up from the bottom.
					const int64_t image_y = top_down ? sy + image_row : image.height - 1 - (sy + (abs_sh - 1 - image_row));
					if (image_x < 0 || image_x >= image.width || image_y < 0 || image_y >= image.height)
						continue;
					const uint32_t source_pixel = image.pixels[size_t(image_y) * image.width + size_t(image_x)];
					painter.plot(device_x, device_y, colorref(source_pixel & 0xffffff), 13);
					// A DIB section keeps the alpha channel of what is copied into it.
					if (painter.bitmap.dib_section && painter.bitmap.depth == 32 && image.has_alpha)
						if (auto *target = painter.pixel_pointer(device_x, device_y))
							target[3] = uint8_t(source_pixel >> 24);
				}
			if (painter.changed())
				touched(dc);
			call.ret(uint64_t(abs_dh));
		});
	});
	// CreateDIBitmap: a device bitmap of the header's size, filled from the bits when CBM_INIT asks for it.
	registry.add("CreateDIBitmap", 6, [&registry, this](ApiCall &call) {
		if (!call[1]) {
			call.fail(87);
			return;
		}
		const int32_t width = call.load<int32_t>(call[1] + 4), height = call.load<int32_t>(call[1] + 8);
		if (width <= 0 || !height) {
			call.fail(87);
			return;
		}
		const uint32_t rows = uint32_t(height < 0 ? -height : height);
		const uint64_t bitmap = allocate_bitmap(uint32_t(width), rows, 32, false, true);
		if (!bitmap) {
			call.fail(8);
			return;
		}
		if ((call.u32(2) & 4) && call[3] && call[4]) {
			ApiCall fill(call.process, call.environment, {call[0], bitmap, 0, rows, call[3], call[4], call[5]});
			registry.find("SetDIBits")->second(fill);
		}
		call.ret(bitmap);
	});
	registry.add("SetDIBits", 7, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto *bitmap = objects.bitmap(call[1]);
			if (!bitmap || bitmap->depth != 32)
				throw uint32_t(6);
			const uint32_t start = call.u32(2), lines = call.u32(3);
			int32_t rows = 0;
			const auto image = decode_dib(call, call[5], call[4], call.u32(6), lines, &rows);
			auto pixels = bitmap->pixels();
			const bool top_down = call.load<int32_t>(call[5] + 8) < 0;
			uint32_t done = 0;
			for (uint32_t row = 0; row < lines && row + start < uint32_t(rows) && row + start < bitmap->height; ++row, ++done) {
				// Storage line start+row of the DIB lands on the bitmap row with the same top-to-bottom position.
				const uint32_t image_row = top_down ? start + row : uint32_t(rows) - 1 - (start + row);
				const size_t memory_row = bitmap->top_down ? image_row : size_t(bitmap->height) - 1 - image_row;
				if (image_row >= uint32_t(image.height) || image_row >= bitmap->height)
					continue;
				for (int32_t x = 0; x < std::min<int32_t>(image.width, int32_t(bitmap->width)); ++x) {
					auto *p = &pixels[(memory_row * bitmap->width + size_t(x)) * 4];
					const uint32_t value = image.pixels[size_t(image_row) * image.width + size_t(x)];
					p[0] = uint8_t(value); p[1] = uint8_t(value >> 8); p[2] = uint8_t(value >> 16); p[3] = uint8_t(value >> 24);
				}
			}
			call.ret(done);
		});
	});
	registry.add("GetDIBits", 7, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto *bitmap = objects.bitmap(call[1]);
			if (!bitmap)
				throw uint32_t(6);
			const auto header_size = call.load<uint32_t>(call[5]);
			if (header_size < 40)
				throw uint32_t(87);
			auto bit_count = call.load<uint16_t>(call[5] + 14);
			if (!bit_count) {
				// A query fills in the format: width, height, planes and bit count of the selected bitmap.
				call.store<int32_t>(call[5] + 4, int32_t(bitmap->width));
				call.store<int32_t>(call[5] + 8, int32_t(bitmap->height));
				call.store<uint16_t>(call[5] + 12, 1);
				// A DIB section answers with the format its creator asked for.
				const uint32_t format = bitmap->depth == 1 || bitmap->dib_section ? bitmap->depth : 32;
				call.store<uint16_t>(call[5] + 14, uint16_t(format));
				call.store<uint32_t>(call[5] + 16, 0);
				call.store<uint32_t>(call[5] + 20, uint32_t(((size_t(bitmap->width) * format + 31) / 32) * 4 * bitmap->height));
				call.ret(1);
				return;
			}
			if (!call[4]) {
				call.ret(1);
				return;
			}
			// An indexed request of the bitmap's own depth is answered with its own indices and colours.
			const bool same_indexed = bit_count <= 8 && bit_count == bitmap->depth && !bitmap->palette.empty();
			if (bit_count != 32 && bit_count != 24 && bit_count != 16 && bit_count != 8 && bit_count != 1 && !same_indexed)
				throw uint32_t(120);
			const uint32_t start = call.u32(2), lines = call.u32(3);
			const int32_t height_field = call.load<int32_t>(call[5] + 8);
			const bool top_down = height_field < 0;
			const size_t stride = ((size_t(bitmap->width) * bit_count + 31) / 32) * 4;
			if (start > bitmap->height)
				throw uint32_t(87);
			const uint32_t take = std::min<uint32_t>(lines, bitmap->height - start);
			std::vector<uint8_t> output(stride * take, 0);
			const auto pixels = bitmap->pixels();
			for (uint32_t row = 0; row < take; ++row) {
				const uint32_t image_row = start + row;
				const size_t memory_row = bitmap->top_down ? image_row : bitmap->height - 1 - image_row;
				// A bottom-up DIB stores the image's last row first.
				const size_t out_row = top_down ? row : take - 1 - row;
				const uint8_t *line = pixels.data() + memory_row * bitmap_row_bytes(*bitmap);
				if (same_indexed) {
					std::copy_n(line, (size_t(bitmap->width) * bit_count + 7) / 8, &output[out_row * stride]);
					continue;
				}
				for (uint32_t x = 0; x < bitmap->width; ++x) {
					const uint32_t color = bitmap_color(*bitmap, line, x), alpha = bitmap->depth == 32 ? line[x * 4 + 3] : 0;
					auto *target = &output[out_row * stride];
					if (bit_count == 32) {
						// A bitmap that carries an alpha channel hands it back, which image lists use to tell ARGB images from plain ones.
						target[x * 4] = uint8_t(color >> 16); target[x * 4 + 1] = uint8_t(color >> 8); target[x * 4 + 2] = uint8_t(color); target[x * 4 + 3] = uint8_t(alpha);
					} else if (bit_count == 24) {
						target[x * 3] = uint8_t(color >> 16); target[x * 3 + 1] = uint8_t(color >> 8); target[x * 3 + 2] = uint8_t(color);
					} else if (bit_count == 16) {
						const uint32_t word = (color_red(color) >> 3) << 10 | (color_green(color) >> 3) << 5 | color_blue(color) >> 3;
						target[x * 2] = uint8_t(word); target[x * 2 + 1] = uint8_t(word >> 8);
					} else if (bit_count == 8) {
						target[x] = uint8_t((color_red(color) * 30 + color_green(color) * 59 + color_blue(color) * 11) / 100);
					} else {
						target[x / 8] = uint8_t(target[x / 8] | ((color_red(color) + color_green(color) + color_blue(color) >= 384) ? (0x80 >> (x % 8)) : 0));
					}
				}
			}
			call.write(call[4], output);
			// The colour table of an indexed result is part of the answer: black and white, or the grey ramp the indices stand for.
			if (bit_count <= 8) {
				const uint32_t entries = 1u << bit_count;
				std::vector<uint8_t> table(entries * 4);
				for (uint32_t entry = 0; entry < entries; ++entry) {
					if (same_indexed) {
						const uint32_t color = entry < bitmap->palette.size() ? bitmap->palette[entry] : 0;
						table[entry * 4] = uint8_t(color_blue(color)); table[entry * 4 + 1] = uint8_t(color_green(color)); table[entry * 4 + 2] = uint8_t(color_red(color));
						continue;
					}
					const uint8_t level = bit_count == 1 ? (entry ? 255 : 0) : uint8_t(entry);
					table[entry * 4] = table[entry * 4 + 1] = table[entry * 4 + 2] = level;
				}
				call.write(call[5] + header_size, table);
			}
			call.ret(take);
		});
	});
	registry.add("GetDIBColorTable", 4, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			require_dc(call, 0);
			call.ret(0);
		});
	});
	registry.add("SetDIBColorTable", 4, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			require_dc(call, 0);
			call.ret(0);
		});
	});
	registry.add("PlayEnhMetaFile", 3, [=, this](ApiCall &call) {
		call.fail(120, 0);
	});

	// ---- printing is not provided ----
	for (const auto *name : {"StartDocW", "StartDocA", "EndDoc", "StartPage", "EndPage", "AbortDoc"})
		registry.add(name, 2, [](ApiCall &call) { call.fail(120, 0); });
	registry.add("GdiGetCodePage", 1, [](ApiCall &call) { call.ret(1252); });
}


void NativeDesktop::register_image_api(ApiRegistry &registry)
{
	const auto guarded = [](ApiCall &call, uint64_t failure, const std::function<void()> &body) {
		try {
			body();
		} catch (const uint32_t &error) {
			call.fail(error, failure);
		} catch (const std::bad_alloc &) {
			call.fail(8, failure);
		}
	};
	// A persistent guest page for names and small buffers that must be passed to resource lookups.
	const auto page = [this]() {
		if (!scratch)
			scratch = process.map_memory(8192, PROT_READ | PROT_WRITE);
		return scratch + 4096;
	};
	// Resource names are integers or strings; an ANSI string is widened into the scratch page first.
	const auto wide_name = [=, this](ApiCall &call, uint64_t name, bool ansi) -> uint64_t {
		if (name < 0x10000 || !ansi)
			return name;
		const auto text = from_ansi(call.narrow(name, 512), 1252);
		std::vector<uint8_t> bytes((text.size() + 1) * 2, 0);
		std::memcpy(bytes.data(), text.data(), text.size() * 2);
		call.write(page(), bytes);
		return page();
	};
	const auto make_bitmap = [this](const ApiCall &call, uint64_t info, uint64_t bits, bool keep_alpha) -> uint64_t {
		const auto image = decode_dib(call, info, bits, 0);
		const auto handle = allocate_bitmap(uint32_t(image.width), uint32_t(image.height), 32, false, true);
		if (!handle)
			throw uint32_t(8);
		auto *bitmap = objects.bitmap(handle);
		auto pixels = bitmap->pixels();
		for (size_t i = 0; i < image.pixels.size(); ++i) {
			const uint32_t value = image.pixels[i];
			pixels[i * 4] = uint8_t(value); pixels[i * 4 + 1] = uint8_t(value >> 8); pixels[i * 4 + 2] = uint8_t(value >> 16);
			pixels[i * 4 + 3] = keep_alpha && image.has_alpha ? uint8_t(value >> 24) : 0;
		}
		return handle;
	};
	// Locates the pixel data of a packed DIB (header, optional masks, palette, then bits).
	const auto dib_bits = [](const ApiCall &call, uint64_t info) -> uint64_t {
		const auto header = call.load<uint32_t>(info);
		const auto bit_count = call.load<uint16_t>(info + 14);
		const auto compression = call.load<uint32_t>(info + 16);
		auto colors = call.load<uint32_t>(info + 32);
		if (bit_count <= 8 && (!colors || colors > (1U << bit_count)))
			colors = 1U << bit_count;
		return info + header + (bit_count <= 8 ? uint64_t(colors) * 4 : 0) + (compression == 3 && header == 40 ? 12 : 0);
	};
	const auto bitmap_resource = [=, this](ApiCall &call, uint64_t instance, uint64_t name, bool keep_alpha) -> uint64_t {
		if (!instance && name >= 32700 && name < 32800)
			instance = oem_module;
		const auto view = resources.view(instance, 2, name);
		if (!view)
			throw uint32_t(1814);
		return make_bitmap(call, view->address, dib_bits(call, view->address), keep_alpha);
	};
	const auto host_file = [=, this](const ApiCall &call, uint64_t name, bool ansi) -> std::vector<uint8_t> {
		const auto path = ansi ? from_ansi(call.narrow(name, 1024), 1252) : call.wide(name, 1024);
		if (!read_host_file)
			throw uint32_t(2);
		const auto bytes = read_host_file(path);
		if (!bytes)
			throw uint32_t(2);
		return *bytes;
	};
	const auto icon_resource = [=, this](ApiCall &call, uint64_t instance, uint64_t name, uint32_t cx, uint32_t cy) -> uint64_t {
		if (!scratch)
			scratch = process.map_memory(8192, PROT_READ | PROT_WRITE);
		const uint64_t forwarded[] = {instance, name, cx, cy, scratch};
		const auto status = load_icon(forwarded);
		if (status) {
			throw uint32_t((status & 0xffff0000U) == 0x80070000U ? status & 0xffffU : 1814);
		}
		return call.load<uint64_t>(scratch);
	};
	// ICO file: directory of images, each a DIB (without file header) or PNG.
	const auto icon_file = [=, this](const std::vector<uint8_t> &bytes, uint32_t cx, uint32_t cy) -> uint64_t {
		if (bytes.size() < 22 || bytes[2] != 1)
			throw uint32_t(13);
		const unsigned count = bytes[4] | bytes[5] << 8;
		size_t best = 0;
		uint32_t best_distance = UINT32_MAX;
		for (unsigned i = 0; i < count && 6 + (i + 1) * 16 <= bytes.size(); ++i) {
			const uint32_t width = bytes[6 + i * 16] ? bytes[6 + i * 16] : 256, height = bytes[7 + i * 16] ? bytes[7 + i * 16] : 256;
			const uint32_t distance = cx ? uint32_t(std::abs(int(width) - int(cx)) + std::abs(int(height) - int(cy))) : 1000 - width;
			if (distance < best_distance) {
				best_distance = distance;
				best = i;
			}
		}
		const uint32_t size = bytes[6 + best * 16 + 8] | bytes[6 + best * 16 + 9] << 8 | bytes[6 + best * 16 + 10] << 16 | uint32_t(bytes[6 + best * 16 + 11]) << 24;
		const uint32_t offset = bytes[6 + best * 16 + 12] | bytes[6 + best * 16 + 13] << 8 | bytes[6 + best * 16 + 14] << 16 | uint32_t(bytes[6 + best * 16 + 15]) << 24;
		if (offset > bytes.size() || size > bytes.size() - offset)
			throw uint32_t(13);
		const auto handle = create_icon(std::span<const uint8_t>(bytes.data() + offset, size), cx, cy, 0);
		if (!handle)
			throw uint32_t(environment.last_error());
		return handle;
	};
	const auto load_image = [=, this](ApiCall &call, bool ansi) {
		guarded(call, 0, [&] {
			const uint32_t type = call.u32(2), flags = call.u32(5);
			uint32_t cx = call.u32(3), cy = call.u32(4);
			if (flags & 0x40)
				cx = cy = 0;
			if (flags & 0x10) {
				const auto bytes = host_file(call, call[1], ansi);
				if (type == 0) {
					if (bytes.size() < 54 || bytes[0] != 'B')
						throw uint32_t(13);
					const auto buffer = process.map_memory((bytes.size() + 4095) & ~size_t(4095), PROT_READ | PROT_WRITE);
					uint64_t handle = 0;
					try {
						process.write_memory(buffer, bytes);
						handle = make_bitmap(call, buffer + 14, buffer + 14 + (dib_bits(call, buffer + 14) - (buffer + 14)), (flags & 0x2000) != 0);
					} catch (...) {
						process.unmap_memory(buffer);
						throw;
					}
					process.unmap_memory(buffer);
					call.ret(handle);
				} else if (type == 1) {
					call.ret(icon_file(bytes, cx, cy));
				} else {
					call.ret(desktop_stock_cursor);
				}
				return;
			}
			const auto name = wide_name(call, call[1], ansi);
			if (type == 0)
				call.ret(bitmap_resource(call, call[0], name, (flags & 0x2000) != 0));
			else if (type == 1)
				call.ret(icon_resource(call, call[0], name, cx, cy));
			else if (call[0] == 0 && name >= 32512 && name < 32512 + desktop_stock_cursor_count)
				call.ret(desktop_stock_cursor + (name - 32512));
			else
				// Module cursors are not rendered by the host; callers receive the arrow.
				call.ret(desktop_stock_cursor);
		});
	};
	registry.add("LoadImageW", 6, [=](ApiCall &call) { load_image(call, false); });
	registry.add("LoadImageA", 6, [=](ApiCall &call) { load_image(call, true); });
	const auto load_bitmap = [=, this](ApiCall &call, bool ansi) {
		guarded(call, 0, [&] { call.ret(bitmap_resource(call, call[0], wide_name(call, call[1], ansi), false)); });
	};
	registry.add("LoadBitmapW", 2, [=](ApiCall &call) { load_bitmap(call, false); });
	registry.add("LoadBitmapA", 2, [=](ApiCall &call) { load_bitmap(call, true); });
	registry.add("LoadIconA", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			if (!call[0])
				throw uint32_t(120);
			call.ret(icon_resource(call, call[0], wide_name(call, call[1], true), 0, 0));
		});
	});
	registry.add("LoadCursorA", 2, [=, this](ApiCall &call) {
		if (!call[0] && call[1] >= 32512 && call[1] < 32512 + desktop_stock_cursor_count)
			call.ret(desktop_stock_cursor + (call[1] - 32512));
		else
			call.ret(desktop_stock_cursor);
	});
	registry.add("LoadStringA", 4, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			const int32_t capacity = call.i32(3);
			if (capacity <= 0 || capacity > 2040) {
				call.ret(0);
				return;
			}
			const uint64_t forwarded[] = {call[0], call[1], page(), uint64_t(capacity)};
			const auto length = load_string(forwarded);
			if (!length) {
				call.ret(0);
				return;
			}
			const auto text = call.wide(page(), size_t(capacity));
			auto narrow = to_ansi(text, 1252);
			call.put_narrow(call[2], narrow, size_t(capacity));
			call.ret(std::min<size_t>(narrow.size(), size_t(capacity) - 1));
		});
	});
	registry.add("LoadMenuA", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			if (call[1] >= 0x10000)
				throw uint32_t(120);
			call.ret(menu_resource(call[0], call[1]));
		});
	});
	registry.add("CopyImage", 5, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			const uint32_t type = call.u32(1);
			if (type == 0) {
				const auto *source = objects.bitmap(call[0]);
				if (!source)
					throw uint32_t(1409);
				const uint32_t width = call.u32(2) ? call.u32(2) : source->width, height = call.u32(3) ? call.u32(3) : source->height;
				const auto handle = allocate_bitmap(width, height, source->depth, false, true);
				if (!handle)
					throw uint32_t(8);
				auto *copy = objects.bitmap(handle);
				if (source->depth == 32) {
					const auto from = source->pixels();
					auto to = copy->pixels();
					for (uint32_t y = 0; y < height; ++y)
						for (uint32_t x = 0; x < width; ++x) {
							const size_t sy = source->top_down ? size_t(y) * source->height / height : source->height - 1 - size_t(y) * source->height / height;
							const size_t sx = size_t(x) * source->width / width;
							std::memcpy(&to[(size_t(y) * width + x) * 4], &from[(sy * source->width + sx) * 4], 4);
						}
				} else if (width == source->width && height == source->height) {
					std::copy(source->pixels().begin(), source->pixels().end(), copy->pixels().begin());
				}
				call.ret(handle);
			} else {
				const auto *icon = objects.icon(call[0]);
				if (!icon && !(call[0] >= desktop_stock_cursor && call[0] < desktop_stock_cursor + desktop_stock_cursor_count))
					throw uint32_t(1402);
				if (!icon) {
					call.ret(call[0]);
					return;
				}
				const auto handle = objects.add(*icon);
				if (!handle)
					throw uint32_t(8);
				call.ret(handle);
			}
		});
	});
	// CreateIcon(instance, width, height, planes, bits per pixel, AND bits, XOR bits): a monochrome mask whose rows are padded to words,
	// and the color image in the same top-down rows, at 32 bits per pixel or as a second monochrome plane.
	registry.add("CreateIcon", 7, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			const uint32_t width = call.u32(1), height = call.u32(2), depth = uint32_t(uint8_t(call.u32(3))) * uint32_t(uint8_t(call.u32(4)));
			if (!width || !height || width > 1024 || height > 1024 || !call[5] || !call[6] || (depth != 1 && depth != 32))
				throw uint32_t(87);
			const size_t mask_stride = ((size_t(width) + 15) / 16) * 2;
			const auto and_bits = call.read(call[5], mask_stride * height);
			const auto xor_bits = call.read(call[6], depth == 32 ? size_t(width) * height * 4 : mask_stride * height);
			DesktopIcon icon{};
			icon.monochrome = depth == 1;
			icon.shared = false;
			auto &image = icon.image;
			image.color.width = width;
			image.color.height = height;
			image.color.rgba.assign(size_t(width) * height * 4, 0);
			image.and_mask.assign(size_t(width) * height, 0);
			bool any_alpha = false;
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x) {
					const bool transparent = (and_bits[y * mask_stride + x / 8] >> (7 - x % 8)) & 1;
					const size_t at = (size_t(y) * width + x) * 4;
					if (depth == 32) {
						const auto *p = &xor_bits[at];
						image.color.rgba[at] = p[2];
						image.color.rgba[at + 1] = p[1];
						image.color.rgba[at + 2] = p[0];
						image.color.rgba[at + 3] = p[3];
						any_alpha = any_alpha || p[3];
					} else {
						const uint8_t level = ((xor_bits[y * mask_stride + x / 8] >> (7 - x % 8)) & 1) ? 255 : 0;
						image.color.rgba[at] = image.color.rgba[at + 1] = image.color.rgba[at + 2] = level;
					}
					image.and_mask[size_t(y) * width + x] = transparent;
				}
			image.has_alpha = any_alpha;
			if (!any_alpha)
				for (size_t i = 0; i < image.and_mask.size(); ++i)
					image.color.rgba[i * 4 + 3] = image.and_mask[i] ? 0 : 255;
			const auto handle = objects.add(std::move(icon));
			if (!handle)
				throw uint32_t(8);
			call.ret(handle);
		});
	});
	registry.add("CreateIconIndirect", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			// ICONINFO: fIcon, the cursor hotspot, then the mask and color bitmaps at the program's pointer width.
			const auto is_icon = call.load<uint32_t>(call[0]);
			const size_t width = call.pointer_width();
			const auto mask_handle = call.load_pointer(call[0] + (width == 8 ? 16 : 12)), color_handle = call.load_pointer(call[0] + (width == 8 ? 24 : 16));
			const auto *mask = objects.bitmap(mask_handle);
			const auto *color = objects.bitmap(color_handle);
			if (!mask || !color || color->depth != 32)
				throw uint32_t(120);
			DesktopIcon icon{};
			icon.monochrome = false;
			icon.shared = false;
			icon.cursor = !is_icon;
			icon.hotspot_x = int32_t(call.load<uint32_t>(call[0] + 4));
			icon.hotspot_y = int32_t(call.load<uint32_t>(call[0] + 8));
			auto &image = icon.image;
			image.color.width = color->width;
			image.color.height = color->height;
			image.color.rgba.assign(size_t(color->width) * color->height * 4, 0);
			image.and_mask.assign(size_t(color->width) * color->height, 0);
			image.xor_mask.clear();
			const auto from = color->pixels();
			const auto bits = mask->pixels();
			const size_t stride = ((size_t(mask->width) + 15) / 16) * 2;
			bool any_alpha = false;
			for (uint32_t y = 0; y < color->height; ++y)
				for (uint32_t x = 0; x < color->width; ++x) {
					const size_t row = color->top_down ? y : color->height - 1 - y;
					const auto *p = &from[(row * color->width + x) * 4];
					const size_t mrow = mask->top_down ? y : mask->height - 1 - y;
					const bool transparent = x < mask->width && y < mask->height && ((bits[mrow * stride + x / 8] >> (7 - x % 8)) & 1);
					const size_t at = (size_t(y) * color->width + x) * 4;
					image.color.rgba[at] = p[2]; image.color.rgba[at + 1] = p[1]; image.color.rgba[at + 2] = p[0];
					image.color.rgba[at + 3] = p[3];
					any_alpha = any_alpha || p[3];
					image.and_mask[size_t(y) * color->width + x] = transparent;
				}
			image.has_alpha = any_alpha;
			if (!any_alpha)
				for (size_t i = 0; i < image.and_mask.size(); ++i)
					image.color.rgba[i * 4 + 3] = image.and_mask[i] ? 0 : 255;
			const auto handle = objects.add(std::move(icon));
			if (!handle)
				throw uint32_t(8);
			call.ret(handle);
		});
	});
}
}
