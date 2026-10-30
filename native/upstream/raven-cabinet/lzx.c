/* LZX as cabinets carry it: the Microsoft LZX Data Compression Format specification, with one output frame per cabinet block. The bit stream is
   16-bit little-endian words read from the most significant bit down, and every block starts on a fresh word. */

#include <string.h>

#include "format.h"

enum {
	main_literals = 256,
	main_limit = main_literals + 8 * 50,
	length_limit = 249,
	aligned_limit = 8,
	pretree_limit = 20,
	code_bits_limit = 16,
	fast_bits = 10,
	block_verbatim = 1,
	block_aligned = 2,
	block_raw = 3,
};

struct code_table
{
	/* Codes up to fast_bits resolve in one step: symbol in the high bits and length in the low four. Longer ones walk the canonical ranges. */
	uint16_t fast[1 << fast_bits];
	uint32_t first_code[code_bits_limit + 2];
	uint16_t first_index[code_bits_limit + 2];
	uint16_t sorted[main_limit];
	int usable;
};

struct bit_reader
{
	const unsigned char *data, *end;
	uint32_t bits;
	int count;
};

struct lzx_decoder
{
	unsigned char *window;
	uint32_t window_size, window_position, main_size;
	uint32_t recent[3];
	/* Goes negative when a match runs past its block; the next block starts that much shorter. */
	int64_t block_left;
	uint32_t frame_index, produced_total;
	uint32_t translation_size;
	int block_type, raw_odd, header_done;
	unsigned char main_lengths[main_limit], length_lengths[length_limit], aligned_lengths[aligned_limit];
	struct code_table main_code, length_code, aligned_code, pretree_code;
	uint32_t slot_base[51];
	unsigned char slot_extra[51];
};

static void refill(struct bit_reader *reader, int wanted)
{
	while (reader->count < wanted) {
		uint32_t word = 0;
		if (reader->end - reader->data >= 2) {
			word = get_16(reader->data);
			reader->data += 2;
		} else {
			/* Past the end reads as zeros; the frame check turns any real shortage into a failure. */
			reader->data = reader->end;
		}
		reader->bits |= word << (16 - reader->count);
		reader->count += 16;
	}
}

static uint32_t take_bits(struct bit_reader *reader, int count)
{
	uint32_t value = 0;
	while (count > 16) {
		value = value << 16 | take_bits(reader, 16);
		count -= 16;
	}
	if (!count)
		return value;
	refill(reader, count);
	value = value << count | reader->bits >> (32 - count);
	reader->bits <<= count;
	reader->count -= count;
	return value;
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
	/* An oversubscribed code cannot decode anything; an incomplete one is legal only for the single-symbol tree. */
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
			table->fast[start + entry] = (uint16_t)(symbol << 4 | length);
	}
	return 1;
}

static int read_symbol(struct bit_reader *reader, const struct code_table *table)
{
	if (!table->usable)
		return -1;
	refill(reader, code_bits_limit);
	uint32_t fast = table->fast[reader->bits >> (32 - fast_bits)];
	if (fast) {
		int length = fast & 15;
		reader->bits <<= length;
		reader->count -= length;
		return fast >> 4;
	}
	uint32_t window = reader->bits >> (32 - code_bits_limit);
	for (int length = fast_bits + 1; length <= code_bits_limit; length++) {
		uint32_t code = window >> (code_bits_limit - length);
		uint32_t first = table->first_code[length];
		uint32_t total = table->first_index[length + 1] - table->first_index[length];
		if (code >= first && code - first < total) {
			reader->bits <<= length;
			reader->count -= length;
			return table->sorted[table->first_index[length] + code - first];
		}
	}
	return -1;
}

/* A tree's lengths travel as deltas against the previous block's, themselves coded through a small tree sent first. */
static int read_lengths(struct bit_reader *reader, struct code_table *pretree_code, unsigned char *lengths, unsigned from, unsigned to)
{
	unsigned char pretree[pretree_limit];
	for (unsigned index = 0; index < pretree_limit; index++)
		pretree[index] = (unsigned char)take_bits(reader, 4);
	if (!build_code(pretree_code, pretree, pretree_limit))
		return 0;
	for (unsigned index = from; index < to;) {
		int symbol = read_symbol(reader, pretree_code);
		if (symbol < 0)
			return 0;
		if (symbol <= 16) {
			lengths[index] = (unsigned char)((lengths[index] + 17 - symbol) % 17);
			index++;
		} else if (symbol == 19) {
			unsigned run = 4 + take_bits(reader, 1);
			int delta = read_symbol(reader, pretree_code);
			if (delta < 0 || delta > 16 || index + run > to)
				return 0;
			unsigned char value = (unsigned char)((lengths[index] + 17 - delta) % 17);
			memset(lengths + index, value, run);
			index += run;
		} else {
			unsigned run = symbol == 17 ? 4 + take_bits(reader, 4) : 20 + take_bits(reader, 5);
			if (index + run > to)
				return 0;
			memset(lengths + index, 0, run);
			index += run;
		}
	}
	return 1;
}

static int read_block_header(struct lzx_decoder *decoder, struct bit_reader *reader)
{
	struct code_table *pretree_code = &decoder->pretree_code;
	decoder->block_type = (int)take_bits(reader, 3);
	uint32_t size = take_bits(reader, 24);
	if (decoder->block_type == block_aligned) {
		for (unsigned index = 0; index < aligned_limit; index++)
			decoder->aligned_lengths[index] = (unsigned char)take_bits(reader, 3);
		if (!build_code(&decoder->aligned_code, decoder->aligned_lengths, aligned_limit))
			return 0;
	}
	if (decoder->block_type == block_verbatim || decoder->block_type == block_aligned) {
		if (!read_lengths(reader, pretree_code, decoder->main_lengths, 0, main_literals) ||
		    !read_lengths(reader, pretree_code, decoder->main_lengths, main_literals, decoder->main_size) ||
		    !read_lengths(reader, pretree_code, decoder->length_lengths, 0, length_limit) ||
		    !build_code(&decoder->main_code, decoder->main_lengths, decoder->main_size) ||
		    !build_code(&decoder->length_code, decoder->length_lengths, length_limit))
			return 0;
	} else if (decoder->block_type == block_raw) {
		/* Back to the word boundary (a word of padding when already on one), then the repeated offsets as plain bytes. */
		refill(reader, 16);
		if (reader->count > 16)
			reader->data -= 2;
		reader->bits = 0;
		reader->count = 0;
		if (reader->end - reader->data < 12)
			return 0;
		for (unsigned index = 0; index < 3; index++)
			decoder->recent[index] = get_32(reader->data + index * 4);
		reader->data += 12;
		decoder->raw_odd = size & 1;
	} else {
		return 0;
	}
	decoder->block_left += size;
	return 1;
}

static int copy_match(struct lzx_decoder *decoder, uint32_t offset, uint32_t length)
{
	uint32_t mask = decoder->window_size - 1;
	if (!offset || offset > decoder->window_size)
		return 0;
	unsigned char *window = decoder->window;
	uint32_t position = decoder->window_position;
	uint32_t source = (position - offset) & mask;
	for (uint32_t done = 0; done < length; done++) {
		window[position] = window[source];
		position = (position + 1) & mask;
		source = (source + 1) & mask;
	}
	decoder->window_position = position;
	return 1;
}

/* A match may run past its block but never past the frame, whose whole output has to come from this block's input. */
static int decode_compressed(struct lzx_decoder *decoder, struct bit_reader *reader, uint32_t *frame_left)
{
	uint32_t mask = decoder->window_size - 1;
	while (decoder->block_left > 0 && *frame_left) {
		int symbol = read_symbol(reader, &decoder->main_code);
		if (symbol < 0)
			return 0;
		if (symbol < main_literals) {
			decoder->window[decoder->window_position] = (unsigned char)symbol;
			decoder->window_position = (decoder->window_position + 1) & mask;
			decoder->block_left--;
			--*frame_left;
			continue;
		}
		uint32_t element = (uint32_t)symbol - main_literals;
		uint32_t slot = element >> 3, length = element & 7;
		if (length == 7) {
			int extra = read_symbol(reader, &decoder->length_code);
			if (extra < 0)
				return 0;
			length += (uint32_t)extra;
		}
		length += 2;
		uint32_t offset;
		if (slot == 0) {
			offset = decoder->recent[0];
		} else if (slot <= 2) {
			offset = decoder->recent[slot];
			decoder->recent[slot] = decoder->recent[0];
			decoder->recent[0] = offset;
		} else {
			unsigned extra = decoder->slot_extra[slot];
			uint32_t verbatim;
			if (decoder->block_type == block_aligned && extra >= 3) {
				verbatim = take_bits(reader, (int)extra - 3) << 3;
				int aligned = read_symbol(reader, &decoder->aligned_code);
				if (aligned < 0)
					return 0;
				verbatim += (uint32_t)aligned;
			} else {
				verbatim = take_bits(reader, (int)extra);
			}
			offset = decoder->slot_base[slot] + verbatim - 2;
			decoder->recent[2] = decoder->recent[1];
			decoder->recent[1] = decoder->recent[0];
			decoder->recent[0] = offset;
		}
		if (length > *frame_left || !copy_match(decoder, offset, length))
			return 0;
		decoder->block_left -= length;
		*frame_left -= length;
	}
	return 1;
}

static int decode_raw(struct lzx_decoder *decoder, struct bit_reader *reader, uint32_t *frame_left)
{
	uint32_t run = *frame_left;
	if (run > decoder->block_left)
		run = (uint32_t)decoder->block_left;
	if ((size_t)(reader->end - reader->data) < run)
		return 0;
	uint32_t mask = decoder->window_size - 1;
	for (uint32_t index = 0; index < run; index++) {
		decoder->window[decoder->window_position] = reader->data[index];
		decoder->window_position = (decoder->window_position + 1) & mask;
	}
	reader->data += run;
	decoder->block_left -= run;
	*frame_left -= run;
	if (!decoder->block_left && decoder->raw_odd && reader->data < reader->end)
		reader->data++;
	return 1;
}

/* The compressor rewrites x86 CALL targets to absolute addresses; this undoes it on the output only, the window keeps the rewritten bytes. */
static void restore_calls(const struct lzx_decoder *decoder, unsigned char *output, size_t size)
{
	if (!decoder->translation_size || decoder->frame_index >= 32768 || size <= 10)
		return;
	int32_t translation = (int32_t)decoder->translation_size;
	int32_t frame_start = (int32_t)decoder->produced_total;
	for (size_t index = 0; index < size - 10; index++) {
		if (output[index] != 0xe8)
			continue;
		int32_t target = (int32_t)get_32(output + index + 1);
		int32_t here = frame_start + (int32_t)index;
		if (target >= -here && target < translation)
			put_32(output + index + 1, (uint32_t)(target >= 0 ? target - here : target + translation));
		index += 4;
	}
}

int lzx_decode(struct lzx_decoder *decoder, const unsigned char *input, size_t size, unsigned char *output, size_t produce)
{
	struct bit_reader reader = { input, input + size, 0, 0 };
	if (!decoder->header_done) {
		decoder->header_done = 1;
		if (take_bits(&reader, 1))
			decoder->translation_size = take_bits(&reader, 32);
	}
	uint32_t start = decoder->window_position, frame_left = (uint32_t)produce;
	while (frame_left) {
		if (decoder->block_left <= 0 && !read_block_header(decoder, &reader))
			return 0;
		/* A block ending exactly with the frame leaves the next header for the next frame's input. */
		if (decoder->block_left <= 0)
			continue;
		int done = decoder->block_type == block_raw ? decode_raw(decoder, &reader, &frame_left) : decode_compressed(decoder, &reader, &frame_left);
		if (!done)
			return 0;
		if (decoder->block_type == block_raw && decoder->block_left <= 0) {
			reader.bits = 0;
			reader.count = 0;
		}
	}
	size_t first = decoder->window_size - start;
	if (first > produce)
		first = produce;
	memcpy(output, decoder->window + start, first);
	memcpy(output + first, decoder->window, produce - first);
	restore_calls(decoder, output, produce);
	decoder->frame_index++;
	decoder->produced_total += (uint32_t)produce;
	return 1;
}

struct lzx_decoder *lzx_create(unsigned window_bits, decoder_alloc alloc, void *context)
{
	static const unsigned char slots_by_bits[] = { 30, 32, 34, 36, 38, 42, 50 };
	if (window_bits < 15 || window_bits > 21)
		return NULL;
	struct lzx_decoder *decoder = alloc(sizeof(*decoder), context);
	if (!decoder)
		return NULL;
	memset(decoder, 0, sizeof(*decoder));
	decoder->window_size = 1u << window_bits;
	decoder->window = alloc(decoder->window_size, context);
	if (!decoder->window)
		return NULL;
	/* A corrupt or truncated stream may refer to history that never was: it reads as zeros, not as stale heap. */
	memset(decoder->window, 0, decoder->window_size);
	unsigned slots = slots_by_bits[window_bits - 15];
	decoder->main_size = main_literals + 8 * slots;
	decoder->recent[0] = decoder->recent[1] = decoder->recent[2] = 1;
	uint32_t base = 0;
	for (unsigned slot = 0; slot < slots; slot++) {
		unsigned extra = slot < 4 ? 0 : (slot - 2) / 2;
		decoder->slot_extra[slot] = (unsigned char)(extra > 17 ? 17 : extra);
		decoder->slot_base[slot] = base;
		base += 1u << decoder->slot_extra[slot];
	}
	return decoder;
}

void lzx_destroy(struct lzx_decoder *decoder, decoder_free release, void *context)
{
	if (!decoder)
		return;
	release(decoder->window, context);
	release(decoder, context);
}
