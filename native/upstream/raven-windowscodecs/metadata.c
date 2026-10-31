/* SPDX-License-Identifier: MIT */
#include "wic.h"

/* One object serves as reader and writer of every metadata block: a list of schema, id and value, filled by the parser
   of its kind. The kinds differ only in that parser. */
typedef struct
{
	PROPVARIANT schema, id, value;
} metadata_item;

typedef struct metadata metadata;

typedef struct
{
	const GUID *format;
	const CLSID *reader, *writer;
	const WCHAR *name;
	HRESULT (*parse)(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options);
} metadata_kind;

struct metadata
{
	IWICMetadataWriter iface;
	IWICPersistStream persist;
	IWICStreamProvider provider;
	LONG references;
	const metadata_kind *kind;
	BOOL writable;
	metadata_item *items;
	UINT count, room;
	IStream *stream;
	ULARGE_INTEGER origin;
	DWORD options;
	GUID vendor;
};

static metadata *metadata_of(IWICMetadataWriter *iface)
{
	return WIC_CONTAINING(iface, metadata, iface);
}

static WCHAR *text_copy(const WCHAR *text)
{
	const SIZE_T size = (lstrlenW(text) + 1) * sizeof(WCHAR);
	WCHAR *copy = CoTaskMemAlloc(size);
	if (copy)
		memcpy(copy, text, size);
	return copy;
}

static char *narrow_copy(const BYTE *text, SIZE_T size)
{
	char *copy = CoTaskMemAlloc(size + 1);
	if (copy) {
		memcpy(copy, text, size);
		copy[size] = 0;
	}
	return copy;
}

static HRESULT add_item(metadata *self, const PROPVARIANT *id, const PROPVARIANT *value)
{
	metadata_item *item;
	if (self->count == self->room) {
		const UINT room = self->room ? self->room * 2 : 16;
		metadata_item *larger = wic_alloc(room * sizeof(*larger));
		if (!larger)
			return E_OUTOFMEMORY;
		memcpy(larger, self->items, self->count * sizeof(*larger));
		wic_free(self->items);
		self->items = larger;
		self->room = room;
	}
	item = &self->items[self->count++];
	memset(item, 0, sizeof(*item));
	item->id = *id;
	item->value = *value;
	return S_OK;
}

/* A named item takes over whatever the value owns. */
static HRESULT add_named(metadata *self, const WCHAR *name, const PROPVARIANT *value)
{
	PROPVARIANT id = {0};
	id.vt = VT_LPWSTR;
	if (!(id.pwszVal = text_copy(name)))
		return E_OUTOFMEMORY;
	return add_item(self, &id, value);
}

static HRESULT add_u1(metadata *self, const WCHAR *name, BYTE number)
{
	PROPVARIANT value = {0};
	value.vt = VT_UI1;
	value.bVal = number;
	return add_named(self, name, &value);
}

static HRESULT add_u2(metadata *self, const WCHAR *name, USHORT number)
{
	PROPVARIANT value = {0};
	value.vt = VT_UI2;
	value.uiVal = number;
	return add_named(self, name, &value);
}

static HRESULT add_u4(metadata *self, const WCHAR *name, ULONG number)
{
	PROPVARIANT value = {0};
	value.vt = VT_UI4;
	value.ulVal = number;
	return add_named(self, name, &value);
}

static HRESULT add_flag(metadata *self, const WCHAR *name, BOOL flag)
{
	PROPVARIANT value = {0};
	value.vt = VT_BOOL;
	/* Stored as a plain one, not as VARIANT_TRUE. */
	value.boolVal = flag ? 1 : 0;
	return add_named(self, name, &value);
}

static HRESULT add_bytes(metadata *self, const WCHAR *name, const BYTE *bytes, SIZE_T size)
{
	PROPVARIANT value = {0};
	value.vt = VT_UI1 | VT_VECTOR;
	value.caub.cElems = (ULONG)size;
	if (size && !(value.caub.pElems = CoTaskMemAlloc(size)))
		return E_OUTOFMEMORY;
	if (size)
		memcpy(value.caub.pElems, bytes, size);
	return add_named(self, name, &value);
}

static HRESULT parse_unknown(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options)
{
	PROPVARIANT id = {0}, value = {0};
	value.vt = VT_BLOB;
	value.blob.cbSize = (ULONG)(size - start);
	if (value.blob.cbSize && !(value.blob.pBlobData = CoTaskMemAlloc(value.blob.cbSize)))
		return E_OUTOFMEMORY;
	if (value.blob.cbSize)
		memcpy(value.blob.pBlobData, data + start, value.blob.cbSize);
	return add_item(self, &id, &value);
}

static ULONG big_u4(const BYTE *at)
{
	return (ULONG)at[0] << 24 | at[1] << 16 | at[2] << 8 | at[3];
}

/* A PNG chunk as stored: length, type, body, CRC. The body comes back only if the chunk has the wanted type. */
static const BYTE *chunk_body(const BYTE *data, SIZE_T size, const char *type, ULONG *length)
{
	if (size < 8 || memcmp(data + 4, type, 4))
		return NULL;
	*length = big_u4(data);
	return *length <= size - 8 ? data + 8 : NULL;
}

static HRESULT parse_text(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options)
{
	ULONG length, split = 0;
	const BYTE *body = chunk_body(data + start, size - start, "tEXt", &length);
	PROPVARIANT id = {0}, value = {0};
	if (!body)
		return E_FAIL;
	while (split < length && body[split])
		split++;
	if (split >= length)
		return E_FAIL;
	id.vt = value.vt = VT_LPSTR;
	id.pszVal = narrow_copy(body, split);
	value.pszVal = narrow_copy(body + split + 1, length - split - 1);
	if (!id.pszVal || !value.pszVal)
		return E_OUTOFMEMORY;
	return add_item(self, &id, &value);
}

static HRESULT parse_gamma(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options)
{
	ULONG length;
	const BYTE *body = chunk_body(data + start, size - start, "gAMA", &length);
	if (!body || length != 4)
		return E_FAIL;
	return add_u4(self, L"ImageGamma", big_u4(body));
}

static HRESULT parse_chromaticity(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options)
{
	static const WCHAR *const names[] = {L"WhitePointX", L"WhitePointY", L"RedX", L"RedY", L"GreenX", L"GreenY", L"BlueX", L"BlueY"};
	ULONG length;
	const BYTE *body = chunk_body(data + start, size - start, "cHRM", &length);
	UINT index;
	if (!body || length != 32)
		return E_FAIL;
	for (index = 0; index < ARRAYSIZE(names); index++)
		add_u4(self, names[index], big_u4(body + index * 4));
	return S_OK;
}

static HRESULT parse_histogram(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options)
{
	ULONG length, index;
	const BYTE *body = chunk_body(data + start, size - start, "hIST", &length);
	PROPVARIANT value = {0};
	if (!body || length % 2)
		return E_FAIL;
	value.vt = VT_UI2 | VT_VECTOR;
	value.caui.cElems = length / 2;
	if (length && !(value.caui.pElems = CoTaskMemAlloc(length)))
		return E_OUTOFMEMORY;
	for (index = 0; index < length / 2; index++)
		value.caui.pElems[index] = (USHORT)(body[index * 2] << 8 | body[index * 2 + 1]);
	return add_named(self, L"Frequencies", &value);
}

static HRESULT parse_time(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options)
{
	ULONG length;
	const BYTE *body = chunk_body(data + start, size - start, "tIME", &length);
	if (!body || length != 7)
		return E_FAIL;
	add_u2(self, L"Year", (USHORT)(body[0] << 8 | body[1]));
	add_u1(self, L"Month", body[2]);
	add_u1(self, L"Day", body[3]);
	add_u1(self, L"Hour", body[4]);
	add_u1(self, L"Minute", body[5]);
	add_u1(self, L"Second", body[6]);
	return S_OK;
}

static HRESULT parse_background(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options)
{
	ULONG length;
	const BYTE *body = chunk_body(data + start, size - start, "bKGD", &length);
	PROPVARIANT value = {0};
	if (!body)
		return E_FAIL;
	if (length == 1)
		return add_u1(self, L"BackgroundColor", body[0]);
	if (length == 2)
		return add_u2(self, L"BackgroundColor", (USHORT)(body[0] << 8 | body[1]));
	if (length != 6)
		return E_FAIL;
	value.vt = VT_UI2 | VT_VECTOR;
	value.caui.cElems = 3;
	if (!(value.caui.pElems = CoTaskMemAlloc(6)))
		return E_OUTOFMEMORY;
	value.caui.pElems[0] = (USHORT)(body[0] << 8 | body[1]);
	value.caui.pElems[1] = (USHORT)(body[2] << 8 | body[3]);
	value.caui.pElems[2] = (USHORT)(body[4] << 8 | body[5]);
	return add_named(self, L"BackgroundColor", &value);
}

/* ---- TIFF directories: offsets count from the start of the stream, and the options name the byte order. ---- */
static USHORT ifd_u2(const BYTE *at, BOOL big)
{
	return big ? (USHORT)(at[0] << 8 | at[1]) : (USHORT)(at[1] << 8 | at[0]);
}

static ULONG ifd_u4(const BYTE *at, BOOL big)
{
	return big ? big_u4(at) : (ULONG)at[3] << 24 | at[2] << 16 | at[1] << 8 | at[0];
}

static HRESULT ifd_value(PROPVARIANT *value, USHORT type, ULONG count, const BYTE *at, BOOL big)
{
	static const VARTYPE kinds[] = {0, VT_UI1, VT_LPSTR, VT_UI2, VT_UI4, VT_UI8, VT_I1, VT_BLOB, VT_I2, VT_I4, VT_I8, VT_R4, VT_R8};
	static const BYTE widths[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
	const UINT width = widths[type];
	BYTE *elements;
	ULONG index;
	if (type == 2) {
		value->vt = VT_LPSTR;
		return (value->pszVal = narrow_copy(at, count)) ? S_OK : E_OUTOFMEMORY;
	}
	if (type == 7) {
		value->vt = VT_BLOB;
		value->blob.cbSize = count;
		if (count && !(value->blob.pBlobData = CoTaskMemAlloc(count)))
			return E_OUTOFMEMORY;
		if (count)
			memcpy(value->blob.pBlobData, at, count);
		return S_OK;
	}
	value->vt = kinds[type];
	if (count <= 1)
		elements = (BYTE *)&value->bVal;
	else {
		value->vt |= VT_VECTOR;
		value->caub.cElems = count;
		if (count && !(value->caub.pElems = CoTaskMemAlloc((SIZE_T)count * width)))
			return E_OUTOFMEMORY;
		elements = value->caub.pElems;
	}
	/* An item that counts nothing still reads as the one value its entry has room for. */
	for (index = 0; index < max(count, width <= 4 ? 1 : 0); index++, at += width, elements += width) {
		UINT part;
		if (width == 1)
			elements[0] = at[0];
		else if (type == 5 || type == 10) {
			/* A fraction: numerator in the low half, denominator in the high one. */
			const ULONG low = ifd_u4(at, big), high = ifd_u4(at + 4, big);
			memcpy(elements, &low, 4);
			memcpy(elements + 4, &high, 4);
		}
		else
			for (part = 0; part < width; part++)
				elements[part] = at[big ? width - part - 1 : part];
	}
	return S_OK;
}

static HRESULT parse_ifd(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options);
static HRESULT parse_app1(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options);
static HRESULT nested_ifd(REFGUID format, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options, PROPVARIANT *value);

static HRESULT parse_ifd(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options)
{
	const BOOL big = (options & WICPersistOptionBigEndian) != 0;
	USHORT entries, index;
	const BYTE *at;
	if (start + 2 > size)
		return WINCODEC_ERR_BADMETADATAHEADER;
	entries = ifd_u2(data + start, big);
	at = data + start + 2;
	if (at + (SIZE_T)entries * 12 > data + size)
		return WINCODEC_ERR_BADMETADATAHEADER;
	for (index = 0; index < entries; index++, at += 12) {
		static const BYTE widths[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
		const USHORT tag = ifd_u2(at, big), type = ifd_u2(at + 2, big);
		const ULONG count = ifd_u4(at + 4, big);
		PROPVARIANT id = {0}, value = {0};
		SIZE_T bytes;
		const BYTE *place = at + 8;
		HRESULT result;
		if (!type || type >= ARRAYSIZE(widths))
			return WINCODEC_ERR_BADMETADATAHEADER;
		bytes = (SIZE_T)count * widths[type];
		if (bytes > 4) {
			const ULONG offset = ifd_u4(at + 8, big);
			if (offset > size || bytes > size - offset)
				return WINCODEC_ERR_BADMETADATAHEADER;
			place = data + offset;
		}
		id.vt = VT_UI2;
		id.uiVal = tag;
		/* Exif and GPS hang off the first directory by offset, and read as readers of their own. */
		if ((tag == 0x8769 || tag == 0x8825) && IsEqualGUID(self->kind->format, &GUID_MetadataFormatIfd) && type == 4 && count == 1
			&& SUCCEEDED(nested_ifd(tag == 0x8769 ? &GUID_MetadataFormatExif : &GUID_MetadataFormatGps, data, size, ifd_u4(place, big), options, &value))) {
			if (FAILED(result = add_item(self, &id, &value)))
				return result;
			continue;
		}
		if (FAILED(result = ifd_value(&value, type, count, place, big)) || FAILED(result = add_item(self, &id, &value)))
			return result;
	}
	return S_OK;
}

/* Offsets inside an APP1 block count from its TIFF header, so a nested reader gets those bytes as its stream, positioned
   at its directory. */
static HRESULT nested_ifd(REFGUID format, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options, PROPVARIANT *value)
{
	IStream *memory = NULL;
	IWICStream *stream = NULL;
	IWICMetadataReader *reader = NULL;
	LARGE_INTEGER place;
	HRESULT result;
	if (start > size)
		return WINCODEC_ERR_BADMETADATAHEADER;
	if (FAILED(result = CreateStreamOnHGlobal(NULL, TRUE, &memory)))
		return result;
	place.QuadPart = (LONGLONG)start;
	if (SUCCEEDED(result = IStream_Write(memory, data, (ULONG)size, NULL)) && SUCCEEDED(result = wic_stream_create(&stream))
		&& SUCCEEDED(result = IWICStream_InitializeFromIStream(stream, memory)) && SUCCEEDED(result = IWICStream_Seek(stream, place, STREAM_SEEK_SET, NULL)))
		result = wic_metadata_reader_create(format, NULL, options, (IStream *)stream, &reader);
	/* What it reads from stays positioned at the directory, for whoever asks for the stream. */
	if (stream) {
		IWICStream_Seek(stream, place, STREAM_SEEK_SET, NULL);
		IWICStream_Release(stream);
	}
	IStream_Release(memory);
	if (FAILED(result))
		return result;
	value->vt = VT_UNKNOWN;
	value->punkVal = (IUnknown *)reader;
	return S_OK;
}

static HRESULT parse_app1(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options)
{
	static const BYTE exif[6] = {'E', 'x', 'i', 'f', 0, 0};
	const BYTE *tiff;
	PROPVARIANT id = {0}, value = {0};
	BOOL big;
	ULONG offset;
	HRESULT result;
	if (start + 14 > size || memcmp(data + start, exif, sizeof(exif)))
		return WINCODEC_ERR_BADMETADATAHEADER;
	tiff = data + start + sizeof(exif);
	size -= start + sizeof(exif);
	if ((tiff[0] != 'I' || tiff[1] != 'I') && (tiff[0] != 'M' || tiff[1] != 'M'))
		return WINCODEC_ERR_BADMETADATAHEADER;
	big = tiff[0] == 'M';
	if (ifd_u2(tiff + 2, big) != 42)
		return WINCODEC_ERR_BADMETADATAHEADER;
	offset = ifd_u4(tiff + 4, big);
	options = (options & ~WICPersistOptionBigEndian) | (big ? WICPersistOptionBigEndian : 0);
	if (FAILED(result = nested_ifd(&GUID_MetadataFormatIfd, tiff, size, offset, options, &value)))
		return result;
	id.vt = VT_UI2;
	return add_item(self, &id, &value);
}

/* APP0 holds the JFIF fields, in the order they lie in the block; with nothing loaded they read as zeros. */
static HRESULT parse_app0(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options)
{
	static const BYTE signature[5] = {'J', 'F', 'I', 'F', 0};
	BYTE fixed[14] = {0};
	PROPVARIANT thumbnail = {0};
	const BOOL present = size >= start + sizeof(fixed);
	if (data && (!present || memcmp(data + start, signature, sizeof(signature))))
		return WINCODEC_ERR_BADMETADATAHEADER;
	if (data)
		memcpy(fixed, data + start, sizeof(fixed));
	{
		const struct
		{
			USHORT number;
			BOOL wide;
		} fields[] = {{(USHORT)(fixed[5] << 8 | fixed[6]), TRUE}, {fixed[7], FALSE}, {(USHORT)(fixed[8] << 8 | fixed[9]), TRUE}, {(USHORT)(fixed[10] << 8 | fixed[11]), TRUE}, {fixed[12], FALSE}, {fixed[13], FALSE}};
		USHORT index;
		for (index = 0; index < ARRAYSIZE(fields); index++) {
			PROPVARIANT id = {0}, value = {0};
			id.vt = VT_UI2;
			id.uiVal = index;
			value.vt = fields[index].wide ? VT_UI2 : VT_UI1;
			value.uiVal = fields[index].number;
			add_item(self, &id, &value);
		}
	}
	/* The picture is as long as its two sizes say, three bytes a pixel; anything after it is not its concern. */
	thumbnail.vt = VT_BLOB;
	thumbnail.blob.cbSize = (ULONG)fixed[12] * fixed[13] * 3;
	if (thumbnail.blob.cbSize) {
		if (size - start - sizeof(fixed) < thumbnail.blob.cbSize)
			return WINCODEC_ERR_BADMETADATAHEADER;
		if (!(thumbnail.blob.pBlobData = CoTaskMemAlloc(thumbnail.blob.cbSize)))
			return E_OUTOFMEMORY;
		memcpy(thumbnail.blob.pBlobData, data + start + sizeof(fixed), thumbnail.blob.cbSize);
	}
	{
		PROPVARIANT id = {0};
		id.vt = VT_UI2;
		id.uiVal = 6;
		return add_item(self, &id, &thumbnail);
	}
}

/* ---- The blocks of a GIF, each with or without the bytes that introduce it. ---- */
static HRESULT parse_screen(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options)
{
	const BYTE *at = data + start;
	if (size - start < 13)
		return E_FAIL;
	add_bytes(self, L"Signature", at, 6);
	add_u2(self, L"Width", (USHORT)(at[6] | at[7] << 8));
	add_u2(self, L"Height", (USHORT)(at[8] | at[9] << 8));
	add_flag(self, L"GlobalColorTableFlag", at[10] >> 7);
	add_u1(self, L"ColorResolution", at[10] >> 4 & 7);
	add_flag(self, L"SortFlag", at[10] >> 3 & 1);
	add_u1(self, L"GlobalColorTableSize", at[10] & 7);
	add_u1(self, L"BackgroundColorIndex", at[11]);
	add_u1(self, L"PixelAspectRatio", at[12]);
	return S_OK;
}

static HRESULT parse_image(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options)
{
	const BYTE *at = data + start;
	if (size - start < 9)
		return E_FAIL;
	add_u2(self, L"Left", (USHORT)(at[0] | at[1] << 8));
	add_u2(self, L"Top", (USHORT)(at[2] | at[3] << 8));
	add_u2(self, L"Width", (USHORT)(at[4] | at[5] << 8));
	add_u2(self, L"Height", (USHORT)(at[6] | at[7] << 8));
	add_flag(self, L"LocalColorTableFlag", at[8] >> 7);
	add_flag(self, L"InterlaceFlag", at[8] >> 6 & 1);
	add_flag(self, L"SortFlag", at[8] >> 5 & 1);
	add_u1(self, L"LocalColorTableSize", at[8] & 7);
	return S_OK;
}

static HRESULT parse_control(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options)
{
	const BYTE *at = data + start;
	if (size - start < 4)
		return E_FAIL;
	add_u1(self, L"Disposal", at[0] >> 2 & 7);
	add_flag(self, L"UserInputFlag", at[0] >> 1 & 1);
	add_flag(self, L"TransparencyFlag", at[0] & 1);
	add_u2(self, L"Delay", (USHORT)(at[1] | at[2] << 8));
	add_u1(self, L"TransparentColorIndex", at[3]);
	return S_OK;
}

static HRESULT parse_application(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options)
{
	const BYTE *at = data + start, *end = data + size, *blocks;
	if (end - at < 2 || at[0] != 0x21 || at[1] != 0xff)
		return E_FAIL;
	at += 2;
	if (end - at < 12 || at[0] != 11)
		return E_FAIL;
	add_bytes(self, L"Application", at + 1, 11);
	blocks = at += 12;
	while (at < end && at[0])
		at += at[0] + 1;
	return add_bytes(self, L"Data", blocks, (SIZE_T)(min(at, end) - blocks));
}

static HRESULT parse_comment(metadata *self, const BYTE *data, SIZE_T size, SIZE_T start, DWORD options)
{
	const BYTE *at = data + start, *end = data + size;
	PROPVARIANT value = {0};
	SIZE_T used = 0;
	if (end - at < 2 || at[0] != 0x21 || at[1] != 0xfe)
		return E_FAIL;
	at += 2;
	value.vt = VT_LPSTR;
	if (!(value.pszVal = CoTaskMemAlloc((SIZE_T)(end - at) + 1)))
		return E_OUTOFMEMORY;
	while (at < end && at[0] && at + at[0] < end) {
		memcpy(value.pszVal + used, at + 1, at[0]);
		used += at[0];
		at += at[0] + 1;
	}
	value.pszVal[used] = 0;
	return add_named(self, L"TextEntry", &value);
}

static const metadata_kind kinds[] = {
	{&GUID_MetadataFormatUnknown, &CLSID_WICUnknownMetadataReader, &CLSID_WICUnknownMetadataWriter, L"Unknown Metadata", parse_unknown},
	{&GUID_MetadataFormatChunktEXt, &CLSID_WICPngTextMetadataReader, &CLSID_WICPngTextMetadataWriter, L"PNG TEXT Metadata", parse_text},
	{&GUID_MetadataFormatChunkgAMA, &CLSID_WICPngGamaMetadataReader, &CLSID_WICPngGamaMetadataWriter, L"PNG Gamma Metadata", parse_gamma},
	{&GUID_MetadataFormatChunkcHRM, &CLSID_WICPngChrmMetadataReader, &CLSID_WICPngChrmMetadataWriter, L"PNG Chromaticity Metadata", parse_chromaticity},
	{&GUID_MetadataFormatChunkhIST, &CLSID_WICPngHistMetadataReader, &CLSID_WICPngHistMetadataWriter, L"PNG Histogram Metadata", parse_histogram},
	{&GUID_MetadataFormatChunktIME, &CLSID_WICPngTimeMetadataReader, &CLSID_WICPngTimeMetadataWriter, L"PNG Time Metadata", parse_time},
	{&GUID_MetadataFormatChunkbKGD, &CLSID_WICPngBkgdMetadataReader, &CLSID_WICPngBkgdMetadataWriter, L"PNG Background Metadata", parse_background},
	{&GUID_MetadataFormatIfd, &CLSID_WICIfdMetadataReader, &CLSID_WICIfdMetadataWriter, L"Ifd Metadata", parse_ifd},
	{&GUID_MetadataFormatExif, &CLSID_WICExifMetadataReader, &CLSID_WICExifMetadataWriter, L"Exif Metadata", parse_ifd},
	{&GUID_MetadataFormatGps, &CLSID_WICGpsMetadataReader, &CLSID_WICGpsMetadataWriter, L"Gps Metadata", parse_ifd},
	{&GUID_MetadataFormatLSD, &CLSID_WICLSDMetadataReader, &CLSID_WICLSDMetadataWriter, L"Logical Screen Descriptor Reader", parse_screen},
	{&GUID_MetadataFormatIMD, &CLSID_WICIMDMetadataReader, &CLSID_WICIMDMetadataWriter, L"Image Descriptor Reader", parse_image},
	{&GUID_MetadataFormatGCE, &CLSID_WICGCEMetadataReader, &CLSID_WICGCEMetadataWriter, L"Graphic Control Extension Reader", parse_control},
	{&GUID_MetadataFormatAPE, &CLSID_WICAPEMetadataReader, &CLSID_WICAPEMetadataWriter, L"Application Extension Reader", parse_application},
	{&GUID_MetadataFormatGifComment, &CLSID_WICGifCommentMetadataReader, &CLSID_WICGifCommentMetadataWriter, L"Comment Extension Reader", parse_comment},
	{&GUID_MetadataFormatApp0, &CLSID_WICApp0MetadataReader, &CLSID_WICApp0MetadataWriter, L"App0 Metadata", parse_app0},
	{&GUID_MetadataFormatApp1, &CLSID_WICApp1MetadataReader, &CLSID_WICApp1MetadataWriter, L"App1 Metadata", parse_app1},
};

static void clear_items(metadata *self)
{
	UINT index;
	for (index = 0; index < self->count; index++) {
		PropVariantClear(&self->items[index].schema);
		PropVariantClear(&self->items[index].id);
		PropVariantClear(&self->items[index].value);
	}
	self->count = 0;
}

/* Text ids compare without case; anything else by its stored value. */
/* A name matches whichever width it was asked in. */
/* Some blocks have a set list of items: nothing joins or leaves it, and it holds its defaults until something is loaded. */
static BOOL fixed_list(const metadata *self)
{
	const metadata_kind *kind = self->kind;
	return kind->parse == parse_unknown || kind->parse == parse_control || kind->parse == parse_application || kind->parse == parse_comment || kind->parse == parse_app0
		|| kind->parse == parse_gamma || kind->parse == parse_chromaticity || kind->parse == parse_histogram
		|| kind->parse == parse_time || kind->parse == parse_background;
}

static void seed_defaults(metadata *self)
{
	const metadata_kind *kind = self->kind;
	if (kind->parse == parse_unknown)
		parse_unknown(self, NULL, 0, 0, 0);
	else if (kind->parse == parse_gamma)
		add_u4(self, L"ImageGamma", 45455);
	else if (kind->parse == parse_chromaticity) {
		static const WCHAR *const names[] = {L"WhitePointX", L"WhitePointY", L"RedX", L"RedY", L"GreenX", L"GreenY", L"BlueX", L"BlueY"};
		static const ULONG values[] = {31270, 32900, 64000, 33000, 30000, 60000, 15000, 6000};
		UINT index;
		for (index = 0; index < ARRAYSIZE(names); index++)
			add_u4(self, names[index], values[index]);
	}
	else if (kind->parse == parse_histogram) {
		PROPVARIANT value = {0};
		value.vt = VT_UI2 | VT_VECTOR;
		add_named(self, L"Frequencies", &value);
	}
	else if (kind->parse == parse_time) {
		add_u2(self, L"Year", 0);
		add_u1(self, L"Month", 1);
		add_u1(self, L"Day", 1);
		add_u1(self, L"Hour", 0);
		add_u1(self, L"Minute", 0);
		add_u1(self, L"Second", 0);
	}
	else if (kind->parse == parse_background) {
		PROPVARIANT value = {0};
		add_named(self, L"BackgroundColor", &value);
	}
	else if (kind->parse == parse_control) {
		add_u1(self, L"Disposal", 0);
		add_flag(self, L"UserInputFlag", FALSE);
		add_flag(self, L"TransparencyFlag", FALSE);
		add_u2(self, L"Delay", 0);
		add_u1(self, L"TransparentColorIndex", 0);
	}
	else if (kind->parse == parse_application) {
		PROPVARIANT empty = {0}, data = {0};
		add_named(self, L"Application", &empty);
		data.vt = VT_UI1 | VT_VECTOR;
		add_named(self, L"Data", &data);
	}
	else if (kind->parse == parse_comment) {
		PROPVARIANT value = {0};
		value.vt = VT_LPSTR;
		if ((value.pszVal = CoTaskMemAlloc(1))) {
			value.pszVal[0] = 0;
			add_named(self, L"TextEntry", &value);
		}
	}
	else if (kind->parse == parse_app0)
		parse_app0(self, NULL, 0, 0, 0);
}

static BOOL same_text(const char *narrow, const WCHAR *wide)
{
	WCHAR widened[256];
	return MultiByteToWideChar(CP_ACP, 0, narrow, -1, widened, ARRAYSIZE(widened)) && !lstrcmpiW(widened, wide);
}

static BOOL same_variant(const PROPVARIANT *one, const PROPVARIANT *two)
{
	if (one->vt == VT_LPSTR && two->vt == VT_LPWSTR)
		return same_text(one->pszVal, two->pwszVal);
	if (one->vt == VT_LPWSTR && two->vt == VT_LPSTR)
		return same_text(two->pszVal, one->pwszVal);
	if (one->vt != two->vt)
		return FALSE;
	switch (one->vt) {
	case VT_EMPTY:
		return TRUE;
	case VT_LPWSTR:
		return !lstrcmpiW(one->pwszVal, two->pwszVal);
	case VT_LPSTR:
		return !lstrcmpiA(one->pszVal, two->pszVal);
	case VT_UI1:
	case VT_I1:
		return one->bVal == two->bVal;
	case VT_UI2:
	case VT_I2:
	case VT_BOOL:
		return one->uiVal == two->uiVal;
	case VT_UI4:
	case VT_I4:
	case VT_R4:
		return one->ulVal == two->ulVal;
	case VT_UI8:
	case VT_I8:
	case VT_R8:
		return one->uhVal.QuadPart == two->uhVal.QuadPart;
	default:
		return FALSE;
	}
}

static INT find_item(metadata *self, const PROPVARIANT *schema, const PROPVARIANT *id)
{
	UINT index;
	/* A format as the id asks for the block of that format held inside this one. */
	if (id->vt == VT_CLSID) {
		for (index = 0; index < self->count; index++) {
			IWICMetadataReader *reader;
			GUID format;
			BOOL same;
			if (self->items[index].value.vt != VT_UNKNOWN || !self->items[index].value.punkVal
				|| FAILED(IUnknown_QueryInterface(self->items[index].value.punkVal, &IID_IWICMetadataReader, (void **)&reader)))
				continue;
			same = SUCCEEDED(IWICMetadataReader_GetMetadataFormat(reader, &format)) && IsEqualGUID(&format, id->puuid);
			IWICMetadataReader_Release(reader);
			if (same)
				return (INT)index;
		}
		return -1;
	}
	/* A directory has one namespace; what schema is asked for makes no difference. */
	if (self->kind->parse == parse_ifd)
		schema = NULL;
	for (index = 0; index < self->count; index++)
		if (same_variant(&self->items[index].id, id) && (!schema || schema->vt == VT_EMPTY || same_variant(&self->items[index].schema, schema)))
			return (INT)index;
	return -1;
}

static HRESULT WINAPI item_QueryInterface(IWICMetadataWriter *iface, REFIID riid, void **out)
{
	metadata *self = metadata_of(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICMetadataReader) || (self->writable && IsEqualGUID(riid, &IID_IWICMetadataWriter)))
		*out = &self->iface;
	else if (IsEqualGUID(riid, &IID_IPersist) || IsEqualGUID(riid, &IID_IPersistStream) || IsEqualGUID(riid, &IID_IWICPersistStream))
		*out = &self->persist;
	else if (IsEqualGUID(riid, &IID_IWICStreamProvider))
		*out = &self->provider;
	else
		return E_NOINTERFACE;
	InterlockedIncrement(&self->references);
	return S_OK;
}

static ULONG WINAPI item_AddRef(IWICMetadataWriter *iface)
{
	return InterlockedIncrement(&metadata_of(iface)->references);
}

static ULONG WINAPI item_Release(IWICMetadataWriter *iface)
{
	metadata *self = metadata_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		clear_items(self);
		if (self->stream)
			IStream_Release(self->stream);
		wic_free(self->items);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI item_GetMetadataFormat(IWICMetadataWriter *iface, GUID *format)
{
	if (!format)
		return E_INVALIDARG;
	*format = *metadata_of(iface)->kind->format;
	return S_OK;
}

static HRESULT WINAPI item_GetMetadataHandlerInfo(IWICMetadataWriter *iface, IWICMetadataHandlerInfo **out)
{
	metadata *self = metadata_of(iface);
	if (!out)
		return E_INVALIDARG;
	return wic_component_info_create(self->writable ? self->kind->writer : self->kind->reader, (IWICComponentInfo **)out);
}

static HRESULT WINAPI item_GetCount(IWICMetadataWriter *iface, UINT *count)
{
	if (!count)
		return E_INVALIDARG;
	*count = metadata_of(iface)->count;
	return S_OK;
}

static HRESULT WINAPI item_GetValueByIndex(IWICMetadataWriter *iface, UINT index, PROPVARIANT *schema, PROPVARIANT *id, PROPVARIANT *value)
{
	metadata *self = metadata_of(iface);
	HRESULT result = S_OK;
	if (index >= self->count)
		return E_INVALIDARG;
	if (schema)
		result = PropVariantCopy(schema, &self->items[index].schema);
	if (SUCCEEDED(result) && id)
		result = PropVariantCopy(id, &self->items[index].id);
	if (SUCCEEDED(result) && value)
		result = PropVariantCopy(value, &self->items[index].value);
	return result;
}

static HRESULT WINAPI item_GetValue(IWICMetadataWriter *iface, const PROPVARIANT *schema, const PROPVARIANT *id, PROPVARIANT *value)
{
	metadata *self = metadata_of(iface);
	INT index;
	if (!id)
		return E_INVALIDARG;
	if (fixed_list(self) && self->kind->parse != parse_unknown && id->vt != VT_LPSTR && id->vt != VT_LPWSTR)
		return E_INVALIDARG;
	if ((index = find_item(self, schema, id)) < 0)
		return WINCODEC_ERR_PROPERTYNOTFOUND;
	return value ? PropVariantCopy(value, &self->items[index].value) : S_OK;
}

/* The enumerator walks a copy of the list taken when it is made. */
typedef struct
{
	IWICEnumMetadataItem iface;
	LONG references;
	metadata *owner;
	UINT at;
} item_enumerator;

static HRESULT enumerator_create(metadata *owner, UINT at, IWICEnumMetadataItem **out);

static HRESULT WINAPI walk_QueryInterface(IWICEnumMetadataItem *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICEnumMetadataItem)) {
		*out = iface;
		IWICEnumMetadataItem_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI walk_AddRef(IWICEnumMetadataItem *iface)
{
	return InterlockedIncrement(&WIC_CONTAINING(iface, item_enumerator, iface)->references);
}

static ULONG WINAPI walk_Release(IWICEnumMetadataItem *iface)
{
	item_enumerator *self = WIC_CONTAINING(iface, item_enumerator, iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		IWICMetadataWriter_Release(&self->owner->iface);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI walk_Next(IWICEnumMetadataItem *iface, ULONG count, PROPVARIANT *schema, PROPVARIANT *id, PROPVARIANT *value, ULONG *fetched)
{
	item_enumerator *self = WIC_CONTAINING(iface, item_enumerator, iface);
	ULONG got = 0;
	if (count > 1 && !fetched)
		return E_INVALIDARG;
	while (got < count && self->at < self->owner->count) {
		item_GetValueByIndex(&self->owner->iface, self->at++, schema ? schema + got : NULL, id ? id + got : NULL, value ? value + got : NULL);
		got++;
	}
	if (fetched)
		*fetched = got;
	return got == count ? S_OK : S_FALSE;
}

static HRESULT WINAPI walk_Skip(IWICEnumMetadataItem *iface, ULONG count)
{
	item_enumerator *self = WIC_CONTAINING(iface, item_enumerator, iface);
	const UINT left = self->owner->count - min(self->at, self->owner->count);
	self->at += min(count, left);
	return count <= left ? S_OK : S_FALSE;
}

static HRESULT WINAPI walk_Reset(IWICEnumMetadataItem *iface)
{
	WIC_CONTAINING(iface, item_enumerator, iface)->at = 0;
	return S_OK;
}

static HRESULT WINAPI walk_Clone(IWICEnumMetadataItem *iface, IWICEnumMetadataItem **out)
{
	item_enumerator *self = WIC_CONTAINING(iface, item_enumerator, iface);
	if (!out)
		return E_INVALIDARG;
	return enumerator_create(self->owner, self->at, out);
}

static const IWICEnumMetadataItemVtbl walk_vtbl = {
	walk_QueryInterface, walk_AddRef, walk_Release, walk_Next, walk_Skip, walk_Reset, walk_Clone,
};

static HRESULT enumerator_create(metadata *owner, UINT at, IWICEnumMetadataItem **out)
{
	item_enumerator *self = wic_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &walk_vtbl;
	self->references = 1;
	self->owner = owner;
	self->at = at;
	IWICMetadataWriter_AddRef(&owner->iface);
	*out = &self->iface;
	return S_OK;
}

static HRESULT WINAPI item_GetEnumerator(IWICMetadataWriter *iface, IWICEnumMetadataItem **out)
{
	if (!out)
		return E_INVALIDARG;
	return enumerator_create(metadata_of(iface), 0, out);
}

static HRESULT WINAPI item_SetValue(IWICMetadataWriter *iface, const PROPVARIANT *schema, const PROPVARIANT *id, const PROPVARIANT *value)
{
	metadata *self = metadata_of(iface);
	PROPVARIANT id_copy = {0}, value_copy = {0};
	INT index;
	HRESULT result;
	if (!id || !value)
		return E_INVALIDARG;
	if (fixed_list(self) && (id->vt == VT_EMPTY && self->kind->parse != parse_unknown))
		return E_INVALIDARG;
	if ((index = find_item(self, schema, id)) >= 0) {
		PropVariantClear(&self->items[index].value);
		return PropVariantCopy(&self->items[index].value, value);
	}
	if (FAILED(result = PropVariantCopy(&id_copy, id)) || FAILED(result = PropVariantCopy(&value_copy, value)))
		return result;
	if (FAILED(result = add_item(self, &id_copy, &value_copy)))
		return result;
	return schema && self->kind->parse != parse_ifd ? PropVariantCopy(&self->items[self->count - 1].schema, schema) : S_OK;
}

static HRESULT WINAPI item_SetValueByIndex(IWICMetadataWriter *iface, UINT index, const PROPVARIANT *schema, const PROPVARIANT *id, const PROPVARIANT *value)
{
	metadata *self = metadata_of(iface);
	if (index >= self->count)
		return E_INVALIDARG;
	if (!id || !value)
		return E_INVALIDARG;
	PropVariantClear(&self->items[index].id);
	PropVariantClear(&self->items[index].value);
	PropVariantCopy(&self->items[index].id, id);
	return PropVariantCopy(&self->items[index].value, value);
}

static HRESULT WINAPI item_RemoveValueByIndex(IWICMetadataWriter *iface, UINT index)
{
	if (index >= metadata_of(iface)->count)
		return E_INVALIDARG;
	if (fixed_list(metadata_of(iface)))
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	metadata *self = metadata_of(iface);
	if (index >= self->count)
		return WINCODEC_ERR_PROPERTYNOTFOUND;
	PropVariantClear(&self->items[index].schema);
	PropVariantClear(&self->items[index].id);
	PropVariantClear(&self->items[index].value);
	memmove(&self->items[index], &self->items[index + 1], (self->count - index - 1) * sizeof(*self->items));
	self->count--;
	return S_OK;
}

static HRESULT WINAPI item_RemoveValue(IWICMetadataWriter *iface, const PROPVARIANT *schema, const PROPVARIANT *id)
{
	if (fixed_list(metadata_of(iface)))
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	const INT index = id ? find_item(metadata_of(iface), schema, id) : -1;
	if (!id)
		return E_INVALIDARG;
	return index < 0 ? WINCODEC_ERR_PROPERTYNOTFOUND : item_RemoveValueByIndex(iface, (UINT)index);
}

static const IWICMetadataWriterVtbl item_vtbl = {
	item_QueryInterface, item_AddRef, item_Release, item_GetMetadataFormat, item_GetMetadataHandlerInfo, item_GetCount, item_GetValueByIndex,
	item_GetValue, item_GetEnumerator, item_SetValue, item_SetValueByIndex, item_RemoveValue, item_RemoveValueByIndex,
};

static metadata *persist_of(IWICPersistStream *iface)
{
	return WIC_CONTAINING(iface, metadata, persist);
}

static HRESULT WINAPI persist_QueryInterface(IWICPersistStream *iface, REFIID riid, void **out)
{
	return item_QueryInterface(&persist_of(iface)->iface, riid, out);
}

static ULONG WINAPI persist_AddRef(IWICPersistStream *iface)
{
	return item_AddRef(&persist_of(iface)->iface);
}

static ULONG WINAPI persist_Release(IWICPersistStream *iface)
{
	return item_Release(&persist_of(iface)->iface);
}

static HRESULT WINAPI persist_GetClassID(IWICPersistStream *iface, CLSID *clsid)
{
	metadata *self = persist_of(iface);
	if (!clsid)
		return E_INVALIDARG;
	*clsid = *(self->writable ? self->kind->writer : self->kind->reader);
	return S_OK;
}

static HRESULT WINAPI persist_IsDirty(IWICPersistStream *iface)
{
	return S_FALSE;
}

/* The block is read whole: directories point back to the start of the stream, so it is kept with the place to begin at. */
static HRESULT WINAPI persist_LoadEx(IWICPersistStream *iface, IStream *stream, const GUID *vendor, DWORD options)
{
	metadata *self = persist_of(iface);
	LARGE_INTEGER zero = {{0}};
	ULARGE_INTEGER at, end;
	BYTE *data;
	ULONG got = 0;
	HRESULT result;
	clear_items(self);
	self->options = options;
	self->vendor = vendor ? *vendor : GUID_VendorMicrosoft;
	if (self->stream) {
		IStream_Release(self->stream);
		self->stream = NULL;
	}
	if (!stream)
		return seed_defaults(self), S_OK;
	if (FAILED(result = IStream_Seek(stream, zero, STREAM_SEEK_CUR, &at)) || FAILED(result = IStream_Seek(stream, zero, STREAM_SEEK_END, &end)))
		return result;
	if (!(data = wic_alloc((SIZE_T)end.QuadPart + 1)))
		return E_OUTOFMEMORY;
	IStream_Seek(stream, zero, STREAM_SEEK_SET, NULL);
	result = IStream_Read(stream, data, (ULONG)end.QuadPart, &got);
	/* Loading at the end of a stream finds nothing to read, which is no failure. */
	if (SUCCEEDED(result) && at.QuadPart < got)
		result = self->kind->parse(self, data, got, (SIZE_T)at.QuadPart, options);
	else if (SUCCEEDED(result))
		seed_defaults(self);
	wic_free(data);
	if (FAILED(result)) {
		LARGE_INTEGER back;
		back.QuadPart = (LONGLONG)at.QuadPart;
		IStream_Seek(stream, back, STREAM_SEEK_SET, NULL);
		clear_items(self);
		return result;
	}
	self->origin = at;
	if (!(options & WICPersistOptionNoCacheStream)) {
		self->stream = stream;
		IStream_AddRef(stream);
	}
	return S_OK;
}

static HRESULT WINAPI persist_Load(IWICPersistStream *iface, IStream *stream)
{
	return stream ? persist_LoadEx(iface, stream, NULL, WICPersistOptionDefault) : E_INVALIDARG;
}

static HRESULT WINAPI persist_Save(IWICPersistStream *iface, IStream *stream, BOOL clear)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI persist_GetSizeMax(IWICPersistStream *iface, ULARGE_INTEGER *size)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI persist_SaveEx(IWICPersistStream *iface, IStream *stream, DWORD options, BOOL clear)
{
	return E_NOTIMPL;
}

static const IWICPersistStreamVtbl persist_vtbl = {
	persist_QueryInterface, persist_AddRef, persist_Release, persist_GetClassID, persist_IsDirty, persist_Load, persist_Save, persist_GetSizeMax,
	persist_LoadEx, persist_SaveEx,
};

static metadata *provider_of(IWICStreamProvider *iface)
{
	return WIC_CONTAINING(iface, metadata, provider);
}

static HRESULT WINAPI provider_QueryInterface(IWICStreamProvider *iface, REFIID riid, void **out)
{
	return item_QueryInterface(&provider_of(iface)->iface, riid, out);
}

static ULONG WINAPI provider_AddRef(IWICStreamProvider *iface)
{
	return item_AddRef(&provider_of(iface)->iface);
}

static ULONG WINAPI provider_Release(IWICStreamProvider *iface)
{
	return item_Release(&provider_of(iface)->iface);
}

/* Asking for the stream sets it back to where the block starts. */
static HRESULT WINAPI provider_GetStream(IWICStreamProvider *iface, IStream **stream)
{
	metadata *self = provider_of(iface);
	if (!stream)
		return E_INVALIDARG;
	if (!self->stream)
		return WINCODEC_ERR_STREAMNOTAVAILABLE;
	{
		LARGE_INTEGER back;
		back.QuadPart = (LONGLONG)self->origin.QuadPart;
		IStream_Seek(self->stream, back, STREAM_SEEK_SET, NULL);
	}
	*stream = self->stream;
	IStream_AddRef(self->stream);
	return S_OK;
}

static HRESULT WINAPI provider_GetPersistOptions(IWICStreamProvider *iface, DWORD *options)
{
	if (!options)
		return E_INVALIDARG;
	*options = provider_of(iface)->options;
	return S_OK;
}

static HRESULT WINAPI provider_GetPreferredVendorGUID(IWICStreamProvider *iface, GUID *vendor)
{
	if (!vendor)
		return E_INVALIDARG;
	*vendor = provider_of(iface)->vendor;
	return S_OK;
}

static HRESULT WINAPI provider_RefreshStream(IWICStreamProvider *iface)
{
	return E_NOTIMPL;
}

static const IWICStreamProviderVtbl provider_vtbl = {
	provider_QueryInterface, provider_AddRef, provider_Release, provider_GetStream, provider_GetPersistOptions, provider_GetPreferredVendorGUID, provider_RefreshStream,
};

static HRESULT metadata_make(const metadata_kind *kind, BOOL writable, metadata **out)
{
	metadata *self = wic_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &item_vtbl;
	self->persist.lpVtbl = &persist_vtbl;
	self->provider.lpVtbl = &provider_vtbl;
	self->references = 1;
	self->kind = kind;
	self->writable = writable;
	self->vendor = GUID_VendorMicrosoft;
	seed_defaults(self);
	*out = self;
	return S_OK;
}

/* The class of a reader or writer, or nothing for any other class. */
BOOL wic_metadata_class(REFCLSID clsid, REFIID riid, void **out, HRESULT *result)
{
	UINT index;
	for (index = 0; index < ARRAYSIZE(kinds); index++) {
		const BOOL writable = IsEqualGUID(clsid, kinds[index].writer);
		if (writable || IsEqualGUID(clsid, kinds[index].reader)) {
			metadata *self;
			if (SUCCEEDED(*result = metadata_make(&kinds[index], writable, &self))) {
				*result = item_QueryInterface(&self->iface, riid, out);
				item_Release(&self->iface);
			}
			return TRUE;
		}
	}
	return FALSE;
}

const CLSID *wic_metadata_class_at(UINT index)
{
	const UINT kind = index / 2;
	if (kind >= ARRAYSIZE(kinds))
		return NULL;
	return index % 2 ? kinds[kind].writer : kinds[kind].reader;
}

const WCHAR *wic_metadata_name(REFCLSID clsid, const GUID **format, BOOL *writer)
{
	UINT index;
	for (index = 0; index < ARRAYSIZE(kinds); index++) {
		*writer = IsEqualGUID(clsid, kinds[index].writer);
		if (*writer || IsEqualGUID(clsid, kinds[index].reader)) {
			*format = kinds[index].format;
			return kinds[index].name;
		}
	}
	return NULL;
}

/* A block of no known format falls back to the unknown reader, unless the caller refuses that. */
HRESULT wic_metadata_reader_create(REFGUID format, const GUID *vendor, DWORD options, IStream *stream, IWICMetadataReader **out)
{
	const metadata_kind *chosen = &kinds[0];
	metadata *self;
	UINT index;
	HRESULT result;
	if (!format || !out)
		return E_INVALIDARG;
	*out = NULL;
	for (index = 0; index < ARRAYSIZE(kinds); index++)
		if (IsEqualGUID(format, kinds[index].format))
			chosen = &kinds[index];
	if (chosen == &kinds[0] && (options & WICMetadataCreationFailUnknown) && !IsEqualGUID(format, &GUID_MetadataFormatUnknown))
		return WINCODEC_ERR_COMPONENTNOTFOUND;
	if (FAILED(result = metadata_make(chosen, FALSE, &self)))
		return result;
	if (stream && FAILED(result = persist_LoadEx(&self->persist, stream, vendor, options & WICPersistOptionMask))) {
		item_Release(&self->iface);
		return result;
	}
	*out = (IWICMetadataReader *)&self->iface;
	return S_OK;
}

HRESULT wic_metadata_writer_create(REFGUID format, const GUID *vendor, DWORD options, IWICMetadataWriter **out)
{
	metadata *self;
	UINT index;
	HRESULT result;
	if (!format || !out || (options & ~WICMetadataCreationFailUnknown))
		return E_INVALIDARG;
	for (index = 0; index < ARRAYSIZE(kinds); index++)
		if (IsEqualGUID(format, kinds[index].format))
			break;
	/* A format nobody handles gets the unknown writer, unless asked to fail. */
	if (index == ARRAYSIZE(kinds)) {
		if (options & WICMetadataCreationFailUnknown)
			return WINCODEC_ERR_COMPONENTNOTFOUND;
		index = 0;
	}
	if (FAILED(result = metadata_make(&kinds[index], TRUE, &self)))
		return result;
	*out = &self->iface;
	return S_OK;
}

/* The writer starts as a copy of what the reader holds; a block held inside becomes a writer of its own. */
HRESULT wic_metadata_writer_from_reader(IWICMetadataReader *reader, const GUID *vendor, IWICMetadataWriter **out)
{
	IWICMetadataWriter *writer;
	GUID format;
	UINT count = 0, index;
	HRESULT result;
	if (!reader || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (FAILED(result = IWICMetadataReader_GetMetadataFormat(reader, &format)) || FAILED(result = IWICMetadataReader_GetCount(reader, &count))
		|| FAILED(result = wic_metadata_writer_create(&format, vendor, 0, &writer)))
		return result;
	for (index = 0; index < count && SUCCEEDED(result); index++) {
		PROPVARIANT schema, id, value;
		IWICMetadataReader *held;
		PropVariantInit(&schema);
		PropVariantInit(&id);
		PropVariantInit(&value);
		if (SUCCEEDED(result = IWICMetadataReader_GetValueByIndex(reader, index, &schema, &id, &value))) {
			if (value.vt == VT_UNKNOWN && value.punkVal && SUCCEEDED(IUnknown_QueryInterface(value.punkVal, &IID_IWICMetadataReader, (void **)&held))) {
				IWICMetadataWriter *nested;
				if (SUCCEEDED(result = wic_metadata_writer_from_reader(held, vendor, &nested))) {
					IUnknown_Release(value.punkVal);
					value.punkVal = (IUnknown *)nested;
				}
				IWICMetadataReader_Release(held);
			}
			if (SUCCEEDED(result))
				result = IWICMetadataWriter_SetValue(writer, &schema, &id, &value);
		}
		PropVariantClear(&schema);
		PropVariantClear(&id);
		PropVariantClear(&value);
	}
	if (FAILED(result)) {
		IWICMetadataWriter_Release(writer);
		return result;
	}
	/* The writer takes the reader's options, and reads the same bytes through a stream of its own. */
	{
		metadata *made = metadata_of(writer);
		IWICPersistStream *persist;
		IWICStreamProvider *provider;
		IStream *source;
		if (SUCCEEDED(IWICMetadataReader_QueryInterface(reader, &IID_IWICStreamProvider, (void **)&provider))) {
			IWICStreamProvider_GetPersistOptions(provider, &made->options);
			if (SUCCEEDED(IWICStreamProvider_GetStream(provider, &source))) {
				IWICStream *copy;
				LARGE_INTEGER back;
				back.QuadPart = 0;
				if (SUCCEEDED(IWICMetadataReader_QueryInterface(reader, &IID_IWICPersistStream, (void **)&persist))) {
					back.QuadPart = (LONGLONG)persist_of(persist)->origin.QuadPart;
					IWICPersistStream_Release(persist);
				}
				IStream_Seek(source, back, STREAM_SEEK_SET, NULL);
				if (SUCCEEDED(wic_stream_create(&copy))) {
					if (SUCCEEDED(IWICStream_InitializeFromIStream(copy, source)) && SUCCEEDED(IWICStream_Seek(copy, back, STREAM_SEEK_SET, NULL))) {
						made->stream = (IStream *)copy;
						made->origin.QuadPart = (ULONGLONG)back.QuadPart;
					}
					else
						IWICStream_Release(copy);
				}
				IStream_Release(source);
			}
			IWICStreamProvider_Release(provider);
		}
	}
	*out = writer;
	return S_OK;
}

/* A container names its blocks its own way; only PNG chunks are told apart by the bytes here, the rest read as unknown. */
HRESULT wic_metadata_reader_from_container(REFGUID container, const GUID *vendor, DWORD options, IStream *stream, IWICMetadataReader **out)
{
	const GUID *format = &GUID_MetadataFormatUnknown;
	if (!container || !stream || !out)
		return E_INVALIDARG;
	*out = NULL;
	{
		BYTE head[8];
		ULONG got = 0;
		LARGE_INTEGER back;
		if (SUCCEEDED(IStream_Read(stream, head, sizeof(head), &got))) {
			if (got >= 2 && IsEqualGUID(container, &GUID_ContainerFormatGif) && head[0] == 0x21) {
				if (head[1] == 0xf9)
					format = &GUID_MetadataFormatGCE;
				else if (head[1] == 0xff)
					format = &GUID_MetadataFormatAPE;
				else if (head[1] == 0xfe)
					format = &GUID_MetadataFormatGifComment;
			}
			else if (got >= 3 && IsEqualGUID(container, &GUID_ContainerFormatGif) && !memcmp(head, "GIF", 3))
				format = &GUID_MetadataFormatLSD;
			else if (got >= 1 && IsEqualGUID(container, &GUID_ContainerFormatGif) && head[0] == 0x2c)
				format = &GUID_MetadataFormatIMD;
		}
		back.QuadPart = -(LONGLONG)got;
		IStream_Seek(stream, back, STREAM_SEEK_CUR, NULL);
	}
	if (IsEqualGUID(container, &GUID_ContainerFormatPng)) {
		static const struct
		{
			char type[5];
			const GUID *format;
		} chunks[] = {
			{"tEXt", &GUID_MetadataFormatChunktEXt}, {"gAMA", &GUID_MetadataFormatChunkgAMA}, {"cHRM", &GUID_MetadataFormatChunkcHRM},
			{"hIST", &GUID_MetadataFormatChunkhIST}, {"tIME", &GUID_MetadataFormatChunktIME}, {"bKGD", &GUID_MetadataFormatChunkbKGD},
		};
		BYTE head[8];
		ULONG got = 0;
		LARGE_INTEGER back;
		UINT index;
		if (SUCCEEDED(IStream_Read(stream, head, sizeof(head), &got)) && got == sizeof(head))
			for (index = 0; index < ARRAYSIZE(chunks); index++)
				if (!memcmp(head + 4, chunks[index].type, 4))
					format = chunks[index].format;
		back.QuadPart = -(LONGLONG)got;
		IStream_Seek(stream, back, STREAM_SEEK_CUR, NULL);
	}
	if ((options & WICMetadataCreationFailUnknown) && format == &GUID_MetadataFormatUnknown)
		return WINCODEC_ERR_COMPONENTNOTFOUND;
	return wic_metadata_reader_create(format, vendor, options, stream, out);
}

/* Only a block that stands on its own in a file can be written from; APP1 is a wrapper around one. */
BOOL wic_metadata_format_writable(const GUID *format)
{
	return !IsEqualGUID(format, &GUID_MetadataFormatApp1);
}
