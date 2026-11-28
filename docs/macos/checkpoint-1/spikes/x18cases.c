// Each custom-x18 case in its own child: mode and x18 checked with os_custom_x18_abi_enabled(), never by disabling blindly.
#include <os/arch/arm64.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define S 0x5eedull
static inline void set_x18(uint64_t v) { __asm__ volatile("mov x18, %0" ::"r"(v)); }
static inline uint64_t get_x18(void) { uint64_t v; __asm__ volatile("mov %0, x18" : "=r"(v)); return v; }

// Child exit codes: 0 mode kept and x18 kept, 1 mode kept x18 lost, 2 mode lost.
static int verdict(void) {
	uint64_t x = get_x18();
	if (!os_custom_x18_abi_enabled()) return 2;
	return x == S ? 0 : 1;
}
static void on_trap(int s, siginfo_t *i, void *u) { (void)s; (void)i; ((ucontext_t *)u)->uc_mcontext->__ss.__pc += 4; }

static mach_port_t port;
static void *server(void *u) {
	(void)u;
	struct { mach_msg_header_t h; char b[2048]; } m, r;
	for (;;) {
		if (mach_msg(&m.h, MACH_RCV_MSG, 0, sizeof m, port, 0, 0)) return 0;
		typedef struct { mach_msg_header_t h; mach_msg_body_t b; mach_msg_port_descriptor_t t, k; NDR_record_t n; exception_type_t e; mach_msg_type_number_t nc; int64_t c[2]; int f; mach_msg_type_number_t cnt; uint32_t st[ARM_THREAD_STATE64_COUNT]; } rq_t;
		typedef struct { mach_msg_header_t h; NDR_record_t n; kern_return_t ret; int f; mach_msg_type_number_t cnt; uint32_t st[ARM_THREAD_STATE64_COUNT]; } rp_t;
		rq_t *q = (rq_t *)&m; rp_t *p = (rp_t *)&r;
		arm_thread_state64_t st; memcpy(&st, q->st, sizeof st); st.__pc += 4;
		memset(p, 0, sizeof *p);
		p->h.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(q->h.msgh_bits), 0);
		p->h.msgh_remote_port = q->h.msgh_remote_port; p->h.msgh_size = sizeof *p; p->h.msgh_id = q->h.msgh_id + 100;
		p->n = NDR_record; p->f = ARM_THREAD_STATE64; p->cnt = ARM_THREAD_STATE64_COUNT; memcpy(p->st, &st, sizeof st);
		mach_msg(&p->h, MACH_SEND_MSG, p->h.msgh_size, 0, 0, 0, 0);
	}
}

static int run(const char *c) {
	if (!strcmp(c, "spin3s")) { // pure preemption: no kernel entry by this thread, other threads load all cores
		os_set_custom_x18_abi_enabled(true); set_x18(S);
		for (volatile long i = 0; i < 3000000000L; i++) if (get_x18() != S || !os_custom_x18_abi_enabled()) return verdict();
		return verdict();
	}
	if (!strcmp(c, "signal")) {
		struct sigaction sa = {0}; sa.sa_sigaction = on_trap; sa.sa_flags = SA_SIGINFO; sigaction(SIGTRAP, &sa, 0);
		os_set_custom_x18_abi_enabled(true); set_x18(S); __asm__ volatile("brk #1"); return verdict();
	}
	if (!strcmp(c, "mach")) {
		mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
		mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
		pthread_t t; pthread_create(&t, 0, server, 0);
		thread_set_exception_ports(mach_thread_self(), EXC_MASK_BREAKPOINT, port, EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
		os_set_custom_x18_abi_enabled(true); set_x18(S); __asm__ volatile("brk #1"); return verdict();
	}
	os_set_custom_x18_abi_enabled(true); set_x18(S);
	if (!strcmp(c, "malloc")) { volatile void *p = malloc(64); (void)p; }
	else if (!strcmp(c, "printf")) { printf("printf in custom mode\n"); fflush(stdout); }
	else if (!strcmp(c, "getpid")) { (void)getpid(); }
	else if (!strcmp(c, "enable_twice")) { os_set_custom_x18_abi_enabled(true); }
	return 3 + verdict(); // 3/4/5: call returned normally
}

static void *burn(void *u) { (void)u; for (;;) __asm__ volatile(""); }

int main(int argc, char **argv) {
	if (argc > 2) return run(argv[2]); // child entry: x18cases child CASE
	const char *cases[] = {"spin3s", "signal", "mach", "malloc", "printf", "getpid", "enable_twice"};
	for (int i = 0; i < 7; i++) for (int rep = 0; rep < 3; rep++) {
		pid_t p = fork();
		if (!p) {
			if (!strcmp(cases[i], "spin3s")) { pthread_t t; for (int k = 0; k < 20; k++) pthread_create(&t, 0, burn, 0); }
			execl(argv[0], argv[0], "child", cases[i], (char *)0);
			_exit(99);
		}
		int st; waitpid(p, &st, 0);
		const char *meaning = "?";
		if (WIFSIGNALED(st)) meaning = "killed by signal";
		else switch (WEXITSTATUS(st)) {
			case 0: meaning = "mode kept, x18 kept"; break; case 1: meaning = "mode kept, x18 LOST"; break; case 2: meaning = "mode LOST"; break;
			case 3: meaning = "call returned, mode+x18 kept"; break; case 4: meaning = "call returned, x18 LOST"; break; case 5: meaning = "call returned, mode LOST"; break;
		}
		printf("%-14s rep%d: %s %d -> %s\n", cases[i], rep, WIFSIGNALED(st) ? "signal" : "exit", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st), meaning);
		fflush(stdout);
	}
	return 0;
}
