#include <windows.h>

// The library of a private side-by-side assembly: it lives in the assembly's folder beside the program, not in the program's folder.
BOOL WINAPI DllMainCRTStartup(HINSTANCE instance, DWORD reason, void *reserved)
{
	(void)instance, (void)reason, (void)reserved;
	return TRUE;
}

__declspec(dllexport) int private_value(void) { return 42; }
