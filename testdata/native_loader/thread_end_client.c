// An ended thread lives on while a handle names it and is gone once the last one is closed, as Steam's threads come and go for hours:
// each thread waited for is a kernel object of the runtime, and Steam ran out of descriptors after a while when ended threads were kept.
// The test runs this with few descriptors to spare (ulimit -n 512), so that three thousand threads kept after their end fail it. The exit
// code is 1 when a thread could not be made or did not end, 2 when an ended thread with a handle cannot be opened by its id or tells no
// exit code, 3 when one without handles still can be opened, 4 when its id comes back with another error than ERROR_INVALID_PARAMETER.
#include <windows.h>

#define THREADS 3000

static DWORD WINAPI quick(void *parameter)
{
	return (DWORD)(ULONG_PTR)parameter;
}

// Makes a thread, waits for its end, and closes it: the runtime is given the chance to forget an ended thread, as each new one is.
static int waited(DWORD code)
{
	HANDLE thread = CreateThread(NULL, 0, quick, (void *)(ULONG_PTR)code, 0, NULL);
	if (!thread || WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0)
		return 0;
	CloseHandle(thread);
	return 1;
}

int raven_entry(void)
{
	DWORD id = 0, code = 0;
	HANDLE thread = CreateThread(NULL, 0, quick, (void *)7, 0, &id);
	if (!thread || WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0)
		return 1;
	HANDLE again = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
	if (!again || !GetExitCodeThread(again, &code) || code != 7)
		return 2;
	CloseHandle(again);
	CloseHandle(thread);
	if (!waited(0))
		return 1;
	if ((again = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id))) {
		CloseHandle(again);
		return 3;
	}
	if (GetLastError() != ERROR_INVALID_PARAMETER)
		return 4;
	for (int index = 0; index < THREADS; ++index)
		if (!waited(0))
			return 1;
	return 0;
}
