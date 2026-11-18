#include <os/arch/arm64.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <mach-o/getsect.h>
#include <mach-o/dyld.h>

static void try_map(uintptr_t a) {
    void *p = mmap((void *)a, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) { printf("map %#lx: FAIL errno=%d %s\n", a, errno, strerror(errno)); return; }
    *(volatile int *)p = 1;
    printf("map %#lx: OK (touched)\n", a);
    munmap(p, 0x4000);
}

static volatile int x, y, r1, r2;
static volatile int go1, go2, done1, done2;
static void *t1(void *u) { for (;;) { while (!go1); go1 = 0; x = 1; r1 = y; done1 = 1; } return u; }
static void *t2(void *u) { for (;;) { while (!go2); go2 = 0; y = 1; r2 = x; done2 = 1; } return u; }

int main(void) {
    printf("os_cross_arch_is_supported(X86_64)=%d\n", os_cross_arch_is_supported(OS_CROSS_ARCH_X86_64));
    const struct segment_command_64 *pz = getsegbyname("__PAGEZERO");
    printf("__PAGEZERO vmsize=%#llx\n", pz ? (unsigned long long)pz->vmsize : 0ULL);
    uintptr_t addrs[] = {0x10000, 0x400000, 0x7ffe0000, 0x140000000};
    for (unsigned i = 0; i < 4; i++) try_map(addrs[i]);
    pthread_t a, b;
    pthread_create(&a, 0, t1, 0); pthread_create(&b, 0, t2, 0);
    long n = 2000000, reord = 0;
    for (long i = 0; i < n; i++) {
        x = y = 0; done1 = done2 = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        go1 = go2 = 1;
        while (!done1 || !done2);
        if (r1 == 0 && r2 == 0) reord++;
    }
    printf("SB litmus: %ld/%ld reorderings (r1=r2=0)\n", reord, n);
    return 0;
}
