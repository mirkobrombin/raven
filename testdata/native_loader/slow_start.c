// A library that takes its time to start: it is ready only when its entry point returns, as a packed DLL is once it has unpacked.
// It counts the threads it hears about before then.
#include <windows.h>

static volatile LONG attached, ready, early;

__declspec(dllexport) int started(void)
{
	return ready * 10 + attached;
}

__declspec(dllexport) int early_threads(void)
{
	return early;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	(void)instance;
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		InterlockedIncrement(&attached);
		Sleep(300);
		ready = 1;
	} else if (reason == DLL_THREAD_ATTACH && !ready) {
		InterlockedIncrement(&early);
	}
	return TRUE;
}
