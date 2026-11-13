// Direct2D, DirectWrite and Windows Animation are libraries of the system directory, so a program that asks for them there alone finds
// them, as the .NET runtime loads d2d1.dll for paint.net and its setup. The exit code names the first check that failed.
#include <windows.h>

static int check(const wchar_t *name, const char *function, DWORD flags)
{
	HMODULE library = LoadLibraryExW(name, NULL, flags);
	if (!library || !GetProcAddress(library, function))
		return 0;
	// The path Windows reports is the system directory's.
	wchar_t path[MAX_PATH], system[MAX_PATH];
	const UINT length = GetSystemDirectoryW(system, MAX_PATH);
	if (!GetModuleFileNameW(library, path, MAX_PATH) || CompareStringOrdinal(path, length, system, length, TRUE) != CSTR_EQUAL)
		return 0;
	FreeLibrary(library);
	return 1;
}

int raven_entry(void)
{
	if (!check(L"d2d1.dll", "D2D1CreateFactory", LOAD_LIBRARY_SEARCH_SYSTEM32))
		return 1;
	if (!check(L"d2d1", "D2D1CreateFactory", LOAD_LIBRARY_SEARCH_SYSTEM32))
		return 2;
	if (!check(L"dwrite.dll", "DWriteCreateFactory", LOAD_LIBRARY_SEARCH_SYSTEM32))
		return 3;
	if (!check(L"uianimation.dll", "DllGetClassObject", LOAD_LIBRARY_SEARCH_SYSTEM32))
		return 4;
	// The default directories, the program's own folder among them, as the .NET runtime asks after it found none beside the assembly.
	if (!check(L"d2d1.dll", "D2D1CreateFactory", LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
		return 5;
	if (!check(L"d2d1", "D2D1CreateFactory", LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
		return 6;
	if (!check(L"d2d1.dll", "D2D1CreateFactory", 0))
		return 7;
	return 0;
}
