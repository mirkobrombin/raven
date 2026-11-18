#include <stdarg.h>
#include <windows.h>
#include <shlwapi.h>

/* What the setups of older frameworks rely on: a heap made to run code runs the code written into it, a folder marked read-only still
   takes new files, the ANSI event log calls answer, the 64-bit .NET switch keeps the value it was given, a program whose path has
   spaces starts from a command line that does not quote it, a service's optional settings read back as they were set, a critical
   section shows its owner where code that tests it inline looks, a program of either width loads as a data file, wnsprintf stops at
   the size it is given, regedit /s imports registration entries in both of their formats and
   value names as long as the registry allows, refusing longer ones without harm, lodctr's library takes counter names, and
   a process of the services' session sees its window station is not visible. */

static int run_from_heap(void)
{
	/* mov eax, 42; ret */
	static const unsigned char code[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};
	HANDLE heap = HeapCreate(HEAP_CREATE_ENABLE_EXECUTE, 0, 0);
	if (!heap)
		return 1;
	unsigned char *block = HeapAlloc(heap, 0, sizeof code);
	if (!block)
		return 2;
	for (unsigned i = 0; i < sizeof code; ++i)
		block[i] = code[i];
	int (*answer)(void) = (int (*)(void))(void *)block;
	return answer() == 42 ? 0 : 3;
}

static int read_only_folder(void)
{
	WCHAR folder[MAX_PATH], file[MAX_PATH];
	if (!GetTempPathW(MAX_PATH, folder))
		return 10;
	lstrcatW(folder, L"raven-read-only");
	CreateDirectoryW(folder, NULL);
	if (!SetFileAttributesW(folder, FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM))
		return 11;
	lstrcpyW(file, folder);
	lstrcatW(file, L"\\desktop.ini");
	HANDLE handle = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		return 12;
	CloseHandle(handle);
	DeleteFileW(file);
	return RemoveDirectoryW(folder) ? 0 : 13;
}

static int event_log(void)
{
	const char *lines[] = {"from the legacy client"};
	HANDLE source = RegisterEventSourceA(NULL, "Raven");
	if (!source)
		return 20;
	if (!ReportEventA(source, EVENTLOG_INFORMATION_TYPE, 0, 1, NULL, 1, 0, lines, NULL))
		return 21;
	return DeregisterEventSource(source) ? 0 : 22;
}

static int complus_switch(void)
{
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	ULONG (WINAPI *get)(void) = (ULONG (WINAPI *)(void))(void *)GetProcAddress(kernel, "GetComPlusPackageInstallStatus");
	BOOL (WINAPI *set)(ULONG) = (BOOL (WINAPI *)(ULONG))(void *)GetProcAddress(kernel, "SetComPlusPackageInstallStatus");
	if (!get || !set)
		return 30;
	const ULONG before = get();
	if (set(2) || GetLastError() != ERROR_INVALID_PARAMETER)
		return 31;
	if (!set(0) || get() != 0 || !set(1) || get() != 1)
		return 32;
	return set(before) ? 0 : 33;
}

static int unquoted_path(void)
{
	WCHAR self[MAX_PATH], folder[MAX_PATH], copy[MAX_PATH], line[MAX_PATH + 16];
	if (!GetModuleFileNameW(NULL, self, MAX_PATH) || !GetTempPathW(MAX_PATH, folder))
		return 40;
	lstrcatW(folder, L"raven spaced folder");
	CreateDirectoryW(folder, NULL);
	lstrcpyW(copy, folder);
	lstrcatW(copy, L"\\legacy copy.exe");
	if (!CopyFileW(self, copy, FALSE))
		return 41;
	lstrcpyW(line, copy);
	lstrcatW(line, L" --child");
	STARTUPINFOW startup = {sizeof startup};
	PROCESS_INFORMATION started;
	if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &started))
		return 42;
	DWORD code = 0;
	WaitForSingleObject(started.hProcess, 30000);
	GetExitCodeProcess(started.hProcess, &code);
	CloseHandle(started.hThread);
	CloseHandle(started.hProcess);
	DeleteFileW(copy);
	RemoveDirectoryW(folder);
	return code == 77 ? 0 : 43;
}

static int service_settings(void)
{
	SC_HANDLE manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (!manager)
		return 50;
	SC_HANDLE service = CreateServiceW(manager, L"RavenLegacy", L"Raven legacy", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
		SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, L"C:\\Windows\\System32\\svchost.exe", NULL, NULL, NULL, NULL, NULL);
	if (!service)
		service = OpenServiceW(manager, L"RavenLegacy", SERVICE_ALL_ACCESS);
	if (!service)
		return 51;
	SERVICE_DESCRIPTIONW description = {L"Kept by the service manager"};
	if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description))
		return 52;
	DWORD needed = 0;
	if (QueryServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, NULL, 0, &needed) || GetLastError() != ERROR_INSUFFICIENT_BUFFER || !needed)
		return 53;
	BYTE buffer[512];
	SERVICE_DESCRIPTIONW *read = (SERVICE_DESCRIPTIONW *)buffer;
	if (needed > sizeof buffer || !QueryServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, buffer, sizeof buffer, &needed) ||
		!read->lpDescription || lstrcmpW(read->lpDescription, description.lpDescription))
		return 54;
	SERVICE_DELAYED_AUTO_START_INFO delayed = {TRUE};
	if (!QueryServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, (BYTE *)&delayed, sizeof delayed, &needed) || delayed.fDelayedAutostart)
		return 55;
	DeleteService(service);
	CloseServiceHandle(service);
	CloseServiceHandle(manager);
	return 0;
}

static int section_fields(void)
{
	CRITICAL_SECTION section;
	InitializeCriticalSection(&section);
	if (section.LockCount != -1 || section.OwningThread)
		return 60;
	EnterCriticalSection(&section);
	if (section.LockCount == -1 || (DWORD)(ULONG_PTR)section.OwningThread != GetCurrentThreadId() || section.RecursionCount != 1)
		return 61;
	LeaveCriticalSection(&section);
	if (section.LockCount != -1 || section.OwningThread || section.RecursionCount)
		return 62;
	DeleteCriticalSection(&section);
	return 0;
}

static int data_files(void)
{
	WCHAR self[MAX_PATH], other[MAX_PATH];
	if (!GetModuleFileNameW(NULL, self, MAX_PATH))
		return 70;
	HMODULE image = LoadLibraryExW(self, NULL, LOAD_LIBRARY_AS_DATAFILE);
	if (!image)
		return 71;
	FreeLibrary(image);
	/* Two files of the same name in two folders, the first gone before the second loads, as fusion stages assemblies. */
	static WCHAR folder[MAX_PATH], copy[MAX_PATH];
	for (int round = 0; round < 2; ++round) {
		GetTempPathW(MAX_PATH, folder);
		lstrcatW(folder, round ? L"raven-stage-2" : L"raven-stage-1");
		CreateDirectoryW(folder, NULL);
		lstrcpyW(copy, folder);
		lstrcatW(copy, L"\\staged.dll");
		if (!CopyFileW(self, copy, FALSE) || !(image = LoadLibraryExW(copy, NULL, LOAD_LIBRARY_AS_DATAFILE)))
			return 73 + round;
		FreeLibrary(image);
		DeleteFileW(copy);
		RemoveDirectoryW(folder);
	}
#ifdef _WIN64
	/* The 32-bit build of this program sits in the x86 folder beside this one's. */
	lstrcpyW(other, self);
	WCHAR *slash = other;
	for (WCHAR *at = other; *at; ++at)
		if (*at == L'\\')
			slash = at;
	*slash = 0;
	for (WCHAR *at = other; *at; ++at)
		if (*at == L'\\')
			slash = at;
	lstrcpyW(slash + 1, L"x86\\legacy.exe");
	if (GetFileAttributesW(other) != INVALID_FILE_ATTRIBUTES) {
		if (!(image = LoadLibraryExW(other, NULL, LOAD_LIBRARY_AS_DATAFILE)))
			return 72;
		FreeLibrary(image);
	}
#else
	(void)other;
#endif
	return 0;
}

static int format_list(WCHAR *out, int size, const WCHAR *pattern, ...)
{
	va_list list;
	va_start(list, pattern);
	const int written = wvnsprintfW(out, size, pattern, list);
	va_end(list);
	return written;
}

static int bounded_format(void)
{
	char narrow[8];
	WCHAR wide[32];
	if (wnsprintfA(narrow, 8, "%s-%d", "abcdef", 42) != 7 || lstrcmpA(narrow, "abcdef-"))
		return 80;
	if (wnsprintfW(wide, 32, L"%s=%04x", L"key", 255) != 8 || lstrcmpW(wide, L"key=00ff"))
		return 81;
	if (format_list(wide, 6, L"%d.%s", 1234, L"tail") != 5 || lstrcmpW(wide, L"1234."))
		return 82;
	return 0;
}

static int run_and_wait(WCHAR *line)
{
	STARTUPINFOW startup = {sizeof startup};
	PROCESS_INFORMATION started;
	if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &started))
		return -1;
	DWORD code = 1;
	WaitForSingleObject(started.hProcess, 30000);
	GetExitCodeProcess(started.hProcess, &code);
	CloseHandle(started.hThread);
	CloseHandle(started.hProcess);
	return (int)code;
}

static BOOL write_file(const WCHAR *path, const void *bytes, DWORD size)
{
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD written = 0;
	const BOOL done = file != INVALID_HANDLE_VALUE && WriteFile(file, bytes, size, &written, NULL) && written == size;
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);
	return done;
}

static int registration_entries(void)
{
	static WCHAR folder[MAX_PATH], unicode[MAX_PATH], ansi[MAX_PATH], removal[MAX_PATH], line[3 * MAX_PATH];
	if (!GetTempPathW(MAX_PATH, folder))
		return 90;
	lstrcpyW(unicode, folder);
	lstrcatW(unicode, L"raven-unicode.reg");
	lstrcpyW(ansi, folder);
	lstrcatW(ansi, L"raven ansi.reg");
	lstrcpyW(removal, folder);
	lstrcatW(removal, L"raven-removal.reg");
	static const WCHAR text[] = L"\xfeffWindows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\Software\\RavenRegedit]\r\n"
		L"@=\"default\"\r\n\"Quoted\"=\"a\\\"b\\\\c\"\r\n\"Count\"=dword:0000002a\r\n\"Path\"=hex(2):25,00,41,00,25,00,00,00\r\n"
		L"\"List\"=hex(7):61,00,00,00,62,00,00,00,\\\r\n  00,00\r\n\"Gone\"=\"x\"\r\n\"Gone\"=-\r\n";
	static const char narrow[] = "REGEDIT4\r\n\r\n[HKEY_CURRENT_USER\\Software\\RavenRegedit\\Old]\r\n\"Name\"=\"value\"\r\n";
	static const WCHAR remove_text[] = L"\xfeffWindows Registry Editor Version 5.00\r\n\r\n[-HKEY_CURRENT_USER\\Software\\RavenRegedit]\r\n";
	if (!write_file(unicode, text, sizeof text - sizeof(WCHAR)) || !write_file(ansi, narrow, sizeof narrow - 1) ||
		!write_file(removal, remove_text, sizeof remove_text - sizeof(WCHAR)))
		return 91;
	lstrcpyW(line, L"regedit.exe /s \"");
	lstrcatW(line, unicode);
	lstrcatW(line, L"\" \"");
	lstrcatW(line, ansi);
	lstrcatW(line, L"\"");
	if (run_and_wait(line))
		return 92;
	HKEY key;
	WCHAR value[64];
	DWORD size = sizeof value, type = 0, number = 0;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RavenRegedit", 0, KEY_READ, &key))
		return 93;
	if (RegQueryValueExW(key, NULL, NULL, &type, (BYTE *)value, &size) || type != REG_SZ || lstrcmpW(value, L"default"))
		return 94;
	size = sizeof value;
	if (RegQueryValueExW(key, L"Quoted", NULL, &type, (BYTE *)value, &size) || lstrcmpW(value, L"a\"b\\c"))
		return 95;
	size = sizeof number;
	if (RegQueryValueExW(key, L"Count", NULL, &type, (BYTE *)&number, &size) || type != REG_DWORD || number != 42)
		return 96;
	size = sizeof value;
	if (RegQueryValueExW(key, L"Path", NULL, &type, (BYTE *)value, &size) || type != REG_EXPAND_SZ || lstrcmpW(value, L"%A%"))
		return 97;
	size = sizeof value;
	if (RegQueryValueExW(key, L"List", NULL, &type, (BYTE *)value, &size) || type != REG_MULTI_SZ || size != 10 || value[0] != L'a' || value[2] != L'b')
		return 98;
	size = sizeof value;
	if (RegQueryValueExW(key, L"Gone", NULL, &type, (BYTE *)value, &size) != ERROR_FILE_NOT_FOUND)
		return 99;
	RegCloseKey(key);
	size = sizeof value;
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\RavenRegedit\\Old", L"Name", RRF_RT_REG_SZ, NULL, value, &size) || lstrcmpW(value, L"value"))
		return 100;
	lstrcpyW(line, L"regedit /s \"");
	lstrcatW(line, removal);
	lstrcatW(line, L"\"");
	if (run_and_wait(line) || !RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RavenRegedit", 0, KEY_READ, &key))
		return 101;
	DeleteFileW(unicode);
	DeleteFileW(ansi);
	DeleteFileW(removal);
	return 0;
}

/* The names in a file of registration entries are the file's to choose: one of 4000 characters is stored whole, one past the registry's
   16383 is refused, and the import goes on to the next line either way. */
__attribute__((optimize("no-tree-loop-distribute-patterns"))) static int long_names(void)
{
	static WCHAR folder[MAX_PATH], path[MAX_PATH], line[2 * MAX_PATH], text[32768], name[4001], value[16];
	static const WCHAR head[] = L"\xfeffWindows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\Software\\RavenRegeditLong]\r\n";
	UINT length = 0;
	if (!GetTempPathW(MAX_PATH, folder))
		return 140;
	lstrcpyW(path, folder);
	lstrcatW(path, L"raven-long.reg");
	for (const WCHAR *at = head; *at; ++at)
		text[length++] = *at;
	/* "NNN...N"="long", then a name of 20000 characters, then "After"="ok". */
	const UINT names[2] = {4000, 20000};
	for (UINT which = 0; which < 2; ++which) {
		text[length++] = L'"';
		for (UINT index = 0; index < names[which]; ++index)
			text[length++] = L'N';
		for (const WCHAR *at = L"\"=\"long\"\r\n"; *at; ++at)
			text[length++] = *at;
	}
	for (const WCHAR *at = L"\"After\"=\"ok\"\r\n"; *at; ++at)
		text[length++] = *at;
	if (!write_file(path, text, length * sizeof(WCHAR)))
		return 141;
	lstrcpyW(line, L"regedit /s \"");
	lstrcatW(line, path);
	lstrcatW(line, L"\"");
	/* Whether the refused line counts as a failure is regedit's; a crash is not. */
	const int code = run_and_wait(line);
	if (code != 0 && code != 1)
		return 142;
	for (UINT index = 0; index < 4000; ++index)
		name[index] = L'N';
	name[4000] = 0;
	DWORD size = sizeof value;
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\RavenRegeditLong", name, RRF_RT_REG_SZ, NULL, value, &size) || lstrcmpW(value, L"long"))
		return 143;
	size = sizeof value;
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\RavenRegeditLong", L"After", RRF_RT_REG_SZ, NULL, value, &size) || lstrcmpW(value, L"ok"))
		return 144;
	RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\RavenRegeditLong");
	DeleteFileW(path);
	return 0;
}

static int counter_names(void)
{
	HMODULE library = LoadLibraryW(L"loadperf.dll");
	if (!library)
		return 110;
	DWORD (WINAPI *load)(LPWSTR, BOOL) = (DWORD (WINAPI *)(LPWSTR, BOOL))(void *)GetProcAddress(library, "LoadPerfCounterTextStringsW");
	DWORD (WINAPI *unload)(LPWSTR, BOOL) = (DWORD (WINAPI *)(LPWSTR, BOOL))(void *)GetProcAddress(library, "UnloadPerfCounterTextStringsW");
	WCHAR load_line[] = L"lodctr counters.ini", unload_line[] = L"unlodctr RavenService";
	if (!load || !unload || load(load_line, TRUE) || unload(unload_line, TRUE))
		return 111;
	/* The registration tools of the frameworks ask for their console window, which is none here. */
	return GetConsoleWindow() ? 112 : 0;
}

/* Installers take some shell functions by number: shell32 89 and 85, shlwapi 437. A number the system does not publish finds nothing. Under
 * WOW64 a 32-bit setup learns so from IsOS and IsWow64Process, and turns to the 64-bit program folder. */
static int system_numbers(void)
{
	HMODULE shell = LoadLibraryW(L"shell32.dll"), light = LoadLibraryW(L"shlwapi.dll");
	if (!shell || !light)
		return 130;
	FARPROC clone = GetProcAddress(shell, (LPCSTR)89), stream = GetProcAddress(shell, (LPCSTR)85);
	if (!clone || clone != GetProcAddress(shell, "SHCloneSpecialIDList") || !stream || stream != GetProcAddress(shell, "OpenRegStream"))
		return 131;
	if (GetProcAddress(shell, (LPCSTR)300) || GetProcAddress(light, (LPCSTR)300))
		return 132;
	BOOL (WINAPI *is_os)(DWORD) = (BOOL (WINAPI *)(DWORD))(void *)GetProcAddress(light, (LPCSTR)437);
	const BOOL wow64 = sizeof(void *) == 4;
	BOOL process_wow64 = !wow64;
	if (!is_os || is_os != (BOOL (WINAPI *)(DWORD))(void *)GetProcAddress(light, "IsOS") || !is_os(1) || is_os(29) || !is_os(30) != !wow64)
		return 133;
	if (!IsWow64Process(GetCurrentProcess(), &process_wow64) || !process_wow64 != !wow64)
		return 134;
	void *(WINAPI *clone_list)(HWND, int, BOOL) = (void *(WINAPI *)(HWND, int, BOOL))(void *)clone;
	BOOL (WINAPI *list_path)(const void *, WCHAR *) = (BOOL (WINAPI *)(const void *, WCHAR *))(void *)GetProcAddress(shell, "SHGetPathFromIDListW");
	WCHAR path[MAX_PATH] = L"";
	void *list = clone_list(NULL, 0x26, FALSE);
	if (!list || !list_path || !list_path(list, path) || lstrcmpiW(path, wow64 ? L"C:\\Program Files (x86)" : L"C:\\Program Files"))
		return 135;
	return clone_list(NULL, 0x7f, FALSE) ? 136 : 0;
}

int raven_entry(void)
{
	const WCHAR *line = GetCommandLineW();
	for (const WCHAR *at = line; *at; ++at) {
		if (at[0] == L'-' && at[1] == L'-' && at[2] == L'c' && at[3] == L'h')
			return 77;
		/* Started in the services' session, as the custom actions of an installer are: the window station is not visible. */
		if (at[0] == L'-' && at[1] == L'-' && at[2] == L's' && at[3] == L'e') {
			USEROBJECTFLAGS flags = {0};
			DWORD needed = 0;
			if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &flags, sizeof flags, &needed))
				return 120;
			return flags.dwFlags & WSF_VISIBLE ? 121 : 0;
		}
	}
	int result;
	if ((result = run_from_heap()) || (result = read_only_folder()) || (result = event_log()) || (result = complus_switch()) ||
		(result = unquoted_path()) || (result = service_settings()) || (result = section_fields()) || (result = data_files()) ||
		(result = bounded_format()) || (result = registration_entries()) || (result = long_names()) || (result = counter_names()) ||
		(result = system_numbers()))
		return result;
	return 0;
}
