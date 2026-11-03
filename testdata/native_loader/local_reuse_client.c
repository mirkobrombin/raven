#include <windows.h>

// A block the program frees with HeapFree, as Windows allows for LocalAlloc memory, leaves nothing behind for the next block at its address.
int main(void)
{
	HANDLE heap = GetProcessHeap();
	for (int round = 0; round < 64; round++) {
		HLOCAL empty = LocalAlloc(LMEM_FIXED, 0);
		if (!empty)
			return 1;
		if (!HeapFree(heap, 0, empty))
			return 2;
		HLOCAL block = LocalAlloc(LMEM_FIXED, 32);
		if (!block)
			return 3;
		if (block == empty && LocalSize(block) < 32)
			return 4;
		if (LocalFree(block))
			return 5;
	}
	HLOCAL handle = LocalAlloc(LMEM_MOVEABLE, 16);
	void *data = LocalLock(handle);
	if (!handle || !data)
		return 6;
	LocalUnlock(handle);
	if (!HeapFree(heap, 0, data))
		return 7;
	for (int round = 0; round < 64; round++) {
		void *block = LocalAlloc(LMEM_FIXED, 16);
		if (!block)
			return 8;
		if (LocalFree(block))
			return 9;
	}
	return 0;
}
