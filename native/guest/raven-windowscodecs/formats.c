/* SPDX-License-Identifier: MIT */
#include "decode.h"

/* BMP, GIF and ICO, from their published file format descriptions. */

static UINT read_u16(const BYTE *data)
{
	return data[0] | data[1] << 8;
}

static UINT read_u32(const BYTE *data)
{
	return (UINT)data[0] | data[1] << 8 | data[2] << 16 | (UINT)data[3] << 24;
}

static UINT mask_value(UINT value, UINT mask)
{
	if (!mask)
		return 0;
	while (!(mask & 1)) {
		mask >>= 1;
		value >>= 1;
	}
	return value & mask & 0xff;
}

static void expand_rle(const BYTE *from, SIZE_T size, BYTE *to, UINT stride, UINT width, UINT height, UINT depth)
{
	SIZE_T at = 0;
	UINT x = 0, y = 0;
	#define PUT(value) do { if (x < width && y < height) { if (depth == 8) to[(SIZE_T)y * stride + x] = (BYTE)(value); else to[(SIZE_T)y * stride + x / 2] |= (BYTE)(x & 1 ? (value) & 15 : (value) << 4); } x++; } while (0)
	while (at + 2 <= size) {
		const UINT count = from[at], code = from[at + 1];
		at += 2;
		if (count) {
			UINT index;
			for (index = 0; index < count; index++)
				PUT(depth == 8 ? code : (index & 1 ? code & 15 : code >> 4));
		}
		else if (code == 0) {
			x = 0;
			y++;
		}
		else if (code == 1)
			break;
		else if (code == 2) {
			if (at + 2 > size)
				break;
			x += from[at];
			y += from[at + 1];
			at += 2;
		}
		else {
			const UINT bytes = depth == 8 ? code : (code + 1) / 2;
			UINT index;
			if (at + bytes > size)
				break;
			for (index = 0; index < code; index++) {
				const BYTE value = depth == 8 ? from[at + index] : (index & 1 ? from[at + index / 2] & 15 : from[at + index / 2] >> 4);
				PUT(value);
			}
			at += bytes + (bytes & 1);
		}
	}
	#undef PUT
}

HRESULT parse_dib(image_list *list, const BYTE *data, SIZE_T size, const BYTE *bits_at, BOOL double_height)
{
	UINT header, depth, compression = 0, colors_used = 0, width, stride, source_stride, y, masks[4] = {0, 0, 0, 0};
	INT height;
	BOOL flipped;
	const GUID *format;
	const BYTE *table;
	BYTE *pixels;
	UINT entry_size = 4;
	WICColor colors[256] = {0};
	BOOL rle;
	double dpi_x = 96.0, dpi_y = 96.0;
	if (size < 12)
		return WINCODEC_ERR_BADHEADER;
	header = read_u32(data);
	if (header == 12) {
		width = read_u16(data + 4);
		height = (short)read_u16(data + 6);
		depth = read_u16(data + 10);
		entry_size = 3;
	}
	else if (header >= 40 && size >= header) {
		width = read_u32(data + 4);
		height = (INT)read_u32(data + 8);
		depth = read_u16(data + 14);
		compression = read_u32(data + 16);
		if (read_u32(data + 24) && read_u32(data + 28)) {
			dpi_x = read_u32(data + 24) * 0.0254;
			dpi_y = read_u32(data + 28) * 0.0254;
		}
		colors_used = read_u32(data + 32);
	}
	else
		return WINCODEC_ERR_BADHEADER;
	if (double_height)
		height /= 2;
	flipped = height > 0;
	if (height < 0)
		height = -height;
	if (!width || !height)
		return WINCODEC_ERR_BADIMAGE;
	table = data + header;
	if (compression == BI_BITFIELDS || compression == 6) {
		const BYTE *place = header >= 52 ? data + 40 : table;
		if (place + 12 > data + size)
			return WINCODEC_ERR_BADHEADER;
		masks[0] = read_u32(place);
		masks[1] = read_u32(place + 4);
		masks[2] = read_u32(place + 8);
		if (header >= 56 || compression == 6)
			masks[3] = read_u32(place + 12);
		if (header == 40)
			table += 12;
	}
	switch (depth) {
	case 1:
		format = &GUID_WICPixelFormat1bppIndexed;
		break;
	case 2:
		format = &GUID_WICPixelFormat2bppIndexed;
		break;
	case 4:
		format = &GUID_WICPixelFormat4bppIndexed;
		break;
	case 8:
		format = &GUID_WICPixelFormat8bppIndexed;
		break;
	case 16:
		format = compression == BI_BITFIELDS && masks[1] == 0x7e0 ? &GUID_WICPixelFormat16bppBGR565 : &GUID_WICPixelFormat16bppBGR555;
		break;
	case 24:
		format = &GUID_WICPixelFormat24bppBGR;
		break;
	case 32:
		/* The alpha of a plain BMP is not read as alpha; an icon's colors carry it. */
		format = double_height ? &GUID_WICPixelFormat32bppBGRA : &GUID_WICPixelFormat32bppBGR;
		break;
	default:
		return WINCODEC_ERR_BADIMAGE;
	}
	rle = compression == BI_RLE8 || compression == BI_RLE4;
	/* RLE8 runs fill 8-bit pixels and RLE4 runs 4-bit ones; with another depth the rows the runs fill are longer than the rows kept. */
	if ((compression == BI_RLE8 && depth != 8) || (compression == BI_RLE4 && depth != 4))
		return WINCODEC_ERR_BADIMAGE;
	if (rle)
		format = &GUID_WICPixelFormat32bppBGR;
	if (!(pixels = image_add(list, width, (UINT)height, format, &stride)))
		return E_OUTOFMEMORY;
	image_set_resolution(list, dpi_x, dpi_y);
	if (depth <= 8) {
		UINT index, count = colors_used && colors_used <= (1U << depth) ? colors_used : 1U << depth;
		if (table + (SIZE_T)count * entry_size > data + size)
			return WINCODEC_ERR_BADHEADER;
		for (index = 0; index < count; index++)
			colors[index] = 0xff000000 | table[index * entry_size + 2] << 16 | table[index * entry_size + 1] << 8 | table[index * entry_size];
		image_set_colors(list, colors, count);
		table += (SIZE_T)count * entry_size;
	}
	if (!bits_at)
		bits_at = table;
	if (bits_at > data + size)
		return WINCODEC_ERR_BADIMAGE;
	source_stride = wic_stride(width, depth);
	if (rle) {
		const SIZE_T rle_size = (SIZE_T)(data + size - bits_at);
		BYTE *buffer = wic_alloc((SIZE_T)source_stride * height);
		UINT x;
		if (!buffer)
			return E_OUTOFMEMORY;
		expand_rle(bits_at, rle_size, buffer, source_stride, width, (UINT)height, compression == BI_RLE8 ? 8 : 4);
		for (y = 0; y < (UINT)height; y++) {
			const BYTE *from = buffer + (SIZE_T)(flipped ? (UINT)height - 1 - y : y) * source_stride;
			for (x = 0; x < width; x++) {
				const UINT index = depth == 8 ? from[x] : x & 1 ? from[x / 2] & 15 : from[x / 2] >> 4;
				((UINT *)(pixels + (SIZE_T)y * stride))[x] = colors[index] & 0xffffff;
			}
		}
		wic_free(buffer);
		return S_OK;
	}
	/* The rows that fit between the pixels and the end, counted without forming an address past it. */
	if (!source_stride || (SIZE_T)(data + size - bits_at) / source_stride < (UINT)height)
		return WINCODEC_ERR_BADIMAGE;
	for (y = 0; y < (UINT)height; y++)
		memcpy(pixels + (SIZE_T)y * stride, bits_at + (SIZE_T)(flipped ? (UINT)height - 1 - y : y) * source_stride, source_stride);
	/* Colors at other places than the plain layout are moved there. */
	if (depth == 32 && (masks[0] || masks[1] || masks[2]) && (masks[0] != 0xff0000 || masks[1] != 0xff00 || masks[2] != 0xff)) {
		UINT x;
		for (y = 0; y < (UINT)height; y++)
			for (x = 0; x < width; x++) {
				UINT *pixel = (UINT *)(pixels + (SIZE_T)y * stride) + x;
				const UINT value = *pixel;
				*pixel = mask_value(value, masks[0]) << 16 | mask_value(value, masks[1]) << 8 | mask_value(value, masks[2]) | (double_height ? mask_value(value, masks[3]) << 24 : 0);
			}
	}
	return S_OK;
}

HRESULT parse_bmp(image_list *list, const BYTE *data, SIZE_T size)
{
	UINT offset;
	if (size < 18 || data[0] != 'B' || data[1] != 'M')
		return WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
	/* The pixel offset is checked as a number before it becomes an address: in a 32-bit program one near four gigabytes would wrap round
	   to below the file. */
	offset = read_u32(data + 10);
	if (offset > size)
		return WINCODEC_ERR_BADIMAGE;
	return parse_dib(list, data + 14, size - 14, offset ? data + offset : NULL, FALSE);
}

/* The data blocks of one image joined into a new buffer, `at` left on the terminator or the end of the file; NULL without memory. */
static BYTE *join_blocks(const BYTE *data, SIZE_T size, SIZE_T *at, SIZE_T *joined)
{
	SIZE_T scan = *at, total = 0;
	BYTE *codes;
	while (scan < size && data[scan]) {
		const SIZE_T block = min((SIZE_T)data[scan], size - scan - 1);
		total += block;
		scan += 1 + block;
	}
	if (!(codes = wic_alloc(total ? total : 1)))
		return NULL;
	*joined = 0;
	while (*at < size && data[*at]) {
		const SIZE_T block = min((SIZE_T)data[*at], size - *at - 1);
		memcpy(codes + *joined, data + *at + 1, block);
		*joined += block;
		*at += 1 + block;
	}
	return codes;
}

HRESULT parse_gif(image_list *list, const BYTE *data, SIZE_T size)
{
	WICColor global[256];
	UINT global_count = 0, index;
	SIZE_T at = 13;
	int transparent = -1;
	BOOL first_image = TRUE;
	double dpi_x = 96.0;
	if (size < 13 || (memcmp(data, "GIF87a", 6) && memcmp(data, "GIF89a", 6)))
		return WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
	if (data[10] & 0x80) {
		global_count = 2U << (data[10] & 7);
		if (at + (SIZE_T)global_count * 3 > size)
			return WINCODEC_ERR_BADIMAGE;
		for (index = 0; index < global_count; index++)
			global[index] = 0xff000000 | data[at + index * 3] << 16 | data[at + index * 3 + 1] << 8 | data[at + index * 3 + 2];
		at += (SIZE_T)global_count * 3;
	}
	else {
		/* Without a table the same bits still give its size: black, and white for the second entry. */
		global_count = 2U << (data[10] & 7);
		for (index = 0; index < global_count; index++)
			global[index] = index == 1 ? 0xffffffff : 0xff000000;
	}
	memcpy(list->colors, global, sizeof(WICColor) * global_count);
	list->color_count = global_count;
	if (data[12])
		dpi_x = 96.0 * 64 / (data[12] + 15);
	while (at < size && data[at] != 0x3b) {
		const BYTE kind = data[at++];
		if (kind == 0x21) {
			BYTE label;
			if (at >= size)
				break;
			label = data[at++];
			if (label == 0xf9 && at + 6 <= size && data[at] == 4)
			{
				transparent = data[at + 1] & 1 ? data[at + 4] : -1;
				/* The first image's transparency also marks the file's own table. */
				if (first_image && transparent >= 0 && (UINT)transparent < global_count)
					list->colors[transparent] &= 0xffffff;
			}
			while (at < size && data[at])
				at += (SIZE_T)data[at] + 1;
			at++;
		}
		else if (kind == 0x2c) {
			UINT width, height, stride, y;
			WICColor colors[256];
			UINT count = global_count;
			BYTE flags, *pixels, *flat;
			if (at + 9 > size)
				return WINCODEC_ERR_BADIMAGE;
			width = read_u16(data + at + 4);
			height = read_u16(data + at + 6);
			flags = data[at + 8];
			at += 9;
			memcpy(colors, global, sizeof(WICColor) * global_count);
			if (flags & 0x80) {
				count = 2U << (flags & 7);
				if (at + (SIZE_T)count * 3 > size)
					return WINCODEC_ERR_BADIMAGE;
				for (index = 0; index < count; index++)
					colors[index] = 0xff000000 | data[at + index * 3] << 16 | data[at + index * 3 + 1] << 8 | data[at + index * 3 + 2];
				at += (SIZE_T)count * 3;
			}
			if (transparent >= 0 && (UINT)transparent < count)
				colors[transparent] &= 0xffffff;
			if (!width || !height)
				return WINCODEC_ERR_BADIMAGE;
			if (!(pixels = image_add(list, width, height, &GUID_WICPixelFormat8bppIndexed, &stride)))
				return E_OUTOFMEMORY;
			image_set_colors(list, colors, count);
			image_set_resolution(list, dpi_x, 96.0);
			first_image = FALSE;
			if (!(flat = wic_alloc((SIZE_T)width * height)))
				return E_OUTOFMEMORY;
			{
				/* A file may simply end before the image's data, which leaves the image empty. */
				const UINT minimum = at < size ? data[at++] : 8;
				SIZE_T joined;
				BYTE *codes;
				if (minimum < 2 || minimum > 8) {
					wic_free(flat);
					return WINCODEC_ERR_BADIMAGE;
				}
				if (!(codes = join_blocks(data, size, &at, &joined))) {
					wic_free(flat);
					return E_OUTOFMEMORY;
				}
				lzw_expand(codes, joined, minimum, flat, (SIZE_T)width * height);
				wic_free(codes);
			}
			if (flags & 0x40) {
				static const BYTE starts[4] = {0, 4, 2, 1}, steps[4] = {8, 8, 4, 2};
				UINT pass, source = 0;
				for (pass = 0; pass < 4; pass++)
					for (y = starts[pass]; y < height; y += steps[pass])
						memcpy(pixels + (SIZE_T)y * stride, flat + (SIZE_T)(source++) * width, width);
			}
			else
				for (y = 0; y < height; y++)
					memcpy(pixels + (SIZE_T)y * stride, flat + (SIZE_T)y * width, width);
			wic_free(flat);
			transparent = -1;
			while (at < size && data[at])
				at += (SIZE_T)data[at] + 1;
			at++;
		}
		else
			break;
	}
	return S_OK;
}

HRESULT parse_ico(image_list *list, const BYTE *data, SIZE_T size)
{
	UINT count, index;
	if (size < 6)
		return WINCODEC_ERR_STREAMREAD;
	if (read_u16(data) || (read_u16(data + 2) != 1 && read_u16(data + 2) != 2))
		return WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
	count = read_u16(data + 4);
	if (!count)
		return WINCODEC_ERR_BADHEADER;
	if (6 + (SIZE_T)count * 16 > size)
		return WINCODEC_ERR_BADIMAGE;
	for (index = 0; index < count; index++) {
		const BYTE *entry = data + 6 + index * 16;
		const UINT bytes = read_u32(entry + 8), offset = read_u32(entry + 12);
		const BYTE *body;
		BYTE *pixels;
		image_list part;
		UINT stride, width, height, y, x;
		IWICBitmapSource *converted;
		HRESULT result;
		/* The entry's place is checked as numbers before it becomes an address: in a 32-bit program an offset and a size near four
		   gigabytes would add up round to a small number, and the address of the entry to below the file. */
		if (offset > size || bytes > size - offset)
			return WINCODEC_ERR_BADIMAGE;
		body = data + offset;
		image_list_init(&part);
		if (bytes >= 8 && body[0] == 0x89)
			result = parse_png(&part, body, bytes);
		else
			result = parse_dib(&part, body, bytes, NULL, TRUE);
		if (FAILED(result)) {
			image_list_clear(&part);
			return result;
		}
		result = image_list_first_as_bgra(&part, &converted);
		if (SUCCEEDED(result)) {
			IWICBitmapSource_GetSize(converted, &width, &height);
			if (!(pixels = image_add(list, width, height, &GUID_WICPixelFormat32bppBGRA, &stride)))
				result = E_OUTOFMEMORY;
			else {
				result = IWICBitmapSource_CopyPixels(converted, NULL, stride, stride * height, pixels);
				/* A DIB entry keeps its transparency in the mask that follows the colors, unless the colors carry alpha themselves. The
				   header gives the depth and the colors: a core header of 12 bytes with entries of three bytes, a longer one with entries of
				   four. Where the mask lies is counted in 64 bits, and it is read only when the entry holds all of it. */
				if (SUCCEEDED(result) && body[0] != 0x89) {
					const UINT header = read_u32(body), core = header == 12;
					const UINT depth = core ? read_u16(body + 10) : read_u16(body + 14);
					const UINT used = core ? 0 : read_u32(body + 32), colors = used ? used : depth <= 8 ? 1U << depth : 0;
					const UINT64 mask_stride = wic_stride(width, 1);
					const UINT64 mask_at = (UINT64)header + (UINT64)colors * (core ? 3 : 4) + (UINT64)wic_stride(width, depth) * height;
					if (depth < 32 && mask_at <= bytes && mask_stride * height <= bytes - mask_at)
						for (y = 0; y < height; y++) {
							const BYTE *mask_row = body + (SIZE_T)(mask_at + (UINT64)(height - 1 - y) * mask_stride);
							for (x = 0; x < width; x++)
								pixels[(SIZE_T)y * stride + x * 4 + 3] = mask_row[x / 8] & (0x80 >> (x % 8)) ? 0 : 255;
						}
				}
			}
			IWICBitmapSource_Release(converted);
		}
		image_list_clear(&part);
		if (FAILED(result))
			return result;
	}
	return S_OK;
}
