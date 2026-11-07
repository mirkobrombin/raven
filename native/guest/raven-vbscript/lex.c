/* SPDX-License-Identifier: MIT */
#include "vbs.h"

/* The lexer: names and keywords, numbers, strings with doubled quotes, date literals, operators and line ends. A comment runs to the end
 * of its line; an underscore at the end of a line continues the statement on the next. */

static void push(token_list *list, token t)
{
	if (list->count == list->room) {
		list->room = list->room ? list->room * 2 : 256;
		list->items = vbs_grow(list->items, list->room * sizeof(token));
	}
	list->items[list->count++] = t;
}

static BOOL name_char(WCHAR c, BOOL first)
{
	return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c >= 0xc0 || (!first && ((c >= L'0' && c <= L'9') || c == L'_'));
}

static BOOL digit(WCHAR c, UINT base)
{
	if (base == 16)
		return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
	if (base == 8)
		return c >= L'0' && c <= L'7';
	return c >= L'0' && c <= L'9';
}

/* Integer literals take the smallest of Integer and Long that holds them, and a Double beyond; hexadecimal and octal ones of up to four
 * digits are Integers and of up to eight Longs, read as the bits of the type, so &HFFFF is -1. */
static BOOL read_number(const WCHAR **cursor, VARIANT *out)
{
	const WCHAR *at = *cursor;
	VariantInit(out);
	if (at[0] == L'&') {
		UINT base = at[1] == L'h' || at[1] == L'H' ? 16 : 8;
		at += 2;
		ULONGLONG bits = 0;
		UINT count = 0;
		while (digit(*at, base)) {
			bits = bits * base + (*at <= L'9' ? *at - L'0' : (*at | 0x20) - L'a' + 10);
			if (++count > 11)
				return FALSE;
			at++;
		}
		if (!count)
			return FALSE;
		BOOL is_long = *at == L'&';
		if (is_long)
			at++;
		if (!is_long && bits <= 0xffff) {
			V_VT(out) = VT_I2;
			V_I2(out) = (SHORT)(USHORT)bits;
		} else if (bits <= 0xffffffffULL) {
			V_VT(out) = VT_I4;
			V_I4(out) = (LONG)(ULONG)bits;
		} else {
			return FALSE;
		}
		*cursor = at;
		return TRUE;
	}
	const WCHAR *start = at;
	BOOL real = FALSE;
	while (digit(*at, 10))
		at++;
	if (*at == L'.' && digit(at[1], 10)) {
		real = TRUE;
		for (at++; digit(*at, 10); at++)
			;
	} else if (*at == L'.' && at > start) {
		real = TRUE;
		at++;
	}
	if ((*at == L'e' || *at == L'E') && (digit(at[1], 10) || ((at[1] == L'-' || at[1] == L'+') && digit(at[2], 10)))) {
		real = TRUE;
		at += 2;
		while (digit(*at, 10))
			at++;
	}
	if (name_char(*at, TRUE))
		return FALSE;
	double number = 0, scale = 0;
	int exponent = 0, sign = 1;
	for (const WCHAR *c = start; c < at; c++) {
		if (*c == L'.') {
			scale = 1;
		} else if (*c == L'e' || *c == L'E') {
			c++;
			if (*c == L'-' || *c == L'+')
				sign = *c++ == L'-' ? -1 : 1;
			for (; c < at; c++)
				exponent = exponent * 10 + (*c - L'0');
			break;
		} else {
			number = number * 10 + (*c - L'0');
			if (scale)
				scale *= 10;
		}
	}
	if (scale)
		number /= scale;
	for (int i = 0; i < exponent && i < 400; i++)
		number = sign > 0 ? number * 10 : number / 10;
	if (!real && number <= 32767) {
		V_VT(out) = VT_I2;
		V_I2(out) = (SHORT)number;
	} else if (!real && number <= 2147483647.0) {
		V_VT(out) = VT_I4;
		V_I4(out) = (LONG)number;
	} else {
		V_VT(out) = VT_R8;
		V_R8(out) = number;
	}
	*cursor = at;
	return TRUE;
}

BOOL vbs_tokenize(const WCHAR *code, token_list *list, UINT *line_out, UINT *column_out)
{
	UINT line = 1;
	const WCHAR *at = code, *line_start = code;
	while (*at) {
		WCHAR c = *at;
		UINT column = (UINT)(at - line_start);
		token t = { .line = line, .column = column, .spaced = at > code && (at[-1] == L' ' || at[-1] == L'\t') };
		if (c == L' ' || c == L'\t' || c == L'\r' || c == 0xa0) {
			at++;
		} else if (c == L'\n' || c == L':') {
			t.kind = T_LINE;
			t.colon = c == L':';
			push(list, t);
			if (c == L'\n') {
				line++;
				line_start = at + 1;
			}
			at++;
		} else if (c == L'\'' || ((c == L'r' || c == L'R') && (at[1] == L'e' || at[1] == L'E') && (at[2] == L'm' || at[2] == L'M') &&
			!name_char(at[3], FALSE) && (at == code || !name_char(at[-1], FALSE)) && (at == code || at[-1] != L'.'))) {
			while (*at && *at != L'\n')
				at++;
		} else if (c == L'_' && !name_char(at[1], FALSE)) {
			/* A continuation: nothing but blanks follow it on its line. */
			const WCHAR *next = at + 1;
			while (*next == L' ' || *next == L'\t' || *next == L'\r')
				next++;
			if (*next && *next != L'\n')
				goto bad;
			at = *next ? next + 1 : next;
			line++;
			line_start = at;
		} else if (name_char(c, TRUE)) {
			const WCHAR *start = at;
			while (name_char(*at, FALSE))
				at++;
			t.kind = T_NAME;
			t.text = vbs_copy_n(start, at - start);
			push(list, t);
		} else if (c == L'[') {
			/* A bracketed name may hold any character but the closing bracket and a line end. */
			const WCHAR *start = ++at;
			while (*at && *at != L']' && *at != L'\n')
				at++;
			if (*at != L']')
				goto bad;
			t.kind = T_NAME;
			t.text = vbs_copy_n(start, at - start);
			at++;
			push(list, t);
		} else if (digit(c, 10) || (c == L'.' && digit(at[1], 10)) || (c == L'&' && (at[1] == L'h' || at[1] == L'H' || at[1] == L'o' ||
			at[1] == L'O'))) {
			t.kind = T_NUMBER;
			if (!read_number(&at, &t.number))
				goto bad;
			push(list, t);
		} else if (c == L'"') {
			SIZE_T room = 16, length = 0;
			WCHAR *text = vbs_alloc(room * sizeof(WCHAR));
			for (at++;; at++) {
				if (!*at || *at == L'\n') {
					vbs_free(text);
					goto bad;
				}
				if (*at == L'"') {
					if (at[1] != L'"')
						break;
					at++;
				}
				if (length + 2 > room)
					text = vbs_grow(text, (room *= 2) * sizeof(WCHAR));
				text[length++] = *at;
			}
			text[length] = 0;
			at++;
			t.kind = T_TEXT;
			t.text = text;
			push(list, t);
		} else if (c == L'#') {
			const WCHAR *start = ++at;
			while (*at && *at != L'#' && *at != L'\n')
				at++;
			if (*at != L'#')
				goto bad;
			WCHAR *text = vbs_copy_n(start, at - start);
			at++;
			DATE date;
			HRESULT hr = VarDateFromStr(text, MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT), 0, &date);
			vbs_free(text);
			if (FAILED(hr))
				goto bad;
			t.kind = T_DATE;
			V_VT(&t.number) = VT_DATE;
			V_DATE(&t.number) = date;
			push(list, t);
		} else {
			static const WCHAR *const pairs[] = { L"<>", L"<=", L">=", L"=<", L"=>", NULL };
			t.kind = T_SYMBOL;
			for (UINT i = 0; pairs[i]; i++)
				if (at[0] == pairs[i][0] && at[1] == pairs[i][1]) {
					/* =< and => are the same operators as <= and >=. */
					t.symbol[0] = pairs[i][0] == L'=' ? pairs[i][1] : pairs[i][0];
					t.symbol[1] = pairs[i][0] == L'=' ? L'=' : pairs[i][1];
					at += 2;
					break;
				}
			if (!t.symbol[0]) {
				if (!wcschr(L"+-*/\\^&=<>(),.", c))
					goto bad;
				t.symbol[0] = c;
				at++;
			}
			push(list, t);
		}
	}
	push(list, (token){ .kind = T_LINE, .line = line });
	push(list, (token){ .kind = T_END, .line = line });
	return TRUE;
bad:
	*line_out = line;
	*column_out = (UINT)(at - line_start);
	return FALSE;
}

void vbs_tokens_free(token_list *list)
{
	for (UINT i = 0; i < list->count; i++)
		vbs_free(list->items[i].text);
	vbs_free(list->items);
	*list = (token_list){ 0 };
}
