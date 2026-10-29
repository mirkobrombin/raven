#include <windows.h>

// CreateProcess finds a program named alone in the folders of PATH, after the program's folder, the current folder and the system
// folders, as Nim starts gcc.exe. The program copies itself into a folder it then adds to PATH, and starts the copy by its name; the
// copy (started with "child" on its command line) exits with 7. Returns 0, or the number of the check that failed.
static int contains(const WCHAR *text, const WCHAR *part)
{
	for (; *text; ++text) {
		int index = 0;
		while (part[index] && text[index] == part[index])
			++index;
		if (!part[index])
			return 1;
	}
	return 0;
}

int raven_entry(void)
{
	static WCHAR self[MAX_PATH], folder[MAX_PATH + 32], copy[MAX_PATH + 64], path[4096], line[] = L"raven-on-path.exe child";
	if (contains(GetCommandLineW(), L" child"))
		ExitProcess(7);
	if (!GetModuleFileNameW(NULL, self, MAX_PATH) || !GetTempPathW(MAX_PATH, folder))
		return 1;
	lstrcatW(folder, L"raven-path-search");
	CreateDirectoryW(folder, NULL);
	lstrcpyW(copy, folder);
	lstrcatW(copy, L"\\raven-on-path.exe");
	if (!CopyFileW(self, copy, FALSE))
		return 2;
	const DWORD used = GetEnvironmentVariableW(L"PATH", path, 4000);
	if (used >= 4000)
		return 3;
	if (used)
		lstrcatW(path, L";");
	lstrcatW(path, folder);
	if (!SetEnvironmentVariableW(L"PATH", path))
		return 4;
	STARTUPINFOW startup = {sizeof startup};
	PROCESS_INFORMATION child;
	if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child))
		return 5;
	DWORD code = 0;
	if (WaitForSingleObject(child.hProcess, 30000) != WAIT_OBJECT_0 || !GetExitCodeProcess(child.hProcess, &code) || code != 7)
		return 6;
	CloseHandle(child.hThread);
	CloseHandle(child.hProcess);
	DeleteFileW(copy);
	RemoveDirectoryW(folder);
	return 0;
}
