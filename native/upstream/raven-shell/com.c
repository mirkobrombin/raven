/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven shell component: classes registered in the registry, as COM finds them on Windows.
 *
 * A class is looked up under HKEY_CLASSES_ROOT\CLSID\{class}\InprocServer32, whose default value names the library that serves it;
 * the library is loaded and its DllGetClassObject asked for the class factory. Program identifiers lead to their class through
 * HKEY_CLASSES_ROOT\<identifier>\CLSID, following CurVer when the identifier is version independent. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <ctxtcall.h>

typedef HRESULT (WINAPI *class_object_function)(REFCLSID, REFIID, void **);

/* The key of a class: "CLSID\{XXXXXXXX-...}" followed by a subkey. */
static void class_key(REFCLSID clsid, const WCHAR *subkey, WCHAR *key)
{
	WCHAR text[40];
	StringFromGUID2(clsid, text, 40);
	lstrcpyW(key, L"CLSID\\");
	lstrcatW(key, text);
	lstrcatW(key, subkey);
}

/* The default value of a key under HKEY_CLASSES_ROOT, with environment variables expanded; FALSE when there is none. */
static BOOL class_value(const WCHAR *key, WCHAR *value, DWORD capacity)
{
	WCHAR raw[MAX_PATH];
	DWORD type = 0, size = sizeof raw;
	HKEY handle;
	if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &handle))
		return FALSE;
	const LONG status = RegQueryValueExW(handle, NULL, NULL, &type, (BYTE *)raw, &size);
	RegCloseKey(handle);
	if (status || (type != REG_SZ && type != REG_EXPAND_SZ) || size < sizeof(WCHAR))
		return FALSE;
	raw[size / sizeof(WCHAR) - 1] = 0;
	if (type == REG_EXPAND_SZ)
		return ExpandEnvironmentStringsW(raw, value, capacity) != 0;
	lstrcpynW(value, raw, (int)capacity);
	return TRUE;
}

HRESULT raven_registered_class_object(REFCLSID clsid, DWORD context, REFIID iid, void **out)
{
	WCHAR key[MAX_PATH], library[MAX_PATH];
	*out = NULL;
	if (!(context & (CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER)))
		return REGDB_E_CLASSNOTREG;
	class_key(clsid, L"\\InprocServer32", key);
	if (!class_value(key, library, MAX_PATH))
		return REGDB_E_CLASSNOTREG;
	/* The library stays loaded for the objects it makes, as COM keeps it until CoFreeUnusedLibraries. */
	HMODULE module = LoadLibraryExW(library, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!module)
		return CO_E_DLLNOTFOUND;
	class_object_function get = (class_object_function)GetProcAddress(module, "DllGetClassObject");
	if (!get)
		return CO_E_ERRORINDLL;
	return get(clsid, iid, out);
}

HRESULT raven_create_registered(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, void **out)
{
	IClassFactory *factory;
	HRESULT result = raven_registered_class_object(clsid, context, &IID_IClassFactory, (void **)&factory);
	if (FAILED(result))
		return result;
	result = IClassFactory_CreateInstance(factory, outer, iid, out);
	IClassFactory_Release(factory);
	return result;
}

HRESULT WINAPI CLSIDFromProgID(LPCOLESTR identifier, LPCLSID clsid)
{
	WCHAR key[MAX_PATH], text[MAX_PATH];
	if (!identifier || !clsid)
		return E_INVALIDARG;
	if (lstrlenW(identifier) > MAX_PATH - 16) {
		ZeroMemory(clsid, sizeof *clsid);
		return CO_E_CLASSSTRING;
	}
	/* A version independent identifier names its current version, whose class is the one asked for. */
	lstrcpyW(key, identifier);
	lstrcatW(key, L"\\CurVer");
	if (class_value(key, text, MAX_PATH) && lstrlenW(text) < MAX_PATH - 16)
		lstrcpyW(key, text);
	else
		lstrcpyW(key, identifier);
	lstrcatW(key, L"\\CLSID");
	if (!class_value(key, text, MAX_PATH) || FAILED(CLSIDFromString(text, clsid))) {
		ZeroMemory(clsid, sizeof *clsid);
		return CO_E_CLASSSTRING;
	}
	return S_OK;
}

/* The object context of an apartment, whose pointer is also its token: a runtime compares tokens to know whether two calls come from the
 * same context and asks the object to run a call in it. Every call here already runs in its own context, so a callback runs at once.
 * The multithreaded apartment is one context for all its threads; each single-threaded apartment has its own, kept for the process. */
typedef struct object_context {
	IContextCallback callback;
	DWORD thread;
	struct object_context *next;
} object_context;

static HRESULT WINAPI context_query(IContextCallback *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (InlineIsEqualGUID(iid, &IID_IUnknown) || InlineIsEqualGUID(iid, &IID_IContextCallback)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI context_reference(IContextCallback *self) { return 1; }

static HRESULT WINAPI context_call(IContextCallback *self, PFNCONTEXTCALL callback, ComCallData *data, REFIID iid, int method,
	IUnknown *reserved)
{
	return callback ? callback(data) : E_INVALIDARG;
}

static IContextCallbackVtbl context_table = {context_query, context_reference, context_reference, context_call};
static object_context multithreaded_context = {{&context_table}, 0, NULL};
static object_context *volatile apartment_contexts;

/* The caller's context, or CO_E_NOTINITIALIZED for a thread in no apartment. */
static HRESULT current_context(object_context **out)
{
	APTTYPE type;
	APTTYPEQUALIFIER qualifier;
	const HRESULT result = CoGetApartmentType(&type, &qualifier);
	if (FAILED(result))
		return result;
	if (type == APTTYPE_MTA) {
		*out = &multithreaded_context;
		return S_OK;
	}
	const DWORD thread = GetCurrentThreadId();
	for (object_context *context = apartment_contexts; context; context = context->next)
		if (context->thread == thread) {
			*out = context;
			return S_OK;
		}
	object_context *context = HeapAlloc(GetProcessHeap(), 0, sizeof *context);
	if (!context)
		return E_OUTOFMEMORY;
	context->callback.lpVtbl = &context_table;
	context->thread = thread;
	do
		context->next = apartment_contexts;
	while (InterlockedCompareExchangePointer((void *volatile *)&apartment_contexts, context, context->next) != context->next);
	*out = context;
	return S_OK;
}

HRESULT WINAPI CoGetContextToken(ULONG_PTR *token)
{
	object_context *context;
	if (!token)
		return E_POINTER;
	const HRESULT result = current_context(&context);
	if (SUCCEEDED(result))
		*token = (ULONG_PTR)context;
	return result;
}

HRESULT WINAPI CoGetObjectContext(REFIID iid, void **out)
{
	object_context *context;
	if (!out)
		return E_POINTER;
	*out = NULL;
	const HRESULT result = current_context(&context);
	return FAILED(result) ? result : context_query(&context->callback, iid, out);
}
