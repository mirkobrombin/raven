// AVX workloads for the performance check of the AVX translation, as sse_workloads.c is for SSE: one call does a fixed amount of work
// and returns a checksum. Exactly one WORKLOAD_* names the function exported as workload(). Built for x64 with AVX2 and FMA3 (256-bit
// intrinsics, so the instructions do not depend on the vectorizer) and natively with the NEON instructions the translator maps them
// to, for comparison. The x64 build has no popcnt (-mno-popcnt): the translator does not run it.
#include <stdint.h>
#include <string.h>

#define COUNT 1024

static float xs[COUNT], ys[COUNT];
static uint8_t text[4096];

#if defined(__x86_64__)
#include <immintrin.h>

// y = a * x + y over 1024 singles, 16 times: vfmadd231ps on YMM registers.
static void fma_pass(float a)
{
	const __m256 factor = _mm256_set1_ps(a);
	for (unsigned i = 0; i < COUNT; i += 8)
		_mm256_storeu_ps(ys + i, _mm256_fmadd_ps(factor, _mm256_loadu_ps(xs + i), _mm256_loadu_ps(ys + i)));
}

// How many bytes of 4096 equal a value: vpcmpeqb and vpmovmskb on YMM registers, as a string search does.
static unsigned count_pass(uint8_t value)
{
	const __m256i wanted = _mm256_set1_epi8((char)value);
	unsigned count = 0;
	for (unsigned i = 0; i < sizeof text; i += 32) {
		unsigned mask = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(text + i)), wanted));
		while (mask) {
			mask &= mask - 1;
			++count;
		}
	}
	return count;
}
#else
#include <arm_neon.h>

static void fma_pass(float a)
{
	const float32x4_t factor = vdupq_n_f32(a);
	for (unsigned i = 0; i < COUNT; i += 4)
		vst1q_f32(ys + i, vfmaq_f32(vld1q_f32(ys + i), factor, vld1q_f32(xs + i)));
}

static unsigned count_pass(uint8_t value)
{
	const uint8x16_t wanted = vdupq_n_u8(value);
	unsigned count = 0;
	for (unsigned i = 0; i < sizeof text; i += 16)
		count += vaddvq_u8(vandq_u8(vceqq_u8(vld1q_u8(text + i), wanted), vdupq_n_u8(1)));
	return count;
}
#endif

#if defined(WORKLOAD_FSAXPY)
uint64_t workload(void)
{
	for (unsigned i = 0; i < COUNT; ++i) {
		xs[i] = (float)i * 0.25f;
		ys[i] = 1.0f;
	}
	for (unsigned pass = 0; pass < 16; ++pass)
		fma_pass(0.5f + (float)pass);
	uint64_t sum = 0;
	for (unsigned i = 0; i < COUNT; ++i) {
		uint32_t bits;
		memcpy(&bits, &ys[i], sizeof bits);
		sum = sum * 31 + bits;
	}
	return sum;
}
#elif defined(WORKLOAD_BYTECOUNT)
uint64_t workload(void)
{
	// Scalar, so the setup uses no instruction the translator lacks (vpackusdw, vpermq).
#pragma clang loop vectorize(disable)
	for (unsigned i = 0; i < sizeof text; ++i)
		text[i] = (uint8_t)(i * 7 + (i >> 5));
	uint64_t sum = 0;
	for (unsigned value = 0; value < 16; ++value)
		sum = sum * 4099 + count_pass((uint8_t)value);
	return sum;
}
#endif
