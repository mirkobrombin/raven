// Item 9: does x18 survive (a) voluntary switch, (b) preemption, (c) signal, (d) Mach exception?
#include "bench.h"
#include <signal.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include "mach_exc.h"

uint64_t x18_get(void); uint64_t x18_spin(uint64_t s, uint64_t iters, uint64_t *changes);
uint64_t x18_brk(uint64_t s); uint64_t x18_load(uint64_t s, void *p);
uint64_t x18_call(uint64_t s, void (*fn)(uint64_t), uint64_t arg);
uint64_t x18_rawsvc(uint64_t s); void x18_noop(uint64_t);
extern boolean_t mach_exc_server(mach_msg_header_t *, mach_msg_header_t *);

#define S1 0x5eed5eed12345678ULL
#define S2 0x0badc0de0badc0deULL
static volatile uint64_t seen_x18, seen_count;
static volatile int rewrite;   // handler writes S2 into saved x18
static int fails;
static void verdict(const char *what, uint64_t got, uint64_t want) {
    int ok = got == want; if (!ok) fails++;
    printf("  %-62s x18=0x%016llx %s\n", what, (unsigned long long)got, ok ? "PRESERVED" : (got == 0 ? "ZEROED" : "CHANGED"));
}

static void f_yield(uint64_t n) { for (uint64_t i = 0; i < n; i++) sched_yield(); }
static void f_usleep(uint64_t n) { for (uint64_t i = 0; i < n; i++) usleep(1000); }
static int pfd[2];
static void f_pipe(uint64_t n) { char c; for (uint64_t i = 0; i < n; i++) read(pfd[0], &c, 1); }
static void *pipe_writer(void *p) { for (int i = 0; i < 200; i++) { usleep(500); write(pfd[1], "x", 1); } return NULL; }

static void on_sig(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_; seen_x18 = uc->uc_mcontext->__ss.__x[18]; seen_count++;
    if (sig == SIGTRAP || sig == SIGBUS || sig == SIGSEGV) uc->uc_mcontext->__ss.__pc += 4;
    if (rewrite) uc->uc_mcontext->__ss.__x[18] = S2;
}

kern_return_t catch_mach_exception_raise(mach_port_t p, mach_port_t th, mach_port_t tk, exception_type_t e, mach_exception_data_t c, mach_msg_type_number_t n) { return KERN_FAILURE; }
kern_return_t catch_mach_exception_raise_state(mach_port_t p, exception_type_t e, const mach_exception_data_t c, mach_msg_type_number_t n, int *f, const thread_state_t os, mach_msg_type_number_t osc, thread_state_t ns, mach_msg_type_number_t *nsc) { return KERN_FAILURE; }
kern_return_t catch_mach_exception_raise_state_identity(mach_port_t port, mach_port_t thread, mach_port_t task, exception_type_t exc,
    mach_exception_data_t code, mach_msg_type_number_t codeCnt, int *flavor, thread_state_t os, mach_msg_type_number_t oc, thread_state_t ns, mach_msg_type_number_t *nc) {
    memcpy(ns, os, oc * sizeof(natural_t)); *nc = oc;
    arm_thread_state64_t *ts = (arm_thread_state64_t *)ns;
    seen_x18 = ts->__x[18]; seen_count++;
    ts->__pc += 4;
    if (rewrite) ts->__x[18] = S2;
    mach_port_deallocate(mach_task_self(), thread); mach_port_deallocate(mach_task_self(), task);
    return KERN_SUCCESS;
}
static void *server(void *p) { mach_msg_server(mach_exc_server, 4096, (mach_port_t)(uintptr_t)p, 0); return NULL; }

static volatile int spin_stop;
static void *burner(void *p) { volatile uint64_t x = 0; while (!spin_stop) x++; return NULL; }
typedef struct { uint64_t iters, changes, final; } spin_t;
static void *spinner(void *p) { spin_t *s = p; s->final = x18_spin(S1, s->iters, &s->changes); return NULL; }

static pthread_t victim; static volatile int victim_ready;
static spin_t vspin;
static void *victim_fn(void *p) { victim_ready = 1; vspin.final = x18_spin(S1, vspin.iters, &vspin.changes); return NULL; }

int main(void) {
    printf("x18 sanity: initial x18 in main thread = 0x%llx\n", (unsigned long long)x18_get());
    printf("(0) controls\n");
    verdict("x18_call(no-op function, no kernel entry)  [control]", x18_call(S1, x18_noop, 0), S1);
    { uint64_t c = 0; verdict("x18_spin 1e6 iterations, no kernel entry  [control]", x18_spin(S1, 1000000, &c), S1); }
    verdict("raw svc getpid (no libc, non-blocking syscall)", x18_rawsvc(S1), S1);
    printf("(a) voluntary context switches\n");
    verdict("x18_call(sched_yield x 10000)", x18_call(S1, f_yield, 10000), S1);
    verdict("x18_call(usleep(1ms) x 100)", x18_call(S1, f_usleep, 100), S1);
    pipe(pfd); pthread_t pw; pthread_create(&pw, NULL, pipe_writer, NULL);
    verdict("x18_call(blocking read() on pipe x 200)", x18_call(S1, f_pipe, 200), S1);
    pthread_join(pw, NULL);

    printf("(b) preemption under load: 3x ncpu threads spinning with x18=sentinel for ~3 s\n");
    int ncpu = 10; size_t sz = sizeof ncpu; sysctlbyname("hw.ncpu", &ncpu, &sz, NULL, 0);
    int nt = 3 * ncpu; pthread_t th[64]; spin_t sp[64];
    struct rusage r0, r1; getrusage(RUSAGE_SELF, &r0);
    uint64_t t0 = now_ns();
    for (int i = 0; i < nt; i++) { sp[i].iters = 300000000ULL; sp[i].changes = 0; pthread_create(&th[i], NULL, spinner, &sp[i]); }
    uint64_t tot = 0; int bad = 0;
    for (int i = 0; i < nt; i++) { pthread_join(th[i], NULL); tot += sp[i].changes; bad += sp[i].final != S1; }
    getrusage(RUSAGE_SELF, &r1);
    printf("  threads=%d wall=%.2fs involuntary_ctx_switches(process)=%ld voluntary=%ld; iterations where x18!=sentinel: %llu; threads ending with wrong x18: %d\n",
           nt, (now_ns() - t0) / 1e9, r1.ru_nivcsw - r0.ru_nivcsw, r1.ru_nvcsw - r0.ru_nvcsw, (unsigned long long)tot, bad);
    if (tot || bad) fails++;
    printf("  %-62s %s\n", "preemption", (tot || bad) ? "x18 NOT preserved" : "PRESERVED");

    printf("(c) POSIX signals\n");
    struct sigaction sa = {0}; sa.sa_sigaction = on_sig; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGSEGV, &sa, NULL); sigaction(SIGUSR1, &sa, NULL);
    void *pn = mmap(NULL, 16384, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint64_t v = x18_brk(S1); printf("    handler saw uc x18=0x%016llx\n", (unsigned long long)seen_x18); verdict("brk -> SIGTRAP handler -> return", v, S1);
    v = x18_load(S1, pn); printf("    handler saw uc x18=0x%016llx\n", (unsigned long long)seen_x18); verdict("PROT_NONE load -> SIGBUS/SEGV handler -> return", v, S1);
    rewrite = 1; v = x18_brk(S1); rewrite = 0; verdict("brk; handler writes uc x18=S2 (expect S2 if honoured)", v, S2);
    // async signals delivered to a spinning thread
    vspin.iters = 2000000000ULL; seen_count = 0;
    pthread_create(&victim, NULL, victim_fn, NULL);
    while (!victim_ready) ;
    for (int i = 0; i < 2000; i++) { pthread_kill(victim, SIGUSR1); usleep(200); }
    pthread_join(victim, NULL);
    printf("  async SIGUSR1 x%llu delivered to spinning thread: changes=%llu final=0x%016llx last uc x18=0x%016llx %s\n",
           (unsigned long long)seen_count, (unsigned long long)vspin.changes, (unsigned long long)vspin.final, (unsigned long long)seen_x18,
           (vspin.changes || vspin.final != S1) ? "NOT preserved" : "PRESERVED");
    if (vspin.changes || vspin.final != S1) fails++;

    printf("(d) Mach exception round-trip (thread exception port, EXCEPTION_STATE_IDENTITY)\n");
    signal(SIGTRAP, SIG_DFL); signal(SIGBUS, SIG_DFL); signal(SIGSEGV, SIG_DFL);
    mach_port_t port; mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
    CHECK(thread_set_exception_ports(mach_thread_self(), EXC_MASK_BREAKPOINT | EXC_MASK_BAD_ACCESS, port,
          EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64) == KERN_SUCCESS, "set ports");
    pthread_t st; pthread_create(&st, NULL, server, (void *)(uintptr_t)port);
    v = x18_brk(S1); printf("    server saw x18=0x%016llx\n", (unsigned long long)seen_x18); verdict("brk -> Mach exception -> reply", v, S1);
    v = x18_load(S1, pn); printf("    server saw x18=0x%016llx\n", (unsigned long long)seen_x18); verdict("PROT_NONE load -> Mach exception -> reply", v, S1);
    rewrite = 1; v = x18_brk(S1); rewrite = 0; verdict("brk; server writes new_state x18=S2 (expect S2 if honoured)", v, S2);

    printf("RESULT x18: %s (%d failing checks)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
