/* SPDX-License-Identifier: MIT */
#include "wic.h"

/* An in-memory bitmap. Readers share a lock, a writer has it alone; the count says which: -1 for the writer. */

typedef struct
{
	IWICBitmap iface;
	LONG references, locks;
	UINT width, height, stride;
	GUID format;
	double dpi_x, dpi_y;
	BYTE *pixels;
	IWICPalette *palette;
} bitmap;

typedef struct
{
	IWICBitmapLock iface;
	LONG references;
	bitmap *owner;
	WICRect area;
	BOOL writing;
} bitmap_lock;

static bitmap *bitmap_of(IWICBitmap *iface)
{
	return WIC_CONTAINING(iface, bitmap, iface);
}

static bitmap_lock *lock_of(IWICBitmapLock *iface)
{
	return WIC_CONTAINING(iface, bitmap_lock, iface);
}

static HRESULT WINAPI lock_QueryInterface(IWICBitmapLock *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICBitmapLock)) {
		*out = iface;
		IWICBitmapLock_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI lock_AddRef(IWICBitmapLock *iface)
{
	return InterlockedIncrement(&lock_of(iface)->references);
}

static ULONG WINAPI lock_Release(IWICBitmapLock *iface)
{
	bitmap_lock *self = lock_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		if (self->writing)
			InterlockedExchange(&self->owner->locks, 0);
		else
			InterlockedDecrement(&self->owner->locks);
		IWICBitmap_Release(&self->owner->iface);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI lock_GetSize(IWICBitmapLock *iface, UINT *width, UINT *height)
{
	if (!width || !height)
		return E_INVALIDARG;
	*width = (UINT)lock_of(iface)->area.Width;
	*height = (UINT)lock_of(iface)->area.Height;
	return S_OK;
}

static HRESULT WINAPI lock_GetStride(IWICBitmapLock *iface, UINT *stride)
{
	if (!stride)
		return E_INVALIDARG;
	*stride = lock_of(iface)->owner->stride;
	return S_OK;
}

static HRESULT WINAPI lock_GetDataPointer(IWICBitmapLock *iface, UINT *size, BYTE **data)
{
	bitmap_lock *self = lock_of(iface);
	const bitmap *owner = self->owner;
	const UINT bits = wic_format_bits(&owner->format);
	if (!size || !data)
		return E_INVALIDARG;
	*size = owner->stride * (self->area.Height - 1) + (UINT)(((UINT64)self->area.Width * bits + 7) / 8);
	*data = owner->pixels + (SIZE_T)owner->stride * self->area.Y + (UINT64)self->area.X * bits / 8;
	return S_OK;
}

static HRESULT WINAPI lock_GetPixelFormat(IWICBitmapLock *iface, WICPixelFormatGUID *format)
{
	if (!format)
		return E_INVALIDARG;
	*format = lock_of(iface)->owner->format;
	return S_OK;
}

static const IWICBitmapLockVtbl lock_vtbl = {
	lock_QueryInterface, lock_AddRef, lock_Release, lock_GetSize, lock_GetStride, lock_GetDataPointer, lock_GetPixelFormat,
};

static HRESULT WINAPI bitmap_QueryInterface(IWICBitmap *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICBitmapSource) || IsEqualGUID(riid, &IID_IWICBitmap)) {
		*out = iface;
		IWICBitmap_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI bitmap_AddRef(IWICBitmap *iface)
{
	return InterlockedIncrement(&bitmap_of(iface)->references);
}

static ULONG WINAPI bitmap_Release(IWICBitmap *iface)
{
	bitmap *self = bitmap_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		if (self->palette)
			IWICPalette_Release(self->palette);
		wic_free(self->pixels);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI bitmap_GetSize(IWICBitmap *iface, UINT *width, UINT *height)
{
	if (!width || !height)
		return E_INVALIDARG;
	*width = bitmap_of(iface)->width;
	*height = bitmap_of(iface)->height;
	return S_OK;
}

static HRESULT WINAPI bitmap_GetPixelFormat(IWICBitmap *iface, WICPixelFormatGUID *format)
{
	if (!format)
		return E_INVALIDARG;
	*format = bitmap_of(iface)->format;
	return S_OK;
}

static HRESULT WINAPI bitmap_GetResolution(IWICBitmap *iface, double *x, double *y)
{
	if (!x || !y)
		return E_INVALIDARG;
	*x = bitmap_of(iface)->dpi_x;
	*y = bitmap_of(iface)->dpi_y;
	return S_OK;
}

static HRESULT WINAPI bitmap_CopyPalette(IWICBitmap *iface, IWICPalette *target)
{
	bitmap *self = bitmap_of(iface);
	if (!target)
		return E_INVALIDARG;
	if (!self->palette)
		return WINCODEC_ERR_PALETTEUNAVAILABLE;
	return IWICPalette_InitializeFromPalette(target, self->palette);
}

static HRESULT WINAPI bitmap_CopyPixels(IWICBitmap *iface, const WICRect *area, UINT stride, UINT size, BYTE *target)
{
	bitmap *self = bitmap_of(iface);
	if (!target)
		return E_INVALIDARG;
	return wic_copy_rows(area, self->width, self->height, wic_format_bits(&self->format), self->stride, self->pixels, stride, size, target);
}

static HRESULT WINAPI bitmap_Lock(IWICBitmap *iface, const WICRect *area, DWORD flags, IWICBitmapLock **out)
{
	bitmap *self = bitmap_of(iface);
	WICRect whole = {0, 0, (INT)self->width, (INT)self->height};
	bitmap_lock *lock;
	const BOOL writing = (flags & WICBitmapLockWrite) != 0;
	if (!out || !(flags & (WICBitmapLockRead | WICBitmapLockWrite)))
		return E_INVALIDARG;
	*out = NULL;
	if (!area)
		area = &whole;
	if (area->X < 0 || area->Y < 0 || area->Width <= 0 || area->Height <= 0 || (UINT)area->X + area->Width > self->width || (UINT)area->Y + area->Height > self->height)
		return E_INVALIDARG;
	if (writing) {
		if (InterlockedCompareExchange(&self->locks, -1, 0) != 0)
			return WINCODEC_ERR_ALREADYLOCKED;
	}
	else {
		LONG seen;
		do {
			seen = self->locks;
			if (seen < 0)
				return WINCODEC_ERR_ALREADYLOCKED;
		} while (InterlockedCompareExchange(&self->locks, seen + 1, seen) != seen);
	}
	if (!(lock = wic_alloc(sizeof(*lock)))) {
		if (writing)
			InterlockedExchange(&self->locks, 0);
		else
			InterlockedDecrement(&self->locks);
		return E_OUTOFMEMORY;
	}
	lock->iface.lpVtbl = &lock_vtbl;
	lock->references = 1;
	lock->owner = self;
	lock->area = *area;
	lock->writing = writing;
	IWICBitmap_AddRef(iface);
	*out = &lock->iface;
	return S_OK;
}

static HRESULT WINAPI bitmap_SetPalette(IWICBitmap *iface, IWICPalette *source)
{
	bitmap *self = bitmap_of(iface);
	IWICPalette *copy;
	HRESULT result;
	if (!source)
		return E_INVALIDARG;
	if (FAILED(result = wic_palette_create(&copy)))
		return result;
	if (FAILED(result = IWICPalette_InitializeFromPalette(copy, source))) {
		IWICPalette_Release(copy);
		return result;
	}
	if (self->palette)
		IWICPalette_Release(self->palette);
	self->palette = copy;
	return S_OK;
}

static HRESULT WINAPI bitmap_SetResolution(IWICBitmap *iface, double x, double y)
{
	bitmap_of(iface)->dpi_x = x;
	bitmap_of(iface)->dpi_y = y;
	return S_OK;
}

static const IWICBitmapVtbl bitmap_vtbl = {
	bitmap_QueryInterface, bitmap_AddRef, bitmap_Release, bitmap_GetSize, bitmap_GetPixelFormat, bitmap_GetResolution, bitmap_CopyPalette,
	bitmap_CopyPixels, bitmap_Lock, bitmap_SetPalette, bitmap_SetResolution,
};

HRESULT wic_bitmap_create(UINT width, UINT height, const GUID *format, BYTE *adopted, UINT stride, IWICBitmap **out)
{
	const UINT bits = wic_format_bits(format);
	bitmap *self;
	if (!width || !height || !out)
		return E_INVALIDARG;
	if (!bits)
		return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
	if (!(self = wic_alloc(sizeof(*self))))
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &bitmap_vtbl;
	self->references = 1;
	self->width = width;
	self->height = height;
	self->format = *format;
	self->stride = stride ? stride : wic_stride(width, bits);
	self->pixels = adopted ? adopted : wic_alloc((SIZE_T)self->stride * height);
	if (!self->pixels) {
		wic_free(self);
		return E_OUTOFMEMORY;
	}
	*out = &self->iface;
	return S_OK;
}
