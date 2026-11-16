#include <windows.h>

// A thread that ends holding a mutex abandons it: the next wait for it gets it with WAIT_ABANDONED, once, and from then on the mutex is
// an ordinary one. A pulse releases the thread that waits at that moment and leaves the event reset.

static HANDLE mutex, event;

static DWORD WINAPI holder(void *unused)
{
	(void)unused;
	return WaitForSingleObject(mutex, INFINITE) == WAIT_OBJECT_0 ? 0 : 1;
}

static DWORD WINAPI pulsed(void *unused)
{
	(void)unused;
	return WaitForSingleObject(event, 5000) == WAIT_OBJECT_0 ? 0 : 1;
}

static int ends_holding(void)
{
	HANDLE thread = CreateThread(0, 0, holder, 0, 0, 0);
	DWORD code = 1;
	if (!thread || WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &code) || code)
		return 0;
	CloseHandle(thread);
	return 1;
}

int raven_entry(void)
{
	mutex = CreateMutexA(0, FALSE, 0);
	event = CreateEventA(0, TRUE, FALSE, 0);
	if (!mutex || !event || !ends_holding())
		return 1;
	if (WaitForSingleObject(mutex, 1000) != WAIT_ABANDONED)
		return 2;
	if (WaitForSingleObject(mutex, 0) != WAIT_OBJECT_0 || !ReleaseMutex(mutex) || !ReleaseMutex(mutex) || ReleaseMutex(mutex))
		return 3;
	// Once taken, the abandonment is not reported again.
	if (WaitForSingleObject(mutex, 0) != WAIT_OBJECT_0 || !ReleaseMutex(mutex))
		return 4;
	if (!ends_holding())
		return 5;
	HANDLE both[2] = {event, mutex};
	if (WaitForMultipleObjects(2, both, FALSE, 1000) != WAIT_ABANDONED_0 + 1 || !ReleaseMutex(mutex))
		return 6;
	HANDLE thread = CreateThread(0, 0, pulsed, 0, 0, 0);
	Sleep(100);
	if (!PulseEvent(event))
		return 7;
	DWORD code = 1;
	if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &code) || code)
		return 8;
	if (WaitForSingleObject(event, 0) != WAIT_TIMEOUT)
		return 9;
	return 0;
}
