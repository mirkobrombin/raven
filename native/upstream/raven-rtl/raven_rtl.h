/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#ifndef RAVEN_RTL_H
#define RAVEN_RTL_H

#include <windows.h>
#include <winternl.h>
#include <ntstatus.h>

#ifndef HEAP_CREATE_ENABLE_EXECUTE
#define HEAP_CREATE_ENABLE_EXECUTE 0x00040000
#endif
#ifndef RTL_CRITICAL_SECTION_ALL_FLAG_BITS
#define RTL_CRITICAL_SECTION_ALL_FLAG_BITS 0xff000000U
#endif

/* The x64 PEB and TEB fields this component reads, at their documented offsets. */
#define PEB_FIELD(type, offset) (*(type *)((PUCHAR)NtCurrentTeb()->ProcessEnvironmentBlock + (offset)))
#define PEB_PROCESS_HEAP PEB_FIELD(PVOID, 0x30)
#define PEB_NT_GLOBAL_FLAG PEB_FIELD(ULONG, 0xbc)
#define PEB_NUMBER_OF_HEAPS PEB_FIELD(ULONG, 0xe8)
#define PEB_MAXIMUM_HEAPS PEB_FIELD(ULONG, 0xec)
#define PEB_PROCESS_HEAPS PEB_FIELD(PVOID *, 0xf0)
#define TEB_THREAD_ID (*(HANDLE *)((PUCHAR)NtCurrentTeb() + 0x48))

#define CURRENT_PROCESS ((HANDLE)(LONG_PTR)-1)

NTSYSAPI NTSTATUS NTAPI ZwAllocateVirtualMemory(HANDLE process, PVOID *base, ULONG_PTR zero_bits, PSIZE_T size, ULONG type, ULONG protection);
NTSYSAPI NTSTATUS NTAPI ZwFreeVirtualMemory(HANDLE process, PVOID *base, PSIZE_T size, ULONG type);
NTSYSAPI NTSTATUS NTAPI NtTerminateThread(HANDLE thread, NTSTATUS status);
NTSYSAPI VOID NTAPI RtlRaiseStatus(NTSTATUS status);
VOID NTAPI RtlAcquireSRWLockExclusive(PRTL_SRWLOCK lock);
VOID NTAPI RtlAcquireSRWLockShared(PRTL_SRWLOCK lock);

PVOID NTAPI RtlAllocateHeap(PVOID handle, ULONG flags, SIZE_T size);
PVOID NTAPI RtlReAllocateHeap(PVOID handle, ULONG flags, PVOID block, SIZE_T size);
BOOLEAN NTAPI RtlFreeHeap(PVOID handle, ULONG flags, PVOID block);
SIZE_T NTAPI RtlSizeHeap(PVOID handle, ULONG flags, PVOID block);
PVOID NTAPI RtlDestroyHeap(PVOID handle);

/* The generic table types, as the Windows DDK documents them. */
typedef struct _RTL_BALANCED_LINKS {
	struct _RTL_BALANCED_LINKS *Parent, *LeftChild, *RightChild;
	CHAR Balance;
	UCHAR Reserved[3];
} RTL_BALANCED_LINKS, *PRTL_BALANCED_LINKS;

typedef enum _RTL_GENERIC_COMPARE_RESULTS { GenericLessThan, GenericGreaterThan, GenericEqual } RTL_GENERIC_COMPARE_RESULTS;
typedef enum _TABLE_SEARCH_RESULT { TableEmptyTree, TableFoundNode, TableInsertAsLeft, TableInsertAsRight } TABLE_SEARCH_RESULT;

struct _RTL_AVL_TABLE;
typedef RTL_GENERIC_COMPARE_RESULTS(NTAPI *PRTL_AVL_COMPARE_ROUTINE)(struct _RTL_AVL_TABLE *table, PVOID first, PVOID second);
typedef PVOID(NTAPI *PRTL_AVL_ALLOCATE_ROUTINE)(struct _RTL_AVL_TABLE *table, ULONG size);
typedef VOID(NTAPI *PRTL_AVL_FREE_ROUTINE)(struct _RTL_AVL_TABLE *table, PVOID buffer);

typedef struct _RTL_AVL_TABLE {
	RTL_BALANCED_LINKS BalancedRoot;
	PVOID OrderedPointer;
	ULONG WhichOrderedElement, NumberGenericTableElements, DepthOfTree;
	PRTL_BALANCED_LINKS RestartKey;
	ULONG DeleteCount;
	PRTL_AVL_COMPARE_ROUTINE CompareRoutine;
	PRTL_AVL_ALLOCATE_ROUTINE AllocateRoutine;
	PRTL_AVL_FREE_ROUTINE FreeRoutine;
	PVOID TableContext;
} RTL_AVL_TABLE, *PRTL_AVL_TABLE;

#endif
