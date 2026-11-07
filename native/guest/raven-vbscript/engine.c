/* SPDX-License-Identifier: MIT */
#include "vbs.h"
#include <objsafe.h>

/* The engine as Active Scripting hosts see it: IActiveScript to give it a site and named items and to start it, IActiveScriptParse to
 * hand it code, IObjectSafety for hosts that ask. Code parsed before the engine starts waits and runs, in order, when it does. The
 * script's global procedures and variables are an IDispatch of their own, which GetScriptDispatch hands out. */

/* libuuid lacks the parse interfaces' identifiers; these are the ones activscp.h gives. */
const GUID IID_IActiveScriptParse32 = { 0xbb1a2ae2, 0xa4f9, 0x11cf, { 0x8f, 0x20, 0x00, 0x80, 0x5f, 0x2c, 0xd0, 0x64 } };
const GUID IID_IActiveScriptParse64 = { 0xc7ef7658, 0xe1ee, 0x480e, { 0x97, 0xea, 0xd5, 0x2c, 0xb4, 0xd7, 0x6d, 0x17 } };
static const CLSID CLSID_VBScript = { 0xb54f3741, 0x5b07, 0x11cf, { 0xa4, 0xb0, 0x00, 0xaa, 0x00, 0x4a, 0x55, 0xe8 } };
static const CLSID CLSID_VBScriptRegExp = { 0x3f4daca4, 0x160d, 0x11d2, { 0xa8, 0xe9, 0x00, 0x10, 0x4b, 0x36, 0x5c, 0x9f } };

static LONG module_locks;

/* Script errors as the site is told of them. */

typedef struct {
	IActiveScriptError IActiveScriptError_iface;
	LONG refs;
	script_failure failure;
	script_context context;
	ULONG line;
	BSTR line_text;
} script_error;

static script_error *error_impl(IActiveScriptError *iface) { return CONTAINING_RECORD(iface, script_error, IActiveScriptError_iface); }

static HRESULT WINAPI error_QueryInterface(IActiveScriptError *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IActiveScriptError)) {
		*out = iface;
		IActiveScriptError_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI error_AddRef(IActiveScriptError *iface) { return InterlockedIncrement(&error_impl(iface)->refs); }

static ULONG WINAPI error_Release(IActiveScriptError *iface)
{
	script_error *error = error_impl(iface);
	LONG refs = InterlockedDecrement(&error->refs);
	if (!refs) {
		vbs_failure_clear(&error->failure);
		SysFreeString(error->line_text);
		vbs_free(error);
	}
	return refs;
}

static HRESULT WINAPI error_GetExceptionInfo(IActiveScriptError *iface, EXCEPINFO *info)
{
	script_error *error = error_impl(iface);
	if (!info)
		return E_POINTER;
	memset(info, 0, sizeof(*info));
	LONG number = error->failure.number;
	info->scode = number > 0 && number <= 0xffff ? MAKE_HRESULT(SEVERITY_ERROR, FACILITY_CONTROL, number) : number;
	info->bstrSource = SysAllocString(error->failure.source);
	info->bstrDescription = SysAllocString(error->failure.description);
	return S_OK;
}

static HRESULT WINAPI error_GetSourcePosition(IActiveScriptError *iface, DWORD *context, ULONG *line, LONG *column)
{
	script_error *error = error_impl(iface);
	if (context)
		*context = (DWORD)error->context;
	if (line)
		*line = error->line;
	if (column)
		*column = (LONG)error->failure.column;
	return S_OK;
}

static HRESULT WINAPI error_GetSourceLineText(IActiveScriptError *iface, BSTR *text)
{
	script_error *error = error_impl(iface);
	if (!text)
		return E_POINTER;
	*text = SysAllocString(error->line_text ? error->line_text : L"");
	return S_OK;
}

static IActiveScriptErrorVtbl error_vtbl = {
	error_QueryInterface, error_AddRef, error_Release, error_GetExceptionInfo, error_GetSourcePosition, error_GetSourceLineText,
};

/* The text of a line of a source, counting from one. */
static BSTR source_line(const WCHAR *source, UINT line)
{
	if (!source || !line)
		return NULL;
	const WCHAR *at = source;
	for (UINT i = 1; i < line && *at; at++)
		if (*at == L'\n')
			i++;
	const WCHAR *end = at;
	while (*end && *end != L'\n' && *end != L'\r')
		end++;
	return SysAllocStringLen(at, (UINT)(end - at));
}

HRESULT vbs_report(engine *en, const script_failure *failure)
{
	if (!en->site)
		return SCRIPT_E_REPORTED;
	script_error *error = vbs_alloc(sizeof(script_error));
	error->IActiveScriptError_iface.lpVtbl = &error_vtbl;
	error->refs = 1;
	error->failure.number = failure->number;
	error->failure.description = SysAllocString(failure->description);
	error->failure.source = SysAllocString(failure->source);
	error->failure.column = failure->column;
	error->context = en->context;
	/* Lines go to the site counted from zero and from the start line the host gave the code. */
	error->line = en->start_line + (failure->line ? failure->line - 1 : 0);
	error->line_text = source_line(en->running ? en->running->source : NULL, failure->line);
	IActiveScriptSite_OnScriptError(en->site, &error->IActiveScriptError_iface);
	IActiveScriptError_Release(&error->IActiveScriptError_iface);
	return SCRIPT_E_REPORTED;
}

/* Code and named items. */

void vbs_add_code(engine *en, script_code *code)
{
	en->kept = vbs_grow(en->kept, (en->kept_count + 1) * sizeof(script_code *));
	en->kept[en->kept_count++] = code;
	if (code->procedure_count) {
		en->procedures = vbs_grow(en->procedures, (en->procedure_count + code->procedure_count) * sizeof(procedure *));
		for (UINT i = 0; i < code->procedure_count; i++)
			en->procedures[en->procedure_count++] = code->procedures[i];
	}
	if (code->class_count) {
		en->classes = vbs_grow(en->classes, (en->class_count + code->class_count) * sizeof(class_def *));
		for (UINT i = 0; i < code->class_count; i++)
			en->classes[en->class_count++] = code->classes[i];
	}
	if (code->explicit_names)
		en->explicit_names = TRUE;
}

IDispatch *vbs_item_object(engine *en, UINT index)
{
	named_item *item = &en->items[index];
	if (!item->fetched && en->site) {
		item->fetched = TRUE;
		IUnknown *unknown = NULL;
		if (SUCCEEDED(IActiveScriptSite_GetItemInfo(en->site, item->name, SCRIPTINFO_IUNKNOWN, &unknown, NULL)) && unknown) {
			IUnknown_QueryInterface(unknown, &IID_IDispatch, (void **)&item->object);
			IUnknown_Release(unknown);
		}
	}
	return item->object;
}

HRESULT vbs_create_object(engine *en, const WCHAR *name, IDispatch **out)
{
	CLSID clsid;
	*out = NULL;
	HRESULT hr = name[0] == L'{' ? CLSIDFromString(name, &clsid) : CLSIDFromProgID(name, &clsid);
	if (FAILED(hr))
		return hr;
	return CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, &IID_IDispatch, (void **)out);
}

static BOOL syntax_failure(const parse_error *error, script_failure *failure)
{
	failure->number = error->number ? error->number : VBS_E_SYNTAX;
	failure->description = SysAllocString(error->text);
	failure->source = SysAllocString(L"Microsoft VBScript compilation error");
	failure->line = error->line;
	failure->column = error->column;
	return FALSE;
}

BOOL vbs_execute_text(engine *en, const WCHAR *source, BOOL expression, VARIANT *result, script_failure *failure)
{
	parse_error error = { 0 };
	script_code *code = vbs_parse(source, expression, &error);
	if (!code)
		return syntax_failure(&error, failure);
	vbs_add_code(en, code);
	if (expression)
		return vbs_evaluate(en, code, result, failure);
	return vbs_run(en, code, failure);
}

/* Runs one piece of parsed code at global level, telling the site of an error nothing caught. */
static HRESULT run_code(engine *en, script_code *code, script_context context, ULONG start_line, VARIANT *result)
{
	script_failure failure = { 0 };
	script_code *outer = en->running;
	script_context outer_context = en->context;
	ULONG outer_line = en->start_line;
	en->running = code;
	en->context = context;
	en->start_line = start_line;
	if (en->site)
		IActiveScriptSite_OnEnterScript(en->site);
	BOOL fine = result ? vbs_evaluate(en, code, result, &failure) : vbs_run(en, code, &failure);
	if (en->site)
		IActiveScriptSite_OnLeaveScript(en->site);
	HRESULT hr = fine ? S_OK : vbs_report(en, &failure);
	vbs_failure_clear(&failure);
	en->running = outer;
	en->context = outer_context;
	en->start_line = outer_line;
	return hr;
}

static void run_pending(engine *en)
{
	/* What arrives while the pending code runs waits for the next start. */
	UINT count = en->pending_count;
	script_code **pending = en->pending;
	en->pending = NULL;
	en->pending_count = 0;
	for (UINT i = 0; i < count; i++)
		run_code(en, pending[i], 0, 0, NULL);
	vbs_free(pending);
}

static void change_state(engine *en, SCRIPTSTATE state)
{
	en->state = state;
	if (en->site)
		IActiveScriptSite_OnStateChange(en->site, state);
}

/* The script's globals as an IDispatch: procedures and variables by name. */

#define GLOBAL_PROCEDURE_IDS 0x10000
#define GLOBAL_VARIABLE_IDS 0x20000

typedef struct {
	IDispatch IDispatch_iface;
	LONG refs;
	engine *en;
} global_dispatch;

static global_dispatch *global_impl(IDispatch *iface) { return CONTAINING_RECORD(iface, global_dispatch, IDispatch_iface); }

static HRESULT WINAPI global_QueryInterface(IDispatch *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch)) {
		*out = iface;
		IDispatch_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI global_AddRef(IDispatch *iface)
{
	global_dispatch *global = global_impl(iface);
	LONG refs = InterlockedIncrement(&global->refs);
	if (refs == 1)
		IActiveScript_AddRef(&global->en->IActiveScript_iface);
	return refs;
}

static ULONG WINAPI global_Release(IDispatch *iface)
{
	global_dispatch *global = global_impl(iface);
	LONG refs = InterlockedDecrement(&global->refs);
	/* The engine owns the object; references to it keep the engine alive. */
	if (!refs)
		IActiveScript_Release(&global->en->IActiveScript_iface);
	return refs;
}

static HRESULT WINAPI global_GetTypeInfoCount(IDispatch *iface, UINT *count)
{
	*count = 0;
	return S_OK;
}

static HRESULT WINAPI global_GetTypeInfo(IDispatch *iface, UINT index, LCID lcid, ITypeInfo **info)
{
	*info = NULL;
	return DISP_E_BADINDEX;
}

/* A global variable's place counted from the oldest, which stays the same as new ones come in front of the list. */
static variable *variable_at(engine *en, DISPID id)
{
	UINT total = 0;
	for (variable *v = en->globals; v; v = v->next)
		total++;
	UINT index = 0;
	for (variable *v = en->globals; v; v = v->next, index++)
		if ((UINT)(id - GLOBAL_VARIABLE_IDS) == total - 1 - index)
			return v;
	return NULL;
}

static HRESULT WINAPI global_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	engine *en = global_impl(iface)->en;
	for (UINT n = 0; n < count; n++)
		ids[n] = DISPID_UNKNOWN;
	if (!count)
		return S_OK;
	for (UINT i = en->procedure_count; i-- > 0;)
		if (vbs_same(en->procedures[i]->name, names[0])) {
			ids[0] = GLOBAL_PROCEDURE_IDS + i;
			return count == 1 ? S_OK : DISP_E_UNKNOWNNAME;
		}
	UINT total = 0;
	for (variable *v = en->globals; v; v = v->next)
		total++;
	UINT index = 0;
	for (variable *v = en->globals; v; v = v->next, index++)
		if (vbs_same(v->name, names[0])) {
			ids[0] = GLOBAL_VARIABLE_IDS + (total - 1 - index);
			return count == 1 ? S_OK : DISP_E_UNKNOWNNAME;
		}
	return DISP_E_UNKNOWNNAME;
}

static HRESULT WINAPI global_Invoke(IDispatch *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result,
	EXCEPINFO *info, UINT *bad)
{
	engine *en = global_impl(iface)->en;
	UINT count = params ? params->cArgs : 0;
	if (result)
		VariantInit(result);
	if (id >= GLOBAL_VARIABLE_IDS) {
		variable *v = variable_at(en, id);
		if (!v)
			return DISP_E_MEMBERNOTFOUND;
		VARIANT *place = V_VT(&v->value) == (VT_BYREF | VT_VARIANT) ? V_VARIANTREF(&v->value) : &v->value;
		if (flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) {
			if (count != 1)
				return DISP_E_BADPARAMCOUNT;
			if (v->constant)
				return DISP_E_EXCEPTION;
			VariantClear(place);
			return VariantCopyInd(place, &params->rgvarg[0]);
		}
		return result ? VariantCopy(result, place) : S_OK;
	}
	if (id < GLOBAL_PROCEDURE_IDS || (UINT)(id - GLOBAL_PROCEDURE_IDS) >= en->procedure_count)
		return DISP_E_MEMBERNOTFOUND;
	procedure *proc = en->procedures[id - GLOBAL_PROCEDURE_IDS];
	VARIANT *args = count ? vbs_alloc(count * sizeof(VARIANT)) : NULL;
	for (UINT i = 0; i < count; i++)
		args[i] = params->rgvarg[count - 1 - i];
	script_failure failure = { 0 };
	VARIANT value;
	VariantInit(&value);
	if (en->site)
		IActiveScriptSite_OnEnterScript(en->site);
	BOOL fine = vbs_call_global(en, proc->name, args, count, &value, &failure);
	if (en->site)
		IActiveScriptSite_OnLeaveScript(en->site);
	vbs_free(args);
	if (!fine) {
		if (info) {
			memset(info, 0, sizeof(*info));
			LONG number = failure.number;
			info->scode = number > 0 && number <= 0xffff ? MAKE_HRESULT(SEVERITY_ERROR, FACILITY_CONTROL, number) : number;
			info->bstrDescription = SysAllocString(failure.description);
			info->bstrSource = SysAllocString(failure.source);
		}
		vbs_failure_clear(&failure);
		return DISP_E_EXCEPTION;
	}
	if (result)
		*result = value;
	else
		VariantClear(&value);
	return S_OK;
}

static IDispatchVtbl global_vtbl = {
	global_QueryInterface, global_AddRef, global_Release, global_GetTypeInfoCount, global_GetTypeInfo, global_GetIDsOfNames, global_Invoke,
};

/* What GetRef hands out: an object whose default method calls one of the script's procedures, as an event handler is called. */

typedef struct {
	IDispatch IDispatch_iface;
	LONG refs;
	engine *en;
	DISPID id;
} function_ref;

static function_ref *ref_impl(IDispatch *iface) { return CONTAINING_RECORD(iface, function_ref, IDispatch_iface); }

static ULONG WINAPI ref_AddRef(IDispatch *iface) { return InterlockedIncrement(&ref_impl(iface)->refs); }

static ULONG WINAPI ref_Release(IDispatch *iface)
{
	function_ref *ref = ref_impl(iface);
	LONG refs = InterlockedDecrement(&ref->refs);
	if (!refs) {
		IActiveScript_Release(&ref->en->IActiveScript_iface);
		vbs_free(ref);
	}
	return refs;
}

static HRESULT WINAPI ref_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	for (UINT n = 0; n < count; n++)
		ids[n] = DISPID_UNKNOWN;
	return DISP_E_UNKNOWNNAME;
}

static HRESULT WINAPI ref_Invoke(IDispatch *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result,
	EXCEPINFO *info, UINT *bad)
{
	function_ref *ref = ref_impl(iface);
	if (id != DISPID_VALUE || (flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)))
		return DISP_E_MEMBERNOTFOUND;
	return global_Invoke(ref->en->global_dispatch, ref->id, riid, lcid, DISPATCH_METHOD, params, result, info, bad);
}

static IDispatchVtbl ref_vtbl = {
	global_QueryInterface, ref_AddRef, ref_Release, global_GetTypeInfoCount, global_GetTypeInfo, ref_GetIDsOfNames, ref_Invoke,
};

IDispatch *vbs_function_ref(engine *en, const WCHAR *name)
{
	for (UINT i = en->procedure_count; i-- > 0;)
		if (vbs_same(en->procedures[i]->name, name)) {
			function_ref *ref = vbs_alloc(sizeof(function_ref));
			ref->IDispatch_iface.lpVtbl = &ref_vtbl;
			ref->refs = 1;
			ref->en = en;
			ref->id = GLOBAL_PROCEDURE_IDS + i;
			IActiveScript_AddRef(&en->IActiveScript_iface);
			return &ref->IDispatch_iface;
		}
	return NULL;
}

/* IActiveScript. */

static engine *script_impl(IActiveScript *iface) { return CONTAINING_RECORD(iface, engine, IActiveScript_iface); }

static HRESULT WINAPI script_QueryInterface(IActiveScript *iface, REFIID riid, void **out)
{
	engine *en = script_impl(iface);
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IActiveScript))
		*out = &en->IActiveScript_iface;
	else if (IsEqualIID(riid, &IID_IActiveScriptParse))
		*out = &en->IActiveScriptParse_iface;
	else if (IsEqualIID(riid, &IID_IObjectSafety))
		*out = &en->IObjectSafety_iface;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static ULONG WINAPI script_AddRef(IActiveScript *iface) { return InterlockedIncrement(&script_impl(iface)->refs); }

static void release_items(engine *en)
{
	for (UINT i = 0; i < en->item_count; i++) {
		if (en->items[i].object)
			IDispatch_Release(en->items[i].object);
		vbs_free(en->items[i].name);
	}
	vbs_free(en->items);
	en->items = NULL;
	en->item_count = 0;
}

static void close_engine(engine *en)
{
	/* Variables go first: objects the script holds may run Class_Terminate, which needs the code still there. */
	vbs_runtime_free(en);
	release_items(en);
	/* Pending code is in the kept list too, which frees it. */
	vbs_free(en->pending);
	en->pending = NULL;
	en->pending_count = 0;
	for (UINT i = 0; i < en->kept_count; i++)
		vbs_code_free(en->kept[i]);
	vbs_free(en->kept);
	en->kept = NULL;
	en->kept_count = 0;
	vbs_free(en->procedures);
	en->procedures = NULL;
	en->procedure_count = 0;
	vbs_free(en->classes);
	en->classes = NULL;
	en->class_count = 0;
	vbs_failure_clear(&en->err);
	if (en->site) {
		IActiveScriptSite_Release(en->site);
		en->site = NULL;
	}
}

static ULONG WINAPI script_Release(IActiveScript *iface)
{
	engine *en = script_impl(iface);
	LONG refs = InterlockedDecrement(&en->refs);
	if (!refs) {
		close_engine(en);
		vbs_free(en->global_dispatch);
		vbs_free(en);
		InterlockedDecrement(&module_locks);
	}
	return refs;
}

static HRESULT WINAPI script_SetScriptSite(IActiveScript *iface, IActiveScriptSite *site)
{
	engine *en = script_impl(iface);
	if (!site)
		return E_POINTER;
	if (en->site)
		return E_UNEXPECTED;
	IActiveScriptSite_AddRef(site);
	en->site = site;
	if (en->state == SCRIPTSTATE_UNINITIALIZED)
		change_state(en, SCRIPTSTATE_INITIALIZED);
	return S_OK;
}

static HRESULT WINAPI script_GetScriptSite(IActiveScript *iface, REFIID riid, void **out)
{
	engine *en = script_impl(iface);
	if (!en->site) {
		*out = NULL;
		return S_FALSE;
	}
	return IActiveScriptSite_QueryInterface(en->site, riid, out);
}

static HRESULT WINAPI script_SetScriptState(IActiveScript *iface, SCRIPTSTATE state)
{
	engine *en = script_impl(iface);
	if (en->state == SCRIPTSTATE_CLOSED)
		return E_UNEXPECTED;
	switch (state) {
	case SCRIPTSTATE_STARTED:
	case SCRIPTSTATE_CONNECTED:
		if (!en->site)
			return E_UNEXPECTED;
		if (en->state != state)
			change_state(en, state);
		run_pending(en);
		return S_OK;
	case SCRIPTSTATE_INITIALIZED:
	case SCRIPTSTATE_DISCONNECTED:
		change_state(en, state);
		return S_OK;
	case SCRIPTSTATE_UNINITIALIZED:
		/* Back to before the site: the code stays, the state of its run goes. */
		vbs_runtime_free(en);
		release_items(en);
		if (en->site) {
			IActiveScriptSite_OnStateChange(en->site, state);
			IActiveScriptSite_Release(en->site);
			en->site = NULL;
		}
		en->state = state;
		return S_OK;
	case SCRIPTSTATE_CLOSED:
		return IActiveScript_Close(iface);
	}
	return E_INVALIDARG;
}

static HRESULT WINAPI script_GetScriptState(IActiveScript *iface, SCRIPTSTATE *state)
{
	if (!state)
		return E_POINTER;
	*state = script_impl(iface)->state;
	return S_OK;
}

static HRESULT WINAPI script_Close(IActiveScript *iface)
{
	engine *en = script_impl(iface);
	if (en->site)
		IActiveScriptSite_OnStateChange(en->site, SCRIPTSTATE_CLOSED);
	close_engine(en);
	en->state = SCRIPTSTATE_CLOSED;
	return S_OK;
}

static HRESULT WINAPI script_AddNamedItem(IActiveScript *iface, LPCOLESTR name, DWORD flags)
{
	engine *en = script_impl(iface);
	if (!name)
		return E_POINTER;
	if (en->state == SCRIPTSTATE_CLOSED || en->state == SCRIPTSTATE_UNINITIALIZED)
		return E_UNEXPECTED;
	en->items = vbs_grow(en->items, (en->item_count + 1) * sizeof(named_item));
	named_item *item = &en->items[en->item_count++];
	*item = (named_item){ .name = vbs_copy(name), .flags = flags, .global_members = (flags & SCRIPTITEM_GLOBALMEMBERS) != 0 };
	return S_OK;
}

static HRESULT WINAPI script_AddTypeLib(IActiveScript *iface, REFGUID library, DWORD major, DWORD minor, DWORD flags)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI script_GetScriptDispatch(IActiveScript *iface, LPCOLESTR item, IDispatch **out)
{
	engine *en = script_impl(iface);
	if (!out)
		return E_POINTER;
	if (en->state == SCRIPTSTATE_CLOSED || !en->site) {
		*out = NULL;
		return E_UNEXPECTED;
	}
	*out = en->global_dispatch;
	IDispatch_AddRef(*out);
	return S_OK;
}

static HRESULT WINAPI script_GetCurrentScriptThreadID(IActiveScript *iface, SCRIPTTHREADID *id)
{
	*id = GetCurrentThreadId();
	return S_OK;
}

static HRESULT WINAPI script_GetScriptThreadID(IActiveScript *iface, DWORD thread, SCRIPTTHREADID *id)
{
	*id = thread;
	return S_OK;
}

static HRESULT WINAPI script_GetScriptThreadState(IActiveScript *iface, SCRIPTTHREADID id, SCRIPTTHREADSTATE *state)
{
	*state = script_impl(iface)->depth ? SCRIPTTHREADSTATE_RUNNING : SCRIPTTHREADSTATE_NOTINSCRIPT;
	return S_OK;
}

static HRESULT WINAPI script_InterruptScriptThread(IActiveScript *iface, SCRIPTTHREADID id, const EXCEPINFO *info, DWORD flags)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI script_Clone(IActiveScript *iface, IActiveScript **out)
{
	*out = NULL;
	return E_NOTIMPL;
}

static IActiveScriptVtbl script_vtbl = {
	script_QueryInterface, script_AddRef, script_Release, script_SetScriptSite, script_GetScriptSite, script_SetScriptState,
	script_GetScriptState, script_Close, script_AddNamedItem, script_AddTypeLib, script_GetScriptDispatch,
	script_GetCurrentScriptThreadID, script_GetScriptThreadID, script_GetScriptThreadState, script_InterruptScriptThread, script_Clone,
};

/* IActiveScriptParse. */

static engine *parse_impl(IActiveScriptParse *iface) { return CONTAINING_RECORD(iface, engine, IActiveScriptParse_iface); }

static HRESULT WINAPI parse_QueryInterface(IActiveScriptParse *iface, REFIID riid, void **out)
{
	return IActiveScript_QueryInterface(&parse_impl(iface)->IActiveScript_iface, riid, out);
}

static ULONG WINAPI parse_AddRef(IActiveScriptParse *iface) { return IActiveScript_AddRef(&parse_impl(iface)->IActiveScript_iface); }
static ULONG WINAPI parse_Release(IActiveScriptParse *iface) { return IActiveScript_Release(&parse_impl(iface)->IActiveScript_iface); }

static HRESULT WINAPI parse_InitNew(IActiveScriptParse *iface)
{
	engine *en = parse_impl(iface);
	if (en->state == SCRIPTSTATE_CLOSED)
		return E_UNEXPECTED;
	return S_OK;
}

static HRESULT WINAPI parse_AddScriptlet(IActiveScriptParse *iface, LPCOLESTR default_name, LPCOLESTR code, LPCOLESTR item,
	LPCOLESTR subitem, LPCOLESTR event, LPCOLESTR delimiter, script_context context, ULONG start_line, DWORD flags, BSTR *name, EXCEPINFO *info)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI parse_ParseScriptText(IActiveScriptParse *iface, LPCOLESTR text, LPCOLESTR item, IUnknown *context_object,
	LPCOLESTR delimiter, script_context context, ULONG start_line, DWORD flags, VARIANT *result, EXCEPINFO *info)
{
	engine *en = parse_impl(iface);
	if (en->state == SCRIPTSTATE_CLOSED || en->state == SCRIPTSTATE_UNINITIALIZED)
		return E_UNEXPECTED;
	BOOL expression = (flags & SCRIPTTEXT_ISEXPRESSION) != 0;
	parse_error error = { 0 };
	script_code *code = vbs_parse(text ? text : L"", expression, &error);
	if (!code) {
		script_failure failure = { 0 };
		syntax_failure(&error, &failure);
		en->context = context;
		en->start_line = start_line;
		script_code source = { .source = (WCHAR *)text };
		script_code *outer = en->running;
		en->running = &source;
		HRESULT hr = vbs_report(en, &failure);
		en->running = outer;
		vbs_failure_clear(&failure);
		return hr;
	}
	vbs_add_code(en, code);
	if (result)
		VariantInit(result);
	/* An expression is evaluated at once; statements run at once in a started engine and wait for the start otherwise. */
	if (expression) {
		VARIANT ignored;
		VariantInit(&ignored);
		HRESULT hr = run_code(en, code, context, start_line, result ? result : &ignored);
		VariantClear(&ignored);
		return hr;
	}
	if (en->state == SCRIPTSTATE_STARTED || en->state == SCRIPTSTATE_CONNECTED || en->state == SCRIPTSTATE_DISCONNECTED)
		return run_code(en, code, context, start_line, NULL);
	en->pending = vbs_grow(en->pending, (en->pending_count + 1) * sizeof(script_code *));
	en->pending[en->pending_count++] = code;
	return S_OK;
}

static IActiveScriptParseVtbl parse_vtbl = {
	parse_QueryInterface, parse_AddRef, parse_Release, parse_InitNew, parse_AddScriptlet, parse_ParseScriptText,
};

/* IObjectSafety: the engine accepts any safety options and keeps them. */

static engine *safety_impl(IObjectSafety *iface) { return CONTAINING_RECORD(iface, engine, IObjectSafety_iface); }

static HRESULT WINAPI safety_QueryInterface(IObjectSafety *iface, REFIID riid, void **out)
{
	return IActiveScript_QueryInterface(&safety_impl(iface)->IActiveScript_iface, riid, out);
}

static ULONG WINAPI safety_AddRef(IObjectSafety *iface) { return IActiveScript_AddRef(&safety_impl(iface)->IActiveScript_iface); }
static ULONG WINAPI safety_Release(IObjectSafety *iface) { return IActiveScript_Release(&safety_impl(iface)->IActiveScript_iface); }

#define SUPPORTED_SAFETY (INTERFACESAFE_FOR_UNTRUSTED_CALLER | INTERFACESAFE_FOR_UNTRUSTED_DATA | INTERFACE_USES_DISPEX | \
	INTERFACE_USES_SECURITY_MANAGER)

static HRESULT WINAPI safety_GetInterfaceSafetyOptions(IObjectSafety *iface, REFIID riid, DWORD *supported, DWORD *enabled)
{
	if (!supported || !enabled)
		return E_POINTER;
	*supported = SUPPORTED_SAFETY;
	*enabled = safety_impl(iface)->safety;
	return S_OK;
}

static HRESULT WINAPI safety_SetInterfaceSafetyOptions(IObjectSafety *iface, REFIID riid, DWORD mask, DWORD enabled)
{
	if (mask & ~SUPPORTED_SAFETY)
		return E_FAIL;
	engine *en = safety_impl(iface);
	en->safety = (en->safety & ~mask) | (enabled & mask);
	return S_OK;
}

static IObjectSafetyVtbl safety_vtbl = {
	safety_QueryInterface, safety_AddRef, safety_Release, safety_GetInterfaceSafetyOptions, safety_SetInterfaceSafetyOptions,
};

static HRESULT create_engine(REFIID riid, void **out)
{
	engine *en = vbs_alloc(sizeof(engine));
	en->IActiveScript_iface.lpVtbl = &script_vtbl;
	en->IActiveScriptParse_iface.lpVtbl = &parse_vtbl;
	en->IObjectSafety_iface.lpVtbl = &safety_vtbl;
	en->refs = 1;
	en->state = SCRIPTSTATE_UNINITIALIZED;
	en->thread = GetCurrentThreadId();
	/* The seed Rnd starts from before any Randomize. */
	en->random_seed = 0x50000;
	global_dispatch *global = vbs_alloc(sizeof(global_dispatch));
	global->IDispatch_iface.lpVtbl = &global_vtbl;
	global->en = en;
	en->global_dispatch = &global->IDispatch_iface;
	InterlockedIncrement(&module_locks);
	HRESULT hr = IActiveScript_QueryInterface(&en->IActiveScript_iface, riid, out);
	IActiveScript_Release(&en->IActiveScript_iface);
	return hr;
}

/* The class factory and the exports COM looks for. */

static HRESULT WINAPI factory_QueryInterface(IClassFactory *iface, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef(IClassFactory *iface) { return 2; }
static ULONG WINAPI factory_Release(IClassFactory *iface) { return 1; }

static HRESULT WINAPI factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **out)
{
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	return create_engine(riid, out);
}

static HRESULT WINAPI factory_LockServer(IClassFactory *iface, BOOL lock)
{
	if (lock)
		InterlockedIncrement(&module_locks);
	else
		InterlockedDecrement(&module_locks);
	return S_OK;
}

static IClassFactoryVtbl factory_vtbl = {
	factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateInstance, factory_LockServer,
};

static IClassFactory factory = { &factory_vtbl };

/* VBScript.RegExp, the regular expression object scripts in other languages create. */
static HRESULT WINAPI regexp_factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **out)
{
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	IDispatch *regexp = vbs_regexp_new();
	HRESULT hr = IDispatch_QueryInterface(regexp, riid, out);
	IDispatch_Release(regexp);
	return hr;
}

static IClassFactoryVtbl regexp_factory_vtbl = {
	factory_QueryInterface, factory_AddRef, factory_Release, regexp_factory_CreateInstance, factory_LockServer,
};

static IClassFactory regexp_factory = { &regexp_factory_vtbl };

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
	*out = NULL;
	if (IsEqualCLSID(clsid, &CLSID_VBScript))
		return IClassFactory_QueryInterface(&factory, riid, out);
	if (IsEqualCLSID(clsid, &CLSID_VBScriptRegExp))
		return IClassFactory_QueryInterface(&regexp_factory, riid, out);
	return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return module_locks ? S_FALSE : S_OK;
}

/* The classes are registered with the rest of the system's; there is nothing to write here. */
HRESULT WINAPI DllRegisterServer(void) { return S_OK; }
HRESULT WINAPI DllUnregisterServer(void) { return S_OK; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
