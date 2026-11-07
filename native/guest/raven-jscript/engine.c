/* SPDX-License-Identifier: MIT */
#include "js.h"

/* The engine as Active Scripting hosts see it: IActiveScript to give it a site and named items and to start it, IActiveScriptParse to
 * hand it code, IObjectSafety for hosts that ask. Code parsed before the engine starts waits and runs, in order, when it does. The
 * global object is what GetScriptDispatch hands out. */

/* libuuid lacks the identifiers of the parse interfaces and of IDispatchEx; these are the ones activscp.h and dispex.h give. */
const GUID IID_IActiveScriptParse32 = { 0xbb1a2ae2, 0xa4f9, 0x11cf, { 0x8f, 0x20, 0x00, 0x80, 0x5f, 0x2c, 0xd0, 0x64 } };
const GUID IID_IActiveScriptParse64 = { 0xc7ef7658, 0xe1ee, 0x480e, { 0x97, 0xea, 0xd5, 0x2c, 0xb4, 0xd7, 0x6d, 0x17 } };
const GUID IID_IDispatchEx = { 0xa6ef9860, 0xc720, 0x11d0, { 0x93, 0x37, 0x00, 0xa0, 0xc9, 0x0d, 0xca, 0xa9 } };
static const CLSID CLSID_JScript = { 0xf414c260, 0x6ac0, 0x11cf, { 0xb6, 0xd1, 0x00, 0xaa, 0x00, 0xbb, 0xbb, 0x58 } };

static LONG module_locks;

/* The runtime: the prototypes, the global object and the built-ins in it. */

void js_init_runtime(engine *en)
{
	en->object_prototype = js_object_new(en, JC_OBJECT, NULL);
	en->function_prototype = js_object_new(en, JC_FUNCTION, en->object_prototype);
	en->array_prototype = js_object_new(en, JC_ARRAY, en->object_prototype);
	en->string_prototype = js_object_new(en, JC_STRING, en->object_prototype);
	en->number_prototype = js_object_new(en, JC_NUMBER, en->object_prototype);
	en->boolean_prototype = js_object_new(en, JC_BOOLEAN, en->object_prototype);
	en->date_prototype = js_object_new(en, JC_DATE, en->object_prototype);
	en->regexp_prototype = js_object_new(en, JC_OBJECT, en->object_prototype);
	en->enumerator_prototype = js_object_new(en, JC_OBJECT, en->object_prototype);
	en->vbarray_prototype = js_object_new(en, JC_OBJECT, en->object_prototype);
	en->error_prototypes[0] = js_object_new(en, JC_OBJECT, en->object_prototype);
	for (UINT i = 1; i < ARRAYSIZE(en->error_prototypes); i++)
		en->error_prototypes[i] = js_object_new(en, JC_OBJECT, en->error_prototypes[0]);
	en->error_prototype = js_object_retain(en->error_prototypes[0]);
	en->global = js_object_new(en, JC_GLOBAL, en->object_prototype);
	js_init_object(en);
	js_init_function(en);
	js_init_array(en);
	js_init_string(en);
	js_init_number(en);
	js_init_boolean(en);
	js_init_math(en);
	js_init_date(en);
	js_init_regexp(en);
	js_init_error(en);
	js_init_global(en);
	js_init_host(en);
}

void js_free_runtime(engine *en)
{
	if (!en->global)
		return;
	en->closing = TRUE;
	if (en->throwing) {
		js_release(en->exception);
		en->exception = js_undefined;
		en->throwing = FALSE;
	}
	/* Emptying every object first breaks the cycles between them; the references below then free them. */
	js_free_objects(en);
	js_object **own[] = { &en->global, &en->object_prototype, &en->function_prototype, &en->array_prototype, &en->string_prototype,
		&en->number_prototype, &en->boolean_prototype, &en->date_prototype, &en->regexp_prototype, &en->error_prototype,
		&en->enumerator_prototype, &en->vbarray_prototype, &en->regexp_constructor };
	for (UINT i = 0; i < ARRAYSIZE(own); i++) {
		js_object_release(*own[i]);
		*own[i] = NULL;
	}
	for (UINT i = 0; i < ARRAYSIZE(en->error_prototypes); i++) {
		js_object_release(en->error_prototypes[i]);
		en->error_prototypes[i] = NULL;
	}
	for (UINT i = 0; i < en->dispatch_name_count; i++)
		js_string_release(en->dispatch_names[i]);
	js_free(en->dispatch_names);
	en->dispatch_names = NULL;
	en->dispatch_name_count = 0;
	js_string_release(en->last_input);
	en->last_input = NULL;
	js_free(en->last_spans);
	en->last_spans = NULL;
	en->closing = FALSE;
}

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
		js_failure_clear(&error->failure);
		SysFreeString(error->line_text);
		js_free(error);
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
	info->scode = number > 0 && number <= 0xffff ? JS_HRESULT(number) : number;
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

HRESULT js_report(engine *en, const script_failure *failure)
{
	if (!en->site)
		return SCRIPT_E_REPORTED;
	script_error *error = js_alloc(sizeof(script_error));
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

void js_failure_of_exception(engine *en, script_failure *failure)
{
	js_value v = en->exception;
	en->exception = js_undefined;
	en->throwing = FALSE;
	failure->line = en->exception_line;
	failure->column = en->exception_column;
	if (v.type == JV_OBJECT && v.object->cls == JC_ERROR) {
		js_value number = js_undefined;
		double n = 0;
		if (js_get_text(en, v.object, L"number", &number))
			js_to_number(en, number, &n);
		js_release(number);
		js_string *text = js_error_message(en, v.object);
		failure->number = n ? (LONG)(ULONG)(LONGLONG)n : JS_HRESULT(JS_E_NOT_CAUGHT);
		failure->description = SysAllocStringLen(text->text, text->length);
		js_string_release(text);
		/* A syntax error that eval or new Function met is a compilation error, as one in the code itself is. */
		BOOL syntax = v.object->proto == en->error_prototypes[4];
		failure->source = SysAllocString(syntax ? L"Microsoft JScript compilation error" : L"Microsoft JScript runtime error");
	} else {
		failure->number = JS_HRESULT(JS_E_NOT_CAUGHT);
		failure->description = SysAllocString(js_error_text(JS_E_NOT_CAUGHT));
		failure->source = SysAllocString(L"Microsoft JScript runtime error");
	}
	/* Something converting the value may have thrown again; that is not reported. */
	if (en->throwing) {
		js_release(en->exception);
		en->exception = js_undefined;
		en->throwing = FALSE;
	}
	js_release(v);
}

/* Named items. */

IDispatch *js_item_object(engine *en, UINT index)
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

static void release_items(engine *en)
{
	for (UINT i = 0; i < en->item_count; i++) {
		if (en->items[i].object)
			IDispatch_Release(en->items[i].object);
		js_free(en->items[i].name);
		js_string_release(en->items[i].key);
	}
	js_free(en->items);
	en->items = NULL;
	en->item_count = 0;
}

/* Running code. */

static void syntax_failure(const parse_error *error, script_failure *failure)
{
	LONG number = error->number ? error->number : JS_E_SYNTAX;
	failure->number = JS_HRESULT(number);
	failure->description = SysAllocString(js_error_text(number));
	failure->source = SysAllocString(L"Microsoft JScript compilation error");
	failure->line = error->line;
	failure->column = error->column;
}

static void keep_code(engine *en, script_code *code)
{
	code->next = en->kept;
	en->kept = code;
}

/* Runs one piece of parsed code at global level, telling the site of an exception nothing caught. */
static HRESULT run_code(engine *en, script_code *code, script_context cookie, ULONG start_line, VARIANT *result)
{
	if (!en->global)
		js_init_runtime(en);
	script_code *outer = en->running;
	script_context outer_context = en->context;
	ULONG outer_line = en->start_line;
	en->running = code;
	en->context = cookie;
	en->start_line = start_line;
	context global = { 0 };
	global.scope = js_scope_new(en->global, NULL);
	global.self = js_obj(en->global);
	global.variables = en->global;
	js_value value = js_undefined;
	if (en->site)
		IActiveScriptSite_OnEnterScript(en->site);
	BOOL fine = js_run_program(en, code->program, &global, result ? &value : NULL);
	if (en->site)
		IActiveScriptSite_OnLeaveScript(en->site);
	js_scope_release(global.scope);
	HRESULT hr = S_OK;
	if (fine && result)
		hr = js_to_variant(en, value, result);
	js_release(value);
	if (!fine) {
		script_failure failure = { 0 };
		js_failure_of_exception(en, &failure);
		hr = js_report(en, &failure);
		js_failure_clear(&failure);
	}
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
	js_free(pending);
}

static void change_state(engine *en, SCRIPTSTATE state)
{
	en->state = state;
	if (en->site)
		IActiveScriptSite_OnStateChange(en->site, state);
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

static void close_engine(engine *en)
{
	if (en->closing)
		return;
	/* Objects of the script may be released while it closes and come back through AddRef; a reference holds the engine. */
	en->refs++;
	js_free_runtime(en);
	release_items(en);
	/* Pending code is in the kept list too, which frees it. */
	js_free(en->pending);
	en->pending = NULL;
	en->pending_count = 0;
	while (en->kept) {
		script_code *next = en->kept->next;
		js_code_free(en->kept);
		en->kept = next;
	}
	if (en->site) {
		IActiveScriptSite_Release(en->site);
		en->site = NULL;
	}
	en->refs--;
}

static ULONG WINAPI script_Release(IActiveScript *iface)
{
	engine *en = script_impl(iface);
	LONG refs = InterlockedDecrement(&en->refs);
	if (!refs) {
		close_engine(en);
		js_free(en);
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
		js_free_runtime(en);
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
	en->items = js_grow(en->items, (en->item_count + 1) * sizeof(named_item));
	named_item *item = &en->items[en->item_count++];
	UINT length = lstrlenW(name);
	*item = (named_item){ .name = js_alloc((length + 1) * sizeof(WCHAR)), .key = js_string_new(name, length), .flags = flags };
	memcpy(item->name, name, (length + 1) * sizeof(WCHAR));
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
	*out = NULL;
	if (en->state == SCRIPTSTATE_CLOSED || !en->site)
		return E_UNEXPECTED;
	if (!en->global)
		js_init_runtime(en);
	/* Made on each call: the engine keeping it would be a cycle through the reference it holds on the engine. */
	*out = js_global_dispatch_new(en);
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
	script_code *code = js_parse(text ? text : L"", expression, &error);
	if (result)
		VariantInit(result);
	if (!code) {
		script_failure failure = { 0 };
		syntax_failure(&error, &failure);
		en->context = context;
		en->start_line = start_line;
		script_code source = { .source = (WCHAR *)text };
		script_code *outer = en->running;
		en->running = &source;
		HRESULT hr = js_report(en, &failure);
		en->running = outer;
		js_failure_clear(&failure);
		return hr;
	}
	keep_code(en, code);
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
	en->pending = js_grow(en->pending, (en->pending_count + 1) * sizeof(script_code *));
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
	engine *en = js_alloc(sizeof(engine));
	en->IActiveScript_iface.lpVtbl = &script_vtbl;
	en->IActiveScriptParse_iface.lpVtbl = &parse_vtbl;
	en->IObjectSafety_iface.lpVtbl = &safety_vtbl;
	en->refs = 1;
	en->state = SCRIPTSTATE_UNINITIALIZED;
	en->thread = GetCurrentThreadId();
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

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
	*out = NULL;
	if (IsEqualCLSID(clsid, &CLSID_JScript))
		return IClassFactory_QueryInterface(&factory, riid, out);
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
