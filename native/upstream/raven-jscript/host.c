/* SPDX-License-Identifier: MIT */
#include "js.h"

/* What JScript adds for its hosts: ActiveXObject and GetObject, Enumerator over collections and VBArray over safe arrays. */

HRESULT js_create_object(const WCHAR *name, IDispatch **out)
{
	CLSID clsid;
	*out = NULL;
	HRESULT hr = name[0] == L'{' ? CLSIDFromString(name, &clsid) : CLSIDFromProgID(name, &clsid);
	if (FAILED(hr))
		return hr;
	return CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, &IID_IDispatch, (void **)out);
}

static BOOL object_result(engine *en, IDispatch *dispatch, js_value *result)
{
	js_object *o = js_dispatch_object(en, dispatch);
	IDispatch_Release(dispatch);
	*result = js_obj(o);
	return TRUE;
}

static BOOL activex_ctor(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *name;
	if (!js_to_string(en, js_arg(argc, argv, 0), &name))
		return FALSE;
	IDispatch *dispatch;
	HRESULT hr = js_create_object(name->text, &dispatch);
	js_string_release(name);
	if (FAILED(hr) || !dispatch)
		return js_throw_error(en, L"Error", JS_E_CANNOT_CREATE, NULL);
	return object_result(en, dispatch, result);
}

static BOOL global_get_object(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	/* GetObject(path) binds a moniker; GetObject("", class) asks for the running object of the class. */
	js_string *path = NULL, *name = NULL;
	if (!js_to_string(en, js_arg(argc, argv, 0), &path))
		return FALSE;
	if (argc >= 2 && !js_to_string(en, argv[1], &name)) {
		js_string_release(path);
		return FALSE;
	}
	IDispatch *dispatch = NULL;
	HRESULT hr;
	if (path->length)
		hr = CoGetObject(path->text, NULL, &IID_IDispatch, (void **)&dispatch);
	else if (name) {
		CLSID clsid;
		IUnknown *unknown = NULL;
		hr = name->text[0] == L'{' ? CLSIDFromString(name->text, &clsid) : CLSIDFromProgID(name->text, &clsid);
		if (SUCCEEDED(hr))
			hr = GetActiveObject(&clsid, NULL, &unknown);
		if (SUCCEEDED(hr)) {
			hr = IUnknown_QueryInterface(unknown, &IID_IDispatch, (void **)&dispatch);
			IUnknown_Release(unknown);
		}
	} else
		hr = E_INVALIDARG;
	js_string_release(path);
	js_string_release(name);
	if (FAILED(hr) || !dispatch)
		return js_throw_error(en, L"Error", JS_E_CANNOT_CREATE, NULL);
	return object_result(en, dispatch, result);
}

/* Enumerator. */

static void enumerator_fetch(js_object *e)
{
	VariantClear(&e->item);
	ULONG fetched = 0;
	e->at_end = !e->enumerator || IEnumVARIANT_Next(e->enumerator, 1, &e->item, &fetched) != S_OK || !fetched;
}

static BOOL enumerator_ctor(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	if (!(self.type == JV_OBJECT && self.object == callee))
		return js_throw_error(en, L"TypeError", JS_E_INVALID_CALL, NULL);
	js_object *e = js_object_new(en, JC_ENUMERATOR, en->enumerator_prototype);
	js_value collection = js_arg(argc, argv, 0);
	if (collection.type == JV_OBJECT && collection.object->cls == JC_DISPATCH) {
		DISPPARAMS none = { 0 };
		VARIANT v;
		VariantInit(&v);
		HRESULT hr = IDispatch_Invoke(collection.object->dispatch, DISPID_NEWENUM, &IID_NULL, LOCALE_USER_DEFAULT,
			DISPATCH_METHOD | DISPATCH_PROPERTYGET, &none, &v, NULL, NULL);
		if (SUCCEEDED(hr) && (V_VT(&v) == VT_UNKNOWN || V_VT(&v) == VT_DISPATCH) && V_UNKNOWN(&v))
			hr = IUnknown_QueryInterface(V_UNKNOWN(&v), &IID_IEnumVARIANT, (void **)&e->enumerator);
		else if (SUCCEEDED(hr))
			hr = E_NOINTERFACE;
		VariantClear(&v);
		if (FAILED(hr)) {
			js_object_release(e);
			return js_throw_error(en, L"TypeError", JS_E_OBJECT_EXPECTED, NULL);
		}
	} else if (collection.type != JV_UNDEFINED) {
		js_object_release(e);
		return js_throw_error(en, L"TypeError", JS_E_OBJECT_EXPECTED, NULL);
	}
	enumerator_fetch(e);
	*result = js_obj(e);
	return TRUE;
}

static BOOL this_enumerator(engine *en, js_value self, js_object **out)
{
	if (self.type == JV_OBJECT && self.object->cls == JC_ENUMERATOR) {
		*out = self.object;
		return TRUE;
	}
	*out = NULL;
	return js_throw_error(en, L"TypeError", JS_E_ENUMERATOR_EXPECTED, NULL);
}

static BOOL enumerator_at_end(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *e;
	if (!this_enumerator(en, self, &e))
		return FALSE;
	*result = js_bool(e->at_end);
	return TRUE;
}

static BOOL enumerator_item(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *e;
	if (!this_enumerator(en, self, &e))
		return FALSE;
	if (e->at_end) {
		*result = js_undefined;
		return TRUE;
	}
	return js_from_variant(en, &e->item, result);
}

static BOOL enumerator_move_first(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *e;
	if (!this_enumerator(en, self, &e))
		return FALSE;
	if (e->enumerator)
		IEnumVARIANT_Reset(e->enumerator);
	enumerator_fetch(e);
	*result = js_undefined;
	return TRUE;
}

static BOOL enumerator_move_next(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *e;
	if (!this_enumerator(en, self, &e))
		return FALSE;
	if (!e->at_end)
		enumerator_fetch(e);
	*result = js_undefined;
	return TRUE;
}

/* VBArray. */

static BOOL this_vbarray(engine *en, js_value self, js_object **out)
{
	if (self.type == JV_OBJECT && self.object->cls == JC_VBARRAY && self.object->array) {
		*out = self.object;
		return TRUE;
	}
	*out = NULL;
	return js_throw_error(en, L"TypeError", JS_E_VBARRAY_EXPECTED, NULL);
}

static BOOL vbarray_ctor(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_value v = js_arg(argc, argv, 0);
	if (v.type != JV_OBJECT || v.object->cls != JC_VBARRAY || !v.object->array)
		return js_throw_error(en, L"TypeError", JS_E_VBARRAY_EXPECTED, NULL);
	if (!(self.type == JV_OBJECT && self.object == callee)) {
		*result = js_retain(v);
		return TRUE;
	}
	SAFEARRAY *copy;
	HRESULT hr = SafeArrayCopy(v.object->array, &copy);
	if (FAILED(hr))
		return js_throw_hresult(en, hr, NULL);
	js_object *a = js_object_new(en, JC_VBARRAY, en->vbarray_prototype);
	a->array = copy;
	V_VT(&a->item) = VT_I2;
	V_I2(&a->item) = V_I2(&v.object->item);
	*result = js_obj(a);
	return TRUE;
}

/* An element as a variant that borrows its contents from the array's memory. */
static void element_variant(js_object *a, const BYTE *data, VARIANT *out)
{
	VARTYPE type = (VARTYPE)V_I2(&a->item);
	if (type == VT_VARIANT) {
		*out = *(const VARIANT *)data;
		return;
	}
	VariantInit(out);
	V_VT(out) = type;
	ULONG size = a->array->cbElements;
	/* The values of every type but VARIANT and DECIMAL fit in eight bytes. */
	memcpy(&V_UI1(out), data, size < 8 ? size : 8);
}

static BOOL vbarray_dimensions(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *a;
	if (!this_vbarray(en, self, &a))
		return FALSE;
	*result = js_number(SafeArrayGetDim(a->array));
	return TRUE;
}

static BOOL bound(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, BOOL upper)
{
	js_object *a;
	INT32 dimension = 1;
	if (!this_vbarray(en, self, &a))
		return FALSE;
	if (argc && !js_to_int32(en, argv[0], &dimension))
		return FALSE;
	LONG value;
	HRESULT hr = upper ? SafeArrayGetUBound(a->array, dimension, &value) : SafeArrayGetLBound(a->array, dimension, &value);
	if (FAILED(hr))
		return js_throw_error(en, L"RangeError", JS_E_SUBSCRIPT, NULL);
	*result = js_number(value);
	return TRUE;
}

static BOOL vbarray_lbound(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	return bound(en, self, argc, argv, result, FALSE);
}

static BOOL vbarray_ubound(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	return bound(en, self, argc, argv, result, TRUE);
}

static BOOL vbarray_get_item(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *a;
	if (!this_vbarray(en, self, &a))
		return FALSE;
	UINT dims = SafeArrayGetDim(a->array);
	if (argc < dims)
		return js_throw_error(en, L"RangeError", JS_E_SUBSCRIPT, NULL);
	LONG indexes[64];
	if (dims > ARRAYSIZE(indexes))
		return js_throw_error(en, L"RangeError", JS_E_SUBSCRIPT, NULL);
	for (UINT i = 0; i < dims; i++) {
		INT32 n;
		if (!js_to_int32(en, argv[i], &n))
			return FALSE;
		indexes[i] = n;
	}
	void *data;
	if (FAILED(SafeArrayPtrOfIndex(a->array, indexes, &data)))
		return js_throw_error(en, L"RangeError", JS_E_SUBSCRIPT, NULL);
	VARIANT v;
	element_variant(a, data, &v);
	return js_from_variant(en, &v, result);
}

static BOOL vbarray_to_array(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *a;
	if (!this_vbarray(en, self, &a))
		return FALSE;
	SAFEARRAY *array = a->array;
	ULONG total = 1;
	for (UINT i = 0; i < array->cDims; i++)
		total *= array->rgsabound[i].cElements;
	BYTE *data;
	if (FAILED(SafeArrayAccessData(array, (void **)&data)))
		return js_throw_error(en, L"TypeError", JS_E_VBARRAY_EXPECTED, NULL);
	/* The elements in the order they sit in memory, the first dimension fastest, as JScript gives them. */
	js_object *out = js_array_new(en, 0);
	BOOL fine = TRUE;
	for (ULONG i = 0; i < total && fine; i++) {
		VARIANT v;
		js_value item;
		element_variant(a, data + i * array->cbElements, &v);
		if ((fine = js_from_variant(en, &v, &item))) {
			js_array_push(out, item);
			js_release(item);
		}
	}
	SafeArrayUnaccessData(array);
	if (!fine) {
		js_object_release(out);
		return FALSE;
	}
	*result = js_obj(out);
	return TRUE;
}

static js_object *host_class(engine *en, const WCHAR *name, js_native ctor, UINT length, js_object *proto)
{
	js_object *c = js_define_native(en->global, name, ctor, length);
	js_define_text(c, L"prototype", js_obj(js_object_retain(proto)), JP_HIDDEN | JP_READONLY);
	js_define_text(proto, L"constructor", js_obj(js_object_retain(c)), JP_DONTENUM);
	return c;
}

void js_init_host(engine *en)
{
	js_object *proto = js_object_new(en, JC_OBJECT, en->object_prototype);
	host_class(en, L"ActiveXObject", activex_ctor, 1, proto);
	js_object_release(proto);
	js_define_native(en->global, L"GetObject", global_get_object, 2);
	proto = en->enumerator_prototype;
	host_class(en, L"Enumerator", enumerator_ctor, 1, proto);
	js_define_native(proto, L"atEnd", enumerator_at_end, 0);
	js_define_native(proto, L"item", enumerator_item, 0);
	js_define_native(proto, L"moveFirst", enumerator_move_first, 0);
	js_define_native(proto, L"moveNext", enumerator_move_next, 0);
	proto = en->vbarray_prototype;
	host_class(en, L"VBArray", vbarray_ctor, 1, proto);
	js_define_native(proto, L"dimensions", vbarray_dimensions, 0);
	js_define_native(proto, L"getItem", vbarray_get_item, 1);
	js_define_native(proto, L"lbound", vbarray_lbound, 0);
	js_define_native(proto, L"toArray", vbarray_to_array, 0);
	js_define_native(proto, L"ubound", vbarray_ubound, 0);
}
