#include "api.h"
#include <iostream>

#include <algorithm>
#include <array>
#include <new>
#include <optional>
#include <stdexcept>
#include <sys/mman.h>

namespace raven {

static uint32_t field(std::span<const uint8_t> bytes, size_t offset, size_t width = 4)
{
	uint32_t value = 0;
	for (size_t byte = 0; byte < width; ++byte)
		value |= uint32_t(bytes[offset+byte]) << (byte*8);
	return value;
}

uint64_t NativeDesktop::create_dc(uint64_t source)
{
	if (source && !objects.dc(source)) {
		environment.set_last_error(6);
		return 0;
	}
	const auto dc = objects.add(DesktopDC{0x10000,0x10000});
	if (!dc)
		environment.set_last_error(8);
	return dc;
}

void NativeDesktop::apply_deferred(uint32_t operation, uint64_t handle)
{
	if (operation == 1) {
		if (const auto *icon = objects.icon(handle); icon && !icon->shared)
			objects.erase(handle);
	} else if (operation == 2) {
		delete_dc(handle);
	} else if (operation == 3) {
		delete_bitmap(handle);
	}
}

bool NativeDesktop::delete_dc(uint64_t handle)
{
	const auto *dc = objects.dc(handle);
	if (!dc || dc->window)
		return false;
	return objects.erase(handle);
}

uint64_t NativeDesktop::select(uint64_t handle, uint64_t bitmap_handle)
{
	auto *dc = objects.dc(handle);
	if (dc && objects.font(bitmap_handle)) return objects.select_font(handle,bitmap_handle);
	if (dc && objects.brush(bitmap_handle)) return objects.select_brush(handle,bitmap_handle);
	if (dc && objects.pen(bitmap_handle)) return objects.select_pen(handle,bitmap_handle);
	if (dc && objects.region(bitmap_handle)) {
		const auto value=*objects.region(bitmap_handle);const auto result=value.complexity();
		if (!objects.select_clip(handle,value)) {environment.set_last_error(8);return 0;}
		return result;
	}
	const auto *bitmap = objects.bitmap(bitmap_handle);
	if (!dc || !bitmap || dc->window || (bitmap->stock && bitmap_handle != dc->default_bitmap)) {
		environment.set_last_error(6);
		return 0;
	}
	if (dc->bitmap == bitmap_handle)
		return bitmap_handle;
	if (!bitmap->stock && objects.selected(bitmap_handle)) {
		environment.set_last_error(87);
		return 0;
	}
	const auto previous = dc->bitmap;
	objects.reselect(*dc, bitmap_handle);
	return previous;
}

bool NativeDesktop::delete_bitmap(uint64_t handle)
{
	const auto *bitmap = objects.bitmap(handle);
	if (!bitmap) return false;
	if (bitmap->stock) return true;
	if (objects.selected(handle)) return false;
	if (bitmap->address)
		process.unmap_memory(bitmap->address);
	return objects.erase(handle);
}

uint64_t NativeDesktop::create_section(std::span<const uint64_t> args)
{
	// The bits pointer the program gets back is as wide as its pointers: a 32-bit program keeps other data in the four bytes after it.
	std::vector<uint8_t> output(process.mode()==NativeMode::x86 ? 4 : 8);
	if (args[3]) process.write_memory(args[3],output);
	if (args[0] && !objects.dc(args[0])) {
		environment.set_last_error(6);
		return 0;
	}
	if (uint32_t(args[2]) > 1) {
		environment.set_last_error(120);
		return 0;
	}
	const auto header_size = field(process.read_memory(args[1],4),0);
	if (header_size != 40 && header_size != 108 && header_size != 124) {
		environment.set_last_error(87);
		return 0;
	}
	const auto header = process.read_memory(args[1],header_size);
	const auto width = int32_t(field(header,4)), height = int32_t(field(header,8));
	const auto bits = field(header,14,2), compression = field(header,16);
	if (width <= 0 || width > 16384 || !height || height < -16384 || height > 16384 ||
		size_t(width) * uint32_t(height < 0 ? -height : height) > 16*1024*1024) {
		environment.set_last_error(87);
		return 0;
	}
	if (std::getenv("RAVEN_TRACE_FAIL")) std::cerr << "dib header size=" << header_size << " w=" << width << " h=" << height << " planes=" << field(header,12,2) << " bits=" << bits << " compression=" << compression << "\n";
	if (field(header,12,2) != 1 || !(bits == 1 || bits == 4 || bits == 8 || bits == 16 || bits == 24 || bits == 32) ||
		(compression != 0 && !(compression == 3 && (bits == 16 || bits == 32)))) {
		environment.set_last_error(87);
		return 0;
	}
	std::vector<uint32_t> palette;
	if (bits <= 8) {
		uint32_t colors = field(header,32);
		if (!colors || colors > (1U << bits)) colors = 1U << bits;
		if (uint32_t(args[2]) == 1) {
			// DIB_PAL_COLORS entries index the device palette; without one they select evenly spaced grays.
			for (uint32_t i = 0; i < colors; ++i) { const uint32_t level = bits == 1 ? i * 255 : i * 255 / ((1U << bits) - 1); palette.push_back(level | level << 8 | level << 16); }
		} else {
			const auto entries = process.read_memory(args[1] + header_size, size_t(colors) * 4);
			for (uint32_t i = 0; i < colors; ++i) palette.push_back(uint32_t(entries[i*4+2]) | uint32_t(entries[i*4+1]) << 8 | uint32_t(entries[i*4]) << 16);
		}
		while (palette.size() < (1U << bits)) palette.push_back(0);
	}
	const auto bitmap=allocate_dib(uint32_t(width),uint32_t(height<0 ? -height : height),bits,height<0,std::move(palette),args[4],uint32_t(args[5]));
	if (!bitmap) return 0;
	try {
		if (compression == 3) {
			const auto masks = process.read_memory(args[1] + 40, 12);
			for (size_t channel = 0; channel < 3; ++channel)
				objects.bitmap(bitmap)->masks[channel] = field(masks, channel * 4);
		}
		const auto address=objects.bitmap(bitmap)->address;
		for (size_t byte=0;byte<output.size();++byte) output[byte]=uint8_t(address>>(byte*8));
		if (args[3]) process.write_memory(args[3],output);
	} catch (...) { delete_bitmap(bitmap); throw; }
	return bitmap;
}

uint64_t NativeDesktop::allocate_dib(uint32_t width,uint32_t height,uint32_t bits,bool top_down,std::vector<uint32_t> palette,uint64_t section,uint64_t section_offset)
{
	const size_t stride=((size_t(width)*bits+31)/32)*4;
	if (height>64*1024*1024/stride) { environment.set_last_error(8); return 0; }
	const size_t bytes=stride*height;
	uint64_t address=0,bitmap=0;
	try {
		std::shared_ptr<SharedPixels> shared;
		if (section) {
			// The pixels live in the section the program made; the caller's offset and the section's size bound them.
			const auto found=section_source ? section_source(section) : std::nullopt;
			if (!found) { environment.set_last_error(6); return 0; }
			if (section_offset>found->second || bytes>found->second-section_offset) { environment.set_last_error(87); return 0; }
			try { shared=std::make_shared<SharedPixels>(found->first,section_offset,(bytes+4095)&~size_t(4095)); }
			catch (const std::invalid_argument &) { environment.set_last_error(87); return 0; }
		} else {
			shared=std::make_shared<SharedPixels>((bytes+4095)&~size_t(4095));
		}
		address=process.map_shared(shared->descriptor(),shared->size(),shared->offset());
		DesktopBitmap record{width,height,bits,{},address,top_down,false,true,shared};
		record.stride=uint32_t(stride);
		record.palette=std::move(palette);
		bitmap=objects.add(std::move(record));
	} catch (const std::bad_alloc &) {
		if (address) process.unmap_memory(address);
		environment.set_last_error(8); return 0;
	} catch (...) {
		if (address) process.unmap_memory(address);
		throw;
	}
	if (!bitmap) {
		if (address) process.unmap_memory(address);
		environment.set_last_error(8);
	}
	return bitmap;
}

uint64_t NativeDesktop::allocate_bitmap(uint32_t width,uint32_t height,uint32_t depth,bool section,bool top_down)
{
	const size_t stride=depth==1 ? ((size_t(width)+15)/16)*2 : size_t(width)*4;
	if (height>64*1024*1024/stride) { environment.set_last_error(8); return 0; }
	const size_t bytes=stride*height;
	uint64_t address=0,bitmap=0;
	try {
		std::shared_ptr<SharedPixels> shared;
		if (section) {
			shared=std::make_shared<SharedPixels>((bytes+4095)&~size_t(4095));
			address=process.map_shared(shared->descriptor(),shared->size());
		}
		bitmap=objects.add(DesktopBitmap{width,height,depth,section ? std::vector<uint8_t>{} : std::vector<uint8_t>(bytes),address,top_down,false,section,shared});
	} catch (const std::bad_alloc &) {
		if (address) process.unmap_memory(address);
		environment.set_last_error(8); return 0;
	} catch (...) {
		if (address) process.unmap_memory(address);
		throw;
	}
	if (!bitmap) {
		if (address) process.unmap_memory(address);
		environment.set_last_error(8);
	}
	return bitmap;
}

uint64_t NativeDesktop::compatible_bitmap(std::span<const uint64_t> args)
{
	const int64_t width=int32_t(uint32_t(args[1])),height=int32_t(uint32_t(args[2]));
	// Zero extents return the immutable default identity before validating the DC.
	if (!width || !height) return 0x10000;
	const auto *dc=objects.dc(args[0]);
	if (!dc) { environment.set_last_error(87); return 0; }
	const auto *selected=objects.bitmap(dc->bitmap);
	if (!selected || (selected->depth!=1 && selected->depth!=32)) { environment.set_last_error(120); return 0; }
	if (selected->dib_section && width<0) return 0;
	return allocate_bitmap(uint32_t(width<0 ? -width : width),uint32_t(height<0 ? -height : height),
		selected->depth,selected->dib_section,selected->dib_section ? height<0 : true);
}

// A color icon stretched by averaging the source area each target pixel covers, weighted by alpha, so that thin lines survive
// shrinking as partly transparent pixels; the mask keeps every pixel any part of the icon shows in.
static IconBitmap stretch_icon(const IconBitmap &image, uint32_t width, uint32_t height)
{
	const auto source_width = image.color.width, source_height = image.color.height;
	IconBitmap out = image;
	out.has_alpha = true;
	out.color.width = width;
	out.color.height = height;
	out.color.rgba.assign(size_t(width) * height * 4, 0);
	out.and_mask.assign(size_t(width) * height, 1);
	out.xor_mask.clear();
	const double scale_x = double(source_width) / width, scale_y = double(source_height) / height;
	for (uint32_t y = 0; y < height; ++y)
		for (uint32_t x = 0; x < width; ++x) {
			const double left = x * scale_x, right = (x + 1) * scale_x, top = y * scale_y, bottom = (y + 1) * scale_y;
			double sum[4] = {}, total = 0, shown = 0;
			for (auto sy = uint32_t(top); sy < source_height && sy < bottom; ++sy)
				for (auto sx = uint32_t(left); sx < source_width && sx < right; ++sx) {
					const double weight = (std::min(right, sx + 1.0) - std::max(left, double(sx))) * (std::min(bottom, sy + 1.0) - std::max(top, double(sy)));
					const size_t at = size_t(sy) * source_width + sx;
					const double alpha = image.has_alpha ? image.color.rgba[at * 4 + 3] / 255.0 : image.and_mask[at] ? 0.0 : 1.0;
					for (size_t channel = 0; channel < 3; ++channel)
						sum[channel] += weight * alpha * image.color.rgba[at * 4 + channel];
					sum[3] += weight * alpha;
					total += weight;
					if (!image.and_mask[at])
						shown += weight;
				}
			const size_t at = size_t(y) * width + x;
			for (size_t channel = 0; channel < 3; ++channel)
				out.color.rgba[at * 4 + channel] = sum[3] > 0 ? uint8_t(std::min(255.0, sum[channel] / sum[3] + 0.5)) : 0;
			out.color.rgba[at * 4 + 3] = total > 0 ? uint8_t(std::min(255.0, sum[3] / total * 255 + 0.5)) : 0;
			out.and_mask[at] = shown > 0 ? 0 : 1;
		}
	return out;
}

bool NativeDesktop::draw(std::span<const uint64_t> args)
{
	const auto *dc = objects.dc(args[0]);
	const auto *icon = objects.icon(args[3]);
	if (!dc || !icon) {
		environment.set_last_error(6);
		return false;
	}
	auto *bitmap = objects.bitmap(dc->bitmap);
	const auto width = int32_t(uint32_t(args[4])), height = int32_t(uint32_t(args[5]));
	const auto flags = uint32_t(args[8]), mode = flags & 3;
	if (bitmap->depth != 32 || args[7] || !mode || (flags & ~7U) || width < 0 || height < 0 || width > 1024 || height > 1024) {
		environment.set_last_error(120);
		return false;
	}
	// A size other than the icon's own stretches it, as programs draw their check box glyphs smaller than they made them.
	std::optional<IconBitmap> stretched;
	if ((width && uint32_t(width) != icon->image.color.width) || (height && uint32_t(height) != icon->image.color.height)) {
		const auto target_width = width ? uint32_t(width) : icon->image.color.width, target_height = height ? uint32_t(height) : icon->image.color.height;
		if (icon->monochrome) {
			stretched = icon->image;
			resize_icon(*stretched, target_width, target_height, true);
		} else
			stretched = stretch_icon(icon->image, target_width, target_height);
	}
	const auto &image = stretched ? *stretched : icon->image;
	const int64_t left = int64_t(int32_t(uint32_t(args[1])))+dc->origin_x;
	const int64_t top = int64_t(int32_t(uint32_t(args[2])))+dc->origin_y;
	const auto x0 = std::max(int64_t(0),left), y0 = std::max(int64_t(0),top);
	const auto x1 = std::min(int64_t(bitmap->width),left+image.color.width);
	const auto y1 = std::min(int64_t(bitmap->height),top+image.color.height);
	if (x0 >= x1 || y0 >= y1)
		return true;
	if (dc->clip && dc->clip->empty()) return true;
	const auto pixels = bitmap->pixels();
	bool changed=false;
	for (auto y = y0; y < y1; ++y)
		for (auto x = x0; x < x1; ++x) {
			if (!dc->contains(x,y)) continue;
			changed=true;
			const size_t source = size_t(y-top)*image.color.width+size_t(x-left);
			const size_t row = bitmap->top_down ? size_t(y) : bitmap->height-1-size_t(y);
			const size_t target = (row*bitmap->width+size_t(x))*4;
			const auto alpha = image.color.rgba[source*4+3];
			for (size_t channel = 0; channel < 4; ++channel) {
				const auto color = image.color.rgba[source*4+(channel<3 ? 2-channel : 3)];
				auto &destination = pixels[target+channel];
				if (mode == 1)
					destination = channel==3 ? 0 : uint8_t(image.and_mask[source]*255);
				else if (image.has_alpha)
					destination = uint8_t((channel==3 ? alpha : (uint32_t(color)*alpha+127)/255) +
						(uint32_t(destination)*(255-alpha)+127)/255);
				else {
					const uint8_t component = channel==3 ? uint8_t(0) : color;
					destination = mode==2 ? component : uint8_t((destination & uint8_t(image.and_mask[source]*255)) ^ component);
				}
			}
		}
	if (!changed) return true;
	if (dc->window && dc->leased && updated) updated(dc->window);
	return true;
}

}
