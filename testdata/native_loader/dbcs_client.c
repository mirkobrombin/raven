// The double-byte code pages convert both ways, report their lead bytes and their default characters.
#include <windows.h>

static int same(const WCHAR *left, const WCHAR *right, int count)
{
	for (int index = 0; index < count; ++index)
		if (left[index] != right[index])
			return 0;
	return 1;
}

static int same_bytes(const char *left, const char *right, int count)
{
	for (int index = 0; index < count; ++index)
		if (left[index] != right[index])
			return 0;
	return 1;
}

int raven_entry(void)
{
	WCHAR wide[16];
	char narrow[16];
	BOOL used = FALSE;

	// Shift-JIS: a hiragana, an ASCII letter, a half-width katakana and a pair whose trail byte is a backslash.
	static const WCHAR japanese[] = {0x3042, 'A', 0xff71, 0x30bd};
	if (MultiByteToWideChar(932, 0, "\x82\xa0" "A" "\xb1" "\x83\x5c", 6, wide, 16) != 4 || !same(wide, japanese, 4))
		return 1;
	if (MultiByteToWideChar(932, 0, "\x82\xa0" "A" "\xb1" "\x83\x5c", 6, NULL, 0) != 4)
		return 2;
	if (WideCharToMultiByte(932, 0, japanese, 4, narrow, 16, NULL, &used) != 6 || used ||
		!same_bytes(narrow, "\x82\xa0" "A" "\xb1" "\x83\x5c", 6))
		return 3;

	// A lead byte with nothing after it becomes the page's default character, or an error when invalid characters are refused.
	if (MultiByteToWideChar(932, 0, "A\x82", 2, wide, 16) != 2 || wide[1] != 0x30fb)
		return 4;
	SetLastError(0);
	if (MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, "A\x82", 2, wide, 16) || GetLastError() != ERROR_NO_UNICODE_TRANSLATION)
		return 5;

	// A character with only a best fit takes it unless the caller refuses best fits; one the page lacks becomes the default.
	static const WCHAR accented[] = {0x00e9};
	if (WideCharToMultiByte(932, 0, accented, 1, narrow, 16, NULL, &used) != 1 || narrow[0] != 'e' || used)
		return 6;
	if (WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, accented, 1, narrow, 16, NULL, &used) != 1 || narrow[0] != '?' || !used)
		return 7;
	static const WCHAR thai[] = {0x0e01};
	if (WideCharToMultiByte(932, 0, thai, 1, narrow, 16, "*", &used) != 1 || narrow[0] != '*' || !used)
		return 8;

	// The same character in the other pages.
	static const WCHAR middle[] = {0x4e2d};
	if (WideCharToMultiByte(936, 0, middle, 1, narrow, 16, NULL, NULL) != 2 || !same_bytes(narrow, "\xd6\xd0", 2))
		return 9;
	if (WideCharToMultiByte(950, 0, middle, 1, narrow, 16, NULL, NULL) != 2 || !same_bytes(narrow, "\xa4\xa4", 2))
		return 10;
	if (MultiByteToWideChar(949, 0, "\xc7\xd1", 2, wide, 16) != 1 || wide[0] != 0xd55c)
		return 11;
	if (MultiByteToWideChar(936, 0, "\xd6\xd0", -1, wide, 16) != 2 || wide[0] != 0x4e2d || wide[1])
		return 12;

	CPINFO info;
	if (!GetCPInfo(932, &info) || info.MaxCharSize != 2 || info.DefaultChar[0] != '?' || info.LeadByte[0] != 0x81 ||
		info.LeadByte[1] != 0x9f || info.LeadByte[2] != 0xe0 || info.LeadByte[3] != 0xfc || info.LeadByte[4] || info.LeadByte[5])
		return 13;
	CPINFOEXW extended;
	if (!GetCPInfoExW(932, 0, &extended) || extended.UnicodeDefaultChar != 0x30fb || extended.CodePage != 932 || extended.MaxCharSize != 2)
		return 14;
	if (!GetCPInfo(949, &info) || info.LeadByte[0] != 0x81 || info.LeadByte[1] != 0xfe || info.LeadByte[2])
		return 15;
	if (!IsDBCSLeadByteEx(932, 0x82) || IsDBCSLeadByteEx(932, 'A') || IsDBCSLeadByteEx(932, 0xb1) || IsDBCSLeadByteEx(1252, 0x82))
		return 16;
	if (!IsValidCodePage(932) || !IsValidCodePage(936) || !IsValidCodePage(949) || !IsValidCodePage(950))
		return 17;
	return 0;
}
