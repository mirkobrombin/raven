// Item 2: cost of ordering primitives a translator would insert.
#include "bench.h"

#define X(n) void bar_##n(uint64_t iters, uint64_t *buf);
#include "barriers_list.h"
#undef X
typedef void (*fn_t)(uint64_t, uint64_t *);
static struct { const char *name; fn_t f; } V[] = {
#define X(n) { #n, bar_##n },
#include "barriers_list.h"
#undef X
};
enum { NV = sizeof V / sizeof V[0], RUNS = 30 };
static const uint64_t ITERS = 1000000;   // x8 ops per iteration
static uint64_t *shared;
static hbarrier_t hb;
static double tsamp[2][RUNS];

static void spin_warm(uint64_t ms) { uint64_t e = now_ns() + ms * 1000000ULL; while (now_ns() < e) ; }
static double run_once(fn_t f, uint64_t *buf) {
    buf[0] = (uint64_t)buf;   // self pointer for chase variants
    uint64_t a = now_ns(); f(ITERS, buf); uint64_t b = now_ns();
    return (double)(b - a) / (ITERS * 8.0);
}

static void *contender(void *p) {
    int idx = (int)(intptr_t)p; set_qos();
    for (int v = 0; v < NV; v++) for (int r = 0; r < RUNS; r++) {
        hbarrier_wait(&hb);
        tsamp[idx][r] = run_once(V[v].f, shared);
        hbarrier_wait(&hb);
    }
    return NULL;
}

int main(void) {
    set_qos();
    reset_jsonl("barriers_1t"); reset_jsonl("barriers_2t");
    uint64_t *priv = aligned_alloc(16384, 16384);
    memset(priv, 0, 16384);
    for (int v = 0; v < NV; v++) { run_once(V[v].f, priv); }   // warm-up
    double s[RUNS];
    for (int v = 0; v < NV; v++) {
        spin_warm(100);   // let the core ramp up before each variant
        for (int r = 0; r < RUNS; r++) s[r] = run_once(V[v].f, priv);
        report("barriers_1t", V[v].name, "ns_per_op", s, RUNS, "1 thread, private line, 8e6 ops/run");
    }
    shared = aligned_alloc(16384, 16384); memset(shared, 0, 16384);
    hbarrier_init(&hb, 2);
    pthread_t t; pthread_create(&t, NULL, contender, (void *)1);
    for (int v = 0; v < NV; v++) {
        for (int r = 0; r < RUNS; r++) {
            hbarrier_wait(&hb);
            tsamp[0][r] = run_once(V[v].f, shared);
            hbarrier_wait(&hb);
        }
        for (int r = 0; r < RUNS; r++) s[r] = 0.5 * (tsamp[0][r] + tsamp[1][r]);
        report("barriers_2t", V[v].name, "ns_per_op", s, RUNS, "2 threads same 128B line, mean of both threads");
    }
    pthread_join(t, NULL);
    return 0;
}
