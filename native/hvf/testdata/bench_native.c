// Times bench_loop natively as hvf_bench times it translated: one first call of one iteration, then RUNS runs of one bench_loop(CALLS)
// each, printed as ns per workload call in hvf_bench's format.
#include <pthread/qos.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

uint64_t bench_loop(uint64_t calls);

int main(int argc, char **argv)
{
	const int runs = argc > 1 ? atoi(argv[1]) : 10;
	const uint64_t calls = argc > 2 ? strtoull(argv[2], NULL, 10) : 20;
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	uint64_t sink = bench_loop(1);
	printf("first %.3f\n", (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start));
	for (int run = 0; run < runs; ++run) {
		start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
		sink += bench_loop(calls);
		printf("run %d %.3f\n", run, (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / (double)calls);
	}
	printf("checksum %llu\n", (unsigned long long)sink);
	return 0;
}
