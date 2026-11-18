// An ARM64EC DLL with no CRT and no imports, for hvf_arm64ec_test: native code that x64 code calls through the compiler's export and
// entry thunks, and that calls x64 code back through an exit thunk. Built for this host too, as the test's oracle (EC_LIBRARY_HOST).
#include <stdint.h>

#ifdef EC_LIBRARY_HOST
#define EXPORT
#else
#define EXPORT __declspec(dllexport)
#endif

EXPORT int64_t ec_zero(void)
{
	return 0x5eed;
}

EXPORT int64_t ec_add(int64_t a, int64_t b)
{
	return a + b;
}

// One to seven arguments: from the fifth on they come from the x64 stack.
EXPORT int64_t ec_sum1(int64_t a)
{
	return 3 * a + 1;
}

EXPORT int64_t ec_sum3(int64_t a, int64_t b, int64_t c)
{
	return a + 2 * b + 3 * c;
}

EXPORT int64_t ec_sum4(int64_t a, int64_t b, int64_t c, int64_t d)
{
	return a + 2 * b + 3 * c + 4 * d;
}

EXPORT int64_t ec_sum5(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e)
{
	return a + 2 * b + 3 * c + 4 * d + 5 * e;
}

EXPORT int64_t ec_sum6(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f)
{
	return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}

EXPORT int64_t ec_sum7(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g)
{
	return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g;
}

// Eight arguments: the last four come from the x64 stack.
EXPORT int64_t ec_sum8(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g, int64_t h)
{
	return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}

// FNV-1a of a NUL-terminated string in x64 memory, with its length in the top byte.
EXPORT uint64_t ec_hash(const char *text)
{
	uint64_t hash = 0xcbf29ce484222325ull, length = 0;
	for (; text[length]; ++length)
		hash = (hash ^ (uint8_t)text[length]) * 0x100000001b3ull;
	return (hash & 0x00ffffffffffffffull) | (length << 56);
}

// An integer and a double: x64 passes them in RCX and XMM1, AAPCS64 in x0 and d0, and the double comes back in XMM0 and d0.
EXPORT double ec_scale(int64_t a, double b)
{
	return (double)a * b + 0.25;
}

// Calls back x64 code, as qsort or EnumWindows do.
EXPORT int64_t ec_call(int64_t (*callback)(int64_t, int64_t), int64_t a, int64_t b)
{
	return callback(a, b) * 3 + callback(b, a);
}

// The TEB's own address, NT_TIB.Self at 0x30, read through x18 as native Windows ARM64 code does.
EXPORT uint64_t ec_teb_self(void)
{
#ifdef EC_LIBRARY_HOST
	return 0;
#else
	uint64_t self;
	__asm__ volatile("ldr %0, [x18, #0x30]" : "=r"(self));
	return self;
#endif
}

// How many of count reads of NT_TIB.Self through x18 give self: a long native loop that forced exits land in.
EXPORT int64_t ec_teb_count(uint64_t self, int64_t count)
{
#ifdef EC_LIBRARY_HOST
	(void)self;
	return count;
#else
	int64_t right = 0;
	for (int64_t i = 0; i < count; ++i) {
		uint64_t read;
		__asm__ volatile("ldr %0, [x18, #0x30]" : "=r"(read));
		right += read == self;
	}
	return right;
#endif
}

#ifndef EC_LIBRARY_HOST
int __stdcall ec_library_entry(void *module, unsigned reason, void *reserved)
{
	(void)module, (void)reason, (void)reserved;
	return 1;
}
#endif
