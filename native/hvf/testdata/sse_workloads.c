// SSE workloads for the performance check of the SSE and SSE2 translation, as workloads.c is for integer code: one call does a fixed
// amount of work and returns a checksum. Exactly one WORKLOAD_* names the function exported as workload(). Built for x64 (SSE2, the
// baseline: packed singles for saxpy, scalar doubles for dot, 16-byte integer moves for copy) and natively for comparison.
#include <stdint.h>
#include <string.h>

// SSE2 has no fused multiply-add; without this the native arm64 build fuses a * x + y and rounds once instead of twice.
#pragma STDC FP_CONTRACT OFF

#define COUNT 1024

typedef uint8_t bytes16 __attribute__((vector_size(16), aligned(1)));

static float xs[COUNT], ys[COUNT];
static double xd[COUNT], yd[COUNT];
static uint8_t from[4096], to[4096];

static uint64_t bits_of(double value)
{
	uint64_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

// y = a * x + y over 1024 singles, 16 times: mulps, addps, movups.
static uint64_t saxpy(void)
{
	for (unsigned i = 0; i < COUNT; ++i) {
		xs[i] = (float)i * 0.25f;
		ys[i] = 1.0f;
	}
	for (int round = 0; round < 16; ++round)
		for (unsigned i = 0; i < COUNT; ++i)
			ys[i] = 0.999f * xs[i] + ys[i];
	double sum = 0;
	for (unsigned i = 0; i < COUNT; i += 64)
		sum += ys[i];
	return bits_of(sum);
}

// A dot product of 1024 doubles, 8 times, in order (no reassociation): mulsd, addsd, cvtsi2sd.
static uint64_t dot(void)
{
	for (unsigned i = 0; i < COUNT; ++i) {
		xd[i] = (double)i / 3.0;
		yd[i] = (double)(COUNT - i) * 0.5;
	}
	double sum = 0;
	for (int round = 0; round < 8; ++round)
		for (unsigned i = 0; i < COUNT; ++i)
			sum += xd[i] * yd[i];
	return bits_of(sum);
}

// 4096 bytes copied 16 bytes at a time with an xor, 16 times, so the loop stays a loop and not a call to memcpy: movdqu, pxor.
static uint64_t copy(void)
{
	for (unsigned i = 0; i < sizeof from; ++i)
		from[i] = (uint8_t)(i * 7);
	const bytes16 key = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
	for (int round = 0; round < 16; ++round)
		for (unsigned i = 0; i < sizeof from; i += 16)
			*(bytes16 *)(to + i) = *(const bytes16 *)(from + i) ^ key;
	uint64_t sum = 0;
	for (unsigned i = 0; i < sizeof to; i += 8)
		sum = sum * 31 + to[i];
	return sum;
}

uint64_t workload(void)
{
#if defined(WORKLOAD_SAXPY)
	return saxpy();
#elif defined(WORKLOAD_DOT)
	return dot();
#elif defined(WORKLOAD_COPY)
	return copy();
#else
#error "define WORKLOAD_SAXPY, WORKLOAD_DOT or WORKLOAD_COPY"
#endif
}
