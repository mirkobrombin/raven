// x18 under os_set_custom_x18_abi_enabled (macOS 26.4+): survival, toggle cost, accidental system calls.
#include "bench.h"
#include <os/arch/arm64.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define S 0x5eed5eed12345678ull
#define RUNS 30

static inline void set_x18(uint64_t v) { __asm__ volatile("mov x18, %0" ::"r"(v) : "x18"); }
static inline uint64_t get_x18(void) { uint64_t v; __asm__ volatile("mov %0, x18" : "=r"(v)); return v; }
static inline long raw_getpid(void) {
	register long x16 __asm__("x16") = 20;
	register long x0 __asm__("x0");
	__asm__ volatile("svc #0x80" : "=r"(x0) : "r"(x16) : "memory", "cc");
	return x0;
}
static inline long raw_yield(void) {
	// swtch_pri Mach trap (-60): a voluntary context switch without libc.
	register long x16 __asm__("x16") = -60;
	register long x0 __asm__("x0") = 0;
	__asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x16) : "memory", "cc");
	return x0;
}

static int fails;
static void check(const char *what, uint64_t got) {
	int ok = got == S;
	if (!ok) fails++;
	printf("  %-62s x18=0x%016llx %s\n", what, (unsigned long long)got, ok ? "PRESERVED" : "LOST");
}

// Runs fn with custom x18 mode on and x18=S, returns x18 seen afterwards, before leaving the mode.
static uint64_t in_custom(void (*fn)(void)) {
	os_set_custom_x18_abi_enabled(true);
	set_x18(S);
	fn();
	uint64_t v = get_x18();
	os_set_custom_x18_abi_enabled(false);
	return v;
}

static void f_none(void) { __asm__ volatile("" ::: "memory"); }
static void f_svc(void) { for (int i = 0; i < 1000; i++) raw_getpid(); }
static void f_yield(void) { for (int i = 0; i < 10000; i++) raw_yield(); }
static void f_brk(void) { __asm__ volatile("brk #1"); }

static void on_trap(int sig, siginfo_t *si, void *uc_) {
	(void)sig; (void)si;
	// The handler is macOS code: leave the custom mode around it as the header requires.
	ucontext_t *uc = uc_;
	uc->uc_mcontext->__ss.__pc += 4;
}

static volatile int spin_stop;
static volatile long spin_bad;
static void *spinner(void *u) {
	(void)u;
	os_set_custom_x18_abi_enabled(true);
	set_x18(S);
	long bad = 0;
	while (!spin_stop)
		if (get_x18() != S) { bad++; set_x18(S); }
	os_set_custom_x18_abi_enabled(false);
	__atomic_add_fetch(&spin_bad, bad, __ATOMIC_RELAXED);
	return 0;
}

static mach_port_t exc_port;
static void *exc_server(void *u) {
	(void)u;
	struct { mach_msg_header_t h; char body[2048]; } msg, reply;
	for (;;) {
		if (mach_msg(&msg.h, MACH_RCV_MSG, 0, sizeof msg, exc_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL)) return 0;
		// EXCEPTION_STATE_IDENTITY request (id 2403): advance pc past brk in the returned state.
		typedef struct { mach_msg_header_t h; mach_msg_body_t b; mach_msg_port_descriptor_t thread, task; NDR_record_t ndr; exception_type_t exc; mach_msg_type_number_t ncode; int64_t code[2]; int flavor; mach_msg_type_number_t cnt; uint32_t state[ARM_THREAD_STATE64_COUNT]; } req_t;
		typedef struct { mach_msg_header_t h; NDR_record_t ndr; kern_return_t ret; int flavor; mach_msg_type_number_t cnt; uint32_t state[ARM_THREAD_STATE64_COUNT]; } rep_t;
		req_t *rq = (req_t *)&msg;
		rep_t *rp = (rep_t *)&reply;
		arm_thread_state64_t st;
		memcpy(&st, rq->state, sizeof st);
		printf("    server saw x18=0x%016llx\n", (unsigned long long)st.__x[18]);
		st.__pc += 4;
		memset(rp, 0, sizeof *rp);
		rp->h.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(rq->h.msgh_bits), 0);
		rp->h.msgh_remote_port = rq->h.msgh_remote_port;
		rp->h.msgh_size = sizeof *rp;
		rp->h.msgh_id = rq->h.msgh_id + 100;
		rp->ndr = NDR_record;
		rp->flavor = ARM_THREAD_STATE64;
		rp->cnt = ARM_THREAD_STATE64_COUNT;
		memcpy(rp->state, &st, sizeof st);
		mach_msg(&rp->h, MACH_SEND_MSG, rp->h.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	}
}

static int accidental(const char *name, void (*fn)(void)) {
	pid_t p = fork();
	if (p == 0) {
		os_set_custom_x18_abi_enabled(true);
		set_x18(S);
		fn();
		os_set_custom_x18_abi_enabled(false);
		_exit(0);
	}
	int st = 0;
	waitpid(p, &st, 0);
	printf("  %-40s -> %s %d\n", name, WIFSIGNALED(st) ? "signal" : "exit", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
	return st;
}
static void a_malloc(void) { volatile void *p = malloc(64); (void)p; }
static void a_printf(void) { printf("hello from custom x18\n"); fflush(stdout); }
static void a_getpid(void) { (void)getpid(); }
static void a_pthread_self(void) { volatile void *p = pthread_self(); (void)p; }
static void a_double_enable(void) { os_set_custom_x18_abi_enabled(true); }

int main(int argc, char **argv) {
	set_qos(); setvbuf(stdout, 0, _IONBF, 0);
	if (argc > 1 && !strcmp(argv[1], "cost")) {
		reset_jsonl("x18c");
		static double s[RUNS];
		const int n = 100000;
		for (int r = 0; r < RUNS; r++) {
			uint64_t t0 = now_ns();
			for (int i = 0; i < n; i++) { os_set_custom_x18_abi_enabled(true); os_set_custom_x18_abi_enabled(false); }
			s[r] = (double)(now_ns() - t0) / n;
		}
		report("x18c", "toggle_pair", "ns_per_pair", s, RUNS, "1e5 enable+disable pairs per run");
		return 0;
	}
	printf("custom x18 enabled at start: %d\n", os_custom_x18_abi_enabled());
	printf("(0) control\n");
	check("empty function, no kernel entry", in_custom(f_none));
	printf("(a) syscalls and voluntary switches\n");
	check("raw svc getpid x1000", in_custom(f_svc));
	check("swtch_pri Mach trap x10000", in_custom(f_yield));
	printf("(b) preemption: 3x ncpu spinning threads for ~3 s\n");
	int nt = 30;
	pthread_t t[30];
	for (int i = 0; i < nt; i++) pthread_create(&t[i], 0, spinner, 0);
	sleep(3);
	spin_stop = 1;
	for (int i = 0; i < nt; i++) pthread_join(t[i], 0);
	printf("  iterations where x18 != sentinel: %ld -> %s\n", spin_bad, spin_bad ? "LOST" : "PRESERVED");
	if (spin_bad) fails++;
	printf("(c) POSIX signal (brk -> SIGTRAP handler -> return)\n");
	struct sigaction sa = {0};
	sa.sa_sigaction = on_trap;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGTRAP, &sa, 0);
	check("brk -> SIGTRAP -> return", in_custom(f_brk));
	signal(SIGTRAP, SIG_DFL);
	printf("(d) Mach exception (thread port, EXCEPTION_STATE_IDENTITY)\n");
	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &exc_port);
	mach_port_insert_right(mach_task_self(), exc_port, exc_port, MACH_MSG_TYPE_MAKE_SEND);
	pthread_t srv;
	pthread_create(&srv, 0, exc_server, 0);
	thread_set_exception_ports(mach_thread_self(), EXC_MASK_BREAKPOINT, exc_port, EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
	check("brk -> Mach exception -> reply", in_custom(f_brk));
	printf("(e) accidental calls into macOS code while in custom mode (child processes)\n");
	accidental("malloc", a_malloc);
	accidental("printf", a_printf);
	accidental("getpid (libc wrapper)", a_getpid);
	accidental("pthread_self", a_pthread_self);
	accidental("enable twice (header: must abort)", a_double_enable);
	printf("RESULT custom x18: %s (%d failing checks)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
