// x64 guest code for the vCPU tests, called as raven_entry(role, shared, n).
//   role 0, 1: the writer and the reader of a message-passing litmus on shared[0] (x) and shared[16] (y), started together through
//              the counter at shared[32]; the reader returns how often it saw y's new value with x's old one, which TSO forbids, and
//              leaves its loop count at shared[33], proof that it ran while the writer wrote.
//   role 2:    n steps of a 64-bit LCG from n, a long run of translated code with no exit.
//   role 3:    calls patch_target, rewrites the immediate it returns to n, and returns what a second call returns: n, unless a stale
//              translation runs.
//   role 4:    calls patch_target until it returns something else than at first, at most n times; returns how many times it did.
//              shared[48] says it is running.
//   role 5:    waits for role 4 to run, spins n steps, and rewrites patch_target's immediate to shared[50].
#include <stdint.h>

// mov eax, imm32; ret, alone on its page, so writing it touches no other code.
uint32_t patch_target(void);
__asm__(".text\n.p2align 12\n.globl patch_target\npatch_target:\n\tmovl $1, %eax\n\tret\n.p2align 12\n");

static volatile uint32_t *patch_immediate(void)
{
	return (volatile uint32_t *)((volatile uint8_t *)patch_target + 1);
}

static void start_together(volatile uint64_t *go)
{
	uint64_t arrived = __atomic_fetch_add((uint64_t *)go, 1, __ATOMIC_SEQ_CST) + 1;
	while (arrived < 2)
		arrived = __atomic_load_n((uint64_t *)go, __ATOMIC_ACQUIRE);
}

uint64_t raven_entry(uint64_t role, volatile uint64_t *shared, uint64_t n)
{
	volatile uint64_t *x = shared, *y = shared + 16, *go = shared + 32;
	if (role == 0) {
		start_together(go);
		for (uint64_t i = 1; i <= n; ++i) {
			*x = i;
			*y = i;
		}
		return 0;
	}
	if (role == 1) {
		start_together(go);
		uint64_t violations = 0, loops = 0, a, b;
		do {
			a = *y;
			b = *x;
			violations += b < a;
			++loops;
		} while (a < n);
		go[1] = loops;
		return violations;
	}
	if (role == 3) {
		patch_target();
		*patch_immediate() = (uint32_t)n;
		return patch_target();
	}
	if (role == 4) {
		const uint32_t first = patch_target();
		uint64_t count = 0;
		shared[48] = 1;
		while (patch_target() == first && ++count < n)
			;
		return count;
	}
	if (role == 5) {
		while (!shared[48])
			;
		for (volatile uint64_t i = 0; i < n; i = i + 1)
			;
		*patch_immediate() = (uint32_t)shared[50];
		return 0;
	}
	uint64_t value = n;
	for (uint64_t i = 0; i < n; ++i)
		value = value * 6364136223846793005ull + 1442695040888963407ull;
	return value;
}
