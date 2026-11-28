#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* A prefix where a 32-bit setup's product was recorded under Wow6432Node, as Raven's installer engine once did, has it in the 64-bit
   view from the next program on, where Windows Installer keeps it: the first run writes the old record and starts a second, which looks.
   A record already in the 64-bit view stays as it was. */

static const WCHAR stale[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Installer\\UserData\\S-1-5-18\\Products\\"
	L"0123456789ABCDEF0123456789ABCDEF\\InstallProperties";
static const WCHAR kept[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Installer\\UserData\\S-1-5-18\\Products\\"
	L"FEDCBA9876543210FEDCBA9876543210\\InstallProperties";

static BOOL write_text(REGSAM view, const WCHAR *path, const WCHAR *name, const WCHAR *text)
{
	HKEY key;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_WRITE | view, NULL, &key, NULL))
		return FALSE;
	const LSTATUS status = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)text, (DWORD)(lstrlenW(text) + 1) * sizeof(WCHAR));
	RegCloseKey(key);
	return status == ERROR_SUCCESS;
}

static BOOL reads(REGSAM view, const WCHAR *path, const WCHAR *name, const WCHAR *expected)
{
	WCHAR text[64];
	DWORD size = sizeof text;
	return !RegGetValueW(HKEY_LOCAL_MACHINE, path, name, RRF_RT_REG_SZ | (view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY : RRF_SUBKEY_WOW6464KEY),
			NULL, text, &size) && !lstrcmpW(text, expected);
}

int raven_entry(void)
{
	const WCHAR *line = GetCommandLineW();
	BOOL second = FALSE;
	for (const WCHAR *at = line; *at; ++at)
		if (at[0] == L' ' && at[1] == L'/' && at[2] == L'l' && at[3] == L'o' && at[4] == L'o' && at[5] == L'k' && !at[6])
			second = TRUE;
	if (second) {
		if (!reads(KEY_WOW64_64KEY, stale, L"DisplayVersion", L"10.0.40219"))
			return 20;
		HKEY key;
		if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, stale, 0, KEY_READ | KEY_WOW64_32KEY, &key)) {
			RegCloseKey(key);
			return 21;
		}
		if (!reads(KEY_WOW64_64KEY, kept, L"DisplayVersion", L"14.34.31938"))
			return 22;
		return 0;
	}
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Installer\\UserData\\S-1-5-18\\Products\\"
		L"0123456789ABCDEF0123456789ABCDEF");
	/* The old record of one product, and one of another in both views, the 64-bit one the newer. */
	if (!write_text(KEY_WOW64_32KEY, stale, L"DisplayVersion", L"10.0.40219") || !write_text(KEY_WOW64_32KEY, kept, L"DisplayVersion", L"14.0.0") ||
		!write_text(KEY_WOW64_64KEY, kept, L"DisplayVersion", L"14.34.31938"))
		return 1;
	if (reads(KEY_WOW64_64KEY, stale, L"DisplayVersion", L"10.0.40219"))
		return 2;
	WCHAR command[MAX_PATH + 16];
	const DWORD length = GetModuleFileNameW(NULL, command + 1, MAX_PATH);
	command[0] = L'"';
	lstrcpyW(command + 1 + length, L"\" /look");
	STARTUPINFOW startup = {sizeof startup};
	PROCESS_INFORMATION process;
	if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process))
		return 3;
	WaitForSingleObject(process.hProcess, 60000);
	DWORD code = 4;
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return (int)code;
}
