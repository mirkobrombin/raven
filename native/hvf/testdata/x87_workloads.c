// x87 workloads for the performance check of the x87 core, as sse_workloads.c is for SSE: one call does a fixed amount of work and
// returns a checksum. Exactly one WORKLOAD_* names the function exported as workload(). Built for x64 with mingw's 80-bit long
// double, so the arithmetic is x87; the native arm64 build has a 64-bit long double, so its checksum differs and only its time is
// comparable.
#include <stdint.h>
#include <string.h>

#define COUNT 512

static double xs[COUNT], ys[COUNT];

// The significand and the sign and exponent; the 6 bytes of padding after the 80 bits are not the value's.
static uint64_t bits_of(long double value)
{
	uint64_t bits[2] = {0, 0};
	memcpy(bits, &value, sizeof value < sizeof bits ? sizeof value : sizeof bits);
	return bits[0] ^ (bits[1] & 0xffff) << 48;
}

// A dot product of 512 doubles in long double, 8 times, in order: fld, fmul, faddp per element.
static uint64_t ldot(void)
{
	for (unsigned i = 0; i < COUNT; ++i) {
		xs[i] = (double)i / 3.0;
		ys[i] = (double)(COUNT - i) * 0.5;
	}
	long double sum = 0;
	for (int round = 0; round < 8; ++round)
		for (unsigned i = 0; i < COUNT; ++i)
			sum += (long double)xs[i] * ys[i];
	return bits_of(sum);
}

// A degree-12 polynomial by Horner's rule at 256 points, in long double: fmul and fadd on the stack.
static uint64_t lpoly(void)
{
	static const long double coefficients[13] = {1.0L, -0.5L, 0.25L, -0.125L, 0.0625L, 1e-3L, -2e-4L, 3e-5L, -4e-6L, 5e-7L,
		-6e-8L, 7e-9L, -8e-10L};
	long double total = 0;
	for (unsigned i = 0; i < 256; ++i) {
		const long double x = (long double)i / 256.0L;
		long double y = coefficients[12];
		for (int k = 11; k >= 0; --k)
			y = y * x + coefficients[k];
		total += y;
	}
	return bits_of(total);
}

uint64_t workload(void)
{
#if defined(WORKLOAD_LDOT)
	return ldot();
#elif defined(WORKLOAD_LPOLY)
	return lpoly();
#else
#error "define WORKLOAD_LDOT or WORKLOAD_LPOLY"
#endif
}
