#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static volatile LONG ran;
static HANDLE go;

static DWORD WINAPI counter(void *parameter)
{
	if (WaitForSingleObject(go, 5000) != WAIT_OBJECT_0)
		return 0;
	InterlockedIncrement(&ran);
	return (DWORD)(ULONG_PTR)parameter;
}

/* A thread suspended right after CreateThread returns, often before it first ran, keeps the context it starts from: reading it gives
 * an instruction and a stack pointer, and once resumed it runs its routine with its own environment block and ends with its code. It
 * waits for an event set only after it is resumed, so it cannot end before it is suspended (a thread that ended cannot be). */
int raven_entry(void)
{
	go = CreateEventW(0, FALSE, FALSE, 0);
	if (!go) return 10;
	for (ULONG_PTR round = 1; round <= 200; ++round) {
		HANDLE thread = CreateThread(0, 0, counter, (void *)round, 0, 0);
		if (!thread) return 1;
		if (SuspendThread(thread) == (DWORD)-1) return 2;
		CONTEXT context;
		context.ContextFlags = CONTEXT_CONTROL;
		if (!GetThreadContext(thread, &context)) return 3;
		if (!context.Rip) return 4;
		if (!context.Rsp) return 5;
		if (ResumeThread(thread) == (DWORD)-1) return 6;
		if (!SetEvent(go)) return 11;
		if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) return 7;
		DWORD code = 0;
		if (!GetExitCodeThread(thread, &code) || code != round) return 8;
		CloseHandle(thread);
	}
	return ran == 200 ? 0 : 9;
}
