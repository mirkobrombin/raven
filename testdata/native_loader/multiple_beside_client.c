// A wait for several objects goes on beside the other calls of the process: a worker answers each of two events through its own
// semaphore, waiting for either of the events, then for both at once, and a wait for an event and a mutex together takes the mutex.
// The exit code names the first check that failed.
#include <windows.h>

static HANDLE first, second, answered, both, lock;

static DWORD WINAPI worker(void *parameter)
{
	(void)parameter;
	HANDLE either[2] = {first, second};
	for (int round = 0; round < 2000; ++round) {
		DWORD woke = WaitForMultipleObjects(2, either, FALSE, 10000);
		if (woke != WAIT_OBJECT_0 + (round & 1))
			return 1;
		ReleaseSemaphore(answered, 1, NULL);
	}
	for (int round = 0; round < 500; ++round) {
		if (WaitForMultipleObjectsEx(2, either, TRUE, 10000, FALSE) != WAIT_OBJECT_0)
			return 2;
		ReleaseSemaphore(both, 1, NULL);
	}
	return 0;
}

int raven_entry(void)
{
	first = CreateEventW(NULL, FALSE, FALSE, NULL);
	second = CreateEventW(NULL, FALSE, FALSE, NULL);
	answered = CreateSemaphoreW(NULL, 0, 1, NULL);
	both = CreateSemaphoreW(NULL, 0, 1, NULL);
	lock = CreateMutexW(NULL, FALSE, NULL);
	if (!first || !second || !answered || !both || !lock)
		return 1;
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	if (!thread)
		return 2;
	for (int round = 0; round < 2000; ++round) {
		SetEvent(round & 1 ? second : first);
		if (WaitForSingleObject(answered, 10000) != WAIT_OBJECT_0)
			return 3;
	}
	for (int round = 0; round < 500; ++round) {
		SetEvent(first);
		SetEvent(second);
		if (WaitForSingleObject(both, 10000) != WAIT_OBJECT_0)
			return 4;
	}
	DWORD code = 1;
	if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &code) || code)
		return 5;
	// The mutex is free and the event is not set: waiting for either takes the mutex, index 1.
	HANDLE pair[2] = {first, lock};
	if (WaitForMultipleObjects(2, pair, FALSE, 0) != WAIT_OBJECT_0 + 1 || !ReleaseMutex(lock))
		return 6;
	if (WaitForMultipleObjects(2, pair, TRUE, 0) != WAIT_TIMEOUT)
		return 7;
	return 0;
}
