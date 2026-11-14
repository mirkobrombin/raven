// A program keeps a buffer in physical memory while it plays from it, as MusicBee's 32-bit sound library locks the buffers it mixes
// into before playback starts. The exit code names the first check that failed.
#include <windows.h>

int raven_entry(void)
{
	BYTE *buffer = VirtualAlloc(NULL, 65536, MEM_COMMIT, PAGE_READWRITE);
	if (!buffer)
		return 1;
	if (!VirtualLock(buffer, 65536))
		return 2;
	buffer[100] = 7;
	if (!VirtualUnlock(buffer, 65536) || buffer[100] != 7)
		return 3;
	VirtualFree(buffer, 0, MEM_RELEASE);
	return 0;
}
