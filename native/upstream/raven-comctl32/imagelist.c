/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <initguid.h>
#include <commoncontrols.h>

#undef ImageList_AddIcon

/* A list keeps its images in one column of a top-down DIB section, with a matching one-bit mask when it has one, so the
 * pixels can be read directly. A handle is the object's IImageList pointer, and every entry point checks it against the
 * table of live lists, since callers do pass stale handles. */

#define FIRST_ALLOCATION_LIMIT 256
#define OVERLAY_COUNT 15
#define PERSIST_MAGIC 0x4C49
#define PERSIST_VERSION 0x101
#define PERSIST_TILES 4
#define SOURCE_NOT_DEST 0x00220326

#pragma pack(push, 2)
typedef struct {
	USHORT magic, version;
	WORD count, capacity, grow, cx, cy;
	COLORREF background;
	WORD flags;
	SHORT overlays[4];
} persisted_head;
#pragma pack(pop)

typedef struct image_list {
	IImageList2 face;
	LONG references;
	struct image_list *next;
	UINT flags;
	int cx, cy, count, capacity, grow, bits_per_pixel;
	COLORREF background;
	int overlays[OVERLAY_COUNT];
	HBITMAP image, mask;
	HGDIOBJ image_old, mask_old;
	HDC image_dc, mask_dc;
	BYTE *image_bits, *mask_bits;
	int image_stride, mask_stride;
	BYTE *item_alpha;
} image_list;

static image_list *live_lists;
static volatile LONG live_guard;

static void lock_table(void)
{
	while (InterlockedCompareExchange(&live_guard, 1, 0))
		Sleep(0);
}

static void unlock_table(void)
{
	InterlockedExchange(&live_guard, 0);
}

static image_list *checked(HIMAGELIST handle)
{
	image_list *walk;
	lock_table();
	for (walk = live_lists; walk; walk = walk->next) {
		if ((HIMAGELIST)walk == handle)
			break;
	}
	unlock_table();
	return walk;
}

static BOOL has_mask(const image_list *list)
{
	return (list->flags & ILC_MASK) != 0;
}

static int depth_of_flags(UINT flags)
{
	UINT depth = flags & 0xFE;
	HDC screen;
	int result;
	switch (depth) {
	case ILC_COLOR4:
	case ILC_COLOR8:
	case ILC_COLOR16:
	case ILC_COLOR24:
	case ILC_COLOR32:
		return (int)depth;
	case ILC_COLORDDB:
		screen = GetDC(NULL);
		result = GetDeviceCaps(screen, BITSPIXEL);
		ReleaseDC(NULL, screen);
		return result >= 16 ? result : 8;
	default:
		return 4;
	}
}

/* Storage */

static void fill_standard_colors(RGBQUAD *table, int depth)
{
	static const BYTE vga[16][3] = {
		{ 0, 0, 0 }, { 128, 0, 0 }, { 0, 128, 0 }, { 128, 128, 0 }, { 0, 0, 128 }, { 128, 0, 128 }, { 0, 128, 128 }, { 192, 192, 192 },
		{ 128, 128, 128 }, { 255, 0, 0 }, { 0, 255, 0 }, { 255, 255, 0 }, { 0, 0, 255 }, { 255, 0, 255 }, { 0, 255, 255 }, { 255, 255, 255 },
	};
	int index;
	if (depth == 4) {
		for (index = 0; index < 16; index++) {
			table[index].rgbRed = vga[index][0];
			table[index].rgbGreen = vga[index][1];
			table[index].rgbBlue = vga[index][2];
			table[index].rgbReserved = 0;
		}
		return;
	}
	/* Eight bits: a colour cube, with the sixteen standard colours kept where every palette has them. */
	for (index = 0; index < 256; index++) {
		table[index].rgbRed = (BYTE)(((index >> 5) & 7) * 255 / 7);
		table[index].rgbGreen = (BYTE)(((index >> 2) & 7) * 255 / 7);
		table[index].rgbBlue = (BYTE)((index & 3) * 255 / 3);
		table[index].rgbReserved = 0;
	}
	for (index = 0; index < 8; index++) {
		table[index].rgbRed = vga[index][0];
		table[index].rgbGreen = vga[index][1];
		table[index].rgbBlue = vga[index][2];
		table[248 + index].rgbRed = vga[8 + index][0];
		table[248 + index].rgbGreen = vga[8 + index][1];
		table[248 + index].rgbBlue = vga[8 + index][2];
	}
}

static HBITMAP new_section(int width, int height, int depth, const RGBQUAD *colors, BYTE **bits)
{
	BYTE buffer[sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD)];
	BITMAPINFO *info = (BITMAPINFO *)buffer;
	ZeroMemory(buffer, sizeof(buffer));
	info->bmiHeader.biSize = sizeof(info->bmiHeader);
	info->bmiHeader.biWidth = width > 0 ? width : 1;
	info->bmiHeader.biHeight = -(height > 0 ? height : 1);
	info->bmiHeader.biPlanes = 1;
	info->bmiHeader.biBitCount = (WORD)depth;
	info->bmiHeader.biCompression = BI_RGB;
	if (colors)
		CopyMemory(info->bmiColors, colors, sizeof(RGBQUAD) << depth);
	return CreateDIBSection(NULL, info, DIB_RGB_COLORS, (void **)bits, NULL, 0);
}

static int stride_of(int width, int depth)
{
	return ((width > 0 ? width : 1) * depth + 31) / 32 * 4;
}

/* Replaces the two bitmaps with ones of the given capacity, keeping the images that fit. */
static BOOL set_capacity(image_list *list, int capacity)
{
	RGBQUAD colors[256];
	const RGBQUAD *table = NULL;
	HBITMAP image, mask = NULL;
	BYTE *image_bits, *mask_bits = NULL;
	BYTE *alpha;
	int kept;

	if (capacity < 1)
		capacity = 1;
	if (list->bits_per_pixel <= 8) {
		if (list->image_dc)
			GetDIBColorTable(list->image_dc, 0, 1u << list->bits_per_pixel, colors);
		else
			fill_standard_colors(colors, list->bits_per_pixel);
		table = colors;
	}
	image = new_section(list->cx, list->cy * capacity, list->bits_per_pixel, table, &image_bits);
	if (!image)
		return FALSE;
	if (has_mask(list)) {
		static const RGBQUAD pair[2] = { { 0, 0, 0, 0 }, { 255, 255, 255, 0 } };
		mask = new_section(list->cx, list->cy * capacity, 1, pair, &mask_bits);
		if (!mask) {
			DeleteObject(image);
			return FALSE;
		}
	}
	alpha = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, capacity);
	if (!alpha) {
		DeleteObject(image);
		if (mask)
			DeleteObject(mask);
		return FALSE;
	}
	kept = list->count < capacity ? list->count : capacity;
	if (list->image_bits && kept) {
		CopyMemory(image_bits, list->image_bits, (SIZE_T)list->image_stride * list->cy * kept);
		if (mask)
			CopyMemory(mask_bits, list->mask_bits, (SIZE_T)list->mask_stride * list->cy * kept);
		CopyMemory(alpha, list->item_alpha, kept);
	}
	if (list->image_dc) {
		SelectObject(list->image_dc, list->image_old);
		DeleteObject(list->image);
		HeapFree(GetProcessHeap(), 0, list->item_alpha);
	}
	else {
		list->image_dc = CreateCompatibleDC(NULL);
	}
	list->image = image;
	list->image_bits = image_bits;
	list->image_stride = stride_of(list->cx, list->bits_per_pixel);
	list->image_old = SelectObject(list->image_dc, image);
	list->item_alpha = alpha;
	if (has_mask(list)) {
		if (list->mask_dc) {
			SelectObject(list->mask_dc, list->mask_old);
			DeleteObject(list->mask);
		}
		else {
			list->mask_dc = CreateCompatibleDC(NULL);
		}
		list->mask = mask;
		list->mask_bits = mask_bits;
		list->mask_stride = stride_of(list->cx, 1);
		list->mask_old = SelectObject(list->mask_dc, mask);
		SetTextColor(list->mask_dc, RGB(0, 0, 0));
		SetBkColor(list->mask_dc, RGB(255, 255, 255));
	}
	list->capacity = capacity;
	return TRUE;
}

static BOOL make_room(image_list *list, int extra)
{
	int wanted = list->count + extra;
	if (wanted < list->capacity)
		return TRUE;
	return set_capacity(list, wanted + list->grow + 1);
}

static void clear_slot(image_list *list, int index, int slots)
{
	if (index < 0 || slots < 1)
		return;
	ZeroMemory(list->image_bits + (SIZE_T)list->image_stride * list->cy * index, (SIZE_T)list->image_stride * list->cy * slots);
	if (has_mask(list))
		ZeroMemory(list->mask_bits + (SIZE_T)list->mask_stride * list->cy * index, (SIZE_T)list->mask_stride * list->cy * slots);
	ZeroMemory(list->item_alpha + index, slots);
}

/* Taking pixels in */

static DWORD *pixels_of_bitmap(HBITMAP bitmap, int width, int height)
{
	BITMAPINFO info;
	DWORD *pixels = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)width * height * 4);
	HDC dc;
	if (!pixels)
		return NULL;
	ZeroMemory(&info, sizeof(info));
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	dc = GetDC(NULL);
	if (!GetDIBits(dc, bitmap, 0, height, pixels, &info, DIB_RGB_COLORS)) {
		HeapFree(GetProcessHeap(), 0, pixels);
		pixels = NULL;
	}
	ReleaseDC(NULL, dc);
	return pixels;
}

static BOOL any_alpha(const DWORD *pixels, int count)
{
	int index;
	for (index = 0; index < count; index++) {
		if (pixels[index] & 0xFF000000)
			return TRUE;
	}
	return FALSE;
}

static void set_mask_bit(image_list *list, int x, int y, BOOL transparent)
{
	BYTE *row = list->mask_bits + (SIZE_T)list->mask_stride * y;
	if (transparent)
		row[x / 8] |= (BYTE)(0x80 >> (x % 8));
	else
		row[x / 8] &= (BYTE)~(0x80 >> (x % 8));
}

static BOOL mask_bit(const image_list *list, int x, int y)
{
	return (list->mask_bits[(SIZE_T)list->mask_stride * y + x / 8] & (0x80 >> (x % 8))) != 0;
}

/* The first bitmap with a colour table of a still empty low-depth list gives it that table. */
static void adopt_colors(image_list *list, HDC source)
{
	RGBQUAD colors[256];
	UINT count;
	if (list->bits_per_pixel > 8 || list->count)
		return;
	count = GetDIBColorTable(source, 0, 256, colors);
	if (count)
		SetDIBColorTable(list->image_dc, 0, count < (1u << list->bits_per_pixel) ? count : (1u << list->bits_per_pixel), colors);
}

/* Copies a strip of images from a bitmap into consecutive slots. */
static void take_strip(image_list *list, int first, int slots, HBITMAP bitmap, HBITMAP mask, int width, int height)
{
	HDC source = CreateCompatibleDC(NULL);
	HGDIOBJ old = SelectObject(source, bitmap);
	int slot;
	DWORD *pixels = list->bits_per_pixel == 32 ? pixels_of_bitmap(bitmap, width, height) : NULL;

	adopt_colors(list, source);
	for (slot = 0; slot < slots; slot++) {
		int top = (first + slot) * list->cy, from = slot * list->cx;
		BOOL alpha = FALSE;
		if (pixels) {
			int y, x;
			for (y = 0; y < list->cy; y++) {
				DWORD *row = (DWORD *)(list->image_bits + (SIZE_T)list->image_stride * (top + y));
				for (x = 0; x < list->cx; x++) {
					int sx = from + x;
					row[x] = sx < width && y < height ? pixels[(SIZE_T)y * width + sx] : 0;
				}
				alpha = alpha || any_alpha(row, list->cx);
			}
		}
		else {
			BitBlt(list->image_dc, 0, top, list->cx, list->cy, source, from, 0, SRCCOPY);
		}
		list->item_alpha[first + slot] = (BYTE)alpha;
		if (has_mask(list)) {
			if (mask) {
				HDC mask_source = CreateCompatibleDC(NULL);
				HGDIOBJ mask_old = SelectObject(mask_source, mask);
				BitBlt(list->mask_dc, 0, top, list->cx, list->cy, mask_source, from, 0, SRCCOPY);
				SelectObject(mask_source, mask_old);
				DeleteDC(mask_source);
				if (!alpha)
					BitBlt(list->image_dc, 0, top, list->cx, list->cy, list->mask_dc, 0, top, SOURCE_NOT_DEST);
			}
			else {
				PatBlt(list->mask_dc, 0, top, list->cx, list->cy, BLACKNESS);
			}
		}
	}
	if (pixels)
		HeapFree(GetProcessHeap(), 0, pixels);
	SelectObject(source, old);
	DeleteDC(source);
}

static BOOL size_of_bitmap(HBITMAP bitmap, int *width, int *height)
{
	BITMAP info;
	if (!bitmap || !GetObjectW(bitmap, sizeof(info), &info))
		return FALSE;
	*width = info.bmWidth;
	*height = info.bmHeight;
	return TRUE;
}

static int slots_in(const image_list *list, int width)
{
	int slots = list->cx ? width / list->cx : 0;
	return slots > 0 ? slots : 1;
}

static int add_bitmap(image_list *list, HBITMAP bitmap, HBITMAP mask)
{
	int width, height, slots, first;
	if (!size_of_bitmap(bitmap, &width, &height))
		return -1;
	slots = slots_in(list, width);
	if (!make_room(list, slots))
		return -1;
	first = list->count;
	take_strip(list, first, slots, bitmap, mask, width, height);
	list->count += slots;
	return first;
}

static int add_masked(image_list *list, HBITMAP bitmap, COLORREF key)
{
	int width, height, slots, first, slot, x, y;
	DWORD *pixels;
	DWORD wanted;
	BOOL use_top_left = key == CLR_DEFAULT;

	if (!has_mask(list))
		return add_bitmap(list, bitmap, NULL);
	if (!size_of_bitmap(bitmap, &width, &height))
		return -1;
	slots = slots_in(list, width);
	if (!make_room(list, slots))
		return -1;
	pixels = pixels_of_bitmap(bitmap, width, height);
	if (!pixels)
		return -1;
	first = list->count;
	take_strip(list, first, slots, bitmap, NULL, width, height);
	wanted = use_top_left ? pixels[0] & 0xFFFFFF : (DWORD)(GetBValue(key) | (GetGValue(key) << 8) | (GetRValue(key) << 16));
	for (slot = 0; slot < slots; slot++) {
		int top = (first + slot) * list->cy;
		for (y = 0; y < list->cy && y < height; y++) {
			for (x = 0; x < list->cx && slot * list->cx + x < width; x++)
				set_mask_bit(list, x, top + y, (pixels[(SIZE_T)y * width + slot * list->cx + x] & 0xFFFFFF) == wanted);
		}
		if (!list->item_alpha[first + slot])
			BitBlt(list->image_dc, 0, top, list->cx, list->cy, list->mask_dc, 0, top, SOURCE_NOT_DEST);
	}
	HeapFree(GetProcessHeap(), 0, pixels);
	list->count += slots;
	return first;
}

static int store_icon(image_list *list, int index, HICON icon)
{
	ICONINFO info;
	int width, height, top;
	BOOL monochrome;

	if (!GetIconInfo(icon, &info))
		return -1;
	if (index < 0) {
		if (!make_room(list, 1)) {
			index = -1;
			goto done;
		}
		index = list->count++;
	}
	clear_slot(list, index, 1);
	top = index * list->cy;
	monochrome = !info.hbmColor;
	if (!size_of_bitmap(monochrome ? info.hbmMask : info.hbmColor, &width, &height)) {
		index = -1;
		goto done;
	}
	if (monochrome)
		height /= 2;
	{
		HDC source = CreateCompatibleDC(NULL);
		HGDIOBJ old = SelectObject(source, info.hbmMask);
		BOOL copied_alpha = FALSE;
		if (has_mask(list))
			StretchBlt(list->mask_dc, 0, top, list->cx, list->cy, source, 0, 0, width, height, SRCCOPY);
		if (monochrome) {
			SelectObject(source, info.hbmMask);
			StretchBlt(list->image_dc, 0, top, list->cx, list->cy, source, 0, height, width, height, SRCCOPY);
		}
		else {
			DWORD *pixels = list->bits_per_pixel == 32 && width == list->cx && height == list->cy ? pixels_of_bitmap(info.hbmColor, width, height) : NULL;
			if (pixels && any_alpha(pixels, width * height)) {
				int y;
				for (y = 0; y < height; y++)
					CopyMemory(list->image_bits + (SIZE_T)list->image_stride * (top + y), pixels + (SIZE_T)y * width, width * 4);
				list->item_alpha[index] = 1;
				copied_alpha = TRUE;
			}
			if (pixels)
				HeapFree(GetProcessHeap(), 0, pixels);
			if (!copied_alpha) {
				SelectObject(source, info.hbmColor);
				StretchBlt(list->image_dc, 0, top, list->cx, list->cy, source, 0, 0, width, height, SRCCOPY);
			}
		}
		SelectObject(source, old);
		DeleteDC(source);
		if (has_mask(list) && !copied_alpha)
			BitBlt(list->image_dc, 0, top, list->cx, list->cy, list->mask_dc, 0, top, SOURCE_NOT_DEST);
		/* A deep list without a mask keeps the outline of the icon in its alpha. */
		if (list->bits_per_pixel == 32 && !has_mask(list) && !copied_alpha && !monochrome) {
			DWORD *outline = pixels_of_bitmap(info.hbmMask, width, height);
			if (outline && width == list->cx && height == list->cy) {
				int y, x;
				for (y = 0; y < list->cy; y++) {
					DWORD *row = (DWORD *)(list->image_bits + (SIZE_T)list->image_stride * (top + y));
					for (x = 0; x < list->cx; x++) {
						if (outline[(SIZE_T)y * width + x] & 0xFFFFFF)
							row[x] = 0;
						else
							row[x] |= 0xFF000000;
					}
				}
				list->item_alpha[index] = 1;
			}
			if (outline)
				HeapFree(GetProcessHeap(), 0, outline);
		}
	}
done:
	if (info.hbmColor)
		DeleteObject(info.hbmColor);
	if (info.hbmMask)
		DeleteObject(info.hbmMask);
	return index;
}

/* Drawing */

static void color_channels(COLORREF color, int *red, int *green, int *blue)
{
	*red = GetRValue(color);
	*green = GetGValue(color);
	*blue = GetBValue(color);
}

/* Composes pixel by pixel, for everything GDI cannot express: alpha, grey, constant opacity and tinting. */
static void draw_pixels(const image_list *list, const IMAGELISTDRAWPARAMS *params, int source_x, int source_y, int width, int height)
{
	BYTE *target_bits, *source_bits = NULL;
	HBITMAP target, source = NULL;
	HDC dc = CreateCompatibleDC(NULL);
	HGDIOBJ old;
	const BOOL item_alpha = list->item_alpha[params->i] != 0;
	const BOOL grey = (params->fState & ILS_SATURATE) != 0;
	const BOOL faded = (params->fState & ILS_ALPHA) != 0;
	const BOOL keep_alpha = (params->fStyle & ILD_PRESERVEALPHA) && !grey && !faded;
	const int tint_share = (params->fStyle & ILD_BLEND50) ? 128 : (params->fStyle & ILD_BLEND25) ? 64 : 0;
	const COLORREF key = params->rgbBk == CLR_DEFAULT ? list->background : params->rgbBk;
	const BOOL over_key = !(params->fStyle & ILD_TRANSPARENT) && key != CLR_NONE;
	int tint_red = 0, tint_green = 0, tint_blue = 0, key_red, key_green, key_blue, x, y;

	if (tint_share)
		color_channels(params->rgbFg == CLR_DEFAULT || params->rgbFg == CLR_NONE ? GetSysColor(COLOR_HIGHLIGHT) : params->rgbFg, &tint_red, &tint_green, &tint_blue);
	color_channels(over_key ? key : 0, &key_red, &key_green, &key_blue);
	target = new_section(width, height, 32, NULL, &target_bits);
	if (!target) {
		DeleteDC(dc);
		return;
	}
	old = SelectObject(dc, target);
	BitBlt(dc, 0, 0, width, height, params->hdcDst, params->x, params->y, SRCCOPY);
	if (list->bits_per_pixel != 32) {
		HDC copy = CreateCompatibleDC(NULL);
		source = new_section(width, height, 32, NULL, &source_bits);
		if (source) {
			HGDIOBJ copy_old = SelectObject(copy, source);
			BitBlt(copy, 0, 0, width, height, list->image_dc, source_x, source_y, SRCCOPY);
			SelectObject(copy, copy_old);
		}
		DeleteDC(copy);
	}
	for (y = 0; y < height; y++) {
		DWORD *out = (DWORD *)(target_bits + (SIZE_T)y * width * 4);
		const DWORD *in = source_bits ? (const DWORD *)(source_bits + (SIZE_T)y * width * 4) : (const DWORD *)(list->image_bits + (SIZE_T)list->image_stride * (source_y + y)) + source_x;
		if (list->bits_per_pixel != 32 && !source_bits)
			break;
		for (x = 0; x < width; x++) {
			DWORD pixel = in[x], below = out[x];
			int alpha = item_alpha ? (int)(pixel >> 24) : 255;
			int red = (pixel >> 16) & 255, green = (pixel >> 8) & 255, blue = pixel & 255;
			int below_red = (below >> 16) & 255, below_green = (below >> 8) & 255, below_blue = below & 255, below_alpha = below >> 24;
			if (!item_alpha && has_mask(list) && mask_bit(list, source_x + x, source_y + y))
				alpha = 0;
			if (over_key) {
				below_red = key_red;
				below_green = key_green;
				below_blue = key_blue;
				below_alpha = 255;
			}
			if (grey) {
				red = green = blue = (red * 299 + green * 587 + blue * 114 + 500) / 1000;
				if (item_alpha)
					red = green = blue = red * alpha / 255;
			}
			if (tint_share && alpha) {
				red = (red * (256 - tint_share) + tint_red * tint_share) / 256;
				green = (green * (256 - tint_share) + tint_green * tint_share) / 256;
				blue = (blue * (256 - tint_share) + tint_blue * tint_share) / 256;
			}
			if (faded)
				alpha = alpha * (int)(params->Frame & 255) / 255;
			red = red * alpha / 255;
			green = green * alpha / 255;
			blue = blue * alpha / 255;
			if (keep_alpha) {
				out[x] = ((DWORD)alpha << 24) | ((DWORD)red << 16) | ((DWORD)green << 8) | (DWORD)blue;
			}
			else {
				int rest = 255 - alpha;
				out[x] = ((DWORD)(alpha + below_alpha * rest / 255) << 24) | ((DWORD)(red + below_red * rest / 255) << 16) | ((DWORD)(green + below_green * rest / 255) << 8) | (DWORD)(blue + below_blue * rest / 255);
			}
		}
	}
	BitBlt(params->hdcDst, params->x, params->y, width, height, dc, 0, 0, SRCCOPY);
	if (source)
		DeleteObject(source);
	SelectObject(dc, old);
	DeleteObject(target);
	DeleteDC(dc);
}

static void blit(HDC target, const IMAGELISTDRAWPARAMS *params, int width, int height, int scaled_width, int scaled_height, HDC source, int source_x, int source_y, DWORD operation)
{
	if (scaled_width != width || scaled_height != height)
		StretchBlt(target, params->x, params->y, scaled_width, scaled_height, source, source_x, source_y, width, height, operation);
	else
		BitBlt(target, params->x, params->y, width, height, source, source_x, source_y, operation);
}

static void draw_plain(const image_list *list, const IMAGELISTDRAWPARAMS *params, int source_x, int source_y, int width, int height, int scaled_width, int scaled_height)
{
	HDC target = params->hdcDst;
	const UINT style = params->fStyle;
	const COLORREF key = params->rgbBk == CLR_DEFAULT ? list->background : params->rgbBk;
	COLORREF old_text, old_back;

	if ((style & ILD_ROP) || !has_mask(list) || (style & ILD_IMAGE)) {
		if (!(style & ILD_MASK) || !has_mask(list)) {
			blit(target, params, width, height, scaled_width, scaled_height, list->image_dc, source_x, source_y, (style & ILD_ROP) ? params->dwRop : SRCCOPY);
			return;
		}
	}
	old_text = SetTextColor(target, RGB(0, 0, 0));
	old_back = SetBkColor(target, RGB(255, 255, 255));
	if (style & ILD_MASK) {
		blit(target, params, width, height, scaled_width, scaled_height, list->mask_dc, source_x, source_y, SRCCOPY);
	}
	else {
		if (!(style & ILD_TRANSPARENT) && key != CLR_NONE) {
			HBRUSH brush = CreateSolidBrush(key);
			RECT area;
			SetRect(&area, params->x, params->y, params->x + scaled_width, params->y + scaled_height);
			FillRect(target, &area, brush);
			DeleteObject(brush);
		}
		blit(target, params, width, height, scaled_width, scaled_height, list->mask_dc, source_x, source_y, SRCAND);
		blit(target, params, width, height, scaled_width, scaled_height, list->image_dc, source_x, source_y, SRCPAINT);
	}
	SetTextColor(target, old_text);
	SetBkColor(target, old_back);
}

static BOOL draw_item(const IMAGELISTDRAWPARAMS *given)
{
	IMAGELISTDRAWPARAMS params;
	const image_list *list;
	int source_x, source_y, width, height, overlay;
	BOOL scaled;

	if (!given || given->cbSize < IMAGELISTDRAWPARAMS_V3_SIZE)
		return FALSE;
	ZeroMemory(&params, sizeof(params));
	CopyMemory(&params, given, given->cbSize < sizeof(params) ? given->cbSize : sizeof(params));
	list = checked(params.himl);
	if (!list || !params.hdcDst || params.i < 0 || params.i >= list->count)
		return FALSE;
	source_x = params.xBitmap;
	source_y = params.i * list->cy + params.yBitmap;
	width = params.cx > 0 ? params.cx : list->cx;
	height = params.cy > 0 ? params.cy : list->cy;
	scaled = (params.fStyle & (ILD_SCALE | ILD_DPISCALE)) && (width != list->cx || height != list->cy);
	if (!scaled) {
		if (width > list->cx - params.xBitmap)
			width = list->cx - params.xBitmap;
		if (height > list->cy - params.yBitmap)
			height = list->cy - params.yBitmap;
	}
	if (width > 0 && height > 0) {
		BOOL by_pixel = !scaled && (list->item_alpha[params.i] || (params.fState & (ILS_SATURATE | ILS_ALPHA)) || (params.fStyle & (ILD_BLEND25 | ILD_BLEND50)));
		if (by_pixel)
			draw_pixels(list, &params, source_x, source_y, width, height);
		else if (scaled)
			draw_plain(list, &params, source_x, source_y, list->cx, list->cy, width, height);
		else
			draw_plain(list, &params, source_x, source_y, width, height, width, height);
	}
	overlay = (params.fStyle & ILD_OVERLAYMASK) >> 8;
	if (overlay >= 1 && overlay <= OVERLAY_COUNT && list->overlays[overlay - 1] >= 0) {
		IMAGELISTDRAWPARAMS top = params;
		top.i = list->overlays[overlay - 1];
		top.fStyle = (params.fStyle & ~(ILD_OVERLAYMASK | ILD_IMAGE)) | ILD_TRANSPARENT;
		top.xBitmap = top.yBitmap = 0;
		draw_item(&top);
	}
	return TRUE;
}

/* Lists */

static const IImageList2Vtbl face_table;

static image_list *new_list(int cx, int cy, UINT flags, int initial, int grow)
{
	image_list *list;
	int index;
	UINT depth = flags & 0xFE;

	if (cx < 0 || cy < 0)
		return NULL;
	if ((cx == 0 || cy == 0) && depth != ILC_COLORDDB)
		return NULL;
	list = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*list));
	if (!list)
		return NULL;
	list->face.lpVtbl = &face_table;
	list->references = 1;
	list->flags = flags;
	list->cx = cx;
	list->cy = cy;
	list->background = CLR_NONE;
	list->bits_per_pixel = depth_of_flags(flags);
	list->grow = ((grow > 1 ? grow : 1) + 3) & ~3;
	for (index = 0; index < OVERLAY_COUNT; index++)
		list->overlays[index] = -1;
	/* An enormous first size is only a hint: the column grows when the images really arrive. */
	if (!set_capacity(list, initial + 1 < 1 ? 1 : initial + 1 > FIRST_ALLOCATION_LIMIT ? FIRST_ALLOCATION_LIMIT : initial + 1)) {
		HeapFree(GetProcessHeap(), 0, list);
		return NULL;
	}
	lock_table();
	list->next = live_lists;
	live_lists = list;
	unlock_table();
	return list;
}

static void free_list(image_list *list)
{
	image_list **walk;
	lock_table();
	for (walk = &live_lists; *walk; walk = &(*walk)->next) {
		if (*walk == list) {
			*walk = list->next;
			break;
		}
	}
	unlock_table();
	SelectObject(list->image_dc, list->image_old);
	DeleteObject(list->image);
	DeleteDC(list->image_dc);
	if (list->mask_dc) {
		SelectObject(list->mask_dc, list->mask_old);
		DeleteObject(list->mask);
		DeleteDC(list->mask_dc);
	}
	HeapFree(GetProcessHeap(), 0, list->item_alpha);
	HeapFree(GetProcessHeap(), 0, list);
}

static BOOL remove_item(image_list *list, int index)
{
	int after;
	if (index == -1) {
		list->count = 0;
		return set_capacity(list, list->grow);
	}
	if (index < 0 || index >= list->count)
		return FALSE;
	after = list->count - index - 1;
	if (after) {
		MoveMemory(list->image_bits + (SIZE_T)list->image_stride * list->cy * index, list->image_bits + (SIZE_T)list->image_stride * list->cy * (index + 1), (SIZE_T)list->image_stride * list->cy * after);
		if (has_mask(list))
			MoveMemory(list->mask_bits + (SIZE_T)list->mask_stride * list->cy * index, list->mask_bits + (SIZE_T)list->mask_stride * list->cy * (index + 1), (SIZE_T)list->mask_stride * list->cy * after);
		MoveMemory(list->item_alpha + index, list->item_alpha + index + 1, after);
	}
	list->count--;
	return TRUE;
}

static void exchange_or_copy(image_list *list, int to, int from, BOOL exchange)
{
	const SIZE_T image_size = (SIZE_T)list->image_stride * list->cy, mask_size = (SIZE_T)list->mask_stride * list->cy;
	BYTE *one = list->image_bits + image_size * to, *two = list->image_bits + image_size * from;
	if (exchange) {
		SIZE_T at;
		BYTE flag = list->item_alpha[to];
		for (at = 0; at < image_size; at++) {
			BYTE swap = one[at];
			one[at] = two[at];
			two[at] = swap;
		}
		list->item_alpha[to] = list->item_alpha[from];
		list->item_alpha[from] = flag;
	}
	else {
		MoveMemory(one, two, image_size);
		list->item_alpha[to] = list->item_alpha[from];
	}
	if (has_mask(list)) {
		one = list->mask_bits + mask_size * to;
		two = list->mask_bits + mask_size * from;
		if (exchange) {
			SIZE_T at;
			for (at = 0; at < mask_size; at++) {
				BYTE swap = one[at];
				one[at] = two[at];
				two[at] = swap;
			}
		}
		else {
			MoveMemory(one, two, mask_size);
		}
	}
}

static image_list *clone_list(const image_list *list)
{
	image_list *copy = new_list(list->cx, list->cy, list->flags, list->count, list->grow);
	int index;
	if (!copy)
		return NULL;
	if (list->bits_per_pixel <= 8) {
		RGBQUAD colors[256];
		UINT count = GetDIBColorTable(list->image_dc, 0, 256, colors);
		if (count)
			SetDIBColorTable(copy->image_dc, 0, count, colors);
	}
	copy->count = list->count;
	copy->background = list->background;
	for (index = 0; index < OVERLAY_COUNT; index++)
		copy->overlays[index] = list->overlays[index];
	if (list->count) {
		CopyMemory(copy->image_bits, list->image_bits, (SIZE_T)list->image_stride * list->cy * list->count);
		if (has_mask(list))
			CopyMemory(copy->mask_bits, list->mask_bits, (SIZE_T)list->mask_stride * list->cy * list->count);
		CopyMemory(copy->item_alpha, list->item_alpha, list->count);
	}
	return copy;
}

static void paint_into(image_list *target, const image_list *source, int index, int x, int y)
{
	IMAGELISTDRAWPARAMS params;
	if (has_mask(source)) {
		BitBlt(target->mask_dc, x, y, source->cx, source->cy, source->mask_dc, 0, index * source->cy, SRCAND);
	}
	else {
		PatBlt(target->mask_dc, x, y, source->cx, source->cy, BLACKNESS);
	}
	ZeroMemory(&params, sizeof(params));
	params.cbSize = sizeof(params);
	params.himl = (HIMAGELIST)source;
	params.i = index;
	params.hdcDst = target->image_dc;
	params.x = x;
	params.y = y;
	params.rgbBk = CLR_NONE;
	params.rgbFg = CLR_NONE;
	params.fStyle = ILD_TRANSPARENT;
	draw_item(&params);
}

static image_list *merge_lists(const image_list *first, int first_index, const image_list *second, int second_index, int dx, int dy)
{
	image_list *merged;
	int cx, cy, x1 = dx < 0 ? -dx : 0, y1 = dy < 0 ? -dy : 0, x2 = dx > 0 ? dx : 0, y2 = dy > 0 ? dy : 0;
	int depth;
	UINT color_flag;

	cx = x1 + first->cx > x2 + second->cx ? x1 + first->cx : x2 + second->cx;
	cy = y1 + first->cy > y2 + second->cy ? y1 + first->cy : y2 + second->cy;
	depth = first->bits_per_pixel > second->bits_per_pixel ? first->bits_per_pixel : second->bits_per_pixel;
	/* A list of sixteen bits stays so even when merged with one that follows the screen. */
	if (first->bits_per_pixel == 16 && (second->flags & 0xFE) == ILC_COLORDDB)
		depth = 16;
	color_flag = (UINT)depth;
	merged = new_list(cx > 0 ? cx : 1, cy > 0 ? cy : 1, color_flag | ILC_MASK, 1, 1);
	if (!merged)
		return NULL;
	merged->count = 1;
	PatBlt(merged->mask_dc, 0, 0, merged->cx, merged->cy, WHITENESS);
	if (first_index >= 0)
		paint_into(merged, first, first_index, x1, y1);
	if (second_index >= 0)
		paint_into(merged, second, second_index, x2, y2);
	return merged;
}

/* Persistence: a header, then the images and the mask as bitmap files, four images to a row. */

static void copy_bits(const BYTE *from, int from_bit, BYTE *to, int to_bit, int bits)
{
	if (!(from_bit % 8) && !(to_bit % 8) && !(bits % 8)) {
		CopyMemory(to + to_bit / 8, from + from_bit / 8, bits / 8);
		return;
	}
	while (bits-- > 0) {
		BOOL value = (from[from_bit / 8] >> (7 - from_bit % 8)) & 1;
		if (value)
			to[to_bit / 8] |= (BYTE)(0x80 >> (to_bit % 8));
		else
			to[to_bit / 8] &= (BYTE)~(0x80 >> (to_bit % 8));
		from_bit++;
		to_bit++;
	}
}

static BOOL write_bitmap_file(IStream *stream, const image_list *list, BOOL mask)
{
	const int depth = mask ? 1 : list->bits_per_pixel;
	const int rows = (list->capacity + PERSIST_TILES - 1) / PERSIST_TILES;
	const int width = list->cx * PERSIST_TILES, height = rows * list->cy;
	const int stride = stride_of(width, depth);
	const int from_stride = mask ? list->mask_stride : list->image_stride;
	const BYTE *from = mask ? list->mask_bits : list->image_bits;
	const SIZE_T size = (SIZE_T)stride * height;
	BYTE head[sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD)];
	BITMAPFILEHEADER *file = (BITMAPFILEHEADER *)head;
	BITMAPINFOHEADER *info = (BITMAPINFOHEADER *)(file + 1);
	RGBQUAD *table = (RGBQUAD *)(info + 1);
	const int colors = depth <= 8 ? 1 << depth : 0;
	const ULONG head_size = sizeof(*file) + sizeof(*info) + colors * sizeof(RGBQUAD);
	BYTE *data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1);
	int index;
	HRESULT result;

	if (!data)
		return FALSE;
	ZeroMemory(head, sizeof(head));
	file->bfType = 0x4D42;
	file->bfSize = head_size;
	file->bfOffBits = head_size;
	info->biSize = sizeof(*info);
	info->biWidth = width;
	info->biHeight = height;
	info->biPlanes = 1;
	info->biBitCount = (WORD)depth;
	info->biCompression = BI_RGB;
	info->biSizeImage = (DWORD)size;
	if (mask) {
		table[1].rgbRed = table[1].rgbGreen = table[1].rgbBlue = 255;
	}
	else if (colors) {
		GetDIBColorTable(list->image_dc, 0, colors, table);
	}
	/* The file runs bottom up, and the images in it left to right from the top. */
	for (index = 0; index < list->count; index++) {
		int line;
		for (line = 0; line < list->cy; line++) {
			int file_row = height - 1 - ((index / PERSIST_TILES) * list->cy + line);
			copy_bits(from + (SIZE_T)from_stride * (index * list->cy + line), 0, data + (SIZE_T)stride * file_row, (index % PERSIST_TILES) * list->cx * depth, list->cx * depth);
		}
	}
	result = IStream_Write(stream, head, head_size, NULL);
	if (SUCCEEDED(result))
		result = IStream_Write(stream, data, (ULONG)size, NULL);
	HeapFree(GetProcessHeap(), 0, data);
	return SUCCEEDED(result);
}

static BOOL write_list(const image_list *list, IStream *stream)
{
	persisted_head head;
	int index;
	ZeroMemory(&head, sizeof(head));
	head.magic = PERSIST_MAGIC;
	head.version = PERSIST_VERSION;
	head.count = (WORD)list->count;
	head.capacity = (WORD)list->capacity;
	head.grow = (WORD)list->grow;
	head.cx = (WORD)list->cx;
	head.cy = (WORD)list->cy;
	head.background = list->background;
	head.flags = (WORD)list->flags;
	for (index = 0; index < 4; index++)
		head.overlays[index] = (SHORT)list->overlays[index];
	if (FAILED(IStream_Write(stream, &head, sizeof(head), NULL)))
		return FALSE;
	if (!write_bitmap_file(stream, list, FALSE))
		return FALSE;
	return !has_mask(list) || write_bitmap_file(stream, list, TRUE);
}

static BOOL read_exact(IStream *stream, void *buffer, ULONG size)
{
	ULONG got = 0;
	return SUCCEEDED(IStream_Read(stream, buffer, size, &got)) && got == size;
}

static BOOL read_bitmap_file(IStream *stream, image_list *list, BOOL mask, int tiles)
{
	BITMAPFILEHEADER file;
	BITMAPINFOHEADER info;
	RGBQUAD table[256];
	const int depth = mask ? 1 : list->bits_per_pixel;
	int colors, stride, height, index;
	BYTE *data;
	BOOL bottom_up;
	const int to_stride = mask ? list->mask_stride : list->image_stride;
	BYTE *to = mask ? list->mask_bits : list->image_bits;

	if (!read_exact(stream, &file, sizeof(file)) || !read_exact(stream, &info, sizeof(info)) || file.bfType != 0x4D42)
		return FALSE;
	if (info.biBitCount != depth)
		return FALSE;
	colors = depth <= 8 ? 1 << depth : 0;
	if (colors) {
		if (!read_exact(stream, table, colors * sizeof(RGBQUAD)))
			return FALSE;
		if (!mask)
			SetDIBColorTable(list->image_dc, 0, colors, table);
	}
	bottom_up = info.biHeight > 0;
	height = bottom_up ? info.biHeight : -info.biHeight;
	stride = stride_of(info.biWidth, depth);
	data = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)stride * height + 1);
	if (!data)
		return FALSE;
	if (!read_exact(stream, data, (ULONG)((SIZE_T)stride * height))) {
		HeapFree(GetProcessHeap(), 0, data);
		return FALSE;
	}
	for (index = 0; index < list->count; index++) {
		int line;
		for (line = 0; line < list->cy; line++) {
			int file_row = (index / tiles) * list->cy + line;
			if (file_row >= height)
				continue;
			if (bottom_up)
				file_row = height - 1 - file_row;
			copy_bits(data + (SIZE_T)stride * file_row, (index % tiles) * list->cx * depth, to + (SIZE_T)to_stride * (index * list->cy + line), 0, list->cx * depth);
		}
		if (!mask && depth == 32)
			list->item_alpha[index] = (BYTE)any_alpha((const DWORD *)(list->image_bits + (SIZE_T)list->image_stride * list->cy * index), list->cx * list->cy);
	}
	HeapFree(GetProcessHeap(), 0, data);
	return TRUE;
}

static image_list *read_list(IStream *stream)
{
	persisted_head head;
	image_list *list;
	int index, tiles;

	if (!read_exact(stream, &head, sizeof(head)) || head.magic != PERSIST_MAGIC)
		return NULL;
	if (head.version != PERSIST_VERSION && (head.version & 0xFF00) != 0x600)
		return NULL;
	tiles = head.version == PERSIST_VERSION ? PERSIST_TILES : 1;
	list = new_list(head.cx, head.cy, head.flags, head.capacity > 0 ? head.capacity - 1 : 0, head.grow);
	if (!list)
		return NULL;
	if (head.count >= list->capacity && !set_capacity(list, head.count + 1)) {
		free_list(list);
		return NULL;
	}
	list->count = head.count;
	list->background = head.background;
	for (index = 0; index < 4; index++)
		list->overlays[index] = head.overlays[index];
	if (!read_bitmap_file(stream, list, FALSE, tiles) || (has_mask(list) && !read_bitmap_file(stream, list, TRUE, tiles))) {
		free_list(list);
		return NULL;
	}
	return list;
}

/* Dragging */

static struct {
	image_list *image;
	POINT hotspot, position;
	HWND window;
	BOOL shown;
	HBITMAP under;
	HDC under_dc;
	HGDIOBJ under_old;
	int under_width, under_height;
} drag;

static HDC drag_dc(void)
{
	return GetDCEx(drag.window, NULL, DCX_WINDOW | DCX_CACHE | DCX_LOCKWINDOWUPDATE);
}

static void drag_restore(HDC dc)
{
	if (drag.under_dc)
		BitBlt(dc, drag.position.x - drag.hotspot.x, drag.position.y - drag.hotspot.y, drag.under_width, drag.under_height, drag.under_dc, 0, 0, SRCCOPY);
}

static void drag_paint(HDC dc)
{
	IMAGELISTDRAWPARAMS params;
	int x = drag.position.x - drag.hotspot.x, y = drag.position.y - drag.hotspot.y;
	if (!drag.under_dc || drag.under_width != drag.image->cx || drag.under_height != drag.image->cy) {
		if (drag.under_dc) {
			SelectObject(drag.under_dc, drag.under_old);
			DeleteObject(drag.under);
			DeleteDC(drag.under_dc);
		}
		drag.under_dc = CreateCompatibleDC(dc);
		drag.under = CreateCompatibleBitmap(dc, drag.image->cx, drag.image->cy);
		drag.under_old = SelectObject(drag.under_dc, drag.under);
		drag.under_width = drag.image->cx;
		drag.under_height = drag.image->cy;
	}
	BitBlt(drag.under_dc, 0, 0, drag.under_width, drag.under_height, dc, x, y, SRCCOPY);
	ZeroMemory(&params, sizeof(params));
	params.cbSize = sizeof(params);
	params.himl = (HIMAGELIST)drag.image;
	params.hdcDst = dc;
	params.x = x;
	params.y = y;
	params.rgbBk = CLR_NONE;
	params.rgbFg = CLR_NONE;
	params.fStyle = ILD_TRANSPARENT;
	draw_item(&params);
}

static void drag_set_shown(BOOL shown)
{
	HDC dc;
	if (!drag.image || drag.shown == shown)
		return;
	dc = drag_dc();
	if (dc) {
		if (shown)
			drag_paint(dc);
		else
			drag_restore(dc);
		ReleaseDC(drag.window, dc);
	}
	drag.shown = shown;
}

static void drag_free_under(void)
{
	if (drag.under_dc) {
		SelectObject(drag.under_dc, drag.under_old);
		DeleteObject(drag.under);
		DeleteDC(drag.under_dc);
		drag.under_dc = NULL;
	}
}

/* Flat entry points */

HIMAGELIST WINAPI ImageList_Create(INT cx, INT cy, UINT flags, INT initial, INT grow)
{
	return (HIMAGELIST)new_list(cx, cy, flags, initial, grow);
}

BOOL WINAPI ImageList_Destroy(HIMAGELIST handle)
{
	image_list *list = checked(handle);
	if (!list)
		return FALSE;
	IImageList2_Release(&list->face);
	return TRUE;
}

INT WINAPI ImageList_Add(HIMAGELIST handle, HBITMAP bitmap, HBITMAP mask)
{
	image_list *list = checked(handle);
	return list ? add_bitmap(list, bitmap, mask) : -1;
}

INT WINAPI ImageList_AddMasked(HIMAGELIST handle, HBITMAP bitmap, COLORREF key)
{
	image_list *list = checked(handle);
	return list ? add_masked(list, bitmap, key) : -1;
}

INT WINAPI ImageList_AddIcon(HIMAGELIST handle, HICON icon)
{
	return ImageList_ReplaceIcon(handle, -1, icon);
}

INT WINAPI ImageList_ReplaceIcon(HIMAGELIST handle, INT index, HICON icon)
{
	image_list *list = checked(handle);
	if (!list || index < -1 || index >= list->count)
		return -1;
	return store_icon(list, index, icon);
}

BOOL WINAPI ImageList_Replace(HIMAGELIST handle, INT index, HBITMAP bitmap, HBITMAP mask)
{
	image_list *list = checked(handle);
	int width, height;
	if (!list || index < 0 || index >= list->count || !size_of_bitmap(bitmap, &width, &height))
		return FALSE;
	take_strip(list, index, 1, bitmap, mask, width, height);
	return TRUE;
}

BOOL WINAPI ImageList_Remove(HIMAGELIST handle, INT index)
{
	image_list *list = checked(handle);
	return list && remove_item(list, index);
}

INT WINAPI ImageList_GetImageCount(HIMAGELIST handle)
{
	const image_list *list = checked(handle);
	return list ? list->count : 0;
}

BOOL WINAPI ImageList_SetImageCount(HIMAGELIST handle, UINT count)
{
	image_list *list = checked(handle);
	int old;
	if (!list)
		return FALSE;
	old = list->count;
	if ((int)count >= list->capacity && !set_capacity(list, (int)count + list->grow + 1))
		return FALSE;
	list->count = (int)count;
	if ((int)count > old)
		clear_slot(list, old, (int)count - old);
	return TRUE;
}

BOOL WINAPI ImageList_GetIconSize(HIMAGELIST handle, INT *cx, INT *cy)
{
	const image_list *list = checked(handle);
	if (!list || !cx || !cy)
		return FALSE;
	*cx = list->cx;
	*cy = list->cy;
	return TRUE;
}

BOOL WINAPI ImageList_SetIconSize(HIMAGELIST handle, INT cx, INT cy)
{
	image_list *list = checked(handle);
	if (!list || cx < 0 || cy < 0)
		return FALSE;
	list->cx = cx;
	list->cy = cy;
	list->count = 0;
	return set_capacity(list, list->grow);
}

BOOL WINAPI ImageList_GetImageInfo(HIMAGELIST handle, INT index, IMAGEINFO *info)
{
	const image_list *list = checked(handle);
	if (!list || !info || index < 0 || index >= list->count)
		return FALSE;
	info->hbmImage = list->image;
	info->hbmMask = list->mask;
	SetRect(&info->rcImage, 0, index * list->cy, list->cx, (index + 1) * list->cy);
	return TRUE;
}

BOOL WINAPI ImageList_GetImageRect(HIMAGELIST handle, INT index, RECT *area)
{
	const image_list *list = checked(handle);
	if (!list || !area || index < 0 || index >= list->count)
		return FALSE;
	SetRect(area, 0, index * list->cy, list->cx, (index + 1) * list->cy);
	return TRUE;
}

COLORREF WINAPI ImageList_GetBkColor(HIMAGELIST handle)
{
	const image_list *list = checked(handle);
	return list ? list->background : CLR_NONE;
}

COLORREF WINAPI ImageList_SetBkColor(HIMAGELIST handle, COLORREF color)
{
	image_list *list = checked(handle);
	COLORREF old;
	if (!list)
		return CLR_NONE;
	old = list->background;
	list->background = color;
	return old;
}

DWORD WINAPI ImageList_GetFlags(HIMAGELIST handle)
{
	const image_list *list = checked(handle);
	return list ? list->flags & (ILC_MASK | 0xFE) : 0;
}

DWORD WINAPI ImageList_SetFlags(HIMAGELIST handle, DWORD flags)
{
	return 0;
}

BOOL WINAPI ImageList_SetFilter(HIMAGELIST handle, INT index, DWORD filter)
{
	return FALSE;
}

BOOL WINAPI ImageList_SetOverlayImage(HIMAGELIST handle, INT index, INT overlay)
{
	image_list *list = checked(handle);
	if (!list || index < 0 || index >= list->count || overlay < 1 || overlay > OVERLAY_COUNT)
		return FALSE;
	list->overlays[overlay - 1] = index;
	return TRUE;
}

UINT WINAPI ImageList_SetColorTable(HIMAGELIST handle, UINT first, UINT count, const RGBQUAD *colors)
{
	const image_list *list = checked(handle);
	return list ? SetDIBColorTable(list->image_dc, first, count, colors) : 0;
}

BOOL WINAPI ImageList_Copy(HIMAGELIST target, INT to, HIMAGELIST source, INT from, UINT flags)
{
	image_list *list = checked(target);
	if (!list || target != source || to < 0 || to >= list->count || from < 0 || from >= list->count)
		return FALSE;
	exchange_or_copy(list, to, from, (flags & ILCF_SWAP) != 0);
	return TRUE;
}

BOOL WINAPI ImageList_DrawIndirect(IMAGELISTDRAWPARAMS *params)
{
	return draw_item(params);
}

BOOL WINAPI ImageList_DrawEx(HIMAGELIST handle, INT index, HDC dc, INT x, INT y, INT cx, INT cy, COLORREF background, COLORREF foreground, UINT style)
{
	IMAGELISTDRAWPARAMS params;
	ZeroMemory(&params, sizeof(params));
	params.cbSize = sizeof(params);
	params.himl = handle;
	params.i = index;
	params.hdcDst = dc;
	params.x = x;
	params.y = y;
	params.cx = cx;
	params.cy = cy;
	params.rgbBk = background;
	params.rgbFg = foreground;
	params.fStyle = style;
	return draw_item(&params);
}

BOOL WINAPI ImageList_Draw(HIMAGELIST handle, INT index, HDC dc, INT x, INT y, UINT style)
{
	return ImageList_DrawEx(handle, index, dc, x, y, 0, 0, CLR_DEFAULT, CLR_DEFAULT, style);
}

HICON WINAPI ImageList_GetIcon(HIMAGELIST handle, INT index, UINT style)
{
	const image_list *list = checked(handle);
	ICONINFO info;
	BYTE *color_bits, *mask_bits;
	HICON icon = NULL;
	HDC dc;
	HGDIOBJ old;
	IMAGELISTDRAWPARAMS params;
	static const RGBQUAD pair[2] = { { 0, 0, 0, 0 }, { 255, 255, 255, 0 } };

	if (!list || index < 0 || index >= list->count)
		return NULL;
	ZeroMemory(&info, sizeof(info));
	info.fIcon = TRUE;
	info.hbmColor = new_section(list->cx, list->cy, 32, NULL, &color_bits);
	info.hbmMask = new_section(list->cx, list->cy, 1, pair, &mask_bits);
	if (info.hbmColor && info.hbmMask) {
		int y;
		if (list->bits_per_pixel == 32) {
			for (y = 0; y < list->cy; y++)
				CopyMemory(color_bits + (SIZE_T)y * list->cx * 4, list->image_bits + (SIZE_T)list->image_stride * (index * list->cy + y), list->cx * 4);
		}
		else {
			dc = CreateCompatibleDC(NULL);
			old = SelectObject(dc, info.hbmColor);
			ZeroMemory(&params, sizeof(params));
			params.cbSize = sizeof(params);
			params.himl = handle;
			params.i = index;
			params.hdcDst = dc;
			params.rgbBk = CLR_NONE;
			params.rgbFg = CLR_NONE;
			params.fStyle = ILD_IMAGE;
			draw_item(&params);
			SelectObject(dc, old);
			DeleteDC(dc);
		}
		if (has_mask(list)) {
			for (y = 0; y < list->cy; y++)
				CopyMemory(mask_bits + (SIZE_T)list->mask_stride * y, list->mask_bits + (SIZE_T)list->mask_stride * (index * list->cy + y), list->mask_stride);
		}
		icon = CreateIconIndirect(&info);
	}
	if (info.hbmColor)
		DeleteObject(info.hbmColor);
	if (info.hbmMask)
		DeleteObject(info.hbmMask);
	return icon;
}

HIMAGELIST WINAPI ImageList_Duplicate(HIMAGELIST handle)
{
	const image_list *list = checked(handle);
	return list ? (HIMAGELIST)clone_list(list) : NULL;
}

HIMAGELIST WINAPI ImageList_Merge(HIMAGELIST first, INT first_index, HIMAGELIST second, INT second_index, INT dx, INT dy)
{
	const image_list *one = checked(first), *two = checked(second);
	if (!one || !two || first_index >= one->count || second_index >= two->count || first_index < 0 || second_index < 0)
		return NULL;
	return (HIMAGELIST)merge_lists(one, first_index, two, second_index, dx, dy);
}

static HIMAGELIST load_from_resource(HINSTANCE instance, LPCWSTR name, INT cx, INT grow, COLORREF key, UINT type, UINT load_flags)
{
	HANDLE loaded = LoadImageW(instance, name, type, 0, 0, load_flags | (type == IMAGE_BITMAP ? LR_CREATEDIBSECTION : 0));
	image_list *list = NULL;
	if (!loaded)
		return NULL;
	if (type == IMAGE_BITMAP) {
		BITMAP info;
		if (GetObjectW(loaded, sizeof(info), &info)) {
			int width = cx > 0 ? cx : info.bmHeight;
			UINT flags = (key != CLR_NONE ? ILC_MASK : 0) | (info.bmBitsPixel <= 4 ? ILC_COLOR4 : info.bmBitsPixel <= 8 ? ILC_COLOR8 : (UINT)info.bmBitsPixel);
			list = new_list(width, info.bmHeight, flags, width ? info.bmWidth / width : 1, grow);
			if (list) {
				if (key != CLR_NONE)
					add_masked(list, (HBITMAP)loaded, key);
				else
					add_bitmap(list, (HBITMAP)loaded, NULL);
			}
		}
		DeleteObject(loaded);
	}
	else if (type == IMAGE_ICON || type == IMAGE_CURSOR) {
		ICONINFO info;
		if (GetIconInfo((HICON)loaded, &info)) {
			BITMAP size;
			if (GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(size), &size)) {
				int height = info.hbmColor ? size.bmHeight : size.bmHeight / 2;
				list = new_list(size.bmWidth, height, ILC_MASK | ILC_COLOR32, 1, grow);
				if (list)
					store_icon(list, -1, (HICON)loaded);
			}
			if (info.hbmColor)
				DeleteObject(info.hbmColor);
			if (info.hbmMask)
				DeleteObject(info.hbmMask);
		}
		DestroyIcon((HICON)loaded);
	}
	return (HIMAGELIST)list;
}

HIMAGELIST WINAPI ImageList_LoadImageW(HINSTANCE instance, LPCWSTR name, INT cx, INT grow, COLORREF key, UINT type, UINT load_flags)
{
	return load_from_resource(instance, name, cx, grow, key, type, load_flags);
}

HIMAGELIST WINAPI ImageList_LoadImageA(HINSTANCE instance, LPCSTR name, INT cx, INT grow, COLORREF key, UINT type, UINT load_flags)
{
	WCHAR wide[MAX_PATH];
	if (IS_INTRESOURCE(name))
		return load_from_resource(instance, (LPCWSTR)name, cx, grow, key, type, load_flags);
	if (!MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH))
		return NULL;
	return load_from_resource(instance, wide, cx, grow, key, type, load_flags);
}

HIMAGELIST WINAPI ImageList_Read(IStream *stream)
{
	return stream ? (HIMAGELIST)read_list(stream) : NULL;
}

BOOL WINAPI ImageList_Write(HIMAGELIST handle, IStream *stream)
{
	const image_list *list = checked(handle);
	return list && stream && write_list(list, stream);
}

BOOL WINAPI ImageList_BeginDrag(HIMAGELIST handle, INT index, INT hot_x, INT hot_y)
{
	const image_list *list = checked(handle);
	image_list *image;
	if (!list || drag.image || index >= list->count)
		return FALSE;
	image = new_list(list->cx, list->cy, list->flags, 1, 1);
	if (!image)
		return FALSE;
	image->count = 1;
	if (index >= 0) {
		CopyMemory(image->image_bits, list->image_bits + (SIZE_T)list->image_stride * list->cy * index, (SIZE_T)list->image_stride * list->cy);
		if (has_mask(list))
			CopyMemory(image->mask_bits, list->mask_bits + (SIZE_T)list->mask_stride * list->cy * index, (SIZE_T)list->mask_stride * list->cy);
		image->item_alpha[0] = list->item_alpha[index];
	}
	drag.image = image;
	drag.hotspot.x = hot_x;
	drag.hotspot.y = hot_y;
	drag.position.x = drag.position.y = 0;
	drag.window = NULL;
	drag.shown = FALSE;
	return TRUE;
}

VOID WINAPI ImageList_EndDrag(void)
{
	drag_set_shown(FALSE);
	drag_free_under();
	if (drag.image)
		ImageList_Destroy((HIMAGELIST)drag.image);
	drag.image = NULL;
	drag.window = NULL;
}

BOOL WINAPI ImageList_DragEnter(HWND window, INT x, INT y)
{
	if (!drag.image)
		return FALSE;
	drag_set_shown(FALSE);
	drag.window = window;
	drag.position.x = x;
	drag.position.y = y;
	drag_set_shown(TRUE);
	return TRUE;
}

BOOL WINAPI ImageList_DragLeave(HWND window)
{
	if (!drag.image)
		return FALSE;
	drag_set_shown(FALSE);
	drag.window = NULL;
	return TRUE;
}

BOOL WINAPI ImageList_DragMove(INT x, INT y)
{
	BOOL was_shown = drag.shown;
	if (!drag.image)
		return FALSE;
	drag_set_shown(FALSE);
	drag.position.x = x;
	drag.position.y = y;
	if (was_shown)
		drag_set_shown(TRUE);
	return TRUE;
}

BOOL WINAPI ImageList_DragShowNolock(BOOL show)
{
	if (!drag.image)
		return FALSE;
	drag_set_shown(show);
	return TRUE;
}

BOOL WINAPI ImageList_SetDragCursorImage(HIMAGELIST handle, INT index, INT dx, INT dy)
{
	const image_list *cursor = checked(handle);
	image_list *merged;
	BOOL was_shown = drag.shown;
	if (!cursor || !drag.image || index < 0 || index >= cursor->count)
		return FALSE;
	merged = merge_lists(drag.image, 0, cursor, index, dx, dy);
	if (!merged)
		return FALSE;
	drag_set_shown(FALSE);
	ImageList_Destroy((HIMAGELIST)drag.image);
	drag.image = merged;
	if (was_shown)
		drag_set_shown(TRUE);
	return TRUE;
}

HIMAGELIST WINAPI ImageList_GetDragImage(POINT *position, POINT *hotspot)
{
	if (!drag.image)
		return NULL;
	if (position)
		*position = drag.position;
	if (hotspot)
		*hotspot = drag.hotspot;
	return (HIMAGELIST)drag.image;
}

/* The COM face */

static BOOL same_guid(const GUID *one, const GUID *two)
{
	const BYTE *a = (const BYTE *)one, *b = (const BYTE *)two;
	int index;
	for (index = 0; index < (int)sizeof(GUID); index++) {
		if (a[index] != b[index])
			return FALSE;
	}
	return TRUE;
}

static image_list *of_face(IImageList2 *face)
{
	return CONTAINING_RECORD(face, image_list, face);
}

static HRESULT WINAPI face_QueryInterface(IImageList2 *face, REFIID iid, void **object)
{
	if (!object)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IImageList) || same_guid(iid, &IID_IImageList2)) {
		*object = face;
		IImageList2_AddRef(face);
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI face_AddRef(IImageList2 *face)
{
	return (ULONG)InterlockedIncrement(&of_face(face)->references);
}

static ULONG WINAPI face_Release(IImageList2 *face)
{
	LONG left = InterlockedDecrement(&of_face(face)->references);
	if (!left)
		free_list(of_face(face));
	return (ULONG)left;
}

static HRESULT WINAPI face_Add(IImageList2 *face, HBITMAP image, HBITMAP mask, int *index)
{
	int result = add_bitmap(of_face(face), image, mask);
	if (index)
		*index = result;
	return result >= 0 ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_ReplaceIcon(IImageList2 *face, int at, HICON icon, int *index)
{
	int result = ImageList_ReplaceIcon((HIMAGELIST)face, at, icon);
	if (index)
		*index = result;
	return result >= 0 ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_SetOverlayImage(IImageList2 *face, int image, int overlay)
{
	return ImageList_SetOverlayImage((HIMAGELIST)face, image, overlay) ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_Replace(IImageList2 *face, int at, HBITMAP image, HBITMAP mask)
{
	return ImageList_Replace((HIMAGELIST)face, at, image, mask) ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_AddMasked(IImageList2 *face, HBITMAP image, COLORREF key, int *index)
{
	int result = add_masked(of_face(face), image, key);
	if (index)
		*index = result;
	return result >= 0 ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_Draw(IImageList2 *face, IMAGELISTDRAWPARAMS *params)
{
	IMAGELISTDRAWPARAMS copy;
	if (!params)
		return E_INVALIDARG;
	copy = *params;
	copy.himl = (HIMAGELIST)face;
	return draw_item(&copy) ? S_OK : E_INVALIDARG;
}

static HRESULT WINAPI face_Remove(IImageList2 *face, int at)
{
	return remove_item(of_face(face), at) ? S_OK : E_INVALIDARG;
}

static HRESULT WINAPI face_GetIcon(IImageList2 *face, int at, UINT flags, HICON *icon)
{
	if (!icon)
		return E_INVALIDARG;
	*icon = ImageList_GetIcon((HIMAGELIST)face, at, flags);
	return *icon ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_GetImageInfo(IImageList2 *face, int at, IMAGEINFO *info)
{
	return ImageList_GetImageInfo((HIMAGELIST)face, at, info) ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_Copy(IImageList2 *face, int to, IUnknown *source, int from, UINT flags)
{
	IImageList *other = NULL;
	HRESULT result;
	if (!source)
		return E_FAIL;
	result = IUnknown_QueryInterface(source, &IID_IImageList, (void **)&other);
	if (FAILED(result))
		return E_FAIL;
	result = ImageList_Copy((HIMAGELIST)face, to, (HIMAGELIST)other, from, flags) ? S_OK : E_FAIL;
	IImageList_Release(other);
	return result;
}

static HRESULT WINAPI face_Merge(IImageList2 *face, int first, IUnknown *second, int second_index, int dx, int dy, REFIID iid, void **object)
{
	IImageList *other = NULL;
	HIMAGELIST merged;
	HRESULT result;
	if (!second || !object)
		return E_FAIL;
	*object = NULL;
	result = IUnknown_QueryInterface(second, &IID_IImageList, (void **)&other);
	if (FAILED(result))
		return E_FAIL;
	merged = ImageList_Merge((HIMAGELIST)face, first, (HIMAGELIST)other, second_index, dx, dy);
	IImageList_Release(other);
	if (!merged)
		return E_FAIL;
	result = IImageList2_QueryInterface((IImageList2 *)merged, iid, object);
	IImageList2_Release((IImageList2 *)merged);
	return result;
}

static HRESULT WINAPI face_Clone(IImageList2 *face, REFIID iid, void **object)
{
	image_list *copy;
	HRESULT result;
	if (!object)
		return E_INVALIDARG;
	*object = NULL;
	copy = clone_list(of_face(face));
	if (!copy)
		return E_FAIL;
	result = IImageList2_QueryInterface(&copy->face, iid, object);
	IImageList2_Release(&copy->face);
	return result;
}

static HRESULT WINAPI face_GetImageRect(IImageList2 *face, int at, RECT *area)
{
	return ImageList_GetImageRect((HIMAGELIST)face, at, area) ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_GetIconSize(IImageList2 *face, int *cx, int *cy)
{
	return ImageList_GetIconSize((HIMAGELIST)face, cx, cy) ? S_OK : E_INVALIDARG;
}

static HRESULT WINAPI face_SetIconSize(IImageList2 *face, int cx, int cy)
{
	return ImageList_SetIconSize((HIMAGELIST)face, cx, cy) ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_GetImageCount(IImageList2 *face, int *count)
{
	if (!count)
		return E_INVALIDARG;
	*count = of_face(face)->count;
	return S_OK;
}

static HRESULT WINAPI face_SetImageCount(IImageList2 *face, UINT count)
{
	return ImageList_SetImageCount((HIMAGELIST)face, count) ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_SetBkColor(IImageList2 *face, COLORREF color, COLORREF *old)
{
	COLORREF previous = ImageList_SetBkColor((HIMAGELIST)face, color);
	if (old)
		*old = previous;
	return S_OK;
}

static HRESULT WINAPI face_GetBkColor(IImageList2 *face, COLORREF *color)
{
	if (!color)
		return E_INVALIDARG;
	*color = of_face(face)->background;
	return S_OK;
}

static HRESULT WINAPI face_BeginDrag(IImageList2 *face, int at, int hot_x, int hot_y)
{
	return ImageList_BeginDrag((HIMAGELIST)face, at, hot_x, hot_y) ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_EndDrag(IImageList2 *face)
{
	ImageList_EndDrag();
	return S_OK;
}

static HRESULT WINAPI face_DragEnter(IImageList2 *face, HWND window, int x, int y)
{
	return ImageList_DragEnter(window, x, y) ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_DragLeave(IImageList2 *face, HWND window)
{
	return ImageList_DragLeave(window) ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_DragMove(IImageList2 *face, int x, int y)
{
	return ImageList_DragMove(x, y) ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_SetDragCursorImage(IImageList2 *face, IUnknown *source, int at, int dx, int dy)
{
	IImageList *other = NULL;
	HRESULT result;
	if (!source)
		return E_FAIL;
	result = IUnknown_QueryInterface(source, &IID_IImageList, (void **)&other);
	if (FAILED(result))
		return E_FAIL;
	result = ImageList_SetDragCursorImage((HIMAGELIST)other, at, dx, dy) ? S_OK : E_FAIL;
	IImageList_Release(other);
	return result;
}

static HRESULT WINAPI face_DragShowNolock(IImageList2 *face, BOOL show)
{
	return ImageList_DragShowNolock(show) ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_GetDragImage(IImageList2 *face, POINT *position, POINT *hotspot, REFIID iid, void **object)
{
	HIMAGELIST image = ImageList_GetDragImage(position, hotspot);
	if (!image)
		return E_FAIL;
	return IImageList2_QueryInterface((IImageList2 *)image, iid, object);
}

static HRESULT WINAPI face_GetItemFlags(IImageList2 *face, int at, DWORD *flags)
{
	const image_list *list = of_face(face);
	if (at < 0 || at >= list->count || !flags)
		return E_INVALIDARG;
	*flags = list->item_alpha[at] ? ILIF_ALPHA : 0;
	return S_OK;
}

static HRESULT WINAPI face_GetOverlayImage(IImageList2 *face, int overlay, int *index)
{
	const image_list *list = of_face(face);
	if (overlay < 1 || overlay > OVERLAY_COUNT || !index)
		return E_INVALIDARG;
	*index = list->overlays[overlay - 1];
	return S_OK;
}

/* Scaling on request and callback-backed images are not offered: every list holds its own pixels at its own size. */
static HRESULT WINAPI face_Resize(IImageList2 *face, INT cx, INT cy)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI face_GetOriginalSize(IImageList2 *face, INT at, DWORD flags, INT *cx, INT *cy)
{
	return ImageList_GetIconSize((HIMAGELIST)face, cx, cy) ? S_OK : E_INVALIDARG;
}

static HRESULT WINAPI face_SetOriginalSize(IImageList2 *face, INT at, INT cx, INT cy)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI face_SetCallback(IImageList2 *face, IUnknown *callback)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI face_GetCallback(IImageList2 *face, REFIID iid, void **object)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI face_ForceImagePresent(IImageList2 *face, INT at, DWORD flags)
{
	return S_OK;
}

static HRESULT WINAPI face_DiscardImages(IImageList2 *face, INT first, INT last, DWORD flags)
{
	return S_OK;
}

static HRESULT WINAPI face_PreloadImages(IImageList2 *face, IMAGELISTDRAWPARAMS *params)
{
	return S_OK;
}

static HRESULT WINAPI face_GetStatistics(IImageList2 *face, IMAGELISTSTATS *statistics)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI face_Initialize(IImageList2 *face, INT cx, INT cy, UINT flags, INT initial, INT grow)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI face_Replace2(IImageList2 *face, INT at, HBITMAP image, HBITMAP mask, IUnknown *unknown, DWORD flags)
{
	return ImageList_Replace((HIMAGELIST)face, at, image, mask) ? S_OK : E_FAIL;
}

static HRESULT WINAPI face_ReplaceFromImageList(IImageList2 *face, INT at, IImageList *source, INT from, IUnknown *unknown, DWORD flags)
{
	return E_NOTIMPL;
}

static const IImageList2Vtbl face_table = {
	face_QueryInterface, face_AddRef, face_Release, face_Add, face_ReplaceIcon, face_SetOverlayImage, face_Replace, face_AddMasked,
	face_Draw, face_Remove, face_GetIcon, face_GetImageInfo, face_Copy, face_Merge, face_Clone, face_GetImageRect, face_GetIconSize,
	face_SetIconSize, face_GetImageCount, face_SetImageCount, face_SetBkColor, face_GetBkColor, face_BeginDrag, face_EndDrag,
	face_DragEnter, face_DragLeave, face_DragMove, face_SetDragCursorImage, face_DragShowNolock, face_GetDragImage, face_GetItemFlags,
	face_GetOverlayImage, face_Resize, face_GetOriginalSize, face_SetOriginalSize, face_SetCallback, face_GetCallback,
	face_ForceImagePresent, face_DiscardImages, face_PreloadImages, face_GetStatistics, face_Initialize, face_Replace2,
	face_ReplaceFromImageList,
};

HRESULT WINAPI HIMAGELIST_QueryInterface(HIMAGELIST handle, REFIID iid, void **object)
{
	image_list *list = checked(handle);
	if (!object)
		return E_FAIL;
	*object = NULL;
	if (!list)
		return E_FAIL;
	return IImageList2_QueryInterface(&list->face, iid, object);
}

HRESULT WINAPI ImageList_CoCreateInstance(REFCLSID clsid, const IUnknown *outer, REFIID iid, void **object)
{
	image_list *list;
	HRESULT result;
	if (!object)
		return E_POINTER;
	*object = NULL;
	if (!same_guid(clsid, &CLSID_ImageList))
		return CLASS_E_CLASSNOTAVAILABLE;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	list = new_list(1, 1, ILC_COLOR32, 0, 1);
	if (!list)
		return E_OUTOFMEMORY;
	result = IImageList2_QueryInterface(&list->face, iid, object);
	IImageList2_Release(&list->face);
	return result;
}

HRESULT WINAPI ImageList_ReadEx(DWORD flags, IStream *stream, REFIID iid, void **object)
{
	image_list *list;
	HRESULT result;
	if (!stream || !object)
		return E_INVALIDARG;
	*object = NULL;
	list = read_list(stream);
	if (!list)
		return E_FAIL;
	result = IImageList2_QueryInterface(&list->face, iid, object);
	IImageList2_Release(&list->face);
	return result;
}

HRESULT WINAPI ImageList_WriteEx(HIMAGELIST handle, DWORD flags, IStream *stream)
{
	return ImageList_Write(handle, stream) ? S_OK : E_FAIL;
}
