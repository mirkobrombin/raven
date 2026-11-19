// Runs the cases x86_semantics.py wrote on an x86-64 CPU (here: Rosetta) and records what each left, for x86_semantics_test and
// hvf_integer_test (x86_semantics_check.h reads the file).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Case {
	uint64_t registers[16];
	uint64_t flags;
	uint64_t defined;
	const char *start;
	const char *end;
	const char *text;
	const char *family;
};

struct Io {
	uint64_t registers[16];
	uint64_t flags;
	uint8_t buffer[64];
};

void run_case(struct Io *io, const void *code);

#include "x86_semantics_cases.h"

static void put(FILE *out, const void *bytes, size_t size)
{
	fwrite(bytes, 1, size, out);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: x86_semantics_oracle OUTPUT\n");
		return 2;
	}
	FILE *out = fopen(argv[1], "wb");
	if (!out)
		return 1;
	const uint32_t count = sizeof cases / sizeof cases[0];
	put(out, &count, 4);
	for (uint32_t i = 0; i < count; ++i) {
		const struct Case *item = &cases[i];
		struct Io io;
		memcpy(io.registers, item->registers, sizeof io.registers);
		io.flags = item->flags | 2;
		for (unsigned b = 0; b < 64; ++b)
			io.buffer[b] = (uint8_t)(b * 37 + i);
		const struct Io input = io;
		run_case(&io, item->start);
		const uint32_t length = (uint32_t)(item->end - item->start);
		const uint32_t text_length = (uint32_t)strlen(item->text), family_length = (uint32_t)strlen(item->family);
		put(out, &length, 4);
		put(out, item->start, length);
		put(out, &text_length, 4);
		put(out, item->text, text_length);
		put(out, &family_length, 4);
		put(out, item->family, family_length);
		put(out, &item->defined, 8);
		put(out, &input, sizeof input);
		put(out, &io, sizeof io);
	}
	fclose(out);
	return 0;
}
