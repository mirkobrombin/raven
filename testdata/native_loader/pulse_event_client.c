#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* PulseEvent releases the threads that wait on the event now, all of them for a manual-reset event and one for an auto-reset event,
   and leaves the event not signalled. The waiters here are threads of this process. */
static HANDLE event;
static volatile LONG waiting, released;

static DWORD WINAPI waiter(void *unused)
{
	(void)unused;
	InterlockedIncrement(&waiting);
	if (WaitForSingleObject(event, 5000) == WAIT_OBJECT_0)
		InterlockedIncrement(&released);
	return 0;
}

/* Starts `count` waiters, pulses once they wait, and gives how many the pulse released. */
static LONG pulse(BOOL manual, int count, HANDLE *threads)
{
	waiting = released = 0;
	event = CreateEventW(NULL, manual, FALSE, NULL);
	for (int index = 0; index < count; ++index)
		threads[index] = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
	for (int spins = 0; waiting < count && spins < 200; ++spins)
		Sleep(5);
	Sleep(100);
	if (!PulseEvent(event))
		return -1;
	Sleep(300);
	return released;
}

int raven_entry(void)
{
	HANDLE threads[3];
	if (pulse(TRUE, 3, threads) != 3)
		return 1;
	if (WaitForSingleObject(event, 0) != WAIT_TIMEOUT)
		return 2;
	if (WaitForMultipleObjects(3, threads, TRUE, 2000) != WAIT_OBJECT_0)
		return 3;

	if (pulse(FALSE, 2, threads) != 1)
		return 4;
	if (WaitForSingleObject(event, 0) != WAIT_TIMEOUT)
		return 5;
	/* The second pulse releases the waiter the first one left. */
	if (!PulseEvent(event) || WaitForMultipleObjects(2, threads, TRUE, 2000) != WAIT_OBJECT_0 || released != 2)
		return 6;

	/* With nobody waiting a pulse does nothing, and a thread that waits afterwards is not released. */
	HANDLE idle = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!PulseEvent(idle) || WaitForSingleObject(idle, 50) != WAIT_TIMEOUT)
		return 7;
	return 0;
}
