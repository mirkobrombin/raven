/* The LZX and Quantum decoders of cabinet.dll (native/guest/raven-cabinet), fed changed and random blocks on the host under the address and
   undefined behaviour sanitizers. Random bytes rarely make a valid LZX tree, so most LZX blocks are written here with valid trees and
   random codes after them, then have a few bits flipped: that reaches the matches and the window copies, not only the header checks. A
   decoder may refuse any block; it must never read or write outside what it was given.

   cc -fsanitize=address,undefined -fno-sanitize-recover=all -Inative/guest/raven-cabinet native/fuzz/cabinet_decoders.c
      native/guest/raven-cabinet/lzx.c native/guest/raven-cabinet/quantum.c
   ./a.out [rounds] [seed] */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "format.h"

static uint64_t state = 0x9e3779b97f4a7c15ull;

static uint64_t next(void)
{
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	return state;
}

static void *take(size_t size, void *context)
{
	(void)context;
	return malloc(size);
}

static void give(void *memory, void *context)
{
	(void)context;
	free(memory);
}

/* LZX bits: 16-bit little-endian words, filled from the most significant bit down. */
struct writer
{
	unsigned char *data;
	size_t size, room;
	uint32_t word;
	int count;
};

static void put_bits(struct writer *writer, uint32_t value, int count)
{
	while (count--) {
		writer->word = writer->word << 1 | (value >> count & 1);
		if (++writer->count < 16)
			continue;
		if (writer->size + 2 <= writer->room) {
			writer->data[writer->size++] = (unsigned char)writer->word;
			writer->data[writer->size++] = (unsigned char)(writer->word >> 8);
		}
		writer->word = 0;
		writer->count = 0;
	}
}

/* A pretree whose one symbol is `symbol`, a code of one bit, then `count` of that code: every length moves by the same delta. */
static void put_lengths(struct writer *writer, unsigned symbol, unsigned count)
{
	for (unsigned index = 0; index < 20; ++index)
		put_bits(writer, index == symbol, 4);
	for (unsigned index = 0; index < count; ++index)
		put_bits(writer, 0, 1);
}

/* A verbatim or aligned block whose main lengths are all 9, length lengths 8 and aligned lengths 3, with random codes after its trees. The
   first block of a folder moves the lengths from 0 (deltas 8 and 9); a later one keeps them (delta 0). */
static size_t lzx_block(unsigned char *out, size_t room, unsigned window_bits, int first, int aligned)
{
	static const unsigned slots_by_bits[] = {30, 32, 34, 36, 38, 42, 50};
	const unsigned main_size = 256 + 8 * slots_by_bits[window_bits - 15];
	struct writer writer = {out, 0, room, 0, 0};
	if (first) {
		const int translated = next() & 1;
		put_bits(&writer, translated, 1);
		if (translated)
			put_bits(&writer, (uint32_t)next(), 32);
	}
	put_bits(&writer, aligned ? 2 : 1, 3);
	put_bits(&writer, 1 + next() % 40000, 24);
	if (aligned)
		for (int index = 0; index < 8; ++index)
			put_bits(&writer, 3, 3);
	put_lengths(&writer, first ? 8 : 0, 256);
	put_lengths(&writer, first ? 8 : 0, main_size - 256);
	put_lengths(&writer, first ? 9 : 0, 249);
	while (writer.size + 2 <= room)
		put_bits(&writer, (uint32_t)next(), 16);
	return writer.size;
}

int main(int argc, char **argv)
{
	static unsigned char input[block_input_limit];
	const unsigned long rounds = argc > 1 ? strtoul(argv[1], NULL, 0) : 20000;
	if (argc > 2)
		state = strtoull(argv[2], NULL, 0) | 1;
	for (unsigned long round = 0; round < rounds; ++round) {
		const int lzx = next() & 1;
		const unsigned window_bits = lzx ? 15 + next() % 7 : 10 + next() % 12;
		void *decoder = lzx ? (void *)lzx_create(window_bits, take, NULL) : (void *)quantum_create(window_bits, take, NULL);
		if (!decoder) {
			fprintf(stderr, "no decoder for a window of %u bits\n", window_bits);
			return 1;
		}
		/* The blocks of one folder share the decoder's history. */
		for (unsigned block = 0, blocks = 1 + next() % 4; block < blocks; ++block) {
			size_t size;
			if (lzx && next() % 4) {
				size = lzx_block(input, 200 + next() % (sizeof input - 200), window_bits, block == 0, next() & 1);
				for (unsigned flips = next() % 4; flips; --flips)
					input[next() % size] ^= (unsigned char)(1u << next() % 8);
			} else {
				size = next() % 4 ? 1 + next() % sizeof input : next() % 16;
				for (size_t index = 0; index < size; ++index)
					input[index] = (unsigned char)next();
			}
			/* Exactly sized copies, so the sanitizer sees a read past either end. */
			const size_t produce = 1 + next() % block_output_limit;
			unsigned char *in = malloc(size ? size : 1), *out = malloc(produce);
			if (!in || !out)
				return 1;
			memcpy(in, input, size);
			const int done = lzx ? lzx_decode(decoder, in, size, out, produce) : quantum_decode(decoder, in, size, out, produce);
			free(in);
			free(out);
			if (!done)
				break;
		}
		if (lzx)
			lzx_destroy(decoder, give, NULL);
		else
			quantum_destroy(decoder, give, NULL);
	}
	printf("%lu rounds\n", rounds);
	return 0;
}
