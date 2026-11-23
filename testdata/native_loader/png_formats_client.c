#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

/* PNG files of the kinds the format allows, made here with stored deflate blocks and read through the imaging component: each keeps
   the pixel format it was written in, and every pixel and palette entry comes back. A chunk whose checksum is wrong is read as the others
   are. So is a picture compressed as literals alone, with a distance code of no codes (RFC 1951, 3.2.7), as some encoders write small
   palettes. A 32-bit program reading a picture whose pixels would not fit its address space gets an error instead of a damaged heap. The
   exit code names the first check that failed: ten for each case, plus the step. */

#define PLAIN __attribute__((optimize("no-tree-loop-distribute-patterns")))

typedef struct
{
	UINT width, height, depth, kind, interlace;
	UINT (*sample)(UINT x, UINT y, UINT channel);
	const BYTE *palette, *transparency;
	UINT colors, transparent;
	BOOL damaged, literals;
} picture;

static BYTE file[65536], raw[32768], packed[40000], pixels[8192], expected[8192];
static UINT file_size, crc_table[256];
static const BYTE adam_x[7] = {0, 4, 0, 2, 0, 1, 0}, adam_y[7] = {0, 0, 4, 0, 2, 0, 1}, adam_dx[7] = {8, 8, 4, 4, 2, 2, 1},
	adam_dy[7] = {8, 8, 8, 4, 4, 2, 2};

/* The pixel formats of the imaging component share all but their last byte. */
static GUID pixel_format(BYTE last)
{
	GUID format = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, last}};
	return format;
}

static UINT channels_of(UINT kind)
{
	return kind == 2 ? 3 : kind == 4 ? 2 : kind == 6 ? 4 : 1;
}

PLAIN static void put(const BYTE *data, UINT size)
{
	for (UINT at = 0; at < size; ++at)
		file[file_size++] = data[at];
}

static void put32(UINT value)
{
	const BYTE bytes[4] = {(BYTE)(value >> 24), (BYTE)(value >> 16), (BYTE)(value >> 8), (BYTE)value};
	put(bytes, 4);
}

static UINT crc(const BYTE *data, UINT size)
{
	UINT value = 0xffffffff;
	for (UINT at = 0; at < size; ++at)
		value = crc_table[(value ^ data[at]) & 255] ^ (value >> 8);
	return value ^ 0xffffffff;
}

static void chunk(const char *name, const BYTE *body, UINT size, BOOL damaged)
{
	const UINT start = file_size + 4;
	put32(size);
	put((const BYTE *)name, 4);
	put(body, size);
	put32(crc(file + start, size + 4) ^ (damaged ? 1 : 0));
}

/* The rows of each pass, each after its filter byte (none), the samples packed from the high bits down as the format stores them. */
PLAIN static UINT raw_rows(const picture *p)
{
	UINT size = 0;
	for (UINT pass = 0; pass < (p->interlace ? 7u : 1u); ++pass) {
		const UINT start_x = p->interlace ? adam_x[pass] : 0, start_y = p->interlace ? adam_y[pass] : 0;
		const UINT step_x = p->interlace ? adam_dx[pass] : 1, step_y = p->interlace ? adam_dy[pass] : 1;
		if (start_x >= p->width || start_y >= p->height)
			continue;
		for (UINT y = start_y; y < p->height; y += step_y) {
			UINT bits = 0, held = 0;
			raw[size++] = 0;
			for (UINT x = start_x; x < p->width; x += step_x)
				for (UINT channel = 0; channel < channels_of(p->kind); ++channel) {
					const UINT value = p->sample(x, y, channel);
					if (p->depth == 16) {
						raw[size++] = (BYTE)(value >> 8);
						raw[size++] = (BYTE)value;
					}
					else if (p->depth == 8)
						raw[size++] = (BYTE)value;
					else {
						bits = bits << p->depth | value;
						held += p->depth;
						if (held == 8) {
							raw[size++] = (BYTE)bits;
							bits = held = 0;
						}
					}
				}
			if (held)
				raw[size++] = (BYTE)(bits << (8 - held));
		}
	}
	return size;
}

/* A zlib stream of one stored block. */
PLAIN static UINT zlib_stored(const BYTE *data, UINT size)
{
	UINT at = 0, a = 1, b = 0;
	packed[at++] = 0x78;
	packed[at++] = 0x01;
	packed[at++] = 1;
	packed[at++] = (BYTE)size;
	packed[at++] = (BYTE)(size >> 8);
	packed[at++] = (BYTE)~size;
	packed[at++] = (BYTE)(~size >> 8);
	for (UINT index = 0; index < size; ++index) {
		packed[at++] = data[index];
		a = (a + data[index]) % 65521;
		b = (b + a) % 65521;
	}
	packed[at++] = (BYTE)(b >> 8);
	packed[at++] = (BYTE)b;
	packed[at++] = (BYTE)(a >> 8);
	packed[at++] = (BYTE)a;
	return at;
}

/* A zlib stream of one block of fixed lengths (RFC 1951, 3.2.7): literals 0 to 254 of 8 bits, 255 and the end of 9, and one distance
   code of no bits, which says no distance follows. The lengths are told in the code lengths' own code: 8 in 1 bit, 0 and 9 in 2. */
static UINT bits_at, bits_held, bits_count;

static void put_bits(UINT value, UINT count)
{
	bits_held |= value << bits_count;
	bits_count += count;
	while (bits_count >= 8) {
		packed[bits_at++] = (BYTE)bits_held;
		bits_held >>= 8;
		bits_count -= 8;
	}
}

/* A Huffman code goes out from its first bit down. */
static void put_code(UINT code, UINT length)
{
	UINT reversed = 0;
	for (UINT bit = 0; bit < length; ++bit)
		reversed |= (code >> bit & 1) << (length - 1 - bit);
	put_bits(reversed, length);
}

static void put_literal(UINT symbol)
{
	if (symbol < 255)
		put_code(symbol, 8);
	else
		put_code(510 + (symbol - 255), 9);
}

static UINT zlib_literals(const BYTE *data, UINT size)
{
	static const UINT order_lengths[7] = {0, 0, 0, 2, 1, 0, 2};
	UINT a = 1, b = 0;
	bits_at = bits_held = bits_count = 0;
	packed[bits_at++] = 0x78;
	packed[bits_at++] = 0x01;
	put_bits(1, 1);
	put_bits(2, 2);
	put_bits(0, 5);
	put_bits(0, 5);
	put_bits(3, 4);
	for (UINT index = 0; index < 7; ++index)
		put_bits(order_lengths[index], 3);
	for (UINT symbol = 0; symbol < 257; ++symbol)
		put_code(symbol < 255 ? 0 : 3, symbol < 255 ? 1 : 2);
	put_code(2, 2);
	for (UINT index = 0; index < size; ++index) {
		put_literal(data[index]);
		a = (a + data[index]) % 65521;
		b = (b + a) % 65521;
	}
	put_literal(256);
	if (bits_count)
		put_bits(0, 8 - bits_count);
	packed[bits_at++] = (BYTE)(b >> 8);
	packed[bits_at++] = (BYTE)b;
	packed[bits_at++] = (BYTE)(a >> 8);
	packed[bits_at++] = (BYTE)a;
	return bits_at;
}

static void write_png(const picture *p, const BYTE *data, UINT size)
{
	static const BYTE signature[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
	const BYTE header[13] = {(BYTE)(p->width >> 24), (BYTE)(p->width >> 16), (BYTE)(p->width >> 8), (BYTE)p->width, (BYTE)(p->height >> 24),
		(BYTE)(p->height >> 16), (BYTE)(p->height >> 8), (BYTE)p->height, (BYTE)p->depth, (BYTE)p->kind, 0, 0, (BYTE)p->interlace};
	file_size = 0;
	put(signature, 8);
	chunk("IHDR", header, 13, FALSE);
	if (p->palette)
		chunk("PLTE", p->palette, p->colors * 3, FALSE);
	if (p->transparency)
		chunk("tRNS", p->transparency, p->transparent, FALSE);
	chunk("IDAT", packed, p->literals ? zlib_literals(data, size) : zlib_stored(data, size), p->damaged);
	chunk("IEND", NULL, 0, FALSE);
}

/* What the imaging component gives back for the picture: packed samples for palettes and gray of up to 8 bits (a gray file with a
   transparent level reads as a palette of its levels), little-endian words for 16 bits, and blue first for 8-bit colour. */
static UINT expect(const picture *p, BYTE *last_byte, UINT *bits_per_pixel)
{
	const UINT channels = channels_of(p->kind);
	const BOOL packed_samples = p->kind == 3 || (p->kind == 0 && p->depth <= 8);
	const UINT bits = packed_samples ? p->depth : p->kind == 0 ? (p->transparency ? 64 : 16) : p->kind == 2 ? (p->depth == 16 ? 48 : 24) :
		p->depth == 16 ? 64 : 32;
	const UINT stride = (p->width * bits + 7) / 8;
	*bits_per_pixel = bits;
	if (packed_samples)
		*last_byte = p->kind == 3 || p->transparency ? (p->depth == 1 ? 0x01 : p->depth == 2 ? 0x02 : p->depth == 4 ? 0x03 : 0x04) :
			p->depth == 1 ? 0x05 : p->depth == 2 ? 0x06 : p->depth == 4 ? 0x07 : 0x08;
	else
		*last_byte = p->kind == 0 ? (p->transparency ? 0x16 : 0x0b) : p->kind == 2 ? (p->depth == 16 ? 0x15 : 0x0c) : p->depth == 16 ? 0x16 : 0x0f;
	for (UINT y = 0; y < p->height; ++y) {
		BYTE *row = expected + y * stride;
		for (UINT x = 0; x < p->width; ++x) {
			if (packed_samples) {
				const UINT bit = x * p->depth;
				if (!(bit % 8))
					row[bit / 8] = 0;
				row[bit / 8] |= (BYTE)(p->sample(x, y, 0) << (8 - p->depth - bit % 8));
			}
			else if (p->depth == 16) {
				const UINT gray = p->sample(x, y, 0);
				UINT words[4];
				if (p->kind == 0 && !p->transparency) {
					row[x * 2] = (BYTE)gray;
					row[x * 2 + 1] = (BYTE)(gray >> 8);
					continue;
				}
				words[0] = gray;
				words[1] = p->kind == 2 || p->kind == 6 ? p->sample(x, y, 1) : gray;
				words[2] = p->kind == 2 || p->kind == 6 ? p->sample(x, y, 2) : gray;
				words[3] = p->kind == 6 ? p->sample(x, y, 3) : p->kind == 4 ? p->sample(x, y, 1) :
					(p->transparency && gray == (UINT)(p->transparency[0] << 8 | p->transparency[1]) ? 0 : 0xffff);
				for (UINT word = 0; word < (p->kind == 2 ? 3u : 4u); ++word) {
					row[(x * (p->kind == 2 ? 3 : 4) + word) * 2] = (BYTE)words[word];
					row[(x * (p->kind == 2 ? 3 : 4) + word) * 2 + 1] = (BYTE)(words[word] >> 8);
				}
			}
			else {
				const UINT pixel = channels == 3 ? 3 : 4;
				const UINT red = p->sample(x, y, 0), green = channels >= 3 ? p->sample(x, y, 1) : red, blue = channels >= 3 ? p->sample(x, y, 2) : red;
				row[x * pixel] = (BYTE)blue;
				row[x * pixel + 1] = (BYTE)green;
				row[x * pixel + 2] = (BYTE)red;
				if (pixel == 4)
					row[x * pixel + 3] = (BYTE)(channels == 4 ? p->sample(x, y, 3) : p->sample(x, y, 1));
			}
		}
	}
	return stride;
}

PLAIN static int same(const BYTE *a, const BYTE *b, UINT size)
{
	for (UINT at = 0; at < size; ++at)
		if (a[at] != b[at])
			return 0;
	return 1;
}

/* Decodes the file just written and checks it; 0 when all of it comes back. */
static int check(IWICImagingFactory *factory, const picture *p, const WICColor *colors, UINT color_count)
{
	IWICStream *stream = NULL;
	IWICBitmapDecoder *decoder = NULL;
	IWICBitmapFrameDecode *frame = NULL;
	WICPixelFormatGUID format;
	UINT width = 0, height = 0, bits = 0;
	BYTE last = 0;
	const UINT stride = expect(p, &last, &bits);
	const GUID wanted = pixel_format(last);
	if (IWICImagingFactory_CreateStream(factory, &stream) != S_OK || IWICStream_InitializeFromMemory(stream, file, file_size) != S_OK)
		return 1;
	if (IWICImagingFactory_CreateDecoderFromStream(factory, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder) != S_OK ||
		IWICBitmapDecoder_GetFrame(decoder, 0, &frame) != S_OK)
		return 2;
	if (IWICBitmapFrameDecode_GetSize(frame, &width, &height) != S_OK || width != p->width || height != p->height)
		return 3;
	if (IWICBitmapFrameDecode_GetPixelFormat(frame, &format) != S_OK || !same((const BYTE *)&format, (const BYTE *)&wanted, sizeof(GUID)))
		return 4;
	if (IWICBitmapFrameDecode_CopyPixels(frame, NULL, stride, stride * p->height, pixels) != S_OK || !same(pixels, expected, stride * p->height))
		return 5;
	if (colors) {
		IWICPalette *palette = NULL;
		WICColor back[256];
		UINT count = 0;
		if (IWICImagingFactory_CreatePalette(factory, &palette) != S_OK || IWICBitmapFrameDecode_CopyPalette(frame, palette) != S_OK ||
			IWICPalette_GetColors(palette, 256, back, &count) != S_OK || count != color_count ||
			!same((const BYTE *)back, (const BYTE *)colors, count * sizeof(WICColor)))
			return 6;
		IWICPalette_Release(palette);
	}
	IWICBitmapFrameDecode_Release(frame);
	IWICBitmapDecoder_Release(decoder);
	IWICStream_Release(stream);
	return 0;
}

static UINT indexed_sample(UINT x, UINT y, UINT channel)
{
	(void)channel;
	return (x * 3 + y) % 4;
}

static UINT gray4_sample(UINT x, UINT y, UINT channel)
{
	(void)channel;
	return (x * 5 + y * 3) % 16;
}

static UINT gray8_sample(UINT x, UINT y, UINT channel)
{
	(void)channel;
	return (x * 41 + y * 13) % 256;
}

static UINT rgb16_sample(UINT x, UINT y, UINT channel)
{
	return (x * 20000 + y * 3000 + channel * 7777) & 0xffff;
}

static UINT gray_alpha16_sample(UINT x, UINT y, UINT channel)
{
	return channel ? (x * y * 1234 + 99) & 0xffff : (x * 7000 + y * 900) & 0xffff;
}

static UINT rgb8_sample(UINT x, UINT y, UINT channel)
{
	return (x * 60 + y * 20 + channel * 90) & 255;
}

static UINT rgba8_sample(UINT x, UINT y, UINT channel)
{
	return (x * 50 + y * 70 + channel * 33) & 255;
}

int raven_entry(void)
{
	static const BYTE palette[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 128, 128, 128}, alphas[2] = {0x00, 0x80}, gray_key[2] = {0, 7},
		gray16_key[2] = {0x55, 0x8c}, rgb_key[6] = {0, 1, 0, 2, 0, 3};
	static const WICColor palette_colors[4] = {0x00ff0000, 0x8000ff00, 0xff0000ff, 0xff808080};
	static WICColor levels[256];
	static const picture cases[] = {
		{7, 5, 2, 3, 1, indexed_sample, palette, alphas, 4, 2, FALSE},
		{5, 3, 4, 0, 0, gray4_sample, NULL, NULL, 0, 0, TRUE},
		{6, 4, 8, 0, 0, gray8_sample, NULL, gray_key, 0, 2, FALSE},
		{3, 2, 16, 2, 0, rgb16_sample, NULL, NULL, 0, 0, FALSE},
		{9, 9, 16, 4, 1, gray_alpha16_sample, NULL, NULL, 0, 0, FALSE},
		{4, 3, 8, 2, 0, rgb8_sample, NULL, rgb_key, 0, 6, FALSE},
		{5, 2, 8, 6, 0, rgba8_sample, NULL, NULL, 0, 0, FALSE},
		{4, 2, 16, 0, 0, gray_alpha16_sample, NULL, gray16_key, 0, 2, FALSE},
		{16, 16, 8, 3, 0, indexed_sample, palette, alphas, 4, 2, FALSE, TRUE},
	};
	IWICImagingFactory *factory = NULL;
	for (UINT index = 0; index < 256; ++index) {
		UINT value = index;
		for (int bit = 0; bit < 8; ++bit)
			value = value & 1 ? 0xedb88320 ^ (value >> 1) : value >> 1;
		crc_table[index] = value;
		levels[index] = (index == 7 ? 0 : 0xff000000) | index << 16 | index << 8 | index;
	}
	if (FAILED(CoInitialize(NULL)) ||
		CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory) != S_OK)
		return 1;
	for (UINT index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
		const picture *p = &cases[index];
		int failed;
		write_png(p, raw, raw_rows(p));
		if ((failed = check(factory, p, p->kind == 3 ? palette_colors : p->kind == 0 && p->depth == 8 && p->transparency ? levels : NULL,
				p->kind == 3 ? 4 : 256)))
			return 10 * (int)(index + 1) + failed;
	}
#ifndef _WIN64
	{
		/* 65536 by 65537 pixels of colour with alpha: sixteen gigabytes, which a 32-bit program cannot hold. The file itself is small. */
		static const picture huge = {65536, 65537, 8, 6, 0, rgba8_sample, NULL, NULL, 0, 0, FALSE};
		IWICStream *stream = NULL;
		IWICBitmapDecoder *decoder = NULL;
		IWICBitmapFrameDecode *frame = NULL;
		BYTE row[16];
		for (UINT index = 0; index < sizeof raw; ++index)
			raw[index] = 0;
		write_png(&huge, raw, sizeof raw);
		if (IWICImagingFactory_CreateStream(factory, &stream) != S_OK || IWICStream_InitializeFromMemory(stream, file, file_size) != S_OK)
			return 91;
		if (IWICImagingFactory_CreateDecoderFromStream(factory, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder) == S_OK &&
			IWICBitmapDecoder_GetFrame(decoder, 0, &frame) == S_OK)
		{
			WICRect corner = {0, 0, 4, 1};
			if (IWICBitmapFrameDecode_CopyPixels(frame, &corner, 16, sizeof row, row) == S_OK)
				return 92;
		}
		if (frame)
			IWICBitmapFrameDecode_Release(frame);
		if (decoder)
			IWICBitmapDecoder_Release(decoder);
		IWICStream_Release(stream);
	}
#endif
	IWICImagingFactory_Release(factory);
	CoUninitialize();
	return 0;
}
