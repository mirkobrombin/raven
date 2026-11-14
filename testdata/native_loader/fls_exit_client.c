// A thread that ends hands each fiber-local value it still holds to the destructor its index was allocated with, whether it returns or
// calls ExitThread, and the destructor runs on that thread. The .NET runtime learns that a thread has died this way and drops it from
// its list of threads; without it the garbage collector later reads the dead thread's allocation state. A thread killed from outside
// runs no destructor. The exit code names the first check that failed.
#include <windows.h>

static DWORD slot;
static volatile LONG calls;
static volatile UINT_PTR last_value;
static volatile DWORD last_thread;
static volatile UINT_PTR seen_inside;

static void WINAPI destructor(void *value)
{
	last_value = (UINT_PTR)value;
	last_thread = GetCurrentThreadId();
	// The value is already cleared when its destructor runs.
	seen_inside = (UINT_PTR)FlsGetValue(slot);
	InterlockedIncrement(&calls);
}

static volatile DWORD worker_thread;

static DWORD WINAPI returning(void *argument)
{
	worker_thread = GetCurrentThreadId();
	FlsSetValue(slot, (void *)0x1234);
	return 0;
}

static DWORD WINAPI exiting(void *argument)
{
	worker_thread = GetCurrentThreadId();
	FlsSetValue(slot, (void *)0x5678);
	ExitThread(7);
	return 0;
}

static DWORD WINAPI empty(void *argument)
{
	return 0;
}

static DWORD WINAPI waiting(void *argument)
{
	FlsSetValue(slot, (void *)0x9abc);
	Sleep(INFINITE);
	return 0;
}

static BOOL run(LPTHREAD_START_ROUTINE start)
{
	HANDLE thread = CreateThread(NULL, 0, start, NULL, 0, NULL);
	if (!thread)
		return FALSE;
	const BOOL ended = WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0;
	CloseHandle(thread);
	return ended;
}

int raven_entry(void)
{
	slot = FlsAlloc(destructor);
	if (slot == FLS_OUT_OF_INDEXES)
		return 1;
	if (!run(returning))
		return 2;
	if (calls != 1 || last_value != 0x1234 || last_thread != worker_thread)
		return 3;
	if (seen_inside)
		return 4;
	if (!run(exiting))
		return 5;
	if (calls != 2 || last_value != 0x5678 || last_thread != worker_thread)
		return 6;
	// No value, no destructor.
	if (!run(empty) || calls != 2)
		return 7;
	HANDLE killed = CreateThread(NULL, 0, waiting, NULL, 0, NULL);
	Sleep(100);
	if (!killed || !TerminateThread(killed, 9) || WaitForSingleObject(killed, 5000))
		return 8;
	CloseHandle(killed);
	if (calls != 2)
		return 9;
	return 0;
}
