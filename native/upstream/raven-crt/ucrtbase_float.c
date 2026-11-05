/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Floating-point numbers to decimal text and back, exactly. Digits come from the exact binary value on unsigned integers of a few
 * thousand bits and are rounded to nearest with exact halves to even, as the Universal CRT prints them; reading decimal text rounds to the nearest double, ties to
 * even. ucrtbase.c includes this file before the formatter. */

#define FLOAT_WORDS 136

typedef struct {
	UINT32 w[FLOAT_WORDS];
	UINT n;
} float_big;

typedef union {
	double value;
	ULONGLONG bits;
} float_bits;

static void big_set(float_big *a, ULONGLONG v)
{
	a->w[0] = (UINT32)v;
	a->w[1] = (UINT32)(v >> 32);
	a->n = a->w[1] ? 2 : a->w[0] ? 1 : 0;
}

static void big_mul_small(float_big *a, UINT32 m)
{
	ULONGLONG carry = 0;
	for (UINT i = 0; i < a->n; ++i) {
		const ULONGLONG t = (ULONGLONG)a->w[i] * m + carry;
		a->w[i] = (UINT32)t;
		carry = t >> 32;
	}
	if (carry && a->n < FLOAT_WORDS)
		a->w[a->n++] = (UINT32)carry;
}

static void big_mul_pow10(float_big *a, int k)
{
	while (k >= 9) {
		big_mul_small(a, 1000000000u);
		k -= 9;
	}
	static const UINT32 small[9] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
	if (k > 0)
		big_mul_small(a, small[k]);
}

static void big_shl(float_big *a, UINT bits)
{
	const UINT words = bits / 32, shift = bits % 32;
	if (!a->n)
		return;
	UINT n = a->n + words + 1;
	if (n > FLOAT_WORDS)
		n = FLOAT_WORDS;
	for (int i = (int)n - 1; i >= 0; --i) {
		const int from = i - (int)words;
		UINT32 high = from >= 0 && from < (int)a->n ? a->w[from] : 0;
		UINT32 low = from - 1 >= 0 && from - 1 < (int)a->n ? a->w[from - 1] : 0;
		a->w[i] = shift ? (high << shift) | (low >> (32 - shift)) : high;
	}
	a->n = n;
	while (a->n && !a->w[a->n - 1])
		--a->n;
}

static void big_shr1(float_big *a)
{
	for (UINT i = 0; i < a->n; ++i)
		a->w[i] = (a->w[i] >> 1) | (i + 1 < a->n ? a->w[i + 1] << 31 : 0);
	while (a->n && !a->w[a->n - 1])
		--a->n;
}

static int big_cmp(const float_big *a, const float_big *b)
{
	if (a->n != b->n)
		return a->n < b->n ? -1 : 1;
	for (int i = (int)a->n - 1; i >= 0; --i)
		if (a->w[i] != b->w[i])
			return a->w[i] < b->w[i] ? -1 : 1;
	return 0;
}

static void big_add(float_big *a, const float_big *b)
{
	ULONGLONG carry = 0;
	const UINT n = a->n > b->n ? a->n : b->n;
	for (UINT i = 0; i < n; ++i) {
		const ULONGLONG t = (ULONGLONG)(i < a->n ? a->w[i] : 0) + (i < b->n ? b->w[i] : 0) + carry;
		a->w[i] = (UINT32)t;
		carry = t >> 32;
	}
	a->n = n;
	if (carry && a->n < FLOAT_WORDS)
		a->w[a->n++] = (UINT32)carry;
}

/* a - b, where a is at least b. */
static void big_sub(float_big *a, const float_big *b)
{
	LONGLONG borrow = 0;
	for (UINT i = 0; i < a->n; ++i) {
		LONGLONG t = (LONGLONG)a->w[i] - (i < b->n ? b->w[i] : 0) - borrow;
		borrow = t < 0;
		a->w[i] = (UINT32)(t + (borrow ? 0x100000000LL : 0));
	}
	while (a->n && !a->w[a->n - 1])
		--a->n;
}

static UINT big_bits(const float_big *a)
{
	if (!a->n)
		return 0;
	UINT bits = (a->n - 1) * 32;
	for (UINT32 top = a->w[a->n - 1]; top; top >>= 1)
		++bits;
	return bits;
}

static BOOL big_bit(const float_big *a, UINT i) { return i / 32 < a->n && (a->w[i / 32] >> (i % 32)) & 1; }

/* The digit r / s, at most 9, leaving the remainder in r. */
static int big_digit(float_big *r, const float_big *s)
{
	int d = 0;
	while (big_cmp(r, s) >= 0) {
		big_sub(r, s);
		++d;
	}
	return d;
}

/* r / s is the positive finite v scaled so that r < s, and point is where the decimal point goes: v = 0.ddd * 10^point. */
static void float_scaled(double v, float_big *r, float_big *s, int *point)
{
	float_bits parts = {v};
	const int biased = (int)((parts.bits >> 52) & 0x7ff);
	const ULONGLONG fraction = parts.bits & 0xfffffffffffffULL;
	const ULONGLONG f = biased ? fraction | (1ULL << 52) : fraction;
	const int e = biased ? biased - 1075 : -1074;
	big_set(r, f);
	big_set(s, 1);
	if (e >= 0)
		big_shl(r, (UINT)e);
	else
		big_shl(s, (UINT)-e);
	/* An estimate of the point from the bit length, then corrected. */
	int k = (int)((e + 52) * 0.30102999566398114) + 1;
	if (k > 0)
		big_mul_pow10(s, k);
	else
		big_mul_pow10(r, -k);
	while (big_cmp(r, s) >= 0) {
		big_mul_small(s, 10);
		++k;
	}
	for (;;) {
		float_big t = *r;
		big_mul_small(&t, 10);
		if (big_cmp(&t, s) >= 0)
			break;
		*r = t;
		--k;
	}
	*point = k;
}

/* The first count significant digits of v, rounded to nearest, with v = 0.ddd * 10^point; zero has no digits and its point is 1. Digits
 * past the returned count read as zeros. */
static int float_digits(double v, int count, char *digits, int *point)
{
	float_big r, s;
	digits[0] = 0;
	if (v == 0 || count < 0) {
		if (v == 0)
			*point = 1;
		else
			float_scaled(v, &r, &s, point);
		return 0;
	}
	float_scaled(v, &r, &s, point);
	for (int i = 0; i <= count; ++i) {
		big_mul_small(&r, 10);
		digits[i] = (char)('0' + big_digit(&r, &s));
	}
	/* Past the last digit: above half rounds up, below down, and an exact half to the even digit, as the default rounding mode does. */
	const BOOL up = digits[count] > '5' || (digits[count] == '5' && (r.n || (count ? digits[count - 1] - '0' : 0) & 1));
	digits[count] = 0;
	if (!up)
		return count;
	int i = count;
	while (i > 0 && digits[i - 1] == '9')
		digits[--i] = '0';
	if (i > 0) {
		++digits[i - 1];
		return count;
	}
	/* 999 rounds to 1000: the digits become 100 and the point moves one on. */
	digits[0] = '1';
	++*point;
	if (!count) {
		digits[1] = 0;
		return 1;
	}
	return count;
}

/* The digits of v rounded to `fraction` places after the point. */
static int float_fixed_digits(double v, int fraction, char *digits, int *point)
{
	float_big r, s;
	if (v == 0) {
		digits[0] = 0;
		*point = 1;
		return 0;
	}
	float_scaled(v, &r, &s, point);
	return float_digits(v, *point + fraction, digits, point);
}

static double float_infinity(void)
{
	float_bits parts;
	parts.bits = 0x7ff0000000000000ULL;
	return parts.value;
}

/* A double from q * 2^b, where sticky says there was more below q. q has at least 55 bits unless it is exact. */
static double float_make(ULONGLONG q, int b, BOOL sticky)
{
	if (!q)
		return 0;
	int length = 64;
	while (!(q >> (length - 1)))
		--length;
	int lead = length - 1 + b;
	if (lead > 1023)
		return float_infinity();
	const int keep = lead >= -1022 ? 53 : 53 - (-1022 - lead);
	if (keep < 0)
		return 0;
	const int drop = length - keep;
	ULONGLONG mantissa;
	if (drop > 0) {
		mantissa = drop >= 64 ? 0 : q >> drop;
		const BOOL half = drop <= 64 && ((q >> (drop - 1)) & 1);
		const BOOL rest = sticky || (drop >= 2 && (q & (drop - 1 >= 64 ? ~0ULL : (1ULL << (drop - 1)) - 1)));
		if (half && (rest || (mantissa & 1)))
			++mantissa;
	} else
		mantissa = q << -drop;
	float_bits parts;
	if (lead >= -1022) {
		if (mantissa >> 53) {
			mantissa >>= 1;
			if (++lead > 1023)
				return float_infinity();
		}
		parts.bits = ((ULONGLONG)(lead + 1023) << 52) | (mantissa & 0xfffffffffffffULL);
	} else
		/* A subnormal: the mantissa is the bits, and one that rounded up to 2^52 is the smallest normal. */
		parts.bits = mantissa;
	return parts.value;
}

/* The top 64 bits of a, with the shift that puts them back and whether anything below them was set. */
static ULONGLONG big_top(const float_big *a, int *shift, BOOL *sticky)
{
	const UINT bits = big_bits(a);
	*sticky = FALSE;
	if (bits <= 64) {
		*shift = 0;
		return a->n ? ((ULONGLONG)(a->n > 1 ? a->w[1] : 0) << 32) | a->w[0] : 0;
	}
	const UINT low = bits - 64;
	ULONGLONG q = 0;
	for (UINT i = 0; i < 64; ++i)
		if (big_bit(a, low + i))
			q |= 1ULL << i;
	for (UINT i = 0; i < low / 32 && !*sticky; ++i)
		*sticky = a->w[i] != 0;
	for (UINT i = low / 32 * 32; i < low && !*sticky; ++i)
		*sticky = big_bit(a, i);
	*shift = (int)low;
	return q;
}

/* The double nearest to the integer of the digits times ten to exponent; dropped says nonzero digits were cut off past the end. */
static double float_decimal(const char *digits, int count, int exponent, BOOL dropped)
{
	while (count && digits[0] == '0')
		++digits, --count;
	if (!count)
		return 0;
	if (exponent + count > 310)
		return float_infinity();
	if (exponent + count < -330)
		return 0;
	/* Few digits and a small power of ten are exact as doubles, and one operation on them rounds once. */
	if (count <= 15 && !dropped && exponent >= -22 && exponent <= 22) {
		double d = 0, p = 1;
		for (int i = 0; i < count; ++i)
			d = d * 10 + (digits[i] - '0');
		for (int i = 0; i < (exponent < 0 ? -exponent : exponent); ++i)
			p *= 10;
		return exponent < 0 ? d / p : d * p;
	}
	float_big n, p;
	big_set(&n, 0);
	for (int i = 0; i < count; ++i) {
		float_big digit;
		big_mul_small(&n, 10);
		big_set(&digit, (ULONGLONG)(digits[i] - '0'));
		big_add(&n, &digit);
	}
	if (exponent >= 0) {
		int shift;
		BOOL sticky;
		big_mul_pow10(&n, exponent);
		const ULONGLONG q = big_top(&n, &shift, &sticky);
		return float_make(q, shift, sticky || dropped);
	}
	/* n / 10^-exponent: scale n up until the quotient has 55 or 56 bits, then divide bit by bit. */
	big_set(&p, 1);
	big_mul_pow10(&p, -exponent);
	int b = (int)big_bits(&p) - (int)big_bits(&n) + 55;
	if (b > 0)
		big_shl(&n, (UINT)b);
	else
		b = 0;
	int s = (int)big_bits(&n) - (int)big_bits(&p);
	if (s < 0)
		s = 0;
	big_shl(&p, (UINT)s);
	ULONGLONG q = 0;
	for (int i = s; i >= 0; --i) {
		q <<= 1;
		if (big_cmp(&n, &p) >= 0) {
			big_sub(&n, &p);
			q |= 1;
		}
		big_shr1(&p);
	}
	return float_make(q, -b, n.n != 0 || dropped);
}

static int float_lower(wchar_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* Whether text starts with the lowercase word, in any case. */
static int float_word(const wchar_t *text, const char *word)
{
	while (*word)
		if (float_lower(*text++) != *word++)
			return 0;
	return 1;
}

static int float_hex(wchar_t c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = (wchar_t)float_lower(c);
	return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* A number as strtod reads it after its blanks and sign: decimal digits with an optional point and exponent, hexadecimal digits with
 * an optional binary exponent, infinity or nan. used is how many characters it took, zero for none. */
static double float_read(const wchar_t *text, size_t *used)
{
	const wchar_t *c = text;
	*used = 0;
	if (float_word(c, "inf")) {
		*used = float_word(c, "infinity") ? 8 : 3;
		return float_infinity();
	}
	if (float_word(c, "nan")) {
		float_bits parts;
		c += 3;
		if (*c == '(') {
			const wchar_t *close = c + 1;
			while ((*close >= '0' && *close <= '9') || (float_lower(*close) >= 'a' && float_lower(*close) <= 'z') || *close == '_')
				++close;
			if (*close == ')')
				c = close + 1;
		}
		*used = (size_t)(c - text);
		parts.bits = 0x7ff8000000000000ULL;
		return parts.value;
	}
	if (c[0] == '0' && float_lower(c[1]) == 'x' && (float_hex(c[2]) >= 0 || (c[2] == '.' && float_hex(c[3]) >= 0))) {
		ULONGLONG q = 0;
		int exponent = 0;
		BOOL sticky = FALSE;
		for (c += 2; float_hex(*c) >= 0; ++c) {
			if (q >> 60) {
				sticky |= float_hex(*c) != 0;
				exponent += 4;
			} else
				q = q << 4 | (ULONGLONG)float_hex(*c);
		}
		if (*c == '.')
			for (++c; float_hex(*c) >= 0; ++c)
				if (!(q >> 60)) {
					q = q << 4 | (ULONGLONG)float_hex(*c);
					exponent -= 4;
				} else
					sticky |= float_hex(*c) != 0;
		if (float_lower(*c) == 'p' && ((c[1] >= '0' && c[1] <= '9') || ((c[1] == '-' || c[1] == '+') && c[2] >= '0' && c[2] <= '9'))) {
			BOOL minus = FALSE;
			int e = 0;
			if (*++c == '-' || *c == '+')
				minus = *c++ == '-';
			for (; *c >= '0' && *c <= '9'; ++c)
				if (e < 100000)
					e = e * 10 + (*c - '0');
			exponent += minus ? -e : e;
		}
		*used = (size_t)(c - text);
		return float_make(q, exponent, sticky);
	}
	char digits[800];
	int count = 0, exponent = 0, seen = 0;
	BOOL dropped = FALSE;
	for (; *c >= '0' && *c <= '9'; ++c, ++seen) {
		if (count < (int)sizeof digits)
			digits[count++] = (char)*c;
		else {
			++exponent;
			dropped |= *c != '0';
		}
	}
	if (*c == '.')
		for (++c; *c >= '0' && *c <= '9'; ++c, ++seen) {
			if (count < (int)sizeof digits) {
				digits[count++] = (char)*c;
				--exponent;
			} else
				dropped |= *c != '0';
		}
	if (!seen)
		return 0;
	if (float_lower(*c) == 'e' && ((c[1] >= '0' && c[1] <= '9') || ((c[1] == '-' || c[1] == '+') && c[2] >= '0' && c[2] <= '9'))) {
		BOOL minus = FALSE;
		int e = 0;
		if (*++c == '-' || *c == '+')
			minus = *c++ == '-';
		for (; *c >= '0' && *c <= '9'; ++c)
			if (e < 100000)
				e = e * 10 + (*c - '0');
		exponent += minus ? -e : e;
	}
	*used = (size_t)(c - text);
	return float_decimal(digits, count, exponent, dropped);
}

#define FLOAT_DIGITS 360

/* The fixed form of digits whose point is at `point`, with `fraction` places; missing digits are zeros. */
static int float_layout_fixed(char *text, const char *digits, int count, int point, int fraction, int alternate)
{
	int length = 0;
	if (point <= 0)
		text[length++] = '0';
	for (int i = 0; i < point; ++i)
		text[length++] = i < count ? digits[i] : '0';
	if (fraction > 0 || alternate)
		text[length++] = '.';
	for (int i = 0; i < fraction; ++i) {
		const int at = point + i;
		text[length++] = at >= 0 && at < count ? digits[at] : '0';
	}
	return length;
}

static int float_layout_exponent(char *text, const char *digits, int count, int exponent, int fraction, int alternate, int upper)
{
	int length = 0;
	text[length++] = count ? digits[0] : '0';
	if (fraction > 0 || alternate)
		text[length++] = '.';
	for (int i = 1; i <= fraction; ++i)
		text[length++] = i < count ? digits[i] : '0';
	text[length++] = upper ? 'E' : 'e';
	text[length++] = exponent < 0 ? '-' : '+';
	if (exponent < 0)
		exponent = -exponent;
	if (exponent >= 100)
		text[length++] = (char)('0' + exponent / 100);
	text[length++] = (char)('0' + exponent / 10 % 10);
	text[length++] = (char)('0' + exponent % 10);
	return length;
}

/* The text of a positive or zero finite value for the conversions f, e and g, without its sign. text holds FLOAT_DIGITS + 340. */
static int float_text(char *text, double value, wchar_t spec, int precision, int alternate)
{
	char digits[FLOAT_DIGITS + 340];
	const int upper = spec == 'E' || spec == 'G';
	int point;
	if (precision < 0)
		precision = 6;
	if (precision > FLOAT_DIGITS - 10)
		precision = FLOAT_DIGITS - 10;
	if (spec == 'f' || spec == 'F') {
		const int count = float_fixed_digits(value, precision, digits, &point);
		return float_layout_fixed(text, digits, count, point, precision, alternate);
	}
	if (spec == 'e' || spec == 'E') {
		const int count = float_digits(value, precision + 1, digits, &point);
		return float_layout_exponent(text, digits, count, value == 0 ? 0 : point - 1, precision, alternate, upper);
	}
	/* g: the exponent form when the exponent is below -4 or not below the precision, trailing zeros dropped unless # asks for them. */
	if (!precision)
		precision = 1;
	const int count = float_digits(value, precision, digits, &point);
	const int exponent = value == 0 ? 0 : point - 1;
	int length, end;
	if (exponent < -4 || exponent >= precision)
		length = float_layout_exponent(text, digits, count, exponent, precision - 1, alternate, upper);
	else
		length = float_layout_fixed(text, digits, count, point, precision - 1 - exponent, alternate);
	if (alternate)
		return length;
	int dot = -1;
	for (end = 0; end < length && text[end] != 'e' && text[end] != 'E'; ++end)
		if (text[end] == '.')
			dot = end;
	if (dot < 0)
		return length;
	int keep = end;
	while (keep > dot + 1 && text[keep - 1] == '0')
		--keep;
	if (keep == dot + 1)
		keep = dot;
	for (int i = end; i < length; ++i)
		text[keep + i - end] = text[i];
	return keep + length - end;
}
