/* SPDX-License-Identifier: MIT */
#include "decode.h"
#include <tiffio.h>

/* libtiff reads through these callbacks from the file held in memory. */
typedef struct
{
	const BYTE *data;
	SIZE_T size, at;
} memory_file;

static tmsize_t file_read(thandle_t handle, void *to, tmsize_t size)
{
	memory_file *file = (memory_file *)handle;
	const SIZE_T left = file->at < file->size ? file->size - file->at : 0, count = (SIZE_T)size < left ? (SIZE_T)size : left;
	memcpy(to, file->data + file->at, count);
	file->at += count;
	return (tmsize_t)count;
}

static tmsize_t file_write(thandle_t handle, void *from, tmsize_t size)
{
	return 0;
}

static toff_t file_seek(thandle_t handle, toff_t place, int origin)
{
	memory_file *file = (memory_file *)handle;
	file->at = origin == SEEK_SET ? (SIZE_T)place : origin == SEEK_CUR ? file->at + (SIZE_T)place : file->size + (SIZE_T)place;
	return file->at;
}

static int file_close(thandle_t handle)
{
	return 0;
}

static toff_t file_size(thandle_t handle)
{
	return ((memory_file *)handle)->size;
}

static int file_map(thandle_t handle, void **base, toff_t *size)
{
	return 0;
}

static void file_unmap(thandle_t handle, void *base, toff_t size)
{
}

static void quiet(const char *module, const char *format, va_list arguments)
{
}

/* The stored rows of one directory, gathered from its strips or tiles, top row first. */
static BOOL read_rows(TIFF *file, BYTE *rows, UINT width, UINT height, UINT row_bytes, UINT pixel_bits)
{
	UINT32 tile_width = 0, tile_height = 0;
	if (TIFFGetField(file, TIFFTAG_TILEWIDTH, &tile_width) && TIFFGetField(file, TIFFTAG_TILELENGTH, &tile_height) && tile_width && tile_height) {
		const UINT tile_row = (tile_width * pixel_bits + 7) / 8;
		BYTE *tile = wic_alloc((SIZE_T)tile_row * tile_height);
		UINT number = 0, x, y, line;
		if (!tile)
			return FALSE;
		for (y = 0; y < height; y += tile_height)
			for (x = 0; x < width; x += tile_width, number++) {
				const UINT at = x * pixel_bits / 8, count = min(tile_row, row_bytes - at);
				if (TIFFReadEncodedTile(file, number, tile, (tmsize_t)tile_row * tile_height) < 0) {
					wic_free(tile);
					return FALSE;
				}
				for (line = 0; line < tile_height && y + line < height; line++)
					memcpy(rows + (SIZE_T)(y + line) * row_bytes + at, tile + (SIZE_T)line * tile_row, count);
			}
		wic_free(tile);
		return TRUE;
	}
	{
		UINT32 per_strip = height;
		UINT number = 0, y;
		TIFFGetField(file, TIFFTAG_ROWSPERSTRIP, &per_strip);
		if (!per_strip || per_strip > height)
			per_strip = height;
		for (y = 0; y < height; y += per_strip, number++) {
			const UINT lines = min(per_strip, height - y);
			if (TIFFReadEncodedStrip(file, number, rows + (SIZE_T)y * row_bytes, (tmsize_t)lines * row_bytes) < 0)
				return FALSE;
		}
	}
	return TRUE;
}

static HRESULT read_directory(TIFF *file, image_list *list)
{
	UINT32 width = 0, height = 0;
	UINT16 bits = 1, samples = 1, photometric = PHOTOMETRIC_MINISBLACK, planar = PLANARCONFIG_CONTIG, extra = 0, *extra_kinds = NULL, unit = RESUNIT_INCH;
	float x_resolution = 0, y_resolution = 0;
	const GUID *format;
	UINT stride, row_bytes, y, x, colors = 0;
	BYTE *pixels, *rows;
	BOOL swap_rgb = FALSE, invert = FALSE, premultiplied = FALSE, widen = FALSE, gray_alpha = FALSE;
	if (!TIFFGetField(file, TIFFTAG_IMAGEWIDTH, &width) || !TIFFGetField(file, TIFFTAG_IMAGELENGTH, &height) || !width || !height)
		return WINCODEC_ERR_BADIMAGE;
	TIFFGetField(file, TIFFTAG_BITSPERSAMPLE, &bits);
	TIFFGetField(file, TIFFTAG_SAMPLESPERPIXEL, &samples);
	TIFFGetField(file, TIFFTAG_PHOTOMETRIC, &photometric);
	TIFFGetField(file, TIFFTAG_PLANARCONFIG, &planar);
	TIFFGetField(file, TIFFTAG_EXTRASAMPLES, &extra, &extra_kinds);
	/* Separate planes are read for eight bit RGB only, one plane's strips after another. */
	if (planar != PLANARCONFIG_CONTIG && (photometric != PHOTOMETRIC_RGB || samples != 3 || bits != 8))
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	switch (photometric) {
	case PHOTOMETRIC_MINISWHITE:
		invert = TRUE;
		/* fall through */
	case PHOTOMETRIC_MINISBLACK:
		/* Gray with alpha has no format of its own, and reads as colour. */
		if (samples == 2 && bits == 8) {
			format = extra && extra_kinds && extra_kinds[0] == EXTRASAMPLE_ASSOCALPHA ? &GUID_WICPixelFormat32bppPBGRA : &GUID_WICPixelFormat32bppBGRA;
			gray_alpha = TRUE;
			break;
		}
		if (samples != 1)
			return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
		format = bits == 1 ? &GUID_WICPixelFormatBlackWhite : bits == 2 ? &GUID_WICPixelFormat2bppGray : bits == 4 ? &GUID_WICPixelFormat4bppGray
			: bits == 8 ? &GUID_WICPixelFormat8bppGray : bits == 16 ? &GUID_WICPixelFormat16bppGray : bits == 32 ? &GUID_WICPixelFormat32bppGrayFloat : NULL;
		break;
	case PHOTOMETRIC_PALETTE:
		if (samples != 1 || bits > 8)
			return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
		colors = 1U << bits;
		format = bits == 1 ? &GUID_WICPixelFormat1bppIndexed : bits == 2 ? &GUID_WICPixelFormat2bppIndexed : bits == 4 ? &GUID_WICPixelFormat4bppIndexed : &GUID_WICPixelFormat8bppIndexed;
		break;
	case PHOTOMETRIC_RGB:
		/* One and four bit samples read as eight; a tagged extra sample says the colours are already multiplied by it. */
		premultiplied = samples == 4 && extra && extra_kinds && extra_kinds[0] == EXTRASAMPLE_ASSOCALPHA;
		if (samples == 3)
			format = bits == 1 || bits == 4 || bits == 8 ? &GUID_WICPixelFormat24bppBGR : bits == 16 ? &GUID_WICPixelFormat48bppRGB
				: bits == 32 ? &GUID_WICPixelFormat96bppRGBFloat : NULL;
		else if (samples == 4 && premultiplied)
			format = bits == 1 || bits == 4 || bits == 8 ? &GUID_WICPixelFormat32bppPBGRA : bits == 16 ? &GUID_WICPixelFormat64bppPRGBA
				: bits == 32 ? &GUID_WICPixelFormat128bppPRGBAFloat : NULL;
		else if (samples == 4)
			format = bits == 1 || bits == 4 || bits == 8 ? &GUID_WICPixelFormat32bppBGRA : bits == 16 ? &GUID_WICPixelFormat64bppRGBA
				: bits == 32 ? &GUID_WICPixelFormat128bppRGBAFloat : NULL;
		else
			format = NULL;
		swap_rgb = bits == 1 || bits == 4 || bits == 8;
		widen = bits == 1 || bits == 4;
		break;
	case PHOTOMETRIC_SEPARATED:
		format = samples == 4 && bits == 8 ? &GUID_WICPixelFormat32bppCMYK : samples == 4 && bits == 16 ? &GUID_WICPixelFormat64bppCMYK : NULL;
		break;
	default:
		format = NULL;
		break;
	}
	if (!format)
		return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
	if (!(pixels = image_add(list, width, height, format, &stride)))
		return E_OUTOFMEMORY;
	if (TIFFGetField(file, TIFFTAG_XRESOLUTION, &x_resolution) && TIFFGetField(file, TIFFTAG_YRESOLUTION, &y_resolution) && x_resolution > 0 && y_resolution > 0) {
		const double scale = (TIFFGetField(file, TIFFTAG_RESOLUTIONUNIT, &unit), unit) == RESUNIT_CENTIMETER ? 2.54 : 1.0;
		image_set_resolution(list, x_resolution * scale, y_resolution * scale);
	}
	if (colors) {
		UINT16 *red, *green, *blue;
		if (TIFFGetField(file, TIFFTAG_COLORMAP, &red, &green, &blue)) {
			WICColor table[256];
			UINT index;
			for (index = 0; index < colors; index++)
				table[index] = 0xff000000 | (red[index] >> 8) << 16 | (green[index] >> 8) << 8 | blue[index] >> 8;
			image_set_colors(list, table, colors);
		}
	}
	row_bytes = (width * bits * samples + 7) / 8;
	if (!(rows = wic_alloc((SIZE_T)row_bytes * height)))
		return E_OUTOFMEMORY;
	if (planar != PLANARCONFIG_CONTIG) {
		UINT32 per_strip = height;
		const SIZE_T plane_size = (SIZE_T)width * height;
		BYTE *plane = wic_alloc(plane_size);
		UINT sample, number = 0;
		BOOL good = plane != NULL;
		TIFFGetField(file, TIFFTAG_ROWSPERSTRIP, &per_strip);
		if (!per_strip || per_strip > height)
			per_strip = height;
		for (sample = 0; good && sample < samples; sample++) {
			UINT line;
			SIZE_T at;
			for (line = 0; good && line < height; line += per_strip, number++)
				good = TIFFReadEncodedStrip(file, number, plane + (SIZE_T)line * width, (tmsize_t)min(per_strip, height - line) * width) >= 0;
			for (at = 0; good && at < plane_size; at++)
				rows[at * samples + sample] = plane[at];
		}
		wic_free(plane);
		if (!good) {
			wic_free(rows);
			return WINCODEC_ERR_BADIMAGE;
		}
	}
	else if (!read_rows(file, rows, width, height, row_bytes, (UINT)bits * samples)) {
		wic_free(rows);
		return WINCODEC_ERR_BADIMAGE;
	}
	for (y = 0; y < height; y++) {
		BYTE *from = rows + (SIZE_T)y * row_bytes, *to = pixels + (SIZE_T)y * stride;
		if (invert)
			for (x = 0; x < row_bytes; x++)
				from[x] = ~from[x];
		if (gray_alpha) {
			for (x = 0; x < width; x++) {
				to[x * 4] = to[x * 4 + 1] = to[x * 4 + 2] = from[x * 2];
				to[x * 4 + 3] = from[x * 2 + 1];
			}
			continue;
		}
		if (widen) {
			const UINT total = width * samples;
			for (x = 0; x < total; x++) {
				const UINT at = x * bits;
				const BYTE sample = (BYTE)(from[at / 8] >> (8 - bits - at % 8) & ((1U << bits) - 1));
				to[x] = (BYTE)(bits == 1 ? (sample ? 0xff : 0) : sample * 17);
			}
			if (swap_rgb)
				for (x = 0; x < width; x++) {
					const BYTE swap = to[x * samples];
					to[x * samples] = to[x * samples + 2];
					to[x * samples + 2] = swap;
				}
			continue;
		}
		if (swap_rgb)
			for (x = 0; x < width; x++) {
				const BYTE swap = from[x * samples];
				from[x * samples] = from[x * samples + 2];
				from[x * samples + 2] = swap;
			}
		memcpy(to, from, min(row_bytes, stride));
	}
	wic_free(rows);
	return S_OK;
}

HRESULT parse_tiff(image_list *list, const BYTE *data, SIZE_T size)
{
	memory_file source = {data, size, 0};
	TIFF *file;
	UINT count, index;
	HRESULT result = S_OK;
	TIFFSetErrorHandler(quiet);
	TIFFSetWarningHandler(quiet);
	if (!(file = TIFFClientOpen("memory", "rm", (thandle_t)&source, file_read, file_write, file_seek, file_close, file_size, file_map, file_unmap)))
		return WINCODEC_ERR_BADIMAGE;
	count = TIFFNumberOfDirectories(file);
	for (index = 0; index < count && SUCCEEDED(result); index++)
		if (TIFFSetDirectory(file, (UINT16)index)) {
			/* A reduced copy of another image is not a frame. */
			UINT32 kind = 0;
			if (TIFFGetField(file, TIFFTAG_SUBFILETYPE, &kind) && (kind & FILETYPE_REDUCEDIMAGE))
				continue;
			result = read_directory(file, list);
		}
	TIFFClose(file);
	if (SUCCEEDED(result) && !list->count)
		result = WINCODEC_ERR_BADIMAGE;
	return result;
}

/* An encoder's whole file grows in memory: libtiff seeks back to fill in its directories. */
struct wic_tiff_writer
{
	BYTE *data;
	SIZE_T size, used, at;
	TIFF *file;
};

static tmsize_t grow_read(thandle_t handle, void *to, tmsize_t size)
{
	wic_tiff_writer *self = (wic_tiff_writer *)handle;
	const SIZE_T left = self->at < self->used ? self->used - self->at : 0, count = (SIZE_T)size < left ? (SIZE_T)size : left;
	memcpy(to, self->data + self->at, count);
	self->at += count;
	return (tmsize_t)count;
}

static tmsize_t grow_write(thandle_t handle, void *from, tmsize_t size)
{
	wic_tiff_writer *self = (wic_tiff_writer *)handle;
	const SIZE_T end = self->at + (SIZE_T)size;
	if (end > self->size) {
		const SIZE_T wanted = end * 2 + 4096;
		BYTE *larger = wic_alloc(wanted);
		if (!larger)
			return 0;
		memcpy(larger, self->data, self->used);
		wic_free(self->data);
		self->data = larger;
		self->size = wanted;
	}
	memcpy(self->data + self->at, from, (SIZE_T)size);
	self->at = end;
	if (end > self->used)
		self->used = end;
	return size;
}

static toff_t grow_seek(thandle_t handle, toff_t place, int origin)
{
	wic_tiff_writer *self = (wic_tiff_writer *)handle;
	self->at = origin == SEEK_SET ? (SIZE_T)place : origin == SEEK_CUR ? self->at + (SIZE_T)place : self->used + (SIZE_T)place;
	return self->at;
}

static toff_t grow_size(thandle_t handle)
{
	return ((wic_tiff_writer *)handle)->used;
}

wic_tiff_writer *wic_tiff_begin(void)
{
	wic_tiff_writer *self = wic_alloc(sizeof(*self));
	if (!self)
		return NULL;
	TIFFSetErrorHandler(quiet);
	TIFFSetWarningHandler(quiet);
	self->file = TIFFClientOpen("memory", "wm", (thandle_t)self, grow_read, grow_write, grow_seek, file_close, grow_size, file_map, file_unmap);
	if (!self->file) {
		wic_free(self);
		return NULL;
	}
	return self;
}

HRESULT wic_tiff_frame(wic_tiff_writer *self, UINT width, UINT height, const GUID *format, double dpi_x, double dpi_y, const WICColor *colors, UINT color_count, UINT stride, const BYTE *pixels)
{
	const UINT bits = wic_format_bits(format);
	const BOOL indexed = IsEqualGUID(format, &GUID_WICPixelFormat1bppIndexed) || IsEqualGUID(format, &GUID_WICPixelFormat4bppIndexed) || IsEqualGUID(format, &GUID_WICPixelFormat8bppIndexed);
	const BOOL bgr = IsEqualGUID(format, &GUID_WICPixelFormat24bppBGR), bgra = IsEqualGUID(format, &GUID_WICPixelFormat32bppBGRA);
	const UINT samples = bgr ? 3 : bgra ? 4 : IsEqualGUID(format, &GUID_WICPixelFormat32bppCMYK) ? 4 : 1;
	const UINT row_bytes = (width * bits + 7) / 8;
	BYTE *line = wic_alloc(row_bytes);
	UINT y;
	if (!line)
		return E_OUTOFMEMORY;
	TIFFSetField(self->file, TIFFTAG_IMAGEWIDTH, width);
	TIFFSetField(self->file, TIFFTAG_IMAGELENGTH, height);
	TIFFSetField(self->file, TIFFTAG_BITSPERSAMPLE, bits / samples);
	TIFFSetField(self->file, TIFFTAG_SAMPLESPERPIXEL, samples);
	TIFFSetField(self->file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
	TIFFSetField(self->file, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
	TIFFSetField(self->file, TIFFTAG_ROWSPERSTRIP, height);
	TIFFSetField(self->file, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
	TIFFSetField(self->file, TIFFTAG_XRESOLUTION, (float)dpi_x);
	TIFFSetField(self->file, TIFFTAG_YRESOLUTION, (float)dpi_y);
	if (indexed) {
		static USHORT tables[3][256];
		const UINT count = 1U << bits;
		UINT index;
		for (index = 0; index < count; index++) {
			const WICColor color = index < color_count ? colors[index] : 0;
			tables[0][index] = (USHORT)((color >> 16 & 0xff) * 257);
			tables[1][index] = (USHORT)((color >> 8 & 0xff) * 257);
			tables[2][index] = (USHORT)((color & 0xff) * 257);
		}
		TIFFSetField(self->file, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_PALETTE);
		TIFFSetField(self->file, TIFFTAG_COLORMAP, tables[0], tables[1], tables[2]);
	}
	else if (bgr || bgra)
		TIFFSetField(self->file, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
	else if (samples == 4)
		TIFFSetField(self->file, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_SEPARATED);
	else
		TIFFSetField(self->file, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
	if (bgra) {
		const USHORT kind = EXTRASAMPLE_UNASSALPHA;
		TIFFSetField(self->file, TIFFTAG_EXTRASAMPLES, 1, &kind);
	}
	for (y = 0; y < height; y++) {
		memcpy(line, pixels + (SIZE_T)y * stride, row_bytes);
		if (bgr || bgra) {
			UINT x;
			for (x = 0; x < width; x++) {
				const BYTE swap = line[x * samples];
				line[x * samples] = line[x * samples + 2];
				line[x * samples + 2] = swap;
			}
		}
		if (TIFFWriteScanline(self->file, line, y, 0) < 0) {
			wic_free(line);
			return E_FAIL;
		}
	}
	wic_free(line);
	return TIFFWriteDirectory(self->file) ? S_OK : E_FAIL;
}

HRESULT wic_tiff_end(wic_tiff_writer *self, IStream *stream)
{
	ULONG written;
	HRESULT result;
	TIFFClose(self->file);
	result = IStream_Write(stream, self->data, (ULONG)self->used, &written);
	wic_free(self->data);
	wic_free(self);
	return result;
}
