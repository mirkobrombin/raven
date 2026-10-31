/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_IMAGE_H
#define RAVEN_IMAGE_H

#include "gdip.h"
#include <wincodec.h>

typedef INT PixelFormat;

enum
{
	PixelFormatUndefined = 0, PixelFormat1bppIndexed = 0x30101, PixelFormat4bppIndexed = 0x30402, PixelFormat8bppIndexed = 0x30803,
	PixelFormat16bppGrayScale = 0x101004, PixelFormat16bppRGB555 = 0x21005, PixelFormat16bppRGB565 = 0x21006, PixelFormat16bppARGB1555 = 0x61007,
	PixelFormat24bppRGB = 0x21808, PixelFormat32bppRGB = 0x22009, PixelFormat32bppARGB = 0x26200a, PixelFormat32bppPARGB = 0xe200b,
	PixelFormat48bppRGB = 0x10300c, PixelFormat64bppARGB = 0x34400d, PixelFormat64bppPARGB = 0x1c400e, PixelFormat32bppCMYK = 0x200f
};

#define FORMAT_BITS(format) (((format) >> 8) & 0xff)
#define FORMAT_INDEXED(format) (((format) & 0x10000) != 0)
#define FORMAT_ALPHA(format) (((format) & 0x40000) != 0)
#define FORMAT_PREMULTIPLIED(format) (((format) & 0x80000) != 0)

enum { ImageTypeUnknown, ImageTypeBitmap, ImageTypeMetafile };
enum { ImageLockModeRead = 1, ImageLockModeWrite = 2, ImageLockModeUserInputBuf = 4 };

typedef struct
{
	UINT Flags, Count;
	ARGB Entries[1];
} ColorPalette;

typedef struct
{
	UINT Width, Height;
	INT Stride;
	PixelFormat Format;
	void *Scan0;
	UINT_PTR Reserved;
} BitmapData;

typedef struct
{
	PROPID id;
	ULONG length;
	WORD type;
	void *value;
} PropertyItem;

struct IWICBitmapDecoder;

typedef struct gp_image
{
	INT type;
	LONG refs;
	UINT width, height;
	PixelFormat format;
	INT stride;
	BYTE *bits;
	BYTE *owned;
	ColorPalette *palette;
	REAL dpi_x, dpi_y;
	UINT flags;
	GUID raw_format;
	INT locked;
	// A lock whose format or rectangle differs works on a private copy, written back on unlock when opened for writing.
	BYTE *lock_copy;
	GpRect lock_rect;
	UINT lock_mode;
	// Multi-frame images stay open; selecting a frame decodes it over the pixels.
	void *decoder;
	void *saving;
	IStream *saving_stream;
	GUID dimension;
	UINT frame_count, frame_active;
	IStream *stream;
	PropertyItem *properties;
	UINT property_count;
	// Metafile side.
	HENHMETAFILE emf;
	GpRectF frame;
	INT frame_unit;
	void *recording;
} GpImage;

typedef GpImage GpBitmap;

typedef struct
{
	BOOL enabled, no_op;
	BOOL has_matrix, has_gray;
	REAL matrix[5][5], gray[5][5];
	INT matrix_flags;
	BOOL has_threshold, has_gamma, has_keys, has_channel, has_remap;
	REAL threshold, gamma;
	ARGB key_low, key_high;
	INT channel;
	void *remap;
	UINT remap_count;
} gp_adjust;

typedef struct gp_attributes
{
	gp_adjust adjust[5];
	GpWrapMode wrap;
	ARGB wrap_color;
	BOOL wrap_clamp;
} GpImageAttributes;

ARGB gp_pixel_read(const GpImage *image, INT x, INT y);
void gp_pixel_write(GpImage *image, INT x, INT y, ARGB color);
ARGB gp_attributes_apply(const GpImageAttributes *attributes, ARGB color);
GpStatus gp_bitmap_new(INT width, INT height, PixelFormat format, GpImage **out);
void gp_image_release(GpImage *image);

GpStatus GDIPAPI GdipDisposeImage(GpImage *image);
GpStatus GDIPAPI GdipCloneImage(GpImage *image, GpImage **out);
GpStatus GDIPAPI GdipCreateBitmapFromScan0(INT width, INT height, INT stride, PixelFormat format, BYTE *bits, GpImage **out);

GpStatus gp_property_set(GpImage *image, PROPID id, WORD type, ULONG length, const void *value);
void gp_property_clear(GpImage *image);
void gp_properties_load(GpImage *image, IWICBitmapDecoder *decoder, IWICBitmapFrameDecode *frame, IWICImagingFactory *factory);

#endif
