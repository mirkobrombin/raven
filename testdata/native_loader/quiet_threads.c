// A library that counts the threads it hears about; built with QUIET it asks not to hear of them (DisableThreadLibraryCalls).
#include <windows.h>

static volatile LONG heard;

__declspec(dllexport) int threads_heard(void)
{
	return heard;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
#ifdef QUIET
		if (!DisableThreadLibraryCalls(instance))
			return FALSE;
#else
		(void)instance;
#endif
	} else if (reason == DLL_THREAD_ATTACH || reason == DLL_THREAD_DETACH) {
		InterlockedIncrement(&heard);
	}
	return TRUE;
}
