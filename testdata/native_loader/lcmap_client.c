#include <windows.h>

// LCMapString's lengths, as CPython's ntpath.normcase relies on them: a source given with its length maps to that many characters
// and nothing is written after them (CPython sizes the buffer from the first call exactly); a source ended by a zero maps, counts
// and writes the zero too, and a buffer without room for it fails with ERROR_INSUFFICIENT_BUFFER. Returns 0, or the number of the
// check that failed.
int raven_entry(void)
{
	static WCHAR wide[8];
	static char narrow[8];
	if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, L"ABC", 3, NULL, 0, NULL, NULL, 0) != 3)
		return 1;
	wide[3] = 0x7777;
	if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, L"ABC", 3, wide, 3, NULL, NULL, 0) != 3 || wide[0] != L'a' ||
		wide[2] != L'c' || wide[3] != 0x7777)
		return 2;
	if (LCMapStringW(LOCALE_INVARIANT, LCMAP_UPPERCASE, L"abc", -1, NULL, 0) != 4)
		return 3;
	SetLastError(0);
	if (LCMapStringW(LOCALE_INVARIANT, LCMAP_UPPERCASE, L"abc", -1, wide, 3) != 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return 4;
	if (LCMapStringW(LOCALE_INVARIANT, LCMAP_UPPERCASE, L"abc", -1, wide, 4) != 4 || wide[0] != L'A' || wide[3] != 0)
		return 5;
	narrow[3] = 0x77;
	if (LCMapStringA(LOCALE_INVARIANT, LCMAP_UPPERCASE, "abc", 3, narrow, 3) != 3 || narrow[0] != 'A' || narrow[3] != 0x77)
		return 6;
	if (LCMapStringA(LOCALE_INVARIANT, LCMAP_UPPERCASE, "abc", -1, NULL, 0) != 4 ||
		LCMapStringA(LOCALE_INVARIANT, LCMAP_UPPERCASE, "abc", -1, narrow, 4) != 4 || narrow[3] != 0)
		return 7;
	return 0;
}
