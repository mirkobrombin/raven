/* SPDX-License-Identifier: MIT */
#include "js.h"
#include <math.h>

/* Memory, strings, values, and numbers as text. */

void *js_alloc(SIZE_T size) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1); }

void *js_grow(void *block, SIZE_T size)
{
	if (!block)
		return js_alloc(size);
	return HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, block, size ? size : 1);
}

void js_free(void *block)
{
	if (block)
		HeapFree(GetProcessHeap(), 0, block);
}

WCHAR *js_wcsdup(const WCHAR *text)
{
	UINT length = lstrlenW(text);
	WCHAR *copy = js_alloc((length + 1) * sizeof(WCHAR));
	memcpy(copy, text, length * sizeof(WCHAR));
	return copy;
}

/* Strings. */

js_string *js_string_new(const WCHAR *text, UINT length)
{
	js_string *s = js_alloc(sizeof(js_string) + length * sizeof(WCHAR));
	s->refs = 1;
	s->length = length;
	if (text)
		memcpy(s->text, text, length * sizeof(WCHAR));
	s->text[length] = 0;
	return s;
}

js_string *js_string_of(const WCHAR *text) { return js_string_new(text, text ? lstrlenW(text) : 0); }

js_string *js_string_concat(const js_string *a, const js_string *b)
{
	js_string *s = js_string_new(NULL, a->length + b->length);
	memcpy(s->text, a->text, a->length * sizeof(WCHAR));
	memcpy(s->text + a->length, b->text, b->length * sizeof(WCHAR));
	return s;
}

void js_string_release(js_string *s)
{
	if (s && !InterlockedDecrement(&s->refs))
		js_free(s);
}

BOOL js_string_equal(const js_string *a, const js_string *b)
{
	return a == b || (a->length == b->length && !memcmp(a->text, b->text, a->length * sizeof(WCHAR)));
}

BOOL js_string_is(const js_string *s, const WCHAR *text)
{
	UINT length = lstrlenW(text);
	return s->length == length && !memcmp(s->text, text, length * sizeof(WCHAR));
}

/* By code units, as ECMAScript compares strings. */
int js_string_compare(const js_string *a, const js_string *b)
{
	UINT n = a->length < b->length ? a->length : b->length;
	for (UINT i = 0; i < n; i++)
		if (a->text[i] != b->text[i])
			return a->text[i] < b->text[i] ? -1 : 1;
	return a->length == b->length ? 0 : a->length < b->length ? -1 : 1;
}

void js_buffer_add(js_buffer *b, const WCHAR *text, UINT length)
{
	if (b->length + length + 1 > b->room) {
		b->room = (b->length + length + 1) * 2 + 16;
		b->text = js_grow(b->text, b->room * sizeof(WCHAR));
	}
	memcpy(b->text + b->length, text, length * sizeof(WCHAR));
	b->length += length;
	b->text[b->length] = 0;
}

void js_buffer_text(js_buffer *b, const WCHAR *text) { js_buffer_add(b, text, lstrlenW(text)); }
void js_buffer_char(js_buffer *b, WCHAR c) { js_buffer_add(b, &c, 1); }

js_string *js_buffer_string(js_buffer *b)
{
	js_string *s = js_string_new(b->text, b->length);
	js_free(b->text);
	b->text = NULL;
	b->length = b->room = 0;
	return s;
}

/* Values. */

const js_value js_undefined = { JV_UNDEFINED };
const js_value js_null = { JV_NULL };

js_value js_text(const WCHAR *text) { return js_str(js_string_of(text)); }

js_value js_retain(js_value v)
{
	if (v.type == JV_STRING)
		js_string_retain(v.string);
	else if (v.type == JV_OBJECT)
		js_object_retain(v.object);
	return v;
}

void js_release(js_value v)
{
	if (v.type == JV_STRING)
		js_string_release(v.string);
	else if (v.type == JV_OBJECT)
		js_object_release(v.object);
}

/* Numbers. */

double js_nan(void)
{
	union {
		ULONGLONG bits;
		double d;
	} u = { 0x7ff8000000000000ULL };
	return u.d;
}

double js_infinity(void)
{
	union {
		ULONGLONG bits;
		double d;
	} u = { 0x7ff0000000000000ULL };
	return u.d;
}

BOOL js_is_nan(double n) { return n != n; }
BOOL js_is_finite(double n) { return n == n && n != js_infinity() && n != -js_infinity(); }

double js_integer(double n)
{
	if (js_is_nan(n))
		return 0;
	if (!js_is_finite(n) || n == 0)
		return n;
	return n < 0 ? -floor(-n) : floor(n);
}

UINT32 js_uint32(double n)
{
	if (!js_is_finite(n) || n == 0)
		return 0;
	double i = js_integer(n);
	double m = fmod(i, 4294967296.0);
	if (m < 0)
		m += 4294967296.0;
	return (UINT32)m;
}

INT32 js_int32(double n) { return (INT32)js_uint32(n); }

/* The shortest digits that read back as the same number, as ECMAScript's ToString chooses them, laid out by its rules: plain up to
 * 21 digits before the point and 6 zeros after it, with an exponent beyond. */
void js_number_text(double n, WCHAR *out)
{
	if (js_is_nan(n)) {
		lstrcpyW(out, L"NaN");
		return;
	}
	if (n == 0) {
		lstrcpyW(out, L"0");
		return;
	}
	if (!js_is_finite(n)) {
		lstrcpyW(out, n < 0 ? L"-Infinity" : L"Infinity");
		return;
	}
	WCHAR *o = out;
	if (n < 0) {
		*o++ = L'-';
		n = -n;
	}
	char digits[40];
	int point, count = js_shortest_digits(n, digits, &point);
	/* The value is 0.digits times ten to the point. */
	if (count <= point && point <= 21) {
		for (int i = 0; i < count; i++)
			*o++ = digits[i];
		for (int i = count; i < point; i++)
			*o++ = L'0';
	} else if (0 < point && point <= 21) {
		for (int i = 0; i < point; i++)
			*o++ = digits[i];
		*o++ = L'.';
		for (int i = point; i < count; i++)
			*o++ = digits[i];
	} else if (-6 < point && point <= 0) {
		*o++ = L'0';
		*o++ = L'.';
		for (int i = point; i < 0; i++)
			*o++ = L'0';
		for (int i = 0; i < count; i++)
			*o++ = digits[i];
	} else {
		*o++ = digits[0];
		if (count > 1) {
			*o++ = L'.';
			for (int i = 1; i < count; i++)
				*o++ = digits[i];
		}
		*o++ = L'e';
		*o++ = point - 1 < 0 ? L'-' : L'+';
		int e = point - 1 < 0 ? 1 - point : point - 1;
		WCHAR exp_text[8];
		int length = 0;
		do {
			exp_text[length++] = (WCHAR)(L'0' + e % 10);
			e /= 10;
		} while (e);
		while (length)
			*o++ = exp_text[--length];
	}
	*o = 0;
}

js_string *js_number_string(double n)
{
	WCHAR text[40];
	js_number_text(n, text);
	return js_string_of(text);
}

static BOOL is_space(WCHAR c)
{
	return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\v' || c == L'\f' || c == 0xa0 || c == 0xfeff || c == 0x2028 ||
		c == 0x2029 || (c >= 0x2000 && c <= 0x200a) || c == 0x3000 || c == 0x1680 || c == 0x202f || c == 0x205f;
}

/* ToNumber of a string: blank is 0, then a hexadecimal integer, Infinity, or a decimal literal; anything else is NaN. */
double js_string_number(const js_string *s)
{
	const WCHAR *at = s->text, *end = s->text + s->length;
	while (at < end && is_space(*at))
		at++;
	while (end > at && is_space(end[-1]))
		end--;
	if (at == end)
		return 0;
	if (end - at > 2 && at[0] == L'0' && (at[1] == L'x' || at[1] == L'X')) {
		double value = 0;
		for (const WCHAR *c = at + 2; c < end; c++) {
			int digit = *c >= L'0' && *c <= L'9' ? *c - L'0' : *c >= L'a' && *c <= L'f' ? *c - L'a' + 10 : *c >= L'A' && *c <= L'F' ? *c - L'A' + 10 : -1;
			if (digit < 0)
				return js_nan();
			value = value * 16 + digit;
		}
		return value;
	}
	BOOL negative = FALSE;
	const WCHAR *c = at;
	if (*c == L'+' || *c == L'-')
		negative = *c++ == L'-';
	if (end - c == 8 && !memcmp(c, L"Infinity", 8 * sizeof(WCHAR)))
		return negative ? -js_infinity() : js_infinity();
	/* Digits, a point, digits, an exponent: checked here, read by js_strtod. */
	UINT length = 0;
	BOOL digits = FALSE;
	const WCHAR *d = c;
	while (d < end && *d >= L'0' && *d <= L'9')
		d++, digits = TRUE;
	if (d < end && *d == L'.') {
		d++;
		while (d < end && *d >= L'0' && *d <= L'9')
			d++, digits = TRUE;
	}
	if (!digits)
		return js_nan();
	if (d < end && (*d == L'e' || *d == L'E')) {
		d++;
		if (d < end && (*d == L'+' || *d == L'-'))
			d++;
		const WCHAR *exponent = d;
		while (d < end && *d >= L'0' && *d <= L'9')
			d++;
		if (d == exponent)
			return js_nan();
	}
	if (d != end)
		return js_nan();
	char *text = js_alloc(end - c + 1);
	for (const WCHAR *k = c; k < end; k++)
		text[length++] = (char)*k;
	double value = js_strtod(text);
	js_free(text);
	return negative ? -value : value;
}

/* Errors. */

void js_failure_clear(script_failure *failure)
{
	SysFreeString(failure->description);
	SysFreeString(failure->source);
	failure->description = failure->source = NULL;
}

static FARPROC crt_function(const char *name)
{
	static HMODULE crt;
	if (!crt)
		crt = LoadLibraryW(L"msvcrt.dll");
	return crt ? GetProcAddress(crt, name) : NULL;
}

double js_crt_math(const char *name, double x)
{
	double (__cdecl *op)(double) = (double (__cdecl *)(double))(void *)crt_function(name);
	return op ? op(x) : js_nan();
}

double js_crt_pow(double x, double y)
{
	double (__cdecl *op)(double, double) = (double (__cdecl *)(double, double))(void *)crt_function("pow");
	return op ? op(x, y) : js_nan();
}


const WCHAR *js_error_text(LONG number)
{
	static const struct {
		LONG number;
		const WCHAR *text;
	} texts[] = {
		{ JS_E_INVALID_CALL, L"Invalid procedure call or argument" }, { JS_E_OUT_OF_MEMORY, L"Out of memory" },
		{ JS_E_SUBSCRIPT, L"Subscript out of range" }, { JS_E_TYPE_MISMATCH, L"Type mismatch" }, { JS_E_OUT_OF_STACK, L"Out of stack space" },
		{ JS_E_CANNOT_CREATE, L"Automation server can't create object" },
		{ JS_E_NO_PROPERTY, L"Object doesn't support this property or method" }, { JS_E_NUMBER_EXPECTED, L"Number expected" },
		{ JS_E_FUNCTION_EXPECTED, L"Function expected" }, { JS_E_ASSIGN_RESULT, L"Cannot assign to a function result" },
		{ JS_E_STRING_EXPECTED, L"String expected" }, { JS_E_DATE_EXPECTED, L"Date object expected" }, { JS_E_OBJECT_EXPECTED, L"Object expected" },
		{ JS_E_ILLEGAL_ASSIGN, L"Illegal assignment" }, { JS_E_UNDEFINED, L"'%s' is undefined" },
		{ JS_E_BOOLEAN_EXPECTED, L"Boolean expected" }, { JS_E_JSCRIPT_OBJECT, L"JScript object expected" },
		{ JS_E_ENUMERATOR_EXPECTED, L"Enumerator object expected" }, { JS_E_REGEXP_EXPECTED, L"Regular Expression object expected" },
		{ JS_E_REGEXP_SYNTAX, L"Syntax error in regular expression" }, { 5019, L"Expected ']' in regular expression" },
		{ 5020, L"Expected ')' in regular expression" }, { JS_E_NOT_CAUGHT, L"Exception thrown and not caught" },
		{ JS_E_ARRAY_LENGTH, L"Array length must be a finite positive integer" }, { JS_E_ARRAY_EXPECTED, L"Array or arguments object expected" },
		{ JS_E_VBARRAY_EXPECTED, L"VBArray expected" }, { JS_E_INVALID_URI, L"The URI to be encoded contains an invalid character" },
		{ JS_E_PRECISION, L"The precision is out of range" }, { JS_E_FRACTION, L"The number of fractional digits is out of range" },
		{ JS_E_RADIX, L"The radix must be between 2 and 36" }, { JS_E_SYNTAX, L"Syntax error" },
		{ JS_E_EXPECTED_SEMICOLON, L"Expected ';'" }, { JS_E_EXPECTED_LPAREN, L"Expected '('" }, { JS_E_EXPECTED_RPAREN, L"Expected ')'" },
		{ JS_E_EXPECTED_RBRACKET, L"Expected ']'" }, { JS_E_EXPECTED_LBRACE, L"Expected '{'" }, { JS_E_EXPECTED_RBRACE, L"Expected '}'" },
		{ JS_E_EXPECTED_IDENTIFIER, L"Expected identifier" }, { JS_E_EXPECTED_ASSIGN, L"Expected '='" },
		{ JS_E_INVALID_CHAR, L"Invalid character" }, { JS_E_UNTERMINATED_STRING, L"Unterminated string constant" },
		{ JS_E_UNTERMINATED_COMMENT, L"Unterminated comment" }, { JS_E_RETURN_OUTSIDE, L"'return' statement outside of function" },
		{ JS_E_BREAK_OUTSIDE, L"Can't have 'break' outside of loop" }, { JS_E_CONTINUE_OUTSIDE, L"Can't have 'continue' outside of loop" },
		{ JS_E_EXPECTED_WHILE, L"Expected 'while'" }, { JS_E_LABEL_REDEFINED, L"Label redefined" }, { JS_E_LABEL_NOT_FOUND, L"Label not found" },
		{ JS_E_EXPECTED_COLON, L"Expected ':'" }, { JS_E_EXPECTED_MEMBER, L"Expected identifier, string or number" },
		{ JS_E_EXPECTED_CATCH, L"Expected 'catch'" }, { JS_E_EXPECTED_ESCAPE, L"Expected hexadecimal digit" },
	};
	for (UINT i = 0; i < ARRAYSIZE(texts); i++)
		if (texts[i].number == number)
			return texts[i].text;
	return L"Unknown runtime error";
}
