// VirtualQuery goes on beside the other calls of the process: two threads each reserve a region, commit part of it, change its
// protection and free it again many times, and ask for its pages between the steps; each answer must match the step it follows. A
// buffer too small for the record still fails with ERROR_BAD_LENGTH. The exit code names the first check that failed.
#include <windows.h>

#define ROUNDS 800
#define SIZE (64 * 4096)

static int expect(void *address, void *base, DWORD state, DWORD protect, SIZE_T size)
{
	MEMORY_BASIC_INFORMATION info;
	if (VirtualQuery(address, &info, sizeof info) != sizeof info)
		return 0;
	return info.BaseAddress == address && info.AllocationBase == base && info.State == state &&
		(state != MEM_COMMIT || info.Protect == protect) && info.RegionSize == size;
}

static DWORD WINAPI worker(void *parameter)
{
	(void)parameter;
	for (DWORD round = 0; round < ROUNDS; ++round) {
		BYTE *base = VirtualAlloc(NULL, SIZE, MEM_RESERVE, PAGE_NOACCESS);
		if (!base)
			return 1;
		if (!expect(base, base, MEM_RESERVE, 0, SIZE))
			return 2;
		if (!VirtualAlloc(base, 4 * 4096, MEM_COMMIT, PAGE_READWRITE))
			return 3;
		base[0] = (BYTE)round;
		if (!expect(base, base, MEM_COMMIT, PAGE_READWRITE, 4 * 4096) || !expect(base + 4 * 4096, base, MEM_RESERVE, 0, SIZE - 4 * 4096))
			return 4;
		DWORD old = 0;
		if (!VirtualProtect(base, 4096, PAGE_READONLY, &old) || old != PAGE_READWRITE)
			return 5;
		if (!expect(base, base, MEM_COMMIT, PAGE_READONLY, 4096) || !expect(base + 4096, base, MEM_COMMIT, PAGE_READWRITE, 3 * 4096))
			return 6;
		if (!VirtualFree(base, 0, MEM_RELEASE))
			return 7;
		MEMORY_BASIC_INFORMATION info;
		if (VirtualQuery(base, &info, sizeof info) != sizeof info || info.State != MEM_FREE)
			return 8;
	}
	MEMORY_BASIC_INFORMATION info;
	SetLastError(0);
	if (VirtualQuery(&info, &info, sizeof info - 1) || GetLastError() != ERROR_BAD_LENGTH)
		return 9;
	return 0;
}

int raven_entry(void)
{
	HANDLE threads[2];
	for (DWORD id = 0; id < 2; ++id)
		if (!(threads[id] = CreateThread(NULL, 0, worker, NULL, 0, NULL)))
			return 10;
	if (WaitForMultipleObjects(2, threads, TRUE, 60000) != WAIT_OBJECT_0)
		return 11;
	for (DWORD id = 0; id < 2; ++id) {
		DWORD code = 1;
		if (!GetExitCodeThread(threads[id], &code) || code)
			return 20 + (int)id * 10 + (int)code;
	}
	return 0;
}
