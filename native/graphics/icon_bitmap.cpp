#include "icon_bitmap.h"

#include <stdexcept>

namespace raven {

static uint32_t read_dib(std::span<const uint8_t> bytes, size_t offset, size_t size)
{
	if (offset > bytes.size() || size > bytes.size() - offset)
		throw std::runtime_error("truncated icon DIB field");
	uint32_t value = 0;
	for (size_t index = 0; index < size; ++index)
		value |= uint32_t(bytes[offset + index]) << (index * 8);
	return value;
}

IconBitmap decode_icon_dib(std::span<const uint8_t> bytes)
{
	if (bytes.size() > 128 * 1024 * 1024)
		throw std::runtime_error("icon DIB input exceeds policy limit");
	if (read_dib(bytes, 0, 4) != 40)
		throw std::runtime_error("unsupported icon DIB header");
	if (bytes.size() < 40)
		throw std::runtime_error("truncated icon DIB header");
	const auto width = read_dib(bytes, 4, 4);
	const auto double_height = read_dib(bytes, 8, 4);
	if (!width || !double_height || double_height % 2 || width > 8192 || double_height > 16384 ||
		uint64_t(width) * (double_height / 2) > 16 * 1024 * 1024)
		throw std::runtime_error("invalid or unsupported icon DIB dimensions");
	if (read_dib(bytes, 12, 2) != 1)
		throw std::runtime_error("invalid icon DIB planes");
	const auto depth = read_dib(bytes, 14, 2);
	if (depth != 1 && depth != 4 && depth != 8 && depth != 16 && depth != 24 && depth != 32)
		throw std::runtime_error("unsupported icon DIB bit depth");
	if (read_dib(bytes, 16, 4) != 0)
		throw std::runtime_error("unsupported icon DIB compression");
	uint32_t colors = read_dib(bytes, 32, 4);
	if (depth <= 8) {
		if (!colors)
			colors = 1U << depth;
		if (colors > (1U << depth))
			throw std::runtime_error("invalid icon DIB palette size");
	} else if (colors > 256) {
		throw std::runtime_error("icon DIB palette exceeds policy limit");
	}
	const uint32_t height = double_height / 2;
	const size_t color_stride = ((size_t(width) * depth + 31) / 32) * 4;
	const size_t mask_stride = ((size_t(width) + 31) / 32) * 4;
	const size_t color_offset = 40 + size_t(colors) * 4;
	const size_t mask_offset = color_offset + color_stride * height;
	// A 32-bit icon may end after its pixels, as Windows accepts: its alpha then says which pixels are transparent.
	const bool alpha_mask = depth == 32 && mask_offset == bytes.size();
	if (!alpha_mask && mask_offset + mask_stride * height > bytes.size())
		throw std::runtime_error("truncated icon DIB palette, pixels or AND mask");
	const size_t count = size_t(width) * height;
	IconBitmap icon{{width, height, std::vector<uint8_t>(count * 4)},
		std::vector<uint8_t>(count), {}, false};
	if (depth == 1)
		icon.xor_mask.resize(count);
	for (size_t y = 0; y < height; ++y) {
		const size_t row = height - 1 - y;
		const size_t color_row = color_offset + row * color_stride;
		const size_t mask_row = mask_offset + row * mask_stride;
		for (size_t x = 0; x < width; ++x) {
			const size_t pixel = y * width + x;
			const size_t output = pixel * 4;
			if (!alpha_mask)
				icon.and_mask[pixel] = uint8_t((bytes[mask_row + x / 8] >> (7 - x % 8)) & 1);
			if (depth <= 8) {
				const size_t bit = x * depth;
				const auto index = (bytes[color_row + bit / 8] >> (8 - depth - bit % 8)) & ((1U << depth) - 1);
				if (index >= colors)
					throw std::runtime_error("icon DIB palette index out of range");
				const size_t palette = 40 + index * 4;
				for (size_t channel = 0; channel < 3; ++channel)
					icon.color.rgba[output + channel] = bytes[palette + 2 - channel];
				icon.color.rgba[output + 3] = 255;
				if (depth == 1)
					icon.xor_mask[pixel] = uint8_t(index);
			} else if (depth == 16) {
				const auto value = read_dib(bytes, color_row + x * 2, 2);
				for (size_t channel = 0; channel < 3; ++channel) {
					const auto component = (value >> ((2 - channel) * 5)) & 31;
					icon.color.rgba[output + channel] = uint8_t((component << 3) | (component >> 2));
				}
				icon.color.rgba[output + 3] = 255;
			} else {
				const size_t source = color_row + x * (depth / 8);
				for (size_t channel = 0; channel < 3; ++channel)
					icon.color.rgba[output + channel] = bytes[source + 2 - channel];
				const uint8_t alpha = depth == 32 ? bytes[source + 3] : uint8_t(255);
				icon.color.rgba[output + 3] = alpha;
				if (alpha_mask)
					icon.and_mask[pixel] = alpha == 0;
				if (depth == 32 && alpha)
					icon.has_alpha = true;
			}
		}
	}
	return icon;
}

}
