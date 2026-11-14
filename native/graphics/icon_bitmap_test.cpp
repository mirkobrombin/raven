#include "icon_bitmap.h"
#include "pe/loaded_image.h"
#include "pe/resources.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static void put(std::vector<uint8_t> &bytes, size_t offset, uint32_t value, size_t size = 4)
{
	for (size_t index = 0; index < size; ++index)
		bytes.at(offset + index) = uint8_t(value >> (index * 8));
}

static const std::array<std::array<uint8_t, 3>, 6> colors{{
	{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {8, 99, 247}, {247, 8, 99}, {99, 247, 8}}};
static const std::array<uint8_t, 6> alphas{0, 128, 255, 1, 254, 0};
static const std::array<uint8_t, 8> mono_and{0, 0, 1, 1, 1, 1, 0, 0};
static const std::array<uint8_t, 8> mono_xor{0, 1, 0, 1, 1, 0, 1, 0};

static std::vector<uint8_t> make_dib(uint32_t depth)
{
	const size_t width = depth == 1 ? 4 : 3;
	const size_t palette = depth <= 8 ? size_t(1) << depth : 0;
	const size_t stride = ((width * depth + 31) / 32) * 4;
	const size_t offset = 40 + palette * 4;
	std::vector<uint8_t> bytes(offset + stride * 2 + 8);
	put(bytes, 0, 40);
	put(bytes, 4, uint32_t(width));
	put(bytes, 8, 4);
	put(bytes, 12, 1, 2);
	put(bytes, 14, depth, 2);
	if (depth == 1) {
		put(bytes, 44, 0x00ffffff);
	} else if (palette) {
		for (size_t index = 0; index < colors.size(); ++index)
			for (size_t channel = 0; channel < 3; ++channel)
				bytes[40 + index * 4 + channel] = colors[index][2 - channel];
	}
	for (size_t y = 0; y < 2; ++y) {
		const size_t row = 1 - y;
		for (size_t x = 0; x < width; ++x) {
			const size_t pixel = y * width + x;
			const size_t start = offset + row * stride;
			const uint8_t mask = depth == 1 ? mono_and[pixel] : uint8_t(pixel % 2);
			bytes[offset + stride * 2 + row * 4] |= uint8_t(mask << (7 - x));
			if (depth <= 8) {
				const auto index = depth == 1 ? mono_xor[pixel] : uint8_t(pixel);
				const size_t bit = x * depth;
				bytes[start + bit / 8] |= uint8_t(index << (8 - depth - bit % 8));
			} else if (depth == 16) {
				uint32_t value = 0;
				for (size_t channel = 0; channel < 3; ++channel)
					value |= uint32_t(colors[pixel][channel] >> 3) << ((2 - channel) * 5);
				put(bytes, start + x * 2, value, 2);
			} else {
				for (size_t channel = 0; channel < 3; ++channel)
					bytes[start + x * (depth / 8) + channel] = colors[pixel][2 - channel];
				if (depth == 32)
					bytes[start + x * 4 + 3] = alphas[pixel];
			}
		}
	}
	return bytes;
}

static void rejected(const std::function<void()> &operation, std::string_view reason)
{
	std::string failure;
	try { operation(); } catch (const std::runtime_error &error) { failure = error.what(); }
	if (failure.empty() || failure.find(reason) == failure.npos)
		throw std::runtime_error("DIB negative expected " + std::string(reason) + "; observed: " + failure);
}

static void save(const std::filesystem::path &path, std::span<const uint8_t> bytes)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	check(bool(output), "cannot save icon DIB evidence");
}

static void save_icon(const std::filesystem::path &path, const raven::IconBitmap &icon)
{
	std::vector<uint8_t> bytes(16);
	put(bytes, 0, icon.color.width);
	put(bytes, 4, icon.color.height);
	put(bytes, 8, icon.xor_mask.empty() ? 0 : 1);
	put(bytes, 12, icon.has_alpha ? 1 : 0);
	bytes.insert(bytes.end(), icon.color.rgba.begin(), icon.color.rgba.end());
	bytes.insert(bytes.end(), icon.and_mask.begin(), icon.and_mask.end());
	bytes.insert(bytes.end(), icon.xor_mask.begin(), icon.xor_mask.end());
	save(path, bytes);
}

int main(int argc, char **argv)
{
	try {
		check(argc == 3, "application and icon output directory required");
		const std::filesystem::path directory(argv[2]);
		std::filesystem::create_directories(directory);
		for (const uint32_t depth : {1U, 4U, 8U, 16U, 24U, 32U}) {
			const auto bytes = make_dib(depth);
			const auto icon = raven::decode_icon_dib(bytes);
			check(icon.color.height == 2 && icon.color.width == (depth == 1 ? 4 : 3), "DIB dimensions differ");
			check(icon.has_alpha == (depth == 32), "DIB alpha detection differs");
			if (depth == 1) {
				check(std::equal(mono_and.begin(), mono_and.end(), icon.and_mask.begin()) &&
					std::equal(mono_xor.begin(), mono_xor.end(), icon.xor_mask.begin()), "monochrome raster bits differ");
			} else {
				check(icon.xor_mask.empty(), "color DIB has monochrome XOR data");
				for (size_t pixel = 0; pixel < colors.size(); ++pixel) {
					for (size_t channel = 0; channel < 3; ++channel)
						check(icon.color.rgba[pixel * 4 + channel] == colors[pixel][channel], "DIB channel or row order differs");
					check(icon.color.rgba[pixel * 4 + 3] == (depth == 32 ? alphas[pixel] : 255), "DIB straight alpha differs");
					check(icon.and_mask[pixel] == pixel % 2, "DIB AND mask differs");
				}
			}
			save(directory / ("known-" + std::to_string(depth) + ".dib"), bytes);
			save_icon(directory / ("known-" + std::to_string(depth) + ".icon"), icon);
		}
		auto zero_alpha = make_dib(32);
		for (size_t offset = 43; offset < 64; offset += 4)
			zero_alpha[offset] = 0;
		const auto legacy = raven::decode_icon_dib(zero_alpha);
		check(!legacy.has_alpha && legacy.and_mask == raven::decode_icon_dib(make_dib(32)).and_mask,
			"zero-alpha DIB lost AND mask");
		save(directory / "known-zero-alpha.dib", zero_alpha);
		save_icon(directory / "known-zero-alpha.icon", legacy);
		// A 32-bit icon may end after its pixels, as the icons Windows Forms carries do: its alpha says which pixels are transparent.
		const auto alpha_only = make_dib(32);
		const auto unmasked = raven::decode_icon_dib(std::span(alpha_only).first(alpha_only.size() - 8));
		for (size_t pixel = 0; pixel < alphas.size(); ++pixel)
			check(unmasked.and_mask[pixel] == (alphas[pixel] == 0) && unmasked.color.rgba[pixel * 4 + 3] == alphas[pixel],
				"32-bit DIB without an AND mask differs");
		const auto valid = make_dib(8);
		rejected([&] { raven::decode_icon_dib({}); }, "truncated");
		for (size_t size : {4U, 39U, 40U, 1063U, 1079U})
			rejected([&] { raven::decode_icon_dib(std::span(valid).first(size)); }, "truncated");
		for (const auto &change : std::array<std::array<uint32_t, 3>, 12>{{
			{0, 12, 4}, {4, 0, 4}, {4, 0xffffffff, 4}, {8, 3, 4}, {8, 0xfffffffc, 4},
			{4, 8193, 4}, {8, 16386, 4}, {12, 2, 2}, {14, 2, 2}, {16, 3, 4}, {32, 257, 4}, {32, 1, 4}}}) {
			auto bad = valid;
			put(bad, change[0], change[1], change[2]);
			rejected([&] { raven::decode_icon_dib(bad); }, change[0] == 32 && change[1] == 1 ? "index out of range" : "icon DIB");
		}
		auto large = valid;
		put(large, 4, 4097);
		put(large, 8, 8194);
		rejected([&] { raven::decode_icon_dib(large); }, "dimensions");
		std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg() > 0 && file.tellg() < 1024 * 1024 * 1024, "invalid real icon input");
		std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg()));
		file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))), "cannot read real icon input");
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		unsigned decoded = 0;
		for (const auto &resource : image.resources()) {
			if (resource.type != raven::ResourceKey(uint32_t(3)))
				continue;
			const auto data = std::span<const uint8_t>(loaded.data()).subspan(resource.data_address, resource.size);
			if (data.size() >= 4 && data[0] == 137 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
				continue;
			const auto icon = raven::decode_icon_dib(data);
			const auto id = std::get<uint32_t>(resource.name);
			save(directory / (std::to_string(id) + ".dib"), data);
			save_icon(directory / (std::to_string(id) + ".icon"), icon);
			++decoded;
		}
		check(decoded == 31, "real application DIB corpus changed");
		std::cout << "Decoded 31 real DIB icons; six bit depths, alpha, raster masks and malformed controls passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
