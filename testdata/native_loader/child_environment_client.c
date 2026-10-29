#include <windows.h>

// CreateProcessW reads the environment block it is given only up to its end: here the block ends at the end of a committed page with
// nothing mapped after it, and the child sees the variable it holds. The child (started with "child" on its command line) exits with
// 7 when it does. Returns 0, or the number of the check that failed.
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
	static WCHAR value[32], path[MAX_PATH], line[MAX_PATH + 16];
	if (contains(GetCommandLineW(), L" child"))
		ExitProcess(GetEnvironmentVariableW(L"RAVEN_CHILD_VALUE", value, 32) == 8 && contains(value, L"page-end") ? 7 : 1);
	// A page alone: what was reserved after it is released again.
	char *area = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!area || !VirtualFree(area, 0, MEM_RELEASE) || VirtualAlloc(area, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != area)
		return 1;
	static const WCHAR block[] = L"RAVEN_CHILD_VALUE=page-end\0";
	WCHAR *at = (WCHAR *)(area + 4096 - sizeof block);
	for (size_t index = 0; index < sizeof block / sizeof *block; ++index)
		at[index] = block[index];
	const DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
	if (!length)
		return 2;
	line[0] = L'"';
	for (DWORD index = 0; index < length; ++index)
		line[1 + index] = path[index];
	const WCHAR tail[] = L"\" child";
	for (size_t index = 0; index < sizeof tail / sizeof *tail; ++index)
		line[1 + length + index] = tail[index];
	STARTUPINFOW startup = {sizeof startup};
	PROCESS_INFORMATION child;
	if (!CreateProcessW(path, line, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, at, NULL, &startup, &child))
		return 3;
	DWORD code = 0;
	if (WaitForSingleObject(child.hProcess, 30000) != WAIT_OBJECT_0 || !GetExitCodeProcess(child.hProcess, &code) || code != 7)
		return 4;
	CloseHandle(child.hThread);
	CloseHandle(child.hProcess);
	return 0;
}
