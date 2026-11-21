// Hypervisor.framework spikes (items 1b and 3).
// Modes: info | mmu4k | roundtrip | litmus | ipa4k
#include "bench.h"
#include <Hypervisor/Hypervisor.h>
#include <sys/mman.h>
#include <unistd.h>

extern char gk_start[], gk_end[], gk_vectors[], gk_vectors_end[];
extern char mp_writer[], mp_reader[], sb_thread[], gk_hvc[], mmu_touch[], spin_client[];
#include <stdatomic.h>
#include <mach/mach.h>

#define HVCHK(x) do { hv_return_t _r = (x); if (_r != HV_SUCCESS) { fprintf(stderr, "FAIL %s -> 0x%x\n", #x, _r); exit(2); } } while (0)

#define G        0x40000000ULL        // guest RAM IPA base (identity-mapped VA)
#define GSIZE    (16ULL << 20)
#define OFF_L1   0x1000
#define OFF_L2   0x2000
#define OFF_L3   0x3000
#define OFF_L3B  0x4000
#define OFF_CODE 0x10000
#define OFF_DATA 0x100000
#define OFF_TOUCH 0x180000
#define OFF_RES0 0x400000
#define OFF_RES1 0x600000
#define TESTVA   0x50000000ULL

static uint8_t *ram;

#define GA(sym) (G + OFF_CODE + (uint64_t)((char *)(sym) - gk_start))
#define DESC_TABLE 3ULL
#define DESC_PAGE  (3ULL | (1ULL << 10) | (3ULL << 8))  // L3 page, AF, inner-shareable, AttrIndx 0 (normal WB)
#define DESC_BLOCK (1ULL | (1ULL << 10) | (3ULL << 8))  // L2 2MB block

static void build_tables(void) {
    uint64_t *l1 = (uint64_t *)(ram + OFF_L1), *l2 = (uint64_t *)(ram + OFF_L2);
    uint64_t *l3 = (uint64_t *)(ram + OFF_L3), *l3b = (uint64_t *)(ram + OFF_L3B);
    memset(l1, 0, 0x4000 + 0x1000);
    l1[1] = (G + OFF_L2) | DESC_TABLE;                    // VA 1GB..2GB
    l2[0] = (G + OFF_L3) | DESC_TABLE;                    // first 2MB via 4K pages
    for (int i = 0; i < 512; i++) l3[i] = (G + (uint64_t)i * 0x1000) | DESC_PAGE;
    for (int i = 1; i < (int)(GSIZE >> 21); i++) l2[i] = (G + ((uint64_t)i << 21)) | DESC_BLOCK;
    l2[(TESTVA - G) >> 21] = (G + OFF_L3B) | DESC_TABLE;   // VA 0x50000000 -> scattered 4K frames
    l3b[0] = (G + OFF_TOUCH + 0x3000) | DESC_PAGE;         // VA page 0 -> 4th 4K frame of a 16K host page
    l3b[1] = (G + OFF_TOUCH + 0x1000) | DESC_PAGE;         // VA page 1 -> 2nd 4K frame of the same 16K page
}

static hv_vm_config_t vm_setup(void) {
    hv_vm_config_t cfg = hv_vm_config_create();
    HVCHK(hv_vm_create(cfg));
    ram = mmap(NULL, GSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(ram != MAP_FAILED, "mmap ram");
    HVCHK(hv_vm_map(ram, G, GSIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    memcpy(ram, gk_vectors, gk_vectors_end - gk_vectors);
    memcpy(ram + OFF_CODE, gk_start, gk_end - gk_start);
    build_tables();
    return cfg;
}

typedef struct { hv_vcpu_t id; hv_vcpu_exit_t *exit; } vcpu_t;

static void vcpu_init(vcpu_t *v, int mmu, int tso) {
    HVCHK(hv_vcpu_create(&v->id, &v->exit, NULL));
    HVCHK(hv_vcpu_set_vtimer_mask(v->id, true));
    HVCHK(hv_vcpu_set_reg(v->id, HV_REG_CPSR, 0x3c5));      // EL1h, DAIF masked
    HVCHK(hv_vcpu_set_sys_reg(v->id, HV_SYS_REG_VBAR_EL1, G));
    HVCHK(hv_vcpu_set_sys_reg(v->id, HV_SYS_REG_SP_EL1, G + GSIZE - 0x1000));
    if (mmu) {
        uint64_t tcr = 25 | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (0ULL << 14) | (1ULL << 23) | (1ULL << 32);
        HVCHK(hv_vcpu_set_sys_reg(v->id, HV_SYS_REG_MAIR_EL1, 0xFF));
        HVCHK(hv_vcpu_set_sys_reg(v->id, HV_SYS_REG_TCR_EL1, tcr));
        HVCHK(hv_vcpu_set_sys_reg(v->id, HV_SYS_REG_TTBR0_EL1, G + OFF_L1));
        HVCHK(hv_vcpu_set_sys_reg(v->id, HV_SYS_REG_SCTLR_EL1, 0x30d00800ULL | 1 | 4 | 0x1000));
    }
    if (tso >= 0) {
        hv_return_t r = hv_vcpu_set_sys_reg(v->id, HV_SYS_REG_ACTLR_EL1, tso ? 2 : 0);
        uint64_t back = ~0ULL;
        hv_return_t r2 = hv_vcpu_get_sys_reg(v->id, HV_SYS_REG_ACTLR_EL1, &back);
        printf("vcpu %llu: set ACTLR_EL1=%d -> 0x%x, readback -> 0x%x value=0x%llx\n",
               (unsigned long long)v->id, tso ? 2 : 0, r, r2, (unsigned long long)back);
        CHECK(r == HV_SUCCESS, "ACTLR set");
    }
}

// Run guest function at gpc with args; returns x0 when the guest hits hvc #0 in gk_hvc.
static uint64_t vcpu_call(vcpu_t *v, uint64_t gpc, const uint64_t *args, int nargs) {
    for (int i = 0; i < nargs; i++) HVCHK(hv_vcpu_set_reg(v->id, HV_REG_X0 + i, args[i]));
    HVCHK(hv_vcpu_set_reg(v->id, HV_REG_LR, GA(gk_hvc)));
    HVCHK(hv_vcpu_set_reg(v->id, HV_REG_PC, gpc));
    for (;;) {
        HVCHK(hv_vcpu_run(v->id));
        if (v->exit->reason != HV_EXIT_REASON_EXCEPTION) { fprintf(stderr, "exit reason %d\n", v->exit->reason); continue; }
        uint64_t esr = v->exit->exception.syndrome, x0, x1, x2;
        uint32_t ec = (esr >> 26) & 0x3f, imm = esr & 0xffff;
        if (ec == 0x16 && imm == 0) { HVCHK(hv_vcpu_get_reg(v->id, HV_REG_X0, &x0)); return x0; }
        hv_vcpu_get_reg(v->id, HV_REG_X0, &x0); hv_vcpu_get_reg(v->id, HV_REG_X1, &x1); hv_vcpu_get_reg(v->id, HV_REG_X2, &x2);
        fprintf(stderr, "guest exception: exit ESR=0x%llx ec=0x%x imm=0x%x; if vector: ESR_EL1=0x%llx FAR=0x%llx ELR=0x%llx\n",
                (unsigned long long)esr, ec, imm, (unsigned long long)x0, (unsigned long long)x1, (unsigned long long)x2);
        exit(3);
    }
}

static int mode_info(void) {
    uint32_t maxipa = 0, defipa = 0, maxvcpu = 0;
    HVCHK(hv_vm_config_get_max_ipa_size(&maxipa));
    HVCHK(hv_vm_config_get_default_ipa_size(&defipa));
    hv_ipa_granule_t gran = 99;
    hv_return_t rg = hv_vm_config_get_default_ipa_granule(&gran);
    HVCHK(hv_vm_get_max_vcpu_count(&maxvcpu));
    hv_vcpu_config_t vc = hv_vcpu_config_create();
    uint64_t mmfr0 = 0, mmfr1 = 0;
    HVCHK(hv_vcpu_config_get_feature_reg(vc, HV_FEATURE_REG_ID_AA64MMFR0_EL1, &mmfr0));
    HVCHK(hv_vcpu_config_get_feature_reg(vc, HV_FEATURE_REG_ID_AA64MMFR1_EL1, &mmfr1));
    unsigned tgran4 = (mmfr0 >> 28) & 0xf, tgran16 = (mmfr0 >> 20) & 0xf, tgran64 = (mmfr0 >> 24) & 0xf;
    unsigned tgran4_2 = (mmfr0 >> 40) & 0xf, tgran16_2 = (mmfr0 >> 32) & 0xf;
    printf("max_ipa_size=%u default_ipa_size=%u max_vcpu=%u\n", maxipa, defipa, maxvcpu);
    printf("default_ipa_granule ret=0x%x value=%u (0=4KB,1=16KB)\n", rg, gran);
    printf("guest ID_AA64MMFR0_EL1=0x%016llx TGran4=%u(0=supported,0xf=no) TGran16=%u(1=supported) TGran64=%u(0=supported) TGran4_2=%u TGran16_2=%u PARange=%llu\n",
           (unsigned long long)mmfr0, tgran4, tgran16, tgran64, tgran4_2, tgran16_2, (unsigned long long)(mmfr0 & 0xf));
    FILE *f = fopen(RESULTS_DIR "/hvf_info.json", "w");
    fprintf(f, "{\"max_ipa_size\":%u,\"default_ipa_size\":%u,\"max_vcpu\":%u,\"default_ipa_granule\":%u,\"default_ipa_granule_ret\":%u,"
               "\"id_aa64mmfr0_el1\":\"0x%016llx\",\"tgran4\":%u,\"tgran16\":%u,\"tgran64\":%u}\n",
            maxipa, defipa, maxvcpu, gran, rg, (unsigned long long)mmfr0, tgran4, tgran16, tgran64);
    fclose(f);
    return 0;
}

static int mode_mmu4k(void) {
    vm_setup();
    vcpu_t v; vcpu_init(&v, 1, -1);
    uint64_t args[2] = { TESTVA, TESTVA + 0x1000 };
    uint64_t r = vcpu_call(&v, GA(mmu_touch), args, 2);
    uint32_t a = *(uint32_t *)(ram + OFF_TOUCH + 0x3000), b = *(uint32_t *)(ram + OFF_TOUCH + 0x1000);
    uint32_t c = *(uint32_t *)(ram + OFF_TOUCH), d = *(uint32_t *)(ram + OFF_TOUCH + 0x2000);
    uint64_t sctlr = 0, tcr = 0; hv_vcpu_get_sys_reg(v.id, HV_SYS_REG_SCTLR_EL1, &sctlr); hv_vcpu_get_sys_reg(v.id, HV_SYS_REG_TCR_EL1, &tcr);
    printf("guest SCTLR_EL1=0x%llx TCR_EL1=0x%llx (TG0=%llu => 4K)\n", (unsigned long long)sctlr, (unsigned long long)tcr, (unsigned long long)((tcr >> 14) & 3));
    printf("guest returned 0x%llx; host sees frame3=0x%x frame1=0x%x frame0=0x%x frame2=0x%x\n", (unsigned long long)r, a, b, c, d);
    int ok = r == 0x3333 && a == 0x1111 && b == 0x2222 && c == 0 && d == 0;
    printf("RESULT mmu4k: %s (VA 0x%llx->frame3, VA 0x%llx->frame1 of one 16K host page, code runs from 4K L3 pages)\n",
           ok ? "PASS" : "FAIL", TESTVA, TESTVA + 0x1000);
    return ok ? 0 : 1;
}

static int mode_roundtrip(void) {
    enum { RUNS = 30, ITERS = 100000 };
    reset_jsonl("hvf_roundtrip");
    uint64_t t0 = now_ns();
    hv_vm_config_t cfg = hv_vm_config_create();
    HVCHK(hv_vm_create(cfg));
    double vmcreate = (double)(now_ns() - t0);
    ram = mmap(NULL, GSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    HVCHK(hv_vm_map(ram, G, GSIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    memcpy(ram, gk_vectors, gk_vectors_end - gk_vectors);
    memcpy(ram + OFF_CODE, gk_start, gk_end - gk_start);
    build_tables();
    printf("hv_vm_create (single, first in process): %.0f ns\n", vmcreate);

    double first[RUNS], create[RUNS], rt[RUNS];
    for (int r = 0; r < RUNS; r++) {
        vcpu_t v;
        uint64_t a = now_ns();
        vcpu_init(&v, 1, -1);
        uint64_t b = now_ns();
        HVCHK(hv_vcpu_set_reg(v.id, HV_REG_PC, GA(gk_hvc)));
        HVCHK(hv_vcpu_run(v.id));
        uint64_t c = now_ns();
        CHECK(v.exit->reason == HV_EXIT_REASON_EXCEPTION && ((v.exit->exception.syndrome >> 26) & 0x3f) == 0x16, "first exit not HVC");
        create[r] = (double)(b - a); first[r] = (double)(c - b);
        if (r == 0) {
            uint64_t pc; hv_vcpu_get_reg(v.id, HV_REG_PC, &pc);
            printf("after HVC exit: PC=0x%llx, hvc insn at 0x%llx (%s)\n", (unsigned long long)pc, (unsigned long long)GA(gk_hvc),
                   pc == GA(gk_hvc) + 4 ? "PC already advanced past HVC" : pc == GA(gk_hvc) ? "PC at HVC" : "other");
        }
        HVCHK(hv_vcpu_destroy(v.id));
    }
    vcpu_t v; vcpu_init(&v, 1, -1);
    HVCHK(hv_vcpu_set_reg(v.id, HV_REG_PC, GA(gk_hvc)));
    for (int i = 0; i < 1000; i++) HVCHK(hv_vcpu_run(v.id));  // warm-up
    for (int r = 0; r < RUNS; r++) {
        uint64_t a = now_ns();
        for (int i = 0; i < ITERS; i++) {
            hv_vcpu_run(v.id);
            if (__builtin_expect(v.exit->reason != HV_EXIT_REASON_EXCEPTION, 0)) { fprintf(stderr, "unexpected exit %d\n", v.exit->reason); exit(4); }
        }
        rt[r] = (double)(now_ns() - a) / ITERS;
    }
    report("hvf_roundtrip", "vcpu_create_plus_sysregs", "ns", create, RUNS, "hv_vcpu_create + ~10 set_reg calls");
    report("hvf_roundtrip", "first_vcpu_run", "ns", first, RUNS, "first hv_vcpu_run on a fresh vcpu up to HVC exit");
    report("hvf_roundtrip", "hvc_exit_roundtrip", "ns", rt, RUNS, "guest hvc -> host -> hv_vcpu_run resume, 1e5 per run");
    return 0;
}

// ---- litmus inside the VM: two vCPUs on two host threads ----
typedef struct { int idx, tso, trials; uint64_t n; uint64_t *viol, *loops; hbarrier_t *bar; } lit_arg_t;
#define D_X   (OFF_DATA + 0x0000)
#define D_Y   (OFF_DATA + 0x1000)
#define D_GO  (OFF_DATA + 0x2000)
#define D_BAR (OFF_DATA + 0x3000)

static void *lit_thread(void *p) {
    lit_arg_t *a = p; set_qos();
    vcpu_t v; vcpu_init(&v, 1, a->tso);
    for (int t = 0; t < a->trials; t++) {
        hbarrier_wait(a->bar);           // host resets data between barriers
        hbarrier_wait(a->bar);
        uint64_t args[4] = { G + D_X, G + D_Y, a->n, G + D_GO };
        uint64_t r = vcpu_call(&v, a->idx == 0 ? GA(mp_writer) : GA(mp_reader), args, 4);
        if (a->idx == 1) { a->viol[t] = r; a->loops[t] = *(volatile uint64_t *)(ram + D_GO + 8); }
    }
    // SB phase
    hbarrier_wait(a->bar);
    hbarrier_wait(a->bar);
    uint64_t sbargs[5] = { G + (a->idx ? D_Y : D_X), G + (a->idx ? D_X : D_Y), G + (a->idx ? OFF_RES1 : OFF_RES0), a->n / 10, G + D_BAR };
    vcpu_call(&v, GA(sb_thread), sbargs, 5);
    hbarrier_wait(a->bar);
    return NULL;
}

static int mode_litmus(int tso, int mmu_off_control) {
    (void)mmu_off_control;
    vm_setup();
    enum { TRIALS = 30 }; const uint64_t N = 2000000;
    uint64_t viol[TRIALS], loops[TRIALS];
    hbarrier_t bar; hbarrier_init(&bar, 3);
    lit_arg_t a0 = { 0, tso, TRIALS, N, viol, loops, &bar }, a1 = { 1, tso, TRIALS, N, viol, loops, &bar };
    pthread_t t0, t1;
    pthread_create(&t0, NULL, lit_thread, &a0); pthread_create(&t1, NULL, lit_thread, &a1);
    for (int t = 0; t < TRIALS; t++) {
        hbarrier_wait(&bar);
        *(volatile uint64_t *)(ram + D_X) = 0; *(volatile uint64_t *)(ram + D_Y) = 0; *(volatile uint64_t *)(ram + D_GO) = 0; *(volatile uint64_t *)(ram + D_GO + 8) = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        hbarrier_wait(&bar);
    }
    hbarrier_wait(&bar);
    *(volatile uint64_t *)(ram + D_X) = 0; *(volatile uint64_t *)(ram + D_Y) = 0; *(volatile uint64_t *)(ram + D_BAR) = 0;
    memset(ram + OFF_RES0, 0xee, N / 10); memset(ram + OFF_RES1, 0xee, N / 10);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    hbarrier_wait(&bar);
    hbarrier_wait(&bar);
    pthread_join(t0, NULL); pthread_join(t1, NULL);
    uint64_t sb = 0, bad = 0;
    for (uint64_t i = 0; i < N / 10; i++) { uint8_t r0 = ram[OFF_RES0 + i], r1 = ram[OFF_RES1 + i]; if (r0 > 1 || r1 > 1) bad++; if (!r0 && !r1) sb++; }
    double dv[TRIALS], dl[TRIALS]; uint64_t tot = 0;
    for (int t = 0; t < TRIALS; t++) { dv[t] = (double)viol[t]; dl[t] = (double)loops[t]; tot += viol[t]; }
    char name[64]; snprintf(name, sizeof name, "litmus_vm_tso%d", tso);
    reset_jsonl(name);
    report(name, "mp_violations_per_2e6", "count", dv, TRIALS, tso ? "vCPU ACTLR_EL1.EnTSO=1" : "vCPU ACTLR_EL1.EnTSO=0");
    report(name, "mp_reader_loops", "count", dl, TRIALS, "reader iterations while writer ran (overlap control)");
    printf("RESULT %s: MP violations total=%llu over %d x %llu writes; SB r0=r1=0 count=%llu of %llu iters (bad=%llu)\n",
           name, (unsigned long long)tot, TRIALS, (unsigned long long)N, (unsigned long long)sb, (unsigned long long)(N / 10), (unsigned long long)bad);
    FILE *f = fopen(RESULTS_DIR "/litmus_summary.csv", "a");
    fprintf(f, "%s,%llu,%d,%llu,%llu,%llu\n", name, (unsigned long long)tot, TRIALS, (unsigned long long)N, (unsigned long long)sb, (unsigned long long)(N / 10));
    fclose(f);
    return 0;
}

// Stage-2 granule: can a 4K-aligned, 4K-sized host slice be mapped at a 4K-aligned IPA?
static int mode_ipa4k(int use4k) {
    hv_vm_config_t cfg = hv_vm_config_create();
    hv_return_t rs = HV_SUCCESS;
    if (use4k) rs = hv_vm_config_set_ipa_granule(cfg, HV_IPA_GRANULE_4KB);
    hv_ipa_granule_t g = 99; hv_vm_config_get_ipa_granule(cfg, &g);
    printf("set_ipa_granule(4KB) ret=0x%x, config granule now=%u\n", rs, g);
    HVCHK(hv_vm_create(cfg));
    uint8_t *mem = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint8_t *code = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    memset(mem, 0, 0x10000);
    *(uint64_t *)(mem + 0x1000) = 0x4b4b4b4b;
    hv_return_t r1 = hv_vm_map(mem + 0x1000, 0x80001000, 0x1000, HV_MEMORY_READ | HV_MEMORY_WRITE);
    printf("hv_vm_map(host+0x1000, IPA 0x80001000, size 0x1000) -> 0x%x\n", r1);
    // guest: ldr x0,[x1]; hvc #0  (MMU off, IPA==VA)
    uint32_t prog[] = { 0xf9400020, 0xd4000002 };
    memcpy(code, prog, sizeof prog);
    HVCHK(hv_vm_map(code, 0x90000000, 0x4000, HV_MEMORY_READ | HV_MEMORY_EXEC));
    int ok = 0;
    if (r1 == HV_SUCCESS) {
        hv_vcpu_t id; hv_vcpu_exit_t *ex;
        HVCHK(hv_vcpu_create(&id, &ex, NULL));
        hv_vcpu_set_reg(id, HV_REG_CPSR, 0x3c5); hv_vcpu_set_reg(id, HV_REG_PC, 0x90000000); hv_vcpu_set_reg(id, HV_REG_X1, 0x80001000);
        HVCHK(hv_vcpu_run(id));
        uint64_t x0 = 0; hv_vcpu_get_reg(id, HV_REG_X0, &x0);
        printf("guest exit ESR=0x%llx x0=0x%llx\n", (unsigned long long)ex->exception.syndrome, (unsigned long long)x0);
        ok = (((ex->exception.syndrome >> 26) & 0x3f) == 0x16) && x0 == 0x4b4b4b4b;
        // Also probe the neighbouring 4K IPA, which must be unmapped
        hv_vcpu_set_reg(id, HV_REG_PC, 0x90000000); hv_vcpu_set_reg(id, HV_REG_X1, 0x80002000);
        HVCHK(hv_vcpu_run(id));
        printf("neighbour IPA 0x80002000 access: exit reason=%d ESR=0x%llx (EC 0x24 = data abort from lower EL => unmapped)\n",
               ex->reason, (unsigned long long)ex->exception.syndrome);
        ok = ok && ((ex->exception.syndrome >> 26) & 0x3f) == 0x24;
    }
    printf("RESULT ipa4k(use4k=%d): %s\n", use4k, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ---- item 13: host thread <-> vCPU shared-memory spin round-trip (no VM exit) ----
static volatile int srv_stop;
static void *spin_server(void *p) {
    set_qos();
    _Atomic uint32_t *req = (_Atomic uint32_t *)(ram + D_X), *resp = (_Atomic uint32_t *)(ram + D_Y);
    uint32_t last = 0;
    while (!srv_stop) {
        uint32_t v = atomic_load_explicit(req, memory_order_acquire);
        if (v != last) { last = v; atomic_store_explicit(resp, v, memory_order_release); }
    }
    return NULL;
}
static int mode_shmspin(void) {
    vm_setup();
    enum { RUNS = 30 }; const uint64_t N = 100000;
    reset_jsonl("hvf_shmspin");
    vcpu_t v; vcpu_init(&v, 1, -1);
    double s[RUNS];
    for (int r = -3; r < RUNS; r++) {
        *(volatile uint32_t *)(ram + D_X) = 0; *(volatile uint32_t *)(ram + D_Y) = 0;
        srv_stop = 0; pthread_t t; pthread_create(&t, NULL, spin_server, NULL);
        uint64_t args[3] = { G + D_X, G + D_Y, N };
        uint64_t a = now_ns();
        vcpu_call(&v, GA(spin_client), args, 3);
        uint64_t b = now_ns();
        srv_stop = 1; pthread_join(t, NULL);
        if (r >= 0) s[r] = (double)(b - a) / N;
    }
    report("hvf_shmspin", "guest_host_spin_roundtrip", "ns", s, RUNS, "vCPU stlr req / host thread spins and answers / vCPU ldar spin; 1e5 per run incl. one hv_vcpu_run entry+exit");
    return 0;
}

// ---- item 12: how many vCPUs can be created, memory per vCPU ----
static hbarrier_t vb; static pthread_mutex_t vm_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t vm_cv = PTHREAD_COND_INITIALIZER;
static int created, failed_at = -1; static hv_return_t fail_code; static volatile int release_all;
static uint64_t footprint(void) {
    task_vm_info_data_t vi; mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &c); return vi.phys_footprint;
}
static void *vcpu_holder(void *p) {
    int idx = (int)(intptr_t)p; hv_vcpu_t id; hv_vcpu_exit_t *ex;
    hv_return_t r = hv_vcpu_create(&id, &ex, NULL);
    pthread_mutex_lock(&vm_mu);
    if (r == HV_SUCCESS) created++; else if (failed_at < 0) { failed_at = idx; fail_code = r; }
    pthread_cond_broadcast(&vm_cv);
    while (!release_all) pthread_cond_wait(&vm_cv, &vm_mu);
    pthread_mutex_unlock(&vm_mu);
    if (r == HV_SUCCESS) hv_vcpu_destroy(id);
    return NULL;
}
static int mode_maxvcpu(void) {
    uint32_t maxv = 0; HVCHK(hv_vm_get_max_vcpu_count(&maxv));
    hv_vm_config_t cfg = hv_vm_config_create(); HVCHK(hv_vm_create(cfg));
    int target = 512; pthread_t th[600];
    FILE *f = fopen(RESULTS_DIR "/hvf_maxvcpu.csv", "w"); fprintf(f, "vcpus,phys_footprint_bytes,create_ns\n");
    uint64_t base = footprint(); fprintf(f, "0,%llu,0\n", (unsigned long long)base);
    int i;
    for (i = 0; i < target; i++) {
        pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64 * 1024);
        uint64_t a = now_ns();
        pthread_create(&th[i], &at, vcpu_holder, (void *)(intptr_t)i);
        pthread_mutex_lock(&vm_mu);
        while (created + (failed_at >= 0) < i + 1) pthread_cond_wait(&vm_cv, &vm_mu);
        int stop = failed_at >= 0;
        pthread_mutex_unlock(&vm_mu);
        uint64_t b = now_ns();
        fprintf(f, "%d,%llu,%llu\n", created, (unsigned long long)footprint(), (unsigned long long)(b - a));
        if (stop) { i++; break; }
    }
    fclose(f);
    uint64_t fp = footprint();
    printf("hv_vm_get_max_vcpu_count=%u; created=%d; first failure at index %d with 0x%x; phys_footprint base=%llu after=%llu => %.1f KiB per vCPU (thread stack 64K incl.)\n",
           maxv, created, failed_at, fail_code, (unsigned long long)base, (unsigned long long)fp, created ? (double)(fp - base) / created / 1024.0 : 0.0);
    pthread_mutex_lock(&vm_mu); release_all = 1; pthread_cond_broadcast(&vm_cv); pthread_mutex_unlock(&vm_mu);
    for (int k = 0; k < i; k++) pthread_join(th[k], NULL);
    return 0;
}

int main(int argc, char **argv) {
    set_qos();
    const char *m = argc > 1 ? argv[1] : "info";
    if (!strcmp(m, "info")) return mode_info();
    if (!strcmp(m, "mmu4k")) return mode_mmu4k();
    if (!strcmp(m, "roundtrip")) return mode_roundtrip();
    if (!strcmp(m, "litmus")) return mode_litmus(argc > 2 ? atoi(argv[2]) : 0, 0);
    if (!strcmp(m, "shmspin")) return mode_shmspin();
    if (!strcmp(m, "maxvcpu")) return mode_maxvcpu();
    if (!strcmp(m, "ipa4k")) return mode_ipa4k(argc > 2 ? atoi(argv[2]) : 1);
    fprintf(stderr, "unknown mode\n");
    return 2;
}
