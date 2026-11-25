#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HANDLE flicker;
static void say(const char *text)
{
	DWORD written = 0;
	const char *end = text;
	while (*end) ++end;
	WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, (DWORD)(end - text), &written, 0);
}

static volatile LONG stop_flicker;

static DWORD WINAPI flickerer(void *parameter)
{
	(void)parameter;
	while (!stop_flicker)
		if (!SetEvent(flicker) || !ResetEvent(flicker))
			return 1;
	return 0;
}

/* A thread the runtime stops or ends in the middle of the sets and resets it makes itself (native/nt/guest_events.h) leaves the event
 * usable: waits the runtime looks at by itself hold the event back from the guest, which waits for no thread stopped there. */
int raven_entry(void)
{
	HANDLE timer = CreateWaitableTimerW(0, TRUE, 0);
	flicker = CreateEventW(0, TRUE, FALSE, 0);
	HANDLE thread = CreateThread(0, 0, flickerer, 0, 0, 0);
	if (!timer || !flicker || !thread) return 1;
	say("stops: start\n");
	for (unsigned round = 0; round < 300; ++round) {
		if (round % 50 == 0) say("stops: round\n");
		if (SuspendThread(thread) == (DWORD)-1) return 2;
		HANDLE both[2] = {flicker, timer};
		const DWORD looked = WaitForMultipleObjects(2, both, FALSE, 1);
		if (looked != WAIT_OBJECT_0 && looked != WAIT_TIMEOUT) return 3;
		if (ResumeThread(thread) == (DWORD)-1) return 4;
	}
	say("stops: suspend loop done\n");
	InterlockedExchange(&stop_flicker, 1);
	DWORD code = 1;
	if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &code) || code || !CloseHandle(thread)) return 5;
	say("stops: first thread joined\n");
	// Ended at a random point of its sets many times over, one of them lands in the middle of one.
	for (unsigned ending = 0; ending < 50; ++ending) {
		stop_flicker = 0;
		thread = CreateThread(0, 0, flickerer, 0, 0, 0);
		if (!thread) return 6;
		Sleep(2);
		if (ending % 10 == 0) say("stops: terminating\n");
		if (!TerminateThread(thread, 0) || WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || !CloseHandle(thread)) return 7;
		HANDLE both[2] = {flicker, timer};
		const DWORD looked = WaitForMultipleObjects(2, both, FALSE, 1);
		if (looked != WAIT_OBJECT_0 && looked != WAIT_TIMEOUT) return 8;
	}
	say("stops: terminated\n");
	if (!SetEvent(flicker) || WaitForSingleObject(flicker, 0) != WAIT_OBJECT_0) return 9;
	say("stops: done\n");
	return 0;
}
