// Releasing a semaphore goes on beside the other calls of the process: two threads hand a turn back and forth through two semaphores
// without a release getting lost, and a release past the maximum still fails as Windows fails it. The exit code names the first check
// that failed.
#include <windows.h>

static HANDLE mine, theirs;
static volatile LONG rounds;

static DWORD WINAPI partner(void *parameter)
{
	(void)parameter;
	for (int round = 0; round < 2000; ++round) {
		if (WaitForSingleObject(theirs, 10000) != WAIT_OBJECT_0)
			return 1;
		InterlockedIncrement(&rounds);
		ReleaseSemaphore(mine, 1, NULL);
	}
	return 0;
}

int raven_entry(void)
{
	mine = CreateSemaphoreW(NULL, 0, 1, NULL);
	theirs = CreateSemaphoreW(NULL, 0, 1, NULL);
	if (!mine || !theirs)
		return 1;
	HANDLE thread = CreateThread(NULL, 0, partner, NULL, 0, NULL);
	if (!thread)
		return 2;
	for (int round = 0; round < 2000; ++round) {
		ReleaseSemaphore(theirs, 1, NULL);
		if (WaitForSingleObject(mine, 10000) != WAIT_OBJECT_0)
			return 3;
	}
	DWORD code = 1;
	if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &code) || code || rounds != 2000)
		return 4;
	// The count is at its maximum of 1 after this release, so the next one fails with ERROR_TOO_MANY_POSTS.
	if (!ReleaseSemaphore(mine, 1, NULL) || ReleaseSemaphore(mine, 1, NULL) || GetLastError() != ERROR_TOO_MANY_POSTS)
		return 5;
	return 0;
}
