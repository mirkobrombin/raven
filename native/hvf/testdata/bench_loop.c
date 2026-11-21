// The caller loop of a workload, built into the x64 image the engines translate and into the native build they are compared with, so
// the loop runs as translated code in one and as native code in the other. BENCH_FUNCTION names the workload, BENCH_RESULT its type.
#include <stdint.h>

#ifndef BENCH_RESULT
#define BENCH_RESULT uint64_t
#endif

BENCH_RESULT BENCH_FUNCTION(void);

uint64_t bench_loop(uint64_t calls)
{
	uint64_t sum = 0;
	for (; calls; --calls)
		sum += BENCH_FUNCTION();
	return sum;
}
