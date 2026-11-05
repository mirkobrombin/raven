/* The PA19 patch file: a header naming the new file's size and CRC-32 and, for each old file it applies to, that file's size and CRC-32,
   the ranges of it to leave out of the comparison or to carry over unchanged, and an LZX DELTA stream; the streams follow the header in
   the same order, and four last bytes bring the CRC-32 of the whole file to 0xffffffff.

   Integers in the header are little-endian base-128 numbers whose last byte has the high bit set; the signed ones keep their sign in
   bit 6 of the first byte, which then carries six bits of the value. */

#include <stdlib.h>
#include <string.h>

#include "patch.h"

enum {
	signature = 0x39314150,
	option_large = 0x4,
	option_no_rebase = 0x00040000,
	/* For decoding it means the opposite: the header carries the new file's time. */
	option_no_timestamp = 0x00800000,
	option_extra_flags = 0x80000000,
	options_known = 0x1 | 0x2 | option_large | 0x00010000 | 0x00020000 | option_no_rebase | 0x00080000 | 0x00100000 | 0x00200000 | 0x00400000 |
		option_no_timestamp | option_extra_flags,
	ranges_limit = 255,
};

struct range
{
	size_t old_offset, new_offset, length;
};

struct source
{
	size_t size, stream_size;
	uint32_t crc;
	const unsigned char *stream;
	unsigned ignore_count, retain_count;
	struct range ignore[ranges_limit], retain[ranges_limit];
};

struct header
{
	uint32_t options, stamp;
	size_t new_size;
	uint32_t new_crc;
	unsigned count;
	struct source *sources;
};

struct cursor
{
	const unsigned char *at, *end;
	int failed;
};

uint32_t patch_crc32(uint32_t crc, const unsigned char *data, size_t size)
{
	static uint32_t table[256];
	if (!table[1])
		for (uint32_t index = 0; index < 256; index++) {
			uint32_t value = index;
			for (int bit = 0; bit < 8; bit++)
				value = value & 1 ? value >> 1 ^ 0xedb88320u : value >> 1;
			table[index] = value;
		}
	crc = ~crc;
	for (size_t index = 0; index < size; index++)
		crc = table[(crc ^ data[index]) & 0xff] ^ crc >> 8;
	return ~crc;
}

static uint32_t zeros_crc32(uint32_t crc, size_t size)
{
	static const unsigned char zeros[1024];
	while (size) {
		size_t run = size < sizeof(zeros) ? size : sizeof(zeros);
		crc = patch_crc32(crc, zeros, run);
		size -= run;
	}
	return crc;
}

static unsigned read_byte(struct cursor *cursor)
{
	if (cursor->at >= cursor->end) {
		cursor->failed = 1;
		return 0;
	}
	return *cursor->at++;
}

static uint32_t read_32(struct cursor *cursor)
{
	uint32_t value = 0;
	for (int shift = 0; shift < 32; shift += 8)
		value |= (uint32_t)read_byte(cursor) << shift;
	return value;
}

static uint64_t read_unsigned(struct cursor *cursor)
{
	uint64_t value = 0;
	for (int index = 0; index < 9; index++) {
		unsigned byte = read_byte(cursor);
		if (cursor->failed)
			return 0;
		value |= (uint64_t)(byte & 0x7f) << (7 * index);
		if (byte & 0x80)
			return value;
	}
	cursor->failed = 1;
	return 0;
}

static int64_t read_signed(struct cursor *cursor)
{
	unsigned byte = read_byte(cursor);
	int negative = (byte & 0x40) != 0;
	int64_t value = byte & 0x3f;
	for (int index = 1; !(byte & 0x80); index++) {
		if (cursor->failed || index == 9) {
			cursor->failed = 1;
			return 0;
		}
		byte = read_byte(cursor);
		value |= (int64_t)(byte & 0x7f) << (7 * index - 1);
	}
	return negative ? -value : value;
}

static int by_old_offset(const void *left, const void *right)
{
	const struct range *a = left, *b = right;
	return a->old_offset < b->old_offset ? -1 : a->old_offset > b->old_offset;
}

static int by_new_offset(const void *left, const void *right)
{
	const struct range *a = left, *b = right;
	return a->new_offset < b->new_offset ? -1 : a->new_offset > b->new_offset;
}

/* An offset moved by a signed delta, which must land inside a file of the given size together with the length after it. */
static int place(size_t base, int64_t delta, size_t length, size_t size, size_t *offset)
{
	if ((delta < 0 && (uint64_t)-delta > base) || (delta > 0 && (uint64_t)delta > size - base))
		return 0;
	*offset = (size_t)((int64_t)base + delta);
	return *offset <= size && length <= size - *offset;
}

static enum patch_status read_source(struct cursor *cursor, struct header *header, unsigned index)
{
	struct source *source = &header->sources[index];
	int64_t difference = read_signed(cursor);
	if ((difference < 0 && (uint64_t)-difference > header->new_size))
		return patch_corrupt;
	source->size = (size_t)((int64_t)header->new_size + difference);
	source->crc = read_32(cursor);

	source->ignore_count = read_byte(cursor);
	size_t previous = 0;
	for (unsigned range = 0; range < source->ignore_count && !cursor->failed; range++) {
		struct range *ignore = &source->ignore[range];
		int64_t delta = read_signed(cursor);
		ignore->length = (size_t)read_unsigned(cursor);
		if (!place(previous, delta, ignore->length, source->size, &ignore->old_offset))
			return patch_corrupt;
		previous = ignore->old_offset + ignore->length;
	}

	source->retain_count = read_byte(cursor);
	previous = 0;
	for (unsigned range = 0; range < source->retain_count && !cursor->failed; range++) {
		struct range *retain = &source->retain[range];
		int64_t delta = read_signed(cursor), moved = read_signed(cursor);
		retain->length = (size_t)read_unsigned(cursor);
		if (!place(previous, delta, retain->length, source->size, &retain->old_offset) ||
		    !place(retain->old_offset, moved, retain->length, header->new_size, &retain->new_offset))
			return patch_corrupt;
		previous = retain->old_offset + retain->length;
		/* Every old file leaves the same parts of the new one alone. */
		if (index && (retain->new_offset != header->sources[0].retain[range].new_offset ||
			retain->length != header->sources[0].retain[range].length))
			return patch_corrupt;
	}

	/* Pairs that undo the rewriting of 32-bit executables before they were compared; that rewriting is not done here. */
	if (read_unsigned(cursor))
		return patch_unsupported;
	source->stream_size = (size_t)read_unsigned(cursor);
	return cursor->failed ? patch_corrupt : patch_done;
}

static enum patch_status read_header(const unsigned char *patch, size_t size, struct header *header)
{
	struct cursor cursor = { patch, patch + size, 0 };
	if (read_32(&cursor) != signature)
		return patch_corrupt;
	header->options = read_32(&cursor);
	if (cursor.failed)
		return patch_corrupt;
	if (header->options & ~(uint32_t)options_known)
		return patch_unsupported;
	if (header->options & option_extra_flags)
		read_32(&cursor);
	if (header->options & option_no_timestamp)
		header->stamp = read_32(&cursor);
	/* The base a 32-bit executable was rebased to before it was compared. */
	if (!(header->options & option_no_rebase))
		read_unsigned(&cursor);
	header->new_size = (size_t)read_unsigned(&cursor);
	header->new_crc = read_32(&cursor);
	header->count = read_byte(&cursor);
	if (cursor.failed || !header->count)
		return patch_corrupt;
	header->sources = calloc(header->count, sizeof(*header->sources));
	if (!header->sources)
		return patch_no_memory;
	for (unsigned index = 0; index < header->count; index++) {
		enum patch_status status = read_source(&cursor, header, index);
		if (status != patch_done)
			return status;
	}
	/* A patch made to test only its header leaves the streams out; the CRC then covers what there is. */
	for (unsigned index = 0; index < header->count; index++) {
		struct source *source = &header->sources[index];
		qsort(source->ignore, source->ignore_count, sizeof(source->ignore[0]), by_old_offset);
		qsort(source->retain, source->retain_count, sizeof(source->retain[0]), by_old_offset);
		source->stream = cursor.at;
		cursor.at = source->stream_size > (size_t)(cursor.end - cursor.at) ? cursor.end : cursor.at + source->stream_size;
	}
	cursor.at = cursor.end - cursor.at < 4 ? cursor.end : cursor.at + 4;
	if (patch_crc32(0, patch, (size_t)(cursor.at - patch)) != 0xffffffffu)
		return patch_corrupt;
	return patch_done;
}

/* The CRC-32 an old file is known by, with its retained ranges and, when the patch has a stream for it, its ignored ones read as zeros. */
static uint32_t source_crc(const struct source *source, const unsigned char *old)
{
	struct range zeroed[2 * ranges_limit];
	unsigned count = 0;
	if (source->stream_size)
		for (unsigned index = 0; index < source->ignore_count; index++)
			zeroed[count++] = source->ignore[index];
	for (unsigned index = 0; index < source->retain_count; index++)
		zeroed[count++] = source->retain[index];
	qsort(zeroed, count, sizeof(zeroed[0]), by_old_offset);
	uint32_t crc = 0;
	size_t at = 0;
	for (unsigned index = 0; index < count; index++) {
		size_t start = zeroed[index].old_offset, end = start + zeroed[index].length;
		if (end <= at)
			continue;
		if (start > at) {
			crc = patch_crc32(crc, old + at, start - at);
			at = start;
		}
		crc = zeros_crc32(crc, end - at);
		at = end;
	}
	return patch_crc32(crc, old + at, source->size - at);
}

/* The new file's CRC-32 reads its retained ranges as zeros. */
static uint32_t target_crc(struct source *source, const unsigned char *output, size_t size)
{
	struct range retained[ranges_limit];
	memcpy(retained, source->retain, source->retain_count * sizeof(retained[0]));
	qsort(retained, source->retain_count, sizeof(retained[0]), by_new_offset);
	uint32_t crc = 0;
	size_t at = 0;
	for (unsigned index = 0; index < source->retain_count; index++) {
		size_t start = retained[index].new_offset, end = start + retained[index].length;
		if (end <= at)
			continue;
		if (start > at) {
			crc = patch_crc32(crc, output + at, start - at);
			at = start;
		}
		crc = zeros_crc32(crc, end - at);
		at = end;
	}
	return patch_crc32(crc, output + at, size - at);
}

static enum patch_status decode(struct header *header, struct source *source, const unsigned char *old, unsigned options,
	struct patch_result *result, lzxd_progress progress, void *context)
{
	size_t size = header->new_size;
	if (source->size > SIZE_MAX - size)
		return patch_no_memory;
	unsigned char *buffer = malloc(source->size + size + 1);
	if (!buffer)
		return patch_no_memory;
	if (source->size)
		memcpy(buffer, old, source->size);
	for (unsigned index = 0; index < source->ignore_count; index++)
		memset(buffer + source->ignore[index].old_offset, 0, source->ignore[index].length);
	for (unsigned index = 0; index < source->retain_count; index++)
		memset(buffer + source->retain[index].old_offset, 0, source->retain[index].length);

	enum patch_status status = patch_done;
	unsigned char *output = buffer + source->size;
	if (source->stream_size) {
		enum lzxd_status decoded = lzxd_decode(source->stream, source->stream_size, buffer, source->size, size,
			(header->options & option_large) != 0, progress, context);
		status = decoded == lzxd_cancelled ? patch_cancelled : decoded == lzxd_no_memory ? patch_no_memory :
			decoded == lzxd_done ? patch_done : patch_decode_failure;
	} else if (source->size == size) {
		memcpy(output, buffer, size);
	} else {
		status = patch_corrupt;
	}
	if (status == patch_done) {
		for (unsigned index = 0; index < source->retain_count; index++)
			memcpy(output + source->retain[index].new_offset, old + source->retain[index].old_offset, source->retain[index].length);
		if (target_crc(source, output, size) != header->new_crc)
			status = patch_corrupt;
		else if ((options & patch_fail_if_exact) && source->size == size && !memcmp(buffer, output, size))
			status = patch_not_necessary;
	}
	if (status != patch_done || (options & patch_test_only)) {
		free(buffer);
		return status;
	}
	memmove(buffer, output, size);
	result->output = buffer;
	return patch_done;
}

enum patch_status patch_apply(const unsigned char *patch, size_t patch_size, const unsigned char *old, size_t old_size, unsigned options,
	int header_only, struct patch_result *result, lzxd_progress progress, void *context)
{
	memset(result, 0, sizeof(*result));
	if (!old)
		old_size = 0;
	struct header header = { 0 };
	enum patch_status status = read_header(patch, patch_size, &header);
	if (status != patch_done)
		goto done;
	result->new_size = header.new_size;
	result->has_stamp = (header.options & option_no_timestamp) != 0;
	result->stamp = header.stamp;

	struct source *source = NULL;
	for (unsigned index = 0; index < header.count && !source; index++)
		if (header.sources[index].size == old_size && source_crc(&header.sources[index], old) == header.sources[index].crc)
			source = &header.sources[index];
	status = patch_wrong_file;
	if (!source)
		goto done;
	status = patch_not_necessary;
	if (!source->stream_size && (options & patch_fail_if_exact))
		goto done;
	/* Old files past the window come as streams interleaved with the new file, which this decoder does not take. */
	status = patch_unsupported;
	if (source->stream_size && source->size > lzxd_window_limit((header.options & option_large) != 0))
		goto done;
	status = patch_done;
	if (header_only)
		goto done;
	status = patch_not_available;
	if (source->stream_size > (size_t)(patch + patch_size - source->stream))
		goto done;
	status = decode(&header, source, old, options, result, progress, context);
done:
	free(header.sources);
	return status;
}
