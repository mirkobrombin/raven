/* SPDX-License-Identifier: MIT */
#include "wic.h"

/* Clipper, flip and rotate, and scaler share one shape: a source and a Width by Height view of it, answering in the source's format. */

enum view_kind { VIEW_CLIP, VIEW_FLIP, VIEW_SCALE };

typedef struct
{
	union {
		IWICBitmapClipper clipper;
		IWICBitmapFlipRotator flipper;
		IWICBitmapScaler scaler;
		IWICBitmapSource source;
	} iface;
	LONG references;
	enum view_kind kind;
	IWICBitmapSource *inner;
	UINT width, height, inner_width, inner_height, bits;
	WICRect area;
	WICBitmapTransformOptions options;
	GUID format;
} view;

static view *view_of(IWICBitmapSource *iface)
{
	return WIC_CONTAINING(iface, view, iface);
}

static HRESULT WINAPI view_QueryInterface(IWICBitmapSource *iface, REFIID riid, void **out)
{
	view *self = view_of(iface);
	const IID *own = self->kind == VIEW_CLIP ? &IID_IWICBitmapClipper : self->kind == VIEW_FLIP ? &IID_IWICBitmapFlipRotator : &IID_IWICBitmapScaler;
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICBitmapSource) || IsEqualGUID(riid, own)) {
		*out = iface;
		IWICBitmapSource_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI view_AddRef(IWICBitmapSource *iface)
{
	return InterlockedIncrement(&view_of(iface)->references);
}

static ULONG WINAPI view_Release(IWICBitmapSource *iface)
{
	view *self = view_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		if (self->inner)
			IWICBitmapSource_Release(self->inner);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI view_GetSize(IWICBitmapSource *iface, UINT *width, UINT *height)
{
	view *self = view_of(iface);
	if (!width || !height)
		return E_INVALIDARG;
	if (!self->inner)
		return WINCODEC_ERR_NOTINITIALIZED;
	*width = self->width;
	*height = self->height;
	return S_OK;
}

static HRESULT WINAPI view_GetPixelFormat(IWICBitmapSource *iface, WICPixelFormatGUID *format)
{
	view *self = view_of(iface);
	if (!format)
		return E_INVALIDARG;
	if (!self->inner) {
		*format = GUID_WICPixelFormatDontCare;
		return WINCODEC_ERR_NOTINITIALIZED;
	}
	return IWICBitmapSource_GetPixelFormat(self->inner, format);
}

static HRESULT WINAPI view_GetResolution(IWICBitmapSource *iface, double *x, double *y)
{
	view *self = view_of(iface);
	if (!x || !y)
		return E_INVALIDARG;
	if (!self->inner)
		return WINCODEC_ERR_NOTINITIALIZED;
	return IWICBitmapSource_GetResolution(self->inner, x, y);
}

static HRESULT WINAPI view_CopyPalette(IWICBitmapSource *iface, IWICPalette *palette)
{
	view *self = view_of(iface);
	if (!palette)
		return E_INVALIDARG;
	if (!self->inner)
		return WINCODEC_ERR_NOTINITIALIZED;
	return IWICBitmapSource_CopyPalette(self->inner, palette);
}

/* Flip and scale read the whole source and pick pixels from it, whole bytes at a time; below a byte a pixel is read with a mask. */
static void copy_pixel(const BYTE *from_row, UINT from_x, BYTE *to_row, UINT to_x, UINT bits)
{
	if (bits >= 8)
		memcpy(to_row + (SIZE_T)to_x * (bits / 8), from_row + (SIZE_T)from_x * (bits / 8), bits / 8);
	else {
		const UINT from_bit = from_x * bits, to_bit = to_x * bits;
		const BYTE mask = (BYTE)((1 << bits) - 1);
		const BYTE value = (from_row[from_bit / 8] >> (8 - bits - from_bit % 8)) & mask;
		const UINT shift = 8 - bits - to_bit % 8;
		to_row[to_bit / 8] = (BYTE)((to_row[to_bit / 8] & ~(mask << shift)) | value << shift);
	}
}

static HRESULT WINAPI view_CopyPixels(IWICBitmapSource *iface, const WICRect *wanted, UINT stride, UINT size, BYTE *target)
{
	view *self = view_of(iface);
	WICRect whole;
	UINT row_bytes, x, y;
	BYTE *full, *picked;
	UINT inner_stride, picked_stride;
	HRESULT result;
	if (!self->inner)
		return WINCODEC_ERR_NOTINITIALIZED;
	if (!target)
		return E_INVALIDARG;
	whole.X = whole.Y = 0;
	whole.Width = (INT)self->width;
	whole.Height = (INT)self->height;
	if (!wanted)
		wanted = &whole;
	if (wanted->X < 0 || wanted->Y < 0 || wanted->Width < 0 || wanted->Height < 0 || (UINT)wanted->X + wanted->Width > self->width || (UINT)wanted->Y + wanted->Height > self->height)
		return E_INVALIDARG;
	if (self->kind == VIEW_CLIP) {
		WICRect shifted = *wanted;
		shifted.X += self->area.X;
		shifted.Y += self->area.Y;
		return IWICBitmapSource_CopyPixels(self->inner, &shifted, stride, size, target);
	}
	row_bytes = (UINT)(((UINT64)wanted->Width * self->bits + 7) / 8);
	if (stride < row_bytes)
		return E_INVALIDARG;
	if (!wanted->Width || !wanted->Height)
		return S_OK;
	if ((UINT64)stride * (wanted->Height - 1) + row_bytes > size)
		return WINCODEC_ERR_INSUFFICIENTBUFFER;
	inner_stride = wic_stride(self->inner_width, self->bits);
	picked_stride = wic_stride(self->width, self->bits);
	full = wic_alloc((SIZE_T)inner_stride * self->inner_height);
	picked = wic_alloc((SIZE_T)picked_stride * self->height);
	if (!full || !picked) {
		wic_free(full);
		wic_free(picked);
		return E_OUTOFMEMORY;
	}
	result = IWICBitmapSource_CopyPixels(self->inner, NULL, inner_stride, inner_stride * self->inner_height, full);
	if (SUCCEEDED(result)) {
		for (y = 0; y < self->height; y++)
			for (x = 0; x < self->width; x++) {
				UINT from_x, from_y;
				if (self->kind == VIEW_SCALE) {
					/* The pixel under the centre of each target pixel. */
					from_x = (UINT)(((UINT64)x * 2 + 1) * self->inner_width / (self->width * 2ULL));
					from_y = (UINT)(((UINT64)y * 2 + 1) * self->inner_height / (self->height * 2ULL));
				}
				else {
					const WICBitmapTransformOptions turn = self->options & 3;
					UINT u = x, v = y;
					if (turn == WICBitmapTransformRotate90) {
						u = y;
						v = self->inner_height - 1 - x;
					}
					else if (turn == WICBitmapTransformRotate180) {
						u = self->inner_width - 1 - x;
						v = self->inner_height - 1 - y;
					}
					else if (turn == WICBitmapTransformRotate270) {
						u = self->inner_width - 1 - y;
						v = x;
					}
					if (self->options & WICBitmapTransformFlipHorizontal)
						u = (turn == WICBitmapTransformRotate90 || turn == WICBitmapTransformRotate270) ? self->inner_width - 1 - u : self->inner_width - 1 - u;
					if (self->options & WICBitmapTransformFlipVertical)
						v = self->inner_height - 1 - v;
					from_x = u;
					from_y = v;
				}
				copy_pixel(full + (SIZE_T)from_y * inner_stride, from_x, picked + (SIZE_T)y * picked_stride, x, self->bits);
			}
		result = wic_copy_rows(wanted, self->width, self->height, self->bits, picked_stride, picked, stride, size, target);
	}
	wic_free(full);
	wic_free(picked);
	return result;
}

static HRESULT take_source(view *self, IWICBitmapSource *source)
{
	HRESULT result;
	GUID format;
	if (!source)
		return E_INVALIDARG;
	if (self->inner)
		return WINCODEC_ERR_WRONGSTATE;
	if (FAILED(result = IWICBitmapSource_GetSize(source, &self->inner_width, &self->inner_height)) || FAILED(result = IWICBitmapSource_GetPixelFormat(source, &format)))
		return result;
	if (!(self->bits = wic_format_bits(&format)))
		return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
	IWICBitmapSource_AddRef(source);
	self->inner = source;
	return S_OK;
}

static HRESULT WINAPI clipper_Initialize(IWICBitmapClipper *iface, IWICBitmapSource *source, const WICRect *area)
{
	view *self = view_of((IWICBitmapSource *)iface);
	UINT width, height;
	HRESULT result;
	if (!source || !area)
		return E_INVALIDARG;
	if (FAILED(result = IWICBitmapSource_GetSize(source, &width, &height)))
		return result;
	if (area->X < 0 || area->Y < 0 || area->Width <= 0 || area->Height <= 0 || (UINT)area->X + area->Width > width || (UINT)area->Y + area->Height > height)
		return E_INVALIDARG;
	if (FAILED(result = take_source(self, source)))
		return result;
	self->area = *area;
	self->width = (UINT)area->Width;
	self->height = (UINT)area->Height;
	return S_OK;
}

static HRESULT WINAPI flipper_Initialize(IWICBitmapFlipRotator *iface, IWICBitmapSource *source, WICBitmapTransformOptions options)
{
	view *self = view_of((IWICBitmapSource *)iface);
	HRESULT result;
	const WICBitmapTransformOptions turn = options & 3;
	if (FAILED(result = take_source(self, source)))
		return result;
	self->options = options;
	self->width = turn == WICBitmapTransformRotate90 || turn == WICBitmapTransformRotate270 ? self->inner_height : self->inner_width;
	self->height = turn == WICBitmapTransformRotate90 || turn == WICBitmapTransformRotate270 ? self->inner_width : self->inner_height;
	return S_OK;
}

static HRESULT WINAPI scaler_Initialize(IWICBitmapScaler *iface, IWICBitmapSource *source, UINT width, UINT height, WICBitmapInterpolationMode mode)
{
	view *self = view_of((IWICBitmapSource *)iface);
	HRESULT result;
	if (!width || !height)
		return E_INVALIDARG;
	if (FAILED(result = take_source(self, source)))
		return result;
	self->width = width;
	self->height = height;
	return S_OK;
}

#define SHARED(type) \
	(HRESULT (WINAPI *)(type *, REFIID, void **))view_QueryInterface, (ULONG (WINAPI *)(type *))view_AddRef, (ULONG (WINAPI *)(type *))view_Release, \
	(HRESULT (WINAPI *)(type *, UINT *, UINT *))view_GetSize, (HRESULT (WINAPI *)(type *, WICPixelFormatGUID *))view_GetPixelFormat, \
	(HRESULT (WINAPI *)(type *, double *, double *))view_GetResolution, (HRESULT (WINAPI *)(type *, IWICPalette *))view_CopyPalette, \
	(HRESULT (WINAPI *)(type *, const WICRect *, UINT, UINT, BYTE *))view_CopyPixels

static const IWICBitmapClipperVtbl clipper_vtbl = {SHARED(IWICBitmapClipper), clipper_Initialize};
static const IWICBitmapFlipRotatorVtbl flipper_vtbl = {SHARED(IWICBitmapFlipRotator), flipper_Initialize};
static const IWICBitmapScalerVtbl scaler_vtbl = {SHARED(IWICBitmapScaler), scaler_Initialize};

static view *view_new(enum view_kind kind, const void *vtbl)
{
	view *self = wic_alloc(sizeof(*self));
	if (self) {
		self->iface.source.lpVtbl = vtbl;
		self->references = 1;
		self->kind = kind;
	}
	return self;
}

HRESULT wic_clipper_create(IWICBitmapClipper **out)
{
	view *self = view_new(VIEW_CLIP, &clipper_vtbl);
	if (!self)
		return E_OUTOFMEMORY;
	*out = &self->iface.clipper;
	return S_OK;
}

HRESULT wic_flipper_create(IWICBitmapFlipRotator **out)
{
	view *self = view_new(VIEW_FLIP, &flipper_vtbl);
	if (!self)
		return E_OUTOFMEMORY;
	*out = &self->iface.flipper;
	return S_OK;
}

HRESULT wic_scaler_create(IWICBitmapScaler **out)
{
	view *self = view_new(VIEW_SCALE, &scaler_vtbl);
	if (!self)
		return E_OUTOFMEMORY;
	*out = &self->iface.scaler;
	return S_OK;
}
