// An I/O completion port hands out the packets posted to it in order, reports a wait that runs out of time with WAIT_TIMEOUT and no
// OVERLAPPED, and, when its last handle is closed while a thread waits on it, ends that wait with ERROR_ABANDONED_WAIT_0 and no
// OVERLAPPED. A handle duplicated from it keeps it open: closing the other one does not end the wait. The exit code names the first
// check that failed.
#include <windows.h>

static HANDLE port;
static volatile LONG returned;
static DWORD waited_error;
static ULONG_PTR waited_key;
static OVERLAPPED *waited_overlapped;

static DWORD WINAPI waiter(void *parameter)
{
	(void)parameter;
	DWORD bytes = 0;
	waited_key = 1;
	waited_overlapped = (OVERLAPPED *)1;
	SetLastError(0);
	const BOOL got = GetQueuedCompletionStatus(port, &bytes, &waited_key, &waited_overlapped, INFINITE);
	waited_error = GetLastError();
	InterlockedExchange(&returned, 1);
	return got ? 1 : 0;
}

int raven_entry(void)
{
	OVERLAPPED first, second;
	DWORD bytes = 0;
	ULONG_PTR key = 0;
	OVERLAPPED *overlapped = NULL;
	if (!(port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0)))
		return 1;
	// Two packets come back in the order they were posted, with their byte counts and keys.
	if (!PostQueuedCompletionStatus(port, 11, 21, &first) || !PostQueuedCompletionStatus(port, 12, 22, &second))
		return 2;
	if (!GetQueuedCompletionStatus(port, &bytes, &key, &overlapped, 0) || bytes != 11 || key != 21 || overlapped != &first)
		return 3;
	if (!GetQueuedCompletionStatus(port, &bytes, &key, &overlapped, 0) || bytes != 12 || key != 22 || overlapped != &second)
		return 4;
	// An empty port: the wait runs out, with no OVERLAPPED.
	overlapped = &first;
	SetLastError(0);
	if (GetQueuedCompletionStatus(port, &bytes, &key, &overlapped, 10) || GetLastError() != WAIT_TIMEOUT || overlapped)
		return 5;
	// A thread waits on the port; a second handle to it is closed and the wait goes on.
	HANDLE copy = NULL;
	if (!DuplicateHandle(GetCurrentProcess(), port, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
		return 6;
	HANDLE thread = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
	if (!thread)
		return 7;
	Sleep(100);
	if (!CloseHandle(copy))
		return 8;
	Sleep(100);
	if (returned)
		return 9;
	// The last handle goes: the wait ends as abandoned.
	if (!CloseHandle(port))
		return 10;
	if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
		return 11;
	DWORD code = 2;
	if (!GetExitCodeThread(thread, &code) || code)
		return 12;
	if (waited_error != ERROR_ABANDONED_WAIT_0)
		return 13;
	if (waited_overlapped)
		return 14;
	CloseHandle(thread);
	return 0;
}
