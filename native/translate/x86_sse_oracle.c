// Runs the cases x86_sse.py wrote on an x86-64 CPU (here: Rosetta) and records what each left, for x86_sse_test and hvf_sse_test.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Case {
	uint64_t registers[16];
	uint64_t flags;
	uint32_t mxcsr;
	uint64_t xmm[16][2];
	uint64_t memory[16];
	const char *family;
	const char *start;
	const char *end;
	const char *text;
};

// The layout run_case reads and writes (x86_sse.py) and x86_sse_check.h reads.
struct Io {
	uint64_t registers[16];
	uint64_t flags;
	uint32_t mxcsr;
	uint32_t reserved;
	uint64_t xmm[16][2];
	uint8_t buffer[256];
};
_Static_assert(__builtin_offsetof(struct Io, xmm) == 144 && __builtin_offsetof(struct Io, buffer) == 400, "run_case's offsets");

void run_case(struct Io *io, const void *code);

#include "x86_sse_cases.h"

static void put(FILE *out, const void *bytes, size_t size)
{
	fwrite(bytes, 1, size, out);
}

static void put_string(FILE *out, const char *text)
{
	const uint32_t length = (uint32_t)strlen(text);
	put(out, &length, 4);
	put(out, text, length);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: x86_sse_oracle OUTPUT\n");
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
		memset(&io, 0, sizeof io);
		memcpy(io.registers, item->registers, sizeof io.registers);
		io.flags = item->flags | 2;
		io.mxcsr = item->mxcsr;
		memcpy(io.xmm, item->xmm, sizeof io.xmm);
		memcpy(io.buffer, item->memory, sizeof item->memory);
		for (unsigned b = sizeof item->memory; b < sizeof io.buffer; ++b)
			io.buffer[b] = (uint8_t)(b * 37 + i);
		const struct Io input = io;
		run_case(&io, item->start);
		const uint32_t length = (uint32_t)(item->end - item->start);
		put(out, &length, 4);
		put(out, item->start, length);
		put_string(out, item->text);
		put_string(out, item->family);
		put(out, &input, sizeof input);
		put(out, &io, sizeof io);
	}
	fclose(out);
	return 0;
}
