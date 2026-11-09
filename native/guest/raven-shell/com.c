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

/* The service that serves a class, as HKEY_CLASSES_ROOT\AppID\{application} names it with its LocalService value, and the arguments
 * COM starts it with, its ServiceParameters. */
static BOOL class_service(REFCLSID clsid, WCHAR *service, WCHAR *parameters, DWORD capacity)
{
	WCHAR key[MAX_PATH], application[64];
	class_key(clsid, L"", key);
	if (!named_value(key, L"AppID", application, 64))
		return FALSE;
	lstrcpyW(key, L"AppID\\");
	lstrcatW(key, application);
	if (!named_value(key, L"LocalService", service, MAX_PATH))
		return FALSE;
	if (parameters && !named_value(key, L"ServiceParameters", parameters, capacity))
		parameters[0] = 0;
	return TRUE;
}

static BOOL service_class(REFCLSID clsid)
{
	WCHAR service[MAX_PATH];
	return class_service(clsid, service, NULL, 0);
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

/* Class objects a program offers with CoRegisterClassObject. Those it offers to other programs (CLSCTX_LOCAL_SERVER) are also
 * published: a file named after the class, in a directory every process of the prefix sees, holds the process that serves it and its
 * class object marshaled for a table, which any number of clients can read. */
typedef struct offered_class {
	struct offered_class *next;
	CLSID clsid;
	IUnknown *object;
	DWORD cookie, context, flags;
	BOOL published, suspended;
} offered_class;

static offered_class *offered;
static SRWLOCK offered_lock = SRWLOCK_INIT;
static DWORD next_cookie = 1;
static LONG server_references;

typedef struct {
	DWORD process, context;
} published_head;

static void published_path(REFCLSID clsid, WCHAR *path)
{
	WCHAR text[40];
	GetWindowsDirectoryW(path, MAX_PATH);
	lstrcatW(path, L"\\Temp");
	CreateDirectoryW(path, NULL);
	lstrcatW(path, L"\\raven-classes");
	CreateDirectoryW(path, NULL);
	StringFromGUID2(clsid, text, 40);
	lstrcatW(path, L"\\");
	lstrcatW(path, text);
}

/* The marshaled class object goes to a file of its own first and replaces the published one in a single step, so a reader never sees half
 * of it. */
static HRESULT publish(offered_class *entry)
{
	WCHAR path[MAX_PATH], staged[MAX_PATH + 16];
	IStream *stream;
	HGLOBAL global;
	DWORD written;
	HRESULT result = CreateStreamOnHGlobal(NULL, TRUE, &stream);
	if (FAILED(result))
		return result;
	result = CoMarshalInterface(stream, &IID_IUnknown, entry->object, MSHCTX_LOCAL, NULL, MSHLFLAGS_TABLESTRONG);
	if (SUCCEEDED(result))
		result = GetHGlobalFromStream(stream, &global);
	if (SUCCEEDED(result)) {
		published_head head = {GetCurrentProcessId(), entry->context};
		const DWORD size = (DWORD)GlobalSize(global);
		published_path(&entry->clsid, path);
		wsprintfW(staged, L"%s.%lu", path, GetCurrentProcessId());
		HANDLE file = CreateFileW(staged, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		BOOL saved = file != INVALID_HANDLE_VALUE && WriteFile(file, &head, sizeof head, &written, NULL) && WriteFile(file, GlobalLock(global), size, &written, NULL);
		GlobalUnlock(global);
		if (file != INVALID_HANDLE_VALUE)
			CloseHandle(file);
		if (!saved || !MoveFileExW(staged, path, MOVEFILE_REPLACE_EXISTING)) {
			DeleteFileW(staged);
			LARGE_INTEGER start = {0};
			IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
			CoReleaseMarshalData(stream);
			result = CO_E_CLASS_CREATE_FAILED;
		} else
			entry->published = TRUE;
	}
	IStream_Release(stream);
	return result;
}

/* The published class object of another process, or nothing. A file its process left behind when it ended does not answer and goes. */
static HRESULT published_object(REFCLSID clsid, REFIID iid, void **out)
{
	WCHAR path[MAX_PATH];
	published_head head;
	BYTE data[1024];
	DWORD got = 0, size = 0;
	IStream *stream;
	published_path(clsid, path);
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return REGDB_E_CLASSNOTREG;
	BOOL read = ReadFile(file, &head, sizeof head, &got, NULL) && got == sizeof head && ReadFile(file, data, sizeof data, &size, NULL) && size;
	CloseHandle(file);
	if (!read || head.process == GetCurrentProcessId() || !(head.context & CLSCTX_LOCAL_SERVER))
		return REGDB_E_CLASSNOTREG;
	HRESULT result = CreateStreamOnHGlobal(NULL, TRUE, &stream);
	if (FAILED(result))
		return result;
	LARGE_INTEGER start = {0};
	IStream_Write(stream, data, size, NULL);
	IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
	result = CoUnmarshalInterface(stream, iid, out);
	IStream_Release(stream);
	if (result == RPC_E_SERVER_DIED || result == RPC_E_DISCONNECTED) {
		HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, head.process);
		if (!process || WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
			DeleteFileW(path);
		if (process)
			CloseHandle(process);
		return REGDB_E_CLASSNOTREG;
	}
	return result;
}

/* A class object this process offers for the contexts asked for. */
static HRESULT offered_object(REFCLSID clsid, DWORD context, REFIID iid, void **out)
{
	HRESULT result = REGDB_E_CLASSNOTREG;
	AcquireSRWLockShared(&offered_lock);
	for (offered_class *entry = offered; entry; entry = entry->next)
		if (IsEqualCLSID(&entry->clsid, clsid) && (entry->context & context) && !entry->suspended) {
			result = IUnknown_QueryInterface(entry->object, iid, out);
			break;
		}
	ReleaseSRWLockShared(&offered_lock);
	return result;
}

HRESULT WINAPI CoRegisterClassObject(REFCLSID clsid, IUnknown *object, DWORD context, DWORD flags, DWORD *cookie)
{
	if (!clsid || !object || !cookie)
		return E_INVALIDARG;
	*cookie = 0;
	offered_class *entry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *entry);
	if (!entry)
		return E_OUTOFMEMORY;
	entry->clsid = *clsid;
	entry->object = object;
	entry->context = context;
	entry->flags = flags;
	entry->suspended = (flags & REGCLS_SUSPENDED) != 0;
	IUnknown_AddRef(object);
	if ((context & CLSCTX_LOCAL_SERVER) && !entry->suspended) {
		const HRESULT result = publish(entry);
		if (FAILED(result)) {
			IUnknown_Release(object);
			HeapFree(GetProcessHeap(), 0, entry);
			return result;
		}
	}
	AcquireSRWLockExclusive(&offered_lock);
	entry->cookie = next_cookie++;
	entry->next = offered;
	offered = entry;
	ReleaseSRWLockExclusive(&offered_lock);
	*cookie = entry->cookie;
	return S_OK;
}

/* The published file goes only while it still names this process: a server started later may have replaced it. */
static void withdraw(offered_class *entry)
{
	WCHAR path[MAX_PATH];
	published_head head;
	DWORD got = 0;
	if (!entry->published)
		return;
	entry->published = FALSE;
	published_path(&entry->clsid, path);
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return;
	const BOOL ours = ReadFile(file, &head, sizeof head, &got, NULL) && got == sizeof head && head.process == GetCurrentProcessId();
	IStream *stream = NULL;
	if (ours && SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &stream))) {
		BYTE data[1024];
		DWORD size = 0;
		if (ReadFile(file, data, sizeof data, &size, NULL) && size) {
			LARGE_INTEGER start = {0};
			IStream_Write(stream, data, size, NULL);
			IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
			CoReleaseMarshalData(stream);
		}
		IStream_Release(stream);
	}
	CloseHandle(file);
	if (ours)
		DeleteFileW(path);
}

HRESULT WINAPI CoRevokeClassObject(DWORD cookie)
{
	offered_class *entry = NULL;
	AcquireSRWLockExclusive(&offered_lock);
	for (offered_class **link = &offered; *link; link = &(*link)->next)
		if ((*link)->cookie == cookie) {
			entry = *link;
			*link = entry->next;
			break;
		}
	ReleaseSRWLockExclusive(&offered_lock);
	if (!entry)
		return E_INVALIDARG;
	withdraw(entry);
	IUnknown_Release(entry->object);
	HeapFree(GetProcessHeap(), 0, entry);
	return S_OK;
}

/* Classes registered with REGCLS_SUSPENDED become visible together, as a server offers all its classes at once. */
HRESULT WINAPI CoResumeClassObjects(void)
{
	HRESULT result = S_OK;
	AcquireSRWLockExclusive(&offered_lock);
	for (offered_class *entry = offered; entry; entry = entry->next)
		if (entry->suspended) {
			entry->suspended = FALSE;
			if ((entry->context & CLSCTX_LOCAL_SERVER) && FAILED(publish(entry)))
				result = CO_E_CLASS_CREATE_FAILED;
		}
	ReleaseSRWLockExclusive(&offered_lock);
	return result;
}

HRESULT WINAPI CoSuspendClassObjects(void)
{
	AcquireSRWLockExclusive(&offered_lock);
	for (offered_class *entry = offered; entry; entry = entry->next)
		if (!entry->suspended) {
			entry->suspended = TRUE;
			withdraw(entry);
		}
	ReleaseSRWLockExclusive(&offered_lock);
	return S_OK;
}

/* A server counts the objects it keeps alive; when the count drops to zero it takes no more activations, as its classes are withdrawn
 * then. */
ULONG WINAPI CoAddRefServerProcess(void)
{
	return (ULONG)InterlockedIncrement(&server_references);
}

ULONG WINAPI CoReleaseServerProcess(void)
{
	const LONG left = InterlockedDecrement(&server_references);
	if (!left)
		CoSuspendClassObjects();
	return (ULONG)(left < 0 ? 0 : left);
}

/* The command that starts a class's local server: its LocalServer32 value, which may quote the program and add arguments, followed by
 * -Embedding as COM starts a server for an activation. */
static BOOL server_command(REFCLSID clsid, WCHAR *command, DWORD capacity)
{
	WCHAR key[MAX_PATH];
	class_key(clsid, L"\\LocalServer32", key);
	if (!class_value(key, command, capacity - 16) || !command[0])
		return FALSE;
	lstrcatW(command, L" -Embedding");
	return TRUE;
}

/* A service started for a class, with the class's ServiceParameters split into its arguments; one already running is left as it is.
 * The wait for its class ends when the service stops. */
static HRESULT service_object(REFCLSID clsid, const WCHAR *name, WCHAR *parameters, REFIID iid, void **out)
{
	const WCHAR *arguments[32];
	DWORD count = 0;
	for (WCHAR *at = parameters; *at && count < ARRAYSIZE(arguments);) {
		while (*at == L' ' || *at == L'\t')
			at++;
		if (!*at)
			break;
		const BOOL quoted = *at == L'"';
		if (quoted)
			at++;
		arguments[count++] = at;
		while (*at && (quoted ? *at != L'"' : *at != L' ' && *at != L'\t'))
			at++;
		if (*at)
			*at++ = 0;
	}
	SC_HANDLE manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	SC_HANDLE service = manager ? OpenServiceW(manager, name, SERVICE_START | SERVICE_QUERY_STATUS) : NULL;
	HRESULT result = CO_E_SERVER_EXEC_FAILURE;
	if (service && (StartServiceW(service, count, count ? arguments : NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING))
		for (DWORD waited = 0; waited < 120000; waited += 20) {
			const HRESULT found = published_object(clsid, iid, out);
			if (found != REGDB_E_CLASSNOTREG) {
				result = found;
				break;
			}
			SERVICE_STATUS status;
			if (waited > 1000 && QueryServiceStatus(service, &status) && status.dwCurrentState == SERVICE_STOPPED)
				break;
			Sleep(20);
		}
	if (service)
		CloseServiceHandle(service);
	if (manager)
		CloseServiceHandle(manager);
	return result;
}

/* The class object of a local server, started when no process offers it yet. COM waits for the server to register its classes; a server
 * that ends before that failed to start. */
static HRESULT local_server_object(REFCLSID clsid, REFIID iid, void **out)
{
	WCHAR command[MAX_PATH * 2], service[MAX_PATH];
	HRESULT result = published_object(clsid, iid, out);
	if (result != REGDB_E_CLASSNOTREG)
		return result;
	if (class_service(clsid, service, command, ARRAYSIZE(command)))
		return service_object(clsid, service, command, iid, out);
	if (!server_command(clsid, command, ARRAYSIZE(command)))
		return REGDB_E_CLASSNOTREG;
	STARTUPINFOW startup = {sizeof startup};
	PROCESS_INFORMATION process;
	if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process))
		return CO_E_SERVER_EXEC_FAILURE;
	CloseHandle(process.hThread);
	result = CO_E_SERVER_EXEC_FAILURE;
	for (DWORD waited = 0; waited < 120000; waited += 20) {
		const HRESULT found = published_object(clsid, iid, out);
		if (found != REGDB_E_CLASSNOTREG) {
			result = found;
			break;
		}
		if (WaitForSingleObject(process.hProcess, 20) == WAIT_OBJECT_0)
			break;
	}
	CloseHandle(process.hProcess);
	return result;
}

HRESULT raven_registered_class_object(REFCLSID clsid, DWORD context, REFIID iid, void **out)
{
	WCHAR key[MAX_PATH], library[MAX_PATH];
	*out = NULL;
	/* A class this process offers itself comes first, as COM looks at the classes registered in the process before the registry. */
	if (SUCCEEDED(offered_object(clsid, context, iid, out)))
		return S_OK;
	const BOOL in_process = (context & (CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER)) != 0;
	class_key(clsid, L"\\InprocServer32", key);
	/* The context in effect comes before the registry, as Windows looks there first. A class of a service that also names a library
	 * is served from that library in the caller's process. */
	if (!(in_process && context_library(clsid, library, MAX_PATH)) &&
		!((in_process || ((context & CLSCTX_LOCAL_SERVER) && service_class(clsid))) && class_value(key, library, MAX_PATH))) {
		if (context & CLSCTX_LOCAL_SERVER) {
			const HRESULT result = local_server_object(clsid, iid, out);
			if (result != REGDB_E_CLASSNOTREG)
				return result;
		}
		return unregistered(clsid);
	}
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
