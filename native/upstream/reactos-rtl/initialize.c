/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <rtl.h>

BOOLEAN LdrpShutdownInProgress;
HANDLE LdrpShutdownThreadId;
static BOOLEAN initialized;
VOID NTAPI RtlpInitDeferredCriticalSection(VOID);
VOID NTAPI RtlInitializeHeapManager(VOID);
extern LARGE_INTEGER RtlpTimeout;

NTSTATUS NTAPI RavenInitializeRTL(PVOID *heaps, ULONG capacity)
{
	PPEB peb=NtCurrentPeb();
	if (initialized || !heaps || !capacity || capacity>512 || peb->ProcessHeaps!=heaps ||
		peb->MaximumNumberOfHeaps!=capacity || peb->NumberOfHeaps || peb->ProcessHeap || peb->NtGlobalFlag)
		return STATUS_INVALID_PARAMETER;
	// The wait of a thread for a critical section, as the process loader sets it up from the PEB.
	RtlpTimeout=peb->CriticalSectionTimeout;
	// A PEB without one has the Windows default of thirty days, in 100 ns units and relative.
	if (!RtlpTimeout.QuadPart)
		RtlpTimeout.QuadPart=-25920000000000LL;
	RtlpInitDeferredCriticalSection();
	RtlInitializeHeapManager();
	initialized=TRUE;
	return STATUS_SUCCESS;
}

PVOID NTAPI RavenCreateHeap(ULONG flags, PVOID base, SIZE_T reserve, SIZE_T commit,
	PVOID lock, PRTL_HEAP_PARAMETERS parameters)
{
	PPEB peb=NtCurrentPeb();
	// HEAP_NO_SERIALIZE, HEAP_GROWABLE, HEAP_GENERATE_EXCEPTIONS and HEAP_CREATE_ENABLE_EXECUTE, whose pages are committed executable.
	if (!initialized || (flags & ~(7U | HEAP_CREATE_ENABLE_EXECUTE)) || peb->NtGlobalFlag) {
		RtlSetLastWin32ErrorAndNtStatusFromNtStatus(STATUS_NOT_SUPPORTED);
		return NULL;
	}
	if (peb->NumberOfHeaps>=peb->MaximumNumberOfHeaps) {
		RtlSetLastWin32ErrorAndNtStatusFromNtStatus(STATUS_NO_MEMORY);
		if (flags & HEAP_GENERATE_EXCEPTIONS) RtlRaiseStatus(STATUS_NO_MEMORY);
		return NULL;
	}
	return RtlCreateHeap(flags,base,reserve,commit,lock,parameters);
}
