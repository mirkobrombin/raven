/* SPDX-License-Identifier: MIT */
#include "object.h"

/* The shared pieces of the libraries built on it: memory, the IDispatch every object uses, arguments and results, the enumerator of
 * collections, and the class factories. */

static LONG module_locks;

void *sr_alloc(SIZE_T size) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1); }

void *sr_grow(void *block, SIZE_T size)
{
	return block ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, block, size) : sr_alloc(size);
}

void sr_free(void *block)
{
	if (block)
		HeapFree(GetProcessHeap(), 0, block);
}

WCHAR *sr_copy(const WCHAR *text)
{
	SIZE_T length = lstrlenW(text);
	WCHAR *copy = sr_alloc((length + 1) * sizeof(WCHAR));
	memcpy(copy, text, length * sizeof(WCHAR));
	return copy;
}

/* IDispatch. */

static sr_object *impl(IDispatch *iface) { return CONTAINING_RECORD(iface, sr_object, IDispatch_iface); }

static HRESULT WINAPI object_QueryInterface(IDispatch *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch)) {
		*out = iface;
		IDispatch_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI object_AddRef(IDispatch *iface) { return InterlockedIncrement(&impl(iface)->refs); }

static ULONG WINAPI object_Release(IDispatch *iface)
{
	sr_object *object = impl(iface);
	LONG refs = InterlockedDecrement(&object->refs);
	if (!refs) {
		if (object->cls->destroy)
			object->cls->destroy(object);
		sr_free(object);
		InterlockedDecrement(&module_locks);
	}
	return refs;
}

static HRESULT WINAPI object_GetTypeInfoCount(IDispatch *iface, UINT *count)
{
	*count = 0;
	return S_OK;
}

static HRESULT WINAPI object_GetTypeInfo(IDispatch *iface, UINT index, LCID lcid, ITypeInfo **info)
{
	*info = NULL;
	return DISP_E_BADINDEX;
}

static HRESULT WINAPI object_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	for (UINT i = 0; i < count; i++)
		ids[i] = DISPID_UNKNOWN;
	if (!count)
		return S_OK;
	for (const sr_member *m = impl(iface)->cls->members; m->name; m++)
		if (!lstrcmpiW(m->name, names[0])) {
			ids[0] = m->id;
			return count == 1 ? S_OK : DISP_E_UNKNOWNNAME;
		}
	return DISP_E_UNKNOWNNAME;
}

static HRESULT WINAPI object_Invoke(IDispatch *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result,
	EXCEPINFO *info, UINT *bad)
{
	DISPPARAMS none = { 0 };
	if (!params)
		params = &none;
	if (sr_is_put(flags) && !params->cArgs)
		return DISP_E_BADPARAMCOUNT;
	UINT given = params->cArgs - (sr_is_put(flags) ? 1 : 0);
	for (const sr_member *m = impl(iface)->cls->members; m->name; m++)
		if (m->id == id) {
			if (given > m->takes)
				return DISP_E_BADPARAMCOUNT;
			break;
		}
	if (result)
		VariantInit(result);
	return impl(iface)->cls->invoke(impl(iface), id, flags, params, result);
}

static IDispatchVtbl object_vtbl = {
	object_QueryInterface, object_AddRef, object_Release, object_GetTypeInfoCount, object_GetTypeInfo, object_GetIDsOfNames, object_Invoke,
};

void *sr_object_new(const sr_class *cls, SIZE_T size)
{
	sr_object *object = sr_alloc(size);
	object->IDispatch_iface.lpVtbl = &object_vtbl;
	object->refs = 1;
	object->cls = cls;
	InterlockedIncrement(&module_locks);
	return object;
}

sr_object *sr_object_of(IDispatch *dispatch, const sr_class *cls)
{
	if (!dispatch || dispatch->lpVtbl != &object_vtbl || impl(dispatch)->cls != cls)
		return NULL;
	return impl(dispatch);
}

/* Arguments. */

BOOL sr_is_put(WORD flags) { return (flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) != 0; }

static VARIANT *plain(VARIANT *v)
{
	while (v && V_VT(v) == (VT_BYREF | VT_VARIANT))
		v = V_VARIANTREF(v);
	return v;
}

VARIANT *sr_arg(DISPPARAMS *params, WORD flags, UINT index)
{
	UINT count = params->cArgs - (sr_is_put(flags) && params->cArgs ? 1 : 0);
	if (index >= count)
		return NULL;
	VARIANT *v = plain(&params->rgvarg[params->cArgs - 1 - index]);
	if (V_VT(v) == VT_ERROR && V_ERROR(v) == DISP_E_PARAMNOTFOUND)
		return NULL;
	return v;
}

VARIANT *sr_put_value(DISPPARAMS *params) { return params->cArgs ? plain(&params->rgvarg[0]) : NULL; }

/* A value through its default member, then changed to the type wanted. */
static HRESULT change(VARIANT *v, VARTYPE type, VARIANT *out)
{
	VariantInit(out);
	if (!v)
		return DISP_E_PARAMNOTFOUND;
	if (V_VT(v) == VT_NULL)
		return DISP_E_TYPEMISMATCH;
	return VariantChangeType(out, v, type == VT_BSTR ? VARIANT_ALPHABOOL : 0, type);
}

HRESULT sr_text(VARIANT *v, BSTR *out)
{
	VARIANT text;
	*out = NULL;
	if (v && V_VT(v) == VT_EMPTY) {
		*out = SysAllocString(L"");
		return S_OK;
	}
	HRESULT hr = change(v, VT_BSTR, &text);
	if (SUCCEEDED(hr))
		*out = V_BSTR(&text);
	return hr;
}

HRESULT sr_long(VARIANT *v, LONG *out)
{
	VARIANT number;
	*out = 0;
	HRESULT hr = change(v, VT_I4, &number);
	if (SUCCEEDED(hr))
		*out = V_I4(&number);
	return hr;
}

HRESULT sr_bool(VARIANT *v, BOOL *out)
{
	VARIANT flag;
	*out = FALSE;
	HRESULT hr = change(v, VT_BOOL, &flag);
	if (SUCCEEDED(hr))
		*out = V_BOOL(&flag) != VARIANT_FALSE;
	return hr;
}

HRESULT sr_long_or(VARIANT *v, LONG otherwise, LONG *out)
{
	if (!v) {
		*out = otherwise;
		return S_OK;
	}
	return sr_long(v, out);
}

HRESULT sr_bool_or(VARIANT *v, BOOL otherwise, BOOL *out)
{
	if (!v) {
		*out = otherwise;
		return S_OK;
	}
	return sr_bool(v, out);
}

/* Results. */

HRESULT sr_return_bstr(VARIANT *result, BSTR text)
{
	if (!text)
		return E_OUTOFMEMORY;
	if (!result) {
		SysFreeString(text);
		return S_OK;
	}
	V_VT(result) = VT_BSTR;
	V_BSTR(result) = text;
	return S_OK;
}

HRESULT sr_return_text(VARIANT *result, const WCHAR *text) { return sr_return_bstr(result, SysAllocString(text ? text : L"")); }

HRESULT sr_return_long(VARIANT *result, LONG value)
{
	if (result) {
		V_VT(result) = VT_I4;
		V_I4(result) = value;
	}
	return S_OK;
}

HRESULT sr_return_bool(VARIANT *result, BOOL value)
{
	if (result) {
		V_VT(result) = VT_BOOL;
		V_BOOL(result) = value ? VARIANT_TRUE : VARIANT_FALSE;
	}
	return S_OK;
}

HRESULT sr_return_double(VARIANT *result, double value)
{
	if (result) {
		V_VT(result) = VT_R8;
		V_R8(result) = value;
	}
	return S_OK;
}

HRESULT sr_return_object(VARIANT *result, IDispatch *object)
{
	if (!object)
		return E_OUTOFMEMORY;
	if (!result) {
		IDispatch_Release(object);
		return S_OK;
	}
	V_VT(result) = VT_DISPATCH;
	V_DISPATCH(result) = object;
	return S_OK;
}

HRESULT sr_return_date(VARIANT *result, const FILETIME *time)
{
	FILETIME local;
	SYSTEMTIME parts;
	double date;
	if (!FileTimeToLocalFileTime(time, &local) || !FileTimeToSystemTime(&local, &parts) || !SystemTimeToVariantTime(&parts, &date))
		return E_FAIL;
	if (result) {
		V_VT(result) = VT_DATE;
		V_DATE(result) = date;
	}
	return S_OK;
}

/* Errors. */

HRESULT sr_error(LONG number) { return MAKE_HRESULT(SEVERITY_ERROR, FACILITY_CONTROL, number); }

HRESULT sr_last_error(void)
{
	switch (GetLastError()) {
	case ERROR_FILE_NOT_FOUND:
		return sr_error(SR_E_FILE_NOT_FOUND);
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_NAME:
	case ERROR_BAD_PATHNAME:
	case ERROR_INVALID_DRIVE:
		return sr_error(SR_E_PATH_NOT_FOUND);
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
	case ERROR_DIR_NOT_EMPTY:
		return sr_error(SR_E_PERMISSION);
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:
		return sr_error(SR_E_FILE_EXISTS);
	case ERROR_NOT_READY:
		return sr_error(SR_E_DISK_NOT_READY);
	case ERROR_DISK_FULL:
	case ERROR_HANDLE_DISK_FULL:
		return sr_error(61);
	}
	return HRESULT_FROM_WIN32(GetLastError());
}

/* The enumerator collections hand out for For Each, over a copy of their items. */

typedef struct {
	IEnumVARIANT IEnumVARIANT_iface;
	LONG refs;
	VARIANT *items;
	ULONG count, at;
} enumerator;

static enumerator *enum_impl(IEnumVARIANT *iface) { return CONTAINING_RECORD(iface, enumerator, IEnumVARIANT_iface); }

static HRESULT WINAPI enum_QueryInterface(IEnumVARIANT *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumVARIANT)) {
		*out = iface;
		IEnumVARIANT_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI enum_AddRef(IEnumVARIANT *iface) { return InterlockedIncrement(&enum_impl(iface)->refs); }

static ULONG WINAPI enum_Release(IEnumVARIANT *iface)
{
	enumerator *e = enum_impl(iface);
	LONG refs = InterlockedDecrement(&e->refs);
	if (!refs) {
		for (ULONG i = 0; i < e->count; i++)
			VariantClear(&e->items[i]);
		sr_free(e->items);
		sr_free(e);
		InterlockedDecrement(&module_locks);
	}
	return refs;
}

static HRESULT WINAPI enum_Next(IEnumVARIANT *iface, ULONG wanted, VARIANT *out, ULONG *fetched)
{
	enumerator *e = enum_impl(iface);
	ULONG got = 0;
	for (; got < wanted && e->at < e->count; got++, e->at++) {
		VariantInit(&out[got]);
		VariantCopy(&out[got], &e->items[e->at]);
	}
	if (fetched)
		*fetched = got;
	return got == wanted ? S_OK : S_FALSE;
}

static HRESULT WINAPI enum_Skip(IEnumVARIANT *iface, ULONG count)
{
	enumerator *e = enum_impl(iface);
	ULONG left = e->count - e->at;
	e->at += count < left ? count : left;
	return count <= left ? S_OK : S_FALSE;
}

static HRESULT WINAPI enum_Reset(IEnumVARIANT *iface)
{
	enum_impl(iface)->at = 0;
	return S_OK;
}

static IEnumVARIANT *make_enumerator(VARIANT *items, ULONG count);

static HRESULT WINAPI enum_Clone(IEnumVARIANT *iface, IEnumVARIANT **out)
{
	enumerator *e = enum_impl(iface);
	*out = make_enumerator(e->items, e->count);
	enum_impl(*out)->at = e->at;
	return S_OK;
}

static IEnumVARIANTVtbl enum_vtbl = { enum_QueryInterface, enum_AddRef, enum_Release, enum_Next, enum_Skip, enum_Reset, enum_Clone };

static IEnumVARIANT *make_enumerator(VARIANT *items, ULONG count)
{
	enumerator *e = sr_alloc(sizeof(enumerator));
	e->IEnumVARIANT_iface.lpVtbl = &enum_vtbl;
	e->refs = 1;
	e->items = sr_alloc((count ? count : 1) * sizeof(VARIANT));
	e->count = count;
	for (ULONG i = 0; i < count; i++)
		VariantCopy(&e->items[i], &items[i]);
	InterlockedIncrement(&module_locks);
	return &e->IEnumVARIANT_iface;
}

HRESULT sr_return_enum(VARIANT *result, VARIANT *items, ULONG count)
{
	IEnumVARIANT *e = make_enumerator(items, count);
	if (!result) {
		IEnumVARIANT_Release(e);
		return S_OK;
	}
	V_VT(result) = VT_UNKNOWN;
	V_UNKNOWN(result) = (IUnknown *)e;
	return S_OK;
}

/* Class factories. */

typedef struct {
	IClassFactory IClassFactory_iface;
	LONG refs;
	IDispatch *(*create)(void);
} factory;

static factory *factory_impl(IClassFactory *iface) { return CONTAINING_RECORD(iface, factory, IClassFactory_iface); }

static HRESULT WINAPI factory_QueryInterface(IClassFactory *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
		*out = iface;
		IClassFactory_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef(IClassFactory *iface) { return InterlockedIncrement(&factory_impl(iface)->refs); }

static ULONG WINAPI factory_Release(IClassFactory *iface)
{
	LONG refs = InterlockedDecrement(&factory_impl(iface)->refs);
	if (!refs)
		sr_free(factory_impl(iface));
	return refs;
}

static HRESULT WINAPI factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **out)
{
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	IDispatch *object = factory_impl(iface)->create();
	if (!object)
		return E_OUTOFMEMORY;
	HRESULT hr = IDispatch_QueryInterface(object, riid, out);
	IDispatch_Release(object);
	return hr;
}

static HRESULT WINAPI factory_LockServer(IClassFactory *iface, BOOL lock)
{
	if (lock)
		InterlockedIncrement(&module_locks);
	else
		InterlockedDecrement(&module_locks);
	return S_OK;
}

static IClassFactoryVtbl factory_vtbl = {
	factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateInstance, factory_LockServer,
};

HRESULT sr_class_object(const sr_coclass *classes, REFCLSID clsid, REFIID riid, void **out)
{
	*out = NULL;
	for (const sr_coclass *c = classes; c->clsid; c++)
		if (IsEqualCLSID(clsid, c->clsid)) {
			factory *made = sr_alloc(sizeof(*made));
			made->IClassFactory_iface.lpVtbl = &factory_vtbl;
			made->refs = 1;
			made->create = c->create;
			HRESULT hr = IClassFactory_QueryInterface(&made->IClassFactory_iface, riid, out);
			IClassFactory_Release(&made->IClassFactory_iface);
			return hr;
		}
	return CLASS_E_CLASSNOTAVAILABLE;
}

BOOL sr_can_unload(void) { return !module_locks; }
