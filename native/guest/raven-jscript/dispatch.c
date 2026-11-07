/* SPDX-License-Identifier: MIT */
#include "js.h"

/* The bridge to COM. Values cross as variants. The host's objects are script objects of the dispatch class, whose properties and
 * methods go through IDispatch by name. Script objects the host is handed are IDispatchEx objects: their properties by name, a
 * function called through DISPID_VALUE, and the global object as the dispatch the host asks the engine for. */

/* Variants. */

static BOOL from_safearray(engine *en, const VARIANT *v, js_value *result)
{
	js_object *array = js_object_new(en, JC_VBARRAY, NULL);
	SAFEARRAY *copy = NULL;
	HRESULT hr = SafeArrayCopy((V_VT(v) & VT_BYREF) ? *V_ARRAYREF(v) : V_ARRAY(v), &copy);
	if (FAILED(hr)) {
		js_object_release(array);
		return js_throw_hresult(en, hr, NULL);
	}
	array->array = copy;
	/* The element type comes with the copy. */
	VARTYPE type = V_VT(v) & VT_TYPEMASK;
	V_VT(&array->item) = VT_I2;
	V_I2(&array->item) = (SHORT)type;
	*result = js_obj(array);
	return TRUE;
}

BOOL js_from_variant(engine *en, const VARIANT *v, js_value *result)
{
	while (V_VT(v) == (VT_BYREF | VT_VARIANT))
		v = V_VARIANTREF(v);
	if (V_VT(v) & VT_ARRAY)
		return from_safearray(en, v, result);
	VARIANT plain;
	VariantInit(&plain);
	if (V_VT(v) & VT_BYREF) {
		if (FAILED(VariantCopyInd(&plain, (VARIANT *)v)))
			return js_throw_error(en, L"TypeError", JS_E_TYPE_MISMATCH, NULL);
		v = &plain;
	}
	BOOL fine = TRUE;
	switch (V_VT(v)) {
	case VT_EMPTY:
		*result = js_undefined;
		break;
	case VT_NULL:
		*result = js_null;
		break;
	case VT_BOOL:
		*result = js_bool(V_BOOL(v) != VARIANT_FALSE);
		break;
	case VT_BSTR:
		*result = js_str(js_string_new(V_BSTR(v), V_BSTR(v) ? SysStringLen(V_BSTR(v)) : 0));
		break;
	case VT_DISPATCH:
	case VT_UNKNOWN: {
		IDispatch *dispatch = NULL;
		if (!V_UNKNOWN(v))
			*result = js_null;
		else if (SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(v), &IID_IDispatch, (void **)&dispatch))) {
			*result = js_obj(js_dispatch_object(en, dispatch));
			IDispatch_Release(dispatch);
		} else
			*result = js_null;
		break;
	}
	case VT_DATE:
		result->type = JV_DATE;
		result->number = js_from_var_date(V_DATE(v));
		break;
	case VT_ERROR:
		*result = js_undefined;
		break;
	default: {
		VARIANT number;
		VariantInit(&number);
		if (SUCCEEDED(VariantChangeType(&number, (VARIANT *)v, 0, VT_R8)))
			*result = js_number(V_R8(&number));
		else
			fine = js_throw_error(en, L"TypeError", JS_E_TYPE_MISMATCH, NULL);
		break;
	}
	}
	VariantClear(&plain);
	return fine;
}

HRESULT js_to_variant(engine *en, js_value v, VARIANT *result)
{
	VariantInit(result);
	switch (v.type) {
	case JV_UNDEFINED:
		return S_OK;
	case JV_NULL:
		V_VT(result) = VT_NULL;
		return S_OK;
	case JV_BOOL:
		V_VT(result) = VT_BOOL;
		V_BOOL(result) = v.boolean ? VARIANT_TRUE : VARIANT_FALSE;
		return S_OK;
	case JV_NUMBER:
		/* Whole numbers that fit go as integers, as JScript hands them. */
		if (v.number == (double)(INT32)v.number && !(v.number == 0 && 1 / v.number < 0)) {
			V_VT(result) = VT_I4;
			V_I4(result) = (INT32)v.number;
		} else {
			V_VT(result) = VT_R8;
			V_R8(result) = v.number;
		}
		return S_OK;
	case JV_STRING:
		V_VT(result) = VT_BSTR;
		V_BSTR(result) = SysAllocStringLen(v.string->text, v.string->length);
		return V_BSTR(result) ? S_OK : E_OUTOFMEMORY;
	case JV_DATE:
		V_VT(result) = VT_DATE;
		V_DATE(result) = js_to_var_date(v.number);
		return S_OK;
	case JV_OBJECT:
		if (v.object->cls == JC_DISPATCH) {
			V_VT(result) = VT_DISPATCH;
			V_DISPATCH(result) = v.object->dispatch;
			IDispatch_AddRef(v.object->dispatch);
			return S_OK;
		}
		if (v.object->cls == JC_VBARRAY && v.object->array) {
			SAFEARRAY *copy;
			HRESULT hr = SafeArrayCopy(v.object->array, &copy);
			if (FAILED(hr))
				return hr;
			V_VT(result) = VT_ARRAY | (VARTYPE)V_I2(&v.object->item);
			V_ARRAY(result) = copy;
			return S_OK;
		}
		V_VT(result) = VT_DISPATCH;
		V_DISPATCH(result) = js_outward(v.object);
		return S_OK;
	}
	return S_OK;
}

/* The host's objects. */

js_object *js_dispatch_object(engine *en, IDispatch *dispatch)
{
	/* A script object that went out and came back is itself again. */
	js_object *own = NULL;
	extern js_object *js_outward_object(IDispatch *dispatch);
	if ((own = js_outward_object(dispatch)) != NULL && own->en == en)
		return js_object_retain(own);
	js_object *o = js_object_new(en, JC_DISPATCH, NULL);
	o->dispatch = dispatch;
	IDispatch_AddRef(dispatch);
	return o;
}

static BOOL id_of(engine *en, IDispatch *dispatch, js_string *name, DISPID *id, BOOL quiet)
{
	LPOLESTR names = name->text;
	HRESULT hr = IDispatch_GetIDsOfNames(dispatch, &IID_NULL, &names, 1, LOCALE_USER_DEFAULT, id);
	if (SUCCEEDED(hr))
		return TRUE;
	/* IDispatchEx objects make the names they are given. */
	IDispatchEx *ex;
	if (SUCCEEDED(IDispatch_QueryInterface(dispatch, &IID_IDispatchEx, (void **)&ex))) {
		BSTR text = SysAllocStringLen(name->text, name->length);
		hr = IDispatchEx_GetDispID(ex, text, fdexNameEnsure | fdexNameCaseSensitive, id);
		SysFreeString(text);
		IDispatchEx_Release(ex);
		if (SUCCEEDED(hr))
			return TRUE;
	}
	if (!quiet)
		js_throw_error(en, L"TypeError", JS_E_NO_PROPERTY, NULL);
	return FALSE;
}

BOOL js_dispatch_call(engine *en, IDispatch *dispatch, DISPID id, WORD flags, UINT argc, js_value *argv, js_value *result)
{
	*result = js_undefined;
	VARIANT *args = argc ? js_alloc(argc * sizeof(VARIANT)) : NULL;
	HRESULT hr = S_OK;
	/* The arguments go last first. */
	for (UINT i = 0; i < argc && SUCCEEDED(hr); i++)
		hr = js_to_variant(en, argv[i], &args[argc - 1 - i]);
	VARIANT value;
	VariantInit(&value);
	EXCEPINFO info = { 0 };
	if (SUCCEEDED(hr)) {
		DISPPARAMS params = { args, NULL, argc, 0 };
		UINT failed_arg = 0;
		hr = IDispatch_Invoke(dispatch, id, &IID_NULL, LOCALE_USER_DEFAULT, flags, &params, &value, &info, &failed_arg);
	}
	for (UINT i = 0; i < argc; i++)
		VariantClear(&args[i]);
	js_free(args);
	BOOL fine;
	if (FAILED(hr)) {
		if (hr == DISP_E_EXCEPTION && info.pfnDeferredFillIn)
			info.pfnDeferredFillIn(&info);
		fine = js_throw_hresult(en, hr == DISP_E_EXCEPTION && info.scode ? info.scode : hr, &info);
	} else
		fine = js_from_variant(en, &value, result);
	VariantClear(&value);
	SysFreeString(info.bstrSource);
	SysFreeString(info.bstrDescription);
	SysFreeString(info.bstrHelpFile);
	return fine;
}

BOOL js_dispatch_get(engine *en, js_object *o, js_string *name, js_value *result)
{
	DISPID id;
	if (!id_of(en, o->dispatch, name, &id, TRUE)) {
		*result = js_undefined;
		return TRUE;
	}
	return js_dispatch_call(en, o->dispatch, id, DISPATCH_PROPERTYGET, 0, NULL, result);
}

BOOL js_dispatch_has(engine *en, js_object *o, js_string *name)
{
	DISPID id;
	LPOLESTR names = name->text;
	return SUCCEEDED(IDispatch_GetIDsOfNames(o->dispatch, &IID_NULL, &names, 1, LOCALE_USER_DEFAULT, &id));
}

BOOL js_dispatch_put_at(engine *en, js_object *o, js_string *name, UINT argc, js_value *argv, js_value value)
{
	DISPID id = DISPID_VALUE;
	if (name && !id_of(en, o->dispatch, name, &id, FALSE))
		return FALSE;
	/* The value goes first, the indexes after it last first. */
	VARIANT *args = js_alloc((argc + 1) * sizeof(VARIANT));
	HRESULT hr = js_to_variant(en, value, &args[0]);
	for (UINT i = 0; i < argc && SUCCEEDED(hr); i++)
		hr = js_to_variant(en, argv[i], &args[argc - i]);
	if (FAILED(hr)) {
		for (UINT i = 0; i <= argc; i++)
			VariantClear(&args[i]);
		js_free(args);
		return js_throw_hresult(en, hr, NULL);
	}
	DISPID put = DISPID_PROPERTYPUT;
	DISPPARAMS params = { args, &put, argc + 1, 1 };
	EXCEPINFO info = { 0 };
	hr = IDispatch_Invoke(o->dispatch, id, &IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT, &params, NULL, &info, NULL);
	/* An object goes by reference to a property that takes it so. */
	if (FAILED(hr) && V_VT(&args[0]) == VT_DISPATCH && hr != DISP_E_EXCEPTION)
		hr = IDispatch_Invoke(o->dispatch, id, &IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUTREF, &params, NULL, &info, NULL);
	for (UINT i = 0; i <= argc; i++)
		VariantClear(&args[i]);
	js_free(args);
	if (FAILED(hr) && hr == DISP_E_EXCEPTION && info.pfnDeferredFillIn)
		info.pfnDeferredFillIn(&info);
	BOOL fine = SUCCEEDED(hr) || js_throw_hresult(en, hr == DISP_E_EXCEPTION && info.scode ? info.scode : hr, &info);
	SysFreeString(info.bstrSource);
	SysFreeString(info.bstrDescription);
	SysFreeString(info.bstrHelpFile);
	return fine;
}

BOOL js_dispatch_put(engine *en, js_object *o, js_string *name, js_value value) { return js_dispatch_put_at(en, o, name, 0, NULL, value); }

BOOL js_dispatch_invoke_name(engine *en, js_object *o, js_string *name, UINT argc, js_value *argv, js_value *result)
{
	DISPID id;
	if (!id_of(en, o->dispatch, name, &id, FALSE))
		return FALSE;
	return js_dispatch_call(en, o->dispatch, id, DISPATCH_METHOD | DISPATCH_PROPERTYGET, argc, argv, result);
}

/* Names by dispatch identifier, the same for every script object of an engine. */

#define NAME_IDS 0x1000

DISPID js_dispatch_id(engine *en, const js_string *name)
{
	for (UINT i = 0; i < en->dispatch_name_count; i++)
		if (js_string_equal(en->dispatch_names[i], name))
			return NAME_IDS + (DISPID)i;
	en->dispatch_names = js_grow(en->dispatch_names, (en->dispatch_name_count + 1) * sizeof(js_string *));
	en->dispatch_names[en->dispatch_name_count] = js_string_new(name->text, name->length);
	return NAME_IDS + (DISPID)en->dispatch_name_count++;
}

js_string *js_dispatch_name(engine *en, DISPID id)
{
	if (id < NAME_IDS || (UINT)(id - NAME_IDS) >= en->dispatch_name_count)
		return NULL;
	return en->dispatch_names[id - NAME_IDS];
}

/* Script objects as the host sees them. */

typedef struct outward {
	IDispatchEx IDispatchEx_iface;
	LONG refs;
	js_object *object;
	engine *en;
} outward;

static outward *outward_impl(IDispatchEx *iface) { return CONTAINING_RECORD(iface, outward, IDispatchEx_iface); }

static HRESULT WINAPI outward_QueryInterface(IDispatchEx *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_IDispatchEx)) {
		*out = iface;
		IDispatchEx_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI outward_AddRef(IDispatchEx *iface) { return InterlockedIncrement(&outward_impl(iface)->refs); }

static ULONG WINAPI outward_Release(IDispatchEx *iface)
{
	outward *w = outward_impl(iface);
	LONG refs = InterlockedDecrement(&w->refs);
	if (!refs) {
		engine *en = w->en;
		js_object *object = w->object;
		if (object) {
			object->outward = NULL;
			js_object_release(object);
		}
		js_free(w);
		/* The wrapper kept the engine alive. */
		IActiveScript_Release(&en->IActiveScript_iface);
	}
	return refs;
}

static HRESULT WINAPI outward_GetTypeInfoCount(IDispatchEx *iface, UINT *count)
{
	*count = 0;
	return S_OK;
}

static HRESULT WINAPI outward_GetTypeInfo(IDispatchEx *iface, UINT index, LCID lcid, ITypeInfo **info)
{
	*info = NULL;
	return DISP_E_BADINDEX;
}

/* The property a name finds: on the object or its prototypes; for the global object also the functions and variables of the script. */
static HRESULT outward_id(outward *w, const WCHAR *name, BOOL ensure, DISPID *id)
{
	if (!w->object)
		return E_UNEXPECTED;
	js_string *key = js_string_of(name);
	BOOL known = js_has(w->en, w->object, key);
	if (known || ensure)
		*id = js_dispatch_id(w->en, key);
	js_string_release(key);
	return known || ensure ? S_OK : DISP_E_UNKNOWNNAME;
}

static HRESULT WINAPI outward_GetIDsOfNames(IDispatchEx *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	outward *w = outward_impl(iface);
	HRESULT hr = outward_id(w, names[0], FALSE, &ids[0]);
	for (UINT i = 1; i < count; i++)
		ids[i] = DISPID_UNKNOWN;
	return hr;
}

static HRESULT invoke_object(outward *w, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *info)
{
	engine *en = w->en;
	js_object *o = w->object;
	if (!o)
		return E_UNEXPECTED;
	if (result)
		VariantInit(result);
	UINT argc = params ? params->cArgs - params->cNamedArgs : 0;
	js_value *argv = argc ? js_alloc(argc * sizeof(js_value)) : NULL;
	BOOL fine = TRUE;
	/* Arguments arrive last first; the named ones (the value of a put) come first. */
	for (UINT i = 0; i < argc && fine; i++)
		fine = js_from_variant(en, &params->rgvarg[params->cArgs - 1 - i], &argv[i]);
	js_value value = js_undefined;
	js_string *name = id == DISPID_VALUE ? NULL : js_dispatch_name(en, id);
	HRESULT hr = S_OK;
	if (fine && id != DISPID_VALUE && !name)
		hr = DISP_E_MEMBERNOTFOUND;
	else if (fine) {
		if (flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) {
			js_value put = js_undefined;
			if (params && params->cArgs && (fine = js_from_variant(en, &params->rgvarg[0], &put))) {
				fine = name ? js_put(en, o, name, put) : FALSE;
				if (!name)
					hr = DISP_E_MEMBERNOTFOUND;
				js_release(put);
			}
		} else if (!name) {
			/* The object itself: a function is called, anything else gives its value. */
			if (o->cls == JC_FUNCTION && (flags & (DISPATCH_METHOD | DISPATCH_CONSTRUCT)))
				fine = flags & DISPATCH_CONSTRUCT ? js_construct(en, js_obj(o), argc, argv, &value) :
					js_call(en, js_obj(o), js_undefined, argc, argv, &value);
			else
				fine = js_to_primitive(en, js_obj(o), HINT_NONE, &value);
		} else {
			fine = js_get(en, o, name, &value);
			/* A function asked for as a method is called, with or without arguments. */
			if (fine && (flags & DISPATCH_METHOD) && js_is_callable(value)) {
				js_value method = value;
				fine = js_call(en, method, js_obj(o), argc, argv, &value);
				js_release(method);
			}
		}
	}
	for (UINT i = 0; i < argc; i++)
		js_release(argv[i]);
	js_free(argv);
	if (!fine && en->throwing) {
		script_failure failure = { 0 };
		js_failure_of_exception(en, &failure);
		if (info) {
			ZeroMemory(info, sizeof(*info));
			info->scode = failure.number;
			info->bstrDescription = failure.description;
			info->bstrSource = failure.source;
			failure.description = failure.source = NULL;
		}
		js_failure_clear(&failure);
		js_release(value);
		return DISP_E_EXCEPTION;
	}
	if (FAILED(hr)) {
		js_release(value);
		return hr;
	}
	if (result)
		hr = js_to_variant(en, value, result);
	js_release(value);
	return hr;
}

static HRESULT WINAPI outward_Invoke(IDispatchEx *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result,
	EXCEPINFO *info, UINT *arg_error)
{
	return invoke_object(outward_impl(iface), id, flags, params, result, info);
}

static HRESULT WINAPI outward_GetDispID(IDispatchEx *iface, BSTR name, DWORD flags, DISPID *id)
{
	return outward_id(outward_impl(iface), name, (flags & fdexNameEnsure) != 0, id);
}

static HRESULT WINAPI outward_InvokeEx(IDispatchEx *iface, DISPID id, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result,
	EXCEPINFO *info, IServiceProvider *caller)
{
	return invoke_object(outward_impl(iface), id, flags, params, result, info);
}

static HRESULT WINAPI outward_DeleteMemberByName(IDispatchEx *iface, BSTR name, DWORD flags)
{
	outward *w = outward_impl(iface);
	if (!w->object)
		return E_UNEXPECTED;
	js_string *key = js_string_of(name);
	BOOL deleted;
	js_delete(w->en, w->object, key, &deleted);
	js_string_release(key);
	return deleted ? S_OK : S_FALSE;
}

static HRESULT WINAPI outward_DeleteMemberByDispID(IDispatchEx *iface, DISPID id)
{
	outward *w = outward_impl(iface);
	js_string *name = js_dispatch_name(w->en, id);
	if (!w->object || !name)
		return DISP_E_MEMBERNOTFOUND;
	BOOL deleted;
	js_delete(w->en, w->object, name, &deleted);
	return deleted ? S_OK : S_FALSE;
}

static HRESULT WINAPI outward_GetMemberProperties(IDispatchEx *iface, DISPID id, DWORD fetch, DWORD *properties)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI outward_GetMemberName(IDispatchEx *iface, DISPID id, BSTR *name)
{
	js_string *text = js_dispatch_name(outward_impl(iface)->en, id);
	if (!text)
		return DISP_E_MEMBERNOTFOUND;
	*name = SysAllocStringLen(text->text, text->length);
	return S_OK;
}

static HRESULT WINAPI outward_GetNextDispID(IDispatchEx *iface, DWORD flags, DISPID id, DISPID *next)
{
	/* The enumerable properties in order, then the end. */
	outward *w = outward_impl(iface);
	if (!w->object)
		return E_UNEXPECTED;
	UINT count;
	js_string **names = js_enumerate(w->en, w->object, &count);
	js_string *current = id == DISPID_STARTENUM ? NULL : js_dispatch_name(w->en, id);
	BOOL take = id == DISPID_STARTENUM;
	HRESULT hr = S_FALSE;
	*next = DISPID_UNKNOWN;
	for (UINT i = 0; i < count; i++) {
		if (take && hr == S_FALSE) {
			*next = js_dispatch_id(w->en, names[i]);
			hr = S_OK;
		}
		if (current && js_string_equal(names[i], current))
			take = TRUE;
		js_string_release(names[i]);
	}
	js_free(names);
	return hr;
}

static HRESULT WINAPI outward_GetNameSpaceParent(IDispatchEx *iface, IUnknown **parent)
{
	*parent = NULL;
	return E_NOTIMPL;
}

static IDispatchExVtbl outward_vtbl = {
	outward_QueryInterface, outward_AddRef, outward_Release, outward_GetTypeInfoCount, outward_GetTypeInfo, outward_GetIDsOfNames,
	outward_Invoke, outward_GetDispID, outward_InvokeEx, outward_DeleteMemberByName, outward_DeleteMemberByDispID,
	outward_GetMemberProperties, outward_GetMemberName, outward_GetNextDispID, outward_GetNameSpaceParent,
};

IDispatch *js_outward(js_object *o)
{
	if (o->outward) {
		IDispatchEx_AddRef(&o->outward->IDispatchEx_iface);
		return (IDispatch *)&o->outward->IDispatchEx_iface;
	}
	outward *w = js_alloc(sizeof(outward));
	w->IDispatchEx_iface.lpVtbl = &outward_vtbl;
	w->refs = 1;
	w->object = js_object_retain(o);
	w->en = o->en;
	IActiveScript_AddRef(&o->en->IActiveScript_iface);
	o->outward = w;
	return (IDispatch *)&w->IDispatchEx_iface;
}

js_object *js_outward_object(IDispatch *dispatch)
{
	if (dispatch->lpVtbl != (IDispatchVtbl *)&outward_vtbl)
		return NULL;
	return outward_impl((IDispatchEx *)dispatch)->object;
}

void js_outward_detach(js_object *o)
{
	if (o->outward) {
		o->outward->object = NULL;
		o->outward = NULL;
	}
}

IDispatch *js_global_dispatch_new(engine *en) { return js_outward(en->global); }
