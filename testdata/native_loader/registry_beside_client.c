// Registry calls go on beside the other calls of the process: two threads each create their own key, write a value and read it back
// many times, and read the other's key too, without a value seen torn or lost. A query for a value that is not there still answers
// ERROR_FILE_NOT_FOUND. The exit code names the first check that failed.
#include <windows.h>

static volatile LONG failed;

static DWORD WINAPI worker(void *parameter)
{
	const DWORD id = (DWORD)(ULONG_PTR)parameter;
	WCHAR path[64] = L"Software\\RavenBeside\\Thread0";
	path[27] = (WCHAR)(L'0' + id);
	HKEY key;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL))
		return 1;
	for (DWORD round = 0; round < 1500; ++round) {
		DWORD value = id * 100000 + round, read = 0, size = sizeof read, type = 0;
		if (RegSetValueExW(key, L"Round", 0, REG_DWORD, (const BYTE *)&value, sizeof value))
			return 2;
		if (RegQueryValueExW(key, L"Round", NULL, &type, (BYTE *)&read, &size) || type != REG_DWORD || read != value)
			return 3;
		HKEY other;
		WCHAR theirs[64];
		lstrcpyW(theirs, path);
		theirs[27] = (WCHAR)(L'0' + (id ^ 1));
		if (!RegOpenKeyExW(HKEY_CURRENT_USER, theirs, 0, KEY_READ, &other)) {
			size = sizeof read;
			LONG status = RegQueryValueExW(other, L"Round", NULL, &type, (BYTE *)&read, &size);
			if (!status && (read / 100000) != (id ^ 1))
				return 4;
			RegCloseKey(other);
		}
	}
	DWORD read = 0, size = sizeof read;
	if (RegQueryValueExW(key, L"Missing", NULL, NULL, (BYTE *)&read, &size) != ERROR_FILE_NOT_FOUND)
		return 5;
	RegCloseKey(key);
	return 0;
}

int raven_entry(void)
{
	HANDLE threads[2];
	for (DWORD id = 0; id < 2; ++id)
		if (!(threads[id] = CreateThread(NULL, 0, worker, (void *)(ULONG_PTR)id, 0, NULL)))
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
