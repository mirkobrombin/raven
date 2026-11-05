#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The kernel string services the runtime may answer inside the program must agree with the ones it serves: lstrcmpW with
 * CompareStringW, lstrlenW and lstrcpynW with what they are given, and the plain ASCII conversions with their round trip. A null string
 * comes before every other in the lstrcmp family. */

static int sign(int value)
{
	return value < 0 ? -1 : value > 0 ? 1 : 0;
}

int raven_entry(void)
{
	static const WCHAR *const words[] = {L"", L"a", L"A", L"b", L"B", L"ab", L"aB", L"Ab", L"AB", L"abc", L"abd", L"ABC", L"a b", L"a-b",
		L"a_b", L"a1", L"a10", L"a2", L"Zeta", L"zeta", L"zetA", L"[x]", L"@x", L"~x", L"notepad++.exe", L"Notepad++.EXE", L"1", L"10", L"9"};
	const unsigned count = sizeof(words) / sizeof(words[0]);
	for (unsigned left = 0; left < count; ++left)
		for (unsigned right = 0; right < count; ++right) {
			const int expected = CompareStringW(LOCALE_USER_DEFAULT, 0, words[left], -1, words[right], -1) - 2;
			if (sign(lstrcmpW(words[left], words[right])) != expected)
				return 1;
		}
	if (lstrlenW(L"notepad") != 7 || lstrlenW(L"") != 0 || lstrlenW(NULL) != 0 || lstrlenA("abc") != 3)
		return 2;
	WCHAR copy[8];
	for (unsigned at = 0; at < 8; ++at)
		copy[at] = 0x7777;
	if (lstrcpynW(copy, L"abcdefgh", 4) != copy || lstrcmpW(copy, L"abc") || copy[4] != 0x7777)
		return 3;
	char ansi[16];
	WCHAR wide[16];
	if (MultiByteToWideChar(1252, MB_PRECOMPOSED | MB_ERR_INVALID_CHARS, "Raven", -1, wide, 16) != 6 || lstrcmpW(wide, L"Raven") ||
		MultiByteToWideChar(CP_UTF8, 0, "Raven", 5, NULL, 0) != 5 ||
		WideCharToMultiByte(CP_ACP, 0, wide, -1, ansi, 16, NULL, NULL) != 6 || lstrcmpA(ansi, "Raven"))
		return 4;
	SetLastError(71);
	if (MultiByteToWideChar(CP_UTF8, 0, "Raven", -1, wide, 2) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return 5;
	if (lstrcmpW(NULL, NULL) || lstrcmpW(NULL, L"") != -1 || lstrcmpW(L"", NULL) != 1 || lstrcmpiW(NULL, L"a") != -1 ||
		lstrcmpA("a", NULL) != 1 || lstrcmpiA(NULL, NULL))
		return 6;
	return 0;
}
