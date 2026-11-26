/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "raven_rtl.h"

/* The kernel32 functions a program calls most often, run in the guest as Windows runs them in user mode: the process heap, critical
 * sections, SRW locks and condition variables. The runtime forwards kernel32's exports of these names here, so a call costs what a
 * function call costs. Waiting stays with the runtime's scheduler: a thread waits for a held lock or a condition variable on its word,
 * and the release or wake that changes the word wakes it, both through ntdll. */

#define LAST_ERROR (*(volatile ULONG *)((PUCHAR)NtCurrentTeb() + 0x68))
#define THREAD_ID ((ULONG)(ULONG_PTR)TEB_THREAD_ID)

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

/* ---- COM's task allocator, whose blocks are the process heap's: the memory the runtime hands a program to free with CoTaskMemFree
   comes from the same heap. ole32's and combase's exports of these names are forwarded here. ---- */
PVOID WINAPI RavenCoTaskMemAlloc(SIZE_T size)
{
	return RtlAllocateHeap(PEB_PROCESS_HEAP, 0, size ? size : 1);
}

/* No block reallocates as a new one; a size of zero frees the block and answers NULL. */
PVOID WINAPI RavenCoTaskMemRealloc(PVOID block, SIZE_T size)
{
	if (!block)
		return RavenCoTaskMemAlloc(size);
	if (!size) {
		RtlFreeHeap(PEB_PROCESS_HEAP, 0, block);
		return NULL;
	}
	return RtlReAllocateHeap(PEB_PROCESS_HEAP, 0, block, size);
}

VOID WINAPI RavenCoTaskMemFree(PVOID block)
{
	if (block)
		RtlFreeHeap(PEB_PROCESS_HEAP, 0, block);
}

/* ---- critical sections: without spinning, since only one guest thread runs at a time and spinning only delays the owner. The wait goes on
   LockCount itself, so nothing here needs a handle a program could close by mistake. ---- */

/* A section has a debug record unless its caller asks for none (CRITICAL_SECTION_NO_DEBUG_INFO), as Windows documents and as the runtime's
   32-bit sections have; callers may write into it, as the components built from Wine sources name their sections there. The records live
   in a pool of their own outside the process's list of heaps, as Windows keeps them, so deleting a section gives back only a record that
   came from there and leaves a program's own, static one alone. Without memory for a record, DebugInfo holds -1, as for a section without one. */
typedef struct section_block {
	struct section_block *next;
	RTL_CRITICAL_SECTION_DEBUG records[(0x10000 - sizeof(void *) * 2) / sizeof(RTL_CRITICAL_SECTION_DEBUG)];
} section_block;

static section_block *section_blocks;
static PVOID section_free, section_lock;
static ULONG section_used;

static void lock_sections(void)
{
	while (InterlockedCompareExchangePointer(&section_lock, (PVOID)1, NULL))
		YieldProcessor();
}

static void unlock_sections(void)
{
	InterlockedExchangePointer(&section_lock, NULL);
}

static PRTL_CRITICAL_SECTION_DEBUG new_section_record(PRTL_CRITICAL_SECTION section)
{
	PRTL_CRITICAL_SECTION_DEBUG record = NULL;
	lock_sections();
	if (section_free) {
		record = section_free;
		section_free = *(PVOID *)record;
	} else {
		if (!section_blocks || section_used == ARRAYSIZE(section_blocks->records)) {
			PVOID base = NULL;
			SIZE_T size = sizeof(section_block);
			if (NT_SUCCESS(ZwAllocateVirtualMemory(CURRENT_PROCESS, &base, 0, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) {
				((section_block *)base)->next = section_blocks;
				section_blocks = base;
				section_used = 0;
			}
		}
		if (section_blocks && section_used < ARRAYSIZE(section_blocks->records))
			record = &section_blocks->records[section_used++];
	}
	unlock_sections();
	if (!record)
		return NULL;
	ZeroMemory(record, sizeof(*record));
	record->Type = RTL_CRITSECT_TYPE;
	record->CriticalSection = section;
	record->ProcessLocksList.Flink = record->ProcessLocksList.Blink = &record->ProcessLocksList;
	return record;
}

/* Only what the pool handed out goes back to it: a program may give a section a record of its own. */
static void free_section_record(PRTL_CRITICAL_SECTION_DEBUG record)
{
	lock_sections();
	for (section_block *block = section_blocks; block; block = block->next)
		if ((BYTE *)record >= (BYTE *)block->records && (BYTE *)record < (BYTE *)(block->records + ARRAYSIZE(block->records))) {
			*(PVOID *)record = section_free;
			section_free = record;
			break;
		}
	unlock_sections();
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
	if (record && record != (PRTL_CRITICAL_SECTION_DEBUG)(LONG_PTR)-1)
		free_section_record(record);
	section->DebugInfo = NULL;
	section->LockCount = -1;
	section->RecursionCount = 0;
	section->OwningThread = NULL;
}

/* LockCount is -1 when the section is free, 0 when it is held and 1 when it is held and a thread waits for it, as a futex keeps a
   mutex. A thread that finds it held marks it so and waits on the word; leaving puts -1 back and wakes one waiter only when one was
   marked. The section goes to nobody in particular, so a thread that releases it and asks again gets it back at once, as Windows has
   done since Vista: handing it to the next waiter would make the threads take turns through the scheduler on every entry. */
VOID WINAPI RavenEnterCriticalSection(PRTL_CRITICAL_SECTION section)
{
	HANDLE self = TEB_THREAD_ID;
	LONG state, contended = 1;
	if (section->OwningThread == self) {
		section->RecursionCount++;
		return;
	}
	state = InterlockedCompareExchange(&section->LockCount, 0, -1);
	if (state != -1) {
		if (state != 1)
			state = InterlockedExchange(&section->LockCount, 1);
		while (state != -1) {
			RtlWaitOnAddress(&section->LockCount, &contended, sizeof(contended), NULL);
			state = InterlockedExchange(&section->LockCount, 1);
		}
	}
	section->OwningThread = self;
	section->RecursionCount = 1;
}

VOID WINAPI RavenLeaveCriticalSection(PRTL_CRITICAL_SECTION section)
{
	if (--section->RecursionCount)
		return;
	section->OwningThread = NULL;
	if (InterlockedExchange(&section->LockCount, -1) == 1)
		RtlWakeAddressSingle((PVOID)&section->LockCount);
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

/* ---- SRW locks: one word, 1 when held exclusively, shared holders counting in steps of 2, and its top bit set while a thread waits
   for it. A thread that finds the lock busy sets that bit and waits on the word; the release that frees the lock clears the word and
   wakes every waiter, which then tries again, as a futex-based lock does ---- */
#define SRW_WAITING ((ULONG_PTR)1 << (sizeof(ULONG_PTR) * 8 - 1))

/* Waits until the word is no longer the busy value it was seen as, after marking that a thread waits. */
static void srw_wait(PRTL_SRWLOCK lock, ULONG_PTR value)
{
	if (!(value & SRW_WAITING) &&
		InterlockedCompareExchangePointer(&lock->Ptr, (PVOID)(value | SRW_WAITING), (PVOID)value) != (PVOID)value)
		return;
	value |= SRW_WAITING;
	RtlWaitOnAddress(&lock->Ptr, &value, sizeof(value), NULL);
}

VOID WINAPI RavenInitializeSRWLock(PRTL_SRWLOCK lock) { lock->Ptr = NULL; }

VOID WINAPI RavenAcquireSRWLockExclusive(PRTL_SRWLOCK lock)
{
	for (;;) {
		ULONG_PTR value = (ULONG_PTR)lock->Ptr;
		if (!(value & ~SRW_WAITING)) {
			if (InterlockedCompareExchangePointer(&lock->Ptr, (PVOID)(value | 1), (PVOID)value) == (PVOID)value)
				return;
			continue;
		}
		srw_wait(lock, value);
	}
}

BOOLEAN WINAPI RavenTryAcquireSRWLockExclusive(PRTL_SRWLOCK lock)
{
	ULONG_PTR value = (ULONG_PTR)lock->Ptr;
	if (value & ~SRW_WAITING)
		return FALSE;
	return InterlockedCompareExchangePointer(&lock->Ptr, (PVOID)(value | 1), (PVOID)value) == (PVOID)value;
}

VOID WINAPI RavenReleaseSRWLockExclusive(PRTL_SRWLOCK lock)
{
	if ((ULONG_PTR)InterlockedExchangePointer(&lock->Ptr, NULL) & SRW_WAITING)
		RtlWakeAddressAll(&lock->Ptr);
}

VOID WINAPI RavenAcquireSRWLockShared(PRTL_SRWLOCK lock)
{
	for (;;) {
		ULONG_PTR value = (ULONG_PTR)lock->Ptr;
		if (!(value & 1)) {
			if (InterlockedCompareExchangePointer(&lock->Ptr, (PVOID)(value + 2), (PVOID)value) == (PVOID)value)
				return;
			continue;
		}
		srw_wait(lock, value);
	}
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

/* The last shared holder frees the lock: it clears the word, and wakes the waiters if any were marked. */
VOID WINAPI RavenReleaseSRWLockShared(PRTL_SRWLOCK lock)
{
	for (;;) {
		ULONG_PTR value = (ULONG_PTR)lock->Ptr, holders = value & ~SRW_WAITING;
		if (holders < 2)
			return;
		ULONG_PTR next = holders == 2 ? 0 : value - 2;
		if (InterlockedCompareExchangePointer(&lock->Ptr, (PVOID)next, (PVOID)value) == (PVOID)value) {
			if (!next && (value & SRW_WAITING))
				RtlWakeAddressAll(&lock->Ptr);
			return;
		}
	}
}

/* ---- condition variables: one word, its low half a count of wakes and its high half how many threads sleep on it. A sleeper counts
   itself and reads the wakes in one step, releases the lock and waits on the low half for it to change; a wake counts itself and wakes
   the word only if somebody sleeps. As both change the word with one interlocked operation, a wake either sees the sleeper counted or
   changes the value the sleeper then waits on, so no wake is lost. A sleeper may wake without a wake, which waits on a condition
   variable allow. ---- */
#define CV_SLEEPER ((LONG64)1 << 32)

VOID WINAPI RavenInitializeConditionVariable(PRTL_CONDITION_VARIABLE variable) { variable->Ptr = NULL; }

static void cv_wake(PRTL_CONDITION_VARIABLE variable, BOOL all)
{
	LONG64 value, next;
	do {
		value = *(volatile LONG64 *)&variable->Ptr;
		next = (value & ~(LONG64)0xffffffff) | (ULONG)(value + 1);
	} while (InterlockedCompareExchange64((volatile LONG64 *)&variable->Ptr, next, value) != value);
	if (!(next >> 32))
		return;
	if (all)
		RtlWakeAddressAll(&variable->Ptr);
	else
		RtlWakeAddressSingle(&variable->Ptr);
}

VOID WINAPI RavenWakeConditionVariable(PRTL_CONDITION_VARIABLE variable) { cv_wake(variable, FALSE); }
VOID WINAPI RavenWakeAllConditionVariable(PRTL_CONDITION_VARIABLE variable) { cv_wake(variable, TRUE); }

/* Sleeps until a wake or the time-out, with the lock released meanwhile: STATUS_SUCCESS or STATUS_TIMEOUT. */
static NTSTATUS cv_sleep(PRTL_CONDITION_VARIABLE variable, PRTL_SRWLOCK lock, PRTL_CRITICAL_SECTION section, BOOL shared,
	const LARGE_INTEGER *timeout)
{
	ULONG wakes = (ULONG)InterlockedAdd64((volatile LONG64 *)&variable->Ptr, CV_SLEEPER);
	LARGE_INTEGER limit;
	NTSTATUS status;
	if (section)
		RavenLeaveCriticalSection(section);
	else if (shared)
		RavenReleaseSRWLockShared(lock);
	else
		RavenReleaseSRWLockExclusive(lock);
	if (timeout)
		limit = *timeout;
	status = RtlWaitOnAddress(&variable->Ptr, &wakes, sizeof(wakes), timeout ? &limit : NULL);
	InterlockedAdd64((volatile LONG64 *)&variable->Ptr, -CV_SLEEPER);
	if (section)
		RavenEnterCriticalSection(section);
	else if (shared)
		RavenAcquireSRWLockShared(lock);
	else
		RavenAcquireSRWLockExclusive(lock);
	return status == STATUS_TIMEOUT ? STATUS_TIMEOUT : STATUS_SUCCESS;
}

/* kernel32's milliseconds as an NT time-out, none for INFINITE; and its answer, FALSE with ERROR_TIMEOUT when the time ran out. */
static BOOL cv_answer(NTSTATUS status)
{
	if (status == STATUS_TIMEOUT) {
		LAST_ERROR = ERROR_TIMEOUT;
		return FALSE;
	}
	return TRUE;
}

static const LARGE_INTEGER *cv_timeout(DWORD milliseconds, LARGE_INTEGER *timeout)
{
	if (milliseconds == INFINITE)
		return NULL;
	timeout->QuadPart = -(LONGLONG)milliseconds * 10000;
	return timeout;
}

BOOL WINAPI RavenSleepConditionVariableSRW(PRTL_CONDITION_VARIABLE variable, PRTL_SRWLOCK lock, DWORD milliseconds, ULONG flags)
{
	LARGE_INTEGER timeout;
	return cv_answer(cv_sleep(variable, lock, NULL, flags & CONDITION_VARIABLE_LOCKMODE_SHARED, cv_timeout(milliseconds, &timeout)));
}

BOOL WINAPI RavenSleepConditionVariableCS(PRTL_CONDITION_VARIABLE variable, PRTL_CRITICAL_SECTION section, DWORD milliseconds)
{
	LARGE_INTEGER timeout;
	return cv_answer(cv_sleep(variable, NULL, section, FALSE, cv_timeout(milliseconds, &timeout)));
}

NTSTATUS NTAPI RavenRtlSleepConditionVariableSRW(PRTL_CONDITION_VARIABLE variable, PRTL_SRWLOCK lock, const LARGE_INTEGER *timeout,
	ULONG flags)
{
	return cv_sleep(variable, lock, NULL, flags & CONDITION_VARIABLE_LOCKMODE_SHARED, timeout);
}

NTSTATUS NTAPI RavenRtlSleepConditionVariableCS(PRTL_CONDITION_VARIABLE variable, PRTL_CRITICAL_SECTION section, const LARGE_INTEGER *timeout)
{
	return cv_sleep(variable, NULL, section, FALSE, timeout);
}

/* ---- waits on an address: kernel32's names over ntdll's, which answer with a status ---- */
BOOL WINAPI RavenWaitOnAddress(volatile VOID *address, PVOID compare, SIZE_T size, DWORD milliseconds)
{
	LARGE_INTEGER timeout;
	NTSTATUS status = RtlWaitOnAddress(address, compare, size, (PLARGE_INTEGER)cv_timeout(milliseconds, &timeout));
	if (status == STATUS_TIMEOUT) {
		LAST_ERROR = ERROR_TIMEOUT;
		return FALSE;
	}
	if (status) {
		LAST_ERROR = RtlNtStatusToDosError(status);
		return FALSE;
	}
	return TRUE;
}

VOID WINAPI RavenWakeByAddressSingle(PVOID address) { RtlWakeAddressSingle(address); }
VOID WINAPI RavenWakeByAddressAll(PVOID address) { RtlWakeAddressAll(address); }

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

/* ---- the I/O completion threads of BindIoCompletionCallback ---- */
/* The runtime binds each file to one completion port with the callback as the key, and starts threads here: each takes the packets of
 * the port in turn and hands the callback the error, the bytes moved and the operation, as the thread pool of Windows does. A packet
 * without an operation is not one of an I/O and is passed over. */
struct io_completion_binding {
	BOOL (WINAPI *dequeue)(HANDLE port, DWORD *bytes, ULONG_PTR *key, LPOVERLAPPED *operation, DWORD timeout);
	HANDLE port;
};

DWORD WINAPI RavenIoCompletionWorker(struct io_completion_binding *binding)
{
	for (;;) {
		DWORD bytes = 0;
		ULONG_PTR key = 0;
		LPOVERLAPPED operation = NULL;
		const BOOL done = binding->dequeue(binding->port, &bytes, &key, &operation, INFINITE);
		if (!operation || !key)
			continue;
		((LPOVERLAPPED_COMPLETION_ROUTINE)key)(done ? ERROR_SUCCESS : LAST_ERROR, bytes, operation);
	}
}

/* ---- the I/O completion threads of CreateThreadpoolIo ----
 * Each file bound to the pool posts to one completion port with its I/O object as the key; threads here hand each finished operation to
 * the object's callback with its context, the result and the bytes moved, and count the callbacks running so that a wait for them knows
 * when they are over. The runtime lays the object out and starts the threads. */
struct thread_pool_io {
	void (CALLBACK *callback)(void *instance, void *context, void *operation, ULONG result, ULONG_PTR bytes, void *io);
	void *context;
	volatile LONG running;
};

DWORD WINAPI RavenThreadpoolIoWorker(struct io_completion_binding *binding)
{
	for (;;) {
		DWORD bytes = 0;
		ULONG_PTR key = 0;
		LPOVERLAPPED operation = NULL;
		/* A callback instance is opaque to the callback, which only hands it back to the pool's own functions. */
		ULONG_PTR instance[8] = {0};
		struct thread_pool_io *io;
		const BOOL done = binding->dequeue(binding->port, &bytes, &key, &operation, INFINITE);
		if (!operation || !key)
			continue;
		io = (struct thread_pool_io *)key;
		InterlockedIncrement(&io->running);
		io->callback(instance, io->context, operation, done ? ERROR_SUCCESS : LAST_ERROR, bytes, io);
		InterlockedDecrement(&io->running);
	}
}

/* ---- the worker threads of SubmitThreadpoolWork ----
 * Windows queues submitted work and hands it to the pool's threads, which take one item after another; it adds a thread only when none
 * is free to take the next. Each submission is posted to the pool's completion port with its work item as the key. The workers count
 * themselves free while they wait, and the items taken, so that the runtime knows when the queue needs another thread; a work item
 * counts its submissions not yet finished, which a wait for its callbacks looks at. The runtime lays both out and starts the threads. */
struct thread_pool_work {
	void (CALLBACK *callback)(void *instance, void *context, void *work);
	void *context;
	void *handle;
	volatile LONG pending;
};

struct thread_pool_queue {
	BOOL (WINAPI *dequeue)(HANDLE port, DWORD *bytes, ULONG_PTR *key, LPOVERLAPPED *operation, DWORD timeout);
	HANDLE port;
	volatile LONG free;
	volatile LONG queued;
	volatile LONG taken;
};

DWORD WINAPI RavenThreadpoolWorker(struct thread_pool_queue *pool)
{
	for (;;) {
		DWORD bytes = 0;
		ULONG_PTR key = 0;
		LPOVERLAPPED operation = NULL;
		ULONG_PTR instance[8] = {0};
		struct thread_pool_work *work;
		InterlockedIncrement(&pool->free);
		pool->dequeue(pool->port, &bytes, &key, &operation, INFINITE);
		InterlockedDecrement(&pool->free);
		if (!key)
			continue;
		InterlockedDecrement(&pool->queued);
		InterlockedIncrement(&pool->taken);
		work = (struct thread_pool_work *)key;
		work->callback(instance, work->context, work->handle);
		InterlockedDecrement(&work->pending);
	}
}
