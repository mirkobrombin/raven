// Item 4: Mach exception round-trip vs in-process POSIX signal round-trip.
// usage: exc <mach|sig> <brk|svc|fault> [task]
#include "bench.h"
#include <signal.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/thread_status.h>
#include "mach_exc.h"

extern char do_brk_insn[], do_badsvc_insn[], do_load_insn[];
uint64_t do_brk(void); uint64_t do_badsvc(void); uint64_t do_load(void *);
extern boolean_t mach_exc_server(mach_msg_header_t *, mach_msg_header_t *);

enum { RUNS = 30 };
static uint64_t ITERS = 10000;
static void *protnone;
static volatile uint64_t nexc, last_exc_type, last_code0, first_pc, first_pc_set;
static int g_kind;   // 0 brk, 1 svc, 2 fault

static void fixup(uint64_t *pc, uint64_t *x0) {
    if (!first_pc_set) { first_pc = *pc; first_pc_set = 1; }
    if (g_kind == 0 && *pc == (uint64_t)do_brk_insn) *pc += 4;
    else if (g_kind == 2 && *pc == (uint64_t)do_load_insn) { *pc += 4; *x0 = 42; }
}

kern_return_t catch_mach_exception_raise(mach_port_t p, mach_port_t th, mach_port_t tk, exception_type_t e,
                                         mach_exception_data_t c, mach_msg_type_number_t n) { return KERN_FAILURE; }
kern_return_t catch_mach_exception_raise_state(mach_port_t p, exception_type_t e, const mach_exception_data_t c,
    mach_msg_type_number_t n, int *f, const thread_state_t os, mach_msg_type_number_t osc, thread_state_t ns, mach_msg_type_number_t *nsc) { return KERN_FAILURE; }
kern_return_t catch_mach_exception_raise_state_identity(mach_port_t port, mach_port_t thread, mach_port_t task,
    exception_type_t exc, mach_exception_data_t code, mach_msg_type_number_t codeCnt, int *flavor,
    thread_state_t old_state, mach_msg_type_number_t old_cnt, thread_state_t new_state, mach_msg_type_number_t *new_cnt) {
    nexc++; last_exc_type = exc; last_code0 = codeCnt ? code[0] : 0;
    CHECK(*flavor == ARM_THREAD_STATE64, "flavor %d", *flavor);
    arm_thread_state64_t *ts = (arm_thread_state64_t *)new_state;
    memcpy(new_state, old_state, old_cnt * sizeof(natural_t));
    *new_cnt = old_cnt;
    uint64_t pc = arm_thread_state64_get_pc(*ts);
    fixup(&pc, &ts->__x[0]);
    arm_thread_state64_set_pc_fptr(*ts, (void *)pc);
    mach_port_deallocate(mach_task_self(), thread);
    mach_port_deallocate(mach_task_self(), task);
    return KERN_SUCCESS;
}

static void *server(void *p) {
    set_qos();
    mach_msg_server(mach_exc_server, 4096, (mach_port_t)(uintptr_t)p, 0);
    return NULL;
}

static void sig_handler(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    nexc++; last_exc_type = sig; last_code0 = si->si_code;
    uint64_t pc = arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
    fixup(&pc, &uc->uc_mcontext->__ss.__x[0]);
    arm_thread_state64_set_pc_fptr(uc->uc_mcontext->__ss, (void *)pc);
}

static uint64_t trigger(void) {
    switch (g_kind) { case 0: return do_brk(); case 1: return do_badsvc(); default: return do_load(protnone); }
}

int main(int argc, char **argv) {
    set_qos();
    int mach = argc > 1 && !strcmp(argv[1], "mach");
    const char *k = argc > 2 ? argv[2] : "brk";
    int task_level = argc > 3 && !strcmp(argv[3], "task");
    g_kind = !strcmp(k, "brk") ? 0 : !strcmp(k, "svc") ? 1 : 2;
    protnone = mmap(NULL, 16384, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mach) {
        mach_port_t port;
        CHECK(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS, "alloc");
        CHECK(mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS, "insert");
        exception_mask_t mask = EXC_MASK_BREAKPOINT | EXC_MASK_BAD_ACCESS | EXC_MASK_SOFTWARE | EXC_MASK_BAD_INSTRUCTION | EXC_MASK_SYSCALL | EXC_MASK_ARITHMETIC;
        exception_behavior_t beh = EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES;
        kern_return_t kr = task_level
            ? task_set_exception_ports(mach_task_self(), mask, port, beh, ARM_THREAD_STATE64)
            : thread_set_exception_ports(mach_thread_self(), mask, port, beh, ARM_THREAD_STATE64);
        CHECK(kr == KERN_SUCCESS, "set_exception_ports %d", kr);
        pthread_t t; pthread_create(&t, NULL, server, (void *)(uintptr_t)port);
    } else {
        struct sigaction sa = {0};
        sa.sa_sigaction = sig_handler; sa.sa_flags = SA_SIGINFO;
        sigaction(SIGTRAP, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGSYS, &sa, NULL); sigaction(SIGILL, &sa, NULL);
    }
    // probe once and describe what was delivered
    uint64_t r = trigger();
    printf("mode=%s kind=%s level=%s: first trigger returned x0=%llu; deliveries=%llu last=%llu code0=0x%llx first_pc=0x%llx insn=0x%llx\n",
           mach ? "mach" : "signal", k, mach ? (task_level ? "task" : "thread") : "-", (unsigned long long)r, (unsigned long long)nexc,
           (unsigned long long)last_exc_type, (unsigned long long)last_code0, (unsigned long long)first_pc,
           (unsigned long long)(g_kind == 0 ? (uint64_t)do_brk_insn : g_kind == 1 ? (uint64_t)do_badsvc_insn : (uint64_t)do_load_insn));
    if (nexc != 1) { printf("RESULT: no single delivery per trigger (deliveries=%llu); not timing\n", (unsigned long long)nexc); return 1; }
    double s[RUNS];
    for (int i = 0; i < 1000; i++) trigger();
    for (int run = 0; run < RUNS; run++) {
        uint64_t before = nexc, a = now_ns();
        for (uint64_t i = 0; i < ITERS; i++) trigger();
        s[run] = (double)(now_ns() - a) / ITERS;
        CHECK(nexc - before == ITERS, "lost deliveries");
    }
    char bench[64], metric[64];
    snprintf(bench, sizeof bench, "exc_%s", mach ? "mach" : "signal");
    snprintf(metric, sizeof metric, "%s%s", k, mach && task_level ? "_task" : "");
    report(bench, metric, "ns", s, RUNS, "1e4 round-trips per run");
    return 0;
}
