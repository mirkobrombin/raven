/* SPDX-License-Identifier: MIT */
#include "wic.h"

/* Encoders keep a frame's rows in memory and write the file when the frame commits. PNG is written with stored deflate blocks. */
typedef struct
{
	IPropertyBag2 iface;
	LONG references;
	const wic_codec *codec;
} property_bag;

/* The options each encoder names; asked for, they hold their defaults. */
typedef struct
{
	const WCHAR *name;
	VARTYPE type;
	float value;
} bag_option;

static const bag_option png_options[] = {{L"InterlaceOption", VT_BOOL}, {L"FilterOption", VT_UI1}};
static const bag_option bmp_options[] = {{L"EnableV5Header32bppBGRA", VT_BOOL}};
static const bag_option tiff_options[] = {{L"CompressionQuality", VT_R4}, {L"TiffCompressionMethod", VT_UI1}};
static const bag_option jpeg_options[] = {{L"ImageQuality", VT_R4}, {L"BitmapTransform", VT_UI1}, {L"Luminance", VT_I4 | VT_ARRAY}, {L"Chrominance", VT_I4 | VT_ARRAY},
	{L"JpegYCrCbSubsampling", VT_UI1}, {L"SuppressApp0", VT_BOOL}};

static const bag_option *bag_options(const property_bag *self, ULONG *count)
{
	const GUID *container = self->codec->container;
	if (IsEqualGUID(container, &GUID_ContainerFormatPng)) {
		*count = ARRAYSIZE(png_options);
		return png_options;
	}
	if (IsEqualGUID(container, &GUID_ContainerFormatBmp)) {
		*count = ARRAYSIZE(bmp_options);
		return bmp_options;
	}
	if (IsEqualGUID(container, &GUID_ContainerFormatTiff)) {
		*count = ARRAYSIZE(tiff_options);
		return tiff_options;
	}
	if (IsEqualGUID(container, &GUID_ContainerFormatJpeg)) {
		*count = ARRAYSIZE(jpeg_options);
		return jpeg_options;
	}
	*count = 0;
	return NULL;
}

typedef struct encoder encoder;

typedef struct
{
	IWICBitmapFrameEncode iface;
	LONG references;
	encoder *owner;
	UINT width, height, stride, lines;
	BOOL has_size, has_format, committed, started;
	GUID format;
	double dpi_x, dpi_y;
	WICColor colors[256];
	UINT color_count;
	BYTE *pixels;
} frame;

struct encoder
{
	IWICBitmapEncoder iface;
	LONG references;
	const wic_codec *codec;
	IStream *stream;
	UINT frames;
	BOOL committed;
	WICColor colors[256];
	UINT color_count;
	wic_tiff_writer *tiff;
	BOOL committed_header;
};

static HRESULT WINAPI bag_QueryInterface(IPropertyBag2 *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IPropertyBag2)) {
		*out = iface;
		IPropertyBag2_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI bag_AddRef(IPropertyBag2 *iface)
{
	return InterlockedIncrement(&WIC_CONTAINING(iface, property_bag, iface)->references);
}

static ULONG WINAPI bag_Release(IPropertyBag2 *iface)
{
	property_bag *self = WIC_CONTAINING(iface, property_bag, iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left)
		wic_free(self);
	return left;
}

static HRESULT WINAPI bag_Read(IPropertyBag2 *iface, ULONG count, PROPBAG2 *bags, IErrorLog *log, VARIANT *values, HRESULT *errors)
{
	property_bag *self = WIC_CONTAINING(iface, property_bag, iface);
	ULONG known, index, option;
	const bag_option *options = bag_options(self, &known);
	HRESULT result = S_OK;
	for (index = 0; index < count; index++) {
		errors[index] = E_FAIL;
		for (option = 0; option < known; option++)
			if (bags[index].pstrName && !lstrcmpW(bags[index].pstrName, options[option].name)) {
				/* Floating options and tables stay unset until written. */
				const BOOL unset = options[option].type == VT_R4 || (options[option].type & VT_ARRAY);
				values[index].vt = unset ? VT_EMPTY : options[option].type;
				values[index].llVal = 0;
				errors[index] = S_OK;
			}
		if (FAILED(errors[index]))
			result = E_FAIL;
	}
	return result;
}

/* No encoder here has an option that changes what it writes, so values are accepted and left unread. */
static HRESULT WINAPI bag_Write(IPropertyBag2 *iface, ULONG count, PROPBAG2 *bags, VARIANT *values)
{
	return S_OK;
}

static HRESULT WINAPI bag_CountProperties(IPropertyBag2 *iface, ULONG *count)
{
	if (!count)
		return E_INVALIDARG;
	bag_options(WIC_CONTAINING(iface, property_bag, iface), count);
	return S_OK;
}

static HRESULT WINAPI bag_GetPropertyInfo(IPropertyBag2 *iface, ULONG first, ULONG count, PROPBAG2 *bags, ULONG *written)
{
	ULONG known, index;
	const bag_option *options = bag_options(WIC_CONTAINING(iface, property_bag, iface), &known);
	if (!bags || !written)
		return E_INVALIDARG;
	if (first >= known || count > known - first)
		return WINCODEC_ERR_VALUEOUTOFRANGE;
	for (index = 0; index < count; index++) {
		const SIZE_T size = (lstrlenW(options[first + index].name) + 1) * sizeof(WCHAR);
		memset(&bags[index], 0, sizeof(bags[index]));
		bags[index].dwType = PROPBAG2_TYPE_DATA;
		bags[index].vt = options[first + index].type;
		if (!(bags[index].pstrName = CoTaskMemAlloc(size)))
			return E_OUTOFMEMORY;
		memcpy(bags[index].pstrName, options[first + index].name, size);
	}
	*written = count;
	return S_OK;
}

static HRESULT WINAPI bag_LoadObject(IPropertyBag2 *iface, LPCOLESTR name, DWORD hint, IUnknown *object, IErrorLog *log)
{
	return E_NOTIMPL;
}

static const IPropertyBag2Vtbl bag_vtbl = {
	bag_QueryInterface, bag_AddRef, bag_Release, bag_Read, bag_Write, bag_CountProperties, bag_GetPropertyInfo, bag_LoadObject,
};

static frame *frame_of(IWICBitmapFrameEncode *iface)
{
	return WIC_CONTAINING(iface, frame, iface);
}

static encoder *encoder_of(IWICBitmapEncoder *iface)
{
	return WIC_CONTAINING(iface, encoder, iface);
}

static HRESULT put(IStream *stream, const void *data, SIZE_T size)
{
	ULONG written = 0;
	return IStream_Write(stream, data, (ULONG)size, &written);
}

static void set_u16(BYTE *to, UINT value)
{
	to[0] = (BYTE)value;
	to[1] = (BYTE)(value >> 8);
}

static void set_u32(BYTE *to, UINT value)
{
	set_u16(to, value);
	set_u16(to + 2, value >> 16);
}

static void set_be32(BYTE *to, UINT value)
{
	to[0] = (BYTE)(value >> 24);
	to[1] = (BYTE)(value >> 16);
	to[2] = (BYTE)(value >> 8);
	to[3] = (BYTE)value;
}

static HRESULT write_bmp(frame *self, IStream *stream)
{
	const UINT bits = wic_format_bits(&self->format), row = (self->width * bits + 31) / 32 * 4;
	const UINT colors = bits <= 8 ? (self->color_count ? self->color_count : 1U << bits) : 0;
	const BOOL masks = IsEqualGUID(&self->format, &GUID_WICPixelFormat16bppBGR565);
	const UINT head = 14 + 40 + (masks ? 12 : 0) + colors * 4, image = row * self->height;
	BYTE header[66] = {'B', 'M'};
	BYTE *line;
	UINT index, y;
	HRESULT result;
	set_u32(header + 2, head + image);
	set_u32(header + 10, head);
	set_u32(header + 14, 40);
	set_u32(header + 18, self->width);
	set_u32(header + 22, self->height);
	set_u16(header + 26, 1);
	set_u16(header + 28, bits);
	set_u32(header + 30, masks ? BI_BITFIELDS : BI_RGB);
	set_u32(header + 34, image);
	set_u32(header + 38, (UINT)(self->dpi_x / 0.0254 + 0.5));
	set_u32(header + 42, (UINT)(self->dpi_y / 0.0254 + 0.5));
	set_u32(header + 46, colors);
	set_u32(header + 50, colors);
	set_u32(header + 54, 0xf800);
	set_u32(header + 58, 0x7e0);
	set_u32(header + 62, 0x1f);
	if (FAILED(result = put(stream, header, 54 + (masks ? 12 : 0))))
		return result;
	for (index = 0; index < colors; index++) {
		BYTE entry[4];
		set_u32(entry, self->colors[index] & 0xffffff);
		if (FAILED(result = put(stream, entry, 4)))
			return result;
	}
	if (!(line = wic_alloc(row)))
		return E_OUTOFMEMORY;
	for (y = self->height; y-- && SUCCEEDED(result);) {
		memcpy(line, self->pixels + (SIZE_T)y * self->stride, min(row, self->stride));
		result = put(stream, line, row);
	}
	wic_free(line);
	return result;
}

static UINT crc_of(const BYTE *data, SIZE_T size, UINT crc)
{
	SIZE_T at;
	for (at = 0; at < size; at++) {
		int bit;
		crc ^= data[at];
		for (bit = 0; bit < 8; bit++)
			crc = crc & 1 ? 0xedb88320 ^ (crc >> 1) : crc >> 1;
	}
	return crc;
}

static HRESULT png_chunk(IStream *stream, const char *name, const BYTE *body, SIZE_T size)
{
	BYTE head[8], tail[4];
	UINT crc;
	HRESULT result;
	set_be32(head, (UINT)size);
	memcpy(head + 4, name, 4);
	crc = crc_of(head + 4, 4, 0xffffffff);
	crc = crc_of(body, size, crc);
	set_be32(tail, ~crc);
	if (FAILED(result = put(stream, head, 8)) || (size && FAILED(result = put(stream, body, size))))
		return result;
	return put(stream, tail, 4);
}

static HRESULT write_png(frame *self, IStream *stream)
{
	static const BYTE signature[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
	const UINT bits = wic_format_bits(&self->format), row = (self->width * bits + 7) / 8;
	const SIZE_T raw_size = (SIZE_T)(row + 1) * self->height;
	const SIZE_T blocks = raw_size / 65535 + 1, packed_size = 2 + raw_size + blocks * 5 + 4;
	BYTE header[13], physical[9];
	BYTE *raw, *packed, *to;
	UINT depth = 8, kind = 6, y, x, a = 1, b = 0;
	SIZE_T at;
	HRESULT result;
	if (IsEqualGUID(&self->format, &GUID_WICPixelFormat24bppBGR))
		kind = 2;
	else if (IsEqualGUID(&self->format, &GUID_WICPixelFormat48bppRGB)) {
		kind = 2;
		depth = 16;
	}
	else if (IsEqualGUID(&self->format, &GUID_WICPixelFormat64bppRGBA))
		depth = 16;
	else if (IsEqualGUID(&self->format, &GUID_WICPixelFormat16bppGray)) {
		kind = 0;
		depth = 16;
	}
	else if (bits <= 8) {
		const BOOL gray = IsEqualGUID(&self->format, &GUID_WICPixelFormatBlackWhite) || IsEqualGUID(&self->format, &GUID_WICPixelFormat2bppGray)
			|| IsEqualGUID(&self->format, &GUID_WICPixelFormat4bppGray) || IsEqualGUID(&self->format, &GUID_WICPixelFormat8bppGray);
		kind = gray ? 0 : 3;
		depth = bits;
	}
	raw = wic_alloc(raw_size);
	packed = wic_alloc(packed_size);
	if (!raw || !packed) {
		wic_free(raw);
		wic_free(packed);
		return E_OUTOFMEMORY;
	}
	for (y = 0; y < self->height; y++) {
		BYTE *line = raw + (SIZE_T)y * (row + 1) + 1;
		memcpy(line, self->pixels + (SIZE_T)y * self->stride, row);
		if (depth == 8 && kind == 2)
			for (x = 0; x < self->width; x++) {
				const BYTE swap = line[x * 3];
				line[x * 3] = line[x * 3 + 2];
				line[x * 3 + 2] = swap;
			}
		else if (depth == 8 && kind == 6)
			for (x = 0; x < self->width; x++) {
				const BYTE swap = line[x * 4];
				line[x * 4] = line[x * 4 + 2];
				line[x * 4 + 2] = swap;
			}
		else if (depth == 16)
			for (x = 0; x + 1 < row; x += 2) {
				const BYTE swap = line[x];
				line[x] = line[x + 1];
				line[x + 1] = swap;
			}
	}
	to = packed;
	*to++ = 0x78;
	*to++ = 1;
	for (at = 0; at < raw_size || at == 0;) {
		const UINT count = (UINT)min(raw_size - at, 65535);
		*to++ = at + count >= raw_size;
		set_u16(to, count);
		set_u16(to + 2, ~count & 0xffff);
		to += 4;
		memcpy(to, raw + at, count);
		to += count;
		at += count;
		if (!count)
			break;
	}
	for (at = 0; at < raw_size; at++) {
		a = (a + raw[at]) % 65521;
		b = (b + a) % 65521;
	}
	set_be32(to, b << 16 | a);
	to += 4;
	set_be32(header, self->width);
	set_be32(header + 4, self->height);
	header[8] = (BYTE)depth;
	header[9] = (BYTE)kind;
	header[10] = header[11] = header[12] = 0;
	set_be32(physical, (UINT)(self->dpi_x / 0.0254 + 0.5));
	set_be32(physical + 4, (UINT)(self->dpi_y / 0.0254 + 0.5));
	physical[8] = 1;
	result = put(stream, signature, 8);
	if (SUCCEEDED(result))
		result = png_chunk(stream, "IHDR", header, 13);
	if (SUCCEEDED(result))
		result = png_chunk(stream, "pHYs", physical, 9);
	if (SUCCEEDED(result) && kind == 3) {
		BYTE table[768];
		UINT index;
		for (index = 0; index < self->color_count; index++) {
			table[index * 3] = (BYTE)(self->colors[index] >> 16);
			table[index * 3 + 1] = (BYTE)(self->colors[index] >> 8);
			table[index * 3 + 2] = (BYTE)self->colors[index];
		}
		result = png_chunk(stream, "PLTE", table, (SIZE_T)self->color_count * 3);
		{
			/* Colors that are not opaque keep their alpha in a chunk of its own. */
			BYTE alphas[256];
			UINT used = 0, index;
			for (index = 0; index < self->color_count; index++) {
				alphas[index] = (BYTE)(self->colors[index] >> 24);
				if (alphas[index] != 0xff)
					used = index + 1;
			}
			if (used && SUCCEEDED(result))
				result = png_chunk(stream, "tRNS", alphas, used);
		}
	}
	if (SUCCEEDED(result))
		result = png_chunk(stream, "IDAT", packed, (SIZE_T)(to - packed));
	if (SUCCEEDED(result))
		result = png_chunk(stream, "IEND", NULL, 0);
	wic_free(raw);
	wic_free(packed);
	return result;
}

static HRESULT WINAPI frame_QueryInterface(IWICBitmapFrameEncode *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICBitmapFrameEncode)) {
		*out = iface;
		IWICBitmapFrameEncode_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI frame_AddRef(IWICBitmapFrameEncode *iface)
{
	return InterlockedIncrement(&frame_of(iface)->references);
}

static ULONG WINAPI frame_Release(IWICBitmapFrameEncode *iface)
{
	frame *self = frame_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		IWICBitmapEncoder_Release(&self->owner->iface);
		wic_free(self->pixels);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI frame_Initialize(IWICBitmapFrameEncode *iface, IPropertyBag2 *options)
{
	frame_of(iface)->started = TRUE;
	return S_OK;
}

static HRESULT WINAPI frame_SetSize(IWICBitmapFrameEncode *iface, UINT width, UINT height)
{
	frame *self = frame_of(iface);
	if (!width || !height)
		return E_INVALIDARG;
	self->width = width;
	self->height = height;
	self->has_size = TRUE;
	return S_OK;
}

static HRESULT WINAPI frame_SetResolution(IWICBitmapFrameEncode *iface, double x, double y)
{
	frame *self = frame_of(iface);
	self->dpi_x = x;
	self->dpi_y = y;
	return S_OK;
}

/* GIF's variable-width codes, packed low bit first into blocks of at most 255 bytes. */
typedef struct
{
	IStream *stream;
	BYTE block[256];
	UINT used, pending, pending_bits;
	HRESULT result;
} code_writer;

static void codes_flush(code_writer *out)
{
	ULONG written;
	if (out->used && SUCCEEDED(out->result)) {
		out->block[0] = (BYTE)out->used;
		out->result = IStream_Write(out->stream, out->block, out->used + 1, &written);
	}
	out->used = 0;
}

static void codes_put(code_writer *out, UINT code, UINT width)
{
	out->pending |= code << out->pending_bits;
	out->pending_bits += width;
	while (out->pending_bits >= 8) {
		out->block[++out->used] = (BYTE)out->pending;
		out->pending >>= 8;
		out->pending_bits -= 8;
		if (out->used == 255)
			codes_flush(out);
	}
}

static HRESULT write_lzw(IStream *stream, const BYTE *pixels, UINT width, UINT height, UINT stride)
{
	enum { slots = 1 << 14 };
	const UINT clear = 256, end = 257;
	code_writer out = {stream};
	const BYTE minimum = 8;
	ULONG *table = wic_alloc(slots * sizeof(ULONG));
	UINT next = end + 1, code_width = 9, x, y;
	INT current = -1;
	ULONG written;
	if (!table)
		return E_OUTOFMEMORY;
	IStream_Write(stream, &minimum, 1, &written);
	codes_put(&out, clear, code_width);
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++) {
			const BYTE value = pixels[(SIZE_T)y * stride + x];
			const ULONG key = (ULONG)current << 8 | value;
			UINT slot;
			if (current < 0) {
				current = value;
				continue;
			}
			/* An entry holds its key above the code it stands for; empty entries are zero. */
			for (slot = (key * 2654435761U) >> 18; table[slot] && table[slot] >> 12 != key; slot = (slot + 1) & (slots - 1))
				;
			if (table[slot]) {
				current = table[slot] & 0xfff;
				continue;
			}
			codes_put(&out, (UINT)current, code_width);
			if (next < 4096) {
				if (next == 1U << code_width)
					code_width++;
				table[slot] = key << 12 | next++;
			}
			else {
				codes_put(&out, clear, code_width);
				memset(table, 0, slots * sizeof(ULONG));
				next = end + 1;
				code_width = 9;
			}
			current = value;
		}
	if (current >= 0)
		codes_put(&out, (UINT)current, code_width);
	codes_put(&out, end, code_width);
	if (out.pending_bits)
		codes_put(&out, 0, 8 - out.pending_bits);
	codes_flush(&out);
	wic_free(table);
	if (SUCCEEDED(out.result)) {
		const BYTE terminator = 0;
		out.result = IStream_Write(stream, &terminator, 1, &written);
	}
	return out.result;
}

static HRESULT write_color_table(IStream *stream, const WICColor *colors, UINT count)
{
	BYTE table[768] = {0};
	UINT index;
	ULONG written;
	for (index = 0; index < count; index++) {
		table[index * 3] = (BYTE)(colors[index] >> 16);
		table[index * 3 + 1] = (BYTE)(colors[index] >> 8);
		table[index * 3 + 2] = (BYTE)colors[index];
	}
	return IStream_Write(stream, table, sizeof(table), &written);
}

/* The first frame opens the file with the screen and the encoder's palette; a frame with a palette of its own carries it. */
static HRESULT write_gif(frame *self, IStream *stream)
{
	encoder *owner = self->owner;
	BYTE descriptor[10] = {0x2c};
	ULONG written;
	HRESULT result;
	if (!owner->committed_header) {
		BYTE header[13] = {'G', 'I', 'F', '8', '9', 'a'};
		header[6] = (BYTE)self->width;
		header[7] = (BYTE)(self->width >> 8);
		header[8] = (BYTE)self->height;
		header[9] = (BYTE)(self->height >> 8);
		header[10] = owner->color_count ? 0xf7 : 0x70;
		if (FAILED(result = IStream_Write(stream, header, sizeof(header), &written)))
			return result;
		if (owner->color_count && FAILED(result = write_color_table(stream, owner->colors, owner->color_count)))
			return result;
		owner->committed_header = TRUE;
	}
	descriptor[5] = (BYTE)self->width;
	descriptor[6] = (BYTE)(self->width >> 8);
	descriptor[7] = (BYTE)self->height;
	descriptor[8] = (BYTE)(self->height >> 8);
	descriptor[9] = self->color_count ? 0x87 : 0;
	if (FAILED(result = IStream_Write(stream, descriptor, sizeof(descriptor), &written)))
		return result;
	if (self->color_count && FAILED(result = write_color_table(stream, self->colors, self->color_count)))
		return result;
	return write_lzw(stream, self->pixels, self->width, self->height, self->stride);
}

static BOOL format_writable(const wic_codec *codec, const GUID *format)
{
	static const GUID *const bmp[] = {&GUID_WICPixelFormat1bppIndexed, &GUID_WICPixelFormat4bppIndexed, &GUID_WICPixelFormat8bppIndexed,
		&GUID_WICPixelFormat16bppBGR555, &GUID_WICPixelFormat16bppBGR565, &GUID_WICPixelFormat24bppBGR, &GUID_WICPixelFormat32bppBGR, &GUID_WICPixelFormat32bppBGRA};
	static const GUID *const png[] = {&GUID_WICPixelFormatBlackWhite, &GUID_WICPixelFormat2bppGray, &GUID_WICPixelFormat4bppGray, &GUID_WICPixelFormat8bppGray,
		&GUID_WICPixelFormat16bppGray, &GUID_WICPixelFormat1bppIndexed, &GUID_WICPixelFormat2bppIndexed, &GUID_WICPixelFormat4bppIndexed, &GUID_WICPixelFormat8bppIndexed,
		&GUID_WICPixelFormat24bppBGR, &GUID_WICPixelFormat32bppBGRA, &GUID_WICPixelFormat48bppRGB, &GUID_WICPixelFormat64bppRGBA};
	static const GUID *const jpeg[] = {&GUID_WICPixelFormat8bppGray, &GUID_WICPixelFormat24bppBGR, &GUID_WICPixelFormat32bppCMYK};
	const BOOL is_png = IsEqualGUID(codec->container, &GUID_ContainerFormatPng);
	const GUID *const *list = is_png ? png : bmp;
	const UINT count = is_png ? ARRAYSIZE(png) : ARRAYSIZE(bmp);
	static const GUID *const tiff[] = {&GUID_WICPixelFormatBlackWhite, &GUID_WICPixelFormat1bppIndexed, &GUID_WICPixelFormat4bppIndexed, &GUID_WICPixelFormat8bppIndexed,
		&GUID_WICPixelFormat4bppGray, &GUID_WICPixelFormat8bppGray, &GUID_WICPixelFormat24bppBGR, &GUID_WICPixelFormat32bppBGRA, &GUID_WICPixelFormat32bppCMYK};
	UINT index;
	if (IsEqualGUID(codec->container, &GUID_ContainerFormatGif))
		return IsEqualGUID(format, &GUID_WICPixelFormat8bppIndexed);
	if (IsEqualGUID(codec->container, &GUID_ContainerFormatTiff)) {
		for (index = 0; index < ARRAYSIZE(tiff); index++)
			if (IsEqualGUID(format, tiff[index]))
				return TRUE;
		return FALSE;
	}
	if (IsEqualGUID(codec->container, &GUID_ContainerFormatJpeg)) {
		for (index = 0; index < ARRAYSIZE(jpeg); index++)
			if (IsEqualGUID(format, jpeg[index]))
				return TRUE;
		return FALSE;
	}
	for (index = 0; index < count; index++)
		if (IsEqualGUID(format, list[index]))
			return TRUE;
	return FALSE;
}

static HRESULT WINAPI frame_SetPixelFormat(IWICBitmapFrameEncode *iface, WICPixelFormatGUID *format)
{
	frame *self = frame_of(iface);
	if (!format)
		return E_INVALIDARG;
	if (!format_writable(self->owner->codec, format)) {
		const GUID *container = self->owner->codec->container;
		if (IsEqualGUID(container, &GUID_ContainerFormatGif))
			*format = GUID_WICPixelFormat8bppIndexed;
		else if (IsEqualGUID(container, &GUID_ContainerFormatBmp) && IsEqualGUID(format, &GUID_WICPixelFormatBlackWhite))
			*format = GUID_WICPixelFormat1bppIndexed;
		else if (IsEqualGUID(container, &GUID_ContainerFormatBmp) && IsEqualGUID(format, &GUID_WICPixelFormat2bppIndexed))
			*format = GUID_WICPixelFormat4bppIndexed;
		else if (IsEqualGUID(container, &GUID_ContainerFormatPng) && IsEqualGUID(format, &GUID_WICPixelFormat32bppBGR))
			*format = GUID_WICPixelFormat24bppBGR;
		else if (IsEqualGUID(container, &GUID_ContainerFormatTiff) && IsEqualGUID(format, &GUID_WICPixelFormat2bppIndexed))
			*format = GUID_WICPixelFormat4bppIndexed;
		else if (IsEqualGUID(container, &GUID_ContainerFormatTiff) && IsEqualGUID(format, &GUID_WICPixelFormat2bppGray))
			*format = GUID_WICPixelFormat4bppGray;
		else
			*format = IsEqualGUID(container, &GUID_ContainerFormatJpeg) ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
	}
	self->format = *format;
	self->has_format = TRUE;
	return S_OK;
}

static HRESULT WINAPI frame_SetColorContexts(IWICBitmapFrameEncode *iface, UINT count, IWICColorContext **contexts)
{
	return WINCODEC_ERR_UNSUPPORTEDOPERATION;
}

static HRESULT WINAPI frame_SetPalette(IWICBitmapFrameEncode *iface, IWICPalette *palette)
{
	frame *self = frame_of(iface);
	if (!palette)
		return E_INVALIDARG;
	if (!self->started)
		return WINCODEC_ERR_NOTINITIALIZED;
	return IWICPalette_GetColors(palette, 256, self->colors, &self->color_count);
}

static HRESULT WINAPI frame_SetThumbnail(IWICBitmapFrameEncode *iface, IWICBitmapSource *thumbnail)
{
	return WINCODEC_ERR_UNSUPPORTEDOPERATION;
}

static HRESULT WINAPI frame_WritePixels(IWICBitmapFrameEncode *iface, UINT lines, UINT stride, UINT size, BYTE *pixels)
{
	frame *self = frame_of(iface);
	UINT y, row;
	if (!self->has_size || !self->has_format)
		return WINCODEC_ERR_WRONGSTATE;
	if (!pixels)
		return E_INVALIDARG;
	row = (self->width * wic_format_bits(&self->format) + 7) / 8;
	if (self->lines + lines > self->height || stride < row || (SIZE_T)size < (SIZE_T)stride * lines)
		return E_INVALIDARG;
	if (!self->pixels) {
		self->stride = row;
		if (!(self->pixels = wic_alloc((SIZE_T)row * self->height)))
			return E_OUTOFMEMORY;
	}
	for (y = 0; y < lines; y++)
		memcpy(self->pixels + (SIZE_T)(self->lines + y) * row, pixels + (SIZE_T)y * stride, row);
	self->lines += lines;
	return S_OK;
}

static HRESULT WINAPI frame_WriteSource(IWICBitmapFrameEncode *iface, IWICBitmapSource *source, WICRect *area)
{
	frame *self = frame_of(iface);
	IWICBitmapSource *converted = NULL;
	WICRect whole;
	GUID format;
	BYTE *rows;
	UINT width, height, row, size;
	HRESULT result;
	if (!source || (area && (area->Width <= 0 || area->Height <= 0)))
		return E_INVALIDARG;
	/* A frame nobody sized takes the size of what is written into it. */
	if (!self->has_size) {
		if (area)
			frame_SetSize(iface, area->Width > 0 ? (UINT)area->Width : 0, area->Height > 0 ? (UINT)area->Height : 0);
		else if (SUCCEEDED(IWICBitmapSource_GetSize(source, &width, &height)))
			frame_SetSize(iface, width, height);
		if (!self->has_size)
			return E_INVALIDARG;
	}
	if (!self->has_format) {
		if (FAILED(result = IWICBitmapSource_GetPixelFormat(source, &format)))
			return result;
		frame_SetPixelFormat(iface, &format);
	}
	if (FAILED(result = WICConvertBitmapSource(&self->format, source, &converted)))
		return result;
	IWICBitmapSource_GetSize(converted, &width, &height);
	if (!area) {
		whole.X = whole.Y = 0;
		whole.Width = (INT)width;
		whole.Height = (INT)height;
		area = &whole;
	}
	row = ((UINT)area->Width * wic_format_bits(&self->format) + 7) / 8;
	size = row * (UINT)area->Height;
	if (!(rows = wic_alloc(size)))
		result = E_OUTOFMEMORY;
	else {
		result = IWICBitmapSource_CopyPixels(converted, area, row, size, rows);
		if (SUCCEEDED(result)) {
			IWICPalette *palette;
			if (wic_format_bits(&self->format) <= 8 && !self->color_count && SUCCEEDED(wic_palette_create(&palette))) {
				if (SUCCEEDED(IWICBitmapSource_CopyPalette(source, palette)))
					frame_SetPalette(iface, palette);
				IWICPalette_Release(palette);
			}
			result = frame_WritePixels(iface, (UINT)area->Height, row, size, rows);
		}
		wic_free(rows);
	}
	IWICBitmapSource_Release(converted);
	return result;
}

static HRESULT WINAPI frame_Commit(IWICBitmapFrameEncode *iface)
{
	frame *self = frame_of(iface);
	HRESULT result;
	if (self->committed || !self->pixels || self->lines != self->height)
		return WINCODEC_ERR_WRONGSTATE;
	if (IsEqualGUID(self->owner->codec->container, &GUID_ContainerFormatGif))
		result = write_gif(self, self->owner->stream);
	else if (IsEqualGUID(self->owner->codec->container, &GUID_ContainerFormatTiff)) {
		if (!self->owner->tiff && !(self->owner->tiff = wic_tiff_begin()))
			return E_OUTOFMEMORY;
		result = wic_tiff_frame(self->owner->tiff, self->width, self->height, &self->format, self->dpi_x, self->dpi_y, self->colors, self->color_count, self->stride, self->pixels);
	}
	else if (IsEqualGUID(self->owner->codec->container, &GUID_ContainerFormatJpeg))
		result = wic_write_jpeg(self->owner->stream, self->width, self->height, &self->format, self->dpi_x, self->dpi_y, self->stride, self->pixels);
	else
		result = IsEqualGUID(self->owner->codec->container, &GUID_ContainerFormatPng) ? write_png(self, self->owner->stream) : write_bmp(self, self->owner->stream);
	if (SUCCEEDED(result))
		self->committed = TRUE;
	return result;
}

static HRESULT WINAPI frame_GetMetadataQueryWriter(IWICBitmapFrameEncode *iface, IWICMetadataQueryWriter **out)
{
	if (out)
		*out = NULL;
	return WINCODEC_ERR_UNSUPPORTEDOPERATION;
}

static const IWICBitmapFrameEncodeVtbl frame_vtbl = {
	frame_QueryInterface, frame_AddRef, frame_Release, frame_Initialize, frame_SetSize, frame_SetResolution, frame_SetPixelFormat, frame_SetColorContexts,
	frame_SetPalette, frame_SetThumbnail, frame_WritePixels, frame_WriteSource, frame_Commit, frame_GetMetadataQueryWriter,
};

static HRESULT WINAPI encoder_QueryInterface(IWICBitmapEncoder *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICBitmapEncoder)) {
		*out = iface;
		IWICBitmapEncoder_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI encoder_AddRef(IWICBitmapEncoder *iface)
{
	return InterlockedIncrement(&encoder_of(iface)->references);
}

static ULONG WINAPI encoder_Release(IWICBitmapEncoder *iface)
{
	encoder *self = encoder_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		if (self->stream)
			IStream_Release(self->stream);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI encoder_Initialize(IWICBitmapEncoder *iface, IStream *stream, WICBitmapEncoderCacheOption option)
{
	encoder *self = encoder_of(iface);
	if (!stream)
		return E_INVALIDARG;
	if (self->stream)
		return WINCODEC_ERR_WRONGSTATE;
	self->stream = stream;
	IStream_AddRef(stream);
	return S_OK;
}

static HRESULT WINAPI encoder_GetContainerFormat(IWICBitmapEncoder *iface, GUID *format)
{
	if (!format)
		return E_INVALIDARG;
	*format = *encoder_of(iface)->codec->container;
	return S_OK;
}

static HRESULT WINAPI encoder_GetEncoderInfo(IWICBitmapEncoder *iface, IWICBitmapEncoderInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	return wic_codec_info_create(encoder_of(iface)->codec, TRUE, out);
}

static HRESULT WINAPI encoder_SetColorContexts(IWICBitmapEncoder *iface, UINT count, IWICColorContext **contexts)
{
	return WINCODEC_ERR_UNSUPPORTEDOPERATION;
}

static HRESULT WINAPI encoder_SetPalette(IWICBitmapEncoder *iface, IWICPalette *palette)
{
	encoder *self = encoder_of(iface);
	if (!self->stream)
		return WINCODEC_ERR_NOTINITIALIZED;
	if (!IsEqualGUID(self->codec->container, &GUID_ContainerFormatGif))
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	return palette ? IWICPalette_GetColors(palette, 256, self->colors, &self->color_count) : E_INVALIDARG;
}

static HRESULT WINAPI encoder_SetSource(IWICBitmapEncoder *iface, IWICBitmapSource *source)
{
	return WINCODEC_ERR_UNSUPPORTEDOPERATION;
}

static HRESULT WINAPI encoder_CreateNewFrame(IWICBitmapEncoder *iface, IWICBitmapFrameEncode **out, IPropertyBag2 **options)
{
	encoder *self = encoder_of(iface);
	frame *created;
	if (!out)
		return E_INVALIDARG;
	if (!self->stream)
		return WINCODEC_ERR_NOTINITIALIZED;
	if (self->committed || (self->frames && !IsEqualGUID(self->codec->container, &GUID_ContainerFormatGif) && !IsEqualGUID(self->codec->container, &GUID_ContainerFormatTiff)))
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	if (!(created = wic_alloc(sizeof(*created))))
		return E_OUTOFMEMORY;
	if (options) {
		property_bag *bag = wic_alloc(sizeof(*bag));
		if (!bag) {
			wic_free(created);
			return E_OUTOFMEMORY;
		}
		bag->iface.lpVtbl = &bag_vtbl;
		bag->references = 1;
		bag->codec = self->codec;
		*options = &bag->iface;
	}
	created->iface.lpVtbl = &frame_vtbl;
	created->references = 1;
	created->owner = self;
	created->dpi_x = created->dpi_y = 96.0;
	IWICBitmapEncoder_AddRef(iface);
	self->frames++;
	*out = &created->iface;
	return S_OK;
}

static HRESULT WINAPI encoder_Commit(IWICBitmapEncoder *iface)
{
	encoder *self = encoder_of(iface);
	if (!self->stream || self->committed || !self->frames)
		return WINCODEC_ERR_WRONGSTATE;
	self->committed = TRUE;
	if (self->tiff) {
		const HRESULT result = wic_tiff_end(self->tiff, self->stream);
		self->tiff = NULL;
		return result;
	}
	if (IsEqualGUID(self->codec->container, &GUID_ContainerFormatGif)) {
		const BYTE trailer = 0x3b;
		ULONG written;
		return IStream_Write(self->stream, &trailer, 1, &written);
	}
	return S_OK;
}

static HRESULT WINAPI encoder_GetMetadataQueryWriter(IWICBitmapEncoder *iface, IWICMetadataQueryWriter **out)
{
	if (out)
		*out = NULL;
	return WINCODEC_ERR_UNSUPPORTEDOPERATION;
}

static const IWICBitmapEncoderVtbl encoder_vtbl = {
	encoder_QueryInterface, encoder_AddRef, encoder_Release, encoder_Initialize, encoder_GetContainerFormat, encoder_GetEncoderInfo, encoder_SetColorContexts,
	encoder_SetPalette, encoder_SetSource, encoder_SetSource, encoder_CreateNewFrame, encoder_Commit, encoder_GetMetadataQueryWriter,
};

HRESULT wic_encoder_create(const wic_codec *codec, IWICBitmapEncoder **out)
{
	encoder *self;
	if (!codec->encoder_class)
		return WINCODEC_ERR_COMPONENTNOTFOUND;
	if (!(self = wic_alloc(sizeof(*self))))
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &encoder_vtbl;
	self->references = 1;
	self->codec = codec;
	*out = &self->iface;
	return S_OK;
}

/* The exported helper lets an empty rectangle pass as nothing to do, where the method itself refuses it. */
HRESULT WINAPI IWICBitmapFrameEncode_WriteSource_Proxy(IWICBitmapFrameEncode *iface, IWICBitmapSource *source, WICRect *area)
{
	if (area && (area->Width <= 0 || area->Height <= 0))
		return S_OK;
	return IWICBitmapFrameEncode_WriteSource(iface, source, area);
}
