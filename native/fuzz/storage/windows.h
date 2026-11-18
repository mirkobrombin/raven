/* The Windows types and calls the compound file reader of ole32's storage (native/guest/raven-storage/container.c) uses, declared for
   building it on the host for native/fuzz/storage_container.c, which defines what is declared here. Built with -fshort-wchar, so that
   wide literals are the 16-bit units of Windows. objbase.h and objidl.h beside it only include this file. */
#ifndef RAVEN_FUZZ_STORAGE_WINDOWS_H
#define RAVEN_FUZZ_STORAGE_WINDOWS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD, ULONG;
typedef int32_t LONG, HRESULT;
typedef uint64_t ULONGLONG;
typedef size_t SIZE_T;
typedef wchar_t WCHAR;
typedef int BOOL;
typedef void *HANDLE;
typedef uintptr_t ULONG_PTR;
#define TRUE 1
#define FALSE 0
#define WINAPI

#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_OUTOFMEMORY ((HRESULT)0x8007000Eu)
#define E_FAIL ((HRESULT)0x80004005u)
#define STG_E_FILEALREADYEXISTS ((HRESULT)0x80030050u)
#define STG_E_MEDIUMFULL ((HRESULT)0x80030070u)
#define STG_E_OLDFORMAT ((HRESULT)0x80030104u)
#define STG_E_DOCFILECORRUPT ((HRESULT)0x80030109u)
#define SUCCEEDED(status) ((HRESULT)(status) >= 0)
#define FAILED(status) ((HRESULT)(status) < 0)

typedef struct
{
	uint32_t Data1;
	uint16_t Data2, Data3;
	uint8_t Data4[8];
} GUID, CLSID;

typedef struct
{
	DWORD dwLowDateTime, dwHighDateTime;
} FILETIME;

typedef union
{
	struct {
		DWORD LowPart, HighPart;
	} u;
	ULONGLONG QuadPart;
} ULARGE_INTEGER;

typedef struct
{
	int unused;
} CRITICAL_SECTION;

#define HEAP_ZERO_MEMORY 8u
HANDLE GetProcessHeap(void);
void *HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size);
void *HeapReAlloc(HANDLE heap, DWORD flags, void *block, SIZE_T size);
BOOL HeapFree(HANDLE heap, DWORD flags, void *block);
void InitializeCriticalSection(CRITICAL_SECTION *section);
void DeleteCriticalSection(CRITICAL_SECTION *section);
void EnterCriticalSection(CRITICAL_SECTION *section);
void LeaveCriticalSection(CRITICAL_SECTION *section);
LONG InterlockedIncrement(LONG volatile *value);
LONG InterlockedDecrement(LONG volatile *value);
void GetSystemTimeAsFileTime(FILETIME *time);
int lstrlenW(const WCHAR *text);
WCHAR *lstrcpyW(WCHAR *to, const WCHAR *from);
WCHAR *lstrcpynW(WCHAR *to, const WCHAR *from, int count);
/* Given a unit rather than a string (its high word zero), the unit in upper case. */
WCHAR *CharUpperW(WCHAR *text);

/* The bytes under a compound file: here a growing block of memory. */
typedef struct ILockBytes ILockBytes;
HRESULT ILockBytes_ReadAt(ILockBytes *bytes, ULARGE_INTEGER offset, void *buffer, ULONG count, ULONG *done);
HRESULT ILockBytes_WriteAt(ILockBytes *bytes, ULARGE_INTEGER offset, const void *buffer, ULONG count, ULONG *done);
HRESULT ILockBytes_SetSize(ILockBytes *bytes, ULARGE_INTEGER size);
HRESULT ILockBytes_Flush(ILockBytes *bytes);
ULONG ILockBytes_AddRef(ILockBytes *bytes);
ULONG ILockBytes_Release(ILockBytes *bytes);

/* Declared by docfile.h for the rest of the component; never called here. */
typedef struct IStorage IStorage;
typedef struct IPropertySetStorage IPropertySetStorage;

#endif
