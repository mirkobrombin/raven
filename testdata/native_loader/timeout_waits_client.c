/* Every thread waits far longer than the run may last, the main one too, and last, so that the time limit is reached inside its wait: the
 * run ends there, with the main thread taken out of its wait and the others unwound, instead of waiting on. The event has a name, so another
 * process could still set it and the threads do not count as deadlocked. */
#include <windows.h>

static HANDLE never;

static DWORD WINAPI wait_long(void *parameter)
{
	(void)parameter;
	WaitForSingleObject(never, 60000);
	return 0;
}

int raven_entry(void)
{
	never = CreateEventW(NULL, TRUE, FALSE, L"Local\\RavenTimeoutWaits");
	for (int index = 0; index < 3; ++index)
		if (!CreateThread(NULL, 0, wait_long, NULL, 0, NULL))
			return 1;
	Sleep(300);
	Sleep(60000);
	return 2;
}
