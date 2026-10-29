#include <windows.h>

// The ntdll forms of kernel32 functions that programs without kernel32 imports call: RtlUpcaseUnicodeChar, the performance
// counter, the vectored handlers, and RtlExitUserProcess, which ends the process with its code as ExitProcess does (the test expects
// 0). Returns the number of the check that failed.
typedef LONG NTSTATUS;
__declspec(dllimport) WCHAR NTAPI RtlUpcaseUnicodeChar(WCHAR);
__declspec(dllimport) BOOLEAN NTAPI RtlQueryPerformanceCounter(LARGE_INTEGER *);
__declspec(dllimport) BOOLEAN NTAPI RtlQueryPerformanceFrequency(LARGE_INTEGER *);
__declspec(dllimport) void *NTAPI RtlAddVectoredExceptionHandler(ULONG, PVECTORED_EXCEPTION_HANDLER);
__declspec(dllimport) ULONG NTAPI RtlRemoveVectoredExceptionHandler(void *);
__declspec(dllimport) void NTAPI RtlExitUserProcess(NTSTATUS);

static LONG CALLBACK handler(EXCEPTION_POINTERS *pointers)
{
	(void)pointers;
	return EXCEPTION_CONTINUE_SEARCH;
}

int raven_entry(void)
{
	if (RtlUpcaseUnicodeChar(L'a') != L'A' || RtlUpcaseUnicodeChar(L'\x00e9') != L'\x00c9' || RtlUpcaseUnicodeChar(L'1') != L'1')
		return 1;
	LARGE_INTEGER frequency, first, second;
	if (!RtlQueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 || !RtlQueryPerformanceCounter(&first))
		return 2;
	Sleep(2);
	if (!RtlQueryPerformanceCounter(&second) || second.QuadPart <= first.QuadPart)
		return 3;
	void *registered = RtlAddVectoredExceptionHandler(1, handler);
	if (!registered || !RtlRemoveVectoredExceptionHandler(registered))
		return 4;
	RtlExitUserProcess(0);
	return 5;
}
