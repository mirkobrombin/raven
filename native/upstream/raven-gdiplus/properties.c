/* SPDX-License-Identifier: MIT */
#include "image.h"
#include "gmath.h"
#include <wincodecsdk.h>

// Property items come from the codec's metadata. Tags of the TIFF family keep their own numbers; the other formats have
// theirs translated to the numbers GDI+ documents for them.
enum
{
	TagTypeByte = 1, TagTypeAscii, TagTypeShort, TagTypeLong, TagTypeRational, TagTypeUndefined = 7, TagTypeSLong = 9, TagTypeSRational,
};
enum
{
	TagGamma = 0x301, TagImageTitle = 0x320, TagArtist = 0x13b, TagDescription = 0x10e, TagSoftware = 0x131, TagCopyright = 0x8298,
	TagDateTime = 0x132, TagFrameDelay = 0x5100, TagLoopCount, TagGlobalPalette, TagIndexBackground, TagIndexTransparent,
	TagPixelUnit = 0x5110, TagPixelPerUnitX, TagPixelPerUnitY, TagPaletteHistogram, TagUserComment = 0x9286,
};

extern HRESULT WINAPI PropVariantClear(PROPVARIANT *);

static const GUID reader_iid = {0x9204fe99, 0xd8fc, 0x4fd5, {0xa0, 0x01, 0x95, 0x36, 0xb0, 0x67, 0xa8, 0x99}};
static const GUID block_reader_iid = {0xfeaa2a8d, 0xb3f3, 0x43e4, {0xb2, 0x5c, 0xd1, 0xde, 0x99, 0x0a, 0x1a, 0xe1}};

GpStatus gp_property_set(GpImage *image, PROPID id, WORD type, ULONG length, const void *value)
{
	void *copy = gp_dup(value, length);
	if (!copy)
		return OutOfMemory;
	for (UINT i = 0; i < image->property_count; i++) {
		if (image->properties[i].id == id) {
			gp_free(image->properties[i].value);
			image->properties[i].type = type;
			image->properties[i].length = length;
			image->properties[i].value = copy;
			return Ok;
		}
	}
	PropertyItem *grown = gp_realloc(image->properties, (image->property_count + 1) * sizeof(PropertyItem));
	if (!grown) {
		gp_free(copy);
		return OutOfMemory;
	}
	image->properties = grown;
	PropertyItem *item = &grown[image->property_count++];
	item->id = id;
	item->type = type;
	item->length = length;
	item->value = copy;
	return Ok;
}

void gp_property_clear(GpImage *image)
{
	for (UINT i = 0; i < image->property_count; i++)
		gp_free(image->properties[i].value);
	gp_free(image->properties);
	image->properties = NULL;
	image->property_count = 0;
}

// The tag type a metadata value stands for, and where its bytes are.
static BOOL value_bytes(const PROPVARIANT *value, WORD *type, ULONG *length, const void **data)
{
	static const struct { VARTYPE vt; WORD type; BYTE size; } kinds[] = {
		{VT_UI1, TagTypeByte, 1}, {VT_I1, 6, 1}, {VT_UI2, TagTypeShort, 2}, {VT_I2, 8, 2}, {VT_UI4, TagTypeLong, 4}, {VT_I4, TagTypeSLong, 4},
		{VT_UI8, TagTypeRational, 8}, {VT_I8, TagTypeSRational, 8}, {VT_R4, 11, 4}, {VT_R8, 12, 8},
	};
	VARTYPE base = value->vt & VT_TYPEMASK;
	if (base == VT_LPSTR && !(value->vt & VT_VECTOR)) {
		*type = TagTypeAscii;
		*length = value->pszVal ? (ULONG)strlen(value->pszVal) + 1 : 0;
		*data = value->pszVal;
		return value->pszVal != NULL;
	}
	if (value->vt == VT_BLOB) {
		*type = TagTypeUndefined;
		*length = value->blob.cbSize;
		*data = value->blob.pBlobData;
		return TRUE;
	}
	for (size_t i = 0; i < ARRAYSIZE(kinds); i++) {
		if (kinds[i].vt != base)
			continue;
		*type = kinds[i].type;
		if (value->vt & VT_VECTOR) {
			*length = value->caub.cElems * kinds[i].size;
			*data = value->caub.pElems;
		} else {
			*length = kinds[i].size;
			*data = &value->bVal;
		}
		return TRUE;
	}
	return FALSE;
}

static void add_value(GpImage *image, PROPID id, const PROPVARIANT *value)
{
	WORD type;
	ULONG length;
	const void *data;
	if (value_bytes(value, &type, &length, &data))
		gp_property_set(image, id, type, length, data);
}

// A nested directory comes as a reader of its own, in the middle of the entries of its parent.
static void walk_reader(GpImage *image, IWICMetadataReader *reader)
{
	IWICEnumMetadataItem *items;
	if (FAILED(IWICMetadataReader_GetEnumerator(reader, &items)))
		return;
	PROPVARIANT schema, id, value;
	ULONG got;
	PropVariantInit(&schema);
	PropVariantInit(&id);
	PropVariantInit(&value);
	while (IWICEnumMetadataItem_Next(items, 1, &schema, &id, &value, &got) == S_OK && got) {
		if (value.vt == VT_UNKNOWN && value.punkVal) {
			IWICMetadataReader *inner;
			if (SUCCEEDED(IUnknown_QueryInterface(value.punkVal, &reader_iid, (void **)&inner))) {
				walk_reader(image, inner);
				IWICMetadataReader_Release(inner);
			}
		} else if (id.vt == VT_UI2) {
			add_value(image, id.uiVal, &value);
		}
		PropVariantClear(&schema);
		PropVariantClear(&id);
		PropVariantClear(&value);
	}
	IWICEnumMetadataItem_Release(items);
}

static BOOL query_value(IWICMetadataQueryReader *query, const WCHAR *name, PROPVARIANT *out)
{
	PropVariantInit(out);
	return query && SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(query, name, out));
}

static UINT query_number(IWICMetadataQueryReader *query, const WCHAR *name, UINT otherwise)
{
	PROPVARIANT value;
	if (!query_value(query, name, &value))
		return otherwise;
	UINT number = otherwise;
	if (value.vt == VT_UI1 || value.vt == VT_BOOL)
		number = value.vt == VT_BOOL ? value.boolVal != 0 : value.bVal;
	else if (value.vt == VT_UI2)
		number = value.uiVal;
	else if (value.vt == VT_UI4)
		number = value.ulVal;
	PropVariantClear(&value);
	return number;
}

static IWICMetadataQueryReader *frame_query(IWICBitmapDecoder *decoder, UINT index)
{
	IWICBitmapFrameDecode *frame;
	IWICMetadataQueryReader *query = NULL;
	if (SUCCEEDED(IWICBitmapDecoder_GetFrame(decoder, index, &frame))) {
		if (FAILED(IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &query)))
			query = NULL;
		IWICBitmapFrameDecode_Release(frame);
	}
	return query;
}

// Delays cover every frame, and a frame without its own repeats the one before it.
static void load_gif(GpImage *image, IWICBitmapDecoder *decoder, IWICImagingFactory *factory)
{
	UINT frames = image->frame_count;
	DWORD *delays = gp_alloc(frames * sizeof(DWORD));
	DWORD last = 0;
	for (UINT i = 0; delays && i < frames; i++) {
		IWICMetadataQueryReader *query = frame_query(decoder, i);
		last = query_number(query, L"/grctlext/Delay", last);
		delays[i] = last;
		if (query)
			IWICMetadataQueryReader_Release(query);
	}
	if (delays)
		gp_property_set(image, TagFrameDelay, TagTypeLong, frames * sizeof(DWORD), delays);
	gp_free(delays);

	IWICMetadataQueryReader *global = NULL;
	if (FAILED(IWICBitmapDecoder_GetMetadataQueryReader(decoder, &global)))
		global = NULL;
	PROPVARIANT value;
	if (query_value(global, L"/commentext/TextEntry", &value)) {
		add_value(image, TagUserComment, &value);
		PropVariantClear(&value);
	}
	// The loop count sits in the Netscape application block: a sub-block of three bytes, of which the last two count.
	USHORT loops = 1;
	if (query_value(global, L"/appext/Data", &value)) {
		if (value.vt == (VT_UI1 | VT_VECTOR) && value.caub.cElems >= 4 && value.caub.pElems[0] == 3 && value.caub.pElems[1] == 1)
			loops = (USHORT)(value.caub.pElems[2] | value.caub.pElems[3] << 8);
		PropVariantClear(&value);
	}
	gp_property_set(image, TagLoopCount, TagTypeShort, sizeof(loops), &loops);
	if (query_number(global, L"/logscrdesc/GlobalColorTableFlag", 0)) {
		IWICPalette *palette;
		if (SUCCEEDED(IWICImagingFactory_CreatePalette(factory, &palette))) {
			UINT count = 0;
			WICColor colors[256];
			if (SUCCEEDED(IWICBitmapDecoder_CopyPalette(decoder, palette)) && SUCCEEDED(IWICPalette_GetColors(palette, 256, colors, &count))) {
				BYTE bytes[256 * 3];
				for (UINT i = 0; i < count; i++) {
					bytes[i * 3] = (BYTE)(colors[i] >> 16);
					bytes[i * 3 + 1] = (BYTE)(colors[i] >> 8);
					bytes[i * 3 + 2] = (BYTE)colors[i];
				}
				gp_property_set(image, TagGlobalPalette, TagTypeByte, count * 3, bytes);
			}
			IWICPalette_Release(palette);
		}
		BYTE background = (BYTE)query_number(global, L"/logscrdesc/BackgroundColorIndex", 0);
		gp_property_set(image, TagIndexBackground, TagTypeByte, 1, &background);
	}
	if (global)
		IWICMetadataQueryReader_Release(global);
	IWICMetadataQueryReader *first = frame_query(decoder, 0);
	if (query_number(first, L"/grctlext/TransparencyFlag", 0)) {
		BYTE index = (BYTE)query_number(first, L"/grctlext/TransparentColorIndex", 0);
		gp_property_set(image, TagIndexTransparent, TagTypeByte, 1, &index);
	}
	if (first)
		IWICMetadataQueryReader_Release(first);
}

static void add_png_text(GpImage *image, IWICMetadataQueryReader *query)
{
	static const struct { const WCHAR *key; PROPID id; } names[] = {
		{L"/tEXt/{str=Title}", TagImageTitle}, {L"/tEXt/{str=Author}", TagArtist}, {L"/tEXt/{str=Description}", TagDescription},
		{L"/tEXt/{str=Copyright}", TagCopyright}, {L"/tEXt/{str=Software}", TagSoftware}, {L"/tEXt/{str=Comment}", TagUserComment},
	};
	for (size_t i = 0; i < ARRAYSIZE(names); i++) {
		PROPVARIANT value;
		if (query_value(query, names[i].key, &value)) {
			add_value(image, names[i].id, &value);
			PropVariantClear(&value);
		}
	}
}

static void load_png(GpImage *image, IWICBitmapFrameDecode *frame)
{
	IWICMetadataQueryReader *query = NULL;
	if (FAILED(IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &query)))
		query = NULL;
	UINT year = query_number(query, L"/tIME/Year", 0);
	if (year) {
		char text[20];
		UINT parts[5] = {query_number(query, L"/tIME/Month", 1), query_number(query, L"/tIME/Day", 1), query_number(query, L"/tIME/Hour", 0),
			query_number(query, L"/tIME/Minute", 0), query_number(query, L"/tIME/Second", 0)};
		wsprintfA(text, "%04u:%02u:%02u %02u:%02u:%02u", year, parts[0], parts[1], parts[2], parts[3], parts[4]);
		gp_property_set(image, TagDateTime, TagTypeAscii, sizeof(text), text);
	}
	PROPVARIANT value;
	if (query_value(query, L"/hIST/Frequencies", &value)) {
		add_value(image, TagPaletteHistogram, &value);
		PropVariantClear(&value);
	}
	UINT gamma = query_number(query, L"/gAMA/ImageGamma", 0);
	if (gamma) {
		DWORD rational[2] = {100000, gamma};
		gp_property_set(image, TagGamma, TagTypeRational, sizeof(rational), rational);
	}
	add_png_text(image, query);
	if (query)
		IWICMetadataQueryReader_Release(query);
	BYTE unit = 1;
	gp_property_set(image, TagPixelUnit, TagTypeByte, 1, &unit);
	double dpi_x = 0.0, dpi_y = 0.0;
	IWICBitmapFrameDecode_GetResolution(frame, &dpi_x, &dpi_y);
	// A file that states no density reads back as the default of exactly 96.
	DWORD per_x = dpi_x == 96.0 ? 0 : (DWORD)(dpi_x / 0.0254 + 0.5), per_y = dpi_y == 96.0 ? 0 : (DWORD)(dpi_y / 0.0254 + 0.5);
	gp_property_set(image, TagPixelPerUnitX, TagTypeLong, sizeof(per_x), &per_x);
	gp_property_set(image, TagPixelPerUnitY, TagTypeLong, sizeof(per_y), &per_y);
}

void gp_properties_load(GpImage *image, IWICBitmapDecoder *decoder, IWICBitmapFrameDecode *frame, IWICImagingFactory *factory)
{
	gp_property_clear(image);
	GUID container;
	if (FAILED(IWICBitmapDecoder_GetContainerFormat(decoder, &container)))
		return;
	if (IsEqualGUID(&container, &GUID_ContainerFormatGif)) {
		load_gif(image, decoder, factory);
		return;
	}
	if (IsEqualGUID(&container, &GUID_ContainerFormatPng)) {
		load_png(image, frame);
		return;
	}
	IWICMetadataBlockReader *blocks;
	if (FAILED(IWICBitmapFrameDecode_QueryInterface(frame, &block_reader_iid, (void **)&blocks)))
		return;
	UINT count = 0;
	IWICMetadataBlockReader_GetCount(blocks, &count);
	for (UINT i = 0; i < count; i++) {
		IWICMetadataReader *reader;
		if (SUCCEEDED(IWICMetadataBlockReader_GetReaderByIndex(blocks, i, &reader))) {
			walk_reader(image, reader);
			IWICMetadataReader_Release(reader);
		}
	}
	IWICMetadataBlockReader_Release(blocks);
}
