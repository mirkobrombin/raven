#include "api.h"
#include "bitblt.h"
#include "graphics/stretch.h"
#ifdef RAVEN_PNG
#include "graphics/png_codec.h"
#endif

#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>

namespace raven {

NativeDesktop::NativeDesktop(NativeProcess &process, NativeEnvironment &environment, DesktopObjects &objects, NativeResources &resources)
	: process(process), environment(environment), objects(objects), resources(resources) {}

std::span<const std::string_view> NativeDesktop::control_exports()
{
	static constexpr std::array<std::string_view,1> names{"LoadIconWithScaleDown"};
	return names;
}

std::span<const std::string_view> NativeDesktop::user_exports()
{
	static constexpr std::array<std::string_view, 89> names{
		"AdjustWindowRect", "AdjustWindowRectEx", "BeginPaint", "CallWindowProcW", "ClientToScreen", "CopyIcon", "CreateIconFromResourceEx", "CreateWindowExW",
		"DefWindowProcW", "DestroyIcon", "DestroyMenu", "DestroyWindow", "DispatchMessageW", "DrawIcon", "DrawIconEx", "EndPaint", "FillRect",
		"FrameRect", "GetActiveWindow", "GetClassInfoExW", "GetClassInfoW", "GetClassLongA", "GetClassLongPtrA", "GetClassLongPtrW", "GetClassLongW",
		"GetClassNameA", "GetClassNameW", "GetClientRect", "GetDC", "GetDlgCtrlID", "GetDlgItem", "GetFocus", "GetIconInfo",
		"GetMenuItemCount", "GetMenuItemID", "GetMessageW", "GetParent", "GetSubMenu", "GetSysColor", "GetSysColorBrush", "GetSystemMetrics",
		"GetUpdateRect", "GetWindowDC", "GetWindowLongA", "GetWindowLongPtrA", "GetWindowLongPtrW", "GetWindowLongW", "GetWindowRect", "GetWindowTextLengthW",
		"GetWindowTextW", "InternalGetWindowText", "InvalidateRect", "IsChild", "IsMenu", "IsWindow", "IsWindowUnicode", "IsWindowVisible",
		"LoadMenuW", "LoadStringW", "LookupIconIdFromDirectoryEx", "MapWindowPoints", "PeekMessageW", "PostMessageW", "PostQuitMessage", "PostThreadMessageW",
		"PrivateExtractIconsW", "RegisterClassExW", "RegisterClassW", "ReleaseDC", "ScreenToClient", "SendMessageTimeoutW", "SendMessageW", "SendNotifyMessageW", "SetClassLongA",
		"SetClassLongPtrA", "SetClassLongPtrW", "SetClassLongW", "SetFocus", "SetWindowLongA", "SetWindowLongPtrA", "SetWindowLongPtrW", "SetWindowLongW",
		"SetWindowTextW", "ShowWindow", "TranslateMessage", "UnregisterClassW", "UpdateWindow", "ValidateRect", "WindowFromDC",
	};
	return names;
}

std::span<const std::string_view> NativeDesktop::gdi_exports()
{
#ifdef RAVEN_SKIA
	static constexpr std::array<std::string_view, 52> names{
		"BitBlt", "CombineRgn", "CreateCompatibleBitmap", "CreateCompatibleDC", "CreateDCW", "CreateDIBSection", "CreateFontIndirectW", "CreatePen", "CreateRectRgn", "CreateSolidBrush", "DPtoLP", "DeleteDC", "DeleteObject", "EqualRgn", "ExcludeClipRect", "ExtTextOutW", "GdiFlush", "GetBitmapBits", "GetBkColor", "GetBkMode", "GetClipBox", "GetClipRgn", "GetCurrentObject", "GetCurrentPositionEx", "GetGraphicsMode", "GetLayout", "GetObjectA", "GetObjectType", "GetObjectW", "GetRgnBox", "GetStockObject", "GetTextColor", "GetTextExtentExPointW", "GetTextExtentPoint32W", "GetTextExtentPointW", "GetTextMetricsW", "GetViewportExtEx", "GetWindowExtEx", "IntersectClipRect", "LPtoDP", "LineTo", "MoveToEx", "OffsetRgn", "PtInRegion", "RectInRegion", "SelectClipRgn", "SelectObject", "SetBkColor", "SetBkMode", "SetRectRgn", "SetTextColor", "TextOutW"};
#else
	static constexpr std::array<std::string_view, 46> names{
		"BitBlt", "CombineRgn", "CreateCompatibleBitmap", "CreateCompatibleDC", "CreateDCW", "CreateDIBSection", "CreateFontIndirectW", "CreatePen", "CreateRectRgn", "CreateSolidBrush", "DPtoLP", "DeleteDC", "DeleteObject", "EqualRgn", "ExcludeClipRect", "GdiFlush", "GetBitmapBits", "GetBkColor", "GetBkMode", "GetClipBox", "GetClipRgn", "GetCurrentObject", "GetCurrentPositionEx", "GetGraphicsMode", "GetLayout", "GetObjectA", "GetObjectType", "GetObjectW", "GetRgnBox", "GetStockObject", "GetTextColor", "GetViewportExtEx", "GetWindowExtEx", "IntersectClipRect", "LPtoDP", "LineTo", "MoveToEx", "OffsetRgn", "PtInRegion", "RectInRegion", "SelectClipRgn", "SelectObject", "SetBkColor", "SetBkMode", "SetRectRgn", "SetTextColor"};
#endif
	return names;
}

static void put(std::span<uint8_t> bytes, size_t offset, uint64_t value, size_t size = 4)
{
	for (size_t index = 0; index < size; ++index)
		bytes[offset + index] = uint8_t(value >> (index * 8));
}

uint64_t NativeDesktop::lookup(std::span<const uint64_t> args)
{
	const auto width = int32_t(uint32_t(args[2]));
	const auto height = int32_t(uint32_t(args[3]));
	if (!uint32_t(args[1]) || (uint32_t(args[4]) & ~1U)) {
		environment.set_last_error(120);
		return 0;
	}
	if (width < 0 || height < 0) {
		environment.set_last_error(87);
		return 0;
	}
	const auto header = process.read_memory(args[0], 6);
	const size_t count = size_t(header[4]) | (size_t(header[5]) << 8);
	const auto bytes = process.read_memory(args[0], 6 + count * 14);
	std::vector<IconEntry> entries;
	try {
		entries = decode_icon_group(bytes);
	} catch (const std::runtime_error &) {
		environment.set_last_error(13);
		return 0;
	}
	return lookup_icon(entries, uint32_t(width), uint32_t(height), objects.display(), (uint32_t(args[4]) & 1U) != 0);
}

void resize_icon(IconBitmap &image, uint32_t width, uint32_t height, bool monochrome)
{
	const auto source_width = image.color.width, source_height = image.color.height;
	const auto stretch = [&](std::span<const uint8_t> pixels, uint32_t channels, uint32_t planes = 1) {
		return stretch_blackonwhite(pixels, source_width, source_height * planes, channels, width, height * planes);
	};
	if (monochrome) {
		// Resize the stacked AND/XOR bitmap as one operand.
		auto compound = image.and_mask;
		compound.insert(compound.end(), image.xor_mask.begin(), image.xor_mask.end());
		const auto mask = stretch(compound, 1, 2);
		const size_t pixels = size_t(width) * height;
		image.and_mask.assign(mask.begin(), mask.begin() + static_cast<std::ptrdiff_t>(pixels));
		image.xor_mask.assign(mask.begin() + static_cast<std::ptrdiff_t>(pixels), mask.end());
		image.color.rgba.assign(pixels * 4, 0);
		for (size_t index = 0; index < pixels; ++index)
			for (size_t channel = 0; channel < 3; ++channel)
				image.color.rgba[index * 4 + channel] = image.xor_mask[index] ? 255 : 0;
	} else {
		auto color = stretch(image.color.rgba, 4);
		auto mask = stretch(image.and_mask, 1);
		if (!image.xor_mask.empty())
			image.xor_mask = stretch(image.xor_mask, 1);
		image.color.rgba = std::move(color);
		image.and_mask = std::move(mask);
	}
	image.color.width = width;
	image.color.height = height;
}

uint64_t NativeDesktop::create(std::span<const uint64_t> args)
{
	const auto size = uint32_t(args[1]);
	const auto version = uint32_t(args[3]);
	const auto flags = uint32_t(args[6]);
	const auto width = int32_t(uint32_t(args[4]));
	const auto height = int32_t(uint32_t(args[5]));
	if (version != 0x30000 || (flags & ~0x8000U)) {
		environment.set_last_error(120);
		return 0;
	}
	if (!size || size > 128 * 1024 * 1024 || width < 0 || height < 0 || width > 8192 || height > 8192 ||
		(width && height && uint64_t(width) * uint64_t(height) > 16 * 1024 * 1024)) {
		environment.set_last_error(87);
		return 0;
	}
	const auto bytes = process.read_memory(args[0], size);
	if (uint32_t(args[2]))
		return create_icon(bytes,uint32_t(width),uint32_t(height),flags);
	// A cursor's resource starts with its hot spot, two words, before the bitmap an icon's has.
	if (bytes.size() < 8) {
		environment.set_last_error(13);
		return 0;
	}
	const auto handle = create_icon(std::span<const uint8_t>(bytes).subspan(4),uint32_t(width),uint32_t(height),flags);
	if (auto *cursor = handle ? objects.icon(handle) : nullptr) {
		cursor->cursor = true;
		cursor->hotspot_x = uint32_t(bytes[0] | bytes[1] << 8);
		cursor->hotspot_y = uint32_t(bytes[2] | bytes[3] << 8);
	}
	return handle;
}

uint64_t NativeDesktop::create_icon(std::span<const uint8_t> bytes,uint32_t width,uint32_t height,uint32_t flags)
{
	const std::array<uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
	const bool png = bytes.size() >= 8 && std::equal(signature.begin(), signature.end(), bytes.begin());
	IconBitmap image;
	try {
		if (png) {
#ifdef RAVEN_PNG
			auto pixels = decode_png(bytes);
			std::vector<uint8_t> mask(size_t(pixels.width) * pixels.height);
			for (size_t index = 0; index < mask.size(); ++index)
				mask[index] = pixels.rgba[index * 4 + 3] != 255;
			image = {std::move(pixels), std::move(mask), {}, true};
#else
			environment.set_last_error(120);
			return 0;
#endif
		} else {
			image = decode_icon_dib(bytes);
		}
	} catch (const std::runtime_error &) {
		environment.set_last_error(13);
		return 0;
	}
	const bool monochrome = !png && !image.xor_mask.empty() && bytes.size() >= 48 &&
		bytes[40] == 0 && bytes[41] == 0 && bytes[42] == 0 &&
		bytes[44] == 255 && bytes[45] == 255 && bytes[46] == 255;
	const uint32_t target_width = width ? uint32_t(width) : image.color.width;
	const uint32_t target_height = height ? uint32_t(height) : image.color.height;
	if (uint64_t(target_width) * target_height > 16 * 1024 * 1024) {
		environment.set_last_error(87);
		return 0;
	}
	try {
		if (target_width != image.color.width || target_height != image.color.height)
			resize_icon(image, target_width, target_height, monochrome);
	} catch (const std::bad_alloc &) {
		environment.set_last_error(8);
		return 0;
	}
	const auto handle = objects.add(DesktopIcon{std::move(image), monochrome, bool(flags & 0x8000)});
	if (!handle)
		environment.set_last_error(8);
	return handle;
}

uint64_t NativeDesktop::load_icon(std::span<const uint64_t> args)
{
	const uint32_t invalid = 0x80070057U;
	if (!args[4])
		return invalid;
	// The icon handle goes back in a word as wide as the program's pointers.
	std::vector<uint8_t> output(process.mode()==NativeMode::x86 ? 4 : 8);
	process.write_memory(args[4],output);
	const auto width = int32_t(uint32_t(args[2])), height = int32_t(uint32_t(args[3]));
	if (!args[1] || width < 0 || height < 0)
		return invalid;
	if (!args[0])
		return 0x80004001U;
	const auto failure = [&] { return uint64_t(0x80070000U | (environment.last_error() & 0xffffU)); };
	const auto group = resources.view(args[0],14,args[1]);
	if (!group)
		return failure();
	std::vector<IconEntry> entries;
	const auto group_bytes = process.read_memory(group->address,group->size);
	try { entries = decode_icon_group(group_bytes); }
	catch (const std::runtime_error &) { environment.set_last_error(13); return failure(); }
	if (group->language > UINT16_MAX)
		return 0x80004001U;
	uint32_t desired_width = width ? uint32_t(width) : entries.front().width;
	uint32_t desired_height = height ? uint32_t(height) : entries.front().height;
	const auto standard = [](uint32_t size) { return size==16 || size==32 || size==48 || size==256; };
	std::vector<IconEntry> candidates;
	if (width && height && !(standard(desired_width) && standard(desired_height))) {
		for (const auto &entry : entries)
			if (entry.width >= desired_width && entry.height >= desired_height)
				candidates.push_back(entry);
		if (candidates.empty())
			for (const auto &entry : entries)
				if (entry.width <= desired_width && entry.height <= desired_height)
					candidates.push_back(entry);
		if (candidates.empty())
			return 0x80004001U;
	}
	const auto selected = lookup_icon(candidates.empty() ? std::span<const IconEntry>(entries) : std::span<const IconEntry>(candidates),
		desired_width,desired_height,objects.display(),false);
	const auto data = resources.view(args[0],3,selected,uint16_t(group->language));
	if (!data)
		return failure();
	const auto icon = create(std::array<uint64_t,7>{data->address,data->size,1,0x30000,uint32_t(width),uint32_t(height),0});
	if (!icon)
		return failure();
	put(output,0,icon,output.size());
	try { process.write_memory(args[4],output); }
	catch (...) { objects.erase(icon); throw; }
	return 0;
}

static DesktopBitmap mask_bitmap(const DesktopIcon &icon)
{
	const auto &image = icon.image;
	const uint32_t width = image.color.width;
	const uint32_t height = image.color.height;
	const size_t stride = ((size_t(width) + 15) / 16) * 2;
	DesktopBitmap bitmap{width, height * (icon.monochrome ? 2U : 1U), 1,
		std::vector<uint8_t>(stride * height * (icon.monochrome ? 2 : 1))};
	for (size_t y = 0; y < height; ++y)
		for (size_t x = 0; x < width; ++x) {
			const size_t pixel = y * width + x;
			bitmap.bits[y * stride + x / 8] |= uint8_t(image.and_mask[pixel] << (7 - x % 8));
			if (icon.monochrome && pixel < image.xor_mask.size())
				bitmap.bits[(y + height) * stride + x / 8] |= uint8_t(image.xor_mask[pixel] << (7 - x % 8));
		}
	return bitmap;
}

bool NativeDesktop::info(uint64_t handle, uint64_t output)
{
	const auto *icon = objects.icon(handle);
	if (!icon) {
		environment.set_last_error(1402);
		return false;
	}
	uint64_t mask = objects.add(mask_bitmap(*icon));
	uint64_t color = 0;
	try {
		if (!icon->monochrome) {
			const auto &pixels = icon->image.color;
			DesktopBitmap bitmap{pixels.width, pixels.height, 32, pixels.rgba};
			for (size_t index = 0; index < bitmap.bits.size(); index += 4) {
				std::swap(bitmap.bits[index], bitmap.bits[index + 2]);
				if (!icon->image.has_alpha)
					bitmap.bits[index + 3] = 0;
			}
			color = objects.add(std::move(bitmap));
		}
		if (!mask || (!icon->monochrome && !color)) {
			objects.erase(mask);
			objects.erase(color);
			environment.set_last_error(8);
			return false;
		}
		// ICONINFO holds its two bitmap handles at the program's pointer width: 20 bytes for a 32-bit program, 32 for a 64-bit one.
		const bool narrow = process.mode() == NativeMode::x86;
		std::array<uint8_t, 32> bytes{};
		put(bytes, 0, icon->cursor ? 0 : 1);
		put(bytes, 4, icon->cursor ? icon->hotspot_x : icon->image.color.width / 2);
		put(bytes, 8, icon->cursor ? icon->hotspot_y : icon->image.color.height / 2);
		put(bytes, narrow ? 12 : 16, mask, narrow ? 4 : 8);
		put(bytes, narrow ? 16 : 24, color, narrow ? 4 : 8);
		process.write_memory(output, std::span<const uint8_t>(bytes.data(), narrow ? 20 : 32));
	} catch (...) {
		objects.erase(mask);
		objects.erase(color);
		throw;
	}
	return true;
}

uint64_t NativeDesktop::create_font(uint64_t address)
{
	if (!address) { environment.set_last_error(87); return 0; }
	const auto definition=process.read_memory(address,92);
	DesktopFont record;
	std::copy(definition.begin(),definition.end(),record.definition.begin());
	const auto handle=objects.add(record);
	if (!handle) environment.set_last_error(8);
	return handle;
}

uint64_t NativeDesktop::get_object(std::span<const uint64_t> args,bool ansi)
{
	if (const auto *font=objects.font(args[0])) {
		const size_t size=ansi ? 60 : 92;
		if (!args[2]) return size;
		const auto count=std::min(size_t(uint32_t(args[1])),size);
		if (!count) return 0;
		if (!ansi) {
			process.write_memory(args[2],std::span(font->definition).first(count));
			return count;
		}
		std::array<uint8_t,60> bytes{};
		std::copy_n(font->definition.begin(),28,bytes.begin());
		for (size_t i=0;i<32 && 28+i<count;++i) {
			const auto character=uint16_t(font->definition[28+i*2] | (uint16_t(font->definition[29+i*2])<<8));
			if (!character) break;
			if (character>127) { environment.set_last_error(120); return 0; }
			bytes[28+i]=uint8_t(character);
		}
		process.write_memory(args[2],std::span(bytes).first(count));
		return count;
	}
	if (const auto *brush=objects.brush(args[0])) {
		if (!args[2]) return 16;
		const auto count=std::min(size_t(uint32_t(args[1])),size_t(16)); if (!count) return 0;
		std::array<uint8_t,16> bytes{}; put(bytes,0,brush->hollow ? 1 : 0); put(bytes,4,brush->color);
		process.write_memory(args[2],std::span(bytes).first(count)); return count;
	}
	const auto *bitmap = objects.bitmap(args[0]);
	if (!bitmap)
		return 0;
	// BITMAP ends with the bits pointer, as wide as the program's pointers; DIBSECTION follows it with the header, the masks, the section
	// handle and its offset.
	const bool narrow = process.mode() == NativeMode::x86;
	const size_t pointer = narrow ? 4 : 8, plain = narrow ? 24 : 32, extended = narrow ? 84 : 104;
	if (!args[2])
		return plain;
	if (int32_t(uint32_t(args[1])) < int32_t(plain))
		return 0;
	std::vector<uint8_t> bytes(bitmap->dib_section && int32_t(uint32_t(args[1])) >= int32_t(extended) ? extended : plain);
	put(bytes, 4, bitmap->width);
	put(bytes, 8, bitmap->height);
	put(bytes, 12, bitmap->stride ? bitmap->stride : bitmap->address ? size_t(bitmap->width) * 4 : bitmap->bits.size() / bitmap->height);
	put(bytes, 16, 1, 2);
	put(bytes, 18, bitmap->depth, 2);
	put(bytes, plain - pointer, bitmap->dib_section ? bitmap->address : 0, pointer);
	if (bytes.size() == extended) {
		put(bytes, plain, 40);
		put(bytes, plain + 4, bitmap->width);
		put(bytes, plain + 8, bitmap->height);
		put(bytes, plain + 12, 1, 2);
		put(bytes, plain + 14, bitmap->depth, 2);
		put(bytes, plain + 20, size_t(bitmap->stride ? bitmap->stride : bitmap->width*4)*bitmap->height);
	}
	process.write_memory(args[2], bytes);
	return bytes.size();
}

uint64_t NativeDesktop::get_bits(std::span<const uint64_t> args)
{
	const auto *bitmap = objects.bitmap(args[0]);
	const auto count = int32_t(uint32_t(args[1]));
	if (!bitmap || count <= 0)
		return 0;
	const size_t size = std::min(size_t(count), bitmap->address ? size_t(bitmap->width) * bitmap->height * 4 : bitmap->bits.size());
	if (bitmap->address) {
		const auto view = bitmap->pixels();
		std::vector<uint8_t> pixels(view.begin(),view.begin()+static_cast<ptrdiff_t>(bitmap->top_down ? size : size_t(bitmap->width)*bitmap->height*4));
		if (!bitmap->top_down) {
			const size_t stride = size_t(bitmap->width)*4;
			for (size_t y = 0; y < bitmap->height/2; ++y)
				std::swap_ranges(pixels.begin()+static_cast<ptrdiff_t>(y*stride),pixels.begin()+static_cast<ptrdiff_t>((y+1)*stride),
					pixels.begin()+static_cast<ptrdiff_t>((bitmap->height-1-y)*stride));
		}
		process.write_memory(args[2], std::span(pixels).first(size));
	} else
		process.write_memory(args[2], std::span(bitmap->bits).first(size));
	return size;
}

uint64_t NativeDesktop::load_string(std::span<const uint64_t> args)
{
	const auto capacity = int32_t(uint32_t(args[3]));
	if (capacity < 0)
		throw std::runtime_error("negative LoadStringW capacity is not qualified");
	const uint8_t zero[] = {0, 0};
	if (capacity)
		process.write_memory(args[2], zero);
	const auto id = uint16_t(args[1]);
	const auto table = resources.view(args[0], 6, (id >> 4) + 1);
	if (!table)
		return 0;
	size_t offset = 0;
	uint16_t length = 0;
	for (unsigned index = 0; index <= (id & 15U); ++index) {
		if (offset > table->size || table->size - offset < 2)
			throw std::runtime_error("truncated string resource length");
		const auto word = process.read_memory(table->address + offset, 2);
		length = uint16_t(word[0] | uint16_t(word[1]) << 8);
		offset += 2;
		if (size_t(length) * 2 > table->size - offset)
			throw std::runtime_error("string resource exceeds table payload");
		if (index != (id & 15U))
			offset += size_t(length) * 2;
	}
	if (!capacity) {
		// The buffer holds one pointer as wide as the program's; a wider write overruns the caller's stack.
		std::array<uint8_t, 8> pointer{};
		const size_t width = process.mode() == NativeMode::x86 ? 4 : 8;
		put(pointer, 0, table->address + offset, width);
		process.write_memory(args[2], std::span(pointer).first(width));
		return length;
	}
	const size_t count = std::min(size_t(length), size_t(capacity - 1));
	auto bytes = process.read_memory(table->address + offset, count * 2);
	bytes.insert(bytes.end(), {0, 0});
	process.write_memory(args[2], bytes);
	return count;
}

bool NativeDesktop::dispatch(std::string_view name)
{
	if (color_dispatch(name)) return true;
	if (clip_dispatch(name)) return true;
	if (pen_dispatch(name)) return true;
	if (caps_dispatch(name)) return true;
	if (load_dispatch(name)) return true;
	if (extract_dispatch(name)) return true;
	if (menu_dispatch(name)) return true;
#ifdef RAVEN_SKIA
	if (text_dispatch(name)) return true;
#endif
	uint64_t result = 0;
	if (name == "LoadStringW") {
		result = load_string(process.api_arguments(4));
	} else if (name == "CreateIconFromResourceEx") {
		result = create(process.api_arguments(7));
	} else if (name == "LoadIconWithScaleDown") {
		result = load_icon(process.api_arguments(5));
	} else if (name == "LookupIconIdFromDirectoryEx") {
		result = lookup(process.api_arguments(5));
	} else if (name == "GetIconInfo") {
		const auto args = process.api_arguments(2);
		result = info(args[0], args[1]);
	} else if (name == "CopyIcon") {
		const auto handle = process.api_arguments(1)[0];
		if (const auto *icon = objects.icon(handle)) {
			auto copy = *icon;
			copy.shared = false;
			result = objects.add(std::move(copy));
			if (!result)
				environment.set_last_error(8);
		} else {
			environment.set_last_error(1402);
		}
	} else if (name == "DestroyIcon") {
		const auto handle = process.api_arguments(1)[0];
		if (const auto *icon = objects.icon(handle))
			result = icon->shared || objects.erase(handle);
	} else if (name == "DeleteObject") {
		const auto handle = process.api_arguments(1)[0];
		result = objects.pen(handle) ? objects.delete_pen(handle) : objects.region(handle) ? objects.erase(handle) : objects.brush(handle) ? objects.delete_brush(handle) : objects.font(handle) ? objects.delete_font(handle) : objects.dc(handle) ? delete_dc(handle) : delete_bitmap(handle);
	} else if (name == "CreateSolidBrush") {
		auto color=uint32_t(process.api_arguments(1)[0]);
		// A palette-relative color is the color itself on a display without a palette.
		if (color>>24==2) color&=0xffffff;
		if (color & 0xff000000) environment.set_last_error(120);
		else { result=objects.add(DesktopBrush{color}); if (!result) environment.set_last_error(8); }
	} else if (name == "FrameRect") {
		result=frame(process.api_arguments(3));
	} else if (name == "FillRect") {
		result=fill(process.api_arguments(3));
	} else if (name == "CreateFontIndirectW") {
		result=create_font(process.api_arguments(1)[0]);
	} else if (name == "GetStockObject") {
		const auto index=uint32_t(process.api_arguments(1)[0]);
		if (index<6) result=desktop_stock_brush+index;
		else if (index<=8) result=desktop_stock_pen+index-6;
		else if (index==13) result=desktop_system_font;
		else environment.set_last_error(120);
	} else if (name == "GetCurrentObject") {
		const auto args=process.api_arguments(2);
		if (const auto *dc=objects.dc(args[0])) {
			if (uint32_t(args[1])==1) result=dc->pen;
			else if (uint32_t(args[1])==2) result=dc->brush;
			else if (uint32_t(args[1])==6) result=dc->font;
			else if (uint32_t(args[1])==7 && !dc->window) result=dc->bitmap;
			else environment.set_last_error(120);
		} else environment.set_last_error(6);
	} else if (name == "CreateCompatibleDC") {
		result = create_dc(process.api_arguments(1)[0]);
	} else if (name == "CreateDCW") {
		// A display device context is created like a memory DC compatible with the screen; drawing on it stays off screen.
		const auto args = process.api_arguments(4);
		(void)args;
		result = create_dc(0);
	} else if (name == "DeleteDC") {
		result = delete_dc(process.api_arguments(1)[0]);
	} else if (name == "SelectObject") {
		const auto args = process.api_arguments(2);
		result = select(args[0],args[1]);
	} else if (name == "CreateCompatibleBitmap") {
		result = compatible_bitmap(process.api_arguments(3));
	} else if (name == "CreateDIBSection") {
		result = create_section(process.api_arguments(6));
	} else if (name == "BitBlt") {
		const auto args=process.api_arguments(9);
		try {
			const bool changed=bit_blt(process,objects,args);
			const auto *dc=objects.dc(args[0]);
			if (changed && dc->window && dc->leased && updated) updated(dc->window);
			result=1;
		} catch (uint32_t error) { environment.set_last_error(error); }
	} else if (name == "DrawIconEx") {
		result = draw(process.api_arguments(9));
	} else if (name == "DrawIcon") {
		// DrawIcon is DrawIconEx at the icon's own size with both the image and the mask.
		const auto args = process.api_arguments(4);
		const std::array<uint64_t, 9> extended{args[0], args[1], args[2], args[3], 0, 0, 0, 0, 3};
		result = draw(extended);
	} else if (name == "GetObjectA" || name == "GetObjectW") {
		result = get_object(process.api_arguments(3),name=="GetObjectA");
	} else if (name == "GetBitmapBits") {
		result = get_bits(process.api_arguments(3));
	} else {
		return false;
	}
	process.return_api(result);
	return true;
}

}
