/* SPDX-License-Identifier: MIT */
/* zlib streams through the zlib release the component is built with (scripts/provision-installer.py), which reads every stream RFC 1950
 * and 1951 allow. Wuffs (wuffs.c) refuses a few of them, such as a block of literals alone whose distance code has no codes: a PNG file
 * whose pixels Wuffs will not inflate comes back from here with them inflated and kept in stored blocks, which it reads. */
#include <zlib.h>

#include "decode.h"

static voidpf allocate(voidpf opaque, uInt items, uInt size)
{
	(void)opaque;
	return size && items > (uInt)-1 / size ? Z_NULL : wic_alloc((SIZE_T)items * size);
}

static void release(voidpf opaque, voidpf block)
{
	(void)opaque;
	wic_free(block);
}

/* The deflate stream inside a zlib stream, inflated into out until it ends; the checksum after it is not looked at, as the decoder the
   component had first never did, and pictures with wrong ones are read as before. */
static BOOL inflate_into(const BYTE *data, SIZE_T size, BYTE *out, SIZE_T room, SIZE_T *written)
{
	z_stream stream;
	int status = Z_OK;
	*written = 0;
	if (size < 2 || (data[0] & 15) != 8 || (data[0] >> 4) > 7 || (data[0] << 8 | data[1]) % 31 || (data[1] & 0x20))
		return FALSE;
	memset(&stream, 0, sizeof stream);
	stream.zalloc = allocate;
	stream.zfree = release;
	if (inflateInit2(&stream, -15) != Z_OK)
		return FALSE;
	data += 2;
	size -= 2;
	/* The counts of a stream are 32-bit: larger buffers go in slices. */
	while (status == Z_OK) {
		if (!stream.avail_in && size) {
			stream.next_in = (Bytef *)data;
			stream.avail_in = size > 0x40000000 ? 0x40000000 : (uInt)size;
			data += stream.avail_in;
			size -= stream.avail_in;
		}
		if (!stream.avail_out && room > *written) {
			stream.next_out = out + *written;
			stream.avail_out = room - *written > 0x40000000 ? 0x40000000 : (uInt)(room - *written);
			*written += stream.avail_out;
		}
		status = inflate(&stream, Z_NO_FLUSH);
	}
	*written -= stream.avail_out;
	inflateEnd(&stream);
	return status == Z_STREAM_END;
}

BYTE *zlib_inflate(const BYTE *data, SIZE_T size, SIZE_T room, SIZE_T *written)
{
	BYTE *out = room ? wic_alloc(room) : NULL;
	if (out && inflate_into(data, size, out, room, written))
		return out;
	wic_free(out);
	*written = 0;
	return NULL;
}

static UINT read_be32(const BYTE *data)
{
	return (UINT)data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
}

static void write_be32(BYTE *to, UINT value)
{
	to[0] = (BYTE)(value >> 24);
	to[1] = (BYTE)(value >> 16);
	to[2] = (BYTE)(value >> 8);
	to[3] = (BYTE)value;
}

/* The bytes the filtered rows of a picture take, each row after its filter byte, over the seven passes of an interlaced one; 0 when the
   header is not one of a picture. */
static UINT64 filtered_size(UINT width, UINT height, UINT depth, UINT kind, UINT interlace)
{
	static const BYTE start_x[7] = {0, 4, 0, 2, 0, 1, 0}, start_y[7] = {0, 0, 4, 0, 2, 0, 1}, step_x[7] = {8, 8, 4, 4, 2, 2, 1},
		step_y[7] = {8, 8, 8, 4, 4, 2, 2};
	const UINT channels = kind == 2 ? 3 : kind == 4 ? 2 : kind == 6 ? 4 : 1;
	UINT64 total = 0;
	if (!width || !height || !depth || depth > 16 || interlace > 1)
		return 0;
	for (UINT pass = 0; pass < (interlace ? 7u : 1u); ++pass) {
		const UINT64 columns = interlace ? (width > start_x[pass] ? (width - start_x[pass] + step_x[pass] - 1) / step_x[pass] : 0) : width;
		const UINT64 rows = interlace ? (height > start_y[pass] ? (height - start_y[pass] + step_y[pass] - 1) / step_y[pass] : 0) : height;
		if (columns && rows)
			total += rows * (1 + (columns * channels * depth + 7) / 8);
	}
	return total;
}

/* Puts a chunk of `size` bytes at `to`, with its checksum; returns the bytes the chunk takes. */
static SIZE_T put_chunk(BYTE *to, const char *name, const BYTE *body, UINT size)
{
	write_be32(to, size);
	memcpy(to + 4, name, 4);
	if (size)
		memcpy(to + 8, body, size);
	write_be32(to + 8 + size, (UINT)crc32(0, to + 4, size + 4));
	return 12 + (SIZE_T)size;
}

BYTE *png_restored(const BYTE *data, SIZE_T size, SIZE_T *restored_size)
{
	/* Stored blocks of at most 65535 bytes, each behind 5 bytes of header, in IDAT chunks of at most 2^30 bytes. */
	const UINT64 block = 65535, chunk_room = 0x40000000;
	SIZE_T at = 8, idat_size = 0, inflated_size = 0, out_at = 8;
	UINT64 filtered = 0, stream_size, chunks;
	BYTE *idat = NULL, *inflated = NULL, *out = NULL, *stream = NULL;
	BOOL placed = FALSE;
	*restored_size = 0;
	if (size < 33 || memcmp(data + 12, "IHDR", 4))
		return NULL;
	filtered = filtered_size(read_be32(data + 16), read_be32(data + 20), data[24], data[25], data[28]);
	if (!filtered || filtered > (SIZE_T)-1 / 2)
		return NULL;
	/* The pixel data of all IDAT chunks, in their order, is one zlib stream. */
	for (at = 8; at + 12 <= size && at + 12 + (SIZE_T)read_be32(data + at) <= size; at += 12 + (SIZE_T)read_be32(data + at))
		if (!memcmp(data + at + 4, "IDAT", 4))
			idat_size += read_be32(data + at);
	if (!idat_size || !(idat = wic_alloc(idat_size)))
		return NULL;
	idat_size = 0;
	for (at = 8; at + 12 <= size && at + 12 + (SIZE_T)read_be32(data + at) <= size; at += 12 + (SIZE_T)read_be32(data + at))
		if (!memcmp(data + at + 4, "IDAT", 4)) {
			memcpy(idat + idat_size, data + at + 8, read_be32(data + at));
			idat_size += read_be32(data + at);
		}
	inflated = zlib_inflate(idat, idat_size, (SIZE_T)filtered, &inflated_size);
	wic_free(idat);
	if (!inflated)
		return NULL;
	stream_size = 2 + (inflated_size / block + 1) * 5 + inflated_size + 4;
	chunks = (stream_size + chunk_room - 1) / chunk_room;
	if (stream_size > (SIZE_T)-1 / 2 || !(stream = wic_alloc((SIZE_T)stream_size)) ||
		!(out = wic_alloc(size + (SIZE_T)stream_size + (SIZE_T)chunks * 12))) {
		wic_free(stream);
		wic_free(inflated);
		return NULL;
	}
	/* The zlib stream: its header, the stored blocks, the checksum of what they hold. */
	stream[0] = 0x78;
	stream[1] = 0x01;
	stream_size = 2;
	for (SIZE_T from = 0;;) {
		const UINT length = (UINT)(inflated_size - from > block ? block : inflated_size - from);
		const BOOL last = from + length == inflated_size;
		stream[stream_size] = last ? 1 : 0;
		stream[stream_size + 1] = (BYTE)length;
		stream[stream_size + 2] = (BYTE)(length >> 8);
		stream[stream_size + 3] = (BYTE)~length;
		stream[stream_size + 4] = (BYTE)(~length >> 8);
		if (length)
			memcpy(stream + stream_size + 5, inflated + from, length);
		stream_size += 5 + (UINT64)length;
		from += length;
		if (last)
			break;
	}
	{
		uLong sum = adler32(0, Z_NULL, 0);
		for (SIZE_T from = 0; from < inflated_size;) {
			const uInt length = inflated_size - from > 0x40000000 ? 0x40000000 : (uInt)(inflated_size - from);
			sum = adler32(sum, inflated + from, length);
			from += length;
		}
		write_be32(stream + stream_size, (UINT)sum);
	}
	stream_size += 4;
	wic_free(inflated);
	/* The file again, with the new pixel data where the first IDAT chunk stood and every other chunk as it was. */
	memcpy(out, data, 8);
	for (at = 8; at + 12 <= size && at + 12 + (SIZE_T)read_be32(data + at) <= size; at += 12 + (SIZE_T)read_be32(data + at)) {
		const UINT length = read_be32(data + at);
		if (memcmp(data + at + 4, "IDAT", 4)) {
			memcpy(out + out_at, data + at, 12 + (SIZE_T)length);
			out_at += 12 + (SIZE_T)length;
			continue;
		}
		if (placed)
			continue;
		for (UINT64 from = 0; from < stream_size; from += chunk_room)
			out_at += put_chunk(out + out_at, "IDAT", stream + from, (UINT)(stream_size - from > chunk_room ? chunk_room : stream_size - from));
		placed = TRUE;
	}
	wic_free(stream);
	*restored_size = out_at;
	return out;
}
