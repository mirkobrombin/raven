// Taking and releasing a mutex goes on beside the other calls of the process: two threads take turns at a counter under one mutex
// without either seeing the other inside, a mutex taken twice is released twice, and one released by a thread that does not own it
// fails as Windows fails it. The exit code names the first check that failed.
#include <windows.h>

static HANDLE lock;
static volatile LONG inside, total;

static DWORD WINAPI partner(void *parameter)
{
	(void)parameter;
	for (int round = 0; round < 2000; ++round) {
		if (WaitForSingleObject(lock, 10000) != WAIT_OBJECT_0)
			return 1;
		if (InterlockedIncrement(&inside) != 1)
			return 2;
		++total;
		InterlockedDecrement(&inside);
		if (!ReleaseMutex(lock))
			return 3;
	}
	return 0;
}

int raven_entry(void)
{
	lock = CreateMutexW(NULL, FALSE, NULL);
	if (!lock)
		return 1;
	HANDLE thread = CreateThread(NULL, 0, partner, NULL, 0, NULL);
	if (!thread)
		return 2;
	for (int round = 0; round < 2000; ++round) {
		if (WaitForSingleObject(lock, 10000) != WAIT_OBJECT_0)
			return 3;
		if (InterlockedIncrement(&inside) != 1)
			return 4;
		++total;
		InterlockedDecrement(&inside);
		if (!ReleaseMutex(lock))
			return 5;
	}
	DWORD code = 1;
	if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &code) || code || total != 4000)
		return 6;
	// Taken twice by its owner, it is released twice; a third release fails with ERROR_NOT_OWNER.
	if (WaitForSingleObject(lock, 0) != WAIT_OBJECT_0 || WaitForSingleObject(lock, 0) != WAIT_OBJECT_0)
		return 7;
	if (!ReleaseMutex(lock) || !ReleaseMutex(lock) || ReleaseMutex(lock) || GetLastError() != ERROR_NOT_OWNER)
		return 8;
	return 0;
}
