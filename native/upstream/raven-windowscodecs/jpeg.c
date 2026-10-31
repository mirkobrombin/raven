/* SPDX-License-Identifier: MIT */
#include "decode.h"
#include <stdio.h>
#include <setjmp.h>
#include <jpeglib.h>

/* libjpeg reports an error by calling back, and only leaving the callback ends its work. */
typedef struct
{
	struct jpeg_error_mgr base;
	void *jump[5];
} error_exit;

static void on_error(j_common_ptr info)
{
	__builtin_longjmp(((error_exit *)info->err)->jump, 1);
}

static void on_message(j_common_ptr info, int level)
{
}

static void on_output(j_common_ptr info)
{
}

static void source_init(j_decompress_ptr info)
{
}

static boolean source_fill(j_decompress_ptr info)
{
	static const JOCTET end[2] = {0xff, JPEG_EOI};
	info->src->next_input_byte = end;
	info->src->bytes_in_buffer = 2;
	return TRUE;
}

static void source_skip(j_decompress_ptr info, long count)
{
	if (count > 0) {
		if ((size_t)count > info->src->bytes_in_buffer)
			source_fill(info);
		else {
			info->src->next_input_byte += count;
			info->src->bytes_in_buffer -= count;
		}
	}
}

static void source_term(j_decompress_ptr info)
{
}

HRESULT parse_jpeg(image_list *list, const BYTE *data, SIZE_T size)
{
	struct jpeg_decompress_struct info;
	struct jpeg_source_mgr source;
	error_exit errors;
	const GUID *format;
	volatile HRESULT result = WINCODEC_ERR_BADIMAGE;
	BYTE *pixels;
	UINT stride, y;
	info.err = jpeg_std_error(&errors.base);
	errors.base.error_exit = on_error;
	errors.base.emit_message = on_message;
	errors.base.output_message = on_output;
	if (__builtin_setjmp(errors.jump)) {
		jpeg_destroy_decompress(&info);
		return result;
	}
	jpeg_create_decompress(&info);
	source.next_input_byte = data;
	source.bytes_in_buffer = size;
	source.init_source = source_init;
	source.fill_input_buffer = source_fill;
	source.skip_input_data = source_skip;
	source.resync_to_restart = jpeg_resync_to_restart;
	source.term_source = source_term;
	info.src = &source;
	jpeg_read_header(&info, TRUE);
	if (info.jpeg_color_space == JCS_GRAYSCALE) {
		info.out_color_space = JCS_GRAYSCALE;
		format = &GUID_WICPixelFormat8bppGray;
	}
	else if (info.jpeg_color_space == JCS_CMYK || info.jpeg_color_space == JCS_YCCK) {
		info.out_color_space = JCS_CMYK;
		format = &GUID_WICPixelFormat32bppCMYK;
	}
	else {
		info.out_color_space = JCS_RGB;
		format = &GUID_WICPixelFormat24bppBGR;
	}
	jpeg_start_decompress(&info);
	if (!(pixels = image_add(list, info.output_width, info.output_height, format, &stride))) {
		result = E_OUTOFMEMORY;
		jpeg_destroy_decompress(&info);
		return result;
	}
	if (info.density_unit == 1 && info.X_density && info.Y_density)
		image_set_resolution(list, info.X_density, info.Y_density);
	else if (info.density_unit == 2 && info.X_density && info.Y_density)
		image_set_resolution(list, info.X_density * 2.54, info.Y_density * 2.54);
	for (y = 0; y < info.output_height; y++) {
		JSAMPROW row = pixels + (SIZE_T)y * stride;
		jpeg_read_scanlines(&info, &row, 1);
		if (format == &GUID_WICPixelFormat24bppBGR) {
			UINT x;
			for (x = 0; x < info.output_width; x++) {
				const BYTE swap = row[x * 3];
				row[x * 3] = row[x * 3 + 2];
				row[x * 3 + 2] = swap;
			}
		}
		else if (format == &GUID_WICPixelFormat32bppCMYK && info.saw_Adobe_marker) {
			/* Adobe stores these inverted. */
			UINT x;
			for (x = 0; x < info.output_width * 4; x++)
				row[x] = 255 - row[x];
		}
	}
	jpeg_finish_decompress(&info);
	jpeg_destroy_decompress(&info);
	return S_OK;
}

typedef struct
{
	struct jpeg_destination_mgr base;
	IStream *stream;
	JOCTET buffer[65536];
	HRESULT result;
} destination;

static void destination_init(j_compress_ptr info)
{
	destination *self = (destination *)info->dest;
	self->base.next_output_byte = self->buffer;
	self->base.free_in_buffer = sizeof(self->buffer);
}

static void destination_put(destination *self, SIZE_T size)
{
	ULONG written;
	if (SUCCEEDED(self->result))
		self->result = IStream_Write(self->stream, self->buffer, (ULONG)size, &written);
}

static boolean destination_empty(j_compress_ptr info)
{
	destination *self = (destination *)info->dest;
	destination_put(self, sizeof(self->buffer));
	destination_init(info);
	return TRUE;
}

static void destination_term(j_compress_ptr info)
{
	destination *self = (destination *)info->dest;
	destination_put(self, sizeof(self->buffer) - self->base.free_in_buffer);
}

HRESULT wic_write_jpeg(IStream *stream, UINT width, UINT height, const GUID *format, double dpi_x, double dpi_y, UINT stride, const BYTE *pixels)
{
	struct jpeg_compress_struct info;
	error_exit errors;
	destination *target = wic_alloc(sizeof(*target));
	BYTE *volatile line = wic_alloc((SIZE_T)width * 4);
	const BOOL gray = IsEqualGUID(format, &GUID_WICPixelFormat8bppGray), cmyk = IsEqualGUID(format, &GUID_WICPixelFormat32bppCMYK);
	HRESULT result;
	UINT y;
	if (!target || !line) {
		wic_free(target);
		wic_free(line);
		return E_OUTOFMEMORY;
	}
	info.err = jpeg_std_error(&errors.base);
	errors.base.error_exit = on_error;
	errors.base.emit_message = on_message;
	errors.base.output_message = on_output;
	if (__builtin_setjmp(errors.jump)) {
		jpeg_destroy_compress(&info);
		wic_free(target);
		wic_free(line);
		return WINCODEC_ERR_BADIMAGE;
	}
	jpeg_create_compress(&info);
	target->base.init_destination = destination_init;
	target->base.empty_output_buffer = destination_empty;
	target->base.term_destination = destination_term;
	target->stream = stream;
	info.dest = &target->base;
	info.image_width = width;
	info.image_height = height;
	info.input_components = gray ? 1 : cmyk ? 4 : 3;
	info.in_color_space = gray ? JCS_GRAYSCALE : cmyk ? JCS_CMYK : JCS_RGB;
	jpeg_set_defaults(&info);
	info.density_unit = 1;
	info.X_density = (UINT16)dpi_x;
	info.Y_density = (UINT16)dpi_y;
	jpeg_set_quality(&info, 90, TRUE);
	jpeg_start_compress(&info, TRUE);
	for (y = 0; y < height; y++) {
		JSAMPROW row = line;
		const BYTE *from = pixels + (SIZE_T)y * stride;
		UINT x;
		if (gray || cmyk)
			memcpy(line, from, gray ? width : (SIZE_T)width * 4);
		else
			for (x = 0; x < width; x++) {
				line[x * 3] = from[x * 3 + 2];
				line[x * 3 + 1] = from[x * 3 + 1];
				line[x * 3 + 2] = from[x * 3];
			}
		jpeg_write_scanlines(&info, &row, 1);
	}
	jpeg_finish_compress(&info);
	jpeg_destroy_compress(&info);
	result = target->result;
	wic_free(target);
	wic_free(line);
	return result;
}
