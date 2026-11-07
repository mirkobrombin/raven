/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "urlmon_private.h"
#include <shellapi.h>

const CLSID CLSID_CUri = {0xdf2fce13, 0x25ec, 0x45bb, {0x9d, 0x4c, 0xce, 0xcd, 0x47, 0xc2, 0x43, 0x0c}};

const IID IID_IAsyncBindCtx = {0x79eac9d4, 0xbaf9, 0x11ce, {0x8c, 0x82, 0x00, 0xaa, 0x00, 0x4b, 0xa9, 0x0b}};

HINSTANCE urlmon_instance;
static LONG module_locks;

void urlmon_lock_module(void)
{
	InterlockedIncrement(&module_locks);
}

void urlmon_unlock_module(void)
{
	InterlockedDecrement(&module_locks);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		urlmon_instance = instance;
		DisableThreadLibraryCalls(instance);
		urlmon_session_startup();
	}
	else if (reason == DLL_PROCESS_DETACH && !reserved)
		urlmon_session_shutdown();
	return TRUE;
}

typedef HRESULT (*creator)(IUnknown *outer, REFIID iid, void **object);

typedef struct
{
	IClassFactory IClassFactory_iface;
	const CLSID *clsid;
	creator create;
	const WCHAR *name;
	const WCHAR *scheme;
	BOOL can_aggregate;
} class_entry;

static inline class_entry *from_factory(IClassFactory *iface)
{
	return CONTAINING_RECORD(iface, class_entry, IClassFactory_iface);
}

static HRESULT WINAPI factory_query_interface(IClassFactory *iface, REFIID iid, void **object)
{
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IClassFactory))
	{
		*object = iface;
		IClassFactory_AddRef(iface);
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_add_ref(IClassFactory *iface)
{
	urlmon_lock_module();
	return 2;
}

static ULONG WINAPI factory_release(IClassFactory *iface)
{
	urlmon_unlock_module();
	return 1;
}

static HRESULT WINAPI factory_create_instance(IClassFactory *iface, IUnknown *outer, REFIID iid, void **object)
{
	class_entry *self = from_factory(iface);
	if (!object)
		return E_POINTER;
	*object = NULL;
	if (outer && !self->can_aggregate)
		return CLASS_E_NOAGGREGATION;
	return self->create(outer, iid, object);
}

static HRESULT WINAPI factory_lock_server(IClassFactory *iface, BOOL lock)
{
	if (lock)
		urlmon_lock_module();
	else
		urlmon_unlock_module();
	return S_OK;
}

static const IClassFactoryVtbl factory_vtable = {factory_query_interface, factory_add_ref, factory_release, factory_create_instance, factory_lock_server};

static HRESULT create_empty_uri(IUnknown *outer, REFIID iid, void **object)
{
	return urlmon_create_empty_uri(iid, object);
}

static class_entry classes[] = {
	{{&factory_vtable}, &CLSID_FileProtocol, urlmon_create_file_protocol, L"file: Asynchronous Pluggable Protocol Handler", L"file", TRUE},
	{{&factory_vtable}, &CLSID_FtpProtocol, urlmon_create_ftp_protocol, L"ftp: Asynchronous Pluggable Protocol Handler", L"ftp", TRUE},
	{{&factory_vtable}, &CLSID_HttpProtocol, urlmon_create_http_protocol, L"http: Asynchronous Pluggable Protocol Handler", L"http", TRUE},
	{{&factory_vtable}, &CLSID_HttpSProtocol, urlmon_create_https_protocol, L"https: Asynchronous Pluggable Protocol Handler", L"https", TRUE},
	{{&factory_vtable}, &CLSID_MkProtocol, urlmon_create_mk_protocol, L"mk: Asynchronous Pluggable Protocol Handler", L"mk", TRUE},
	{{&factory_vtable}, &CLSID_StdURLMoniker, urlmon_create_moniker_object, L"URL Moniker", NULL, FALSE},
	{{&factory_vtable}, &CLSID_InternetSecurityManager, urlmon_create_security_manager, L"Security Manager", NULL, FALSE},
	{{&factory_vtable}, &CLSID_InternetZoneManager, urlmon_create_zone_manager, L"URL Zone Manager", NULL, FALSE},
	{{&factory_vtable}, &CLSID_CUri, create_empty_uri, L"CUri", NULL, FALSE},
};

BOOL urlmon_builtin_protocol(const WCHAR *scheme, IClassFactory **factory, CLSID *clsid)
{
	DWORD index;
	for (index = 0; index < ARRAYSIZE(classes); index++)
		if (classes[index].scheme && !lstrcmpiW(classes[index].scheme, scheme))
		{
			*factory = &classes[index].IClassFactory_iface;
			IClassFactory_AddRef(*factory);
			if (clsid)
				*clsid = *classes[index].clsid;
			return TRUE;
		}
	return FALSE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **object)
{
	DWORD index;
	for (index = 0; index < ARRAYSIZE(classes); index++)
		if (IsEqualGUID(clsid, classes[index].clsid))
			return IClassFactory_QueryInterface(&classes[index].IClassFactory_iface, iid, object);
	*object = NULL;
	return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return module_locks ? S_FALSE : S_OK;
}

static LONG set_text(HKEY key, const WCHAR *name, const WCHAR *value)
{
	return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (DWORD)(wcslen(value) + 1) * sizeof(WCHAR));
}

static HRESULT register_classes(void)
{
	WCHAR module[MAX_PATH], guid[40], path[96];
	DWORD index;
	HKEY key, inproc, handler;
	GetModuleFileNameW(urlmon_instance, module, ARRAYSIZE(module));
	for (index = 0; index < ARRAYSIZE(classes); index++)
	{
		StringFromGUID2(classes[index].clsid, guid, ARRAYSIZE(guid));
		lstrcpyW(path, L"CLSID\\");
		lstrcatW(path, guid);
		if (RegCreateKeyExW(HKEY_CLASSES_ROOT, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL))
			return E_FAIL;
		set_text(key, NULL, classes[index].name);
		if (!RegCreateKeyExW(key, L"InProcServer32", 0, NULL, 0, KEY_WRITE, NULL, &inproc, NULL))
		{
			set_text(inproc, NULL, module);
			set_text(inproc, L"ThreadingModel", L"Apartment");
			RegCloseKey(inproc);
		}
		RegCloseKey(key);
		if (!classes[index].scheme)
			continue;
		lstrcpyW(path, L"PROTOCOLS\\Handler\\");
		lstrcatW(path, classes[index].scheme);
		if (!RegCreateKeyExW(HKEY_CLASSES_ROOT, path, 0, NULL, 0, KEY_WRITE, NULL, &handler, NULL))
		{
			set_text(handler, L"CLSID", guid);
			RegCloseKey(handler);
		}
	}
	return S_OK;
}

HRESULT WINAPI DllRegisterServer(void)
{
	return register_classes();
}

HRESULT WINAPI DllRegisterServerEx(void)
{
	return register_classes();
}

static BOOL delete_key_tree(HKEY parent, const WCHAR *name)
{
	WCHAR child[256];
	HKEY key;
	DWORD length = ARRAYSIZE(child);
	BOOL deleted;
	if (RegOpenKeyExW(parent, name, 0, KEY_READ | KEY_WRITE, &key))
		return FALSE;
	while (!RegEnumKeyExW(key, 0, child, &length, NULL, NULL, NULL, NULL) && delete_key_tree(key, child))
		length = ARRAYSIZE(child);
	RegCloseKey(key);
	deleted = !RegDeleteKeyW(parent, name);
	return deleted;
}

HRESULT WINAPI DllUnregisterServer(void)
{
	WCHAR guid[40], path[96];
	DWORD index;
	for (index = 0; index < ARRAYSIZE(classes); index++)
	{
		StringFromGUID2(classes[index].clsid, guid, ARRAYSIZE(guid));
		lstrcpyW(path, L"CLSID\\");
		lstrcatW(path, guid);
		delete_key_tree(HKEY_CLASSES_ROOT, path);
		if (classes[index].scheme)
		{
			lstrcpyW(path, L"PROTOCOLS\\Handler\\");
			lstrcatW(path, classes[index].scheme);
			delete_key_tree(HKEY_CLASSES_ROOT, path);
		}
	}
	return S_OK;
}

HRESULT WINAPI DllInstall(BOOL install, LPCWSTR line)
{
	return S_OK;
}

HRESULT WINAPI FaultInIEFeature(HWND window, uCLSSPEC *spec, QUERYCONTEXT *query, DWORD flags)
{
	return S_OK;
}

HRESULT WINAPI RegisterMediaTypes(UINT count, const LPCSTR *names, CLIPFORMAT *formats)
{
	UINT index;
	if (!names || !formats)
		return E_INVALIDARG;
	for (index = 0; index < count; index++)
		formats[index] = (CLIPFORMAT)RegisterClipboardFormatA(names[index]);
	return S_OK;
}

/* Sending a component off the net to be installed is not something a program here can ask for. */
HRESULT WINAPI AsyncInstallDistributionUnit(LPCWSTR type, LPCWSTR guid, LPCWSTR ext, DWORD ms, DWORD ls, LPCWSTR url, IBindCtx *context, void *reserved, DWORD flags)
{
	return E_NOTIMPL;
}

HRESULT WINAPI CoGetClassObjectFromURL(REFCLSID clsid, LPCWSTR base, DWORD ms, DWORD ls, LPCWSTR type, LPBINDCTX context, DWORD execution, void *reserved, REFIID iid, void **object)
{
	if (!object)
		return E_INVALIDARG;
	*object = NULL;
	return CoGetClassObject(clsid, execution, NULL, iid, object);
}

HRESULT WINAPI Extract(void *fdi, LPCSTR file)
{
	HMODULE module = LoadLibraryW(L"cabinet.dll");
	typedef HRESULT (WINAPI *extract_call)(void *, LPCSTR);
	extract_call call;
	HRESULT result = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
	if (!module)
		return HRESULT_FROM_WIN32(GetLastError());
	call = (extract_call)GetProcAddress(module, "Extract");
	if (call)
		result = call(fdi, file);
	FreeLibrary(module);
	return result;
}

HRESULT WINAPI GetSoftwareUpdateInfo(LPCWSTR path, LPSOFTDISTINFO info)
{
	return E_INVALIDARG;
}

BOOL urlmon_shell_execute(SHELLEXECUTEINFOW *info)
{
	return ShellExecuteExW(info);
}

/* Everything the DLL has to name but nothing is known to call. */
HRESULT urlmon_missing(void)
{
	return E_NOTIMPL;
}
