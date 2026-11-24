#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HANDLE port;
static volatile LONG woke;

static DWORD WINAPI worker(void *parameter)
{
	(void)parameter;
	DWORD bytes = 0;
	ULONG_PTR key = 0;
	OVERLAPPED *overlapped = NULL;
	if (!GetQueuedCompletionStatus(port, &bytes, &key, &overlapped, INFINITE) || key != 42)
		return 1;
	InterlockedExchange(&woke, 1);
	return 0;
}

/* A thread that waits long for a completion port, as the pool threads of the .NET runtime do, is suspended and read the way its
 * garbage collector does: suspending it returns at once, it stays where it is while suspended, and the packet ends its wait once it
 * is resumed. */
int raven_entry(void)
{
	port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
	if (!port) return 1;
	HANDLE thread = CreateThread(0, 0, worker, 0, 0, 0);
	if (!thread) return 2;
	Sleep(100);
	for (unsigned round = 0; round < 20; ++round) {
		if (SuspendThread(thread) != 0) return 3;
#ifdef __x86_64__
		CONTEXT context;
		context.ContextFlags = CONTEXT_CONTROL;
		if (!GetThreadContext(thread, &context) || !context.Rip || !context.Rsp) return 4;
#endif
		if (ResumeThread(thread) != 1) return 5;
		Sleep(5);
	}
	if (SuspendThread(thread) != 0) return 6;
	if (!PostQueuedCompletionStatus(port, 0, 42, NULL)) return 7;
	Sleep(50);
	if (woke) return 8;
	if (ResumeThread(thread) != 1) return 9;
	if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) return 10;
	DWORD code = 1;
	if (!GetExitCodeThread(thread, &code) || code != 0 || !woke) return 11;
	CloseHandle(thread);
	CloseHandle(port);
	return 0;
}
