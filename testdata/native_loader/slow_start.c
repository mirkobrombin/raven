// A library that takes its time to start: it is ready only when its entry point returns, as a packed DLL is once it has unpacked.
#include <windows.h>

static volatile LONG attached, ready;

__declspec(dllexport) int started(void)
{
	return ready * 10 + attached;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	(void)instance;
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		InterlockedIncrement(&attached);
		Sleep(300);
		ready = 1;
	}
	return TRUE;
}
