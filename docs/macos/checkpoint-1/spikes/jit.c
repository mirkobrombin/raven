// Item 6: W^X. Capability probes run in forked children; timings in the parent.
// usage: jit <label>   (label names the signing variant, used in output names)
#include "bench.h"
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <libkern/OSCacheControl.h>

static const uint32_t RET42[] = { 0xd2800540 /* mov x0,#42 */, 0xd65f03c0 /* ret */ };
typedef int (*fn_t)(void);

static int probe(const char *name, int (*f)(void)) {
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) { int rc = f(); fflush(stdout); _exit(rc); }
    int st; waitpid(p, &st, 0);
    if (WIFEXITED(st)) printf("  %-34s exit=%d %s\n", name, WEXITSTATUS(st), WEXITSTATUS(st) == 0 ? "PASS" : "FAIL");
    else printf("  %-34s killed by signal %d (%s) FAIL\n", name, WTERMSIG(st), strsignal(WTERMSIG(st)));
    return st;
}

static int t_mapjit_rwx(void) {
    void *p = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) { printf("    mmap(MAP_JIT,RWX) errno=%d %s\n", errno, strerror(errno)); return 1; }
    pthread_jit_write_protect_np(0);
    memcpy(p, RET42, sizeof RET42);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(p, sizeof RET42);
    int r = ((fn_t)p)();
    printf("    MAP_JIT code returned %d\n", r);
    return r == 42 ? 0 : 2;
}
static int t_mmap_rwx_nojit(void) {
    void *p = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { printf("    mmap(RWX, no MAP_JIT) errno=%d %s\n", errno, strerror(errno)); return 1; }
    memcpy(p, RET42, sizeof RET42); sys_icache_invalidate(p, sizeof RET42);
    return ((fn_t)p)() == 42 ? 0 : 2;
}
static int t_mprotect_rwx(void) {
    void *p = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    int r = mprotect(p, 16384, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (r) { printf("    mprotect(RWX) errno=%d %s\n", errno, strerror(errno)); return 1; }
    memcpy(p, RET42, sizeof RET42); sys_icache_invalidate(p, sizeof RET42);
    return ((fn_t)p)() == 42 ? 0 : 2;
}
static int t_rw_then_rx(void) {
    void *p = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    memcpy(p, RET42, sizeof RET42);
    int r = mprotect(p, 16384, PROT_READ | PROT_EXEC);
    if (r) { printf("    mprotect(RX) errno=%d %s\n", errno, strerror(errno)); return 1; }
    sys_icache_invalidate(p, sizeof RET42);
    return ((fn_t)p)() == 42 ? 0 : 2;
}

enum { RUNS = 30 };
int main(int argc, char **argv) {
    set_qos();
    const char *label = argc > 1 ? argv[1] : "default";
    printf("== jit probes, signing variant: %s\n", label);
    probe("mmap MAP_JIT RWX + toggle + exec", t_mapjit_rwx);
    probe("mmap RWX without MAP_JIT + exec", t_mmap_rwx_nojit);
    probe("mprotect RW->RWX (non-JIT) + exec", t_mprotect_rwx);
    probe("mprotect RW->RX (non-JIT) + exec", t_rw_then_rx);
    if (strcmp(label, "adhoc_runtime_allowjit") && strcmp(label, "adhoc_noruntime")) return 0;

    char bench[64]; snprintf(bench, sizeof bench, "jit_%s", label); reset_jsonl(bench);
    uint8_t *p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    CHECK(p != MAP_FAILED, "MAP_JIT");
    double s[RUNS]; const int N = 100000;
    for (int r = -3; r < RUNS; r++) {
        uint64_t a = now_ns();
        for (int i = 0; i < N; i++) { pthread_jit_write_protect_np(0); pthread_jit_write_protect_np(1); }
        if (r >= 0) s[r] = (double)(now_ns() - a) / N;
    }
    report(bench, "jit_write_protect_toggle_pair", "ns", s, RUNS, "pthread_jit_write_protect_np(0)+(1), 1e5 pairs per run");
    for (int r = -3; r < RUNS; r++) {
        uint64_t a = now_ns();
        for (int i = 0; i < N; i++) { pthread_jit_write_protect_np(0); p[(i & 1023) * 64] = 0; pthread_jit_write_protect_np(1); }
        if (r >= 0) s[r] = (double)(now_ns() - a) / N;
    }
    report(bench, "toggle_pair_plus_store", "ns", s, RUNS, "toggle pair with one store into JIT page");
    pthread_jit_write_protect_np(0); memset(p, 0, 1 << 20); pthread_jit_write_protect_np(1);
    size_t sizes[] = { 4096, 16384, 1 << 20 };
    for (int k = 0; k < 3; k++) {
        int n = sizes[k] == (1 << 20) ? 1000 : 100000;
        for (int r = -3; r < RUNS; r++) {
            uint64_t a = now_ns();
            for (int i = 0; i < n; i++) sys_icache_invalidate(p, sizes[k]);
            if (r >= 0) s[r] = (double)(now_ns() - a) / n;
        }
        char m[64]; snprintf(m, sizeof m, "sys_icache_invalidate_%zuB", sizes[k]);
        report(bench, m, "ns", s, RUNS, sizes[k] == (1 << 20) ? "1e3 calls per run, clean lines" : "1e5 calls per run, clean lines");
    }
    // invalidate after actually writing the range (dirty dcache lines)
    for (int k = 0; k < 3; k++) {
        int n = sizes[k] == (1 << 20) ? 200 : 20000;
        for (int r = -3; r < RUNS; r++) {
            uint64_t tot = 0;
            for (int i = 0; i < n; i++) {
                pthread_jit_write_protect_np(0); memset(p, i, sizes[k]); pthread_jit_write_protect_np(1);
                uint64_t a = now_ns(); sys_icache_invalidate(p, sizes[k]); tot += now_ns() - a;
            }
            if (r >= 0) s[r] = (double)tot / n;
        }
        char m[64]; snprintf(m, sizeof m, "sys_icache_invalidate_dirty_%zuB", sizes[k]);
        report(bench, m, "ns", s, RUNS, "after memset of the range; per-call timing (24MHz clock, 41.7ns tick)");
    }
    return 0;
}
