#include <os/arch/arm64.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
static inline uint64_t tpidr(void) { uint64_t v; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v)); return v; }
static long trap(long num, long a0) { register long x16 __asm__("x16") = num; register long x0 __asm__("x0") = a0; __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x16) : "memory", "cc", "x1"); return x0; }
int main(int argc, char **argv) {
	long num = strtol(argv[1], 0, 0), n = strtol(argv[2], 0, 0);
	uint64_t before = tpidr();
	os_set_custom_x18_abi_enabled(true);
	uint64_t on = tpidr();
	__asm__ volatile("mov x18, %0" :: "r"(0x5eedull) : "x18");
	long r = 0; for (long i = 0; i < n; i++) r = trap(num, 0);
	uint64_t x18; __asm__ volatile("mov %0, x18" : "=r"(x18));
	uint64_t after = tpidr();
	int still = os_custom_x18_abi_enabled();
	if (still) os_set_custom_x18_abi_enabled(false);
	printf("trap %ld x%ld ret=%ld tpidr before=%#llx on=%#llx after=%#llx mode_after=%d x18=%#llx\n", num, n, r, (unsigned long long)before, (unsigned long long)on, (unsigned long long)after, still, (unsigned long long)x18);
	return 0;
}
