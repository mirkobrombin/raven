// x64 client of ec_library.dll for hvf_arm64ec_test, called as raven_entry(role, a, b):
//   0 ec_zero()            1 ec_add(a, b)           2 ec_sum8(a, b, a + 1, b + 1, a + 2, b + 2, a + 3, b + 3)
//   3 ec_hash("...")       4 ec_call(x64 callback, a, b)
//   5 ec_teb_self() compared with gs:[0x30] as x64 code reads it: 1 when equal
//   9 ec_teb_count(gs:[0x30], a): how many of a reads through x18 in native code saw the TEB
//   10-15 ec_sum1, 3, 4, 5, 6, 7 of (a, b, a + 1, b + 1, a + 2, b + 2, a + 3)
//   8 ec_scale(a, the double whose bits are b), returning the bits of the double result
//   6 a calls of ec_add(i, b), summed        7 the same through an x64 function pointer to x64 code
#include <stdint.h>

__declspec(dllimport) int64_t ec_zero(void);
__declspec(dllimport) int64_t ec_add(int64_t a, int64_t b);
__declspec(dllimport) int64_t ec_sum1(int64_t a);
__declspec(dllimport) int64_t ec_sum3(int64_t a, int64_t b, int64_t c);
__declspec(dllimport) int64_t ec_sum4(int64_t a, int64_t b, int64_t c, int64_t d);
__declspec(dllimport) int64_t ec_sum5(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e);
__declspec(dllimport) int64_t ec_sum6(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f);
__declspec(dllimport) int64_t ec_sum7(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g);
__declspec(dllimport) int64_t ec_sum8(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g, int64_t h);
__declspec(dllimport) uint64_t ec_hash(const char *text);
__declspec(dllimport) int64_t ec_call(int64_t (*callback)(int64_t, int64_t), int64_t a, int64_t b);
__declspec(dllimport) uint64_t ec_teb_self(void);
__declspec(dllimport) int64_t ec_teb_count(uint64_t self, int64_t count);
__declspec(dllimport) double ec_scale(int64_t a, double b);

__attribute__((noinline)) static int64_t x64_combine(int64_t a, int64_t b)
{
	return a * 10 - b;
}

__attribute__((noinline)) static int64_t x64_add(int64_t a, int64_t b)
{
	return a + b;
}

static int64_t (*volatile x64_function)(int64_t, int64_t) = x64_add;

int64_t raven_entry(int64_t role, int64_t a, int64_t b)
{
	switch (role) {
	case 0: return ec_zero();
	case 1: return ec_add(a, b);
	case 2: return ec_sum8(a, b, a + 1, b + 1, a + 2, b + 2, a + 3, b + 3);
	case 3: return (int64_t)ec_hash("translated x64 hands native code a string");
	case 4: return ec_call(x64_combine, a, b);
	case 5: {
		uint64_t self;
		__asm__ volatile("movq %%gs:0x30, %0" : "=r"(self));
		return ec_teb_self() == self && self != 0;
	}
	case 6: {
		int64_t sum = 0;
		for (int64_t i = 0; i < a; ++i)
			sum += ec_add(i, b);
		return sum;
	}
	case 7: {
		int64_t sum = 0;
		for (int64_t i = 0; i < a; ++i)
			sum += x64_function(i, b);
		return sum;
	}
	case 8: {
		// The translator moves doubles between general and XMM registers with movq only: keep the compiler to that.
		register double x asm("xmm1");
		__asm__ volatile("movq %1, %0" : "=x"(x) : "r"(b));
		register double result asm("xmm0") = ec_scale(a, x);
		int64_t bits;
		__asm__ volatile("movq %1, %0" : "=r"(bits) : "x"(result));
		return bits;
	}
	case 9: {
		uint64_t self;
		__asm__ volatile("movq %%gs:0x30, %0" : "=r"(self));
		return ec_teb_count(self, a);
	}
	case 10: return ec_sum1(a);
	case 11: return ec_sum3(a, b, a + 1);
	case 12: return ec_sum4(a, b, a + 1, b + 1);
	case 13: return ec_sum5(a, b, a + 1, b + 1, a + 2);
	case 14: return ec_sum6(a, b, a + 1, b + 1, a + 2, b + 2);
	case 15: return ec_sum7(a, b, a + 1, b + 1, a + 2, b + 2, a + 3);
	default: return -1;
	}
}
