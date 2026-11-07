/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven shell component: classes registered in the registry, as COM finds them on Windows.
 *
 * A class the activation context in effect declares is served by the library its manifest names. Otherwise a class is looked up
 * under HKEY_CLASSES_ROOT\CLSID\{class}\InprocServer32, whose default value names the library that serves it; the library is loaded
 * and its DllGetClassObject asked for the class factory. A class a Windows service serves as a local server is
 * served the same way. Program identifiers lead to their class through HKEY_CLASSES_ROOT\<identifier>\CLSID, following CurVer when
 * the identifier is version independent. */
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

/* A value of a key under HKEY_CLASSES_ROOT, the default one without a name, with environment variables expanded; FALSE when there is
 * none. */
static BOOL named_value(const WCHAR *key, const WCHAR *name, WCHAR *value, DWORD capacity)
{
	WCHAR raw[MAX_PATH];
	DWORD type = 0, size = sizeof raw;
	HKEY handle;
	if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &handle))
		return FALSE;
	const LONG status = RegQueryValueExW(handle, name, NULL, &type, (BYTE *)raw, &size);
	RegCloseKey(handle);
	if (status || (type != REG_SZ && type != REG_EXPAND_SZ) || size < sizeof(WCHAR))
		return FALSE;
	raw[size / sizeof(WCHAR) - 1] = 0;
	if (type == REG_EXPAND_SZ)
		return ExpandEnvironmentStringsW(raw, value, capacity) != 0;
	lstrcpynW(value, raw, (int)capacity);
	return TRUE;
}

static BOOL class_value(const WCHAR *key, WCHAR *value, DWORD capacity)
{
	return named_value(key, NULL, value, capacity);
}

/* Whether a class belongs to an application that a Windows service runs, as HKEY_CLASSES_ROOT\AppID\{application} names it with its
 * LocalService value. Raven runs no services, and serves their classes in the process of the program that asks for them. */
static BOOL service_class(REFCLSID clsid)
{
	WCHAR key[MAX_PATH], application[64], service[MAX_PATH];
	class_key(clsid, L"", key);
	if (!named_value(key, L"AppID", application, 64))
		return FALSE;
	lstrcpyW(key, L"AppID\\");
	lstrcatW(key, application);
	return named_value(key, L"LocalService", service, MAX_PATH);
}

/* A class nobody registered is named in Raven's trace, so the classes a program misses can be found. */
static HRESULT unregistered(REFCLSID clsid)
{
	static void (WINAPI *raven_trace)(const char *);
	static BOOL looked;
	char line[64];
	if (!looked) {
		raven_trace = (void (WINAPI *)(const char *))GetProcAddress(LoadLibraryW(L"ravenhost.dll"), "RavenTrace");
		looked = TRUE;
	}
	if (raven_trace) {
		wsprintfA(line, "com: no class {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", clsid->Data1, clsid->Data2, clsid->Data3,
			clsid->Data4[0], clsid->Data4[1], clsid->Data4[2], clsid->Data4[3], clsid->Data4[4], clsid->Data4[5], clsid->Data4[6], clsid->Data4[7]);
		raven_trace(line);
	}
	return REGDB_E_CLASSNOTREG;
}

/* The library that serves a class the activation context in effect declares, as a manifest's comClass or clrClass names it: the file
 * of its assembly, or mscoree.dll for managed code, which finds the class in the context again. The entry is the runtime's: its size,
 * its kind, then the offset and length of each of its strings, the library first. */
static BOOL context_library(REFCLSID clsid, WCHAR *library, DWORD capacity)
{
	ACTCTX_SECTION_KEYED_DATA data = {sizeof data};
	if (!FindActCtxSectionGuid(0, NULL, ACTIVATION_CONTEXT_SECTION_COM_SERVER_REDIRECTION, clsid, &data) || data.ulLength < 16)
		return FALSE;
	const ULONG *entry = data.lpData;
	if (entry[2] + entry[3] > data.ulLength || entry[3] / sizeof(WCHAR) >= capacity)
		return FALSE;
	lstrcpynW(library, (const WCHAR *)((const BYTE *)data.lpData + entry[2]), (int)capacity);
	return TRUE;
}

HRESULT raven_registered_class_object(REFCLSID clsid, DWORD context, REFIID iid, void **out)
{
	WCHAR key[MAX_PATH], library[MAX_PATH];
	*out = NULL;
	if (!(context & (CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER)) && !((context & CLSCTX_LOCAL_SERVER) && service_class(clsid)))
		return unregistered(clsid);
	class_key(clsid, L"\\InprocServer32", key);
	/* The context in effect comes before the registry, as Windows looks there first. */
	if (!((context & (CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER)) && context_library(clsid, library, MAX_PATH)) && !class_value(key, library, MAX_PATH))
		return unregistered(clsid);
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

/* A class the registry marks as emulated by another one: HKEY_CLASSES_ROOT\CLSID\{class}\TreatAs. S_FALSE hands back the class itself. */
HRESULT WINAPI CoGetTreatAsClass(REFCLSID original, LPCLSID result)
{
	WCHAR key[MAX_PATH], text[MAX_PATH];
	if (!original || !result)
		return E_INVALIDARG;
	*result = *original;
	class_key(original, L"\\TreatAs", key);
	if (!class_value(key, text, MAX_PATH))
		return S_FALSE;
	if (FAILED(CLSIDFromString(text, result))) {
		*result = *original;
		return REGDB_E_INVALIDVALUE;
	}
	return S_OK;
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

/* A wait of COM: in a single-threaded apartment the thread keeps dispatching the messages that reach it while it waits, as the modal
 * loop of COM does, so a window of the apartment, or a completion posted to it, is served while the thread waits on a task. A quit
 * message seen meanwhile is posted again for the thread's own loop. In the multithreaded apartment it is a plain wait. The result is the
 * index of the handle that ended the wait (or WAIT_IO_COMPLETION after the thread ran its APCs), RPC_S_CALLPENDING when the time runs
 * out. */
HRESULT WINAPI CoWaitForMultipleHandles(DWORD flags, DWORD timeout, ULONG count, LPHANDLE handles, LPDWORD index)
{
	enum { wait_all = 1, alertable = 2, input_available = 4, known_flags = 0x1f };
	APTTYPE type;
	APTTYPEQUALIFIER qualifier;
	if (!index)
		return E_INVALIDARG;
	*index = 0;
	if (!handles || !count || (flags & ~known_flags))
		return E_INVALIDARG;
	const BOOL single = SUCCEEDED(CoGetApartmentType(&type, &qualifier)) && (type == APTTYPE_STA || type == APTTYPE_MAINSTA);
	DWORD result;
	if (!single)
		result = WaitForMultipleObjectsEx(count, handles, (flags & wait_all) != 0, timeout, (flags & alertable) != 0);
	else {
		const DWORD wait_flags = ((flags & wait_all) ? MWMO_WAITALL : 0) | ((flags & alertable) ? MWMO_ALERTABLE : 0) |
			((flags & input_available) ? MWMO_INPUTAVAILABLE : 0);
		const DWORD start = GetTickCount();
		BOOL quit = FALSE;
		WPARAM quit_code = 0;
		for (;;) {
			DWORD remaining = timeout;
			if (timeout != INFINITE) {
				const DWORD spent = GetTickCount() - start;
				remaining = spent >= timeout ? 0 : timeout - spent;
			}
			result = MsgWaitForMultipleObjectsEx(count, handles, remaining, QS_ALLINPUT, wait_flags);
			if (result != WAIT_OBJECT_0 + count)
				break;
			MSG message;
			while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
				if (message.message == WM_QUIT) {
					quit = TRUE;
					quit_code = message.wParam;
				} else
					DispatchMessageW(&message);
			}
			if (quit || (timeout != INFINITE && GetTickCount() - start >= timeout)) {
				result = WaitForMultipleObjectsEx(count, handles, (flags & wait_all) != 0, 0, FALSE);
				if (quit || result != WAIT_TIMEOUT)
					break;
			}
		}
		if (quit)
			PostQuitMessage((int)quit_code);
	}
	if (result == WAIT_TIMEOUT)
		return RPC_S_CALLPENDING;
	if (result == WAIT_FAILED)
		return HRESULT_FROM_WIN32(GetLastError());
	*index = result;
	return S_OK;
}
