#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The ntdll names a program that calls the RTL directly imports: the process heap and the critical sections. A 32-bit program has no
 * guest RTL component, so the runtime answers these itself; each step below fails with its own code. */

typedef LONG NTSTATUS;

__declspec(dllimport) NTSTATUS NTAPI RtlInitializeCriticalSection(CRITICAL_SECTION *section);
__declspec(dllimport) NTSTATUS NTAPI RtlEnterCriticalSection(CRITICAL_SECTION *section);
__declspec(dllimport) NTSTATUS NTAPI RtlLeaveCriticalSection(CRITICAL_SECTION *section);
__declspec(dllimport) BOOLEAN NTAPI RtlTryEnterCriticalSection(CRITICAL_SECTION *section);
__declspec(dllimport) NTSTATUS NTAPI RtlDeleteCriticalSection(CRITICAL_SECTION *section);
__declspec(dllimport) PVOID NTAPI RtlAllocateHeap(HANDLE heap, ULONG flags, SIZE_T size);
__declspec(dllimport) BOOLEAN NTAPI RtlFreeHeap(HANDLE heap, ULONG flags, PVOID block);
__declspec(dllimport) SIZE_T NTAPI RtlSizeHeap(HANDLE heap, ULONG flags, PVOID block);
__declspec(dllimport) PVOID NTAPI RtlReAllocateHeap(HANDLE heap, ULONG flags, PVOID block, SIZE_T size);
__declspec(dllimport) PVOID NTAPI RtlCreateHeap(ULONG flags, PVOID base, SIZE_T reserve, SIZE_T commit, PVOID lock, PVOID parameters);
__declspec(dllimport) PVOID NTAPI RtlDestroyHeap(PVOID heap);

int raven_entry(void)
{
	HANDLE heap = GetProcessHeap();
	CRITICAL_SECTION section;
	BYTE *block, *grown, *zeroed;
	PVOID own;

	/* The same heap the kernel32 call names, and a section entered twice by its owner before it is left twice. */
	if (!heap || heap != GetProcessHeap()) return 1;
	if (RtlInitializeCriticalSection(&section) != 0) return 2;
	if (RtlEnterCriticalSection(&section) != 0) return 3;
	if (RtlEnterCriticalSection(&section) != 0) return 4;
	if (!RtlTryEnterCriticalSection(&section)) return 5;
	if (RtlLeaveCriticalSection(&section) != 0) return 6;
	if (RtlLeaveCriticalSection(&section) != 0) return 7;
	if (RtlLeaveCriticalSection(&section) != 0) return 8;
	if (RtlDeleteCriticalSection(&section) != 0) return 9;

	/* A block that zeroes what it hands out, reports its size and keeps its contents when it grows. */
	zeroed = (BYTE *)RtlAllocateHeap(heap, HEAP_ZERO_MEMORY, 64);
	if (!zeroed) return 10;
	for (int at = 0; at < 64; ++at)
		if (zeroed[at]) return 11;
	block = (BYTE *)RtlAllocateHeap(heap, 0, 32);
	if (!block) return 12;
	for (int at = 0; at < 32; ++at) block[at] = (BYTE)at;
	if (RtlSizeHeap(heap, 0, block) < 32) return 13;
	grown = (BYTE *)RtlReAllocateHeap(heap, 0, block, 4096);
	if (!grown || RtlSizeHeap(heap, 0, grown) < 4096) return 14;
	for (int at = 0; at < 32; ++at)
		if (grown[at] != (BYTE)at) return 15;
	if (!RtlFreeHeap(heap, 0, grown) || !RtlFreeHeap(heap, 0, zeroed)) return 16;

	/* A heap of the program's own, which the runtime hands the same arena, and its blocks freed before it goes. */
	own = RtlCreateHeap(0, NULL, 0, 0, NULL, NULL);
	if (!own) return 17;
	block = (BYTE *)RtlAllocateHeap(own, 0, 128);
	if (!block) return 18;
	block[0] = 1;
	block[127] = 2;
	if (block[0] != 1 || block[127] != 2) return 19;
	if (!RtlFreeHeap(own, 0, block)) return 20;
	if (!RtlDestroyHeap(own)) return 21;
	return 0;
}
