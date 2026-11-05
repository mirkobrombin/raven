/* SPDX-License-Identifier: MIT */
#include "js.h"

/* String. The methods work on any value, made a string first; those that take a regular expression go to regexp.c. */

static BOOL this_string(engine *en, js_value self, js_string **out)
{
	if (self.type == JV_OBJECT && self.object->cls == JC_STRING) {
		*out = js_string_retain(self.object->primitive.string);
		return TRUE;
	}
	return js_to_string(en, self, out);
}

static BOOL string_ctor(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *s;
	if (!argc)
		s = js_string_of(L"");
	else if (!js_to_string(en, argv[0], &s))
		return FALSE;
	if (!(self.type == JV_OBJECT && self.object == callee)) {
		*result = js_str(s);
		return TRUE;
	}
	js_object *o = js_object_new(en, JC_STRING, en->string_prototype);
	o->primitive = js_str(s);
	*result = js_obj(o);
	return TRUE;
}

static BOOL string_from_char_code(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_buffer b = { 0 };
	for (UINT i = 0; i < argc; i++) {
		UINT32 code;
		if (!js_to_uint32(en, argv[i], &code)) {
			js_free(b.text);
			return FALSE;
		}
		js_buffer_char(&b, (WCHAR)code);
	}
	*result = js_str(b.text ? js_buffer_string(&b) : js_string_of(L""));
	return TRUE;
}

static BOOL string_value(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	if (self.type == JV_STRING) {
		*result = js_retain(self);
		return TRUE;
	}
	if (self.type == JV_OBJECT && self.object->cls == JC_STRING) {
		*result = js_retain(self.object->primitive);
		return TRUE;
	}
	return js_throw_error(en, L"TypeError", JS_E_STRING_EXPECTED, NULL);
}

/* A position argument made an integer and held to [0, length]. */
static BOOL position(engine *en, js_value v, UINT length, UINT fallback, UINT *out)
{
	if (v.type == JV_UNDEFINED) {
		*out = fallback;
		return TRUE;
	}
	double x;
	if (!js_to_integer(en, v, &x))
		return FALSE;
	*out = x < 0 ? 0 : x > length ? length : (UINT)x;
	return TRUE;
}

static BOOL string_char_at(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *s;
	double x;
	if (!this_string(en, self, &s))
		return FALSE;
	if (!js_to_integer(en, js_arg(argc, argv, 0), &x)) {
		js_string_release(s);
		return FALSE;
	}
	*result = x >= 0 && x < s->length ? js_str(js_string_new(&s->text[(UINT)x], 1)) : js_text(L"");
	js_string_release(s);
	return TRUE;
}

static BOOL string_char_code_at(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *s;
	double x;
	if (!this_string(en, self, &s))
		return FALSE;
	if (!js_to_integer(en, js_arg(argc, argv, 0), &x)) {
		js_string_release(s);
		return FALSE;
	}
	*result = js_number(x >= 0 && x < s->length ? s->text[(UINT)x] : js_nan());
	js_string_release(s);
	return TRUE;
}

static BOOL string_concat(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *s;
	if (!this_string(en, self, &s))
		return FALSE;
	js_buffer b = { 0 };
	js_buffer_add(&b, s->text, s->length);
	js_string_release(s);
	for (UINT i = 0; i < argc; i++) {
		js_string *part;
		if (!js_to_string(en, argv[i], &part)) {
			js_free(b.text);
			return FALSE;
		}
		js_buffer_add(&b, part->text, part->length);
		js_string_release(part);
	}
	*result = js_str(b.text ? js_buffer_string(&b) : js_string_of(L""));
	return TRUE;
}

static BOOL find(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, BOOL last)
{
	js_string *s, *what;
	if (!this_string(en, self, &s))
		return FALSE;
	if (!js_to_string(en, js_arg(argc, argv, 0), &what)) {
		js_string_release(s);
		return FALSE;
	}
	UINT from;
	BOOL fine;
	if (last) {
		double x = js_infinity();
		js_value v = js_arg(argc, argv, 1);
		fine = TRUE;
		if (v.type != JV_UNDEFINED) {
			fine = js_to_number(en, v, &x);
			if (js_is_nan(x))
				x = js_infinity();
		}
		from = x < 0 ? 0 : x > s->length ? s->length : (UINT)x;
	} else
		fine = position(en, js_arg(argc, argv, 1), s->length, 0, &from);
	double found = -1;
	if (fine && what->length <= s->length) {
		UINT top = s->length - what->length;
		if (last) {
			for (UINT i = from < top ? from : top;; i--) {
				if (!memcmp(&s->text[i], what->text, what->length * sizeof(WCHAR))) {
					found = i;
					break;
				}
				if (!i)
					break;
			}
		} else
			for (UINT i = from; i <= top; i++)
				if (!memcmp(&s->text[i], what->text, what->length * sizeof(WCHAR))) {
					found = i;
					break;
				}
	}
	js_string_release(s);
	js_string_release(what);
	if (fine)
		*result = js_number(found);
	return fine;
}

static BOOL string_index_of(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	return find(en, self, argc, argv, result, FALSE);
}

static BOOL string_last_index_of(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	return find(en, self, argc, argv, result, TRUE);
}

static BOOL string_locale_compare(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *s, *other;
	if (!this_string(en, self, &s))
		return FALSE;
	if (!js_to_string(en, js_arg(argc, argv, 0), &other)) {
		js_string_release(s);
		return FALSE;
	}
	int c = CompareStringW(LOCALE_USER_DEFAULT, 0, s->text, s->length, other->text, other->length);
	*result = js_number(c ? c - CSTR_EQUAL : 0);
	js_string_release(s);
	js_string_release(other);
	return TRUE;
}

/* The pattern argument of match and search: a regular expression, or a string made one. */
static BOOL pattern_of(engine *en, js_value v, js_object **out)
{
	if (v.type == JV_OBJECT && v.object->cls == JC_REGEXP) {
		*out = js_object_retain(v.object);
		return TRUE;
	}
	js_string *source;
	if (v.type == JV_UNDEFINED)
		source = js_string_of(L"");
	else if (!js_to_string(en, v, &source))
		return FALSE;
	BOOL failed = FALSE;
	*out = js_regexp_new(en, source, 0, &failed);
	js_string_release(source);
	return !failed;
}

static BOOL string_match(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *s;
	js_object *re;
	if (!this_string(en, self, &s))
		return FALSE;
	if (!pattern_of(en, js_arg(argc, argv, 0), &re)) {
		js_string_release(s);
		return FALSE;
	}
	BOOL fine = js_string_match(en, re, s, result);
	js_object_release(re);
	js_string_release(s);
	return fine;
}

static BOOL string_search(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *s;
	js_object *re;
	if (!this_string(en, self, &s))
		return FALSE;
	if (!pattern_of(en, js_arg(argc, argv, 0), &re)) {
		js_string_release(s);
		return FALSE;
	}
	BOOL fine = js_string_search(en, re, s, result);
	js_object_release(re);
	js_string_release(s);
	return fine;
}

static BOOL string_replace(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	return js_string_replace(en, self, argc, argv, result);
}

static BOOL string_split(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *s;
	UINT32 limit = 0xffffffff;
	if (!this_string(en, self, &s))
		return FALSE;
	if (js_arg(argc, argv, 1).type != JV_UNDEFINED && !js_to_uint32(en, argv[1], &limit)) {
		js_string_release(s);
		return FALSE;
	}
	BOOL fine = js_string_split(en, s, js_arg(argc, argv, 0), limit, result);
	js_string_release(s);
	return fine;
}

static BOOL string_slice(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *s;
	if (!this_string(en, self, &s))
		return FALSE;
	double bounds[2] = { 0, s->length };
	for (UINT i = 0; i < 2; i++) {
		js_value v = js_arg(argc, argv, i);
		if (v.type == JV_UNDEFINED)
			continue;
		if (!js_to_integer(en, v, &bounds[i])) {
			js_string_release(s);
			return FALSE;
		}
		if (bounds[i] < 0)
			bounds[i] = bounds[i] + s->length < 0 ? 0 : bounds[i] + s->length;
		else if (bounds[i] > s->length)
			bounds[i] = s->length;
	}
	UINT start = (UINT)bounds[0], end = (UINT)bounds[1];
	*result = js_str(end > start ? js_string_new(&s->text[start], end - start) : js_string_of(L""));
	js_string_release(s);
	return TRUE;
}

static BOOL string_substring(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *s;
	UINT start, end;
	if (!this_string(en, self, &s))
		return FALSE;
	if (!position(en, js_arg(argc, argv, 0), s->length, 0, &start) || !position(en, js_arg(argc, argv, 1), s->length, s->length, &end)) {
		js_string_release(s);
		return FALSE;
	}
	if (start > end) {
		UINT t = start;
		start = end;
		end = t;
	}
	*result = js_str(js_string_new(&s->text[start], end - start));
	js_string_release(s);
	return TRUE;
}

static BOOL string_substr(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *s;
	if (!this_string(en, self, &s))
		return FALSE;
	double start = 0, count = js_infinity();
	if ((js_arg(argc, argv, 0).type != JV_UNDEFINED && !js_to_integer(en, argv[0], &start)) ||
		(js_arg(argc, argv, 1).type != JV_UNDEFINED && !js_to_integer(en, argv[1], &count))) {
		js_string_release(s);
		return FALSE;
	}
	if (start < 0)
		start = start + s->length < 0 ? 0 : start + s->length;
	if (start > s->length)
		start = s->length;
	if (count < 0)
		count = 0;
	if (count > s->length - start)
		count = s->length - start;
	*result = js_str(js_string_new(&s->text[(UINT)start], (UINT)count));
	js_string_release(s);
	return TRUE;
}

static BOOL change_case(engine *en, js_value self, js_value *result, BOOL upper)
{
	js_string *s;
	if (!this_string(en, self, &s))
		return FALSE;
	js_string *copy = js_string_new(s->text, s->length);
	js_string_release(s);
	if (copy->length) {
		if (upper)
			CharUpperBuffW(copy->text, copy->length);
		else
			CharLowerBuffW(copy->text, copy->length);
	}
	*result = js_str(copy);
	return TRUE;
}

static BOOL string_to_lower(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	return change_case(en, self, result, FALSE);
}

static BOOL string_to_upper(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	return change_case(en, self, result, TRUE);
}

/* The HTML methods of JScript: "x".bold() is "<B>x</B>", and those with an attribute put the argument in quotes. */
typedef struct {
	const WCHAR *method, *tag, *attribute;
} html_wrap;

static const html_wrap html_wraps[] = {
	{ L"anchor", L"A", L"NAME" }, { L"big", L"BIG", NULL }, { L"blink", L"BLINK", NULL }, { L"bold", L"B", NULL },
	{ L"fixed", L"TT", NULL }, { L"fontcolor", L"FONT", L"COLOR" }, { L"fontsize", L"FONT", L"SIZE" }, { L"italics", L"I", NULL },
	{ L"link", L"A", L"HREF" }, { L"small", L"SMALL", NULL }, { L"strike", L"STRIKE", NULL }, { L"sub", L"SUB", NULL },
	{ L"sup", L"SUP", NULL },
};

static BOOL string_html(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	const html_wrap *wrap = callee->native_data;
	js_string *s, *value = NULL;
	if (!this_string(en, self, &s))
		return FALSE;
	if (wrap->attribute && !js_to_string(en, js_arg(argc, argv, 0), &value)) {
		js_string_release(s);
		return FALSE;
	}
	js_buffer b = { 0 };
	js_buffer_char(&b, L'<');
	js_buffer_text(&b, wrap->tag);
	if (value) {
		js_buffer_char(&b, L' ');
		js_buffer_text(&b, wrap->attribute);
		js_buffer_text(&b, L"=\"");
		js_buffer_add(&b, value->text, value->length);
		js_buffer_char(&b, L'"');
		js_string_release(value);
	}
	js_buffer_char(&b, L'>');
	js_buffer_add(&b, s->text, s->length);
	js_buffer_text(&b, L"</");
	js_buffer_text(&b, wrap->tag);
	js_buffer_char(&b, L'>');
	js_string_release(s);
	*result = js_str(js_buffer_string(&b));
	return TRUE;
}

void js_init_string(engine *en)
{
	js_object *proto = en->string_prototype;
	proto->primitive = js_text(L"");
	js_object *ctor = js_define_native(en->global, L"String", string_ctor, 1);
	js_define_text(ctor, L"prototype", js_obj(js_object_retain(proto)), JP_HIDDEN | JP_READONLY);
	js_define_text(proto, L"constructor", js_obj(js_object_retain(ctor)), JP_DONTENUM);
	js_define_native(ctor, L"fromCharCode", string_from_char_code, 1);
	static const struct {
		const WCHAR *name;
		js_native fn;
		UINT length;
	} methods[] = {
		{ L"charAt", string_char_at, 1 }, { L"charCodeAt", string_char_code_at, 1 }, { L"concat", string_concat, 1 },
		{ L"indexOf", string_index_of, 1 }, { L"lastIndexOf", string_last_index_of, 1 }, { L"localeCompare", string_locale_compare, 1 },
		{ L"match", string_match, 1 }, { L"replace", string_replace, 2 }, { L"search", string_search, 1 }, { L"slice", string_slice, 2 },
		{ L"split", string_split, 2 }, { L"substr", string_substr, 2 }, { L"substring", string_substring, 2 },
		{ L"toLocaleLowerCase", string_to_lower, 0 }, { L"toLocaleUpperCase", string_to_upper, 0 }, { L"toLowerCase", string_to_lower, 0 },
		{ L"toString", string_value, 0 }, { L"toUpperCase", string_to_upper, 0 }, { L"valueOf", string_value, 0 },
	};
	for (UINT i = 0; i < ARRAYSIZE(methods); i++)
		js_define_native(proto, methods[i].name, methods[i].fn, methods[i].length);
	for (UINT i = 0; i < ARRAYSIZE(html_wraps); i++)
		js_define_native(proto, html_wraps[i].method, string_html, html_wraps[i].attribute ? 1 : 0)->native_data = &html_wraps[i];
}
