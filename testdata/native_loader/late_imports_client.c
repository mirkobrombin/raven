/* A library loaded after start finds the directory services, the HTTP server, audio compression and color management libraries, and its
   imports bind to their functions, by number and by name, as they do to ntdll's keyed events and, in 64-bit processes, to the path function
   that allocates its result. The printing library publishes its default printer functions by their numbers too. */
#include <windows.h>

typedef void (*late_imports_function)(void **);
typedef HRESULT (WINAPI *canonicalize_function)(const WCHAR *, ULONG, WCHAR **);

static const struct {
	const WCHAR *library;
	const char *name;
	WORD ordinal;
} imports[] = {
	{L"activeds.dll", "ADsGetObject", 3}, {L"activeds.dll", "ADsGetLastError", 13}, {L"httpapi.dll", "HttpInitialize", 0},
	{L"msacm32.dll", "acmMetrics", 0}, {L"mscms.dll", "OpenColorProfileW", 0}, {L"ntdll.dll", "NtCreateKeyedEvent", 0},
};

int raven_entry(void)
{
	WCHAR path[MAX_PATH];
	void *addresses[ARRAYSIZE(imports) + 1];
	DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
	if (!length || length >= MAX_PATH)
		return 1;
	while (length && path[length - 1] != L'\\')
		--length;
	lstrcpyW(path + length, L"late-imports.dll");
	HMODULE library = LoadLibraryW(path);
	if (!library)
		return 2;
	late_imports_function late_imports = (late_imports_function)(void *)GetProcAddress(library, "late_imports");
	if (!late_imports)
		return 3;
	late_imports(addresses);
	for (int index = 0; index < (int)ARRAYSIZE(imports); ++index) {
		HMODULE system = GetModuleHandleW(imports[index].library);
		if (!system)
			return 10 + index;
		void *named = (void *)GetProcAddress(system, imports[index].name);
		if (!named || named != addresses[index])
			return 20 + index;
		if (imports[index].ordinal && (void *)GetProcAddress(system, MAKEINTRESOURCEA(imports[index].ordinal)) != named)
			return 30 + index;
	}
#ifdef _WIN64
	/* The path import, which the API set names, works: it gives the canonical form in memory freed with LocalFree. */
	WCHAR *canonical = NULL;
	if (((canonicalize_function)addresses[ARRAYSIZE(imports)])(L"C:\\a\\..\\b", 0, &canonical) || !canonical ||
		lstrcmpW(canonical, L"C:\\b") || LocalFree(canonical))
		return 38;
#endif
	HMODULE printing = LoadLibraryW(L"winspool.drv");
	if (!printing || (void *)GetProcAddress(printing, MAKEINTRESOURCEA(203)) != (void *)GetProcAddress(printing, "GetDefaultPrinterW") ||
		(void *)GetProcAddress(printing, MAKEINTRESOURCEA(201)) != (void *)GetProcAddress(printing, "GetDefaultPrinterA") ||
		!GetProcAddress(printing, "GetDefaultPrinterW"))
		return 39;
	/* A name the library does not publish is not found. */
	SetLastError(0);
	if (GetProcAddress(GetModuleHandleW(L"activeds.dll"), "RavenNoSuchFunction") || GetLastError() != ERROR_PROC_NOT_FOUND)
		return 40;
	return FreeLibrary(library) ? 0 : 41;
}
