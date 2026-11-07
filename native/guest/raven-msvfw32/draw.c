/* SPDX-License-Identifier: MIT */
#include "vf.h"

/* DrawDib stretches a DIB onto a device. A compressed source passes through a decompressor to the depth DrawDib asks of it. */

typedef struct
{
	HIC decompressor;
	BITMAPINFOHEADER *format;
	BITMAPINFOHEADER *decoded;
	void *decoded_bits;
	HPALETTE palette;
	DWORD compression;
} drawing;

HDRAWDIB WINAPI DrawDibOpen(void)
{
	return vf_alloc(sizeof(drawing));
}

static DWORD colors_of(const BITMAPINFOHEADER *format)
{
	if (format->biClrUsed)
		return format->biClrUsed;
	return format->biBitCount <= 8 ? 1U << format->biBitCount : 0;
}

static void end_drawing(drawing *self)
{
	if (self->decompressor)
	{
		ICDecompressEnd(self->decompressor);
		ICClose(self->decompressor);
		self->decompressor = NULL;
	}
	vf_free(self->format);
	vf_free(self->decoded);
	vf_free(self->decoded_bits);
	self->format = NULL;
	self->decoded = NULL;
	self->decoded_bits = NULL;
	self->compression = 0;
}

BOOL WINAPI DrawDibEnd(HDRAWDIB handle)
{
	if (!handle)
		return FALSE;
	end_drawing(handle);
	return TRUE;
}

BOOL WINAPI DrawDibClose(HDRAWDIB handle)
{
	if (!handle)
		return FALSE;
	end_drawing(handle);
	vf_free(handle);
	return TRUE;
}

static BOOL valid_format(const BITMAPINFOHEADER *format)
{
	return format && format->biWidth > 0 && format->biHeight > 0;
}

static BOOL prepare_compressed(drawing *self, const BITMAPINFOHEADER *format)
{
	DWORD format_size = format->biSize + colors_of(format) * sizeof(RGBQUAD);
	if (self->decompressor && self->compression == format->biCompression && self->format && self->format->biWidth == format->biWidth && self->format->biHeight == format->biHeight)
		return TRUE;
	end_drawing(self);
	if (!(self->format = vf_alloc(format_size)) || !(self->decoded = vf_alloc(sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD))))
		return FALSE;
	memcpy(self->format, format, format_size);
	if (!(self->decompressor = ICGetDisplayFormat(NULL, self->format, self->decoded, 32, format->biWidth, format->biHeight)))
		return FALSE;
	if (ICDecompressBegin(self->decompressor, self->format, self->decoded) != ICERR_OK)
		return FALSE;
	self->compression = format->biCompression;
	return (self->decoded_bits = vf_alloc(self->decoded->biSizeImage)) != NULL;
}

BOOL WINAPI DrawDibBegin(HDRAWDIB handle, HDC context, int width, int height, BITMAPINFOHEADER *format, int source_width, int source_height, UINT flags)
{
	if (!handle || !valid_format(format))
		return FALSE;
	if (format->biCompression == BI_RGB || format->biCompression == BI_BITFIELDS)
		return TRUE;
	return prepare_compressed(handle, format);
}

BOOL WINAPI DrawDibDraw(HDRAWDIB handle, HDC context, int x, int y, int width, int height, BITMAPINFOHEADER *format, void *bits, int source_x, int source_y, int source_width, int source_height, UINT flags)
{
	drawing *self = handle;
	const BITMAPINFOHEADER *shown;
	const void *shown_bits = bits;
	if (!self || !valid_format(format))
		return FALSE;
	if (flags & DDF_UPDATE)
		return TRUE;
	if (format->biCompression != BI_RGB && format->biCompression != BI_BITFIELDS)
	{
		if (!prepare_compressed(self, format))
			return FALSE;
		if (bits && ICDecompress(self->decompressor, 0, format, bits, self->decoded, self->decoded_bits) != ICERR_OK)
			return FALSE;
		shown = self->decoded;
		shown_bits = self->decoded_bits;
	}
	else
		shown = format;
	if (!shown_bits)
		shown_bits = (const BYTE *)format + format->biSize + colors_of(format) * sizeof(RGBQUAD);
	if (width < 0)
		width = source_width;
	if (height < 0)
		height = source_height;
	return StretchDIBits(context, x, y, width, height, source_x, source_y, source_width, source_height, shown_bits, (const BITMAPINFO *)shown, DIB_RGB_COLORS, SRCCOPY) != GDI_ERROR;
}

void *WINAPI DrawDibGetBuffer(HDRAWDIB handle, BITMAPINFOHEADER *format, DWORD size, DWORD flags)
{
	drawing *self = handle;
	if (!self || !self->decoded)
		return NULL;
	if (format)
		memcpy(format, self->decoded, min(size, sizeof(BITMAPINFOHEADER)));
	return self->decoded_bits;
}

HPALETTE WINAPI DrawDibGetPalette(HDRAWDIB handle)
{
	return handle ? ((drawing *)handle)->palette : NULL;
}

BOOL WINAPI DrawDibSetPalette(HDRAWDIB handle, HPALETTE palette)
{
	if (!handle)
		return FALSE;
	((drawing *)handle)->palette = palette;
	return TRUE;
}

BOOL WINAPI DrawDibChangePalette(HDRAWDIB handle, int first, int count, PALETTEENTRY *colors)
{
	return handle != NULL;
}

UINT WINAPI DrawDibRealize(HDRAWDIB handle, HDC context, BOOL background)
{
	drawing *self = handle;
	HPALETTE previous;
	UINT mapped;
	if (!self || !self->palette)
		return 0;
	previous = SelectPalette(context, self->palette, background);
	mapped = RealizePalette(context);
	SelectPalette(context, previous, TRUE);
	return mapped;
}

BOOL WINAPI DrawDibStart(HDRAWDIB handle, DWORD rate)
{
	return handle != NULL;
}

BOOL WINAPI DrawDibStop(HDRAWDIB handle)
{
	return handle != NULL;
}

BOOL WINAPI DrawDibTime(HDRAWDIB handle, LPDRAWDIBTIME times)
{
	return FALSE;
}

LRESULT WINAPI DrawDibProfileDisplay(BITMAPINFOHEADER *format)
{
	return 0;
}
