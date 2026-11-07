/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <oaidl.h>

/* The error objects of automation: CreateErrorInfo makes one, SetErrorInfo hands it to the thread, GetErrorInfo takes it back
 * and leaves the thread without one. */

typedef struct
{
	IErrorInfo error;
	ICreateErrorInfo create;
	LONG refs;
	GUID guid;
	BSTR source, description, help_file;
	DWORD help_context;
} error_object;

static BOOL same_iid(REFIID left, REFIID right)
{
	return !memcmp(left, right, sizeof(IID));
}

static error_object *from_error(IErrorInfo *iface)
{
	return CONTAINING_RECORD(iface, error_object, error);
}

static error_object *from_create(ICreateErrorInfo *iface)
{
	return CONTAINING_RECORD(iface, error_object, create);
}

static HRESULT query(error_object *object, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_iid(riid, &IID_IUnknown) || same_iid(riid, &IID_IErrorInfo))
		*out = &object->error;
	else if (same_iid(riid, &IID_ICreateErrorInfo))
		*out = &object->create;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&object->refs);
	return S_OK;
}

static ULONG release(error_object *object)
{
	LONG refs = InterlockedDecrement(&object->refs);
	if (!refs) {
		SysFreeString(object->source);
		SysFreeString(object->description);
		SysFreeString(object->help_file);
		HeapFree(GetProcessHeap(), 0, object);
	}
	return refs;
}

// A copy the caller owns, or NULL with E_OUTOFMEMORY when there is text and no room for it.
static HRESULT give_text(BSTR text, BSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = text ? SysAllocStringLen(text, SysStringLen(text)) : NULL;
	return text && !*out ? E_OUTOFMEMORY : S_OK;
}

static HRESULT keep_text(BSTR *field, LPOLESTR text)
{
	BSTR copy = text ? SysAllocString(text) : NULL;
	if (text && !copy)
		return E_OUTOFMEMORY;
	SysFreeString(*field);
	*field = copy;
	return S_OK;
}

static HRESULT WINAPI error_QueryInterface(IErrorInfo *iface, REFIID riid, void **out) { return query(from_error(iface), riid, out); }
static ULONG WINAPI error_AddRef(IErrorInfo *iface) { return InterlockedIncrement(&from_error(iface)->refs); }
static ULONG WINAPI error_Release(IErrorInfo *iface) { return release(from_error(iface)); }

static HRESULT WINAPI error_GetGUID(IErrorInfo *iface, GUID *guid)
{
	if (!guid)
		return E_INVALIDARG;
	*guid = from_error(iface)->guid;
	return S_OK;
}

static HRESULT WINAPI error_GetSource(IErrorInfo *iface, BSTR *out) { return give_text(from_error(iface)->source, out); }
static HRESULT WINAPI error_GetDescription(IErrorInfo *iface, BSTR *out) { return give_text(from_error(iface)->description, out); }
static HRESULT WINAPI error_GetHelpFile(IErrorInfo *iface, BSTR *out) { return give_text(from_error(iface)->help_file, out); }

static HRESULT WINAPI error_GetHelpContext(IErrorInfo *iface, DWORD *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = from_error(iface)->help_context;
	return S_OK;
}

static const IErrorInfoVtbl error_vtbl = {
	error_QueryInterface, error_AddRef, error_Release, error_GetGUID, error_GetSource, error_GetDescription, error_GetHelpFile,
	error_GetHelpContext,
};

static HRESULT WINAPI create_QueryInterface(ICreateErrorInfo *iface, REFIID riid, void **out) { return query(from_create(iface), riid, out); }
static ULONG WINAPI create_AddRef(ICreateErrorInfo *iface) { return InterlockedIncrement(&from_create(iface)->refs); }
static ULONG WINAPI create_Release(ICreateErrorInfo *iface) { return release(from_create(iface)); }

static HRESULT WINAPI create_SetGUID(ICreateErrorInfo *iface, REFGUID guid)
{
	from_create(iface)->guid = *guid;
	return S_OK;
}

static HRESULT WINAPI create_SetSource(ICreateErrorInfo *iface, LPOLESTR text) { return keep_text(&from_create(iface)->source, text); }
static HRESULT WINAPI create_SetDescription(ICreateErrorInfo *iface, LPOLESTR text) { return keep_text(&from_create(iface)->description, text); }
static HRESULT WINAPI create_SetHelpFile(ICreateErrorInfo *iface, LPOLESTR text) { return keep_text(&from_create(iface)->help_file, text); }

static HRESULT WINAPI create_SetHelpContext(ICreateErrorInfo *iface, DWORD context)
{
	from_create(iface)->help_context = context;
	return S_OK;
}

static const ICreateErrorInfoVtbl create_vtbl = {
	create_QueryInterface, create_AddRef, create_Release, create_SetGUID, create_SetSource, create_SetDescription, create_SetHelpFile,
	create_SetHelpContext,
};

HRESULT WINAPI CreateErrorInfo(ICreateErrorInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	error_object *object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *object);
	*out = NULL;
	if (!object)
		return E_OUTOFMEMORY;
	object->error.lpVtbl = &error_vtbl;
	object->create.lpVtbl = &create_vtbl;
	object->refs = 1;
	*out = &object->create;
	return S_OK;
}

static INIT_ONCE slot_once = INIT_ONCE_STATIC_INIT;
static DWORD slot = TLS_OUT_OF_INDEXES;

static BOOL CALLBACK make_slot(INIT_ONCE *once, void *parameter, void **context)
{
	slot = TlsAlloc();
	return slot != TLS_OUT_OF_INDEXES;
}

static BOOL have_slot(void)
{
	return InitOnceExecuteOnce(&slot_once, make_slot, NULL, NULL);
}

HRESULT WINAPI SetErrorInfo(ULONG reserved, IErrorInfo *error)
{
	if (reserved)
		return E_INVALIDARG;
	if (!have_slot())
		return E_OUTOFMEMORY;
	IErrorInfo *old = TlsGetValue(slot);
	if (error)
		IErrorInfo_AddRef(error);
	TlsSetValue(slot, error);
	if (old)
		IErrorInfo_Release(old);
	return S_OK;
}

HRESULT WINAPI GetErrorInfo(ULONG reserved, IErrorInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (reserved)
		return E_INVALIDARG;
	if (!have_slot())
		return S_FALSE;
	*out = TlsGetValue(slot);
	if (!*out)
		return S_FALSE;
	TlsSetValue(slot, NULL);
	return S_OK;
}
