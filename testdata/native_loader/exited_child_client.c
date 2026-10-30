#include <windows.h>

// The basic information of a child that has ended: NtQueryInformationProcess answers its exit status, its id and this process as
// its parent once it is gone. The child (started with "child" on its command line) exits with 7. Returns 0, or the number of the
// check that failed.
typedef LONG NTSTATUS;
__declspec(dllimport) NTSTATUS NTAPI NtQueryInformationProcess(HANDLE, int, void *, ULONG, ULONG *);

int raven_entry(void)
{
	static WCHAR path[MAX_PATH], line[MAX_PATH + 16];
	const WCHAR *command = GetCommandLineW();
	for (; *command; ++command)
		if (command[0] == L' ' && command[1] == L'c' && command[2] == L'h')
			ExitProcess(7);
	const DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
	if (!length)
		return 1;
	line[0] = L'"';
	for (DWORD index = 0; index < length; ++index)
		line[1 + index] = path[index];
	const WCHAR tail[] = L"\" child";
	for (size_t index = 0; index < sizeof tail / sizeof *tail; ++index)
		line[1 + length + index] = tail[index];
	STARTUPINFOW startup = {sizeof startup};
	PROCESS_INFORMATION child;
	if (!CreateProcessW(path, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child))
		return 2;
	if (WaitForSingleObject(child.hProcess, 30000) != WAIT_OBJECT_0)
		return 3;
	// PROCESS_BASIC_INFORMATION: the exit status, the PEB, the affinity, the priority, the id and the parent's id.
	ULONG_PTR basic[6] = {0};
	ULONG size = 0;
	if (NtQueryInformationProcess(child.hProcess, 0, basic, sizeof basic, &size) != 0 || size != sizeof basic)
		return 4;
	if ((DWORD)basic[0] != 7 || basic[4] != child.dwProcessId || basic[5] != GetCurrentProcessId())
		return 5;
	CloseHandle(child.hThread);
	CloseHandle(child.hProcess);
	return 0;
}
