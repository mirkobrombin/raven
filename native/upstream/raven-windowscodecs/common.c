/* SPDX-License-Identifier: MIT */
#include <initguid.h>
#include "wic.h"

void *wic_alloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1);
}

void wic_free(void *block)
{
	HeapFree(GetProcessHeap(), 0, block);
}

static const struct
{
	const GUID *format;
	UINT bits;
} format_bits[] = {
	{&GUID_WICPixelFormatBlackWhite, 1},
	{&GUID_WICPixelFormat1bppIndexed, 1},
	{&GUID_WICPixelFormat2bppIndexed, 2},
	{&GUID_WICPixelFormat2bppGray, 2},
	{&GUID_WICPixelFormat4bppIndexed, 4},
	{&GUID_WICPixelFormat4bppGray, 4},
	{&GUID_WICPixelFormat8bppIndexed, 8},
	{&GUID_WICPixelFormat8bppGray, 8},
	{&GUID_WICPixelFormat8bppAlpha, 8},
	{&GUID_WICPixelFormat16bppGray, 16},
	{&GUID_WICPixelFormat16bppGrayFixedPoint, 16},
	{&GUID_WICPixelFormat32bppBGR101010, 32},
	{&GUID_WICPixelFormat32bppR10G10B10A2, 32},
	{&GUID_WICPixelFormat16bppGrayHalf, 16},
	{&GUID_WICPixelFormat16bppBGR555, 16},
	{&GUID_WICPixelFormat16bppBGR565, 16},
	{&GUID_WICPixelFormat16bppBGRA5551, 16},
	{&GUID_WICPixelFormat24bppBGR, 24},
	{&GUID_WICPixelFormat24bppRGB, 24},
	{&GUID_WICPixelFormat32bppBGR, 32},
	{&GUID_WICPixelFormat32bppRGB, 32},
	{&GUID_WICPixelFormat32bppBGRA, 32},
	{&GUID_WICPixelFormat32bppRGBA, 32},
	{&GUID_WICPixelFormat32bppPBGRA, 32},
	{&GUID_WICPixelFormat32bppPRGBA, 32},
	{&GUID_WICPixelFormat32bppCMYK, 32},
	{&GUID_WICPixelFormat32bppGrayFloat, 32},
	{&GUID_WICPixelFormat32bppGrayFixedPoint, 32},
	{&GUID_WICPixelFormat48bppRGB, 48},
	{&GUID_WICPixelFormat48bppBGR, 48},
	{&GUID_WICPixelFormat64bppRGBA, 64},
	{&GUID_WICPixelFormat64bppBGRA, 64},
	{&GUID_WICPixelFormat64bppPRGBA, 64},
	{&GUID_WICPixelFormat64bppPBGRA, 64},
	{&GUID_WICPixelFormat64bppCMYK, 64},
	{&GUID_WICPixelFormat16bppGrayHalf, 16},
	{&GUID_WICPixelFormat48bppRGBHalf, 48},
	{&GUID_WICPixelFormat96bppRGBFloat, 96},
	{&GUID_WICPixelFormat128bppRGBFloat, 128},
	{&GUID_WICPixelFormat128bppRGBAFloat, 128},
	{&GUID_WICPixelFormat128bppPRGBAFloat, 128},
};

const GUID *wic_format_at(UINT index)
{
	return index < ARRAYSIZE(format_bits) ? format_bits[index].format : NULL;
}

UINT wic_format_bits(const GUID *format)
{
	UINT index;
	for (index = 0; index < ARRAYSIZE(format_bits); index++)
		if (IsEqualGUID(format, format_bits[index].format))
			return format_bits[index].bits;
	return 0;
}

UINT wic_stride(UINT width, UINT bits)
{
	return (UINT)((((UINT64)width * bits + 31) / 32) * 4);
}

HRESULT wic_copy_rows(const WICRect *wanted, UINT width, UINT height, UINT bits, UINT source_stride, const BYTE *source,
	UINT stride, UINT size, BYTE *target)
{
	WICRect whole = {0, 0, (INT)width, (INT)height};
	const WICRect *area = wanted ? wanted : &whole;
	UINT row_bytes, row;
	if (area->X < 0 || area->Y < 0 || area->Width < 0 || area->Height < 0 || (UINT)area->X + area->Width > width || (UINT)area->Y + area->Height > height)
		return E_INVALIDARG;
	row_bytes = (UINT)(((UINT64)area->Width * bits + 7) / 8);
	if (stride < row_bytes)
		return E_INVALIDARG;
	if (!area->Width || !area->Height)
		return S_OK;
	if ((UINT64)stride * (area->Height - 1) + row_bytes > size)
		return WINCODEC_ERR_INSUFFICIENTBUFFER;
	source += (SIZE_T)area->Y * source_stride + ((UINT64)area->X * bits) / 8;
	/* Rectangles that start inside a byte are shifted into place, one source byte and the next at a time. */
	if ((area->X * bits) % 8) {
		const UINT shift = (area->X * bits) % 8;
		for (row = 0; row < (UINT)area->Height; row++) {
			UINT at;
			const BYTE *line = source + (SIZE_T)row * source_stride;
			for (at = 0; at < row_bytes; at++) {
				const BYTE high = line[at], low = (at + 1 < source_stride) ? line[at + 1] : 0;
				target[(SIZE_T)row * stride + at] = (BYTE)(high << shift | low >> (8 - shift));
			}
		}
		return S_OK;
	}
	for (row = 0; row < (UINT)area->Height; row++)
		memcpy(target + (SIZE_T)row * stride, source + (SIZE_T)row * source_stride, row_bytes);
	return S_OK;
}
