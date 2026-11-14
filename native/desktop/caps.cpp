#include "api.h"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace raven {

namespace {

uint64_t le(std::span<const uint8_t> bytes, size_t at, size_t width)
{
	uint64_t value = 0;
	for (size_t byte = 0; byte < width; ++byte)
		value |= uint64_t(bytes[at + byte]) << (byte * 8);
	return value;
}

}

uint32_t device_caps(const DesktopObjects &objects, const DesktopDC &dc, int32_t index)
{
	if (!objects.bitmap(dc.bitmap))
		throw uint32_t(6);
	const auto &screen = objects.nonclient_metrics();
	// Every context answers for the device it is compatible with, the display: a memory DC reports the screen, not the bitmap
	// selected into it, as Windows memory DCs do.
	switch (index) {
	case 2: return 1;                                             // TECHNOLOGY: DT_RASDISPLAY
	case 4: return uint32_t(int64_t(screen.screen_x) * 254 / 960);  // HORZSIZE in mm at 96 dpi
	case 6: return uint32_t(int64_t(screen.screen_y) * 254 / 960);  // VERTSIZE
	case 8: return uint32_t(screen.screen_x);                     // HORZRES
	case 10: return uint32_t(screen.screen_y);                    // VERTRES
	case 12: return 32;                                           // BITSPIXEL
	case 14: return 1;                                            // PLANES
	case 24: return uint32_t(-1);                                 // NUMCOLORS: more than 8 bits per pixel
	case 28: return 0xff;                                         // CURVECAPS: every curve
	case 30: return 0xfe;                                         // LINECAPS: every line
	case 32: return 0xff;                                         // POLYGONALCAPS: every polygon
	case 34: return 0x7807;                                       // TEXTCAPS: OP_CHARACTER | OP_STROKE | CP_STROKE | UA | SO | RA | VA
	case 36: return 1;                                            // CLIPCAPS: CP_RECTANGLE
	// RC_BITBLT | RC_BITMAP64 | RC_GDI20_OUTPUT | RC_DI_BITMAP | RC_DIBTODEV | RC_BIGFONT | RC_STRETCHBLT | RC_FLOODFILL | RC_STRETCHDIB
	// | RC_DEVBITS, what a true-colour display reports.
	case 38: return 0x7e99;
	case 40: case 42: return 36;                                   // ASPECTX / ASPECTY
	case 44: return 51;                                           // ASPECTXY
	case 88: case 90: return 96;                                  // LOGPIXELSX / LOGPIXELSY
	case 104: return 0;                                           // SIZEPALETTE: no palette device
	case 106: return 0;                                           // NUMRESERVED
	case 108: return 24;                                          // COLORRES
	default: return 0;
	}
}

bool NativeDesktop::caps_dispatch(std::string_view name)
{
	const bool caps = name == "GetDeviceCaps", rop = name == "SetROP2", layout = name == "SetLayout";
	const bool pixel = name == "SetPixel", bits = name == "SetDIBitsToDevice";
	if (!caps && !rop && !layout && !pixel && !bits)
		return false;
	const auto args = process.api_arguments(caps || rop || layout ? 2 : pixel ? 4 : 12);
	uint64_t result = 0;
	bool changed = false;
	uint64_t window = 0;
	bool leased = false;
	try {
		auto *dc = objects.dc(args[0]);
		if (!dc)
			throw uint32_t(6);
		window = dc->window;
		leased = dc->leased;
		if (caps) {
			result = device_caps(objects, *dc, int32_t(uint32_t(args[1])));
		} else if (rop) {
			const auto mode = uint32_t(args[1]);
			if (mode < 1 || mode > 16)
				throw uint32_t(87);
			result = dc->rop2;
			dc->rop2 = mode;
		} else if (layout) {
			// Right-to-left mirroring is not implemented, so only the default layout can be selected.
			if (uint32_t(args[1]) & ~8U)
				throw uint32_t(120);
			result = 0;
		} else {
			const auto x = int64_t(int32_t(uint32_t(args[1]))) + dc->origin_x, y = int64_t(int32_t(uint32_t(args[2]))) + dc->origin_y;
			const auto color = uint32_t(args[3]);
			auto *bitmap = objects.bitmap(dc->bitmap);
			if (!bitmap || bitmap->depth != 32 || (color & 0xff000000U))
				throw uint32_t(120);
			if (x < 0 || y < 0 || x >= bitmap->width || y >= bitmap->height || !dc->contains(x, y))
				throw uint32_t(87);
			const auto pixels = bitmap->pixels();
			const auto row = bitmap->top_down ? y : bitmap->height - 1 - y;
			const auto at = (size_t(row) * bitmap->width + size_t(x)) * 4;
			pixels[at] = uint8_t(color >> 16);
			pixels[at + 1] = uint8_t(color >> 8);
			pixels[at + 2] = uint8_t(color);
			pixels[at + 3] = 0;
			result = color;
			changed = true;
		}
		if (bits) {
			// SetDIBitsToDevice(hdc, xDest, yDest, w, h, xSrc, ySrc, StartScan, cLines, lpBits, lpbmi, ColorUse)
			const auto destination_x = int64_t(int32_t(uint32_t(args[1]))), destination_y = int64_t(int32_t(uint32_t(args[2])));
			const auto width = uint32_t(args[3]), height = uint32_t(args[4]);
			const auto source_x = int64_t(int32_t(uint32_t(args[5]))), source_y = int64_t(int32_t(uint32_t(args[6])));
			const auto start = uint32_t(args[7]), lines = uint32_t(args[8]);
			if (uint32_t(args[11]) != 0 || width > 16384 || height > 16384)
				throw uint32_t(120);
			const auto header = process.read_memory(args[10], 40);
			if (le(header, 0, 4) != 40)
				throw uint32_t(87);
			const auto image_width = int32_t(uint32_t(le(header, 4, 4)));
			const auto raw_height = int32_t(uint32_t(le(header, 8, 4)));
			const auto planes = uint32_t(le(header, 12, 2)), depth = uint32_t(le(header, 14, 2));
			const auto compression = uint32_t(le(header, 16, 4));
			uint32_t colors = uint32_t(le(header, 32, 4));
			if (planes != 1 || compression != 0 || image_width <= 0 || raw_height == 0 || raw_height == INT32_MIN || image_width > 16384 ||
				(depth != 1 && depth != 4 && depth != 8 && depth != 24 && depth != 32))
				throw uint32_t(120);
			const bool top_down = raw_height < 0;
			const uint32_t image_height = uint32_t(top_down ? -raw_height : raw_height);
			if (image_height > 16384 || start > image_height || lines > image_height - start)
				throw uint32_t(87);
			if (depth <= 8 && (!colors || colors > (1U << depth)))
				colors = 1U << depth;
			std::vector<uint8_t> table;
			if (depth <= 8)
				table = process.read_memory(args[10] + 40, size_t(colors) * 4);
			const size_t stride = ((size_t(image_width) * depth + 31) / 32) * 4;
			const auto payload = process.read_memory(args[9], stride * lines);
			auto *bitmap = objects.bitmap(dc->bitmap);
			if (!bitmap || bitmap->depth != 32)
				throw uint32_t(120);
			const auto pixels = bitmap->pixels();
			for (uint32_t row = 0; row < height; ++row) {
				// Bottom-up DIBs count source rows from their last scan line.
				const int64_t dib_row = top_down ? source_y + row : source_y + (int64_t(height) - 1 - row);
				const int64_t scan = dib_row;
				if (scan < int64_t(start) || scan >= int64_t(start) + lines || scan >= image_height)
					continue;
				const auto stored = size_t(scan - start);
				const auto *line = payload.data() + stored * stride;
				for (uint32_t column = 0; column < width; ++column) {
					const int64_t sx = source_x + column;
					if (sx < 0 || sx >= image_width)
						continue;
					uint8_t blue = 0, green = 0, red = 0;
					if (depth == 24 || depth == 32) {
						const auto *p = line + size_t(sx) * (depth / 8);
						blue = p[0]; green = p[1]; red = p[2];
					} else {
						const uint32_t per_byte = 8 / depth;
						const uint8_t byte = line[size_t(sx) / per_byte];
						const uint32_t shift = (per_byte - 1 - uint32_t(sx) % per_byte) * depth;
						const uint32_t index = (byte >> shift) & ((1U << depth) - 1);
						if (index >= colors)
							continue;
						blue = table[index * 4]; green = table[index * 4 + 1]; red = table[index * 4 + 2];
					}
					const auto px = destination_x + column + dc->origin_x, py = destination_y + row + dc->origin_y;
					if (px < 0 || py < 0 || px >= bitmap->width || py >= bitmap->height || !dc->contains(px, py))
						continue;
					const auto target_row = bitmap->top_down ? py : bitmap->height - 1 - py;
					const auto at = (size_t(target_row) * bitmap->width + size_t(px)) * 4;
					pixels[at] = blue; pixels[at + 1] = green; pixels[at + 2] = red; pixels[at + 3] = 0;
					changed = true;
				}
			}
			result = lines;
		}
	} catch (uint32_t error) {
		environment.set_last_error(error);
		result = layout || pixel ? UINT32_MAX : 0;
	} catch (const std::bad_alloc &) {
		environment.set_last_error(8);
	} catch (const std::runtime_error &failure) {
		const std::string_view message = failure.what();
		if (message.find("readable") == message.npos && message.find("writable") == message.npos)
			throw;
		environment.set_last_error(998);
		result = pixel ? UINT32_MAX : 0;
	}
	if (changed && window && leased && updated)
		updated(window,nullptr);
	process.return_api(result);
	return true;
}

}
