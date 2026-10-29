#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Locale information, whether the runtime or the program answers it: lengths, texts, numbers and the failures. */

static int same(const WCHAR *left, const WCHAR *right)
{
	while (*left && *left == *right)
		++left, ++right;
	return *left == *right;
}

static unsigned enumerated;
static BOOL CALLBACK locale_found(LPWSTR name, DWORD flags, LPARAM parameter)
{
	if (!same(name, L"en-US") || flags != (LOCALE_WINDOWS | LOCALE_SPECIFICDATA) || parameter != (LPARAM)&enumerated)
		enumerated = 100;
	++enumerated;
	return FALSE;
}

int raven_entry(void)
{
	WCHAR text[64];
	for (int round = 0; round < 3; ++round) {
		if (GetLocaleInfoEx(NULL, LOCALE_SDECIMAL, NULL, 0) != 2 || GetLocaleInfoEx(L"en-US", LOCALE_SDECIMAL, text, 64) != 2 || !same(text, L"."))
			return 1;
		if (GetLocaleInfoEx(NULL, LOCALE_SMONTHNAME1, text, 64) != 8 || !same(text, L"January"))
			return 2;
		if (GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SSHORTDATE, text, 64) != 9 || !same(text, L"M/d/yyyy"))
			return 3;
		if (GetLocaleInfoEx(NULL, LOCALE_STIMEFORMAT, text, 64) != 11 || !same(text, L"h:mm:ss tt"))
			return 4;
		DWORD number = 0;
		if (GetLocaleInfoEx(NULL, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER, (LPWSTR)&number, 2) != 2 || number != 6)
			return 5;
		if (GetLocaleInfoEx(NULL, LOCALE_SGROUPING | LOCALE_NOUSEROVERRIDE, text, 64) != 4 || !same(text, L"3;0"))
			return 6;
		SetLastError(0);
		if (GetLocaleInfoEx(NULL, LOCALE_SMONTHNAME1, text, 3) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
			return 7;
		SetLastError(0);
		if (GetLocaleInfoEx(NULL, 0x7f, text, 64) || GetLastError() != ERROR_INVALID_PARAMETER)
			return 8;
		/* The text ends at its terminator and leaves the rest of the buffer alone. */
		text[5] = 0x1234;
		if (GetLocaleInfoEx(NULL, LOCALE_SCURRENCY, text, 64) != 2 || !same(text, L"$") || text[5] != 0x1234)
			return 9;
	}
	if (!EnumSystemLocalesEx(locale_found, LOCALE_ALL, (LPARAM)&enumerated, NULL) || enumerated != 1)
		return 10;
	if (!EnumSystemLocalesEx(locale_found, LOCALE_WINDOWS | LOCALE_SPECIFICDATA, (LPARAM)&enumerated, NULL) || enumerated != 2)
		return 11;
	if (!EnumSystemLocalesEx(locale_found, LOCALE_SUPPLEMENTAL, (LPARAM)&enumerated, NULL) || enumerated != 2)
		return 12;
	if (!EnumSystemLocalesEx(locale_found, LOCALE_NEUTRALDATA, (LPARAM)&enumerated, NULL) || enumerated != 2)
		return 13;
	SetLastError(0);
	if (EnumSystemLocalesEx(NULL, 0, 0, NULL) || GetLastError() != ERROR_INVALID_PARAMETER)
		return 14;
	SetLastError(0);
	if (EnumSystemLocalesEx(locale_found, 0, 0, &enumerated) || GetLastError() != ERROR_INVALID_PARAMETER)
		return 15;
	SetLastError(0);
	if (EnumSystemLocalesEx(locale_found, 0x80000000, 0, NULL) || GetLastError() != ERROR_INVALID_FLAGS || enumerated != 2)
		return 16;
	if (!IsValidLocaleName(L"en-US") || !IsValidLocaleName(L"EN-us") || IsValidLocaleName(L"invalid-locale") || IsValidLocaleName(L"en-US-extra"))
		return 17;
	/* Names convert to identifiers and back; the culture-neutral "x-none" is no locale. */
	WCHAR name[LOCALE_NAME_MAX_LENGTH];
	if (LocaleNameToLCID(L"it-IT", 0) != 0x410 || LocaleNameToLCID(L"EN-us", 0) != 0x409 || LocaleNameToLCID(L"", 0) != 0x7f) return 18;
	SetLastError(0);
	if (LocaleNameToLCID(L"x-none", 0) || GetLastError() != ERROR_INVALID_PARAMETER) return 19;
	if (LCIDToLocaleName(0x407, name, LOCALE_NAME_MAX_LENGTH, 0) != 6 || name[0] != 'd' || name[3] != 'D') return 20;
	if (LCIDToLocaleName(0x1234, name, LOCALE_NAME_MAX_LENGTH, 0)) return 21;
	if (LCIDToLocaleName(0x459, name, LOCALE_NAME_MAX_LENGTH, 0) != 11 || lstrcmpW(name, L"sd-Deva-IN")) return 22;
	if (LocaleNameToLCID(L"ar-EG", 0) != 0xc01) return 23;
	return 0;
}
