// Checks that decide the Hypervisor.framework model (checkpoint 1, open items):
//   tso     plain vs LDAPR/STLR loops, natively and in a vCPU with ACTLR_EL1.TSO clear/set
//   x18     x18 across host exits (EL1) and across EL0 svc/exceptions with host preemption
//   fault   translation fault taken and resolved by the guest's EL1 vector, timed
//   lowva   VA 0x10000 and 0x400000 through the guest's own page tables
//   stage2  data abort on an IPA with no stage-2 mapping, exit to host and resume, timed
//   multivm a second hv_vm_create in the same process
#include "bench.h"
#include <Hypervisor/Hypervisor.h>
#include <sys/mman.h>
#include <sched.h>

extern char v2_vectors[], v2_vectors_end[], v2_start[], v2_end[], v2_hvc[], v2_el0[], v2_el0_end[];
extern uint64_t mem_plain(uint64_t *buf, uint64_t n), mem_rcpc(uint64_t *buf, uint64_t n);
extern char fault_loop[], low_va[], x18_el1[], x18_el0_enter[], stage2_loop[];

#define HVCHK(x) do { hv_return_t _r = (x); if (_r != HV_SUCCESS) { fprintf(stderr, "FAIL %s -> 0x%x\n", #x, _r); exit(2); } } while (0)
#define G        0x40000000ULL
#define GSIZE    (16ULL << 20)
#define OFF_L1   0x1000
#define OFF_L2   0x2000
#define OFF_L3   0x3000
#define OFF_L2B  0x5000
#define OFF_L3C  0x6000
#define OFF_CODE 0x10000
#define OFF_DATA 0x100000
#define OFF_LOWF 0x190000
#define OFF_EL0  0x200000
#define FAULT_VA 0x48000000ULL
#define S2_VA    0x60000000ULL
#define DESC_TABLE 3ULL
#define DESC_PAGE  (3ULL | (1ULL << 10) | (3ULL << 8))
#define DESC_BLOCK (1ULL | (1ULL << 10) | (3ULL << 8))
#define AP_EL0     (1ULL << 6)
#define GA(sym) (G + OFF_CODE + (uint64_t)((char *)(sym) - v2_start))

static uint8_t *ram;

static void tables(void) {
	uint64_t *l1 = (uint64_t *)(ram + OFF_L1), *l2 = (uint64_t *)(ram + OFF_L2), *l3 = (uint64_t *)(ram + OFF_L3);
	uint64_t *l2b = (uint64_t *)(ram + OFF_L2B), *l3c = (uint64_t *)(ram + OFF_L3C);
	memset(ram + OFF_L1, 0, 0x6000);
	l1[1] = (G + OFF_L2) | DESC_TABLE;                                   // VA 1 GB .. 2 GB
	l2[0] = (G + OFF_L3) | DESC_TABLE;
	for (int i = 0; i < 512; i++) l3[i] = (G + (uint64_t)i * 0x1000) | DESC_PAGE;
	for (int i = 1; i < (int)(GSIZE >> 21); i++) l2[i] = (G + ((uint64_t)i << 21)) | DESC_BLOCK;
	l2[OFF_EL0 >> 21] |= AP_EL0;                                         // EL0 code and stack
	l2[(S2_VA - G) >> 21] = S2_VA | DESC_BLOCK;                          // IPA with no stage-2 mapping
	l1[0] = (G + OFF_L2B) | DESC_TABLE;                                  // VA 0 .. 1 GB
	l2b[0] = (G + OFF_L3C) | DESC_TABLE;
	l3c[0x10] = (G + OFF_LOWF) | DESC_PAGE;                              // VA 0x10000 -> one 4 KiB frame
	l2b[2] = (G + 0x400000) | DESC_BLOCK;                                // VA 0x400000 -> 2 MiB block
}

static void vm_setup(void) {
	HVCHK(hv_vm_create(NULL));
	ram = mmap(NULL, GSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	CHECK(ram != MAP_FAILED, "mmap");
	HVCHK(hv_vm_map(ram, G, GSIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
	memcpy(ram, v2_vectors, v2_vectors_end - v2_vectors);
	memcpy(ram + OFF_CODE, v2_start, v2_end - v2_start);
	memcpy(ram + OFF_EL0, v2_el0, v2_el0_end - v2_el0);
	tables();
}

typedef struct { hv_vcpu_t id; hv_vcpu_exit_t *exit; } vcpu_t;
static void vcpu_init(vcpu_t *v, int tso) {
	HVCHK(hv_vcpu_create(&v->id, &v->exit, NULL));
	HVCHK(hv_vcpu_set_vtimer_mask(v->id, true));
	HVCHK(hv_vcpu_set_reg(v->id, HV_REG_CPSR, 0x3c5));
	HVCHK(hv_vcpu_set_sys_reg(v->id, HV_SYS_REG_VBAR_EL1, G));
	HVCHK(hv_vcpu_set_sys_reg(v->id, HV_SYS_REG_SP_EL1, G + GSIZE - 0x1000));
	uint64_t tcr = 25 | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (1ULL << 23) | (1ULL << 32);
	HVCHK(hv_vcpu_set_sys_reg(v->id, HV_SYS_REG_MAIR_EL1, 0xFF));
	HVCHK(hv_vcpu_set_sys_reg(v->id, HV_SYS_REG_TCR_EL1, tcr));
	HVCHK(hv_vcpu_set_sys_reg(v->id, HV_SYS_REG_TTBR0_EL1, G + OFF_L1));
	HVCHK(hv_vcpu_set_sys_reg(v->id, HV_SYS_REG_SCTLR_EL1, 0x30d00800ULL | 1 | 4 | 0x1000));
	HVCHK(hv_vcpu_set_sys_reg(v->id, HV_SYS_REG_ACTLR_EL1, tso ? 2 : 0));
}

// Runs gpc(args) until hvc #0 (normal return) or hvc #2 (EL0 report). Exits for hvc #3 resume at once,
// optionally yielding the host thread; stage-2 data aborts skip the faulting instruction.
static uint64_t exits3, exits_s2, yields;
static int yield_every;
static uint64_t call(vcpu_t *v, uint64_t gpc, const uint64_t *a, int n, int *imm_out) {
	for (int i = 0; i < n; i++) HVCHK(hv_vcpu_set_reg(v->id, HV_REG_X0 + i, a[i]));
	HVCHK(hv_vcpu_set_reg(v->id, HV_REG_LR, GA(v2_hvc)));
	HVCHK(hv_vcpu_set_reg(v->id, HV_REG_PC, gpc));
	for (;;) {
		HVCHK(hv_vcpu_run(v->id));
		if (v->exit->reason != HV_EXIT_REASON_EXCEPTION) continue;
		uint64_t esr = v->exit->exception.syndrome, x0;
		uint32_t ec = (esr >> 26) & 0x3f, imm = esr & 0xffff;
		if (ec == 0x16 && imm == 3) {
			exits3++;
			if (yield_every && exits3 % yield_every == 0) { sched_yield(); yields++; }
			continue;
		}
		if (ec == 0x24) {
			exits_s2++;
			uint64_t pc; hv_vcpu_get_reg(v->id, HV_REG_PC, &pc); hv_vcpu_set_reg(v->id, HV_REG_PC, pc + 4);
			continue;
		}
		HVCHK(hv_vcpu_get_reg(v->id, HV_REG_X0, &x0));
		if (ec == 0x16 && (imm == 0 || imm == 2)) { if (imm_out) *imm_out = imm; return x0; }
		uint64_t x1 = 0, x2 = 0; hv_vcpu_get_reg(v->id, HV_REG_X1, &x1); hv_vcpu_get_reg(v->id, HV_REG_X2, &x2);
		fprintf(stderr, "unexpected exit ESR=0x%llx ec=0x%x imm=0x%x x0=0x%llx x1=0x%llx x2=0x%llx\n",
		        (unsigned long long)esr, ec, imm, (unsigned long long)x0, (unsigned long long)x1, (unsigned long long)x2);
		exit(3);
	}
}

static int mode_tso(void) {
	enum { RUNS = 30 }; const uint64_t N = 4000000;
	vm_setup();
	reset_jsonl("hvf_tso_speed");
	static uint64_t hostbuf[128] __attribute__((aligned(4096)));
	double hp[RUNS], hr[RUNS], g0p[RUNS], g0r[RUNS], g1p[RUNS];
	vcpu_t v0, v1;
	// One vCPU per host thread is the rule; recreate the vCPU to switch ACTLR, which also checks it sticks.
	for (int r = -2; r < RUNS; r++) {
		uint64_t t;
		t = now_ns(); mem_plain(hostbuf, N); double a = (double)(now_ns() - t) / N;
		t = now_ns(); mem_rcpc(hostbuf, N); double b = (double)(now_ns() - t) / N;
		uint64_t args[2] = { G + OFF_DATA, N };
		vcpu_init(&v0, 0);
		call(&v0, GA(mem_plain), args, 2, 0);                          // warm
		t = now_ns(); call(&v0, GA(mem_plain), args, 2, 0); double c = (double)(now_ns() - t) / N;
		t = now_ns(); call(&v0, GA(mem_rcpc), args, 2, 0); double d = (double)(now_ns() - t) / N;
		HVCHK(hv_vcpu_destroy(v0.id));
		vcpu_init(&v1, 1);
		uint64_t actlr = 0; hv_vcpu_get_sys_reg(v1.id, HV_SYS_REG_ACTLR_EL1, &actlr);
		if (r == -2) printf("ACTLR_EL1 readback with TSO requested: 0x%llx\n", (unsigned long long)actlr);
		call(&v1, GA(mem_plain), args, 2, 0);
		t = now_ns(); call(&v1, GA(mem_plain), args, 2, 0); double e = (double)(now_ns() - t) / N;
		HVCHK(hv_vcpu_destroy(v1.id));
		if (r >= 0) { hp[r] = a; hr[r] = b; g0p[r] = c; g0r[r] = d; g1p[r] = e; }
	}
	report("hvf_tso_speed", "host_plain", "ns_per_iter", hp, RUNS, "5 load/store pairs per iteration, 4e6 iterations");
	report("hvf_tso_speed", "host_ldapr_stlr", "ns_per_iter", hr, RUNS, "same loop with LDAPR/STLR");
	report("hvf_tso_speed", "vcpu_tso0_plain", "ns_per_iter", g0p, RUNS, "vCPU ACTLR_EL1.TSO clear, plain loop");
	report("hvf_tso_speed", "vcpu_tso0_ldapr_stlr", "ns_per_iter", g0r, RUNS, "vCPU ACTLR_EL1.TSO clear, LDAPR/STLR loop");
	report("hvf_tso_speed", "vcpu_tso1_plain", "ns_per_iter", g1p, RUNS, "vCPU ACTLR_EL1.TSO set, plain loop");
	return 0;
}

static void *burner(void *u) { (void)u; for (;;) __asm__ volatile(""); return 0; }

static int mode_x18(void) {
	vm_setup();
	int ncpu = 0; size_t sz = sizeof ncpu; sysctlbyname("hw.ncpu", &ncpu, &sz, 0, 0);
	for (int i = 0; i < 2 * ncpu; i++) { pthread_t t; pthread_create(&t, 0, burner, 0); }  // force host preemption and migration
	vcpu_t v; vcpu_init(&v, 0);
	const uint64_t S = 0x5eed5eed12345678ull, N = 200000;
	yield_every = 64;
	uint64_t a[2] = { S, N };
	uint64_t t = now_ns();
	uint64_t got = call(&v, GA(x18_el1), a, 2, 0);
	double secs = (double)(now_ns() - t) / 1e9;
	printf("EL1: x18 after %llu host exits (%llu host yields, %.2f s, %d burner threads): 0x%llx -> %s\n",
	       (unsigned long long)exits3, (unsigned long long)yields, secs, 2 * ncpu, (unsigned long long)got, got == S ? "PRESERVED" : "LOST");
	int imm = -1;
	uint64_t e[4] = { S, 2000000, G + OFF_EL0, G + OFF_EL0 + 0x1ff000 };
	t = now_ns();
	got = call(&v, GA(x18_el0_enter), e, 4, &imm);
	secs = (double)(now_ns() - t) / 1e9;
	uint64_t svcs = 0; hv_vcpu_get_reg(v.id, HV_REG_X1, &svcs);
	printf("EL0: x18 after %llu svc round trips to the EL1 vector and a brk (%.2f s, host loaded): 0x%llx -> %s (report imm=%d)\n",
	       (unsigned long long)svcs, secs, (unsigned long long)got, got == S ? "PRESERVED" : "LOST", imm);
	return 0;
}

static int mode_fault(void) {
	enum { RUNS = 30 }; const uint64_t N = 1000000;
	vm_setup();
	reset_jsonl("hvf_guest_fault");
	vcpu_t v; vcpu_init(&v, 0);
	double s[RUNS];
	uint64_t a[2] = { FAULT_VA, N };
	uint64_t cnt = call(&v, GA(fault_loop), a, 2, 0);
	printf("faults handled in the guest: %llu of %llu\n", (unsigned long long)cnt, (unsigned long long)N);
	CHECK(cnt == N, "fault count");
	for (int r = 0; r < RUNS; r++) { uint64_t t = now_ns(); call(&v, GA(fault_loop), a, 2, 0); s[r] = (double)(now_ns() - t) / N; }
	report("hvf_guest_fault", "el1_translation_fault_roundtrip", "ns", s, RUNS, "load from unmapped VA -> EL1 vector skips it -> eret, no host exit, 1e6 per run");
	return 0;
}

static int mode_lowva(void) {
	vm_setup();
	vcpu_t v; vcpu_init(&v, 0);
	uint64_t a[2] = { 0x10000, 0x400000 };
	uint64_t r = call(&v, GA(low_va), a, 2, 0);
	uint64_t f1 = *(uint64_t *)(ram + OFF_LOWF), f2 = *(uint64_t *)(ram + 0x400000);
	int ok = r == 0x1234 + 0x5678 && f1 == 0x1234 && f2 == 0x5678;
	printf("guest wrote VA 0x10000 and 0x400000 and read back 0x%llx; host frames 0x%llx 0x%llx\n", (unsigned long long)r, (unsigned long long)f1, (unsigned long long)f2);
	printf("RESULT lowva: %s\n", ok ? "PASS" : "FAIL");
	return !ok;
}

static int mode_stage2(void) {
	enum { RUNS = 30 }; const uint64_t N = 100000;
	vm_setup();
	reset_jsonl("hvf_stage2_exit");
	vcpu_t v; vcpu_init(&v, 0);
	double s[RUNS];
	uint64_t a[2] = { S2_VA, N };
	call(&v, GA(stage2_loop), a, 2, 0);
	printf("stage-2 data-abort exits seen by the host: %llu (expected %llu)\n", (unsigned long long)exits_s2, (unsigned long long)N);
	CHECK(exits_s2 == N, "exit count");
	for (int r = 0; r < RUNS; r++) { uint64_t t = now_ns(); call(&v, GA(stage2_loop), a, 2, 0); s[r] = (double)(now_ns() - t) / N; }
	report("hvf_stage2_exit", "data_abort_exit_roundtrip", "ns", s, RUNS, "load from IPA without stage-2 mapping -> host advances PC -> resume, 1e5 per run");
	return 0;
}

static int mode_multivm(void) {
	hv_return_t a = hv_vm_create(NULL);
	hv_return_t b = hv_vm_create(NULL);
	printf("first hv_vm_create -> 0x%x, second in the same process -> 0x%x\n", a, b);
	if (a == HV_SUCCESS) { hv_return_t d = hv_vm_destroy(); hv_return_t c = hv_vm_create(NULL); printf("destroy -> 0x%x, create again -> 0x%x\n", d, c); }
	return 0;
}

int main(int argc, char **argv) {
	set_qos();
	const char *m = argc > 1 ? argv[1] : "";
	if (!strcmp(m, "tso")) return mode_tso();
	if (!strcmp(m, "x18")) return mode_x18();
	if (!strcmp(m, "fault")) return mode_fault();
	if (!strcmp(m, "lowva")) return mode_lowva();
	if (!strcmp(m, "stage2")) return mode_stage2();
	if (!strcmp(m, "multivm")) return mode_multivm();
	fprintf(stderr, "usage: hvf2 tso|x18|fault|lowva|stage2|multivm\n");
	return 2;
}
