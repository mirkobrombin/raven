// Two threads load the same library at once: the second waits until the first has started it, as the loader lock makes it on
// Windows, and the library starts once; a thread that begins meanwhile is announced to the library only after it has started. MusicBee
// loads its packed audio library while other threads come and go. The exit code names the first check that failed.
#include <windows.h>

typedef int (*started_function)(void);

static DWORD WINAPI nothing(void *unused)
{
	(void)unused;
	return 0;
}

static DWORD WINAPI load(void *unused)
{
	(void)unused;
	return LoadLibraryW(L"slow-start.dll") ? 0 : 1;
}

int raven_entry(void)
{
	HANDLE first = CreateThread(NULL, 0, load, NULL, 0, NULL);
	if (!first)
		return 1;
	/* The other thread is inside the library's entry point by now. */
	Sleep(100);
	HANDLE passing = CreateThread(NULL, 0, nothing, NULL, 0, NULL);
	if (!passing)
		return 8;
	Sleep(50);
	HMODULE library = LoadLibraryW(L"slow-start.dll");
	if (!library)
		return 2;
	started_function started = (started_function)GetProcAddress(library, "started");
	if (!started)
		return 3;
	const int state = started();
	if (state != 11)
		return state == 1 ? 4 : 5;
	if (WaitForSingleObject(first, 5000) != WAIT_OBJECT_0)
		return 6;
	DWORD code = 1;
	if (!GetExitCodeThread(first, &code) || code != 0)
		return 7;
	CloseHandle(first);
	started_function early_threads = (started_function)GetProcAddress(library, "early_threads");
	if (!early_threads || WaitForSingleObject(passing, 5000) != WAIT_OBJECT_0)
		return 9;
	if (early_threads() != 0)
		return 10;
	CloseHandle(passing);
	return 0;
}
