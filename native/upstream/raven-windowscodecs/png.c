/* SPDX-License-Identifier: MIT */
#include "decode.h"

/* PNG and the deflate stream inside it, from RFC 2083 and RFC 1951. */

typedef struct
{
	const BYTE *data;
	SIZE_T size, at;
	UINT bits, held;
} bit_reader;

static UINT take_bits(bit_reader *reader, UINT count)
{
	UINT value;
	while (reader->held < count) {
		const UINT next = reader->at < reader->size ? reader->data[reader->at] : 0;
		reader->at++;
		reader->bits |= next << reader->held;
		reader->held += 8;
	}
	value = reader->bits & ((1U << count) - 1);
	reader->bits >>= count;
	reader->held -= count;
	return value;
}

typedef struct
{
	USHORT count[16], symbol[288];
} huffman;

static void build_huffman(huffman *table, const BYTE *lengths, UINT total)
{
	USHORT offsets[16];
	UINT index;
	memset(table->count, 0, sizeof(table->count));
	for (index = 0; index < total; index++)
		table->count[lengths[index]]++;
	table->count[0] = 0;
	offsets[1] = 0;
	for (index = 1; index < 15; index++)
		offsets[index + 1] = offsets[index] + table->count[index];
	for (index = 0; index < total; index++)
		if (lengths[index])
			table->symbol[offsets[lengths[index]]++] = (USHORT)index;
}

static int read_symbol(bit_reader *reader, const huffman *table)
{
	int code = 0, first = 0, index = 0, length;
	for (length = 1; length <= 15; length++) {
		const int count = table->count[length];
		code |= (int)take_bits(reader, 1);
		if (code - count < first)
			return table->symbol[index + (code - first)];
		index += count;
		first = (first + count) << 1;
		code <<= 1;
	}
	return -1;
}

static const USHORT length_base[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const BYTE length_extra[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const USHORT distance_base[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const BYTE distance_extra[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static BOOL inflate_block(bit_reader *reader, const huffman *literals, const huffman *distances, BYTE *out, SIZE_T room, SIZE_T *written)
{
	for (;;) {
		const int symbol = read_symbol(reader, literals);
		if (symbol < 0 || reader->at > reader->size + 4)
			return FALSE;
		if (symbol < 256) {
			if (*written >= room)
				return FALSE;
			out[(*written)++] = (BYTE)symbol;
		}
		else if (symbol == 256)
			return TRUE;
		else {
			int code = symbol - 257, distance_code;
			UINT length, distance, copied;
			if (code >= 29)
				return FALSE;
			length = length_base[code] + take_bits(reader, length_extra[code]);
			distance_code = read_symbol(reader, distances);
			if (distance_code < 0 || distance_code >= 30)
				return FALSE;
			distance = distance_base[distance_code] + take_bits(reader, distance_extra[distance_code]);
			if (distance > *written)
				return FALSE;
			for (copied = 0; copied < length; copied++) {
				if (*written >= room)
					return FALSE;
				out[*written] = out[*written - distance];
				(*written)++;
			}
		}
	}
}

static BOOL inflate_zlib(const BYTE *data, SIZE_T size, BYTE *out, SIZE_T room, SIZE_T *written)
{
	static const BYTE order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
	bit_reader reader = {data, size, 2, 0, 0};
	huffman *literals, *distances;
	BOOL last, good = TRUE;
	if (size < 2 || (data[0] & 15) != 8 || (data[1] & 0x20))
		return FALSE;
	literals = wic_alloc(sizeof(*literals));
	distances = wic_alloc(sizeof(*distances));
	if (!literals || !distances) {
		wic_free(literals);
		wic_free(distances);
		return FALSE;
	}
	*written = 0;
	do {
		UINT type;
		last = take_bits(&reader, 1);
		type = take_bits(&reader, 2);
		if (type == 0) {
			UINT length;
			reader.bits = reader.held = 0;
			if (reader.at + 4 > size) {
				good = FALSE;
				break;
			}
			length = data[reader.at] | data[reader.at + 1] << 8;
			reader.at += 4;
			if (reader.at + length > size || *written + length > room) {
				good = FALSE;
				break;
			}
			memcpy(out + *written, data + reader.at, length);
			*written += length;
			reader.at += length;
		}
		else if (type == 1 || type == 2) {
			BYTE lengths[320];
			UINT literal_total = 288, distance_total = 30, index;
			if (type == 1) {
				for (index = 0; index < 288; index++)
					lengths[index] = index < 144 ? 8 : index < 256 ? 9 : index < 280 ? 7 : 8;
				for (index = 0; index < 30; index++)
					lengths[288 + index] = 5;
			}
			else {
				const UINT code_total = 0;
				UINT codes;
				huffman *lengths_code = wic_alloc(sizeof(*lengths_code));
				if (!lengths_code) {
					good = FALSE;
					break;
				}
				literal_total = take_bits(&reader, 5) + 257;
				distance_total = take_bits(&reader, 5) + 1;
				codes = take_bits(&reader, 4) + 4;
				memset(lengths, 0, 19);
				for (index = code_total; index < codes; index++)
					lengths[order[index]] = (BYTE)take_bits(&reader, 3);
				build_huffman(lengths_code, lengths, 19);
				index = 0;
				while (index < literal_total + distance_total && good) {
					const int symbol = read_symbol(&reader, lengths_code);
					if (symbol < 0)
						good = FALSE;
					else if (symbol < 16)
						lengths[index++] = (BYTE)symbol;
					else {
						const BYTE previous = symbol == 16 && index ? lengths[index - 1] : 0;
						UINT repeat = symbol == 16 ? 3 + take_bits(&reader, 2) : symbol == 17 ? 3 + take_bits(&reader, 3) : 11 + take_bits(&reader, 7);
						if (index + repeat > literal_total + distance_total)
							good = FALSE;
						else
							while (repeat--)
								lengths[index++] = previous;
					}
				}
				wic_free(lengths_code);
				if (!good)
					break;
			}
			build_huffman(literals, lengths, literal_total);
			build_huffman(distances, lengths + literal_total, distance_total);
			if (!inflate_block(&reader, literals, distances, out, room, written))
				good = FALSE;
		}
		else
			good = FALSE;
	} while (good && !last);
	wic_free(literals);
	wic_free(distances);
	return good;
}

static UINT read_be32(const BYTE *data)
{
	return (UINT)data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
}

static int distance_of(int from, int to)
{
	return from > to ? from - to : to - from;
}

static BYTE paeth(BYTE left, BYTE up, BYTE corner)
{
	const int estimate = left + up - corner, a = distance_of(estimate, left), b = distance_of(estimate, up), c = distance_of(estimate, corner);
	return a <= b && a <= c ? left : b <= c ? up : corner;
}

static BOOL unfilter(BYTE *rows, UINT row_bytes, UINT count, UINT pixel_bytes)
{
	UINT y, x;
	for (y = 0; y < count; y++) {
		BYTE *line = rows + (SIZE_T)y * (row_bytes + 1);
		const BYTE kind = line[0];
		BYTE *pixels = line + 1;
		const BYTE *above = y ? pixels - row_bytes - 1 : NULL;
		if (kind > 4)
			return FALSE;
		for (x = 0; x < row_bytes; x++) {
			const BYTE left = x >= pixel_bytes ? pixels[x - pixel_bytes] : 0, up = above ? above[x] : 0, corner = above && x >= pixel_bytes ? above[x - pixel_bytes] : 0;
			switch (kind) {
			case 1:
				pixels[x] += left;
				break;
			case 2:
				pixels[x] += up;
				break;
			case 3:
				pixels[x] += (BYTE)((left + up) / 2);
				break;
			case 4:
				pixels[x] += paeth(left, up, corner);
				break;
			}
		}
	}
	return TRUE;
}

/* Puts one reduced image of an interlaced file, or the whole of a plain one, at its place in the target. */
static void place_row(const BYTE *from, UINT columns, UINT depth, UINT channels, BYTE *to, UINT start, UINT step)
{
	const UINT pixel_bits = depth * channels;
	UINT x;
	for (x = 0; x < columns; x++) {
		const UINT target_x = start + x * step;
		if (pixel_bits >= 8)
			memcpy(to + (SIZE_T)target_x * (pixel_bits / 8), from + (SIZE_T)x * (pixel_bits / 8), pixel_bits / 8);
		else {
			const UINT from_bit = x * pixel_bits, to_bit = target_x * pixel_bits;
			const BYTE value = (from[from_bit / 8] >> (8 - pixel_bits - from_bit % 8)) & ((1 << pixel_bits) - 1);
			to[to_bit / 8] |= (BYTE)(value << (8 - pixel_bits - to_bit % 8));
		}
	}
}

HRESULT parse_png(image_list *list, const BYTE *data, SIZE_T size)
{
	static const BYTE adam_x[7] = {0,4,0,2,0,1,0}, adam_y[7] = {0,0,4,0,2,0,1}, adam_dx[7] = {8,8,4,4,2,2,1}, adam_dy[7] = {8,8,8,4,4,2,2};
	static const BYTE signature[8] = {0x89,'P','N','G',13,10,26,10};
	UINT width = 0, height = 0, depth = 0, kind = 0, interlace = 0, channels, color_count = 0, y, pass;
	WICColor colors[256];
	const BYTE *transparency = NULL;
	UINT transparency_size = 0, ppm_x = 0, ppm_y = 0;
	const BYTE *packed_profile = NULL;
	UINT packed_profile_size = 0;
	SIZE_T at = 8, packed_size = 0, needed, written = 0;
	BYTE *packed, *raw, *pixels, *full = NULL;
	UINT full_stride, stride;
	const GUID *format;
	HRESULT result = WINCODEC_ERR_BADIMAGE;
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
			packed_size += length;
		else if (!memcmp(name, "IEND", 4))
			break;
		at += 12 + (SIZE_T)length;
	}
	if (!width || !height || !packed_size || interlace > 1)
		return WINCODEC_ERR_BADIMAGE;
	if (kind == 1 || kind == 5 || kind > 6 || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)
		|| ((kind == 2 || kind == 4 || kind == 6) && depth < 8) || (kind == 3 && depth == 16))
		return WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
	switch (kind) {
	case 0:
		channels = 1;
		break;
	case 2:
		channels = 3;
		break;
	case 3:
		channels = 1;
		break;
	case 4:
		channels = 2;
		break;
	case 6:
		channels = 4;
		break;
	default:
		return WINCODEC_ERR_BADIMAGE;
	}
	if (kind == 3 && !color_count)
		return WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
	if (!(packed = wic_alloc(packed_size)))
		return E_OUTOFMEMORY;
	packed_size = 0;
	for (at = 8; at + 12 <= size;) {
		const UINT length = read_be32(data + at);
		if (at + 12 + (SIZE_T)length > size)
			break;
		if (!memcmp(data + at + 4, "IDAT", 4)) {
			memcpy(packed + packed_size, data + at + 8, length);
			packed_size += length;
		}
		at += 12 + (SIZE_T)length;
	}
	needed = 0;
	for (pass = 0; pass < (interlace ? 7U : 1U); pass++) {
		const UINT columns = interlace ? (width + adam_dx[pass] - 1 - adam_x[pass]) / adam_dx[pass] : width;
		const UINT rows = interlace ? (height + adam_dy[pass] - 1 - adam_y[pass]) / adam_dy[pass] : height;
		if (columns && rows && adam_x[pass] < width + 8)
			needed += (SIZE_T)rows * (((UINT64)columns * depth * channels + 7) / 8 + 1);
	}
	if (!(raw = wic_alloc(needed))) {
		wic_free(packed);
		return E_OUTOFMEMORY;
	}
	/* A file with more or fewer rows than its header says still gives what it holds. */
	if (!inflate_zlib(packed, packed_size, raw, needed, &written) && written < needed)
		goto done;
	full_stride = (UINT)(((UINT64)width * depth * channels + 7) / 8);
	if (!(full = wic_alloc((SIZE_T)full_stride * height))) {
		result = E_OUTOFMEMORY;
		goto done;
	}
	{
		SIZE_T offset = 0;
		const UINT pixel_bytes = max(1, depth * channels / 8);
		for (pass = 0; pass < (interlace ? 7U : 1U); pass++) {
			const UINT start_x = interlace ? adam_x[pass] : 0, start_y = interlace ? adam_y[pass] : 0, dx = interlace ? adam_dx[pass] : 1, dy = interlace ? adam_dy[pass] : 1;
			const UINT columns = (width + dx - 1 - start_x) / dx, rows = (height + dy - 1 - start_y) / dy;
			UINT row_bytes;
			if (start_x >= width || start_y >= height || !columns || !rows)
				continue;
			row_bytes = (UINT)(((UINT64)columns * depth * channels + 7) / 8);
			if (!unfilter(raw + offset, row_bytes, rows, pixel_bytes))
				goto done;
			for (y = 0; y < rows; y++)
				place_row(raw + offset + (SIZE_T)y * (row_bytes + 1) + 1, columns, depth, channels, full + (SIZE_T)(start_y + y * dy) * full_stride, start_x, dx);
			offset += (SIZE_T)rows * (row_bytes + 1);
		}
	}
	if (kind == 3) {
		UINT index;
		for (index = 0; index < transparency_size && index < color_count; index++)
			colors[index] = (colors[index] & 0xffffff) | (WICColor)transparency[index] << 24;
		format = depth == 1 ? &GUID_WICPixelFormat1bppIndexed : depth == 2 ? &GUID_WICPixelFormat2bppIndexed : depth == 4 ? &GUID_WICPixelFormat4bppIndexed : &GUID_WICPixelFormat8bppIndexed;
	}
	else if (kind == 0 && transparency && depth <= 8) {
		/* A gray file with a transparent level reads as a palette of its levels. */
		const UINT levels = 1U << depth, key = transparency_size >= 2 ? (UINT)transparency[1] : levels;
		UINT index;
		color_count = levels;
		for (index = 0; index < levels; index++) {
			const UINT level = index * 255 / (levels - 1);
			colors[index] = (index == key ? 0 : 0xff000000) | level << 16 | level << 8 | level;
		}
		format = depth == 1 ? &GUID_WICPixelFormat1bppIndexed : depth == 2 ? &GUID_WICPixelFormat2bppIndexed : depth == 4 ? &GUID_WICPixelFormat4bppIndexed : &GUID_WICPixelFormat8bppIndexed;
	}
	else if (kind == 0 && !transparency)
		format = depth == 1 ? &GUID_WICPixelFormatBlackWhite : depth == 2 ? &GUID_WICPixelFormat2bppGray : depth == 4 ? &GUID_WICPixelFormat4bppGray : depth == 8 ? &GUID_WICPixelFormat8bppGray : &GUID_WICPixelFormat16bppGray;
	else if (kind == 2)
		format = depth == 16 ? &GUID_WICPixelFormat48bppRGB : &GUID_WICPixelFormat24bppBGR;
	else
		format = depth == 16 ? &GUID_WICPixelFormat64bppRGBA : &GUID_WICPixelFormat32bppBGRA;
	if (!(pixels = image_add(list, width, height, format, &stride))) {
		result = E_OUTOFMEMORY;
		goto done;
	}
	if (kind == 3 || (kind == 0 && transparency && depth <= 8))
		image_set_colors(list, colors, color_count);
	if (packed_profile) {
		const SIZE_T room = 16 * 1024 * 1024;
		BYTE *profile = wic_alloc(room);
		SIZE_T profile_size = 0;
		if (profile && inflate_zlib(packed_profile, packed_profile_size, profile, room, &profile_size) && profile_size)
			image_set_profile(list, profile, (UINT)profile_size);
		else
			wic_free(profile);
	}
	if (ppm_x && ppm_y)
		image_set_resolution(list, ppm_x * 0.0254, ppm_y * 0.0254);
	for (y = 0; y < height; y++) {
		const BYTE *from = full + (SIZE_T)y * full_stride;
		BYTE *to = pixels + (SIZE_T)y * stride;
		UINT x;
		if (kind == 3 || (kind == 0 && depth <= 8)) {
			memcpy(to, from, full_stride);
			continue;
		}
		for (x = 0; x < width; x++) {
			UINT red, green, blue, alpha = depth == 16 ? 0xffff : 0xff, gray;
			#define SAMPLE(index) (depth == 16 ? (UINT)(from[(SIZE_T)x * channels * 2 + (index) * 2] << 8 | from[(SIZE_T)x * channels * 2 + (index) * 2 + 1]) : depth == 8 ? (UINT)from[(SIZE_T)x * channels + (index)] : (UINT)((from[x * depth / 8] >> (8 - depth - x * depth % 8)) & ((1 << depth) - 1)))
			if (kind == 0 || kind == 4) {
				gray = SAMPLE(0);
				if (kind == 4)
					alpha = SAMPLE(1);
				else if (transparency_size >= 2 && gray == (UINT)(transparency[0] << 8 | transparency[1]))
					alpha = 0;
				if (depth < 8)
					gray = gray * 255 / ((1 << depth) - 1);
				red = green = blue = gray;
			}
			else {
				red = SAMPLE(0);
				green = SAMPLE(1);
				blue = SAMPLE(2);
				if (kind == 6)
					alpha = SAMPLE(3);
			}
			#undef SAMPLE
			if (IsEqualGUID(format, &GUID_WICPixelFormat24bppBGR)) {
				to[x * 3] = (BYTE)blue;
				to[x * 3 + 1] = (BYTE)green;
				to[x * 3 + 2] = (BYTE)red;
			}
			else if (IsEqualGUID(format, &GUID_WICPixelFormat32bppBGRA)) {
				to[x * 4] = (BYTE)blue;
				to[x * 4 + 1] = (BYTE)green;
				to[x * 4 + 2] = (BYTE)red;
				to[x * 4 + 3] = (BYTE)alpha;
			}
			else if (IsEqualGUID(format, &GUID_WICPixelFormat16bppGray))
				((USHORT *)to)[x] = (USHORT)red;
			else if (IsEqualGUID(format, &GUID_WICPixelFormat48bppRGB)) {
				((USHORT *)to)[x * 3] = (USHORT)red;
				((USHORT *)to)[x * 3 + 1] = (USHORT)green;
				((USHORT *)to)[x * 3 + 2] = (USHORT)blue;
			}
			else {
				((USHORT *)to)[x * 4] = (USHORT)red;
				((USHORT *)to)[x * 4 + 1] = (USHORT)green;
				((USHORT *)to)[x * 4 + 2] = (USHORT)blue;
				((USHORT *)to)[x * 4 + 3] = (USHORT)alpha;
			}
		}
	}
	result = S_OK;
done:
	wic_free(full);
	wic_free(raw);
	wic_free(packed);
	return result;
}
