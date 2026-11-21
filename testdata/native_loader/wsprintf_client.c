#include <windows.h>
#include <stdarg.h>

/* Checks wsprintfA and wsprintfW and their relatives in the guest and returns 0, or the number of the first failing check. With "guest" on
   the command line it also checks that the formatting runs in the components that carry it, in the program's own process as Windows does
   it in user mode, and prints the nanoseconds a call took. */

int WINAPIV wnsprintfW(LPWSTR, int, LPCWSTR, ...);
int WINAPIV wnsprintfA(LPSTR, int, LPCSTR, ...);

static int same_narrow(const char *left, const char *right)
{
	while (*left && *left == *right) {
		++left;
		++right;
	}
	return *left == *right;
}

static int same_wide(const WCHAR *left, const WCHAR *right)
{
	while (*left && *left == *right) {
		++left;
		++right;
	}
	return *left == *right;
}

static int narrow_length(const char *text)
{
	volatile int count = 0;
	while (text[count])
		++count;
	return count;
}

static int listed(WCHAR *buffer, const WCHAR *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	const int length = wvsprintfW(buffer, format, arguments);
	va_end(arguments);
	return length;
}

/* Whether the export lies in the image of the named module. */
static int inside(const char *library, const char *name, const WCHAR *module)
{
	const BYTE *base = (const BYTE *)GetModuleHandleW(module);
	const BYTE *entry = (const BYTE *)GetProcAddress(LoadLibraryA(library), name);
	if (!base || !entry)
		return 0;
	const IMAGE_NT_HEADERS *headers = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
	return entry >= base && entry < base + headers->OptionalHeader.SizeOfImage;
}

unsigned int raven_entry(void)
{
	static char narrow[2048];
	static WCHAR wide[2048], long_text[2048];
	DWORD written;
	/* What a call costs, told first, for the runtime that answers it as for the component. */
	LARGE_INTEGER frequency, started, ended;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&started);
	for (int at = 0; at < 20000; ++at)
		wsprintfW(wide, L"%d %s", at, L"abc");
	QueryPerformanceCounter(&ended);
	int length = wsprintfA(narrow, "wsprintfW: %d ns per call\n", (int)((ended.QuadPart - started.QuadPart) * 1000000000 / frequency.QuadPart / 20000));
	WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), narrow, length, &written, NULL);
	length = wsprintfA(narrow, "%d|%5d|%-5d|%05d|%x|%X|%s|%c|%u", -42, 42, 42, 42, 255, 255, "abc", 'z', 7);
	if (!same_narrow(narrow, "-42|   42|42   |00042|ff|FF|abc|z|7") || length != narrow_length(narrow))
		return 1;
	length = wsprintfA(narrow, "%S", L"hi");
	if (!same_narrow(narrow, "hi") || length != 2)
		return 2;
	length = wsprintfA(narrow, "%s", "\xe9");
	if ((unsigned char)narrow[0] != 0xe9 || narrow[1] || length != 1)
		return 3;
	length = wsprintfW(wide, L"%s|%S|%d", L"wide", "narrow", 5);
	if (!same_wide(wide, L"wide|narrow|5") || length != 13)
		return 4;
	length = wsprintfA(narrow, "100%%");
	if (!same_narrow(narrow, "100%") || length != 4)
		return 5;
	length = wsprintfW(wide, L"%d|%i|%u|%x|%X", -5, 42, 3000000000u, 0xbeef, 0xbeef);
	if (!same_wide(wide, L"-5|42|3000000000|beef|BEEF") || length != 26)
		return 6;
	wsprintfW(wide, L"[%.3d][%+d][% d][%#x][%#X][%#x]", 7, 7, 7, 255, 255, 0);
	if (!same_wide(wide, L"[007][+7][ 7][0xff][0XFF][0]"))
		return 7;
	wsprintfW(wide, L"%.2s|%6s|%-6s|%hs|%ls", L"abc", L"abc", L"abc", "row", L"wid");
	if (!same_wide(wide, L"ab|   abc|abc   |row|wid"))
		return 8;
	/* %c is a character of the function's own width, %C of the other. */
	wsprintfW(wide, L"%c%C%lc%hc", L'W', 'N', L'L', 'H');
	if (!same_wide(wide, L"WNLH"))
		return 9;
	wsprintfW(wide, L"%*d|%-*d|%.*s|%*d", 4, 9, 3, 9, 2, L"xyz", -3, 1);
	if (!same_wide(wide, L"   9|9  |xy|1  "))
		return 10;
	wsprintfW(wide, L"%hd|%hu|%s", 0x18001, 0x18001, (WCHAR *)NULL);
	if (!same_wide(wide, L"-32767|32769|(null)"))
		return 11;
	/* A conversion it does not know ends the text. */
	length = wsprintfW(wide, L"100%% %q tail", 1);
	if (!same_wide(wide, L"100% ") || length != 5)
		return 12;
	/* wsprintf writes 1024 characters at most, with the terminator. */
	for (int at = 0; at < 2000; ++at)
		long_text[at] = L'a' + at % 26;
	long_text[2000] = 0;
	if (wsprintfW(wide, L"%s", long_text) != 1023 || wide[1023] || wide[1022] != long_text[1022])
		return 13;
	if (listed(wide, L"%s=%d", L"key", 7) != 5 || !same_wide(wide, L"key=7"))
		return 14;
	if (wnsprintfW(wide, 6, L"%s", L"abcdefgh") != 5 || !same_wide(wide, L"abcde"))
		return 15;
	if (wnsprintfA(narrow, 4, "%s", "abcdef") != 3 || !same_narrow(narrow, "abc"))
		return 16;
	if (wsprintfA(narrow, "%s-%S-%c-%C-%d", "a", L"b", 'c', L'd', 5) != 9 || !same_narrow(narrow, "a-b-c-d-5"))
		return 17;
	/* Outside ASCII the characters go through the process's ANSI code page, as WideCharToMultiByte and MultiByteToWideChar take them. */
	char euro[8] = {0};
	const int euro_length = WideCharToMultiByte(CP_ACP, 0, L"\x20ac", 1, euro, sizeof(euro) - 1, NULL, NULL);
	if (euro_length <= 0 || wsprintfA(narrow, "%S", L"\x20ac") != euro_length || !same_narrow(narrow, euro))
		return 18;
	WCHAR high = 0;
	if (MultiByteToWideChar(CP_ACP, 0, "\x80", 1, &high, 1) != 1 || wsprintfW(wide, L"%hs", "\x80") != 1 || wide[0] != high || wide[1])
		return 19;
	const char *command = GetCommandLineA();
	int guest = 0;
	for (int at = 0; command[at] && !guest; ++at)
		guest = command[at] == ' ' && same_narrow(command + at + 1, "guest");
	if (!guest)
		return 0;
	if (!inside("user32.dll", "wsprintfW", L"raven-user32.dll") || !inside("user32.dll", "wvsprintfA", L"raven-user32.dll"))
		return 20;
	if (!inside("shlwapi.dll", "wnsprintfW", L"raven-shell.dll"))
		return 21;
	WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "wsprintf checks passed\n", 23, &written, NULL);
	return 0;
}
