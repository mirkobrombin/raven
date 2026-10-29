#include <windows.h>
#include <intrin.h>

// ntdll's critical sections, which the guest RTL serves as it serves kernel32's: the PEB names a free one at FastPebLock, which code
// that reads the process parameters takes, and a program's own can be initialized, entered again by its owner, tried and deleted.
// Returns 0, or the number of the check that failed.
typedef LONG NTSTATUS;
__declspec(dllimport) NTSTATUS NTAPI RtlEnterCriticalSection(CRITICAL_SECTION *);
__declspec(dllimport) NTSTATUS NTAPI RtlLeaveCriticalSection(CRITICAL_SECTION *);
__declspec(dllimport) BOOLEAN NTAPI RtlTryEnterCriticalSection(CRITICAL_SECTION *);
__declspec(dllimport) NTSTATUS NTAPI RtlInitializeCriticalSection(CRITICAL_SECTION *);
__declspec(dllimport) NTSTATUS NTAPI RtlDeleteCriticalSection(CRITICAL_SECTION *);

int raven_entry(void)
{
	CRITICAL_SECTION *peb_lock = *(CRITICAL_SECTION **)(__readgsqword(0x60) + 0x38);
	if (!peb_lock || peb_lock->LockCount != -1 || peb_lock->OwningThread)
		return 1;
	if (RtlEnterCriticalSection(peb_lock) != 0 || peb_lock->OwningThread != (HANDLE)(ULONG_PTR)GetCurrentThreadId() ||
		RtlLeaveCriticalSection(peb_lock) != 0 || peb_lock->OwningThread)
		return 2;
	static CRITICAL_SECTION own;
	if (RtlInitializeCriticalSection(&own) != 0 || RtlEnterCriticalSection(&own) != 0 || RtlEnterCriticalSection(&own) != 0 ||
		own.RecursionCount != 2)
		return 3;
	if (!RtlTryEnterCriticalSection(&own) || own.RecursionCount != 3)
		return 4;
	for (int index = 0; index < 3; ++index)
		RtlLeaveCriticalSection(&own);
	if (own.OwningThread || own.RecursionCount || RtlDeleteCriticalSection(&own) != 0)
		return 5;
	return 0;
}
