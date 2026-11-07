/* SPDX-License-Identifier: MIT */
#include "ps.h"
#include <wchar.h>

LONG ps_locks;
static HINSTANCE this_module;

/* Spec-only exports that nothing has needed end here: they stop the caller rather than answering with a guess. */
void ps_unavailable(void)
{
	RaiseException(0xC0000002, EXCEPTION_NONCONTINUABLE, 0, NULL);
}

static const struct
{
	const CLSID *clsid;
	const WCHAR *name;
	HRESULT (*create)(REFIID, void **);
} classes[] = {
	{&CLSID_InMemoryPropertyStore, L"Packed Property Storage Object", ps_store_create},
	{&CLSID_PropertySystem, L"Property System", ps_system_get},
};

typedef struct
{
	IClassFactory iface;
	HRESULT (*create)(REFIID, void **);
} factory;

static HRESULT WINAPI factory_QueryInterface(IClassFactory *self, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IClassFactory))
	{
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef(IClassFactory *self)
{
	return 2;
}

static ULONG WINAPI factory_Release(IClassFactory *self)
{
	return 1;
}

static HRESULT WINAPI factory_CreateInstance(IClassFactory *self, IUnknown *outer, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	return CONTAINING_RECORD(self, factory, iface)->create(riid, out);
}

static HRESULT WINAPI factory_LockServer(IClassFactory *self, BOOL lock)
{
	if (lock)
		InterlockedIncrement(&ps_locks);
	else
		InterlockedDecrement(&ps_locks);
	return S_OK;
}

static const IClassFactoryVtbl factory_vtbl = {
	factory_QueryInterface,
	factory_AddRef,
	factory_Release,
	factory_CreateInstance,
	factory_LockServer,
};

static factory factories[] = {
	{{&factory_vtbl}, ps_store_create},
	{{&factory_vtbl}, ps_system_get},
};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
	UINT index;
	for (index = 0; index < ARRAYSIZE(classes); index++)
		if (IsEqualGUID(clsid, classes[index].clsid))
			return factory_QueryInterface(&factories[index].iface, riid, out);
	if (out)
		*out = NULL;
	return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return ps_locks ? S_FALSE : S_OK;
}

static LONG set_text(HKEY key, const WCHAR *name, const WCHAR *value)
{
	return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (DWORD)(wcslen(value) + 1) * sizeof(WCHAR));
}

HRESULT WINAPI DllRegisterServer(void)
{
	WCHAR module[MAX_PATH], guid[40], path[64];
	UINT index;
	HKEY key, server;
	GetModuleFileNameW(this_module, module, ARRAYSIZE(module));
	for (index = 0; index < ARRAYSIZE(classes); index++)
	{
		StringFromGUID2(classes[index].clsid, guid, ARRAYSIZE(guid));
		lstrcpyW(path, L"CLSID\\");
		lstrcatW(path, guid);
		if (RegCreateKeyExW(HKEY_CLASSES_ROOT, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL))
			return E_FAIL;
		set_text(key, NULL, classes[index].name);
		if (!RegCreateKeyExW(key, L"InProcServer32", 0, NULL, 0, KEY_WRITE, NULL, &server, NULL))
		{
			set_text(server, NULL, module);
			set_text(server, L"ThreadingModel", L"Both");
			RegCloseKey(server);
		}
		RegCloseKey(key);
	}
	return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
	WCHAR guid[40], path[96];
	UINT index;
	for (index = 0; index < ARRAYSIZE(classes); index++)
	{
		StringFromGUID2(classes[index].clsid, guid, ARRAYSIZE(guid));
		lstrcpyW(path, L"CLSID\\");
		lstrcatW(path, guid);
		lstrcatW(path, L"\\InProcServer32");
		RegDeleteKeyW(HKEY_CLASSES_ROOT, path);
		path[6 + 38] = 0;
		RegDeleteKeyW(HKEY_CLASSES_ROOT, path);
	}
	return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		this_module = instance;
		DisableThreadLibraryCalls(instance);
	}
	return TRUE;
}
