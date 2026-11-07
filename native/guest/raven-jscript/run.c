/* SPDX-License-Identifier: MIT */
#include "js.h"
#include "../raven-vbscript/regex.h"
#include <math.h>

/* The interpreter: it walks the syntax tree. Expressions give values; statements give a completion that says whether they ended
 * normally, with break, continue or return, or with a throw, whose value waits in the engine. Names resolve along the scope chain,
 * whose last link is the global object; the host's named items are found from there. */

/* Deeper calls than this fail with "Out of stack space" before the thread's stack runs out. */
#define MAX_DEPTH 300

/* Exceptions. */

BOOL js_throw(engine *en, js_value v)
{
	if (en->throwing)
		js_release(en->exception);
	en->throwing = TRUE;
	en->exception = js_retain(v);
	en->exception_line = en->line;
	en->exception_column = en->column;
	return FALSE;
}

BOOL js_throw_error(engine *en, const WCHAR *kind, LONG number, const WCHAR *argument)
{
	WCHAR text[512];
	const WCHAR *format = js_error_text(number);
	if (argument)
		wsprintfW(text, format, argument);
	else
		lstrcpynW(text, format, ARRAYSIZE(text));
	js_object *error = js_error_new(en, kind, JS_HRESULT(number), text);
	BOOL fine = js_throw(en, js_obj(error));
	js_object_release(error);
	return fine;
}

BOOL js_throw_hresult(engine *en, HRESULT hr, const EXCEPINFO *info)
{
	if (en->throwing)
		return FALSE;
	WCHAR text[512] = L"";
	if (info && info->bstrDescription)
		lstrcpynW(text, info->bstrDescription, ARRAYSIZE(text));
	else if (HRESULT_FACILITY(hr) == FACILITY_CONTROL)
		lstrcpynW(text, js_error_text(HRESULT_CODE(hr)), ARRAYSIZE(text));
	else if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)hr, 0, text, ARRAYSIZE(text), NULL))
		text[0] = 0;
	/* System messages end with a line break. */
	UINT length = lstrlenW(text);
	while (length && (text[length - 1] == L'\n' || text[length - 1] == L'\r'))
		text[--length] = 0;
	if (info && info->scode)
		hr = info->scode;
	else if (info && info->wCode)
		hr = JS_HRESULT(info->wCode);
	js_object *error = js_error_new(en, L"Error", hr, text);
	BOOL fine = js_throw(en, js_obj(error));
	js_object_release(error);
	return fine;
}

/* Conversions. */

BOOL js_to_boolean(js_value v)
{
	switch (v.type) {
	case JV_UNDEFINED:
	case JV_NULL:
		return FALSE;
	case JV_BOOL:
		return v.boolean;
	case JV_NUMBER:
		return v.number != 0 && !js_is_nan(v.number);
	case JV_STRING:
		return v.string->length != 0;
	case JV_DATE:
	case JV_OBJECT:
		return TRUE;
	}
	return FALSE;
}

BOOL js_to_primitive(engine *en, js_value v, int hint, js_value *result)
{
	if (v.type == JV_DATE) {
		*result = js_str(js_date_string(v.number, 0));
		return TRUE;
	}
	if (v.type != JV_OBJECT) {
		*result = js_retain(v);
		return TRUE;
	}
	js_object *o = v.object;
	if (o->cls == JC_DISPATCH) {
		/* The host's object gives its default value. */
		js_value value;
		if (!js_dispatch_call(en, o->dispatch, DISPID_VALUE, DISPATCH_PROPERTYGET | DISPATCH_METHOD, 0, NULL, &value))
			return FALSE;
		if (value.type == JV_OBJECT) {
			js_release(value);
			return js_throw_error(en, L"TypeError", JS_E_NO_PROPERTY, NULL);
		}
		*result = value;
		return TRUE;
	}
	if (hint == HINT_NONE)
		hint = o->cls == JC_DATE ? HINT_STRING : HINT_NUMBER;
	const WCHAR *order[2] = { hint == HINT_STRING ? L"toString" : L"valueOf", hint == HINT_STRING ? L"valueOf" : L"toString" };
	for (int i = 0; i < 2; i++) {
		js_value method;
		if (!js_get_text(en, o, order[i], &method))
			return FALSE;
		if (js_is_callable(method)) {
			js_value value;
			BOOL fine = js_call(en, method, v, 0, NULL, &value);
			js_release(method);
			if (!fine)
				return FALSE;
			if (value.type != JV_OBJECT) {
				*result = value;
				return TRUE;
			}
			js_release(value);
		} else
			js_release(method);
	}
	return js_throw_error(en, L"TypeError", JS_E_NO_PROPERTY, NULL);
}

BOOL js_to_number(engine *en, js_value v, double *result)
{
	switch (v.type) {
	case JV_UNDEFINED:
		*result = js_nan();
		return TRUE;
	case JV_NULL:
		*result = 0;
		return TRUE;
	case JV_BOOL:
		*result = v.boolean ? 1 : 0;
		return TRUE;
	case JV_NUMBER:
	case JV_DATE:
		*result = v.number;
		return TRUE;
	case JV_STRING:
		*result = js_string_number(v.string);
		return TRUE;
	case JV_OBJECT: {
		js_value primitive;
		if (!js_to_primitive(en, v, HINT_NUMBER, &primitive))
			return FALSE;
		BOOL fine = js_to_number(en, primitive, result);
		js_release(primitive);
		return fine;
	}
	}
	*result = js_nan();
	return TRUE;
}

BOOL js_to_string(engine *en, js_value v, js_string **result)
{
	switch (v.type) {
	case JV_UNDEFINED:
		*result = js_string_of(L"undefined");
		return TRUE;
	case JV_NULL:
		*result = js_string_of(L"null");
		return TRUE;
	case JV_BOOL:
		*result = js_string_of(v.boolean ? L"true" : L"false");
		return TRUE;
	case JV_NUMBER:
		*result = js_number_string(v.number);
		return TRUE;
	case JV_STRING:
		*result = js_string_retain(v.string);
		return TRUE;
	case JV_DATE:
		*result = js_date_string(v.number, 0);
		return TRUE;
	case JV_OBJECT: {
		js_value primitive;
		if (!js_to_primitive(en, v, HINT_STRING, &primitive))
			return FALSE;
		BOOL fine = js_to_string(en, primitive, result);
		js_release(primitive);
		return fine;
	}
	}
	*result = js_string_of(L"");
	return TRUE;
}

BOOL js_to_object(engine *en, js_value v, js_object **result)
{
	js_object *o;
	switch (v.type) {
	case JV_UNDEFINED:
	case JV_NULL:
		return js_throw_error(en, L"TypeError", JS_E_OBJECT_EXPECTED, NULL);
	case JV_OBJECT:
		*result = js_object_retain(v.object);
		return TRUE;
	case JV_BOOL:
		o = js_object_new(en, JC_BOOLEAN, en->boolean_prototype);
		break;
	case JV_NUMBER:
		o = js_object_new(en, JC_NUMBER, en->number_prototype);
		break;
	case JV_STRING:
		o = js_object_new(en, JC_STRING, en->string_prototype);
		break;
	case JV_DATE:
		o = js_object_new(en, JC_DATE, en->date_prototype);
		o->primitive = js_number(v.number);
		*result = o;
		return TRUE;
	default:
		return js_throw_error(en, L"TypeError", JS_E_OBJECT_EXPECTED, NULL);
	}
	o->primitive = js_retain(v);
	*result = o;
	return TRUE;
}

BOOL js_to_integer(engine *en, js_value v, double *result)
{
	double n;
	if (!js_to_number(en, v, &n))
		return FALSE;
	*result = js_integer(n);
	return TRUE;
}

BOOL js_to_int32(engine *en, js_value v, INT32 *result)
{
	double n;
	if (!js_to_number(en, v, &n))
		return FALSE;
	*result = js_int32(n);
	return TRUE;
}

BOOL js_to_uint32(engine *en, js_value v, UINT32 *result)
{
	double n;
	if (!js_to_number(en, v, &n))
		return FALSE;
	*result = js_uint32(n);
	return TRUE;
}

const WCHAR *js_typeof(js_value v)
{
	switch (v.type) {
	case JV_UNDEFINED:
		return L"undefined";
	case JV_NULL:
		return L"object";
	case JV_BOOL:
		return L"boolean";
	case JV_NUMBER:
		return L"number";
	case JV_STRING:
		return L"string";
	case JV_DATE:
		return L"date";
	case JV_OBJECT:
		if (v.object->cls == JC_FUNCTION)
			return L"function";
		if (v.object->cls == JC_VBARRAY && !v.object->proto)
			return L"unknown";
		return L"object";
	}
	return L"undefined";
}

/* Equality. */

static BOOL same_object(js_value a, js_value b)
{
	if (a.object == b.object)
		return TRUE;
	/* Two wrappers of one host object are one object. */
	if (a.object->cls == JC_DISPATCH && b.object->cls == JC_DISPATCH) {
		IUnknown *x = NULL, *y = NULL;
		IDispatch_QueryInterface(a.object->dispatch, &IID_IUnknown, (void **)&x);
		IDispatch_QueryInterface(b.object->dispatch, &IID_IUnknown, (void **)&y);
		if (x)
			IUnknown_Release(x);
		if (y)
			IUnknown_Release(y);
		return x && x == y;
	}
	return FALSE;
}

BOOL js_strict_equal(js_value a, js_value b)
{
	if (a.type != b.type)
		return FALSE;
	switch (a.type) {
	case JV_UNDEFINED:
	case JV_NULL:
		return TRUE;
	case JV_BOOL:
		return a.boolean == b.boolean;
	case JV_NUMBER:
	case JV_DATE:
		return a.number == b.number;
	case JV_STRING:
		return js_string_equal(a.string, b.string);
	case JV_OBJECT:
		return same_object(a, b);
	}
	return FALSE;
}

BOOL js_loose_equal(engine *en, js_value a, js_value b, BOOL *result)
{
	if (a.type == JV_DATE)
		a.type = JV_NUMBER;
	if (b.type == JV_DATE)
		b.type = JV_NUMBER;
	if (a.type == b.type) {
		*result = js_strict_equal(a, b);
		return TRUE;
	}
	if ((a.type == JV_UNDEFINED || a.type == JV_NULL) && (b.type == JV_UNDEFINED || b.type == JV_NULL)) {
		*result = TRUE;
		return TRUE;
	}
	if (a.type == JV_UNDEFINED || a.type == JV_NULL || b.type == JV_UNDEFINED || b.type == JV_NULL) {
		*result = FALSE;
		return TRUE;
	}
	if ((a.type == JV_NUMBER && b.type == JV_STRING) || (a.type == JV_STRING && b.type == JV_NUMBER)) {
		double x = a.type == JV_NUMBER ? a.number : js_string_number(a.string);
		double y = b.type == JV_NUMBER ? b.number : js_string_number(b.string);
		*result = x == y;
		return TRUE;
	}
	if (a.type == JV_BOOL)
		return js_loose_equal(en, js_number(a.boolean ? 1 : 0), b, result);
	if (b.type == JV_BOOL)
		return js_loose_equal(en, a, js_number(b.boolean ? 1 : 0), result);
	/* One object, one string or number: the object's primitive value. */
	js_value primitive;
	if (a.type == JV_OBJECT) {
		if (!js_to_primitive(en, a, HINT_NONE, &primitive))
			return FALSE;
		BOOL fine = js_loose_equal(en, primitive, b, result);
		js_release(primitive);
		return fine;
	}
	if (!js_to_primitive(en, b, HINT_NONE, &primitive))
		return FALSE;
	BOOL fine = js_loose_equal(en, a, primitive, result);
	js_release(primitive);
	return fine;
}

/* a < b, with undefined for a comparison involving NaN. */
static BOOL less_than(engine *en, js_value a, js_value b, int *result)
{
	js_value x, y;
	if (!js_to_primitive(en, a, HINT_NUMBER, &x))
		return FALSE;
	if (!js_to_primitive(en, b, HINT_NUMBER, &y)) {
		js_release(x);
		return FALSE;
	}
	if (x.type == JV_STRING && y.type == JV_STRING) {
		*result = js_string_compare(x.string, y.string) < 0;
		js_release(x);
		js_release(y);
		return TRUE;
	}
	double nx, ny;
	BOOL fine = js_to_number(en, x, &nx) && js_to_number(en, y, &ny);
	js_release(x);
	js_release(y);
	if (!fine)
		return FALSE;
	*result = js_is_nan(nx) || js_is_nan(ny) ? -1 : nx < ny;
	return TRUE;
}

/* Operators. */

static BOOL instance_of(engine *en, js_value v, js_value fn, BOOL *result)
{
	if (fn.type != JV_OBJECT || fn.object->cls != JC_FUNCTION)
		return js_throw_error(en, L"TypeError", JS_E_FUNCTION_EXPECTED, NULL);
	*result = FALSE;
	if (v.type != JV_OBJECT)
		return TRUE;
	js_object *target = fn.object;
	while (target->bound)
		target = target->bound;
	js_value proto;
	if (!js_get_text(en, target, L"prototype", &proto))
		return FALSE;
	if (proto.type != JV_OBJECT) {
		js_release(proto);
		return js_throw_error(en, L"TypeError", JS_E_OBJECT_EXPECTED, NULL);
	}
	for (js_object *k = v.object->proto; k; k = k->proto)
		if (k == proto.object) {
			*result = TRUE;
			break;
		}
	js_release(proto);
	return TRUE;
}

static BOOL binary_op(engine *en, js_op op, js_value a, js_value b, js_value *result)
{
	double x, y;
	INT32 i, j;
	UINT32 u, shift;
	int less;
	BOOL equal;
	switch (op) {
	case OP_ADD: {
		js_value pa, pb;
		if (!js_to_primitive(en, a, HINT_NONE, &pa))
			return FALSE;
		if (!js_to_primitive(en, b, HINT_NONE, &pb)) {
			js_release(pa);
			return FALSE;
		}
		BOOL fine;
		if (pa.type == JV_STRING || pb.type == JV_STRING) {
			js_string *sa = NULL, *sb = NULL;
			fine = js_to_string(en, pa, &sa) && js_to_string(en, pb, &sb);
			if (fine)
				*result = js_str(js_string_concat(sa, sb));
			js_string_release(sa);
			js_string_release(sb);
		} else {
			fine = js_to_number(en, pa, &x) && js_to_number(en, pb, &y);
			if (fine)
				*result = js_number(x + y);
		}
		js_release(pa);
		js_release(pb);
		return fine;
	}
	case OP_SUB:
	case OP_MUL:
	case OP_DIV:
	case OP_MOD:
		if (!js_to_number(en, a, &x) || !js_to_number(en, b, &y))
			return FALSE;
		*result = js_number(op == OP_SUB ? x - y : op == OP_MUL ? x * y : op == OP_DIV ? x / y : fmod(x, y));
		return TRUE;
	case OP_SHL:
	case OP_SAR:
	case OP_SHR:
		if (!js_to_int32(en, a, &i) || !js_to_uint32(en, b, &shift))
			return FALSE;
		shift &= 31;
		if (op == OP_SHL)
			*result = js_number((INT32)((UINT32)i << shift));
		else if (op == OP_SAR)
			*result = js_number(i >> shift);
		else
			*result = js_number((UINT32)i >> shift);
		return TRUE;
	case OP_BITAND:
	case OP_BITOR:
	case OP_BITXOR:
		if (!js_to_int32(en, a, &i) || !js_to_int32(en, b, &j))
			return FALSE;
		*result = js_number(op == OP_BITAND ? (i & j) : op == OP_BITOR ? (i | j) : (i ^ j));
		return TRUE;
	case OP_LT:
		if (!less_than(en, a, b, &less))
			return FALSE;
		*result = js_bool(less == 1);
		return TRUE;
	case OP_GT:
		if (!less_than(en, b, a, &less))
			return FALSE;
		*result = js_bool(less == 1);
		return TRUE;
	case OP_LE:
		if (!less_than(en, b, a, &less))
			return FALSE;
		*result = js_bool(less == 0);
		return TRUE;
	case OP_GE:
		if (!less_than(en, a, b, &less))
			return FALSE;
		*result = js_bool(less == 0);
		return TRUE;
	case OP_EQ:
	case OP_NE:
		if (!js_loose_equal(en, a, b, &equal))
			return FALSE;
		*result = js_bool(op == OP_EQ ? equal : !equal);
		return TRUE;
	case OP_SEQ:
		*result = js_bool(js_strict_equal(a, b));
		return TRUE;
	case OP_SNE:
		*result = js_bool(!js_strict_equal(a, b));
		return TRUE;
	case OP_INSTANCEOF:
		if (!instance_of(en, a, b, &equal))
			return FALSE;
		*result = js_bool(equal);
		return TRUE;
	case OP_IN: {
		if (b.type != JV_OBJECT)
			return js_throw_error(en, L"TypeError", JS_E_OBJECT_EXPECTED, NULL);
		js_string *name;
		if (!js_to_string(en, a, &name))
			return FALSE;
		*result = js_bool(js_has(en, b.object, name));
		js_string_release(name);
		return TRUE;
	}
	default:
		break;
	}
	(void)u;
	return js_throw_error(en, L"Error", JS_E_SYNTAX, NULL);
}

/* References: a base object and a name, or a name nothing in the scope chain has; or a call of a member of the host's objects with
 * its arguments, which can be assigned to. */
typedef struct {
	js_object *base;
	js_string *name;
	BOOL unresolved, call;
	UINT argc;
	js_value *argv;
} reference;

static BOOL eval_arguments(engine *en, context *ctx, node *n, js_value **argv);
static void free_arguments(UINT count, js_value *argv);

static void reference_free(reference *r)
{
	js_object_release(r->base);
	js_string_release(r->name);
	if (r->call)
		free_arguments(r->argc, r->argv);
}

/* The global object, then the host's items: an item by its name, the members of the items that give their members globally. */
static BOOL global_has(engine *en, js_string *name)
{
	if (js_has(en, en->global, name))
		return TRUE;
	for (UINT i = 0; i < en->item_count; i++)
		if (js_string_equal(en->items[i].key, name) && js_item_object(en, i))
			return TRUE;
	return FALSE;
}

static BOOL global_member_item(engine *en, js_string *name, IDispatch **dispatch, DISPID *id)
{
	for (UINT i = 0; i < en->item_count; i++) {
		if (!(en->items[i].flags & SCRIPTITEM_GLOBALMEMBERS))
			continue;
		IDispatch *item = js_item_object(en, i);
		LPOLESTR names = name->text;
		if (item && SUCCEEDED(IDispatch_GetIDsOfNames(item, &IID_NULL, &names, 1, LOCALE_USER_DEFAULT, id))) {
			*dispatch = item;
			return TRUE;
		}
	}
	return FALSE;
}

static BOOL resolve(engine *en, context *ctx, js_string *name, reference *r)
{
	ZeroMemory(r, sizeof(*r));
	r->name = js_string_retain(name);
	for (js_scope *s = ctx->scope; s; s = s->outer) {
		if (s->object == en->global ? global_has(en, name) : js_has(en, s->object, name)) {
			r->base = js_object_retain(s->object);
			return TRUE;
		}
		if (en->throwing)
			return FALSE;
	}
	r->base = NULL;
	r->unresolved = TRUE;
	return TRUE;
}

static BOOL get_global(engine *en, js_string *name, js_value *result)
{
	if (js_has(en, en->global, name))
		return js_get(en, en->global, name, result);
	for (UINT i = 0; i < en->item_count; i++)
		if (js_string_equal(en->items[i].key, name)) {
			IDispatch *item = js_item_object(en, i);
			if (item) {
				*result = js_obj(js_dispatch_object(en, item));
				return TRUE;
			}
		}
	*result = js_undefined;
	return TRUE;
}

static BOOL get_value(engine *en, reference *r, js_value *result)
{
	if (r->call) {
		if (!r->name)
			return js_dispatch_call(en, r->base->dispatch, DISPID_VALUE, DISPATCH_METHOD | DISPATCH_PROPERTYGET, r->argc, r->argv, result);
		return js_dispatch_invoke_name(en, r->base, r->name, r->argc, r->argv, result);
	}
	if (r->unresolved) {
		IDispatch *item;
		DISPID id;
		if (global_member_item(en, r->name, &item, &id))
			return js_dispatch_call(en, item, id, DISPATCH_PROPERTYGET | DISPATCH_METHOD, 0, NULL, result);
		return js_throw_error(en, L"ReferenceError", JS_E_UNDEFINED, r->name->text);
	}
	if (r->base == en->global)
		return get_global(en, r->name, result);
	return js_get(en, r->base, r->name, result);
}

static BOOL put_value(engine *en, reference *r, js_value value)
{
	if (r->call)
		return js_dispatch_put_at(en, r->base, r->name, r->argc, r->argv, value);
	if (r->unresolved) {
		IDispatch *item;
		DISPID id;
		if (global_member_item(en, r->name, &item, &id)) {
			VARIANT arg;
			HRESULT hr = js_to_variant(en, value, &arg);
			if (FAILED(hr))
				return js_throw_hresult(en, hr, NULL);
			DISPID put = DISPID_PROPERTYPUT;
			DISPPARAMS params = { &arg, &put, 1, 1 };
			EXCEPINFO info = { 0 };
			hr = IDispatch_Invoke(item, id, &IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT, &params, NULL, &info, NULL);
			VariantClear(&arg);
			BOOL fine = SUCCEEDED(hr) || js_throw_hresult(en, hr, &info);
			SysFreeString(info.bstrSource);
			SysFreeString(info.bstrDescription);
			SysFreeString(info.bstrHelpFile);
			return fine;
		}
		/* Assigning a name nothing declared makes it a property of the global object. */
		return js_put(en, en->global, r->name, value);
	}
	return js_put(en, r->base, r->name, value);
}

/* The interpreter. */

static BOOL eval_node(engine *en, context *ctx, node *n, js_value *result);
static completion exec_node(engine *en, context *ctx, node *n, js_value *value, js_string **label);
static completion exec_list(engine *en, context *ctx, node **list, UINT count, js_value *value, js_string **label);

static void at(engine *en, node *n)
{
	en->line = n->line;
	en->column = n->column;
}

/* The reference an expression names; FALSE when it threw or names nothing assignable. */
static BOOL eval_reference(engine *en, context *ctx, node *n, reference *r)
{
	ZeroMemory(r, sizeof(*r));
	if (n->kind == N_NAME)
		return resolve(en, ctx, n->name, r);
	if (n->kind == N_MEMBER || n->kind == N_INDEX) {
		js_value base, key = js_undefined;
		if (!eval_node(en, ctx, n->a, &base))
			return FALSE;
		if (n->kind == N_INDEX && !eval_node(en, ctx, n->b, &key)) {
			js_release(base);
			return FALSE;
		}
		at(en, n);
		js_object *object;
		BOOL fine = js_to_object(en, base, &object);
		js_release(base);
		if (!fine) {
			js_release(key);
			return FALSE;
		}
		js_string *name;
		if (n->kind == N_MEMBER)
			name = js_string_retain(n->name);
		else if (!js_to_string(en, key, &name)) {
			js_release(key);
			js_object_release(object);
			return FALSE;
		}
		js_release(key);
		r->base = object;
		r->name = name;
		return TRUE;
	}
	if (n->kind == N_CALL) {
		/* A call of the host's object, or of one of its members, names the property with those indexes. */
		reference callee;
		js_value target = js_undefined;
		BOOL named = n->a->kind == N_MEMBER || n->a->kind == N_INDEX;
		if (named ? !eval_reference(en, ctx, n->a, &callee) : !eval_node(en, ctx, n->a, &target))
			return FALSE;
		js_object *host = named ? callee.base : target.type == JV_OBJECT ? target.object : NULL;
		if (host && host->cls == JC_DISPATCH) {
			js_value *argv;
			if (!eval_arguments(en, ctx, n, &argv)) {
				if (named)
					reference_free(&callee);
				js_release(target);
				return FALSE;
			}
			r->base = js_object_retain(host);
			r->name = named ? js_string_retain(callee.name) : NULL;
			r->call = TRUE;
			r->argc = n->count;
			r->argv = argv;
			if (named)
				reference_free(&callee);
			js_release(target);
			return TRUE;
		}
		if (named)
			reference_free(&callee);
		js_release(target);
		at(en, n);
		return js_throw_error(en, L"ReferenceError", JS_E_ASSIGN_RESULT, NULL);
	}
	js_value ignored;
	if (!eval_node(en, ctx, n, &ignored))
		return FALSE;
	js_release(ignored);
	at(en, n);
	return js_throw_error(en, L"ReferenceError", JS_E_ASSIGN_RESULT, NULL);
}

static BOOL eval_arguments(engine *en, context *ctx, node *n, js_value **argv)
{
	*argv = n->count ? js_alloc(n->count * sizeof(js_value)) : NULL;
	for (UINT i = 0; i < n->count; i++)
		if (!eval_node(en, ctx, n->list[i], &(*argv)[i])) {
			for (UINT j = 0; j < i; j++)
				js_release((*argv)[j]);
			js_free(*argv);
			*argv = NULL;
			return FALSE;
		}
	return TRUE;
}

static void free_arguments(UINT argc, js_value *argv)
{
	for (UINT i = 0; i < argc; i++)
		js_release(argv[i]);
	js_free(argv);
}

static BOOL eval_call(engine *en, context *ctx, node *n, js_value *result)
{
	js_value fn, self = js_undefined;
	js_object *host_base = NULL;
	js_string *host_name = NULL;
	if (n->a->kind == N_MEMBER || n->a->kind == N_INDEX) {
		reference r;
		if (!eval_reference(en, ctx, n->a, &r))
			return FALSE;
		if (r.base->cls == JC_DISPATCH) {
			/* A method of the host's object is called by its name, in one Invoke. */
			host_base = r.base;
			host_name = r.name;
		} else {
			BOOL fine = js_get(en, r.base, r.name, &fn);
			if (!fine) {
				reference_free(&r);
				return FALSE;
			}
			self = js_obj(js_object_retain(r.base));
			reference_free(&r);
		}
	} else if (n->a->kind == N_NAME) {
		reference r;
		if (!resolve(en, ctx, n->a->name, &r))
			return FALSE;
		at(en, n->a);
		BOOL fine = get_value(en, &r, &fn);
		/* this is the object of a with statement the name was found in, and the global object otherwise. */
		if (fine && r.base && r.base->cls != JC_ACTIVATION && r.base != en->global)
			self = js_obj(js_object_retain(r.base));
		reference_free(&r);
		if (!fine)
			return FALSE;
	} else if (!eval_node(en, ctx, n->a, &fn))
		return FALSE;
	js_value *argv;
	if (!eval_arguments(en, ctx, n, &argv)) {
		if (host_base) {
			js_object_release(host_base);
			js_string_release(host_name);
		} else {
			js_release(fn);
			js_release(self);
		}
		return FALSE;
	}
	at(en, n);
	BOOL fine;
	if (host_base) {
		fine = js_dispatch_invoke_name(en, host_base, host_name, n->count, argv, result);
		js_object_release(host_base);
		js_string_release(host_name);
	} else {
		if (!js_is_callable(fn))
			fine = js_throw_error(en, L"TypeError", JS_E_FUNCTION_EXPECTED, NULL);
		else if (fn.object->cls == JC_FUNCTION && fn.object->native && n->a->kind == N_NAME && js_string_is(n->a->name, L"eval") &&
			fn.object->native_data == (const void *)en) {
			/* A direct call of eval runs in the caller's context. */
			js_value code = js_arg(n->count, argv, 0);
			if (code.type != JV_STRING)
				*result = js_retain(code), fine = TRUE;
			else
				fine = js_eval_text(en, code.string, ctx, result);
		} else
			fine = js_call(en, fn, self, n->count, argv, result);
		js_release(fn);
		js_release(self);
	}
	free_arguments(n->count, argv);
	return fine;
}

static BOOL eval_node(engine *en, context *ctx, node *n, js_value *result)
{
	js_value a, b;
	reference r;
	BOOL fine;
	*result = js_undefined;
	switch (n->kind) {
	case N_NUMBER:
		*result = js_number(n->number);
		return TRUE;
	case N_STRING:
		*result = js_str(js_string_retain(n->name));
		return TRUE;
	case N_REGEXP: {
		BOOL failed;
		at(en, n);
		js_object *re = js_regexp_new(en, n->name, n->flags, &failed);
		if (!re)
			return FALSE;
		*result = js_obj(re);
		return TRUE;
	}
	case N_NAME:
		if (!resolve(en, ctx, n->name, &r))
			return FALSE;
		at(en, n);
		fine = get_value(en, &r, result);
		reference_free(&r);
		return fine;
	case N_THIS:
		*result = js_retain(ctx->self);
		return TRUE;
	case N_NULL:
		*result = js_null;
		return TRUE;
	case N_TRUE:
	case N_FALSE:
		*result = js_bool(n->kind == N_TRUE);
		return TRUE;
	case N_ARRAY: {
		js_object *array = js_array_new(en, 0);
		for (UINT i = 0; i < n->count; i++) {
			if (!n->list[i])
				continue;
			if (!eval_node(en, ctx, n->list[i], &a)) {
				js_object_release(array);
				return FALSE;
			}
			js_put_index(en, array, i, a);
			js_release(a);
		}
		array->length = n->count;
		*result = js_obj(array);
		return TRUE;
	}
	case N_OBJECT: {
		js_object *object = js_object_new(en, JC_OBJECT, en->object_prototype);
		for (UINT i = 0; i < n->count; i++) {
			if (!eval_node(en, ctx, n->list[i], &a)) {
				js_object_release(object);
				return FALSE;
			}
			js_put(en, object, n->names[i], a);
			js_release(a);
		}
		*result = js_obj(object);
		return TRUE;
	}
	case N_FUNCTION: {
		/* A named function expression sees its own name. */
		js_scope *scope = ctx->scope;
		js_object *own = NULL;
		if (n->function->name) {
			own = js_object_new(en, JC_ACTIVATION, NULL);
			scope = js_scope_new(own, ctx->scope);
		}
		js_object *f = js_function_new(en, n->function, scope);
		if (own) {
			js_define(own, n->function->name, js_obj(js_object_retain(f)), JP_DONTDELETE | JP_READONLY);
			js_scope_release(scope);
			js_object_release(own);
		}
		*result = js_obj(f);
		return TRUE;
	}
	case N_MEMBER:
	case N_INDEX:
		if (!eval_reference(en, ctx, n, &r))
			return FALSE;
		at(en, n);
		fine = js_get(en, r.base, r.name, result);
		reference_free(&r);
		return fine;
	case N_CALL:
		return eval_call(en, ctx, n, result);
	case N_NEW: {
		js_value *argv;
		if (!eval_node(en, ctx, n->a, &a))
			return FALSE;
		if (!eval_arguments(en, ctx, n, &argv)) {
			js_release(a);
			return FALSE;
		}
		at(en, n);
		fine = js_construct(en, a, n->count, argv, result);
		free_arguments(n->count, argv);
		js_release(a);
		return fine;
	}
	case N_POSTFIX:
	case N_PREFIX: {
		double number;
		if (!eval_reference(en, ctx, n->a, &r))
			return FALSE;
		at(en, n);
		fine = get_value(en, &r, &a) && js_to_number(en, a, &number);
		if (fine) {
			js_release(a);
			double changed = n->op == OP_INC ? number + 1 : number - 1;
			fine = put_value(en, &r, js_number(changed));
			*result = js_number(n->kind == N_POSTFIX ? number : changed);
		}
		reference_free(&r);
		return fine;
	}
	case N_UNARY:
		if (n->op == OP_TYPEOF && n->a->kind == N_NAME) {
			/* typeof of a name nothing declared is "undefined", not an error. */
			if (!resolve(en, ctx, n->a->name, &r))
				return FALSE;
			if (r.unresolved) {
				IDispatch *item;
				DISPID id;
				BOOL member = global_member_item(en, r.name, &item, &id);
				reference_free(&r);
				*result = js_text(member ? L"object" : L"undefined");
				return TRUE;
			}
			fine = get_value(en, &r, &a);
			reference_free(&r);
			if (!fine)
				return FALSE;
			*result = js_text(js_typeof(a));
			js_release(a);
			return TRUE;
		}
		if (n->op == OP_DELETE) {
			if (n->a->kind != N_NAME && n->a->kind != N_MEMBER && n->a->kind != N_INDEX) {
				if (!eval_node(en, ctx, n->a, &a))
					return FALSE;
				js_release(a);
				*result = js_bool(TRUE);
				return TRUE;
			}
			if (!eval_reference(en, ctx, n->a, &r))
				return FALSE;
			BOOL deleted = TRUE;
			fine = r.unresolved || js_delete(en, r.base, r.name, &deleted);
			reference_free(&r);
			*result = js_bool(deleted);
			return fine;
		}
		if (!eval_node(en, ctx, n->a, &a))
			return FALSE;
		at(en, n);
		fine = TRUE;
		switch (n->op) {
		case OP_NOT:
			*result = js_bool(!js_to_boolean(a));
			break;
		case OP_TYPEOF:
			*result = js_text(js_typeof(a));
			break;
		case OP_VOID:
			break;
		case OP_BITNOT: {
			INT32 i;
			if ((fine = js_to_int32(en, a, &i)))
				*result = js_number(~i);
			break;
		}
		default: {
			double number;
			if ((fine = js_to_number(en, a, &number)))
				*result = js_number(n->op == OP_NEG ? -number : number);
			break;
		}
		}
		js_release(a);
		return fine;
	case N_BINARY:
		if (!eval_node(en, ctx, n->a, &a))
			return FALSE;
		if (!eval_node(en, ctx, n->b, &b)) {
			js_release(a);
			return FALSE;
		}
		at(en, n);
		fine = binary_op(en, n->op, a, b, result);
		js_release(a);
		js_release(b);
		return fine;
	case N_LOGICAL:
		if (!eval_node(en, ctx, n->a, &a))
			return FALSE;
		if (js_to_boolean(a) == (n->op == OP_OR)) {
			*result = a;
			return TRUE;
		}
		js_release(a);
		return eval_node(en, ctx, n->b, result);
	case N_CONDITIONAL:
		if (!eval_node(en, ctx, n->a, &a))
			return FALSE;
		fine = js_to_boolean(a);
		js_release(a);
		return eval_node(en, ctx, fine ? n->b : n->c, result);
	case N_ASSIGN:
		if (!eval_reference(en, ctx, n->a, &r))
			return FALSE;
		if (n->op != OP_NONE) {
			at(en, n);
			if (!get_value(en, &r, &a)) {
				reference_free(&r);
				return FALSE;
			}
			if (!eval_node(en, ctx, n->b, &b)) {
				js_release(a);
				reference_free(&r);
				return FALSE;
			}
			at(en, n);
			fine = binary_op(en, n->op, a, b, result);
			js_release(a);
			js_release(b);
		} else
			fine = eval_node(en, ctx, n->b, result);
		if (fine) {
			at(en, n);
			fine = put_value(en, &r, *result);
		}
		reference_free(&r);
		return fine;
	case N_COMMA:
		if (!eval_node(en, ctx, n->a, &a))
			return FALSE;
		js_release(a);
		return eval_node(en, ctx, n->b, result);
	default:
		break;
	}
	return js_throw_error(en, L"SyntaxError", JS_E_SYNTAX, NULL);
}

/* Statements. */

static BOOL label_matches(js_string *wanted, js_string *own)
{
	return !wanted || (own && js_string_equal(wanted, own));
}

/* What a loop does with the completion of its body: go on, or end with it. */
static BOOL loop_continues(completion c, js_string **label, js_string *own_label, completion *out)
{
	if (c == C_BREAK && label_matches(*label, own_label)) {
		js_string_release(*label);
		*label = NULL;
		*out = C_NORMAL;
		return FALSE;
	}
	if (c == C_CONTINUE && label_matches(*label, own_label)) {
		js_string_release(*label);
		*label = NULL;
		return TRUE;
	}
	if (c != C_NORMAL) {
		*out = c;
		return FALSE;
	}
	return TRUE;
}

static completion exec_loop(engine *en, context *ctx, node *n, js_value *value, js_string **label, js_string *own_label)
{
	js_value v;
	completion out = C_NORMAL, c;
	switch (n->kind) {
	case N_DO:
		for (;;) {
			c = exec_node(en, ctx, n->a, value, label);
			if (!loop_continues(c, label, own_label, &out))
				return out;
			if (!eval_node(en, ctx, n->b, &v))
				return C_THROW;
			BOOL again = js_to_boolean(v);
			js_release(v);
			if (!again)
				return C_NORMAL;
		}
	case N_WHILE:
		for (;;) {
			if (!eval_node(en, ctx, n->a, &v))
				return C_THROW;
			BOOL again = js_to_boolean(v);
			js_release(v);
			if (!again)
				return C_NORMAL;
			c = exec_node(en, ctx, n->b, value, label);
			if (!loop_continues(c, label, own_label, &out))
				return out;
		}
	case N_FOR:
		if (n->a) {
			if (n->a->kind == N_VAR) {
				c = exec_node(en, ctx, n->a, value, label);
				if (c != C_NORMAL)
					return c;
			} else {
				if (!eval_node(en, ctx, n->a, &v))
					return C_THROW;
				js_release(v);
			}
		}
		for (;;) {
			if (n->b) {
				if (!eval_node(en, ctx, n->b, &v))
					return C_THROW;
				BOOL again = js_to_boolean(v);
				js_release(v);
				if (!again)
					return C_NORMAL;
			}
			c = exec_node(en, ctx, n->d, value, label);
			if (!loop_continues(c, label, own_label, &out))
				return out;
			if (n->c) {
				if (!eval_node(en, ctx, n->c, &v))
					return C_THROW;
				js_release(v);
			}
		}
	case N_FOR_IN: {
		node *target = n->a;
		if (target->kind == N_VAR && target->list[0]) {
			/* for (var x = 1 in o) sets x first. */
			c = exec_node(en, ctx, target, value, label);
			if (c != C_NORMAL)
				return c;
		}
		js_value collection;
		if (!eval_node(en, ctx, n->b, &collection))
			return C_THROW;
		if (collection.type == JV_UNDEFINED || collection.type == JV_NULL)
			return C_NORMAL;
		js_object *object;
		BOOL fine = js_to_object(en, collection, &object);
		js_release(collection);
		if (!fine)
			return C_THROW;
		UINT count;
		js_string **names = js_enumerate(en, object, &count);
		out = C_NORMAL;
		for (UINT i = 0; i < count; i++) {
			/* A property deleted while the loop runs is not visited. */
			if (!js_has(en, object, names[i]))
				continue;
			reference r;
			if (target->kind == N_VAR) {
				fine = resolve(en, ctx, target->names[0], &r);
			} else
				fine = eval_reference(en, ctx, target, &r);
			if (fine) {
				fine = put_value(en, &r, js_str(names[i]));
				reference_free(&r);
			}
			if (!fine) {
				out = C_THROW;
				break;
			}
			c = exec_node(en, ctx, n->d, value, label);
			if (!loop_continues(c, label, own_label, &out))
				break;
		}
		for (UINT i = 0; i < count; i++)
			js_string_release(names[i]);
		js_free(names);
		js_object_release(object);
		return out;
	}
	default:
		return C_NORMAL;
	}
}

static completion exec_switch(engine *en, context *ctx, node *n, js_value *value, js_string **label)
{
	js_value subject, test;
	if (!eval_node(en, ctx, n->a, &subject))
		return C_THROW;
	int start = -1;
	for (UINT i = 0; i < n->count && start < 0; i++) {
		node *c = n->list[i];
		if (c->flags)
			continue;
		if (!eval_node(en, ctx, c->a, &test)) {
			js_release(subject);
			return C_THROW;
		}
		if (js_strict_equal(subject, test))
			start = (int)i;
		js_release(test);
	}
	js_release(subject);
	if (start < 0)
		for (UINT i = 0; i < n->count; i++)
			if (n->list[i]->flags)
				start = (int)i;
	if (start < 0)
		return C_NORMAL;
	for (UINT i = (UINT)start; i < n->count; i++) {
		completion c = exec_list(en, ctx, n->list[i]->list, n->list[i]->count, value, label);
		if (c == C_BREAK && !*label)
			return C_NORMAL;
		if (c != C_NORMAL)
			return c;
	}
	return C_NORMAL;
}

static completion exec_try(engine *en, context *ctx, node *n, js_value *value, js_string **label)
{
	completion c = exec_node(en, ctx, n->a, value, label);
	if (c == C_THROW && n->b) {
		js_value thrown = en->exception;
		en->exception = js_undefined;
		en->throwing = FALSE;
		/* The catch block sees its variable in a scope of its own. */
		js_object *catch_object = js_object_new(en, JC_ACTIVATION, NULL);
		js_define(catch_object, n->name, thrown, JP_DONTDELETE);
		js_scope *outer = ctx->scope;
		ctx->scope = js_scope_new(catch_object, outer);
		js_object_release(catch_object);
		c = exec_node(en, ctx, n->b, value, label);
		js_scope_release(ctx->scope);
		ctx->scope = outer;
	}
	if (n->c) {
		/* finally runs whatever happened, and what it ends with wins only if it does not end normally. */
		BOOL throwing = en->throwing;
		js_value thrown = en->exception;
		js_value kept_value = *value;
		js_string *kept_label = *label;
		en->throwing = FALSE;
		en->exception = js_undefined;
		*value = js_undefined;
		*label = NULL;
		completion f = exec_node(en, ctx, n->c, value, label);
		if (f != C_NORMAL) {
			if (throwing)
				js_release(thrown);
			js_release(kept_value);
			js_string_release(kept_label);
			return f;
		}
		js_release(*value);
		js_string_release(*label);
		*value = kept_value;
		*label = kept_label;
		en->throwing = throwing;
		en->exception = thrown;
	}
	return c;
}

static completion exec_node(engine *en, context *ctx, node *n, js_value *value, js_string **label)
{
	js_value v;
	switch (n->kind) {
	case N_BLOCK:
		return exec_list(en, ctx, n->list, n->count, value, label);
	case N_VAR:
		for (UINT i = 0; i < n->count; i++) {
			if (!n->list[i])
				continue;
			if (!eval_node(en, ctx, n->list[i], &v))
				return C_THROW;
			reference r;
			at(en, n);
			BOOL fine = resolve(en, ctx, n->names[i], &r) && put_value(en, &r, v);
			reference_free(&r);
			js_release(v);
			if (!fine)
				return C_THROW;
		}
		return C_NORMAL;
	case N_EMPTY:
	case N_FUNCTION_DECLARATION:
		return C_NORMAL;
	case N_EXPRESSION:
		at(en, n);
		if (!eval_node(en, ctx, n->a, &v))
			return C_THROW;
		js_release(*value);
		*value = v;
		return C_NORMAL;
	case N_IF: {
		at(en, n);
		if (!eval_node(en, ctx, n->a, &v))
			return C_THROW;
		BOOL taken = js_to_boolean(v);
		js_release(v);
		if (taken)
			return exec_node(en, ctx, n->b, value, label);
		return n->c ? exec_node(en, ctx, n->c, value, label) : C_NORMAL;
	}
	case N_DO:
	case N_WHILE:
	case N_FOR:
	case N_FOR_IN:
		at(en, n);
		return exec_loop(en, ctx, n, value, label, NULL);
	case N_CONTINUE:
	case N_BREAK:
		*label = js_string_retain(n->name);
		return n->kind == N_BREAK ? C_BREAK : C_CONTINUE;
	case N_RETURN:
		at(en, n);
		v = js_undefined;
		if (n->a && !eval_node(en, ctx, n->a, &v))
			return C_THROW;
		js_release(*value);
		*value = v;
		return C_RETURN;
	case N_WITH: {
		at(en, n);
		if (!eval_node(en, ctx, n->a, &v))
			return C_THROW;
		js_object *object;
		BOOL fine = js_to_object(en, v, &object);
		js_release(v);
		if (!fine)
			return C_THROW;
		js_scope *outer = ctx->scope;
		ctx->scope = js_scope_new(object, outer);
		js_object_release(object);
		completion c = exec_node(en, ctx, n->b, value, label);
		js_scope_release(ctx->scope);
		ctx->scope = outer;
		return c;
	}
	case N_SWITCH:
		at(en, n);
		return exec_switch(en, ctx, n, value, label);
	case N_LABEL: {
		node *inner = n->a;
		completion c;
		if (inner->kind == N_DO || inner->kind == N_WHILE || inner->kind == N_FOR || inner->kind == N_FOR_IN)
			c = exec_loop(en, ctx, inner, value, label, n->name);
		else
			c = exec_node(en, ctx, inner, value, label);
		if (c == C_BREAK && *label && js_string_equal(*label, n->name)) {
			js_string_release(*label);
			*label = NULL;
			return C_NORMAL;
		}
		return c;
	}
	case N_THROW:
		at(en, n);
		if (!eval_node(en, ctx, n->a, &v))
			return C_THROW;
		js_throw(en, v);
		js_release(v);
		return C_THROW;
	case N_TRY:
		return exec_try(en, ctx, n, value, label);
	default:
		at(en, n);
		return C_THROW;
	}
}

static completion exec_list(engine *en, context *ctx, node **list, UINT count, js_value *value, js_string **label)
{
	for (UINT i = 0; i < count; i++) {
		completion c = exec_node(en, ctx, list[i], value, label);
		if (c != C_NORMAL)
			return c;
	}
	return C_NORMAL;
}

/* Functions and programs. */

js_object *js_function_new(engine *en, function_info *code, js_scope *scope)
{
	js_object *f = js_object_new(en, JC_FUNCTION, en->function_prototype);
	f->code = code;
	f->scope = scope;
	if (scope)
		InterlockedIncrement(&scope->refs);
	/* Its prototype, for the objects new makes of it. */
	js_object *proto = js_object_new(en, JC_OBJECT, en->object_prototype);
	js_define_text(proto, L"constructor", js_obj(js_object_retain(f)), JP_DONTENUM);
	js_define_text(f, L"prototype", js_obj(proto), JP_DONTDELETE | JP_DONTENUM);
	return f;
}

void js_hoist(engine *en, function_info *code, context *ctx)
{
	for (UINT i = 0; i < code->function_count; i++) {
		node *declaration = code->functions[i];
		js_object *f = js_function_new(en, declaration->function, ctx->scope);
		js_put(en, ctx->variables, declaration->function->name, js_obj(f));
		js_object_release(f);
	}
	for (UINT i = 0; i < code->var_count; i++)
		if (!js_has_own(en, ctx->variables, code->vars[i]))
			js_define(ctx->variables, code->vars[i], js_undefined, JP_DONTDELETE);
}

BOOL js_run_program(engine *en, function_info *program, context *ctx, js_value *result)
{
	js_hoist(en, program, ctx);
	js_value value = js_undefined;
	js_string *label = NULL;
	context *outer = en->current;
	en->current = ctx;
	completion c = exec_list(en, ctx, program->body, program->body_count, &value, &label);
	en->current = outer;
	js_string_release(label);
	if (c == C_THROW) {
		js_release(value);
		return FALSE;
	}
	if (result)
		*result = value;
	else
		js_release(value);
	return TRUE;
}

/* The arguments object: the arguments by index, their count, and the function. */
static js_object *arguments_object(engine *en, js_object *callee, UINT argc, js_value *argv)
{
	js_object *args = js_object_new(en, JC_ARGUMENTS, en->object_prototype);
	for (UINT i = 0; i < argc; i++) {
		js_string *name = js_number_string(i);
		js_define(args, name, js_retain(argv[i]), JP_DONTENUM);
		js_string_release(name);
	}
	js_define_text(args, L"length", js_number(argc), JP_DONTENUM);
	js_define_text(args, L"callee", js_obj(js_object_retain(callee)), JP_DONTENUM);
	return args;
}

static BOOL call_script(engine *en, js_object *f, js_value self, UINT argc, js_value *argv, js_value *result)
{
	function_info *code = f->code;
	js_object *activation = js_object_new(en, JC_ACTIVATION, NULL);
	for (UINT i = 0; i < code->param_count; i++)
		js_define(activation, code->params[i], i < argc ? js_retain(argv[i]) : js_undefined, JP_DONTDELETE);
	if (code->uses_arguments || code->uses_eval) {
		js_string *name = js_string_of(L"arguments");
		if (!js_own(activation, name))
			js_define(activation, name, js_obj(arguments_object(en, f, argc, argv)), JP_DONTDELETE);
		js_string_release(name);
	}
	context ctx = { 0 };
	ctx.scope = js_scope_new(activation, f->scope);
	ctx.variables = activation;
	/* Without an object for this, it is the global object. */
	if (self.type == JV_UNDEFINED || self.type == JV_NULL)
		ctx.self = js_obj(js_object_retain(en->global));
	else if (self.type != JV_OBJECT) {
		js_object *wrapped;
		js_to_object(en, self, &wrapped);
		ctx.self = js_obj(wrapped);
	} else
		ctx.self = js_retain(self);
	ctx.outer = en->current;
	context *outer = en->current;
	en->current = &ctx;
	js_hoist(en, code, &ctx);
	js_value value = js_undefined;
	js_string *label = NULL;
	completion c = exec_list(en, &ctx, code->body, code->body_count, &value, &label);
	en->current = outer;
	js_string_release(label);
	js_scope_release(ctx.scope);
	js_release(ctx.self);
	js_object_release(activation);
	if (c == C_THROW) {
		js_release(value);
		return FALSE;
	}
	if (c == C_RETURN)
		*result = value;
	else {
		js_release(value);
		*result = js_undefined;
	}
	return TRUE;
}

BOOL js_call(engine *en, js_value fn, js_value self, UINT argc, js_value *argv, js_value *result)
{
	*result = js_undefined;
	if (!js_is_callable(fn))
		return js_throw_error(en, L"TypeError", JS_E_FUNCTION_EXPECTED, NULL);
	js_object *f = fn.object;
	if (f->cls == JC_DISPATCH)
		return js_dispatch_call(en, f->dispatch, DISPID_VALUE, DISPATCH_METHOD, argc, argv, result);
	if (en->depth >= MAX_DEPTH)
		return js_throw_error(en, L"Error", JS_E_OUT_OF_STACK, NULL);
	en->depth++;
	UINT line = en->line, column = en->column;
	BOOL fine = f->native ? f->native(en, self, argc, argv, result, f) : call_script(en, f, self, argc, argv, result);
	en->depth--;
	if (fine) {
		en->line = line;
		en->column = column;
	}
	return fine;
}

BOOL js_construct(engine *en, js_value fn, UINT argc, js_value *argv, js_value *result)
{
	*result = js_undefined;
	if (fn.type != JV_OBJECT || (fn.object->cls != JC_FUNCTION && fn.object->cls != JC_DISPATCH))
		return js_throw_error(en, L"TypeError", JS_E_OBJECT_EXPECTED, NULL);
	js_object *f = fn.object;
	if (f->cls == JC_DISPATCH)
		return js_dispatch_call(en, f->dispatch, DISPID_VALUE, DISPATCH_CONSTRUCT, argc, argv, result);
	/* Native constructors tell a call from new by a this that is the constructor itself. */
	if (f->native)
		return js_call(en, fn, js_obj(f), argc, argv, result) ;
	js_value proto;
	if (!js_get_text(en, f, L"prototype", &proto))
		return FALSE;
	js_object *object = js_object_new(en, JC_OBJECT, proto.type == JV_OBJECT ? proto.object : en->object_prototype);
	js_release(proto);
	js_value value;
	BOOL fine = js_call(en, fn, js_obj(object), argc, argv, &value);
	if (!fine) {
		js_object_release(object);
		return FALSE;
	}
	if (value.type == JV_OBJECT) {
		js_object_release(object);
		*result = value;
	} else {
		js_release(value);
		*result = js_obj(object);
	}
	return TRUE;
}

BOOL js_eval_text(engine *en, const js_string *source, context *ctx, js_value *result)
{
	parse_error error = { 0 };
	script_code *code = js_parse(source->text, FALSE, &error);
	if (!code)
		return js_throw_error(en, L"SyntaxError", error.number, NULL);
	/* The code lives as long as the engine: functions it makes keep pointing into it. */
	code->next = en->kept;
	en->kept = code;
	UINT line = en->line, column = en->column;
	BOOL fine = js_run_program(en, code->program, ctx, result);
	if (fine) {
		en->line = line;
		en->column = column;
	}
	return fine;
}
