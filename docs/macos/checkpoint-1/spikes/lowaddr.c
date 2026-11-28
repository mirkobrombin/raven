// Item 7: low-address mappings. Built with several -pagezero_size values.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/getsect.h>
#include <mach-o/dyld.h>
#include <mach/mach_error.h>

static void try_addr(uint64_t a) {
    // 1) non-clobbering: mach_vm_allocate with VM_FLAGS_FIXED fails if anything is there
    mach_vm_address_t va = a;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &va, 0x10000, VM_FLAGS_FIXED);
    int mach_ok = kr == KERN_SUCCESS;
    if (mach_ok) mach_vm_deallocate(mach_task_self(), va, 0x10000);
    // 2) mmap MAP_FIXED, only if the range was free (so we never clobber the image)
    const char *mm = "skipped(not free)"; char buf[96];
    if (mach_ok) {
        void *p = mmap((void *)a, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (p == MAP_FAILED) { snprintf(buf, sizeof buf, "FAIL errno=%d %s", errno, strerror(errno)); mm = buf; }
        else { *(volatile uint64_t *)p = 0x1234; snprintf(buf, sizeof buf, "OK at %p (written)", p); mm = buf; munmap(p, 0x10000); }
    }
    // 3) mmap hint without MAP_FIXED
    void *h = mmap((void *)a, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    printf("  0x%010llx  mach_vm_allocate(FIXED)=%-3s(kr=%d %-22s)  mmap(MAP_FIXED)=%-34s  mmap(hint)->%p%s\n",
           (unsigned long long)a, mach_ok ? "OK" : "NO", kr, mach_error_string(kr), mm, h, h == (void *)a ? " (honoured)" : "");
    if (h != MAP_FAILED) munmap(h, 0x10000);
}

int main(int argc, char **argv) {
    const struct segment_command_64 *pz = getsegbyname("__PAGEZERO");
    printf("== %s: __PAGEZERO vmaddr=0x%llx vmsize=0x%llx; main image slide=0x%lx\n", argc > 1 ? argv[1] : "?",
           pz ? (unsigned long long)pz->vmaddr : 0, pz ? (unsigned long long)pz->vmsize : 0, (long)_dyld_get_image_vmaddr_slide(0));
    uint64_t addrs[] = { 0x1000, 0x10000, 0x400000, 0x1000000, 0x10000000, 0x7ffe0000, 0x80000000, 0xfffe0000, 0x140000000ULL, 0x7f0000000ULL };
    for (unsigned i = 0; i < sizeof addrs / sizeof addrs[0]; i++) try_addr(addrs[i]);
    // lowest address where a 16K fixed allocation succeeds (16K steps up to 4GB)
    uint64_t lowest = 0;
    for (uint64_t a = 0x4000; a < 0x100000000ULL; a += 0x4000) {
        mach_vm_address_t va = a;
        if (mach_vm_allocate(mach_task_self(), &va, 0x4000, VM_FLAGS_FIXED) == KERN_SUCCESS) { mach_vm_deallocate(mach_task_self(), va, 0x4000); lowest = a; break; }
    }
    printf("  lowest fixed-allocatable address below 4GB: 0x%llx%s\n", (unsigned long long)lowest, lowest ? "" : " (none)");
    void *p32 = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_32BIT, -1, 0);
    printf("  MAP_32BIT -> %p %s\n", p32, p32 == MAP_FAILED ? strerror(errno) : ((uint64_t)p32 < 0x100000000ULL ? "(below 4GB)" : "(NOT below 4GB)"));
    return 0;
}
