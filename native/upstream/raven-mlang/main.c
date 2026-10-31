/* SPDX-License-Identifier: MIT */
#include "ml.h"
#include <wchar.h>

LONG ml_locks;
static HINSTANCE this_module;

/* The interface identifiers the toolchain's import library does not carry. */
const IID IID_IMultiLanguage = {0x275c23e1, 0x3747, 0x11d0, {0x9f, 0xea, 0x00, 0xaa, 0x00, 0x3f, 0x86, 0x46}};
const IID IID_IMultiLanguage2 = {0xdccfc164, 0x2b38, 0x11d2, {0xb7, 0xec, 0x00, 0xc0, 0x4f, 0x8f, 0x5d, 0x9a}};
const IID IID_IMultiLanguage3 = {0x4e5868ab, 0xb157, 0x4623, {0x9a, 0xcc, 0x6a, 0x1d, 0x9c, 0xae, 0xbe, 0x04}};
const IID IID_IMLangCodePages = {0x359f3443, 0xbd4a, 0x11d0, {0xb1, 0x88, 0x00, 0xaa, 0x00, 0x38, 0xc9, 0x69}};
const IID IID_IMLangFontLink = {0x359f3441, 0xbd4a, 0x11d0, {0xb1, 0x88, 0x00, 0xaa, 0x00, 0x38, 0xc9, 0x69}};
const IID IID_IMLangFontLink2 = {0xdccfc162, 0x2b38, 0x11d2, {0xb7, 0xec, 0x00, 0xc0, 0x4f, 0x8f, 0x5d, 0x9a}};
const IID IID_IEnumCodePage = {0x275c23e3, 0x3747, 0x11d0, {0x9f, 0xea, 0x00, 0xaa, 0x00, 0x3f, 0x86, 0x46}};
const IID IID_IEnumRfc1766 = {0x3dc39d1d, 0xc030, 0x11d0, {0xb8, 0x1b, 0x00, 0xc0, 0x4f, 0xc9, 0xb3, 0x1f}};
const IID IID_IEnumScript = {0xae5f1430, 0x388b, 0x11d2, {0x83, 0x80, 0x00, 0xc0, 0x4f, 0x8f, 0x5d, 0xa1}};
const IID IID_IMLangConvertCharset = {0xd66d6f98, 0xcdaa, 0x11d0, {0xb8, 0x22, 0x00, 0xc0, 0x4f, 0xc9, 0xb3, 0x1f}};
const CLSID CLSID_CMLangConvertCharset = {0xd66d6f99, 0xcdaa, 0x11d0, {0xb8, 0x22, 0x00, 0xc0, 0x4f, 0xc9, 0xb3, 0x1f}};
const CLSID CLSID_CMultiLanguage = {0x275c23e2, 0x3747, 0x11d0, {0x9f, 0xea, 0x00, 0xaa, 0x00, 0x3f, 0x86, 0x46}};

static const struct
{
	const CLSID *clsid;
	const WCHAR *name;
	HRESULT (*create)(REFIID, void **);
} classes[] = {
	{&CLSID_CMultiLanguage, L"Multi Language Support", ml_language_create},
	{&CLSID_CMLangConvertCharset, L"Multi Language ConvertCharset", ml_convert_charset_create},
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
		InterlockedIncrement(&ml_locks);
	else
		InterlockedDecrement(&ml_locks);
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
	{{&factory_vtbl}, ml_language_create},
	{{&factory_vtbl}, ml_convert_charset_create},
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
	return ml_locks ? S_FALSE : S_OK;
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
