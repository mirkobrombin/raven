# Guest RTL component

Raven's own guest RTL for 64-bit programs, built by `scripts/provision-rtl.py` with
MinGW-w64. It holds the process heap and private heaps (`heap.c`), the kernel32 fast
paths for critical sections, SRW locks, condition variables and fibers (`kernel.c`),
the generic AVL tables documented by the Windows DDK (`tables.c`) and the few memory
functions the compiler may call (`memory.c`).

The component has no DllMain, TLS or loader. It imports only NT services and the
runtime's waits from ntdll. See [LICENSES.md](../../../LICENSES.md).
