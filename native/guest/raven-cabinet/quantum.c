/* Quantum, the arithmetic-coded format some older cabinets use: 16-bit big-endian words read from the most significant bit down, adaptive
   frequency models, and the output of one cabinet block per call. */

#include <string.h>

#include "format.h"

enum {
	model_literal_size = 64,
	model_selector_size = 7,
	model_length_size = 27,
	model_position_limit = 42,
	frequency_step = 8,
	frequency_ceiling = 3800,
	rebuilds_between_sorts = 50,
};

struct model_entry
{
	uint16_t symbol, total;
};

struct model
{
	unsigned size, rebuilds_left;
	struct model_entry *entries;
};

struct quantum_decoder
{
	unsigned char *window;
	uint32_t mask, position;
	struct model literals[4], positions[3], length, selector;
	struct model_entry literal_entries[4][model_literal_size + 1];
	struct model_entry position_entries[3][model_position_limit + 1];
	struct model_entry length_entries[model_length_size + 1];
	struct model_entry selector_entries[model_selector_size + 1];
	uint32_t position_base[model_position_limit], length_base[model_length_size];
	unsigned char position_extra[model_position_limit], length_extra[model_length_size];
};

struct range_reader
{
	const unsigned char *data, *end;
	uint32_t bits;
	int count;
	uint16_t high, low, code;
};

static unsigned next_bit(struct range_reader *reader)
{
	if (!reader->count) {
		uint32_t word = 0;
		for (unsigned index = 0; index < 2; index++)
			word = word << 8 | (reader->data < reader->end ? *reader->data++ : 0);
		reader->bits = word;
		reader->count = 16;
	}
	reader->count--;
	return (reader->bits >> reader->count) & 1;
}

static uint32_t plain_bits(struct range_reader *reader, unsigned count)
{
	uint32_t value = 0;
	while (count--)
		value = value << 1 | next_bit(reader);
	return value;
}

static void init_model(struct model *model, struct model_entry *entries, unsigned first_symbol, unsigned size)
{
	model->size = size;
	model->rebuilds_left = 4;
	model->entries = entries;
	for (unsigned index = 0; index <= size; index++) {
		entries[index].symbol = (uint16_t)(first_symbol + index);
		entries[index].total = (uint16_t)(size - index);
	}
}

/* The cumulative totals halve; every so often they turn into plain frequencies, get re-sorted and turn back. The exact order
   of the sort is part of the format: encoder and decoder must end with the same table. */
static void rebuild_model(struct model *model)
{
	struct model_entry *entries = model->entries;
	if (--model->rebuilds_left) {
		for (int index = (int)model->size - 1; index >= 0; index--) {
			entries[index].total >>= 1;
			if (entries[index].total <= entries[index + 1].total)
				entries[index].total = entries[index + 1].total + 1;
		}
		return;
	}
	model->rebuilds_left = rebuilds_between_sorts;
	for (unsigned index = 0; index < model->size; index++) {
		entries[index].total -= entries[index + 1].total;
		entries[index].total = (uint16_t)((entries[index].total + 1) >> 1);
	}
	for (unsigned low = 0; low + 1 < model->size; low++) {
		for (unsigned high = low + 1; high < model->size; high++) {
			if (entries[low].total < entries[high].total) {
				struct model_entry swap = entries[low];
				entries[low] = entries[high];
				entries[high] = swap;
			}
		}
	}
	for (int index = (int)model->size - 1; index >= 0; index--)
		entries[index].total += entries[index + 1].total;
}

static unsigned read_symbol(struct range_reader *reader, struct model *model)
{
	struct model_entry *entries = model->entries;
	uint32_t range = ((uint32_t)(uint16_t)(reader->high - reader->low)) + 1;
	uint32_t top = entries[0].total;
	uint32_t scaled = (((uint32_t)(uint16_t)(reader->code - reader->low) + 1) * top - 1) / range;
	unsigned found = 1;
	while (found < model->size && entries[found].total > scaled)
		found++;
	unsigned symbol = entries[found - 1].symbol;

	uint32_t low = reader->low;
	reader->high = (uint16_t)(low + entries[found - 1].total * range / top - 1);
	reader->low = (uint16_t)(low + entries[found].total * range / top);
	for (unsigned index = 0; index < found; index++)
		entries[index].total += frequency_step;
	if (entries[0].total > frequency_ceiling)
		rebuild_model(model);

	for (;;) {
		if ((reader->low ^ reader->high) & 0x8000) {
			if (!(reader->low & 0x4000) || (reader->high & 0x4000))
				break;
			reader->code ^= 0x4000;
			reader->low &= 0x3fff;
			reader->high |= 0x4000;
		}
		reader->low = (uint16_t)(reader->low << 1);
		reader->high = (uint16_t)(reader->high << 1 | 1);
		reader->code = (uint16_t)(reader->code << 1 | next_bit(reader));
	}
	return symbol;
}

static void emit(struct quantum_decoder *decoder, unsigned char *output, size_t *done, unsigned char value)
{
	output[(*done)++] = value;
	decoder->window[decoder->position] = value;
	decoder->position = (decoder->position + 1) & decoder->mask;
}

int quantum_decode(struct quantum_decoder *decoder, const unsigned char *input, size_t size, unsigned char *output, size_t produce)
{
	struct range_reader reader = { input, input + size, 0, 0, 0xffff, 0, 0 };
	reader.code = (uint16_t)plain_bits(&reader, 16);
	size_t done = 0;
	while (done < produce) {
		unsigned selector = read_symbol(&reader, &decoder->selector);
		if (selector < 4) {
			emit(decoder, output, &done, (unsigned char)read_symbol(&reader, &decoder->literals[selector]));
			continue;
		}
		uint32_t length;
		unsigned table = selector - 4;
		if (selector == 6) {
			unsigned slot = read_symbol(&reader, &decoder->length);
			length = decoder->length_base[slot] + plain_bits(&reader, decoder->length_extra[slot]) + 5;
		} else {
			length = selector - 1;
		}
		unsigned slot = read_symbol(&reader, &decoder->positions[table]);
		uint32_t offset = decoder->position_base[slot] + plain_bits(&reader, decoder->position_extra[slot]) + 1;
		if (length > produce - done)
			return 0;
		uint32_t source = (decoder->position - offset) & decoder->mask;
		for (uint32_t index = 0; index < length; index++) {
			emit(decoder, output, &done, decoder->window[source]);
			source = (source + 1) & decoder->mask;
		}
	}
	return 1;
}

struct quantum_decoder *quantum_create(unsigned window_bits, decoder_alloc alloc, void *context)
{
	if (window_bits < 10 || window_bits > 21)
		return NULL;
	struct quantum_decoder *decoder = alloc(sizeof(*decoder), context);
	if (!decoder)
		return NULL;
	memset(decoder, 0, sizeof(*decoder));
	decoder->mask = (1u << window_bits) - 1;
	decoder->window = alloc(decoder->mask + 1, context);
	if (!decoder->window)
		return NULL;
	memset(decoder->window, 0, decoder->mask + 1);

	uint32_t base = 0;
	for (unsigned slot = 0; slot < model_position_limit; slot++) {
		unsigned extra = slot < 4 ? 0 : (slot - 2) / 2;
		decoder->position_extra[slot] = (unsigned char)extra;
		decoder->position_base[slot] = base;
		base += 1u << extra;
	}
	base = 0;
	for (unsigned slot = 0; slot < model_length_size; slot++) {
		unsigned extra = slot < 6 || slot == model_length_size - 1 ? 0 : (slot - 2) / 4;
		decoder->length_extra[slot] = (unsigned char)extra;
		decoder->length_base[slot] = base;
		base += 1u << extra;
	}

	for (unsigned index = 0; index < 4; index++)
		init_model(&decoder->literals[index], decoder->literal_entries[index], index * model_literal_size, model_literal_size);
	/* A window of n bits has 2n position slots; the shorter fixed-length matches only reach so far. */
	unsigned slots = window_bits * 2;
	init_model(&decoder->positions[0], decoder->position_entries[0], 0, slots < 24 ? slots : 24);
	init_model(&decoder->positions[1], decoder->position_entries[1], 0, slots < 36 ? slots : 36);
	init_model(&decoder->positions[2], decoder->position_entries[2], 0, slots);
	init_model(&decoder->length, decoder->length_entries, 0, model_length_size);
	init_model(&decoder->selector, decoder->selector_entries, 0, model_selector_size);
	return decoder;
}

void quantum_destroy(struct quantum_decoder *decoder, decoder_free release, void *context)
{
	if (!decoder)
		return;
	release(decoder->window, context);
	release(decoder, context);
}
