// A setter goes on as soon as it has set the event, while the thread the set woke runs on another processor: Windows does not wait for
// the woken thread before SetEvent returns, which on a machine of four processors had not run yet after at least 99 of 2000 sets. Here
// the woken thread works for five milliseconds each time; the median SetEvent of fifty must return within half a millisecond. On a
// single processor the woken thread goes first, as Windows lets it, and there is nothing to measure. The exit code is 0, 100 and the
// median in tenths of a millisecond (99 at most), or names the check that failed.
#include <windows.h>

#define ROUNDS 50

static HANDLE go, back;
static volatile LONG waiting, worked;
static LARGE_INTEGER frequency;

static LONGLONG now(void)
{
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return counter.QuadPart;
}

static DWORD WINAPI worker(void *parameter)
{
	(void)parameter;
	for (LONG round = 1; round <= ROUNDS; ++round) {
		InterlockedExchange(&waiting, round);
		if (WaitForSingleObject(go, INFINITE) != WAIT_OBJECT_0)
			return 1;
		const LONGLONG until = now() + frequency.QuadPart / 200;
		while (now() < until) {
		}
		InterlockedExchange(&worked, round);
		SetEvent(back);
	}
	return 0;
}

int raven_entry(void)
{
	DWORD_PTR process = 0, system = 0;
	if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system))
		return 1;
	if (!(process & (process - 1)))
		return 0;
	if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart < 10000)
		return 2;
	go = CreateEventW(NULL, FALSE, FALSE, NULL);
	back = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!go || !back)
		return 3;
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	if (!thread)
		return 4;
	LONGLONG took[ROUNDS];
	for (LONG round = 1; round <= ROUNDS; ++round) {
		while (waiting != round)
			Sleep(1);
		// The worker is asleep in its wait, as a thread a set hands work to is.
		Sleep(2);
		const LONGLONG before = now();
		SetEvent(go);
		took[round - 1] = now() - before;
		if (WaitForSingleObject(back, 5000) != WAIT_OBJECT_0 || worked != round)
			return 5;
	}
	if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
		return 6;
	CloseHandle(thread);
	CloseHandle(go);
	CloseHandle(back);
	for (int at = 1; at < ROUNDS; ++at)
		for (int other = at; other > 0 && took[other - 1] > took[other]; --other) {
			const LONGLONG swapped = took[other];
			took[other] = took[other - 1];
			took[other - 1] = swapped;
		}
	const LONGLONG tenths = took[ROUNDS / 2] * 10000 / frequency.QuadPart;
	if (tenths < 5)
		return 0;
	return 100 + (int)(tenths > 99 ? 99 : tenths);
}
