/* SPDX-License-Identifier: MIT */
#include "wic.h"

typedef struct
{
	IWICPalette iface;
	LONG references;
	CRITICAL_SECTION lock;
	WICBitmapPaletteType type;
	UINT count;
	WICColor *colors;
} palette;

static palette *palette_of(IWICPalette *iface)
{
	return WIC_CONTAINING(iface, palette, iface);
}

/* Levels of a channel are evenly spaced from 0 to 255. */
static BYTE level(UINT index, UINT levels)
{
	return (BYTE)((index * 510 + levels - 1) / (2 * (levels - 1)));
}

static void add_cube(WICColor *colors, UINT *count, UINT reds, UINT greens, UINT blues)
{
	UINT r, g, b;
	for (r = 0; r < reds; r++)
		for (g = 0; g < greens; g++)
			for (b = 0; b < blues; b++)
				colors[(*count)++] = 0xff000000 | level(r, reds) << 16 | level(g, greens) << 8 | level(b, blues);
}

static void add_system_colors(WICColor *colors, UINT *count)
{
	static const WICColor extra[] = {0xffc0c0c0, 0xff808080, 0xff800000, 0xff008000, 0xff000080, 0xff808000, 0xff800080, 0xff008080};
	UINT index;
	for (index = 0; index < ARRAYSIZE(extra); index++)
		colors[(*count)++] = extra[index];
}

static HRESULT replace_colors(palette *self, const WICColor *colors, UINT count, WICBitmapPaletteType type)
{
	WICColor *copy = NULL;
	if (count) {
		if (!(copy = wic_alloc(count * sizeof(*copy))))
			return E_OUTOFMEMORY;
		memcpy(copy, colors, count * sizeof(*copy));
	}
	EnterCriticalSection(&self->lock);
	wic_free(self->colors);
	self->colors = copy;
	self->count = count;
	self->type = type;
	LeaveCriticalSection(&self->lock);
	return S_OK;
}

static HRESULT WINAPI palette_QueryInterface(IWICPalette *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICPalette)) {
		*out = iface;
		IWICPalette_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI palette_AddRef(IWICPalette *iface)
{
	return InterlockedIncrement(&palette_of(iface)->references);
}

static ULONG WINAPI palette_Release(IWICPalette *iface)
{
	palette *self = palette_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		DeleteCriticalSection(&self->lock);
		wic_free(self->colors);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI palette_InitializePredefined(IWICPalette *iface, WICBitmapPaletteType type, BOOL transparent)
{
	WICColor colors[257];
	UINT count = 0, index;
	switch (type) {
	case WICBitmapPaletteTypeFixedBW:
		colors[count++] = 0xff000000;
		colors[count++] = 0xffffffff;
		transparent = FALSE;
		break;
	case WICBitmapPaletteTypeFixedGray4:
	case WICBitmapPaletteTypeFixedGray16:
	case WICBitmapPaletteTypeFixedGray256: {
		const UINT levels = type == WICBitmapPaletteTypeFixedGray4 ? 4 : type == WICBitmapPaletteTypeFixedGray16 ? 16 : 256;
		for (index = 0; index < levels; index++) {
			const BYTE gray = level(index, levels);
			colors[count++] = 0xff000000 | gray << 16 | gray << 8 | gray;
		}
		transparent = FALSE;
		break;
	}
	case WICBitmapPaletteTypeFixedHalftone8:
		for (index = 0; index < 8; index++)
			colors[count++] = 0xff000000 | (index & 4 ? 0xff0000 : 0) | (index & 2 ? 0xff00 : 0) | (index & 1 ? 0xff : 0);
		add_system_colors(colors, &count);
		break;
	case WICBitmapPaletteTypeFixedHalftone27:
		add_cube(colors, &count, 3, 3, 3);
		colors[count++] = 0xffc0c0c0;
		break;
	case WICBitmapPaletteTypeFixedHalftone64:
		add_cube(colors, &count, 4, 4, 4);
		add_system_colors(colors, &count);
		break;
	case WICBitmapPaletteTypeFixedHalftone125:
		add_cube(colors, &count, 5, 5, 5);
		colors[count++] = 0xffc0c0c0;
		break;
	case WICBitmapPaletteTypeFixedHalftone216:
		add_cube(colors, &count, 6, 6, 6);
		add_system_colors(colors, &count);
		break;
	case WICBitmapPaletteTypeFixedHalftone252:
		add_cube(colors, &count, 6, 7, 6);
		break;
	case WICBitmapPaletteTypeFixedHalftone256:
		add_cube(colors, &count, 8, 8, 4);
		if (transparent) {
			colors[255] = 0;
			transparent = FALSE;
		}
		break;
	default:
		return E_INVALIDARG;
	}
	if (transparent)
		colors[count++] = 0;
	return replace_colors(palette_of(iface), colors, count, type);
}

static HRESULT WINAPI palette_InitializeCustom(IWICPalette *iface, WICColor *colors, UINT count)
{
	if (!colors && count)
		return E_INVALIDARG;
	return replace_colors(palette_of(iface), colors, count, WICBitmapPaletteTypeCustom);
}

/* Median cut: the box with the widest channel splits at its middle pixel until there are enough boxes. */
typedef struct
{
	UINT first, count;
} color_box;

static int widest_channel(const BYTE *pixels, const color_box *box, int *spread)
{
	BYTE low[3] = {255, 255, 255}, high[3] = {0, 0, 0};
	UINT at;
	int channel, best = 0;
	for (at = box->first; at < box->first + box->count; at++)
		for (channel = 0; channel < 3; channel++) {
			const BYTE value = pixels[at * 4 + channel];
			if (value < low[channel])
				low[channel] = value;
			if (value > high[channel])
				high[channel] = value;
		}
	*spread = -1;
	for (channel = 0; channel < 3; channel++)
		if (high[channel] - low[channel] > *spread) {
			*spread = high[channel] - low[channel];
			best = channel;
		}
	return best;
}

static void sort_box(BYTE *pixels, const color_box *box, int channel)
{
	UINT counts[256] = {0}, starts[256], at, total = 0;
	int value;
	BYTE *copy = wic_alloc((SIZE_T)box->count * 4);
	if (!copy)
		return;
	memcpy(copy, pixels + (SIZE_T)box->first * 4, (SIZE_T)box->count * 4);
	for (at = 0; at < box->count; at++)
		counts[copy[at * 4 + channel]]++;
	for (value = 0; value < 256; value++) {
		starts[value] = total;
		total += counts[value];
	}
	for (at = 0; at < box->count; at++)
		memcpy(pixels + (SIZE_T)(box->first + starts[copy[at * 4 + channel]]++) * 4, copy + (SIZE_T)at * 4, 4);
	wic_free(copy);
}

static HRESULT WINAPI palette_InitializeFromBitmap(IWICPalette *iface, IWICBitmapSource *source, UINT wanted, BOOL transparent)
{
	IWICBitmapSource *converted = NULL;
	UINT width, height, boxes = 1, pixels_total, index;
	BYTE *pixels;
	color_box box_list[256];
	WICColor colors[256];
	HRESULT result;
	if (!source || wanted < 2 || wanted > 256)
		return E_INVALIDARG;
	if (FAILED(result = WICConvertBitmapSource(&GUID_WICPixelFormat32bppBGRA, source, &converted)))
		return result;
	IWICBitmapSource_GetSize(converted, &width, &height);
	pixels_total = width * height;
	if (!(pixels = wic_alloc((SIZE_T)pixels_total * 4))) {
		IWICBitmapSource_Release(converted);
		return E_OUTOFMEMORY;
	}
	result = IWICBitmapSource_CopyPixels(converted, NULL, width * 4, pixels_total * 4, pixels);
	IWICBitmapSource_Release(converted);
	if (FAILED(result)) {
		wic_free(pixels);
		return result;
	}
	if (transparent)
		wanted--;
	box_list[0].first = 0;
	box_list[0].count = pixels_total;
	while (boxes < wanted) {
		int best = -1, spread_best = 0, channel_best = 0;
		for (index = 0; index < boxes; index++) {
			int spread, channel;
			if (box_list[index].count < 2)
				continue;
			channel = widest_channel(pixels, &box_list[index], &spread);
			if (spread > spread_best) {
				spread_best = spread;
				best = (int)index;
				channel_best = channel;
			}
		}
		if (best < 0)
			break;
		sort_box(pixels, &box_list[best], channel_best);
		box_list[boxes].first = box_list[best].first + box_list[best].count / 2;
		box_list[boxes].count = box_list[best].count - box_list[best].count / 2;
		box_list[best].count /= 2;
		boxes++;
	}
	for (index = 0; index < boxes; index++) {
		UINT64 sum[3] = {0, 0, 0};
		const UINT count = box_list[index].count ? box_list[index].count : 1;
		UINT at;
		for (at = box_list[index].first; at < box_list[index].first + box_list[index].count; at++) {
			sum[0] += pixels[(SIZE_T)at * 4];
			sum[1] += pixels[(SIZE_T)at * 4 + 1];
			sum[2] += pixels[(SIZE_T)at * 4 + 2];
		}
		colors[index] = 0xff000000 | (WICColor)((sum[2] + count / 2) / count) << 16 | (WICColor)((sum[1] + count / 2) / count) << 8 | (WICColor)((sum[0] + count / 2) / count);
	}
	wic_free(pixels);
	if (transparent)
		colors[boxes++] = 0;
	return replace_colors(palette_of(iface), colors, boxes, WICBitmapPaletteTypeCustom);
}

static HRESULT WINAPI palette_InitializeFromPalette(IWICPalette *iface, IWICPalette *source)
{
	WICColor colors[256];
	WICBitmapPaletteType type;
	UINT count;
	HRESULT result;
	if (!source)
		return E_INVALIDARG;
	if (FAILED(result = IWICPalette_GetType(source, &type)) || FAILED(result = IWICPalette_GetColors(source, ARRAYSIZE(colors), colors, &count)))
		return result;
	return replace_colors(palette_of(iface), colors, count, type);
}

static HRESULT WINAPI palette_GetType(IWICPalette *iface, WICBitmapPaletteType *type)
{
	if (!type)
		return E_INVALIDARG;
	*type = palette_of(iface)->type;
	return S_OK;
}

static HRESULT WINAPI palette_GetColorCount(IWICPalette *iface, UINT *count)
{
	if (!count)
		return E_INVALIDARG;
	*count = palette_of(iface)->count;
	return S_OK;
}

static HRESULT WINAPI palette_GetColors(IWICPalette *iface, UINT room, WICColor *colors, UINT *count)
{
	palette *self = palette_of(iface);
	if (!colors || !count)
		return E_INVALIDARG;
	EnterCriticalSection(&self->lock);
	*count = room < self->count ? room : self->count;
	memcpy(colors, self->colors, *count * sizeof(*colors));
	LeaveCriticalSection(&self->lock);
	return S_OK;
}

static HRESULT WINAPI palette_IsBlackWhite(IWICPalette *iface, BOOL *result)
{
	if (!result)
		return E_INVALIDARG;
	*result = palette_of(iface)->type == WICBitmapPaletteTypeFixedBW;
	return S_OK;
}

static HRESULT WINAPI palette_IsGrayscale(IWICPalette *iface, BOOL *result)
{
	const WICBitmapPaletteType type = palette_of(iface)->type;
	if (!result)
		return E_INVALIDARG;
	*result = type == WICBitmapPaletteTypeFixedBW || type == WICBitmapPaletteTypeFixedGray4 || type == WICBitmapPaletteTypeFixedGray16 || type == WICBitmapPaletteTypeFixedGray256;
	return S_OK;
}

static HRESULT WINAPI palette_HasAlpha(IWICPalette *iface, BOOL *result)
{
	palette *self = palette_of(iface);
	UINT index;
	if (!result)
		return E_INVALIDARG;
	*result = FALSE;
	EnterCriticalSection(&self->lock);
	for (index = 0; index < self->count; index++)
		if ((self->colors[index] >> 24) != 0xff)
			*result = TRUE;
	LeaveCriticalSection(&self->lock);
	return S_OK;
}

static const IWICPaletteVtbl palette_vtbl = {
	palette_QueryInterface, palette_AddRef, palette_Release, palette_InitializePredefined, palette_InitializeCustom,
	palette_InitializeFromBitmap, palette_InitializeFromPalette, palette_GetType, palette_GetColorCount, palette_GetColors,
	palette_IsBlackWhite, palette_IsGrayscale, palette_HasAlpha,
};

HRESULT wic_palette_create(IWICPalette **out)
{
	palette *self = wic_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &palette_vtbl;
	self->references = 1;
	self->type = WICBitmapPaletteTypeCustom;
	InitializeCriticalSection(&self->lock);
	*out = &self->iface;
	return S_OK;
}
