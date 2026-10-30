/* regsvr32.exe: REGSVR32 [/u] [/s] [/c] [/n] [/i[:line]] library..., calls the entries through which a library registers the classes it
   serves. The documented exit codes tell a bad command line (1), no OLE (2), a library that does not load (3), a missing entry (4) and a
   failed registration (5) apart. */

#include "tool.h"

typedef HRESULT(STDAPICALLTYPE *ServerEntry)(void);
typedef HRESULT(STDAPICALLTYPE *InstallEntry)(BOOL, const WCHAR *);
typedef HRESULT(STDAPICALLTYPE *OleStart)(void *);
typedef void(STDAPICALLTYPE *OleStop)(void);
typedef int(WINAPI *MessageBoxEntry)(HWND, const WCHAR *, const WCHAR *, UINT);

enum { exit_arguments = 1, exit_ole = 2, exit_load = 3, exit_entry = 4, exit_registration = 5 };

static BOOL silent, console;

/* Only a failure speaks when silent, on standard error, which a window-less setup can log. */
static void report(const WCHAR *first, const WCHAR *second, const WCHAR *third, BOOL failed)
{
	WCHAR text[1024];
	lstrcpynW(text, first, 512);
	lstrcatW(text, L" ");
	lstrcpynW(text + lstrlenW(text), second, 256);
	lstrcatW(text, third);
	if (silent || console) {
		if (failed || !silent) {
			write_to(failed ? error_output : output, text, length(text));
			write_to(failed ? error_output : output, L"\r\n", 2);
		}
		return;
	}
	HMODULE user = LoadLibraryW(L"user32.dll");
	MessageBoxEntry show = user ? (MessageBoxEntry)(void *)GetProcAddress(user, "MessageBoxW") : NULL;
	if (show)
		show(NULL, text, L"RegSvr32", failed ? MB_ICONERROR : MB_ICONINFORMATION);
}

static int call_server(HMODULE module, const WCHAR *library, const char *entry, const WCHAR *entry_text)
{
	ServerEntry call = (ServerEntry)(void *)GetProcAddress(module, entry);
	if (!call) {
		report(L"The entry point was not found in", library, L".", TRUE);
		return exit_entry;
	}
	if (FAILED(call())) {
		report(entry_text, library, L" failed.", TRUE);
		return exit_registration;
	}
	report(entry_text, library, L" succeeded.", FALSE);
	return 0;
}

static int call_install(HMODULE module, const WCHAR *library, BOOL install, const WCHAR *line)
{
	InstallEntry call = (InstallEntry)(void *)GetProcAddress(module, "DllInstall");
	if (!call) {
		report(L"The entry point DllInstall was not found in", library, L".", TRUE);
		return exit_entry;
	}
	if (FAILED(call(install, line))) {
		report(L"DllInstall in", library, L" failed.", TRUE);
		return exit_registration;
	}
	report(L"DllInstall in", library, L" succeeded.", FALSE);
	return 0;
}

int raven_tool_entry(void)
{
	start_tool();
	WCHAR *at = GetCommandLineW();
	BOOL quoted = FALSE;
	for (; *at && (quoted || !blank(*at)); ++at)
		if (*at == L'"')
			quoted = !quoted;
	BOOL unregister = FALSE, no_server = FALSE, install = FALSE;
	const WCHAR *install_line = L"";
	WCHAR *libraries[64];
	int count = 0;
	while (*at) {
		while (blank(*at))
			++at;
		if (!*at)
			break;
		WCHAR *word = at;
		quoted = FALSE;
		WCHAR *to = at;
		for (; *at && (quoted || !blank(*at)); ++at)
			if (*at == L'"')
				quoted = !quoted;
			else
				*to++ = *at;
		if (*at)
			++at;
		*to = 0;
		if ((*word == L'/' || *word == L'-') && word[1]) {
			switch (lowered(word[1])) {
			case L'u': unregister = TRUE; break;
			case L's': silent = TRUE; break;
			case L'c': console = TRUE; break;
			case L'n': no_server = TRUE; break;
			case L'i':
				install = TRUE;
				if (word[2] == L':')
					install_line = word + 3;
				break;
			default:
				report(L"Unknown switch", word, L".", TRUE);
				return exit_arguments;
			}
		} else if (count < 64) {
			libraries[count++] = word;
		}
	}
	if (!count || (no_server && !install)) {
		report(L"Usage: regsvr32 [/u] [/s] [/c] [/n] [/i[:line]]", L"library", L"", TRUE);
		return exit_arguments;
	}
	HMODULE ole = LoadLibraryW(L"ole32.dll");
	OleStart ole_start = ole ? (OleStart)(void *)GetProcAddress(ole, "OleInitialize") : NULL;
	OleStop ole_stop = ole ? (OleStop)(void *)GetProcAddress(ole, "OleUninitialize") : NULL;
	if (!ole_start || FAILED(ole_start(NULL))) {
		report(L"OLE could not start for", libraries[0], L".", TRUE);
		return exit_ole;
	}
	int result = 0;
	for (int index = 0; index < count && !result; ++index) {
		HMODULE module = LoadLibraryExW(libraries[index], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!module) {
			report(L"Could not load", libraries[index], L".", TRUE);
			result = exit_load;
			break;
		}
		/* Uninstalling runs in the reverse order of installing. */
		if (unregister) {
			if (install)
				result = call_install(module, libraries[index], FALSE, install_line);
			if (!result && !no_server)
				result = call_server(module, libraries[index], "DllUnregisterServer", L"DllUnregisterServer in");
		} else {
			if (!no_server)
				result = call_server(module, libraries[index], "DllRegisterServer", L"DllRegisterServer in");
			if (!result && install)
				result = call_install(module, libraries[index], TRUE, install_line);
		}
		FreeLibrary(module);
	}
	if (ole_stop)
		ole_stop();
	ExitProcess((UINT)result);
	return result;
}
