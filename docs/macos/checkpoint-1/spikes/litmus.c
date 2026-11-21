// Native arm64 and x86_64-under-Rosetta MP/SB litmus (item 1 baselines).
// Same algorithm as the guest kernels in kernels.S.
#include "bench.h"

#if defined(__aarch64__)
uint64_t mp_writer(uint64_t *x, uint64_t *y, uint64_t n, uint64_t *go);
uint64_t mp_reader(uint64_t *x, uint64_t *y, uint64_t n, uint64_t *go);
uint64_t sb_thread(uint64_t *mine, uint64_t *other, uint8_t *res, uint64_t n, uint64_t *bar);
#define ARCH "arm64"
#else
#define ARCH "x86_64"
static void start_sync(uint64_t *go) {
    __atomic_fetch_add(go, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(go, __ATOMIC_ACQUIRE) < 2) ;
}
static uint64_t mp_writer(uint64_t *x, uint64_t *y, uint64_t n, uint64_t *go) {
    start_sync(go);
    for (uint64_t i = 1; i <= n; i++)
        __asm__ volatile("movq %0, (%1)\n\tmovq %0, (%2)" :: "r"(i), "r"(x), "r"(y) : "memory");
    return 0;
}
static uint64_t mp_reader(uint64_t *x, uint64_t *y, uint64_t n, uint64_t *go) {
    start_sync(go);
    uint64_t v = 0, loops = 0, a, b;
    do {
        __asm__ volatile("movq (%2), %0\n\tmovq (%3), %1" : "=&r"(a), "=&r"(b) : "r"(y), "r"(x) : "memory");
        v += b < a; loops++;
    } while (a < n);
    go[1] = loops;
    return v;
}
static uint64_t sb_thread(uint64_t *mine, uint64_t *other, uint8_t *res, uint64_t n, uint64_t *bar) {
    uint64_t target = 0;
    for (uint64_t i = 0; i < n; i++) {
        target += 2;
        __atomic_fetch_add(bar, 1, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(bar, __ATOMIC_ACQUIRE) < target) ;
        uint64_t r;
        __asm__ volatile("movq $1, (%1)\n\tmovq (%2), %0" : "=&r"(r) : "r"(mine), "r"(other) : "memory");
        res[i] = (uint8_t)r;
        target += 2;
        __atomic_fetch_add(bar, 1, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(bar, __ATOMIC_ACQUIRE) < target) ;
        __asm__ volatile("movq $0, (%0)" :: "r"(mine) : "memory");
    }
    return 0;
}
#endif

enum { TRIALS = 30 };
static const uint64_t N = 2000000, NSB = 200000;
static uint64_t *X, *Y, *GO, *BAR; static uint8_t *R0, *R1;
static hbarrier_t hb;
static uint64_t viol[TRIALS], loops[TRIALS];

static void *thr(void *p) {
    int idx = (int)(intptr_t)p; set_qos();
    for (int t = 0; t < TRIALS; t++) {
        hbarrier_wait(&hb); hbarrier_wait(&hb);
        uint64_t r = idx ? mp_reader(X, Y, N, GO) : mp_writer(X, Y, N, GO);
        if (idx) { viol[t] = r; loops[t] = GO[1]; }
    }
    hbarrier_wait(&hb); hbarrier_wait(&hb);
    sb_thread(idx ? Y : X, idx ? X : Y, idx ? R1 : R0, NSB, BAR);
    hbarrier_wait(&hb);
    return NULL;
}

int main(void) {
    set_qos();
    printf("arch=%s proc_translated=%d\n", ARCH, is_translated());
    uint8_t *m = aligned_alloc(16384, 1 << 20);
    memset(m, 0, 1 << 20);
    X = (uint64_t *)(m + 0); Y = (uint64_t *)(m + 4096); GO = (uint64_t *)(m + 8192); BAR = (uint64_t *)(m + 12288);
    R0 = malloc(NSB); R1 = malloc(NSB);
    hbarrier_init(&hb, 3);
    pthread_t a, b;
    pthread_create(&a, NULL, thr, (void *)0); pthread_create(&b, NULL, thr, (void *)1);
    for (int t = 0; t < TRIALS; t++) {
        hbarrier_wait(&hb); *X = 0; *Y = 0; GO[0] = 0; GO[1] = 0; __atomic_thread_fence(__ATOMIC_SEQ_CST); hbarrier_wait(&hb);
    }
    hbarrier_wait(&hb); *X = 0; *Y = 0; *BAR = 0; memset(R0, 0xee, NSB); memset(R1, 0xee, NSB); __atomic_thread_fence(__ATOMIC_SEQ_CST); hbarrier_wait(&hb);
    hbarrier_wait(&hb);
    pthread_join(a, NULL); pthread_join(b, NULL);
    uint64_t sb = 0, tot = 0;
    for (uint64_t i = 0; i < NSB; i++) sb += (!R0[i] && !R1[i]);
    double dv[TRIALS], dl[TRIALS];
    for (int t = 0; t < TRIALS; t++) { dv[t] = (double)viol[t]; dl[t] = (double)loops[t]; tot += viol[t]; }
    char name[64]; snprintf(name, sizeof name, "litmus_native_%s%s", ARCH, is_translated() ? "_rosetta" : "");
    reset_jsonl(name);
    report(name, "mp_violations_per_2e6", "count", dv, TRIALS, "");
    report(name, "mp_reader_loops", "count", dl, TRIALS, "reader iterations while writer ran (overlap control)");
    printf("RESULT %s: MP violations total=%llu over %d x %llu writes; SB r0=r1=0 count=%llu of %llu iters\n",
           name, (unsigned long long)tot, TRIALS, (unsigned long long)N, (unsigned long long)sb, (unsigned long long)NSB);
    FILE *f = fopen(RESULTS_DIR "/litmus_summary.csv", "a");
    fprintf(f, "%s,%llu,%d,%llu,%llu,%llu\n", name, (unsigned long long)tot, TRIALS, (unsigned long long)N, (unsigned long long)sb, (unsigned long long)NSB);
    fclose(f);
    return 0;
}
