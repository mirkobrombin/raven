/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <rtl.h>

/* The kernel32 functions a program calls most often, run in the guest as Windows runs them in user mode: the process heap, critical
 * sections, SRW locks and the waking of condition variables. The runtime forwards kernel32's exports of these names here, so a call
 * costs what a function call costs. Waiting stays with the runtime's scheduler: a lock that is held, and a condition variable with
 * sleepers, go to the runtime's implementation through ntdll. */

#define LAST_ERROR (*(volatile ULONG *)((PUCHAR)NtCurrentTeb() + 0x68))
#define THREAD_ID ((ULONG)(ULONG_PTR)NtCurrentTeb()->ClientId.UniqueThread)

/* The runtime's waits, served by its scheduler. */
VOID NTAPI RtlAcquireSRWLockExclusive(PRTL_SRWLOCK lock);
VOID NTAPI RtlAcquireSRWLockShared(PRTL_SRWLOCK lock);
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
	return NtCurrentPeb()->ProcessHeap;
}

/* ---- critical sections: laid out and initialized by the RTL, without spinning, since only one guest thread runs at a time and
   spinning only delays the owner ---- */
VOID WINAPI RavenInitializeCriticalSection(PRTL_CRITICAL_SECTION section)
{
	RtlInitializeCriticalSectionAndSpinCount(section, 0);
}

BOOL WINAPI RavenInitializeCriticalSectionAndSpinCount(PRTL_CRITICAL_SECTION section, ULONG spin)
{
	return NT_SUCCESS(RtlInitializeCriticalSectionAndSpinCount(section, 0));
}

BOOL WINAPI RavenInitializeCriticalSectionEx(PRTL_CRITICAL_SECTION section, ULONG spin, ULONG flags)
{
	NTSTATUS status;
	/* Only the high byte holds RTL flags; the low bits are ignored during initialization. */
	flags &= RTL_CRITICAL_SECTION_ALL_FLAG_BITS;
	if (flags & ~0x1f000000U) {
		LAST_ERROR = ERROR_INVALID_PARAMETER;
		return FALSE;
	}
	status = RtlInitializeCriticalSectionEx(section, 0, flags);
	if (!NT_SUCCESS(status))
		LAST_ERROR = RtlNtStatusToDosError(status);
	return NT_SUCCESS(status);
}

VOID WINAPI RavenDeleteCriticalSection(PRTL_CRITICAL_SECTION section) { RtlDeleteCriticalSection(section); }

/* Entering takes LockCount from -1 to 0 with one exchange; a section that is held is waited for on that word, which the runtime's
   scheduler looks at again on every round. Leaving puts -1 back and hands the section to nobody, so a thread that releases it and asks
   again gets it back at once, as Windows has done since Vista: handing it to the next waiter would make the threads that share one
   processor here take turns through the scheduler on every entry. */
VOID WINAPI RavenEnterCriticalSection(PRTL_CRITICAL_SECTION section)
{
	HANDLE self = NtCurrentTeb()->ClientId.UniqueThread;
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

BOOL WINAPI RavenTryEnterCriticalSection(PRTL_CRITICAL_SECTION section)
{
	HANDLE self = NtCurrentTeb()->ClientId.UniqueThread;
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

/* ---- the heap lock: a word taken with one exchange, as an SRW lock is, so a thread that releases it and asks again gets it back at once
   instead of handing it to a waiter and queueing behind it; a held lock is waited for by the runtime, which sees it become free when the
   scheduler looks again. The thread that holds it may take it again: the heap code nests its locking. ---- */
typedef struct {
	PVOID word;
	HANDLE owner;
	LONG recursion;
} RAVEN_HEAP_LOCK;

C_ASSERT(sizeof(RAVEN_HEAP_LOCK) <= sizeof(HEAP_LOCK));

NTSTATUS NTAPI RtlInitializeHeapLock(IN OUT PHEAP_LOCK *Lock)
{
	RtlZeroMemory(*Lock, sizeof(RAVEN_HEAP_LOCK));
	return STATUS_SUCCESS;
}

NTSTATUS NTAPI RtlDeleteHeapLock(IN OUT PHEAP_LOCK Lock)
{
	return STATUS_SUCCESS;
}

NTSTATUS NTAPI RtlEnterHeapLock(IN OUT PHEAP_LOCK Lock, IN BOOLEAN Exclusive)
{
	RAVEN_HEAP_LOCK *lock = (RAVEN_HEAP_LOCK *)Lock;
	HANDLE self = NtCurrentTeb()->ClientId.UniqueThread;
	if (lock->owner == self) {
		lock->recursion++;
		return STATUS_SUCCESS;
	}
	if (InterlockedCompareExchangePointer(&lock->word, (PVOID)1, NULL))
		RtlAcquireSRWLockExclusive((PRTL_SRWLOCK)&lock->word);
	lock->owner = self;
	lock->recursion = 1;
	return STATUS_SUCCESS;
}

BOOLEAN NTAPI RtlTryEnterHeapLock(IN OUT PHEAP_LOCK Lock, IN BOOLEAN Exclusive)
{
	RAVEN_HEAP_LOCK *lock = (RAVEN_HEAP_LOCK *)Lock;
	HANDLE self = NtCurrentTeb()->ClientId.UniqueThread;
	if (lock->owner == self) {
		lock->recursion++;
		return TRUE;
	}
	if (InterlockedCompareExchangePointer(&lock->word, (PVOID)1, NULL))
		return FALSE;
	lock->owner = self;
	lock->recursion = 1;
	return TRUE;
}

NTSTATUS NTAPI RtlLeaveHeapLock(IN OUT PHEAP_LOCK Lock)
{
	RAVEN_HEAP_LOCK *lock = (RAVEN_HEAP_LOCK *)Lock;
	if (--lock->recursion)
		return STATUS_SUCCESS;
	lock->owner = NULL;
	InterlockedExchangePointer(&lock->word, NULL);
	return STATUS_SUCCESS;
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
