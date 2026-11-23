#include <windows.h>

int raven_entry(void)
{
	WCHAR mode[2];
	if (GetEnvironmentVariableW(L"IMPORT_PROBE_CASE", mode, ARRAYSIZE(mode)) != 1)
		return 1;
	HMODULE library = LoadLibraryW(mode[0] == L'x' ? L"outside-probe.dll" : L"import-probe.dll");
	if (mode[0] == L'x' || mode[0] == L'd') {
		if (library)
			return 2;
		const DWORD error = GetLastError();
		return error == ERROR_MOD_NOT_FOUND || error == ERROR_PROC_NOT_FOUND ? 0 : 7;
	}
	if (!library)
		return 3;
	HMODULE languages = GetModuleHandleW(L"bcp47langs.dll");
	if (!languages || GetProcAddress(languages, "GetApplicationLanguages") ||
		GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemMetadataPathForPackage") ||
		GetProcAddress(GetModuleHandleW(L"user32.dll"), MAKEINTRESOURCEA(2557)))
		return 4;
	unsigned (*probe)(unsigned) = (unsigned (*)(unsigned))(void *)GetProcAddress(library, "run_probe");
	if (!probe || mode[0] < L'0' || mode[0] > L'2')
		return 5;
	probe(mode[0] - L'0');
	return 6;
}
