#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int raven_entry(void)
{
	HRESULT (WINAPI *append)(WCHAR *, size_t, const WCHAR *) =
		(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "PathCchAppend");
	if (!append)
		return 1;
	WCHAR path[MAX_PATH] = L"C:\\root";
	if (append(path, ARRAYSIZE(path), L"part\\file.txt") || lstrcmpW(path, L"C:\\root\\part\\file.txt"))
		return 2;
	if (append(path, ARRAYSIZE(path), L"\\\\server\\share\\file.txt") || lstrcmpW(path, L"\\\\server\\share\\file.txt"))
		return 3;
	WCHAR small[5] = L"C:\\a";
	if (SUCCEEDED(append(small, ARRAYSIZE(small), L"file.txt")) || lstrcmpW(small, L"C:\\a"))
		return 4;
	if (append(NULL, ARRAYSIZE(path), L"file.txt") != E_INVALIDARG || append(path, 0, L"file.txt") != E_INVALIDARG ||
		append(path, ARRAYSIZE(path), NULL) != E_INVALIDARG)
		return 5;
	WCHAR long_part[MAX_PATH];
	for (size_t at = 0; at < ARRAYSIZE(long_part) - 1; ++at)
		long_part[at] = L'a';
	long_part[ARRAYSIZE(long_part) - 1] = 0;
	if (append(path, ARRAYSIZE(path), long_part) != HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE) ||
		lstrcmpW(path, L"\\\\server\\share\\file.txt"))
		return 6;

	HMODULE path_api = LoadLibraryW(L"api-ms-win-core-path-l1-1-0.dll");
	HRESULT (WINAPI *combine)(WCHAR *, size_t, const WCHAR *, const WCHAR *) =
		(void *)GetProcAddress(path_api, "PathCchCombine");
	HRESULT (WINAPI *remove)(WCHAR *, size_t) =
		(void *)GetProcAddress(path_api, "PathCchRemoveFileSpec");
	if (!combine || !remove)
		return 7 + !combine + 2 * !remove;
	if (combine(path, ARRAYSIZE(path), L"C:\\root\\part", L"\\media\\..\\file.txt") ||
		lstrcmpW(path, L"C:\\file.txt"))
		return 11;
	if (remove(path, ARRAYSIZE(path)) || lstrcmpW(path, L"C:\\") ||
		remove(path, ARRAYSIZE(path)) != S_FALSE || lstrcmpW(path, L"C:\\"))
		return 12;
	if (combine(path, ARRAYSIZE(path), NULL, NULL) || lstrcmpW(path, L"\\") ||
		SUCCEEDED(combine(small, ARRAYSIZE(small), L"C:\\root", L"file.txt")))
		return 13;
	if (combine(NULL, ARRAYSIZE(path), L"C:\\root", L"file.txt") != E_INVALIDARG ||
		remove(path, 0) != E_INVALIDARG)
		return 14;
	if (combine(path, ARRAYSIZE(path), L"C:\\root", long_part) != HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE))
		return 15;
	return 0;
}
