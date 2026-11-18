/* The BMP, GIF, ICO and PNG decoders of windowscodecs.dll (native/guest/raven-windowscodecs), fed changed copies of the files named on the
   command line and random bytes, on the host under the address and undefined behaviour sanitizers. image_decoders.py builds and runs it.
   The frames a decoder adds are plain buffers here, so a decoder that writes or reads past them, or past the file, stops the run, and a
   frame it leaves behind is a leak the sanitizer reports at the end.

   image_decoders [rounds] [seed file]... */

#include <stdio.h>
#include <stdlib.h>

#include "decode.h"

const GUID GUID_WICPixelFormatBlackWhite = {1, 0}, GUID_WICPixelFormat1bppIndexed = {1, 1}, GUID_WICPixelFormat2bppIndexed = {2, 1},
	GUID_WICPixelFormat4bppIndexed = {4, 1}, GUID_WICPixelFormat8bppIndexed = {8, 1}, GUID_WICPixelFormat2bppGray = {2, 2},
	GUID_WICPixelFormat4bppGray = {4, 2}, GUID_WICPixelFormat8bppGray = {8, 2}, GUID_WICPixelFormat16bppGray = {16, 2},
	GUID_WICPixelFormat16bppBGR555 = {16, 3}, GUID_WICPixelFormat16bppBGR565 = {16, 4}, GUID_WICPixelFormat24bppBGR = {24, 3},
	GUID_WICPixelFormat24bppRGB = {24, 4}, GUID_WICPixelFormat32bppBGR = {32, 3}, GUID_WICPixelFormat32bppBGRA = {32, 4},
	GUID_WICPixelFormat32bppPBGRA = {32, 5}, GUID_WICPixelFormat32bppRGBA = {32, 6}, GUID_WICPixelFormat48bppRGB = {48, 3},
	GUID_WICPixelFormat48bppBGR = {48, 4}, GUID_WICPixelFormat64bppRGBA = {64, 3}, GUID_WICPixelFormat64bppBGRA = {64, 4};

static uint64_t state = 0x2545f4914f6cdd1dull;

static uint64_t next(void)
{
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	return state;
}

/* Zeroed, as the component's heap gives it (HEAP_ZERO_MEMORY), and refusing what a 32-bit program could not hold. */
void *wic_alloc(SIZE_T size)
{
	return size <= (SIZE_T)1 << 31 ? calloc(size ? size : 1, 1) : NULL;
}

void wic_free(void *block)
{
	free(block);
}

UINT wic_stride(UINT width, UINT bits)
{
	return (UINT)((((UINT64)width * bits + 31) / 32) * 4);
}

struct frame
{
	UINT width, height, stride;
	BYTE *pixels;
};

struct IWICBitmapSource
{
	UINT width, height;
};

BYTE *image_add(image_list *list, UINT width, UINT height, const GUID *format, UINT *stride)
{
	struct frame *frame;
	IWICBitmap **frames;
	/* Large pictures are left to the tests: here they would only take time. */
	if (!width || !height || (UINT64)width * height > 1u << 22 || !(frame = malloc(sizeof *frame)))
		return NULL;
	frame->width = width;
	frame->height = height;
	frame->stride = *stride = wic_stride(width, format->Data1);
	if (!(frame->pixels = calloc((SIZE_T)frame->stride * height, 1)) ||
		!(frames = realloc(list->frames, sizeof *frames * (list->count + 1)))) {
		free(frame->pixels);
		free(frame);
		return NULL;
	}
	list->frames = frames;
	list->frames[list->count++] = (IWICBitmap *)frame;
	return frame->pixels;
}

void image_set_colors(image_list *list, const WICColor *colors, UINT count)
{
	if (count > 256)
		abort();
	memcpy(list->colors, colors, sizeof *colors * count);
	list->color_count = count;
}

void image_set_profile(image_list *list, BYTE *profile, UINT size)
{
	(void)list;
	if (size)
		profile[size - 1] ^= 0;
	wic_free(profile);
}

void image_set_resolution(image_list *list, double x, double y)
{
	(void)list;
	(void)x;
	(void)y;
}

void image_list_init(image_list *list)
{
	memset(list, 0, sizeof *list);
}

void image_list_clear(image_list *list)
{
	for (UINT index = 0; index < list->count; ++index) {
		struct frame *frame = (struct frame *)list->frames[index];
		free(frame->pixels);
		free(frame);
	}
	free(list->frames);
	memset(list, 0, sizeof *list);
}

HRESULT image_list_first_as_bgra(image_list *list, IWICBitmapSource **out)
{
	const struct frame *frame;
	struct IWICBitmapSource *source;
	if (!list->count || !(source = malloc(sizeof *source)))
		return E_FAIL;
	frame = (const struct frame *)list->frames[0];
	source->width = frame->width;
	source->height = frame->height;
	*out = source;
	return S_OK;
}

HRESULT IWICBitmapSource_GetSize(IWICBitmapSource *source, UINT *width, UINT *height)
{
	*width = source->width;
	*height = source->height;
	return S_OK;
}

HRESULT IWICBitmapSource_CopyPixels(IWICBitmapSource *source, const WICRect *rect, UINT stride, UINT size, BYTE *pixels)
{
	if (rect || (UINT64)stride * source->height > size)
		return E_FAIL;
	memset(pixels, 0x55, (SIZE_T)stride * source->height);
	return S_OK;
}

ULONG IWICBitmapSource_Release(IWICBitmapSource *source)
{
	free(source);
	return 0;
}

/* A copy of `seed` with a few bytes changed: a bit, a byte, or a word set to a large or small number, as offsets and sizes go wrong. */
static BYTE *changed(const BYTE *seed, SIZE_T seed_size, SIZE_T *size)
{
	BYTE *data;
	*size = next() % 4 ? seed_size : next() % (seed_size + 1);
	if (!(data = malloc(*size ? *size : 1)))
		abort();
	memcpy(data, seed, *size);
	for (unsigned changes = 1 + next() % 8; *size && changes; --changes) {
		const SIZE_T at = next() % *size;
		const unsigned kind = next() % 3;
		if (kind == 0)
			data[at] ^= (BYTE)(1u << next() % 8);
		else if (kind == 1)
			data[at] = (BYTE)next();
		else if (at + 4 <= *size) {
			const uint32_t value = next() & 1 ? (uint32_t)next() : (uint32_t)-(int32_t)(next() % 64);
			memcpy(data + at, &value, 4);
		}
	}
	return data;
}

int main(int argc, char **argv)
{
	static HRESULT (*const parsers[])(image_list *, const BYTE *, SIZE_T) = {parse_bmp, parse_gif, parse_ico, parse_png};
	static BYTE seeds[32][1 << 16];
	static SIZE_T seed_sizes[32];
	const unsigned long rounds = argc > 1 ? strtoul(argv[1], NULL, 0) : 20000;
	int seed_count = 0;
	for (int index = 2; index < argc && seed_count < 32; ++index) {
		FILE *file = fopen(argv[index], "rb");
		if (!file) {
			fprintf(stderr, "cannot read %s\n", argv[index]);
			return 1;
		}
		seed_sizes[seed_count] = fread(seeds[seed_count], 1, sizeof seeds[0], file);
		fclose(file);
		++seed_count;
	}
	for (unsigned long round = 0; round < rounds; ++round) {
		SIZE_T size;
		BYTE *data;
		if (seed_count && next() % 8) {
			const int pick = (int)(next() % (uint64_t)seed_count);
			data = changed(seeds[pick], seed_sizes[pick], &size);
		} else {
			size = next() % 512;
			if (!(data = malloc(size ? size : 1)))
				abort();
			for (SIZE_T at = 0; at < size; ++at)
				data[at] = (BYTE)next();
		}
		for (size_t which = 0; which < sizeof parsers / sizeof parsers[0]; ++which) {
			image_list list;
			image_list_init(&list);
			parsers[which](&list, data, size);
			image_list_clear(&list);
		}
		free(data);
	}
	printf("%lu rounds\n", rounds);
	return 0;
}
