// Item 5: indirect call baseline vs in-process gate trampolines.
#include "bench.h"
typedef struct { uint64_t gpr[12], sp, host_sp; void (*fn)(void *); uint64_t pad, d[8], sys[3]; } ctx_t;
void empty_fn(void); void gate_gpr(ctx_t *); void gate_full(ctx_t *);
static volatile uint64_t sink;
static void host_handler(void *c) { sink++; }
enum { RUNS = 30 };
static const uint64_t ITERS = 1000000;
int main(void) {
    set_qos(); reset_jsonl("gate");
    static uint8_t hstack[65536] __attribute__((aligned(16)));
    ctx_t c = {0}; c.host_sp = (uint64_t)(hstack + sizeof hstack); c.fn = host_handler;
    void (*volatile pe)(void) = empty_fn;
    void (*volatile pg)(ctx_t *) = gate_gpr;
    void (*volatile pf)(ctx_t *) = gate_full;
    void (*volatile ph)(void *) = host_handler;
    double s[RUNS];
    struct { const char *n; int k; } V[] = { {"indirect_call_empty", 0}, {"indirect_call_c_handler", 1}, {"gate_gpr_stackswitch", 2}, {"gate_gpr_simd_flags", 3} };
    for (int v = 0; v < 4; v++) {
        for (int r = -3; r < RUNS; r++) {
            uint64_t a = now_ns();
            switch (V[v].k) {
            case 0: for (uint64_t i = 0; i < ITERS; i++) pe(); break;
            case 1: for (uint64_t i = 0; i < ITERS; i++) ph(&c); break;
            case 2: for (uint64_t i = 0; i < ITERS; i++) pg(&c); break;
            case 3: for (uint64_t i = 0; i < ITERS; i++) pf(&c); break;
            }
            if (r >= 0) s[r] = (double)(now_ns() - a) / ITERS;
        }
        report("gate", V[v].n, "ns_per_call", s, RUNS, "1e6 calls per run, loop overhead included");
    }
    return 0;
}
