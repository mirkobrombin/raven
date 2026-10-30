#include <windows.h>

// A library whose own manifest (resource 2) depends on the private assembly raven.private, which declares private_target.dll.
BOOL WINAPI DllMainCRTStartup(HINSTANCE instance, DWORD reason, void *reserved)
{
	(void)instance, (void)reason, (void)reserved;
	return TRUE;
}

__declspec(dllimport) int private_value(void);
__declspec(dllexport) int middle_value(void) { return private_value() + 1; }
