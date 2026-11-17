// A library that called DisableThreadLibraryCalls is not told when a thread starts or ends; one that did not is told both. The exit
// code names the first check that failed.
#include <windows.h>

typedef int (*heard_function)(void);

static DWORD WINAPI nothing(void *parameter)
{
	(void)parameter;
	return 0;
}

int raven_entry(void)
{
	HMODULE quiet = LoadLibraryW(L"quiet-threads.dll"), loud = LoadLibraryW(L"loud-threads.dll");
	if (!quiet || !loud)
		return 1;
	heard_function quiet_heard = (heard_function)GetProcAddress(quiet, "threads_heard");
	heard_function loud_heard = (heard_function)GetProcAddress(loud, "threads_heard");
	if (!quiet_heard || !loud_heard)
		return 2;
	HANDLE thread = CreateThread(NULL, 0, nothing, NULL, 0, NULL);
	if (!thread || WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
		return 3;
	CloseHandle(thread);
	if (quiet_heard() != 0)
		return 4;
	if (loud_heard() != 2)
		return 5;
	// A handle that names no loaded module is refused.
	if (DisableThreadLibraryCalls((HMODULE)(ULONG_PTR)0x10000))
		return 6;
	return 0;
}
