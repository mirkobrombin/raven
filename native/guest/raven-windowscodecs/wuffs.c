/* SPDX-License-Identifier: MIT */
/* The pixels of PNG files and the codes of GIF images, through the decoders of the Wuffs release the component is built with
 * (scripts/provision-installer.py): memory-safe code that checks every bound, in place of hand-written inflating and unfiltering. Its
 * functions stay private to this file; the few it allocates with go to the component's heap. */
#include <stdlib.h>

#define calloc raven_wuffs_calloc
#define free raven_wuffs_free
static void *raven_wuffs_calloc(size_t count, size_t size);
static void raven_wuffs_free(void *block);

#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__STATIC_FUNCTIONS
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__LZW
#define WUFFS_CONFIG__MODULE__ZLIB
#define WUFFS_CONFIG__MODULE__PNG
#include "wuffs-v0.4.c"

#undef calloc
#undef free

#include "decode.h"

static void *raven_wuffs_calloc(size_t count, size_t size)
{
	if (size && count > (size_t)-1 / size)
		return NULL;
	return wic_alloc(count * size);
}

static void raven_wuffs_free(void *block)
{
	wic_free(block);
}

/* A buffer of `size` bytes, or NULL when the size does not fit the address space. */
static BYTE *allocate(UINT64 size)
{
	return size && size <= (SIZE_T)-1 ? wic_alloc((SIZE_T)size) : NULL;
}

/* The pixels as png_pixels gives them; `stream_refused` tells whether the deflate stream was what Wuffs turned down. */
static BYTE *decoded_pixels(const BYTE *data, SIZE_T size, UINT width, UINT height, png_layout layout, BOOL *stream_refused)
{
	const uint32_t format = layout == png_indexed ? WUFFS_BASE__PIXEL_FORMAT__INDEXED__BGRA_NONPREMUL :
		layout == png_wide ? WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL_4X16LE : WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL;
	const UINT64 pixel = layout == png_indexed ? 1 : layout == png_wide ? 8 : 4;
	const UINT64 palette = layout == png_indexed ? WUFFS_BASE__PIXEL_FORMAT__INDEXED__PALETTE_BYTE_LENGTH : 0;
	const size_t decoder_size = sizeof__wuffs_png__decoder();
	wuffs_png__decoder *decoder;
	wuffs_base__image_config config;
	wuffs_base__pixel_buffer buffer;
	wuffs_base__io_buffer source = wuffs_base__ptr_u8__reader((uint8_t *)data, size, true);
	wuffs_base__range_ii_u64 work_size;
	BYTE *memory = NULL, *work = NULL, *result = NULL;
	wuffs_base__status status;
	UINT64 bytes;
	*stream_refused = FALSE;
	if (!width || !height || (UINT64)width * height > (UINT64)-1 / 8)
		return NULL;
	bytes = (UINT64)width * height * pixel + palette;
	if (!(decoder = wic_alloc(decoder_size)))
		return NULL;
	if (wuffs_png__decoder__initialize(decoder, decoder_size, WUFFS_VERSION, WUFFS_INITIALIZE__ALREADY_ZEROED).repr)
		goto done;
	/* The decoder this one replaced never looked at the checksums of a file, and pictures with wrong ones are read as before. */
	if (wuffs_png__decoder__set_quirk(decoder, WUFFS_BASE__QUIRK_IGNORE_CHECKSUM, 1).repr)
		goto done;
	memset(&config, 0, sizeof config);
	if (wuffs_png__decoder__decode_image_config(decoder, &config, &source).repr ||
		wuffs_base__pixel_config__width(&config.pixcfg) != width || wuffs_base__pixel_config__height(&config.pixcfg) != height)
		goto done;
	wuffs_base__pixel_config__set(&config.pixcfg, format, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);
	work_size = wuffs_png__decoder__workbuf_len(decoder);
	if (!(memory = allocate(bytes)) || (work_size.max_incl && !(work = allocate(work_size.max_incl))))
		goto done;
	if (wuffs_base__pixel_buffer__set_from_slice(&buffer, &config.pixcfg, wuffs_base__make_slice_u8(memory, (size_t)bytes)).repr)
		goto done;
	status = wuffs_png__decoder__decode_frame(decoder, &buffer, &source, WUFFS_BASE__PIXEL_BLEND__SRC,
		wuffs_base__make_slice_u8(work, (size_t)work_size.max_incl), NULL);
	if (status.repr) {
		*stream_refused = !memcmp(status.repr, "#deflate: ", 10);
		goto done;
	}
	result = memory;
	memory = NULL;
done:
	wic_free(work);
	wic_free(memory);
	wic_free(decoder);
	return result;
}

BYTE *png_pixels(const BYTE *data, SIZE_T size, UINT width, UINT height, png_layout layout)
{
	BOOL stream_refused = FALSE;
	SIZE_T restored_size = 0;
	BYTE *pixels = decoded_pixels(data, size, width, height, layout, &stream_refused), *restored;
	if (pixels || !stream_refused)
		return pixels;
	/* A stream zlib reads but Wuffs does not comes back to Wuffs inflated (zlib.c). */
	if (!(restored = png_restored(data, size, &restored_size)))
		return NULL;
	pixels = decoded_pixels(restored, restored_size, width, height, layout, &stream_refused);
	wic_free(restored);
	return pixels;
}

SIZE_T lzw_expand(const BYTE *codes, SIZE_T size, UINT literal, BYTE *out, SIZE_T room)
{
	const size_t decoder_size = sizeof__wuffs_lzw__decoder();
	wuffs_lzw__decoder *decoder;
	wuffs_base__io_buffer source = wuffs_base__ptr_u8__reader((uint8_t *)codes, size, true), target = wuffs_base__ptr_u8__writer(out, room);
	if (!(decoder = wic_alloc(decoder_size)))
		return 0;
	/* Whatever ends the codes (a full image, the end code, a broken code or the end of the data), what was written stays. */
	if (!wuffs_lzw__decoder__initialize(decoder, decoder_size, WUFFS_VERSION, WUFFS_INITIALIZE__ALREADY_ZEROED).repr &&
		!wuffs_lzw__decoder__set_quirk(decoder, WUFFS_LZW__QUIRK_LITERAL_WIDTH_PLUS_ONE, literal + 1).repr)
		wuffs_lzw__decoder__transform_io(decoder, &target, &source, wuffs_base__empty_slice_u8());
	wic_free(decoder);
	return target.meta.wi;
}
