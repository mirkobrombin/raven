// Memory taken and given back many times over, each block written and read back: with the guest's system calls filtered, a thread that
// waits in its stub for VirtualAlloc maps the memory itself, and the call is answered without stopping it. The exit code names the first
// check that failed.
#include <windows.h>

int raven_entry(void)
{
	for (int round = 0; round < 400; ++round) {
		volatile unsigned char *block = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!block)
			return 1;
		block[0] = (unsigned char)round;
		block[65535] = (unsigned char)(round + 1);
		if (block[0] != (unsigned char)round || block[65535] != (unsigned char)(round + 1))
			return 2;
		DWORD old = 0;
		if (!VirtualProtect((void *)block, 65536, PAGE_READONLY, &old) || old != PAGE_READWRITE)
			return 3;
		if (!VirtualFree((void *)block, 0, MEM_RELEASE))
			return 4;
	}
	return 0;
}
