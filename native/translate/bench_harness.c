// Times one workload function for the engine comparison: the first call alone (it includes any translation), then RUNS runs of CALLS
// calls each. Prints "first NS" and "run INDEX NS_PER_CALL" lines.
//
// Built two ways from this one file: natively against the C library, and as a freestanding static x86-64 Linux ELF (FREESTANDING)
// that FEX runs with no root filesystem, which talks to the kernel through raw system calls only.
#include <stdint.h>

#ifndef BENCH_FUNCTION
#define BENCH_FUNCTION workload
#endif

uint64_t BENCH_FUNCTION(void);

#ifdef FREESTANDING
struct timespec {
	long tv_sec;
	long tv_nsec;
};

static long system_call(long number, long a, long b, long c)
{
	long result;
	__asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
	return result;
}

static uint64_t now_ns(void)
{
	struct timespec time;
	system_call(228, 1, (long)&time, 0); // clock_gettime(CLOCK_MONOTONIC)
	return (uint64_t)time.tv_sec * 1000000000ull + (uint64_t)time.tv_nsec;
}

static void put(const char *text, unsigned long length)
{
	system_call(1, 1, (long)text, (long)length);
}

void *memset(void *target, int value, unsigned long size)
{
	unsigned char *bytes = target;
	while (size--)
		*bytes++ = (unsigned char)value;
	return target;
}

void *memcpy(void *target, const void *source, unsigned long size)
{
	unsigned char *to = target;
	const unsigned char *from = source;
	while (size--)
		*to++ = *from++;
	return target;
}
#else
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000000000ull + (uint64_t)time.tv_nsec;
}

static void put(const char *text, unsigned long length)
{
	(void)!write(1, text, length);
}
#endif

static unsigned long length_of(const char *text)
{
	unsigned long length = 0;
	while (text[length])
		++length;
	return length;
}

static void put_text(const char *text)
{
	put(text, length_of(text));
}

static void put_number(uint64_t value)
{
	char digits[24];
	int at = 23;
	digits[at] = 0;
	do
		digits[--at] = (char)('0' + value % 10);
	while (value /= 10);
	put_text(digits + at);
}

// Nanoseconds with three decimals, from picoseconds.
static void put_fixed(uint64_t picoseconds)
{
	put_number(picoseconds / 1000);
	char fraction[5] = {'.', (char)('0' + picoseconds / 100 % 10), (char)('0' + picoseconds / 10 % 10), (char)('0' + picoseconds % 10), 0};
	put_text(fraction);
}

static int parse(const char *text)
{
	int value = 0;
	while (*text >= '0' && *text <= '9')
		value = value * 10 + (*text++ - '0');
	return value;
}

static int bench_main(int count, char **arguments)
{
	const int runs = count > 1 ? parse(arguments[1]) : 30;
	const int calls = count > 2 ? parse(arguments[2]) : 200;
	volatile uint64_t sink = 0;
	uint64_t start = now_ns();
	sink += BENCH_FUNCTION();
	put_text("first ");
	put_fixed((now_ns() - start) * 1000);
	put_text("\n");
	for (int run = 0; run < runs; ++run) {
		start = now_ns();
		for (int i = 0; i < calls; ++i)
			sink += BENCH_FUNCTION();
		const uint64_t elapsed = now_ns() - start;
		put_text("run ");
		put_number((uint64_t)run);
		put_text(" ");
		put_fixed(elapsed * 1000 / (uint64_t)calls);
		put_text("\n");
	}
	put_text("checksum ");
	put_number(sink);
	put_text("\n");
	return 0;
}

#ifdef FREESTANDING
__attribute__((used)) static void start_c(long *stack)
{
	const int count = (int)stack[0];
	char **arguments = (char **)(stack + 1);
	system_call(231, bench_main(count, arguments), 0, 0); // exit_group
}

__asm__(".globl _start\n_start:\n\txor %ebp, %ebp\n\tmov %rsp, %rdi\n\tand $-16, %rsp\n\tcall start_c\n\thlt\n");
#else
int main(int count, char **arguments)
{
	return bench_main(count, arguments);
}
#endif
