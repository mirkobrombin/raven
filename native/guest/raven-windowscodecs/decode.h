/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_WIC_DECODE_H
#define RAVEN_WIC_DECODE_H

#include "wic.h"

struct image_list
{
	IWICBitmap **frames;
	BYTE **profiles;
	UINT *profile_sizes;
	UINT count;
	WICColor colors[256];
	UINT color_count;
};

/* A decoder reads its whole file at once into frames held in memory: parsing only has to fill this list. */

BYTE *image_add(image_list *list, UINT width, UINT height, const GUID *format, UINT *stride);
void image_set_colors(image_list *list, const WICColor *colors, UINT count);
void image_set_profile(image_list *list, BYTE *profile, UINT size);
void image_set_resolution(image_list *list, double x, double y);

HRESULT parse_bmp(image_list *list, const BYTE *data, SIZE_T size);
HRESULT parse_png(image_list *list, const BYTE *data, SIZE_T size);
HRESULT parse_gif(image_list *list, const BYTE *data, SIZE_T size);
HRESULT parse_jpeg(image_list *list, const BYTE *data, SIZE_T size);
HRESULT parse_tiff(image_list *list, const BYTE *data, SIZE_T size);
HRESULT parse_ico(image_list *list, const BYTE *data, SIZE_T size);

/* The first image of a private list, in BGRA, and the end of that list. */
HRESULT image_list_first_as_bgra(image_list *list, IWICBitmapSource **out);
void image_list_init(image_list *list);
void image_list_clear(image_list *list);

/* The pixels of a PNG file whose header says width by height, decoded by the Wuffs release (wuffs.c): one 8-bit palette index each,
   after 1024 bytes of palette, for png_indexed; else blue, green, red and alpha, not premultiplied, of 8 bits each or of 16 bits each
   for png_wide. NULL when the file is broken or the pixels do not fit in memory; wic_free releases them. */
typedef enum { png_indexed, png_narrow, png_wide } png_layout;
BYTE *png_pixels(const BYTE *data, SIZE_T size, UINT width, UINT height, png_layout layout);
/* A zlib stream inflated into a new buffer of at most room bytes; NULL when the stream is broken or does not end within the room. */
BYTE *zlib_inflate(const BYTE *data, SIZE_T size, SIZE_T room, SIZE_T *written);

/* The part of a BMP after its file header, which an icon entry also is. */
HRESULT parse_dib(image_list *list, const BYTE *data, SIZE_T size, const BYTE *bits_at, BOOL double_height);

#endif
