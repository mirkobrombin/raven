/* SPDX-License-Identifier: MIT */
#include "wic.h"

/* The metadata blocks of a decoded file, found by walking its structure: each is handed to a reader over a copy of its bytes. */
typedef struct
{
	const GUID *format;
	SIZE_T at, size;
	/* Where a directory begins inside its bytes, and the byte order it was written in. */
	SIZE_T start;
	DWORD options;
} block;

typedef struct
{
	IWICMetadataBlockReader iface;
	LONG references;
	const GUID *container;
	BYTE *data;
	block blocks[64];
	IWICMetadataReader *made[64];
	UINT count;
} block_reader;

static void add_block(block_reader *self, const GUID *format, SIZE_T at, SIZE_T size)
{
	if (self->count < ARRAYSIZE(self->blocks)) {
		self->blocks[self->count].format = format;
		self->blocks[self->count].at = at;
		self->blocks[self->count++].size = size;
	}
}

static SIZE_T skip_subblocks(const BYTE *data, SIZE_T size, SIZE_T at)
{
	while (at < size && data[at])
		at += (SIZE_T)data[at] + 1;
	return at < size ? at + 1 : size;
}

/* The screen belongs to the file; a frame owns what stands before its image, its own description first. */
static void walk_gif(block_reader *self, const BYTE *data, SIZE_T size, INT wanted)
{
	SIZE_T at = 13;
	INT frame = 0, first = -1;
	if (size < 13)
		return;
	if (wanted < 0)
		add_block(self, &GUID_MetadataFormatLSD, 0, 13);
	if (data[10] & 0x80)
		at += (SIZE_T)3 << ((data[10] & 7) + 1);
	while (at < size && data[at] != 0x3b) {
		if (data[at] == 0x2c) {
			const BYTE flags = at + 10 <= size ? data[at + 9] : 0;
			if (frame == wanted) {
				/* The description goes before the extensions that were met on the way here. */
				block held[8];
				UINT kept = 0, index;
				if (first >= 0)
					while (self->count > (UINT)first && kept < ARRAYSIZE(held))
						held[kept++] = self->blocks[--self->count];
				add_block(self, &GUID_MetadataFormatIMD, at + 1, 9);
				for (index = kept; index--;)
					add_block(self, held[index].format, held[index].at, held[index].size);
				return;
			}
			at += 10;
			if (flags & 0x80)
				at += (SIZE_T)3 << ((flags & 7) + 1);
			at = skip_subblocks(data, size, at + 1);
			frame++;
			first = -1;
		}
		else if (data[at] == 0x21 && at + 2 < size) {
			const BYTE label = data[at + 1];
			const SIZE_T end = skip_subblocks(data, size, at + 2);
			const BOOL here = frame == wanted, file = wanted < 0;
			if (here && first < 0)
				first = (INT)self->count;
			if (label == 0xf9 && here && at + 7 <= size)
				add_block(self, &GUID_MetadataFormatGCE, at + 3, 4);
			else if (label == 0xff && (here || (file && !frame)))
				add_block(self, &GUID_MetadataFormatAPE, at, end - at);
			else if (label == 0xfe && (here || (file && !frame)))
				add_block(self, &GUID_MetadataFormatGifComment, at, end - at);
			else if (label != 0xf9 && label != 0xff && label != 0xfe && (here || (file && !frame)))
				add_block(self, &GUID_MetadataFormatUnknown, at, end - at);
			at = end;
		}
		else
			return;
	}
}

static void walk_png(block_reader *self, const BYTE *data, SIZE_T size)
{
	SIZE_T at = 8;
	while (at + 12 <= size) {
		const SIZE_T length = (SIZE_T)data[at] << 24 | data[at + 1] << 16 | data[at + 2] << 8 | data[at + 3];
		const BYTE *type = data + at + 4;
		const GUID *format = &GUID_MetadataFormatUnknown;
		if (length > size - at - 12)
			return;
		if (!memcmp(type, "tEXt", 4))
			format = &GUID_MetadataFormatChunktEXt;
		else if (!memcmp(type, "gAMA", 4))
			format = &GUID_MetadataFormatChunkgAMA;
		else if (!memcmp(type, "cHRM", 4))
			format = &GUID_MetadataFormatChunkcHRM;
		else if (!memcmp(type, "hIST", 4))
			format = &GUID_MetadataFormatChunkhIST;
		else if (!memcmp(type, "tIME", 4))
			format = &GUID_MetadataFormatChunktIME;
		else if (!memcmp(type, "bKGD", 4))
			format = &GUID_MetadataFormatChunkbKGD;
		/* Only the chunks with a reader of their own are metadata. */
		if (format == &GUID_MetadataFormatUnknown) {
			at += length + 12;
			continue;
		}
		if (memcmp(type, "IHDR", 4) && memcmp(type, "PLTE", 4) && memcmp(type, "IDAT", 4) && memcmp(type, "IEND", 4) && memcmp(type, "tRNS", 4))
			add_block(self, format, at, length + 12);
		at += length + 12;
	}
}

/* A TIFF frame is one directory, whose values lie anywhere in the file. */
static void walk_tiff(block_reader *self, const BYTE *data, SIZE_T size, INT wanted)
{
	if (size < 8 || (data[0] != 'I' && data[0] != 'M') || data[0] != data[1])
		return;
	const BOOL big = data[0] == 'M';
	const BYTE *field = data + 4;
	ULONG offset = big ? (ULONG)field[0] << 24 | field[1] << 16 | field[2] << 8 | field[3] : (ULONG)field[3] << 24 | field[2] << 16 | field[1] << 8 | field[0];
	for (INT frame = 0; offset && offset + 2 <= size; frame++) {
		const BYTE *at = data + offset;
		const SIZE_T entries = big ? (SIZE_T)at[0] << 8 | at[1] : (SIZE_T)at[1] << 8 | at[0];
		if (frame == wanted) {
			add_block(self, &GUID_MetadataFormatIfd, 0, size);
			self->blocks[self->count - 1].start = offset;
			self->blocks[self->count - 1].options = big ? WICPersistOptionBigEndian : WICPersistOptionLittleEndian;
			return;
		}
		if (offset + 2 + entries * 12 + 4 > size)
			return;
		at += 2 + entries * 12;
		offset = big ? (ULONG)at[0] << 24 | at[1] << 16 | at[2] << 8 | at[3] : (ULONG)at[3] << 24 | at[2] << 16 | at[1] << 8 | at[0];
	}
}

/* A JPEG frame's metadata is its application segments, up to the start of the scan. */
static void walk_jpeg(block_reader *self, const BYTE *data, SIZE_T size)
{
	static const BYTE exif[6] = {'E', 'x', 'i', 'f', 0, 0};
	SIZE_T at = 2;
	while (at + 4 <= size && data[at] == 0xff && data[at + 1] != 0xda) {
		const SIZE_T length = (SIZE_T)data[at + 2] << 8 | data[at + 3];
		if (length < 2 || at + 2 + length > size)
			return;
		if (data[at + 1] == 0xe1 && length >= 8 && !memcmp(data + at + 4, exif, sizeof(exif)))
			add_block(self, &GUID_MetadataFormatApp1, at + 4, length - 2);
		at += 2 + length;
	}
}

static block_reader *reader_of(IWICMetadataBlockReader *iface)
{
	return WIC_CONTAINING(iface, block_reader, iface);
}

static HRESULT WINAPI blocks_QueryInterface(IWICMetadataBlockReader *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICMetadataBlockReader)) {
		*out = iface;
		IWICMetadataBlockReader_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI blocks_AddRef(IWICMetadataBlockReader *iface)
{
	return InterlockedIncrement(&reader_of(iface)->references);
}

static ULONG WINAPI blocks_Release(IWICMetadataBlockReader *iface)
{
	block_reader *self = reader_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		UINT index;
		for (index = 0; index < self->count; index++)
			if (self->made[index])
				IWICMetadataReader_Release(self->made[index]);
		wic_free(self->data);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI blocks_GetContainerFormat(IWICMetadataBlockReader *iface, GUID *format)
{
	if (!format)
		return E_INVALIDARG;
	*format = *reader_of(iface)->container;
	return S_OK;
}

static HRESULT WINAPI blocks_GetCount(IWICMetadataBlockReader *iface, UINT *count)
{
	if (!count)
		return E_INVALIDARG;
	*count = reader_of(iface)->count;
	return S_OK;
}

static HRESULT WINAPI blocks_GetReaderByIndex(IWICMetadataBlockReader *iface, UINT index, IWICMetadataReader **out)
{
	block_reader *self = reader_of(iface);
	HGLOBAL memory;
	IStream *stream;
	HRESULT result;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (index >= self->count)
		return E_INVALIDARG;
	/* Asking twice gets the same reader. */
	if (self->made[index]) {
		*out = self->made[index];
		IWICMetadataReader_AddRef(*out);
		return S_OK;
	}
	if (!(memory = GlobalAlloc(GMEM_MOVEABLE, self->blocks[index].size ? self->blocks[index].size : 1)))
		return E_OUTOFMEMORY;
	memcpy(GlobalLock(memory), self->data + self->blocks[index].at, self->blocks[index].size);
	GlobalUnlock(memory);
	if (FAILED(result = CreateStreamOnHGlobal(memory, TRUE, &stream))) {
		GlobalFree(memory);
		return result;
	}
	{
		LARGE_INTEGER start;
		start.QuadPart = (LONGLONG)self->blocks[index].start;
		IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
	}
	result = wic_metadata_reader_create(self->blocks[index].format, NULL, self->blocks[index].options, stream, out);
	IStream_Release(stream);
	if (SUCCEEDED(result)) {
		self->made[index] = *out;
		IWICMetadataReader_AddRef(*out);
	}
	return result;
}

/* The enumerator holds a reader of every block, made when it is asked for. */
typedef struct
{
	IEnumUnknown iface;
	LONG references;
	IWICMetadataBlockReader *owner;
	UINT at;
} block_walk;

static HRESULT walk_make(IWICMetadataBlockReader *owner, UINT at, IEnumUnknown **out);

static block_walk *block_walk_of(IEnumUnknown *iface)
{
	return WIC_CONTAINING(iface, block_walk, iface);
}

static HRESULT WINAPI list_QueryInterface(IEnumUnknown *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IEnumUnknown)) {
		*out = iface;
		IEnumUnknown_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI list_AddRef(IEnumUnknown *iface)
{
	return InterlockedIncrement(&block_walk_of(iface)->references);
}

static ULONG WINAPI list_Release(IEnumUnknown *iface)
{
	block_walk *self = block_walk_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		IWICMetadataBlockReader_Release(self->owner);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI list_Next(IEnumUnknown *iface, ULONG count, IUnknown **out, ULONG *fetched)
{
	block_walk *self = block_walk_of(iface);
	ULONG got = 0;
	UINT total = 0;
	if (!out)
		return E_INVALIDARG;
	IWICMetadataBlockReader_GetCount(self->owner, &total);
	while (got < count && self->at < total) {
		if (FAILED(IWICMetadataBlockReader_GetReaderByIndex(self->owner, self->at++, (IWICMetadataReader **)&out[got])))
			break;
		got++;
	}
	if (fetched)
		*fetched = got;
	return got == count ? S_OK : S_FALSE;
}

static HRESULT WINAPI list_Skip(IEnumUnknown *iface, ULONG count)
{
	block_walk *self = block_walk_of(iface);
	UINT total = 0;
	IWICMetadataBlockReader_GetCount(self->owner, &total);
	if (self->at + count > total) {
		self->at = total;
		return S_FALSE;
	}
	self->at += count;
	return S_OK;
}

static HRESULT WINAPI list_Reset(IEnumUnknown *iface)
{
	block_walk_of(iface)->at = 0;
	return S_OK;
}

static HRESULT WINAPI list_Clone(IEnumUnknown *iface, IEnumUnknown **out)
{
	if (!out)
		return E_INVALIDARG;
	return walk_make(block_walk_of(iface)->owner, block_walk_of(iface)->at, out);
}

static const IEnumUnknownVtbl list_vtbl = {
	list_QueryInterface, list_AddRef, list_Release, list_Next, list_Skip, list_Reset, list_Clone,
};

static HRESULT walk_make(IWICMetadataBlockReader *owner, UINT at, IEnumUnknown **out)
{
	block_walk *self = wic_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &list_vtbl;
	self->references = 1;
	self->owner = owner;
	self->at = at;
	IWICMetadataBlockReader_AddRef(owner);
	*out = &self->iface;
	return S_OK;
}

static HRESULT WINAPI blocks_GetEnumerator(IWICMetadataBlockReader *iface, IEnumUnknown **out)
{
	if (!out)
		return E_INVALIDARG;
	return walk_make(iface, 0, out);
}

static const IWICMetadataBlockReaderVtbl blocks_vtbl = {
	blocks_QueryInterface, blocks_AddRef, blocks_Release, blocks_GetContainerFormat, blocks_GetCount, blocks_GetReaderByIndex, blocks_GetEnumerator,
};

/* The file's own blocks when the frame is negative, else one frame's. */
HRESULT wic_block_reader_create(const GUID *container, const BYTE *data, SIZE_T size, INT frame, REFIID riid, void **out)
{
	block_reader *self;
	HRESULT result;
	if (!(self = wic_alloc(sizeof(*self))))
		return E_OUTOFMEMORY;
	if (!(self->data = wic_alloc(size ? size : 1))) {
		wic_free(self);
		return E_OUTOFMEMORY;
	}
	memcpy(self->data, data, size);
	self->iface.lpVtbl = &blocks_vtbl;
	self->references = 1;
	self->container = container;
	if (IsEqualGUID(container, &GUID_ContainerFormatGif))
		walk_gif(self, data, size, frame);
	else if (IsEqualGUID(container, &GUID_ContainerFormatPng) && frame >= 0)
		walk_png(self, data, size);
	else if (IsEqualGUID(container, &GUID_ContainerFormatTiff) && frame >= 0)
		walk_tiff(self, data, size, frame);
	else if (IsEqualGUID(container, &GUID_ContainerFormatJpeg) && frame >= 0)
		walk_jpeg(self, data, size);
	result = blocks_QueryInterface(&self->iface, riid, out);
	blocks_Release(&self->iface);
	return result;
}

/* ---- The short names of the metadata query language. ---- */
static const struct
{
	const WCHAR *name;
	const GUID *format;
} short_names[] = {
	{L"8bimiptc", &GUID_MetadataFormat8BIMIPTC},
	{L"8bimiptcdigest", &GUID_MetadataFormat8BIMIPTCDigest},
	{L"8bimResInfo", &GUID_MetadataFormat8BIMResolutionInfo},
	{L"app0", &GUID_MetadataFormatApp0},
	{L"app1", &GUID_MetadataFormatApp1},
	{L"app13", &GUID_MetadataFormatApp13},
	{L"appext", &GUID_MetadataFormatAPE},
	{L"bKGD", &GUID_MetadataFormatChunkbKGD},
	{L"cHRM", &GUID_MetadataFormatChunkcHRM},
	{L"chrominance", &GUID_MetadataFormatJpegChrominance},
	{L"com", &GUID_MetadataFormatJpegComment},
	{L"commentext", &GUID_MetadataFormatGifComment},
	{L"exif", &GUID_MetadataFormatExif},
	{L"gAMA", &GUID_MetadataFormatChunkgAMA},
	{L"gps", &GUID_MetadataFormatGps},
	{L"grctlext", &GUID_MetadataFormatGCE},
	{L"hIST", &GUID_MetadataFormatChunkhIST},
	{L"iCCP", &GUID_MetadataFormatChunkiCCP},
	{L"ifd", &GUID_MetadataFormatIfd},
	{L"imgdesc", &GUID_MetadataFormatIMD},
	{L"interop", &GUID_MetadataFormatInterop},
	{L"iptc", &GUID_MetadataFormatIPTC},
	{L"irb", &GUID_MetadataFormatIRB},
	{L"iTXt", &GUID_MetadataFormatChunkiTXt},
	{L"logscrdesc", &GUID_MetadataFormatLSD},
	{L"luminance", &GUID_MetadataFormatJpegLuminance},
	{L"sRGB", &GUID_MetadataFormatChunksRGB},
	{L"sub", &GUID_MetadataFormatSubIfd},
	{L"tEXt", &GUID_MetadataFormatChunktEXt},
	{L"thumb", &GUID_MetadataFormatThumbnail},
	{L"tIME", &GUID_MetadataFormatChunktIME},
	{L"unknown", &GUID_MetadataFormatUnknown},
	{L"xmp", &GUID_MetadataFormatXMP},
	{L"xmpalt", &GUID_MetadataFormatXMPAlt},
	{L"xmpbag", &GUID_MetadataFormatXMPBag},
	{L"xmpseq", &GUID_MetadataFormatXMPSeq},
	{L"xmpstruct", &GUID_MetadataFormatXMPStruct},
};

/* The namespaces the XMP specification and its companions name. */
static const struct
{
	const WCHAR *schema, *name;
} schema_names[] = {
	{L"http://www.w3.org/1999/02/22-rdf-syntax-ns#", L"rdf"},
	{L"http://purl.org/dc/elements/1.1/", L"dc"},
	{L"http://ns.adobe.com/xap/1.0/", L"xmp"},
	{L"http://ns.adobe.com/xmp/Identifier/qual/1.0/", L"xmpidq"},
	{L"http://ns.adobe.com/xap/1.0/rights/", L"xmpRights"},
	{L"http://ns.adobe.com/xap/1.0/mm/", L"xmpMM"},
	{L"http://ns.adobe.com/xap/1.0/bj/", L"xmpBJ"},
	{L"http://ns.adobe.com/xap/1.0/t/pg/", L"xmpTPg"},
	{L"http://ns.adobe.com/pdf/1.3/", L"pdf"},
	{L"http://ns.adobe.com/photoshop/1.0/", L"photoshop"},
	{L"http://ns.adobe.com/tiff/1.0/", L"tiff"},
	{L"http://ns.adobe.com/exif/1.0/", L"exif"},
	{L"http://ns.adobe.com/xap/1.0/sType/Dimensions#", L"stDim"},
	{L"http://ns.adobe.com/xap/1.0/g/img/", L"xapGImg"},
	{L"http://ns.adobe.com/xap/1.0/sType/ResourceEvent#", L"stEvt"},
	{L"http://ns.adobe.com/xap/1.0/sType/ResourceRef#", L"stRef"},
	{L"http://ns.adobe.com/xap/1.0/sType/Version#", L"stVer"},
	{L"http://ns.adobe.com/xap/1.0/sType/Job#", L"stJob"},
	{L"http://ns.adobe.com/exif/1.0/aux/", L"aux"},
	{L"http://ns.adobe.com/camera-raw-settings/1.0/", L"crs"},
	{L"http://ns.adobe.com/xmp/1.0/DynamicMedia/", L"xmpDM"},
	{L"http://iptc.org/std/Iptc4xmpCore/1.0/xmlns/", L"Iptc4xmpCore"},
	{L"http://ns.microsoft.com/photo/1.0/", L"MicrosoftPhoto"},
	{L"http://ns.microsoft.com/photo/1.2/", L"MP"},
	{L"http://ns.microsoft.com/photo/1.2/t/RegionInfo#", L"MPRI"},
	{L"http://ns.microsoft.com/photo/1.2/t/Region#", L"MPReg"},
};

/* The room counts the ending zero; too little of it still gets as much of the name as fits. */
static HRESULT give_name(const WCHAR *name, UINT room, WCHAR *out, UINT *actual)
{
	const UINT length = lstrlenW(name) + 1;
	if (room && !out)
		room = 0;
	if (!room && out)
		return E_INVALIDARG;
	if (!out) {
		if (actual)
			*actual = length;
		return S_OK;
	}
	if (room < length) {
		memcpy(out, name, (room - 1) * sizeof(WCHAR));
		out[room - 1] = 0;
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	}
	memcpy(out, name, length * sizeof(WCHAR));
	if (actual)
		*actual = length;
	return S_OK;
}

HRESULT WINAPI WICMapGuidToShortName(REFGUID guid, UINT room, WCHAR *out, UINT *actual)
{
	UINT index;
	if (!guid || (out && !room) || (!out && !actual && room > 0 && 0))
		return E_INVALIDARG;
	for (index = 0; index < ARRAYSIZE(short_names); index++)
		if (IsEqualGUID(guid, short_names[index].format))
			return give_name(short_names[index].name, room, out, actual);
	return WINCODEC_ERR_PROPERTYNOTFOUND;
}

HRESULT WINAPI WICMapShortNameToGuid(const WCHAR *name, GUID *guid)
{
	UINT index;
	if (!name || !guid)
		return E_INVALIDARG;
	for (index = 0; index < ARRAYSIZE(short_names); index++)
		if (!lstrcmpiW(name, short_names[index].name)) {
			*guid = *short_names[index].format;
			return S_OK;
		}
	return WINCODEC_ERR_PROPERTYNOTFOUND;
}

HRESULT WINAPI WICMapSchemaToName(REFGUID format, WCHAR *schema, UINT room, WCHAR *out, UINT *actual)
{
	UINT index;
	if (!format || !schema || !actual || (out && !room))
		return E_INVALIDARG;
	if (IsEqualGUID(format, &GUID_MetadataFormatXMP) || IsEqualGUID(format, &GUID_MetadataFormatXMPStruct))
		for (index = 0; index < ARRAYSIZE(schema_names); index++)
			if (!lstrcmpW(schema, schema_names[index].schema))
				return give_name(schema_names[index].name, room, out, actual);
	return WINCODEC_ERR_PROPERTYNOTFOUND;
}

const WCHAR *wic_schema_of_name(const WCHAR *name)
{
	UINT index;
	for (index = 0; index < ARRAYSIZE(schema_names); index++)
		if (!lstrcmpW(name, schema_names[index].name))
			return schema_names[index].schema;
	return NULL;
}
