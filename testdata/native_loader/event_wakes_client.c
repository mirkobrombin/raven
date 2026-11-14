// A thread that waits for an event runs as soon as the event is set: on Windows it wakes at once on another processor while the setter
// goes on. MusicBee's welcome dialog counts on it: its scanning thread hands the result to the window and ends, and the dialog, closing,
// asks whether that thread is still alive. The exit code names the first check that failed.
#include <windows.h>

static HANDLE go;
static volatile LONG woke;

static DWORD WINAPI worker(void *parameter)
{
	(void)parameter;
	WaitForSingleObject(go, INFINITE);
	InterlockedExchange(&woke, 1);
	return 0;
}

int raven_entry(void)
{
	go = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!go)
		return 1;
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	if (!thread)
		return 2;
	// The worker reaches its wait.
	Sleep(100);
	if (woke)
		return 3;
	SetEvent(go);
	if (!woke)
		return 4;
	// With nothing more to do, the worker has ended too.
	DWORD code = 0;
	if (!GetExitCodeThread(thread, &code) || code == STILL_ACTIVE)
		return 5;
	if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
		return 6;
	CloseHandle(thread);
	CloseHandle(go);
	return 0;
}
