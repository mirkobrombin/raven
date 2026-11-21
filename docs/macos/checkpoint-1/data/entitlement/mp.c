#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#define N 64
static volatile uint64_t data[N], flag[N];
static volatile int start, stop;
static long bad;
static void *writer(void *u) { for (long it = 0; !stop; it++) { while (start != 1 && !stop); for (int i = 0; i < N; i++) { data[i] = it + 1; flag[i] = it + 1; } start = 2; } return u; }
int main(void) {
    pthread_t w; pthread_create(&w, 0, writer, 0);
    long rounds = 300000;
    for (long it = 0; it < rounds; it++) {
        start = 1;
        for (int i = 0; i < N; i++) { uint64_t f = flag[i]; uint64_t d = data[i]; if (d < f) bad++; }
        while (start != 2); 
    }
    stop = 1; start = 1; pthread_join(w, 0);
    printf("MP litmus: %ld violations (data older than flag) in %ld x %d reads\n", bad, rounds, N);
    return 0;
}
