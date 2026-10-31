/* SPDX-License-Identifier: MIT */
#include "decode.h"

typedef struct
{
	IWICBitmapFrameDecode iface;
	LONG references;
	IWICBitmap *bitmap;
	const wic_codec *codec;
	const BYTE *profile;
	UINT profile_size;
	UINT index;
	IWICBitmapDecoder *owner;
} frame;

typedef struct
{
	IWICBitmapDecoder iface;
	LONG references;
	const wic_codec *codec;
	image_list list;
	BYTE *file;
	SIZE_T file_size;
	IStream *stream;
	BOOL ready, queried;
} decoder;

static frame *frame_of(IWICBitmapFrameDecode *iface)
{
	return WIC_CONTAINING(iface, frame, iface);
}

static decoder *decoder_of(IWICBitmapDecoder *iface)
{
	return WIC_CONTAINING(iface, decoder, iface);
}

BYTE *image_add(image_list *list, UINT width, UINT height, const GUID *format, UINT *stride)
{
	IWICBitmap **grown = wic_alloc(sizeof(*grown) * (list->count + 1));
	BYTE **profiles = wic_alloc(sizeof(*profiles) * (list->count + 1));
	UINT *sizes = wic_alloc(sizeof(*sizes) * (list->count + 1));
	IWICBitmap *bitmap;
	IWICBitmapLock *lock;
	BYTE *pixels = NULL;
	UINT size;
	if (!grown || !profiles || !sizes || FAILED(wic_bitmap_create(width, height, format, NULL, 0, &bitmap))) {
		wic_free(grown);
		wic_free(profiles);
		wic_free(sizes);
		return NULL;
	}
	if (list->count) {
		memcpy(grown, list->frames, sizeof(*grown) * list->count);
		memcpy(profiles, list->profiles, sizeof(*profiles) * list->count);
		memcpy(sizes, list->profile_sizes, sizeof(*sizes) * list->count);
	}
	wic_free(list->frames);
	wic_free(list->profiles);
	wic_free(list->profile_sizes);
	list->frames = grown;
	list->profiles = profiles;
	list->profile_sizes = sizes;
	list->frames[list->count++] = bitmap;
	IWICBitmap_SetResolution(bitmap, 96.0, 96.0);
	if (FAILED(IWICBitmap_Lock(bitmap, NULL, WICBitmapLockWrite, &lock)))
		return NULL;
	IWICBitmapLock_GetStride(lock, stride);
	IWICBitmapLock_GetDataPointer(lock, &size, &pixels);
	IWICBitmapLock_Release(lock);
	return pixels;
}

void image_set_colors(image_list *list, const WICColor *colors, UINT count)
{
	IWICPalette *palette;
	if (!list->count || FAILED(wic_palette_create(&palette)))
		return;
	IWICPalette_InitializeCustom(palette, (WICColor *)colors, count);
	IWICBitmap_SetPalette(list->frames[list->count - 1], palette);
	IWICPalette_Release(palette);
}

void image_set_profile(image_list *list, BYTE *profile, UINT size)
{
	if (list->count) {
		list->profiles[list->count - 1] = profile;
		list->profile_sizes[list->count - 1] = size;
	}
}

void image_set_resolution(image_list *list, double x, double y)
{
	if (list->count)
		IWICBitmap_SetResolution(list->frames[list->count - 1], x, y);
}

HRESULT image_list_first_as_bgra(image_list *list, IWICBitmapSource **out)
{
	if (!list->count)
		return WINCODEC_ERR_BADIMAGE;
	return WICConvertBitmapSource(&GUID_WICPixelFormat32bppBGRA, (IWICBitmapSource *)list->frames[0], out);
}

void image_list_init(image_list *list)
{
	memset(list, 0, sizeof(*list));
}

void image_list_clear(image_list *list)
{
	UINT index;
	for (index = 0; index < list->count; index++) {
		IWICBitmap_Release(list->frames[index]);
		wic_free(list->profiles[index]);
	}
	wic_free(list->frames);
	wic_free(list->profiles);
	wic_free(list->profile_sizes);
	list->frames = NULL;
	list->count = 0;
}

static HRESULT WINAPI frame_QueryInterface(IWICBitmapFrameDecode *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IWICMetadataBlockReader)) {
		frame *self = frame_of(iface);
		const decoder *owner = decoder_of(self->owner);
		if (IsEqualGUID(self->codec->container, &GUID_ContainerFormatGif) || IsEqualGUID(self->codec->container, &GUID_ContainerFormatPng)
			|| IsEqualGUID(self->codec->container, &GUID_ContainerFormatTiff) || IsEqualGUID(self->codec->container, &GUID_ContainerFormatJpeg))
			return wic_block_reader_create(self->codec->container, owner->file, owner->file_size, (INT)self->index, riid, out);
	}
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICBitmapSource) || IsEqualGUID(riid, &IID_IWICBitmapFrameDecode)) {
		*out = iface;
		IWICBitmapFrameDecode_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI frame_AddRef(IWICBitmapFrameDecode *iface)
{
	return InterlockedIncrement(&frame_of(iface)->references);
}

static ULONG WINAPI frame_Release(IWICBitmapFrameDecode *iface)
{
	frame *self = frame_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		IWICBitmap_Release(self->bitmap);
		IWICBitmapDecoder_Release(self->owner);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI frame_GetSize(IWICBitmapFrameDecode *iface, UINT *width, UINT *height)
{
	return IWICBitmap_GetSize(frame_of(iface)->bitmap, width, height);
}

static HRESULT WINAPI frame_GetPixelFormat(IWICBitmapFrameDecode *iface, WICPixelFormatGUID *format)
{
	return IWICBitmap_GetPixelFormat(frame_of(iface)->bitmap, format);
}

static HRESULT WINAPI frame_GetResolution(IWICBitmapFrameDecode *iface, double *x, double *y)
{
	return IWICBitmap_GetResolution(frame_of(iface)->bitmap, x, y);
}

static HRESULT WINAPI frame_CopyPalette(IWICBitmapFrameDecode *iface, IWICPalette *palette)
{
	return IWICBitmap_CopyPalette(frame_of(iface)->bitmap, palette);
}

static HRESULT WINAPI frame_CopyPixels(IWICBitmapFrameDecode *iface, const WICRect *area, UINT stride, UINT size, BYTE *target)
{
	return IWICBitmap_CopyPixels(frame_of(iface)->bitmap, area, stride, size, target);
}

static HRESULT WINAPI frame_GetMetadataQueryReader(IWICBitmapFrameDecode *iface, IWICMetadataQueryReader **out)
{
	IWICMetadataBlockReader *blocks;
	HRESULT result;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (FAILED(IWICBitmapFrameDecode_QueryInterface(iface, &IID_IWICMetadataBlockReader, (void **)&blocks)))
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	result = wic_query_reader_create(blocks, out);
	IWICMetadataBlockReader_Release(blocks);
	return result;
}

static HRESULT WINAPI frame_GetColorContexts(IWICBitmapFrameDecode *iface, UINT room, IWICColorContext **contexts, UINT *count)
{
	frame *self = frame_of(iface);
	if (IsEqualGUID(self->codec->container, &GUID_ContainerFormatBmp))
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	if (!count)
		return E_INVALIDARG;
	*count = self->profile ? 1 : 0;
	if (room && self->profile) {
		if (!contexts || !contexts[0])
			return E_INVALIDARG;
		return IWICColorContext_InitializeFromMemory(contexts[0], self->profile, self->profile_size);
	}
	return S_OK;
}

static HRESULT WINAPI frame_GetThumbnail(IWICBitmapFrameDecode *iface, IWICBitmapSource **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (IsEqualGUID(frame_of(iface)->codec->container, &GUID_ContainerFormatIco))
		return IWICBitmap_QueryInterface(frame_of(iface)->bitmap, &IID_IWICBitmapSource, (void **)out);
	return WINCODEC_ERR_CODECNOTHUMBNAIL;
}

static const IWICBitmapFrameDecodeVtbl frame_vtbl = {
	frame_QueryInterface, frame_AddRef, frame_Release, frame_GetSize, frame_GetPixelFormat, frame_GetResolution, frame_CopyPalette,
	frame_CopyPixels, frame_GetMetadataQueryReader, frame_GetColorContexts, frame_GetThumbnail,
};

static HRESULT WINAPI decoder_QueryInterface(IWICBitmapDecoder *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IWICMetadataBlockReader) && IsEqualGUID(decoder_of(iface)->codec->container, &GUID_ContainerFormatGif))
		return wic_block_reader_create(decoder_of(iface)->codec->container, decoder_of(iface)->file, decoder_of(iface)->file_size, -1, riid, out);
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICBitmapDecoder)) {
		*out = iface;
		IWICBitmapDecoder_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI decoder_AddRef(IWICBitmapDecoder *iface)
{
	return InterlockedIncrement(&decoder_of(iface)->references);
}

static ULONG WINAPI decoder_Release(IWICBitmapDecoder *iface)
{
	decoder *self = decoder_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		image_list_clear(&self->list);
		wic_free(self->file);
		if (self->stream)
			IStream_Release(self->stream);
		wic_free(self);
	}
	return left;
}

static HRESULT read_whole(IStream *stream, BYTE **out, SIZE_T *size)
{
	STATSTG stat;
	LARGE_INTEGER start = {{0, 0}};
	BYTE *data;
	ULONG got = 0;
	HRESULT result;
	if (FAILED(result = IStream_Stat(stream, &stat, STATFLAG_NONAME)) || FAILED(result = IStream_Seek(stream, start, STREAM_SEEK_SET, NULL)))
		return result;
	if (stat.cbSize.QuadPart > 0x7fffffff)
		return E_OUTOFMEMORY;
	if (!(data = wic_alloc((SIZE_T)stat.cbSize.QuadPart)))
		return E_OUTOFMEMORY;
	if (FAILED(result = IStream_Read(stream, data, (ULONG)stat.cbSize.QuadPart, &got))) {
		wic_free(data);
		return result;
	}
	*out = data;
	*size = got;
	return S_OK;
}

static HRESULT WINAPI decoder_QueryCapability(IWICBitmapDecoder *iface, IStream *stream, DWORD *capability)
{
	decoder *self = decoder_of(iface);
	BYTE *data;
	SIZE_T size;
	HRESULT result;
	if (!stream || !capability)
		return E_INVALIDARG;
	if (self->ready || self->queried)
		return WINCODEC_ERR_WRONGSTATE;
	*capability = 0;
	if (FAILED(result = read_whole(stream, &data, &size)))
		return result;
	if (wic_codec_matches(self->codec, data, size))
		*capability = WICBitmapDecoderCapabilityCanDecodeAllImages | (IsEqualGUID(self->codec->container, &GUID_ContainerFormatTiff)
			? WICBitmapDecoderCapabilityCanDecodeSomeImages | WICBitmapDecoderCapabilityCanEnumerateMetadata : 0);
	/* Asking a TIFF decoder also leaves it holding the file it looked at. */
	if (*capability && IsEqualGUID(self->codec->container, &GUID_ContainerFormatTiff) && SUCCEEDED(self->codec->parse(&self->list, data, size)))
		self->ready = TRUE;
	wic_free(data);
	self->queried = TRUE;
	return S_OK;
}

static HRESULT WINAPI decoder_Initialize(IWICBitmapDecoder *iface, IStream *stream, WICDecodeOptions options)
{
	decoder *self = decoder_of(iface);
	BYTE *data;
	SIZE_T size;
	HRESULT result;
	if (!stream)
		return E_INVALIDARG;
	if (self->ready || self->queried)
		return WINCODEC_ERR_WRONGSTATE;
	if (FAILED(result = read_whole(stream, &data, &size)))
		return result;
	result = self->codec->parse(&self->list, data, size);
	if (SUCCEEDED(result)) {
		wic_free(self->file);
		self->file = data;
		self->file_size = size;
	}
	else
		wic_free(data);
	if (SUCCEEDED(result)) {
		const LARGE_INTEGER start = {{0, 0}};
		IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
		IStream_AddRef(stream);
		self->stream = stream;
		self->ready = TRUE;
	}
	return result;
}

static HRESULT WINAPI decoder_GetContainerFormat(IWICBitmapDecoder *iface, GUID *format)
{
	if (!format)
		return E_INVALIDARG;
	*format = *decoder_of(iface)->codec->container;
	return S_OK;
}

static HRESULT WINAPI decoder_GetDecoderInfo(IWICBitmapDecoder *iface, IWICBitmapDecoderInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	return wic_codec_info_create(decoder_of(iface)->codec, FALSE, out);
}

static HRESULT WINAPI decoder_CopyPalette(IWICBitmapDecoder *iface, IWICPalette *palette)
{
	decoder *self = decoder_of(iface);
	if (!self->ready)
		return IsEqualGUID(self->codec->container, &GUID_ContainerFormatGif) ? WINCODEC_ERR_WRONGSTATE : WINCODEC_ERR_PALETTEUNAVAILABLE;
	if (!self->list.color_count)
		return WINCODEC_ERR_PALETTEUNAVAILABLE;
	return IWICPalette_InitializeCustom(palette, self->list.colors, self->list.color_count);
}

static HRESULT WINAPI decoder_GetMetadataQueryReader(IWICBitmapDecoder *iface, IWICMetadataQueryReader **out)
{
	IWICMetadataBlockReader *blocks;
	HRESULT result;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (FAILED(IWICBitmapDecoder_QueryInterface(iface, &IID_IWICMetadataBlockReader, (void **)&blocks)))
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	result = wic_query_reader_create(blocks, out);
	IWICMetadataBlockReader_Release(blocks);
	return result;
}

static HRESULT WINAPI decoder_GetPreview(IWICBitmapDecoder *iface, IWICBitmapSource **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	return WINCODEC_ERR_UNSUPPORTEDOPERATION;
}

static HRESULT WINAPI decoder_GetColorContexts(IWICBitmapDecoder *iface, UINT room, IWICColorContext **contexts, UINT *count)
{
	return WINCODEC_ERR_UNSUPPORTEDOPERATION;
}

static HRESULT WINAPI decoder_GetThumbnail(IWICBitmapDecoder *iface, IWICBitmapSource **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	return WINCODEC_ERR_CODECNOTHUMBNAIL;
}

static HRESULT WINAPI decoder_GetFrameCount(IWICBitmapDecoder *iface, UINT *count)
{
	decoder *self = decoder_of(iface);
	if (!count)
		return E_INVALIDARG;
	/* The TIFF decoder alone answers before it has a file: nothing there yet. */
	if (!self->ready && !IsEqualGUID(self->codec->container, &GUID_ContainerFormatTiff))
		return WINCODEC_ERR_NOTINITIALIZED;
	*count = self->ready ? self->list.count : 0;
	return S_OK;
}

static HRESULT WINAPI decoder_GetFrame(IWICBitmapDecoder *iface, UINT index, IWICBitmapFrameDecode **out)
{
	decoder *self = decoder_of(iface);
	frame *created;
	if (!out)
		return E_INVALIDARG;
	if (!self->ready && !IsEqualGUID(self->codec->container, &GUID_ContainerFormatTiff))
		return WINCODEC_ERR_NOTINITIALIZED;
	if (!self->ready || index >= self->list.count)
		return WINCODEC_ERR_FRAMEMISSING;
	if (!(created = wic_alloc(sizeof(*created))))
		return E_OUTOFMEMORY;
	created->iface.lpVtbl = &frame_vtbl;
	created->references = 1;
	created->bitmap = self->list.frames[index];
	IWICBitmap_AddRef(created->bitmap);
	created->codec = self->codec;
	created->profile = self->list.profiles[index];
	created->profile_size = self->list.profile_sizes[index];
	created->index = index;
	created->owner = iface;
	IWICBitmapDecoder_AddRef(iface);
	*out = &created->iface;
	return S_OK;
}

static const IWICBitmapDecoderVtbl decoder_vtbl = {
	decoder_QueryInterface, decoder_AddRef, decoder_Release, decoder_QueryCapability, decoder_Initialize, decoder_GetContainerFormat,
	decoder_GetDecoderInfo, decoder_CopyPalette, decoder_GetMetadataQueryReader, decoder_GetPreview, decoder_GetColorContexts,
	decoder_GetThumbnail, decoder_GetFrameCount, decoder_GetFrame,
};

HRESULT wic_decoder_create(const wic_codec *codec, IWICBitmapDecoder **out)
{
	decoder *self = wic_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &decoder_vtbl;
	self->references = 1;
	self->codec = codec;
	*out = &self->iface;
	return S_OK;
}
