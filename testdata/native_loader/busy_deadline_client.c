// A thread computes for seconds without calling the system while another one ends, as MusicBee's short-lived threads do while it
// starts: the computing thread runs until it is done instead of being stopped as if its time had run out. The exit code names the
// first check that failed.
#include <windows.h>

static DWORD WINAPI finish(void *unused)
{
	(void)unused;
	/* The other thread is computing by the time this one wakes and ends. */
	Sleep(500);
	return 0;
}

static unsigned long long ticks(void)
{
	unsigned int low, high;
	__asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
	return (unsigned long long)high << 32 | low;
}

int raven_entry(void)
{
	/* How many processor ticks make a second. */
	const DWORD began = GetTickCount();
	const unsigned long long first = ticks();
	Sleep(200);
	const unsigned elapsed = (unsigned)(ticks() - first), milliseconds = GetTickCount() - began;
	const unsigned long long per_second = (unsigned long long)(elapsed / milliseconds) * 1000;
	HANDLE worker = CreateThread(NULL, 0, finish, NULL, 0, NULL);
	if (!worker)
		return 1;
	/* Four seconds without a call to the system. */
	const unsigned long long end = ticks() + per_second * 4;
	while (ticks() < end)
		__builtin_ia32_pause();
	DWORD code = 1;
	if (WaitForSingleObject(worker, 10000) != WAIT_OBJECT_0 || !GetExitCodeThread(worker, &code) || code != 0)
		return 2;
	CloseHandle(worker);
	return 0;
}
