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

// A double that stays in a callee-saved register (d8) across a call back into x64.
EXPORT double ec_keep(int64_t (*callback)(int64_t, int64_t), double x, int64_t a)
{
	const double kept = x * 3.0 + 1.0;
	const int64_t result = callback(a, a);
	return kept + (double)result;
}

// Six arguments to x64, two of them on its stack, and an integer and a double, through the compiler's exit thunks.
EXPORT int64_t ec_call6(int64_t (*callback)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t), int64_t a)
{
	return callback(a, a + 1, a + 2, a + 3, a + 4, a + 5) + 1;
}

EXPORT int64_t ec_call_double(int64_t (*callback)(int64_t, double), int64_t a, double x)
{
	return callback(a, x) * 2;
}

// Writes v6 and v7, which x64 callers expect preserved as XMM6 and XMM7: the entry thunk saves them.
EXPORT int64_t ec_clobber_v6v7(void)
{
#ifndef EC_LIBRARY_HOST
	__asm__ volatile("movi v6.2d, #0\n\tmovi v7.2d, #0" ::: "v6", "v7");
#endif
	return 0;
}

static int64_t native_combine(int64_t a, int64_t b)
{
	return a * 100 + b;
}

// An indirect call through __os_arm64x_check_call (the slot the CHPE metadata calls __os_arm64x_dispatch_call): x11 the target, x10
// its exit thunk; the slot leaves x11 at what to call. callback 0 calls a native function instead of x64 code.
EXPORT int64_t ec_call_checked(int64_t (*callback)(int64_t, int64_t), int64_t a, int64_t b)
{
	int64_t (*target)(int64_t, int64_t) = callback ? callback : native_combine;
#ifdef EC_LIBRARY_HOST
	return target(a, b);
#else
	register int64_t x0 __asm__("x0") = a;
	register int64_t x1 __asm__("x1") = b;
	register int64_t (*x11)(int64_t, int64_t) __asm__("x11") = target;
	__asm__ volatile("adrp x10, \"$iexit_thunk$cdecl$i8$i8i8\"\n\t"
					 "add x10, x10, :lo12:\"$iexit_thunk$cdecl$i8$i8i8\"\n\t"
					 "adrp x16, __os_arm64x_check_call\n\t"
					 "ldr x16, [x16, :lo12:__os_arm64x_check_call]\n\t"
					 "blr x16\n\t"
					 "blr x11"
		: "+r"(x0), "+r"(x1), "+r"(x11)
		:
		: "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x12", "x13", "x14", "x15", "x16", "x17", "x30", "v0", "v1", "v2",
		"v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28",
		"v29", "v30", "v31", "memory", "cc");
	return x0;
#endif
}

#ifndef EC_LIBRARY_HOST
// Native code with no entry thunk, the word before it 0: x64 code must not be able to call it.
__asm__(".text\n\t.p2align 4\n\t.word 0\nec_raw_code:\n\tmov x0, #7\n\tret\n");
extern char ec_raw_code[];

EXPORT uint64_t ec_raw_address(void)
{
	return (uint64_t)ec_raw_code;
}

int __stdcall ec_library_entry(void *module, unsigned reason, void *reserved)
{
	(void)module, (void)reason, (void)reserved;
	return 1;
}
#endif
