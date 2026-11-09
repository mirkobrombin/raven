#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

/* An interface known only from a type library crosses to a local server through that description. The client writes the library,
   registers it and asks a server it starts for an object; it calls the object with strings, variants, safe arrays and a callback
   object of its own, which the server calls back while the client waits. The server sees each call's context. */

#ifdef _WIN64
#define WIDTH 0x64
#define SYSTEM SYS_WIN64
#else
#define WIDTH 0x32
#define SYSTEM SYS_WIN32
#endif

static const GUID test_library = {0x6a2e0d10, 0x1c3b, 0x4f6e, {0x8d, 0x21, 0x5b, 0x3c, 0x7a, 0x90, 0x11, WIDTH}};
static const IID test_interface = {0x6a2e0d11, 0x1c3b, 0x4f6e, {0x8d, 0x21, 0x5b, 0x3c, 0x7a, 0x90, 0x11, WIDTH}};
static const CLSID test_class = {0x6a2e0d12, 0x1c3b, 0x4f6e, {0x8d, 0x21, 0x5b, 0x3c, 0x7a, 0x90, 0x11, WIDTH}};

int memcmp(const void *left, const void *right, size_t count)
{
	const unsigned char *a = left, *b = right;
	for (size_t i = 0; i < count; i++)
		if (a[i] != b[i])
			return a[i] - b[i];
	return 0;
}

void *memset(void *target, int value, size_t count)
{
	unsigned char *bytes = target;
	for (size_t i = 0; i < count; i++)
		bytes[i] = (unsigned char)value;
	return target;
}

typedef struct test_object test_object;
typedef struct {
	HRESULT (WINAPI *query)(test_object *, REFIID, void **);
	ULONG (WINAPI *addref)(test_object *);
	ULONG (WINAPI *release)(test_object *);
	HRESULT (WINAPI *echo)(test_object *, BSTR, LONG, BSTR *);
	HRESULT (WINAPI *twice)(test_object *, VARIANT, VARIANT *);
	HRESULT (WINAPI *visit)(test_object *, IUnknown *, LONG *);
	HRESULT (WINAPI *process)(test_object *, LONG *);
	HRESULT (WINAPI *strings)(test_object *, SAFEARRAY *, LONG *);
} test_table;
struct test_object {
	const test_table *table;
	LONG references;
	BOOL serving;
};

static HRESULT WINAPI object_query(test_object *self, REFIID iid, void **out)
{
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &test_interface)) {
		*out = self;
		InterlockedIncrement(&self->references);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI object_addref(test_object *self)
{
	return InterlockedIncrement(&self->references);
}

/* The server's object gone, the server has nothing left to serve. */
static ULONG WINAPI object_release(test_object *self)
{
	const LONG left = InterlockedDecrement(&self->references);
	if (!left && self->serving)
		PostQuitMessage(0);
	return left;
}

static HRESULT WINAPI object_echo(test_object *self, BSTR text, LONG number, BSTR *result)
{
	WCHAR line[128];
	(void)self;
	wsprintfW(line, L"%s%ld", text ? text : L"(null)", number);
	*result = SysAllocString(line);
	return *result ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI object_twice(test_object *self, VARIANT value, VARIANT *doubled)
{
	(void)self;
	VariantInit(doubled);
	if (V_VT(&value) == VT_I4) {
		V_VT(doubled) = VT_I4;
		V_I4(doubled) = V_I4(&value) * 2;
		return S_OK;
	}
	if (V_VT(&value) == VT_BSTR) {
		WCHAR line[128];
		wsprintfW(line, L"%s%s", V_BSTR(&value), V_BSTR(&value));
		V_VT(doubled) = VT_BSTR;
		V_BSTR(doubled) = SysAllocString(line);
		return S_OK;
	}
	return DISP_E_TYPEMISMATCH;
}

/* The visitor is the client's own object: calling it runs code in the client while the client waits for this call. */
static HRESULT WINAPI object_visit(test_object *self, IUnknown *visitor, LONG *answer)
{
	test_object *other = NULL;
	BSTR text = SysAllocString(L"ping"), result = NULL;
	(void)self;
	*answer = -1;
	HRESULT hr = IUnknown_QueryInterface(visitor, &test_interface, (void **)&other);
	if (SUCCEEDED(hr)) {
		hr = other->table->echo(other, text, 1, &result);
		if (SUCCEEDED(hr))
			*answer = (LONG)SysStringLen(result) * (lstrcmpW(result, L"ping1") ? -1 : 1);
		other->table->release(other);
	}
	SysFreeString(text);
	SysFreeString(result);
	return hr;
}

/* A call from another process carries its context, as servers that check their callers ask for it. */
static HRESULT WINAPI object_process(test_object *self, LONG *process)
{
	IServerSecurity *security = NULL;
	DWORD level = 0;
	(void)self;
	*process = 0;
	if (FAILED(CoGetCallContext(&IID_IServerSecurity, (void **)&security)) || !security)
		return E_ACCESSDENIED;
	const HRESULT hr = IServerSecurity_QueryBlanket(security, NULL, NULL, NULL, &level, NULL, NULL, NULL);
	IServerSecurity_Release(security);
	if (FAILED(hr) || !level || FAILED(CoImpersonateClient()) || FAILED(CoRevertToSelf()))
		return E_ACCESSDENIED;
	*process = (LONG)GetCurrentProcessId();
	return S_OK;
}

static HRESULT WINAPI object_strings(test_object *self, SAFEARRAY *list, LONG *count)
{
	LONG low, high;
	(void)self;
	*count = 0;
	if (!list || FAILED(SafeArrayGetLBound(list, 1, &low)) || FAILED(SafeArrayGetUBound(list, 1, &high)))
		return E_INVALIDARG;
	for (LONG index = low; index <= high; index++) {
		BSTR text;
		if (SUCCEEDED(SafeArrayGetElement(list, &index, &text))) {
			*count += (LONG)SysStringLen(text);
			SysFreeString(text);
		}
	}
	return S_OK;
}

static const test_table object_table = {object_query, object_addref, object_release, object_echo, object_twice, object_visit, object_process,
	object_strings};

static test_object served = {&object_table, 0, TRUE};
static test_object visitor = {&object_table, 1, FALSE};

static HRESULT WINAPI factory_query(IClassFactory *iface, REFIID iid, void **out)
{
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IClassFactory)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_addref(IClassFactory *iface)
{
	(void)iface;
	return 2;
}

static ULONG WINAPI factory_release(IClassFactory *iface)
{
	(void)iface;
	return 1;
}

static HRESULT WINAPI factory_create(IClassFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
	(void)iface, (void)outer;
	return object_query(&served, iid, out);
}

static HRESULT WINAPI factory_lock(IClassFactory *iface, BOOL locking)
{
	(void)iface, (void)locking;
	return S_OK;
}

static const IClassFactoryVtbl factory_table = {factory_query, factory_addref, factory_release, factory_create, factory_lock};
static IClassFactory factory = {(IClassFactoryVtbl *)&factory_table};

static int serve(void)
{
	DWORD cookie;
	if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
		return 50;
	if (FAILED(CoRegisterClassObject(&test_class, (IUnknown *)&factory, CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &cookie)))
		return 51;
	const DWORD start = GetTickCount();
	MSG message;
	for (;;) {
		if (GetTickCount() - start > 30000)
			return 52;
		if (MsgWaitForMultipleObjects(0, NULL, FALSE, 1000, QS_ALLINPUT) != WAIT_OBJECT_0)
			continue;
		if (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
			if (message.message == WM_QUIT)
				break;
			DispatchMessageW(&message);
		}
	}
	CoRevokeClassObject(cookie);
	CoUninitialize();
	return 0;
}

/* The description: IRavenTest, derived from IUnknown, with its five methods. */
static HRESULT add_method(ICreateTypeInfo *info, UINT index, const WCHAR *name, const VARTYPE *types, const USHORT *flags, const WCHAR **names,
	UINT count, ITypeInfo *unknown)
{
	ELEMDESC parameters[4];
	TYPEDESC pointed[4], array[4];
	FUNCDESC desc = {0};
	(void)unknown;
	for (UINT at = 0; at < count; at++) {
		ZeroMemory(&parameters[at], sizeof parameters[at]);
		VARTYPE vt = types[at];
		TYPEDESC *desc_at = &parameters[at].tdesc;
		if (vt & VT_BYREF) {
			desc_at->vt = VT_PTR;
			desc_at->lptdesc = &pointed[at];
			desc_at = &pointed[at];
			vt &= ~VT_BYREF;
		}
		if (vt & VT_ARRAY) {
			desc_at->vt = VT_SAFEARRAY;
			desc_at->lptdesc = &array[at];
			array[at].vt = vt & ~VT_ARRAY;
		} else
			desc_at->vt = vt;
		parameters[at].paramdesc.wParamFlags = flags[at];
	}
	desc.memid = (MEMBERID)(index + 1);
	desc.funckind = FUNC_PUREVIRTUAL;
	desc.invkind = INVOKE_FUNC;
	desc.callconv = CC_STDCALL;
	desc.cParams = (SHORT)count;
	desc.lprgelemdescParam = parameters;
	desc.elemdescFunc.tdesc.vt = VT_HRESULT;
	HRESULT hr = ICreateTypeInfo_AddFuncDesc(info, index, &desc);
	if (SUCCEEDED(hr)) {
		const WCHAR *all[5] = {name};
		for (UINT at = 0; at < count; at++)
			all[at + 1] = names[at];
		hr = ICreateTypeInfo_SetFuncAndParamNames(info, index, (LPOLESTR *)all, count + 1);
	}
	return hr;
}

static int describe(const WCHAR *path)
{
	ICreateTypeLib2 *library;
	ICreateTypeInfo *info;
	ITypeLib *standard, *made;
	ITypeInfo *unknown;
	HREFTYPE reference;
	if (FAILED(CreateTypeLib2(SYSTEM, path, &library)))
		return 20;
	ICreateTypeLib2_SetGuid(library, &test_library);
	ICreateTypeLib2_SetVersion(library, 1, 0);
	ICreateTypeLib2_SetName(library, L"RavenTypelibMarshal");
	if (FAILED(ICreateTypeLib2_CreateTypeInfo(library, L"IRavenTest", TKIND_INTERFACE, &info)))
		return 21;
	ICreateTypeInfo_SetGuid(info, &test_interface);
	ICreateTypeInfo_SetTypeFlags(info, TYPEFLAG_FOLEAUTOMATION);
	if (FAILED(LoadTypeLib(L"stdole2.tlb", &standard)) || FAILED(ITypeLib_GetTypeInfoOfGuid(standard, &IID_IUnknown, &unknown)))
		return 22;
	if (FAILED(ICreateTypeInfo_AddRefTypeInfo(info, unknown, &reference)) || FAILED(ICreateTypeInfo_AddImplType(info, 0, reference)))
		return 23;
	static const VARTYPE echo_types[] = {VT_BSTR, VT_I4, VT_BSTR | VT_BYREF}, twice_types[] = {VT_VARIANT, VT_VARIANT | VT_BYREF},
		visit_types[] = {VT_UNKNOWN, VT_I4 | VT_BYREF}, process_types[] = {VT_I4 | VT_BYREF}, strings_types[] = {VT_BSTR | VT_ARRAY, VT_I4 | VT_BYREF};
	static const USHORT echo_flags[] = {PARAMFLAG_FIN, PARAMFLAG_FIN, PARAMFLAG_FOUT | PARAMFLAG_FRETVAL}, twice_flags[] = {PARAMFLAG_FIN, PARAMFLAG_FOUT},
		visit_flags[] = {PARAMFLAG_FIN, PARAMFLAG_FOUT | PARAMFLAG_FRETVAL}, process_flags[] = {PARAMFLAG_FOUT | PARAMFLAG_FRETVAL},
		strings_flags[] = {PARAMFLAG_FIN, PARAMFLAG_FOUT | PARAMFLAG_FRETVAL};
	static const WCHAR *echo_names[] = {L"text", L"number", L"result"}, *twice_names[] = {L"value", L"doubled"}, *visit_names[] = {L"visitor", L"answer"},
		*process_names[] = {L"process"}, *strings_names[] = {L"list", L"count"};
	if (FAILED(add_method(info, 0, L"Echo", echo_types, echo_flags, echo_names, 3, unknown)) ||
		FAILED(add_method(info, 1, L"Twice", twice_types, twice_flags, twice_names, 2, unknown)) ||
		FAILED(add_method(info, 2, L"Visit", visit_types, visit_flags, visit_names, 2, unknown)) ||
		FAILED(add_method(info, 3, L"Process", process_types, process_flags, process_names, 1, unknown)) ||
		FAILED(add_method(info, 4, L"Strings", strings_types, strings_flags, strings_names, 2, unknown)))
		return 24;
	if (FAILED(ICreateTypeInfo_LayOut(info)))
		return 25;
	ICreateTypeInfo_Release(info);
	if (FAILED(ICreateTypeLib2_SaveAllChanges(library)))
		return 26;
	ICreateTypeLib2_Release(library);
	if (FAILED(LoadTypeLibEx(path, REGKIND_NONE, &made)) || FAILED(RegisterTypeLib(made, (LPOLESTR)path, NULL)))
		return 27;
	ITypeLib_Release(made);
	return 0;
}

int raven_entry(void)
{
	const WCHAR *line = GetCommandLineW();
	for (const WCHAR *at = line; *at; at++)
		if (at[0] == L'-' && at[1] == L'E' && at[2] == L'm' && at[3] == L'b')
			return serve();
	if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
		return 1;
	WCHAR self[MAX_PATH], command[MAX_PATH + 4], key[128], text[40], path[MAX_PATH];
	GetTempPathW(MAX_PATH, path);
	lstrcatW(path, sizeof(void *) == 8 ? L"raven-typelib-marshal64.tlb" : L"raven-typelib-marshal32.tlb");
	const int described = describe(path);
	if (described)
		return described;
	GetModuleFileNameW(NULL, self, MAX_PATH);
	wsprintfW(command, L"\"%s\"", self);
	StringFromGUID2(&test_class, text, 40);
	wsprintfW(key, L"Software\\Classes\\CLSID\\%s\\LocalServer32", text);
	HKEY handle;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_WRITE, NULL, &handle, NULL))
		return 2;
	RegSetValueExW(handle, NULL, 0, REG_SZ, (const BYTE *)command, (lstrlenW(command) + 1) * sizeof(WCHAR));
	RegCloseKey(handle);

	/* Outside any call there is no call context. */
	IServerSecurity *security = NULL;
	if (CoGetCallContext(&IID_IServerSecurity, (void **)&security) != RPC_E_CALL_COMPLETE)
		return 13;
	test_object *remote = NULL;
	if (FAILED(CoCreateInstance(&test_class, NULL, CLSCTX_LOCAL_SERVER, &test_interface, (void **)&remote)) || !remote)
		return 3;
	LONG process = 0;
	if (FAILED(remote->table->process(remote, &process)) || !process || (DWORD)process == GetCurrentProcessId())
		return 4;
	BSTR word = SysAllocString(L"abc"), echoed = NULL;
	if (FAILED(remote->table->echo(remote, word, 42, &echoed)) || !echoed || lstrcmpW(echoed, L"abc42"))
		return 5;
	SysFreeString(echoed);
	echoed = NULL;
	if (FAILED(remote->table->echo(remote, NULL, -7, &echoed)) || !echoed || lstrcmpW(echoed, L"(null)-7"))
		return 6;
	SysFreeString(echoed);
	VARIANT value, doubled;
	V_VT(&value) = VT_I4;
	V_I4(&value) = 21;
	if (FAILED(remote->table->twice(remote, value, &doubled)) || V_VT(&doubled) != VT_I4 || V_I4(&doubled) != 42)
		return 7;
	V_VT(&value) = VT_BSTR;
	V_BSTR(&value) = word;
	if (FAILED(remote->table->twice(remote, value, &doubled)) || V_VT(&doubled) != VT_BSTR || lstrcmpW(V_BSTR(&doubled), L"abcabc"))
		return 8;
	VariantClear(&doubled);
	V_VT(&value) = VT_R8;
	if (remote->table->twice(remote, value, &doubled) != DISP_E_TYPEMISMATCH)
		return 9;
	LONG answer = 0;
	if (FAILED(remote->table->visit(remote, (IUnknown *)&visitor, &answer)) || answer != 5)
		return 10;
	SAFEARRAYBOUND bounds = {2, 0};
	SAFEARRAY *list = SafeArrayCreate(VT_BSTR, 1, &bounds);
	LONG index = 0;
	SafeArrayPutElement(list, &index, SysAllocString(L"ab"));
	index = 1;
	SafeArrayPutElement(list, &index, SysAllocString(L"cde"));
	LONG count = 0;
	if (FAILED(remote->table->strings(remote, list, &count)) || count != 5)
		return 11;
	SafeArrayDestroy(list);
	SysFreeString(word);
	remote->table->release(remote);
	/* The server ends once its object is released. */
	HANDLE server = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)process);
	if (server && WaitForSingleObject(server, 30000) != WAIT_OBJECT_0)
		return 12;
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
	return 0;
}
