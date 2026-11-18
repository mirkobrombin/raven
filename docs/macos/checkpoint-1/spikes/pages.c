// Item 8: 16K page granularity behaviour.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

static sigjmp_buf jb;
static void on_fault(int s) { siglongjmp(jb, s); }
// returns 0 if access ok, else signal number
static int probe(volatile uint8_t *p, int write) {
    int s = sigsetjmp(jb, 1);
    if (s) return s;
    if (write) *p = 1; else (void)*p;
    return 0;
}
static void region(void *a, const char *tag) {
    mach_vm_address_t addr = (mach_vm_address_t)a; mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    kern_return_t kr = mach_vm_region(mach_task_self(), &addr, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj);
    printf("    mach_vm_region(%s @%p) kr=%d -> [0x%llx, +0x%llx) prot=%c%c%c\n", tag, a, kr, (unsigned long long)addr, (unsigned long long)sz,
           info.protection & VM_PROT_READ ? 'r' : '-', info.protection & VM_PROT_WRITE ? 'w' : '-', info.protection & VM_PROT_EXECUTE ? 'x' : '-');
}

int main(void) {
    signal(SIGSEGV, on_fault); signal(SIGBUS, on_fault);
    printf("getpagesize()=%d sysconf(_SC_PAGESIZE)=%ld vm_page_size=%lu vm_kernel_page_size=%lu\n",
           getpagesize(), sysconf(_SC_PAGESIZE), (unsigned long)vm_page_size, (unsigned long)vm_kernel_page_size);
    uint8_t *p = mmap(NULL, 0x8000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    printf("anon mmap of 2x16K at %p (16K aligned: %s)\n", p, ((uintptr_t)p & 0x3fff) ? "no" : "yes");
    memset(p, 0, 0x8000);
    // mprotect a 4K-aligned 4K sub-range in the middle of the first 16K page
    errno = 0;
    int r = mprotect(p + 0x1000, 0x1000, PROT_READ);
    printf("mprotect(p+0x1000, 0x1000, PROT_READ) = %d errno=%d (%s)\n", r, errno, strerror(errno));
    for (int off = 0; off < 0x8000; off += 0x1000) {
        int w = probe(p + off, 1);
        printf("  write p+0x%04x -> %s\n", off, w ? (w == SIGBUS ? "SIGBUS" : "SIGSEGV") : "ok");
    }
    region(p, "page0"); region(p + 0x4000, "page1");
    mprotect(p, 0x8000, PROT_READ | PROT_WRITE);
    // mprotect with a non-16K-aligned start
    errno = 0; r = mprotect(p + 0x1000, 0x4000, PROT_NONE);
    printf("mprotect(p+0x1000, 0x4000, PROT_NONE) = %d errno=%d (%s)\n", r, errno, strerror(errno));
    for (int off = 0; off < 0x8000; off += 0x1000) {
        int w = probe(p + off, 0);
        printf("  read p+0x%04x -> %s\n", off, w ? (w == SIGBUS ? "SIGBUS" : "SIGSEGV") : "ok");
    }
    region(p, "page0"); region(p + 0x4000, "page1");
    // aligned start, 4K length: does the kernel round the length up to 16K?
    errno = 0; r = mprotect(p + 0x4000, 0x1000, PROT_READ);
    printf("mprotect(p+0x4000 [16K aligned], 0x1000, PROT_READ) = %d errno=%d (%s)\n", r, errno, strerror(errno));
    for (int off = 0x4000; off < 0x8000; off += 0x1000) {
        int w = probe(p + off, 1);
        printf("  write p+0x%04x -> %s\n", off, w ? (w == SIGBUS ? "SIGBUS" : "SIGSEGV") : "ok");
    }
    region(p + 0x4000, "page1");
    mprotect(p, 0x8000, PROT_READ | PROT_WRITE);
    // mmap fixed at a 4K-aligned (not 16K) address
    void *q = mmap(p + 0x1000, 0x1000, PROT_READ, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    printf("mmap(MAP_FIXED at p+0x1000, 4K) -> %p errno=%d (%s)\n", q, q == MAP_FAILED ? errno : 0, q == MAP_FAILED ? strerror(errno) : "-");
    // file mapping at 4K-aligned offset
    char path[] = "build/raven_pages_XXXXXX"; int fd = mkstemp(path);
    uint8_t buf[0x10000]; for (int i = 0; i < 0x10000; i++) buf[i] = (uint8_t)(i >> 12);
    write(fd, buf, sizeof buf);
    for (off_t off = 0x1000; off <= 0x4000; off += 0x1000) {
        void *f = mmap(NULL, 0x1000, PROT_READ, MAP_PRIVATE, fd, off);
        if (f == MAP_FAILED) printf("mmap(file, offset=0x%llx) -> MAP_FAILED errno=%d (%s)\n", (long long)off, errno, strerror(errno));
        else printf("mmap(file, offset=0x%llx) -> %p first byte=%u (expected %u)\n", (long long)off, f, *(uint8_t *)f, (unsigned)(off >> 12));
    }
    close(fd); unlink(path);
    return 0;
}
