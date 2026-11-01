/* SPDX-License-Identifier: MIT */
#include "msi_own.h"

/* The condition language of the Windows Installer, as its documentation gives it: NOT binds tightest, then AND, OR, XOR,
 * EQV and IMP. A value on its own is true when it is not empty. Two values that read as integers compare as numbers, quoted
 * or not, so ALLUSERS="1" holds; an integer and any other string are only ever different. */

typedef struct {
	msi_package *package;
	const WCHAR *at;
	BOOL broken;
} parser;

typedef struct {
	WCHAR *text;
	BOOL number;
	INT value;
} operand;

static void skip_space(parser *p)
{
	while (*p->at == ' ' || *p->at == '\t' || *p->at == '\r' || *p->at == '\n')
		p->at++;
}

static BOOL word_char(WCHAR c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
}

static BOOL keyword(parser *p, const WCHAR *word)
{
	skip_space(p);
	UINT length = lstrlenW(word);
	if (CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, p->at, length, word, length) != CSTR_EQUAL || word_char(p->at[length]))
		return FALSE;
	p->at += length;
	return TRUE;
}

static BOOL as_number(const WCHAR *text, INT *value)
{
	const WCHAR *at = text;
	BOOL negative = FALSE;
	INT64 total = 0;
	if (*at == '-') {
		negative = TRUE;
		at++;
	}
	if (!*at)
		return FALSE;
	for (; *at; at++) {
		if (*at < '0' || *at > '9')
			return FALSE;
		total = total * 10 + (*at - '0');
		if (total > 0x7fffffff)
			return FALSE;
	}
	*value = negative ? (INT)-total : (INT)total;
	return TRUE;
}

static WCHAR *number_text(INT value)
{
	WCHAR text[16];
	wsprintfW(text, L"%d", value);
	return msi_copy(text);
}

static WCHAR *state_of(parser *p, WCHAR sigil, const WCHAR *key, BOOL *known)
{
	msi_package *package = p->package;
	INT state = INSTALLSTATE_UNKNOWN;
	*known = TRUE;
	if (package->remote) {
		INT result = INSTALLSTATE_UNKNOWN;
		BOOL component = sigil == '$' || sigil == '?';
		remote_call(package->remote, component ? REMOTE_COMPONENT_STATE : REMOTE_FEATURE_STATE, key, NULL, sigil == '$' || sigil == '&', NULL, &result);
		state = result;
	} else {
		tables_load(package);
		if (sigil == '$' || sigil == '?') {
			msi_component *found = component_find(package, key);
			if (found)
				state = sigil == '$' ? found->action : found->installed;
		} else {
			msi_feature *found = feature_find(package, key);
			if (found)
				state = sigil == '&' ? found->action : found->installed;
		}
	}
	return number_text(state);
}

static operand parse_value(parser *p)
{
	operand value = {0};
	skip_space(p);
	WCHAR c = *p->at;
	if (c == '"') {
		const WCHAR *start = ++p->at;
		while (*p->at && *p->at != '"')
			p->at++;
		if (*p->at != '"') {
			p->broken = TRUE;
			return value;
		}
		value.text = msi_copy_n(start, p->at - start);
		value.number = as_number(value.text, &value.value);
		p->at++;
		return value;
	}
	WCHAR sigil = 0;
	if (c == '%' || c == '$' || c == '?' || c == '&' || c == '!') {
		sigil = c;
		p->at++;
	}
	const WCHAR *start = p->at;
	if (!sigil && *p->at == '-')
		p->at++;
	while (word_char(*p->at))
		p->at++;
	if (p->at == start) {
		p->broken = TRUE;
		return value;
	}
	WCHAR *name = msi_copy_n(start, p->at - start);
	if (sigil == '%') {
		WCHAR buffer[4096];
		DWORD got = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
		value.text = msi_copy(got && got < ARRAYSIZE(buffer) ? buffer : L"");
	} else if (sigil) {
		BOOL known;
		value.text = state_of(p, sigil, name, &known);
		as_number(value.text, &value.value);
		value.number = TRUE;
	} else if (as_number(name, &value.value)) {
		value.number = TRUE;
		value.text = msi_copy(name);
	} else {
		value.text = package_get(p->package, name);
		if (!value.text)
			value.text = msi_copy(L"");
		value.number = as_number(value.text, &value.value);
	}
	msi_free(name);
	return value;
}

static void clear(operand *value)
{
	msi_free(value->text);
	value->text = NULL;
}

static BOOL take(parser *p, const WCHAR *symbol)
{
	UINT length = lstrlenW(symbol);
	skip_space(p);
	for (UINT i = 0; i < length; i++)
		if (p->at[i] != symbol[i])
			return FALSE;
	p->at += length;
	return TRUE;
}

static BOOL contains(const WCHAR *text, const WCHAR *part, BOOL caseless)
{
	INT size = lstrlenW(text), part_size = lstrlenW(part);
	for (INT i = 0; i + part_size <= size; i++)
		if (CompareStringW(LOCALE_INVARIANT, caseless ? NORM_IGNORECASE : 0, text + i, part_size, part, part_size) == CSTR_EQUAL)
			return TRUE;
	return FALSE;
}

static BOOL edge(const WCHAR *text, const WCHAR *part, BOOL caseless, BOOL at_end)
{
	INT size = lstrlenW(text), part_size = lstrlenW(part);
	if (part_size > size)
		return FALSE;
	return CompareStringW(LOCALE_INVARIANT, caseless ? NORM_IGNORECASE : 0, text + (at_end ? size - part_size : 0), part_size, part, part_size) == CSTR_EQUAL;
}

static BOOL parse_or_chain(parser *p);

static BOOL parse_comparison(parser *p)
{
	skip_space(p);
	if (*p->at == '(') {
		p->at++;
		BOOL inner = parse_or_chain(p);
		if (!take(p, L")"))
			p->broken = TRUE;
		return inner;
	}
	operand left = parse_value(p), right = {0};
	if (p->broken)
		return FALSE;
	BOOL caseless = take(p, L"~");
	enum { CMP_NONE, CMP_EQ, CMP_NE, CMP_LT, CMP_LE, CMP_GT, CMP_GE, CMP_CONTAINS, CMP_START, CMP_END } op = CMP_NONE;
	if (take(p, L"<>")) op = CMP_NE;
	else if (take(p, L"><")) op = CMP_CONTAINS;
	else if (take(p, L"<<")) op = CMP_START;
	else if (take(p, L">>")) op = CMP_END;
	else if (take(p, L"<=")) op = CMP_LE;
	else if (take(p, L">=")) op = CMP_GE;
	else if (take(p, L"=")) op = CMP_EQ;
	else if (take(p, L"<")) op = CMP_LT;
	else if (take(p, L">")) op = CMP_GT;
	BOOL result;
	if (op == CMP_NONE) {
		if (caseless)
			p->broken = TRUE;
		result = left.number ? left.value != 0 : left.text[0] != 0;
		clear(&left);
		return result;
	}
	right = parse_value(p);
	if (p->broken) {
		clear(&left);
		return FALSE;
	}
	if (op == CMP_CONTAINS)
		result = contains(left.text, right.text, caseless);
	else if (op == CMP_START)
		result = edge(left.text, right.text, caseless, FALSE);
	else if (op == CMP_END)
		result = edge(left.text, right.text, caseless, TRUE);
	else {
		int order;
		if (left.number && right.number)
			order = left.value < right.value ? -1 : left.value > right.value;
		else if (left.number != right.number && left.text[0] && right.text[0]) {
			result = op == CMP_NE;
			clear(&left);
			clear(&right);
			return result;
		} else
			order = CompareStringW(LOCALE_INVARIANT, caseless ? NORM_IGNORECASE : 0, left.text, -1, right.text, -1) - CSTR_EQUAL;
		result = op == CMP_EQ ? order == 0 : op == CMP_NE ? order != 0 : op == CMP_LT ? order < 0 : op == CMP_LE ? order <= 0 : op == CMP_GT ? order > 0 : order >= 0;
	}
	clear(&left);
	clear(&right);
	return result;
}

static BOOL parse_not(parser *p)
{
	if (keyword(p, L"NOT"))
		return !parse_not(p);
	return parse_comparison(p);
}

static BOOL parse_and(parser *p)
{
	BOOL value = parse_not(p);
	while (keyword(p, L"AND")) {
		BOOL next = parse_not(p);
		value = value && next;
	}
	return value;
}

static BOOL parse_or(parser *p)
{
	BOOL value = parse_and(p);
	while (keyword(p, L"OR")) {
		BOOL next = parse_and(p);
		value = value || next;
	}
	return value;
}

static BOOL parse_xor(parser *p)
{
	BOOL value = parse_or(p);
	while (keyword(p, L"XOR"))
		value = value != parse_or(p);
	return value;
}

static BOOL parse_eqv(parser *p)
{
	BOOL value = parse_xor(p);
	while (keyword(p, L"EQV"))
		value = value == parse_xor(p);
	return value;
}

static BOOL parse_or_chain(parser *p)
{
	BOOL value = parse_eqv(p);
	while (keyword(p, L"IMP")) {
		BOOL next = parse_eqv(p);
		value = !value || next;
	}
	return value;
}

MSICONDITION condition_evaluate(msi_package *package, const WCHAR *text)
{
	parser p = { package, text, FALSE };
	skip_space(&p);
	if (!*p.at)
		return MSICONDITION_NONE;
	BOOL value = parse_or_chain(&p);
	skip_space(&p);
	if (p.broken || *p.at)
		return MSICONDITION_ERROR;
	return value ? MSICONDITION_TRUE : MSICONDITION_FALSE;
}

// A missing condition holds; one that cannot be read does not.
BOOL condition_true(msi_package *package, const WCHAR *text)
{
	if (!text || !*text)
		return TRUE;
	MSICONDITION result = condition_evaluate(package, text);
	return result == MSICONDITION_TRUE || result == MSICONDITION_NONE;
}

MSICONDITION WINAPI MsiEvaluateConditionW(MSIHANDLE handle, LPCWSTR text)
{
	msi_package *package = package_from_handle(handle);
	if (!package)
		return MSICONDITION_ERROR;
	MSICONDITION result = text ? condition_evaluate(package, text) : MSICONDITION_NONE;
	object_unref(&package->base);
	return result;
}

MSICONDITION WINAPI MsiEvaluateConditionA(MSIHANDLE handle, LPCSTR text)
{
	WCHAR *wide = text ? msi_from_ansi(text) : NULL;
	MSICONDITION result = MsiEvaluateConditionW(handle, wide);
	msi_free(wide);
	return result;
}
