/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>

/* The character helpers of user32, as Microsoft documents them. A pointer whose high bits are zero stands for one character, which the conversion
 * functions return converted. */

static BOOL is_single(const void *pointer)
{
	return !((ULONG_PTR)pointer >> 16);
}

static DWORD change_case(LPWSTR text, DWORD length, DWORD map)
{
	if (length)
		LCMapStringW(LOCALE_USER_DEFAULT, map | LCMAP_LINGUISTIC_CASING, text, (int)length, text, (int)length);
	return length;
}

/* An ANSI string changes through its wide form, keeping its length when the conversion does. */
static DWORD change_case_ansi(LPSTR text, DWORD length, DWORD map)
{
	if (!length)
		return 0;
	int wide = MultiByteToWideChar(CP_ACP, 0, text, (int)length, NULL, 0);
	WCHAR small_buffer[256], *buffer = wide <= 256 ? small_buffer : HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * wide);
	if (!buffer)
		return 0;
	MultiByteToWideChar(CP_ACP, 0, text, (int)length, buffer, wide);
	change_case(buffer, (DWORD)wide, map);
	int written = WideCharToMultiByte(CP_ACP, 0, buffer, wide, text, (int)length, NULL, NULL);
	if (buffer != small_buffer)
		HeapFree(GetProcessHeap(), 0, buffer);
	return (DWORD)written;
}

static LPWSTR case_wide(LPWSTR text, DWORD map)
{
	if (is_single(text)) {
		WCHAR single = (WCHAR)(ULONG_PTR)text;
		change_case(&single, 1, map);
		return (LPWSTR)(ULONG_PTR)single;
	}
	change_case(text, (DWORD)lstrlenW(text), map);
	return text;
}

static LPSTR case_ansi(LPSTR text, DWORD map)
{
	if (is_single(text)) {
		char single = (char)(ULONG_PTR)text;
		change_case_ansi(&single, 1, map);
		return (LPSTR)(ULONG_PTR)(unsigned char)single;
	}
	change_case_ansi(text, (DWORD)lstrlenA(text), map);
	return text;
}

LPWSTR WINAPI CharLowerW(LPWSTR text)
{
	return case_wide(text, LCMAP_LOWERCASE);
}

LPWSTR WINAPI CharUpperW(LPWSTR text)
{
	return case_wide(text, LCMAP_UPPERCASE);
}

LPSTR WINAPI CharLowerA(LPSTR text)
{
	return case_ansi(text, LCMAP_LOWERCASE);
}

LPSTR WINAPI CharUpperA(LPSTR text)
{
	return case_ansi(text, LCMAP_UPPERCASE);
}

DWORD WINAPI CharLowerBuffW(LPWSTR text, DWORD length)
{
	return text ? change_case(text, length, LCMAP_LOWERCASE) : 0;
}

DWORD WINAPI CharUpperBuffW(LPWSTR text, DWORD length)
{
	return text ? change_case(text, length, LCMAP_UPPERCASE) : 0;
}

DWORD WINAPI CharLowerBuffA(LPSTR text, DWORD length)
{
	return text ? change_case_ansi(text, length, LCMAP_LOWERCASE) : 0;
}

DWORD WINAPI CharUpperBuffA(LPSTR text, DWORD length)
{
	return text ? change_case_ansi(text, length, LCMAP_UPPERCASE) : 0;
}

LPWSTR WINAPI CharNextW(LPCWSTR text)
{
	return (LPWSTR)(*text ? text + 1 : text);
}

LPWSTR WINAPI CharPrevW(LPCWSTR start, LPCWSTR text)
{
	return (LPWSTR)(text > start ? text - 1 : start);
}

LPSTR WINAPI CharNextExA(WORD code_page, LPCSTR text, DWORD flags)
{
	if (!*text)
		return (LPSTR)text;
	if (IsDBCSLeadByteEx(code_page, (BYTE)*text) && text[1])
		return (LPSTR)text + 2;
	return (LPSTR)text + 1;
}

LPSTR WINAPI CharPrevExA(WORD code_page, LPCSTR start, LPCSTR text, DWORD flags)
{
	/* Lead and trail bytes can only be told apart walking from the start. */
	while (*start && start < text) {
		LPCSTR next = CharNextExA(code_page, start, 0);
		if (next >= text)
			break;
		start = next;
	}
	return (LPSTR)start;
}

LPSTR WINAPI CharNextA(LPCSTR text)
{
	return CharNextExA(CP_ACP, text, 0);
}

LPSTR WINAPI CharPrevA(LPCSTR start, LPCSTR text)
{
	return CharPrevExA(CP_ACP, start, text, 0);
}

static BOOL has_type_wide(WCHAR character, WORD mask)
{
	WORD type = 0;
	return GetStringTypeW(CT_CTYPE1, &character, 1, &type) && (type & mask);
}

static BOOL has_type_ansi(CHAR character, WORD mask)
{
	WCHAR wide;
	return MultiByteToWideChar(CP_ACP, 0, &character, 1, &wide, 1) && has_type_wide(wide, mask);
}

BOOL WINAPI IsCharAlphaW(WCHAR character)
{
	return has_type_wide(character, C1_ALPHA);
}

BOOL WINAPI IsCharAlphaNumericW(WCHAR character)
{
	return has_type_wide(character, C1_ALPHA | C1_DIGIT);
}

BOOL WINAPI IsCharLowerW(WCHAR character)
{
	return has_type_wide(character, C1_LOWER);
}

BOOL WINAPI IsCharUpperW(WCHAR character)
{
	return has_type_wide(character, C1_UPPER);
}

BOOL WINAPI IsCharAlphaA(CHAR character)
{
	return has_type_ansi(character, C1_ALPHA);
}

BOOL WINAPI IsCharAlphaNumericA(CHAR character)
{
	return has_type_ansi(character, C1_ALPHA | C1_DIGIT);
}

BOOL WINAPI IsCharLowerA(CHAR character)
{
	return has_type_ansi(character, C1_LOWER);
}

BOOL WINAPI IsCharUpperA(CHAR character)
{
	return has_type_ansi(character, C1_UPPER);
}

BOOL WINAPI CharToOemBuffW(LPCWSTR source, LPSTR target, DWORD length)
{
	if (!source || !target)
		return FALSE;
	WideCharToMultiByte(CP_OEMCP, 0, source, (int)length, target, (int)length, NULL, NULL);
	return TRUE;
}

BOOL WINAPI CharToOemW(LPCWSTR source, LPSTR target)
{
	return source ? CharToOemBuffW(source, target, (DWORD)lstrlenW(source) + 1) : FALSE;
}

BOOL WINAPI OemToCharBuffW(LPCSTR source, LPWSTR target, DWORD length)
{
	if (!source || !target)
		return FALSE;
	MultiByteToWideChar(CP_OEMCP, 0, source, (int)length, target, (int)length);
	return TRUE;
}

BOOL WINAPI OemToCharW(LPCSTR source, LPWSTR target)
{
	return source ? OemToCharBuffW(source, target, (DWORD)lstrlenA(source) + 1) : FALSE;
}

/* Between two single-byte forms the text goes through its wide form, and source and target may be one buffer. */
static BOOL recode(UINT from, UINT to, LPCSTR source, LPSTR target, DWORD length)
{
	if (!source || !target)
		return FALSE;
	if (!length)
		return TRUE;
	WCHAR small_buffer[256], *buffer = length <= 256 ? small_buffer : HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * length);
	if (!buffer)
		return FALSE;
	int wide = MultiByteToWideChar(from, 0, source, (int)length, buffer, (int)length);
	WideCharToMultiByte(to, 0, buffer, wide, target, (int)length, NULL, NULL);
	if (buffer != small_buffer)
		HeapFree(GetProcessHeap(), 0, buffer);
	return TRUE;
}

BOOL WINAPI CharToOemBuffA(LPCSTR source, LPSTR target, DWORD length)
{
	return recode(CP_ACP, CP_OEMCP, source, target, length);
}

BOOL WINAPI CharToOemA(LPCSTR source, LPSTR target)
{
	return source ? recode(CP_ACP, CP_OEMCP, source, target, (DWORD)lstrlenA(source) + 1) : FALSE;
}

BOOL WINAPI OemToCharBuffA(LPCSTR source, LPSTR target, DWORD length)
{
	return recode(CP_OEMCP, CP_ACP, source, target, length);
}

BOOL WINAPI OemToCharA(LPCSTR source, LPSTR target)
{
	return source ? recode(CP_OEMCP, CP_ACP, source, target, (DWORD)lstrlenA(source) + 1) : FALSE;
}
