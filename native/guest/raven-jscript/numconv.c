/* SPDX-License-Identifier: MIT */
#include "js.h"

/* Numbers to decimal digits and back, exactly, without the C runtime. Digits for display are the shortest that read back as the same
 * number, found with the free-format method of Steele and White as Burger and Dybvig describe it; digits for toFixed and the others
 * come from the exact value, rounded half up; reading a decimal rounds to the nearest double, ties to even. All of it runs on unsigned
 * integers of a few thousand bits. */

#define BIG_WORDS 136

typedef struct {
	UINT32 w[BIG_WORDS];
	UINT n;
} big;

static void big_set(big *a, ULONGLONG v)
{
	a->w[0] = (UINT32)v;
	a->w[1] = (UINT32)(v >> 32);
	a->n = a->w[1] ? 2 : a->w[0] ? 1 : 0;
}

static void big_trim(big *a)
{
	while (a->n && !a->w[a->n - 1])
		a->n--;
}

static void big_mul_small(big *a, UINT32 m)
{
	ULONGLONG carry = 0;
	for (UINT i = 0; i < a->n; i++) {
		ULONGLONG p = (ULONGLONG)a->w[i] * m + carry;
		a->w[i] = (UINT32)p;
		carry = p >> 32;
	}
	if (carry && a->n < BIG_WORDS)
		a->w[a->n++] = (UINT32)carry;
}

static void big_mul_pow10(big *a, int k)
{
	for (; k >= 9; k -= 9)
		big_mul_small(a, 1000000000);
	static const UINT32 small[] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000 };
	if (k > 0)
		big_mul_small(a, small[k]);
}

static void big_shl(big *a, UINT bits)
{
	UINT words = bits / 32, shift = bits % 32;
	if (!a->n)
		return;
	UINT n = a->n + words + 1;
	if (n > BIG_WORDS)
		n = BIG_WORDS;
	for (UINT i = n; i-- > 0;) {
		UINT32 hi = i >= words && i - words < a->n ? a->w[i - words] : 0;
		UINT32 lo = i >= words + 1 && i - words - 1 < a->n ? a->w[i - words - 1] : 0;
		a->w[i] = shift ? (hi << shift) | (lo >> (32 - shift)) : hi;
	}
	a->n = n;
	big_trim(a);
}

static void big_shr1(big *a)
{
	for (UINT i = 0; i < a->n; i++)
		a->w[i] = (a->w[i] >> 1) | (i + 1 < a->n ? a->w[i + 1] << 31 : 0);
	big_trim(a);
}

static int big_cmp(const big *a, const big *b)
{
	if (a->n != b->n)
		return a->n < b->n ? -1 : 1;
	for (UINT i = a->n; i-- > 0;)
		if (a->w[i] != b->w[i])
			return a->w[i] < b->w[i] ? -1 : 1;
	return 0;
}

static void big_add(big *a, const big *b)
{
	ULONGLONG carry = 0;
	UINT n = a->n > b->n ? a->n : b->n;
	for (UINT i = 0; i < n; i++) {
		ULONGLONG s = (ULONGLONG)(i < a->n ? a->w[i] : 0) + (i < b->n ? b->w[i] : 0) + carry;
		a->w[i] = (UINT32)s;
		carry = s >> 32;
	}
	a->n = n;
	if (carry && n < BIG_WORDS)
		a->w[a->n++] = (UINT32)carry;
}

/* a -= b, where a >= b. */
static void big_sub(big *a, const big *b)
{
	LONGLONG borrow = 0;
	for (UINT i = 0; i < a->n; i++) {
		LONGLONG d = (LONGLONG)a->w[i] - (i < b->n ? b->w[i] : 0) - borrow;
		borrow = d < 0;
		a->w[i] = (UINT32)(d + (borrow ? 0x100000000LL : 0));
	}
	big_trim(a);
}

static UINT big_bits(const big *a)
{
	if (!a->n)
		return 0;
	UINT32 top = a->w[a->n - 1];
	UINT bits = 0;
	while (top)
		bits++, top >>= 1;
	return (a->n - 1) * 32 + bits;
}

static BOOL big_bit(const big *a, UINT i) { return i / 32 < a->n && (a->w[i / 32] >> (i % 32)) & 1; }

/* The digit r / s, at most 9, leaving the remainder in r. */
static int big_digit(big *r, const big *s)
{
	int d = 0;
	while (big_cmp(r, s) >= 0) {
		big_sub(r, s);
		d++;
	}
	return d;
}

/* The parts of a positive finite double: v = f * 2^e. */
static void split_double(double v, ULONGLONG *f, int *e)
{
	ULONGLONG bits;
	memcpy(&bits, &v, sizeof(bits));
	int exponent = (int)((bits >> 52) & 0x7ff);
	ULONGLONG fraction = bits & 0xfffffffffffffULL;
	if (exponent) {
		*f = fraction | (1ULL << 52);
		*e = exponent - 1075;
	} else {
		*f = fraction;
		*e = -1074;
	}
}

/* r / s is v scaled so that r < s, and point is where the decimal point goes: v = 0.ddd * 10^point. */
static void scaled(double v, big *r, big *s, int *point)
{
	ULONGLONG f;
	int e;
	split_double(v, &f, &e);
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
		k++;
	}
	for (;;) {
		big t = *r;
		big_mul_small(&t, 10);
		if (big_cmp(&t, s) >= 0)
			break;
		*r = t;
		k--;
	}
	*point = k;
}

int js_shortest_digits(double v, char *digits, int *point)
{
	ULONGLONG f;
	int e;
	split_double(v, &f, &e);
	BOOL even = (f & 1) == 0;
	big r, s, plus, minus;
	/* r / s is v, and the halfway points to its neighbours are (r - minus) / s and (r + plus) / s, all over 2. */
	big_set(&r, f);
	big_set(&s, 1);
	big_set(&plus, 1);
	big_set(&minus, 1);
	BOOL uneven = f == (1ULL << 52) && e > -1074;
	if (e >= 0) {
		big_shl(&r, (UINT)e);
		big_shl(&plus, (UINT)e);
		big_shl(&minus, (UINT)e);
	} else
		big_shl(&s, (UINT)-e);
	/* Everything twice, so the half gaps are whole; at a power of two the gap below is half the one above. */
	big_shl(&r, uneven ? 2 : 1);
	big_shl(&s, uneven ? 2 : 1);
	if (uneven)
		big_shl(&plus, 1);
	int k = (int)((e + 52) * 0.30102999566398114) + 1;
	if (k > 0)
		big_mul_pow10(&s, k);
	else {
		big_mul_pow10(&r, -k);
		big_mul_pow10(&plus, -k);
		big_mul_pow10(&minus, -k);
	}
	for (;;) {
		big high = r;
		big_add(&high, &plus);
		int c = big_cmp(&high, &s);
		if (even ? c < 0 : c <= 0)
			break;
		big_mul_small(&s, 10);
		k++;
	}
	for (;;) {
		big high = r;
		big_add(&high, &plus);
		big_mul_small(&high, 10);
		int c = big_cmp(&high, &s);
		if (even ? c >= 0 : c > 0)
			break;
		big_mul_small(&r, 10);
		big_mul_small(&plus, 10);
		big_mul_small(&minus, 10);
		k--;
	}
	*point = k;
	int count = 0;
	for (;;) {
		big_mul_small(&r, 10);
		big_mul_small(&plus, 10);
		big_mul_small(&minus, 10);
		int d = big_digit(&r, &s);
		int c = big_cmp(&r, &minus);
		BOOL low = even ? c <= 0 : c < 0;
		big high = r;
		big_add(&high, &plus);
		c = big_cmp(&high, &s);
		BOOL up = even ? c >= 0 : c > 0;
		if (!low && !up && count < 30) {
			digits[count++] = (char)('0' + d);
			continue;
		}
		if (low && up) {
			big twice = r;
			big_shl(&twice, 1);
			up = big_cmp(&twice, &s) >= 0;
		}
		digits[count++] = (char)('0' + d + (up ? 1 : 0));
		break;
	}
	digits[count] = 0;
	return count;
}

/* The first count digits of r / s, rounded half up by the next one. */
static int round_digits(big *r, const big *s, int count, char *digits, int *point)
{
	if (count < 0) {
		digits[0] = 0;
		return 0;
	}
	for (int i = 0; i <= count; i++) {
		big_mul_small(r, 10);
		digits[i] = (char)('0' + big_digit(r, s));
	}
	BOOL up = digits[count] >= '5';
	digits[count] = 0;
	if (!up)
		return count;
	int i = count;
	while (i > 0 && digits[i - 1] == '9')
		digits[--i] = '0';
	if (i > 0) {
		digits[i - 1]++;
		return count;
	}
	/* 999 rounds to 1000: the digits are 100 and the point moves one on. */
	digits[0] = '1';
	(*point)++;
	if (!count) {
		digits[1] = 0;
		return 1;
	}
	return count;
}

int js_exact_digits(double v, int count, char *digits, int *point)
{
	big r, s;
	scaled(v, &r, &s, point);
	return round_digits(&r, &s, count, digits, point);
}

int js_fixed_digits(double v, int fraction, char *digits, int *point)
{
	big r, s;
	scaled(v, &r, &s, point);
	return round_digits(&r, &s, *point + fraction, digits, point);
}

/* A double from q * 2^b, where sticky says there was more below q. q has at least 55 bits unless it is exact. */
static double make_double(ULONGLONG q, int b, BOOL sticky)
{
	if (!q)
		return 0;
	int length = 64;
	while (!(q >> (length - 1)))
		length--;
	int lead = length - 1 + b;
	if (lead > 1023)
		return js_infinity();
	int keep = lead >= -1022 ? 53 : 53 - (-1022 - lead);
	if (keep < 0)
		return 0;
	int drop = length - keep;
	ULONGLONG mantissa;
	if (drop > 0) {
		mantissa = drop >= 64 ? 0 : q >> drop;
		BOOL half = drop <= 64 && ((q >> (drop - 1)) & 1);
		BOOL rest = sticky || (drop >= 2 && (q & ((drop - 1 >= 64 ? ~0ULL : (1ULL << (drop - 1)) - 1))));
		if (half && (rest || (mantissa & 1)))
			mantissa++;
	} else
		mantissa = q << -drop;
	ULONGLONG bits;
	if (lead >= -1022) {
		if (mantissa >> 53) {
			mantissa >>= 1;
			lead++;
			if (lead > 1023)
				return js_infinity();
		}
		bits = ((ULONGLONG)(lead + 1023) << 52) | (mantissa & 0xfffffffffffffULL);
	} else
		/* A subnormal: the mantissa is the bits, and one that rounded up to 2^52 is the smallest normal. */
		bits = mantissa;
	double v;
	memcpy(&v, &bits, sizeof(v));
	return v;
}

/* The top 64 bits of a, with the shift that puts them back and whether anything below them was set. */
static ULONGLONG big_top(const big *a, int *shift, BOOL *sticky)
{
	UINT bits = big_bits(a);
	*sticky = FALSE;
	if (bits <= 64) {
		*shift = 0;
		return a->n ? ((ULONGLONG)(a->n > 1 ? a->w[1] : 0) << 32) | a->w[0] : 0;
	}
	UINT low = bits - 64;
	ULONGLONG q = 0;
	for (UINT i = 0; i < 64; i++)
		if (big_bit(a, low + i))
			q |= 1ULL << i;
	for (UINT i = 0; i < low / 32 && !*sticky; i++)
		*sticky = a->w[i] != 0;
	for (UINT i = low / 32 * 32; i < low && !*sticky; i++)
		*sticky = big_bit(a, i);
	*shift = (int)low;
	return q;
}

/* The double nearest to the integer of the digits times ten to exponent. */
static double decimal_value(const char *digits, int count, int exponent, BOOL dropped)
{
	while (count && digits[0] == '0')
		digits++, count--;
	if (!count)
		return 0;
	if (exponent + count > 310)
		return js_infinity();
	if (exponent + count < -330)
		return 0;
	/* Few digits and a small power of ten are exact as doubles, and one operation on them rounds once. */
	if (count <= 15 && !dropped && exponent >= -22 && exponent <= 22) {
		double d = 0, p = 1;
		for (int i = 0; i < count; i++)
			d = d * 10 + (digits[i] - '0');
		for (int i = 0; i < (exponent < 0 ? -exponent : exponent); i++)
			p *= 10;
		return exponent < 0 ? d / p : d * p;
	}
	big n, p;
	big_set(&n, 0);
	for (int i = 0; i < count; i++) {
		big_mul_small(&n, 10);
		big one;
		big_set(&one, (ULONGLONG)(digits[i] - '0'));
		big_add(&n, &one);
	}
	if (exponent >= 0) {
		big_mul_pow10(&n, exponent);
		int shift;
		BOOL sticky;
		ULONGLONG q = big_top(&n, &shift, &sticky);
		return make_double(q, shift, sticky || dropped);
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
	for (int i = s; i >= 0; i--) {
		q <<= 1;
		if (big_cmp(&n, &p) >= 0) {
			big_sub(&n, &p);
			q |= 1;
		}
		big_shr1(&p);
	}
	return make_double(q, -b, n.n != 0 || dropped);
}

double js_strtod(const char *text)
{
	/* Digits with an optional point and exponent, as js_string_number has checked them. */
	char digits[800];
	int count = 0, exponent = 0;
	BOOL dropped = FALSE, negative = FALSE;
	const char *c = text;
	if (*c == '+' || *c == '-')
		negative = *c++ == '-';
	for (; *c >= '0' && *c <= '9'; c++) {
		if (count < (int)sizeof(digits))
			digits[count++] = *c;
		else {
			exponent++;
			dropped |= *c != '0';
		}
	}
	if (*c == '.')
		for (c++; *c >= '0' && *c <= '9'; c++) {
			if (count < (int)sizeof(digits)) {
				digits[count++] = *c;
				exponent--;
			} else
				dropped |= *c != '0';
		}
	if (*c == 'e' || *c == 'E') {
		c++;
		BOOL minus = FALSE;
		if (*c == '+' || *c == '-')
			minus = *c++ == '-';
		int e = 0;
		for (; *c >= '0' && *c <= '9'; c++)
			if (e < 100000)
				e = e * 10 + (*c - '0');
		exponent += minus ? -e : e;
	}
	double v = decimal_value(digits, count, exponent, dropped);
	return negative ? -v : v;
}
