/* SPDX-License-Identifier: MIT */
#include "wshom.h"

/* The script host's object model: WScript.Shell and WScript.Network, made through COM, and the collection both hand out. */

/* WshShell and WshNetwork, under the class identifiers of the Windows Script Host 5.6 and those of the releases before it. */
static const CLSID CLSID_WshShell = { 0x72c24dd5, 0xd70a, 0x438b, { 0x8a, 0x42, 0x98, 0x42, 0x4b, 0x88, 0xaf, 0xb8 } };
static const CLSID CLSID_WshNetwork = { 0x093ff999, 0x1ea0, 0x4079, { 0x95, 0x25, 0x96, 0x14, 0xc3, 0x50, 0x4b, 0x74 } };
static const CLSID CLSID_WshShell_old = { 0xf935dc22, 0x1cf0, 0x11d0, { 0xad, 0xb9, 0x00, 0xc0, 0x4f, 0xd5, 0x8a, 0x0b } };
static const CLSID CLSID_WshNetwork_old = { 0xf935dc26, 0x1cf0, 0x11d0, { 0xad, 0xb9, 0x00, 0xc0, 0x4f, 0xd5, 0x8a, 0x0b } };

static const sr_coclass classes[] = {
	{ &CLSID_WshShell, wsh_shell_new },
	{ &CLSID_WshNetwork, wsh_network_new },
	{ &CLSID_WshShell_old, wsh_shell_new },
	{ &CLSID_WshNetwork_old, wsh_network_new },
	{ NULL },
};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out) { return sr_class_object(classes, clsid, riid, out); }
HRESULT WINAPI DllCanUnloadNow(void) { return sr_can_unload() ? S_OK : S_FALSE; }

/* The classes are registered with the rest of the system's; there is nothing to write here. */
HRESULT WINAPI DllRegisterServer(void) { return S_OK; }
HRESULT WINAPI DllUnregisterServer(void) { return S_OK; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}

/* The collection. */

typedef struct {
	sr_object base;
	VARIANT *items;
	WCHAR **names;
	ULONG count;
} collection;

enum { COLLECTION_COUNT = 1, COLLECTION_LENGTH };

static const sr_member collection_members[] = {
	{ L"Item", DISPID_VALUE, 1 }, { L"Count", COLLECTION_COUNT, 0 }, { L"length", COLLECTION_LENGTH, 0 }, { L"_NewEnum", DISPID_NEWENUM, 0 },
	{ NULL },
};

static HRESULT collection_invoke(sr_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	collection *c = (collection *)self;
	if (sr_is_put(flags))
		return DISP_E_MEMBERNOTFOUND;
	switch (id) {
	case DISPID_VALUE: {
		VARIANT *which = sr_arg(params, flags, 0);
		if (!which)
			return DISP_E_PARAMNOTFOUND;
		/* A name finds its item; anything else is a position from 0. */
		if (V_VT(which) == VT_BSTR && c->names) {
			for (ULONG i = 0; i < c->count; i++)
				if (!lstrcmpiW(c->names[i], V_BSTR(which)))
					return result ? VariantCopy(result, &c->items[i]) : S_OK;
			return sr_return_text(result, L"");
		}
		LONG index;
		HRESULT hr = sr_long(which, &index);
		if (FAILED(hr))
			return hr;
		if (index < 0 || (ULONG)index >= c->count)
			return sr_error(9);
		return result ? VariantCopy(result, &c->items[index]) : S_OK;
	}
	case COLLECTION_COUNT:
	case COLLECTION_LENGTH:
		return sr_return_long(result, (LONG)c->count);
	case DISPID_NEWENUM:
		return sr_return_enum(result, c->items, c->count);
	}
	return DISP_E_MEMBERNOTFOUND;
}

static void collection_destroy(sr_object *self)
{
	collection *c = (collection *)self;
	for (ULONG i = 0; i < c->count; i++) {
		VariantClear(&c->items[i]);
		if (c->names)
			sr_free(c->names[i]);
	}
	sr_free(c->items);
	sr_free(c->names);
}

static const sr_class collection_class = { collection_members, collection_invoke, collection_destroy };

IDispatch *wsh_collection_new(VARIANT *items, const WCHAR *const *names, ULONG count)
{
	collection *c = sr_object_new(&collection_class, sizeof(collection));
	c->items = sr_alloc((count ? count : 1) * sizeof(VARIANT));
	if (names)
		c->names = sr_alloc((count ? count : 1) * sizeof(WCHAR *));
	for (ULONG i = 0; i < count; i++) {
		VariantInit(&c->items[i]);
		VariantCopy(&c->items[i], &items[i]);
		if (names)
			c->names[i] = sr_copy(names[i]);
	}
	c->count = count;
	return sr_dispatch(c);
}
