// A wait answered beside the turn after its thread stopped spinning goes on without the turn: a thread waits each round for an event
// another thread sets only after working longer than the waiter spins in its stub. The exit code names the first check that failed.
#include <windows.h>

#define ROUNDS 200

static HANDLE ping, pong;
static volatile LONG sink;

static DWORD WINAPI player(void *unused)
{
	(void)unused;
	for (DWORD round = 0; round < ROUNDS; ++round) {
		if (!SetEvent(ping))
			return 1;
		if (WaitForSingleObject(pong, 5000) != WAIT_OBJECT_0)
			return 2;
	}
	return 0;
}

static DWORD WINAPI answerer(void *unused)
{
	(void)unused;
	for (DWORD round = 0; round < ROUNDS; ++round) {
		if (WaitForSingleObject(ping, 5000) != WAIT_OBJECT_0)
			return 1;
		// Work without calls, far longer than a stub spins before it stops.
		for (LONG step = 0; step < 2000000; ++step)
			sink = sink + step;
		if (!SetEvent(pong))
			return 2;
	}
	return 0;
}

int raven_entry(void)
{
	ping = CreateEventA(NULL, FALSE, FALSE, NULL);
	pong = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (!ping || !pong)
		return 10;
	HANDLE threads[2] = {CreateThread(NULL, 0, player, NULL, 0, NULL), CreateThread(NULL, 0, answerer, NULL, 0, NULL)};
	if (!threads[0] || !threads[1])
		return 11;
	if (WaitForMultipleObjects(2, threads, TRUE, 60000) != WAIT_OBJECT_0)
		return 12;
	for (DWORD id = 0; id < 2; ++id) {
		DWORD code = 1;
		if (!GetExitCodeThread(threads[id], &code) || code)
			return 20 + (int)id * 10 + (int)code;
	}
	return 0;
}
