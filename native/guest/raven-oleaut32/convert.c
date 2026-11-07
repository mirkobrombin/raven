/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "oa.h"
#include "number.h"

#define DATE_LOW -657434.0
#define DATE_HIGH 2958466.0

/* Every source reduces to one of these before any target looks at it. */
typedef enum { NUM_SIGNED, NUM_UNSIGNED, NUM_REAL, NUM_FLOAT, NUM_CY, NUM_DEC, NUM_BOOL } num_kind;

typedef struct
{
	num_kind kind;
	LONGLONG i;
	ULONGLONG u;
	double r;
	DECIMAL d;
} num;

static num num_signed(LONGLONG v) { num n; n.kind = NUM_SIGNED; n.i = v; return n; }
static num num_unsigned(ULONGLONG v) { num n; n.kind = NUM_UNSIGNED; n.u = v; return n; }
static num num_real(double v) { num n; n.kind = NUM_REAL; n.r = v; return n; }
static num num_float(float v) { num n; n.kind = NUM_FLOAT; n.r = v; return n; }
static num num_cy(CY v) { num n; n.kind = NUM_CY; n.i = v.int64; return n; }
static num num_dec(const DECIMAL *v) { num n; n.kind = NUM_DEC; n.d = *v; return n; }
static num num_bool(VARIANT_BOOL v) { num n; n.kind = NUM_BOOL; n.i = v; return n; }

static HRESULT num_i8(const num *n, LONGLONG *out)
{
	switch (n->kind)
	{
	case NUM_SIGNED:
	case NUM_BOOL:
		*out = n->i;
		return S_OK;
	case NUM_UNSIGNED:
		if (n->u > 0x7fffffffffffffffull)
			return DISP_E_OVERFLOW;
		*out = (LONGLONG)n->u;
		return S_OK;
	case NUM_REAL:
	case NUM_FLOAT:
		return oa_r8_to_i8(n->r, out);
	case NUM_CY:
		*out = oa_cy_to_i8(n->i);
		return S_OK;
	default:
		return oa_dec_to_i8(&n->d, out);
	}
}

static HRESULT num_u8(const num *n, ULONGLONG *out)
{
	switch (n->kind)
	{
	case NUM_BOOL:
		*out = (ULONGLONG)n->i;
		return S_OK;
	case NUM_SIGNED:
		if (n->i < 0)
			return DISP_E_OVERFLOW;
		*out = (ULONGLONG)n->i;
		return S_OK;
	case NUM_UNSIGNED:
		*out = n->u;
		return S_OK;
	case NUM_REAL:
	case NUM_FLOAT:
		return oa_r8_to_u8(n->r, out);
	case NUM_CY:
		{
			LONGLONG whole = oa_cy_to_i8(n->i);
			if (whole < 0)
				return DISP_E_OVERFLOW;
			*out = (ULONGLONG)whole;
			return S_OK;
		}
	default:
		return oa_dec_to_u8(&n->d, out);
	}
}

static double num_r8(const num *n)
{
	switch (n->kind)
	{
	case NUM_SIGNED:
	case NUM_BOOL:
		return (double)n->i;
	case NUM_UNSIGNED:
		return n->u > 0x7fffffffffffffffull ? (double)(LONGLONG)(n->u >> 1) * 2.0 : (double)(LONGLONG)n->u;
	case NUM_REAL:
	case NUM_FLOAT:
		return n->r;
	case NUM_CY:
		return (double)n->i / 10000.0;
	default:
		return oa_dec_to_r8(&n->d);
	}
}

/* An unsigned target takes a boolean as its bit pattern at that width. */
#define INT_STORE(Name, C, LOW, HIGH) \
	static HRESULT store_##Name(const num *n, C *out) \
	{ \
		LONGLONG value; \
		HRESULT result; \
		if (n->kind == NUM_BOOL) \
		{ \
			*out = (C)n->i; \
			return S_OK; \
		} \
		result = num_i8(n, &value); \
		if (FAILED(result)) \
			return result; \
		if (value < (LONGLONG)(LOW) || value > (LONGLONG)(HIGH)) \
			return DISP_E_OVERFLOW; \
		*out = (C)value; \
		return S_OK; \
	}

INT_STORE(I1, CHAR, -128, 127)
INT_STORE(UI1, BYTE, 0, 255)
INT_STORE(I2, SHORT, -32768, 32767)
INT_STORE(UI2, USHORT, 0, 65535)
INT_STORE(I4, LONG, -2147483647 - 1, 2147483647)
INT_STORE(UI4, ULONG, 0, 4294967295ll)

static HRESULT store_I8(const num *n, LONG64 *out)
{
	LONGLONG value;
	HRESULT result;
	/* From currency this target always steps down past a negative value's fraction. */
	if (n->kind == NUM_CY && n->i < 0)
	{
		*out = n->i / 10000 - 1;
		return S_OK;
	}
	result = num_i8(n, &value);
	if (SUCCEEDED(result))
		*out = value;
	return result;
}

static HRESULT store_UI8(const num *n, ULONG64 *out)
{
	return num_u8(n, out);
}

static HRESULT store_R4(const num *n, float *out)
{
	double value = num_r8(n);
	if (value > 3.4028234663852886e38 || value < -3.4028234663852886e38)
		return DISP_E_OVERFLOW;
	*out = (float)value;
	return S_OK;
}

static HRESULT store_R8(const num *n, double *out)
{
	*out = num_r8(n);
	return S_OK;
}

static HRESULT store_Date(const num *n, DATE *out)
{
	double value = num_r8(n);
	if (value < DATE_LOW || value >= DATE_HIGH)
		return DISP_E_OVERFLOW;
	*out = value;
	return S_OK;
}

static HRESULT store_Cy(const num *n, CY *out)
{
	switch (n->kind)
	{
	case NUM_CY:
		out->int64 = n->i;
		return S_OK;
	case NUM_SIGNED:
	case NUM_BOOL:
		return oa_cy_from_i8(n->i, out);
	case NUM_UNSIGNED:
		if (n->u > 922337203685477ull)
			return DISP_E_OVERFLOW;
		out->int64 = (LONGLONG)n->u * 10000;
		return S_OK;
	default:
		return oa_cy_from_r8(num_r8(n), out);
	}
}

static HRESULT store_Bool(const num *n, VARIANT_BOOL *out)
{
	BOOL set;
	switch (n->kind)
	{
	case NUM_SIGNED: case NUM_BOOL: case NUM_CY:
		set = n->i != 0;
		break;
	case NUM_UNSIGNED:
		set = n->u != 0;
		break;
	case NUM_REAL: case NUM_FLOAT:
		set = n->r != 0.0;
		break;
	default:
		set = DEC_HI32(&n->d) || DEC_LO64(&n->d);
		break;
	}
	*out = set ? VARIANT_TRUE : VARIANT_FALSE;
	return S_OK;
}

static HRESULT store_Dec(const num *n, DECIMAL *out)
{
	switch (n->kind)
	{
	case NUM_BOOL:
		/* A negative value of any size is one negative unit. */
		oa_dec_from_i8(n->i < 0 ? -1 : n->i != 0, out);
		return S_OK;
	case NUM_SIGNED:
		oa_dec_from_i8(n->i, out);
		return S_OK;
	case NUM_UNSIGNED:
		oa_dec_from_u8(n->u, out);
		return S_OK;
	case NUM_DEC:
		*out = n->d;
		return S_OK;
	case NUM_CY:
		oa_dec_from_i8(n->i, out);
		DEC_SCALE(out) = 4;
		return S_OK;
	default:
		{
			BYTE digits[20];
			int exponent = 0;
			ULONG count;
			double value = n->r;
			BOOL negative = value < 0;
			count = oa_digits_of_r8(negative ? -value : value, n->kind == NUM_FLOAT ? 7 : 15, digits, &exponent);
			return oa_dec_from_parts(digits, count, exponent - (int)count, negative, out);
		}
	}
}

/* --- Text sources go through the number parser, objects through their default value. --- */

static HRESULT parse_into(LPCOLESTR text, LCID lcid, ULONG flags, DWORD bits, VARIANT *out)
{
	BYTE digits[1024];
	NUMPARSE parse;
	HRESULT result;
	if (!text)
		return DISP_E_TYPEMISMATCH;
	parse.cDig = ARRAYSIZE(digits);
	parse.dwInFlags = NUMPRS_STD;
	result = VarParseNumFromStr(text, lcid, flags, &parse, digits);
	if (FAILED(result))
		return result;
	return VarNumFromParseNum(&parse, digits, bits, out);
}

static HRESULT default_value(IDispatch *object, LCID lcid, VARTYPE type, VARIANT *out)
{
	DISPPARAMS none = {NULL, NULL, 0, 0};
	VARIANT value;
	HRESULT result;
	if (!object)
		return DISP_E_TYPEMISMATCH;
	VariantInit(&value);
	result = IDispatch_Invoke(object, DISPID_VALUE, &IID_NULL, lcid, DISPATCH_PROPERTYGET, &none, &value, NULL, NULL);
	if (FAILED(result))
		return DISP_E_TYPEMISMATCH;
	result = VariantChangeTypeEx(out, &value, lcid, 0, type);
	VariantClear(&value);
	return result;
}

static HRESULT num_of_variant(const VARIANT *value, num *n)
{
	switch (V_VT(value))
	{
	case VT_I1: *n = num_signed(V_I1(value)); break;
	case VT_UI1: *n = num_unsigned(V_UI1(value)); break;
	case VT_I2: *n = num_signed(V_I2(value)); break;
	case VT_UI2: *n = num_unsigned(V_UI2(value)); break;
	case VT_I4: case VT_INT: *n = num_signed(V_I4(value)); break;
	case VT_UI4: case VT_UINT: *n = num_unsigned(V_UI4(value)); break;
	case VT_I8: *n = num_signed(V_I8(value)); break;
	case VT_UI8: *n = num_unsigned(V_UI8(value)); break;
	case VT_R4: *n = num_float(V_R4(value)); break;
	case VT_R8: *n = num_real(V_R8(value)); break;
	case VT_DATE: *n = num_real(V_DATE(value)); break;
	case VT_CY: *n = num_cy(V_CY(value)); break;
	case VT_DECIMAL: *n = num_dec(&V_DECIMAL(value)); break;
	case VT_BOOL: *n = num_bool(V_BOOL(value)); break;
	default:
		return DISP_E_TYPEMISMATCH;
	}
	return S_OK;
}

#define NUMERIC_BITS (VTBIT_I1 | VTBIT_UI1 | VTBIT_I2 | VTBIT_UI2 | VTBIT_I4 | VTBIT_UI4 | VTBIT_I8 | VTBIT_UI8 | VTBIT_R4 | VTBIT_R8 | VTBIT_CY | VTBIT_DECIMAL)

/* Text and objects reach a target through the same widest forms. */
#define FROM_ALL(Name, C, VT) \
	HRESULT WINAPI Var##Name##FromI1(CHAR in, C *out) { num n = num_signed(in); return store_##Name(&n, out); } \
	HRESULT WINAPI Var##Name##FromUI1(BYTE in, C *out) { num n = num_unsigned(in); return store_##Name(&n, out); } \
	HRESULT WINAPI Var##Name##FromI2(SHORT in, C *out) { num n = num_signed(in); return store_##Name(&n, out); } \
	HRESULT WINAPI Var##Name##FromUI2(USHORT in, C *out) { num n = num_unsigned(in); return store_##Name(&n, out); } \
	HRESULT WINAPI Var##Name##FromI4(LONG in, C *out) { num n = num_signed(in); return store_##Name(&n, out); } \
	HRESULT WINAPI Var##Name##FromUI4(ULONG in, C *out) { num n = num_unsigned(in); return store_##Name(&n, out); } \
	HRESULT WINAPI Var##Name##FromI8(LONG64 in, C *out) { num n = num_signed(in); return store_##Name(&n, out); } \
	HRESULT WINAPI Var##Name##FromUI8(ULONG64 in, C *out) { num n = num_unsigned(in); return store_##Name(&n, out); } \
	HRESULT WINAPI Var##Name##FromR4(FLOAT in, C *out) { num n = num_float(in); return store_##Name(&n, out); } \
	HRESULT WINAPI Var##Name##FromR8(DOUBLE in, C *out) { num n = num_real(in); return store_##Name(&n, out); } \
	HRESULT WINAPI Var##Name##FromCy(CY in, C *out) { num n = num_cy(in); return store_##Name(&n, out); } \
	HRESULT WINAPI Var##Name##FromDec(DECIMAL *in, C *out) \
	{ \
		num n; \
		if (DEC_SCALE(in) > 28 || DEC_SIGN(in) & ~DECIMAL_NEG) \
			return E_INVALIDARG; \
		n = num_dec(in); \
		return store_##Name(&n, out); \
	} \
	HRESULT WINAPI Var##Name##FromBool(VARIANT_BOOL in, C *out) { num n = num_bool(in); return store_##Name(&n, out); } \
	HRESULT WINAPI Var##Name##FromDisp(IDispatch *in, LCID lcid, C *out) \
	{ \
		VARIANT value; \
		HRESULT result; \
		num n; \
		VariantInit(&value); \
		result = default_value(in, lcid, VT, &value); \
		if (SUCCEEDED(result)) \
			result = num_of_variant(&value, &n); \
		if (SUCCEEDED(result)) \
			result = store_##Name(&n, out); \
		VariantClear(&value); \
		return result; \
	}

#define FROM_DATE(Name, C) \
	HRESULT WINAPI Var##Name##FromDate(DATE in, C *out) { num n = num_real(in); return store_##Name(&n, out); }

#define FROM_TEXT(Name, C, BITS) \
	HRESULT WINAPI Var##Name##FromStr(LPCOLESTR in, LCID lcid, ULONG flags, C *out) \
	{ \
		VARIANT value; \
		HRESULT result; \
		num n; \
		VariantInit(&value); \
		result = parse_into(in, lcid, flags, BITS, &value); \
		if (SUCCEEDED(result)) \
			result = num_of_variant(&value, &n); \
		if (SUCCEEDED(result)) \
			result = store_##Name(&n, out); \
		VariantClear(&value); \
		return result; \
	}

#define FAMILY(Name, C, VT, BITS) FROM_ALL(Name, C, VT) FROM_DATE(Name, C) FROM_TEXT(Name, C, BITS)

FAMILY(I1, CHAR, VT_I1, VTBIT_I1 | VTBIT_I2 | VTBIT_I4 | VTBIT_I8 | VTBIT_R8)
FAMILY(UI1, BYTE, VT_UI1, VTBIT_UI1 | VTBIT_I2 | VTBIT_I4 | VTBIT_I8 | VTBIT_R8)
FAMILY(I2, SHORT, VT_I2, VTBIT_I2 | VTBIT_I4 | VTBIT_I8 | VTBIT_R8)
FAMILY(UI2, USHORT, VT_UI2, VTBIT_UI2 | VTBIT_I4 | VTBIT_I8 | VTBIT_R8)
FAMILY(I4, LONG, VT_I4, VTBIT_I4 | VTBIT_I8 | VTBIT_R8)
FAMILY(UI4, ULONG, VT_UI4, VTBIT_UI4 | VTBIT_I8 | VTBIT_R8)
FAMILY(I8, LONG64, VT_I8, VTBIT_I8 | VTBIT_UI8 | VTBIT_DECIMAL | VTBIT_R8)
FAMILY(UI8, ULONG64, VT_UI8, VTBIT_UI8 | VTBIT_I8 | VTBIT_DECIMAL | VTBIT_R8)
FAMILY(R4, FLOAT, VT_R4, VTBIT_R8)
FAMILY(R8, DOUBLE, VT_R8, VTBIT_R8)
FAMILY(Cy, CY, VT_CY, VTBIT_CY | VTBIT_I8 | VTBIT_DECIMAL | VTBIT_R8)
FAMILY(Dec, DECIMAL, VT_DECIMAL, VTBIT_DECIMAL | VTBIT_R8)
FROM_ALL(Date, DATE, VT_DATE)
FROM_ALL(Bool, VARIANT_BOOL, VT_BOOL)
FROM_DATE(Bool, VARIANT_BOOL)
FROM_DATE(Date, DATE)

/* The locale's words count, then the fixed English and any number. */
HRESULT WINAPI VarBoolFromStr(LPCOLESTR in, LCID lcid, ULONG flags, VARIANT_BOOL *out)
{
	VARIANT value;
	HRESULT result;
	num n;
	if (!in || !out)
		return !in ? DISP_E_TYPEMISMATCH : E_INVALIDARG;
	if (!lstrcmpiW(in, L"True") || !lstrcmpW(in, L"#TRUE#"))
	{
		*out = VARIANT_TRUE;
		return S_OK;
	}
	if (!lstrcmpiW(in, L"False") || !lstrcmpW(in, L"#FALSE#"))
	{
		*out = VARIANT_FALSE;
		return S_OK;
	}
	VariantInit(&value);
	result = parse_into(in, lcid, flags, VTBIT_R8, &value);
	if (SUCCEEDED(result))
		result = num_of_variant(&value, &n);
	if (SUCCEEDED(result))
		result = store_Bool(&n, out);
	VariantClear(&value);
	return result;
}

/* --- Parsing --- */

static WCHAR locale_char(LCID lcid, LCTYPE what, ULONG flags, WCHAR fallback)
{
	WCHAR text[8];
	if (GetLocaleInfoW(lcid, what | (flags & LOCALE_NOUSEROVERRIDE), text, ARRAYSIZE(text)) >= 2)
		return text[0];
	return fallback;
}

static BOOL is_space(WCHAR c)
{
	return c == L' ' || (c >= 9 && c <= 13) || c == 0xa0;
}

static int hex_value(WCHAR c)
{
	if (c >= L'0' && c <= L'9')
		return c - L'0';
	if (c >= L'a' && c <= L'f')
		return c - L'a' + 10;
	if (c >= L'A' && c <= L'F')
		return c - L'A' + 10;
	return -1;
}

HRESULT WINAPI VarParseNumFromStr(const OLECHAR *text, LCID lcid, ULONG flags, NUMPARSE *parse, BYTE *digits)
{
	const WCHAR *at = text;
	WCHAR decimal, group, currency, negative_sign = L'-', positive_sign = L'+';
	ULONG in, kept = 0, total = 0;
	int power = 0, exponent = 0;
	BOOL seen_digit = FALSE, exp_negative = FALSE, in_fraction = FALSE, trailing_ok;
	DWORD out = 0;
	if (!parse || !digits)
		return E_POINTER;
	if (!text)
		return DISP_E_TYPEMISMATCH;
	in = parse->dwInFlags;
	parse->dwOutFlags = 0;
	parse->cchUsed = 0;
	parse->nBaseShift = 0;
	parse->nPwr10 = 0;
	decimal = locale_char(lcid, LOCALE_SDECIMAL, flags, L'.');
	group = locale_char(lcid, LOCALE_STHOUSAND, flags, L',');
	currency = locale_char(lcid, LOCALE_SCURRENCY, flags, L'$');
	if (in & NUMPRS_LEADING_WHITE)
		while (is_space(*at))
		{
			out |= NUMPRS_LEADING_WHITE;
			at++;
		}
	for (;;)
	{
		if (!(out & NUMPRS_LEADING_PLUS || out & NUMPRS_LEADING_MINUS || out & NUMPRS_PARENS) && *at == positive_sign && in & NUMPRS_LEADING_PLUS)
		{
			out |= NUMPRS_LEADING_PLUS;
			at++;
		}
		else if (!(out & NUMPRS_LEADING_PLUS || out & NUMPRS_LEADING_MINUS) && *at == negative_sign && in & NUMPRS_LEADING_MINUS)
		{
			out |= NUMPRS_LEADING_MINUS | NUMPRS_NEG;
			at++;
		}
		else if (!(out & NUMPRS_CURRENCY) && *at == currency && in & NUMPRS_CURRENCY)
		{
			out |= NUMPRS_CURRENCY;
			at++;
		}
		else if (!(out & NUMPRS_PARENS) && *at == L'(' && in & NUMPRS_PARENS)
		{
			out |= NUMPRS_PARENS | NUMPRS_NEG;
			at++;
		}
		else
			break;
		if (in & NUMPRS_LEADING_WHITE)
			while (is_space(*at))
			{
				out |= NUMPRS_LEADING_WHITE;
				at++;
			}
	}
	if (out & NUMPRS_CURRENCY && *at == currency)
		return DISP_E_TYPEMISMATCH;
	/* &H and &O numbers carry their digits in that base. */
	if (at[0] == L'&' && (in & (NUMPRS_HEX_OCT)) && (at[1] == L'h' || at[1] == L'H' || at[1] == L'o' || at[1] == L'O'))
	{
		BOOL hex = at[1] == L'h' || at[1] == L'H';
		int base = hex ? 16 : 8;
		const WCHAR *start;
		if (out & NUMPRS_CURRENCY)
			return DISP_E_TYPEMISMATCH;
		at += 2;
		start = at;
		while (hex_value(*at) >= 0 && hex_value(*at) < base)
		{
			if (!kept && !hex_value(*at))
			{
				at++;
				continue;
			}
			if (kept < parse->cDig)
				digits[kept++] = (BYTE)hex_value(*at);
			else if (hex_value(*at))
				out |= NUMPRS_INEXACT;
			total++;
			at++;
		}
		if (at == start)
			return DISP_E_TYPEMISMATCH;
		if (!kept)
		{
			digits[0] = 0;
			kept = 1;
		}
		parse->nBaseShift = hex ? 4 : 3;
		out |= NUMPRS_HEX_OCT;
		parse->cDig = kept;
		seen_digit = TRUE;
		in &= ~NUMPRS_CURRENCY;
		goto trailing;
	}
	for (;; at++)
	{
		if (*at >= L'0' && *at <= L'9')
		{
			int digit = *at - L'0';
			seen_digit = TRUE;
			if (!digit && !kept)
			{
				if (in_fraction)
					power--;
				continue;
			}
			if (kept < parse->cDig)
			{
				digits[kept++] = (BYTE)digit;
				if (in_fraction)
					power--;
			}
			else
			{
				if (digit)
					out |= NUMPRS_INEXACT;
				if (!in_fraction)
					power++;
			}
			total++;
			continue;
		}
		if (*at == decimal && !in_fraction && in & NUMPRS_DECIMAL)
		{
			in_fraction = TRUE;
			out |= NUMPRS_DECIMAL;
			continue;
		}
		if (*at == group && !in_fraction && in & NUMPRS_THOUSANDS && seen_digit)
		{
			out |= NUMPRS_THOUSANDS;
			continue;
		}
		break;
	}
	if (!seen_digit)
		return DISP_E_TYPEMISMATCH;
	/* An exponent only counts with digits after it; otherwise the E is trailing text. */
	if ((*at == L'e' || *at == L'E' || *at == L'd' || *at == L'D') && in & NUMPRS_EXPONENT)
	{
		const WCHAR *probe = at + 1;
		BOOL negative = FALSE;
		if (*probe == L'+' || *probe == L'-')
			negative = *probe++ == L'-';
		if (*probe >= L'0' && *probe <= L'9')
		{
			out |= NUMPRS_EXPONENT;
			while (*probe >= L'0' && *probe <= L'9')
			{
				if (exponent < 100000)
					exponent = exponent * 10 + (*probe - L'0');
				probe++;
			}
			exp_negative = negative;
			at = probe;
		}
	}
	power += exp_negative ? -exponent : exponent;
	while (kept > 1 && !digits[kept - 1])
	{
		kept--;
		power++;
	}
	if (!kept)
	{
		digits[0] = 0;
		kept = 1;
		if (!in_fraction)
			power = 0;
	}
	parse->cDig = kept;
	parse->nPwr10 = power;
trailing:
	trailing_ok = TRUE;
	while (trailing_ok)
	{
		if (*at == L')' && out & NUMPRS_PARENS && !(out & 0x40000000))
		{
			at++;
			out |= 0x40000000;
		}
		else if (in & NUMPRS_TRAILING_PLUS && *at == positive_sign && !(out & (NUMPRS_LEADING_PLUS | NUMPRS_LEADING_MINUS | NUMPRS_TRAILING_PLUS | NUMPRS_TRAILING_MINUS)))
		{
			at++;
			out |= NUMPRS_TRAILING_PLUS;
		}
		else if (in & NUMPRS_TRAILING_MINUS && *at == negative_sign && !(out & (NUMPRS_LEADING_PLUS | NUMPRS_LEADING_MINUS | NUMPRS_TRAILING_PLUS | NUMPRS_TRAILING_MINUS)))
		{
			at++;
			out |= NUMPRS_TRAILING_MINUS | NUMPRS_NEG;
		}
		else if (in & NUMPRS_CURRENCY && *at == currency)
		{
			at++;
			out |= NUMPRS_CURRENCY;
		}
		else if (in & NUMPRS_TRAILING_WHITE && is_space(*at) && !(is_space(group) && (*at == L' ' || *at == 0xa0)))
		{
			at++;
			out |= NUMPRS_TRAILING_WHITE;
		}
		else
			trailing_ok = FALSE;
	}
	if (out & NUMPRS_PARENS && !(out & 0x40000000))
		return DISP_E_TYPEMISMATCH;
	out &= ~0x40000000u;
	parse->dwOutFlags = out;
	parse->cchUsed = (ULONG)(at - text);
	return *at && !(in & NUMPRS_USE_ALL) ? S_OK : *at ? DISP_E_TYPEMISMATCH : S_OK;
}

/* The smallest allowed type that holds the parsed value exactly wins. */
HRESULT WINAPI VarNumFromParseNum(NUMPARSE *parse, BYTE *digits, ULONG bits, VARIANT *out)
{
	BOOL negative;
	ULONGLONG whole = 0;
	BOOL fits = TRUE, integral;
	ULONG index;
	int power;
	if (!parse || !digits || !out)
		return E_INVALIDARG;
	negative = (parse->dwOutFlags & NUMPRS_NEG) != 0;
	power = parse->nPwr10;
	if (parse->nBaseShift)
	{
		ULONG used_bits = 0;
		for (index = 0; index < parse->cDig; index++)
		{
			if (whole >> (64 - parse->nBaseShift))
				fits = FALSE;
			whole = (whole << parse->nBaseShift) | digits[index];
			if (whole)
				used_bits += parse->nBaseShift;
		}
		(void)used_bits;
		if (!fits)
			return DISP_E_OVERFLOW;
		/* Base shifted text fills the sign bit at each width. */
		if (bits & VTBIT_I1 && whole <= 0xff)
		{
			V_VT(out) = VT_I1;
			V_I1(out) = (CHAR)whole;
			return S_OK;
		}
		if (bits & VTBIT_UI1 && whole <= 0xff)
		{
			V_VT(out) = VT_UI1;
			V_UI1(out) = (BYTE)whole;
			return S_OK;
		}
		if (bits & VTBIT_I2 && whole <= 0xffff)
		{
			V_VT(out) = VT_I2;
			V_I2(out) = (SHORT)whole;
			return S_OK;
		}
		if (bits & VTBIT_UI2 && whole <= 0xffff)
		{
			V_VT(out) = VT_UI2;
			V_UI2(out) = (USHORT)whole;
			return S_OK;
		}
		if (bits & VTBIT_I4 && whole <= 0xffffffffull)
		{
			V_VT(out) = VT_I4;
			V_I4(out) = (LONG)whole;
			return S_OK;
		}
		if (bits & VTBIT_UI4 && whole <= 0xffffffffull)
		{
			V_VT(out) = VT_UI4;
			V_UI4(out) = (ULONG)whole;
			return S_OK;
		}
		if (bits & VTBIT_I8)
		{
			V_VT(out) = VT_I8;
			V_I8(out) = (LONG64)whole;
			return S_OK;
		}
		if (bits & VTBIT_UI8)
		{
			V_VT(out) = VT_UI8;
			V_UI8(out) = whole;
			return S_OK;
		}
		if (bits & VTBIT_DECIMAL)
		{
			oa_dec_from_u8(whole, &V_DECIMAL(out));
			V_VT(out) = VT_DECIMAL;
			return S_OK;
		}
		if (bits & VTBIT_R4 && !(bits & VTBIT_R8) && whole <= 0xffffffffffffffffull)
		{
			V_VT(out) = VT_R4;
			V_R4(out) = (float)whole;
			return S_OK;
		}
		if (bits & VTBIT_R8)
		{
			V_VT(out) = VT_R8;
			V_R8(out) = (double)whole;
			return S_OK;
		}
		return DISP_E_OVERFLOW;
	}
	integral = power >= 0;
	/* When only whole types may hold it, a fraction rounds first. */
	if (!integral && !(bits & (VTBIT_R4 | VTBIT_R8 | VTBIT_CY | VTBIT_DECIMAL)))
	{
		ULONG keep = (int)parse->cDig + power > 0 ? parse->cDig + power : 0;
		BOOL up = FALSE;
		for (index = 0; index < keep; index++)
		{
			if (whole > (0xffffffffffffffffull - digits[index]) / 10)
				fits = FALSE;
			else
				whole = whole * 10 + digits[index];
		}
		if ((int)parse->cDig + power >= 0 && keep < parse->cDig)
		{
			BOOL rest = FALSE;
			for (index = keep + 1; index < parse->cDig; index++)
				rest |= digits[index] != 0;
			up = digits[keep] > 5 || (digits[keep] == 5 && (rest || whole & 1));
		}
		if (up)
			whole++;
		if (!whole)
			negative = FALSE;
		integral = TRUE;
	}
	else if (integral)
	{
		for (index = 0; index < parse->cDig && fits; index++)
		{
			if (whole > (0xffffffffffffffffull - digits[index]) / 10)
				fits = FALSE;
			else
				whole = whole * 10 + digits[index];
		}
		for (index = 0; (int)index < power && fits; index++)
		{
			if (whole > 0xffffffffffffffffull / 10)
				fits = FALSE;
			else
				whole *= 10;
		}
	}
	if (integral && fits)
	{
		#define TRY_SIGNED(Bit, VT, Field, HIGH) \
			if (bits & Bit && (negative ? whole <= (ULONGLONG)(HIGH) + 1 : whole <= (ULONGLONG)(HIGH))) \
			{ \
				V_VT(out) = VT; \
				Field(out) = negative ? (LONG64)(0 - whole) : (LONG64)whole; \
				return S_OK; \
			}
		#define TRY_UNSIGNED(Bit, VT, Field, HIGH) \
			if (bits & Bit && !negative && whole <= (ULONGLONG)(HIGH)) \
			{ \
				V_VT(out) = VT; \
				Field(out) = whole; \
				return S_OK; \
			}
		TRY_SIGNED(VTBIT_I1, VT_I1, V_I1, 127)
		TRY_UNSIGNED(VTBIT_UI1, VT_UI1, V_UI1, 255)
		TRY_SIGNED(VTBIT_I2, VT_I2, V_I2, 32767)
		TRY_UNSIGNED(VTBIT_UI2, VT_UI2, V_UI2, 65535)
		TRY_SIGNED(VTBIT_I4, VT_I4, V_I4, 2147483647)
		TRY_UNSIGNED(VTBIT_UI4, VT_UI4, V_UI4, 4294967295ull)
		TRY_SIGNED(VTBIT_I8, VT_I8, V_I8, 0x7fffffffffffffffull)
		TRY_UNSIGNED(VTBIT_UI8, VT_UI8, V_UI8, 0xffffffffffffffffull)
	}
	if (bits & VTBIT_DECIMAL && !(bits & VTBIT_CY))
	{
		HRESULT result = oa_dec_from_parts(digits, parse->cDig, power, negative, &V_DECIMAL(out));
		if (SUCCEEDED(result))
		{
			V_VT(out) = VT_DECIMAL;
			return S_OK;
		}
	}
	if (bits & (VTBIT_R4 | VTBIT_R8 | VTBIT_CY))
	{
		long double total = 0.0L, scale = 1.0L, step = 10.0L;
		int left = power < 0 ? -power : power;
		for (index = 0; index < parse->cDig; index++)
			total = total * 10.0L + digits[index];
		while (left)
		{
			if (left & 1)
				scale *= step;
			step *= step;
			left >>= 1;
		}
		total = power < 0 ? total / scale : total * scale;
		if (negative)
			total = -total;
		if (bits & VTBIT_R4 && !(bits & VTBIT_CY) && (!(bits & VTBIT_R8) || (long double)(float)total == total))
		{
			if (total > 3.4028234663852886e38L || total < -3.4028234663852886e38L)
				return DISP_E_OVERFLOW;
			V_VT(out) = VT_R4;
			V_R4(out) = (float)total;
			return S_OK;
		}
		if (bits & VTBIT_CY && !(bits & VTBIT_R8))
		{
			HRESULT result = oa_cy_from_r8((double)total, &V_CY(out));
			if (SUCCEEDED(result))
				V_VT(out) = VT_CY;
			return result;
		}
		if (bits & VTBIT_R8)
		{
			if (total > 1.7976931348623157e308L || total < -1.7976931348623157e308L)
				return DISP_E_OVERFLOW;
			V_VT(out) = VT_R8;
			V_R8(out) = (double)total;
			return S_OK;
		}
	}
	return DISP_E_OVERFLOW;
}

/* --- Exact digits of a double: a big integer in base 1e9, then rounding half to even. --- */

ULONG oa_digits_of_r8(double value, int precision, BYTE *digits, int *exponent)
{
	union { double d; ULONGLONG u; } bits;
	DWORD big[120];
	int used = 1, shift, index, count = 0, written, point;
	char text[1100];
	ULONGLONG mantissa;
	bits.d = value;
	if (value == 0.0)
	{
		digits[0] = 0;
		*exponent = 1;
		return 1;
	}
	mantissa = bits.u & 0xfffffffffffffull;
	shift = (int)((bits.u >> 52) & 0x7ff);
	if (shift)
		mantissa |= 0x10000000000000ull;
	else
		shift = 1;
	shift -= 1075;
	/* value = mantissa * 2^shift. Multiply by 5^-shift when shift is negative to stay whole. */
	big[0] = (DWORD)(mantissa % 1000000000u);
	big[1] = (DWORD)((mantissa / 1000000000u) % 1000000000u);
	big[2] = (DWORD)(mantissa / 1000000000u / 1000000000u);
	used = 3;
	point = 0;
	{
		int rounds = shift < 0 ? -shift : shift;
		DWORD factor = shift < 0 ? 5 : 2;
		for (index = 0; index < rounds; index++)
		{
			ULONGLONG carry = 0;
			int word;
			for (word = 0; word < used; word++)
			{
				carry += (ULONGLONG)big[word] * factor;
				big[word] = (DWORD)(carry % 1000000000u);
				carry /= 1000000000u;
			}
			if (carry && used < (int)ARRAYSIZE(big))
				big[used++] = (DWORD)carry;
		}
		if (shift < 0)
			point = rounds;
	}
	written = 0;
	for (index = used - 1; index >= 0; index--)
	{
		DWORD word = big[index];
		DWORD divisor = 100000000;
		for (; divisor; divisor /= 10)
			text[written++] = (char)('0' + (word / divisor) % 10);
	}
	/* Leading zeros off; what is left has written digits with point of them after the dot. */
	{
		int first = 0;
		while (first < written - 1 && text[first] == '0')
			first++;
		count = written - first;
		*exponent = count - point;
		for (index = 0; index < precision && index < count; index++)
			digits[index] = (BYTE)(text[first + index] - '0');
		written = index;
		if (count > precision)
		{
			int next = text[first + precision] - '0';
			BOOL rest = FALSE;
			for (index = precision + 1; index < count; index++)
				if (text[first + index] != '0')
					rest = TRUE;
			if (next > 5 || (next == 5 && (rest || digits[precision - 1] & 1)))
			{
				index = precision - 1;
				while (index >= 0 && digits[index] == 9)
					digits[index--] = 0;
				if (index >= 0)
					digits[index]++;
				else
				{
					memmove(digits + 1, digits, precision - 1);
					digits[0] = 1;
					(*exponent)++;
				}
			}
		}
	}
	while (written > 1 && !digits[written - 1])
		written--;
	return (ULONG)written;
}

/* Power through the coprocessor: y * log2(x), then 2 to that, split into whole and fraction. */
HRESULT oa_pow(double base, double power, double *out)
{
	long double result;
	double whole = (double)(LONGLONG)power;
	BOOL negative = FALSE;
	if (power == 0.0)
	{
		*out = 1.0;
		return S_OK;
	}
	if (base == 0.0)
	{
		if (power < 0)
			return DISP_E_DIVBYZERO;
		*out = 0.0;
		return S_OK;
	}
	if (base < 0.0)
	{
		if (whole != power)
			return E_INVALIDARG;
		negative = ((LONGLONG)whole & 1) != 0;
		base = -base;
	}
	{
		long double x = base, y = power;
		__asm__ __volatile__(
			"fyl2x\n\t"
			"fld %%st(0)\n\t"
			"frndint\n\t"
			"fsubr %%st, %%st(1)\n\t"
			"fxch\n\t"
			"f2xm1\n\t"
			"fld1\n\t"
			"faddp\n\t"
			"fscale\n\t"
			"fstp %%st(1)\n\t"
			: "=t"(result) : "0"(x), "u"(y) : "st(1)", "cc");
	}
	if (result > 1.7976931348623157e308L)
		return DISP_E_OVERFLOW;
	*out = (double)(negative ? -result : result);
	return S_OK;
}

/* --- Text out --- */

static HRESULT text_from_digits(const BYTE *digits, ULONG count, int point, BOOL negative, LCID lcid, ULONG flags, BOOL allow_exponent, int precision, BSTR *out)
{
	WCHAR buffer[64], *at = buffer, decimal = locale_char(lcid, LOCALE_SDECIMAL, flags, L'.');
	ULONG index;
	if (negative)
		*at++ = L'-';
	if (allow_exponent && (point > precision || point < -10))
	{
		int power = point - 1, shown;
		*at++ = L'0' + digits[0];
		if (count > 1)
		{
			*at++ = decimal;
			for (index = 1; index < count; index++)
				*at++ = L'0' + digits[index];
		}
		*at++ = L'E';
		*at++ = power < 0 ? L'-' : L'+';
		shown = power < 0 ? -power : power;
		if (shown >= 100)
			*at++ = L'0' + shown / 100;
		*at++ = L'0' + (shown / 10) % 10;
		*at++ = L'0' + shown % 10;
	}
	else if (point <= 0)
	{
		int zeros;
		*at++ = L'0';
		*at++ = decimal;
		for (zeros = point; zeros < 0; zeros++)
			*at++ = L'0';
		for (index = 0; index < count; index++)
			*at++ = L'0' + digits[index];
	}
	else
	{
		for (index = 0; (int)index < point; index++)
			*at++ = L'0' + (index < count ? digits[index] : 0);
		if ((int)count > point)
		{
			*at++ = decimal;
			for (; index < count; index++)
				*at++ = L'0' + digits[index];
		}
	}
	*out = SysAllocStringLen(buffer, (UINT)(at - buffer));
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT text_from_real(double value, int precision, LCID lcid, ULONG flags, BSTR *out)
{
	BYTE digits[24];
	int point = 0;
	ULONG count;
	if (value == 0.0)
	{
		*out = SysAllocString(L"0");
		return *out ? S_OK : E_OUTOFMEMORY;
	}
	count = oa_digits_of_r8(value < 0 ? -value : value, precision, digits, &point);
	return text_from_digits(digits, count, point, value < 0, lcid, flags, TRUE, precision, out);
}

static HRESULT text_from_u8(ULONGLONG value, BOOL negative, BSTR *out)
{
	WCHAR buffer[24], *at = buffer + ARRAYSIZE(buffer);
	do
		*--at = L'0' + (WCHAR)(value % 10);
	while (value /= 10);
	if (negative)
		*--at = L'-';
	*out = SysAllocStringLen(at, (UINT)(buffer + ARRAYSIZE(buffer) - at));
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT text_from_i8(LONGLONG value, BSTR *out)
{
	return text_from_u8(value < 0 ? 0 - (ULONGLONG)value : (ULONGLONG)value, value < 0, out);
}

/* Scaled integers print with exactly the digits they hold, less trailing zeros. */
static HRESULT text_scaled(const BYTE *big, int total, int scale, BOOL negative, LCID lcid, ULONG flags, BSTR *out)
{
	BYTE digits[40];
	int first = 0, count, index;
	while (first < total - 1 && first < total - scale - 1 && !big[first])
		first++;
	count = total - first;
	memcpy(digits, big + first, count);
	while (count > total - first - scale && count > 1 && !digits[count - 1])
		count--;
	(void)index;
	return text_from_digits(digits, count, total - first - scale, negative, lcid, flags, FALSE, 0, out);
}

HRESULT WINAPI VarBstrFromI1(CHAR in, LCID lcid, ULONG flags, BSTR *out) { return out ? text_from_i8(in, out) : E_INVALIDARG; }
HRESULT WINAPI VarBstrFromUI1(BYTE in, LCID lcid, ULONG flags, BSTR *out) { return out ? text_from_i8(in, out) : E_INVALIDARG; }
HRESULT WINAPI VarBstrFromI2(SHORT in, LCID lcid, ULONG flags, BSTR *out) { return out ? text_from_i8(in, out) : E_INVALIDARG; }
HRESULT WINAPI VarBstrFromUI2(USHORT in, LCID lcid, ULONG flags, BSTR *out) { return out ? text_from_i8(in, out) : E_INVALIDARG; }
HRESULT WINAPI VarBstrFromI4(LONG in, LCID lcid, ULONG flags, BSTR *out) { return out ? text_from_i8(in, out) : E_INVALIDARG; }
HRESULT WINAPI VarBstrFromUI4(ULONG in, LCID lcid, ULONG flags, BSTR *out) { return out ? text_from_i8(in, out) : E_INVALIDARG; }
HRESULT WINAPI VarBstrFromI8(LONG64 in, LCID lcid, ULONG flags, BSTR *out) { return out ? text_from_i8(in, out) : E_INVALIDARG; }
HRESULT WINAPI VarBstrFromUI8(ULONG64 in, LCID lcid, ULONG flags, BSTR *out) { return out ? text_from_u8(in, FALSE, out) : E_INVALIDARG; }
HRESULT WINAPI VarBstrFromR4(FLOAT in, LCID lcid, ULONG flags, BSTR *out) { return out ? text_from_real(in, 7, lcid, flags, out) : E_INVALIDARG; }
HRESULT WINAPI VarBstrFromR8(DOUBLE in, LCID lcid, ULONG flags, BSTR *out) { return out ? text_from_real(in, 15, lcid, flags, out) : E_INVALIDARG; }

HRESULT WINAPI VarBstrFromCy(CY in, LCID lcid, ULONG flags, BSTR *out)
{
	BYTE digits[24];
	ULONGLONG value;
	int index = ARRAYSIZE(digits);
	if (!out)
		return E_INVALIDARG;
	value = in.int64 < 0 ? 0 - (ULONGLONG)in.int64 : (ULONGLONG)in.int64;
	while (index > 0)
	{
		digits[--index] = (BYTE)(value % 10);
		value /= 10;
	}
	return text_scaled(digits, ARRAYSIZE(digits), 4, in.int64 < 0, lcid, flags, out);
}

HRESULT WINAPI VarBstrFromDec(DECIMAL *in, LCID lcid, ULONG flags, BSTR *out)
{
	BYTE digits[32];
	DWORD words[3];
	int index = ARRAYSIZE(digits);
	if (!in || !out)
		return E_INVALIDARG;
	words[0] = (DWORD)DEC_LO64(in);
	words[1] = (DWORD)(DEC_LO64(in) >> 32);
	words[2] = DEC_HI32(in);
	while (index > 0)
	{
		ULONGLONG rest = 0;
		int word;
		for (word = 2; word >= 0; word--)
		{
			ULONGLONG part = (rest << 32) | words[word];
			words[word] = (DWORD)(part / 10);
			rest = part % 10;
		}
		digits[--index] = (BYTE)rest;
	}
	return text_scaled(digits, ARRAYSIZE(digits), DEC_SCALE(in), (DEC_SIGN(in) & DECIMAL_NEG) != 0, lcid, flags, out);
}

HRESULT WINAPI VarBstrFromBool(VARIANT_BOOL in, LCID lcid, ULONG flags, BSTR *out)
{
	const WCHAR *text;
	if (!out)
		return E_INVALIDARG;
	text = in ? L"True" : L"False";
	*out = SysAllocString(text);
	return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI VarBstrFromDisp(IDispatch *in, LCID lcid, ULONG flags, BSTR *out)
{
	VARIANT value;
	HRESULT result;
	if (!out)
		return E_INVALIDARG;
	VariantInit(&value);
	result = default_value(in, lcid, VT_BSTR, &value);
	if (SUCCEEDED(result))
		*out = V_BSTR(&value);
	return result;
}
