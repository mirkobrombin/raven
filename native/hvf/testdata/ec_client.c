// x64 client of ec_library.dll for hvf_arm64ec_test, called as raven_entry(role, a, b):
//   0 ec_zero()            1 ec_add(a, b)           2 ec_sum8(a, b, a + 1, b + 1, a + 2, b + 2, a + 3, b + 3)
//   3 ec_hash("...")       4 ec_call(x64 callback, a, b)
//   5 ec_teb_self() compared with gs:[0x30] as x64 code reads it: 1 when equal
//   9 ec_teb_count(gs:[0x30], a): how many of a reads through x18 in native code saw the TEB
//   10-15 ec_sum1, 3, 4, 5, 6, 7 of (a, b, a + 1, b + 1, a + 2, b + 2, a + 3)
//   16 ec_keep(x64 callback, the double whose bits are b, a), returning the bits of the double result
//   17 ec_call_checked(x64 callback, a, b)   18 the same calling native code
//   19 ec_raw_address()     20 calls the address a as a function of no arguments
//   21 ec_call6(x64 callback of six arguments, a)    22 ec_call_double(x64 callback, a, the double whose bits are b)
//   23 XMM6 = a and XMM7 = b across ec_clobber_v6v7(); returns XMM6 - XMM7
//   24 the bits of ec_fmax(the doubles whose bits are a and b)    25 then maxsd of the same in x64 code, after the call
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
__declspec(dllimport) double ec_keep(int64_t (*callback)(int64_t, int64_t), double x, int64_t a);
__declspec(dllimport) int64_t ec_call6(int64_t (*callback)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t), int64_t a);
__declspec(dllimport) int64_t ec_call_double(int64_t (*callback)(int64_t, double), int64_t a, double x);
__declspec(dllimport) int64_t ec_call_checked(int64_t (*callback)(int64_t, int64_t), int64_t a, int64_t b);
__declspec(dllimport) uint64_t ec_raw_address(void);
__declspec(dllimport) double ec_scale(int64_t a, double b);

__attribute__((noinline)) static int64_t x64_combine(int64_t a, int64_t b)
{
	return a * 10 - b;
}

__attribute__((noinline)) static int64_t x64_add(int64_t a, int64_t b)
{
	return a + b;
}

__attribute__((noinline)) static int64_t x64_keep(int64_t a, int64_t b)
{
	return a * 5 + b;
}

__attribute__((noinline)) static int64_t x64_checked(int64_t a, int64_t b)
{
	return a * 1000 - b;
}

__attribute__((noinline)) static int64_t x64_sum6(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f)
{
	return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}

// The callbacks, the XMM6/XMM7 check and the fmax check in assembly, so that the XMM registers they use are exactly the ones named.
// x64_xmm_keep breaks the ABI towards raven_entry, which keeps nothing in XMM registers.
int64_t x64_double_callback(int64_t a, double x);
int64_t x64_xmm_keep(int64_t a, int64_t b);
int64_t x64_fmax(int64_t a, int64_t b, int64_t after);
__asm__(".text\n"
		"x64_double_callback:\n\tmovq %xmm1, %rax\n\taddq %rcx, %rax\n\tret\n"
		"x64_xmm_keep:\n\tmovq %rcx, %xmm6\n\tmovq %rdx, %xmm7\n\tsubq $40, %rsp\n\tcallq *__imp_ec_clobber_v6v7(%rip)\n"
		"\taddq $40, %rsp\n\tmovq %xmm6, %rax\n\tmovq %xmm7, %rcx\n\tsubq %rcx, %rax\n\tret\n"
		"x64_fmax:\n\tpushq %rbx\n\tpushq %rsi\n\tpushq %rdi\n\tsubq $32, %rsp\n\tmovq %rcx, %rbx\n\tmovq %rdx, %rsi\n\tmovq %r8, %rdi\n"
		"\tmovq %rcx, %xmm0\n\tmovq %rdx, %xmm1\n\tcallq *__imp_ec_fmax(%rip)\n\tmovq %xmm0, %rax\n\ttestq %rdi, %rdi\n\tjz 1f\n"
		"\tmovq %rbx, %xmm2\n\tmovq %rsi, %xmm3\n\tmaxsd %xmm3, %xmm2\n\tmovq %xmm2, %rax\n"
		"1:\taddq $32, %rsp\n\tpopq %rdi\n\tpopq %rsi\n\tpopq %rbx\n\tret\n");

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
		// The double in XMM1 and the result in XMM0, as the call passes them.
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
	case 16: {
		register double x asm("xmm1");
		__asm__ volatile("movq %1, %0" : "=x"(x) : "r"(b));
		register double result asm("xmm0") = ec_keep(x64_keep, x, a);
		int64_t bits;
		__asm__ volatile("movq %1, %0" : "=r"(bits) : "x"(result));
		return bits;
	}
	case 17: return ec_call_checked(x64_checked, a, b);
	case 18: return ec_call_checked(0, a, b);
	case 19: return (int64_t)ec_raw_address();
	case 20: return ((int64_t(*)(void))a)();
	case 21: return ec_call6(x64_sum6, a);
	case 22: {
		register double x asm("xmm2");
		__asm__ volatile("movq %1, %0" : "=x"(x) : "r"(b));
		return ec_call_double(x64_double_callback, a, x);
	}
	case 23: return x64_xmm_keep(a, b);
	case 24: return x64_fmax(a, b, 0);
	case 25: return x64_fmax(a, b, 1);
	default: return -1;
	}
}
