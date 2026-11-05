/* SPDX-License-Identifier: MIT */
#include "js.h"
#include "../raven-vbscript/regex.h"

/* RegExp, and the string methods that take one: match, replace, search and split. The matcher is the one VBScript shares. */

static void define_flags(js_object *re)
{
	js_define_text(re, L"source", js_str(js_string_retain(re->source)), JP_HIDDEN | JP_READONLY);
	js_define_text(re, L"global", js_bool(re->rx_flags & RXF_GLOBAL), JP_HIDDEN | JP_READONLY);
	js_define_text(re, L"ignoreCase", js_bool(re->rx_flags & RX_IGNORE_CASE), JP_HIDDEN | JP_READONLY);
	js_define_text(re, L"multiline", js_bool(re->rx_flags & RX_MULTILINE), JP_HIDDEN | JP_READONLY);
}

js_object *js_regexp_new(engine *en, const js_string *pattern, DWORD flags, BOOL *failed)
{
	*failed = FALSE;
	LONG error = 0;
	rx *compiled = rx_compile(pattern->text, pattern->length, flags & (RX_IGNORE_CASE | RX_MULTILINE), &error);
	if (!compiled) {
		*failed = TRUE;
		js_throw_error(en, L"SyntaxError", error ? error : JS_E_REGEXP_SYNTAX, NULL);
		return NULL;
	}
	js_object *re = js_object_new(en, JC_REGEXP, en->regexp_prototype);
	re->regex = compiled;
	re->source = js_string_new(pattern->text, pattern->length);
	re->rx_flags = flags;
	define_flags(re);
	js_define_text(re, L"lastIndex", js_number(0), JP_HIDDEN);
	return re;
}

BOOL js_regexp_flags(engine *en, const js_string *text, DWORD *out)
{
	*out = 0;
	for (UINT i = 0; i < text->length; i++) {
		DWORD flag = text->text[i] == L'g' ? RXF_GLOBAL : text->text[i] == L'i' ? RX_IGNORE_CASE : text->text[i] == L'm' ? RX_MULTILINE : 0;
		if (!flag || (*out & flag))
			return js_throw_error(en, L"SyntaxError", JS_E_REGEXP_SYNTAX, NULL);
		*out |= flag;
	}
	return TRUE;
}

static js_string *piece(const js_string *text, int start, int end)
{
	if (start < 0 || end < start)
		return js_string_of(L"");
	return js_string_new(&text->text[start], (UINT)(end - start));
}

/* RegExp.$1 to $9 and the rest describe the last match any expression made. */
void js_record_match(engine *en, js_string *input, const int *spans, UINT groups)
{
	js_object *ctor = en->regexp_constructor;
	if (!ctor)
		return;
	js_define_text(ctor, L"input", js_str(js_string_retain(input)), JP_DONTENUM);
	js_define_text(ctor, L"$_", js_str(js_string_retain(input)), JP_DONTENUM);
	js_define_text(ctor, L"index", js_number(spans[0]), JP_DONTENUM);
	js_define_text(ctor, L"lastIndex", js_number(spans[1]), JP_DONTENUM);
	js_value last = js_str(piece(input, spans[0], spans[1]));
	js_define_text(ctor, L"lastMatch", js_retain(last), JP_DONTENUM);
	js_define_text(ctor, L"$&", last, JP_DONTENUM);
	js_value left = js_str(piece(input, 0, spans[0]));
	js_define_text(ctor, L"leftContext", js_retain(left), JP_DONTENUM);
	js_define_text(ctor, L"$`", left, JP_DONTENUM);
	js_value right = js_str(piece(input, spans[1], (int)input->length));
	js_define_text(ctor, L"rightContext", js_retain(right), JP_DONTENUM);
	js_define_text(ctor, L"$'", right, JP_DONTENUM);
	js_value paren = groups ? js_str(piece(input, spans[2 * groups], spans[2 * groups + 1])) : js_text(L"");
	js_define_text(ctor, L"lastParen", js_retain(paren), JP_DONTENUM);
	js_define_text(ctor, L"$+", paren, JP_DONTENUM);
	for (UINT i = 1; i <= 9; i++) {
		WCHAR name[3] = { L'$', (WCHAR)(L'0' + i) };
		js_define_text(ctor, name, i <= groups ? js_str(piece(input, spans[2 * i], spans[2 * i + 1])) : js_text(L""), JP_DONTENUM);
	}
}

static BOOL last_index(engine *en, js_object *re, double *out)
{
	js_value v;
	if (!js_get_text(en, re, L"lastIndex", &v))
		return FALSE;
	BOOL fine = js_to_integer(en, v, out);
	js_release(v);
	return fine;
}

/* Runs the expression from start; on a match spans holds the groups. The caller owns spans. */
static BOOL search_at(engine *en, js_object *re, js_string *text, UINT start, int **spans)
{
	UINT groups = rx_groups(re->regex);
	*spans = js_alloc(2 * (groups + 1) * sizeof(int));
	if (start > text->length || !rx_search(re->regex, text->text, text->length, start, *spans)) {
		js_free(*spans);
		*spans = NULL;
		return FALSE;
	}
	js_record_match(en, text, *spans, groups);
	return TRUE;
}

/* The array exec gives back: the match and its groups, with index, input and lastIndex. */
static js_object *match_array(engine *en, js_object *re, js_string *text, const int *spans)
{
	UINT groups = rx_groups(re->regex);
	js_object *a = js_array_new(en, 0);
	for (UINT i = 0; i <= groups; i++) {
		js_value v = js_str(piece(text, spans[2 * i], spans[2 * i + 1]));
		js_array_push(a, v);
		js_release(v);
	}
	js_define_text(a, L"index", js_number(spans[0]), 0);
	js_define_text(a, L"lastIndex", js_number(spans[1]), 0);
	js_define_text(a, L"input", js_str(js_string_retain(text)), 0);
	return a;
}

BOOL js_regexp_exec(engine *en, js_object *re, js_string *text, BOOL update_last_index, js_value *result)
{
	BOOL global = (re->rx_flags & RXF_GLOBAL) != 0;
	double start = 0;
	if (global && !last_index(en, re, &start))
		return FALSE;
	int *spans;
	if (start < 0 || start > text->length || !search_at(en, re, text, (UINT)start, &spans)) {
		if (global && update_last_index)
			js_put_text(en, re, L"lastIndex", js_number(0));
		*result = js_null;
		return TRUE;
	}
	/* As JScript does, an empty match moves lastIndex on by one, so a loop over exec ends. */
	if (global && update_last_index)
		js_put_text(en, re, L"lastIndex", js_number(spans[1] + (spans[1] == spans[0] ? 1 : 0)));
	*result = js_obj(match_array(en, re, text, spans));
	js_free(spans);
	return TRUE;
}

static BOOL this_regexp(engine *en, js_value self, js_object **out)
{
	if (self.type == JV_OBJECT && self.object->cls == JC_REGEXP) {
		*out = self.object;
		return TRUE;
	}
	*out = NULL;
	return js_throw_error(en, L"TypeError", JS_E_REGEXP_EXPECTED, NULL);
}

static BOOL text_argument(engine *en, UINT argc, js_value *argv, js_string **out)
{
	js_value v = js_arg(argc, argv, 0);
	if (v.type == JV_UNDEFINED && en->regexp_constructor) {
		/* With no argument the input of the last match is searched, as JScript does. */
		js_value input;
		if (!js_get_text(en, en->regexp_constructor, L"input", &input))
			return FALSE;
		BOOL fine = js_to_string(en, input, out);
		js_release(input);
		return fine;
	}
	return js_to_string(en, v, out);
}

static BOOL regexp_exec(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *re;
	js_string *text;
	if (!this_regexp(en, self, &re) || !text_argument(en, argc, argv, &text))
		return FALSE;
	BOOL fine = js_regexp_exec(en, re, text, TRUE, result);
	js_string_release(text);
	return fine;
}

static BOOL regexp_test(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_value match;
	if (!regexp_exec(en, self, argc, argv, &match, callee))
		return FALSE;
	*result = js_bool(match.type != JV_NULL);
	js_release(match);
	return TRUE;
}

static BOOL regexp_to_string(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *re;
	if (!this_regexp(en, self, &re))
		return FALSE;
	js_buffer b = { 0 };
	js_buffer_char(&b, L'/');
	js_buffer_add(&b, re->source->text, re->source->length);
	js_buffer_char(&b, L'/');
	if (re->rx_flags & RXF_GLOBAL)
		js_buffer_char(&b, L'g');
	if (re->rx_flags & RX_IGNORE_CASE)
		js_buffer_char(&b, L'i');
	if (re->rx_flags & RX_MULTILINE)
		js_buffer_char(&b, L'm');
	*result = js_str(js_buffer_string(&b));
	return TRUE;
}

/* The pattern and flags of new RegExp(pattern, flags) and of compile. */
static BOOL pattern_arguments(engine *en, UINT argc, js_value *argv, js_string **pattern, DWORD *flags)
{
	js_value p = js_arg(argc, argv, 0), f = js_arg(argc, argv, 1);
	*flags = 0;
	if (p.type == JV_OBJECT && p.object->cls == JC_REGEXP) {
		if (f.type != JV_UNDEFINED)
			return js_throw_error(en, L"TypeError", JS_E_REGEXP_SYNTAX, NULL);
		*pattern = js_string_retain(p.object->source);
		*flags = p.object->rx_flags;
		return TRUE;
	}
	if (p.type == JV_UNDEFINED)
		*pattern = js_string_of(L"");
	else if (!js_to_string(en, p, pattern))
		return FALSE;
	if (f.type != JV_UNDEFINED) {
		js_string *text;
		if (!js_to_string(en, f, &text)) {
			js_string_release(*pattern);
			return FALSE;
		}
		BOOL fine = js_regexp_flags(en, text, flags);
		js_string_release(text);
		if (!fine) {
			js_string_release(*pattern);
			return FALSE;
		}
	}
	return TRUE;
}

static BOOL regexp_ctor(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_value p = js_arg(argc, argv, 0);
	if (!(self.type == JV_OBJECT && self.object == callee) && p.type == JV_OBJECT && p.object->cls == JC_REGEXP &&
		js_arg(argc, argv, 1).type == JV_UNDEFINED) {
		*result = js_retain(p);
		return TRUE;
	}
	js_string *pattern;
	DWORD flags;
	if (!pattern_arguments(en, argc, argv, &pattern, &flags))
		return FALSE;
	BOOL failed;
	js_object *re = js_regexp_new(en, pattern, flags, &failed);
	js_string_release(pattern);
	if (!re)
		return FALSE;
	*result = js_obj(re);
	return TRUE;
}

static BOOL regexp_compile(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *re;
	js_string *pattern;
	DWORD flags;
	if (!this_regexp(en, self, &re) || !pattern_arguments(en, argc, argv, &pattern, &flags))
		return FALSE;
	LONG error = 0;
	rx *compiled = rx_compile(pattern->text, pattern->length, flags & (RX_IGNORE_CASE | RX_MULTILINE), &error);
	if (!compiled) {
		js_string_release(pattern);
		return js_throw_error(en, L"SyntaxError", error ? error : JS_E_REGEXP_SYNTAX, NULL);
	}
	rx_free(re->regex);
	re->regex = compiled;
	js_string_release(re->source);
	re->source = pattern;
	re->rx_flags = flags;
	define_flags(re);
	js_put_text(en, re, L"lastIndex", js_number(0));
	*result = js_obj(js_object_retain(re));
	return TRUE;
}

void js_init_regexp(engine *en)
{
	js_object *proto = en->regexp_prototype;
	js_object *ctor = js_define_native(en->global, L"RegExp", regexp_ctor, 2);
	en->regexp_constructor = js_object_retain(ctor);
	js_define_text(ctor, L"prototype", js_obj(js_object_retain(proto)), JP_HIDDEN | JP_READONLY);
	js_define_text(proto, L"constructor", js_obj(js_object_retain(ctor)), JP_DONTENUM);
	js_define_native(proto, L"exec", regexp_exec, 1);
	js_define_native(proto, L"test", regexp_test, 1);
	js_define_native(proto, L"toString", regexp_to_string, 0);
	js_define_native(proto, L"compile", regexp_compile, 2);
	js_define_text(ctor, L"input", js_text(L""), JP_DONTENUM);
	for (UINT i = 1; i <= 9; i++) {
		WCHAR name[3] = { L'$', (WCHAR)(L'0' + i) };
		js_define_text(ctor, name, js_text(L""), JP_DONTENUM);
	}
}

/* The string methods. */

BOOL js_string_match(engine *en, js_object *re, js_string *text, js_value *result)
{
	if (!(re->rx_flags & RXF_GLOBAL))
		return js_regexp_exec(en, re, text, TRUE, result);
	js_object *a = NULL;
	UINT at = 0;
	int *spans;
	while (at <= text->length && search_at(en, re, text, at, &spans)) {
		if (!a)
			a = js_array_new(en, 0);
		js_value v = js_str(piece(text, spans[0], spans[1]));
		js_array_push(a, v);
		js_release(v);
		at = spans[1] > spans[0] ? (UINT)spans[1] : (UINT)spans[1] + 1;
		js_free(spans);
	}
	js_put_text(en, re, L"lastIndex", js_number(0));
	*result = a ? js_obj(a) : js_null;
	return TRUE;
}

BOOL js_string_search(engine *en, js_object *re, js_string *text, js_value *result)
{
	int *spans;
	if (!search_at(en, re, text, 0, &spans)) {
		*result = js_number(-1);
		return TRUE;
	}
	*result = js_number(spans[0]);
	js_free(spans);
	return TRUE;
}

/* The replacement for one match: the result of a function, or a string with $$, $&, $`, $' and $1 to $99 in it. */
static BOOL replacement(engine *en, js_value with, js_string *text, const int *spans, UINT groups, js_buffer *out)
{
	if (js_is_callable(with)) {
		UINT argc = groups + 3;
		js_value *argv = js_alloc(argc * sizeof(js_value));
		for (UINT i = 0; i <= groups; i++)
			argv[i] = js_str(piece(text, spans[2 * i], spans[2 * i + 1]));
		argv[groups + 1] = js_number(spans[0]);
		argv[groups + 2] = js_str(js_string_retain(text));
		js_value r;
		BOOL fine = js_call(en, with, js_obj(en->global), argc, argv, &r);
		for (UINT i = 0; i < argc; i++)
			js_release(argv[i]);
		js_free(argv);
		if (!fine)
			return FALSE;
		js_string *s;
		fine = js_to_string(en, r, &s);
		js_release(r);
		if (!fine)
			return FALSE;
		js_buffer_add(out, s->text, s->length);
		js_string_release(s);
		return TRUE;
	}
	const js_string *pattern = with.string;
	for (UINT i = 0; i < pattern->length; i++) {
		WCHAR c = pattern->text[i];
		if (c != L'$' || i + 1 >= pattern->length) {
			js_buffer_char(out, c);
			continue;
		}
		WCHAR d = pattern->text[i + 1];
		if (d == L'$') {
			js_buffer_char(out, L'$');
			i++;
		} else if (d == L'&') {
			js_buffer_add(out, &text->text[spans[0]], spans[1] - spans[0]);
			i++;
		} else if (d == L'`') {
			js_buffer_add(out, text->text, spans[0]);
			i++;
		} else if (d == L'\'') {
			js_buffer_add(out, &text->text[spans[1]], text->length - spans[1]);
			i++;
		} else if (d >= L'0' && d <= L'9') {
			UINT n = d - L'0', used = 1;
			if (i + 2 < pattern->length && pattern->text[i + 2] >= L'0' && pattern->text[i + 2] <= L'9' && n * 10 + (pattern->text[i + 2] - L'0') <= groups) {
				n = n * 10 + (pattern->text[i + 2] - L'0');
				used = 2;
			}
			if (n >= 1 && n <= groups) {
				if (spans[2 * n] >= 0)
					js_buffer_add(out, &text->text[spans[2 * n]], spans[2 * n + 1] - spans[2 * n]);
				i += used;
			} else
				js_buffer_char(out, c);
		} else
			js_buffer_char(out, c);
	}
	return TRUE;
}

BOOL js_string_replace(engine *en, js_value self, UINT argc, js_value *argv, js_value *result)
{
	js_string *text;
	if (self.type == JV_OBJECT && self.object->cls == JC_STRING)
		text = js_string_retain(self.object->primitive.string);
	else if (!js_to_string(en, self, &text))
		return FALSE;
	js_value pattern = js_arg(argc, argv, 0), with = js_arg(argc, argv, 1);
	js_string *with_text = NULL;
	if (!js_is_callable(with)) {
		if (!js_to_string(en, with, &with_text)) {
			js_string_release(text);
			return FALSE;
		}
		with = js_str(with_text);
	}
	js_buffer b = { 0 };
	BOOL fine = TRUE;
	if (pattern.type == JV_OBJECT && pattern.object->cls == JC_REGEXP) {
		js_object *re = pattern.object;
		BOOL global = (re->rx_flags & RXF_GLOBAL) != 0;
		UINT groups = rx_groups(re->regex), at = 0, copied = 0;
		int *spans;
		while (fine && at <= text->length && search_at(en, re, text, at, &spans)) {
			js_buffer_add(&b, &text->text[copied], spans[0] - copied);
			fine = replacement(en, with, text, spans, groups, &b);
			copied = spans[1];
			at = spans[1] > spans[0] ? (UINT)spans[1] : (UINT)spans[1] + 1;
			if (spans[1] == spans[0] && spans[0] < (int)text->length && global) {
				/* An empty match keeps the character it stopped at. */
				js_buffer_char(&b, text->text[spans[0]]);
				copied = spans[0] + 1;
			}
			js_free(spans);
			if (!global)
				break;
		}
		if (global)
			js_put_text(en, re, L"lastIndex", js_number(0));
		if (fine && copied < text->length)
			js_buffer_add(&b, &text->text[copied], text->length - copied);
	} else {
		js_string *what;
		if (!js_to_string(en, pattern, &what))
			fine = FALSE;
		else {
			int found = -1;
			for (UINT i = 0; what->length <= text->length && i <= text->length - what->length; i++)
				if (!memcmp(&text->text[i], what->text, what->length * sizeof(WCHAR))) {
					found = (int)i;
					break;
				}
			if (found < 0)
				js_buffer_add(&b, text->text, text->length);
			else {
				int spans[2] = { found, found + (int)what->length };
				js_buffer_add(&b, text->text, found);
				fine = replacement(en, with, text, spans, 0, &b);
				js_buffer_add(&b, &text->text[spans[1]], text->length - spans[1]);
			}
			js_string_release(what);
		}
	}
	js_string_release(with_text);
	js_string_release(text);
	if (!fine) {
		js_free(b.text);
		return FALSE;
	}
	*result = js_str(b.text ? js_buffer_string(&b) : js_string_of(L""));
	return TRUE;
}

static BOOL push_piece(js_object *a, const js_string *text, int start, int end, UINT32 limit)
{
	if (a->length >= limit)
		return FALSE;
	js_value v = js_str(piece(text, start, end));
	js_array_push(a, v);
	js_release(v);
	return TRUE;
}

BOOL js_string_split(engine *en, js_string *text, js_value separator, UINT32 limit, js_value *result)
{
	js_object *a = js_array_new(en, 0);
	*result = js_obj(a);
	if (!limit)
		return TRUE;
	if (separator.type == JV_UNDEFINED) {
		push_piece(a, text, 0, (int)text->length, limit);
		return TRUE;
	}
	if (separator.type == JV_OBJECT && separator.object->cls == JC_REGEXP) {
		js_object *re = separator.object;
		UINT groups = rx_groups(re->regex);
		int *spans = js_alloc(2 * (groups + 1) * sizeof(int));
		UINT start = 0, at = 0;
		while (at < text->length && rx_search(re->regex, text->text, text->length, at, spans)) {
			/* A match at the very end splits nothing; an empty one where the piece starts moves on a character. */
			if ((UINT)spans[0] >= text->length)
				break;
			if (spans[1] == (int)start) {
				at = spans[0] + 1;
				continue;
			}
			if (!push_piece(a, text, (int)start, spans[0], limit))
				break;
			for (UINT g = 1; g <= groups; g++)
				push_piece(a, text, spans[2 * g], spans[2 * g + 1], limit);
			start = spans[1];
			at = spans[1];
		}
		js_free(spans);
		push_piece(a, text, (int)start, (int)text->length, limit);
		return TRUE;
	}
	js_string *by;
	if (!js_to_string(en, separator, &by)) {
		js_release(*result);
		return FALSE;
	}
	if (!by->length) {
		for (UINT i = 0; i < text->length; i++)
			if (!push_piece(a, text, (int)i, (int)i + 1, limit))
				break;
	} else {
		UINT start = 0;
		for (UINT i = 0; by->length <= text->length && i <= text->length - by->length;) {
			if (!memcmp(&text->text[i], by->text, by->length * sizeof(WCHAR))) {
				if (!push_piece(a, text, (int)start, (int)i, limit))
					break;
				i += by->length;
				start = i;
			} else
				i++;
		}
		push_piece(a, text, (int)start, (int)text->length, limit);
	}
	js_string_release(by);
	return TRUE;
}
