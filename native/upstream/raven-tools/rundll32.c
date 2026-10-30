/* rundll32.exe: RUNDLL32 library,entry [arguments], calls an entry of a library, which installers use to run their code in a process of
   its own. The entry takes a window, the library's instance, the rest of the command line and the show state. */

#include "tool.h"

typedef void(CALLBACK *WideEntry)(HWND, HINSTANCE, LPWSTR, int);
typedef void(CALLBACK *NarrowEntry)(HWND, HINSTANCE, LPSTR, int);

/* Past a program or library name on the command line, whose quotes may hold spaces; stop names what else ends it. */
static WCHAR *past_name(WCHAR *at, WCHAR stop)
{
	BOOL quoted = FALSE;
	for (; *at && (quoted || (*at != L' ' && *at != L'\t' && *at != stop)); ++at)
		if (*at == L'"')
			quoted = !quoted;
	return at;
}

static WCHAR *past_blanks(WCHAR *at)
{
	while (*at == L' ' || *at == L'\t')
		++at;
	return at;
}

int raven_tool_entry(void)
{
	start_tool();
	WCHAR *line = past_blanks(past_name(GetCommandLineW(), 0));
	WCHAR *library = line;
	WCHAR *at = past_name(line, L',');
	if (at == line || !*at)
		ExitProcess(0);
	*at++ = 0;
	/* The library may be quoted and its entry may follow a comma, blanks or both. */
	at = past_blanks(at);
	if (*at == L',')
		at = past_blanks(at + 1);
	if (*library == L'"') {
		++library;
		for (WCHAR *end = library; *end; ++end)
			if (*end == L'"')
				*end = 0;
	}
	WCHAR *entry = at;
	while (*at && *at != L' ' && *at != L'\t')
		++at;
	if (*at)
		*at++ = 0;
	WCHAR *rest = past_blanks(at);
	HMODULE module = LoadLibraryW(library);
	if (!module)
		ExitProcess(GetLastError());
	char name[256], wide_name[258];
	int length = WideCharToMultiByte(CP_ACP, 0, entry, -1, name, sizeof name, NULL, NULL);
	if (length <= 0 || length > 255)
		ExitProcess(ERROR_INVALID_PARAMETER);
	memcpy(wide_name, name, (size_t)length - 1);
	wide_name[length - 1] = 'W';
	wide_name[length] = 0;
	/* The wide form of an entry comes first, as its name with a W; the name itself takes the command line in the ANSI code page. */
	WideEntry wide = (WideEntry)(void (*)(void))GetProcAddress(module, wide_name);
	if (wide) {
		wide(NULL, module, rest, SW_SHOWDEFAULT);
		ExitProcess(0);
	}
	NarrowEntry narrow = (NarrowEntry)(void (*)(void))GetProcAddress(module, name);
	if (!narrow)
		ExitProcess(ERROR_PROC_NOT_FOUND);
	static char text[32768];
	WideCharToMultiByte(CP_ACP, 0, rest, -1, text, sizeof text, NULL, NULL);
	narrow(NULL, module, text, SW_SHOWDEFAULT);
	ExitProcess(0);
	return 0;
}
