/* CSCRIPT and WSCRIPT: the script host. It runs a script file in the engine of its language (VBScript for .vbs, JScript for .js, or
   the one //E names), made through COM, and hands it the WScript object: Echo, Quit, Sleep, CreateObject, the arguments, the standard
   streams and the names of the script and the host. CSCRIPT writes to the console; WSCRIPT, built with WSH_WINDOWED, shows message
   boxes. Options for the host start with //, anywhere on the command line; everything else goes to the script. */

#define COBJMACROS
#include "tool.h"
#include <oleauto.h>
#include <activscp.h>

/* IsEqualIID compares through memcmp, which tool.h does not give. */
int memcmp(const void *left, const void *right, size_t size)
{
	const unsigned char *a = left, *b = right;
	for (; size--; a++, b++)
		if (*a != *b)
			return *a < *b ? -1 : 1;
	return 0;
}

const GUID IID_IActiveScriptParse32 = { 0xbb1a2ae2, 0xa4f9, 0x11cf, { 0x8f, 0x20, 0x00, 0x80, 0x5f, 0x2c, 0xd0, 0x64 } };
const GUID IID_IActiveScriptParse64 = { 0xc7ef7658, 0xe1ee, 0x480e, { 0x97, 0xea, 0xd5, 0x2c, 0xb4, 0xd7, 0x6d, 0x17 } };

#ifdef _WIN64
#define parse_iid IID_IActiveScriptParse64
#define parse_interface IActiveScriptParse64
#define parse_init IActiveScriptParse64_InitNew
#define parse_text IActiveScriptParse64_ParseScriptText
#define parse_release IActiveScriptParse64_Release
#else
#define parse_iid IID_IActiveScriptParse32
#define parse_interface IActiveScriptParse32
#define parse_init IActiveScriptParse32_InitNew
#define parse_text IActiveScriptParse32_ParseScriptText
#define parse_release IActiveScriptParse32_Release
#endif

#ifdef WSH_WINDOWED
static const BOOL windowed = TRUE;
#else
static const BOOL windowed = FALSE;
#endif

static BOOL interactive = TRUE, logo = FALSE;
static WCHAR *script_path, *script_name, *host_path;
static WCHAR **script_arguments;
static DWORD script_argument_count;
static DWORD timeout_seconds;
static int exit_code;

static void show(const WCHAR *text, BOOL error)
{
	if (windowed) {
		if (interactive)
			MessageBoxW(NULL, text, L"Windows Script Host", MB_OK | (error ? MB_ICONERROR : 0));
		return;
	}
	write_to(error ? error_output : output, text, length(text));
	write_to(error ? error_output : output, L"\r\n", 2);
}

/* A dispatch object of the host: its members by name, and what calling one does. */

typedef struct host_object host_object;
typedef HRESULT (*invoke_fn)(host_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *info);

/* A member by name, with the most arguments it takes: WScript.Arguments(0) is a call of Arguments with one, which the language
 * resolves as the property's value indexed once it is refused. */
typedef struct {
	const WCHAR *name;
	DISPID id;
	BYTE takes;
} member;

struct host_object {
	IDispatch IDispatch_iface;
	LONG refs;
	const member *members;
	invoke_fn invoke;
	/* Which arguments a collection holds: all, the named or the unnamed ones. */
	int which;
};

static host_object *impl(IDispatch *iface) { return CONTAINING_RECORD(iface, host_object, IDispatch_iface); }

static HRESULT WINAPI object_QueryInterface(IDispatch *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch)) {
		*out = iface;
		IDispatch_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI object_AddRef(IDispatch *iface) { return InterlockedIncrement(&impl(iface)->refs); }

static ULONG WINAPI object_Release(IDispatch *iface)
{
	LONG refs = InterlockedDecrement(&impl(iface)->refs);
	if (!refs)
		release(impl(iface));
	return refs;
}

static HRESULT WINAPI object_GetTypeInfoCount(IDispatch *iface, UINT *count)
{
	*count = 0;
	return S_OK;
}

static HRESULT WINAPI object_GetTypeInfo(IDispatch *iface, UINT index, LCID lcid, ITypeInfo **info)
{
	*info = NULL;
	return DISP_E_BADINDEX;
}

static HRESULT WINAPI object_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	for (UINT i = 0; i < count; i++)
		ids[i] = DISPID_UNKNOWN;
	if (!count)
		return S_OK;
	for (const member *m = impl(iface)->members; m->name; m++)
		if (!compare_folded(m->name, names[0])) {
			ids[0] = m->id;
			return count == 1 ? S_OK : DISP_E_UNKNOWNNAME;
		}
	return DISP_E_UNKNOWNNAME;
}

static HRESULT WINAPI object_Invoke(IDispatch *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result,
	EXCEPINFO *info, UINT *bad)
{
	DISPPARAMS none = { 0 };
	if (!params)
		params = &none;
	UINT given = params->cArgs - ((flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) && params->cArgs ? 1 : 0);
	for (const member *m = impl(iface)->members; m->name; m++)
		if (m->id == id) {
			if (given > m->takes)
				return DISP_E_BADPARAMCOUNT;
			break;
		}
	if (result)
		VariantInit(result);
	return impl(iface)->invoke(impl(iface), id, flags, params, result, info);
}

static IDispatchVtbl object_vtbl = {
	object_QueryInterface, object_AddRef, object_Release, object_GetTypeInfoCount, object_GetTypeInfo, object_GetIDsOfNames, object_Invoke,
};

static IDispatch *new_object(const member *members, invoke_fn invoke, int which)
{
	host_object *object = allocate(sizeof(host_object));
	object->IDispatch_iface.lpVtbl = &object_vtbl;
	object->refs = 1;
	object->members = members;
	object->invoke = invoke;
	object->which = which;
	return &object->IDispatch_iface;
}

static VARIANT *argument(DISPPARAMS *params, UINT index)
{
	if (index >= params->cArgs)
		return NULL;
	VARIANT *v = &params->rgvarg[params->cArgs - 1 - index];
	while (V_VT(v) == (VT_BYREF | VT_VARIANT))
		v = V_VARIANTREF(v);
	return V_VT(v) == VT_ERROR && V_ERROR(v) == DISP_E_PARAMNOTFOUND ? NULL : v;
}

static HRESULT return_text(VARIANT *result, const WCHAR *text)
{
	if (result) {
		V_VT(result) = VT_BSTR;
		V_BSTR(result) = SysAllocString(text);
	}
	return S_OK;
}

static HRESULT return_long(VARIANT *result, LONG value)
{
	if (result) {
		V_VT(result) = VT_I4;
		V_I4(result) = value;
	}
	return S_OK;
}

static HRESULT return_object(VARIANT *result, IDispatch *object)
{
	if (result) {
		V_VT(result) = VT_DISPATCH;
		V_DISPATCH(result) = object;
	} else if (object)
		IDispatch_Release(object);
	return S_OK;
}

static HRESULT exception(EXCEPINFO *info, HRESULT code, const WCHAR *text)
{
	if (info) {
		memset(info, 0, sizeof(*info));
		info->scode = code;
		info->bstrSource = SysAllocString(windowed ? L"WScript" : L"CScript");
		info->bstrDescription = SysAllocString(text);
	}
	return DISP_E_EXCEPTION;
}

/* The text of a value as Echo writes it. */
static void append_value(struct Text *text, VARIANT *v)
{
	VARIANT converted;
	VariantInit(&converted);
	if (V_VT(v) == VT_NULL)
		text_put(text, L"Null");
	else if (V_VT(v) != VT_EMPTY && SUCCEEDED(VariantChangeType(&converted, v, VARIANT_ALPHABOOL, VT_BSTR)))
		text_put(text, V_BSTR(&converted));
	VariantClear(&converted);
}

/* Arguments, Named and Unnamed: which is 0 for all of them, 1 for /name:value ones, 2 for the rest. */

enum { ARGS_ITEM = DISPID_VALUE, ARGS_COUNT = 1, ARGS_LENGTH, ARGS_NAMED, ARGS_UNNAMED, ARGS_EXISTS, ARGS_SHOWUSAGE };

static const member arguments_members[] = {
	{ L"Item", ARGS_ITEM, 1 }, { L"Count", ARGS_COUNT, 0 }, { L"Length", ARGS_LENGTH, 0 }, { L"Named", ARGS_NAMED, 0 }, { L"Unnamed", ARGS_UNNAMED, 0 },
	{ L"Exists", ARGS_EXISTS, 1 }, { L"ShowUsage", ARGS_SHOWUSAGE, 0 }, { L"_NewEnum", DISPID_NEWENUM, 0 }, { NULL },
};

static BOOL is_named(const WCHAR *argument) { return argument[0] == L'/' && argument[1]; }

/* The name of a named argument, and where its value starts (after a colon), or NULL when it has none. */
static WCHAR *named_name(const WCHAR *argument, const WCHAR **value)
{
	DWORD end = 1;
	while (argument[end] && argument[end] != L':')
		end++;
	*value = argument[end] == L':' ? argument + end + 1 : NULL;
	return duplicate(argument + 1, end - 1);
}

static DWORD selected(int which, WCHAR ***out)
{
	WCHAR **items = allocate((script_argument_count + 1) * sizeof(WCHAR *));
	DWORD count = 0;
	for (DWORD i = 0; i < script_argument_count; i++) {
		if (which == 1 && is_named(script_arguments[i])) {
			const WCHAR *value;
			items[count++] = named_name(script_arguments[i], &value);
		} else if (which == 0 || (which == 2 && !is_named(script_arguments[i])))
			items[count++] = copy_of(script_arguments[i]);
	}
	*out = items;
	return count;
}

static void free_selected(WCHAR **items, DWORD count)
{
	for (DWORD i = 0; i < count; i++)
		release(items[i]);
	release(items);
}

/* The enumerator For Each takes from a collection. */

typedef struct {
	IEnumVARIANT IEnumVARIANT_iface;
	LONG refs;
	BSTR *items;
	ULONG count, at;
} enumerator;

static enumerator *enum_impl(IEnumVARIANT *iface) { return CONTAINING_RECORD(iface, enumerator, IEnumVARIANT_iface); }

static HRESULT WINAPI enum_QueryInterface(IEnumVARIANT *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumVARIANT)) {
		*out = iface;
		IEnumVARIANT_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI enum_AddRef(IEnumVARIANT *iface) { return InterlockedIncrement(&enum_impl(iface)->refs); }

static ULONG WINAPI enum_Release(IEnumVARIANT *iface)
{
	enumerator *e = enum_impl(iface);
	LONG refs = InterlockedDecrement(&e->refs);
	if (!refs) {
		for (ULONG i = 0; i < e->count; i++)
			SysFreeString(e->items[i]);
		release(e->items);
		release(e);
	}
	return refs;
}

static HRESULT WINAPI enum_Next(IEnumVARIANT *iface, ULONG wanted, VARIANT *out, ULONG *fetched)
{
	enumerator *e = enum_impl(iface);
	ULONG got = 0;
	for (; got < wanted && e->at < e->count; got++, e->at++) {
		V_VT(&out[got]) = VT_BSTR;
		V_BSTR(&out[got]) = SysAllocString(e->items[e->at]);
	}
	if (fetched)
		*fetched = got;
	return got == wanted ? S_OK : S_FALSE;
}

static HRESULT WINAPI enum_Skip(IEnumVARIANT *iface, ULONG count)
{
	enumerator *e = enum_impl(iface);
	e->at = e->at + count < e->count ? e->at + count : e->count;
	return e->at < e->count ? S_OK : S_FALSE;
}

static HRESULT WINAPI enum_Reset(IEnumVARIANT *iface)
{
	enum_impl(iface)->at = 0;
	return S_OK;
}

static HRESULT WINAPI enum_Clone(IEnumVARIANT *iface, IEnumVARIANT **out)
{
	*out = NULL;
	return E_NOTIMPL;
}

static IEnumVARIANTVtbl enum_vtbl = { enum_QueryInterface, enum_AddRef, enum_Release, enum_Next, enum_Skip, enum_Reset, enum_Clone };

static HRESULT arguments_invoke(host_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *info)
{
	WCHAR **items;
	DWORD count = selected(self->which, &items);
	HRESULT hr = S_OK;
	switch (id) {
	case ARGS_COUNT:
	case ARGS_LENGTH:
		hr = return_long(result, (LONG)count);
		break;
	case ARGS_NAMED:
	case ARGS_UNNAMED:
		hr = return_object(result, new_object(arguments_members, arguments_invoke, id == ARGS_NAMED ? 1 : 2));
		break;
	case ARGS_SHOWUSAGE:
		break;
	case DISPID_NEWENUM: {
		enumerator *e = allocate(sizeof(enumerator));
		e->IEnumVARIANT_iface.lpVtbl = &enum_vtbl;
		e->refs = 1;
		e->items = allocate((count + 1) * sizeof(BSTR));
		e->count = count;
		for (DWORD i = 0; i < count; i++)
			e->items[i] = SysAllocString(items[i]);
		if (result) {
			V_VT(result) = VT_UNKNOWN;
			V_UNKNOWN(result) = (IUnknown *)&e->IEnumVARIANT_iface;
		} else
			enum_Release(&e->IEnumVARIANT_iface);
		break;
	}
	case ARGS_ITEM:
	case ARGS_EXISTS: {
		VARIANT *key = argument(params, 0);
		VARIANT text;
		VariantInit(&text);
		if (!key) {
			hr = DISP_E_BADPARAMCOUNT;
			break;
		}
		if (self->which == 1) {
			/* Named("x"): the value after the colon, or Empty for a name given without one. */
			if (FAILED(hr = VariantChangeType(&text, key, 0, VT_BSTR)))
				break;
			BOOL found = FALSE;
			for (DWORD i = 0; i < script_argument_count && !found; i++) {
				const WCHAR *value;
				if (!is_named(script_arguments[i]))
					continue;
				WCHAR *name = named_name(script_arguments[i], &value);
				if (!compare_folded(name, V_BSTR(&text))) {
					found = TRUE;
					if (id == ARGS_ITEM && value)
						hr = return_text(result, value);
				}
				release(name);
			}
			if (id == ARGS_EXISTS) {
				if (result) {
					V_VT(result) = VT_BOOL;
					V_BOOL(result) = found ? VARIANT_TRUE : VARIANT_FALSE;
				}
			}
			VariantClear(&text);
			break;
		}
		if (id == ARGS_EXISTS) {
			hr = DISP_E_MEMBERNOTFOUND;
			break;
		}
		if (FAILED(hr = VariantChangeType(&text, key, 0, VT_I4)))
			break;
		if (V_I4(&text) < 0 || (DWORD)V_I4(&text) >= count)
			hr = exception(info, DISP_E_BADINDEX, L"Subscript out of range");
		else
			hr = return_text(result, items[V_I4(&text)]);
		break;
	}
	default:
		hr = DISP_E_MEMBERNOTFOUND;
	}
	free_selected(items, count);
	return hr;
}

/* WScript. */

enum {
	WS_NAME = DISPID_VALUE, WS_ECHO = 1, WS_QUIT, WS_SLEEP, WS_CREATEOBJECT, WS_GETOBJECT, WS_ARGUMENTS, WS_SCRIPTNAME, WS_SCRIPTFULLNAME,
	WS_PATH, WS_FULLNAME, WS_VERSION, WS_BUILDVERSION, WS_INTERACTIVE, WS_TIMEOUT, WS_STDIN, WS_STDOUT, WS_STDERR, WS_CONNECTOBJECT,
	WS_DISCONNECTOBJECT,
};

static const member wscript_members[] = {
	{ L"Name", WS_NAME, 0 }, { L"Echo", WS_ECHO, 255 }, { L"Quit", WS_QUIT, 1 }, { L"Sleep", WS_SLEEP, 1 }, { L"CreateObject", WS_CREATEOBJECT, 2 },
	{ L"GetObject", WS_GETOBJECT, 2 }, { L"Arguments", WS_ARGUMENTS, 0 }, { L"ScriptName", WS_SCRIPTNAME, 0 }, { L"ScriptFullName", WS_SCRIPTFULLNAME, 0 },
	{ L"Path", WS_PATH, 0 }, { L"FullName", WS_FULLNAME, 0 }, { L"Version", WS_VERSION, 0 }, { L"BuildVersion", WS_BUILDVERSION, 0 },
	{ L"Interactive", WS_INTERACTIVE, 0 }, { L"Timeout", WS_TIMEOUT, 0 }, { L"StdIn", WS_STDIN, 0 }, { L"StdOut", WS_STDOUT, 0 }, { L"StdErr", WS_STDERR, 0 },
	{ L"ConnectObject", WS_CONNECTOBJECT, 2 }, { L"DisconnectObject", WS_DISCONNECTOBJECT, 1 }, { NULL },
};

static HRESULT create_object(const WCHAR *name, IDispatch **out)
{
	CLSID clsid;
	HRESULT hr = name[0] == L'{' ? CLSIDFromString((LPOLESTR)name, &clsid) : CLSIDFromProgID(name, &clsid);
	if (SUCCEEDED(hr))
		hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, &IID_IDispatch, (void **)out);
	return hr;
}

/* The standard streams, as the scripting runtime's text streams. */
static HRESULT standard_stream(LONG which, VARIANT *result, EXCEPINFO *info)
{
	if (windowed)
		return exception(info, E_HANDLE, L"The handle is invalid.");
	IDispatch *fso;
	HRESULT hr = create_object(L"Scripting.FileSystemObject", &fso);
	if (FAILED(hr))
		return hr;
	DISPID id;
	LPOLESTR name = L"GetStandardStream";
	if (SUCCEEDED(hr = IDispatch_GetIDsOfNames(fso, &IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &id))) {
		VARIANT arg;
		V_VT(&arg) = VT_I4;
		V_I4(&arg) = which;
		DISPPARAMS params = { &arg, NULL, 1, 0 };
		hr = IDispatch_Invoke(fso, id, &IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &params, result, info, NULL);
	}
	IDispatch_Release(fso);
	return hr;
}

static HRESULT wscript_invoke(host_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *info)
{
	BOOL put = (flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) != 0;
	VARIANT *first = argument(params, 0), converted;
	VariantInit(&converted);
	if (put && id != WS_INTERACTIVE && id != WS_TIMEOUT)
		return DISP_E_MEMBERNOTFOUND;
	switch (id) {
	case WS_NAME:
		return return_text(result, L"Windows Script Host");
	case WS_ECHO: {
		struct Text text = { 0 };
		text_put(&text, L"");
		for (UINT i = 0; i < params->cArgs; i++) {
			if (i)
				text_char(&text, L' ');
			VARIANT *v = argument(params, i);
			if (v)
				append_value(&text, v);
		}
		WCHAR *line = text_take(&text);
		show(line, FALSE);
		release(line);
		return S_OK;
	}
	case WS_QUIT:
		if (first && SUCCEEDED(VariantChangeType(&converted, first, 0, VT_I4)))
			exit_code = V_I4(&converted);
		ExitProcess((UINT)exit_code);
	case WS_SLEEP:
		if (!first || FAILED(VariantChangeType(&converted, first, 0, VT_I4)))
			return DISP_E_TYPEMISMATCH;
		Sleep(V_I4(&converted) > 0 ? (DWORD)V_I4(&converted) : 0);
		return S_OK;
	case WS_CREATEOBJECT: {
		if (!first || FAILED(VariantChangeType(&converted, first, 0, VT_BSTR)))
			return DISP_E_TYPEMISMATCH;
		IDispatch *object = NULL;
		HRESULT hr = create_object(V_BSTR(&converted), &object);
		if (FAILED(hr)) {
			struct Text text = { 0 };
			text_put(&text, L"Could not locate automation class named \"");
			text_put(&text, V_BSTR(&converted));
			text_put(&text, L"\".");
			WCHAR *message = text_take(&text);
			hr = exception(info, hr, message);
			release(message);
		} else
			return_object(result, object);
		VariantClear(&converted);
		return hr;
	}
	case WS_GETOBJECT:
		return exception(info, MK_E_UNAVAILABLE, L"GetObject is not available in this host.");
	case WS_ARGUMENTS:
		return return_object(result, new_object(arguments_members, arguments_invoke, 0));
	case WS_SCRIPTNAME:
		return return_text(result, script_name);
	case WS_SCRIPTFULLNAME:
		return return_text(result, script_path);
	case WS_PATH: {
		WCHAR *folder, *name;
		split_path(host_path, &folder, &name);
		return_text(result, folder);
		release(folder);
		release(name);
		return S_OK;
	}
	case WS_FULLNAME:
		return return_text(result, host_path);
	case WS_VERSION:
		return return_text(result, L"5.8");
	case WS_BUILDVERSION:
		return return_long(result, 0);
	case WS_INTERACTIVE:
		if (put) {
			VARIANT *value = params->cArgs ? &params->rgvarg[0] : NULL;
			if (!value || FAILED(VariantChangeType(&converted, value, 0, VT_BOOL)))
				return DISP_E_TYPEMISMATCH;
			interactive = V_BOOL(&converted) != VARIANT_FALSE;
			return S_OK;
		}
		if (result) {
			V_VT(result) = VT_BOOL;
			V_BOOL(result) = interactive ? VARIANT_TRUE : VARIANT_FALSE;
		}
		return S_OK;
	case WS_TIMEOUT:
		if (put)
			return S_OK;
		return return_long(result, (LONG)timeout_seconds);
	case WS_STDIN:
	case WS_STDOUT:
	case WS_STDERR:
		return standard_stream(id == WS_STDIN ? 0 : id == WS_STDOUT ? 1 : 2, result, info);
	case WS_CONNECTOBJECT:
	case WS_DISCONNECTOBJECT:
		return S_OK;
	}
	return DISP_E_MEMBERNOTFOUND;
}

/* The site: the WScript item, and the errors the engine reports. */

static IDispatch *wscript;
static BOOL script_failed;

static HRESULT WINAPI site_QueryInterface(IActiveScriptSite *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IActiveScriptSite)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI site_AddRef(IActiveScriptSite *iface) { return 2; }
static ULONG WINAPI site_Release(IActiveScriptSite *iface) { return 1; }

static HRESULT WINAPI site_GetLCID(IActiveScriptSite *iface, LCID *lcid)
{
	*lcid = GetUserDefaultLCID();
	return S_OK;
}

static HRESULT WINAPI site_GetItemInfo(IActiveScriptSite *iface, LPCOLESTR name, DWORD mask, IUnknown **unknown, ITypeInfo **info)
{
	if (info)
		*info = NULL;
	if (unknown)
		*unknown = NULL;
	if (compare_folded(name, L"WScript") && compare_folded(name, L"WSH"))
		return TYPE_E_ELEMENTNOTFOUND;
	if (mask & SCRIPTINFO_IUNKNOWN) {
		IDispatch_AddRef(wscript);
		*unknown = (IUnknown *)wscript;
	}
	return S_OK;
}

static HRESULT WINAPI site_GetDocVersionString(IActiveScriptSite *iface, BSTR *version)
{
	*version = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI site_OnScriptTerminate(IActiveScriptSite *iface, const VARIANT *result, const EXCEPINFO *info) { return S_OK; }
static HRESULT WINAPI site_OnStateChange(IActiveScriptSite *iface, SCRIPTSTATE state) { return S_OK; }

static void put_decimal(struct Text *text, ULONG value)
{
	WCHAR digits[16];
	int at = 15;
	digits[at] = 0;
	do
		digits[--at] = (WCHAR)(L'0' + value % 10);
	while (value /= 10);
	text_put(text, digits + at);
}

static HRESULT WINAPI site_OnScriptError(IActiveScriptSite *iface, IActiveScriptError *error)
{
	EXCEPINFO info = { 0 };
	DWORD context = 0;
	ULONG line = 0;
	LONG column = 0;
	IActiveScriptError_GetExceptionInfo(error, &info);
	IActiveScriptError_GetSourcePosition(error, &context, &line, &column);
	if (info.pfnDeferredFillIn)
		info.pfnDeferredFillIn(&info);
	HRESULT code = info.scode ? info.scode : MAKE_HRESULT(SEVERITY_ERROR, FACILITY_CONTROL, info.wCode);
	struct Text text = { 0 };
	if (windowed) {
		text_put(&text, L"Script:\t");
		text_put(&text, script_path);
		text_put(&text, L"\nLine:\t");
		put_decimal(&text, line + 1);
		text_put(&text, L"\nChar:\t");
		put_decimal(&text, (ULONG)column + 1);
		text_put(&text, L"\nError:\t");
		text_put(&text, info.bstrDescription ? info.bstrDescription : L"");
		text_put(&text, L"\nCode:\t");
		put_hex(&text, (ULONG)code, 8, FALSE);
		text_put(&text, L"\nSource: \t");
		text_put(&text, info.bstrSource ? info.bstrSource : L"");
	} else {
		/* script(line, column) source: description */
		text_put(&text, script_path);
		text_char(&text, L'(');
		put_decimal(&text, line + 1);
		text_put(&text, L", ");
		put_decimal(&text, (ULONG)column + 1);
		text_put(&text, L") ");
		if (info.bstrSource && info.bstrSource[0]) {
			text_put(&text, info.bstrSource);
			text_put(&text, L": ");
		}
		text_put(&text, info.bstrDescription && info.bstrDescription[0] ? info.bstrDescription : L"Unknown error");
	}
	WCHAR *message = text_take(&text);
	show(message, TRUE);
	release(message);
	SysFreeString(info.bstrSource);
	SysFreeString(info.bstrDescription);
	SysFreeString(info.bstrHelpFile);
	script_failed = TRUE;
	return S_OK;
}

static HRESULT WINAPI site_OnEnterScript(IActiveScriptSite *iface) { return S_OK; }
static HRESULT WINAPI site_OnLeaveScript(IActiveScriptSite *iface) { return S_OK; }

static IActiveScriptSiteVtbl site_vtbl = {
	site_QueryInterface, site_AddRef, site_Release, site_GetLCID, site_GetItemInfo, site_GetDocVersionString, site_OnScriptTerminate,
	site_OnStateChange, site_OnScriptError, site_OnEnterScript, site_OnLeaveScript,
};

static IActiveScriptSite site = { &site_vtbl };

/* The script's text: UTF-16 with its mark, UTF-8 with its mark, or the ANSI code page. */
static WCHAR *script_text(const WCHAR *path)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return NULL;
	DWORD size = 0;
	char *bytes = read_all(file, &size);
	CloseHandle(file);
	if (!bytes)
		return NULL;
	WCHAR *text;
	if (size >= 2 && (BYTE)bytes[0] == 0xff && (BYTE)bytes[1] == 0xfe)
		text = duplicate((const WCHAR *)(bytes + 2), (size - 2) / 2);
	else {
		UINT page = CP_ACP, skip = 0;
		if (size >= 3 && (BYTE)bytes[0] == 0xef && (BYTE)bytes[1] == 0xbb && (BYTE)bytes[2] == 0xbf)
			page = CP_UTF8, skip = 3;
		int count = MultiByteToWideChar(page, 0, bytes + skip, (int)(size - skip), NULL, 0);
		text = allocate(((SIZE_T)count + 1) * sizeof(WCHAR));
		MultiByteToWideChar(page, 0, bytes + skip, (int)(size - skip), text, count);
	}
	release(bytes);
	return text;
}

static DWORD WINAPI watch_time(void *unused)
{
	Sleep(timeout_seconds * 1000);
	struct Text text = { 0 };
	text_put(&text, script_path);
	text_put(&text, L": Script execution time was exceeded on script \"");
	text_put(&text, script_path);
	text_put(&text, L"\".\r\nScript execution was terminated.");
	WCHAR *message = text_take(&text);
	show(message, TRUE);
	ExitProcess(1);
	return 0;
}

static void usage(void)
{
	show(L"Usage: CScript scriptname.extension [option...] [arguments...]\r\n\r\n"
		L"Options:\r\n"
		L" //B         Batch mode: suppresses script errors and prompts from displaying\r\n"
		L" //D         Enable Active Debugging\r\n"
		L" //E:engine  Use engine for executing script\r\n"
		L" //I         Interactive mode (default, opposite of //B)\r\n"
		L" //Logo      Display logo (default)\r\n"
		L" //Nologo    Prevent logo display: No banner will be shown at execution time\r\n"
		L" //T:nn      Time out in seconds:  Maximum time a script is permitted to run\r\n"
		L" //X         Execute script in debugger\r\n"
		L" //U         Use Unicode for redirected I/O from the console", FALSE);
}

static BOOL ends_with(const WCHAR *text, const WCHAR *suffix)
{
	DWORD a = length(text), b = length(suffix);
	return a >= b && !compare_folded(text + a - b, suffix);
}

int raven_tool_entry(void)
{
	start_tool();
	struct Arguments arguments;
	read_arguments(&arguments);
	const WCHAR *engine = NULL, *script = NULL;
	script_arguments = allocate((arguments.count + 1) * sizeof(WCHAR *));
	for (DWORD i = 0; i < arguments.count; i++) {
		WCHAR *a = arguments.items[i];
		if (a[0] == L'/' && a[1] == L'/') {
			const WCHAR *option = a + 2;
			if (!compare_folded(option, L"B"))
				interactive = FALSE;
			else if (!compare_folded(option, L"I"))
				interactive = TRUE;
			else if (!compare_folded(option, L"nologo"))
				logo = FALSE;
			else if (!compare_folded(option, L"logo"))
				logo = TRUE;
			else if ((option[0] | 0x20) == L'e' && option[1] == L':')
				engine = option + 2;
			else if ((option[0] | 0x20) == L't' && option[1] == L':') {
				ULONGLONG seconds;
				if (read_number(option + 2, &seconds))
					timeout_seconds = (DWORD)seconds;
			} else if (!compare_folded(option, L"?")) {
				usage();
				return 0;
			}
			/* //D, //X, //U, //S, //H: and //Job: change nothing here. */
			continue;
		}
		if (!script)
			script = a;
		else
			script_arguments[script_argument_count++] = a;
	}
	if (!script) {
		usage();
		return 1;
	}
	WCHAR self[MAX_PATH + 1];
	DWORD self_size = GetModuleFileNameW(NULL, self, MAX_PATH);
	host_path = duplicate(self, self_size);
	DWORD full_size = GetFullPathNameW(script, 0, NULL, NULL);
	script_path = allocate(((SIZE_T)full_size + 1) * sizeof(WCHAR));
	GetFullPathNameW(script, full_size + 1, script_path, NULL);
	WCHAR *folder;
	split_path(script_path, &folder, &script_name);
	release(folder);
	DWORD attributes = GetFileAttributesW(script_path);
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
		struct Text text = { 0 };
		text_put(&text, L"Input Error: Can not find script file \"");
		text_put(&text, script_path);
		text_put(&text, L"\".");
		WCHAR *message = text_take(&text);
		show(message, TRUE);
		return 1;
	}
	if (!engine) {
		if (ends_with(script_path, L".vbs") || ends_with(script_path, L".vbe"))
			engine = L"VBScript";
		else if (ends_with(script_path, L".js") || ends_with(script_path, L".jse"))
			engine = L"JScript";
		else {
			const WCHAR *dot = NULL;
			for (const WCHAR *c = script_name; *c; c++)
				if (*c == L'.')
					dot = c;
			struct Text text = { 0 };
			text_put(&text, L"Input Error: There is no script engine for file extension \"");
			text_put(&text, dot ? dot : L"");
			text_put(&text, L"\".");
			WCHAR *message = text_take(&text);
			show(message, TRUE);
			return 1;
		}
	} else if (!compare_folded(engine, L"vbs") || !compare_folded(engine, L"vbscript"))
		engine = L"VBScript";
	else if (!compare_folded(engine, L"js") || !compare_folded(engine, L"jscript") || !compare_folded(engine, L"javascript"))
		engine = L"JScript";
	if (logo && !windowed)
		show(L"Raven script host\r\n", FALSE);

	WCHAR *code = script_text(script_path);
	if (!code) {
		show(L"Input Error: The script file cannot be read.", TRUE);
		return 1;
	}
	CoInitialize(NULL);
	CLSID clsid;
	IActiveScript *active = NULL;
	parse_interface *parse = NULL;
	HRESULT hr = CLSIDFromProgID(engine, &clsid);
	if (SUCCEEDED(hr))
		hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IActiveScript, (void **)&active);
	if (FAILED(hr)) {
		struct Text text = { 0 };
		text_put(&text, L"CScript Error: Can't find script engine \"");
		text_put(&text, engine);
		text_put(&text, L"\" for script \"");
		text_put(&text, script_path);
		text_put(&text, L"\".");
		WCHAR *message = text_take(&text);
		show(message, TRUE);
		return 1;
	}
	wscript = new_object(wscript_members, wscript_invoke, 0);
	if (timeout_seconds)
		CloseHandle(CreateThread(NULL, 0, watch_time, NULL, 0, NULL));
	hr = IActiveScript_QueryInterface(active, &parse_iid, (void **)&parse);
	if (SUCCEEDED(hr))
		hr = parse_init(parse);
	if (SUCCEEDED(hr))
		hr = IActiveScript_SetScriptSite(active, &site);
	if (SUCCEEDED(hr))
		hr = IActiveScript_AddNamedItem(active, L"WScript", SCRIPTITEM_ISVISIBLE);
	if (SUCCEEDED(hr))
		hr = IActiveScript_AddNamedItem(active, L"WSH", SCRIPTITEM_ISVISIBLE);
	if (SUCCEEDED(hr))
		hr = parse_text(parse, code, NULL, NULL, NULL, 0, 0, 0, NULL, NULL);
	if (SUCCEEDED(hr))
		hr = IActiveScript_SetScriptState(active, SCRIPTSTATE_CONNECTED);
	if (FAILED(hr) && !script_failed)
		show(L"CScript Error: Execution of the script failed.", TRUE);
	IActiveScript_Close(active);
	if (parse)
		parse_release(parse);
	IActiveScript_Release(active);
	IDispatch_Release(wscript);
	CoUninitialize();
	return script_failed || FAILED(hr) ? 1 : exit_code;
}
