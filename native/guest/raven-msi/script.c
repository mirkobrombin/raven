/* SPDX-License-Identifier: MIT */
#include "msi_own.h"
#include <oleauto.h>
#include <activscp.h>

/* Script custom actions. The installer is the script host: it makes the language's engine (VBScript or JScript, through COM by its
 * name), hands it the Session object as a named item, runs the script's own statements and then the function the action names, whose
 * answer is the action's status. Session, Installer and Record are dispatch objects over the installer's handles. */

const GUID IID_IActiveScriptParse32 = { 0xbb1a2ae2, 0xa4f9, 0x11cf, { 0x8f, 0x20, 0x00, 0x80, 0x5f, 0x2c, 0xd0, 0x64 } };
const GUID IID_IActiveScriptParse64 = { 0xc7ef7658, 0xe1ee, 0x480e, { 0x97, 0xea, 0xd5, 0x2c, 0xb4, 0xd7, 0x6d, 0x17 } };

#ifdef _WIN64
typedef IActiveScriptParse64 script_parse;
#define script_parse_iid IID_IActiveScriptParse64
#define script_parse_release IActiveScriptParse64_Release
#define script_parse_init IActiveScriptParse64_InitNew
#define script_parse_text IActiveScriptParse64_ParseScriptText
#else
typedef IActiveScriptParse32 script_parse;
#define script_parse_iid IID_IActiveScriptParse32
#define script_parse_release IActiveScriptParse32_Release
#define script_parse_init IActiveScriptParse32_InitNew
#define script_parse_text IActiveScriptParse32_ParseScriptText
#endif

typedef enum { OBJECT_SESSION, OBJECT_INSTALLER, OBJECT_RECORD } host_kind;

typedef struct {
	IDispatch IDispatch_iface;
	LONG refs;
	host_kind kind;
	MSIHANDLE handle;
	BOOL owned;
} host_object;

typedef struct {
	const WCHAR *name;
	DISPID id;
} member;

enum {
	SESSION_PROPERTY = 1, SESSION_INSTALLER, SESSION_MESSAGE, SESSION_EVALUATECONDITION, SESSION_MODE, SESSION_TARGETPATH,
	SESSION_SOURCEPATH, SESSION_DOACTION, SESSION_LANGUAGE, SESSION_FEATURECURRENTSTATE, SESSION_FEATUREREQUESTSTATE,
	SESSION_COMPONENTCURRENTSTATE, SESSION_COMPONENTREQUESTSTATE,
	INSTALLER_CREATERECORD = 1,
	RECORD_STRINGDATA = 1, RECORD_INTEGERDATA, RECORD_FIELDCOUNT, RECORD_ISNULL, RECORD_DATASIZE, RECORD_CLEARDATA,
};

static const member session_members[] = {
	{ L"Property", SESSION_PROPERTY }, { L"Installer", SESSION_INSTALLER }, { L"Message", SESSION_MESSAGE },
	{ L"EvaluateCondition", SESSION_EVALUATECONDITION }, { L"Mode", SESSION_MODE }, { L"TargetPath", SESSION_TARGETPATH },
	{ L"SourcePath", SESSION_SOURCEPATH }, { L"DoAction", SESSION_DOACTION }, { L"Language", SESSION_LANGUAGE },
	{ L"FeatureCurrentState", SESSION_FEATURECURRENTSTATE }, { L"FeatureRequestState", SESSION_FEATUREREQUESTSTATE },
	{ L"ComponentCurrentState", SESSION_COMPONENTCURRENTSTATE }, { L"ComponentRequestState", SESSION_COMPONENTREQUESTSTATE },
	{ NULL },
};
static const member installer_members[] = { { L"CreateRecord", INSTALLER_CREATERECORD }, { NULL } };
static const member record_members[] = {
	{ L"StringData", RECORD_STRINGDATA }, { L"IntegerData", RECORD_INTEGERDATA }, { L"FieldCount", RECORD_FIELDCOUNT },
	{ L"IsNull", RECORD_ISNULL }, { L"DataSize", RECORD_DATASIZE }, { L"ClearData", RECORD_CLEARDATA }, { NULL },
};

static const IDispatchVtbl host_vtbl;

static host_object *host_impl(IDispatch *iface) { return CONTAINING_RECORD(iface, host_object, IDispatch_iface); }

static IDispatch *host_create(host_kind kind, MSIHANDLE handle, BOOL owned)
{
	host_object *object = msi_alloc(sizeof(host_object));
	object->IDispatch_iface.lpVtbl = &host_vtbl;
	object->refs = 1;
	object->kind = kind;
	object->handle = handle;
	object->owned = owned;
	return &object->IDispatch_iface;
}

static HRESULT WINAPI host_QueryInterface(IDispatch *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch)) {
		*out = iface;
		IDispatch_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI host_AddRef(IDispatch *iface) { return InterlockedIncrement(&host_impl(iface)->refs); }

static ULONG WINAPI host_Release(IDispatch *iface)
{
	host_object *object = host_impl(iface);
	LONG refs = InterlockedDecrement(&object->refs);
	if (!refs) {
		if (object->owned)
			MsiCloseHandle(object->handle);
		msi_free(object);
	}
	return refs;
}

static HRESULT WINAPI host_GetTypeInfoCount(IDispatch *iface, UINT *count)
{
	*count = 0;
	return S_OK;
}

static HRESULT WINAPI host_GetTypeInfo(IDispatch *iface, UINT index, LCID lcid, ITypeInfo **info)
{
	*info = NULL;
	return DISP_E_BADINDEX;
}

static HRESULT WINAPI host_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	host_kind kind = host_impl(iface)->kind;
	const member *members = kind == OBJECT_SESSION ? session_members : kind == OBJECT_INSTALLER ? installer_members : record_members;
	for (UINT i = 0; i < count; i++)
		ids[i] = DISPID_UNKNOWN;
	if (!count)
		return S_OK;
	for (const member *m = members; m->name; m++)
		if (!lstrcmpiW(m->name, names[0])) {
			ids[0] = m->id;
			return count == 1 ? S_OK : DISP_E_UNKNOWNNAME;
		}
	return DISP_E_UNKNOWNNAME;
}

/* The arguments come last first; the value a property is set to is the named DISPID_PROPERTYPUT argument, always rgvarg[0]. */
static VARIANT *argument(DISPPARAMS *params, BOOL put, UINT index)
{
	UINT count = params->cArgs - (put ? 1 : 0);
	if (index >= count)
		return NULL;
	VARIANT *v = &params->rgvarg[params->cArgs - 1 - index];
	return V_VT(v) == (VT_BYREF | VT_VARIANT) ? V_VARIANTREF(v) : v;
}

static HRESULT as_text(VARIANT *v, BSTR *out)
{
	VARIANT text;
	VariantInit(&text);
	*out = NULL;
	if (!v)
		return DISP_E_PARAMNOTFOUND;
	if (V_VT(v) == VT_EMPTY || V_VT(v) == VT_NULL) {
		*out = SysAllocString(L"");
		return S_OK;
	}
	HRESULT hr = VariantChangeType(&text, v, VARIANT_ALPHABOOL, VT_BSTR);
	if (SUCCEEDED(hr))
		*out = V_BSTR(&text);
	return hr;
}

static HRESULT as_long(VARIANT *v, LONG *out)
{
	VARIANT number;
	VariantInit(&number);
	*out = 0;
	if (!v)
		return DISP_E_PARAMNOTFOUND;
	if (V_VT(v) == VT_EMPTY)
		return S_OK;
	HRESULT hr = VariantChangeType(&number, v, 0, VT_I4);
	if (SUCCEEDED(hr))
		*out = V_I4(&number);
	return hr;
}

static void set_long(VARIANT *result, LONG value)
{
	if (result) {
		V_VT(result) = VT_I4;
		V_I4(result) = value;
	}
}

static void set_bool(VARIANT *result, BOOL value)
{
	if (result) {
		V_VT(result) = VT_BOOL;
		V_BOOL(result) = value ? VARIANT_TRUE : VARIANT_FALSE;
	}
}

static void set_text(VARIANT *result, WCHAR *text)
{
	if (result) {
		V_VT(result) = VT_BSTR;
		V_BSTR(result) = SysAllocString(text ? text : L"");
	}
	msi_free(text);
}

static void set_object(VARIANT *result, IDispatch *object)
{
	if (result) {
		V_VT(result) = VT_DISPATCH;
		V_DISPATCH(result) = object;
	} else
		IDispatch_Release(object);
}

/* A failure the script sees as a runtime error with this text. */
static HRESULT failure(EXCEPINFO *info, const WCHAR *text)
{
	if (info) {
		memset(info, 0, sizeof(*info));
		info->scode = E_FAIL;
		info->bstrSource = SysAllocString(L"Msi API Error");
		info->bstrDescription = SysAllocString(text);
	}
	return DISP_E_EXCEPTION;
}

typedef UINT (WINAPI *text_getter)(MSIHANDLE, LPCWSTR, LPWSTR, LPDWORD);

static UINT text_out(MSIHANDLE handle, text_getter get, const WCHAR *name, WCHAR **out)
{
	DWORD size = 0;
	WCHAR small[1];
	UINT status = get(handle, name, small, &size);
	*out = NULL;
	if (status == ERROR_MORE_DATA) {
		*out = msi_alloc((++size) * sizeof(WCHAR));
		status = get(handle, name, *out, &size);
	} else if (!status)
		*out = msi_copy(L"");
	return status;
}

static UINT WINAPI target_path(MSIHANDLE h, LPCWSTR n, LPWSTR o, LPDWORD s) { return MsiGetTargetPathW(h, n, o, s); }
static UINT WINAPI source_path(MSIHANDLE h, LPCWSTR n, LPWSTR o, LPDWORD s) { return MsiGetSourcePathW(h, n, o, s); }
static UINT WINAPI get_property(MSIHANDLE h, LPCWSTR n, LPWSTR o, LPDWORD s) { return MsiGetPropertyW(h, n, o, s); }

static HRESULT session_invoke(MSIHANDLE h, DISPID id, BOOL put, DISPPARAMS *params, VARIANT *result, EXCEPINFO *info)
{
	VARIANT *value = put ? &params->rgvarg[0] : NULL;
	BSTR first = NULL, text = NULL;
	LONG number = 0;
	HRESULT hr = S_OK;
	WCHAR *out = NULL;
	if (id != SESSION_INSTALLER && id != SESSION_LANGUAGE && id != SESSION_MODE && id != SESSION_MESSAGE && FAILED(as_text(argument(params, put, 0), &first)))
		return DISP_E_TYPEMISMATCH;
	switch (id) {
	case SESSION_PROPERTY:
		if (put) {
			if (SUCCEEDED(hr = as_text(value, &text)))
				MsiSetPropertyW(h, first, text);
		} else {
			text_out(h, get_property, first, &out);
			set_text(result, out);
		}
		break;
	case SESSION_INSTALLER:
		set_object(result, host_create(OBJECT_INSTALLER, 0, FALSE));
		break;
	case SESSION_MESSAGE: {
		VARIANT *record = argument(params, FALSE, 1);
		if (!record || V_VT(record) != VT_DISPATCH || !V_DISPATCH(record) || V_DISPATCH(record)->lpVtbl != &host_vtbl ||
			host_impl(V_DISPATCH(record))->kind != OBJECT_RECORD || FAILED(hr = as_long(argument(params, FALSE, 0), &number))) {
			hr = DISP_E_TYPEMISMATCH;
			break;
		}
		set_long(result, MsiProcessMessage(h, (INSTALLMESSAGE)number, host_impl(V_DISPATCH(record))->handle));
		break;
	}
	case SESSION_EVALUATECONDITION:
		set_long(result, MsiEvaluateConditionW(h, first));
		break;
	case SESSION_MODE:
		if (FAILED(hr = as_long(argument(params, put, 0), &number)))
			break;
		if (put) {
			VARIANT flag;
			VariantInit(&flag);
			if (SUCCEEDED(hr = VariantChangeType(&flag, value, 0, VT_BOOL)))
				MsiSetMode(h, (MSIRUNMODE)number, V_BOOL(&flag) != VARIANT_FALSE);
		} else
			set_bool(result, MsiGetMode(h, (MSIRUNMODE)number));
		break;
	case SESSION_TARGETPATH:
	case SESSION_SOURCEPATH:
		if (put && id == SESSION_TARGETPATH) {
			if (SUCCEEDED(hr = as_text(value, &text)) && MsiSetTargetPathW(h, first, text))
				hr = failure(info, L"The folder cannot be set");
		} else if (text_out(h, id == SESSION_TARGETPATH ? target_path : source_path, first, &out)) {
			msi_free(out);
			hr = failure(info, L"The folder is not known");
		} else
			set_text(result, out);
		break;
	case SESSION_DOACTION: {
		UINT status = MsiDoActionW(h, first);
		set_long(result, status == ERROR_SUCCESS ? 1 : status == ERROR_FUNCTION_NOT_CALLED ? 0 : status == ERROR_INSTALL_USEREXIT ? 2 :
			status == ERROR_INSTALL_SUSPEND ? 4 : status == ERROR_NO_MORE_ITEMS ? 5 : 3);
		break;
	}
	case SESSION_LANGUAGE:
		set_long(result, MsiGetLanguage(h));
		break;
	default: {
		BOOL feature = id == SESSION_FEATURECURRENTSTATE || id == SESSION_FEATUREREQUESTSTATE;
		BOOL request = id == SESSION_FEATUREREQUESTSTATE || id == SESSION_COMPONENTREQUESTSTATE;
		if (put && request) {
			if (SUCCEEDED(hr = as_long(value, &number)) &&
				(feature ? MsiSetFeatureStateW(h, first, (INSTALLSTATE)number) : MsiSetComponentStateW(h, first, (INSTALLSTATE)number)))
				hr = failure(info, L"The state cannot be set");
		} else if (put) {
			hr = DISP_E_MEMBERNOTFOUND;
		} else {
			INSTALLSTATE installed = INSTALLSTATE_UNKNOWN, action = INSTALLSTATE_UNKNOWN;
			if (feature ? MsiGetFeatureStateW(h, first, &installed, &action) : MsiGetComponentStateW(h, first, &installed, &action))
				hr = failure(info, L"The feature or component is not known");
			else
				set_long(result, request ? action : installed);
		}
		break;
	}
	}
	SysFreeString(first);
	SysFreeString(text);
	return hr;
}

static HRESULT record_invoke(MSIHANDLE h, DISPID id, BOOL put, DISPPARAMS *params, VARIANT *result)
{
	VARIANT *value = put ? &params->rgvarg[0] : NULL;
	LONG field = 0, number = 0;
	HRESULT hr = S_OK;
	if (id != RECORD_FIELDCOUNT && id != RECORD_CLEARDATA && FAILED(as_long(argument(params, put, 0), &field)))
		return DISP_E_TYPEMISMATCH;
	switch (id) {
	case RECORD_STRINGDATA:
		if (put) {
			BSTR text;
			if (SUCCEEDED(hr = as_text(value, &text)))
				MsiRecordSetStringW(h, field, text);
			SysFreeString(text);
		} else {
			DWORD size = 0;
			WCHAR small[1], *text = NULL;
			if (MsiRecordGetStringW(h, field, small, &size) == ERROR_MORE_DATA) {
				text = msi_alloc((++size) * sizeof(WCHAR));
				MsiRecordGetStringW(h, field, text, &size);
			}
			set_text(result, text);
		}
		break;
	case RECORD_INTEGERDATA:
		if (put) {
			if (SUCCEEDED(hr = as_long(value, &number)))
				MsiRecordSetInteger(h, field, number);
		} else
			set_long(result, MsiRecordGetInteger(h, field));
		break;
	case RECORD_FIELDCOUNT:
		set_long(result, MsiRecordGetFieldCount(h));
		break;
	case RECORD_ISNULL:
		set_bool(result, MsiRecordIsNull(h, field));
		break;
	case RECORD_DATASIZE:
		set_long(result, MsiRecordDataSize(h, field));
		break;
	case RECORD_CLEARDATA:
		MsiRecordClearData(h);
		break;
	default:
		hr = DISP_E_MEMBERNOTFOUND;
	}
	return hr;
}

static HRESULT WINAPI host_Invoke(IDispatch *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result,
	EXCEPINFO *info, UINT *bad)
{
	host_object *object = host_impl(iface);
	BOOL put = (flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) != 0;
	DISPPARAMS none = { 0 };
	if (!params)
		params = &none;
	if (put && !params->cArgs)
		return DISP_E_BADPARAMCOUNT;
	if (result)
		VariantInit(result);
	switch (object->kind) {
	case OBJECT_SESSION:
		return session_invoke(object->handle, id, put, params, result, info);
	case OBJECT_RECORD:
		return record_invoke(object->handle, id, put, params, result);
	case OBJECT_INSTALLER:
		if (id == INSTALLER_CREATERECORD && !put) {
			LONG count;
			if (FAILED(as_long(argument(params, FALSE, 0), &count)) || count < 0)
				return DISP_E_TYPEMISMATCH;
			set_object(result, host_create(OBJECT_RECORD, MsiCreateRecord((UINT)count), TRUE));
			return S_OK;
		}
		break;
	}
	return DISP_E_MEMBERNOTFOUND;
}

static const IDispatchVtbl host_vtbl = {
	host_QueryInterface, host_AddRef, host_Release, host_GetTypeInfoCount, host_GetTypeInfo, host_GetIDsOfNames, host_Invoke,
};

/* The site: it gives the engine the Session item and writes the errors it is told of to the log. */
typedef struct {
	IActiveScriptSite IActiveScriptSite_iface;
	IDispatch *session;
	const WCHAR *action;
	BOOL failed;
} script_site;

static script_site *site_impl(IActiveScriptSite *iface) { return CONTAINING_RECORD(iface, script_site, IActiveScriptSite_iface); }

static HRESULT WINAPI site_QueryInterface(IActiveScriptSite *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IActiveScriptSite)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

/* The site lives on the stack of the run that made it, past the engine's last call. */
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
	if (lstrcmpiW(name, L"Session"))
		return TYPE_E_ELEMENTNOTFOUND;
	if (mask & SCRIPTINFO_IUNKNOWN) {
		IDispatch_AddRef(site_impl(iface)->session);
		*unknown = (IUnknown *)site_impl(iface)->session;
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

static HRESULT WINAPI site_OnScriptError(IActiveScriptSite *iface, IActiveScriptError *error)
{
	script_site *site = site_impl(iface);
	EXCEPINFO info = { 0 };
	DWORD context = 0;
	ULONG line = 0;
	LONG column = 0;
	IActiveScriptError_GetExceptionInfo(error, &info);
	IActiveScriptError_GetSourcePosition(error, &context, &line, &column);
	if (!info.scode && info.wCode)
		info.scode = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_CONTROL, info.wCode);
	package_log(L"Script error in %s, line %u: %08x %s", site->action, line + 1, (UINT)info.scode,
		info.bstrDescription ? info.bstrDescription : L"");
	SysFreeString(info.bstrSource);
	SysFreeString(info.bstrDescription);
	SysFreeString(info.bstrHelpFile);
	site->failed = TRUE;
	return S_OK;
}

static HRESULT WINAPI site_OnEnterScript(IActiveScriptSite *iface) { return S_OK; }
static HRESULT WINAPI site_OnLeaveScript(IActiveScriptSite *iface) { return S_OK; }

static const IActiveScriptSiteVtbl site_vtbl = {
	site_QueryInterface, site_AddRef, site_Release, site_GetLCID, site_GetItemInfo, site_GetDocVersionString, site_OnScriptTerminate,
	site_OnStateChange, site_OnScriptError, site_OnEnterScript, site_OnLeaveScript,
};

static WCHAR *script_text(const BYTE *data, SIZE_T size)
{
	if (size >= 2 && data[0] == 0xff && data[1] == 0xfe)
		return msi_copy_n((const WCHAR *)(data + 2), (size - 2) / 2);
	UINT codepage = CP_ACP;
	if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) {
		data += 3;
		size -= 3;
		codepage = CP_UTF8;
	}
	int length = MultiByteToWideChar(codepage, 0, (const char *)data, (int)size, NULL, 0);
	WCHAR *text = msi_alloc((length + 1) * sizeof(WCHAR));
	MultiByteToWideChar(codepage, 0, (const char *)data, (int)size, text, length);
	return text;
}

/* Calls the entry function the action names; its answer, an integer, is the action's. */
static HRESULT call_entry(IActiveScript *script, const WCHAR *entry, VARIANT *answer)
{
	IDispatch *globals = NULL;
	HRESULT hr = IActiveScript_GetScriptDispatch(script, NULL, &globals);
	if (FAILED(hr))
		return hr;
	DISPID id;
	LPOLESTR name = (LPOLESTR)entry;
	hr = IDispatch_GetIDsOfNames(globals, &IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &id);
	if (SUCCEEDED(hr)) {
		DISPPARAMS none = { 0 };
		EXCEPINFO info = { 0 };
		hr = IDispatch_Invoke(globals, id, &IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &none, answer, &info, NULL);
		SysFreeString(info.bstrSource);
		SysFreeString(info.bstrDescription);
		SysFreeString(info.bstrHelpFile);
	}
	IDispatch_Release(globals);
	return hr;
}

UINT script_action_run(msi_package *package, MSIHANDLE session, const WCHAR *language, const BYTE *data, SIZE_T size, const WCHAR *entry,
	const WCHAR *action)
{
	HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	CLSID clsid;
	IActiveScript *script = NULL;
	script_parse *parse = NULL;
	UINT status = ERROR_INSTALL_FAILURE;
	HRESULT hr = CLSIDFromProgID(language, &clsid);
	if (SUCCEEDED(hr))
		hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IActiveScript, (void **)&script);
	if (FAILED(hr)) {
		package_log(L"The %s engine for %s cannot be made: %08x", language, action, (UINT)hr);
		if (SUCCEEDED(init))
			CoUninitialize();
		return ERROR_INSTALL_FAILURE;
	}
	script_site site = { { &site_vtbl }, host_create(OBJECT_SESSION, session, FALSE), action, FALSE };
	WCHAR *code = script_text(data, size);
	VARIANT answer;
	VariantInit(&answer);
	hr = IActiveScript_QueryInterface(script, &script_parse_iid, (void **)&parse);
	if (SUCCEEDED(hr))
		hr = script_parse_init(parse);
	if (SUCCEEDED(hr))
		hr = IActiveScript_SetScriptSite(script, &site.IActiveScriptSite_iface);
	if (SUCCEEDED(hr))
		hr = IActiveScript_AddNamedItem(script, L"Session", SCRIPTITEM_ISVISIBLE);
	if (SUCCEEDED(hr))
		hr = script_parse_text(parse, code, NULL, NULL, NULL, 0, 0, 0, NULL, NULL);
	if (SUCCEEDED(hr))
		hr = IActiveScript_SetScriptState(script, SCRIPTSTATE_CONNECTED);
	if (SUCCEEDED(hr) && !site.failed && entry && *entry) {
		hr = call_entry(script, entry, &answer);
		if (hr == DISP_E_UNKNOWNNAME)
			package_log(L"The entry function %s is not in the script of %s", entry, action);
	}
	if (FAILED(hr) || site.failed) {
		if (!site.failed && hr != DISP_E_UNKNOWNNAME)
			package_log(L"The script of %s failed: %08x", action, (UINT)hr);
	} else {
		/* The documented answers: 1 succeeds, 2 the user cancelled, 3 failed, 4 suspends, 5 skips the remaining actions. */
		LONG number = 1;
		if (V_VT(&answer) != VT_EMPTY && FAILED(as_long(&answer, &number)))
			number = 3;
		status = number == 0 || number == 1 ? ERROR_SUCCESS : number == 2 ? ERROR_INSTALL_USEREXIT : number == 4 ? ERROR_INSTALL_SUSPEND :
			number == 5 ? ERROR_NO_MORE_ITEMS : ERROR_INSTALL_FAILURE;
	}
	VariantClear(&answer);
	IActiveScript_Close(script);
	if (parse)
		script_parse_release(parse);
	IActiveScript_Release(script);
	IDispatch_Release(site.session);
	msi_free(code);
	if (SUCCEEDED(init))
		CoUninitialize();
	return status;
}
