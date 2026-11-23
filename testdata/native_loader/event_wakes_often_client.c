// The promise of event_wakes_client two thousand times over: a thread that waits for an event runs once SetEvent has woken it, also when
// it left its wait before the setter looked for the threads that waited, within a tenth of a second. Whether it has run when SetEvent
// returns is not promised: on Windows it had not after at least 99 sets of 2000 on four processors, and after up to 75 on one. The exit
// code is 0, or names the check that failed.
#include <windows.h>

#define ROUNDS 2000

static HANDLE go, back;
static volatile LONG waiting, woke;

static DWORD WINAPI worker(void *parameter)
{
	(void)parameter;
	for (LONG round = 1; round <= ROUNDS; ++round) {
		InterlockedExchange(&waiting, round);
		if (WaitForSingleObject(go, INFINITE) != WAIT_OBJECT_0)
			return 1;
		InterlockedExchange(&woke, round);
		SetEvent(back);
	}
	return 0;
}

int raven_entry(void)
{
	go = CreateEventW(NULL, FALSE, FALSE, NULL);
	back = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!go || !back)
		return 1;
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	if (!thread)
		return 2;
	for (LONG round = 1; round <= ROUNDS; ++round) {
		while (waiting != round)
			Sleep(1);
		// The main thread leaves the processor to the worker, which only has its wait to begin.
		Sleep(2);
		if (woke == round)
			return 3;
		SetEvent(go);
		if (WaitForSingleObject(back, 100) != WAIT_OBJECT_0 || woke != round)
			return 4;
	}
	if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
		return 5;
	CloseHandle(thread);
	CloseHandle(go);
	CloseHandle(back);
	return 0;
}
