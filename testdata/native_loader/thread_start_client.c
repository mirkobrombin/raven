// A thread is made and waited for at once, three hundred times, as Notepad++ checks each file it remembers on a thread of its own: the
// new thread starts within a fraction of a millisecond. The thread that made it gives the runtime up at once to wait, often before the
// new thread's host thread is there; one that then waited for the runtime's next look at every thread started milliseconds late. The
// exit code is 1 when a thread could not be made or did not end, 2 when three or more started later than eight milliseconds.
#include <windows.h>

#define THREADS 300

static LARGE_INTEGER started;

static DWORD WINAPI start(void *parameter)
{
	(void)parameter;
	QueryPerformanceCounter(&started);
	return 0;
}

int raven_entry(void)
{
	LARGE_INTEGER frequency, made;
	QueryPerformanceFrequency(&frequency);
	int late = 0;
	for (int index = 0; index < THREADS; ++index) {
		started.QuadPart = 0;
		HANDLE thread = CreateThread(NULL, 0, start, NULL, 0, NULL);
		QueryPerformanceCounter(&made);
		if (!thread || WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0)
			return 1;
		CloseHandle(thread);
		if ((started.QuadPart - made.QuadPart) * 1000 > frequency.QuadPart * 8)
			++late;
	}
	return late >= 3 ? 2 : 0;
}
