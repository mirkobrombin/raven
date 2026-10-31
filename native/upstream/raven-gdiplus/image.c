/* SPDX-License-Identifier: MIT */
#include "image.h"
#include "gmath.h"
#include <wincodec.h>
#include <wincodecsdk.h>

#define FORMAT_GUID(name, a) static const GUID name = {0xb96b3c00 | (a), 0x0728, 0x11d3, {0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}}
FORMAT_GUID(format_memory, 0xaa);
FORMAT_GUID(format_bmp, 0xab);
FORMAT_GUID(format_jpeg, 0xae);
FORMAT_GUID(format_png, 0xaf);
FORMAT_GUID(format_gif, 0xb0);
FORMAT_GUID(format_tiff, 0xb1);
FORMAT_GUID(format_icon, 0xb5);
static const GUID dimension_page = {0x7462dc86, 0x6180, 0x4c7e, {0x8e, 0x3f, 0xee, 0x73, 0x33, 0xa7, 0xa4, 0x83}};
static const GUID dimension_time = {0x6aedbd6d, 0x3fb5, 0x418a, {0x83, 0xa6, 0x7f, 0x45, 0x22, 0x9d, 0xc8, 0x72}};

#define CODEC_CLSID(last) {0x557cf400 | (last), 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}}

static REAL screen_dpi_x(void)
{
	HDC dc = GetDC(NULL);
	INT dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
	if (dc)
		ReleaseDC(NULL, dc);
	return dpi > 0 ? (REAL)dpi : 96.0f;
}

static INT stride_of(INT width, PixelFormat format)
{
	return (INT)((((UINT64)width * FORMAT_BITS(format) + 31) / 32) * 4);
}

static BOOL is_valid_format(PixelFormat format)
{
	switch (format) {
	case PixelFormat1bppIndexed: case PixelFormat4bppIndexed: case PixelFormat8bppIndexed: case PixelFormat16bppGrayScale:
	case PixelFormat16bppRGB555: case PixelFormat16bppRGB565: case PixelFormat16bppARGB1555: case PixelFormat24bppRGB:
	case PixelFormat32bppRGB: case PixelFormat32bppARGB: case PixelFormat32bppPARGB: case PixelFormat48bppRGB:
	case PixelFormat64bppARGB: case PixelFormat64bppPARGB: case PixelFormat32bppCMYK:
		return TRUE;
	default:
		return FALSE;
	}
}

// Indexed bitmaps start with the system's halftone-like default: black and white for 1 bit, a colour ramp otherwise.
static ColorPalette *default_palette(PixelFormat format)
{
	if (!FORMAT_INDEXED(format))
		return NULL;
	UINT count = 1u << FORMAT_BITS(format);
	ColorPalette *palette = gp_alloc(sizeof(ColorPalette) + count * sizeof(ARGB));
	if (!palette)
		return NULL;
	palette->Count = count;
	if (format == PixelFormat1bppIndexed) {
		palette->Flags = 2;
		palette->Entries[0] = 0xff000000;
		palette->Entries[1] = 0xffffffff;
	} else if (format == PixelFormat4bppIndexed) {
		static const ARGB colors[16] = {0xff000000, 0xff800000, 0xff008000, 0xff808000, 0xff000080, 0xff800080, 0xff008080, 0xff808080,
			0xffc0c0c0, 0xffff0000, 0xff00ff00, 0xffffff00, 0xff0000ff, 0xffff00ff, 0xff00ffff, 0xffffffff};
		palette->Flags = 0;
		memcpy(palette->Entries, colors, sizeof(colors));
	} else {
		static const ARGB colors[16] = {0xff000000, 0xff800000, 0xff008000, 0xff808000, 0xff000080, 0xff800080, 0xff008080, 0xff808080,
			0xffc0c0c0, 0xffff0000, 0xff00ff00, 0xffffff00, 0xff0000ff, 0xffff00ff, 0xff00ffff, 0xffffffff};
		static const BYTE steps[6] = {0x00, 0x33, 0x66, 0x99, 0xcc, 0xff};
		palette->Flags = 4;
		memcpy(palette->Entries, colors, sizeof(colors));
		UINT at = 40;
		for (int b = 0; b < 6; b++)
			for (int g = 0; g < 6; g++)
				for (int r = 0; r < 6; r++)
					palette->Entries[at++] = 0xff000000 | steps[b] << 16 | steps[g] << 8 | steps[r];
	}
	return palette;
}

GpStatus gp_bitmap_new(INT width, INT height, PixelFormat format, GpImage **out)
{
	return GdipCreateBitmapFromScan0(width, height, 0, format, NULL, out);
}

GpStatus GDIPAPI GdipCreateBitmapFromScan0(INT width, INT height, INT stride, PixelFormat format, BYTE *bits, GpImage **out)
{
	if (!out || (bits && !stride))
		return InvalidParameter;
	*out = NULL;
	if (width <= 0 || height <= 0 || (bits && (stride % 4)))
		return InvalidParameter;
	if (!is_valid_format(format))
		return InvalidParameter;
	if (stride_of(width, format) <= 0 || (UINT64)width * height > 0x7fffffff / 2)
		return bits ? InvalidParameter : OutOfMemory;
	GpImage *image = gp_alloc(sizeof(GpImage));
	if (!image)
		return OutOfMemory;
	image->type = ImageTypeBitmap;
	image->refs = 1;
	image->width = width;
	image->height = height;
	image->format = format;
	image->raw_format = format_memory;
	image->dpi_x = image->dpi_y = screen_dpi_x();
	image->flags = FORMAT_ALPHA(format) || FORMAT_INDEXED(format) ? 2 : 0;
	if (bits) {
		image->bits = bits;
		image->stride = stride;
	} else {
		image->stride = stride_of(width, format);
		SIZE_T bytes = (SIZE_T)image->stride * height;
		if (!(image->owned = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) {
			gp_free(image);
			return OutOfMemory;
		}
		image->bits = image->owned;
	}
	if (FORMAT_INDEXED(format) && !(image->palette = default_palette(format))) {
		GdipDisposeImage(image);
		return OutOfMemory;
	}
	*out = image;
	return Ok;
}

// A save left open for more frames holds its encoder and stream until it is flushed.
static void end_saving(GpImage *image)
{
	if (image->saving)
		IWICBitmapEncoder_Release((IWICBitmapEncoder *)image->saving);
	if (image->saving_stream)
		IStream_Release(image->saving_stream);
	image->saving = NULL;
	image->saving_stream = NULL;
}

void gp_image_release(GpImage *image)
{
	if (InterlockedDecrement(&image->refs))
		return;
	if (image->owned)
		VirtualFree(image->owned, 0, MEM_RELEASE);
	gp_free(image->palette);
	gp_free(image->lock_copy);
	gp_property_clear(image);
	end_saving(image);
	if (image->decoder)
		IWICBitmapDecoder_Release((IWICBitmapDecoder *)image->decoder);
	if (image->stream)
		IStream_Release(image->stream);
	if (image->emf)
		DeleteEnhMetaFile(image->emf);
	gp_free(image);
}

GpImage *gp_image_share(GpImage *image)
{
	InterlockedIncrement(&image->refs);
	return image;
}

GpStatus GDIPAPI GdipDisposeImage(GpImage *image)
{
	if (!image)
		return InvalidParameter;
	if (image->type == ImageTypeUnknown)
		return ObjectBusy;
	gp_image_release(image);
	return Ok;
}

static inline ARGB unpremultiply(ARGB c)
{
	UINT a = c >> 24;
	// A pixel with no alpha keeps the colour it was stored with.
	if (a == 255 || !a)
		return c;
	UINT factor = 0xff00 / a;
	UINT r = (((c >> 16) & 255) * factor) >> 8, g = (((c >> 8) & 255) * factor) >> 8, b = ((c & 255) * factor) >> 8;
	return a << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

static inline ARGB premultiply(ARGB c)
{
	UINT a = c >> 24;
	if (a == 255)
		return c;
	UINT r = (((c >> 16) & 255) * a + 127) / 255, g = (((c >> 8) & 255) * a + 127) / 255, b = ((c & 255) * a + 127) / 255;
	return a << 24 | r << 16 | g << 8 | b;
}

ARGB gp_pixel_read(const GpImage *image, INT x, INT y)
{
	const BYTE *row = image->bits + (SSIZE_T)y * image->stride;
	const BYTE *p;
	switch (image->format) {
	case PixelFormat1bppIndexed:
		return image->palette->Entries[(row[x / 8] >> (7 - x % 8)) & 1];
	case PixelFormat4bppIndexed:
		return image->palette->Entries[(x & 1) ? row[x / 2] & 15 : row[x / 2] >> 4];
	case PixelFormat8bppIndexed:
		return image->palette->Entries[row[x]];
	case PixelFormat16bppGrayScale: {
		BYTE v = row[x * 2 + 1];
		return 0xff000000 | v << 16 | v << 8 | v;
	}
	case PixelFormat16bppRGB555: {
		UINT v = row[x * 2] | row[x * 2 + 1] << 8, r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
		return 0xff000000 | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
	}
	case PixelFormat16bppRGB565: {
		UINT v = row[x * 2] | row[x * 2 + 1] << 8, r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
		return 0xff000000 | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
	}
	case PixelFormat16bppARGB1555: {
		UINT v = row[x * 2] | row[x * 2 + 1] << 8, r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
		return ((v & 0x8000) ? 0xff000000 : 0) | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
	}
	case PixelFormat24bppRGB:
		p = row + x * 3;
		return 0xff000000 | p[2] << 16 | p[1] << 8 | p[0];
	case PixelFormat32bppRGB:
		return 0xff000000 | *(const DWORD *)(row + x * 4);
	case PixelFormat32bppARGB:
		return *(const DWORD *)(row + x * 4);
	case PixelFormat32bppPARGB:
		return unpremultiply(*(const DWORD *)(row + x * 4));
	case PixelFormat48bppRGB:
		p = row + x * 6;
		return 0xff000000 | p[5] << 16 | p[3] << 8 | p[1];
	case PixelFormat64bppARGB:
		p = row + x * 8;
		return (ARGB)p[7] << 24 | p[5] << 16 | p[3] << 8 | p[1];
	case PixelFormat64bppPARGB:
		p = row + x * 8;
		return unpremultiply((ARGB)p[7] << 24 | p[5] << 16 | p[3] << 8 | p[1]);
	default:
		return 0;
	}
}

static BYTE nearest_index(const ColorPalette *palette, ARGB color)
{
	UINT best = 0, best_distance = 0xffffffff;
	for (UINT i = 0; i < palette->Count; i++) {
		ARGB e = palette->Entries[i];
		INT da = (INT)(e >> 24) - (INT)(color >> 24), dr = (INT)((e >> 16) & 255) - (INT)((color >> 16) & 255);
		INT dg = (INT)((e >> 8) & 255) - (INT)((color >> 8) & 255), db = (INT)(e & 255) - (INT)(color & 255);
		UINT distance = da * da + dr * dr + dg * dg + db * db;
		if (distance < best_distance) {
			best_distance = distance;
			best = i;
		}
	}
	return (BYTE)best;
}

void gp_pixel_write(GpImage *image, INT x, INT y, ARGB c)
{
	BYTE *row = image->bits + (SSIZE_T)y * image->stride;
	UINT a = c >> 24, r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
	BYTE *p;
	switch (image->format) {
	case PixelFormat1bppIndexed: {
		BYTE mask = 0x80 >> (x % 8);
		if (nearest_index(image->palette, c))
			row[x / 8] |= mask;
		else
			row[x / 8] &= ~mask;
		break;
	}
	case PixelFormat4bppIndexed: {
		BYTE index = nearest_index(image->palette, c);
		row[x / 2] = (x & 1) ? (row[x / 2] & 0xf0) | index : (row[x / 2] & 0x0f) | index << 4;
		break;
	}
	case PixelFormat8bppIndexed:
		row[x] = nearest_index(image->palette, c);
		break;
	case PixelFormat16bppGrayScale: {
		UINT v = (r * 299 + g * 587 + b * 114) / 1000;
		row[x * 2] = (BYTE)v;
		row[x * 2 + 1] = (BYTE)v;
		break;
	}
	case PixelFormat16bppRGB555:
		*(WORD *)(row + x * 2) = (WORD)((r >> 3) << 10 | (g >> 3) << 5 | b >> 3);
		break;
	case PixelFormat16bppRGB565:
		*(WORD *)(row + x * 2) = (WORD)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
		break;
	case PixelFormat16bppARGB1555:
		*(WORD *)(row + x * 2) = (WORD)((a >= 128 ? 0x8000 : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3);
		break;
	case PixelFormat24bppRGB:
		p = row + x * 3;
		p[0] = (BYTE)b;
		p[1] = (BYTE)g;
		p[2] = (BYTE)r;
		break;
	case PixelFormat32bppRGB:
		*(DWORD *)(row + x * 4) = 0xff000000 | c;
		break;
	case PixelFormat32bppARGB:
		*(DWORD *)(row + x * 4) = c;
		break;
	case PixelFormat32bppPARGB:
		*(DWORD *)(row + x * 4) = premultiply(c);
		break;
	case PixelFormat48bppRGB:
		p = row + x * 6;
		p[0] = p[1] = (BYTE)b;
		p[2] = p[3] = (BYTE)g;
		p[4] = p[5] = (BYTE)r;
		break;
	case PixelFormat64bppARGB:
	case PixelFormat64bppPARGB:
		if (image->format == PixelFormat64bppPARGB)
			c = premultiply(c);
		p = row + x * 8;
		p[0] = p[1] = (BYTE)c;
		p[2] = p[3] = (BYTE)(c >> 8);
		p[4] = p[5] = (BYTE)(c >> 16);
		p[6] = p[7] = (BYTE)(c >> 24);
		break;
	default:
		break;
	}
}

GpStatus GDIPAPI GdipBitmapGetPixel(GpImage *image, INT x, INT y, ARGB *out)
{
	if (!image || !out || x < 0 || y < 0 || (UINT)x >= image->width || (UINT)y >= image->height || image->type != ImageTypeBitmap)
		return InvalidParameter;
	if (image->locked)
		return ObjectBusy;
	*out = gp_pixel_read(image, x, y);
	return Ok;
}

GpStatus GDIPAPI GdipBitmapSetPixel(GpImage *image, INT x, INT y, ARGB color)
{
	if (!image || x < 0 || y < 0 || (UINT)x >= image->width || (UINT)y >= image->height || image->type != ImageTypeBitmap)
		return InvalidParameter;
	if (image->locked)
		return ObjectBusy;
	gp_pixel_write(image, x, y, color);
	return Ok;
}

static void copy_area(GpImage *to, INT to_x, INT to_y, const GpImage *from, INT from_x, INT from_y, INT width, INT height)
{
	if (to->format == from->format && !FORMAT_INDEXED(from->format)) {
		INT bytes = FORMAT_BITS(from->format) / 8;
		for (INT y = 0; y < height; y++)
			memcpy(to->bits + (SSIZE_T)(to_y + y) * to->stride + to_x * bytes, from->bits + (SSIZE_T)(from_y + y) * from->stride + from_x * bytes, width * bytes);
		return;
	}
	for (INT y = 0; y < height; y++)
		for (INT x = 0; x < width; x++)
			gp_pixel_write(to, to_x + x, to_y + y, gp_pixel_read(from, from_x + x, from_y + y));
}

GpStatus GDIPAPI GdipBitmapLockBits(GpImage *image, const GpRect *rect, UINT mode, PixelFormat format, BitmapData *data)
{
	if (!image || !data || image->type != ImageTypeBitmap)
		return InvalidParameter;
	GpRect area = rect ? *rect : (GpRect){0, 0, (INT)image->width, (INT)image->height};
	if (area.X < 0 || area.Y < 0 || area.Width <= 0 || area.Height <= 0 || (UINT)(area.X + area.Width) > image->width || (UINT)(area.Y + area.Height) > image->height)
		return InvalidParameter;
	if (image->locked)
		return WrongState;
	if (!is_valid_format(format))
		return format ? InvalidParameter : InvalidParameter;
	BOOL direct = format == image->format && !(mode & ImageLockModeUserInputBuf)
		&& (area.X * FORMAT_BITS(format)) % 8 == 0;
	image->lock_rect = area;
	image->lock_mode = mode;
	image->locked = 1;
	data->Width = area.Width;
	data->Height = area.Height;
	data->Format = format;
	data->Reserved = 0;
	if (direct) {
		data->Stride = image->stride;
		data->Scan0 = image->bits + (SSIZE_T)area.Y * image->stride + area.X * FORMAT_BITS(format) / 8;
		return Ok;
	}
	BYTE *buffer = data->Scan0;
	INT stride = data->Stride;
	if (!(mode & ImageLockModeUserInputBuf)) {
		stride = stride_of(area.Width, format);
		buffer = gp_alloc((SIZE_T)stride * area.Height);
		if (!buffer) {
			image->locked = 0;
			return OutOfMemory;
		}
		image->lock_copy = buffer;
		data->Stride = stride;
		data->Scan0 = buffer;
	} else if (!buffer) {
		image->locked = 0;
		return InvalidParameter;
	}
	// A lock that names neither direction still shows the pixels, unless the program brought its own buffer for them.
	if (mode & ImageLockModeRead) {
		// An indexed lock of another format speaks in that format's standard palette.
		GpImage view = *image;
		view.format = format;
		view.bits = buffer;
		view.stride = stride;
		if (FORMAT_INDEXED(format) && format != image->format && !image->palette)
			view.palette = default_palette(format);
		for (INT y = 0; y < area.Height; y++)
			for (INT x = 0; x < area.Width; x++)
				gp_pixel_write(&view, x, y, gp_pixel_read(image, area.X + x, area.Y + y));
		if (view.palette != image->palette)
			gp_free(view.palette);
	}
	return Ok;
}

GpStatus GDIPAPI GdipBitmapUnlockBits(GpImage *image, BitmapData *data)
{
	if (!image || !data)
		return InvalidParameter;
	if (!image->locked)
		return WrongState;
	BOOL direct = data->Format == image->format && data->Scan0 == image->bits + (SSIZE_T)image->lock_rect.Y * image->stride + image->lock_rect.X * FORMAT_BITS(data->Format) / 8;
	if (!direct && (image->lock_mode & ImageLockModeWrite)) {
		GpImage view = *image;
		view.format = data->Format;
		view.bits = data->Scan0;
		view.stride = data->Stride;
		if (FORMAT_INDEXED(data->Format) && data->Format != image->format && !image->palette)
			view.palette = default_palette(data->Format);
		for (INT y = 0; y < image->lock_rect.Height; y++)
			for (INT x = 0; x < image->lock_rect.Width; x++)
				gp_pixel_write(image, image->lock_rect.X + x, image->lock_rect.Y + y, gp_pixel_read(&view, x, y));
		if (view.palette != image->palette)
			gp_free(view.palette);
	}
	gp_free(image->lock_copy);
	image->lock_copy = NULL;
	image->locked = 0;
	return Ok;
}

GpStatus GDIPAPI GdipCloneBitmapAreaI(INT x, INT y, INT width, INT height, PixelFormat format, GpImage *image, GpImage **out)
{
	if (!image || !out || image->type != ImageTypeBitmap)
		return InvalidParameter;
	if (x < 0 || y < 0 || width <= 0 || height <= 0 || (UINT)(x + width) > image->width || (UINT)(y + height) > image->height)
		return InvalidParameter;
	if (format == PixelFormatUndefined)
		format = image->format;
	GpStatus status = gp_bitmap_new(width, height, format, out);
	if (status != Ok)
		return status;
	if (image->palette && (*out)->palette && (*out)->palette->Count == image->palette->Count)
		memcpy((*out)->palette->Entries, image->palette->Entries, image->palette->Count * sizeof(ARGB));
	(*out)->dpi_x = image->dpi_x;
	(*out)->dpi_y = image->dpi_y;
	if (FORMAT_INDEXED(format) && format == image->format && (x * FORMAT_BITS(format)) % 8 == 0) {
		INT bytes = stride_of(width, format);
		for (INT row = 0; row < height; row++)
			memcpy((*out)->bits + (SSIZE_T)row * (*out)->stride, image->bits + (SSIZE_T)(y + row) * image->stride + x * FORMAT_BITS(format) / 8, bytes);
	} else {
		copy_area(*out, 0, 0, image, x, y, width, height);
	}
	return Ok;
}

GpStatus GDIPAPI GdipCloneBitmapArea(REAL x, REAL y, REAL width, REAL height, PixelFormat format, GpImage *image, GpImage **out)
{
	return GdipCloneBitmapAreaI((INT)x, (INT)y, (INT)width, (INT)height, format, image, out);
}

GpStatus GDIPAPI GdipCloneImage(GpImage *image, GpImage **out)
{
	if (!image || !out)
		return InvalidParameter;
	if (image->type == ImageTypeBitmap) {
		GpStatus status = GdipCloneBitmapAreaI(0, 0, image->width, image->height, image->format, image, out);
		if (status == Ok)
			(*out)->raw_format = image->raw_format;
		return status;
	}
	GpImage *copy = gp_dup(image, sizeof(GpImage));
	if (!copy)
		return OutOfMemory;
	copy->refs = 1;
	copy->emf = image->emf ? CopyEnhMetaFileW(image->emf, NULL) : NULL;
	copy->recording = NULL;
	*out = copy;
	return Ok;
}

BOOL gp_image_bounds(GpImage *image, GpRectF *out)
{
	out->X = out->Y = 0.0f;
	out->Width = (REAL)image->width;
	out->Height = (REAL)image->height;
	return image->width && image->height;
}

// A texture keeps a private copy of its source rectangle, so later edits of the image do not reach the brush.
GpImage *gp_image_crop(GpImage *image, const GpRectF *area)
{
	GpImage *copy;
	if (image->type != ImageTypeBitmap)
		return NULL;
	if (GdipCloneBitmapAreaI((INT)area->X, (INT)area->Y, (INT)area->Width, (INT)area->Height, image->format, image, &copy) != Ok)
		return NULL;
	return copy;
}

ARGB gp_image_sample(GpImage *image, const void *attributes, REAL x, REAL y)
{
	INT px = (INT)gp_floor(x), py = (INT)gp_floor(y);
	if (px < 0) px = 0;
	if (py < 0) py = 0;
	if ((UINT)px >= image->width) px = image->width - 1;
	if ((UINT)py >= image->height) py = image->height - 1;
	ARGB color = gp_pixel_read(image, px, py);
	return attributes ? gp_attributes_apply(attributes, color) : color;
}

GpStatus GDIPAPI GdipGetImageType(GpImage *image, INT *out)
{
	if (!image || !out)
		return InvalidParameter;
	*out = image->type;
	return Ok;
}

GpStatus GDIPAPI GdipGetImageWidth(GpImage *image, UINT *out)
{
	if (!image || !out)
		return InvalidParameter;
	*out = image->width;
	return Ok;
}

GpStatus GDIPAPI GdipGetImageHeight(GpImage *image, UINT *out)
{
	if (!image || !out)
		return InvalidParameter;
	*out = image->height;
	return Ok;
}

GpStatus GDIPAPI GdipGetImageDimension(GpImage *image, REAL *width, REAL *height)
{
	if (!image || !width || !height)
		return InvalidParameter;
	*width = image->type == ImageTypeMetafile ? image->frame.Width : (REAL)image->width;
	*height = image->type == ImageTypeMetafile ? image->frame.Height : (REAL)image->height;
	return Ok;
}

GpStatus GDIPAPI GdipGetImageBounds(GpImage *image, GpRectF *out, GpUnit *unit)
{
	if (!image || !out || !unit)
		return InvalidParameter;
	if (image->type == ImageTypeMetafile) {
		*out = image->frame;
		*unit = image->frame_unit;
		return Ok;
	}
	gp_image_bounds(image, out);
	*unit = UnitPixel;
	return Ok;
}

GpStatus GDIPAPI GdipGetImagePixelFormat(GpImage *image, PixelFormat *out)
{
	if (!image || !out)
		return InvalidParameter;
	*out = image->type == ImageTypeBitmap ? image->format : PixelFormat32bppRGB;
	return Ok;
}

GpStatus GDIPAPI GdipGetImageFlags(GpImage *image, UINT *out)
{
	if (!image || !out)
		return InvalidParameter;
	*out = image->flags;
	return Ok;
}

GpStatus GDIPAPI GdipGetImageRawFormat(GpImage *image, GUID *out)
{
	if (!image || !out)
		return InvalidParameter;
	*out = image->raw_format;
	return Ok;
}

GpStatus GDIPAPI GdipGetImageHorizontalResolution(GpImage *image, REAL *out)
{
	if (!image || !out)
		return InvalidParameter;
	*out = image->dpi_x;
	return Ok;
}

GpStatus GDIPAPI GdipGetImageVerticalResolution(GpImage *image, REAL *out)
{
	if (!image || !out)
		return InvalidParameter;
	*out = image->dpi_y;
	return Ok;
}

GpStatus GDIPAPI GdipBitmapSetResolution(GpImage *image, REAL x, REAL y)
{
	if (!image || x <= 0.0f || y <= 0.0f)
		return InvalidParameter;
	image->dpi_x = x;
	image->dpi_y = y;
	return Ok;
}

GpStatus GDIPAPI GdipGetImagePaletteSize(GpImage *image, INT *out)
{
	if (!image || !out)
		return InvalidParameter;
	UINT count = image->palette ? image->palette->Count : 0;
	*out = (INT)(sizeof(UINT) * 2 + (count ? count : 1) * sizeof(ARGB));
	return Ok;
}

GpStatus GDIPAPI GdipGetImagePalette(GpImage *image, ColorPalette *out, INT size)
{
	if (!image || !out)
		return InvalidParameter;
	INT needed;
	GdipGetImagePaletteSize(image, &needed);
	if (size < needed)
		return InsufficientBuffer;
	if (image->palette) {
		memcpy(out, image->palette, sizeof(UINT) * 2 + image->palette->Count * sizeof(ARGB));
	} else {
		out->Flags = 0;
		out->Count = 0;
	}
	return Ok;
}

GpStatus GDIPAPI GdipSetImagePalette(GpImage *image, const ColorPalette *palette)
{
	if (!image || !palette || palette->Count > 256)
		return InvalidParameter;
	SIZE_T bytes = sizeof(UINT) * 2 + palette->Count * sizeof(ARGB);
	ColorPalette *copy = gp_alloc(bytes + sizeof(ARGB));
	if (!copy)
		return OutOfMemory;
	memcpy(copy, palette, bytes);
	gp_free(image->palette);
	image->palette = copy;
	return Ok;
}

GpStatus GDIPAPI GdipImageForceValidation(GpImage *image)
{
	return image ? Ok : InvalidParameter;
}

GpStatus GDIPAPI GdipImageSetAbort(GpImage *image, void *abort)
{
	(void)abort;
	return image ? Ok : InvalidParameter;
}

GpStatus GDIPAPI GdipImageRotateFlip(GpImage *image, INT kind)
{
	if (!image || image->type != ImageTypeBitmap || kind < 0 || kind > 7)
		return image && image->type != ImageTypeBitmap ? NotImplemented : InvalidParameter;
	if (image->locked)
		return WrongState;
	if (!kind)
		return Ok;
	// Rotations of 90 and 270 swap the sides; the flips follow the rotation.
	BOOL swap = kind & 1;
	static const BYTE flip_x_table[8] = {0, 0, 1, 1, 1, 1, 0, 0}, flip_y_table[8] = {0, 1, 1, 0, 0, 0, 1, 1};
	GpImage *made;
	UINT out_w = swap ? image->height : image->width, out_h = swap ? image->width : image->height;
	GpStatus status = gp_bitmap_new(out_w, out_h, image->format, &made);
	if (status != Ok)
		return status;
	if (image->palette)
		memcpy(made->palette, image->palette, sizeof(UINT) * 2 + image->palette->Count * sizeof(ARGB));
	for (UINT y = 0; y < out_h; y++) {
		for (UINT x = 0; x < out_w; x++) {
			UINT sx = swap ? y : x, sy = swap ? out_w - 1 - x : y;
			static const BYTE y_flip_when_swapped[8] = {0, 0, 0, 1, 0, 1, 0, 0};
			if (swap) {
				sy = (kind == 1 || kind == 7) ? out_w - 1 - x : x;
				sx = (kind == 1 || kind == 5) ? y : out_h - 1 - y;
				(void)y_flip_when_swapped;
			} else {
				if (flip_x_table[kind])
					sx = image->width - 1 - x;
				if (flip_y_table[kind])
					sy = image->height - 1 - y;
			}
			if (FORMAT_INDEXED(image->format)) {
				const BYTE *row = image->bits + (SSIZE_T)sy * image->stride;
				BYTE *to = made->bits + (SSIZE_T)y * made->stride;
				UINT value = FORMAT_BITS(image->format) == 8 ? row[sx] : FORMAT_BITS(image->format) == 4 ? ((sx & 1) ? row[sx / 2] & 15 : row[sx / 2] >> 4) : (row[sx / 8] >> (7 - sx % 8)) & 1;
				if (FORMAT_BITS(image->format) == 8)
					to[x] = (BYTE)value;
				else if (FORMAT_BITS(image->format) == 4)
					to[x / 2] = (x & 1) ? (to[x / 2] & 0xf0) | value : (to[x / 2] & 0x0f) | value << 4;
				else if (value)
					to[x / 8] |= 0x80 >> (x % 8);
			} else {
				gp_pixel_write(made, x, y, gp_pixel_read(image, sx, sy));
			}
		}
	}
	if (image->owned)
		VirtualFree(image->owned, 0, MEM_RELEASE);
	image->owned = made->owned;
	image->bits = made->bits;
	image->stride = made->stride;
	image->width = out_w;
	image->height = out_h;
	made->owned = NULL;
	made->bits = NULL;
	gp_image_release(made);
	// What was turned is the bitmap in memory now; the other frames of the file are gone.
	if (image->decoder)
		IWICBitmapDecoder_Release((IWICBitmapDecoder *)image->decoder);
	if (image->stream)
		IStream_Release(image->stream);
	image->decoder = NULL;
	image->stream = NULL;
	image->frame_count = 1;
	image->raw_format = format_memory;
	return Ok;
}

static HRESULT imaging_factory(IWICImagingFactory **out)
{
	static HRESULT (WINAPI *create)(UINT, IWICImagingFactory **);
	if (!create) {
		HMODULE module = LoadLibraryW(L"windowscodecs.dll");
		if (module)
			create = (void *)GetProcAddress(module, "WICCreateImagingFactory_Proxy");
		if (!create)
			return E_FAIL;
	}
	return create(WINCODEC_SDK_VERSION, out);
}

// The pixel format a decoded frame keeps: the format it already has when this API has an equal one, else 32 bit ARGB.
static PixelFormat format_from_wic(const GUID *wic, const GUID **convert_to)
{
	static const struct { const GUID *wic; PixelFormat format; } table[] = {
		{&GUID_WICPixelFormat1bppIndexed, PixelFormat1bppIndexed}, {&GUID_WICPixelFormat4bppIndexed, PixelFormat4bppIndexed},
		{&GUID_WICPixelFormat8bppIndexed, PixelFormat8bppIndexed}, {&GUID_WICPixelFormat16bppBGR555, PixelFormat16bppRGB555},
		{&GUID_WICPixelFormat16bppBGR565, PixelFormat16bppRGB565}, {&GUID_WICPixelFormat16bppBGRA5551, PixelFormat16bppARGB1555},
		{&GUID_WICPixelFormat24bppBGR, PixelFormat24bppRGB}, {&GUID_WICPixelFormat32bppBGR, PixelFormat32bppRGB},
		{&GUID_WICPixelFormat32bppBGRA, PixelFormat32bppARGB}, {&GUID_WICPixelFormat32bppPBGRA, PixelFormat32bppPARGB},
		{&GUID_WICPixelFormat48bppRGB, PixelFormat48bppRGB}, {&GUID_WICPixelFormat48bppBGR, PixelFormat48bppRGB},
		{&GUID_WICPixelFormat64bppRGBA, PixelFormat64bppARGB}, {&GUID_WICPixelFormat64bppBGRA, PixelFormat64bppARGB},
		{&GUID_WICPixelFormat64bppPBGRA, PixelFormat64bppPARGB}, {&GUID_WICPixelFormat32bppCMYK, PixelFormat32bppCMYK},
		{&GUID_WICPixelFormatBlackWhite, PixelFormat1bppIndexed}, {&GUID_WICPixelFormat8bppGray, PixelFormat8bppIndexed},
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (IsEqualGUID(wic, table[i].wic)) {
			*convert_to = table[i].wic;
			return table[i].format;
		}
	}
	*convert_to = &GUID_WICPixelFormat32bppBGRA;
	return PixelFormat32bppARGB;
}

// Decodes one frame over the image's pixels, replacing format, size and palette.
// A transparency chunk turns any colour type into one with alpha, which the codec's own format does not tell.
static BOOL png_has_transparency(IStream *stream)
{
	LARGE_INTEGER at = {{8, 0}};
	BOOL found = FALSE;
	BYTE header[8];
	ULONG got;
	if (!stream || FAILED(IStream_Seek(stream, at, STREAM_SEEK_SET, NULL)))
		return FALSE;
	while (SUCCEEDED(IStream_Read(stream, header, sizeof(header), &got)) && got == sizeof(header) && memcmp(header + 4, "IDAT", 4)) {
		if (!memcmp(header + 4, "tRNS", 4)) {
			found = TRUE;
			break;
		}
		at.QuadPart = (LONGLONG)((ULONG)header[0] << 24 | header[1] << 16 | header[2] << 8 | header[3]) + 4;
		if (FAILED(IStream_Seek(stream, at, STREAM_SEEK_CUR, NULL)))
			break;
	}
	return found;
}

static UINT frame_metadata(IWICMetadataQueryReader *query, const WCHAR *name, UINT otherwise)
{
	PROPVARIANT value;
	PropVariantInit(&value);
	if (!query || FAILED(IWICMetadataQueryReader_GetMetadataByName(query, name, &value)))
		return otherwise;
	UINT number = value.vt == VT_UI2 ? value.uiVal : value.vt == VT_UI1 ? value.bVal : value.vt == VT_BOOL ? value.boolVal != 0 : otherwise;
	return number;
}

// An animation's frames are patches on a shared canvas: each is painted over what the one before left behind.
static GpStatus compose_animation(IWICBitmapDecoder *decoder, IWICImagingFactory *factory, UINT index, GpImage **out)
{
	IWICMetadataQueryReader *global = NULL;
	if (FAILED(IWICBitmapDecoder_GetMetadataQueryReader(decoder, &global)))
		global = NULL;
	UINT width = frame_metadata(global, L"/logscrdesc/Width", 0), height = frame_metadata(global, L"/logscrdesc/Height", 0);
	// Clearing a frame's patch shows the file's background colour, when it has one.
	UINT background_index = frame_metadata(global, L"/logscrdesc/BackgroundColorIndex", 0);
	ARGB background = 0;
	IWICPalette *global_palette;
	if (SUCCEEDED(IWICImagingFactory_CreatePalette(factory, &global_palette))) {
		WICColor colors[256];
		UINT colors_count = 0;
		if (SUCCEEDED(IWICBitmapDecoder_CopyPalette(decoder, global_palette)) && SUCCEEDED(IWICPalette_GetColors(global_palette, 256, colors, &colors_count)) && background_index < colors_count)
			background = colors[background_index];
		IWICPalette_Release(global_palette);
	}
	if (global)
		IWICMetadataQueryReader_Release(global);
	GpImage *canvas = NULL, *saved = NULL;
	GpStatus status = Ok;
	for (UINT k = 0; k <= index && status == Ok; k++) {
		IWICBitmapFrameDecode *frame;
		if (FAILED(IWICBitmapDecoder_GetFrame(decoder, k, &frame))) {
			status = GenericError;
			break;
		}
		UINT frame_width = 0, frame_height = 0, left = 0, top = 0, disposal = 0;
		IWICBitmapFrameDecode_GetSize(frame, &frame_width, &frame_height);
		IWICMetadataQueryReader *query = NULL;
		if (SUCCEEDED(IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &query))) {
			left = frame_metadata(query, L"/imgdesc/Left", 0);
			top = frame_metadata(query, L"/imgdesc/Top", 0);
			disposal = frame_metadata(query, L"/grctlext/Disposal", 0);
			// A first frame with a transparent colour makes the whole animation clear to nothing.
			if (!k && frame_metadata(query, L"/grctlext/TransparencyFlag", 0))
				background = 0;
			IWICMetadataQueryReader_Release(query);
		}
		if (!width || !height) {
			width = frame_width;
			height = frame_height;
		}
		if (!canvas)
			status = gp_bitmap_new(width, height, PixelFormat32bppARGB, &canvas);
		GpImage *patch = NULL;
		IWICFormatConverter *converter = NULL;
		if (status == Ok)
			status = gp_bitmap_new(frame_width, frame_height, PixelFormat32bppARGB, &patch);
		WICRect all = {0, 0, (INT)frame_width, (INT)frame_height};
		if (status == Ok && (FAILED(IWICImagingFactory_CreateFormatConverter(factory, &converter))
			|| FAILED(IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))
			|| FAILED(IWICFormatConverter_CopyPixels(converter, &all, patch->stride, patch->stride * frame_height, patch->bits))))
			status = GenericError;
		if (status == Ok) {
			// Restoring to the previous state needs that state kept before painting.
			if (disposal == 3 && k < index && !saved && gp_bitmap_new(width, height, PixelFormat32bppARGB, &saved) == Ok)
				copy_area(saved, 0, 0, canvas, 0, 0, width, height);
			for (UINT y = 0; y < frame_height && top + y < height; y++) {
				for (UINT x = 0; x < frame_width && left + x < width; x++) {
					ARGB color = gp_pixel_read(patch, x, y);
					if (color >> 24)
						gp_pixel_write(canvas, left + x, top + y, color);
				}
			}
			if (k < index && disposal == 2) {
				for (UINT y = 0; y < frame_height && top + y < height; y++)
					for (UINT x = 0; x < frame_width && left + x < width; x++)
						gp_pixel_write(canvas, left + x, top + y, background);
			} else if (k < index && disposal == 3 && saved) {
				copy_area(canvas, 0, 0, saved, 0, 0, width, height);
			}
		}
		if (saved && disposal != 3) {
			gp_image_release(saved);
			saved = NULL;
		}
		if (converter)
			IWICFormatConverter_Release(converter);
		if (patch)
			gp_image_release(patch);
		IWICBitmapFrameDecode_Release(frame);
	}
	if (saved)
		gp_image_release(saved);
	if (status == Ok) {
		*out = canvas;
	} else if (canvas) {
		gp_image_release(canvas);
	}
	return status;
}

static GpStatus decode_frame(GpImage *image, IWICBitmapDecoder *decoder, IWICImagingFactory *factory, UINT index, IStream *stream)
{
	IWICBitmapFrameDecode *frame;
	if (FAILED(IWICBitmapDecoder_GetFrame(decoder, index, &frame)))
		return GenericError;
	GpStatus status = GenericError;
	UINT width, height;
	GUID wic;
	IWICBitmapSource *source = (IWICBitmapSource *)frame;
	IWICFormatConverter *converter = NULL;
	if (FAILED(IWICBitmapFrameDecode_GetSize(frame, &width, &height)) || FAILED(IWICBitmapFrameDecode_GetPixelFormat(frame, &wic)))
		goto done;
	const GUID *target;
	PixelFormat format = format_from_wic(&wic, &target);
	BOOL png = image->raw_format.Data1 == format_png.Data1, png_alpha = png && png_has_transparency(stream);
	BOOL gray = IsEqualGUID(&wic, &GUID_WICPixelFormatBlackWhite) || IsEqualGUID(&wic, &GUID_WICPixelFormat2bppGray) || IsEqualGUID(&wic, &GUID_WICPixelFormat4bppGray)
		|| IsEqualGUID(&wic, &GUID_WICPixelFormat8bppGray) || IsEqualGUID(&wic, &GUID_WICPixelFormat16bppGray);
	// An animation is handed out as whole coloured frames, and so is a PNG with transparency or more than one bit of gray.
	if ((image->raw_format.Data1 == format_gif.Data1 && image->frame_count > 1) || png_alpha
		|| (png && gray && !IsEqualGUID(&wic, &GUID_WICPixelFormatBlackWhite))) {
		format = PixelFormat32bppARGB;
		target = &GUID_WICPixelFormat32bppBGRA;
	}
	if (!IsEqualGUID(target, &wic)) {
		if (FAILED(IWICImagingFactory_CreateFormatConverter(factory, &converter)))
			goto done;
		if (FAILED(IWICFormatConverter_Initialize(converter, source, target, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)))
			goto done;
		source = (IWICBitmapSource *)converter;
	}
	GpImage *made;
	if (image->raw_format.Data1 == format_gif.Data1 && image->frame_count > 1) {
		if ((status = compose_animation(decoder, factory, index, &made)) != Ok)
			goto done;
		width = made->width;
		height = made->height;
	} else {
		if ((status = gp_bitmap_new(width, height, format, &made)) != Ok)
			goto done;
		WICRect all = {0, 0, (INT)width, (INT)height};
		if (FAILED(IWICBitmapSource_CopyPixels(source, &all, made->stride, made->stride * height, made->bits))) {
			gp_image_release(made);
			status = GenericError;
			goto done;
		}
	}
	if (FORMAT_INDEXED(format)) {
		IWICPalette *palette;
		if (SUCCEEDED(IWICImagingFactory_CreatePalette(factory, &palette))) {
			UINT count = 0;
			if (SUCCEEDED(IWICBitmapFrameDecode_CopyPalette(frame, palette)) && SUCCEEDED(IWICPalette_GetColorCount(palette, &count)) && count <= made->palette->Count)
				IWICPalette_GetColors(palette, count, (WICColor *)made->palette->Entries, &count);
			if (count)
				made->palette->Count = count;
			IWICPalette_Release(palette);
			made->palette->Flags = 0;
			for (UINT i = 0; i < count; i++) {
				if (made->palette->Entries[i] >> 24 != 255)
					made->palette->Flags = 1;
			}
		}
		// Gray sources carry no palette of their own: the ramp is implied by the format.
		if (IsEqualGUID(&wic, &GUID_WICPixelFormatBlackWhite)) {
			made->palette->Count = 2;
			made->palette->Entries[0] = 0xff000000;
			made->palette->Entries[1] = 0xffffffff;
		} else if (IsEqualGUID(&wic, &GUID_WICPixelFormat8bppGray)) {
			made->palette->Count = 256;
			for (UINT i = 0; i < 256; i++)
				made->palette->Entries[i] = 0xff000000 | i << 16 | i << 8 | i;
		}
	}
	if (gray && !png_alpha && !(png && IsEqualGUID(&wic, &GUID_WICPixelFormatBlackWhite)))
		image->flags = (image->flags & ~0x10u) | 0x40u;
	double dpi_x = 0.0, dpi_y = 0.0;
	if (SUCCEEDED(IWICBitmapFrameDecode_GetResolution(frame, &dpi_x, &dpi_y)) && dpi_x > 0.0 && dpi_y > 0.0) {
		image->dpi_x = (REAL)dpi_x;
		image->dpi_y = (REAL)dpi_y;
	} else {
		image->dpi_x = image->dpi_y = screen_dpi_x();
	}
	if (index == 0 || image->raw_format.Data1 != format_gif.Data1)
		gp_properties_load(image, decoder, frame, factory);
	if (image->owned)
		VirtualFree(image->owned, 0, MEM_RELEASE);
	gp_free(image->palette);
	image->owned = made->owned;
	image->bits = made->bits;
	image->stride = made->stride;
	image->palette = made->palette;
	image->width = width;
	image->height = height;
	image->format = format;
	made->owned = NULL;
	made->bits = NULL;
	made->palette = NULL;
	gp_image_release(made);
	status = Ok;
done:
	if (converter)
		IWICFormatConverter_Release(converter);
	IWICBitmapFrameDecode_Release(frame);
	return status;
}

GpStatus GDIPAPI GdipLoadImageFromStream(IStream *stream, GpImage **out)
{
	if (!stream || !out)
		return InvalidParameter;
	*out = NULL;
	IWICImagingFactory *factory;
	if (FAILED(imaging_factory(&factory)))
		return GenericError;
	LARGE_INTEGER zero = {{0, 0}};
	IStream_Seek(stream, zero, STREAM_SEEK_SET, NULL);
	STATSTG stat;
	if (SUCCEEDED(IStream_Stat(stream, &stat, STATFLAG_NONAME)) && !stat.cbSize.QuadPart) {
		IWICImagingFactory_Release(factory);
		return InvalidParameter;
	}
	IWICBitmapDecoder *decoder;
	HRESULT result = IWICImagingFactory_CreateDecoderFromStream(factory, stream, NULL, WICDecodeMetadataCacheOnLoad, &decoder);
	if (FAILED(result)) {
		IWICImagingFactory_Release(factory);
		return OutOfMemory;
	}
	GpImage *image = gp_alloc(sizeof(GpImage));
	GpStatus status = image ? Ok : OutOfMemory;
	UINT frames = 0;
	GUID container;
	if (status == Ok && (FAILED(IWICBitmapDecoder_GetFrameCount(decoder, &frames)) || !frames))
		status = OutOfMemory;
	if (status == Ok) {
		image->type = ImageTypeBitmap;
		image->refs = 1;
		image->flags = 0x10;
		image->raw_format = format_memory;
		if (SUCCEEDED(IWICBitmapDecoder_GetContainerFormat(decoder, &container))) {
			if (IsEqualGUID(&container, &GUID_ContainerFormatBmp)) image->raw_format = format_bmp;
			else if (IsEqualGUID(&container, &GUID_ContainerFormatPng)) image->raw_format = format_png;
			else if (IsEqualGUID(&container, &GUID_ContainerFormatJpeg)) image->raw_format = format_jpeg;
			else if (IsEqualGUID(&container, &GUID_ContainerFormatGif)) image->raw_format = format_gif;
			else if (IsEqualGUID(&container, &GUID_ContainerFormatTiff)) image->raw_format = format_tiff;
			else if (IsEqualGUID(&container, &GUID_ContainerFormatIco)) image->raw_format = format_icon;
		}
		image->dimension = image->raw_format.Data1 == format_gif.Data1 ? dimension_time : dimension_page;
		image->frame_count = frames;
		status = decode_frame(image, decoder, factory, 0, stream);
	}
	if (status == Ok) {
		image->flags |= 0x10000 | (FORMAT_ALPHA(image->format) ? 2 : 0) | 0x1000 | 0x2000;
		if (image->palette)
			image->flags |= 0;
		image->decoder = decoder;
		IStream_AddRef(stream);
		image->stream = stream;
		*out = image;
	} else {
		IWICBitmapDecoder_Release(decoder);
		gp_free(image);
	}
	IWICImagingFactory_Release(factory);
	return status;
}

GpStatus GDIPAPI GdipLoadImageFromStreamICM(IStream *stream, GpImage **out)
{
	return GdipLoadImageFromStream(stream, out);
}

GpStatus GDIPAPI GdipCreateBitmapFromStream(IStream *stream, GpImage **out)
{
	return GdipLoadImageFromStream(stream, out);
}

GpStatus GDIPAPI GdipCreateBitmapFromStreamICM(IStream *stream, GpImage **out)
{
	return GdipLoadImageFromStream(stream, out);
}

GpStatus GDIPAPI GdipCreateStreamOnFile(const WCHAR *name, UINT access, IStream **out)
{
	extern HRESULT WINAPI SHCreateStreamOnFileW(const WCHAR *, DWORD, IStream **);
	if (!name || !out)
		return InvalidParameter;
	DWORD mode = access == GENERIC_WRITE ? (STGM_CREATE | STGM_WRITE | STGM_SHARE_DENY_WRITE) : access == GENERIC_READ ? (STGM_READ | STGM_SHARE_DENY_WRITE) : (STGM_READWRITE | STGM_SHARE_DENY_WRITE);
	return FAILED(SHCreateStreamOnFileW(name, mode, out)) ? GenericError : Ok;
}

GpStatus GDIPAPI GdipLoadImageFromFile(const WCHAR *name, GpImage **out)
{
	if (!name || !out)
		return InvalidParameter;
	IStream *stream;
	*out = NULL;
	if (GdipCreateStreamOnFile(name, GENERIC_READ, &stream) != Ok)
		return OutOfMemory;
	GpStatus status = GdipLoadImageFromStream(stream, out);
	IStream_Release(stream);
	return status;
}

GpStatus GDIPAPI GdipLoadImageFromFileICM(const WCHAR *name, GpImage **out)
{
	return GdipLoadImageFromFile(name, out);
}

GpStatus GDIPAPI GdipCreateBitmapFromFile(const WCHAR *name, GpImage **out)
{
	return GdipLoadImageFromFile(name, out);
}

GpStatus GDIPAPI GdipCreateBitmapFromFileICM(const WCHAR *name, GpImage **out)
{
	return GdipLoadImageFromFile(name, out);
}

GpStatus GDIPAPI GdipImageGetFrameDimensionsCount(GpImage *image, UINT *out)
{
	if (!image || !out)
		return InvalidParameter;
	*out = 1;
	return Ok;
}

GpStatus GDIPAPI GdipImageGetFrameDimensionsList(GpImage *image, GUID *out, UINT count)
{
	if (!image || !out || !count)
		return InvalidParameter;
	if (count != 1)
		return InvalidParameter;
	*out = image->decoder ? image->dimension : dimension_page;
	return Ok;
}

GpStatus GDIPAPI GdipImageGetFrameCount(GpImage *image, const GUID *dimension, UINT *out)
{
	if (!image || !out)
		return InvalidParameter;
	if (image->decoder && dimension && !IsEqualGUID(dimension, &image->dimension))
		return InvalidParameter;
	*out = image->decoder ? image->frame_count : 1;
	return Ok;
}

GpStatus GDIPAPI GdipImageSelectActiveFrame(GpImage *image, const GUID *dimension, UINT index)
{
	if (!image)
		return InvalidParameter;
	// A bitmap built in memory has nothing to move through.
	if (!image->decoder)
		return Ok;
	const GUID *own = &image->dimension;
	if (!dimension || !IsEqualGUID(dimension, own))
		return InvalidParameter;
	UINT frames = image->decoder ? image->frame_count : 1;
	if (index >= frames)
		return InvalidParameter;
	if (!image->decoder || index == image->frame_active)
		return Ok;
	IWICImagingFactory *factory;
	if (FAILED(imaging_factory(&factory)))
		return GenericError;
	GpStatus status = decode_frame(image, image->decoder, factory, index, image->stream);
	IWICImagingFactory_Release(factory);
	if (status == Ok)
		image->frame_active = index;
	return status;
}

typedef struct
{
	CLSID clsid;
	const GUID *format;
	const GUID *container;
	const WCHAR *name, *dll, *description, *extensions, *mime;
	const BYTE *signature, *mask;
	DWORD signature_size;
} codec_row;

static const BYTE sig_bmp[] = {'B', 'M'}, mask_two[] = {0xff, 0xff};
static const BYTE sig_png[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a}, mask_eight[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const BYTE sig_jpeg[] = {0xff, 0xd8};
static const BYTE sig_gif[] = {'G', 'I', 'F', '8', '7', 'a'}, mask_six[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const BYTE sig_tiff[] = {'I', 'I', 0x2a, 0x00}, mask_four[] = {0xff, 0xff, 0xff, 0xff};

static const codec_row codec_rows[] = {
	{CODEC_CLSID(0x00), &format_bmp, &GUID_ContainerFormatBmp, L"Built-in BMP", L"gdiplus.dll", L"BMP", L"*.BMP;*.DIB;*.RLE", L"image/bmp", sig_bmp, mask_two, 2},
	{CODEC_CLSID(0x01), &format_jpeg, &GUID_ContainerFormatJpeg, L"Built-in JPEG", L"gdiplus.dll", L"JPEG", L"*.JPG;*.JPEG;*.JPE;*.JFIF", L"image/jpeg", sig_jpeg, mask_two, 2},
	{CODEC_CLSID(0x02), &format_gif, &GUID_ContainerFormatGif, L"Built-in GIF", L"gdiplus.dll", L"GIF", L"*.GIF", L"image/gif", sig_gif, mask_six, 6},
	{CODEC_CLSID(0x05), &format_tiff, &GUID_ContainerFormatTiff, L"Built-in TIFF", L"gdiplus.dll", L"TIFF", L"*.TIFF;*.TIF", L"image/tiff", sig_tiff, mask_four, 4},
	{CODEC_CLSID(0x06), &format_png, &GUID_ContainerFormatPng, L"Built-in PNG", L"gdiplus.dll", L"PNG", L"*.PNG", L"image/png", sig_png, mask_eight, 8},
};

typedef struct
{
	CLSID Clsid;
	GUID FormatID;
	const WCHAR *CodecName, *DllName, *FormatDescription, *FilenameExtension, *MimeType;
	DWORD Flags, Version, SigCount, SigSize;
	const BYTE *SigPattern, *SigMask;
} ImageCodecInfo;

static UINT wide_bytes(const WCHAR *text)
{
	return (lstrlenW(text) + 1) * sizeof(WCHAR);
}

static UINT codec_list_bytes(void)
{
	UINT total = 0;
	for (size_t i = 0; i < sizeof(codec_rows) / sizeof(codec_rows[0]); i++) {
		const codec_row *row = &codec_rows[i];
		total += sizeof(ImageCodecInfo) + wide_bytes(row->name) + wide_bytes(row->dll) + wide_bytes(row->description)
			+ wide_bytes(row->extensions) + wide_bytes(row->mime) + row->signature_size * 2;
	}
	return total;
}

static GpStatus codec_list(UINT count, UINT size, ImageCodecInfo *out, DWORD flags)
{
	UINT rows = sizeof(codec_rows) / sizeof(codec_rows[0]);
	if (!out || count != rows || size != codec_list_bytes())
		return GenericError;
	BYTE *strings = (BYTE *)(out + rows);
	for (UINT i = 0; i < rows; i++) {
		const codec_row *row = &codec_rows[i];
		ImageCodecInfo *info = &out[i];
		info->Clsid = row->clsid;
		info->FormatID = *row->format;
		const WCHAR *texts[5] = {row->name, row->dll, row->description, row->extensions, row->mime};
		const WCHAR **slots[5] = {&info->CodecName, &info->DllName, &info->FormatDescription, &info->FilenameExtension, &info->MimeType};
		for (int t = 0; t < 5; t++) {
			UINT bytes = wide_bytes(texts[t]);
			memcpy(strings, texts[t], bytes);
			*slots[t] = (const WCHAR *)strings;
			strings += bytes;
		}
		info->Flags = flags;
		info->Version = 1;
		info->SigCount = 1;
		info->SigSize = row->signature_size;
		memcpy(strings, row->signature, row->signature_size);
		info->SigPattern = strings;
		strings += row->signature_size;
		memcpy(strings, row->mask, row->signature_size);
		info->SigMask = strings;
		strings += row->signature_size;
	}
	return Ok;
}

GpStatus GDIPAPI GdipGetImageEncodersSize(UINT *count, UINT *size)
{
	if (!count || !size)
		return InvalidParameter;
	*count = sizeof(codec_rows) / sizeof(codec_rows[0]);
	*size = codec_list_bytes();
	return Ok;
}

GpStatus GDIPAPI GdipGetImageEncoders(UINT count, UINT size, ImageCodecInfo *out)
{
	return codec_list(count, size, out, 0x10007);
}

GpStatus GDIPAPI GdipGetImageDecodersSize(UINT *count, UINT *size)
{
	return GdipGetImageEncodersSize(count, size);
}

GpStatus GDIPAPI GdipGetImageDecoders(UINT count, UINT size, ImageCodecInfo *out)
{
	return codec_list(count, size, out, 0x10006);
}

// A frame is written as the closest format the WIC encoder takes; indexed images keep their palette.
static GpStatus write_frame(IWICImagingFactory *factory, IWICBitmapEncoder *encoder, GpImage *image)
{
	GpStatus status = GenericError;
	IWICBitmapFrameEncode *frame = NULL;
	IPropertyBag2 *options = NULL;
	if (FAILED(IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &options)) || FAILED(IWICBitmapFrameEncode_Initialize(frame, options)))
		goto done;
	IWICBitmapFrameEncode_SetSize(frame, image->width, image->height);
	IWICBitmapFrameEncode_SetResolution(frame, image->dpi_x, image->dpi_y);
	WICPixelFormatGUID wanted = GUID_WICPixelFormat32bppBGRA;
	GpImage *pixels = NULL;
	if (image->format == PixelFormat24bppRGB)
		wanted = GUID_WICPixelFormat24bppBGR;
	else if (image->format == PixelFormat32bppRGB)
		wanted = GUID_WICPixelFormat32bppBGR;
	else if (image->format == PixelFormat8bppIndexed)
		wanted = GUID_WICPixelFormat8bppIndexed;
	else if (image->format == PixelFormat4bppIndexed)
		wanted = GUID_WICPixelFormat4bppIndexed;
	else if (image->format == PixelFormat1bppIndexed)
		wanted = GUID_WICPixelFormat1bppIndexed;
	WICPixelFormatGUID accepted = wanted;
	IWICBitmapFrameEncode_SetPixelFormat(frame, &accepted);
	PixelFormat row_format = IsEqualGUID(&accepted, &GUID_WICPixelFormat24bppBGR) ? PixelFormat24bppRGB
		: IsEqualGUID(&accepted, &GUID_WICPixelFormat32bppBGR) ? PixelFormat32bppRGB
		: IsEqualGUID(&accepted, &GUID_WICPixelFormat8bppIndexed) ? PixelFormat8bppIndexed
		: IsEqualGUID(&accepted, &GUID_WICPixelFormat4bppIndexed) ? PixelFormat4bppIndexed
		: IsEqualGUID(&accepted, &GUID_WICPixelFormat1bppIndexed) ? PixelFormat1bppIndexed
		: PixelFormat32bppARGB;
	if (row_format == image->format) {
		pixels = image;
	} else if (FORMAT_INDEXED(row_format)) {
		// An encoder that only takes a palette gets the pixels matched against the fixed halftone one.
		GpImage *straight = NULL;
		IWICBitmap *memory = NULL;
		IWICFormatConverter *converter = NULL;
		IWICPalette *palette = NULL;
		BOOL converted = FALSE;
		if (gp_bitmap_new(image->width, image->height, PixelFormat32bppARGB, &straight) == Ok
			&& gp_bitmap_new(image->width, image->height, row_format, &pixels) == Ok) {
			copy_area(straight, 0, 0, image, 0, 0, image->width, image->height);
			WICRect all = {0, 0, (INT)image->width, (INT)image->height};
			UINT count = 0;
			converted = SUCCEEDED(IWICImagingFactory_CreateBitmapFromMemory(factory, image->width, image->height, &GUID_WICPixelFormat32bppBGRA, straight->stride, straight->stride * image->height, straight->bits, &memory))
				&& SUCCEEDED(IWICImagingFactory_CreatePalette(factory, &palette))
				&& SUCCEEDED(IWICPalette_InitializePredefined(palette, WICBitmapPaletteTypeFixedHalftone256, FALSE))
				&& SUCCEEDED(IWICImagingFactory_CreateFormatConverter(factory, &converter))
				&& SUCCEEDED(IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)memory, &accepted, WICBitmapDitherTypeNone, palette, 0.0, WICBitmapPaletteTypeCustom))
				&& SUCCEEDED(IWICFormatConverter_CopyPixels(converter, &all, pixels->stride, pixels->stride * image->height, pixels->bits))
				&& SUCCEEDED(IWICBitmapFrameEncode_SetPalette(frame, palette))
				&& SUCCEEDED(IWICPalette_GetColorCount(palette, &count));
		}
		if (converter)
			IWICFormatConverter_Release(converter);
		if (palette)
			IWICPalette_Release(palette);
		if (memory)
			IWICBitmap_Release(memory);
		if (straight)
			gp_image_release(straight);
		if (!converted) {
			if (pixels)
				gp_image_release(pixels);
			goto done;
		}
	} else {
		if ((status = gp_bitmap_new(image->width, image->height, row_format, &pixels)) != Ok)
			goto done;
		status = GenericError;
		copy_area(pixels, 0, 0, image, 0, 0, image->width, image->height);
	}
	if (image->palette && FORMAT_INDEXED(row_format)) {
		IWICPalette *palette;
		if (SUCCEEDED(IWICImagingFactory_CreatePalette(factory, &palette))) {
			IWICPalette_InitializeCustom(palette, (WICColor *)image->palette->Entries, image->palette->Count);
			IWICBitmapFrameEncode_SetPalette(frame, palette);
			IWICPalette_Release(palette);
		}
	}
	if (SUCCEEDED(IWICBitmapFrameEncode_WritePixels(frame, image->height, pixels->stride, pixels->stride * image->height, pixels->bits))
		&& SUCCEEDED(IWICBitmapFrameEncode_Commit(frame)))
		status = Ok;
	if (pixels != image)
		gp_image_release(pixels);
done:
	if (options)
		IPropertyBag2_Release(options);
	if (frame)
		IWICBitmapFrameEncode_Release(frame);
	return status;
}

static const GUID encoder_save_flag = {0x292266fc, 0xac40, 0x47bf, {0x8c, 0xfc, 0xa8, 0x5b, 0x89, 0xa6, 0x55, 0xde}};
enum { SaveMultiFrame = 18, SaveFlush = 20, SaveFrameTime = 21, SaveFrameResolution = 22, SaveFramePage = 23 };

typedef struct
{
	GUID guid;
	ULONG values;
	ULONG type;
	void *value;
} encoder_parameter;

typedef struct
{
	UINT count;
	encoder_parameter items[1];
} encoder_parameters;

GpStatus GDIPAPI GdipSaveAddImage(GpImage *, GpImage *, const void *);

static LONG save_flag(const void *parameters)
{
	const encoder_parameters *list = parameters;
	for (UINT i = 0; list && i < list->count; i++) {
		if (IsEqualGUID(&list->items[i].guid, &encoder_save_flag) && list->items[i].values)
			return *(const LONG *)list->items[i].value;
	}
	return 0;
}

GpStatus GDIPAPI GdipSaveImageToStream(GpImage *image, IStream *stream, const CLSID *clsid, const void *parameters)
{
	if (!image || !stream || !clsid)
		return InvalidParameter;
	if (image->type != ImageTypeBitmap)
		return InvalidParameter;
	const codec_row *row = NULL;
	for (size_t i = 0; i < sizeof(codec_rows) / sizeof(codec_rows[0]); i++) {
		if (IsEqualGUID(clsid, &codec_rows[i].clsid))
			row = &codec_rows[i];
	}
	if (!row)
		return UnknownImageFormat;
	IWICImagingFactory *factory;
	if (FAILED(imaging_factory(&factory)))
		return GenericError;
	end_saving(image);
	GpStatus status = GenericError;
	IWICBitmapEncoder *encoder = NULL;
	if (SUCCEEDED(IWICImagingFactory_CreateEncoder(factory, row->container, NULL, &encoder))
		&& SUCCEEDED(IWICBitmapEncoder_Initialize(encoder, stream, WICBitmapEncoderNoCache)))
		status = write_frame(factory, encoder, image);
	if (status == Ok && save_flag(parameters) == SaveMultiFrame) {
		IStream_AddRef(stream);
		image->saving = encoder;
		image->saving_stream = stream;
		encoder = NULL;
	} else if (status == Ok && FAILED(IWICBitmapEncoder_Commit(encoder))) {
		status = GenericError;
	}
	if (encoder)
		IWICBitmapEncoder_Release(encoder);
	IWICImagingFactory_Release(factory);
	return status;
}

GpStatus GDIPAPI GdipSaveImageToFile(GpImage *image, const WCHAR *name, const CLSID *clsid, const void *parameters)
{
	if (!image || !name || !clsid)
		return InvalidParameter;
	IStream *stream;
	GpStatus status = GdipCreateStreamOnFile(name, GENERIC_WRITE, &stream);
	if (status != Ok)
		return status;
	status = GdipSaveImageToStream(image, stream, clsid, parameters);
	IStream_Release(stream);
	return status;
}

GpStatus GDIPAPI GdipSaveAdd(GpImage *image, const void *parameters)
{
	if (!image || !parameters)
		return InvalidParameter;
	LONG flag = save_flag(parameters);
	if (!image->saving)
		return Win32Error;
	if (flag == SaveFlush) {
		HRESULT result = IWICBitmapEncoder_Commit((IWICBitmapEncoder *)image->saving);
		end_saving(image);
		return SUCCEEDED(result) ? Ok : GenericError;
	}
	return flag == SaveFramePage || flag == SaveFrameTime || flag == SaveFrameResolution ? GdipSaveAddImage(image, image, parameters) : Win32Error;
}

GpStatus GDIPAPI GdipSaveAddImage(GpImage *image, GpImage *add, const void *parameters)
{
	if (!image || !add || !parameters)
		return InvalidParameter;
	LONG flag = save_flag(parameters);
	if (!image->saving || (flag != SaveFramePage && flag != SaveFrameTime && flag != SaveFrameResolution))
		return Win32Error;
	IWICImagingFactory *factory;
	if (FAILED(imaging_factory(&factory)))
		return GenericError;
	GpStatus status = write_frame(factory, image->saving, add);
	IWICImagingFactory_Release(factory);
	return status;
}

GpStatus GDIPAPI GdipGetEncoderParameterListSize(GpImage *image, const CLSID *clsid, UINT *out)
{
	if (!image || !clsid || !out)
		return InvalidParameter;
	return NotImplemented;
}

GpStatus GDIPAPI GdipGetEncoderParameterList(GpImage *image, const CLSID *clsid, UINT size, void *out)
{
	(void)size;
	if (!image || !clsid || !out)
		return InvalidParameter;
	return NotImplemented;
}

GpStatus GDIPAPI GdipGetImageThumbnail(GpImage *image, UINT width, UINT height, GpImage **out, void *abort, void *data)
{
	(void)abort;
	(void)data;
	if (!image || !out)
		return InvalidParameter;
	if (image->type != ImageTypeBitmap)
		return NotImplemented;
	if (!width)
		width = 120;
	if (!height)
		height = 120;
	GpStatus status = gp_bitmap_new(width, height, PixelFormat32bppPARGB, out);
	if (status != Ok)
		return status;
	(*out)->dpi_x = image->dpi_x;
	(*out)->dpi_y = image->dpi_y;
	for (UINT y = 0; y < height; y++)
		for (UINT x = 0; x < width; x++)
			gp_pixel_write(*out, x, y, gp_pixel_read(image, (UINT)((UINT64)x * image->width / width), (UINT)((UINT64)y * image->height / height)));
	return Ok;
}

GpStatus GDIPAPI GdipGetPropertyCount(GpImage *image, UINT *out)
{
	if (!image || !out)
		return InvalidParameter;
	*out = image->property_count;
	return Ok;
}

GpStatus GDIPAPI GdipGetPropertyIdList(GpImage *image, UINT count, PROPID *out)
{
	if (!image || !out)
		return InvalidParameter;
	if (count != image->property_count)
		return InvalidParameter;
	for (UINT i = 0; i < count; i++)
		out[i] = image->properties[i].id;
	return Ok;
}

static const PropertyItem *find_property(GpImage *image, PROPID id)
{
	for (UINT i = 0; i < image->property_count; i++) {
		if (image->properties[i].id == id)
			return &image->properties[i];
	}
	return NULL;
}

GpStatus GDIPAPI GdipGetPropertyItemSize(GpImage *image, PROPID id, UINT *out)
{
	if (!image || !out)
		return InvalidParameter;
	const PropertyItem *item = find_property(image, id);
	if (!item) {
		*out = 0;
		return PropertyNotFound;
	}
	*out = sizeof(PropertyItem) + item->length;
	return Ok;
}

GpStatus GDIPAPI GdipGetPropertyItem(GpImage *image, PROPID id, UINT size, PropertyItem *out)
{
	if (!image || !out)
		return InvalidParameter;
	const PropertyItem *item = find_property(image, id);
	if (!item)
		return PropertyNotFound;
	if (size != sizeof(PropertyItem) + item->length)
		return InvalidParameter;
	*out = *item;
	out->value = out + 1;
	memcpy(out->value, item->value, item->length);
	return Ok;
}

GpStatus GDIPAPI GdipGetPropertySize(GpImage *image, UINT *total, UINT *count)
{
	if (!image || !total || !count)
		return InvalidParameter;
	UINT sum = image->property_count * sizeof(PropertyItem);
	for (UINT i = 0; i < image->property_count; i++)
		sum += image->properties[i].length;
	*total = sum;
	*count = image->property_count;
	return Ok;
}

GpStatus GDIPAPI GdipGetAllPropertyItems(GpImage *image, UINT size, UINT count, PropertyItem *out)
{
	if (!image || !out)
		return InvalidParameter;
	UINT total, have;
	GdipGetPropertySize(image, &total, &have);
	if (size != total || count != have)
		return InvalidParameter;
	if (!have)
		return GenericError;
	BYTE *at = (BYTE *)(out + have);
	for (UINT i = 0; i < have; i++) {
		out[i] = image->properties[i];
		out[i].value = at;
		memcpy(at, image->properties[i].value, image->properties[i].length);
		at += image->properties[i].length;
	}
	return Ok;
}

GpStatus GDIPAPI GdipRemovePropertyItem(GpImage *image, PROPID id)
{
	if (!image)
		return InvalidParameter;
	for (UINT i = 0; i < image->property_count; i++) {
		if (image->properties[i].id == id) {
			gp_free(image->properties[i].value);
			image->properties[i] = image->properties[--image->property_count];
			return Ok;
		}
	}
	return PropertyNotFound;
}

GpStatus GDIPAPI GdipSetPropertyItem(GpImage *image, const PropertyItem *item)
{
	if (!image || !item)
		return InvalidParameter;
	return gp_property_set(image, item->id, item->type, item->length, item->value);
}

GpStatus GDIPAPI GdipBitmapGetHistogramSize(INT format, UINT *out)
{
	if (!out)
		return InvalidParameter;
	*out = 256;
	return Ok;
}

GpStatus GDIPAPI GdipBitmapGetHistogram(GpImage *image, INT format, UINT count, UINT *c0, UINT *c1, UINT *c2, UINT *c3)
{
	enum { argb, pargb, rgb, gray, blue, green, red, alpha };
	if (!image || count != 256 || format < 0 || format > alpha)
		return InvalidParameter;
	UINT *channels[4] = {c0, c1, c2, c3};
	INT needed = format <= pargb ? 4 : format == rgb ? 3 : 1;
	for (INT i = 0; i < 4; i++) {
		if ((i < needed) != (channels[i] != NULL))
			return InvalidParameter;
		if (channels[i])
			memset(channels[i], 0, 256 * sizeof(UINT));
	}
	for (UINT y = 0; y < image->height; y++) {
		for (UINT x = 0; x < image->width; x++) {
			ARGB c = gp_pixel_read(image, x, y);
			if (format == pargb)
				c = premultiply(c);
			BYTE a = c >> 24, r = (BYTE)(c >> 16), g = (BYTE)(c >> 8), b = (BYTE)c;
			switch (format) {
			case argb:
			case pargb:
				c0[a]++; c1[r]++; c2[g]++; c3[b]++;
				break;
			case rgb:
				c0[r]++; c1[g]++; c2[b]++;
				break;
			case gray:
				c0[(r * 76 + g * 150 + b * 29) >> 8]++;
				break;
			case blue: c0[b]++; break;
			case green: c0[g]++; break;
			case red: c0[r]++; break;
			default: c0[a]++; break;
			}
		}
	}
	return Ok;
}

GpStatus GDIPAPI GdipCreateBitmapFromHBITMAP(HBITMAP handle, HPALETTE palette, GpImage **out)
{
	(void)palette;
	if (!handle || !out)
		return InvalidParameter;
	DIBSECTION section;
	memset(&section, 0, sizeof(section));
	INT got = GetObjectA(handle, sizeof(section), &section);
	if (got != sizeof(BITMAP) && got != sizeof(DIBSECTION))
		return InvalidParameter;
	const BITMAP info = section.dsBm;
	PixelFormat format = info.bmBitsPixel == 1 ? PixelFormat1bppIndexed : info.bmBitsPixel == 4 ? PixelFormat4bppIndexed
		: info.bmBitsPixel == 8 ? PixelFormat8bppIndexed
		: info.bmBitsPixel == 16 ? (section.dsBitfields[0] == 0xf800 ? PixelFormat16bppRGB565 : PixelFormat16bppRGB555)
		: info.bmBitsPixel == 24 ? PixelFormat24bppRGB : PixelFormat32bppRGB;
	GpStatus status = gp_bitmap_new(info.bmWidth, info.bmHeight, format, out);
	if (status != Ok)
		return status;
	// Indexed sections keep their own indices and colour table: going through colours could not bring them back.
	if (FORMAT_INDEXED(format) && got == sizeof(DIBSECTION) && info.bmBits) {
		BOOL bottom_up = section.dsBmih.biHeight > 0;
		for (INT y = 0; y < info.bmHeight; y++)
			memcpy((*out)->bits + (SSIZE_T)y * (*out)->stride, (BYTE *)info.bmBits + (SSIZE_T)(bottom_up ? info.bmHeight - 1 - y : y) * info.bmWidthBytes, stride_of(info.bmWidth, format));
		HDC table_dc = CreateCompatibleDC(NULL);
		HGDIOBJ table_old = table_dc ? SelectObject(table_dc, handle) : NULL;
		RGBQUAD colors[256];
		UINT count = table_old ? GetDIBColorTable(table_dc, 0, 256, colors) : 0;
		for (UINT i = 0; i < count && i < (*out)->palette->Count; i++)
			(*out)->palette->Entries[i] = 0xff000000 | *(const DWORD *)&colors[i];
		if (table_old)
			SelectObject(table_dc, table_old);
		if (table_dc)
			DeleteDC(table_dc);
		return Ok;
	}
	BITMAPINFO header = {{sizeof(BITMAPINFOHEADER), info.bmWidth, -info.bmHeight, 1, 32, BI_RGB}};
	DWORD *pixels = gp_alloc((SIZE_T)info.bmWidth * info.bmHeight * 4);
	HDC dc = CreateCompatibleDC(NULL);
	if (pixels && dc && GetDIBits(dc, handle, 0, info.bmHeight, pixels, &header, DIB_RGB_COLORS)) {
		for (INT y = 0; y < info.bmHeight; y++)
			for (INT x = 0; x < info.bmWidth; x++)
				gp_pixel_write(*out, x, y, pixels[(SIZE_T)y * info.bmWidth + x] | 0xff000000);
	}
	if (dc)
		DeleteDC(dc);
	gp_free(pixels);
	return Ok;
}

GpStatus GDIPAPI GdipCreateHBITMAPFromBitmap(GpImage *image, HBITMAP *out, ARGB background)
{
	if (!image || !out || image->type != ImageTypeBitmap)
		return InvalidParameter;
	// The result is a bottom-up section whose alpha stays that of the image, over the background's colour.
	BITMAPINFO header = {{sizeof(BITMAPINFOHEADER), (LONG)image->width, (LONG)image->height, 1, 32, BI_RGB}};
	void *bits;
	HBITMAP handle = CreateDIBSection(NULL, &header, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!handle)
		return GenericError;
	UINT back_r = (background >> 16) & 255, back_g = (background >> 8) & 255, back_b = background & 255;
	for (UINT y = 0; y < image->height; y++) {
		DWORD *to = (DWORD *)bits + (SIZE_T)(image->height - 1 - y) * image->width;
		for (UINT x = 0; x < image->width; x++) {
			ARGB c = gp_pixel_read(image, x, y);
			UINT a = c >> 24, inverse = 255 - a;
			UINT r = (((c >> 16) & 255) * a + back_r * inverse + 127) / 255;
			UINT g = (((c >> 8) & 255) * a + back_g * inverse + 127) / 255;
			UINT b = ((c & 255) * a + back_b * inverse + 127) / 255;
			*to++ = a << 24 | r << 16 | g << 8 | b;
		}
	}
	*out = handle;
	return Ok;
}

GpStatus GDIPAPI GdipCreateBitmapFromHICON(HICON icon, GpImage **out)
{
	if (!icon || !out)
		return InvalidParameter;
	ICONINFO info;
	if (!GetIconInfo(icon, &info))
		return GenericError;
	GpImage *color = NULL;
	GpStatus status = info.hbmColor ? GdipCreateBitmapFromHBITMAP(info.hbmColor, NULL, &color) : InvalidParameter;
	if (status == Ok) {
		status = GdipCloneBitmapAreaI(0, 0, color->width, color->height, PixelFormat32bppARGB, color, out);
		gp_image_release(color);
	}
	if (info.hbmColor)
		DeleteObject(info.hbmColor);
	if (info.hbmMask)
		DeleteObject(info.hbmMask);
	return status;
}

GpStatus GDIPAPI GdipCreateHICONFromBitmap(GpImage *image, HICON *out)
{
	if (!image || !out)
		return InvalidParameter;
	HBITMAP color;
	GpStatus status = GdipCreateHBITMAPFromBitmap(image, &color, 0);
	if (status != Ok)
		return status;
	ICONINFO info = {TRUE, 0, 0, CreateBitmap(image->width, image->height, 1, 1, NULL), color};
	*out = CreateIconIndirect(&info);
	DeleteObject(info.hbmMask);
	DeleteObject(color);
	return *out ? Ok : GenericError;
}

GpStatus GDIPAPI GdipCreateBitmapFromGdiDib(const BITMAPINFO *info, void *bits, GpImage **out)
{
	if (!info || !bits || !out || info->bmiHeader.biSize < sizeof(BITMAPINFOHEADER))
		return InvalidParameter;
	INT width = info->bmiHeader.biWidth, height = info->bmiHeader.biHeight;
	BOOL top_down = height < 0;
	if (top_down)
		height = -height;
	PixelFormat format;
	switch (info->bmiHeader.biBitCount) {
	case 1: format = PixelFormat1bppIndexed; break;
	case 4: format = PixelFormat4bppIndexed; break;
	case 8: format = PixelFormat8bppIndexed; break;
	case 16:
		// The masks follow the core header, or lie inside the larger ones at the same place.
		format = info->bmiHeader.biCompression == BI_BITFIELDS && *(const DWORD *)(&info->bmiHeader + 1) == 0xf800 ? PixelFormat16bppRGB565 : PixelFormat16bppRGB555;
		break;
	case 24: format = PixelFormat24bppRGB; break;
	case 32: format = PixelFormat32bppRGB; break;
	default: return InvalidParameter;
	}
	// The bitmap wraps the program's pixels, in the row order they have.
	INT stride = stride_of(width, format);
	BYTE *first = top_down ? bits : (BYTE *)bits + (SSIZE_T)(height - 1) * stride;
	GpStatus status = GdipCreateBitmapFromScan0(width, height, top_down ? stride : -stride, format, first, out);
	if (status != Ok)
		return status;
	if (FORMAT_INDEXED(format)) {
		UINT used = info->bmiHeader.biClrUsed ? info->bmiHeader.biClrUsed : 1u << info->bmiHeader.biBitCount;
		for (UINT i = 0; i < used && i < (*out)->palette->Count; i++)
			(*out)->palette->Entries[i] = 0xff000000 | *(const DWORD *)&info->bmiColors[i];
	}
	return Ok;
}

GpStatus GDIPAPI GdipCreateBitmapFromResource(HINSTANCE module, const WCHAR *name, GpImage **out)
{
	if (!module || !name || !out)
		return InvalidParameter;
	HBITMAP handle = LoadImageW(module, name, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
	if (!handle)
		return InvalidParameter;
	GpStatus status = GdipCreateBitmapFromHBITMAP(handle, NULL, out);
	DeleteObject(handle);
	return status;
}

GpStatus GDIPAPI GdipCreateBitmapFromDirectDrawSurface(void *surface, GpImage **out)
{
	(void)surface;
	return out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipInitializePalette(ColorPalette *palette, INT type, INT count, BOOL alpha, GpImage *image)
{
	enum { PaletteTypeCustom, PaletteTypeOptimal, PaletteTypeFixedBW };
	(void)alpha;
	if (!palette)
		return InvalidParameter;
	if (type == PaletteTypeCustom)
		return Ok;
	if (type == PaletteTypeOptimal) {
		if (!image || count <= 0)
			return InvalidParameter;
		return NotImplemented;
	}
	if (type == PaletteTypeFixedBW) {
		if (count < 2)
			return InvalidParameter;
		palette->Flags = 1;
		palette->Count = 2;
		palette->Entries[0] = 0xff000000;
		palette->Entries[1] = 0xffffffff;
		return Ok;
	}
	ColorPalette *base = default_palette(PixelFormat8bppIndexed);
	if (!base)
		return OutOfMemory;
	UINT used = count > 0 && (UINT)count < base->Count ? (UINT)count : base->Count;
	palette->Flags = 4;
	palette->Count = used;
	memcpy(palette->Entries, base->Entries, used * sizeof(ARGB));
	gp_free(base);
	return Ok;
}

GpStatus GDIPAPI GdipBitmapConvertFormat(GpImage *image, PixelFormat format, INT dither, INT palette_type, ColorPalette *palette, REAL alpha)
{
	(void)dither;
	(void)palette_type;
	(void)palette;
	(void)alpha;
	if (!image)
		return InvalidParameter;
	GpImage *made;
	GpStatus status = gp_bitmap_new(image->width, image->height, format, &made);
	if (status != Ok)
		return status;
	copy_area(made, 0, 0, image, 0, 0, image->width, image->height);
	if (image->owned)
		VirtualFree(image->owned, 0, MEM_RELEASE);
	gp_free(image->palette);
	image->owned = made->owned;
	image->bits = made->bits;
	image->stride = made->stride;
	image->palette = made->palette;
	image->format = format;
	made->owned = NULL;
	made->bits = NULL;
	made->palette = NULL;
	gp_image_release(made);
	return Ok;
}

GpStatus GDIPAPI GdipFindFirstImageItem(GpImage *image, void *item)
{
	return image && item ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipFindNextImageItem(GpImage *image, void *item)
{
	return image && item ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipGetImageItemData(GpImage *image, void *item)
{
	return image && item ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipCreateEffect(GUID guid, void **out)
{
	(void)guid;
	return out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipDeleteEffect(void *effect)
{
	return effect ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipGetEffectParameterSize(void *effect, UINT *out)
{
	return effect && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipGetEffectParameters(void *effect, UINT *size, void *out)
{
	return effect && size && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipSetEffectParameters(void *effect, const void *data, UINT size)
{
	(void)size;
	return effect && data ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipBitmapApplyEffect(GpImage *image, void *effect, RECT *roi, BOOL use_aux, void **aux, INT *aux_size)
{
	(void)roi;
	(void)use_aux;
	(void)aux;
	(void)aux_size;
	return image && effect ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipBitmapCreateApplyEffect(GpImage **inputs, INT count, void *effect, RECT *roi, RECT *out_rect, GpImage **out, BOOL use_aux, void **aux, INT *aux_size)
{
	(void)roi;
	(void)out_rect;
	(void)use_aux;
	(void)aux;
	(void)aux_size;
	return inputs && count > 0 && effect && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipTestControl(INT control, void *data)
{
	enum { TestControlForceBilinear, TestControlNoICM, TestControlGetBuildNumber };
	if ((UINT)control > TestControlGetBuildNumber)
		return InvalidParameter;
	if (control == TestControlGetBuildNumber)
		*(DWORD *)data = 4000;
	return Ok;
}
