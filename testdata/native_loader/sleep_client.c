// Sleeps for six seconds and returns 0: longer than a run may last when no time is given to it.

#include <windows.h>

DWORD raven_entry(void)
{
	Sleep(6000);
	return 0;
}
