#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Threads that call at once: GetCurrentDirectoryW, which the runtime answers in its turn, and VirtualAlloc, VirtualProtect, VirtualFree,
// SetEvent, CreateEventW and CloseHandle, which it answers beside the turn. What each costs is what RAVEN_CYCLES reports. Each thread's
// last error stays its own.

static volatile LONG ready;
static HANDLE start, signal;

static DWORD WINAPI calls(void *parameter)
{
	(void)parameter;
	InterlockedIncrement(&ready);
	if (WaitForSingleObject(start, 5000) != WAIT_OBJECT_0)
		return 10;
	for (unsigned i = 0; i < 400; ++i) {
		void *memory = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!memory)
			return 11;
		((volatile char *)memory)[i] = 1;
		DWORD previous;
		if (!VirtualProtect(memory, 4096, PAGE_READONLY, &previous) || previous != PAGE_READWRITE || ((volatile char *)memory)[i] != 1)
			return 17;
		if (!VirtualFree(memory, 0, MEM_RELEASE))
			return 12;
		SetLastError(0);
		if (VirtualFree(memory, 0, MEM_RELEASE) || GetLastError() != ERROR_INVALID_ADDRESS)
			return 18;
		SetLastError(0);
		if (VirtualProtect(memory, 4096, PAGE_READWRITE, &previous) || GetLastError() != ERROR_INVALID_ADDRESS)
			return 19;
		WCHAR directory[MAX_PATH];
		if (!GetCurrentDirectoryW(MAX_PATH, directory))
			return 20;
		if (!SetEvent(signal))
			return 13;
		SetLastError(1234);
		HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
		if (!event || GetLastError() != 0)
			return 14;
		if (!CloseHandle(event))
			return 15;
		SetLastError(0);
		if (CloseHandle((HANDLE)(ULONG_PTR)0x3ffffff0) || GetLastError() != ERROR_INVALID_HANDLE)
			return 16;
	}
	return 0;
}

int raven_entry(void)
{
	start = CreateEventW(NULL, TRUE, FALSE, NULL);
	signal = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!start || !signal)
		return 1;
	HANDLE threads[4];
	for (unsigned i = 0; i < 4; ++i) {
		threads[i] = CreateThread(NULL, 0, calls, NULL, 0, NULL);
		if (!threads[i])
			return 2;
	}
	for (unsigned i = 0; ready != 4 && i < 5000; ++i)
		Sleep(1);
	if (ready != 4 || !SetEvent(start))
		return 3;
	for (unsigned i = 0; i < 4; ++i) {
		DWORD code;
		if (WaitForSingleObject(threads[i], 10000) != WAIT_OBJECT_0 || !GetExitCodeThread(threads[i], &code))
			return 4;
		if (code)
			return code;
	}
	return 0;
}
