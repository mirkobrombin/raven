#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <activscp.h>

/* A script host for vbscript.dll: it creates the engine by its ProgID, gives it a site and one named item, Host, whose members are also
 * global, runs vbscript_tests.vbs from beside it and then the engine's other ways in: an expression, the script's globals through
 * IDispatch, and errors the site is told of. */

#ifdef _WIN64
#define IActiveScriptParse_ParseScriptText IActiveScriptParse64_ParseScriptText
#define IActiveScriptParse_InitNew IActiveScriptParse64_InitNew
#define IActiveScriptParse_Release IActiveScriptParse64_Release
#else
#define IActiveScriptParse_ParseScriptText IActiveScriptParse32_ParseScriptText
#define IActiveScriptParse_InitNew IActiveScriptParse32_InitNew
#define IActiveScriptParse_Release IActiveScriptParse32_Release
#endif

/* libuuid lacks the parse interfaces' identifiers; these are the ones activscp.h gives. */
const GUID IID_IActiveScriptParse32 = { 0xbb1a2ae2, 0xa4f9, 0x11cf, { 0x8f, 0x20, 0x00, 0x80, 0x5f, 0x2c, 0xd0, 0x64 } };
const GUID IID_IActiveScriptParse64 = { 0xc7ef7658, 0xe1ee, 0x480e, { 0x97, 0xea, 0xd5, 0x2c, 0xb4, 0xd7, 0x6d, 0x17 } };

static HANDLE out;
static LONG checks, failures;
static BOOL done;
static VARIANT stored;
/* What the site was told of the last error. */
static LONG error_count;
static HRESULT error_code;
static ULONG error_line;
static WCHAR error_source[128];

static void print(const WCHAR *text)
{
	char bytes[1024];
	int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, bytes, sizeof(bytes), NULL, NULL);
	DWORD written;
	if (n > 1)
		WriteFile(out, bytes, n - 1, &written, NULL);
}

/* The text of a value, an object's default property for an object. */
static void text_of(VARIANT *value, WCHAR *text, UINT room)
{
	VARIANT plain, converted;
	VariantInit(&plain);
	VariantInit(&converted);
	if (V_VT(value) == VT_DISPATCH && V_DISPATCH(value)) {
		DISPPARAMS none = { 0 };
		IDispatch_Invoke(V_DISPATCH(value), DISPID_VALUE, &IID_NULL, 0, DISPATCH_PROPERTYGET | DISPATCH_METHOD, &none, &plain, NULL, NULL);
	} else {
		VariantCopyInd(&plain, value);
	}
	if (V_VT(&plain) == VT_NULL)
		lstrcpynW(text, L"<Null>", room);
	else if (V_VT(&plain) == VT_BOOL)
		lstrcpynW(text, V_BOOL(&plain) ? L"True" : L"False", room);
	else if (SUCCEEDED(VariantChangeType(&converted, &plain, 0, VT_BSTR)))
		lstrcpynW(text, V_BSTR(&converted) ? V_BSTR(&converted) : L"", room);
	else
		wsprintfW(text, L"<type %u>", V_VT(&plain));
	VariantClear(&plain);
	VariantClear(&converted);
}

/* Host: Check(actual, expected, label), Add(a, b), Value, Fail and Done. */

enum { HOST_CHECK = 1, HOST_ADD, HOST_VALUE, HOST_FAIL, HOST_DONE };

static HRESULT WINAPI host_QueryInterface(IDispatch *iface, REFIID riid, void **object)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch)) {
		*object = iface;
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI host_AddRef(IDispatch *iface) { return 2; }
static ULONG WINAPI host_Release(IDispatch *iface) { return 1; }

static HRESULT WINAPI host_GetTypeInfoCount(IDispatch *iface, UINT *count)
{
	*count = 0;
	return S_OK;
}

static HRESULT WINAPI host_GetTypeInfo(IDispatch *iface, UINT index, LCID lcid, ITypeInfo **info)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI host_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	static const WCHAR *const known[] = { L"Check", L"Add", L"Value", L"Fail", L"Done" };
	for (UINT i = 0; i < ARRAYSIZE(known); i++)
		if (!lstrcmpiW(known[i], names[0])) {
			ids[0] = HOST_CHECK + i;
			return S_OK;
		}
	ids[0] = DISPID_UNKNOWN;
	return DISP_E_UNKNOWNNAME;
}

static HRESULT WINAPI host_Invoke(IDispatch *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result,
	EXCEPINFO *info, UINT *bad)
{
	UINT count = params->cArgs;
	switch (id) {
	case HOST_CHECK: {
		if (count != 3)
			return DISP_E_BADPARAMCOUNT;
		WCHAR actual[256], expected[256], label[128];
		text_of(&params->rgvarg[2], actual, ARRAYSIZE(actual));
		text_of(&params->rgvarg[1], expected, ARRAYSIZE(expected));
		text_of(&params->rgvarg[0], label, ARRAYSIZE(label));
		checks++;
		if (lstrcmpW(actual, expected)) {
			WCHAR line[700];
			wsprintfW(line, L"FAIL %s: got [%s], want [%s]\n", label, actual, expected);
			print(line);
			failures++;
		}
		return S_OK;
	}
	case HOST_ADD: {
		if (count != 2 || !result)
			return DISP_E_BADPARAMCOUNT;
		return VarAdd(&params->rgvarg[1], &params->rgvarg[0], result);
	}
	case HOST_VALUE:
		if (flags & DISPATCH_PROPERTYPUT) {
			VariantClear(&stored);
			return VariantCopyInd(&stored, &params->rgvarg[0]);
		}
		return result ? VariantCopy(result, &stored) : S_OK;
	case HOST_FAIL:
		if (info) {
			memset(info, 0, sizeof(*info));
			info->scode = E_ACCESSDENIED;
			info->bstrDescription = SysAllocString(L"denied");
			info->bstrSource = SysAllocString(L"Host");
		}
		return DISP_E_EXCEPTION;
	case HOST_DONE:
		done = TRUE;
		return S_OK;
	}
	return DISP_E_MEMBERNOTFOUND;
}

static IDispatchVtbl host_vtbl = {
	host_QueryInterface, host_AddRef, host_Release, host_GetTypeInfoCount, host_GetTypeInfo, host_GetIDsOfNames, host_Invoke,
};

static IDispatch host = { &host_vtbl };

/* The site. */

static HRESULT WINAPI site_QueryInterface(IActiveScriptSite *iface, REFIID riid, void **object)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IActiveScriptSite)) {
		*object = iface;
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI site_AddRef(IActiveScriptSite *iface) { return 2; }
static ULONG WINAPI site_Release(IActiveScriptSite *iface) { return 1; }

static HRESULT WINAPI site_GetLCID(IActiveScriptSite *iface, LCID *lcid) { return E_NOTIMPL; }

static HRESULT WINAPI site_GetItemInfo(IActiveScriptSite *iface, LPCOLESTR name, DWORD mask, IUnknown **unknown, ITypeInfo **info)
{
	if (lstrcmpiW(name, L"Host"))
		return TYPE_E_ELEMENTNOTFOUND;
	if (mask & SCRIPTINFO_IUNKNOWN)
		*unknown = (IUnknown *)&host;
	if (mask & SCRIPTINFO_ITYPEINFO)
		*info = NULL;
	return S_OK;
}

static HRESULT WINAPI site_GetDocVersionString(IActiveScriptSite *iface, BSTR *version) { return E_NOTIMPL; }
static HRESULT WINAPI site_OnScriptTerminate(IActiveScriptSite *iface, const VARIANT *result, const EXCEPINFO *info) { return S_OK; }
static HRESULT WINAPI site_OnStateChange(IActiveScriptSite *iface, SCRIPTSTATE state) { return S_OK; }

static HRESULT WINAPI site_OnScriptError(IActiveScriptSite *iface, IActiveScriptError *error)
{
	EXCEPINFO info = { 0 };
	DWORD context;
	LONG column;
	error_count++;
	IActiveScriptError_GetExceptionInfo(error, &info);
	IActiveScriptError_GetSourcePosition(error, &context, &error_line, &column);
	error_code = info.scode;
	lstrcpynW(error_source, info.bstrSource ? info.bstrSource : L"", ARRAYSIZE(error_source));
	WCHAR line[400];
	wsprintfW(line, L"script error %08x line %u: %s\n", (unsigned)info.scode, (unsigned)error_line,
		info.bstrDescription ? info.bstrDescription : L"");
	print(line);
	SysFreeString(info.bstrSource);
	SysFreeString(info.bstrDescription);
	SysFreeString(info.bstrHelpFile);
	return S_OK;
}

static HRESULT WINAPI site_OnEnterScript(IActiveScriptSite *iface) { return S_OK; }
static HRESULT WINAPI site_OnLeaveScript(IActiveScriptSite *iface) { return S_OK; }

static IActiveScriptSiteVtbl site_vtbl = {
	site_QueryInterface, site_AddRef, site_Release, site_GetLCID, site_GetItemInfo, site_GetDocVersionString, site_OnScriptTerminate,
	site_OnStateChange, site_OnScriptError, site_OnEnterScript, site_OnLeaveScript,
};

static IActiveScriptSite site = { &site_vtbl };

/* vbscript_tests.vbs from the client's folder, as UTF-16. */
static WCHAR *load_script(void)
{
	WCHAR path[MAX_PATH];
	DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
	while (length && path[length - 1] != L'\\')
		length--;
	lstrcpyW(path + length, L"vbscript_tests.vbs");
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return NULL;
	DWORD size = GetFileSize(file, NULL), read = 0;
	char *bytes = HeapAlloc(GetProcessHeap(), 0, size + 1);
	ReadFile(file, bytes, size, &read, NULL);
	CloseHandle(file);
	WCHAR *text = HeapAlloc(GetProcessHeap(), 0, (read + 1) * sizeof(WCHAR));
	int n = MultiByteToWideChar(CP_UTF8, 0, bytes, (int)read, text, (int)read);
	text[n] = 0;
	HeapFree(GetProcessHeap(), 0, bytes);
	return text;
}

int raven_entry(void)
{
	/* What the checks print goes to vbscript-client.log beside the client. */
	WCHAR log_path[MAX_PATH];
	DWORD length = GetModuleFileNameW(NULL, log_path, MAX_PATH);
	while (length && log_path[length - 1] != L'\\')
		length--;
	lstrcpyW(log_path + length, L"vbscript-client.log");
	out = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
	if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
		return 1;
	CLSID clsid;
	static const CLSID vbscript = { 0xb54f3741, 0x5b07, 0x11cf, { 0xa4, 0xb0, 0x00, 0xaa, 0x00, 0x4a, 0x55, 0xe8 } };
	if (FAILED(CLSIDFromProgID(L"VBScript", &clsid)) || !IsEqualCLSID(&clsid, &vbscript))
		return 2;
	IActiveScript *script = NULL;
	if (FAILED(CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IActiveScript, (void **)&script)))
		return 3;
	IActiveScriptParse *parse = NULL;
	if (FAILED(IActiveScript_QueryInterface(script, &IID_IActiveScriptParse, (void **)&parse)))
		return 4;
	if (FAILED(IActiveScriptParse_InitNew(parse)) || FAILED(IActiveScript_SetScriptSite(script, &site)))
		return 5;
	if (FAILED(IActiveScript_AddNamedItem(script, L"Host", SCRIPTITEM_ISVISIBLE | SCRIPTITEM_GLOBALMEMBERS)))
		return 6;

	/* The checks: parsed before the start, run by it. */
	WCHAR *text = load_script();
	if (!text)
		return 7;
	if (FAILED(IActiveScriptParse_ParseScriptText(parse, text, NULL, NULL, NULL, 0, 0, 0, NULL, NULL)))
		return 8;
	if (checks || done)
		return 9;
	if (FAILED(IActiveScript_SetScriptState(script, SCRIPTSTATE_STARTED)))
		return 10;
	WCHAR line[100];
	wsprintfW(line, L"%d checks, %d failed\n", (int)checks, (int)failures);
	print(line);
	if (error_count)
		return 11;
	if (failures)
		return 12;
	if (!done || checks < 130)
		return 13;

	/* An expression, evaluated at once. */
	VARIANT value;
	VariantInit(&value);
	if (FAILED(IActiveScriptParse_ParseScriptText(parse, L"1 + 2 * 3", NULL, NULL, NULL, 0, 0, SCRIPTTEXT_ISEXPRESSION, &value, NULL)))
		return 14;
	if (V_VT(&value) != VT_I2 || V_I2(&value) != 7)
		return 15;

	/* The script's globals through IDispatch: a function called and a variable read. */
	IDispatch *globals = NULL;
	if (FAILED(IActiveScript_GetScriptDispatch(script, NULL, &globals)) || !globals)
		return 16;
	LPOLESTR name = L"Twice";
	DISPID id;
	if (FAILED(IDispatch_GetIDsOfNames(globals, &IID_NULL, &name, 1, 0, &id)))
		return 17;
	VARIANT argument;
	V_VT(&argument) = VT_I4;
	V_I4(&argument) = 21;
	DISPPARAMS params = { &argument, NULL, 1, 0 };
	VariantInit(&value);
	if (FAILED(IDispatch_Invoke(globals, id, &IID_NULL, 0, DISPATCH_METHOD, &params, &value, NULL, NULL)))
		return 18;
	if (V_VT(&value) != VT_I4 || V_I4(&value) != 42)
		return 19;
	name = L"Greeting";
	DISPPARAMS none = { 0 };
	VariantInit(&value);
	if (FAILED(IDispatch_GetIDsOfNames(globals, &IID_NULL, &name, 1, 0, &id)) ||
		FAILED(IDispatch_Invoke(globals, id, &IID_NULL, 0, DISPATCH_PROPERTYGET, &none, &value, NULL, NULL)))
		return 20;
	if (V_VT(&value) != VT_BSTR || lstrcmpW(V_BSTR(&value), L"hi"))
		return 21;
	VariantClear(&value);
	name = L"NoSuchName";
	if (IDispatch_GetIDsOfNames(globals, &IID_NULL, &name, 1, 0, &id) != DISP_E_UNKNOWNNAME)
		return 22;
	IDispatch_Release(globals);

	/* A runtime error nothing catches goes to the site, with its line counted from zero and from the start line given. */
	HRESULT hr = IActiveScriptParse_ParseScriptText(parse, L"x = 1\ny = 1 / 0\n", NULL, NULL, NULL, 0, 10, 0, NULL, NULL);
	if (hr != SCRIPT_E_REPORTED || error_count != 1)
		return 23;
	if (error_code != MAKE_HRESULT(SEVERITY_ERROR, FACILITY_CONTROL, 11) || error_line != 11)
		return 24;
	if (lstrcmpW(error_source, L"Microsoft VBScript runtime error"))
		return 25;
	/* A syntax error too, and the code does not run. */
	done = FALSE;
	hr = IActiveScriptParse_ParseScriptText(parse, L"Host.Done\nIf x Then\n", NULL, NULL, NULL, 0, 0, 0, NULL, NULL);
	if (hr != SCRIPT_E_REPORTED || error_count != 2)
		return 26;
	if (lstrcmpW(error_source, L"Microsoft VBScript compilation error") || done)
		return 27;

	IActiveScriptParse_Release(parse);
	if (FAILED(IActiveScript_Close(script)))
		return 28;
	IActiveScript_Release(script);
	CoUninitialize();
	print(L"vbscript client passed\n");
	return 0;
}
