/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The entry points the Microsoft 365 programs and their libraries import besides the rest: more of the math library, the floating
 * point environment, the secure number conversions, multibyte strings in the ANSI code page, bounded path and time strings, and the
 * _o_ names that libraries built against the newer runtime use for the same functions. Included by ucrtbase.c. */

/* ---- math ---- */
CRT double hypot(double x, double y) { return _hypot(x, y); }
CRT double fmax(double a, double b) { return a != a ? b : b != b ? a : a > b ? a : b; }
CRT double fmin(double a, double b) { return a != a ? b : b != b ? a : a < b ? a : b; }
CRT float copysignf(float x, float y) { return (float)copysign(x, y); }
CRT double scalbn(double x, int exponent) { return ldexp(x, exponent); }
CRT float scalbnf(float x, int exponent) { return (float)ldexp(x, exponent); }
CRT float tanhf(float x) { return (float)tanh(x); }
CRT float nearbyintf(float x) { return (float)rint(x); }

/* The next representable value toward y: one step of the bit pattern, which is ordered like the magnitudes it encodes. */
CRT double nextafter(double x, double y)
{
	union { double value; unsigned long long bits; } v = {x};
	if (x != x || y != y)
		return x + y;
	if (x == y)
		return y;
	if (x == 0) {
		v.bits = 1;
		return y > 0 ? v.value : -v.value;
	}
	if ((x < y) == (x > 0))
		++v.bits;
	else
		--v.bits;
	return v.value;
}

CRT float nextafterf(float x, float y)
{
	union { float value; unsigned bits; } v = {x};
	if (x != x || y != y)
		return x + y;
	if (x == y)
		return y;
	if (x == 0) {
		v.bits = 1;
		return y > 0 ? v.value : -v.value;
	}
	if ((x < y) == (x > 0))
		++v.bits;
	else
		--v.bits;
	return v.value;
}

/* A first guess from the logarithm, then Newton steps, which double the correct digits each time. */
CRT double cbrt(double x)
{
	if (x == 0 || x != x || x == 1.0 / 0.0 || x == -1.0 / 0.0)
		return x;
	double magnitude = fabs(x), root = exp(log(magnitude) / 3);
	for (int step = 0; step < 3; ++step)
		root -= (root * root * root - magnitude) / (3 * root * root);
	return copysign(root, x);
}

CRT float cbrtf(float x) { return (float)cbrt(x); }
CRT double asinh(double x) { return copysign(log(fabs(x) + sqrt(x * x + 1)), x); }
CRT double acosh(double x) { return x < 1 ? 0.0 / 0.0 : log(x + sqrt(x * x - 1)); }
CRT double atanh(double x) { return fabs(x) > 1 ? 0.0 / 0.0 : 0.5 * log1p(2 * x / (1 - x)); }

/* The power of two of a value as frexp finds it, one less since frexp scales into [0.5, 1). */
CRT int ilogb(double x)
{
	int exponent;
	if (x == 0)
		return (int)0x80000000;
	if (x != x)
		return 0x7fffffff;
	if (x == 1.0 / 0.0 || x == -1.0 / 0.0)
		return 0x7fffffff;
	frexp(x, &exponent);
	return exponent - 1;
}

CRT int ilogbf(float x) { return ilogb(x); }

CRT double _logb(double x)
{
	if (x == 0)
		return -1.0 / 0.0;
	if (x != x)
		return x;
	if (x == 1.0 / 0.0 || x == -1.0 / 0.0)
		return 1.0 / 0.0;
	return ilogb(x);
}

/* The remainder after the quotient rounded to the nearest whole number, ties to even. */
CRT float remainderf(float x, float y)
{
	if (y == 0 || x != x || y != y)
		return 0.0f / 0.0f;
	const double quotient = rint((double)x / y);
	return (float)((double)x - quotient * y);
}

/* The logarithm of the gamma function: the Lanczos series for positive arguments and the reflection formula below one half. */
CRT double lgamma(double x)
{
	static const double coefficients[] = {0.99999999999980993, 676.5203681218851, -1259.1392167224028, 771.32342877765313,
		-176.61502916214059, 12.507343278686905, -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7};
	const double pi = 3.14159265358979323846;
	if (x != x)
		return x;
	if (x == 1.0 / 0.0 || x == -1.0 / 0.0)
		return 1.0 / 0.0;
	if (x < 0.5) {
		const double sine = sin(pi * x);
		if (sine == 0)
			return 1.0 / 0.0;
		return log(pi / fabs(sine)) - lgamma(1 - x);
	}
	x -= 1;
	double sum = coefficients[0];
	for (int at = 1; at < 9; ++at)
		sum += coefficients[at] / (x + at);
	const double t = x + 7.5;
	return 0.91893853320467274178 + (x + 0.5) * log(t) - t + log(sum);
}

/* The rounding mode lives in both units: bits 13-14 of MXCSR and bits 10-11 of the x87 control word share one encoding, which the C
   library reports shifted to bits 8-9. */
CRT int fegetround(void)
{
	unsigned short control;
	__asm__ volatile("fnstcw %0" : "=m"(control));
	return ((control >> 10) & 3) << 8;
}

CRT int fesetround(int mode)
{
	unsigned short control;
	if (mode & ~0x300)
		return 1;
	__asm__ volatile("fnstcw %0" : "=m"(control));
	control = (unsigned short)((control & ~0xc00) | ((mode >> 8) << 10));
	__asm__ volatile("fldcw %0" : : "m"(control));
	unsigned status;
	__asm__ volatile("stmxcsr %0" : "=m"(status));
	status = (status & ~0x6000u) | ((unsigned)(mode >> 8) << 13);
	__asm__ volatile("ldmxcsr %0" : : "m"(status));
	return 0;
}

/* FLT_ROUNDS numbers the modes differently: 1 nearest, 0 toward zero, 2 upward, 3 downward. */
CRT int __fpe_flt_rounds(void)
{
	static const int order[] = {1, 3, 2, 0};
	return order[fegetround() >> 8];
}

/* The default environment: every exception masked, rounding to nearest, the x87 unit at double precision as Windows sets it. */
CRT void _fpreset(void)
{
	const unsigned short control = 0x27f;
	const unsigned status = 0x1f80;
	__asm__ volatile("fnclex\n\tfldcw %0" : : "m"(control));
	__asm__ volatile("ldmxcsr %0" : : "m"(status));
}

CRT void _set_controlfp(unsigned value, unsigned mask) { _controlfp(value, mask); }
CRT int _set_FMA3_enable(int enable) { return 0; }
CRT short _dtest(double *x) { return _dclass(*x); }
CRT short _fdtest(float *x) { return _fdclass(*x); }
CRT short _ldtest(double *x) { return _dclass(*x); }

/* ---- integers ---- */
CRT int abs(int value) { return value < 0 ? -value : value; }
CRT long labs(long value) { return value < 0 ? -value : value; }
CRT long long llabs(long long value) { return value < 0 ? -value : value; }

__asm__(".section .drectve\n\t.ascii \" -export:ldiv=raven_ldiv\"\n\t.text");
typedef struct {
	long quotient, remainder;
} RavenLongDivision;

RavenLongDivision raven_ldiv(long numerator, long denominator)
{
	RavenLongDivision result = {numerator / denominator, numerator % denominator};
	return result;
}

/* ---- number conversions ---- */
static int integer_to_narrow(unsigned long long value, int negative, char *buffer, size_t size, int radix)
{
	wchar_t wide[66];
	size_t at = 0;
	if (!buffer || !size) {
		set_errno(22);
		return 22;
	}
	const int result = integer_to_wide(value, negative, wide, size < 66 ? size : 66, radix);
	if (result) {
		buffer[0] = 0;
		return result;
	}
	do
		buffer[at] = (char)wide[at];
	while (wide[at++]);
	return 0;
}

/* Negative values carry a sign in base ten only; in other bases the bits of the type are written as they are. */
CRT int _itoa_s(int value, char *buffer, size_t size, int radix)
{
	const int negative = radix == 10 && value < 0;
	return integer_to_narrow(negative ? 0ULL - (unsigned long long)(long long)value : (unsigned)value, negative, buffer, size, radix);
}

CRT int _ltoa_s(long value, char *buffer, size_t size, int radix) { return _itoa_s(value, buffer, size, radix); }
CRT int _ultoa_s(unsigned long value, char *buffer, size_t size, int radix) { return integer_to_narrow(value, 0, buffer, size, radix); }
CRT char *_ltoa(long value, char *to, int radix) { return _itoa(value, to, radix); }

CRT int _i64toa_s(long long value, char *buffer, size_t size, int radix)
{
	const int negative = radix == 10 && value < 0;
	return integer_to_narrow(negative ? 0ULL - (unsigned long long)value : (unsigned long long)value, negative, buffer, size, radix);
}

CRT int _ltow_s(long value, wchar_t *buffer, size_t size, int radix) { return _itow_s(value, buffer, size, radix); }

CRT wchar_t *_ltow(long value, wchar_t *to, int radix)
{
	_itow_s(value, to, 34, radix);
	return to;
}

CRT double _atof_l(const char *text, _locale_t locale) { return text ? raven_strtod(text, NULL) : 0.0; }
CRT double _wtof_l(const wchar_t *text, _locale_t locale) { return text ? raven_wcstod(text, NULL) : 0.0; }
CRT double _strtod_l(const char *text, char **end, _locale_t locale) { return raven_strtod(text, end); }

/* The significant decimal digits of a positive finite value, rounded to nearest at `count` and padded with zeros, and the power of ten of
   the first. */
static int decimal_digits(double value, int count, char *digits)
{
	int point;
	int used = float_digits(value, count, digits, &point);
	while (used < count)
		digits[used++] = '0';
	digits[count] = 0;
	return value == 0 ? 0 : point - 1;
}

/* `count` significant digits with no point; where the point goes and the sign come back apart. */
CRT int _ecvt_s(char *buffer, size_t size, double value, int count, int *point, int *sign)
{
	char digits[352];
	if (!buffer || !size || !point || !sign) {
		set_errno(22);
		return 22;
	}
	buffer[0] = 0;
	if (count < 0)
		count = 0;
	if (count > 348)
		count = 348;
	if ((size_t)count + 1 > size) {
		set_errno(34);
		return 34;
	}
	*sign = __builtin_signbit(value) != 0;
	value = fabs(value);
	*point = value == 0 ? 0 : decimal_digits(value, count, digits) + 1;
	if (value == 0)
		decimal_digits(0, count, digits);
	memcpy(buffer, digits, (size_t)count + 1);
	return 0;
}

/* `count` digits after the point; the buffer is the runtime's own, as for _ecvt. */
CRT char *_fcvt(double value, int count, int *point, int *sign)
{
	static char buffer[352];
	char digits[352];
	*sign = __builtin_signbit(value) != 0;
	value = fabs(value);
	if (value == 0) {
		*point = 0;
		buffer[0] = 0;
		return buffer;
	}
	const int first = decimal_digits(value, 1, digits);
	int total = first + 1 + count;
	if (total > 348)
		total = 348;
	if (total <= 0) {
		*point = -count;
		buffer[0] = 0;
		return buffer;
	}
	*point = decimal_digits(value, total, digits) + 1;
	memcpy(buffer, digits, (size_t)total + 1);
	return buffer;
}

/* The shorter of fixed and exponent notation with `count` significant digits, trailing zeros dropped; a whole number keeps its point. */
CRT int _gcvt_s(char *buffer, size_t size, double value, int count)
{
	char digits[352], text[400];
	size_t length = 0;
	if (!buffer || !size || count < 0) {
		set_errno(22);
		return 22;
	}
	if (!count)
		count = 1;
	if (count > 340)
		count = 340;
	if (__builtin_signbit(value))
		text[length++] = '-';
	value = fabs(value);
	const int exponent = value == 0 ? 0 : decimal_digits(value, count, digits);
	if (value == 0)
		decimal_digits(0, count, digits);
	int used = count;
	while (used > 1 && digits[used - 1] == '0')
		--used;
	if (exponent < -4 || exponent >= count) {
		text[length++] = digits[0];
		text[length++] = '.';
		for (int at = 1; at < used; ++at)
			text[length++] = digits[at];
		const int magnitude = exponent < 0 ? -exponent : exponent;
		text[length++] = 'e';
		text[length++] = exponent < 0 ? '-' : '+';
		if (magnitude >= 100)
			text[length++] = (char)('0' + magnitude / 100);
		text[length++] = (char)('0' + magnitude / 10 % 10);
		text[length++] = (char)('0' + magnitude % 10);
	} else if (exponent < 0) {
		text[length++] = '0';
		text[length++] = '.';
		for (int at = -1; at > exponent; --at)
			text[length++] = '0';
		for (int at = 0; at < used; ++at)
			text[length++] = digits[at];
	} else {
		for (int at = 0; at <= exponent; ++at)
			text[length++] = at < used ? digits[at] : '0';
		text[length++] = '.';
		for (int at = exponent + 1; at < used; ++at)
			text[length++] = digits[at];
	}
	text[length] = 0;
	if (length + 1 > size) {
		buffer[0] = 0;
		set_errno(34);
		return 34;
	}
	memcpy(buffer, text, length + 1);
	return 0;
}

/* ---- strings ---- */
static int change_case_narrow(char *text, size_t size, char *(*change)(char *))
{
	size_t length = 0;
	if (!text || !size) {
		set_errno(22);
		return 22;
	}
	while (length < size && text[length])
		++length;
	if (length == size) {
		text[0] = 0;
		set_errno(22);
		return 22;
	}
	change(text);
	return 0;
}

CRT int _strlwr_s(char *text, size_t size) { return change_case_narrow(text, size, _strlwr); }
CRT int _strupr_s(char *text, size_t size) { return change_case_narrow(text, size, _strupr); }

/* At most `count` characters onto the end of the string; _TRUNCATE keeps what fits and reports it, anything else that does not fit
   empties the destination. */
CRT int strncat_s(char *to, size_t size, const char *from, size_t count)
{
	size_t end = 0, at = 0;
	if (!to || !size || (!from && count)) {
		if (to && size)
			to[0] = 0;
		set_errno(22);
		return 22;
	}
	while (end < size && to[end])
		++end;
	if (end == size) {
		to[0] = 0;
		set_errno(22);
		return 22;
	}
	while (at < count && from[at]) {
		if (end + at + 1 >= size) {
			if (count == (size_t)-1) {
				to[end + at] = 0;
				return 80;
			}
			to[0] = 0;
			set_errno(34);
			return 34;
		}
		to[end + at] = from[at];
		++at;
	}
	to[end + at] = 0;
	return 0;
}

CRT int _memicmp(const void *a, const void *b, size_t count)
{
	const unsigned char *left = a, *right = b;
	for (size_t at = 0; at < count; ++at) {
		const int x = tolower(left[at]), y = tolower(right[at]);
		if (x != y)
			return x - y;
	}
	return 0;
}

/* Pairs of bytes swap places; an odd last byte stays. */
CRT void _swab(char *from, char *to, int count)
{
	for (int at = 0; at + 1 < count; at += 2) {
		const char first = from[at];
		to[at] = from[at + 1];
		to[at + 1] = first;
	}
}

CRT size_t wcscspn(const wchar_t *text, const wchar_t *stops)
{
	size_t at = 0;
	for (; text[at]; ++at)
		for (const wchar_t *stop = stops; *stop; ++stop)
			if (text[at] == *stop)
				return at;
	return at;
}

/* The conforming three argument form; a program that passes no context shares one per process, as the older form did. */
CRT wchar_t *wcstok(wchar_t *text, const wchar_t *separators, wchar_t **context)
{
	static wchar_t *shared;
	if (!context)
		context = &shared;
	return wcstok_s(text, separators, context);
}

CRT wchar_t *_wcsrev(wchar_t *text)
{
	size_t length = wcslen(text);
	for (size_t at = 0; at < length / 2; ++at) {
		const wchar_t first = text[at];
		text[at] = text[length - 1 - at];
		text[length - 1 - at] = first;
	}
	return text;
}

CRT int _wcsicoll(const wchar_t *a, const wchar_t *b) { return _wcsicmp(a, b); }
CRT int _isctype_l(int c, int mask, _locale_t locale) { return c >= -1 && c < 256 ? __pctype_func()[c + 1] & mask : 0; }
#undef _iswdigit_l
#undef _iswlower_l
#undef _iswupper_l
#undef _iswpunct_l
CRT int _iswdigit_l(wint_t c, _locale_t locale) { return iswdigit(c); }
CRT int _iswlower_l(wint_t c, _locale_t locale) { return iswlower(c); }
CRT int _iswupper_l(wint_t c, _locale_t locale) { return iswupper(c); }
CRT int _iswpunct_l(wint_t c, _locale_t locale) { return iswpunct(c); }
CRT wint_t _towlower_l(wint_t c, _locale_t locale) { return towlower(c); }
CRT wint_t _towupper_l(wint_t c, _locale_t locale) { return towupper(c); }
#undef iswascii
#undef iswblank
#undef iswgraph
CRT int iswascii(wint_t c) { return c < 0x80; }
CRT int iswblank(wint_t c) { return c == L' ' || c == L'\t' || iswctype(c, 0x40); }
CRT int iswgraph(wint_t c) { return iswctype(c, 0x10 | 0x100 | 0x4); }

CRT int wctomb_s(int *length, char *to, size_t size, wchar_t character)
{
	char bytes[8];
	BOOL lost = FALSE;
	if (length)
		*length = 0;
	if (!to && size) {
		set_errno(22);
		return 22;
	}
	if (!to)
		return 0;
	const int used = WideCharToMultiByte(CP_ACP, 0, &character, 1, bytes, sizeof bytes, NULL, &lost);
	if (!used || lost) {
		set_errno(42);
		return 42;
	}
	if ((size_t)used > size) {
		set_errno(34);
		return 34;
	}
	memcpy(to, bytes, (size_t)used);
	if (length)
		*length = used;
	return 0;
}

/* ---- multibyte strings in the ANSI code page: a lead byte and its trail are one character ---- */
static unsigned mb_code_page;

CRT int _getmbcp(void) { return mb_code_page ? (int)mb_code_page : (int)GetACP(); }

/* -3 selects the ANSI code page, -2 the OEM one and -4 the current locale's, which here is the ANSI one too. */
CRT int _setmbcp(int code_page)
{
	if (code_page == -2)
		mb_code_page = GetOEMCP();
	else if (code_page == -3 || code_page == -4 || code_page == 0)
		mb_code_page = 0;
	else if (IsValidCodePage((unsigned)code_page))
		mb_code_page = (unsigned)code_page;
	else {
		set_errno(22);
		return -1;
	}
	return 0;
}

static int mb_lead(unsigned char byte) { return IsDBCSLeadByteEx(mb_code_page ? mb_code_page : CP_ACP, byte); }
static size_t mb_size(const unsigned char *at) { return mb_lead(*at) && at[1] ? 2 : 1; }
static unsigned mb_code(const unsigned char *at) { return mb_size(at) == 2 ? (unsigned)at[0] << 8 | at[1] : at[0]; }
static unsigned mb_lower(unsigned code) { return code >= 'A' && code <= 'Z' ? code + 32 : code; }
static unsigned mb_upper(unsigned code) { return code >= 'a' && code <= 'z' ? code - 32 : code; }

CRT size_t _mbclen(const unsigned char *at) { return mb_size(at); }
CRT unsigned char *_mbsinc(const unsigned char *at) { return (unsigned char *)at + mb_size(at); }
CRT int _ismbcspace(unsigned code) { return code < 0x100 && (code == ' ' || (code >= '\t' && code <= '\r')); }

/* Whether `at` is a lead byte, judged by walking from the start, since a trail byte can look like one. */
CRT int _ismbslead(const unsigned char *start, const unsigned char *at)
{
	const unsigned char *walk = start;
	while (*walk && walk < at)
		walk += mb_size(walk);
	return walk == at && *at && mb_size(at) == 2 ? -1 : 0;
}

CRT unsigned char *_mbschr(const unsigned char *text, unsigned code)
{
	for (; *text; text += mb_size(text))
		if (mb_code(text) == code)
			return (unsigned char *)text;
	return code ? NULL : (unsigned char *)text;
}

CRT unsigned char *_mbsrchr(const unsigned char *text, unsigned code)
{
	const unsigned char *found = NULL;
	for (; *text; text += mb_size(text))
		if (mb_code(text) == code)
			found = text;
	return (unsigned char *)(code ? found : text);
}

static int mb_compare(const unsigned char *a, const unsigned char *b, size_t bytes, int fold)
{
	const unsigned char *start = a;
	while ((size_t)(a - start) < bytes) {
		unsigned x = mb_code(a), y = mb_code(b);
		if (fold) {
			x = mb_lower(x);
			y = mb_lower(y);
		}
		if (x != y)
			return x < y ? -1 : 1;
		if (!x)
			return 0;
		a += mb_size(a);
		b += mb_size(b);
	}
	return 0;
}

CRT int _mbscmp(const unsigned char *a, const unsigned char *b) { return mb_compare(a, b, (size_t)-1, 0); }
CRT int _mbsicmp(const unsigned char *a, const unsigned char *b) { return mb_compare(a, b, (size_t)-1, 1); }
CRT int _mbscoll(const unsigned char *a, const unsigned char *b) { return _mbscmp(a, b); }
CRT int _mbsicoll(const unsigned char *a, const unsigned char *b) { return _mbsicmp(a, b); }
CRT int _mbsnbcmp(const unsigned char *a, const unsigned char *b, size_t bytes) { return mb_compare(a, b, bytes, 0); }
CRT int _mbsnbicmp(const unsigned char *a, const unsigned char *b, size_t bytes) { return mb_compare(a, b, bytes, 1); }

static int mb_in_set(unsigned code, const unsigned char *set)
{
	for (; *set; set += mb_size(set))
		if (mb_code(set) == code)
			return 1;
	return 0;
}

CRT size_t _mbscspn(const unsigned char *text, const unsigned char *set)
{
	const unsigned char *at = text;
	while (*at && !mb_in_set(mb_code(at), set))
		at += mb_size(at);
	return (size_t)(at - text);
}

CRT size_t _mbsspn(const unsigned char *text, const unsigned char *set)
{
	const unsigned char *at = text;
	while (*at && mb_in_set(mb_code(at), set))
		at += mb_size(at);
	return (size_t)(at - text);
}

CRT unsigned char *_mbspbrk(const unsigned char *text, const unsigned char *set)
{
	text += _mbscspn(text, set);
	return *text ? (unsigned char *)text : NULL;
}

/* A match counts only where a character starts, never at a trail byte. */
CRT unsigned char *_mbsstr(const unsigned char *text, const unsigned char *part)
{
	const size_t length = strlen((const char *)part);
	for (; *text; text += mb_size(text))
		if (!memcmp(text, part, length))
			return (unsigned char *)text;
	return length ? NULL : (unsigned char *)text;
}

/* The characters in reverse order, each double byte character kept in its own order. */
CRT unsigned char *_mbsrev(unsigned char *text)
{
	const size_t length = strlen((char *)text);
	unsigned char *at = text;
	while (*at) {
		const size_t size = mb_size(at);
		if (size == 2) {
			const unsigned char lead = at[0];
			at[0] = at[1];
			at[1] = lead;
		}
		at += size;
	}
	for (size_t from = 0; from < length / 2; ++from) {
		const unsigned char first = text[from];
		text[from] = text[length - 1 - from];
		text[length - 1 - from] = first;
	}
	return text;
}

static int mb_change_case(unsigned char *text, size_t size, unsigned (*change)(unsigned))
{
	size_t length = 0;
	if (!text || !size) {
		set_errno(22);
		return 22;
	}
	while (length < size && text[length])
		++length;
	if (length == size) {
		text[0] = 0;
		set_errno(22);
		return 22;
	}
	for (unsigned char *at = text; *at; at += mb_size(at))
		if (mb_size(at) == 1)
			*at = (unsigned char)change(*at);
	return 0;
}

CRT int _mbslwr_s(unsigned char *text, size_t size) { return mb_change_case(text, size, mb_lower); }
CRT int _mbsupr_s(unsigned char *text, size_t size) { return mb_change_case(text, size, mb_upper); }

CRT unsigned char *_mbstok_s(unsigned char *text, const unsigned char *separators, unsigned char **context)
{
	if (!context || !separators || (!text && !*context)) {
		set_errno(22);
		return NULL;
	}
	if (!text)
		text = *context;
	text += _mbsspn(text, separators);
	if (!*text) {
		*context = text;
		return NULL;
	}
	unsigned char *end = text + _mbscspn(text, separators);
	if (*end) {
		const size_t size = mb_size(end);
		end[0] = 0;
		if (size == 2)
			end[1] = 0;
		*context = end + size;
	} else
		*context = end;
	return text;
}

/* ---- paths ---- */
/* The narrow split goes through the wide one, so a trail byte that looks like a separator is not taken for one. */
CRT int _splitpath_s(const char *path, char *drive, size_t drive_size, char *directory, size_t directory_size, char *name, size_t name_size,
	char *extension, size_t extension_size)
{
	wchar_t wide[MAX_PATH * 2], parts[4][MAX_PATH * 2];
	char *outs[4] = {drive, directory, name, extension};
	const size_t sizes[4] = {drive_size, directory_size, name_size, extension_size};
	int result = 22;
	if (!path || !MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH * 2))
		goto failed;
	if ((result = _wsplitpath_s(wide, parts[0], MAX_PATH * 2, parts[1], MAX_PATH * 2, parts[2], MAX_PATH * 2, parts[3], MAX_PATH * 2)))
		goto failed;
	for (int part = 0; part < 4; ++part) {
		if (!outs[part]) {
			if (sizes[part]) {
				result = 22;
				goto failed;
			}
			continue;
		}
		const int needed = WideCharToMultiByte(CP_ACP, 0, parts[part], -1, NULL, 0, NULL, NULL);
		if (!sizes[part] || needed <= 0) {
			result = 22;
			goto failed;
		}
		if ((size_t)needed > sizes[part]) {
			result = 34;
			goto failed;
		}
		WideCharToMultiByte(CP_ACP, 0, parts[part], -1, outs[part], (int)sizes[part], NULL, NULL);
	}
	return 0;
failed:
	for (int part = 0; part < 4; ++part)
		if (outs[part] && sizes[part])
			outs[part][0] = 0;
	set_errno(result);
	return result;
}

CRT void _wsplitpath(const wchar_t *path, wchar_t *drive, wchar_t *directory, wchar_t *name, wchar_t *extension)
{
	_wsplitpath_s(path, drive, drive ? 3 : 0, directory, directory ? 256 : 0, name, name ? 256 : 0, extension, extension ? 256 : 0);
}

/* A drive letter gains its colon, a directory its trailing separator and an extension its dot. */
CRT int _makepath_s(char *path, size_t size, const char *drive, const char *directory, const char *name, const char *extension)
{
	size_t length = 0;
	if (!path || !size) {
		set_errno(22);
		return 22;
	}
#define PUT(character) do { if (length + 1 >= size) goto overflow; path[length++] = (character); } while (0)
	if (drive && *drive) {
		PUT(drive[0]);
		PUT(':');
	}
	if (directory && *directory) {
		const char *at = directory;
		for (; *at; ++at)
			PUT(*at);
		if (at[-1] != '\\' && at[-1] != '/')
			PUT('\\');
	}
	for (const char *at = name; at && *at; ++at)
		PUT(*at);
	if (extension && *extension) {
		if (*extension != '.')
			PUT('.');
		for (const char *at = extension; *at; ++at)
			PUT(*at);
	}
#undef PUT
	path[length] = 0;
	return 0;
overflow:
	path[0] = 0;
	set_errno(34);
	return 34;
}

/* ---- environment and files ---- */
CRT int _dupenv_s(char **value, size_t *length, const char *name)
{
	if (!value || !name) {
		set_errno(22);
		return 22;
	}
	const char *found = getenv(name);
	*value = found ? _strdup(found) : NULL;
	if (length)
		*length = *value ? strlen(*value) + 1 : 0;
	return found && !*value ? 12 : 0;
}

CRT int _wdupenv_s(wchar_t **value, size_t *length, const wchar_t *name)
{
	if (!value || !name) {
		set_errno(22);
		return 22;
	}
	const wchar_t *found = _wgetenv(name);
	*value = found ? _wcsdup(found) : NULL;
	if (length)
		*length = *value ? wcslen(*value) + 1 : 0;
	return found && !*value ? 12 : 0;
}

CRT int _access_s(const char *path, int mode) { return _access(path, mode) ? *_errno() : 0; }
CRT int _sopen_s(int *fd, const char *path, int flags, int share, int permission) { return _sopen_dispatch(path, flags, share, permission, fd, 1); }
CRT int _wsopen_s(int *fd, const wchar_t *path, int flags, int share, int permission)
{
	return _wsopen_dispatch(path, flags, share, permission, fd, 1);
}

CRT int _chdrive(int drive)
{
	char root[3] = {(char)('A' + drive - 1), ':', 0};
	if (drive < 1 || drive > 26) {
		set_errno(13);
		return -1;
	}
	return SetCurrentDirectoryA(root) ? 0 : -1;
}

typedef struct {
	unsigned total_clusters, available_clusters, sectors_per_cluster, bytes_per_sector;
} RavenDiskFree;

/* Drive 0 is the current one. */
CRT unsigned _getdiskfree(unsigned drive, RavenDiskFree *out)
{
	char root[4] = {(char)('A' + drive - 1), ':', '\\', 0};
	DWORD sectors, bytes, available, total;
	if (!out || drive > 26)
		return 22;
	if (!GetDiskFreeSpaceA(drive ? root : NULL, &sectors, &bytes, &available, &total))
		return GetLastError();
	out->total_clusters = total;
	out->available_clusters = available;
	out->sectors_per_cluster = sectors;
	out->bytes_per_sector = bytes;
	return 0;
}

CRT int tmpnam_s(char *buffer, size_t size)
{
	char name[MAX_PATH];
	if (!buffer || !size)
		return 22;
	if (!tmpnam(name))
		return 13;
	if (strlen(name) + 1 > size) {
		buffer[0] = 0;
		return 34;
	}
	memcpy(buffer, name, strlen(name) + 1);
	return 0;
}

CRT int rename(const char *from, const char *to) { return windows_result(MoveFileA(from, to)); }
CRT void setbuf(void *stream, char *buffer) { setvbuf(stream, buffer, buffer ? 0 : 4, 512); }

CRT int clearerr_s(void *stream)
{
	if (!stream) {
		set_errno(22);
		return 22;
	}
	clearerr(stream);
	return 0;
}

/* Reading more than the buffer holds is refused before anything is read. */
CRT size_t fread_s(void *buffer, size_t size, size_t element, size_t count, void *stream)
{
	if (!element || !count)
		return 0;
	if (!buffer || !stream || count > size / element) {
		set_errno(!buffer || !stream ? 22 : 34);
		return 0;
	}
	return fread(buffer, element, count, stream);
}

CRT int __stdio_common_vfprintf_s(unsigned long long options, void *stream, const char *text, void *locale, va_list arguments)
{
	return __stdio_common_vfprintf(options, stream, text, locale, arguments);
}

/* ---- time ---- */
CRT long _time32(long *out)
{
	const long now = (long)_time64(NULL);
	if (out)
		*out = now;
	return now;
}

CRT wchar_t *_wctime64(const long long *time)
{
	static wchar_t text[32];
	const char *narrow = _ctime64(time);
	if (!narrow)
		return NULL;
	for (size_t at = 0; at < 32 && (text[at] = (unsigned char)narrow[at]); ++at)
		;
	return text;
}

CRT int _wctime64_s(wchar_t *buffer, size_t size, const long long *time)
{
	if (!buffer || !size || !time)
		return 22;
	buffer[0] = 0;
	if (size < 26)
		return 34;
	const wchar_t *text = _wctime64(time);
	if (!text)
		return 22;
	memcpy(buffer, text, (wcslen(text) + 1) * sizeof(wchar_t));
	return 0;
}

static int clock_string_s(wchar_t *buffer, size_t size, char *(*make)(char *))
{
	char text[16];
	if (!buffer || !size)
		return 22;
	if (size < 9) {
		buffer[0] = 0;
		return 34;
	}
	make(text);
	for (size_t at = 0; at < 9; ++at)
		buffer[at] = (unsigned char)text[at];
	return 0;
}

CRT int _wstrdate_s(wchar_t *buffer, size_t size) { return clock_string_s(buffer, size, _strdate); }
CRT int _wstrtime_s(wchar_t *buffer, size_t size) { return clock_string_s(buffer, size, _strtime); }

/* ---- memory ---- */
/* A block can shrink where it is; growing it would move it, which _expand never does. */
CRT void *_expand(void *block, size_t size)
{
	if (!block)
		return NULL;
	return HeapReAlloc(GetProcessHeap(), HEAP_REALLOC_IN_PLACE_ONLY, block, size ? size : 1);
}

/* The old contents are as large as what is left of the raw allocation past the aligned start. */
CRT void *_aligned_realloc(void *block, size_t size, size_t alignment)
{
	if (!block)
		return _aligned_malloc(size, alignment);
	if (!size) {
		_aligned_free(block);
		return NULL;
	}
	char *raw = ((char **)block)[-1];
	const size_t old = HeapSize(GetProcessHeap(), 0, raw) - (size_t)((char *)block - raw);
	void *moved = _aligned_malloc(size, alignment);
	if (!moved)
		return NULL;
	memcpy(moved, block, old < size ? old : size);
	_aligned_free(block);
	return moved;
}

/* No walk of the heap is offered: the first call reports its end. */
CRT int _heapwalk(struct _heapinfo *entry) { return -5; }

__asm__(".section .drectve\n\t.ascii \" -export:_malloc_base=malloc -export:_calloc_base=calloc -export:_realloc_base=realloc"
	" -export:_free_base=free\"\n\t.text");

/* ---- threads ---- */
typedef struct {
	void (__cdecl *function)(void *);
	void *argument;
	HANDLE thread;
} SimpleThreadStart;

/* A thread started by _beginthread closes its own handle when it returns. */
static DWORD WINAPI simple_thread_trampoline(void *parameter)
{
	SimpleThreadStart start = *(SimpleThreadStart *)parameter;
	HeapFree(GetProcessHeap(), 0, parameter);
	start.function(start.argument);
	CloseHandle(start.thread);
	return 0;
}

CRT uintptr_t _beginthread(void (__cdecl *function)(void *), unsigned stack, void *argument)
{
	SimpleThreadStart *start = HeapAlloc(GetProcessHeap(), 0, sizeof *start);
	if (!start || !function) {
		HeapFree(GetProcessHeap(), 0, start);
		set_errno(22);
		return (uintptr_t)-1;
	}
	start->function = function;
	start->argument = argument;
	start->thread = CreateThread(NULL, stack, simple_thread_trampoline, start, CREATE_SUSPENDED, NULL);
	if (!start->thread) {
		HeapFree(GetProcessHeap(), 0, start);
		set_errno(11);
		return (uintptr_t)-1;
	}
	const HANDLE thread = start->thread;
	ResumeThread(thread);
	return (uintptr_t)thread;
}

/* Ending early leaves the handle open: only the thread's own return path knows it. */
CRT void _endthread(void) { ExitThread(0); }

/* ---- diagnostics and handlers ---- */
CRT void _wassert(const wchar_t *expression, const wchar_t *file, unsigned line)
{
	OutputDebugStringW(L"Assertion failed: ");
	OutputDebugStringW(expression);
	OutputDebugStringW(L"\n");
	ExitProcess(3);
}

typedef void (__cdecl *invalid_parameter_handler)(const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t);
static invalid_parameter_handler current_invalid_parameter_handler;

CRT invalid_parameter_handler _set_invalid_parameter_handler(invalid_parameter_handler handler)
{
	const invalid_parameter_handler previous = current_invalid_parameter_handler;
	current_invalid_parameter_handler = handler;
	return previous;
}

CRT invalid_parameter_handler _get_invalid_parameter_handler(void) { return current_invalid_parameter_handler; }

CRT int _get_pgmptr(char **out)
{
	static char path[MAX_PATH];
	if (!out)
		return 22;
	if (!path[0])
		GetModuleFileNameA(NULL, path, MAX_PATH);
	*out = path;
	return 0;
}

CRT wchar_t *_wsetlocale(int category, const wchar_t *name)
{
	static wchar_t result[128];
	char narrow[128];
	if (name && !WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, sizeof narrow, NULL, NULL))
		return NULL;
	const char *chosen = setlocale(category, name ? narrow : NULL);
	if (!chosen || !MultiByteToWideChar(CP_ACP, 0, chosen, -1, result, 128))
		return NULL;
	return result;
}

CRT int __uncaught_exception(void) { return 0; }

/* Undecorated names are not produced; callers fall back to the decorated one. */
CRT char *__unDName(char *out, const char *name, int size, void *allocate, void *release, unsigned short flags) { return NULL; }
CRT char *__unDNameEx(char *out, const char *name, int size, void *allocate, void *release, void *arguments, unsigned flags) { return NULL; }

/* ---- searching and sorting with a context ---- */
static void swap_elements(unsigned char *a, unsigned char *b, size_t size)
{
	while (size--) {
		const unsigned char first = *a;
		*a++ = *b;
		*b++ = first;
	}
}

/* Heap sort: no recursion and no extra memory, whatever the order the elements arrive in. */
CRT void qsort_s(void *data, size_t count, size_t size, int (__cdecl *compare)(void *, const void *, const void *), void *context)
{
	unsigned char *base = data;
	if (count < 2 || !size)
		return;
	for (size_t start = count / 2; start-- > 0;)
		for (size_t root = start, child; (child = root * 2 + 1) < count; root = child) {
			if (child + 1 < count && compare(context, base + child * size, base + (child + 1) * size) < 0)
				++child;
			if (compare(context, base + root * size, base + child * size) >= 0)
				break;
			swap_elements(base + root * size, base + child * size, size);
		}
	for (size_t end = count - 1; end > 0; --end) {
		swap_elements(base, base + end * size, size);
		for (size_t root = 0, child; (child = root * 2 + 1) < end; root = child) {
			if (child + 1 < end && compare(context, base + child * size, base + (child + 1) * size) < 0)
				++child;
			if (compare(context, base + root * size, base + child * size) >= 0)
				break;
			swap_elements(base + root * size, base + child * size, size);
		}
	}
}

CRT void *bsearch_s(const void *key, const void *data, size_t count, size_t size, int (__cdecl *compare)(void *, const void *, const void *),
	void *context)
{
	const unsigned char *base = data;
	size_t low = 0, high = count;
	while (low < high) {
		const size_t middle = low + (high - low) / 2;
		const int order = compare(context, key, base + middle * size);
		if (!order)
			return (void *)(base + middle * size);
		if (order < 0)
			high = middle;
		else
			low = middle + 1;
	}
	return NULL;
}

/* ---- what the shared Office library and the rest of the suite import besides ---- */
/* A long double is a double in this runtime, as in Microsoft's. */
__asm__(".section .drectve\n\t.ascii \" -export:strtold=raven_strtod -export:_copysign=copysign -export:_copysignf=copysignf -export:_nextafter=nextafter"
	" -export:llroundl=llround -export:lgammal=lgamma -export:__std_terminate=terminate\"\n\t.text");
CRT float lgammaf(float x) { return (float)lgamma(x); }
CRT float coshf(float x) { return (float)cosh(x); }
CRT float sinhf(float x) { return (float)sinh(x); }
CRT float acoshf(float x) { return (float)acosh(x); }
CRT float asinhf(float x) { return (float)asinh(x); }
CRT float atanhf(float x) { return (float)atanh(x); }

/* The exception flags live in both units: the low six bits of the x87 status word and of MXCSR. */
CRT int feclearexcept(int flags)
{
	unsigned status;
	if (flags & ~0x3f)
		return 1;
	__asm__ volatile("fnclex");
	__asm__ volatile("stmxcsr %0" : "=m"(status));
	status &= ~(unsigned)flags;
	__asm__ volatile("ldmxcsr %0" : : "m"(status));
	return 0;
}

/* The usable size of an aligned block: what is left of its allocation past the aligned start. */
CRT size_t _aligned_msize(void *block, size_t alignment, size_t offset)
{
	if (!block) {
		set_errno(22);
		return (size_t)-1;
	}
	char *raw = ((char **)((uintptr_t)block & ~(uintptr_t)(sizeof(void *) - 1)))[-1];
	return HeapSize(GetProcessHeap(), 0, raw) - (size_t)((char *)block - raw);
}

CRT int _heapmin(void) { return 0; }
CRT int _set_error_mode(int mode) { return 0; }

CRT int _fcvt_s(char *buffer, size_t size, double value, int count, int *point, int *sign)
{
	if (!buffer || !size || !point || !sign) {
		set_errno(22);
		return 22;
	}
	const char *digits = _fcvt(value, count, point, sign);
	if (strlen(digits) + 1 > size) {
		buffer[0] = 0;
		set_errno(34);
		return 34;
	}
	memcpy(buffer, digits, strlen(digits) + 1);
	return 0;
}

CRT char *_fullpath(char *buffer, const char *path, size_t size)
{
	wchar_t wide[MAX_PATH], whole[MAX_PATH];
	if (!path || !MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH) || !_wfullpath(whole, wide, MAX_PATH))
		return NULL;
	const int needed = WideCharToMultiByte(CP_ACP, 0, whole, -1, NULL, 0, NULL, NULL);
	if (needed <= 0)
		return NULL;
	if (!buffer) {
		if (!(buffer = malloc((size_t)needed)))
			return NULL;
		size = (size_t)needed;
	} else if ((size_t)needed > size) {
		set_errno(34);
		return NULL;
	}
	WideCharToMultiByte(CP_ACP, 0, whole, -1, buffer, (int)size, NULL, NULL);
	return buffer;
}

CRT int _get_tzname(size_t *length, char *buffer, size_t size, int index)
{
	if (!length || index < 0 || index > 1 || (buffer && !size)) {
		set_errno(22);
		return 22;
	}
	_tzset();
	*length = strlen(_tzname[index]) + 1;
	if (!buffer)
		return 0;
	if (*length > size) {
		buffer[0] = 0;
		return 34;
	}
	memcpy(buffer, _tzname[index], *length);
	return 0;
}

CRT int _isctype(int c, int mask) { return _isctype_l(c, mask, NULL); }
#undef _iswprint_l
#undef _isctype
CRT int _iswprint_l(wint_t c, _locale_t locale) { return iswprint(c); }
CRT int _wcsicmp_l(const wchar_t *a, const wchar_t *b, _locale_t locale) { return _wcsicmp(a, b); }
CRT int _wcsnicmp_l(const wchar_t *a, const wchar_t *b, size_t count, _locale_t locale) { return _wcsnicmp(a, b, count); }
CRT int _stricmp_l(const char *a, const char *b, _locale_t locale) { return _stricmp(a, b); }
CRT int _strnicmp_l(const char *a, const char *b, size_t count, _locale_t locale) { return _strnicmp(a, b, count); }

CRT wchar_t *_itow(int value, wchar_t *to, int radix)
{
	_itow_s(value, to, 34, radix);
	return to;
}

CRT wchar_t *_ultow(unsigned long value, wchar_t *to, int radix)
{
	_ultow_s(value, to, 34, radix);
	return to;
}

CRT void _wmakepath(wchar_t *path, const wchar_t *drive, const wchar_t *directory, const wchar_t *name, const wchar_t *extension)
{
	_wmakepath_s(path, 260, drive, directory, name, extension);
}

CRT int _wgetenv_s(size_t *length, wchar_t *buffer, size_t size, const wchar_t *name)
{
	if (!length || !name || (!buffer && size)) {
		set_errno(22);
		return 22;
	}
	const wchar_t *value = _wgetenv(name);
	if (buffer && size)
		buffer[0] = 0;
	*length = value ? wcslen(value) + 1 : 0;
	if (!value || !buffer)
		return 0;
	if (*length > size)
		return 34;
	memcpy(buffer, value, *length * sizeof(wchar_t));
	return 0;
}

/* A linear search with no ordering; the count is not changed. */
CRT void *_lfind(const void *key, const void *data, unsigned *count, unsigned size, int (__cdecl *compare)(const void *, const void *))
{
	const unsigned char *at = data;
	for (unsigned index = 0; index < *count; ++index, at += size)
		if (!compare(key, at))
			return (void *)at;
	return NULL;
}

/* Bytes up to and including the first `stop`; the next destination byte comes back, or NULL when `stop` was not met. */
CRT void *_memccpy(void *to, const void *from, int stop, size_t count)
{
	unsigned char *a = to;
	const unsigned char *b = from;
	while (count--) {
		*a = *b++;
		if (*a++ == (unsigned char)stop)
			return a;
	}
	return NULL;
}

CRT int _mbsnbcpy_s(unsigned char *to, size_t size, const unsigned char *from, size_t count)
{
	return strncpy_s((char *)to, size, (const char *)from, count);
}

CRT int mblen(const char *text, size_t count)
{
	if (!text || !*text || !count)
		return 0;
	return mb_lead((unsigned char)*text) ? (count >= 2 && text[1] ? 2 : -1) : 1;
}

CRT int _ismbcalpha(unsigned code) { return code < 0x100 && ((code | 32) >= 'a' && (code | 32) <= 'z'); }
CRT int _ismbcdigit(unsigned code) { return code >= '0' && code <= '9'; }
CRT int _ismbcalnum(unsigned code) { return _ismbcalpha(code) || _ismbcdigit(code); }
CRT int _ismbcprint(unsigned code) { return code >= 0x100 || (code >= 0x20 && code < 0x7f); }
CRT unsigned _mbctolower(unsigned code) { return mb_lower(code); }
CRT unsigned _mbctoupper(unsigned code) { return mb_upper(code); }

/* _getch reads standard input directly and keeps no key aside, so a key cannot be pushed back: the call reports failure. */
CRT int _ungetch(int key) { return -1; }

CRT int asctime_s(char *buffer, size_t size, const RavenTime *time)
{
	static const char days[] = "SunMonTueWedThuFriSat", months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	if (!buffer || size < 26 || !time || time->month < 0 || time->month > 11 || time->weekday < 0 || time->weekday > 6) {
		if (buffer && size)
			buffer[0] = 0;
		return 22;
	}
	print_into(buffer, size, "%.3s %.3s %02d %02d:%02d:%02d %d\n", days + time->weekday * 3, months + time->month * 3, time->day,
		time->hour, time->minute, time->second, time->year + 1900);
	return 0;
}

/* The exception a catch block is handling, as the runtime's own frames record it, kept per thread. */
static DWORD exception_slot = TLS_OUT_OF_INDEXES;

static void **exception_pair(void)
{
	if (exception_slot == TLS_OUT_OF_INDEXES) {
		DWORD slot = TlsAlloc();
		if (InterlockedCompareExchange((LONG *)&exception_slot, (LONG)slot, (LONG)TLS_OUT_OF_INDEXES) != (LONG)TLS_OUT_OF_INDEXES)
			TlsFree(slot);
	}
	void **pair = TlsGetValue(exception_slot);
	if (!pair && (pair = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 2 * sizeof(void *))))
		TlsSetValue(exception_slot, pair);
	return pair;
}

CRT void **__current_exception(void) { return exception_pair(); }
CRT void **__current_exception_context(void)
{
	void **pair = exception_pair();
	return pair ? pair + 1 : NULL;
}

/* ---- the _o_ names: the same functions under the prefix newer runtime libraries import ---- */
#define SAME(name) " -export:_o_" #name "=" #name
__asm__(".section .drectve\n\t.ascii \""
	SAME(___lc_codepage_func) SAME(___lc_locale_name_func) SAME(___mb_cur_max_func) SAME(__acrt_iob_func) SAME(__pctype_func)
	SAME(__stdio_common_vfprintf) SAME(__stdio_common_vsnprintf_s) SAME(__stdio_common_vsnwprintf_s) SAME(__stdio_common_vsprintf_s)
	SAME(__stdio_common_vsscanf) SAME(__stdio_common_vswprintf_s) SAME(_callnewh) " -export:_o__calloc_base=calloc" SAME(_cexit) SAME(_close)
	SAME(_configure_narrow_argv) SAME(_crt_atexit) SAME(_errno) SAME(_execute_onexit_table) SAME(_filelengthi64)
	SAME(_initialize_narrow_environment) SAME(_initialize_onexit_table) SAME(_invalid_parameter_noinfo)
	SAME(_invalid_parameter_noinfo_noreturn) SAME(_itoa_s) SAME(_lseeki64) SAME(_ltoa) SAME(_mbscmp) SAME(_memicmp) SAME(_open_osfhandle)
	SAME(_purecall) SAME(_read) SAME(_register_onexit_function) SAME(_seh_filter_dll) SAME(_splitpath_s) SAME(_stricmp) SAME(_strlwr)
	SAME(_strlwr_s) SAME(_strnicmp) SAME(_wcsdup) SAME(_wcsicmp) SAME(_wcslwr) SAME(_wcsnicmp) SAME(_wctime64) SAME(_wdupenv_s)
	SAME(_wfsopen) SAME(_wfullpath) SAME(_wgetenv) SAME(_wmakepath_s) SAME(_wsplitpath_s) SAME(_wtoi) SAME(abort) SAME(atoi) SAME(atol)
	SAME(bsearch) SAME(calloc) SAME(fclose) SAME(fflush) SAME(fread) SAME(free) SAME(frexp) SAME(fseek) SAME(ftell) SAME(isspace)
	SAME(iswprint) SAME(iswspace) SAME(iswxdigit) SAME(localeconv) SAME(malloc) SAME(qsort) SAME(realloc) SAME(setlocale) SAME(strcat_s)
	SAME(strcpy_s) SAME(strncat_s) SAME(strncpy_s) SAME(terminate) SAME(tolower) SAME(towlower) SAME(wcscat_s) SAME(wcscpy_s)
	SAME(wcsncat_s) SAME(wcsncpy_s) SAME(wcstoul) SAME(wmemcpy_s)
	SAME(__stdio_common_vsprintf) SAME(_fpclass) SAME(_get_osfhandle) SAME(isalnum) SAME(isalpha) SAME(isdigit) SAME(isprint) SAME(toupper)
	"\"\n\t.text");
#undef SAME
