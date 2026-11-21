#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Two threads hand a turn back and forth through two auto-reset events, as producer and consumer threads do. Each wait is ended by the
 * one that sets its event, so the runtime never needs to look at a waiting thread's condition to find that it can go on: with
 * RAVEN_TRACE_WAITS it says how many times it looked. The main thread waits for its event or for the other thread's end, as a program
 * that must not hang on a thread that died does, so a set of an event and a thread is ended the same way. */

#define ROUNDS 2000

static HANDLE ping, pong;
static volatile LONG rounds;

static DWORD WINAPI answer(void *parameter)
{
	(void)parameter;
	for (unsigned round = 0; round < ROUNDS; ++round) {
		if (WaitForSingleObject(ping, 10000) != WAIT_OBJECT_0)
			return 1;
		InterlockedIncrement(&rounds);
		if (!SetEvent(pong))
			return 2;
	}
	return 0;
}

int raven_entry(void)
{
	ping = CreateEventW(NULL, FALSE, FALSE, NULL);
	pong = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!ping || !pong)
		return 1;
	HANDLE thread = CreateThread(NULL, 0, answer, NULL, 0, NULL);
	if (!thread)
		return 2;
	for (unsigned round = 0; round < ROUNDS; ++round) {
		if (!SetEvent(ping))
			return 3;
		const HANDLE either[2] = {pong, thread};
		if (WaitForMultipleObjects(2, either, FALSE, 10000) != WAIT_OBJECT_0)
			return 4;
	}
	if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0)
		return 5;
	DWORD code = 1;
	if (!GetExitCodeThread(thread, &code) || code || rounds != ROUNDS)
		return 6;
	return 0;
}
