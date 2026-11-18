// entry.c's loop compiled for this host, run as many times as translate_bench runs its x64 build: built for arm64 it gives the native
// speed, built for x86_64 it runs under Rosetta.
#include <pthread/qos.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

unsigned int raven_entry(void);

int main(int argc, char **argv)
{
	const int runs = argc > 1 ? atoi(argv[1]) : 30;
	const int calls = argc > 2 ? atoi(argv[2]) : 2000;
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	unsigned int check = 0;
	for (int warm = 0; warm < 100; ++warm)
		check += raven_entry();
	printf("run,ns_per_call\n");
	for (int run = 0; run < runs; ++run) {
		const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
		for (int i = 0; i < calls; ++i)
			check += raven_entry();
		const uint64_t end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
		printf("%d,%.3f\n", run, (double)(end - start) / calls);
	}
	fprintf(stderr, "check %u\n", check);
	return 0;
}
