// x64 guest code for the vCPU tests, called as raven_entry(role, shared, n).
//   role 0, 1: the writer and the reader of a message-passing litmus on shared[0] (x) and shared[16] (y), started together through
//              the counter at shared[32]; the reader returns how often it saw y's new value with x's old one, which TSO forbids, and
//              leaves its loop count at shared[33], proof that it ran while the writer wrote.
//   role 2:    n steps of a 64-bit LCG from n, a long run of translated code with no exit.
#include <stdint.h>

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
	uint64_t value = n;
	for (uint64_t i = 0; i < n; ++i)
		value = value * 6364136223846793005ull + 1442695040888963407ull;
	return value;
}
