#include <windows.h>

// A library loaded by its path is found again by that path: GetModuleHandleW and GetModuleHandleExW (pinning it) answer the
// handle LoadLibraryW returned, and a path to another folder finds nothing. Returns 0, or the number of the check that failed.
int raven_entry(void)
{
	WCHAR path[MAX_PATH];
	if (!GetModuleFileNameW(NULL, path, MAX_PATH))
		return 1;
	// The fixture library is built beside the program.
	WCHAR *slash = path;
	for (WCHAR *at = path; *at; ++at)
		if (*at == L'\\')
			slash = at + 1;
	const WCHAR name[] = L"module-path-library.dll";
	for (unsigned index = 0; index < sizeof name / sizeof *name; ++index)
		slash[index] = name[index];
	HMODULE loaded = LoadLibraryW(path);
	if (!loaded)
		return 2;
	if (GetModuleHandleW(path) != loaded)
		return 3;
	HMODULE pinned = NULL;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, path, &pinned) || pinned != loaded)
		return 4;
	if (GetModuleHandleW(L"C:\\nowhere\\module-path-library.dll"))
		return 5;
	if (GetModuleHandleW(L"module-path-library.dll") != loaded)
		return 6;
	return 0;
}
