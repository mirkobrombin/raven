/* SPDX-License-Identifier: MIT */
#include "js.h"
#include "../raven-vbscript/regex.h"

/* Objects: their own properties in the order they were made, found through a hash; arrays keep their elements apart, by index. The
 * property operations follow the prototype chain, and send the host's objects through IDispatch. Every object of an engine is on its
 * list; closing the engine empties them all, which breaks the cycles reference counts cannot. */

static UINT hash_of(const WCHAR *text, UINT length)
{
	UINT hash = 2166136261u;
	for (UINT i = 0; i < length; i++)
		hash = (hash ^ text[i]) * 16777619u;
	return hash;
}

js_object *js_object_new(engine *en, js_class cls, js_object *proto)
{
	js_object *o = js_alloc(sizeof(js_object));
	o->refs = 1;
	o->cls = cls;
	o->en = en;
	o->proto = js_object_retain(proto);
	VariantInit(&o->item);
	o->next_object = en->objects;
	if (en->objects)
		en->objects->previous_object = o;
	en->objects = o;
	return o;
}

/* Lets go of everything the object holds; it stays, empty, while references to it remain. */
static void empty_object(js_object *o)
{
	js_property *props = o->props;
	UINT count = o->count;
	js_value *elements = o->elements;
	/* Only the dense part holds elements: a length past it is holes or entries of the property table. */
	UINT length = o->length < o->elements_room ? o->length : o->elements_room;
	BYTE *present = o->present;
	js_object *proto = o->proto, *bound = o->bound;
	js_scope *scope = o->scope;
	js_value primitive = o->primitive;
	js_string *source = o->source;
	o->props = NULL;
	o->count = o->room = 0;
	js_free(o->index);
	o->index = NULL;
	o->index_size = 0;
	o->elements = NULL;
	o->present = NULL;
	o->length = o->elements_room = 0;
	o->proto = o->bound = NULL;
	o->scope = NULL;
	o->primitive = js_undefined;
	o->source = NULL;
	if (o->regex) {
		rx_free(o->regex);
		o->regex = NULL;
	}
	if (o->dispatch) {
		IDispatch_Release(o->dispatch);
		o->dispatch = NULL;
	}
	if (o->enumerator) {
		IEnumVARIANT_Release(o->enumerator);
		o->enumerator = NULL;
	}
	VariantClear(&o->item);
	if (o->array) {
		SafeArrayDestroy(o->array);
		o->array = NULL;
	}
	for (UINT i = 0; i < count; i++) {
		js_string_release(props[i].name);
		js_release(props[i].value);
	}
	js_free(props);
	for (UINT i = 0; i < length; i++)
		if (present[i])
			js_release(elements[i]);
	js_free(elements);
	js_free(present);
	js_object_release(proto);
	js_object_release(bound);
	js_scope_release(scope);
	js_release(primitive);
	js_string_release(source);
}

void js_object_release(js_object *o)
{
	if (!o || InterlockedDecrement(&o->refs))
		return;
	engine *en = o->en;
	/* Keep it alive while it is emptied, so a cycle back to it cannot free it twice. */
	o->refs = 1;
	empty_object(o);
	if (InterlockedDecrement(&o->refs))
		return;
	js_outward_detach(o);
	if (o->previous_object)
		o->previous_object->next_object = o->next_object;
	else if (en->objects == o)
		en->objects = o->next_object;
	if (o->next_object)
		o->next_object->previous_object = o->previous_object;
	js_free(o);
}

/* Empties every object of the engine; the ones nothing outside holds go with it. */
void js_free_objects(engine *en)
{
	for (;;) {
		js_object *o = en->objects;
		/* Take a reference to each in turn so emptying one cannot free the next under the walk. */
		UINT count = 0;
		for (js_object *k = o; k; k = k->next_object)
			count++;
		if (!count)
			return;
		js_object **all = js_alloc(count * sizeof(js_object *));
		UINT i = 0;
		for (js_object *k = o; k; k = k->next_object)
			all[i++] = js_object_retain(k);
		BOOL any = FALSE;
		for (i = 0; i < count; i++)
			if (all[i]->props || all[i]->elements || all[i]->proto || all[i]->scope || all[i]->dispatch || all[i]->primitive.type == JV_OBJECT ||
				all[i]->bound) {
				empty_object(all[i]);
				any = TRUE;
			}
		for (i = 0; i < count; i++)
			js_object_release(all[i]);
		js_free(all);
		if (!any)
			return;
	}
}

/* Own properties. */

static void rebuild_index(js_object *o)
{
	UINT size = 8;
	while (size < o->count * 2)
		size *= 2;
	js_free(o->index);
	o->index = js_alloc(size * sizeof(UINT));
	o->index_size = size;
	for (UINT i = 0; i < size; i++)
		o->index[i] = (UINT)-1;
	for (UINT i = 0; i < o->count; i++) {
		UINT at = o->props[i].hash & (size - 1);
		while (o->index[at] != (UINT)-1)
			at = (at + 1) & (size - 1);
		o->index[at] = i;
	}
}

js_property *js_own(js_object *o, const js_string *name)
{
	if (!o->count)
		return NULL;
	if (o->count < 8) {
		for (UINT i = 0; i < o->count; i++)
			if (js_string_equal(o->props[i].name, name))
				return &o->props[i];
		return NULL;
	}
	if (!o->index)
		rebuild_index(o);
	UINT hash = hash_of(name->text, name->length);
	for (UINT at = hash & (o->index_size - 1); o->index[at] != (UINT)-1; at = (at + 1) & (o->index_size - 1)) {
		js_property *p = &o->props[o->index[at]];
		if (p->hash == hash && js_string_equal(p->name, name))
			return p;
	}
	return NULL;
}

/* Sets an own property, making it with the flags when it is not there; takes the value's reference. */
void js_define(js_object *o, js_string *name, js_value value, DWORD flags)
{
	js_property *p = js_own(o, name);
	if (p) {
		js_value old = p->value;
		p->value = value;
		p->flags = flags;
		js_release(old);
		return;
	}
	if (o->count == o->room) {
		o->room = o->room ? o->room * 2 : 4;
		o->props = js_grow(o->props, o->room * sizeof(js_property));
	}
	p = &o->props[o->count++];
	p->name = js_string_retain(name);
	p->value = value;
	p->flags = flags;
	p->hash = hash_of(name->text, name->length);
	if (o->index && o->count * 2 <= o->index_size) {
		UINT at = p->hash & (o->index_size - 1);
		while (o->index[at] != (UINT)-1)
			at = (at + 1) & (o->index_size - 1);
		o->index[at] = o->count - 1;
	} else if (o->index) {
		js_free(o->index);
		o->index = NULL;
	}
}

void js_define_text(js_object *o, const WCHAR *name, js_value value, DWORD flags)
{
	js_string *key = js_string_of(name);
	js_define(o, key, value, flags);
	js_string_release(key);
}

js_object *js_define_native(js_object *o, const WCHAR *name, js_native fn, UINT length)
{
	js_object *f = js_native_new(o->en, fn, length, NULL);
	js_define_text(o, name, js_obj(js_object_retain(f)), JP_DONTENUM);
	js_object_release(f);
	return f;
}

BOOL js_remove_own(js_object *o, const js_string *name)
{
	js_property *p = js_own(o, name);
	if (!p)
		return FALSE;
	UINT at = (UINT)(p - o->props);
	js_string_release(p->name);
	js_release(p->value);
	memmove(&o->props[at], &o->props[at + 1], (o->count - at - 1) * sizeof(js_property));
	o->count--;
	js_free(o->index);
	o->index = NULL;
	return TRUE;
}

/* Arrays. */

BOOL js_array_index(const js_string *name, UINT32 *index)
{
	if (!name->length || name->length > 10 || (name->length > 1 && name->text[0] == L'0'))
		return FALSE;
	ULONGLONG value = 0;
	for (UINT i = 0; i < name->length; i++) {
		if (name->text[i] < L'0' || name->text[i] > L'9')
			return FALSE;
		value = value * 10 + (name->text[i] - L'0');
	}
	if (value >= 0xffffffffULL)
		return FALSE;
	*index = (UINT32)value;
	return TRUE;
}

/* Elements past this go to the property table, so a[4000000000] = 1 does not ask for the memory of every index before it. */
#define DENSE_LIMIT (1u << 24)

static void ensure_elements(js_object *a, UINT32 length)
{
	if (length <= a->elements_room)
		return;
	UINT room = a->elements_room ? a->elements_room : 8;
	while (room < length)
		room *= 2;
	a->elements = js_grow(a->elements, room * sizeof(js_value));
	a->present = js_grow(a->present, room);
	a->elements_room = room;
}

void js_array_set_length(js_object *a, UINT32 length)
{
	UINT32 dense = a->length < a->elements_room ? a->length : a->elements_room;
	if (length < dense) {
		for (UINT32 i = length; i < dense; i++)
			if (a->present[i]) {
				js_release(a->elements[i]);
				a->present[i] = 0;
				a->elements[i] = js_undefined;
			}
	}
	/* Sparse elements at or past the new length go too. */
	for (UINT i = 0; i < a->count;) {
		UINT32 index;
		if (js_array_index(a->props[i].name, &index) && index >= length) {
			js_remove_own(a, a->props[i].name);
			continue;
		}
		i++;
	}
	a->length = length;
}

static BOOL array_get_index(js_object *a, UINT32 index, js_value *result)
{
	if (index < a->length && index < a->elements_room && a->present[index]) {
		*result = js_retain(a->elements[index]);
		return TRUE;
	}
	return FALSE;
}

static void array_put_index(js_object *a, UINT32 index, js_value value)
{
	if (index < DENSE_LIMIT) {
		ensure_elements(a, index + 1);
		if (a->present[index])
			js_release(a->elements[index]);
		a->elements[index] = js_retain(value);
		a->present[index] = 1;
	} else {
		js_string *name = js_number_string(index);
		js_define(a, name, js_retain(value), 0);
		js_string_release(name);
	}
	if (index >= a->length)
		a->length = index + 1;
}

void js_array_push(js_object *a, js_value value) { array_put_index(a, a->length, value); }

js_object *js_array_new(engine *en, UINT length)
{
	js_object *a = js_object_new(en, JC_ARRAY, en->array_prototype);
	a->length = length;
	return a;
}

/* Property operations. */

static BOOL string_index(js_object *o, const js_string *name, js_value *result)
{
	UINT32 index;
	if (o->cls == JC_STRING && js_array_index(name, &index) && index < o->primitive.string->length) {
		*result = js_str(js_string_new(&o->primitive.string->text[index], 1));
		return TRUE;
	}
	if (o->cls == JC_STRING && js_string_is(name, L"length")) {
		*result = js_number(o->primitive.string->length);
		return TRUE;
	}
	return FALSE;
}

/* What an object has of its own, not following the prototype; FALSE when it has nothing by the name. */
static BOOL get_own(engine *en, js_object *o, js_string *name, js_value *result, BOOL *threw)
{
	*threw = FALSE;
	if (o->cls == JC_ARRAY) {
		UINT32 index;
		if (js_string_is(name, L"length")) {
			*result = js_number(o->length);
			return TRUE;
		}
		if (js_array_index(name, &index) && array_get_index(o, index, result))
			return TRUE;
	}
	if (string_index(o, name, result))
		return TRUE;
	js_property *p = js_own(o, name);
	if (p) {
		*result = js_retain(p->value);
		return TRUE;
	}
	if (o->cls == JC_FUNCTION && js_string_is(name, L"length")) {
		*result = js_number(o->code ? o->code->param_count : o->native_length);
		return TRUE;
	}
	if (o->cls == JC_DISPATCH) {
		if (!js_dispatch_get(en, o, name, result)) {
			*threw = TRUE;
			return FALSE;
		}
		return TRUE;
	}
	return FALSE;
}

BOOL js_get(engine *en, js_object *o, js_string *name, js_value *result)
{
	*result = js_undefined;
	for (js_object *k = o; k; k = k->proto) {
		BOOL threw;
		if (get_own(en, k, name, result, &threw))
			return TRUE;
		if (threw)
			return FALSE;
	}
	return TRUE;
}

BOOL js_get_text(engine *en, js_object *o, const WCHAR *name, js_value *result)
{
	js_string *key = js_string_of(name);
	BOOL fine = js_get(en, o, key, result);
	js_string_release(key);
	return fine;
}

BOOL js_get_index(engine *en, js_object *o, UINT32 index, js_value *result)
{
	if (o->cls == JC_ARRAY && array_get_index(o, index, result))
		return TRUE;
	js_string *key = js_number_string(index);
	BOOL fine = js_get(en, o, key, result);
	js_string_release(key);
	return fine;
}

/* A put that the prototype chain forbids does nothing. */
static BOOL read_only(js_object *o, const js_string *name)
{
	for (js_object *k = o; k; k = k->proto) {
		js_property *p = js_own(k, name);
		if (p)
			return (p->flags & JP_READONLY) != 0;
	}
	return FALSE;
}

BOOL js_put(engine *en, js_object *o, js_string *name, js_value value)
{
	if (o->cls == JC_DISPATCH)
		return js_dispatch_put(en, o, name, value);
	if (o->cls == JC_ARRAY) {
		UINT32 index;
		if (js_string_is(name, L"length")) {
			double length;
			if (!js_to_number(en, value, &length))
				return FALSE;
			if (length < 0 || length != js_uint32(length) || !js_is_finite(length))
				return js_throw_error(en, L"RangeError", JS_E_ARRAY_LENGTH, NULL);
			js_array_set_length(o, js_uint32(length));
			return TRUE;
		}
		if (js_array_index(name, &index)) {
			array_put_index(o, index, value);
			return TRUE;
		}
	}
	if (o->cls == JC_STRING && (js_string_is(name, L"length")))
		return TRUE;
	if (o->cls == JC_FUNCTION && js_string_is(name, L"length"))
		return TRUE;
	js_property *p = js_own(o, name);
	if (p) {
		if (p->flags & JP_READONLY)
			return TRUE;
		js_value old = p->value;
		p->value = js_retain(value);
		js_release(old);
		return TRUE;
	}
	if (read_only(o->proto, name))
		return TRUE;
	js_define(o, name, js_retain(value), 0);
	return TRUE;
}

BOOL js_put_text(engine *en, js_object *o, const WCHAR *name, js_value value)
{
	js_string *key = js_string_of(name);
	BOOL fine = js_put(en, o, key, value);
	js_string_release(key);
	return fine;
}

BOOL js_put_index(engine *en, js_object *o, UINT32 index, js_value value)
{
	if (o->cls == JC_ARRAY) {
		array_put_index(o, index, value);
		return TRUE;
	}
	js_string *key = js_number_string(index);
	BOOL fine = js_put(en, o, key, value);
	js_string_release(key);
	return fine;
}

static BOOL has_own(engine *en, js_object *o, js_string *name)
{
	UINT32 index;
	if (o->cls == JC_ARRAY) {
		if (js_string_is(name, L"length"))
			return TRUE;
		if (js_array_index(name, &index) && index < o->length && index < o->elements_room && o->present[index])
			return TRUE;
	}
	if (o->cls == JC_STRING && (js_string_is(name, L"length") || (js_array_index(name, &index) && index < o->primitive.string->length)))
		return TRUE;
	if (o->cls == JC_FUNCTION && js_string_is(name, L"length"))
		return TRUE;
	if (js_own(o, name))
		return TRUE;
	if (o->cls == JC_DISPATCH)
		return js_dispatch_has(en, o, name);
	return FALSE;
}

BOOL js_has(engine *en, js_object *o, js_string *name)
{
	for (js_object *k = o; k; k = k->proto)
		if (has_own(en, k, name))
			return TRUE;
	return FALSE;
}

BOOL js_has_own(engine *en, js_object *o, js_string *name) { return has_own(en, o, name); }

BOOL js_delete(engine *en, js_object *o, js_string *name, BOOL *deleted)
{
	*deleted = TRUE;
	UINT32 index;
	if (o->cls == JC_ARRAY && js_array_index(name, &index) && index < o->length && index < o->elements_room) {
		if (o->present[index]) {
			js_release(o->elements[index]);
			o->elements[index] = js_undefined;
			o->present[index] = 0;
		}
		return TRUE;
	}
	if ((o->cls == JC_ARRAY || o->cls == JC_STRING || o->cls == JC_FUNCTION) && js_string_is(name, L"length")) {
		*deleted = FALSE;
		return TRUE;
	}
	js_property *p = js_own(o, name);
	if (p) {
		if (p->flags & JP_DONTDELETE) {
			*deleted = FALSE;
			return TRUE;
		}
		js_remove_own(o, name);
	}
	return TRUE;
}

/* for-in: array indexes first, then the enumerable properties, own before inherited, each name once. */
js_string **js_enumerate(engine *en, js_object *o, UINT *count)
{
	js_string **names = NULL;
	UINT n = 0, room = 0;
#define ADD_NAME(s) do { \
		if (n == room) { \
			room = room ? room * 2 : 16; \
			names = js_grow(names, room * sizeof(js_string *)); \
		} \
		names[n++] = (s); \
	} while (0)
	for (js_object *k = o; k; k = k->proto) {
		if (k->cls == JC_ARRAY)
			for (UINT32 i = 0; i < k->length && i < k->elements_room; i++)
				if (k->present[i])
					ADD_NAME(js_number_string(i));
		if (k->cls == JC_STRING)
			for (UINT32 i = 0; i < k->primitive.string->length; i++)
				ADD_NAME(js_number_string(i));
		for (UINT i = 0; i < k->count; i++) {
			if (k->props[i].flags & JP_DONTENUM)
				continue;
			BOOL seen = FALSE;
			for (UINT j = 0; j < n && !seen; j++)
				seen = js_string_equal(names[j], k->props[i].name);
			/* A name an object nearer the start has, even hidden, hides the one further along. */
			for (js_object *nearer = o; nearer != k && !seen; nearer = nearer->proto) {
				js_property *p = js_own(nearer, k->props[i].name);
				seen = p != NULL;
			}
			if (!seen)
				ADD_NAME(js_string_retain(k->props[i].name));
		}
	}
#undef ADD_NAME
	*count = n;
	return names;
}

/* Functions. */

js_object *js_native_new(engine *en, js_native fn, UINT length, const void *data)
{
	js_object *f = js_object_new(en, JC_FUNCTION, en->function_prototype);
	f->native = fn;
	f->native_length = length;
	f->native_data = data;
	return f;
}

BOOL js_is_callable(const js_value v)
{
	return v.type == JV_OBJECT && (v.object->cls == JC_FUNCTION || v.object->cls == JC_DISPATCH);
}

js_scope *js_scope_new(js_object *object, js_scope *outer)
{
	js_scope *scope = js_alloc(sizeof(js_scope));
	scope->refs = 1;
	scope->object = js_object_retain(object);
	scope->outer = outer;
	if (outer)
		InterlockedIncrement(&outer->refs);
	return scope;
}

void js_scope_release(js_scope *scope)
{
	while (scope && !InterlockedDecrement(&scope->refs)) {
		js_scope *outer = scope->outer;
		js_object_release(scope->object);
		js_free(scope);
		scope = outer;
	}
}
