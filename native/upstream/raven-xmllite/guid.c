/* The interface identifiers the two objects answer to, and the entry point. */

#define INITGUID
#include "shared.h"

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void *reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(module);
	return TRUE;
}
