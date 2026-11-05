/* SPDX-License-Identifier: MIT */
#include "js.h"
#include <math.h>

/* Object, Function, Boolean, the errors, Math and the global functions. A native constructor knows it was called by new when this
 * is the constructor itself, as js_construct passes it. */

static BOOL constructing(js_value self, js_object *callee) { return self.type == JV_OBJECT && self.object == callee; }

static js_object *constructor(engine *en, const WCHAR *name, js_native fn, UINT length, js_object *proto)
{
	js_object *ctor = js_define_native(en->global, name, fn, length);
	js_define_text(ctor, L"prototype", js_obj(js_object_retain(proto)), JP_HIDDEN | JP_READONLY);
	js_define_text(proto, L"constructor", js_obj(js_object_retain(ctor)), JP_DONTENUM);
	return ctor;
}

static BOOL this_object(engine *en, js_value self, js_object **out) { return js_to_object(en, self, out); }

/* Object. */

js_string *js_object_class_name(js_object *o)
{
	static const WCHAR *const names[] = {
		[JC_OBJECT] = L"Object", [JC_FUNCTION] = L"Function", [JC_ARRAY] = L"Array", [JC_STRING] = L"String", [JC_NUMBER] = L"Number",
		[JC_BOOLEAN] = L"Boolean", [JC_DATE] = L"Date", [JC_REGEXP] = L"RegExp", [JC_ERROR] = L"Error", [JC_ARGUMENTS] = L"Object",
		[JC_MATH] = L"Math", [JC_GLOBAL] = L"Object", [JC_ACTIVATION] = L"Object", [JC_DISPATCH] = L"Object", [JC_ENUMERATOR] = L"Object",
		[JC_VBARRAY] = L"Object",
	};
	return js_string_of(names[o->cls]);
}

static BOOL object_ctor(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_value v = js_arg(argc, argv, 0);
	if (v.type == JV_UNDEFINED || v.type == JV_NULL) {
		*result = js_obj(js_object_new(en, JC_OBJECT, en->object_prototype));
		return TRUE;
	}
	js_object *o;
	if (!js_to_object(en, v, &o))
		return FALSE;
	*result = js_obj(o);
	return TRUE;
}

static BOOL object_to_string(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	if (self.type == JV_UNDEFINED || self.type == JV_NULL) {
		*result = js_text(L"[object Object]");
		return TRUE;
	}
	js_object *o;
	if (!this_object(en, self, &o))
		return FALSE;
	js_string *name = js_object_class_name(o);
	js_buffer b = { 0 };
	js_buffer_text(&b, L"[object ");
	js_buffer_add(&b, name->text, name->length);
	js_buffer_char(&b, L']');
	js_string_release(name);
	js_object_release(o);
	*result = js_str(js_buffer_string(&b));
	return TRUE;
}

static BOOL object_to_locale(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	if (!this_object(en, self, &o))
		return FALSE;
	js_value method;
	BOOL fine = js_get_text(en, o, L"toString", &method);
	if (fine)
		fine = js_call(en, method, js_obj(o), 0, NULL, result);
	js_release(method);
	js_object_release(o);
	return fine;
}

static BOOL object_value_of(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	if (!this_object(en, self, &o))
		return FALSE;
	*result = js_obj(o);
	return TRUE;
}

static BOOL object_has_own(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	js_string *name;
	if (!this_object(en, self, &o))
		return FALSE;
	if (!js_to_string(en, js_arg(argc, argv, 0), &name)) {
		js_object_release(o);
		return FALSE;
	}
	*result = js_bool(js_has_own(en, o, name));
	js_string_release(name);
	js_object_release(o);
	return TRUE;
}

static BOOL object_is_prototype(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_value v = js_arg(argc, argv, 0);
	*result = js_bool(FALSE);
	if (v.type != JV_OBJECT || self.type != JV_OBJECT)
		return TRUE;
	for (js_object *k = v.object->proto; k; k = k->proto)
		if (k == self.object) {
			*result = js_bool(TRUE);
			break;
		}
	return TRUE;
}

static BOOL object_enumerable(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	js_string *name;
	if (!this_object(en, self, &o))
		return FALSE;
	if (!js_to_string(en, js_arg(argc, argv, 0), &name)) {
		js_object_release(o);
		return FALSE;
	}
	js_property *p = js_own(o, name);
	UINT32 index;
	BOOL element = o->cls == JC_ARRAY && js_array_index(name, &index) && js_has_own(en, o, name);
	*result = js_bool(element || (p && !(p->flags & JP_DONTENUM)));
	js_string_release(name);
	js_object_release(o);
	return TRUE;
}

void js_init_object(engine *en)
{
	js_object *proto = en->object_prototype;
	constructor(en, L"Object", object_ctor, 1, proto);
	js_define_native(proto, L"toString", object_to_string, 0);
	js_define_native(proto, L"toLocaleString", object_to_locale, 0);
	js_define_native(proto, L"valueOf", object_value_of, 0);
	js_define_native(proto, L"hasOwnProperty", object_has_own, 1);
	js_define_native(proto, L"isPrototypeOf", object_is_prototype, 1);
	js_define_native(proto, L"propertyIsEnumerable", object_enumerable, 1);
}

/* Function. */

static BOOL function_ctor(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	/* new Function("a", "b", "return a + b"): the parameters, then the body, as the text of a function. */
	js_buffer b = { 0 };
	js_buffer_text(&b, L"(function anonymous(");
	for (UINT i = 0; i + 1 < argc; i++) {
		js_string *param;
		if (!js_to_string(en, argv[i], &param)) {
			js_free(b.text);
			return FALSE;
		}
		if (i)
			js_buffer_char(&b, L',');
		js_buffer_add(&b, param->text, param->length);
		js_string_release(param);
	}
	js_buffer_text(&b, L") {\n");
	if (argc) {
		js_string *body;
		if (!js_to_string(en, argv[argc - 1], &body)) {
			js_free(b.text);
			return FALSE;
		}
		js_buffer_add(&b, body->text, body->length);
		js_string_release(body);
	}
	js_buffer_text(&b, L"\n})");
	js_string *source = js_buffer_string(&b);
	/* It closes over the global scope only. */
	context global = { 0 };
	global.scope = js_scope_new(en->global, NULL);
	global.self = js_obj(en->global);
	global.variables = en->global;
	BOOL fine = js_eval_text(en, source, &global, result);
	js_scope_release(global.scope);
	js_string_release(source);
	return fine;
}

static BOOL function_to_string(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	if (self.type != JV_OBJECT || self.object->cls != JC_FUNCTION)
		return js_throw_error(en, L"TypeError", JS_E_FUNCTION_EXPECTED, NULL);
	js_object *f = self.object;
	if (f->code && f->code->source) {
		*result = js_str(js_string_retain(f->code->source));
		return TRUE;
	}
	*result = js_text(L"\nfunction() {\n    [native code]\n}\n");
	return TRUE;
}

static BOOL function_call(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	if (!js_is_callable(self))
		return js_throw_error(en, L"TypeError", JS_E_FUNCTION_EXPECTED, NULL);
	return js_call(en, self, js_arg(argc, argv, 0), argc ? argc - 1 : 0, argc ? argv + 1 : NULL, result);
}

static BOOL function_apply(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	if (!js_is_callable(self))
		return js_throw_error(en, L"TypeError", JS_E_FUNCTION_EXPECTED, NULL);
	js_value list = js_arg(argc, argv, 1);
	if (list.type == JV_UNDEFINED || list.type == JV_NULL)
		return js_call(en, self, js_arg(argc, argv, 0), 0, NULL, result);
	if (list.type != JV_OBJECT || (list.object->cls != JC_ARRAY && list.object->cls != JC_ARGUMENTS))
		return js_throw_error(en, L"TypeError", JS_E_ARRAY_EXPECTED, NULL);
	js_value length;
	UINT32 count;
	if (!js_get_text(en, list.object, L"length", &length))
		return FALSE;
	BOOL fine = js_to_uint32(en, length, &count);
	js_release(length);
	if (!fine)
		return FALSE;
	js_value *args = count ? js_alloc(count * sizeof(js_value)) : NULL;
	UINT got = 0;
	for (; got < count && fine; got++)
		fine = js_get_index(en, list.object, got, &args[got]);
	if (fine)
		fine = js_call(en, self, js_arg(argc, argv, 0), count, args, result);
	for (UINT i = 0; i < got; i++)
		js_release(args[i]);
	js_free(args);
	return fine;
}

static BOOL function_prototype_call(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	*result = js_undefined;
	return TRUE;
}

void js_init_function(engine *en)
{
	js_object *proto = en->function_prototype;
	proto->native = function_prototype_call;
	constructor(en, L"Function", function_ctor, 1, proto);
	js_define_native(proto, L"toString", function_to_string, 0);
	js_define_native(proto, L"call", function_call, 1);
	js_define_native(proto, L"apply", function_apply, 2);
}

/* Boolean. */

static BOOL boolean_ctor(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	BOOL value = js_to_boolean(js_arg(argc, argv, 0));
	if (!constructing(self, callee)) {
		*result = js_bool(value);
		return TRUE;
	}
	js_object *o = js_object_new(en, JC_BOOLEAN, en->boolean_prototype);
	o->primitive = js_bool(value);
	*result = js_obj(o);
	return TRUE;
}

static BOOL boolean_value(engine *en, js_value self, BOOL *value)
{
	if (self.type == JV_BOOL) {
		*value = self.boolean;
		return TRUE;
	}
	if (self.type == JV_OBJECT && self.object->cls == JC_BOOLEAN) {
		*value = self.object->primitive.boolean;
		return TRUE;
	}
	return js_throw_error(en, L"TypeError", JS_E_BOOLEAN_EXPECTED, NULL);
}

static BOOL boolean_to_string(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	BOOL value = FALSE;
	if (!boolean_value(en, self, &value))
		return FALSE;
	*result = js_text(value ? L"true" : L"false");
	return TRUE;
}

static BOOL boolean_value_of(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	BOOL value = FALSE;
	if (!boolean_value(en, self, &value))
		return FALSE;
	*result = js_bool(value);
	return TRUE;
}

void js_init_boolean(engine *en)
{
	js_object *proto = en->boolean_prototype;
	proto->primitive = js_bool(FALSE);
	constructor(en, L"Boolean", boolean_ctor, 1, proto);
	js_define_native(proto, L"toString", boolean_to_string, 0);
	js_define_native(proto, L"valueOf", boolean_value_of, 0);
}

/* Errors: JScript gives them a number and a description besides the message and name of ECMAScript. */

static const WCHAR *const error_kinds[] = { L"Error", L"EvalError", L"RangeError", L"ReferenceError", L"SyntaxError", L"TypeError", L"URIError" };

js_object *js_error_new(engine *en, const WCHAR *kind, LONG number, const WCHAR *text)
{
	js_object *proto = en->error_prototypes[0];
	for (UINT i = 0; i < ARRAYSIZE(error_kinds); i++)
		if (!lstrcmpW(error_kinds[i], kind))
			proto = en->error_prototypes[i];
	js_object *error = js_object_new(en, JC_ERROR, proto);
	js_define_text(error, L"number", js_number(number), JP_DONTENUM);
	js_define_text(error, L"description", js_text(text), JP_DONTENUM);
	js_define_text(error, L"message", js_text(text), JP_DONTENUM);
	return error;
}

static BOOL error_ctor(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	/* new Error(number, description), new Error(message): a first argument that is a number is the number. */
	const WCHAR *kind = callee->native_data ? callee->native_data : L"Error";
	double number = 0;
	js_value text = js_arg(argc, argv, 0);
	if (argc >= 1 && (argv[0].type == JV_NUMBER || argc >= 2)) {
		if (!js_to_number(en, argv[0], &number))
			return FALSE;
		text = js_arg(argc, argv, 1);
	}
	js_string *message = NULL;
	if (text.type != JV_UNDEFINED && !js_to_string(en, text, &message))
		return FALSE;
	js_object *error = js_error_new(en, kind, 0, message ? message->text : L"");
	js_define_text(error, L"number", js_number(number), JP_DONTENUM);
	js_string_release(message);
	*result = js_obj(error);
	return TRUE;
}

js_string *js_error_message(engine *en, js_object *error)
{
	js_value message = js_undefined;
	js_string *text = NULL;
	if (js_get_text(en, error, L"description", &message) && message.type != JV_UNDEFINED && js_to_string(en, message, &text) && text->length) {
		js_release(message);
		return text;
	}
	js_string_release(text);
	text = NULL;
	js_release(message);
	if (js_get_text(en, error, L"message", &message) && message.type != JV_UNDEFINED && js_to_string(en, message, &text)) {
		js_release(message);
		return text;
	}
	js_release(message);
	en->throwing = FALSE;
	js_release(en->exception);
	en->exception = js_undefined;
	return js_string_of(L"");
}

static BOOL error_to_string(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	if (self.type != JV_OBJECT)
		return js_throw_error(en, L"TypeError", JS_E_OBJECT_EXPECTED, NULL);
	js_value name;
	js_string *name_text = NULL;
	if (!js_get_text(en, self.object, L"name", &name) || !js_to_string(en, name, &name_text)) {
		js_release(name);
		return FALSE;
	}
	js_release(name);
	js_string *message = js_error_message(en, self.object);
	js_buffer b = { 0 };
	js_buffer_add(&b, name_text->text, name_text->length);
	if (message->length) {
		js_buffer_text(&b, L": ");
		js_buffer_add(&b, message->text, message->length);
	}
	js_string_release(name_text);
	js_string_release(message);
	*result = js_str(b.text ? js_buffer_string(&b) : js_string_of(L""));
	return TRUE;
}

void js_init_error(engine *en)
{
	for (UINT i = 0; i < ARRAYSIZE(error_kinds); i++) {
		js_object *proto = en->error_prototypes[i];
		js_object *ctor = js_define_native(en->global, error_kinds[i], error_ctor, 2);
		ctor->native_data = error_kinds[i];
		js_define_text(ctor, L"prototype", js_obj(js_object_retain(proto)), JP_HIDDEN | JP_READONLY);
		js_define_text(proto, L"constructor", js_obj(js_object_retain(ctor)), JP_DONTENUM);
		js_define_text(proto, L"name", js_text(error_kinds[i]), JP_DONTENUM);
		js_define_text(proto, L"message", js_text(L""), JP_DONTENUM);
		js_define_text(proto, L"description", js_text(L""), JP_DONTENUM);
		js_define_text(proto, L"number", js_number(0), JP_DONTENUM);
		if (!i)
			js_define_native(proto, L"toString", error_to_string, 0);
	}
}

/* Math. */

typedef double (*math_fn)(double);

static BOOL math_one(engine *en, UINT argc, js_value *argv, js_value *result, math_fn fn)
{
	double x;
	if (!js_to_number(en, js_arg(argc, argv, 0), &x))
		return FALSE;
	*result = js_number(fn(x));
	return TRUE;
}

static double js_round(double x)
{
	if (js_is_nan(x) || !js_is_finite(x))
		return x;
	double r = floor(x + 0.5);
	/* -0.5 < x < 0 rounds to minus zero. */
	if (r == 0 && x < 0)
		return -0.0;
	return r;
}

#define CRT1(name) static double crt_##name(double x) { return js_crt_math(#name, x); }
CRT1(cos)
CRT1(exp)
CRT1(log)
CRT1(sin)
CRT1(sqrt)

#define MATH1(name, fn) \
	static BOOL math_##name(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee) \
	{ return math_one(en, argc, argv, result, fn); }
MATH1(abs, fabs)
MATH1(acos, acos)
MATH1(asin, asin)
MATH1(atan, atan)
MATH1(ceil, ceil)
MATH1(cos, crt_cos)
MATH1(exp, crt_exp)
MATH1(floor, floor)
MATH1(log, crt_log)
MATH1(round, js_round)
MATH1(sin, crt_sin)
MATH1(sqrt, crt_sqrt)
MATH1(tan, tan)

static BOOL math_atan2(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	double y, x;
	if (!js_to_number(en, js_arg(argc, argv, 0), &y) || !js_to_number(en, js_arg(argc, argv, 1), &x))
		return FALSE;
	*result = js_number(atan2(y, x));
	return TRUE;
}

static BOOL math_pow(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	double x, y;
	if (!js_to_number(en, js_arg(argc, argv, 0), &x) || !js_to_number(en, js_arg(argc, argv, 1), &y))
		return FALSE;
	/* NaN to the power of 0 is 1; 1 to the power of NaN, and plus or minus 1 to an infinity, are NaN. */
	if (js_is_nan(y) || ((x == 1 || x == -1) && !js_is_finite(y)))
		*result = js_number(js_nan());
	else
		*result = js_number(js_crt_pow(x, y));
	return TRUE;
}

static BOOL math_extreme(engine *en, UINT argc, js_value *argv, js_value *result, BOOL maximum)
{
	double best = maximum ? -js_infinity() : js_infinity();
	for (UINT i = 0; i < argc; i++) {
		double x;
		if (!js_to_number(en, argv[i], &x))
			return FALSE;
		if (js_is_nan(x))
			best = x;
		else if (!js_is_nan(best) && (maximum ? x > best || (x == 0 && best == 0 && 1 / best < 0) : x < best || (x == 0 && best == 0 && 1 / x < 0)))
			best = x;
	}
	*result = js_number(best);
	return TRUE;
}

static BOOL math_max(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	return math_extreme(en, argc, argv, result, TRUE);
}

static BOOL math_min(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	return math_extreme(en, argc, argv, result, FALSE);
}

static BOOL math_random(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	/* A 64-bit xorshift generator seeded from the clock. */
	static ULONGLONG state;
	if (!state) {
		LARGE_INTEGER counter;
		QueryPerformanceCounter(&counter);
		state = (ULONGLONG)counter.QuadPart ^ 0x9e3779b97f4a7c15ULL ^ GetCurrentProcessId();
		if (!state)
			state = 1;
	}
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	*result = js_number((double)(state >> 11) / 9007199254740992.0);
	return TRUE;
}

void js_init_math(engine *en)
{
	js_object *math = js_object_new(en, JC_MATH, en->object_prototype);
	js_define_text(en->global, L"Math", js_obj(math), JP_DONTENUM);
	static const struct {
		const WCHAR *name;
		double value;
	} constants[] = {
		{ L"E", 2.718281828459045 }, { L"LN10", 2.302585092994046 }, { L"LN2", 0.6931471805599453 }, { L"LOG2E", 1.4426950408889634 },
		{ L"LOG10E", 0.4342944819032518 }, { L"PI", 3.141592653589793 }, { L"SQRT1_2", 0.7071067811865476 }, { L"SQRT2", 1.4142135623730951 },
	};
	for (UINT i = 0; i < ARRAYSIZE(constants); i++)
		js_define_text(math, constants[i].name, js_number(constants[i].value), JP_HIDDEN | JP_READONLY);
	static const struct {
		const WCHAR *name;
		js_native fn;
		UINT length;
	} functions[] = {
		{ L"abs", math_abs, 1 }, { L"acos", math_acos, 1 }, { L"asin", math_asin, 1 }, { L"atan", math_atan, 1 }, { L"atan2", math_atan2, 2 },
		{ L"ceil", math_ceil, 1 }, { L"cos", math_cos, 1 }, { L"exp", math_exp, 1 }, { L"floor", math_floor, 1 }, { L"log", math_log, 1 },
		{ L"max", math_max, 2 }, { L"min", math_min, 2 }, { L"pow", math_pow, 2 }, { L"random", math_random, 0 }, { L"round", math_round, 1 },
		{ L"sin", math_sin, 1 }, { L"sqrt", math_sqrt, 1 }, { L"tan", math_tan, 1 },
	};
	for (UINT i = 0; i < ARRAYSIZE(functions); i++)
		js_define_native(math, functions[i].name, functions[i].fn, functions[i].length);
}

/* The global functions. */

static BOOL global_eval(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	/* Called through another name, eval runs at global level. */
	js_value code = js_arg(argc, argv, 0);
	if (code.type != JV_STRING) {
		*result = js_retain(code);
		return TRUE;
	}
	context global = { 0 };
	global.scope = js_scope_new(en->global, NULL);
	global.self = js_obj(en->global);
	global.variables = en->global;
	BOOL fine = js_eval_text(en, code.string, &global, result);
	js_scope_release(global.scope);
	return fine;
}

static BOOL global_parse_int(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *text;
	INT32 radix;
	if (!js_to_string(en, js_arg(argc, argv, 0), &text))
		return FALSE;
	if (!js_to_int32(en, js_arg(argc, argv, 1), &radix)) {
		js_string_release(text);
		return FALSE;
	}
	const WCHAR *c = text->text, *end = text->text + text->length;
	while (c < end && (*c == L' ' || *c == L'\t' || *c == L'\n' || *c == L'\r' || *c == L'\v' || *c == L'\f' || *c == 0xa0 || *c == 0xfeff))
		c++;
	BOOL negative = FALSE;
	if (c < end && (*c == L'+' || *c == L'-'))
		negative = *c++ == L'-';
	if (radix == 0) {
		radix = 10;
		if (end - c >= 2 && c[0] == L'0' && (c[1] == L'x' || c[1] == L'X')) {
			radix = 16;
			c += 2;
		} else if (c < end && c[0] == L'0' && end - c > 1)
			/* JScript reads a leading zero as octal. */
			radix = 8;
	} else if (radix == 16 && end - c >= 2 && c[0] == L'0' && (c[1] == L'x' || c[1] == L'X'))
		c += 2;
	double value = js_nan();
	if (radix >= 2 && radix <= 36) {
		double sum = 0;
		BOOL any = FALSE;
		for (; c < end; c++) {
			int digit = *c >= L'0' && *c <= L'9' ? *c - L'0' : *c >= L'a' && *c <= L'z' ? *c - L'a' + 10 : *c >= L'A' && *c <= L'Z' ? *c - L'A' + 10 : 99;
			if (digit >= radix)
				break;
			sum = sum * radix + digit;
			any = TRUE;
		}
		if (any)
			value = negative ? -sum : sum;
	}
	js_string_release(text);
	*result = js_number(value);
	return TRUE;
}

static BOOL global_parse_float(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *text;
	if (!js_to_string(en, js_arg(argc, argv, 0), &text))
		return FALSE;
	const WCHAR *c = text->text, *end = text->text + text->length;
	while (c < end && (*c == L' ' || *c == L'\t' || *c == L'\n' || *c == L'\r' || *c == L'\v' || *c == L'\f' || *c == 0xa0 || *c == 0xfeff))
		c++;
	/* The longest prefix that is a decimal literal. */
	const WCHAR *start = c, *d = c;
	if (d < end && (*d == L'+' || *d == L'-'))
		d++;
	double value = js_nan();
	if (end - d >= 8 && !memcmp(d, L"Infinity", 8 * sizeof(WCHAR)))
		value = *start == L'-' ? -js_infinity() : js_infinity();
	else {
		const WCHAR *digits = d;
		while (d < end && *d >= L'0' && *d <= L'9')
			d++;
		if (d < end && *d == L'.') {
			d++;
			while (d < end && *d >= L'0' && *d <= L'9')
				d++;
		}
		if (d > digits && !(d == digits + 1 && *digits == L'.')) {
			if (d < end && (*d == L'e' || *d == L'E')) {
				const WCHAR *e = d + 1;
				if (e < end && (*e == L'+' || *e == L'-'))
					e++;
				if (e < end && *e >= L'0' && *e <= L'9') {
					while (e < end && *e >= L'0' && *e <= L'9')
						e++;
					d = e;
				}
			}
			js_string *literal = js_string_new(start, (UINT)(d - start));
			value = js_string_number(literal);
			js_string_release(literal);
		}
	}
	js_string_release(text);
	*result = js_number(value);
	return TRUE;
}

static BOOL global_is_nan(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	double x;
	if (!js_to_number(en, js_arg(argc, argv, 0), &x))
		return FALSE;
	*result = js_bool(js_is_nan(x));
	return TRUE;
}

static BOOL global_is_finite(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	double x;
	if (!js_to_number(en, js_arg(argc, argv, 0), &x))
		return FALSE;
	*result = js_bool(js_is_finite(x));
	return TRUE;
}

/* escape and unescape: %XX and %uXXXX for everything but letters, digits and @*_+-./ */
static BOOL global_escape(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *text;
	if (!js_to_string(en, js_arg(argc, argv, 0), &text))
		return FALSE;
	js_buffer b = { 0 };
	static const WCHAR hex[] = L"0123456789ABCDEF";
	for (UINT i = 0; i < text->length; i++) {
		WCHAR c = text->text[i];
		if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || (c && wcschr(L"@*_+-./", c)))
			js_buffer_char(&b, c);
		else if (c < 256) {
			WCHAR e[3] = { L'%', hex[c >> 4], hex[c & 15] };
			js_buffer_add(&b, e, 3);
		} else {
			WCHAR e[6] = { L'%', L'u', hex[c >> 12], hex[(c >> 8) & 15], hex[(c >> 4) & 15], hex[c & 15] };
			js_buffer_add(&b, e, 6);
		}
	}
	js_string_release(text);
	*result = js_str(b.text ? js_buffer_string(&b) : js_string_of(L""));
	return TRUE;
}

static int hex_digit(WCHAR c)
{
	return c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
}

static int hex_run(const WCHAR *text, UINT count)
{
	int value = 0;
	for (UINT i = 0; i < count; i++) {
		int digit = hex_digit(text[i]);
		if (digit < 0)
			return -1;
		value = value * 16 + digit;
	}
	return value;
}

static BOOL global_unescape(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *text;
	if (!js_to_string(en, js_arg(argc, argv, 0), &text))
		return FALSE;
	js_buffer b = { 0 };
	for (UINT i = 0; i < text->length; i++) {
		WCHAR c = text->text[i];
		int value;
		if (c == L'%' && i + 6 <= text->length && text->text[i + 1] == L'u' && (value = hex_run(&text->text[i + 2], 4)) >= 0) {
			js_buffer_char(&b, (WCHAR)value);
			i += 5;
		} else if (c == L'%' && i + 3 <= text->length && (value = hex_run(&text->text[i + 1], 2)) >= 0) {
			js_buffer_char(&b, (WCHAR)value);
			i += 2;
		} else
			js_buffer_char(&b, c);
	}
	js_string_release(text);
	*result = js_str(b.text ? js_buffer_string(&b) : js_string_of(L""));
	return TRUE;
}

/* encodeURI and its kin: characters outside the set left alone go as the %XX of their UTF-8 bytes. */
static BOOL uri_encode(engine *en, UINT argc, js_value *argv, js_value *result, const WCHAR *keep)
{
	js_string *text;
	if (!js_to_string(en, js_arg(argc, argv, 0), &text))
		return FALSE;
	js_buffer b = { 0 };
	static const WCHAR hex[] = L"0123456789ABCDEF";
	for (UINT i = 0; i < text->length; i++) {
		WCHAR c = text->text[i];
		if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || (c && wcschr(L"-_.!~*'()", c)) ||
			(c && wcschr(keep, c))) {
			js_buffer_char(&b, c);
			continue;
		}
		UINT code = c;
		if (c >= 0xdc00 && c <= 0xdfff) {
			js_free(b.text);
			js_string_release(text);
			return js_throw_error(en, L"URIError", JS_E_INVALID_URI, NULL);
		}
		if (c >= 0xd800 && c <= 0xdbff) {
			if (i + 1 >= text->length || text->text[i + 1] < 0xdc00 || text->text[i + 1] > 0xdfff) {
				js_free(b.text);
				js_string_release(text);
				return js_throw_error(en, L"URIError", JS_E_INVALID_URI, NULL);
			}
			code = 0x10000 + ((c - 0xd800) << 10) + (text->text[++i] - 0xdc00);
		}
		BYTE bytes[4];
		UINT count;
		if (code < 0x80)
			bytes[0] = (BYTE)code, count = 1;
		else if (code < 0x800)
			bytes[0] = (BYTE)(0xc0 | (code >> 6)), bytes[1] = (BYTE)(0x80 | (code & 0x3f)), count = 2;
		else if (code < 0x10000)
			bytes[0] = (BYTE)(0xe0 | (code >> 12)), bytes[1] = (BYTE)(0x80 | ((code >> 6) & 0x3f)), bytes[2] = (BYTE)(0x80 | (code & 0x3f)), count = 3;
		else
			bytes[0] = (BYTE)(0xf0 | (code >> 18)), bytes[1] = (BYTE)(0x80 | ((code >> 12) & 0x3f)), bytes[2] = (BYTE)(0x80 | ((code >> 6) & 0x3f)),
			bytes[3] = (BYTE)(0x80 | (code & 0x3f)), count = 4;
		for (UINT k = 0; k < count; k++) {
			WCHAR e[3] = { L'%', hex[bytes[k] >> 4], hex[bytes[k] & 15] };
			js_buffer_add(&b, e, 3);
		}
	}
	js_string_release(text);
	*result = js_str(b.text ? js_buffer_string(&b) : js_string_of(L""));
	return TRUE;
}

static BOOL uri_decode(engine *en, UINT argc, js_value *argv, js_value *result, const WCHAR *reserved)
{
	js_string *text;
	if (!js_to_string(en, js_arg(argc, argv, 0), &text))
		return FALSE;
	js_buffer b = { 0 };
	for (UINT i = 0; i < text->length; i++) {
		WCHAR c = text->text[i];
		if (c != L'%') {
			js_buffer_char(&b, c);
			continue;
		}
		int first = i + 3 <= text->length ? hex_run(&text->text[i + 1], 2) : -1;
		if (first < 0)
			goto bad;
		UINT start = i;
		i += 2;
		if (first < 0x80) {
			/* An escaped reserved character stays escaped. */
			if (reserved && first && wcschr(reserved, (WCHAR)first))
				js_buffer_add(&b, &text->text[start], 3);
			else
				js_buffer_char(&b, (WCHAR)first);
			continue;
		}
		UINT more = (first & 0xe0) == 0xc0 ? 1 : (first & 0xf0) == 0xe0 ? 2 : (first & 0xf8) == 0xf0 ? 3 : 0;
		if (!more)
			goto bad;
		UINT code = first & (0x3f >> more);
		for (UINT k = 0; k < more; k++) {
			int next = i + 4 <= text->length && text->text[i + 1] == L'%' ? hex_run(&text->text[i + 2], 2) : -1;
			if (next < 0 || (next & 0xc0) != 0x80)
				goto bad;
			code = (code << 6) | (next & 0x3f);
			i += 3;
		}
		if (code >= 0x10000) {
			code -= 0x10000;
			js_buffer_char(&b, (WCHAR)(0xd800 + (code >> 10)));
			js_buffer_char(&b, (WCHAR)(0xdc00 + (code & 0x3ff)));
		} else
			js_buffer_char(&b, (WCHAR)code);
	}
	js_string_release(text);
	*result = js_str(b.text ? js_buffer_string(&b) : js_string_of(L""));
	return TRUE;
bad:
	js_free(b.text);
	js_string_release(text);
	return js_throw_error(en, L"URIError", JS_E_INVALID_URI, NULL);
}

static BOOL global_encode_uri(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	return uri_encode(en, argc, argv, result, L";/?:@&=+$,#");
}

static BOOL global_encode_component(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	return uri_encode(en, argc, argv, result, L"");
}

static BOOL global_decode_uri(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	return uri_decode(en, argc, argv, result, L";/?:@&=+$,#");
}

static BOOL global_decode_component(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	return uri_decode(en, argc, argv, result, NULL);
}

static BOOL global_script_engine(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	*result = js_text(L"JScript");
	return TRUE;
}

static BOOL global_version(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	*result = js_number((double)(ULONG_PTR)callee->native_data);
	return TRUE;
}

static BOOL global_collect(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	*result = js_undefined;
	return TRUE;
}

void js_init_global(engine *en)
{
	js_object *g = en->global;
	js_define_text(g, L"NaN", js_number(js_nan()), JP_HIDDEN);
	js_define_text(g, L"Infinity", js_number(js_infinity()), JP_HIDDEN);
	js_define_text(g, L"undefined", js_undefined, JP_HIDDEN);
	js_object *eval = js_define_native(g, L"eval", global_eval, 1);
	/* The interpreter knows the real eval by this mark, to run a direct call in the caller's context. */
	eval->native_data = en;
	js_define_native(g, L"parseInt", global_parse_int, 2);
	js_define_native(g, L"parseFloat", global_parse_float, 1);
	js_define_native(g, L"isNaN", global_is_nan, 1);
	js_define_native(g, L"isFinite", global_is_finite, 1);
	js_define_native(g, L"escape", global_escape, 1);
	js_define_native(g, L"unescape", global_unescape, 1);
	js_define_native(g, L"encodeURI", global_encode_uri, 1);
	js_define_native(g, L"encodeURIComponent", global_encode_component, 1);
	js_define_native(g, L"decodeURI", global_decode_uri, 1);
	js_define_native(g, L"decodeURIComponent", global_decode_component, 1);
	js_define_native(g, L"ScriptEngine", global_script_engine, 0);
	js_define_native(g, L"ScriptEngineMajorVersion", global_version, 0)->native_data = (const void *)(ULONG_PTR)5;
	js_define_native(g, L"ScriptEngineMinorVersion", global_version, 0)->native_data = (const void *)(ULONG_PTR)8;
	js_define_native(g, L"ScriptEngineBuildVersion", global_version, 0)->native_data = (const void *)(ULONG_PTR)16384;
	js_define_native(g, L"CollectGarbage", global_collect, 0);
}
