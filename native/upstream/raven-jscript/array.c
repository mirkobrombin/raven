/* SPDX-License-Identifier: MIT */
#include "js.h"

/* Array. The methods are generic as ECMAScript asks: they work through get and put on any object with a length. */

static BOOL length_of(engine *en, js_object *o, UINT32 *length)
{
	if (o->cls == JC_ARRAY) {
		*length = o->length;
		return TRUE;
	}
	js_value v;
	if (!js_get_text(en, o, L"length", &v))
		return FALSE;
	BOOL fine = js_to_uint32(en, v, length);
	js_release(v);
	return fine;
}

static BOOL set_length(engine *en, js_object *o, UINT32 length)
{
	if (o->cls == JC_ARRAY) {
		js_array_set_length(o, length);
		return TRUE;
	}
	return js_put_text(en, o, L"length", js_number(length));
}

static BOOL has_index(engine *en, js_object *o, UINT32 index)
{
	if (o->cls == JC_ARRAY && index < o->length && index < o->elements_room && o->present[index])
		return TRUE;
	js_string *key = js_number_string(index);
	BOOL has = js_has(en, o, key);
	js_string_release(key);
	return has;
}

static BOOL delete_index(engine *en, js_object *o, UINT32 index)
{
	js_string *key = js_number_string(index);
	BOOL deleted;
	BOOL fine = js_delete(en, o, key, &deleted);
	js_string_release(key);
	return fine;
}

/* Reads an element that may be a hole: *present tells which. */
static BOOL element(engine *en, js_object *o, UINT32 index, js_value *result, BOOL *present)
{
	*present = has_index(en, o, index);
	if (!*present) {
		*result = js_undefined;
		return TRUE;
	}
	return js_get_index(en, o, index, result);
}

/* Moves element from to place to, or deletes place to when from is a hole. */
static BOOL move_element(engine *en, js_object *o, UINT32 from, UINT32 to)
{
	js_value v;
	BOOL present;
	if (!element(en, o, from, &v, &present))
		return FALSE;
	BOOL fine = present ? js_put_index(en, o, to, v) : delete_index(en, o, to);
	js_release(v);
	return fine;
}

static BOOL array_ctor(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	if (argc == 1 && argv[0].type == JV_NUMBER) {
		double n = argv[0].number;
		if (n < 0 || n != js_uint32(n))
			return js_throw_error(en, L"RangeError", JS_E_ARRAY_LENGTH, NULL);
		*result = js_obj(js_array_new(en, js_uint32(n)));
		return TRUE;
	}
	js_object *a = js_array_new(en, 0);
	for (UINT i = 0; i < argc; i++)
		js_array_push(a, argv[i]);
	*result = js_obj(a);
	return TRUE;
}

static BOOL this_object(engine *en, js_value self, js_object **out) { return js_to_object(en, self, out); }

static BOOL join(engine *en, js_object *o, const js_string *separator, js_value *result)
{
	UINT32 length;
	if (!length_of(en, o, &length))
		return FALSE;
	/* A join that reaches the same array again, as a = [1]; a[1] = a does, gives the empty string for it. */
	if (o->at_end) {
		*result = js_text(L"");
		return TRUE;
	}
	o->at_end = TRUE;
	js_buffer b = { 0 };
	BOOL fine = TRUE;
	for (UINT32 i = 0; i < length && fine; i++) {
		if (i)
			js_buffer_add(&b, separator->text, separator->length);
		js_value v;
		if (!(fine = js_get_index(en, o, i, &v)))
			break;
		if (v.type != JV_UNDEFINED && v.type != JV_NULL) {
			js_string *s;
			if ((fine = js_to_string(en, v, &s))) {
				js_buffer_add(&b, s->text, s->length);
				js_string_release(s);
			}
		}
		js_release(v);
	}
	o->at_end = FALSE;
	if (!fine) {
		js_free(b.text);
		return FALSE;
	}
	*result = js_str(b.text ? js_buffer_string(&b) : js_string_of(L""));
	return TRUE;
}

static BOOL array_join(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	if (!this_object(en, self, &o))
		return FALSE;
	js_string *separator;
	js_value sep = js_arg(argc, argv, 0);
	if (sep.type == JV_UNDEFINED)
		separator = js_string_of(L",");
	else if (!js_to_string(en, sep, &separator)) {
		js_object_release(o);
		return FALSE;
	}
	BOOL fine = join(en, o, separator, result);
	js_string_release(separator);
	js_object_release(o);
	return fine;
}

static BOOL array_to_string(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	if (self.type != JV_OBJECT || self.object->cls != JC_ARRAY)
		return js_throw_error(en, L"TypeError", JS_E_ARRAY_EXPECTED, NULL);
	js_string *separator = js_string_of(L",");
	BOOL fine = join(en, self.object, separator, result);
	js_string_release(separator);
	return fine;
}

static BOOL array_push(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	UINT32 length;
	if (!this_object(en, self, &o))
		return FALSE;
	BOOL fine = length_of(en, o, &length);
	for (UINT i = 0; i < argc && fine; i++)
		fine = js_put_index(en, o, length + i, argv[i]);
	if (fine)
		fine = set_length(en, o, length + argc);
	if (fine)
		*result = js_number((double)length + argc);
	js_object_release(o);
	return fine;
}

static BOOL array_pop(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	UINT32 length;
	if (!this_object(en, self, &o))
		return FALSE;
	BOOL fine = length_of(en, o, &length);
	if (fine && !length) {
		*result = js_undefined;
		fine = set_length(en, o, 0);
	} else if (fine && (fine = js_get_index(en, o, length - 1, result))) {
		fine = delete_index(en, o, length - 1) && set_length(en, o, length - 1);
		if (!fine)
			js_release(*result);
	}
	js_object_release(o);
	return fine;
}

static BOOL array_shift(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	UINT32 length;
	if (!this_object(en, self, &o))
		return FALSE;
	BOOL fine = length_of(en, o, &length);
	if (fine && !length) {
		*result = js_undefined;
		fine = set_length(en, o, 0);
	} else if (fine && (fine = js_get_index(en, o, 0, result))) {
		for (UINT32 i = 1; i < length && fine; i++)
			fine = move_element(en, o, i, i - 1);
		if (fine)
			fine = delete_index(en, o, length - 1) && set_length(en, o, length - 1);
		if (!fine)
			js_release(*result);
	}
	js_object_release(o);
	return fine;
}

static BOOL array_unshift(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	UINT32 length;
	if (!this_object(en, self, &o))
		return FALSE;
	BOOL fine = length_of(en, o, &length);
	for (UINT32 i = length; i > 0 && fine; i--)
		fine = move_element(en, o, i - 1, i - 1 + argc);
	for (UINT i = 0; i < argc && fine; i++)
		fine = js_put_index(en, o, i, argv[i]);
	if (fine)
		fine = set_length(en, o, length + argc);
	/* JScript gives back undefined, as ECMAScript 3 does not; scripts that read it are rare. */
	if (fine)
		*result = js_number((double)length + argc);
	js_object_release(o);
	return fine;
}

static BOOL array_reverse(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	UINT32 length;
	if (!this_object(en, self, &o))
		return FALSE;
	BOOL fine = length_of(en, o, &length);
	for (UINT32 low = 0, high = length ? length - 1 : 0; low < high && fine; low++, high--) {
		js_value a, b;
		BOOL has_a, has_b;
		if (!(fine = element(en, o, low, &a, &has_a)))
			break;
		if (!(fine = element(en, o, high, &b, &has_b))) {
			js_release(a);
			break;
		}
		fine = (has_b ? js_put_index(en, o, low, b) : delete_index(en, o, low)) && (has_a ? js_put_index(en, o, high, a) : delete_index(en, o, high));
		js_release(a);
		js_release(b);
	}
	if (fine)
		*result = js_obj(o);
	else
		js_object_release(o);
	return fine;
}

/* The start of a slice or splice, relative to the length when negative. */
static BOOL relative(engine *en, js_value v, UINT32 length, UINT32 fallback, UINT32 *out)
{
	if (v.type == JV_UNDEFINED) {
		*out = fallback;
		return TRUE;
	}
	double x;
	if (!js_to_integer(en, v, &x))
		return FALSE;
	if (x < 0)
		x = x + length < 0 ? 0 : x + length;
	else if (x > length)
		x = length;
	*out = (UINT32)x;
	return TRUE;
}

static BOOL array_slice(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	UINT32 length, start, end;
	if (!this_object(en, self, &o))
		return FALSE;
	BOOL fine = length_of(en, o, &length) && relative(en, js_arg(argc, argv, 0), length, 0, &start) &&
		relative(en, js_arg(argc, argv, 1), length, length, &end);
	js_object *a = NULL;
	if (fine) {
		a = js_array_new(en, 0);
		for (UINT32 i = start; i < end && fine; i++) {
			js_value v;
			BOOL present;
			if ((fine = element(en, o, i, &v, &present)) && present)
				js_put_index(en, a, i - start, v);
			js_release(v);
		}
		if (fine && end > start)
			js_array_set_length(a, end - start);
	}
	js_object_release(o);
	if (!fine) {
		js_object_release(a);
		return FALSE;
	}
	*result = js_obj(a);
	return TRUE;
}

static BOOL array_splice(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	UINT32 length, start, count = 0;
	if (!this_object(en, self, &o))
		return FALSE;
	BOOL fine = length_of(en, o, &length) && relative(en, js_arg(argc, argv, 0), length, 0, &start);
	if (fine && argc >= 2) {
		double x;
		if ((fine = js_to_integer(en, argv[1], &x)))
			count = x < 0 ? 0 : x > length - start ? length - start : (UINT32)x;
	} else if (fine && argc == 1)
		/* JScript removes everything from the start when the count is missing. */
		count = length - start;
	js_object *removed = fine ? js_array_new(en, 0) : NULL;
	for (UINT32 i = 0; i < count && fine; i++) {
		js_value v;
		BOOL present;
		if ((fine = element(en, o, start + i, &v, &present)) && present)
			js_put_index(en, removed, i, v);
		js_release(v);
	}
	if (fine && removed)
		js_array_set_length(removed, count);
	UINT added = argc > 2 ? argc - 2 : 0;
	if (fine && added < count) {
		for (UINT32 i = start; i + count < length && fine; i++)
			fine = move_element(en, o, i + count, i + added);
		for (UINT32 i = length; i > length - count + added && fine; i--)
			fine = delete_index(en, o, i - 1);
	} else if (fine && added > count) {
		for (UINT32 i = length - count; i > start && fine; i--)
			fine = move_element(en, o, i + count - 1, i + added - 1);
	}
	for (UINT i = 0; i < added && fine; i++)
		fine = js_put_index(en, o, start + i, argv[i + 2]);
	if (fine)
		fine = set_length(en, o, length - count + added);
	js_object_release(o);
	if (!fine) {
		js_object_release(removed);
		return FALSE;
	}
	*result = js_obj(removed);
	return TRUE;
}

static BOOL array_concat(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	if (!this_object(en, self, &o))
		return FALSE;
	js_object *a = js_array_new(en, 0);
	UINT32 n = 0;
	BOOL fine = TRUE;
	for (UINT k = 0; k <= argc && fine; k++) {
		js_value item = k ? argv[k - 1] : js_obj(o);
		if (item.type == JV_OBJECT && item.object->cls == JC_ARRAY) {
			js_object *source = item.object;
			for (UINT32 i = 0; i < source->length && fine; i++) {
				js_value v;
				BOOL present;
				if ((fine = element(en, source, i, &v, &present)) && present)
					js_put_index(en, a, n + i, v);
				js_release(v);
			}
			n += source->length;
			js_array_set_length(a, n);
		} else
			js_put_index(en, a, n++, item);
	}
	js_object_release(o);
	if (!fine) {
		js_object_release(a);
		return FALSE;
	}
	*result = js_obj(a);
	return TRUE;
}

/* sort: a merge sort, which is stable and calls the comparison a bounded number of times even when it is not consistent. */
typedef struct {
	engine *en;
	js_value compare;
	BOOL failed;
} sorter;

static int compare_values(sorter *s, js_value a, js_value b)
{
	if (s->failed)
		return 0;
	if (a.type == JV_UNDEFINED)
		return b.type == JV_UNDEFINED ? 0 : 1;
	if (b.type == JV_UNDEFINED)
		return -1;
	if (s->compare.type != JV_UNDEFINED) {
		js_value args[2] = { a, b }, r;
		double x;
		if (!js_call(s->en, s->compare, js_obj(s->en->global), 2, args, &r)) {
			s->failed = TRUE;
			return 0;
		}
		BOOL fine = js_to_number(s->en, r, &x);
		js_release(r);
		if (!fine) {
			s->failed = TRUE;
			return 0;
		}
		return x < 0 ? -1 : x > 0 ? 1 : 0;
	}
	js_string *sa, *sb;
	if (!js_to_string(s->en, a, &sa)) {
		s->failed = TRUE;
		return 0;
	}
	if (!js_to_string(s->en, b, &sb)) {
		js_string_release(sa);
		s->failed = TRUE;
		return 0;
	}
	UINT n = sa->length < sb->length ? sa->length : sb->length;
	int c = 0;
	for (UINT i = 0; i < n && !c; i++)
		c = sa->text[i] < sb->text[i] ? -1 : sa->text[i] > sb->text[i] ? 1 : 0;
	if (!c)
		c = sa->length < sb->length ? -1 : sa->length > sb->length ? 1 : 0;
	js_string_release(sa);
	js_string_release(sb);
	return c;
}

static void merge_sort(sorter *s, js_value *items, js_value *scratch, UINT32 count)
{
	if (count < 2)
		return;
	UINT32 half = count / 2;
	merge_sort(s, items, scratch, half);
	merge_sort(s, items + half, scratch, count - half);
	UINT32 i = 0, j = half, k = 0;
	while (i < half && j < count)
		scratch[k++] = compare_values(s, items[j], items[i]) < 0 ? items[j++] : items[i++];
	while (i < half)
		scratch[k++] = items[i++];
	while (j < count)
		scratch[k++] = items[j++];
	memcpy(items, scratch, count * sizeof(js_value));
}

static BOOL array_sort(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	UINT32 length;
	if (!this_object(en, self, &o))
		return FALSE;
	sorter s = { en, js_arg(argc, argv, 0), FALSE };
	if (s.compare.type != JV_UNDEFINED && !js_is_callable(s.compare)) {
		js_object_release(o);
		return js_throw_error(en, L"TypeError", JS_E_FUNCTION_EXPECTED, NULL);
	}
	BOOL fine = length_of(en, o, &length);
	js_value *items = fine && length ? js_alloc(length * sizeof(js_value)) : NULL;
	UINT32 count = 0;
	/* The holes go to the end, after the undefined values. */
	for (UINT32 i = 0; i < length && fine; i++) {
		js_value v;
		BOOL present;
		if ((fine = element(en, o, i, &v, &present)) && present)
			items[count++] = v;
	}
	if (fine) {
		js_value *scratch = count ? js_alloc(count * sizeof(js_value)) : NULL;
		merge_sort(&s, items, scratch, count);
		js_free(scratch);
		fine = !s.failed;
	}
	for (UINT32 i = 0; i < count && fine; i++)
		fine = js_put_index(en, o, i, items[i]);
	for (UINT32 i = count; i < length && fine; i++)
		fine = delete_index(en, o, i);
	for (UINT32 i = 0; i < count; i++)
		js_release(items[i]);
	js_free(items);
	if (!fine) {
		js_object_release(o);
		return FALSE;
	}
	*result = js_obj(o);
	return TRUE;
}

void js_init_array(engine *en)
{
	js_object *proto = en->array_prototype;
	js_object *ctor = js_define_native(en->global, L"Array", array_ctor, 1);
	js_define_text(ctor, L"prototype", js_obj(js_object_retain(proto)), JP_HIDDEN | JP_READONLY);
	js_define_text(proto, L"constructor", js_obj(js_object_retain(ctor)), JP_DONTENUM);
	static const struct {
		const WCHAR *name;
		js_native fn;
		UINT length;
	} methods[] = {
		{ L"concat", array_concat, 1 }, { L"join", array_join, 1 }, { L"pop", array_pop, 0 }, { L"push", array_push, 1 },
		{ L"reverse", array_reverse, 0 }, { L"shift", array_shift, 0 }, { L"slice", array_slice, 2 }, { L"sort", array_sort, 1 },
		{ L"splice", array_splice, 2 }, { L"toLocaleString", array_to_string, 0 }, { L"toString", array_to_string, 0 },
		{ L"unshift", array_unshift, 1 },
	};
	for (UINT i = 0; i < ARRAYSIZE(methods); i++)
		js_define_native(proto, methods[i].name, methods[i].fn, methods[i].length);
}
