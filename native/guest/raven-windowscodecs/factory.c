/* SPDX-License-Identifier: MIT */
#include "wic.h"

typedef struct
{
	IWICImagingFactory iface;
	LONG references;
} factory;

typedef struct
{
	IWICColorContext iface;
	LONG references;
	WICColorContextType type;
	UINT color_space;
	BYTE *profile;
	UINT profile_size;
} color_context;

typedef struct
{
	IEnumUnknown iface;
	LONG references;
	IUnknown **items;
	ULONG count, at;
} component_list;

static color_context *context_of(IWICColorContext *iface)
{
	return WIC_CONTAINING(iface, color_context, iface);
}

static HRESULT WINAPI context_QueryInterface(IWICColorContext *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICColorContext)) {
		*out = iface;
		IWICColorContext_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI context_AddRef(IWICColorContext *iface)
{
	return InterlockedIncrement(&context_of(iface)->references);
}

static ULONG WINAPI context_Release(IWICColorContext *iface)
{
	color_context *self = context_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		wic_free(self->profile);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI context_InitializeFromMemory(IWICColorContext *iface, const BYTE *data, UINT size)
{
	color_context *self = context_of(iface);
	if (!data || !size)
		return E_INVALIDARG;
	if (self->type == WICColorContextExifColorSpace)
		return WINCODEC_ERR_WRONGSTATE;
	wic_free(self->profile);
	if (!(self->profile = wic_alloc(size)))
		return E_OUTOFMEMORY;
	memcpy(self->profile, data, size);
	self->profile_size = size;
	self->type = WICColorContextProfile;
	return S_OK;
}

static HRESULT WINAPI context_InitializeFromFilename(IWICColorContext *iface, LPCWSTR name)
{
	HANDLE file;
	DWORD size, got = 0;
	BYTE *data;
	HRESULT result;
	if (!name)
		return E_INVALIDARG;
	file = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return HRESULT_FROM_WIN32(GetLastError());
	size = GetFileSize(file, NULL);
	if (!(data = wic_alloc(size))) {
		CloseHandle(file);
		return E_OUTOFMEMORY;
	}
	result = ReadFile(file, data, size, &got, NULL) ? context_InitializeFromMemory(iface, data, got) : HRESULT_FROM_WIN32(GetLastError());
	wic_free(data);
	CloseHandle(file);
	return result;
}

static HRESULT WINAPI context_InitializeFromExifColorSpace(IWICColorContext *iface, UINT space)
{
	color_context *self = context_of(iface);
	if (self->type == WICColorContextProfile)
		return WINCODEC_ERR_WRONGSTATE;
	self->color_space = space;
	self->type = WICColorContextExifColorSpace;
	return S_OK;
}

static HRESULT WINAPI context_GetType(IWICColorContext *iface, WICColorContextType *type)
{
	if (!type)
		return E_INVALIDARG;
	*type = context_of(iface)->type;
	return S_OK;
}

static HRESULT WINAPI context_GetProfileBytes(IWICColorContext *iface, UINT room, BYTE *out, UINT *actual)
{
	color_context *self = context_of(iface);
	if (self->type != WICColorContextProfile)
		return WINCODEC_ERR_NOTINITIALIZED;
	if (!actual)
		return E_INVALIDARG;
	*actual = self->profile_size;
	if (room && out && room >= self->profile_size)
		memcpy(out, self->profile, self->profile_size);
	else if (room)
		return WINCODEC_ERR_INSUFFICIENTBUFFER;
	return S_OK;
}

static HRESULT WINAPI context_GetExifColorSpace(IWICColorContext *iface, UINT *space)
{
	if (!space)
		return E_INVALIDARG;
	*space = context_of(iface)->color_space;
	return S_OK;
}

static const IWICColorContextVtbl context_vtbl = {
	context_QueryInterface, context_AddRef, context_Release, context_InitializeFromFilename, context_InitializeFromMemory, context_InitializeFromExifColorSpace,
	context_GetType, context_GetProfileBytes, context_GetExifColorSpace,
};

static component_list *list_of(IEnumUnknown *iface)
{
	return WIC_CONTAINING(iface, component_list, iface);
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
	return InterlockedIncrement(&list_of(iface)->references);
}

static ULONG WINAPI list_Release(IEnumUnknown *iface)
{
	component_list *self = list_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		ULONG index;
		for (index = 0; index < self->count; index++)
			IUnknown_Release(self->items[index]);
		wic_free(self->items);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI list_Next(IEnumUnknown *iface, ULONG wanted, IUnknown **items, ULONG *fetched)
{
	component_list *self = list_of(iface);
	ULONG got = 0;
	if (!items || (wanted > 1 && !fetched))
		return E_INVALIDARG;
	while (got < wanted && self->at < self->count) {
		items[got] = self->items[self->at++];
		IUnknown_AddRef(items[got]);
		got++;
	}
	if (fetched)
		*fetched = got;
	return got == wanted ? S_OK : S_FALSE;
}

static HRESULT WINAPI list_Skip(IEnumUnknown *iface, ULONG count)
{
	component_list *self = list_of(iface);
	const ULONG left = self->count - self->at;
	self->at += min(left, count);
	return count > left ? S_FALSE : S_OK;
}

static HRESULT WINAPI list_Reset(IEnumUnknown *iface)
{
	list_of(iface)->at = 0;
	return S_OK;
}

static const IEnumUnknownVtbl list_vtbl;

static HRESULT list_make(component_list **out, ULONG room)
{
	component_list *self = wic_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	if (!(self->items = wic_alloc(sizeof(*self->items) * (room + 1)))) {
		wic_free(self);
		return E_OUTOFMEMORY;
	}
	self->iface.lpVtbl = &list_vtbl;
	self->references = 1;
	*out = self;
	return S_OK;
}

static HRESULT WINAPI list_Clone(IEnumUnknown *iface, IEnumUnknown **out)
{
	component_list *self = list_of(iface), *copy;
	ULONG index;
	HRESULT result;
	if (!out)
		return E_INVALIDARG;
	if (FAILED(result = list_make(&copy, self->count)))
		return result;
	for (index = 0; index < self->count; index++) {
		copy->items[index] = self->items[index];
		IUnknown_AddRef(copy->items[index]);
	}
	copy->count = self->count;
	copy->at = self->at;
	*out = &copy->iface;
	return S_OK;
}

static const IEnumUnknownVtbl list_vtbl = {
	list_QueryInterface, list_AddRef, list_Release, list_Next, list_Skip, list_Reset, list_Clone,
};

static HRESULT WINAPI factory_QueryInterface(IWICImagingFactory *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICImagingFactory) || IsEqualGUID(riid, &IID_IWICComponentFactory)) {
		*out = iface;
		IWICImagingFactory_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef(IWICImagingFactory *iface)
{
	return InterlockedIncrement(&WIC_CONTAINING(iface, factory, iface)->references);
}

static ULONG WINAPI factory_Release(IWICImagingFactory *iface)
{
	factory *self = WIC_CONTAINING(iface, factory, iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left)
		wic_free(self);
	return left;
}

static HRESULT WINAPI factory_CreateDecoderFromStream(IWICImagingFactory *iface, IStream *stream, const GUID *vendor, WICDecodeOptions options, IWICBitmapDecoder **out)
{
	UINT index;
	const wic_codec *codec;
	if (!stream || !out)
		return E_INVALIDARG;
	*out = NULL;
	for (index = 0; (codec = wic_codec_at(index)); index++) {
		BYTE head[16];
		ULONG got = 0;
		const LARGE_INTEGER start = {{0, 0}};
		IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
		if (SUCCEEDED(IStream_Read(stream, head, sizeof(head), &got)) && wic_codec_matches(codec, head, got)) {
			IWICBitmapDecoder *decoder;
			HRESULT result = wic_decoder_create(codec, &decoder);
			if (FAILED(result))
				return result;
			result = IWICBitmapDecoder_Initialize(decoder, stream, options);
			if (SUCCEEDED(result)) {
				*out = decoder;
				return S_OK;
			}
			IWICBitmapDecoder_Release(decoder);
			return result;
		}
	}
	return WINCODEC_ERR_COMPONENTNOTFOUND;
}

static HRESULT WINAPI factory_CreateDecoderFromFilename(IWICImagingFactory *iface, LPCWSTR name, const GUID *vendor, DWORD access, WICDecodeOptions options, IWICBitmapDecoder **out)
{
	IWICStream *stream;
	HRESULT result;
	if (!name || !out)
		return E_INVALIDARG;
	if (FAILED(result = wic_stream_create(&stream)))
		return result;
	result = IWICStream_InitializeFromFilename(stream, name, access);
	if (SUCCEEDED(result))
		result = factory_CreateDecoderFromStream(iface, (IStream *)stream, vendor, options, out);
	IWICStream_Release(stream);
	return result;
}

static HRESULT WINAPI factory_CreateDecoderFromFileHandle(IWICImagingFactory *iface, ULONG_PTR handle, const GUID *vendor, WICDecodeOptions options, IWICBitmapDecoder **out)
{
	const DWORD size = GetFileSize((HANDLE)handle, NULL);
	HGLOBAL memory;
	IStream *stream = NULL;
	DWORD got = 0;
	HRESULT result;
	if (size == INVALID_FILE_SIZE)
		return HRESULT_FROM_WIN32(GetLastError());
	if (!(memory = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1)))
		return E_OUTOFMEMORY;
	SetFilePointer((HANDLE)handle, 0, NULL, FILE_BEGIN);
	if (!ReadFile((HANDLE)handle, GlobalLock(memory), size, &got, NULL)) {
		GlobalUnlock(memory);
		GlobalFree(memory);
		return HRESULT_FROM_WIN32(GetLastError());
	}
	GlobalUnlock(memory);
	if (FAILED(result = CreateStreamOnHGlobal(memory, TRUE, &stream))) {
		GlobalFree(memory);
		return result;
	}
	result = factory_CreateDecoderFromStream(iface, stream, vendor, options, out);
	IStream_Release(stream);
	return result;
}

static HRESULT WINAPI factory_CreateComponentInfo(IWICImagingFactory *iface, REFCLSID clsid, IWICComponentInfo **out)
{
	if (!clsid || !out)
		return E_INVALIDARG;
	return wic_component_info_create(clsid, out);
}

static HRESULT WINAPI factory_CreateDecoder(IWICImagingFactory *iface, REFGUID container, const GUID *vendor, IWICBitmapDecoder **out)
{
	const wic_codec *codec;
	if (!container || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (!(codec = wic_codec_by_container(container)))
		return WINCODEC_ERR_COMPONENTNOTFOUND;
	return wic_decoder_create(codec, out);
}

static HRESULT WINAPI factory_CreateEncoder(IWICImagingFactory *iface, REFGUID container, const GUID *vendor, IWICBitmapEncoder **out)
{
	const wic_codec *codec;
	if (!container || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (!(codec = wic_codec_by_container(container)))
		return WINCODEC_ERR_COMPONENTNOTFOUND;
	return wic_encoder_create(codec, out);
}

static HRESULT WINAPI factory_CreatePalette(IWICImagingFactory *iface, IWICPalette **out)
{
	return out ? wic_palette_create(out) : E_INVALIDARG;
}

static HRESULT WINAPI factory_CreateFormatConverter(IWICImagingFactory *iface, IWICFormatConverter **out)
{
	return out ? wic_converter_create(out) : E_INVALIDARG;
}

static HRESULT WINAPI factory_CreateBitmapScaler(IWICImagingFactory *iface, IWICBitmapScaler **out)
{
	return out ? wic_scaler_create(out) : E_INVALIDARG;
}

static HRESULT WINAPI factory_CreateBitmapClipper(IWICImagingFactory *iface, IWICBitmapClipper **out)
{
	return out ? wic_clipper_create(out) : E_INVALIDARG;
}

static HRESULT WINAPI factory_CreateBitmapFlipRotator(IWICImagingFactory *iface, IWICBitmapFlipRotator **out)
{
	return out ? wic_flipper_create(out) : E_INVALIDARG;
}

static HRESULT WINAPI factory_CreateStream(IWICImagingFactory *iface, IWICStream **out)
{
	return out ? wic_stream_create(out) : E_INVALIDARG;
}

static HRESULT WINAPI factory_CreateColorContext(IWICImagingFactory *iface, IWICColorContext **out)
{
	color_context *self;
	if (!out)
		return E_INVALIDARG;
	if (!(self = wic_alloc(sizeof(*self))))
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &context_vtbl;
	self->references = 1;
	self->color_space = 0xffffffff;
	*out = &self->iface;
	return S_OK;
}

static HRESULT WINAPI factory_CreateColorTransformer(IWICImagingFactory *iface, IWICColorTransform **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI factory_CreateBitmap(IWICImagingFactory *iface, UINT width, UINT height, REFWICPixelFormatGUID format, WICBitmapCreateCacheOption option, IWICBitmap **out)
{
	if (!format || !out)
		return E_INVALIDARG;
	return wic_bitmap_create(width, height, format, NULL, 0, out);
}

static HRESULT copy_source(IWICBitmapSource *source, const WICRect *area, IWICBitmap **out)
{
	IWICBitmap *created;
	IWICBitmapLock *lock;
	IWICPalette *palette;
	GUID format;
	double x, y;
	BYTE *pixels;
	UINT width, height, stride, size;
	HRESULT result;
	if (!source || !out) {
		if (out)
			*out = NULL;
		return E_INVALIDARG;
	}
	if (FAILED(result = IWICBitmapSource_GetPixelFormat(source, &format)) || FAILED(result = IWICBitmapSource_GetSize(source, &width, &height)))
		return result;
	if (area) {
		if (area->X < 0 || area->Y < 0 || area->Width <= 0 || area->Height <= 0 || (UINT)area->X >= width || (UINT)area->Y >= height)
			return E_INVALIDARG;
		width = min((UINT)area->Width, width - area->X);
		height = min((UINT)area->Height, height - area->Y);
	}
	if (FAILED(result = wic_bitmap_create(width, height, &format, NULL, 0, &created)))
		return result;
	if (SUCCEEDED(result = IWICBitmap_Lock(created, NULL, WICBitmapLockWrite, &lock))) {
		const WICRect whole = {area ? area->X : 0, area ? area->Y : 0, (INT)width, (INT)height};
		IWICBitmapLock_GetStride(lock, &stride);
		IWICBitmapLock_GetDataPointer(lock, &size, &pixels);
		result = IWICBitmapSource_CopyPixels(source, &whole, stride, size, pixels);
		IWICBitmapLock_Release(lock);
	}
	if (SUCCEEDED(result) && SUCCEEDED(IWICBitmapSource_GetResolution(source, &x, &y)))
		IWICBitmap_SetResolution(created, x, y);
	if (SUCCEEDED(result) && wic_format_bits(&format) <= 8 && SUCCEEDED(wic_palette_create(&palette))) {
		if (SUCCEEDED(IWICBitmapSource_CopyPalette(source, palette)))
			IWICBitmap_SetPalette(created, palette);
		IWICPalette_Release(palette);
	}
	if (FAILED(result)) {
		IWICBitmap_Release(created);
		return result;
	}
	*out = created;
	return S_OK;
}

static HRESULT WINAPI factory_CreateBitmapFromSource(IWICImagingFactory *iface, IWICBitmapSource *source, WICBitmapCreateCacheOption option, IWICBitmap **out)
{
	/* Nothing has to be cached of what already is a bitmap. */
	if (source && out && option == WICBitmapNoCache && SUCCEEDED(IWICBitmapSource_QueryInterface(source, &IID_IWICBitmap, (void **)out)))
		return S_OK;
	return copy_source(source, NULL, out);
}

static HRESULT WINAPI factory_CreateBitmapFromSourceRect(IWICImagingFactory *iface, IWICBitmapSource *source, UINT x, UINT y, UINT width, UINT height, IWICBitmap **out)
{
	const WICRect area = {(INT)x, (INT)y, (INT)width, (INT)height};
	return copy_source(source, &area, out);
}

static HRESULT fill_bitmap(UINT width, UINT height, const GUID *format, UINT stride, const BYTE *rows, UINT row, IWICBitmap **out)
{
	IWICBitmap *created;
	IWICBitmapLock *lock;
	HRESULT result = wic_bitmap_create(width, height, format, NULL, 0, &created);
	if (FAILED(result))
		return result;
	if (SUCCEEDED(result = IWICBitmap_Lock(created, NULL, WICBitmapLockWrite, &lock))) {
		BYTE *pixels;
		UINT own_stride, size, y;
		IWICBitmapLock_GetStride(lock, &own_stride);
		IWICBitmapLock_GetDataPointer(lock, &size, &pixels);
		for (y = 0; y < height; y++)
			memcpy(pixels + (SIZE_T)y * own_stride, rows + (SIZE_T)y * stride, row);
		IWICBitmapLock_Release(lock);
	}
	if (FAILED(result))
		IWICBitmap_Release(created);
	else
		*out = created;
	return result;
}

HRESULT wic_bitmap_from_rows(UINT width, UINT height, const GUID *format, UINT stride, UINT size, const BYTE *rows, IWICBitmap **out)
{
	UINT row, bits;
	if (!format || !stride || !size || !rows || !out || !width || !height)
		return E_INVALIDARG;
	if (!(bits = wic_format_bits(format)))
		return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
	row = (width * bits + 7) / 8;
	if (stride < row)
		return E_INVALIDARG;
	if ((UINT64)stride * height > size)
		return WINCODEC_ERR_INSUFFICIENTBUFFER;
	return fill_bitmap(width, height, format, stride, rows, row, out);
}

static HRESULT WINAPI factory_CreateBitmapFromMemory(IWICImagingFactory *iface, UINT width, UINT height, REFWICPixelFormatGUID format, UINT stride, UINT size, BYTE *rows, IWICBitmap **out)
{
	return wic_bitmap_from_rows(width, height, format, stride, size, rows, out);
}

static HRESULT WINAPI factory_CreateBitmapFromHBITMAP(IWICImagingFactory *iface, HBITMAP handle, HPALETTE colors, WICBitmapAlphaChannelOption option, IWICBitmap **out)
{
	BITMAP header;
	BYTE space[sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD)] = {0};
	BITMAPINFO *info = (BITMAPINFO *)space;
	const GUID *format;
	BYTE *rows;
	HDC context;
	UINT bits, stride;
	HRESULT result = WINCODEC_ERR_WIN32ERROR;
	if (!out)
		return E_INVALIDARG;
	if (!handle || !GetObjectW(handle, sizeof(header), &header))
		return WINCODEC_ERR_WIN32ERROR;
	bits = header.bmBitsPixel;
	switch (bits) {
	case 1:
		format = &GUID_WICPixelFormat1bppIndexed;
		break;
	case 4:
		format = &GUID_WICPixelFormat4bppIndexed;
		break;
	case 8:
		format = &GUID_WICPixelFormat8bppIndexed;
		break;
	case 16:
		format = &GUID_WICPixelFormat16bppBGR555;
		break;
	case 24:
		format = &GUID_WICPixelFormat24bppBGR;
		break;
	default:
		bits = 32;
		format = option == WICBitmapUseAlpha ? &GUID_WICPixelFormat32bppBGRA : option == WICBitmapUsePremultipliedAlpha ? &GUID_WICPixelFormat32bppPBGRA : &GUID_WICPixelFormat32bppBGR;
		break;
	}
	info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info->bmiHeader.biWidth = header.bmWidth;
	info->bmiHeader.biHeight = -header.bmHeight;
	info->bmiHeader.biPlanes = 1;
	info->bmiHeader.biBitCount = (WORD)bits;
	info->bmiHeader.biCompression = BI_RGB;
	stride = ((UINT)header.bmWidth * bits + 31) / 32 * 4;
	if (!(rows = wic_alloc((SIZE_T)stride * header.bmHeight)))
		return E_OUTOFMEMORY;
	context = CreateCompatibleDC(NULL);
	if (GetDIBits(context, handle, 0, header.bmHeight, rows, info, DIB_RGB_COLORS))
		result = fill_bitmap(header.bmWidth, header.bmHeight, format, stride, rows, ((UINT)header.bmWidth * bits + 7) / 8, out);
	if (SUCCEEDED(result) && bits <= 8) {
		WICColor table[256];
		IWICPalette *palette;
		UINT index;
		const UINT count = 1U << bits;
		for (index = 0; index < count; index++)
			table[index] = 0xff000000 | info->bmiColors[index].rgbRed << 16 | info->bmiColors[index].rgbGreen << 8 | info->bmiColors[index].rgbBlue;
		if (SUCCEEDED(wic_palette_create(&palette))) {
			IWICPalette_InitializeCustom(palette, table, count);
			IWICBitmap_SetPalette(*out, palette);
			IWICPalette_Release(palette);
		}
	}
	DeleteDC(context);
	wic_free(rows);
	return result;
}

static HRESULT WINAPI factory_CreateBitmapFromHICON(IWICImagingFactory *iface, HICON icon, IWICBitmap **out)
{
	ICONINFO parts;
	BITMAP header;
	BITMAPINFO info = {{sizeof(BITMAPINFOHEADER)}};
	BYTE *rows, *mask;
	HDC context;
	UINT width, height, at, pixels;
	BOOL any_alpha = FALSE;
	HRESULT result = WINCODEC_ERR_WIN32ERROR;
	if (!out)
		return E_INVALIDARG;
	if (!icon || !GetIconInfo(icon, &parts))
		return HRESULT_FROM_WIN32(ERROR_INVALID_CURSOR_HANDLE);
	GetObjectW(parts.hbmColor ? parts.hbmColor : parts.hbmMask, sizeof(header), &header);
	width = header.bmWidth;
	height = parts.hbmColor ? header.bmHeight : header.bmHeight / 2;
	pixels = width * height;
	info.bmiHeader.biWidth = (LONG)width;
	info.bmiHeader.biHeight = -(LONG)height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	rows = wic_alloc((SIZE_T)pixels * 4);
	mask = wic_alloc((SIZE_T)pixels * 4);
	context = CreateCompatibleDC(NULL);
	if (rows && mask && parts.hbmColor && GetDIBits(context, parts.hbmColor, 0, height, rows, &info, DIB_RGB_COLORS)
		&& GetDIBits(context, parts.hbmMask, 0, height, mask, &info, DIB_RGB_COLORS)) {
		for (at = 0; at < pixels; at++)
			any_alpha |= rows[at * 4 + 3] != 0;
		if (!any_alpha)
			for (at = 0; at < pixels; at++)
				rows[at * 4 + 3] = mask[at * 4] ? 0 : 255;
		result = fill_bitmap(width, height, &GUID_WICPixelFormat32bppBGRA, width * 4, rows, width * 4, out);
	}
	DeleteDC(context);
	wic_free(rows);
	wic_free(mask);
	if (parts.hbmColor)
		DeleteObject(parts.hbmColor);
	DeleteObject(parts.hbmMask);
	return result;
}

static HRESULT WINAPI factory_CreateComponentEnumerator(IWICImagingFactory *iface, DWORD types, DWORD options, IEnumUnknown **out)
{
	component_list *self;
	const wic_codec *codec;
	UINT index;
	HRESULT result;
	if (!out)
		return E_INVALIDARG;
	if (FAILED(result = list_make(&self, 128)))
		return result;
	for (index = 0; (codec = wic_codec_at(index)); index++) {
		if (types & WICDecoder)
			if (SUCCEEDED(wic_codec_info_create(codec, FALSE, &self->items[self->count])))
				self->count++;
		if ((types & WICEncoder) && codec->encoder_class)
			if (SUCCEEDED(wic_codec_info_create(codec, TRUE, &self->items[self->count])))
				self->count++;
	}
	if (types & WICPixelFormatConverter)
		if (SUCCEEDED(wic_component_info_create(&CLSID_WICDefaultFormatConverter, (IWICComponentInfo **)&self->items[self->count])))
			self->count++;
	if (types & WICPixelFormat) {
		const GUID *format;
		for (index = 0; (format = wic_format_at(index)) && self->count < 128; index++)
			if (SUCCEEDED(wic_component_info_create(format, (IWICComponentInfo **)&self->items[self->count])))
				self->count++;
	}
	if (types & (WICMetadataReader | WICMetadataWriter)) {
		const CLSID *handler;
		for (index = 0; (handler = wic_metadata_class_at(index)) && self->count < 128; index++) {
			const GUID *format;
			BOOL writer = FALSE;
			wic_metadata_name(handler, &format, &writer);
			if ((types & (writer ? WICMetadataWriter : WICMetadataReader)) && SUCCEEDED(wic_component_info_create(handler, (IWICComponentInfo **)&self->items[self->count])))
				self->count++;
		}
	}
	*out = &self->iface;
	return S_OK;
}

static HRESULT WINAPI factory_no_metadata_encoder(IWICImagingFactory *iface, void *from, IWICFastMetadataEncoder **out)
{
	if (out)
		*out = NULL;
	return WINCODEC_ERR_UNSUPPORTEDOPERATION;
}

static HRESULT WINAPI factory_CreateQueryWriter(IWICImagingFactory *iface, REFGUID format, const GUID *vendor, IWICMetadataQueryWriter **out)
{
	IWICMetadataWriter *writer;
	WCHAR location[64] = L"/";
	UINT length;
	HRESULT result;
	if (!format || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (FAILED(result = wic_metadata_writer_create(format, vendor, 0, &writer)))
		return result;
	if (SUCCEEDED(result = WICMapGuidToShortName(format, ARRAYSIZE(location) - 1, location + 1, &length)))
		result = wic_query_writer_over(writer, location, format, out);
	IWICMetadataWriter_Release(writer);
	return result;
}

static HRESULT WINAPI factory_CreateQueryWriterFromReader(IWICImagingFactory *iface, IWICMetadataQueryReader *reader, const GUID *vendor, IWICMetadataQueryWriter **out)
{
	return wic_query_writer_from_reader(reader, vendor, out);
}

static HRESULT WINAPI factory_CreateMetadataReader(IWICImagingFactory *iface, REFGUID format, const GUID *vendor, DWORD options, IStream *stream, IWICMetadataReader **out)
{
	return wic_metadata_reader_create(format, vendor, options, stream, out);
}

static HRESULT WINAPI factory_CreateMetadataWriter(IWICImagingFactory *iface, REFGUID format, const GUID *vendor, DWORD options, IWICMetadataWriter **out)
{
	return wic_metadata_writer_create(format, vendor, options, out);
}

static HRESULT WINAPI factory_CreateQueryReaderFromBlockReader(IWICImagingFactory *iface, IWICMetadataBlockReader *blocks, IWICMetadataQueryReader **out)
{
	return wic_query_reader_create(blocks, out);
}

static HRESULT WINAPI factory_CreateQueryWriterFromBlockWriter(IWICImagingFactory *iface, IWICMetadataBlockWriter *blocks, IWICMetadataQueryWriter **out)
{
	return wic_query_writer_create(blocks, out);
}

static HRESULT WINAPI factory_CreateMetadataReaderFromContainer(IWICImagingFactory *iface, REFGUID container, const GUID *vendor, DWORD options, IStream *stream, IWICMetadataReader **out)
{
	return wic_metadata_reader_from_container(container, vendor, options, stream, out);
}

static HRESULT WINAPI factory_CreateMetadataWriterFromReader(IWICImagingFactory *iface, IWICMetadataReader *reader, const GUID *vendor, IWICMetadataWriter **out)
{
	return wic_metadata_writer_from_reader(reader, vendor, out);
}

static HRESULT WINAPI factory_no_block_query(IWICImagingFactory *iface, void *block, void **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI factory_CreateEncoderPropertyBag(IWICImagingFactory *iface, PROPBAG2 *options, UINT count, IPropertyBag2 **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

/* The component factory only adds to the imaging factory, so one table serves both. */
static const struct
{
	IWICImagingFactoryVtbl base;
	void *added[7];
} factory_table = {{
	factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateDecoderFromFilename, factory_CreateDecoderFromStream, factory_CreateDecoderFromFileHandle,
	factory_CreateComponentInfo, factory_CreateDecoder, factory_CreateEncoder, factory_CreatePalette, factory_CreateFormatConverter, factory_CreateBitmapScaler,
	factory_CreateBitmapClipper, factory_CreateBitmapFlipRotator, factory_CreateStream, factory_CreateColorContext, factory_CreateColorTransformer, factory_CreateBitmap,
	factory_CreateBitmapFromSource, factory_CreateBitmapFromSourceRect, factory_CreateBitmapFromMemory, factory_CreateBitmapFromHBITMAP, factory_CreateBitmapFromHICON,
	factory_CreateComponentEnumerator, (void *)factory_no_metadata_encoder, (void *)factory_no_metadata_encoder, factory_CreateQueryWriter, factory_CreateQueryWriterFromReader,
}, {(void *)factory_CreateMetadataReader, (void *)factory_CreateMetadataReaderFromContainer, (void *)factory_CreateMetadataWriter, (void *)factory_CreateMetadataWriterFromReader,
	(void *)factory_CreateQueryReaderFromBlockReader, (void *)factory_CreateQueryWriterFromBlockWriter, (void *)factory_CreateEncoderPropertyBag}};

HRESULT wic_factory_create(REFIID riid, void **out)
{
	factory *self = wic_alloc(sizeof(*self));
	HRESULT result;
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &factory_table.base;
	self->references = 1;
	result = IWICImagingFactory_QueryInterface(&self->iface, riid, out);
	IWICImagingFactory_Release(&self->iface);
	return result;
}
