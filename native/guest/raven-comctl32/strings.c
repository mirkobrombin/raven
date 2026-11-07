/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>

/* The string helpers comctl32 exports by ordinal, and the owned strings that the controls keep. */

LPVOID WINAPI Alloc(DWORD size);
LPVOID WINAPI ReAlloc(LPVOID memory, DWORD size);
BOOL WINAPI Free(LPVOID memory);

/* ---- owned strings ---- */

INT WINAPI Str_GetPtrA(LPCSTR source, LPSTR out, INT capacity)
{
	if (!out || capacity <= 0)
		return source ? lstrlenA(source) + 1 : 0;
	if (!source) {
		out[0] = 0;
		return 0;
	}
	INT size = lstrlenA(source) + 1, taken = size < capacity ? size : capacity;
	CopyMemory(out, source, taken - 1);
	out[taken - 1] = 0;
	return taken;
}

INT WINAPI Str_GetPtrW(LPCWSTR source, LPWSTR out, INT capacity)
{
	if (!out || capacity <= 0)
		return source ? lstrlenW(source) + 1 : 0;
	if (!source) {
		out[0] = 0;
		return 0;
	}
	INT size = lstrlenW(source) + 1, taken = size < capacity ? size : capacity;
	CopyMemory(out, source, (taken - 1) * sizeof(WCHAR));
	out[taken - 1] = 0;
	return taken;
}

/* Keeps a private copy in *slot, which may hold an earlier one; no source frees it. */
static BOOL set_owned(void **slot, const void *source, SIZE_T bytes)
{
	if (!source) {
		Free(*slot);
		*slot = NULL;
		return TRUE;
	}
	void *copy = *slot ? ReAlloc(*slot, (DWORD)bytes) : Alloc((DWORD)bytes);
	if (!copy)
		return FALSE;
	CopyMemory(copy, source, bytes);
	*slot = copy;
	return TRUE;
}

BOOL WINAPI Str_SetPtrA(LPSTR *slot, LPCSTR source)
{
	return set_owned((void **)slot, source, source ? lstrlenA(source) + 1 : 0);
}

BOOL WINAPI Str_SetPtrW(LPWSTR *slot, LPCWSTR source)
{
	return set_owned((void **)slot, source, source ? (lstrlenW(source) + 1) * sizeof(WCHAR) : 0);
}

/* ---- comparing ---- */

/* Stops at a terminator as the count alone would not, since the comparison functions take exact lengths. */
static int bounded_a(const char *text, int count)
{
	int length = 0;
	while (length < count && text[length])
		++length;
	return length;
}

static int bounded_w(const WCHAR *text, int count)
{
	int length = 0;
	while (length < count && text[length])
		++length;
	return length;
}

static int compare_a(const char *left, const char *right, int count, DWORD flags)
{
	if (count <= 0)
		return 0;
	return CompareStringA(GetThreadLocale(), flags, left, bounded_a(left, count), right, bounded_a(right, count)) - CSTR_EQUAL;
}

static int compare_w(const WCHAR *left, const WCHAR *right, int count, DWORD flags)
{
	if (count <= 0)
		return 0;
	return CompareStringW(GetThreadLocale(), flags, left, bounded_w(left, count), right, bounded_w(right, count)) - CSTR_EQUAL;
}

INT WINAPI StrCmpNA(LPCSTR left, LPCSTR right, INT count)
{
	return compare_a(left, right, count, 0);
}

INT WINAPI StrCmpNIA(LPCSTR left, LPCSTR right, INT count)
{
	return compare_a(left, right, count, NORM_IGNORECASE);
}

INT WINAPI StrCmpNW(LPCWSTR left, LPCWSTR right, INT count)
{
	return compare_w(left, right, count, 0);
}

INT WINAPI StrCmpNIW(LPCWSTR left, LPCWSTR right, INT count)
{
	return compare_w(left, right, count, NORM_IGNORECASE);
}

BOOL WINAPI IntlStrEqWorkerA(BOOL exact, LPCSTR left, LPCSTR right, INT count)
{
	return !compare_a(left, right, count, exact ? 0 : NORM_IGNORECASE);
}

BOOL WINAPI IntlStrEqWorkerW(BOOL exact, LPCWSTR left, LPCWSTR right, INT count)
{
	return !compare_w(left, right, count, exact ? 0 : NORM_IGNORECASE);
}

/* ---- finding characters ---- */

/* A character of narrow text as these functions take it: a double byte one has its lead byte on top. */
static WORD unit_a(const char *text)
{
	BYTE first = (BYTE)text[0];
	return IsDBCSLeadByte(first) && text[1] ? (WORD)(first << 8 | (BYTE)text[1]) : first;
}

static BOOL same_a(WORD left, WORD right, BOOL ignore_case)
{
	if (left == right)
		return TRUE;
	if (!ignore_case)
		return FALSE;
	char a[2], b[2];
	int a_size = 0, b_size = 0;
	if (left > 0xff)
		a[a_size++] = (char)(left >> 8);
	a[a_size++] = (char)left;
	if (right > 0xff)
		b[b_size++] = (char)(right >> 8);
	b[b_size++] = (char)right;
	return CompareStringA(GetThreadLocale(), NORM_IGNORECASE, a, a_size, b, b_size) == CSTR_EQUAL;
}

static BOOL same_w(WCHAR left, WCHAR right, BOOL ignore_case)
{
	if (left == right)
		return TRUE;
	return ignore_case && CompareStringW(GetThreadLocale(), NORM_IGNORECASE, &left, 1, &right, 1) == CSTR_EQUAL;
}

static LPSTR find_first_a(LPCSTR text, WORD wanted, BOOL ignore_case)
{
	if (!text)
		return NULL;
	for (; *text; text = CharNextA(text))
		if (same_a(unit_a(text), wanted, ignore_case))
			return (LPSTR)text;
	return NULL;
}

static LPWSTR find_first_w(LPCWSTR text, WCHAR wanted, BOOL ignore_case)
{
	if (!text)
		return NULL;
	for (; *text; ++text)
		if (same_w(*text, wanted, ignore_case))
			return (LPWSTR)text;
	return NULL;
}

static LPSTR find_last_a(LPCSTR text, LPCSTR end, WORD wanted, BOOL ignore_case)
{
	if (!text)
		return NULL;
	LPCSTR found = NULL;
	for (; *text && (!end || text < end); text = CharNextA(text))
		if (same_a(unit_a(text), wanted, ignore_case))
			found = text;
	return (LPSTR)found;
}

static LPWSTR find_last_w(LPCWSTR text, LPCWSTR end, WCHAR wanted, BOOL ignore_case)
{
	if (!text)
		return NULL;
	LPCWSTR found = NULL;
	for (; *text && (!end || text < end); ++text)
		if (same_w(*text, wanted, ignore_case))
			found = text;
	return (LPWSTR)found;
}

LPSTR WINAPI StrChrA(LPCSTR text, WORD wanted)
{
	return find_first_a(text, wanted, FALSE);
}

LPSTR WINAPI StrChrIA(LPCSTR text, WORD wanted)
{
	return find_first_a(text, wanted, TRUE);
}

LPWSTR WINAPI StrChrW(LPCWSTR text, WCHAR wanted)
{
	return find_first_w(text, wanted, FALSE);
}

LPWSTR WINAPI StrChrIW(LPCWSTR text, WCHAR wanted)
{
	return find_first_w(text, wanted, TRUE);
}

LPSTR WINAPI StrRChrA(LPCSTR text, LPCSTR end, WORD wanted)
{
	return find_last_a(text, end, wanted, FALSE);
}

LPSTR WINAPI StrRChrIA(LPCSTR text, LPCSTR end, WORD wanted)
{
	return find_last_a(text, end, wanted, TRUE);
}

LPWSTR WINAPI StrRChrW(LPCWSTR text, LPCWSTR end, WORD wanted)
{
	return find_last_w(text, end, wanted, FALSE);
}

LPWSTR WINAPI StrRChrIW(LPCWSTR text, LPCWSTR end, WORD wanted)
{
	return find_last_w(text, end, wanted, TRUE);
}

/* ---- finding strings ---- */

static LPSTR find_text_a(LPCSTR text, LPCSTR wanted, DWORD flags)
{
	if (!text || !wanted || !*wanted)
		return NULL;
	int length = lstrlenA(wanted);
	for (; *text; text = CharNextA(text))
		if (!compare_a(text, wanted, length, flags))
			return (LPSTR)text;
	return NULL;
}

static LPWSTR find_text_w(LPCWSTR text, LPCWSTR wanted, DWORD flags)
{
	if (!text || !wanted || !*wanted)
		return NULL;
	int length = lstrlenW(wanted);
	for (; *text; ++text)
		if (!compare_w(text, wanted, length, flags))
			return (LPWSTR)text;
	return NULL;
}

LPSTR WINAPI StrStrA(LPCSTR text, LPCSTR wanted)
{
	return find_text_a(text, wanted, 0);
}

LPSTR WINAPI StrStrIA(LPCSTR text, LPCSTR wanted)
{
	return find_text_a(text, wanted, NORM_IGNORECASE);
}

LPWSTR WINAPI StrStrW(LPCWSTR text, LPCWSTR wanted)
{
	return find_text_w(text, wanted, 0);
}

LPWSTR WINAPI StrStrIW(LPCWSTR text, LPCWSTR wanted)
{
	return find_text_w(text, wanted, NORM_IGNORECASE);
}

LPSTR WINAPI StrRStrIA(LPCSTR text, LPCSTR end, LPCSTR wanted)
{
	if (!text || !wanted || !*wanted)
		return NULL;
	int length = lstrlenA(wanted);
	LPCSTR found = NULL;
	for (; *text && (!end || text < end); text = CharNextA(text))
		if (!compare_a(text, wanted, length, NORM_IGNORECASE))
			found = text;
	return (LPSTR)found;
}

LPWSTR WINAPI StrRStrIW(LPCWSTR text, LPCWSTR end, LPCWSTR wanted)
{
	if (!text || !wanted || !*wanted)
		return NULL;
	int length = lstrlenW(wanted);
	LPCWSTR found = NULL;
	for (; *text && (!end || text < end); ++text)
		if (!compare_w(text, wanted, length, NORM_IGNORECASE))
			found = text;
	return (LPWSTR)found;
}

/* ---- spans and numbers ---- */

static int span_a(LPCSTR text, LPCSTR set, BOOL ignore_case)
{
	if (!text || !set)
		return 0;
	LPCSTR from = text;
	for (; *text; text = CharNextA(text))
		if (find_first_a(set, unit_a(text), ignore_case))
			break;
	return (int)(text - from);
}

static int span_w(LPCWSTR text, LPCWSTR set, BOOL ignore_case)
{
	if (!text || !set)
		return 0;
	LPCWSTR from = text;
	for (; *text; ++text)
		if (find_first_w(set, *text, ignore_case))
			break;
	return (int)(text - from);
}

int WINAPI StrCSpnA(LPCSTR text, LPCSTR set)
{
	return span_a(text, set, FALSE);
}

int WINAPI StrCSpnIA(LPCSTR text, LPCSTR set)
{
	return span_a(text, set, TRUE);
}

int WINAPI StrCSpnW(LPCWSTR text, LPCWSTR set)
{
	return span_w(text, set, FALSE);
}

int WINAPI StrCSpnIW(LPCWSTR text, LPCWSTR set)
{
	return span_w(text, set, TRUE);
}

INT WINAPI StrToIntA(LPCSTR text)
{
	if (!text)
		return 0;
	BOOL negative = *text == '-';
	if (negative)
		++text;
	INT value = 0;
	for (; *text >= '0' && *text <= '9'; ++text)
		value = value * 10 + (*text - '0');
	return negative ? -value : value;
}

INT WINAPI StrToIntW(LPCWSTR text)
{
	if (!text)
		return 0;
	BOOL negative = *text == L'-';
	if (negative)
		++text;
	INT value = 0;
	for (; *text >= L'0' && *text <= L'9'; ++text)
		value = value * 10 + (*text - L'0');
	return negative ? -value : value;
}
