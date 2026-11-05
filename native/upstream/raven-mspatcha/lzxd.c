/* LZX DELTA as patches carry it ([MS-PATCH]): the LZX block format over a window that starts with the old file, cut into chunks of at
   most 32 KiB of output, each led by the byte count of its input. Within a chunk the bits are 16-bit little-endian words read from the
   most significant bit down. */

#include <stdlib.h>
#include <string.h>

#include "lzxd.h"

enum {
	literals = 256,
	slots_limit = 290,
	main_limit = literals + 8 * slots_limit,
	length_limit = 249,
	aligned_limit = 8,
	pretree_limit = 20,
	code_bits_limit = 16,
	fast_bits = 10,
	chunk_output = 32768,
	block_verbatim = 1,
	block_aligned = 2,
	block_raw = 3,
	/* Calls this close to a chunk's end are left as they are. */
	call_margin = 10,
};

struct code_table
{
	/* Codes up to fast_bits resolve in one step: symbol in the high bits and length in the low five. Longer ones walk the canonical ranges. */
	uint32_t fast[1 << fast_bits];
	uint32_t first_code[code_bits_limit + 2];
	uint16_t first_index[code_bits_limit + 2];
	uint16_t sorted[main_limit];
	int usable;
};

struct reader
{
	const unsigned char *start, *at, *chunk_end, *end;
	uint64_t bits;
	/* Unread bits in bits; the last padded of them are the zeros that stand in for words past the chunk's end. */
	int count, padded;
};

struct decoder
{
	struct reader reader;
	unsigned char *buffer;
	size_t position, limit, chunk_limit;
	uint32_t recent[3];
	unsigned main_size;
	uint32_t slot_base[slots_limit];
	unsigned char slot_extra[slots_limit];
	unsigned char main_lengths[main_limit], length_lengths[length_limit], aligned_lengths[aligned_limit];
	struct code_table main_code, length_code, aligned_code, pretree_code;
};

static void refill(struct reader *reader, int wanted)
{
	while (reader->count < wanted) {
		uint32_t word = 0;
		if (reader->chunk_end - reader->at >= 2) {
			word = (uint32_t)reader->at[0] | (uint32_t)reader->at[1] << 8;
			reader->at += 2;
		} else {
			reader->padded += 16;
		}
		reader->bits = reader->bits << 16 | word;
		reader->count += 16;
	}
}

static uint32_t peek_bits(struct reader *reader, int count)
{
	refill(reader, count);
	return (uint32_t)(reader->bits >> (reader->count - count)) & ((1u << count) - 1);
}

static uint32_t take_bits(struct reader *reader, int count)
{
	if (!count)
		return 0;
	uint32_t value = peek_bits(reader, count);
	reader->count -= count;
	return value;
}

/* Reading into the padding means the chunk ended before what was read from it. */
static int overrun(const struct reader *reader)
{
	return reader->count < reader->padded;
}

static int build_code(struct code_table *table, const unsigned char *lengths, unsigned symbols)
{
	uint32_t totals[code_bits_limit + 2] = { 0 };
	for (unsigned symbol = 0; symbol < symbols; symbol++)
		totals[lengths[symbol]]++;
	totals[0] = 0;
	uint32_t code = 0, index = 0;
	for (int length = 1; length <= code_bits_limit; length++) {
		table->first_code[length] = code;
		table->first_index[length] = (uint16_t)index;
		code = (code + totals[length]) << 1;
		index += totals[length];
	}
	table->first_code[code_bits_limit + 1] = code;
	table->first_index[code_bits_limit + 1] = (uint16_t)index;
	table->usable = index != 0;
	if (!index)
		return 1;
	if (code >> 1 > 1u << code_bits_limit)
		return 0;

	uint32_t next[code_bits_limit + 2];
	for (int length = 1; length <= code_bits_limit; length++)
		next[length] = table->first_index[length];
	memset(table->fast, 0, sizeof(table->fast));
	for (unsigned symbol = 0; symbol < symbols; symbol++) {
		unsigned length = lengths[symbol];
		if (!length)
			continue;
		uint32_t position = next[length]++;
		table->sorted[position] = (uint16_t)symbol;
		if (length > fast_bits)
			continue;
		uint32_t assigned = table->first_code[length] + position - table->first_index[length];
		uint32_t start = assigned << (fast_bits - length), span = 1u << (fast_bits - length);
		if (start + span > 1u << fast_bits)
			return 0;
		for (uint32_t entry = 0; entry < span; entry++)
			table->fast[start + entry] = symbol << 5 | length;
	}
	return 1;
}

static int read_symbol(struct reader *reader, const struct code_table *table)
{
	if (!table->usable)
		return -1;
	uint32_t window = peek_bits(reader, code_bits_limit);
	uint32_t fast = table->fast[window >> (code_bits_limit - fast_bits)];
	if (fast) {
		reader->count -= (int)(fast & 31);
		return (int)(fast >> 5);
	}
	for (int length = fast_bits + 1; length <= code_bits_limit; length++) {
		uint32_t code = window >> (code_bits_limit - length);
		uint32_t first = table->first_code[length];
		uint32_t total = table->first_index[length + 1] - table->first_index[length];
		if (code >= first && code - first < total) {
			reader->count -= length;
			return table->sorted[table->first_index[length] + code - first];
		}
	}
	return -1;
}

/* A tree's lengths travel as deltas against the previous block's, themselves coded through a small tree sent first. */
static int read_lengths(struct decoder *decoder, unsigned char *lengths, unsigned from, unsigned to)
{
	struct reader *reader = &decoder->reader;
	unsigned char pretree[pretree_limit];
	for (unsigned index = 0; index < pretree_limit; index++)
		pretree[index] = (unsigned char)take_bits(reader, 4);
	if (!build_code(&decoder->pretree_code, pretree, pretree_limit))
		return 0;
	for (unsigned index = from; index < to;) {
		int symbol = read_symbol(reader, &decoder->pretree_code);
		if (symbol < 0)
			return 0;
		if (symbol <= 16) {
			lengths[index] = (unsigned char)((lengths[index] + 17 - symbol) % 17);
			index++;
		} else if (symbol == 19) {
			unsigned run = 4 + take_bits(reader, 1);
			int delta = read_symbol(reader, &decoder->pretree_code);
			if (delta < 0 || delta > 16)
				return 0;
			unsigned char value = (unsigned char)((lengths[index] + 17 - delta) % 17);
			if (run > to - index)
				run = to - index;
			memset(lengths + index, value, run);
			index += run;
		} else {
			unsigned run = symbol == 17 ? 4 + take_bits(reader, 4) : 20 + take_bits(reader, 5);
			if (run > to - index)
				run = to - index;
			memset(lengths + index, 0, run);
			index += run;
		}
	}
	return 1;
}

/* The next chunk begins where this one's input ends, with its own input size; its output is the next 32 KiB at most. */
static int next_chunk(struct decoder *decoder)
{
	struct reader *reader = &decoder->reader;
	if (overrun(reader))
		return 0;
	if (reader->at < reader->chunk_end)
		reader->at = reader->chunk_end;
	if (reader->end - reader->at < 2)
		return 0;
	size_t size = (size_t)reader->at[0] | (size_t)reader->at[1] << 8;
	reader->at += 2;
	if ((size_t)(reader->end - reader->at) < size)
		return 0;
	reader->chunk_end = reader->at + size;
	reader->bits = 0;
	reader->count = reader->padded = 0;
	decoder->chunk_limit = decoder->limit - decoder->position < chunk_output ? decoder->limit : decoder->position + chunk_output;
	return 1;
}

/* An uncompressed block goes back to the word boundary first. A header that ends on one may be followed by a word of zeros the
   compressor puts there in some blocks and not in others, and the offsets that follow are never zero, which tells the two apart. */
static int raw_block(struct decoder *decoder, uint32_t size)
{
	struct reader *reader = &decoder->reader;
	int partial = reader->count % 16;
	if (partial)
		reader->count -= partial;
	else if (!peek_bits(reader, 16))
		reader->count -= 16;
	for (unsigned index = 0; index < 3; index++) {
		uint32_t low = take_bits(reader, 16);
		decoder->recent[index] = low | take_bits(reader, 16) << 16;
		if (!decoder->recent[index])
			return 0;
	}
	if (overrun(reader))
		return 0;
	/* The bytes follow from the stream itself: give back the words read ahead. */
	reader->at -= (reader->count - reader->padded) / 8;
	reader->bits = 0;
	reader->count = reader->padded = 0;
	if (size > decoder->limit - decoder->position)
		return 0;
	size_t end = decoder->position + size;
	int parity = (int)((reader->at - reader->start) & 1);
	for (;;) {
		size_t run = end - decoder->position;
		if (run > decoder->chunk_limit - decoder->position)
			run = decoder->chunk_limit - decoder->position;
		if (run > (size_t)(reader->end - reader->at))
			run = (size_t)(reader->end - reader->at);
		memcpy(decoder->buffer + decoder->position, reader->at, run);
		decoder->position += run;
		reader->at += run;
		if (decoder->position == end)
			break;
		/* Chunk sizes inside an uncompressed block lie wherever the bytes before them end. */
		if (decoder->position < decoder->chunk_limit)
			return 0;
		reader->chunk_end = reader->at;
		if (!next_chunk(decoder))
			return 0;
	}
	if ((int)((reader->at - reader->start) & 1) != parity)
		reader->at++;
	return 1;
}

static int read_offset(struct decoder *decoder, int block_type, unsigned slot, uint32_t *formatted)
{
	struct reader *reader = &decoder->reader;
	unsigned extra = decoder->slot_extra[slot];
	uint32_t footer;
	if (block_type == block_aligned && extra >= 3) {
		footer = take_bits(reader, (int)extra - 3) << 3;
		int aligned = read_symbol(reader, &decoder->aligned_code);
		if (aligned < 0)
			return 0;
		footer += (uint32_t)aligned;
	} else {
		footer = take_bits(reader, (int)extra);
	}
	*formatted = decoder->slot_base[slot] + footer;
	return 1;
}

/* Length 257 says the real one follows: a prefix of up to three bits picks how many bits it takes. */
static uint32_t long_length(struct reader *reader)
{
	if (!take_bits(reader, 1))
		return 0x101 + take_bits(reader, 8);
	if (!take_bits(reader, 1))
		return 0x201 + take_bits(reader, 10);
	if (!take_bits(reader, 1))
		return 0x601 + take_bits(reader, 12);
	return 0x101 + take_bits(reader, 15);
}

static int compressed_block(struct decoder *decoder, int block_type, uint32_t size)
{
	struct reader *reader = &decoder->reader;
	if (block_type == block_aligned) {
		for (unsigned index = 0; index < aligned_limit; index++)
			decoder->aligned_lengths[index] = (unsigned char)take_bits(reader, 3);
		if (!build_code(&decoder->aligned_code, decoder->aligned_lengths, aligned_limit))
			return 0;
	}
	if (!read_lengths(decoder, decoder->main_lengths, 0, literals) ||
	    !read_lengths(decoder, decoder->main_lengths, literals, decoder->main_size) ||
	    !read_lengths(decoder, decoder->length_lengths, 0, length_limit) ||
	    !build_code(&decoder->main_code, decoder->main_lengths, decoder->main_size) ||
	    !build_code(&decoder->length_code, decoder->length_lengths, length_limit))
		return 0;

	/* A match is cut at the end of its block. */
	size_t end = size > decoder->limit - decoder->position ? decoder->limit : decoder->position + size;
	unsigned char *buffer = decoder->buffer;
	while (decoder->position < end) {
		if (decoder->position >= decoder->chunk_limit && !next_chunk(decoder))
			return 0;
		int symbol = read_symbol(reader, &decoder->main_code);
		if (symbol < 0)
			return 0;
		if (symbol < literals) {
			buffer[decoder->position++] = (unsigned char)symbol;
			continue;
		}
		uint32_t element = (uint32_t)symbol - literals;
		unsigned slot = element >> 3;
		uint32_t length = (element & 7) + 2;
		if (length == 9) {
			int extra = read_symbol(reader, &decoder->length_code);
			if (extra < 0)
				return 0;
			length += (uint32_t)extra;
		}
		uint32_t offset;
		if (slot < 3) {
			offset = decoder->recent[slot];
			decoder->recent[slot] = decoder->recent[0];
		} else {
			uint32_t formatted;
			if (!read_offset(decoder, block_type, slot, &formatted))
				return 0;
			offset = formatted - 2;
			decoder->recent[2] = decoder->recent[1];
			decoder->recent[1] = decoder->recent[0];
		}
		decoder->recent[0] = offset;
		if (length == 257)
			length = long_length(reader);
		if (!offset)
			return 0;
		/* What lies before the window's start reads as zeros: the compressor saw the window that way. */
		while (length && offset > decoder->position && decoder->position < end) {
			buffer[decoder->position++] = 0;
			length--;
		}
		size_t stop = length > end - decoder->position ? end : decoder->position + length;
		for (size_t at = decoder->position; at < stop; at++)
			buffer[at] = buffer[at - offset];
		decoder->position = stop;
	}
	return !overrun(reader);
}

static int decode_block(struct decoder *decoder)
{
	struct reader *reader = &decoder->reader;
	if (decoder->position >= decoder->chunk_limit && !next_chunk(decoder))
		return 0;
	int block_type = (int)take_bits(reader, 3);
	uint32_t size = take_bits(reader, 24);
	if (block_type == block_raw)
		return raw_block(decoder, size);
	if (block_type != block_verbatim && block_type != block_aligned)
		return 0;
	return compressed_block(decoder, block_type, size);
}

/* The compressor rewrote x86 CALL targets as positions in the new file; each 32 KiB of output is turned back on its own. */
static void restore_calls(unsigned char *output, size_t size, uint32_t translation)
{
	size_t limit = size < (size_t)1 << 30 ? size : (size_t)1 << 30;
	for (size_t chunk = 0; chunk + call_margin < limit; chunk += chunk_output) {
		size_t stop = chunk + chunk_output - call_margin;
		if (stop > limit - call_margin)
			stop = limit - call_margin;
		for (size_t at = chunk; at < stop; at++) {
			if (output[at] != 0xe8)
				continue;
			int64_t target = (int32_t)((uint32_t)output[at + 1] | (uint32_t)output[at + 2] << 8 | (uint32_t)output[at + 3] << 16 |
				(uint32_t)output[at + 4] << 24);
			int64_t here = (int64_t)at;
			if (target >= -here && target < (int64_t)translation) {
				uint32_t value = (uint32_t)(target >= 0 ? target - here : target + translation);
				output[at + 1] = (unsigned char)value;
				output[at + 2] = (unsigned char)(value >> 8);
				output[at + 3] = (unsigned char)(value >> 16);
				output[at + 4] = (unsigned char)(value >> 24);
			}
			at += 4;
		}
	}
}

size_t lzxd_window_limit(int large)
{
	return (size_t)(large ? 32 : 8) << 20;
}

/* The stream does not say its window: it is the smallest one holding the old file, rounded up to a chunk, and the new file. */
static unsigned slot_count(size_t reference, size_t size, int large)
{
	size_t total = ((reference + chunk_output - 1) & ~(size_t)(chunk_output - 1)) + size;
	size_t window = (size_t)1 << 17, limit = lzxd_window_limit(large);
	unsigned slots = 34, step = 2;
	while (window < total && window < limit) {
		if (window >= (size_t)1 << 19)
			step = step == 2 ? 4 : step * 2;
		slots += step;
		window <<= 1;
	}
	return slots;
}

enum lzxd_status lzxd_decode(const unsigned char *input, size_t input_size, unsigned char *buffer, size_t reference, size_t size, int large,
	lzxd_progress progress, void *context)
{
	if (!input_size)
		return size ? lzxd_corrupt : lzxd_done;
	if (progress && !progress(context, 0, size))
		return lzxd_cancelled;
	struct decoder *decoder = calloc(1, sizeof(*decoder));
	if (!decoder)
		return lzxd_no_memory;
	decoder->buffer = buffer;
	decoder->position = reference;
	decoder->limit = reference + size;
	decoder->recent[0] = decoder->recent[1] = decoder->recent[2] = 1;
	unsigned slots = slot_count(reference, size, large);
	decoder->main_size = literals + 8 * slots;
	uint32_t base = 0;
	for (unsigned slot = 0; slot < slots; slot++) {
		unsigned extra = slot < 4 ? 0 : (slot - 2) / 2;
		decoder->slot_extra[slot] = (unsigned char)(extra > 17 ? 17 : extra);
		decoder->slot_base[slot] = base;
		base += 1u << decoder->slot_extra[slot];
	}
	struct reader *reader = &decoder->reader;
	reader->start = reader->at = reader->chunk_end = input;
	reader->end = input + input_size;

	enum lzxd_status status = lzxd_corrupt;
	if (!next_chunk(decoder))
		goto done;
	uint32_t translation = 0;
	int translated = (int)take_bits(reader, 1);
	if (translated) {
		uint32_t high = take_bits(reader, 16);
		translation = high << 16 | take_bits(reader, 16);
	}
	while (decoder->position < decoder->limit) {
		if (!decode_block(decoder))
			goto done;
		if (progress && !progress(context, decoder->position - reference, size)) {
			status = lzxd_cancelled;
			goto done;
		}
	}
	if (translated)
		restore_calls(buffer + reference, size, translation);
	status = lzxd_done;
done:
	free(decoder);
	return status;
}
