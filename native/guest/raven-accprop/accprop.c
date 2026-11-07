/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven annotation component: the AccPropServices class of oleacc.dll, through which programs annotate the accessible objects of their
 * windows and menus with property values or with servers that answer for them.
 *
 * Annotations are kept in one store per process, keyed by the identity string of the object and the property, so a later call replaces or
 * clears what an earlier one set. Identity strings are opaque to callers: here they are a tag followed by the window or menu handle and
 * the object and child ids, and only strings made by this component decompose. */
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <oleauto.h>
#include <oleacc.h>
#include <string.h>

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))

enum { tag_window = 0x52415701, tag_menu = 0x52415702 };

struct identity {
	DWORD tag, object, child;
	UINT64 handle;
};

struct annotation {
	struct annotation *next;
	BYTE *key;
	DWORD key_length;
	MSAAPROPID property;
	VARIANT value;
	IAccPropServer *server;
};

static CRITICAL_SECTION guard;
static struct annotation *annotations;

static int same_guid(const GUID *left, const GUID *right)
{
	return !memcmp(left, right, sizeof(GUID));
}

static void forget(struct annotation *item)
{
	VariantClear(&item->value);
	if (item->server)
		IAccPropServer_Release(item->server);
	FREE(item->key);
	FREE(item);
}

/* Removes the annotation of one property of an object; the caller holds the guard. */
static void remove_property(const BYTE *key, DWORD length, const MSAAPROPID *property)
{
	struct annotation **link = &annotations;
	while (*link) {
		struct annotation *item = *link;
		if (item->key_length == length && !memcmp(item->key, key, length) && same_guid(&item->property, property)) {
			*link = item->next;
			forget(item);
		} else {
			link = &item->next;
		}
	}
}

/* Sets one property of an object to a value or to a server, replacing what it had. */
static HRESULT annotate(const BYTE *key, DWORD length, const MSAAPROPID *property, const VARIANT *value, IAccPropServer *server)
{
	struct annotation *item;
	HRESULT result = S_OK;
	if (!key || !length)
		return E_INVALIDARG;
	item = ALLOC(sizeof(*item));
	if (!item || !(item->key = ALLOC(length))) {
		FREE(item);
		return E_OUTOFMEMORY;
	}
	memcpy(item->key, key, length);
	item->key_length = length;
	item->property = *property;
	VariantInit(&item->value);
	if (value && FAILED(result = VariantCopy(&item->value, (VARIANT *)value))) {
		FREE(item->key);
		FREE(item);
		return result;
	}
	if (server) {
		item->server = server;
		IAccPropServer_AddRef(server);
	}
	EnterCriticalSection(&guard);
	remove_property(key, length, property);
	item->next = annotations;
	annotations = item;
	LeaveCriticalSection(&guard);
	return S_OK;
}

static HRESULT clear(const BYTE *key, DWORD length, const MSAAPROPID *properties, int count)
{
	int index;
	if (!key || !length || count < 0 || (count && !properties))
		return E_INVALIDARG;
	EnterCriticalSection(&guard);
	for (index = 0; index < count; ++index)
		remove_property(key, length, &properties[index]);
	LeaveCriticalSection(&guard);
	return S_OK;
}

static HRESULT serve(const BYTE *key, DWORD length, const MSAAPROPID *properties, int count, IAccPropServer *server)
{
	int index;
	HRESULT result = S_OK;
	if (count < 0 || (count && !properties) || !server)
		return E_INVALIDARG;
	for (index = 0; index < count && SUCCEEDED(result); ++index)
		result = annotate(key, length, &properties[index], NULL, server);
	return result;
}

static HRESULT compose(DWORD tag, UINT64 handle, DWORD object, DWORD child, BYTE **string, DWORD *length)
{
	struct identity *identity;
	if (!string || !length)
		return E_POINTER;
	*string = NULL;
	*length = 0;
	identity = CoTaskMemAlloc(sizeof(*identity));
	if (!identity)
		return E_OUTOFMEMORY;
	memset(identity, 0, sizeof(*identity));
	identity->tag = tag;
	identity->handle = handle;
	identity->object = object;
	identity->child = child;
	*string = (BYTE *)identity;
	*length = sizeof(*identity);
	return S_OK;
}

static HRESULT decompose(const BYTE *string, DWORD length, DWORD tag, struct identity *identity)
{
	if (!string || length != sizeof(*identity))
		return E_INVALIDARG;
	memcpy(identity, string, sizeof(*identity));
	return identity->tag == tag ? S_OK : E_INVALIDARG;
}

static HRESULT window_key(HWND window, DWORD object, DWORD child, struct identity *identity)
{
	if (!window)
		return E_INVALIDARG;
	memset(identity, 0, sizeof(*identity));
	identity->tag = tag_window;
	identity->handle = (UINT64)(ULONG_PTR)window;
	identity->object = object;
	identity->child = child;
	return S_OK;
}

static HRESULT menu_key(HMENU menu, DWORD child, struct identity *identity)
{
	if (!menu)
		return E_INVALIDARG;
	memset(identity, 0, sizeof(*identity));
	identity->tag = tag_menu;
	identity->handle = (UINT64)(ULONG_PTR)menu;
	identity->child = child;
	return S_OK;
}

struct services {
	IAccPropServices iface;
	LONG refs;
};

static HRESULT WINAPI services_QueryInterface(IAccPropServices *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IAccPropServices)) {
		IAccPropServices_AddRef(iface);
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI services_AddRef(IAccPropServices *iface) { return (ULONG)InterlockedIncrement(&((struct services *)iface)->refs); }

static ULONG WINAPI services_Release(IAccPropServices *iface)
{
	struct services *self = (struct services *)iface;
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs)
		FREE(self);
	return (ULONG)refs;
}

static HRESULT WINAPI services_SetPropValue(IAccPropServices *iface, const BYTE *key, DWORD length, MSAAPROPID property, VARIANT value)
{
	return annotate(key, length, &property, &value, NULL);
}

static HRESULT WINAPI services_SetPropServer(IAccPropServices *iface, const BYTE *key, DWORD length, const MSAAPROPID *properties, int count,
	IAccPropServer *server, AnnoScope scope)
{
	return serve(key, length, properties, count, server);
}

static HRESULT WINAPI services_ClearProps(IAccPropServices *iface, const BYTE *key, DWORD length, const MSAAPROPID *properties, int count)
{
	return clear(key, length, properties, count);
}

static HRESULT WINAPI services_SetHwndProp(IAccPropServices *iface, HWND window, DWORD object, DWORD child, MSAAPROPID property, VARIANT value)
{
	struct identity key;
	const HRESULT result = window_key(window, object, child, &key);
	return FAILED(result) ? result : annotate((const BYTE *)&key, sizeof(key), &property, &value, NULL);
}

static HRESULT WINAPI services_SetHwndPropStr(IAccPropServices *iface, HWND window, DWORD object, DWORD child, MSAAPROPID property, LPCWSTR text)
{
	struct identity key;
	VARIANT value;
	HRESULT result = window_key(window, object, child, &key);
	if (FAILED(result))
		return result;
	if (!text)
		return E_INVALIDARG;
	VariantInit(&value);
	value.vt = VT_BSTR;
	value.bstrVal = SysAllocString(text);
	if (!value.bstrVal)
		return E_OUTOFMEMORY;
	result = annotate((const BYTE *)&key, sizeof(key), &property, &value, NULL);
	VariantClear(&value);
	return result;
}

static HRESULT WINAPI services_SetHwndPropServer(IAccPropServices *iface, HWND window, DWORD object, DWORD child, const MSAAPROPID *properties,
	int count, IAccPropServer *server, AnnoScope scope)
{
	struct identity key;
	const HRESULT result = window_key(window, object, child, &key);
	return FAILED(result) ? result : serve((const BYTE *)&key, sizeof(key), properties, count, server);
}

static HRESULT WINAPI services_ClearHwndProps(IAccPropServices *iface, HWND window, DWORD object, DWORD child, const MSAAPROPID *properties,
	int count)
{
	struct identity key;
	const HRESULT result = window_key(window, object, child, &key);
	return FAILED(result) ? result : clear((const BYTE *)&key, sizeof(key), properties, count);
}

static HRESULT WINAPI services_ComposeHwndIdentityString(IAccPropServices *iface, HWND window, DWORD object, DWORD child, BYTE **string,
	DWORD *length)
{
	return compose(tag_window, (UINT64)(ULONG_PTR)window, object, child, string, length);
}

static HRESULT WINAPI services_DecomposeHwndIdentityString(IAccPropServices *iface, const BYTE *string, DWORD length, HWND *window,
	DWORD *object, DWORD *child)
{
	struct identity identity;
	const HRESULT result = decompose(string, length, tag_window, &identity);
	if (FAILED(result))
		return result;
	if (!window || !object || !child)
		return E_POINTER;
	*window = (HWND)(ULONG_PTR)identity.handle;
	*object = identity.object;
	*child = identity.child;
	return S_OK;
}

static HRESULT WINAPI services_SetHmenuProp(IAccPropServices *iface, HMENU menu, DWORD child, MSAAPROPID property, VARIANT value)
{
	struct identity key;
	const HRESULT result = menu_key(menu, child, &key);
	return FAILED(result) ? result : annotate((const BYTE *)&key, sizeof(key), &property, &value, NULL);
}

static HRESULT WINAPI services_SetHmenuPropStr(IAccPropServices *iface, HMENU menu, DWORD child, MSAAPROPID property, LPCWSTR text)
{
	struct identity key;
	VARIANT value;
	HRESULT result = menu_key(menu, child, &key);
	if (FAILED(result))
		return result;
	if (!text)
		return E_INVALIDARG;
	VariantInit(&value);
	value.vt = VT_BSTR;
	value.bstrVal = SysAllocString(text);
	if (!value.bstrVal)
		return E_OUTOFMEMORY;
	result = annotate((const BYTE *)&key, sizeof(key), &property, &value, NULL);
	VariantClear(&value);
	return result;
}

static HRESULT WINAPI services_SetHmenuPropServer(IAccPropServices *iface, HMENU menu, DWORD child, const MSAAPROPID *properties, int count,
	IAccPropServer *server, AnnoScope scope)
{
	struct identity key;
	const HRESULT result = menu_key(menu, child, &key);
	return FAILED(result) ? result : serve((const BYTE *)&key, sizeof(key), properties, count, server);
}

static HRESULT WINAPI services_ClearHmenuProps(IAccPropServices *iface, HMENU menu, DWORD child, const MSAAPROPID *properties, int count)
{
	struct identity key;
	const HRESULT result = menu_key(menu, child, &key);
	return FAILED(result) ? result : clear((const BYTE *)&key, sizeof(key), properties, count);
}

static HRESULT WINAPI services_ComposeHmenuIdentityString(IAccPropServices *iface, HMENU menu, DWORD child, BYTE **string, DWORD *length)
{
	return compose(tag_menu, (UINT64)(ULONG_PTR)menu, 0, child, string, length);
}

static HRESULT WINAPI services_DecomposeHmenuIdentityString(IAccPropServices *iface, const BYTE *string, DWORD length, HMENU *menu, DWORD *child)
{
	struct identity identity;
	const HRESULT result = decompose(string, length, tag_menu, &identity);
	if (FAILED(result))
		return result;
	if (!menu || !child)
		return E_POINTER;
	*menu = (HMENU)(ULONG_PTR)identity.handle;
	*child = identity.child;
	return S_OK;
}

static IAccPropServicesVtbl services_table = {services_QueryInterface, services_AddRef, services_Release, services_SetPropValue,
	services_SetPropServer, services_ClearProps, services_SetHwndProp, services_SetHwndPropStr, services_SetHwndPropServer,
	services_ClearHwndProps, services_ComposeHwndIdentityString, services_DecomposeHwndIdentityString, services_SetHmenuProp,
	services_SetHmenuPropStr, services_SetHmenuPropServer, services_ClearHmenuProps, services_ComposeHmenuIdentityString,
	services_DecomposeHmenuIdentityString};

/* ---- class factory ---- */
struct class_factory {
	IClassFactory iface;
	LONG refs;
};

static HRESULT WINAPI factory_QueryInterface(IClassFactory *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IClassFactory)) {
		IClassFactory_AddRef(iface);
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef(IClassFactory *iface) { return (ULONG)InterlockedIncrement(&((struct class_factory *)iface)->refs); }

static ULONG WINAPI factory_Release(IClassFactory *iface)
{
	struct class_factory *self = (struct class_factory *)iface;
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs)
		FREE(self);
	return (ULONG)refs;
}

static HRESULT WINAPI factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **out)
{
	struct services *services;
	HRESULT result;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	services = ALLOC(sizeof(*services));
	if (!services)
		return E_OUTOFMEMORY;
	services->iface.lpVtbl = &services_table;
	services->refs = 1;
	result = IAccPropServices_QueryInterface(&services->iface, riid, out);
	IAccPropServices_Release(&services->iface);
	return result;
}

static HRESULT WINAPI factory_LockServer(IClassFactory *iface, BOOL lock)
{
	return S_OK;
}

static IClassFactoryVtbl factory_table = {factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateInstance, factory_LockServer};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
	struct class_factory *factory;
	HRESULT result;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!same_guid(clsid, &CLSID_AccPropServices))
		return CLASS_E_CLASSNOTAVAILABLE;
	factory = ALLOC(sizeof(*factory));
	if (!factory)
		return E_OUTOFMEMORY;
	factory->iface.lpVtbl = &factory_table;
	factory->refs = 1;
	result = IClassFactory_QueryInterface(&factory->iface, riid, out);
	IClassFactory_Release(&factory->iface);
	return result;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		InitializeCriticalSection(&guard);
	return TRUE;
}
