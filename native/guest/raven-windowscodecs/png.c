/* SPDX-License-Identifier: MIT */
#include "decode.h"

/* PNG, from RFC 2083. The chunks read here choose the pixel format a file is read in, the one Windows reports for it; the pixels come
   from the Wuffs decoder (wuffs.c). */

static UINT read_be32(const BYTE *data)
{
	return (UINT)data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
}

/* The file without the chunk at `skip` (its length field) of `skipped` bytes: the decoder makes the pixels matching a transparent
   colour transparent black, while their colour is kept here, as Windows keeps it, and only their alpha changes. */
static BYTE *without_chunk(const BYTE *data, SIZE_T size, SIZE_T skip, SIZE_T skipped)
{
	BYTE *copy = wic_alloc(size - skipped);
	if (copy) {
		memcpy(copy, data, skip);
		memcpy(copy + skip, data + skip + skipped, size - skip - skipped);
	}
	return copy;
}

/* Puts the low `depth` bits of value at pixel x of a row packed from the high bits down. */
static void put_packed(BYTE *row, UINT x, UINT depth, UINT value)
{
	const UINT bit = x * depth;
	row[bit / 8] |= (BYTE)(value << (8 - depth - bit % 8));
}

HRESULT parse_png(image_list *list, const BYTE *data, SIZE_T size)
{
	static const BYTE signature[8] = {0x89,'P','N','G',13,10,26,10};
	UINT width = 0, height = 0, depth = 0, kind = 0, interlace = 0, color_count = 0, y, stride;
	WICColor colors[256];
	const BYTE *transparency = NULL;
	UINT transparency_size = 0, ppm_x = 0, ppm_y = 0;
	const BYTE *packed_profile = NULL;
	UINT packed_profile_size = 0;
	SIZE_T at = 8;
	BOOL pixel_data = FALSE, gray_key = FALSE;
	BYTE *opaque = NULL, *decoded, *pixels;
	const BYTE *indices;
	UINT key = 0;
	const GUID *format;
	png_layout layout;
	if (size < 8 || memcmp(data, signature, 8))
		return WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
	while (at + 12 <= size) {
		const UINT length = read_be32(data + at);
		const BYTE *name = data + at + 4, *body = data + at + 8;
		if (at + 12 + (SIZE_T)length > size)
			break;
		if (!memcmp(name, "IHDR", 4) && length >= 13) {
			width = read_be32(body);
			height = read_be32(body + 4);
			depth = body[8];
			kind = body[9];
			interlace = body[12];
		}
		else if (!memcmp(name, "PLTE", 4)) {
			UINT index;
			color_count = min(length / 3, 256);
			for (index = 0; index < color_count; index++)
				colors[index] = 0xff000000 | body[index * 3] << 16 | body[index * 3 + 1] << 8 | body[index * 3 + 2];
		}
		else if (!memcmp(name, "tRNS", 4)) {
			transparency = body;
			transparency_size = length;
		}
		else if (!memcmp(name, "pHYs", 4) && length >= 9 && body[8] == 1) {
			ppm_x = read_be32(body);
			ppm_y = read_be32(body + 4);
		}
		else if (!memcmp(name, "iCCP", 4)) {
			UINT skip = 0;
			while (skip < length && body[skip])
				skip++;
			if (skip + 2 < length) {
				packed_profile = body + skip + 2;
				packed_profile_size = length - skip - 2;
			}
		}
		else if (!memcmp(name, "IDAT", 4))
			pixel_data = TRUE;
		else if (!memcmp(name, "IEND", 4))
			break;
		at += 12 + (SIZE_T)length;
	}
	if (!width || !height || !pixel_data || interlace > 1)
		return WINCODEC_ERR_BADIMAGE;
	if (kind == 1 || kind == 5 || kind > 6 || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)
		|| ((kind == 2 || kind == 4 || kind == 6) && depth < 8) || (kind == 3 && depth == 16))
		return WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
	if (kind == 3 && !color_count)
		return WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
	if (kind == 3) {
		UINT index;
		for (index = 0; index < transparency_size && index < color_count; index++)
			colors[index] = (colors[index] & 0xffffff) | (WICColor)transparency[index] << 24;
		format = depth == 1 ? &GUID_WICPixelFormat1bppIndexed : depth == 2 ? &GUID_WICPixelFormat2bppIndexed : depth == 4 ? &GUID_WICPixelFormat4bppIndexed : &GUID_WICPixelFormat8bppIndexed;
	}
	else if (kind == 0 && transparency && depth <= 8) {
		/* A gray file with a transparent level reads as a palette of its levels. */
		const UINT levels = 1U << depth, transparent = transparency_size >= 2 ? (UINT)transparency[1] : levels;
		UINT index;
		color_count = levels;
		for (index = 0; index < levels; index++) {
			const UINT level = index * 255 / (levels - 1);
			colors[index] = (index == transparent ? 0 : 0xff000000) | level << 16 | level << 8 | level;
		}
		format = depth == 1 ? &GUID_WICPixelFormat1bppIndexed : depth == 2 ? &GUID_WICPixelFormat2bppIndexed : depth == 4 ? &GUID_WICPixelFormat4bppIndexed : &GUID_WICPixelFormat8bppIndexed;
	}
	else if (kind == 0 && !transparency)
		format = depth == 1 ? &GUID_WICPixelFormatBlackWhite : depth == 2 ? &GUID_WICPixelFormat2bppGray : depth == 4 ? &GUID_WICPixelFormat4bppGray : depth == 8 ? &GUID_WICPixelFormat8bppGray : &GUID_WICPixelFormat16bppGray;
	else if (kind == 2)
		format = depth == 16 ? &GUID_WICPixelFormat48bppRGB : &GUID_WICPixelFormat24bppBGR;
	else
		format = depth == 16 ? &GUID_WICPixelFormat64bppRGBA : &GUID_WICPixelFormat32bppBGRA;
	/* A transparent colour of a gray or colour file is applied here (see without_chunk): only 16-bit gray keeps an alpha of its own. */
	if (transparency && (kind == 0 || kind == 2)) {
		const SIZE_T chunk = (SIZE_T)(transparency - data) - 8;
		gray_key = kind == 0 && depth == 16 && transparency_size >= 2;
		key = gray_key ? (UINT)transparency[0] << 8 | transparency[1] : 0;
		if (!(opaque = without_chunk(data, size, chunk, (SIZE_T)transparency_size + 12)))
			return E_OUTOFMEMORY;
	}
	layout = kind == 3 ? png_indexed : depth == 16 ? png_wide : png_narrow;
	decoded = png_pixels(opaque ? opaque : data, opaque ? size - transparency_size - 12 : size, width, height, layout);
	wic_free(opaque);
	if (!decoded)
		return WINCODEC_ERR_BADIMAGE;
	if (!(pixels = image_add(list, width, height, format, &stride))) {
		wic_free(decoded);
		return E_OUTOFMEMORY;
	}
	if (kind == 3 || (kind == 0 && transparency && depth <= 8))
		image_set_colors(list, colors, color_count);
	if (packed_profile) {
		SIZE_T profile_size = 0;
		BYTE *profile = zlib_inflate(packed_profile, packed_profile_size, 16 * 1024 * 1024, &profile_size);
		if (profile && profile_size)
			image_set_profile(list, profile, (UINT)profile_size);
		else
			wic_free(profile);
	}
	if (ppm_x && ppm_y)
		image_set_resolution(list, ppm_x * 0.0254, ppm_y * 0.0254);
	indices = decoded + (kind == 3 ? 1024 : 0);
	for (y = 0; y < height; y++) {
		BYTE *to = pixels + (SIZE_T)y * stride;
		UINT x;
		if (kind == 3 || (kind == 0 && depth <= 8)) {
			/* Palette indices as they are; gray levels from the top bits of the blue the decoder widened them to. */
			for (x = 0; x < width; x++) {
				const UINT value = kind == 3 ? indices[(SIZE_T)y * width + x] : decoded[((SIZE_T)y * width + x) * 4] >> (8 - depth);
				if (depth == 8)
					to[x] = (BYTE)value;
				else
					put_packed(to, x, depth, value);
			}
			continue;
		}
		for (x = 0; x < width; x++) {
			if (depth == 16) {
				const BYTE *from = decoded + ((SIZE_T)y * width + x) * 8;
				const USHORT blue = (USHORT)(from[0] | from[1] << 8), green = (USHORT)(from[2] | from[3] << 8), red = (USHORT)(from[4] | from[5] << 8);
				USHORT alpha = (USHORT)(from[6] | from[7] << 8);
				USHORT *words = (USHORT *)to;
				if (gray_key)
					alpha = red == key ? 0 : 0xffff;
				if (IsEqualGUID(format, &GUID_WICPixelFormat16bppGray))
					words[x] = red;
				else if (IsEqualGUID(format, &GUID_WICPixelFormat48bppRGB)) {
					words[x * 3] = red;
					words[x * 3 + 1] = green;
					words[x * 3 + 2] = blue;
				}
				else {
					words[x * 4] = red;
					words[x * 4 + 1] = green;
					words[x * 4 + 2] = blue;
					words[x * 4 + 3] = alpha;
				}
			}
			else {
				const BYTE *from = decoded + ((SIZE_T)y * width + x) * 4;
				if (IsEqualGUID(format, &GUID_WICPixelFormat24bppBGR)) {
					to[x * 3] = from[0];
					to[x * 3 + 1] = from[1];
					to[x * 3 + 2] = from[2];
				}
				else
					memcpy(to + x * 4, from, 4);
			}
		}
	}
	wic_free(decoded);
	return S_OK;
}
