/* SPDX-License-Identifier: MIT */
#include "vbs.h"

/* The functions and constants VBScript gives every script. Where oleaut32 has the operation (rounding, formatting, month names) it does
 * the work, so results match the variant rules the rest of the engine follows. */

typedef BOOL (*builtin_fn)(builtin_call *c);

static BOOL set_bstr(builtin_call *c, BSTR text)
{
	if (!text)
		return vbs_raise(c->failure, VBS_E_OUT_OF_MEMORY, NULL);
	V_VT(c->result) = VT_BSTR;
	V_BSTR(c->result) = text;
	return TRUE;
}

static BOOL set_text(builtin_call *c, const WCHAR *text, UINT length)
{
	return set_bstr(c, SysAllocStringLen(text, length));
}

static BOOL set_long(builtin_call *c, LONG value)
{
	V_VT(c->result) = VT_I4;
	V_I4(c->result) = value;
	return TRUE;
}

static BOOL set_short(builtin_call *c, SHORT value)
{
	V_VT(c->result) = VT_I2;
	V_I2(c->result) = value;
	return TRUE;
}

static BOOL set_double(builtin_call *c, double value)
{
	V_VT(c->result) = VT_R8;
	V_R8(c->result) = value;
	return TRUE;
}

static BOOL set_bool(builtin_call *c, BOOL value)
{
	V_VT(c->result) = VT_BOOL;
	V_BOOL(c->result) = value ? VARIANT_TRUE : VARIANT_FALSE;
	return TRUE;
}

static BOOL set_date(builtin_call *c, DATE value)
{
	V_VT(c->result) = VT_DATE;
	V_DATE(c->result) = value;
	return TRUE;
}

static BOOL failed(builtin_call *c, HRESULT hr)
{
	switch (hr) {
	case DISP_E_TYPEMISMATCH: return vbs_raise(c->failure, VBS_E_TYPE_MISMATCH, NULL);
	case DISP_E_OVERFLOW: return vbs_raise(c->failure, VBS_E_OVERFLOW, NULL);
	case DISP_E_DIVBYZERO: return vbs_raise(c->failure, VBS_E_DIVISION, NULL);
	case E_OUTOFMEMORY: return vbs_raise(c->failure, VBS_E_OUT_OF_MEMORY, NULL);
	case E_INVALIDARG: return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
	}
	return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
}

static const VARIANT *deref(const VARIANT *v)
{
	while (V_VT(v) == (VT_BYREF | VT_VARIANT))
		v = V_VARIANTREF(v);
	return v;
}

static BOOL given(builtin_call *c, UINT i)
{
	if (i >= c->count)
		return FALSE;
	const VARIANT *v = deref(&c->args[i]);
	return !(V_VT(v) == VT_ERROR && V_ERROR(v) == DISP_E_PARAMNOTFOUND);
}

static BOOL is_null_arg(builtin_call *c, UINT i)
{
	return i < c->count && V_VT(deref(&c->args[i])) == VT_NULL;
}

static BOOL null_result(builtin_call *c)
{
	V_VT(c->result) = VT_NULL;
	return TRUE;
}

static BOOL text_arg(builtin_call *c, UINT i, BSTR *out)
{
	if (is_null_arg(c, i))
		return vbs_raise(c->failure, 94, NULL);
	HRESULT hr = vbs_to_text(&c->args[i], out);
	return SUCCEEDED(hr) || failed(c, hr);
}

static BOOL long_arg(builtin_call *c, UINT i, LONG *out)
{
	if (is_null_arg(c, i))
		return vbs_raise(c->failure, 94, NULL);
	HRESULT hr = vbs_to_long(&c->args[i], out);
	return SUCCEEDED(hr) || failed(c, hr);
}

static BOOL long_or(builtin_call *c, UINT i, LONG fallback, LONG *out)
{
	if (!given(c, i)) {
		*out = fallback;
		return TRUE;
	}
	return long_arg(c, i, out);
}

static BOOL number_arg(builtin_call *c, UINT i, double *out)
{
	if (is_null_arg(c, i))
		return vbs_raise(c->failure, 94, NULL);
	HRESULT hr = vbs_to_number(&c->args[i], out);
	return SUCCEEDED(hr) || failed(c, hr);
}

static BOOL date_arg(builtin_call *c, UINT i, DATE *out)
{
	VARIANT value, date;
	if (is_null_arg(c, i))
		return vbs_raise(c->failure, 94, NULL);
	HRESULT hr = vbs_value_of(&c->args[i], &value);
	if (FAILED(hr))
		return failed(c, hr);
	VariantInit(&date);
	hr = VariantChangeType(&date, &value, 0, VT_DATE);
	VariantClear(&value);
	if (FAILED(hr))
		return failed(c, hr);
	*out = V_DATE(&date);
	return TRUE;
}

/* A plain value of an argument, objects read through their default property. */
static BOOL value_arg(builtin_call *c, UINT i, VARIANT *out)
{
	HRESULT hr = vbs_value_of(&c->args[i], out);
	return SUCCEEDED(hr) || failed(c, hr);
}

/* Strings. */

static BOOL fn_len(builtin_call *c)
{
	BSTR text;
	if (is_null_arg(c, 0))
		return null_result(c);
	if (!text_arg(c, 0, &text))
		return FALSE;
	set_long(c, SysStringLen(text));
	SysFreeString(text);
	return TRUE;
}

static BOOL fn_left_right(builtin_call *c, BOOL right)
{
	BSTR text;
	LONG count;
	if (is_null_arg(c, 0))
		return null_result(c);
	if (!long_arg(c, 1, &count))
		return FALSE;
	if (count < 0)
		return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
	if (!text_arg(c, 0, &text))
		return FALSE;
	UINT length = SysStringLen(text);
	UINT take = (ULONG)count < length ? (UINT)count : length;
	BOOL fine = set_text(c, right ? text + length - take : text, take);
	SysFreeString(text);
	return fine;
}

static BOOL fn_left(builtin_call *c) { return fn_left_right(c, FALSE); }
static BOOL fn_right(builtin_call *c) { return fn_left_right(c, TRUE); }

static BOOL fn_mid(builtin_call *c)
{
	BSTR text;
	LONG start, count;
	if (is_null_arg(c, 0))
		return null_result(c);
	if (!long_arg(c, 1, &start) || !long_or(c, 2, -1, &count))
		return FALSE;
	if (start < 1 || (given(c, 2) && count < 0))
		return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
	if (!text_arg(c, 0, &text))
		return FALSE;
	UINT length = SysStringLen(text);
	UINT from = (ULONG)start - 1 < length ? (UINT)start - 1 : length;
	UINT take = length - from;
	if (count >= 0 && (ULONG)count < take)
		take = (UINT)count;
	BOOL fine = set_text(c, text + from, take);
	SysFreeString(text);
	return fine;
}

static int compare_texts(const WCHAR *a, UINT a_length, const WCHAR *b, UINT b_length, BOOL text_mode)
{
	int order = CompareStringW(LOCALE_USER_DEFAULT, text_mode ? NORM_IGNORECASE : 0, a, a_length, b, b_length);
	if (!text_mode) {
		/* Binary comparison orders by code unit. */
		for (UINT i = 0; i < a_length && i < b_length; i++)
			if (a[i] != b[i])
				return a[i] < b[i] ? -1 : 1;
		return a_length == b_length ? 0 : a_length < b_length ? -1 : 1;
	}
	return order - CSTR_EQUAL;
}

static BOOL matches_at(const WCHAR *text, UINT at, const WCHAR *find, UINT find_length, BOOL text_mode)
{
	return !compare_texts(text + at, find_length, find, find_length, text_mode);
}

/* InStr([start,] text, find[, compare]). */
static BOOL fn_instr(builtin_call *c)
{
	UINT first = 0;
	LONG start = 1, mode = 0;
	if (c->count >= 3) {
		if (is_null_arg(c, 0))
			return vbs_raise(c->failure, 94, NULL);
		if (!long_arg(c, 0, &start))
			return FALSE;
		first = 1;
		if (!long_or(c, 3, 0, &mode))
			return FALSE;
	}
	if (start < 1)
		return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
	if (is_null_arg(c, first) || is_null_arg(c, first + 1))
		return null_result(c);
	BSTR text, find;
	if (!text_arg(c, first, &text))
		return FALSE;
	if (!text_arg(c, first + 1, &find)) {
		SysFreeString(text);
		return FALSE;
	}
	UINT length = SysStringLen(text), find_length = SysStringLen(find);
	LONG found = 0;
	if ((ULONG)start > length) {
		found = 0;
	} else if (!find_length) {
		found = start;
	} else {
		for (UINT at = (UINT)start - 1; at + find_length <= length; at++)
			if (matches_at(text, at, find, find_length, mode == 1)) {
				found = (LONG)at + 1;
				break;
			}
	}
	SysFreeString(text);
	SysFreeString(find);
	return set_long(c, found);
}

/* InStrRev(text, find[, start[, compare]]). */
static BOOL fn_instrrev(builtin_call *c)
{
	LONG start, mode;
	if (is_null_arg(c, 0) || is_null_arg(c, 1))
		return null_result(c);
	if (!long_or(c, 2, -1, &start) || !long_or(c, 3, 0, &mode))
		return FALSE;
	if (start == 0 || start < -1)
		return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
	BSTR text, find;
	if (!text_arg(c, 0, &text))
		return FALSE;
	if (!text_arg(c, 1, &find)) {
		SysFreeString(text);
		return FALSE;
	}
	UINT length = SysStringLen(text), find_length = SysStringLen(find);
	if (start == -1)
		start = (LONG)length;
	LONG found = 0;
	if ((ULONG)start > length) {
		found = 0;
	} else if (!find_length) {
		found = start;
	} else if ((ULONG)start >= find_length) {
		for (UINT at = (UINT)start - find_length + 1; at-- > 0;)
			if (matches_at(text, at, find, find_length, mode == 1)) {
				found = (LONG)at + 1;
				break;
			}
	}
	SysFreeString(text);
	SysFreeString(find);
	return set_long(c, found);
}

static BOOL fn_case(builtin_call *c, BOOL upper)
{
	BSTR text;
	if (is_null_arg(c, 0))
		return null_result(c);
	if (!text_arg(c, 0, &text))
		return FALSE;
	UINT length = SysStringLen(text);
	if (length)
		(upper ? CharUpperBuffW : CharLowerBuffW)(text, length);
	return set_bstr(c, text);
}

static BOOL fn_lcase(builtin_call *c) { return fn_case(c, FALSE); }
static BOOL fn_ucase(builtin_call *c) { return fn_case(c, TRUE); }

static BOOL fn_trim_sides(builtin_call *c, BOOL left, BOOL right)
{
	BSTR text;
	if (is_null_arg(c, 0))
		return null_result(c);
	if (!text_arg(c, 0, &text))
		return FALSE;
	UINT from = 0, to = SysStringLen(text);
	while (left && from < to && text[from] == L' ')
		from++;
	while (right && to > from && text[to - 1] == L' ')
		to--;
	BOOL fine = set_text(c, text + from, to - from);
	SysFreeString(text);
	return fine;
}

static BOOL fn_trim(builtin_call *c) { return fn_trim_sides(c, TRUE, TRUE); }
static BOOL fn_ltrim(builtin_call *c) { return fn_trim_sides(c, TRUE, FALSE); }
static BOOL fn_rtrim(builtin_call *c) { return fn_trim_sides(c, FALSE, TRUE); }

static BOOL fn_space(builtin_call *c)
{
	LONG count;
	if (!long_arg(c, 0, &count))
		return FALSE;
	if (count < 0)
		return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
	BSTR text = SysAllocStringLen(NULL, count);
	if (text)
		for (LONG i = 0; i < count; i++)
			text[i] = L' ';
	return set_bstr(c, text);
}

/* String(count, character): the character as a code or the first of a string. */
static BOOL fn_string(builtin_call *c)
{
	LONG count;
	WCHAR fill;
	if (is_null_arg(c, 1))
		return null_result(c);
	if (!long_arg(c, 0, &count))
		return FALSE;
	if (count < 0)
		return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
	if (V_VT(deref(&c->args[1])) == VT_BSTR) {
		BSTR text = V_BSTR(deref(&c->args[1]));
		if (!SysStringLen(text))
			return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
		fill = text[0];
	} else {
		LONG code;
		if (!long_arg(c, 1, &code))
			return FALSE;
		fill = (WCHAR)(code & 0xff);
	}
	BSTR text = SysAllocStringLen(NULL, count);
	if (text)
		for (LONG i = 0; i < count; i++)
			text[i] = fill;
	return set_bstr(c, text);
}

static BOOL fn_strreverse(builtin_call *c)
{
	BSTR text;
	if (!text_arg(c, 0, &text))
		return FALSE;
	UINT length = SysStringLen(text);
	for (UINT i = 0; i < length / 2; i++) {
		WCHAR t = text[i];
		text[i] = text[length - 1 - i];
		text[length - 1 - i] = t;
	}
	return set_bstr(c, text);
}

static BOOL fn_strcomp(builtin_call *c)
{
	LONG mode;
	if (is_null_arg(c, 0) || is_null_arg(c, 1))
		return null_result(c);
	if (!long_or(c, 2, 0, &mode))
		return FALSE;
	BSTR a, b;
	if (!text_arg(c, 0, &a))
		return FALSE;
	if (!text_arg(c, 1, &b)) {
		SysFreeString(a);
		return FALSE;
	}
	int order = compare_texts(a, SysStringLen(a), b, SysStringLen(b), mode == 1);
	SysFreeString(a);
	SysFreeString(b);
	return set_short(c, order < 0 ? -1 : order > 0 ? 1 : 0);
}

/* Replace(text, find, with[, start[, count[, compare]]]): the result starts at start, as on Windows. */
static BOOL fn_replace(builtin_call *c)
{
	LONG start, count, mode;
	if (is_null_arg(c, 0))
		return null_result(c);
	if (!long_or(c, 3, 1, &start) || !long_or(c, 4, -1, &count) || !long_or(c, 5, 0, &mode))
		return FALSE;
	if (start < 1 || count < -1)
		return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
	BSTR text = NULL, find = NULL, with = NULL;
	if (!text_arg(c, 0, &text) || !text_arg(c, 1, &find) || !text_arg(c, 2, &with)) {
		SysFreeString(text);
		SysFreeString(find);
		return FALSE;
	}
	UINT length = SysStringLen(text), find_length = SysStringLen(find), with_length = SysStringLen(with);
	SIZE_T room = length + 1, used = 0;
	WCHAR *out = vbs_alloc(room * sizeof(WCHAR));
	UINT at = (ULONG)start - 1 < length ? (UINT)start - 1 : length;
	while (at < length) {
		if (find_length && count && at + find_length <= length && matches_at(text, at, find, find_length, mode == 1)) {
			if (used + with_length + 1 > room)
				out = vbs_grow(out, (room = (room + with_length) * 2) * sizeof(WCHAR));
			memcpy(out + used, with, with_length * sizeof(WCHAR));
			used += with_length;
			at += find_length;
			if (count > 0)
				count--;
			continue;
		}
		if (used + 2 > room)
			out = vbs_grow(out, (room *= 2) * sizeof(WCHAR));
		out[used++] = text[at++];
	}
	BOOL fine = set_text(c, out, (UINT)used);
	vbs_free(out);
	SysFreeString(text);
	SysFreeString(find);
	SysFreeString(with);
	return fine;
}

static BOOL fn_asc(builtin_call *c, BOOL wide)
{
	BSTR text;
	if (!text_arg(c, 0, &text))
		return FALSE;
	if (!SysStringLen(text)) {
		SysFreeString(text);
		return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
	}
	WCHAR first = text[0];
	SysFreeString(text);
	if (wide)
		return set_short(c, (SHORT)first);
	/* Asc gives the character's code in the ANSI code page. */
	char bytes[4];
	int n = WideCharToMultiByte(CP_ACP, 0, &first, 1, bytes, sizeof(bytes), NULL, NULL);
	if (n == 2)
		return set_short(c, (SHORT)(((BYTE)bytes[0] << 8) | (BYTE)bytes[1]));
	return set_short(c, n == 1 ? (BYTE)bytes[0] : (SHORT)first);
}

static BOOL fn_ascb(builtin_call *c) { return fn_asc(c, FALSE); }
static BOOL fn_ascw(builtin_call *c) { return fn_asc(c, TRUE); }

static BOOL fn_chr(builtin_call *c, BOOL wide)
{
	LONG code;
	if (!long_arg(c, 0, &code))
		return FALSE;
	WCHAR ch;
	if (wide) {
		if (code < -32768 || code > 65535)
			return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
		ch = (WCHAR)code;
	} else {
		if (code < 0 || code > 65535)
			return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
		char bytes[2] = { (char)(code > 255 ? code >> 8 : code), (char)code };
		if (!MultiByteToWideChar(CP_ACP, 0, bytes, code > 255 ? 2 : 1, &ch, 1))
			ch = (WCHAR)code;
	}
	return set_text(c, &ch, 1);
}

static BOOL fn_chrb(builtin_call *c) { return fn_chr(c, FALSE); }
static BOOL fn_chrw(builtin_call *c) { return fn_chr(c, TRUE); }

/* Arrays. */

static SAFEARRAY *array_arg(builtin_call *c, UINT i)
{
	const VARIANT *v = deref(&c->args[i]);
	if (V_VT(v) == (VT_ARRAY | VT_VARIANT) || (V_VT(v) & VT_ARRAY && !(V_VT(v) & VT_BYREF)))
		return V_ARRAY(v);
	if (V_VT(v) & VT_ARRAY && V_VT(v) & VT_BYREF)
		return *V_ARRAYREF(v);
	return NULL;
}

static SAFEARRAY *vector(LONG count)
{
	SAFEARRAYBOUND bound = { (ULONG)count, 0 };
	return SafeArrayCreate(VT_VARIANT, 1, &bound);
}

static BOOL set_array(builtin_call *c, SAFEARRAY *array)
{
	if (!array)
		return vbs_raise(c->failure, VBS_E_OUT_OF_MEMORY, NULL);
	V_VT(c->result) = VT_ARRAY | VT_VARIANT;
	V_ARRAY(c->result) = array;
	return TRUE;
}

static BOOL fn_array(builtin_call *c)
{
	SAFEARRAY *array = vector(c->count);
	if (!array)
		return vbs_raise(c->failure, VBS_E_OUT_OF_MEMORY, NULL);
	VARIANT *data;
	SafeArrayAccessData(array, (void **)&data);
	for (UINT i = 0; i < c->count; i++)
		VariantCopyInd(&data[i], &c->args[i]);
	SafeArrayUnaccessData(array);
	return set_array(c, array);
}

static BOOL fn_bound(builtin_call *c, BOOL upper)
{
	SAFEARRAY *array = array_arg(c, 0);
	LONG dim, bound;
	if (!array)
		return vbs_raise(c->failure, VBS_E_TYPE_MISMATCH, NULL);
	if (!long_or(c, 1, 1, &dim))
		return FALSE;
	if (dim < 1 || (UINT)dim > SafeArrayGetDim(array))
		return vbs_raise(c->failure, VBS_E_SUBSCRIPT, NULL);
	HRESULT hr = upper ? SafeArrayGetUBound(array, dim, &bound) : SafeArrayGetLBound(array, dim, &bound);
	if (FAILED(hr))
		return vbs_raise(c->failure, VBS_E_SUBSCRIPT, NULL);
	return set_long(c, bound);
}

static BOOL fn_ubound(builtin_call *c) { return fn_bound(c, TRUE); }
static BOOL fn_lbound(builtin_call *c) { return fn_bound(c, FALSE); }

/* Split(text[, delimiter[, count[, compare]]]). */
static BOOL fn_split(builtin_call *c)
{
	LONG limit, mode;
	if (is_null_arg(c, 0))
		return null_result(c);
	if (!long_or(c, 2, -1, &limit) || !long_or(c, 3, 0, &mode))
		return FALSE;
	if (limit < -1)
		return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
	BSTR text, delimiter = NULL;
	if (!text_arg(c, 0, &text))
		return FALSE;
	if (given(c, 1) && !text_arg(c, 1, &delimiter)) {
		SysFreeString(text);
		return FALSE;
	}
	if (!delimiter)
		delimiter = SysAllocString(L" ");
	UINT length = SysStringLen(text), delimiter_length = SysStringLen(delimiter);
	/* First count the pieces, then fill them. */
	BSTR *pieces = NULL;
	UINT piece_count = 0, room = 0;
	if (length && limit != 0) {
		UINT from = 0, at = 0;
		while (at <= length) {
			BOOL last = limit > 0 && piece_count + 1 == (UINT)limit;
			BOOL cut = !last && delimiter_length && at + delimiter_length <= length && matches_at(text, at, delimiter, delimiter_length, mode == 1);
			if (cut || at == length) {
				if (piece_count == room)
					pieces = vbs_grow(pieces, (room = room ? room * 2 : 16) * sizeof(BSTR));
				pieces[piece_count++] = SysAllocStringLen(text + from, (cut ? at : length) - from);
				if (!cut)
					break;
				at += delimiter_length;
				from = at;
				continue;
			}
			at++;
		}
	}
	SAFEARRAY *array = vector(piece_count);
	if (array) {
		VARIANT *data;
		SafeArrayAccessData(array, (void **)&data);
		for (UINT i = 0; i < piece_count; i++) {
			V_VT(&data[i]) = VT_BSTR;
			V_BSTR(&data[i]) = pieces[i];
		}
		SafeArrayUnaccessData(array);
	} else {
		for (UINT i = 0; i < piece_count; i++)
			SysFreeString(pieces[i]);
	}
	vbs_free(pieces);
	SysFreeString(text);
	SysFreeString(delimiter);
	return set_array(c, array);
}

static BOOL fn_join(builtin_call *c)
{
	SAFEARRAY *array = array_arg(c, 0);
	if (!array || SafeArrayGetDim(array) != 1)
		return vbs_raise(c->failure, VBS_E_TYPE_MISMATCH, NULL);
	BSTR delimiter = NULL;
	if (given(c, 1) && !text_arg(c, 1, &delimiter))
		return FALSE;
	if (!delimiter)
		delimiter = SysAllocString(L" ");
	LONG low, high;
	SafeArrayGetLBound(array, 1, &low);
	SafeArrayGetUBound(array, 1, &high);
	VARIANT joined;
	V_VT(&joined) = VT_BSTR;
	V_BSTR(&joined) = SysAllocString(L"");
	VARIANT *data;
	SafeArrayAccessData(array, (void **)&data);
	BOOL fine = TRUE;
	for (LONG i = low; i <= high && fine; i++) {
		BSTR piece;
		HRESULT hr = vbs_to_text(&data[i - low], &piece);
		if (FAILED(hr)) {
			fine = failed(c, hr);
			break;
		}
		BSTR next;
		if (i > low) {
			VarBstrCat(V_BSTR(&joined), delimiter, &next);
			SysFreeString(V_BSTR(&joined));
			V_BSTR(&joined) = next;
		}
		VarBstrCat(V_BSTR(&joined), piece, &next);
		SysFreeString(V_BSTR(&joined));
		V_BSTR(&joined) = next;
		SysFreeString(piece);
	}
	SafeArrayUnaccessData(array);
	SysFreeString(delimiter);
	if (!fine) {
		VariantClear(&joined);
		return FALSE;
	}
	*c->result = joined;
	return TRUE;
}

/* Filter(array, value[, include[, compare]]). */
static BOOL fn_filter(builtin_call *c)
{
	SAFEARRAY *array = array_arg(c, 0);
	if (!array || SafeArrayGetDim(array) != 1)
		return vbs_raise(c->failure, VBS_E_TYPE_MISMATCH, NULL);
	BSTR find;
	BOOL include = TRUE;
	LONG mode;
	if (!text_arg(c, 1, &find))
		return FALSE;
	if (given(c, 2) && FAILED(vbs_to_bool(&c->args[2], &include))) {
		SysFreeString(find);
		return vbs_raise(c->failure, VBS_E_TYPE_MISMATCH, NULL);
	}
	if (!long_or(c, 3, 0, &mode)) {
		SysFreeString(find);
		return FALSE;
	}
	LONG low, high;
	SafeArrayGetLBound(array, 1, &low);
	SafeArrayGetUBound(array, 1, &high);
	BSTR *kept = vbs_alloc((high - low + 2) * sizeof(BSTR));
	UINT kept_count = 0, find_length = SysStringLen(find);
	VARIANT *data;
	SafeArrayAccessData(array, (void **)&data);
	for (LONG i = low; i <= high; i++) {
		BSTR piece;
		if (FAILED(vbs_to_text(&data[i - low], &piece)))
			continue;
		BOOL has = !find_length;
		for (UINT at = 0; !has && at + find_length <= SysStringLen(piece); at++)
			has = matches_at(piece, at, find, find_length, mode == 1);
		if (has == include)
			kept[kept_count++] = piece;
		else
			SysFreeString(piece);
	}
	SafeArrayUnaccessData(array);
	SysFreeString(find);
	SAFEARRAY *out = vector(kept_count);
	if (out) {
		SafeArrayAccessData(out, (void **)&data);
		for (UINT i = 0; i < kept_count; i++) {
			V_VT(&data[i]) = VT_BSTR;
			V_BSTR(&data[i]) = kept[i];
		}
		SafeArrayUnaccessData(out);
	}
	vbs_free(kept);
	return set_array(c, out);
}

/* Conversions. */

static BOOL convert(builtin_call *c, VARTYPE type)
{
	VARIANT value;
	if (is_null_arg(c, 0))
		return vbs_raise(c->failure, 94, NULL);
	if (!value_arg(c, 0, &value))
		return FALSE;
	HRESULT hr;
	if (type == VT_BSTR) {
		BSTR text;
		hr = vbs_to_text(&value, &text);
		if (SUCCEEDED(hr)) {
			V_VT(c->result) = VT_BSTR;
			V_BSTR(c->result) = text;
		}
	} else if (V_VT(&value) == VT_EMPTY && type != VT_BOOL) {
		V_VT(c->result) = type;
		V_I8(c->result) = 0;
		hr = S_OK;
	} else {
		hr = VariantChangeType(c->result, &value, type == VT_BOOL ? VARIANT_ALPHABOOL : 0, type);
	}
	VariantClear(&value);
	return SUCCEEDED(hr) || failed(c, hr);
}

static BOOL fn_cstr(builtin_call *c) { return convert(c, VT_BSTR); }
static BOOL fn_cint(builtin_call *c) { return convert(c, VT_I2); }
static BOOL fn_clng(builtin_call *c) { return convert(c, VT_I4); }
static BOOL fn_cdbl(builtin_call *c) { return convert(c, VT_R8); }
static BOOL fn_csng(builtin_call *c) { return convert(c, VT_R4); }
static BOOL fn_cbool(builtin_call *c) { return convert(c, VT_BOOL); }
static BOOL fn_cbyte(builtin_call *c) { return convert(c, VT_UI1); }
static BOOL fn_ccur(builtin_call *c) { return convert(c, VT_CY); }
static BOOL fn_cdate(builtin_call *c) { return convert(c, VT_DATE); }

static BOOL fn_radix(builtin_call *c, UINT base)
{
	if (is_null_arg(c, 0))
		return null_result(c);
	VARIANT value, number;
	if (!value_arg(c, 0, &value))
		return FALSE;
	VariantInit(&number);
	/* Integers keep their own width: Hex(-1) of an Integer is FFFF, of a Long FFFFFFFF. */
	ULONG bits;
	HRESULT hr = S_OK;
	if (V_VT(&value) == VT_I2 || V_VT(&value) == VT_UI1) {
		bits = V_VT(&value) == VT_I2 ? (USHORT)V_I2(&value) : V_UI1(&value);
	} else {
		hr = VariantChangeType(&number, &value, 0, VT_I4);
		bits = (ULONG)V_I4(&number);
	}
	VariantClear(&value);
	if (FAILED(hr))
		return failed(c, hr);
	WCHAR text[24];
	UINT at = ARRAYSIZE(text) - 1;
	text[at] = 0;
	do {
		text[--at] = L"0123456789ABCDEF"[bits % base];
		bits /= base;
	} while (bits);
	return set_bstr(c, SysAllocString(text + at));
}

static BOOL fn_hex(builtin_call *c) { return fn_radix(c, 16); }
static BOOL fn_oct(builtin_call *c) { return fn_radix(c, 8); }

/* Math. */

static BOOL unary_op(builtin_call *c, HRESULT (WINAPI *op)(LPVARIANT, LPVARIANT))
{
	VARIANT value;
	if (is_null_arg(c, 0))
		return null_result(c);
	if (!value_arg(c, 0, &value))
		return FALSE;
	if (V_VT(&value) == VT_BSTR) {
		VARIANT number;
		VariantInit(&number);
		HRESULT hr = VariantChangeType(&number, &value, 0, VT_R8);
		VariantClear(&value);
		if (FAILED(hr))
			return failed(c, hr);
		value = number;
	}
	HRESULT hr = op(&value, c->result);
	VariantClear(&value);
	return SUCCEEDED(hr) || failed(c, hr);
}

static BOOL fn_int(builtin_call *c) { return unary_op(c, VarInt); }
static BOOL fn_fix(builtin_call *c) { return unary_op(c, VarFix); }
static BOOL fn_abs(builtin_call *c) { return unary_op(c, VarAbs); }

static BOOL fn_round(builtin_call *c)
{
	LONG places;
	VARIANT value;
	if (is_null_arg(c, 0))
		return null_result(c);
	if (!long_or(c, 1, 0, &places) || !value_arg(c, 0, &value))
		return FALSE;
	if (V_VT(&value) == VT_BSTR) {
		VARIANT number;
		VariantInit(&number);
		HRESULT hr = VariantChangeType(&number, &value, 0, VT_R8);
		VariantClear(&value);
		if (FAILED(hr))
			return failed(c, hr);
		value = number;
	}
	HRESULT hr = VarRound(&value, places, c->result);
	VariantClear(&value);
	return SUCCEEDED(hr) || failed(c, hr);
}

static BOOL fn_sgn(builtin_call *c)
{
	double x;
	if (!number_arg(c, 0, &x))
		return FALSE;
	return set_short(c, x > 0 ? 1 : x < 0 ? -1 : 0);
}

/* The C runtime's math functions, looked up in msvcrt.dll: MinGW's static library would bring its own versions, which need the startup
 * code a DLL like this one does not have. */
static double runtime_math(const char *name, double x)
{
	static HMODULE crt;
	if (!crt)
		crt = LoadLibraryW(L"msvcrt.dll");
	double (__cdecl *op)(double) = crt ? (double (__cdecl *)(double))(void *)GetProcAddress(crt, name) : NULL;
	return op ? op(x) : 0;
}

static BOOL math(builtin_call *c, const char *name, BOOL positive, BOOL non_negative)
{
	double x;
	if (!number_arg(c, 0, &x))
		return FALSE;
	if ((positive && x <= 0) || (non_negative && x < 0))
		return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
	return set_double(c, runtime_math(name, x));
}

static BOOL fn_sqr(builtin_call *c) { return math(c, "sqrt", FALSE, TRUE); }
static BOOL fn_log(builtin_call *c) { return math(c, "log", TRUE, FALSE); }
static BOOL fn_exp(builtin_call *c) { return math(c, "exp", FALSE, FALSE); }
static BOOL fn_sin(builtin_call *c) { return math(c, "sin", FALSE, FALSE); }
static BOOL fn_cos(builtin_call *c) { return math(c, "cos", FALSE, FALSE); }
static BOOL fn_tan(builtin_call *c) { return math(c, "tan", FALSE, FALSE); }
static BOOL fn_atn(builtin_call *c) { return math(c, "atan", FALSE, FALSE); }

/* Rnd: a 24-bit linear congruential sequence, so a seed gives the same numbers on every run. */
static BOOL fn_rnd(builtin_call *c)
{
	double x = 1;
	if (given(c, 0) && !number_arg(c, 0, &x))
		return FALSE;
	ULONG *seed = &c->en->random_seed;
	if (x < 0) {
		float f = (float)x;
		ULONG bits;
		memcpy(&bits, &f, sizeof(bits));
		*seed = (bits + (bits >> 24)) & 0xffffff;
	}
	if (x != 0)
		*seed = (*seed * 1140671485UL + 12820163UL) & 0xffffff;
	V_VT(c->result) = VT_R4;
	V_R4(c->result) = (float)*seed / 16777216.0f;
	return TRUE;
}

/* Kinds of values. */

static BOOL fn_isempty(builtin_call *c) { return set_bool(c, V_VT(deref(&c->args[0])) == VT_EMPTY); }
static BOOL fn_isnull(builtin_call *c) { return set_bool(c, V_VT(deref(&c->args[0])) == VT_NULL); }
static BOOL fn_isarray(builtin_call *c) { return set_bool(c, (V_VT(deref(&c->args[0])) & VT_ARRAY) != 0); }

static BOOL fn_isobject(builtin_call *c)
{
	VARTYPE vt = V_VT(deref(&c->args[0]));
	return set_bool(c, vt == VT_DISPATCH || vt == VT_UNKNOWN);
}

static BOOL fn_isnumeric(builtin_call *c)
{
	const VARIANT *v = deref(&c->args[0]);
	switch (V_VT(v)) {
	case VT_EMPTY: case VT_I1: case VT_I2: case VT_I4: case VT_I8: case VT_UI1: case VT_UI2: case VT_UI4: case VT_UI8: case VT_R4:
	case VT_R8: case VT_CY: case VT_DECIMAL: case VT_BOOL: case VT_INT: case VT_UINT:
		return set_bool(c, TRUE);
	case VT_BSTR: {
		VARIANT number;
		VariantInit(&number);
		HRESULT hr = VariantChangeType(&number, (VARIANT *)v, 0, VT_R8);
		return set_bool(c, SUCCEEDED(hr));
	}
	}
	return set_bool(c, FALSE);
}

static BOOL fn_isdate(builtin_call *c)
{
	const VARIANT *v = deref(&c->args[0]);
	if (V_VT(v) == VT_DATE)
		return set_bool(c, TRUE);
	if (V_VT(v) != VT_BSTR)
		return set_bool(c, FALSE);
	DATE date;
	return set_bool(c, SUCCEEDED(VarDateFromStr(V_BSTR(v), LOCALE_USER_DEFAULT, 0, &date)));
}

static BOOL fn_vartype(builtin_call *c)
{
	const VARIANT *v = deref(&c->args[0]);
	VARTYPE vt = V_VT(v) & ~VT_BYREF;
	/* An object with a default value reports that value's kind, as VarType does on Windows. */
	if (vt == VT_DISPATCH && V_DISPATCH(v)) {
		VARIANT value;
		if (SUCCEEDED(vbs_value_of(v, &value))) {
			vt = V_VT(&value);
			VariantClear(&value);
		}
	}
	return set_short(c, (SHORT)vt);
}

static BOOL fn_typename(builtin_call *c)
{
	const VARIANT *v = deref(&c->args[0]);
	static const struct {
		VARTYPE vt;
		const WCHAR *name;
	} names[] = {
		{ VT_EMPTY, L"Empty" }, { VT_NULL, L"Null" }, { VT_I2, L"Integer" }, { VT_I4, L"Long" }, { VT_R4, L"Single" },
		{ VT_R8, L"Double" }, { VT_CY, L"Currency" }, { VT_DATE, L"Date" }, { VT_BSTR, L"String" }, { VT_BOOL, L"Boolean" },
		{ VT_VARIANT, L"Variant" }, { VT_DECIMAL, L"Decimal" }, { VT_UI1, L"Byte" }, { VT_ERROR, L"Error" }, { VT_UNKNOWN, L"Unknown" },
	};
	VARTYPE vt = V_VT(v) & ~VT_BYREF;
	if (vt & VT_ARRAY)
		return set_bstr(c, SysAllocString(L"Variant()"));
	if (vt == VT_DISPATCH) {
		IDispatch *object = V_VT(v) & VT_BYREF ? *V_DISPATCHREF(v) : V_DISPATCH(v);
		if (!object)
			return set_bstr(c, SysAllocString(L"Nothing"));
		/* The class name from the object's type information, when it has any. */
		ITypeInfo *info = NULL;
		BSTR name = NULL;
		if (SUCCEEDED(IDispatch_GetTypeInfo(object, 0, LOCALE_USER_DEFAULT, &info)) && info) {
			ITypeInfo_GetDocumentation(info, MEMBERID_NIL, &name, NULL, NULL, NULL);
			ITypeInfo_Release(info);
		}
		if (!name)
			name = vbs_class_name(object);
		return set_bstr(c, name ? name : SysAllocString(L"Object"));
	}
	for (UINT i = 0; i < ARRAYSIZE(names); i++)
		if (names[i].vt == vt)
			return set_bstr(c, SysAllocString(names[i].name));
	return set_bstr(c, SysAllocString(L"Unknown"));
}

/* Dates and times. */

static BOOL date_parts(builtin_call *c, SYSTEMTIME *out)
{
	DATE date;
	if (!date_arg(c, 0, &date))
		return FALSE;
	UDATE parts;
	HRESULT hr = VarUdateFromDate(date, 0, &parts);
	if (FAILED(hr))
		return failed(c, hr);
	*out = parts.st;
	return TRUE;
}

static BOOL fn_part(builtin_call *c, UINT which)
{
	SYSTEMTIME t;
	if (is_null_arg(c, 0))
		return null_result(c);
	if (!date_parts(c, &t))
		return FALSE;
	WORD values[] = { t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond };
	return set_short(c, (SHORT)values[which]);
}

static BOOL fn_year(builtin_call *c) { return fn_part(c, 0); }
static BOOL fn_month(builtin_call *c) { return fn_part(c, 1); }
static BOOL fn_day(builtin_call *c) { return fn_part(c, 2); }
static BOOL fn_hour(builtin_call *c) { return fn_part(c, 3); }
static BOOL fn_minute(builtin_call *c) { return fn_part(c, 4); }
static BOOL fn_second(builtin_call *c) { return fn_part(c, 5); }

static BOOL fn_weekday(builtin_call *c)
{
	SYSTEMTIME t;
	LONG first;
	if (is_null_arg(c, 0))
		return null_result(c);
	if (!long_or(c, 1, 1, &first) || !date_parts(c, &t))
		return FALSE;
	if (first == 0)
		first = 1;
	if (first < 1 || first > 7)
		return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
	return set_short(c, (SHORT)((t.wDayOfWeek + 1 - first + 7) % 7 + 1));
}

static double floor_of(double x)
{
	double whole = (double)(LONGLONG)x;
	return whole > x ? whole - 1 : whole;
}

static double whole_part(double x) { return (double)(LONGLONG)x; }
static double magnitude(double x) { return x < 0 ? -x : x; }

static BOOL now_parts(DATE *out)
{
	SYSTEMTIME t;
	GetLocalTime(&t);
	t.wMilliseconds = 0;
	return SystemTimeToVariantTime(&t, out);
}

static BOOL fn_now(builtin_call *c)
{
	DATE d = 0;
	now_parts(&d);
	return set_date(c, d);
}

static BOOL fn_date(builtin_call *c)
{
	DATE d = 0;
	now_parts(&d);
	return set_date(c, floor_of(d));
}

static BOOL fn_time(builtin_call *c)
{
	DATE d = 0;
	now_parts(&d);
	return set_date(c, d - floor_of(d));
}

static BOOL fn_timer(builtin_call *c)
{
	SYSTEMTIME t;
	GetLocalTime(&t);
	V_VT(c->result) = VT_R4;
	V_R4(c->result) = (float)(t.wHour * 3600 + t.wMinute * 60 + t.wSecond + t.wMilliseconds / 1000.0);
	return TRUE;
}

/* A day number from a year, month and day, each of which may run over: month 13 is January of the next year. */
static BOOL serial_date(builtin_call *c, LONG year, LONG month, LONG day, DATE *out)
{
	if (year >= 0 && year < 30)
		year += 2000;
	else if (year >= 30 && year < 100)
		year += 1900;
	LONG months = year * 12 + (month - 1);
	UDATE parts = { 0 };
	parts.st.wYear = (WORD)(months / 12);
	parts.st.wMonth = (WORD)(months % 12 + 1);
	parts.st.wDay = 1;
	if (months < 0 || parts.st.wYear < 100 || parts.st.wYear > 9999)
		return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
	DATE first;
	HRESULT hr = VarDateFromUdate(&parts, 0, &first);
	if (FAILED(hr))
		return failed(c, hr);
	*out = first + (day - 1);
	return TRUE;
}

static BOOL fn_dateserial(builtin_call *c)
{
	LONG y, m, d;
	DATE date;
	if (!long_arg(c, 0, &y) || !long_arg(c, 1, &m) || !long_arg(c, 2, &d) || !serial_date(c, y, m, d, &date))
		return FALSE;
	return set_date(c, date);
}

static BOOL fn_timeserial(builtin_call *c)
{
	LONG h, m, s;
	if (!long_arg(c, 0, &h) || !long_arg(c, 1, &m) || !long_arg(c, 2, &s))
		return FALSE;
	return set_date(c, (h * 3600.0 + m * 60.0 + s) / 86400.0);
}

static BOOL fn_datevalue(builtin_call *c)
{
	DATE d;
	if (!date_arg(c, 0, &d))
		return FALSE;
	return set_date(c, whole_part(d));
}

static BOOL fn_timevalue(builtin_call *c)
{
	DATE d;
	if (!date_arg(c, 0, &d))
		return FALSE;
	return set_date(c, magnitude(d - whole_part(d)));
}

/* The intervals DateAdd and DateDiff take, as seconds per unit, or 0 for months and years. */
static BOOL interval(builtin_call *c, LONG *months, double *seconds)
{
	BSTR name;
	if (!text_arg(c, 0, &name))
		return FALSE;
	static const struct {
		const WCHAR *name;
		LONG months;
		double seconds;
	} units[] = {
		{ L"yyyy", 12, 0 }, { L"q", 3, 0 }, { L"m", 1, 0 }, { L"y", 0, 86400 }, { L"d", 0, 86400 }, { L"w", 0, 86400 },
		{ L"ww", 0, 7 * 86400 }, { L"h", 0, 3600 }, { L"n", 0, 60 }, { L"s", 0, 1 },
	};
	for (UINT i = 0; i < ARRAYSIZE(units); i++)
		if (vbs_same(name, units[i].name)) {
			*months = units[i].months;
			*seconds = units[i].seconds;
			SysFreeString(name);
			return TRUE;
		}
	SysFreeString(name);
	return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
}

static BOOL fn_dateadd(builtin_call *c)
{
	LONG months;
	double seconds, count;
	DATE date;
	if (!interval(c, &months, &seconds) || !number_arg(c, 1, &count) || !date_arg(c, 2, &date))
		return FALSE;
	count = whole_part(count);
	if (!months)
		return set_date(c, date + count * seconds / 86400.0);
	SYSTEMTIME t;
	UDATE parts;
	HRESULT hr = VarUdateFromDate(date, 0, &parts);
	if (FAILED(hr))
		return failed(c, hr);
	t = parts.st;
	/* The day stays, cut to the last day of the month it lands in. */
	LONG total = t.wYear * 12 + (t.wMonth - 1) + (LONG)count * months;
	DATE first, next;
	if (!serial_date(c, total / 12, total % 12 + 1, 1, &first) || !serial_date(c, total / 12, total % 12 + 2, 1, &next))
		return FALSE;
	LONG last_day = (LONG)(next - first);
	double time = magnitude(date - whole_part(date));
	return set_date(c, first + ((LONG)t.wDay < last_day ? t.wDay : last_day) - 1 + time);
}

static BOOL fn_datediff(builtin_call *c)
{
	LONG months;
	double seconds;
	DATE a, b;
	if (!interval(c, &months, &seconds) || !date_arg(c, 1, &a) || !date_arg(c, 2, &b))
		return FALSE;
	if (months) {
		UDATE pa, pb;
		VarUdateFromDate(a, 0, &pa);
		VarUdateFromDate(b, 0, &pb);
		LONG span = (pb.st.wYear * 12 + pb.st.wMonth) - (pa.st.wYear * 12 + pa.st.wMonth);
		if (months == 12)
			span = pb.st.wYear - pa.st.wYear;
		else if (months == 3)
			span = (pb.st.wYear * 4 + (pb.st.wMonth - 1) / 3) - (pa.st.wYear * 4 + (pa.st.wMonth - 1) / 3);
		return set_long(c, span);
	}
	/* Whole units crossed: days count midnights, hours count hours struck, and so on. */
	double from = floor_of(a * 86400.0 / seconds + 1e-9), to = floor_of(b * 86400.0 / seconds + 1e-9);
	return set_long(c, (LONG)(to - from));
}

static BOOL fn_monthname(builtin_call *c)
{
	LONG month;
	BOOL brief = FALSE;
	if (!long_arg(c, 0, &month))
		return FALSE;
	if (given(c, 1) && FAILED(vbs_to_bool(&c->args[1], &brief)))
		return vbs_raise(c->failure, VBS_E_TYPE_MISMATCH, NULL);
	BSTR name = NULL;
	HRESULT hr = VarMonthName(month, brief, 0, &name);
	return SUCCEEDED(hr) ? set_bstr(c, name) : failed(c, hr);
}

static BOOL fn_weekdayname(builtin_call *c)
{
	LONG day, first;
	BOOL brief = FALSE;
	if (!long_arg(c, 0, &day) || !long_or(c, 2, 1, &first))
		return FALSE;
	if (given(c, 1) && FAILED(vbs_to_bool(&c->args[1], &brief)))
		return vbs_raise(c->failure, VBS_E_TYPE_MISMATCH, NULL);
	BSTR name = NULL;
	HRESULT hr = VarWeekdayName(day, brief, first, 0, &name);
	return SUCCEEDED(hr) ? set_bstr(c, name) : failed(c, hr);
}

/* Formatting, which oleaut32 does. */

static BOOL format_number(builtin_call *c, UINT kind)
{
	LONG digits, leading, parens, groups;
	VARIANT value;
	if (!long_or(c, 1, -1, &digits) || !long_or(c, 2, -2, &leading) || !long_or(c, 3, -2, &parens) || !long_or(c, 4, -2, &groups))
		return FALSE;
	if (!value_arg(c, 0, &value))
		return FALSE;
	BSTR text = NULL;
	HRESULT hr = kind == 0 ? VarFormatNumber(&value, digits, leading, parens, groups, 0, &text) :
		kind == 1 ? VarFormatCurrency(&value, digits, leading, parens, groups, 0, &text) :
		VarFormatPercent(&value, digits, leading, parens, groups, 0, &text);
	VariantClear(&value);
	return SUCCEEDED(hr) ? set_bstr(c, text) : failed(c, hr);
}

static BOOL fn_formatnumber(builtin_call *c) { return format_number(c, 0); }
static BOOL fn_formatcurrency(builtin_call *c) { return format_number(c, 1); }
static BOOL fn_formatpercent(builtin_call *c) { return format_number(c, 2); }

static BOOL fn_formatdatetime(builtin_call *c)
{
	LONG style;
	VARIANT value;
	if (!long_or(c, 1, 0, &style) || !value_arg(c, 0, &value))
		return FALSE;
	BSTR text = NULL;
	HRESULT hr = VarFormatDateTime(&value, style, 0, &text);
	VariantClear(&value);
	return SUCCEEDED(hr) ? set_bstr(c, text) : failed(c, hr);
}

/* Objects and the engine. */

static BOOL fn_createobject(builtin_call *c)
{
	BSTR name;
	if (!text_arg(c, 0, &name))
		return FALSE;
	IDispatch *object = NULL;
	HRESULT hr = vbs_create_object(c->en, name, &object);
	SysFreeString(name);
	if (FAILED(hr))
		return vbs_raise(c->failure, VBS_E_CANNOT_CREATE, NULL);
	V_VT(c->result) = VT_DISPATCH;
	V_DISPATCH(c->result) = object;
	return TRUE;
}

static BOOL fn_getobject(builtin_call *c)
{
	return vbs_raise(c->failure, VBS_E_CANNOT_CREATE, NULL);
}

static BOOL fn_getref(builtin_call *c)
{
	BSTR name;
	if (!text_arg(c, 0, &name))
		return FALSE;
	IDispatch *ref = vbs_function_ref(c->en, name);
	SysFreeString(name);
	if (!ref)
		return vbs_raise(c->failure, VBS_E_ARGUMENT, NULL);
	V_VT(c->result) = VT_DISPATCH;
	V_DISPATCH(c->result) = ref;
	return TRUE;
}

static BOOL fn_err(builtin_call *c)
{
	V_VT(c->result) = VT_DISPATCH;
	V_DISPATCH(c->result) = vbs_err_object(c->en);
	return TRUE;
}

static BOOL run_text(builtin_call *c, BOOL expression)
{
	BSTR source;
	if (!text_arg(c, 0, &source))
		return FALSE;
	BOOL fine = vbs_execute_text(c->en, source, expression, c->result, c->failure);
	SysFreeString(source);
	return fine;
}

static BOOL fn_eval(builtin_call *c) { return run_text(c, TRUE); }
static BOOL fn_execute(builtin_call *c) { return run_text(c, FALSE); }

static BOOL fn_scriptengine(builtin_call *c) { return set_bstr(c, SysAllocString(L"VBScript")); }
static BOOL fn_major(builtin_call *c) { return set_long(c, 5); }
static BOOL fn_minor(builtin_call *c) { return set_long(c, 8); }
static BOOL fn_build(builtin_call *c) { return set_long(c, 16384); }

static BOOL fn_rgb(builtin_call *c)
{
	LONG r, g, b;
	if (!long_arg(c, 0, &r) || !long_arg(c, 1, &g) || !long_arg(c, 2, &b))
		return FALSE;
	r = r < 0 ? 0 : r > 255 ? 255 : r;
	g = g < 0 ? 0 : g > 255 ? 255 : g;
	b = b < 0 ? 0 : b > 255 ? 255 : b;
	return set_long(c, r | (g << 8) | (b << 16));
}

static BOOL fn_msgbox(builtin_call *c)
{
	BSTR text, title = NULL;
	LONG buttons;
	if (!text_arg(c, 0, &text) || !long_or(c, 1, 0, &buttons))
		return FALSE;
	if (given(c, 2) && !text_arg(c, 2, &title)) {
		SysFreeString(text);
		return FALSE;
	}
	HWND owner = NULL;
	if (c->en->site) {
		IActiveScriptSiteWindow *window = NULL;
		if (SUCCEEDED(IActiveScriptSite_QueryInterface(c->en->site, &IID_IActiveScriptSiteWindow, (void **)&window)) && window) {
			IActiveScriptSiteWindow_GetWindow(window, &owner);
			IActiveScriptSiteWindow_Release(window);
		}
	}
	int answer = MessageBoxW(owner, text, title ? title : L"VBScript", (UINT)buttons);
	SysFreeString(text);
	SysFreeString(title);
	return set_long(c, answer);
}

/* Constants that read as functions with no arguments. */
static BOOL constant_text(builtin_call *c, const WCHAR *text, UINT length) { return set_text(c, text, length); }
static BOOL fn_vbcrlf(builtin_call *c) { return constant_text(c, L"\r\n", 2); }
static BOOL fn_vbcr(builtin_call *c) { return constant_text(c, L"\r", 1); }
static BOOL fn_vblf(builtin_call *c) { return constant_text(c, L"\n", 1); }
static BOOL fn_vbtab(builtin_call *c) { return constant_text(c, L"\t", 1); }
static BOOL fn_vbnullchar(builtin_call *c) { return constant_text(c, L"\0", 1); }
static BOOL fn_vbformfeed(builtin_call *c) { return constant_text(c, L"\f", 1); }
static BOOL fn_vbverticaltab(builtin_call *c) { return constant_text(c, L"\v", 1); }

static BOOL fn_vbnullstring(builtin_call *c)
{
	V_VT(c->result) = VT_BSTR;
	V_BSTR(c->result) = NULL;
	return TRUE;
}

typedef struct {
	const WCHAR *name;
	LONG value;
} named_constant;

static const named_constant constants[] = {
	{ L"vbBinaryCompare", 0 }, { L"vbTextCompare", 1 }, { L"vbDatabaseCompare", 2 },
	{ L"vbUseSystemDayOfWeek", 0 }, { L"vbSunday", 1 }, { L"vbMonday", 2 }, { L"vbTuesday", 3 }, { L"vbWednesday", 4 },
	{ L"vbThursday", 5 }, { L"vbFriday", 6 }, { L"vbSaturday", 7 },
	{ L"vbUseSystem", 0 }, { L"vbFirstJan1", 1 }, { L"vbFirstFourDays", 2 }, { L"vbFirstFullWeek", 3 },
	{ L"vbGeneralDate", 0 }, { L"vbLongDate", 1 }, { L"vbShortDate", 2 }, { L"vbLongTime", 3 }, { L"vbShortTime", 4 },
	{ L"vbEmpty", 0 }, { L"vbNull", 1 }, { L"vbInteger", 2 }, { L"vbLong", 3 }, { L"vbSingle", 4 }, { L"vbDouble", 5 },
	{ L"vbCurrency", 6 }, { L"vbDate", 7 }, { L"vbString", 8 }, { L"vbObject", 9 }, { L"vbError", 10 }, { L"vbBoolean", 11 },
	{ L"vbVariant", 12 }, { L"vbDataObject", 13 }, { L"vbDecimal", 14 }, { L"vbByte", 17 }, { L"vbArray", 8192 },
	{ L"vbOKOnly", 0 }, { L"vbOKCancel", 1 }, { L"vbAbortRetryIgnore", 2 }, { L"vbYesNoCancel", 3 }, { L"vbYesNo", 4 },
	{ L"vbRetryCancel", 5 }, { L"vbCritical", 16 }, { L"vbQuestion", 32 }, { L"vbExclamation", 48 }, { L"vbInformation", 64 },
	{ L"vbDefaultButton1", 0 }, { L"vbDefaultButton2", 256 }, { L"vbDefaultButton3", 512 }, { L"vbDefaultButton4", 768 },
	{ L"vbApplicationModal", 0 }, { L"vbSystemModal", 4096 },
	{ L"vbOK", 1 }, { L"vbCancel", 2 }, { L"vbAbort", 3 }, { L"vbRetry", 4 }, { L"vbIgnore", 5 }, { L"vbYes", 6 }, { L"vbNo", 7 },
	{ L"vbUseDefault", -2 }, { L"vbTrue", -1 }, { L"vbFalse", 0 }, { L"vbObjectError", (LONG)0x80040000 },
	{ L"vbBlack", 0 }, { L"vbRed", 0xff }, { L"vbGreen", 0xff00 }, { L"vbYellow", 0xffff }, { L"vbBlue", 0xff0000 },
	{ L"vbMagenta", 0xff00ff }, { L"vbCyan", 0xffff00 }, { L"vbWhite", 0xffffff },
};

typedef struct {
	const WCHAR *name;
	UINT min, max;
	builtin_fn fn;
} builtin;

#define ANY 0xffff

static const builtin builtins[] = {
	{ L"Len", 1, 1, fn_len }, { L"Left", 2, 2, fn_left }, { L"Right", 2, 2, fn_right }, { L"Mid", 2, 3, fn_mid },
	{ L"InStr", 2, 4, fn_instr }, { L"InStrRev", 2, 4, fn_instrrev }, { L"LCase", 1, 1, fn_lcase }, { L"UCase", 1, 1, fn_ucase },
	{ L"Trim", 1, 1, fn_trim }, { L"LTrim", 1, 1, fn_ltrim }, { L"RTrim", 1, 1, fn_rtrim }, { L"Space", 1, 1, fn_space },
	{ L"String", 2, 2, fn_string }, { L"StrReverse", 1, 1, fn_strreverse }, { L"StrComp", 2, 3, fn_strcomp },
	{ L"Replace", 3, 6, fn_replace }, { L"Asc", 1, 1, fn_ascb }, { L"AscB", 1, 1, fn_ascb }, { L"AscW", 1, 1, fn_ascw },
	{ L"Chr", 1, 1, fn_chrb }, { L"ChrB", 1, 1, fn_chrb }, { L"ChrW", 1, 1, fn_chrw },
	{ L"Array", 0, ANY, fn_array }, { L"UBound", 1, 2, fn_ubound }, { L"LBound", 1, 2, fn_lbound }, { L"Split", 1, 4, fn_split },
	{ L"Join", 1, 2, fn_join }, { L"Filter", 2, 4, fn_filter },
	{ L"CStr", 1, 1, fn_cstr }, { L"CInt", 1, 1, fn_cint }, { L"CLng", 1, 1, fn_clng }, { L"CDbl", 1, 1, fn_cdbl },
	{ L"CSng", 1, 1, fn_csng }, { L"CBool", 1, 1, fn_cbool }, { L"CByte", 1, 1, fn_cbyte }, { L"CCur", 1, 1, fn_ccur },
	{ L"CDate", 1, 1, fn_cdate }, { L"Hex", 1, 1, fn_hex }, { L"Oct", 1, 1, fn_oct },
	{ L"Int", 1, 1, fn_int }, { L"Fix", 1, 1, fn_fix }, { L"Abs", 1, 1, fn_abs }, { L"Round", 1, 2, fn_round }, { L"Sgn", 1, 1, fn_sgn },
	{ L"Sqr", 1, 1, fn_sqr }, { L"Log", 1, 1, fn_log }, { L"Exp", 1, 1, fn_exp }, { L"Sin", 1, 1, fn_sin }, { L"Cos", 1, 1, fn_cos },
	{ L"Tan", 1, 1, fn_tan }, { L"Atn", 1, 1, fn_atn }, { L"Rnd", 0, 1, fn_rnd },
	{ L"IsEmpty", 1, 1, fn_isempty }, { L"IsNull", 1, 1, fn_isnull }, { L"IsArray", 1, 1, fn_isarray },
	{ L"IsObject", 1, 1, fn_isobject }, { L"IsNumeric", 1, 1, fn_isnumeric }, { L"IsDate", 1, 1, fn_isdate },
	{ L"VarType", 1, 1, fn_vartype }, { L"TypeName", 1, 1, fn_typename },
	{ L"Year", 1, 1, fn_year }, { L"Month", 1, 1, fn_month }, { L"Day", 1, 1, fn_day }, { L"Hour", 1, 1, fn_hour },
	{ L"Minute", 1, 1, fn_minute }, { L"Second", 1, 1, fn_second }, { L"Weekday", 1, 2, fn_weekday }, { L"Now", 0, 0, fn_now },
	{ L"Date", 0, 0, fn_date }, { L"Time", 0, 0, fn_time }, { L"Timer", 0, 0, fn_timer }, { L"DateSerial", 3, 3, fn_dateserial },
	{ L"TimeSerial", 3, 3, fn_timeserial }, { L"DateValue", 1, 1, fn_datevalue }, { L"TimeValue", 1, 1, fn_timevalue },
	{ L"DateAdd", 3, 3, fn_dateadd }, { L"DateDiff", 3, 5, fn_datediff }, { L"MonthName", 1, 2, fn_monthname },
	{ L"WeekdayName", 1, 3, fn_weekdayname },
	{ L"FormatNumber", 1, 5, fn_formatnumber }, { L"FormatCurrency", 1, 5, fn_formatcurrency },
	{ L"FormatPercent", 1, 5, fn_formatpercent }, { L"FormatDateTime", 1, 2, fn_formatdatetime },
	{ L"CreateObject", 1, 2, fn_createobject }, { L"GetObject", 0, 2, fn_getobject }, { L"GetRef", 1, 1, fn_getref }, { L"Err", 0, 0, fn_err },
	{ L"Eval", 1, 1, fn_eval }, { L"Execute", 1, 1, fn_execute }, { L"ExecuteGlobal", 1, 1, fn_execute },
	{ L"ScriptEngine", 0, 0, fn_scriptengine }, { L"ScriptEngineMajorVersion", 0, 0, fn_major },
	{ L"ScriptEngineMinorVersion", 0, 0, fn_minor }, { L"ScriptEngineBuildVersion", 0, 0, fn_build }, { L"RGB", 3, 3, fn_rgb },
	{ L"MsgBox", 1, 5, fn_msgbox },
	{ L"vbCrLf", 0, 0, fn_vbcrlf }, { L"vbNewLine", 0, 0, fn_vbcrlf }, { L"vbCr", 0, 0, fn_vbcr }, { L"vbLf", 0, 0, fn_vblf },
	{ L"vbTab", 0, 0, fn_vbtab }, { L"vbNullChar", 0, 0, fn_vbnullchar }, { L"vbNullString", 0, 0, fn_vbnullstring },
	{ L"vbFormFeed", 0, 0, fn_vbformfeed }, { L"vbVerticalTab", 0, 0, fn_vbverticaltab },
};

BOOL vbs_builtin(const WCHAR *name, builtin_call *call, BOOL *found)
{
	*found = FALSE;
	for (UINT i = 0; i < ARRAYSIZE(constants); i++)
		if (vbs_same(constants[i].name, name)) {
			*found = TRUE;
			if (call->count)
				return vbs_raise(call->failure, VBS_E_TYPE_MISMATCH, NULL);
			/* The constants are Long, except the small ones VBScript keeps as Integer. */
			if (constants[i].value >= -32768 && constants[i].value <= 32767)
				return set_short(call, (SHORT)constants[i].value);
			return set_long(call, constants[i].value);
		}
	for (UINT i = 0; i < ARRAYSIZE(builtins); i++)
		if (vbs_same(builtins[i].name, name)) {
			*found = TRUE;
			/* A missing argument in the middle still counts; only the trailing ones may be left out. */
			if (call->count < builtins[i].min || (builtins[i].max != ANY && call->count > builtins[i].max))
				return vbs_raise(call->failure, VBS_E_WRONG_ARGUMENTS, NULL);
			for (UINT a = 0; a < builtins[i].min; a++)
				if (!given(call, a))
					return vbs_raise(call->failure, VBS_E_WRONG_ARGUMENTS, NULL);
			return builtins[i].fn(call);
		}
	return FALSE;
}
