// Setting an event, and waiting for one that is already set, go on beside the other calls of the process: a thread that waits for it
// among other objects still wakes, threads that set events of their own leave each the state it set, and an event that resets on its
// own is taken once for each time it was set, however many threads look. The exit code names the first check that failed.
#include <windows.h>

static HANDLE event, sleeper, single;
static volatile LONG woke, taken, finished;

static DWORD WINAPI sleeping(void *parameter)
{
	(void)parameter;
	Sleep(3000);
	return 0;
}

// The thread handle keeps this wait in the runtime: the event alone would be waited for in the process's table of waits.
static DWORD WINAPI waiter(void *parameter)
{
	(void)parameter;
	HANDLE both[2] = {event, sleeper};
	if (WaitForMultipleObjects(2, both, FALSE, 5000) == WAIT_OBJECT_0)
		InterlockedExchange(&woke, 1);
	return 0;
}

static DWORD WINAPI toggler(void *own)
{
	for (int round = 0; round < 20000; ++round) {
		SetEvent((HANDLE)own);
		ResetEvent((HANDLE)own);
	}
	SetEvent((HANDLE)own);
	return 0;
}

static DWORD WINAPI taker(void *parameter)
{
	(void)parameter;
	while (!finished)
		if (WaitForSingleObject(single, 0) == WAIT_OBJECT_0)
			InterlockedIncrement(&taken);
	return 0;
}

int raven_entry(void)
{
	event = CreateEventW(NULL, TRUE, FALSE, NULL);
	sleeper = CreateThread(NULL, 0, sleeping, NULL, 0, NULL);
	if (!event || !sleeper)
		return 1;
	HANDLE waiting = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
	if (!waiting)
		return 2;
	Sleep(100);
	const DWORD began = GetTickCount();
	SetEvent(event);
	if (WaitForSingleObject(waiting, 2000) != WAIT_OBJECT_0 || !woke)
		return 3;
	if (GetTickCount() - began > 1000)
		return 4;
	ResetEvent(event);
	if (WaitForSingleObject(event, 0) != WAIT_TIMEOUT)
		return 5;

	HANDLE owns[4], togglers[4];
	for (int index = 0; index < 4; ++index) {
		if (!(owns[index] = CreateEventW(NULL, TRUE, FALSE, NULL)))
			return 6;
		if (!(togglers[index] = CreateThread(NULL, 0, toggler, owns[index], 0, NULL)))
			return 7;
	}
	if (WaitForMultipleObjects(4, togglers, TRUE, 20000) != WAIT_OBJECT_0)
		return 8;
	for (int index = 0; index < 4; ++index)
		if (WaitForSingleObject(owns[index], 0) != WAIT_OBJECT_0)
			return 9;
	// A handle that is not an event is refused as the runtime refuses it.
	if (SetEvent(sleeper) || GetLastError() != ERROR_INVALID_HANDLE)
		return 10;

	HANDLE takers[4];
	if (!(single = CreateEventW(NULL, FALSE, FALSE, NULL)))
		return 11;
	for (int index = 0; index < 4; ++index)
		if (!(takers[index] = CreateThread(NULL, 0, taker, NULL, 0, NULL)))
			return 12;
	// The setter gives its turn up while it looks: in a process whose threads take turns, the takers run only then.
	for (LONG round = 1; round <= 300; ++round) {
		SetEvent(single);
		const DWORD began = GetTickCount();
		while (taken < round) {
			if (GetTickCount() - began > 5000)
				return 13;
			SwitchToThread();
		}
		if (taken != round)
			return 14;
	}
	finished = 1;
	if (WaitForMultipleObjects(4, takers, TRUE, 5000) != WAIT_OBJECT_0 || taken != 300)
		return 15;
	WaitForSingleObject(sleeper, 5000);
	return 0;
}
