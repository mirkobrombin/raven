/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Replacement for the upstream private header: it keeps the public and shared-structure includes and drops the kernel-mode ones. */
#pragma once
#include <stdio.h>
#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#define OEMRESOURCE
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <winreg.h>
#include <windowsx.h>
#include <undocuser.h>
#include <wine/unicode.h>
#include <wine/debug.h>
#include <imm.h>
#include <immdev.h>
#include <imm32_undoc.h>
#ifndef _UNICODE_STRING_DEFINED
typedef struct _UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef const UNICODE_STRING *PCUNICODE_STRING;
#endif
typedef const char *PCSZ;
typedef USHORT RTL_ATOM, *PRTL_ATOM;
typedef LONG NTSTATUS;
typedef struct _OBJECT_ATTRIBUTES *POBJECT_ATTRIBUTES;
/* Only the client information array is read; its upstream offset in the thread block is kept. */
typedef struct _TEB { char reserved[0x800]; void *Win32ClientInfo[62]; } TEB, *PTEB;
typedef struct _STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; } STRING, *PSTRING;
#define FASTCALL
/* The kernel-mode header that defines the page size is not used here; the shared structures only assert against it. */
#ifndef PAGE_SIZE
#define PAGE_SIZE 0x1000
#endif
#include <ntusrtyp.h>
#include <ntuser.h>
#include <controls.h>
#include <regcontrol.h>
#include <ddeml.h>
#include <dde.h>
#define ASSERT(condition) ((void)0)
#define RtlGetProcessHeap() GetProcessHeap()
/* The guest heap is reached through the public heap functions. */
#define RtlAllocateHeap(heap, flags, size) HeapAlloc((heap), (flags), (size))
#define RtlFreeHeap(heap, flags, pointer) HeapFree((heap), (flags), (pointer))
#ifndef NT_SUCCESS
#define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)
#endif
#ifndef ALIGN_UP_BY
#define ALIGN_UP_BY(address, alignment) (((ULONG_PTR)(address) + (alignment) - 1) & ~((ULONG_PTR)(alignment) - 1))
#define ALIGN_UP(address, type) ALIGN_UP_BY(address, sizeof(type))
#define ALIGN_UP_POINTER_BY(pointer, alignment) ((PVOID)ALIGN_UP_BY(pointer, alignment))
#define ALIGN_UP_POINTER(pointer, type) ((PVOID)ALIGN_UP(pointer, type))
#endif
BOOLEAN NTAPI RtlIsThreadWithinLoaderCallout(VOID);
/* The string helpers the bridge answers: without these prototypes a 32-bit caller would call them as cdecl and lose the stack the
   stdcall bridge pops. */
BOOLEAN NTAPI RtlCreateUnicodeStringFromAsciiz(PUNICODE_STRING destination, PCSZ source);
VOID NTAPI RtlInitUnicodeString(PUNICODE_STRING destination, PCWSTR source);
VOID NTAPI RtlFreeUnicodeString(PUNICODE_STRING string);
#include <user32p.h>
#include <user_x.h>
