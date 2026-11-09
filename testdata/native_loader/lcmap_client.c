#include <windows.h>

// The order of two sort keys compared byte by byte, as .NET compares and hashes them.
static int key_order(const BYTE *left, const BYTE *right, int length)
{
	for (int at = 0; at < length; ++at)
		if (left[at] != right[at])
			return left[at] < right[at] ? -1 : 1;
	return 0;
}

// Sort keys follow CompareString: keys of texts that differ only in case are equal when case is ignored, and order texts as it does.
static int sort_keys(void)
{
	static BYTE first[256], second[256];
	const DWORD ignore = LCMAP_SORTKEY | NORM_IGNORECASE;
	int one = LCMapStringEx(LOCALE_NAME_INVARIANT, ignore, L"PassagelistsSyncBaseUri", -1, (LPWSTR)first, sizeof(first), NULL, NULL, 0);
	int two = LCMapStringEx(LOCALE_NAME_INVARIANT, ignore, L"PassageListsSyncBaseUri", -1, (LPWSTR)second, sizeof(second), NULL, NULL, 0);
	if (!one || one != two || key_order(first, second, one) || first[one - 1] != 0)
		return 1;
	if (LCMapStringEx(LOCALE_NAME_INVARIANT, ignore, L"Passage", -1, NULL, 0, NULL, NULL, 0) <= 0)
		return 2;
	one = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_SORTKEY, L"abc", -1, (LPWSTR)first, sizeof(first), NULL, NULL, 0);
	two = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_SORTKEY, L"ABC", -1, (LPWSTR)second, sizeof(second), NULL, NULL, 0);
	if (!one || one != two || !key_order(first, second, one))
		return 3;
	static const WCHAR *const texts[][2] = {{L"apple", L"Banana"}, {L"Zeta", L"alpha"}, {L"abc", L"ABC"}, {L"a", L"ab"}};
	for (int index = 0; index < 4; ++index) {
		one = LCMapStringW(LOCALE_INVARIANT, LCMAP_SORTKEY, texts[index][0], -1, (LPWSTR)first, sizeof(first));
		two = LCMapStringW(LOCALE_INVARIANT, LCMAP_SORTKEY, texts[index][1], -1, (LPWSTR)second, sizeof(second));
		const int compared = CompareStringW(LOCALE_INVARIANT, 0, texts[index][0], -1, texts[index][1], -1) - CSTR_EQUAL;
		int ordered = key_order(first, second, one < two ? one : two);
		if (!ordered && one != two)
			ordered = one < two ? -1 : 1;
		if (!one || !two || ordered != compared)
			return 4 + index;
	}
	// A hyphen only breaks a tie, but the texts stay different.
	one = LCMapStringW(LOCALE_INVARIANT, LCMAP_SORTKEY | NORM_IGNORECASE, L"co-op", -1, (LPWSTR)first, sizeof(first));
	two = LCMapStringW(LOCALE_INVARIANT, LCMAP_SORTKEY | NORM_IGNORECASE, L"coop", -1, (LPWSTR)second, sizeof(second));
	if (!one || !two || (one == two && !key_order(first, second, one)))
		return 9;
	SetLastError(0);
	if (LCMapStringW(LOCALE_INVARIANT, LCMAP_SORTKEY, L"abc", -1, (LPWSTR)first, 2) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return 8;
	return 0;
}

// FindStringOrdinal: the index of a value code unit by code unit, from either end or anchored to one, -1 when it is absent.
static int ordinal_search(void)
{
	if (FindStringOrdinal(FIND_FROMSTART, L"abcabc", -1, L"bc", -1, FALSE) != 1)
		return 1;
	if (FindStringOrdinal(FIND_FROMEND, L"abcabc", 6, L"bc", 2, FALSE) != 4)
		return 2;
	if (FindStringOrdinal(FIND_STARTSWITH, L"abcabc", -1, L"ABC", -1, TRUE) != 0 ||
		FindStringOrdinal(FIND_STARTSWITH, L"abcabc", -1, L"ABC", -1, FALSE) != -1)
		return 3;
	if (FindStringOrdinal(FIND_ENDSWITH, L"abcabc", -1, L"cabc", -1, FALSE) != 2)
		return 4;
	SetLastError(5);
	if (FindStringOrdinal(FIND_FROMSTART, L"abc", -1, L"x", -1, FALSE) != -1 || GetLastError() != ERROR_SUCCESS)
		return 5;
	if (FindStringOrdinal(0, L"abc", -1, L"a", -1, FALSE) != -1 || GetLastError() != ERROR_INVALID_PARAMETER)
		return 6;
	return 0;
}

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
	const int keys = sort_keys();
	if (keys)
		return 10 + keys;
	const int search = ordinal_search();
	if (search)
		return 30 + search;
	// A thread's message extra information is what it set last, given back by the next set.
	if (SetMessageExtraInfo(0x5150) != 0 || GetMessageExtraInfo() != 0x5150 || SetMessageExtraInfo(7) != 0x5150)
		return 40;
	return 0;
}
