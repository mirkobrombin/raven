/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_WIC_H
#define RAVEN_WIC_H

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <stddef.h>
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <ocidl.h>
#include <wincodec.h>
#include <wincodecsdk.h>

#define WIC_CONTAINING(pointer, type, member) ((type *)((char *)(pointer) - offsetof(type, member)))

void *wic_alloc(SIZE_T size);
void wic_free(void *block);
UINT wic_format_bits(const GUID *format);
UINT wic_stride(UINT width, UINT bits);
/* Copies a rectangle out of packed rows, checking it the way every CopyPixels does. */
HRESULT wic_copy_rows(const WICRect *wanted, UINT width, UINT height, UINT bits, UINT source_stride, const BYTE *source,
	UINT stride, UINT size, BYTE *target);

typedef struct image_list image_list;

/* A file format as the factory and the info objects see it. */
typedef struct
{
	const GUID *container, *decoder_class, *encoder_class;
	const WCHAR *decoder_name, *encoder_name, *extensions, *mime_types;
	const BYTE *pattern;
	UINT pattern_size;
	HRESULT (*parse)(image_list *list, const BYTE *data, SIZE_T size);
} wic_codec;

const wic_codec *wic_codec_by_container(const GUID *container);
const wic_codec *wic_codec_by_class(const GUID *clsid, BOOL *is_encoder);
const wic_codec *wic_codec_at(UINT index);
BOOL wic_codec_matches(const wic_codec *codec, const BYTE *data, SIZE_T size);
HRESULT wic_decoder_create(const wic_codec *codec, IWICBitmapDecoder **out);
HRESULT wic_encoder_create(const wic_codec *codec, IWICBitmapEncoder **out);
HRESULT wic_codec_info_create(const wic_codec *codec, BOOL encoder, void *out);
const GUID *wic_format_at(UINT index);
HRESULT wic_component_info_create(REFCLSID clsid, IWICComponentInfo **out);
HRESULT wic_bitmap_from_rows(UINT width, UINT height, const GUID *format, UINT stride, UINT size, const BYTE *rows, IWICBitmap **out);
HRESULT wic_block_reader_create(const GUID *container, const BYTE *data, SIZE_T size, INT frame, REFIID riid, void **out);
HRESULT wic_query_reader_create(IWICMetadataBlockReader *blocks, IWICMetadataQueryReader **out);
HRESULT wic_query_writer_create(IWICMetadataBlockWriter *blocks, IWICMetadataQueryWriter **out);
HRESULT wic_query_writer_over(IWICMetadataWriter *writer, const WCHAR *location, const GUID *container, IWICMetadataQueryWriter **out);
HRESULT wic_metadata_writer_from_reader(IWICMetadataReader *reader, const GUID *vendor, IWICMetadataWriter **out);
HRESULT wic_metadata_reader_from_container(REFGUID container, const GUID *vendor, DWORD options, IStream *stream, IWICMetadataReader **out);
HRESULT wic_handler_info_create(REFCLSID clsid, IWICComponentInfo **out);
BOOL wic_metadata_format_writable(const GUID *format);
HRESULT wic_query_writer_from_reader(IWICMetadataQueryReader *reader, const GUID *vendor, IWICMetadataQueryWriter **out);
const WCHAR *wic_schema_of_name(const WCHAR *name);
BOOL wic_metadata_class(REFCLSID clsid, REFIID riid, void **out, HRESULT *result);
const CLSID *wic_metadata_class_at(UINT index);
const WCHAR *wic_metadata_name(REFCLSID clsid, const GUID **format, BOOL *writer);
HRESULT wic_metadata_reader_create(REFGUID format, const GUID *vendor, DWORD options, IStream *stream, IWICMetadataReader **out);
HRESULT wic_metadata_writer_create(REFGUID format, const GUID *vendor, DWORD options, IWICMetadataWriter **out);
typedef struct wic_tiff_writer wic_tiff_writer;
wic_tiff_writer *wic_tiff_begin(void);
HRESULT wic_tiff_frame(wic_tiff_writer *self, UINT width, UINT height, const GUID *format, double dpi_x, double dpi_y, const WICColor *colors, UINT color_count, UINT stride, const BYTE *pixels);
HRESULT wic_tiff_end(wic_tiff_writer *self, IStream *stream);
HRESULT wic_write_jpeg(IStream *stream, UINT width, UINT height, const GUID *format, double dpi_x, double dpi_y, UINT stride, const BYTE *pixels);
HRESULT wic_palette_create(IWICPalette **out);
HRESULT wic_bitmap_create(UINT width, UINT height, const GUID *format, BYTE *adopted, UINT stride, IWICBitmap **out);
HRESULT wic_factory_create(REFIID riid, void **out);
HRESULT wic_stream_create(IWICStream **out);
HRESULT wic_clipper_create(IWICBitmapClipper **out);
HRESULT wic_flipper_create(IWICBitmapFlipRotator **out);
HRESULT wic_scaler_create(IWICBitmapScaler **out);
HRESULT wic_converter_create(IWICFormatConverter **out);

#endif
