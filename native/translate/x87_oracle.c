// Runs the cases x87.py wrote on an x86-64 CPU or under Rosetta and records what each left, for x87_test and hvf_x87_test. Nothing
// here uses long double or an x87 instruction: run_case gives the caller its own x87 state back after each case. A case that faults
// (Rosetta raises SIGILL on ffreep) is recorded with the signal in the output's reserved field.
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Case {
	uint64_t registers[16];
	uint64_t flags;
	uint64_t fx[20];
	uint64_t memory[32];
	const char *family;
	const char *start;
	const char *end;
	const char *text;
};

// The layout run_case reads and writes (x87.py) and x87_check.h reads: fx is an FXSAVE image.
struct Io {
	uint64_t registers[16];
	uint64_t flags;
	// In the output: the signal the case raised, 0 when it returned.
	uint64_t reserved;
	_Alignas(16) uint8_t fx[512];
	uint8_t buffer[256];
};
_Static_assert(__builtin_offsetof(struct Io, fx) == 144 && __builtin_offsetof(struct Io, buffer) == 656, "run_case's offsets");

void run_case(struct Io *io, const void *code);
extern uint8_t host_fx[512];

static sigjmp_buf resume;

static void fault(int signal)
{
	__asm__ volatile("fxrstor64 %0" : : "m"(host_fx));
	siglongjmp(resume, signal);
}

#include "x87_cases.h"

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
		fprintf(stderr, "usage: x87_oracle OUTPUT\n");
		return 2;
	}
	FILE *out = fopen(argv[1], "wb");
	if (!out)
		return 1;
	signal(SIGILL, fault);
	signal(SIGFPE, fault);
	signal(SIGSEGV, fault);
	signal(SIGBUS, fault);
	const uint32_t count = sizeof cases / sizeof cases[0];
	put(out, &count, 4);
	static struct Io io, input;
	for (uint32_t i = 0; i < count; ++i) {
		const struct Case *item = &cases[i];
		memset(&io, 0, sizeof io);
		memcpy(io.registers, item->registers, sizeof io.registers);
		io.flags = item->flags | 2;
		memcpy(io.fx, item->fx, sizeof item->fx);
		memcpy(io.buffer, item->memory, sizeof io.buffer);
		input = io;
		const int raised = sigsetjmp(resume, 1);
		if (raised) {
			io = input;
			io.reserved = (uint64_t)raised;
		} else
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
