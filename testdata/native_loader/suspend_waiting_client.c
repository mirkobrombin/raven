#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HANDLE port;
static volatile LONG woke, stop;

static DWORD WINAPI spinner(void *parameter)
{
	(void)parameter;
	volatile unsigned long long turns = 0;
	while (!stop)
		++turns;
	return 0;
}

static DWORD WINAPI worker(void *parameter)
{
	(void)parameter;
	DWORD bytes = 0;
	ULONG_PTR key = 0;
	OVERLAPPED *overlapped = NULL;
	for (;;) {
		if (GetQueuedCompletionStatus(port, &bytes, &key, &overlapped, 20)) {
			InterlockedExchange(&woke, 1);
			return key == 42 ? 0 : 1;
		}
		if (GetLastError() != WAIT_TIMEOUT)
			return 2;
	}
}

/* A thread that keeps waiting for a completion port, as the pool threads of the .NET runtime do, is suspended and read the way its
 * garbage collector does, while more threads than processors keep the machine busy: its call then often stops at its stub before the
 * runtime takes it up. Suspending it returns at once, it stays where it is while suspended, and a packet ends its wait once resumed. */
int raven_entry(void)
{
	SYSTEM_INFO system;
	GetSystemInfo(&system);
	const unsigned spinners = system.dwNumberOfProcessors * 2 > 32 ? 32 : system.dwNumberOfProcessors * 2;
	HANDLE busy[32];
	port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
	HANDLE thread = CreateThread(0, 0, worker, 0, 0, 0);
	if (!port || !thread) return 1;
	for (unsigned index = 0; index < spinners; ++index)
		if (!(busy[index] = CreateThread(0, 0, spinner, 0, 0, 0))) return 2;
	for (unsigned round = 0; round < 200; ++round) {
		if (SuspendThread(thread) != 0) return 3;
#ifdef __x86_64__
		CONTEXT context;
		context.ContextFlags = CONTEXT_CONTROL;
		if (!GetThreadContext(thread, &context) || !context.Rip || !context.Rsp) return 4;
#endif
		if (ResumeThread(thread) != 1) return 5;
		Sleep(1);
	}
	InterlockedExchange(&stop, 1);
	if (WaitForMultipleObjects(spinners, busy, TRUE, 5000) != WAIT_OBJECT_0) return 6;
	if (SuspendThread(thread) != 0) return 7;
	if (!PostQueuedCompletionStatus(port, 0, 42, NULL)) return 8;
	Sleep(50);
	if (woke) return 9;
	if (ResumeThread(thread) != 1) return 10;
	DWORD code = 1;
	if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &code) || code != 0 || !woke) return 11;
	return 0;
}
