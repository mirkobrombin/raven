#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HANDLE ping, pong, mixed, ack, timer;

static DWORD WINAPI ponger(void *parameter)
{
	(void)parameter;
	for (unsigned round = 0; round < 2000; ++round)
		if (WaitForSingleObject(ping, 5000) != WAIT_OBJECT_0 || !SetEvent(pong))
			return 1;
	return 0;
}

static DWORD WINAPI mixed_waiter(void *parameter)
{
	(void)parameter;
	for (unsigned round = 0; round < 200; ++round) {
		HANDLE both[2] = {mixed, timer};
		if (WaitForMultipleObjects(2, both, FALSE, 5000) != WAIT_OBJECT_0 || !SetEvent(ack))
			return 1;
	}
	return 0;
}

static int joined(HANDLE thread)
{
	DWORD code = 1;
	return WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &code) && code == 0 && CloseHandle(thread);
}

/* SetEvent and ResetEvent as the guest makes them itself where the runtime lets it (native/nt/guest_events.h), against what Windows
 * answers: a set wakes another thread's wait, the state is what the last call left, a closed handle fails and a handle value used again
 * names the new event, and a wait the runtime looks at by itself still ends. */
int raven_entry(void)
{
	ping = CreateEventW(0, FALSE, FALSE, 0);
	pong = CreateEventW(0, FALSE, FALSE, 0);
	HANDLE thread = CreateThread(0, 0, ponger, 0, 0, 0);
	if (!ping || !pong || !thread) return 1;
	for (unsigned round = 0; round < 2000; ++round)
		if (!SetEvent(ping) || WaitForSingleObject(pong, 5000) != WAIT_OBJECT_0) return 2;
	if (!joined(thread)) return 3;

	HANDLE manual = CreateEventW(0, TRUE, FALSE, 0);
	for (unsigned round = 0; round < 100; ++round) {
		if (!SetEvent(manual) || WaitForSingleObject(manual, 0) != WAIT_OBJECT_0 || WaitForSingleObject(manual, 0) != WAIT_OBJECT_0) return 4;
		if (!ResetEvent(manual) || WaitForSingleObject(manual, 0) != WAIT_TIMEOUT) return 5;
	}
	const HANDLE closed = manual;
	if (!CloseHandle(manual)) return 6;
	SetLastError(0);
	if (SetEvent(closed) || GetLastError() != ERROR_INVALID_HANDLE) return 7;
	for (unsigned round = 0; round < 50; ++round) {
		HANDLE first = CreateEventW(0, TRUE, FALSE, 0);
		if (!first || !SetEvent(first) || !SetEvent(first) || !CloseHandle(first)) return 8;
		HANDLE second = CreateEventW(0, TRUE, FALSE, 0);
		if (!second || WaitForSingleObject(second, 0) != WAIT_TIMEOUT || !SetEvent(second)) return 9;
		if (WaitForSingleObject(second, 0) != WAIT_OBJECT_0 || !ResetEvent(second) || WaitForSingleObject(second, 0) != WAIT_TIMEOUT) return 10;
		if (!CloseHandle(second)) return 11;
	}

	timer = CreateWaitableTimerW(0, TRUE, 0);
	mixed = CreateEventW(0, FALSE, FALSE, 0);
	ack = CreateEventW(0, FALSE, FALSE, 0);
	if (!timer || !mixed || !ack || !SetEvent(mixed) || !ResetEvent(mixed)) return 12;
	thread = CreateThread(0, 0, mixed_waiter, 0, 0, 0);
	if (!thread) return 13;
	for (unsigned round = 0; round < 200; ++round)
		if (!SetEvent(mixed) || WaitForSingleObject(ack, 5000) != WAIT_OBJECT_0) return 14;
	if (!joined(thread)) return 15;

	return 0;
}
