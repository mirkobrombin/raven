/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "raven_rtl.h"

/* The kernel32 functions a program calls most often, run in the guest as Windows runs them in user mode: the process heap, critical
 * sections, SRW locks and the waking of condition variables. The runtime forwards kernel32's exports of these names here, so a call
 * costs what a function call costs. Waiting stays with the runtime's scheduler: a lock that is held, and a condition variable with
 * sleepers, go to the runtime's implementation through ntdll. */

#define LAST_ERROR (*(volatile ULONG *)((PUCHAR)NtCurrentTeb() + 0x68))
#define THREAD_ID ((ULONG)(ULONG_PTR)TEB_THREAD_ID)

/* The runtime's waits, served by its scheduler. */
VOID NTAPI RtlWakeConditionVariable(PRTL_CONDITION_VARIABLE variable);
VOID NTAPI RtlWakeAllConditionVariable(PRTL_CONDITION_VARIABLE variable);
NTSTATUS NTAPI RtlWaitOnAddress(volatile VOID *address, PVOID compare, SIZE_T size, PLARGE_INTEGER timeout);

/* ---- the heap: the flags kernel32 accepts, and the error it reports when the heap fails ---- */
PVOID WINAPI RavenHeapAlloc(HANDLE heap, ULONG flags, SIZE_T size)
{
	PVOID block;
	if (flags & ~13U) {
		LAST_ERROR = ERROR_CALL_NOT_IMPLEMENTED;
		return NULL;
	}
	block = RtlAllocateHeap(heap, flags, size);
	if (!block)
		LAST_ERROR = ERROR_NOT_ENOUGH_MEMORY;
	return block;
}

PVOID WINAPI RavenHeapReAlloc(HANDLE heap, ULONG flags, PVOID block, SIZE_T size)
{
	PVOID moved;
	if (flags & ~29U) {
		LAST_ERROR = ERROR_CALL_NOT_IMPLEMENTED;
		return NULL;
	}
	moved = RtlReAllocateHeap(heap, flags, block, size);
	if (!moved)
		LAST_ERROR = ERROR_NOT_ENOUGH_MEMORY;
	return moved;
}

BOOL WINAPI RavenHeapFree(HANDLE heap, ULONG flags, PVOID block)
{
	if (flags & ~1U) {
		LAST_ERROR = ERROR_CALL_NOT_IMPLEMENTED;
		return FALSE;
	}
	if (!RtlFreeHeap(heap, flags, block)) {
		LAST_ERROR = ERROR_INVALID_PARAMETER;
		return FALSE;
	}
	return TRUE;
}

SIZE_T WINAPI RavenHeapSize(HANDLE heap, ULONG flags, LPCVOID block)
{
	SIZE_T size;
	if (flags & ~1U) {
		LAST_ERROR = ERROR_CALL_NOT_IMPLEMENTED;
		return (SIZE_T)-1;
	}
	size = RtlSizeHeap(heap, flags, (PVOID)block);
	if (size == (SIZE_T)-1)
		LAST_ERROR = ERROR_INVALID_PARAMETER;
	return size;
}

HANDLE WINAPI RavenGetProcessHeap(VOID)
{
	return PEB_PROCESS_HEAP;
}

/* ---- critical sections: without spinning, since only one guest thread runs at a time and spinning only delays the owner. The wait goes on
   LockCount itself, so nothing here needs a handle a program could close by mistake. ---- */

/* A section has a debug record unless its caller asks for none (CRITICAL_SECTION_NO_DEBUG_INFO), as Windows documents and as the runtime's
   32-bit sections have; callers may write into it, as the components built from Wine sources name their sections there. The records live
   in a heap of their own, made when the first is asked for, so deleting a section gives back only a record that came from there and
   leaves a program's own, static one alone. Without memory for a record, DebugInfo holds -1, as for a section without one. */
static PVOID section_records;

static PRTL_CRITICAL_SECTION_DEBUG new_section_record(PRTL_CRITICAL_SECTION section)
{
	PVOID records = section_records;
	PRTL_CRITICAL_SECTION_DEBUG record;
	if (!records) {
		PVOID made = RtlCreateHeap(HEAP_GROWABLE, NULL, 0, 0, NULL, NULL);
		if (!made)
			return NULL;
		records = InterlockedCompareExchangePointer(&section_records, made, NULL);
		if (records)
			RtlDestroyHeap(made);
		else
			records = made;
	}
	record = RtlAllocateHeap(records, HEAP_ZERO_MEMORY, sizeof(*record));
	if (!record)
		return NULL;
	record->Type = RTL_CRITSECT_TYPE;
	record->CriticalSection = section;
	record->ProcessLocksList.Flink = record->ProcessLocksList.Blink = &record->ProcessLocksList;
	return record;
}

static void init_section(PRTL_CRITICAL_SECTION section, BOOLEAN debug)
{
	PRTL_CRITICAL_SECTION_DEBUG record = debug ? new_section_record(section) : NULL;
	section->DebugInfo = record ? record : (PRTL_CRITICAL_SECTION_DEBUG)(LONG_PTR)-1;
	section->LockCount = -1;
	section->RecursionCount = 0;
	section->OwningThread = NULL;
	section->LockSemaphore = NULL;
	section->SpinCount = 0;
}

VOID WINAPI RavenInitializeCriticalSection(PRTL_CRITICAL_SECTION section)
{
	init_section(section, TRUE);
}

BOOL WINAPI RavenInitializeCriticalSectionAndSpinCount(PRTL_CRITICAL_SECTION section, ULONG spin)
{
	(void)spin;
	init_section(section, TRUE);
	return TRUE;
}

BOOL WINAPI RavenInitializeCriticalSectionEx(PRTL_CRITICAL_SECTION section, ULONG spin, ULONG flags)
{
	(void)spin;
	/* Only the high byte holds RTL flags; the low bits are ignored during initialization. */
	flags &= RTL_CRITICAL_SECTION_ALL_FLAG_BITS;
	if (flags & ~0x1f000000U) {
		LAST_ERROR = ERROR_INVALID_PARAMETER;
		return FALSE;
	}
	init_section(section, !(flags & RTL_CRITICAL_SECTION_FLAG_NO_DEBUG_INFO));
	return TRUE;
}

VOID WINAPI RavenDeleteCriticalSection(PRTL_CRITICAL_SECTION section)
{
	PRTL_CRITICAL_SECTION_DEBUG record = section->DebugInfo;
	/* The heap gives back only what it holds; any other record is not this component's to free. */
	if (record && record != (PRTL_CRITICAL_SECTION_DEBUG)(LONG_PTR)-1 && section_records)
		RtlFreeHeap(section_records, 0, record);
	section->DebugInfo = NULL;
	section->LockCount = -1;
	section->RecursionCount = 0;
	section->OwningThread = NULL;
}

/* Entering takes LockCount from -1 to 0 with one exchange; a section that is held is waited for on that word, which the runtime's
   scheduler looks at again on every round. Leaving puts -1 back and hands the section to nobody, so a thread that releases it and asks
   again gets it back at once, as Windows has done since Vista: handing it to the next waiter would make the threads that share one
   processor here take turns through the scheduler on every entry. */
VOID WINAPI RavenEnterCriticalSection(PRTL_CRITICAL_SECTION section)
{
	HANDLE self = TEB_THREAD_ID;
	LONG held;
	if (section->OwningThread == self) {
		section->RecursionCount++;
		return;
	}
	while (InterlockedCompareExchange(&section->LockCount, 0, -1) != -1) {
		held = section->LockCount;
		if (held != -1)
			RtlWaitOnAddress(&section->LockCount, &held, sizeof(held), NULL);
	}
	section->OwningThread = self;
	section->RecursionCount = 1;
}

VOID WINAPI RavenLeaveCriticalSection(PRTL_CRITICAL_SECTION section)
{
	if (--section->RecursionCount)
		return;
	section->OwningThread = NULL;
	InterlockedExchange(&section->LockCount, -1);
}

/* ntdll's forms of the same, which programs that call ntdll directly (Zig's standard library) use; they answer an NTSTATUS. */
NTSTATUS NTAPI RavenRtlEnterCriticalSection(PRTL_CRITICAL_SECTION section)
{
	RavenEnterCriticalSection(section);
	return STATUS_SUCCESS;
}

NTSTATUS NTAPI RavenRtlLeaveCriticalSection(PRTL_CRITICAL_SECTION section)
{
	RavenLeaveCriticalSection(section);
	return STATUS_SUCCESS;
}

NTSTATUS NTAPI RavenRtlInitializeCriticalSection(PRTL_CRITICAL_SECTION section)
{
	init_section(section, TRUE);
	return STATUS_SUCCESS;
}

NTSTATUS NTAPI RavenRtlInitializeCriticalSectionAndSpinCount(PRTL_CRITICAL_SECTION section, ULONG spin)
{
	(void)spin;
	init_section(section, TRUE);
	return STATUS_SUCCESS;
}

/* The flags are checked as kernel32's form checks them; a bad one is answered with a status instead of the last error. */
NTSTATUS NTAPI RavenRtlInitializeCriticalSectionEx(PRTL_CRITICAL_SECTION section, ULONG spin, ULONG flags)
{
	(void)spin;
	if ((flags & RTL_CRITICAL_SECTION_ALL_FLAG_BITS) & ~0x1f000000U)
		return STATUS_INVALID_PARAMETER;
	init_section(section, !(flags & RTL_CRITICAL_SECTION_FLAG_NO_DEBUG_INFO));
	return STATUS_SUCCESS;
}

NTSTATUS NTAPI RavenRtlDeleteCriticalSection(PRTL_CRITICAL_SECTION section)
{
	RavenDeleteCriticalSection(section);
	return STATUS_SUCCESS;
}

BOOL WINAPI RavenTryEnterCriticalSection(PRTL_CRITICAL_SECTION section)
{
	HANDLE self = TEB_THREAD_ID;
	if (section->OwningThread == self) {
		section->RecursionCount++;
		return TRUE;
	}
	if (InterlockedCompareExchange(&section->LockCount, 0, -1) != -1)
		return FALSE;
	section->OwningThread = self;
	section->RecursionCount = 1;
	return TRUE;
}

/* ---- SRW locks: one word, 1 when held exclusively, shared holders counting in steps of 2, as the runtime keeps them; a lock that is
   busy is waited for by the runtime, which sees it become free when the scheduler looks again ---- */
VOID WINAPI RavenInitializeSRWLock(PRTL_SRWLOCK lock) { lock->Ptr = NULL; }

VOID WINAPI RavenAcquireSRWLockExclusive(PRTL_SRWLOCK lock)
{
	if (!InterlockedCompareExchangePointer(&lock->Ptr, (PVOID)1, NULL))
		return;
	RtlAcquireSRWLockExclusive(lock);
}

BOOLEAN WINAPI RavenTryAcquireSRWLockExclusive(PRTL_SRWLOCK lock)
{
	return InterlockedCompareExchangePointer(&lock->Ptr, (PVOID)1, NULL) == NULL;
}

VOID WINAPI RavenReleaseSRWLockExclusive(PRTL_SRWLOCK lock)
{
	InterlockedExchangePointer(&lock->Ptr, NULL);
}

VOID WINAPI RavenAcquireSRWLockShared(PRTL_SRWLOCK lock)
{
	ULONG_PTR value = (ULONG_PTR)lock->Ptr;
	if (!(value & 1) && InterlockedCompareExchangePointer(&lock->Ptr, (PVOID)(value + 2), (PVOID)value) == (PVOID)value)
		return;
	RtlAcquireSRWLockShared(lock);
}

BOOLEAN WINAPI RavenTryAcquireSRWLockShared(PRTL_SRWLOCK lock)
{
	for (;;) {
		ULONG_PTR value = (ULONG_PTR)lock->Ptr;
		if (value & 1)
			return FALSE;
		if (InterlockedCompareExchangePointer(&lock->Ptr, (PVOID)(value + 2), (PVOID)value) == (PVOID)value)
			return TRUE;
	}
}

VOID WINAPI RavenReleaseSRWLockShared(PRTL_SRWLOCK lock)
{
	for (;;) {
		ULONG_PTR value = (ULONG_PTR)lock->Ptr;
		if (value < 2)
			return;
		if (InterlockedCompareExchangePointer(&lock->Ptr, (PVOID)(value - 2), (PVOID)value) == (PVOID)value)
			return;
	}
}

/* ---- condition variables: the runtime keeps the number of sleepers in the variable, so waking one that nobody waits on is free ---- */
VOID WINAPI RavenInitializeConditionVariable(PRTL_CONDITION_VARIABLE variable) { variable->Ptr = NULL; }

VOID WINAPI RavenWakeConditionVariable(PRTL_CONDITION_VARIABLE variable)
{
	if (*(volatile PVOID *)&variable->Ptr)
		RtlWakeConditionVariable(variable);
}

VOID WINAPI RavenWakeAllConditionVariable(PRTL_CONDITION_VARIABLE variable)
{
	if (*(volatile PVOID *)&variable->Ptr)
		RtlWakeAllConditionVariable(variable);
}

/* ---- fibers: switching fibers only swaps registers and thread block fields, which the program's own thread does without the runtime.
   The current fiber is the thread block's FiberData (gs:0x20), whose first word is the fiber's parameter, as GetFiberData reads it.
   A switch keeps the callee-saved registers (xmm6 to xmm15 among them) and the floating point control words on the stack it leaves,
   and the stack bounds of the thread block (StackBase, StackLimit, DeallocationStack) in the fiber. Fiber-local storage stays the
   thread's. ---- */
typedef struct {
	PVOID data;
	PVOID stack_base, stack_limit, deallocation_stack, stack_pointer;
	LPFIBER_START_ROUTINE start;
	/* The stack the fiber was created with, freed with it; null for a thread converted to a fiber. */
	PVOID allocation;
	SIZE_T allocation_size;
} RAVEN_FIBER;

/* SameTebFlags (TEB 0x17ee) bit HasFiberData, which IsThreadAFiber reads. */
#define RAVEN_HAS_FIBER_DATA 0x4

static USHORT *raven_same_teb_flags(void) { return (USHORT *)((PUCHAR)NtCurrentTeb() + 0x17ee); }
/* The TIB's FiberData. */
static RAVEN_FIBER **raven_fiber_slot(void) { return (RAVEN_FIBER **)((PUCHAR)NtCurrentTeb() + 0x20); }
static RAVEN_FIBER *raven_current_fiber(void) { return *raven_fiber_slot(); }

static void raven_set_fiber(RAVEN_FIBER *fiber)
{
	*raven_fiber_slot() = fiber;
	if (fiber)
		*raven_same_teb_flags() |= RAVEN_HAS_FIBER_DATA;
	else
		*raven_same_teb_flags() &= (USHORT)~RAVEN_HAS_FIBER_DATA;
}

/* SwitchToFiber: the record's stack bounds sit at 8, 16 and 24 and its stack pointer at 32. */
__asm__(".text\n"
	".globl RavenSwitchToFiber\n"
	"RavenSwitchToFiber:\n\t"
	"pushq %rbp\n\tpushq %rbx\n\tpushq %rdi\n\tpushq %rsi\n\tpushq %r12\n\tpushq %r13\n\tpushq %r14\n\tpushq %r15\n\t"
	"subq $168, %rsp\n\t"
	"movaps %xmm6, 0(%rsp)\n\tmovaps %xmm7, 16(%rsp)\n\tmovaps %xmm8, 32(%rsp)\n\tmovaps %xmm9, 48(%rsp)\n\t"
	"movaps %xmm10, 64(%rsp)\n\tmovaps %xmm11, 80(%rsp)\n\tmovaps %xmm12, 96(%rsp)\n\tmovaps %xmm13, 112(%rsp)\n\t"
	"movaps %xmm14, 128(%rsp)\n\tmovaps %xmm15, 144(%rsp)\n\t"
	"stmxcsr 160(%rsp)\n\tfnstcw 164(%rsp)\n\t"
	"movq %gs:0x20, %rax\n\t"
	"movq %rsp, 32(%rax)\n\t"
	"movq %gs:0x8, %rdx\n\tmovq %rdx, 8(%rax)\n\t"
	"movq %gs:0x10, %rdx\n\tmovq %rdx, 16(%rax)\n\t"
	"movq %gs:0x1478, %rdx\n\tmovq %rdx, 24(%rax)\n\t"
	"movq %rcx, %gs:0x20\n\t"
	"movq 8(%rcx), %rdx\n\tmovq %rdx, %gs:0x8\n\t"
	"movq 16(%rcx), %rdx\n\tmovq %rdx, %gs:0x10\n\t"
	"movq 24(%rcx), %rdx\n\tmovq %rdx, %gs:0x1478\n\t"
	"movq 32(%rcx), %rsp\n\t"
	"ldmxcsr 160(%rsp)\n\tfldcw 164(%rsp)\n\t"
	"movaps 0(%rsp), %xmm6\n\tmovaps 16(%rsp), %xmm7\n\tmovaps 32(%rsp), %xmm8\n\tmovaps 48(%rsp), %xmm9\n\t"
	"movaps 64(%rsp), %xmm10\n\tmovaps 80(%rsp), %xmm11\n\tmovaps 96(%rsp), %xmm12\n\tmovaps 112(%rsp), %xmm13\n\t"
	"movaps 128(%rsp), %xmm14\n\tmovaps 144(%rsp), %xmm15\n\t"
	"addq $168, %rsp\n\t"
	"popq %r15\n\tpopq %r14\n\tpopq %r13\n\tpopq %r12\n\tpopq %rsi\n\tpopq %rdi\n\tpopq %rbx\n\tpopq %rbp\n\t"
	"ret\n");
VOID WINAPI RavenSwitchToFiber(PVOID fiber);

/* A fiber whose start routine returns ends its thread, as on Windows. */
void WINAPI raven_fiber_main(void)
{
	RAVEN_FIBER *fiber = raven_current_fiber();
	fiber->start(fiber->data);
	NtTerminateThread(((HANDLE)(LONG_PTR)-2), 0);
}

/* Where a new fiber first runs, on a 16-byte aligned stack: home space, kept aligned for the call, then the routine above. */
__asm__(".text\n"
	"raven_fiber_entry:\n\t"
	"subq $48, %rsp\n\t"
	"call raven_fiber_main\n");
void raven_fiber_entry(void);

PVOID WINAPI RavenCreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE start, PVOID parameter)
{
	SIZE_T size = reserve > commit ? reserve : commit;
	RAVEN_FIBER *fiber;
	PVOID base = NULL;
	if (!start) {
		LAST_ERROR = ERROR_INVALID_PARAMETER;
		return NULL;
	}
	size = ((size ? size : 1024 * 1024) + 0xffff) & ~(SIZE_T)0xffff;
	fiber = RtlAllocateHeap(PEB_PROCESS_HEAP, HEAP_ZERO_MEMORY, sizeof(*fiber));
	if (!fiber || !NT_SUCCESS(ZwAllocateVirtualMemory(CURRENT_PROCESS, &base, 0, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) {
		if (fiber)
			RtlFreeHeap(PEB_PROCESS_HEAP, 0, fiber);
		LAST_ERROR = ERROR_NOT_ENOUGH_MEMORY;
		return NULL;
	}
	fiber->allocation = base;
	fiber->allocation_size = size;
	fiber->data = parameter;
	fiber->start = start;
	fiber->stack_base = (PUCHAR)base + size;
	fiber->stack_limit = base;
	fiber->deallocation_stack = base;
	/* The frame a switch resumes from, 16-byte aligned 256 bytes below the top: xmm6 to xmm15, the control words, the eight saved
	   registers and the return into the entry. */
	{
		PUCHAR frame = (PUCHAR)fiber->stack_base - 256;
		*(ULONG *)(frame + 160) = 0x1f80;
		*(USHORT *)(frame + 164) = 0x27f;
		((ULONG_PTR *)frame)[29] = (ULONG_PTR)raven_fiber_entry;
		fiber->stack_pointer = frame;
	}
	(void)flags;
	return fiber;
}

PVOID WINAPI RavenCreateFiber(SIZE_T size, LPFIBER_START_ROUTINE start, PVOID parameter) { return RavenCreateFiberEx(size, 0, 0, start, parameter); }

BOOL WINAPI RavenIsThreadAFiber(void) { return (*raven_same_teb_flags() & RAVEN_HAS_FIBER_DATA) != 0; }

PVOID WINAPI RavenConvertThreadToFiberEx(PVOID parameter, DWORD flags)
{
	RAVEN_FIBER *fiber;
	(void)flags;
	if (RavenIsThreadAFiber()) {
		LAST_ERROR = ERROR_ALREADY_FIBER;
		return NULL;
	}
	fiber = RtlAllocateHeap(PEB_PROCESS_HEAP, HEAP_ZERO_MEMORY, sizeof(*fiber));
	if (!fiber) {
		LAST_ERROR = ERROR_NOT_ENOUGH_MEMORY;
		return NULL;
	}
	fiber->data = parameter;
	raven_set_fiber(fiber);
	return fiber;
}

PVOID WINAPI RavenConvertThreadToFiber(PVOID parameter) { return RavenConvertThreadToFiberEx(parameter, 0); }

BOOL WINAPI RavenConvertFiberToThread(void)
{
	RAVEN_FIBER *fiber;
	if (!RavenIsThreadAFiber()) {
		LAST_ERROR = ERROR_ALREADY_THREAD;
		return FALSE;
	}
	fiber = raven_current_fiber();
	raven_set_fiber(NULL);
	/* A thread runs on in whatever fiber it is in; only the record of a converted thread is its own to free. */
	if (fiber && !fiber->allocation)
		RtlFreeHeap(PEB_PROCESS_HEAP, 0, fiber);
	return TRUE;
}

VOID WINAPI RavenDeleteFiber(PVOID handle)
{
	RAVEN_FIBER *fiber = handle;
	SIZE_T size = 0;
	if (!fiber)
		return;
	/* Deleting the running fiber ends the thread, whose stack that is. */
	if (fiber == raven_current_fiber())
		NtTerminateThread(((HANDLE)(LONG_PTR)-2), 1);
	if (fiber->allocation)
		ZwFreeVirtualMemory(CURRENT_PROCESS, &fiber->allocation, &size, MEM_RELEASE);
	RtlFreeHeap(PEB_PROCESS_HEAP, 0, fiber);
}
