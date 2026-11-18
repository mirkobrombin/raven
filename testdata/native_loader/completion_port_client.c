// An I/O completion port hands out the packets posted to it in order, reports a wait that runs out of time with WAIT_TIMEOUT and no
// OVERLAPPED, and, when its last handle is closed while a thread waits on it, ends that wait with ERROR_ABANDONED_WAIT_0 and no
// OVERLAPPED. A handle duplicated from it keeps it open: closing the other one does not end the wait. A thread that waits for several
// packets alertably runs the APCs queued to it and comes back with WAIT_IO_COMPLETION. The exit code names the first
// check that failed.
#include <windows.h>

static HANDLE port;
static volatile LONG returned;
static DWORD waited_error;
static ULONG_PTR waited_key;
static OVERLAPPED *waited_overlapped;
static volatile LONG apc_ran;
static DWORD alerted_error;
static ULONG alerted_removed;

static void CALLBACK apc(ULONG_PTR parameter)
{
	InterlockedExchange(&apc_ran, (LONG)parameter);
}

static DWORD WINAPI alertable_waiter(void *parameter)
{
	(void)parameter;
	OVERLAPPED_ENTRY entries[2];
	SetLastError(0);
	const BOOL got = GetQueuedCompletionStatusEx(port, entries, 2, &alerted_removed, INFINITE, TRUE);
	alerted_error = GetLastError();
	return got ? 1 : 0;
}

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
	// An alertable wait for several packets ends for an APC, which runs on the waiting thread.
	HANDLE alerted = CreateThread(NULL, 0, alertable_waiter, NULL, 0, NULL);
	if (!alerted)
		return 15;
	Sleep(100);
	if (!QueueUserAPC(apc, alerted, 7))
		return 16;
	if (WaitForSingleObject(alerted, 5000) != WAIT_OBJECT_0)
		return 17;
	DWORD alerted_code = 2;
	if (!GetExitCodeThread(alerted, &alerted_code) || alerted_code || alerted_error != WAIT_IO_COMPLETION || apc_ran != 7)
		return 18;
	CloseHandle(alerted);
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
