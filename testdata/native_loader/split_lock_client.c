// Locked operations on a value that straddles two cache lines finish while another thread lives, as a .NET program reads a long field
// with Interlocked.Read. Linux may hold such an instruction for milliseconds and start it over when a signal comes meanwhile; Windows
// lets it run. The exit code names the first check that failed.
#include <windows.h>

static HANDLE quit;

static DWORD WINAPI idle(void *unused)
{
	(void)unused;
	WaitForSingleObject(quit, INFINITE);
	return 0;
}

int raven_entry(void)
{
	BYTE *page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!page)
		return 1;
	// Four bytes before the end of a cache line, so the value's other half lies in the next one.
	volatile LONGLONG *split = (volatile LONGLONG *)(page + 60);
	quit = CreateEventW(NULL, TRUE, FALSE, NULL);
	// A second thread makes the runtime share the processor out in slices.
	HANDLE other = quit ? CreateThread(NULL, 0, idle, NULL, 0, NULL) : NULL;
	if (!other)
		return 2;
	for (LONGLONG count = 0; count < 30; count++)
		if (InterlockedCompareExchange64(split, count + 1, count) != count)
			return 3;
	if (*split != 30)
		return 4;
	SetEvent(quit);
	if (WaitForSingleObject(other, 10000) != WAIT_OBJECT_0)
		return 5;
	return 0;
}
