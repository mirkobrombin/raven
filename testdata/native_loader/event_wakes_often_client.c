// The promise of event_wakes_client two thousand times over: a thread that waits for an event has run when SetEvent returns, also when
// it left its wait before the setter looked for the threads that waited. The exit code is 100 and the number of sets it had not run
// after (99 at most), or names the check that failed.
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
	int missed = 0;
	for (LONG round = 1; round <= ROUNDS; ++round) {
		while (waiting != round)
			Sleep(1);
		// The main thread leaves the processor to the worker, which only has its wait to begin.
		Sleep(2);
		if (woke == round)
			return 3;
		SetEvent(go);
		if (woke != round && missed < 99)
			++missed;
		if (WaitForSingleObject(back, 5000) != WAIT_OBJECT_0)
			return 4;
	}
	if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
		return 5;
	CloseHandle(thread);
	CloseHandle(go);
	CloseHandle(back);
	return missed ? 100 + missed : 0;
}
