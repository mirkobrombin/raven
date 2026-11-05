/* SPDX-License-Identifier: MIT */
#include "js.h"
#include <math.h>

/* Number. numconv.c gives the digits; the layout around them follows ECMAScript. */

static BOOL this_number(engine *en, js_value self, double *out)
{
	if (self.type == JV_NUMBER) {
		*out = self.number;
		return TRUE;
	}
	if (self.type == JV_OBJECT && self.object->cls == JC_NUMBER) {
		*out = self.object->primitive.number;
		return TRUE;
	}
	*out = 0;
	return js_throw_error(en, L"TypeError", JS_E_NUMBER_EXPECTED, NULL);
}

static js_value ascii_value(const char *text)
{
	js_buffer b = { 0 };
	for (const char *c = text; *c; c++)
		js_buffer_char(&b, (WCHAR)*c);
	return js_str(b.text ? js_buffer_string(&b) : js_string_of(L""));
}

/* The digits of 0.ddd * 10^point written with fraction places after the point; digits past the count are zeros. */
static void layout_fixed(BOOL negative, const char *digits, int count, int point, int fraction, char *out)
{
	char *o = out;
	if (negative)
		*o++ = '-';
	if (point <= 0)
		*o++ = '0';
	for (int i = 0; i < point; i++)
		*o++ = i < count ? digits[i] : '0';
	if (fraction > 0) {
		*o++ = '.';
		for (int j = 0; j < fraction; j++) {
			int i = point + j;
			*o++ = i >= 0 && i < count ? digits[i] : '0';
		}
	}
	*o = 0;
}

/* n with fraction places, rounded half up; n is finite and below 10^21. */
static void fixed_text(double n, int fraction, char *out)
{
	char digits[128];
	int point = 0, count = 0;
	double a = n < 0 ? -n : n;
	if (a != 0)
		count = js_fixed_digits(a, fraction, digits, &point);
	if (!count)
		point = 0;
	layout_fixed(n < 0, digits, count, point, fraction, out);
}

static BOOL number_ctor(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	double n = 0;
	if (argc && !js_to_number(en, argv[0], &n))
		return FALSE;
	if (!(self.type == JV_OBJECT && self.object == callee)) {
		*result = js_number(n);
		return TRUE;
	}
	js_object *o = js_object_new(en, JC_NUMBER, en->number_prototype);
	o->primitive = js_number(n);
	*result = js_obj(o);
	return TRUE;
}

/* NaN and the infinities print the same way whatever the method. */
static BOOL special(double n, js_value *result)
{
	if (js_is_finite(n))
		return FALSE;
	*result = js_str(js_number_string(n));
	return TRUE;
}

static BOOL number_to_string(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	double n;
	INT32 radix = 10;
	if (!this_number(en, self, &n))
		return FALSE;
	if (js_arg(argc, argv, 0).type != JV_UNDEFINED && !js_to_int32(en, argv[0], &radix))
		return FALSE;
	if (radix < 2 || radix > 36)
		return js_throw_error(en, L"RangeError", JS_E_RADIX, NULL);
	if (radix == 10 || special(n, result)) {
		if (radix == 10)
			*result = js_str(js_number_string(n));
		return TRUE;
	}
	static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	char text[1200];
	UINT at = sizeof(text) / 2, start = at;
	BOOL negative = n < 0;
	double whole = floor(fabs(n)), part = fabs(n) - whole;
	do {
		double q = floor(whole / radix);
		text[--start] = digits[(int)(whole - q * radix)];
		whole = q;
	} while (whole >= 1 && start > 1);
	if (negative)
		text[--start] = '-';
	if (part > 0) {
		text[at++] = '.';
		for (UINT i = 0; i < 52 && part > 0 && at < sizeof(text) - 1; i++) {
			part *= radix;
			int digit = (int)part;
			text[at++] = digits[digit];
			part -= digit;
		}
	}
	text[at] = 0;
	*result = ascii_value(&text[start]);
	return TRUE;
}

static BOOL number_to_locale(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	double n;
	if (!this_number(en, self, &n))
		return FALSE;
	if (special(n, result))
		return TRUE;
	if (fabs(n) >= 1e21) {
		*result = js_str(js_number_string(n));
		return TRUE;
	}
	/* The user's locale with two decimals, as JScript writes it. */
	char text[400];
	fixed_text(n, 2, text);
	WCHAR wide[400], formatted[600];
	MultiByteToWideChar(CP_ACP, 0, text, -1, wide, ARRAYSIZE(wide));
	if (GetNumberFormatW(LOCALE_USER_DEFAULT, 0, wide, NULL, formatted, ARRAYSIZE(formatted)))
		*result = js_text(formatted);
	else
		*result = js_text(wide);
	return TRUE;
}

static BOOL number_value_of(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	double n;
	if (!this_number(en, self, &n))
		return FALSE;
	*result = js_number(n);
	return TRUE;
}

static BOOL digits_argument(engine *en, UINT argc, js_value *argv, int low, int high, LONG error, int *out, BOOL *given)
{
	js_value v = js_arg(argc, argv, 0);
	*given = v.type != JV_UNDEFINED;
	*out = 0;
	if (!*given)
		return TRUE;
	double x;
	if (!js_to_integer(en, v, &x))
		return FALSE;
	if (x < low || x > high)
		return js_throw_error(en, L"RangeError", error, NULL);
	*out = (int)x;
	return TRUE;
}

static BOOL number_to_fixed(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	double n;
	int digits;
	BOOL given;
	if (!this_number(en, self, &n) || !digits_argument(en, argc, argv, 0, 20, JS_E_FRACTION, &digits, &given))
		return FALSE;
	if (special(n, result))
		return TRUE;
	if (fabs(n) >= 1e21) {
		*result = js_str(js_number_string(n));
		return TRUE;
	}
	char text[160];
	fixed_text(n, digits, text);
	*result = ascii_value(text);
	return TRUE;
}

/* d.ddde+x from count digits and the point of 0.ddd * 10^point. */
static void exponential_text(BOOL negative, const char *digits, int count, int point, char *out)
{
	char *o = out;
	if (negative)
		*o++ = '-';
	*o++ = digits[0];
	if (count > 1) {
		*o++ = '.';
		for (int i = 1; i < count; i++)
			*o++ = digits[i];
	}
	int exponent = digits[0] == '0' ? 0 : point - 1;
	*o++ = 'e';
	*o++ = exponent < 0 ? '-' : '+';
	char reversed[8];
	int used = 0, e = exponent < 0 ? -exponent : exponent;
	do {
		reversed[used++] = (char)('0' + e % 10);
		e /= 10;
	} while (e);
	while (used)
		*o++ = reversed[--used];
	*o = 0;
}

static BOOL number_to_exponential(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	double n;
	int fraction;
	BOOL given;
	if (!this_number(en, self, &n) || !digits_argument(en, argc, argv, 0, 20, JS_E_FRACTION, &fraction, &given))
		return FALSE;
	if (special(n, result))
		return TRUE;
	char digits[64], text[96];
	int point = 1, count;
	double a = n < 0 ? -n : n;
	if (a == 0) {
		count = given ? fraction + 1 : 1;
		memset(digits, '0', count);
	} else
		/* Without a count, as many digits as tell n apart. */
		count = given ? js_exact_digits(a, fraction + 1, digits, &point) : js_shortest_digits(a, digits, &point);
	exponential_text(n < 0, digits, count, point, text);
	*result = ascii_value(text);
	return TRUE;
}

static BOOL number_to_precision(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	double n;
	int precision;
	BOOL given;
	if (!this_number(en, self, &n) || !digits_argument(en, argc, argv, 1, 21, JS_E_PRECISION, &precision, &given))
		return FALSE;
	if (!given || special(n, result)) {
		if (!given)
			*result = js_str(js_number_string(n));
		return TRUE;
	}
	char digits[64], text[96];
	int point = 1;
	double a = n < 0 ? -n : n;
	if (a == 0)
		memset(digits, '0', precision);
	else
		js_exact_digits(a, precision, digits, &point);
	int exponent = a == 0 ? 0 : point - 1;
	if (exponent < -6 || exponent >= precision)
		exponential_text(n < 0, digits, precision, point, text);
	else
		layout_fixed(n < 0, digits, precision, a == 0 ? 1 : point, precision - 1 - exponent, text);
	*result = ascii_value(text);
	return TRUE;
}

void js_init_number(engine *en)
{
	js_object *proto = en->number_prototype;
	proto->primitive = js_number(0);
	js_object *ctor = js_define_native(en->global, L"Number", number_ctor, 1);
	js_define_text(ctor, L"prototype", js_obj(js_object_retain(proto)), JP_HIDDEN | JP_READONLY);
	js_define_text(proto, L"constructor", js_obj(js_object_retain(ctor)), JP_DONTENUM);
	js_define_text(ctor, L"MAX_VALUE", js_number(1.7976931348623157e308), JP_HIDDEN | JP_READONLY);
	js_define_text(ctor, L"MIN_VALUE", js_number(4.9406564584124654e-324), JP_HIDDEN | JP_READONLY);
	js_define_text(ctor, L"NaN", js_number(js_nan()), JP_HIDDEN | JP_READONLY);
	js_define_text(ctor, L"NEGATIVE_INFINITY", js_number(-js_infinity()), JP_HIDDEN | JP_READONLY);
	js_define_text(ctor, L"POSITIVE_INFINITY", js_number(js_infinity()), JP_HIDDEN | JP_READONLY);
	static const struct {
		const WCHAR *name;
		js_native fn;
		UINT length;
	} methods[] = {
		{ L"toExponential", number_to_exponential, 1 }, { L"toFixed", number_to_fixed, 1 }, { L"toLocaleString", number_to_locale, 0 },
		{ L"toPrecision", number_to_precision, 1 }, { L"toString", number_to_string, 1 }, { L"valueOf", number_value_of, 0 },
	};
	for (UINT i = 0; i < ARRAYSIZE(methods); i++)
		js_define_native(proto, methods[i].name, methods[i].fn, methods[i].length);
}
