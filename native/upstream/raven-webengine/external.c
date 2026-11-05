/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* window.external for the WebBrowser control's pages: the object the control's site offers pages, as Internet Explorer gives it to them.
 *
 * Pages run in this process on a thread of the page engine, while the program's object belongs to the control's thread. A page's use of
 * the object becomes a call sent to the control's window: the page waits while the control's thread asks the object through IDispatch,
 * and the answer comes back converted to a script value. Objects the program returns reach the page the same way; their references stay
 * with the control's thread, which drops each one once the page has let go of it. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <dispex.h>
#include "engine_cef.h"

static const IID null_iid = {0};
static const IID dispatch_iid = {0x00020400, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID dispatch_ex_iid = {0xa6ef9860, 0xc720, 0x11d0, {0x93, 0x37, 0x00, 0xa0, 0xc9, 0x0d, 0xca, 0xa9}};

/* The control's window of each browser, by the browser's identifier, from the extra information the browser was created with. */
#define BROWSERS 32

static struct {
	int id;
	HWND window;
} browsers[BROWSERS];
static SRWLOCK browsers_lock = SRWLOCK_INIT;

static void remember(int id, HWND window)
{
	AcquireSRWLockExclusive(&browsers_lock);
	for (int i = 0; i < BROWSERS; i++)
		if (!browsers[i].window || browsers[i].id == id) {
			browsers[i].id = id;
			browsers[i].window = window;
			break;
		}
	ReleaseSRWLockExclusive(&browsers_lock);
}

static void forget(int id)
{
	AcquireSRWLockExclusive(&browsers_lock);
	for (int i = 0; i < BROWSERS; i++)
		if (browsers[i].window && browsers[i].id == id)
			browsers[i].window = NULL;
	ReleaseSRWLockExclusive(&browsers_lock);
}

static HWND window_of(cef_browser_t *browser)
{
	const int id = browser->get_identifier(browser);
	HWND window = NULL;
	AcquireSRWLockShared(&browsers_lock);
	for (int i = 0; i < BROWSERS; i++)
		if (browsers[i].window && browsers[i].id == id)
			window = browsers[i].window;
	ReleaseSRWLockShared(&browsers_lock);
	return window;
}

/* One of the program's objects as a page holds it. */
typedef struct holder {
	cef_base_ref_counted_t base;
	LONG refs;
	HWND window;
	IDispatch *dispatch;
} holder;

static holder *holder_from(cef_base_ref_counted_t *self)
{
	return CONTAINING_RECORD(self, holder, base);
}

static void CEF_CALLBACK holder_add_ref(cef_base_ref_counted_t *self)
{
	InterlockedIncrement(&holder_from(self)->refs);
}

static int CEF_CALLBACK holder_release(cef_base_ref_counted_t *self)
{
	holder *owner = holder_from(self);
	if (InterlockedDecrement(&owner->refs))
		return 0;
	// The reference belongs to the control's thread; without the window it stays, as releasing it here could run the object's code
	// on the wrong thread.
	PostMessageW(owner->window, ENGINE_DISPATCH, 1, (LPARAM)owner->dispatch);
	HeapFree(GetProcessHeap(), 0, owner);
	return 1;
}

static int CEF_CALLBACK holder_one_ref(cef_base_ref_counted_t *self)
{
	return holder_from(self)->refs == 1;
}

static int CEF_CALLBACK holder_some_ref(cef_base_ref_counted_t *self)
{
	return holder_from(self)->refs >= 1;
}

/* The holder behind a page's value, with a reference for the caller, when the value is one of the program's objects. */
static holder *holder_of(cef_v8_value_t *value)
{
	if (!value->is_object(value))
		return NULL;
	cef_base_ref_counted_t *data = value->get_user_data(value);
	if (!data)
		return NULL;
	if (data->add_ref != holder_add_ref) {
		engine_release(data);
		return NULL;
	}
	return holder_from(data);
}

/* A call a page makes to one of the program's objects, carried out on the control's thread. */
enum { call_member, call_invoke, call_put };

typedef struct call {
	int kind;
	IDispatch *dispatch;
	const WCHAR *name;
	DISPID member;
	VARIANT *arguments;
	UINT count;
	VARIANT result;
	BOOL method;
	HRESULT status;
	WCHAR error[256];
} call;

/* On the control's thread: an answer reduced to what a page's values can hold. */
static void settle(VARIANT *value)
{
	if (V_VT(value) & VT_BYREF) {
		VARIANT plain;
		VariantInit(&plain);
		const HRESULT status = VariantCopyInd(&plain, value);
		VariantClear(value);
		if (FAILED(status))
			return;
		*value = plain;
	}
	switch (V_VT(value)) {
	case VT_EMPTY:
	case VT_NULL:
	case VT_BOOL:
	case VT_I1:
	case VT_I2:
	case VT_I4:
	case VT_INT:
	case VT_UI1:
	case VT_UI2:
	case VT_UI4:
	case VT_UINT:
	case VT_R8:
	case VT_BSTR:
	case VT_DISPATCH:
		return;
	case VT_UNKNOWN: {
		IDispatch *dispatch = NULL;
		if (V_UNKNOWN(value) && FAILED(IUnknown_QueryInterface(V_UNKNOWN(value), &dispatch_iid, (void **)&dispatch)))
			dispatch = NULL;
		VariantClear(value);
		if (dispatch) {
			V_VT(value) = VT_DISPATCH;
			V_DISPATCH(value) = dispatch;
		}
		return;
	}
	default:
		if (FAILED(VariantChangeType(value, value, 0, VT_R8)) && FAILED(VariantChangeType(value, value, 0, VT_BSTR)))
			VariantClear(value);
	}
}

static void invoke(call *request, WORD flags)
{
	DISPID put = DISPID_PROPERTYPUT;
	DISPPARAMS parameters = {request->arguments, NULL, request->count, 0};
	if (flags == DISPATCH_PROPERTYPUT) {
		parameters.rgdispidNamedArgs = &put;
		parameters.cNamedArgs = 1;
	}
	EXCEPINFO failure = {0};
	UINT wrong = 0;
	VariantInit(&request->result);
	VARIANT *result = flags == DISPATCH_PROPERTYPUT ? NULL : &request->result;
	IDispatchEx *ex = NULL;
	if (SUCCEEDED(IDispatch_QueryInterface(request->dispatch, &dispatch_ex_iid, (void **)&ex)) && ex) {
		request->status = IDispatchEx_InvokeEx(ex, request->member, LOCALE_USER_DEFAULT, flags, &parameters, result, &failure, NULL);
		IDispatchEx_Release(ex);
	} else
		request->status = IDispatch_Invoke(request->dispatch, request->member, &null_iid, LOCALE_USER_DEFAULT, flags, &parameters, result, &failure,
			&wrong);
	if (request->status == DISP_E_EXCEPTION) {
		if (failure.pfnDeferredFillIn)
			failure.pfnDeferredFillIn(&failure);
		if (failure.bstrDescription)
			lstrcpynW(request->error, failure.bstrDescription, ARRAYSIZE(request->error));
		if (FAILED(failure.scode))
			request->status = failure.scode;
		SysFreeString(failure.bstrSource);
		SysFreeString(failure.bstrDescription);
		SysFreeString(failure.bstrHelpFile);
	}
	if (FAILED(request->status))
		VariantClear(&request->result);
	else
		settle(&request->result);
}

/* A member by its name, as Internet Explorer's scripts look it up: through IDispatchEx, matching case, when the object has it. Objects
 * whose members are found by reflection answer only there. */
static HRESULT member_id(IDispatch *dispatch, const WCHAR *name, DISPID *member)
{
	IDispatchEx *ex = NULL;
	if (SUCCEEDED(IDispatch_QueryInterface(dispatch, &dispatch_ex_iid, (void **)&ex)) && ex) {
		BSTR text = SysAllocString(name);
		const HRESULT status = text ? IDispatchEx_GetDispID(ex, text, fdexNameCaseSensitive, member) : E_OUTOFMEMORY;
		SysFreeString(text);
		IDispatchEx_Release(ex);
		if (SUCCEEDED(status))
			return status;
	}
	LPOLESTR text = (LPOLESTR)name;
	return IDispatch_GetIDsOfNames(dispatch, &null_iid, &text, 1, LOCALE_USER_DEFAULT, member);
}

/* What the object's type information says a member is: 1 for a method, 2 for a property, 0 when it does not say. */
static int member_kind(IDispatch *dispatch, DISPID member)
{
	ITypeInfo *info = NULL;
	TYPEATTR *attributes = NULL;
	int kind = 0;
	IDispatchEx *ex = NULL;
	if (SUCCEEDED(IDispatch_QueryInterface(dispatch, &dispatch_ex_iid, (void **)&ex)) && ex) {
		DWORD properties = 0;
		if (SUCCEEDED(IDispatchEx_GetMemberProperties(ex, member, fdexPropCanGet | fdexPropCanCall, &properties))) {
			if ((properties & fdexPropCanCall) && !(properties & fdexPropCanGet))
				kind = 1;
			else if ((properties & fdexPropCanGet) && !(properties & fdexPropCanCall))
				kind = 2;
		}
		IDispatchEx_Release(ex);
		if (kind)
			return kind;
	}
	if (FAILED(IDispatch_GetTypeInfo(dispatch, 0, LOCALE_USER_DEFAULT, &info)) || !info)
		return 0;
	if (SUCCEEDED(ITypeInfo_GetTypeAttr(info, &attributes)) && attributes) {
		for (UINT i = 0; i < attributes->cFuncs && !kind; i++) {
			FUNCDESC *function = NULL;
			if (FAILED(ITypeInfo_GetFuncDesc(info, i, &function)) || !function)
				continue;
			if (function->memid == member)
				kind = function->invkind & INVOKE_FUNC ? 1 : 2;
			ITypeInfo_ReleaseFuncDesc(info, function);
		}
		for (UINT i = 0; i < attributes->cVars && !kind; i++) {
			VARDESC *variable = NULL;
			if (FAILED(ITypeInfo_GetVarDesc(info, i, &variable)) || !variable)
				continue;
			if (variable->memid == member)
				kind = 2;
			ITypeInfo_ReleaseVarDesc(info, variable);
		}
		ITypeInfo_ReleaseTypeAttr(info, attributes);
	}
	ITypeInfo_Release(info);
	return kind;
}

/* A member a page reads: a method becomes a function for the page, a property its value. */
static void find_member(call *request)
{
	request->status = member_id(request->dispatch, request->name, &request->member);
	if (FAILED(request->status))
		return;
	const int kind = member_kind(request->dispatch, request->member);
	if (kind == 1) {
		request->method = TRUE;
		return;
	}
	invoke(request, DISPATCH_PROPERTYGET);
	// Without type information, a member that will not be read is taken for a method.
	if (!kind && (request->status == DISP_E_MEMBERNOTFOUND || request->status == DISP_E_BADPARAMCOUNT || request->status == DISP_E_PARAMNOTOPTIONAL)) {
		request->method = TRUE;
		request->status = S_OK;
	}
}

void external_dispatch(WPARAM wparam, LPARAM lparam)
{
	if (wparam) {
		IDispatch_Release((IDispatch *)lparam);
		return;
	}
	call *request = (call *)lparam;
	switch (request->kind) {
	case call_member: {
		find_member(request);
		// What pages want of the program's objects goes to the trace, which shows what a page waits for.
		char text[64];
		wsprintfA(text, "page reads %s 0x%08lX for", request->method ? "a method" : "a value", (unsigned long)request->status);
		engine_trace(text, request->name);
		break;
	}
	case call_invoke:
		invoke(request, DISPATCH_METHOD);
		break;
	case call_put: {
		request->status = member_id(request->dispatch, request->name, &request->member);
		if (SUCCEEDED(request->status))
			invoke(request, DISPATCH_PROPERTYPUT);
		break;
	}
	}
}

/* The page's side, on the page engine's thread. */
static BOOL send(HWND window, call *request)
{
	request->status = E_FAIL;
	SendMessageW(window, ENGINE_DISPATCH, 0, (LPARAM)request);
	return SUCCEEDED(request->status);
}

static void fail(cef_string_t *exception, const call *request)
{
	WCHAR text[320];
	if (*request->error)
		lstrcpynW(text, request->error, ARRAYSIZE(text));
	else
		wsprintfW(text, L"The program's object failed with 0x%08lX", (unsigned long)request->status);
	cef.string_set((const char16_t *)text, lstrlenW(text), exception, 1);
}

static int CEF_CALLBACK member_get(cef_v8_interceptor_t *self, const cef_string_t *name, cef_v8_value_t *object, cef_v8_value_t **result,
	cef_string_t *exception);
static int CEF_CALLBACK member_set(cef_v8_interceptor_t *self, const cef_string_t *name, cef_v8_value_t *object, cef_v8_value_t *value,
	cef_string_t *exception);
static int CEF_CALLBACK no_index_get(cef_v8_interceptor_t *self, int index, cef_v8_value_t *object, cef_v8_value_t **result, cef_string_t *exception)
{
	engine_release(object);
	return 0;
}

static int CEF_CALLBACK no_index_set(cef_v8_interceptor_t *self, int index, cef_v8_value_t *object, cef_v8_value_t *value, cef_string_t *exception)
{
	engine_release(value);
	engine_release(object);
	return 0;
}

static cef_v8_interceptor_t members = {.base = STATIC_BASE(cef_v8_interceptor_t), .get_byname = member_get, .get_byindex = no_index_get,
	.set_byname = member_set, .set_byindex = no_index_set};

/* A script object for one of the program's objects; the reference passes to it. */
static cef_v8_value_t *wrap(HWND window, IDispatch *dispatch)
{
	holder *owner = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*owner));
	cef_v8_value_t *object = owner ? cef.object(NULL, &members) : NULL;
	if (!object) {
		HeapFree(GetProcessHeap(), 0, owner);
		PostMessageW(window, ENGINE_DISPATCH, 1, (LPARAM)dispatch);
		return cef.null();
	}
	owner->base.size = sizeof(owner->base);
	owner->base.add_ref = holder_add_ref;
	owner->base.release = holder_release;
	owner->base.has_one_ref = holder_one_ref;
	owner->base.has_at_least_one_ref = holder_some_ref;
	owner->refs = 1;
	owner->window = window;
	owner->dispatch = dispatch;
	object->set_user_data(object, &owner->base);
	return object;
}

/* An answer as a script value; the answer's string and object pass to the value. */
static cef_v8_value_t *to_value(HWND window, VARIANT *value)
{
	switch (V_VT(value)) {
	case VT_NULL:
		return cef.null();
	case VT_BOOL:
		return cef.boolean(V_BOOL(value) != VARIANT_FALSE);
	case VT_I1:
		return cef.integer(V_I1(value));
	case VT_I2:
		return cef.integer(V_I2(value));
	case VT_I4:
		return cef.integer(V_I4(value));
	case VT_INT:
		return cef.integer(V_INT(value));
	case VT_UI1:
		return cef.integer(V_UI1(value));
	case VT_UI2:
		return cef.integer(V_UI2(value));
	case VT_UI4:
		return cef.unsigned_integer(V_UI4(value));
	case VT_UINT:
		return cef.unsigned_integer(V_UINT(value));
	case VT_R8:
		return cef.number(V_R8(value));
	case VT_BSTR: {
		cef_string_t text = {0};
		cef.string_set((const char16_t *)(V_BSTR(value) ? V_BSTR(value) : L""), SysStringLen(V_BSTR(value)), &text, 1);
		SysFreeString(V_BSTR(value));
		V_VT(value) = VT_EMPTY;
		cef_v8_value_t *result = cef.string(&text);
		cef.string_clear(&text);
		return result;
	}
	case VT_DISPATCH: {
		IDispatch *dispatch = V_DISPATCH(value);
		V_VT(value) = VT_EMPTY;
		return dispatch ? wrap(window, dispatch) : cef.null();
	}
	default:
		return cef.undefined();
	}
}

/* A page's value as an argument; a string is the argument's own, an object of the program's is lent for the call. */
static void to_variant(cef_v8_value_t *value, VARIANT *out)
{
	VariantInit(out);
	if (value->is_null(value))
		V_VT(out) = VT_NULL;
	else if (value->is_bool(value)) {
		V_VT(out) = VT_BOOL;
		V_BOOL(out) = value->get_bool_value(value) ? VARIANT_TRUE : VARIANT_FALSE;
	} else if (value->is_int(value)) {
		V_VT(out) = VT_I4;
		V_I4(out) = value->get_int_value(value);
	} else if (value->is_uint(value)) {
		const uint32_t number = value->get_uint_value(value);
		if (number <= 0x7fffffff) {
			V_VT(out) = VT_I4;
			V_I4(out) = (LONG)number;
		} else {
			V_VT(out) = VT_R8;
			V_R8(out) = number;
		}
	} else if (value->is_double(value)) {
		V_VT(out) = VT_R8;
		V_R8(out) = value->get_double_value(value);
	} else if (value->is_string(value)) {
		WCHAR *text = engine_take_userfree(value->get_string_value(value));
		V_VT(out) = VT_BSTR;
		V_BSTR(out) = SysAllocString(text ? text : L"");
		HeapFree(GetProcessHeap(), 0, text);
	} else if (!value->is_undefined(value)) {
		holder *owner = holder_of(value);
		if (owner) {
			V_VT(out) = VT_DISPATCH;
			V_DISPATCH(out) = owner->dispatch;
			holder_release(&owner->base);
		} else
			engine_trace("a page passed the program a script object it cannot take", NULL);
	}
}

static void clear_argument(VARIANT *value)
{
	if (V_VT(value) == VT_BSTR)
		SysFreeString(V_BSTR(value));
	V_VT(value) = VT_EMPTY;
}

/* A method of one of the program's objects, as a function of the page. */
typedef struct method {
	cef_v8_handler_t handler;
	LONG refs;
	holder *owner;
	DISPID member;
} method;

static method *method_from(void *self)
{
	return CONTAINING_RECORD(self, method, handler);
}

static void CEF_CALLBACK method_add_ref(cef_base_ref_counted_t *self)
{
	InterlockedIncrement(&method_from(self)->refs);
}

static int CEF_CALLBACK method_release(cef_base_ref_counted_t *self)
{
	method *function = method_from(self);
	if (InterlockedDecrement(&function->refs))
		return 0;
	holder_release(&function->owner->base);
	HeapFree(GetProcessHeap(), 0, function);
	return 1;
}

static int CEF_CALLBACK method_one_ref(cef_base_ref_counted_t *self)
{
	return method_from(self)->refs == 1;
}

static int CEF_CALLBACK method_some_ref(cef_base_ref_counted_t *self)
{
	return method_from(self)->refs >= 1;
}

static int CEF_CALLBACK method_execute(cef_v8_handler_t *self, const cef_string_t *name, cef_v8_value_t *object, size_t count,
	cef_v8_value_t *const *arguments, cef_v8_value_t **result, cef_string_t *exception)
{
	method *function = method_from(self);
	call request = {call_invoke, function->owner->dispatch};
	request.member = function->member;
	VARIANT *list = count ? HeapAlloc(GetProcessHeap(), 0, count * sizeof(VARIANT)) : NULL;
	// IDispatch takes its arguments last first.
	for (size_t i = 0; i < count; i++) {
		if (list)
			to_variant(arguments[i], &list[count - 1 - i]);
		engine_release(arguments[i]);
	}
	engine_release(object);
	if (count && !list) {
		request.status = E_OUTOFMEMORY;
		fail(exception, &request);
		return 1;
	}
	request.arguments = list;
	request.count = (UINT)count;
	const BOOL done = send(function->owner->window, &request);
	WCHAR *called = engine_copy_text(name);
	char text[160];
	int written = wsprintfA(text, "page called with %u arguments, 0x%08lX", (unsigned)count, (unsigned long)request.status);
	for (size_t i = 0; list && i < count && i < 4 && written < 120; i++) {
		const VARIANT *value = &list[count - 1 - i];
		if (V_VT(value) == VT_I4)
			written += wsprintfA(text + written, " %ld", (long)V_I4(value));
		else if (V_VT(value) == VT_R8)
			written += wsprintfA(text + written, " %ld.", (long)V_R8(value));
		else if (V_VT(value) == VT_BSTR)
			written += wsprintfA(text + written, " '%.30ls'", V_BSTR(value));
		else
			written += wsprintfA(text + written, " vt%u", (unsigned)V_VT(value));
	}
	lstrcatA(text, ":");
	engine_trace(text, called);
	HeapFree(GetProcessHeap(), 0, called);
	for (size_t i = 0; i < count; i++)
		clear_argument(&list[i]);
	HeapFree(GetProcessHeap(), 0, list);
	if (!done) {
		fail(exception, &request);
		return 1;
	}
	*result = to_value(function->owner->window, &request.result);
	return 1;
}

static cef_v8_value_t *method_value(holder *owner, DISPID member, const cef_string_t *name)
{
	method *function = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*function));
	if (!function)
		return cef.undefined();
	function->handler.base.size = sizeof(function->handler);
	function->handler.base.add_ref = method_add_ref;
	function->handler.base.release = method_release;
	function->handler.base.has_one_ref = method_one_ref;
	function->handler.base.has_at_least_one_ref = method_some_ref;
	function->handler.execute = method_execute;
	function->refs = 1;
	function->owner = owner;
	function->member = member;
	holder_add_ref(&owner->base);
	return cef.function(name, &function->handler);
}

static int CEF_CALLBACK member_get(cef_v8_interceptor_t *self, const cef_string_t *name, cef_v8_value_t *object, cef_v8_value_t **result,
	cef_string_t *exception)
{
	holder *owner = holder_of(object);
	engine_release(object);
	if (!owner)
		return 0;
	WCHAR *text = engine_copy_text(name);
	call request = {call_member, owner->dispatch, text};
	int handled = 0;
	if (text && send(owner->window, &request)) {
		*result = request.method ? method_value(owner, request.member, name) : to_value(owner->window, &request.result);
		handled = 1;
	} else if (request.status == DISP_E_EXCEPTION || *request.error) {
		fail(exception, &request);
		handled = 1;
	}
	HeapFree(GetProcessHeap(), 0, text);
	holder_release(&owner->base);
	return handled;
}

static int CEF_CALLBACK member_set(cef_v8_interceptor_t *self, const cef_string_t *name, cef_v8_value_t *object, cef_v8_value_t *value,
	cef_string_t *exception)
{
	holder *owner = holder_of(object);
	engine_release(object);
	if (!owner) {
		engine_release(value);
		return 0;
	}
	WCHAR *text = engine_copy_text(name);
	VARIANT argument;
	to_variant(value, &argument);
	engine_release(value);
	call request = {call_put, owner->dispatch, text};
	request.arguments = &argument;
	request.count = 1;
	int handled = 0;
	// A name the object lacks lands on the script object.
	if (text && !send(owner->window, &request) && request.status != DISP_E_UNKNOWNNAME && request.status != DISP_E_MEMBERNOTFOUND) {
		fail(exception, &request);
		handled = 1;
	} else if (text && SUCCEEDED(request.status))
		handled = 1;
	clear_argument(&argument);
	HeapFree(GetProcessHeap(), 0, text);
	holder_release(&owner->base);
	return handled;
}

static void CEF_CALLBACK browser_created(cef_render_process_handler_t *self, cef_browser_t *browser, cef_dictionary_value_t *extra)
{
	if (extra) {
		cef_string_t key = {0};
		engine_set_text(&key, ENGINE_WINDOW_KEY);
		const int window = extra->has_key(extra, &key) ? extra->get_int(extra, &key) : 0;
		cef.string_clear(&key);
		if (window)
			remember(browser->get_identifier(browser), (HWND)(INT_PTR)window);
		engine_release(extra);
	}
	engine_release(browser);
}

static void CEF_CALLBACK browser_destroyed(cef_render_process_handler_t *self, cef_browser_t *browser)
{
	forget(browser->get_identifier(browser));
	engine_release(browser);
}

/* __ravenPost of a page whose control gave it a script to run first: what the page passes reaches the control's window in turn. */
typedef struct poster {
	cef_v8_handler_t handler;
	LONG refs;
	HWND window;
} poster;

static void CEF_CALLBACK poster_add_ref(cef_base_ref_counted_t *self)
{
	InterlockedIncrement(&((poster *)self)->refs);
}

static int CEF_CALLBACK poster_release(cef_base_ref_counted_t *self)
{
	if (InterlockedDecrement(&((poster *)self)->refs))
		return 0;
	HeapFree(GetProcessHeap(), 0, self);
	return 1;
}

static int CEF_CALLBACK poster_one_ref(cef_base_ref_counted_t *self)
{
	return ((poster *)self)->refs == 1;
}

static int CEF_CALLBACK poster_some_ref(cef_base_ref_counted_t *self)
{
	return ((poster *)self)->refs > 0;
}

static int CEF_CALLBACK poster_execute(cef_v8_handler_t *self, const cef_string_t *name, cef_v8_value_t *object, size_t count,
	cef_v8_value_t *const *arguments, cef_v8_value_t **result, cef_string_t *exception)
{
	WCHAR *text = NULL;
	if (count && arguments[0]->is_string(arguments[0]))
		text = engine_take_userfree(arguments[0]->get_string_value(arguments[0]));
	for (size_t i = 0; i < count; i++)
		engine_release(arguments[i]);
	engine_release(object);
	if (text && !PostMessageW(((poster *)self)->window, ENGINE_POSTED, 0, (LPARAM)text))
		HeapFree(GetProcessHeap(), 0, text);
	*result = cef.undefined();
	return 1;
}

/* Runs the control's script for the document, with __ravenPost defined while it runs. */
static void run_first(HWND window, cef_v8_context_t *context, WCHAR *script)
{
	cef_v8_value_t *global = context->get_global(context);
	poster *handler = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*handler));
	if (!global || !handler) {
		HeapFree(GetProcessHeap(), 0, handler);
		engine_release(global);
		return;
	}
	handler->handler.base = (cef_base_ref_counted_t){sizeof(cef_v8_handler_t), poster_add_ref, poster_release, poster_one_ref, poster_some_ref};
	handler->handler.execute = poster_execute;
	handler->refs = 1;
	handler->window = window;
	cef_string_t key = {0}, code = {0}, address = {0};
	engine_set_text(&key, L"__ravenPost");
	cef_v8_value_t *function = cef.function(&key, &handler->handler);
	global->set_value_bykey(global, &key, function, V8_PROPERTY_ATTRIBUTE_DONTENUM);
	engine_set_text(&code, script);
	cef_v8_value_t *value = NULL;
	cef_v8_exception_t *error = NULL;
	if (!context->eval(context, &code, &address, 0, &value, &error)) {
		WCHAR *message = error ? engine_take_userfree(error->get_message(error)) : NULL;
		engine_trace("document script failed", message);
		HeapFree(GetProcessHeap(), 0, message);
	}
	engine_release(value);
	engine_release(error);
	global->delete_value_bykey(global, &key);
	cef.string_clear(&key);
	cef.string_clear(&code);
	engine_release(global);
}

/* Internet Explorer tells an element that its size changed: through its onresize handler up to IE10 and the mselementresize event
 * in IE11. Pages written for the control size their window that way, so an element given either hears, from a ResizeObserver, of
 * every change of its size after it asked; the body and the root keep the window's own resize. */
static const WCHAR element_resize[] =
	L"(function () {"
	L" if (typeof ResizeObserver !== 'function' || window.__ravenElementResize) return;"
	L" Object.defineProperty(window, '__ravenElementResize', {value: true});"
	L" var watched = new WeakMap();"
	L" var observer = new ResizeObserver(function (entries) {"
	L"  entries.forEach(function (entry) {"
	L"   var element = entry.target, seen = watched.get(element);"
	L"   if (!seen || (seen.width === element.offsetWidth && seen.height === element.offsetHeight)) return;"
	L"   seen.width = element.offsetWidth; seen.height = element.offsetHeight;"
	L"   if (seen.handler) element.dispatchEvent(new Event('resize'));"
	L"   if (seen.listener) element.dispatchEvent(new Event('mselementresize'));"
	L"  });"
	L" });"
	L" function watch(element, kind) {"
	L"  if (!(element instanceof Element) || element === document.body || element === document.documentElement) return;"
	L"  var seen = watched.get(element);"
	L"  if (!seen) {"
	L"   seen = {width: element.offsetWidth, height: element.offsetHeight};"
	L"   watched.set(element, seen);"
	L"   observer.observe(element);"
	L"  }"
	L"  seen[kind] = true;"
	L" }"
	L" var handler = Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'onresize');"
	L" if (handler && handler.set) Object.defineProperty(HTMLElement.prototype, 'onresize', {configurable: true, enumerable: handler.enumerable,"
	L"  get: handler.get, set: function (value) { handler.set.call(this, value); if (value) watch(this, 'handler'); }});"
	L" var listen = EventTarget.prototype.addEventListener;"
	L" EventTarget.prototype.addEventListener = function (type) {"
	L"  if (type === 'mselementresize') watch(this, 'listener');"
	L"  return listen.apply(this, arguments);"
	L" };"
	L"})();";

static void run_compatibility(cef_v8_context_t *context)
{
	cef_string_t code = {0}, address = {0};
	engine_set_text(&code, element_resize);
	cef_v8_value_t *value = NULL;
	cef_v8_exception_t *error = NULL;
	if (!context->eval(context, &code, &address, 0, &value, &error)) {
		WCHAR *message = error ? engine_take_userfree(error->get_message(error)) : NULL;
		engine_trace("element resize script failed", message);
		HeapFree(GetProcessHeap(), 0, message);
	}
	engine_release(value);
	engine_release(error);
	cef.string_clear(&code);
}

static void CEF_CALLBACK context_created(cef_render_process_handler_t *self, cef_browser_t *browser, cef_frame_t *frame, cef_v8_context_t *context)
{
	HWND window = window_of(browser);
	if (window)
		run_compatibility(context);
	WCHAR *script = window ? (WCHAR *)SendMessageW(window, ENGINE_SCRIPTS, 0, 0) : NULL;
	if (script) {
		run_first(window, context, script);
		HeapFree(GetProcessHeap(), 0, script);
	}
	IDispatch *external = window ? (IDispatch *)SendMessageW(window, ENGINE_EXTERNAL, 0, 0) : NULL;
	if (external) {
		cef_v8_value_t *global = context->get_global(context);
		cef_v8_value_t *object = wrap(window, external);
		cef_string_t key = {0};
		engine_set_text(&key, L"external");
		if (global && global->set_value_bykey(global, &key, object, V8_PROPERTY_ATTRIBUTE_NONE))
			engine_trace("page given the program's window.external", NULL);
		else
			engine_trace("page kept its own window.external", NULL);
		cef.string_clear(&key);
		engine_release(global);
	}
	engine_release(context);
	engine_release(frame);
	engine_release(browser);
}

cef_render_process_handler_t external_pages = {.base = STATIC_BASE(cef_render_process_handler_t), .on_browser_created = browser_created,
	.on_browser_destroyed = browser_destroyed, .on_context_created = context_created};
