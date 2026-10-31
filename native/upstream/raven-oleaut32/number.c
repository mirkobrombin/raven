/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "oa.h"
#include "number.h"

#define CY_SCALE 10000
#define DEC_MAX_SCALE 28

/* Half way cases go to the even neighbour, in every conversion of this family. */
double oa_round_even(double value)
{
	double whole, gap;
	if (value >= 4503599627370496.0 || value <= -4503599627370496.0)
		return value;
	whole = (double)(LONGLONG)value;
	gap = value - whole;
	if (gap > 0.5 || (gap == 0.5 && ((LONGLONG)whole & 1)))
		whole += 1.0;
	else if (gap < -0.5 || (gap == -0.5 && ((LONGLONG)whole & 1)))
		whole -= 1.0;
	return whole;
}

HRESULT oa_r8_to_i8(double value, LONGLONG *out)
{
	double whole;
	if (value != value)
		return DISP_E_OVERFLOW;
	whole = oa_round_even(value);
	if (whole >= 9223372036854775808.0 || whole < -9223372036854775808.0)
		return DISP_E_OVERFLOW;
	*out = (LONGLONG)whole;
	return S_OK;
}

HRESULT oa_r8_to_u8(double value, ULONGLONG *out)
{
	double whole;
	if (value != value)
		return DISP_E_OVERFLOW;
	whole = oa_round_even(value);
	if (whole >= 18446744073709551616.0 || whole < 0.0)
		return DISP_E_OVERFLOW;
	*out = whole >= 9223372036854775808.0 ? (ULONGLONG)(LONGLONG)(whole - 9223372036854775808.0) + 0x8000000000000000ull : (ULONGLONG)(LONGLONG)whole;
	return S_OK;
}

LONGLONG oa_cy_to_i8(LONGLONG scaled)
{
	LONGLONG whole = scaled / CY_SCALE, rest = scaled % CY_SCALE;
	if (rest >= CY_SCALE / 2 && (rest > CY_SCALE / 2 || whole & 1))
		whole++;
	else if (rest <= -CY_SCALE / 2 && (rest < -CY_SCALE / 2 || whole & 1))
		whole--;
	return whole;
}

/* --- Decimal arithmetic, on little endian arrays of 32 bit words. --- */

static inline void dec_get(const DECIMAL *value, DWORD words[3])
{
	words[0] = (DWORD)DEC_LO64(value);
	words[1] = (DWORD)(DEC_LO64(value) >> 32);
	words[2] = DEC_HI32(value);
}

static inline void dec_put(DECIMAL *value, const DWORD words[3], BYTE scale, BOOL negative)
{
	DEC_HI32(value) = words[2];
	DEC_LO64(value) = ((ULONGLONG)words[1] << 32) | words[0];
	DEC_SCALE(value) = scale;
	DEC_SIGN(value) = negative ? DECIMAL_NEG : 0;
}

static BOOL words_zero(const DWORD *words, int count)
{
	while (count--)
		if (words[count])
			return FALSE;
	return TRUE;
}

static DWORD words_mul(DWORD *words, int count, DWORD factor, DWORD carry)
{
	int index;
	for (index = 0; index < count; index++)
	{
		ULONGLONG product = (ULONGLONG)words[index] * factor + carry;
		words[index] = (DWORD)product;
		carry = (DWORD)(product >> 32);
	}
	return carry;
}

static DWORD words_div(DWORD *words, int count, DWORD divisor)
{
	ULONGLONG rest = 0;
	while (count--)
	{
		ULONGLONG part = (rest << 32) | words[count];
		words[count] = (DWORD)(part / divisor);
		rest = part % divisor;
	}
	return (DWORD)rest;
}

static DWORD words_add(DWORD *to, const DWORD *from, int count)
{
	ULONGLONG carry = 0;
	int index;
	for (index = 0; index < count; index++)
	{
		carry += (ULONGLONG)to[index] + from[index];
		to[index] = (DWORD)carry;
		carry >>= 32;
	}
	return (DWORD)carry;
}

static void words_sub(DWORD *to, const DWORD *from, int count)
{
	LONGLONG borrow = 0;
	int index;
	for (index = 0; index < count; index++)
	{
		LONGLONG part = (LONGLONG)to[index] - from[index] + borrow;
		to[index] = (DWORD)part;
		borrow = part < 0 ? -1 : 0;
	}
}

static int words_cmp(const DWORD *left, const DWORD *right, int count)
{
	while (count--)
		if (left[count] != right[count])
			return left[count] < right[count] ? -1 : 1;
	return 0;
}

/* Divides by ten, remembering what was dropped so the last step can round half to even. */
static void round_drop(DWORD *words, int count, int *scale, DWORD *last, BOOL *sticky)
{
	if (*last)
		*sticky = TRUE;
	*last = words_div(words, count, 10);
	(*scale)--;
}

/* Brings a long result down to 96 bits and a legal scale; fails when it cannot fit. */
static HRESULT dec_settle(DWORD *words, int count, int scale, BOOL negative, DECIMAL *out)
{
	DWORD last = 0;
	BOOL sticky = FALSE;
	while (scale > DEC_MAX_SCALE || (scale > 0 && !words_zero(words + 3, count - 3)))
		round_drop(words, count, &scale, &last, &sticky);
	if (!words_zero(words + 3, count - 3))
		return DISP_E_OVERFLOW;
	if (last > 5 || (last == 5 && (sticky || words[0] & 1)))
	{
		static const DWORD one[3] = {1, 0, 0};
		if (words_add(words, one, 3))
		{
			/* Rounding spilled: give up one more digit if a scale is left to give. */
			if (!scale)
				return DISP_E_OVERFLOW;
			{
				DWORD wide[4] = {words[0], words[1], words[2], 1};
				words_div(wide, 4, 10);
				memcpy(words, wide, 3 * sizeof(DWORD));
				scale--;
			}
		}
	}
	dec_put(out, words, (BYTE)scale, negative);
	return S_OK;
}

static HRESULT dec_validate(const DECIMAL *value)
{
	if (DEC_SCALE(value) > DEC_MAX_SCALE || (DEC_SIGN(value) & ~DECIMAL_NEG))
		return E_INVALIDARG;
	return S_OK;
}

/* Multiplies up to a target scale on a six word working copy. */
static void dec_widen(const DECIMAL *value, int target, DWORD words[7])
{
	int scale = DEC_SCALE(value);
	memset(words, 0, 7 * sizeof(DWORD));
	dec_get(value, words);
	while (scale < target)
	{
		words_mul(words, 7, 10, 0);
		scale++;
	}
}

HRESULT WINAPI VarDecAdd(DECIMAL *left, DECIMAL *right, DECIMAL *out)
{
	DWORD a[7], b[7], sum[8];
	int scale;
	BOOL left_neg, right_neg, negative;
	HRESULT result = dec_validate(left);
	if (SUCCEEDED(result))
		result = dec_validate(right);
	if (FAILED(result))
		return result;
	scale = max(DEC_SCALE(left), DEC_SCALE(right));
	dec_widen(left, scale, a);
	dec_widen(right, scale, b);
	left_neg = (DEC_SIGN(left) & DECIMAL_NEG) != 0;
	right_neg = (DEC_SIGN(right) & DECIMAL_NEG) != 0;
	memset(sum, 0, sizeof(sum));
	if (left_neg == right_neg)
	{
		memcpy(sum, a, sizeof(a));
		sum[7] = words_add(sum, b, 7);
		negative = left_neg;
	}
	else if (words_cmp(a, b, 7) >= 0)
	{
		memcpy(sum, a, sizeof(a));
		words_sub(sum, b, 7);
		negative = left_neg;
	}
	else
	{
		memcpy(sum, b, sizeof(b));
		words_sub(sum, a, 7);
		negative = right_neg;
	}
	return dec_settle(sum, 8, scale, negative, out);
}

HRESULT WINAPI VarDecSub(DECIMAL *left, DECIMAL *right, DECIMAL *out)
{
	DECIMAL flipped = *right;
	DEC_SIGN(&flipped) ^= DECIMAL_NEG;
	return VarDecAdd(left, &flipped, out);
}

HRESULT WINAPI VarDecMul(DECIMAL *left, DECIMAL *right, DECIMAL *out)
{
	DWORD a[3], b[3], product[6] = {0};
	int i, j, scale;
	HRESULT result = dec_validate(left);
	if (SUCCEEDED(result))
		result = dec_validate(right);
	if (FAILED(result))
		return result;
	dec_get(left, a);
	dec_get(right, b);
	for (i = 0; i < 3; i++)
	{
		ULONGLONG carry = 0;
		for (j = 0; j < 3; j++)
		{
			carry += (ULONGLONG)a[i] * b[j] + product[i + j];
			product[i + j] = (DWORD)carry;
			carry >>= 32;
		}
		product[i + 3] = (DWORD)carry;
	}
	scale = DEC_SCALE(left) + DEC_SCALE(right);
	return dec_settle(product, 6, scale, ((DEC_SIGN(left) ^ DEC_SIGN(right)) & DECIMAL_NEG) != 0, out);
}

/* Long division digit by digit: as many digits as fit, then the settling rounds. */
HRESULT WINAPI VarDecDiv(DECIMAL *left, DECIMAL *right, DECIMAL *out)
{
	DWORD top[8] = {0}, bottom[8] = {0}, quotient[8] = {0}, rest[8];
	int scale;
	BOOL negative;
	HRESULT result = dec_validate(left);
	if (SUCCEEDED(result))
		result = dec_validate(right);
	if (FAILED(result))
		return result;
	dec_get(left, top);
	dec_get(right, bottom);
	if (words_zero(bottom, 3))
		return DISP_E_DIVBYZERO;
	negative = ((DEC_SIGN(left) ^ DEC_SIGN(right)) & DECIMAL_NEG) != 0;
	scale = (int)DEC_SCALE(left) - (int)DEC_SCALE(right);
	{
		/* Integer part by repeated subtraction of shifted divisors. */
		DWORD shifted[8];
		int bit;
		memset(rest, 0, sizeof(rest));
		for (bit = 96 - 1; bit >= 0; bit--)
		{
			words_mul(rest, 8, 2, (top[bit / 32] >> (bit % 32)) & 1);
			words_mul(quotient, 8, 2, 0);
			if (words_cmp(rest, bottom, 8) >= 0)
			{
				words_sub(rest, bottom, 8);
				quotient[0] |= 1;
			}
		}
		(void)shifted;
	}
	/* Then decimal digits while there is both a remainder and room for them. */
	while (scale < DEC_MAX_SCALE + 1 && !words_zero(rest, 8) && !quotient[4])
	{
		DWORD digit = 0, probe[8];
		memcpy(probe, quotient, sizeof(probe));
		if (words_mul(probe, 8, 10, 0) || probe[5])
			break;
		words_mul(rest, 8, 10, 0);
		while (words_cmp(rest, bottom, 8) >= 0)
		{
			words_sub(rest, bottom, 8);
			digit++;
		}
		words_mul(quotient, 8, 10, digit);
		scale++;
	}
	/* A negative scale left over means whole number zeros still to add. */
	while (scale < 0)
	{
		if (words_mul(quotient, 8, 10, 0))
			return DISP_E_OVERFLOW;
		scale++;
	}
	result = dec_settle(quotient, 8, scale, negative, out);
	if (SUCCEEDED(result))
	{
		/* The shortest form: exact answers carry no trailing zeros. */
		DWORD words[3];
		dec_get(out, words);
		while (DEC_SCALE(out) > 0)
		{
			DWORD test[3];
			memcpy(test, words, sizeof(test));
			if (words_div(test, 3, 10))
				break;
			memcpy(words, test, sizeof(test));
			dec_put(out, words, DEC_SCALE(out) - 1, negative);
		}
	}
	return result;
}

HRESULT WINAPI VarDecAbs(DECIMAL *in, DECIMAL *out)
{
	*out = *in;
	DEC_SIGN(out) &= ~DECIMAL_NEG;
	return S_OK;
}

HRESULT WINAPI VarDecNeg(DECIMAL *in, DECIMAL *out)
{
	*out = *in;
	DEC_SIGN(out) ^= DECIMAL_NEG;
	return S_OK;
}

/* Drops the fraction toward zero. */
HRESULT WINAPI VarDecFix(DECIMAL *in, DECIMAL *out)
{
	DWORD words[3];
	int scale;
	if (DEC_SIGN(in) & ~DECIMAL_NEG)
		return E_INVALIDARG;
	dec_get(in, words);
	for (scale = DEC_SCALE(in); scale > 0; scale--)
		words_div(words, 3, 10);
	dec_put(out, words, 0, (DEC_SIGN(in) & DECIMAL_NEG) != 0);
	return S_OK;
}

/* Drops the fraction toward negative infinity. */
HRESULT WINAPI VarDecInt(DECIMAL *in, DECIMAL *out)
{
	DWORD words[3];
	int scale;
	BOOL dropped = FALSE;
	if (DEC_SIGN(in) & ~DECIMAL_NEG)
		return E_INVALIDARG;
	dec_get(in, words);
	for (scale = DEC_SCALE(in); scale > 0; scale--)
		if (words_div(words, 3, 10))
			dropped = TRUE;
	if (dropped && DEC_SIGN(in) & DECIMAL_NEG)
	{
		static const DWORD one[3] = {1, 0, 0};
		if (words_add(words, one, 3))
			return DISP_E_OVERFLOW;
	}
	dec_put(out, words, 0, (DEC_SIGN(in) & DECIMAL_NEG) != 0);
	return S_OK;
}

HRESULT WINAPI VarDecRound(DECIMAL *in, int places, DECIMAL *out)
{
	DWORD words[4] = {0};
	int scale;
	if (places < 0 || DEC_SIGN(in) & ~DECIMAL_NEG || DEC_SCALE(in) > DEC_MAX_SCALE)
		return E_INVALIDARG;
	if (places >= DEC_SCALE(in))
	{
		*out = *in;
		return S_OK;
	}
	dec_get(in, words);
	scale = DEC_SCALE(in);
	{
		DWORD last = 0;
		BOOL sticky = FALSE;
		while (scale > places)
			round_drop(words, 4, &scale, &last, &sticky);
		if (last > 5 || (last == 5 && (sticky || words[0] & 1)))
		{
			static const DWORD one[4] = {1, 0, 0, 0};
			words_add(words, one, 4);
		}
	}
	if (words[3])
		return DISP_E_OVERFLOW;
	dec_put(out, words, (BYTE)scale, (DEC_SIGN(in) & DECIMAL_NEG) != 0);
	return S_OK;
}

HRESULT WINAPI VarDecCmp(DECIMAL *left, DECIMAL *right)
{
	DWORD a[7], b[7];
	BOOL left_neg, right_neg, left_zero, right_zero;
	int scale, order;
	if (!left || !right)
		return E_INVALIDARG;
	if ((DEC_SIGN(left) & ~DECIMAL_NEG) || (DEC_SIGN(right) & ~DECIMAL_NEG))
		return E_INVALIDARG;
	left_neg = (DEC_SIGN(left) & DECIMAL_NEG) != 0;
	right_neg = (DEC_SIGN(right) & DECIMAL_NEG) != 0;
	left_zero = !DEC_HI32(left) && !DEC_LO64(left);
	right_zero = !DEC_HI32(right) && !DEC_LO64(right);
	if (left_zero && right_zero)
		return VARCMP_EQ;
	if (left_neg != right_neg && !(left_zero || right_zero))
		return left_neg ? VARCMP_LT : VARCMP_GT;
	scale = max(DEC_SCALE(left), DEC_SCALE(right));
	dec_widen(left, scale, a);
	dec_widen(right, scale, b);
	if (left_zero)
		return right_neg ? VARCMP_GT : VARCMP_LT;
	if (right_zero)
		return left_neg ? VARCMP_LT : VARCMP_GT;
	order = words_cmp(a, b, 7);
	if (left_neg)
		order = -order;
	return order < 0 ? VARCMP_LT : order > 0 ? VARCMP_GT : VARCMP_EQ;
}

HRESULT WINAPI VarDecCmpR8(DECIMAL *left, double right)
{
	DECIMAL other;
	HRESULT result = VarDecFromR8(right, &other);
	return SUCCEEDED(result) ? VarDecCmp(left, &other) : result;
}

/* Integer view of a decimal, rounded half to even; used by the integer conversions. */
HRESULT oa_dec_to_i8(const DECIMAL *value, LONGLONG *out)
{
	DECIMAL rounded;
	HRESULT result = VarDecRound((DECIMAL *)value, 0, &rounded);
	if (FAILED(result))
		return result;
	if (DEC_HI32(&rounded))
		return DISP_E_OVERFLOW;
	if (DEC_SIGN(&rounded) & DECIMAL_NEG)
	{
		if (DEC_LO64(&rounded) > 0x8000000000000000ull)
			return DISP_E_OVERFLOW;
		*out = -(LONGLONG)DEC_LO64(&rounded);
	}
	else
	{
		if (DEC_LO64(&rounded) > 0x7fffffffffffffffull)
			return DISP_E_OVERFLOW;
		*out = (LONGLONG)DEC_LO64(&rounded);
	}
	return S_OK;
}

HRESULT oa_dec_to_u8(const DECIMAL *value, ULONGLONG *out)
{
	DECIMAL rounded;
	HRESULT result = VarDecRound((DECIMAL *)value, 0, &rounded);
	if (FAILED(result))
		return result;
	if (DEC_HI32(&rounded) || (DEC_SIGN(&rounded) & DECIMAL_NEG && DEC_LO64(&rounded)))
		return DISP_E_OVERFLOW;
	*out = DEC_LO64(&rounded);
	return S_OK;
}

double oa_dec_to_r8(const DECIMAL *value)
{
	long double total = (long double)DEC_HI32(value) * 18446744073709551616.0L + (long double)DEC_LO64(value);
	int scale;
	for (scale = DEC_SCALE(value); scale > 0; scale--)
		total /= 10.0L;
	return (double)(DEC_SIGN(value) & DECIMAL_NEG ? -total : total);
}

static void dec_from_u8(ULONGLONG value, BOOL negative, DECIMAL *out)
{
	DEC_HI32(out) = 0;
	DEC_LO64(out) = value;
	DEC_SCALE(out) = 0;
	DEC_SIGN(out) = negative ? DECIMAL_NEG : 0;
}

void oa_dec_from_i8(LONGLONG value, DECIMAL *out)
{
	if (value < 0)
		dec_from_u8((ULONGLONG)0 - (ULONGLONG)value, TRUE, out);
	else
		dec_from_u8((ULONGLONG)value, FALSE, out);
}

void oa_dec_from_u8(ULONGLONG value, DECIMAL *out)
{
	dec_from_u8(value, FALSE, out);
}

HRESULT oa_dec_from_parts(const BYTE *digits, ULONG count, int exponent, BOOL negative, DECIMAL *out)
{
	DWORD words[6] = {0};
	ULONG index;
	int scale = 0;
	for (index = 0; index < count; index++)
		words_mul(words, 6, 10, digits[index]);
	while (exponent > 0)
	{
		if (words_mul(words, 6, 10, 0))
			return DISP_E_OVERFLOW;
		exponent--;
	}
	scale = -exponent;
	return dec_settle(words, 6, scale, negative, out);
}

/* --- Currency, a 64 bit count of ten thousandths. --- */

HRESULT oa_cy_from_r8(double value, CY *out)
{
	long double scaled = (long double)value * CY_SCALE, whole;
	LONGLONG result, rest;
	if (scaled >= 9223372036854775807.0L || scaled <= -9223372036854775808.0L || scaled != scaled)
		return DISP_E_OVERFLOW;
	result = (LONGLONG)scaled;
	whole = scaled - (long double)result;
	rest = result & 1;
	if (whole > 0.5L || (whole == 0.5L && rest))
		result++;
	else if (whole < -0.5L || (whole == -0.5L && rest))
		result--;
	out->int64 = result;
	return S_OK;
}

HRESULT oa_cy_from_i8(LONGLONG value, CY *out)
{
	if (value >= 922337203685477ll || value <= -922337203685477ll)
		return DISP_E_OVERFLOW;
	out->int64 = value * CY_SCALE;
	return S_OK;
}

HRESULT WINAPI VarCyAdd(CY left, CY right, CY *out)
{
	LONGLONG sum = (LONGLONG)((ULONGLONG)left.int64 + (ULONGLONG)right.int64);
	if ((left.int64 < 0) == (right.int64 < 0) && (sum < 0) != (left.int64 < 0))
		return DISP_E_OVERFLOW;
	out->int64 = sum;
	return S_OK;
}

HRESULT WINAPI VarCySub(CY left, CY right, CY *out)
{
	LONGLONG rest = (LONGLONG)((ULONGLONG)left.int64 - (ULONGLONG)right.int64);
	if ((left.int64 < 0) != (right.int64 < 0) && (rest < 0) != (left.int64 < 0))
		return DISP_E_OVERFLOW;
	out->int64 = rest;
	return S_OK;
}

HRESULT WINAPI VarCyMul(CY left, CY right, CY *out)
{
	return oa_cy_from_r8((double)left.int64 / CY_SCALE * ((double)right.int64 / CY_SCALE), out);
}

HRESULT WINAPI VarCyMulI4(CY left, LONG right, CY *out)
{
	return oa_cy_from_r8((double)left.int64 / CY_SCALE * right, out);
}

HRESULT WINAPI VarCyMulI8(CY left, LONG64 right, CY *out)
{
	return oa_cy_from_r8((double)left.int64 / CY_SCALE * (double)right, out);
}

HRESULT WINAPI VarCyAbs(CY in, CY *out)
{
	if (in.int64 == (LONGLONG)0x8000000000000000ull)
		return DISP_E_OVERFLOW;
	out->int64 = in.int64 < 0 ? -in.int64 : in.int64;
	return S_OK;
}

HRESULT WINAPI VarCyNeg(CY in, CY *out)
{
	if (in.int64 == (LONGLONG)0x8000000000000000ull)
		return DISP_E_OVERFLOW;
	out->int64 = -in.int64;
	return S_OK;
}

HRESULT WINAPI VarCyFix(CY in, CY *out)
{
	out->int64 = in.int64 / CY_SCALE * CY_SCALE;
	return S_OK;
}

HRESULT WINAPI VarCyInt(CY in, CY *out)
{
	LONGLONG whole = in.int64 / CY_SCALE;
	if (in.int64 < 0 && in.int64 % CY_SCALE)
		whole--;
	out->int64 = whole * CY_SCALE;
	return S_OK;
}

HRESULT WINAPI VarCyRound(CY in, int places, CY *out)
{
	static const LONG steps[] = {10000, 1000, 100, 10};
	LONGLONG step, whole, rest;
	if (places < 0)
		return E_INVALIDARG;
	if (places > 3)
	{
		*out = in;
		return S_OK;
	}
	step = steps[places];
	whole = in.int64 / step;
	rest = in.int64 % step;
	if ((rest * 2 > step) || (rest * 2 == step && whole & 1))
		whole++;
	else if ((rest * 2 < -step) || (rest * 2 == -step && whole & 1))
		whole--;
	out->int64 = whole * step;
	return S_OK;
}

HRESULT WINAPI VarCyCmp(CY left, CY right)
{
	return left.int64 < right.int64 ? VARCMP_LT : left.int64 > right.int64 ? VARCMP_GT : VARCMP_EQ;
}

HRESULT WINAPI VarCyCmpR8(CY left, double right)
{
	CY other;
	HRESULT result = VarCyFromR8(right, &other);
	return SUCCEEDED(result) ? VarCyCmp(left, other) : result;
}

HRESULT WINAPI VarR4CmpR8(float left, double right)
{
	return left < right ? VARCMP_LT : left > right ? VARCMP_GT : VARCMP_EQ;
}

HRESULT WINAPI VarR8Pow(double left, double right, double *out)
{
	return oa_pow(left, right, out);
}

HRESULT WINAPI VarR8Round(double in, int places, double *out)
{
	double scale = 1.0;
	int index;
	if (places < 0)
		return E_INVALIDARG;
	for (index = 0; index < places; index++)
		scale *= 10.0;
	*out = oa_round_even(in * scale) / scale;
	return S_OK;
}
