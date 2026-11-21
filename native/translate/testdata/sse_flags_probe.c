// Prints the result and the MXCSR exception flags x86 leaves after SSE conversions to integers on boundary values, in every rounding
// mode: the reference for the translator's flags where Rosetta is the only other one. Standalone x86-64 C, no dependencies.
//   cc -O1 -o sse_flags_probe sse_flags_probe.c && ./sse_flags_probe
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint64_t doubles[] = {
	0x41dfffffffe00000ull, // 2147483647.5
	0xc1dfffffffe00000ull, // -2147483647.5
	0x41e0000000100000ull, // 2147483648.5
	0xc1e0000000100000ull, // -2147483648.5
	0x41dfffffffc00000ull, // 2147483647
	0x41e0000000000000ull, // 2147483648
	0xc1e0000000000000ull, // -2147483648
	0xc1e0000000200000ull, // -2147483649
	0x41dfffffffa00000ull, // 2147483646.5
	0x3fe0000000000000ull, // 0.5
	0x4004000000000000ull, // 2.5
	0x43e0000000000000ull, // 2^63
	0x43dfffffffffffffull, // the largest double below 2^63
	0xc3e0000000000000ull, // -2^63
	0x7ff0000000000000ull, // inf
	0xfff0000000000000ull, // -inf
	0x7ff8000000000000ull, // QNaN
	0x7ff0000000000001ull, // SNaN
};

static uint32_t mxcsr_get(void)
{
	uint32_t value;
	__asm__ volatile("stmxcsr %0" : "=m"(value));
	return value;
}

static void mxcsr_set(uint32_t value)
{
	__asm__ volatile("ldmxcsr %0" : : "m"(value));
}

int main(void)
{
	for (unsigned rc = 0; rc < 4; ++rc) {
		const uint32_t control = 0x1f80 | rc << 13;
		for (unsigned i = 0; i < sizeof doubles / sizeof doubles[0]; ++i) {
			double d;
			memcpy(&d, &doubles[i], 8);
			const float f = (float)d;
			uint32_t fbits;
			memcpy(&fbits, &f, 4);
			uint64_t r64;
			uint32_t r32, flags;
			uint64_t packed[2] = {doubles[i], 0};
			uint64_t out[2];

			mxcsr_set(control);
			__asm__ volatile("cvtsd2si %1, %0" : "=r"(r32) : "x"(d));
			flags = mxcsr_get() & 0x3f;
			printf("rc=%u cvtsd2si r32  %016llx -> %08x flags %02x\n", rc, (unsigned long long)doubles[i], r32, flags);

			mxcsr_set(control);
			__asm__ volatile("cvtsd2si %1, %0" : "=r"(r64) : "x"(d));
			flags = mxcsr_get() & 0x3f;
			printf("rc=%u cvtsd2si r64  %016llx -> %016llx flags %02x\n", rc, (unsigned long long)doubles[i], (unsigned long long)r64,
				flags);

			mxcsr_set(control);
			__asm__ volatile("cvttsd2si %1, %0" : "=r"(r32) : "x"(d));
			flags = mxcsr_get() & 0x3f;
			printf("rc=%u cvttsd2si r32 %016llx -> %08x flags %02x\n", rc, (unsigned long long)doubles[i], r32, flags);

			mxcsr_set(control);
			__asm__ volatile("movdqu %1, %%xmm0\n\tcvtpd2dq %%xmm0, %%xmm0\n\tmovdqu %%xmm0, %0" : "=m"(out) : "m"(packed) : "xmm0");
			flags = mxcsr_get() & 0x3f;
			printf("rc=%u cvtpd2dq      %016llx -> %016llx flags %02x\n", rc, (unsigned long long)doubles[i], (unsigned long long)out[0],
				flags);

			mxcsr_set(control);
			__asm__ volatile("cvtss2si %1, %0" : "=r"(r32) : "x"(f));
			flags = mxcsr_get() & 0x3f;
			printf("rc=%u cvtss2si r32  %08x -> %08x flags %02x\n", rc, fbits, r32, flags);
		}
	}
	mxcsr_set(0x1f80);
	return 0;
}
