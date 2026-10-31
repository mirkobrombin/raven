/* SPDX-License-Identifier: MIT */
#include "node.h"
#include <objsafe.h>
#include <mshtml.h>

/* One object serves both request classes: the server one only adds options and timeouts. The transfer goes through a URL
 * moniker, on the caller's thread or, for an asynchronous request, on a thread of its own that reports back through a
 * window on the thread that opened the request. */
typedef struct header
{
	struct header *next;
	BSTR name, value;
} header;

typedef struct
{
	const IServerXMLHTTPRequest2Vtbl *vtbl;
	const IObjectWithSiteVtbl *site_vtbl;
	const IObjectSafetyVtbl *safety_vtbl;
	const ISupportErrorInfoVtbl *errors_vtbl;
	const IBindStatusCallbackVtbl *callback_vtbl;
	const IHttpNegotiateVtbl *negotiate_vtbl;
	mx_dispex dispex;
	LONG refs;
	BOOL server;
	CRITICAL_SECTION lock;
	BSTR verb, url, user, password;
	BOOL asynchronous, aborted;
	LONG state;
	header *request_headers;
	IDispatch *on_change;
	IUnknown *site;
	BSTR site_url;
	DWORD safety_options, safety_mask;
	BYTE *body;
	ULONG body_size;
	BYTE *response;
	ULONG response_size, response_room;
	LONG status;
	BSTR status_text;
	WCHAR *response_headers;
	IBinding *binding;
	HANDLE thread, finished;
	HWND window;
	HRESULT result;
	LONG resolve_timeout, connect_timeout, send_timeout, receive_timeout;
	VARIANT options[8];
} http_request;

static http_request *http_from(IServerXMLHTTPRequest2 *iface)
{
	return (http_request *)iface;
}

#define HTTP_FACE(name, field, type) \
	static http_request *name##_self(type *iface) { return CONTAINING_RECORD(iface, http_request, field); } \
	static HRESULT WINAPI name##_QueryInterface(type *iface, REFIID riid, void **out) { return IServerXMLHTTPRequest2_QueryInterface((IServerXMLHTTPRequest2 *)name##_self(iface), riid, out); } \
	static ULONG WINAPI name##_AddRef(type *iface) { return IServerXMLHTTPRequest2_AddRef((IServerXMLHTTPRequest2 *)name##_self(iface)); } \
	static ULONG WINAPI name##_Release(type *iface) { return IServerXMLHTTPRequest2_Release((IServerXMLHTTPRequest2 *)name##_self(iface)); }

HTTP_FACE(site, site_vtbl, IObjectWithSite)
HTTP_FACE(safety, safety_vtbl, IObjectSafety)
HTTP_FACE(errors, errors_vtbl, ISupportErrorInfo)
HTTP_FACE(callback, callback_vtbl, IBindStatusCallback)
HTTP_FACE(negotiate, negotiate_vtbl, IHttpNegotiate)

static HRESULT WINAPI http_QueryInterface(IServerXMLHTTPRequest2 *iface, REFIID riid, void **out)
{
	http_request *self = http_from(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDispatch) || IsEqualGUID(riid, &IID_IXMLHTTPRequest) ||
		(self->server && (IsEqualGUID(riid, &IID_IServerXMLHTTPRequest) || IsEqualGUID(riid, &IID_IServerXMLHTTPRequest2))))
		*out = iface;
	else if (IsEqualGUID(riid, &IID_IObjectWithSite))
		*out = &self->site_vtbl;
	else if (IsEqualGUID(riid, &IID_IObjectSafety) && !self->server)
		*out = &self->safety_vtbl;
	else if (IsEqualGUID(riid, &IID_ISupportErrorInfo))
		*out = &self->errors_vtbl;
	else if (IsEqualGUID(riid, &IID_IBindStatusCallback))
		*out = &self->callback_vtbl;
	else if (IsEqualGUID(riid, &IID_IHttpNegotiate))
		*out = &self->negotiate_vtbl;
	else
		return E_NOINTERFACE;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static ULONG WINAPI http_AddRef(IServerXMLHTTPRequest2 *iface)
{
	return InterlockedIncrement(&http_from(iface)->refs);
}

static void clear_request_headers(http_request *self)
{
	header *at;
	while ((at = self->request_headers))
	{
		self->request_headers = at->next;
		SysFreeString(at->name);
		SysFreeString(at->value);
		mx_free(at);
	}
}

static void clear_response(http_request *self)
{
	mx_free(self->response);
	self->response = NULL;
	self->response_size = self->response_room = 0;
	mx_free(self->response_headers);
	self->response_headers = NULL;
	SysFreeString(self->status_text);
	self->status_text = NULL;
	self->status = 0;
}

static void wait_for_thread(http_request *self)
{
	if (self->thread)
	{
		WaitForSingleObject(self->thread, INFINITE);
		CloseHandle(self->thread);
		self->thread = NULL;
	}
}

static ULONG WINAPI http_Release(IServerXMLHTTPRequest2 *iface)
{
	http_request *self = http_from(iface);
	ULONG left = InterlockedDecrement(&self->refs);
	unsigned index;
	if (left)
		return left;
	if (self->window)
		DestroyWindow(self->window);
	SysFreeString(self->verb);
	SysFreeString(self->url);
	SysFreeString(self->user);
	SysFreeString(self->password);
	clear_request_headers(self);
	clear_response(self);
	mx_free(self->body);
	if (self->on_change)
		IDispatch_Release(self->on_change);
	if (self->site)
		IUnknown_Release(self->site);
	SysFreeString(self->site_url);
	if (self->finished)
		CloseHandle(self->finished);
	for (index = 0; index < ARRAYSIZE(self->options); index++)
		VariantClear(&self->options[index]);
	DeleteCriticalSection(&self->lock);
	mx_free(self);
	return 0;
}

static HRESULT WINAPI http_GetTypeInfoCount(IServerXMLHTTPRequest2 *iface, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = 1;
	return S_OK;
}

static mx_type http_type(http_request *self)
{
	return self->server ? MX_TYPE_SERVER_HTTP : MX_TYPE_HTTP;
}

static HRESULT WINAPI http_GetTypeInfo(IServerXMLHTTPRequest2 *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	return index ? DISP_E_BADINDEX : mx_type_info(http_type(http_from(iface)), out);
}

static HRESULT WINAPI http_GetIDsOfNames(IServerXMLHTTPRequest2 *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	return mx_get_ids(http_type(http_from(iface)), riid, names, count, lcid, ids);
}

static HRESULT WINAPI http_Invoke(IServerXMLHTTPRequest2 *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	return mx_invoke(http_type(http_from(iface)), iface, id, riid, lcid, flags, params, result, excep, arg_err);
}

/* --- Telling the page that the state moved. --- */

#define HTTP_NOTIFY (WM_USER + 1)

static void fire_change(http_request *self)
{
	if (self->on_change)
	{
		DISPPARAMS none = {0};
		IDispatch_Invoke(self->on_change, 0, &IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_METHOD, &none, NULL, NULL, NULL);
	}
}

static LRESULT CALLBACK notify_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == HTTP_NOTIFY)
	{
		http_request *self = (http_request *)lparam;
		self->state = (LONG)wparam;
		if (wparam == READYSTATE_COMPLETE)
			wait_for_thread(self);
		fire_change(self);
		IServerXMLHTTPRequest2_Release((IServerXMLHTTPRequest2 *)self);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

static const WCHAR notify_class[] = L"RavenXmlHttpNotify";

static HWND notify_window(void)
{
	static LONG registered;
	if (!InterlockedExchange(&registered, 1))
	{
		WNDCLASSW info = {0};
		info.lpfnWndProc = notify_window_proc;
		info.hInstance = mx_instance;
		info.lpszClassName = notify_class;
		RegisterClassW(&info);
	}
	return CreateWindowExW(0, notify_class, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, mx_instance, NULL);
}

/* From the worker the change goes through the window; the caller's own thread can just run it. */
static void change_state(http_request *self, LONG state)
{
	if (self->asynchronous && self->window)
	{
		IServerXMLHTTPRequest2_AddRef((IServerXMLHTTPRequest2 *)self);
		if (state == READYSTATE_COMPLETE && self->finished)
			SetEvent(self->finished);
		PostMessageW(self->window, HTTP_NOTIFY, state, (LPARAM)self);
		return;
	}
	self->state = state;
	fire_change(self);
}

/* --- The transfer's callbacks. --- */

static HRESULT WINAPI callback_OnStartBinding(IBindStatusCallback *iface, DWORD reserved, IBinding *binding)
{
	http_request *self = callback_self(iface);
	IBinding_AddRef(binding);
	self->binding = binding;
	return S_OK;
}

static HRESULT WINAPI callback_GetPriority(IBindStatusCallback *iface, LONG *out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI callback_OnLowResource(IBindStatusCallback *iface, DWORD reserved)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI callback_OnProgress(IBindStatusCallback *iface, ULONG progress, ULONG maximum, ULONG code, LPCWSTR text)
{
	return S_OK;
}

static HRESULT WINAPI callback_OnStopBinding(IBindStatusCallback *iface, HRESULT result, LPCWSTR text)
{
	http_request *self = callback_self(iface);
	self->result = result;
	if (self->binding)
	{
		IBinding_Release(self->binding);
		self->binding = NULL;
	}
	return S_OK;
}

static HRESULT WINAPI callback_GetBindInfo(IBindStatusCallback *iface, DWORD *flags, BINDINFO *info)
{
	http_request *self = callback_self(iface);
	DWORD size = info->cbSize;
	*flags = BINDF_GETNEWESTVERSION | BINDF_PRAGMA_NO_CACHE | BINDF_NOWRITECACHE | BINDF_ASYNCHRONOUS | BINDF_ASYNCSTORAGE;
	memset(info, 0, size);
	info->cbSize = size;
	if (!lstrcmpiW(self->verb, L"GET"))
		info->dwBindVerb = BINDVERB_GET;
	else if (!lstrcmpiW(self->verb, L"POST"))
		info->dwBindVerb = BINDVERB_POST;
	else if (!lstrcmpiW(self->verb, L"PUT"))
		info->dwBindVerb = BINDVERB_PUT;
	else
	{
		SIZE_T bytes = (lstrlenW(self->verb) + 1) * sizeof(WCHAR);
		info->dwBindVerb = BINDVERB_CUSTOM;
		info->szCustomVerb = CoTaskMemAlloc(bytes);
		if (info->szCustomVerb)
			memcpy(info->szCustomVerb, self->verb, bytes);
	}
	if (self->body_size)
	{
		HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, self->body_size);
		if (!copy)
			return E_OUTOFMEMORY;
		memcpy(GlobalLock(copy), self->body, self->body_size);
		GlobalUnlock(copy);
		info->stgmedData.tymed = TYMED_HGLOBAL;
		info->stgmedData.hGlobal = copy;
		info->cbstgmedData = self->body_size;
	}
	return S_OK;
}

static HRESULT WINAPI callback_OnDataAvailable(IBindStatusCallback *iface, DWORD flags, DWORD size, FORMATETC *format, STGMEDIUM *medium)
{
	http_request *self = callback_self(iface);
	if (medium->tymed == TYMED_ISTREAM)
	{
		BYTE chunk[4096];
		ULONG got = 0;
		HRESULT status;
		do
		{
			status = IStream_Read(medium->pstm, chunk, sizeof(chunk), &got);
			if (FAILED(status) || !got)
				break;
			if (self->response_size + got > self->response_room)
			{
				ULONG room = max(self->response_room * 2, self->response_size + got);
				BYTE *grown = mx_realloc(self->response, room);
				if (!grown)
					return E_OUTOFMEMORY;
				self->response = grown;
				self->response_room = room;
			}
			memcpy(self->response + self->response_size, chunk, got);
			self->response_size += got;
		} while (status == S_OK);
	}
	if (self->state != READYSTATE_INTERACTIVE)
		change_state(self, READYSTATE_INTERACTIVE);
	return S_OK;
}

static HRESULT WINAPI callback_OnObjectAvailable(IBindStatusCallback *iface, REFIID riid, IUnknown *object)
{
	return S_OK;
}

static const IBindStatusCallbackVtbl callback_table = {
	callback_QueryInterface, callback_AddRef, callback_Release, callback_OnStartBinding, callback_GetPriority, callback_OnLowResource,
	callback_OnProgress, callback_OnStopBinding, callback_GetBindInfo, callback_OnDataAvailable, callback_OnObjectAvailable,
};

static HRESULT WINAPI negotiate_BeginningTransaction(IHttpNegotiate *iface, LPCWSTR url, LPCWSTR headers, DWORD reserved, LPWSTR *additional)
{
	http_request *self = negotiate_self(iface);
	SIZE_T length = 1;
	header *at;
	WCHAR *text, *to;
	*additional = NULL;
	for (at = self->request_headers; at; at = at->next)
		length += SysStringLen(at->name) + SysStringLen(at->value) + 4;
	if (self->site_url)
		length += SysStringLen(self->site_url) + 11;
	if (length == 1)
		return S_OK;
	text = to = CoTaskMemAlloc(length * sizeof(WCHAR));
	if (!text)
		return E_OUTOFMEMORY;
	for (at = self->request_headers; at; at = at->next)
	{
		to += wsprintfW(to, L"%s: %s\r\n", at->name, at->value);
	}
	if (self->site_url)
		to += wsprintfW(to, L"Referer: %s\r\n", self->site_url);
	*to = 0;
	*additional = text;
	return S_OK;
}

static HRESULT WINAPI negotiate_OnResponse(IHttpNegotiate *iface, DWORD code, LPCWSTR headers, LPCWSTR request_headers, LPWSTR *additional)
{
	http_request *self = negotiate_self(iface);
	if (additional)
		*additional = NULL;
	self->status = code;
	if (headers)
	{
		/* The first line says the status in words; the rest are the headers. */
		const WCHAR *line_end = wcsstr(headers, L"\r\n");
		const WCHAR *reason = wcschr(headers, L' ');
		if (reason && line_end && reason < line_end)
		{
			reason = wcschr(reason + 1, L' ');
			if (reason && reason < line_end)
				self->status_text = SysAllocStringLen(reason + 1, (UINT)(line_end - reason - 1));
		}
		if (!self->status_text)
			self->status_text = SysAllocString(L"");
		if (line_end)
		{
			SIZE_T bytes = (lstrlenW(line_end + 2) + 1) * sizeof(WCHAR);
			self->response_headers = mx_alloc(bytes);
			if (self->response_headers)
				memcpy(self->response_headers, line_end + 2, bytes);
		}
	}
	return S_OK;
}

static const IHttpNegotiateVtbl negotiate_table = {
	negotiate_QueryInterface, negotiate_AddRef, negotiate_Release, negotiate_BeginningTransaction, negotiate_OnResponse,
};

/* Runs the whole transfer; the moniker's binding pumps through messages while it waits. */
static HRESULT transfer(http_request *self)
{
	IMoniker *moniker = NULL;
	IBindCtx *context = NULL;
	IStream *stream = NULL;
	HRESULT status = CreateURLMonikerEx(NULL, self->url, &moniker, URL_MK_UNIFORM);
	if (SUCCEEDED(status))
		status = CreateAsyncBindCtx(0, (IBindStatusCallback *)&self->callback_vtbl, NULL, &context);
	self->result = E_PENDING;
	if (SUCCEEDED(status))
		status = IMoniker_BindToStorage(moniker, context, NULL, &IID_IStream, (void **)&stream);
	if (SUCCEEDED(status) || status == MK_S_ASYNCHRONOUS)
	{
		MSG message;
		while (self->result == E_PENDING && !self->aborted)
		{
			if (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&message);
				DispatchMessageW(&message);
			}
			else
				MsgWaitForMultipleObjects(0, NULL, FALSE, 20, QS_ALLINPUT);
		}
		status = self->aborted ? E_ABORT : self->result;
	}
	if (stream)
		IStream_Release(stream);
	if (context)
		IBindCtx_Release(context);
	if (moniker)
		IMoniker_Release(moniker);
	return status;
}

static DWORD WINAPI worker(void *parameter)
{
	http_request *self = parameter;
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	self->result = transfer(self);
	if (!self->aborted)
		change_state(self, READYSTATE_COMPLETE);
	CoUninitialize();
	IServerXMLHTTPRequest2_Release((IServerXMLHTTPRequest2 *)self);
	return 0;
}

/* --- The request itself. --- */

/* The length of the scheme and host that start an address, or 0 when it names neither. */
static int origin_length(const WCHAR *url)
{
	const WCHAR *at = wcsstr(url, L"://");
	if (!at)
		return 0;
	at += 3;
	while (*at && *at != L'/')
		at++;
	return (int)(at - url);
}

static BOOL same_origin(const WCHAR *first, const WCHAR *second)
{
	int length = origin_length(first);
	return length && length == origin_length(second) && !_wcsnicmp(first, second, length);
}

static HRESULT WINAPI http_open(IServerXMLHTTPRequest2 *iface, BSTR verb, BSTR url, VARIANT asynchronous, VARIANT user, VARIANT password)
{
	http_request *self = http_from(iface);
	VARIANT flag;
	if (!verb || !url)
		return E_INVALIDARG;
	self->aborted = TRUE;
	if (self->binding)
		IBinding_Abort(self->binding);
	wait_for_thread(self);
	self->aborted = FALSE;
	SysFreeString(self->verb);
	SysFreeString(self->url);
	self->url = NULL;
	SysFreeString(self->user);
	SysFreeString(self->password);
	self->user = self->password = NULL;
	clear_request_headers(self);
	clear_response(self);
	self->verb = SysAllocString(verb);
	/* An address is relative to the page hosting the object, and an untrusted page reaches only its own origin. */
	if (self->site_url)
	{
		WCHAR combined[2084];
		DWORD length = 0;
		if (SUCCEEDED(CoInternetCombineUrl(self->site_url, url, 0, combined, ARRAYSIZE(combined), &length, 0)))
		{
			if ((self->safety_options & INTERFACESAFE_FOR_UNTRUSTED_DATA) && !same_origin(self->site_url, combined))
			{
				SysFreeString(self->verb);
				self->verb = self->url = NULL;
				self->state = READYSTATE_UNINITIALIZED;
				return E_ACCESSDENIED;
			}
			self->url = SysAllocString(combined);
		}
	}
	if (!self->url)
		self->url = SysAllocString(url);
	/* A missing flag leaves the request asynchronous for the plain class; the server class waits. */
	self->asynchronous = !self->server;
	VariantInit(&flag);
	if (V_VT(&asynchronous) != VT_ERROR && V_VT(&asynchronous) != VT_EMPTY && V_VT(&asynchronous) != VT_NULL &&
		SUCCEEDED(VariantChangeType(&flag, &asynchronous, 0, VT_BOOL)))
		self->asynchronous = V_BOOL(&flag) != VARIANT_FALSE;
	if (V_VT(&user) == VT_BSTR)
		self->user = SysAllocString(V_BSTR(&user));
	if (V_VT(&password) == VT_BSTR)
		self->password = SysAllocString(V_BSTR(&password));
	self->state = READYSTATE_LOADING;
	change_state(self, READYSTATE_LOADING);
	return S_OK;
}

static HRESULT WINAPI http_setRequestHeader(IServerXMLHTTPRequest2 *iface, BSTR name, BSTR value)
{
	http_request *self = http_from(iface);
	header *at;
	if (!name || !*name)
		return E_INVALIDARG;
	if (self->state != READYSTATE_LOADING)
		return E_FAIL;
	if (!value)
		return E_INVALIDARG;
	for (at = self->request_headers; at; at = at->next)
		if (!lstrcmpiW(at->name, name))
		{
			SysFreeString(at->value);
			at->value = SysAllocString(value);
			return S_OK;
		}
	at = mx_alloc(sizeof(*at));
	if (!at)
		return E_OUTOFMEMORY;
	at->name = SysAllocString(name);
	at->value = SysAllocString(value);
	at->next = self->request_headers;
	self->request_headers = at;
	return S_OK;
}

static HRESULT WINAPI http_getResponseHeader(IServerXMLHTTPRequest2 *iface, BSTR name, BSTR *out)
{
	http_request *self = http_from(iface);
	const WCHAR *line;
	UINT length;
	if (!name)
		return E_INVALIDARG;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->state < READYSTATE_INTERACTIVE && self->state != READYSTATE_COMPLETE)
		return E_FAIL;
	if (!self->response_headers)
		return S_FALSE;
	length = SysStringLen(name);
	for (line = self->response_headers; *line; )
	{
		const WCHAR *end = wcsstr(line, L"\r\n");
		UINT size = end ? (UINT)(end - line) : lstrlenW(line);
		if (size > length && line[length] == L':' && CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, line, length, name, length) == CSTR_EQUAL)
		{
			const WCHAR *value = line + length + 1;
			while (*value == L' ')
				value++;
			*out = SysAllocStringLen(value, (UINT)(line + size - value));
			return *out ? S_OK : E_OUTOFMEMORY;
		}
		if (!end)
			break;
		line = end + 2;
	}
	return S_FALSE;
}

static HRESULT WINAPI http_getAllResponseHeaders(IServerXMLHTTPRequest2 *iface, BSTR *out)
{
	http_request *self = http_from(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->state < READYSTATE_INTERACTIVE && self->state != READYSTATE_COMPLETE)
		return E_FAIL;
	*out = SysAllocString(self->response_headers ? self->response_headers : L"");
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* What goes out as the body: text as UTF-8, bytes as they are, a document as its text, a stream as what it holds. */
static HRESULT take_body(http_request *self, VARIANT *given)
{
	VARIANT *value = V_VT(given) == (VT_VARIANT | VT_BYREF) ? V_VARIANTREF(given) : given;
	mx_free(self->body);
	self->body = NULL;
	self->body_size = 0;
	switch (V_VT(value))
	{
	case VT_BSTR:
		{
			xmlChar *text = mx_to_utf8(V_BSTR(value));
			self->body_size = xmlStrlen(text);
			self->body = mx_alloc(self->body_size + 1);
			if (self->body)
				memcpy(self->body, text, self->body_size);
			xmlFree(text);
			return self->body ? S_OK : E_OUTOFMEMORY;
		}
	case VT_ARRAY | VT_UI1:
		{
			BYTE *data;
			LONG low, high;
			HRESULT status;
			if (SafeArrayGetDim(V_ARRAY(value)) != 1)
				return E_INVALIDARG;
			SafeArrayGetLBound(V_ARRAY(value), 1, &low);
			SafeArrayGetUBound(V_ARRAY(value), 1, &high);
			status = SafeArrayAccessData(V_ARRAY(value), (void **)&data);
			if (FAILED(status))
				return status;
			self->body_size = high - low + 1;
			self->body = mx_alloc(self->body_size + 1);
			if (self->body)
				memcpy(self->body, data, self->body_size);
			SafeArrayUnaccessData(V_ARRAY(value));
			return self->body ? S_OK : E_OUTOFMEMORY;
		}
	case VT_UNKNOWN:
	case VT_DISPATCH:
		{
			IXMLDOMDocument *document;
			IStream *stream;
			if (!V_UNKNOWN(value))
				return S_OK;
			if (SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(value), &IID_IXMLDOMDocument, (void **)&document)))
			{
				BSTR text;
				xmlChar *utf8;
				HRESULT status = IXMLDOMDocument_get_xml(document, &text);
				IXMLDOMDocument_Release(document);
				if (FAILED(status))
					return status;
				utf8 = mx_to_utf8(text);
				SysFreeString(text);
				self->body_size = xmlStrlen(utf8);
				self->body = mx_alloc(self->body_size + 1);
				if (self->body)
					memcpy(self->body, utf8, self->body_size);
				xmlFree(utf8);
				return self->body ? S_OK : E_OUTOFMEMORY;
			}
			if (SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(value), &IID_IStream, (void **)&stream)))
			{
				char *data;
				ULONG size;
				HRESULT status = mx_read_stream(stream, &data, &size);
				IStream_Release(stream);
				if (FAILED(status))
					return status;
				self->body = (BYTE *)data;
				self->body_size = size;
				return S_OK;
			}
			return E_INVALIDARG;
		}
	case VT_EMPTY:
	case VT_NULL:
	case VT_ERROR:
		return S_OK;
	default:
		return E_INVALIDARG;
	}
}

static HRESULT WINAPI http_send(IServerXMLHTTPRequest2 *iface, VARIANT body)
{
	http_request *self = http_from(iface);
	HRESULT status;
	if (self->state != READYSTATE_LOADING)
		return E_FAIL;
	status = take_body(self, &body);
	if (FAILED(status))
		return status;
	change_state(self, READYSTATE_LOADED);
	if (self->asynchronous)
	{
		if (!self->window)
			self->window = notify_window();
		if (!self->finished)
			self->finished = CreateEventW(NULL, TRUE, FALSE, NULL);
		ResetEvent(self->finished);
		IServerXMLHTTPRequest2_AddRef(iface);
		self->thread = CreateThread(NULL, 0, worker, self, 0, NULL);
		if (!self->thread)
		{
			IServerXMLHTTPRequest2_Release(iface);
			return HRESULT_FROM_WIN32(GetLastError());
		}
		return S_OK;
	}
	status = transfer(self);
	self->state = READYSTATE_COMPLETE;
	fire_change(self);
	return status;
}

static HRESULT WINAPI http_abort(IServerXMLHTTPRequest2 *iface)
{
	http_request *self = http_from(iface);
	self->aborted = TRUE;
	if (self->binding)
		IBinding_Abort(self->binding);
	wait_for_thread(self);
	clear_response(self);
	if (self->state != READYSTATE_UNINITIALIZED && self->state != READYSTATE_COMPLETE && self->state != READYSTATE_LOADING)
		fire_change(self);
	self->state = READYSTATE_UNINITIALIZED;
	return S_OK;
}

static HRESULT WINAPI http_get_status(IServerXMLHTTPRequest2 *iface, LONG *out)
{
	http_request *self = http_from(iface);
	if (!out)
		return E_POINTER;
	*out = 0;
	if (self->state != READYSTATE_COMPLETE && self->state != READYSTATE_INTERACTIVE)
		return E_FAIL;
	*out = self->status;
	return S_OK;
}

static HRESULT WINAPI http_get_statusText(IServerXMLHTTPRequest2 *iface, BSTR *out)
{
	http_request *self = http_from(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->state != READYSTATE_COMPLETE && self->state != READYSTATE_INTERACTIVE)
		return E_FAIL;
	*out = SysAllocString(self->status_text ? self->status_text : L"");
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* A byte order mark says UTF-16, as a text response without one is read as UTF-8. */
static HRESULT WINAPI http_get_responseText(IServerXMLHTTPRequest2 *iface, BSTR *out)
{
	http_request *self = http_from(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->state != READYSTATE_COMPLETE && self->state != READYSTATE_INTERACTIVE)
		return E_FAIL;
	if (self->response_size >= 2 && self->response[0] == 0xff && self->response[1] == 0xfe)
		*out = SysAllocStringLen((const WCHAR *)(self->response + 2), (self->response_size - 2) / sizeof(WCHAR));
	else
	{
		const BYTE *from = self->response;
		ULONG size = self->response_size;
		int wide;
		if (size >= 3 && from[0] == 0xef && from[1] == 0xbb && from[2] == 0xbf)
		{
			from += 3;
			size -= 3;
		}
		wide = MultiByteToWideChar(CP_UTF8, 0, (const char *)from, size, NULL, 0);
		*out = SysAllocStringLen(NULL, wide);
		if (*out)
			MultiByteToWideChar(CP_UTF8, 0, (const char *)from, size, *out, wide);
	}
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI http_get_responseXML(IServerXMLHTTPRequest2 *iface, IDispatch **out)
{
	http_request *self = http_from(iface);
	IXMLDOMDocument *document;
	IStream *stream;
	HGLOBAL global;
	VARIANT source;
	VARIANT_BOOL success;
	HRESULT status;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->state != READYSTATE_COMPLETE)
		return E_FAIL;
	status = mx_document_create(MX_DEFAULT, &IID_IXMLDOMDocument, (void **)&document);
	if (FAILED(status))
		return status;
	if (self->response_size)
	{
		global = GlobalAlloc(GMEM_MOVEABLE, self->response_size);
		if (global)
		{
			memcpy(GlobalLock(global), self->response, self->response_size);
			GlobalUnlock(global);
			if (SUCCEEDED(CreateStreamOnHGlobal(global, TRUE, &stream)))
			{
				V_VT(&source) = VT_UNKNOWN;
				V_UNKNOWN(&source) = (IUnknown *)stream;
				IXMLDOMDocument_load(document, source, &success);
				IStream_Release(stream);
			}
			else
				GlobalFree(global);
		}
	}
	*out = (IDispatch *)document;
	return S_OK;
}

static HRESULT WINAPI http_get_responseBody(IServerXMLHTTPRequest2 *iface, VARIANT *out)
{
	http_request *self = http_from(iface);
	SAFEARRAY *array;
	void *data;
	if (!out)
		return E_INVALIDARG;
	V_VT(out) = VT_EMPTY;
	if (self->state != READYSTATE_COMPLETE)
		return E_PENDING;
	array = SafeArrayCreateVector(VT_UI1, 0, self->response_size);
	if (!array)
		return E_OUTOFMEMORY;
	SafeArrayAccessData(array, &data);
	if (self->response_size)
		memcpy(data, self->response, self->response_size);
	SafeArrayUnaccessData(array);
	V_VT(out) = VT_ARRAY | VT_UI1;
	V_ARRAY(out) = array;
	return S_OK;
}

static HRESULT WINAPI http_get_responseStream(IServerXMLHTTPRequest2 *iface, VARIANT *out)
{
	http_request *self = http_from(iface);
	IStream *stream;
	HGLOBAL global;
	if (!out)
		return E_INVALIDARG;
	V_VT(out) = VT_EMPTY;
	if (self->state != READYSTATE_COMPLETE)
		return E_PENDING;
	global = GlobalAlloc(GMEM_MOVEABLE, max(self->response_size, 1));
	if (!global)
		return E_OUTOFMEMORY;
	if (self->response_size)
	{
		memcpy(GlobalLock(global), self->response, self->response_size);
		GlobalUnlock(global);
	}
	if (FAILED(CreateStreamOnHGlobal(global, TRUE, &stream)))
	{
		GlobalFree(global);
		return E_OUTOFMEMORY;
	}
	{
		ULARGE_INTEGER size;
		size.QuadPart = self->response_size;
		IStream_SetSize(stream, size);
	}
	V_VT(out) = VT_UNKNOWN;
	V_UNKNOWN(out) = (IUnknown *)stream;
	return S_OK;
}

static HRESULT WINAPI http_get_readyState(IServerXMLHTTPRequest2 *iface, LONG *out)
{
	if (!out)
		return E_POINTER;
	*out = http_from(iface)->state;
	return S_OK;
}

static HRESULT WINAPI http_put_onreadystatechange(IServerXMLHTTPRequest2 *iface, IDispatch *handler)
{
	http_request *self = http_from(iface);
	if (handler)
		IDispatch_AddRef(handler);
	if (self->on_change)
		IDispatch_Release(self->on_change);
	self->on_change = handler;
	return S_OK;
}

static HRESULT WINAPI http_setTimeouts(IServerXMLHTTPRequest2 *iface, LONG resolve, LONG connect, LONG send, LONG receive)
{
	http_request *self = http_from(iface);
	self->resolve_timeout = resolve;
	self->connect_timeout = connect;
	self->send_timeout = send;
	self->receive_timeout = receive;
	return S_OK;
}

static HRESULT WINAPI http_waitForResponse(IServerXMLHTTPRequest2 *iface, VARIANT timeout, VARIANT_BOOL *out)
{
	http_request *self = http_from(iface);
	DWORD limit = INFINITE;
	VARIANT number;
	VariantInit(&number);
	if (V_VT(&timeout) != VT_ERROR && V_VT(&timeout) != VT_EMPTY && SUCCEEDED(VariantChangeType(&number, &timeout, 0, VT_I4)) && V_I4(&number) >= 0)
		limit = V_I4(&number) * 1000;
	if (out)
		*out = VARIANT_TRUE;
	if (self->thread && self->finished)
	{
		DWORD begun = GetTickCount();
		while (WaitForSingleObject(self->finished, 0) != WAIT_OBJECT_0)
		{
			MSG message;
			if (limit != INFINITE && GetTickCount() - begun >= limit)
			{
				if (out)
					*out = VARIANT_FALSE;
				return S_OK;
			}
			while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&message);
				DispatchMessageW(&message);
			}
			MsgWaitForMultipleObjects(1, &self->finished, FALSE, 20, QS_ALLINPUT);
		}
		wait_for_thread(self);
		self->state = READYSTATE_COMPLETE;
	}
	return S_OK;
}

static HRESULT WINAPI http_getOption(IServerXMLHTTPRequest2 *iface, SERVERXMLHTTP_OPTION option, VARIANT *out)
{
	http_request *self = http_from(iface);
	if (!out)
		return E_INVALIDARG;
	if ((unsigned)option >= ARRAYSIZE(self->options))
		return E_INVALIDARG;
	return VariantCopy(out, &self->options[option]);
}

static HRESULT WINAPI http_setOption(IServerXMLHTTPRequest2 *iface, SERVERXMLHTTP_OPTION option, VARIANT value)
{
	http_request *self = http_from(iface);
	if ((unsigned)option >= ARRAYSIZE(self->options))
		return E_INVALIDARG;
	return VariantCopy(&self->options[option], &value);
}

static HRESULT WINAPI http_setProxy(IServerXMLHTTPRequest2 *iface, SXH_PROXY_SETTING setting, VARIANT server, VARIANT bypass)
{
	return S_OK;
}

static HRESULT WINAPI http_setProxyCredentials(IServerXMLHTTPRequest2 *iface, BSTR user, BSTR password)
{
	return S_OK;
}

static const IServerXMLHTTPRequest2Vtbl http_table = {
	http_QueryInterface, http_AddRef, http_Release, http_GetTypeInfoCount, http_GetTypeInfo, http_GetIDsOfNames, http_Invoke,
	http_open, http_setRequestHeader, http_getResponseHeader, http_getAllResponseHeaders, http_send, http_abort, http_get_status,
	http_get_statusText, http_get_responseXML, http_get_responseText, http_get_responseBody, http_get_responseStream, http_get_readyState,
	http_put_onreadystatechange, http_setTimeouts, http_waitForResponse, http_getOption, http_setOption, http_setProxy, http_setProxyCredentials,
};

/* --- The site and the safety settings. --- */

static const GUID sid_container_dispatch = {0xb722be00, 0x4e68, 0x101b, {0xa2, 0xbc, 0x00, 0xaa, 0x00, 0x40, 0x47, 0x70}};
static const GUID sid_host_security = {0x3af280b6, 0xcb3f, 0x11d0, {0x89, 0x1e, 0x00, 0xc0, 0x4f, 0xb6, 0xbf, 0xc4}};

/* The address of the page the site hosts this object in, by either way a host may offer the page. */
static BSTR page_of_site(IUnknown *site)
{
	IServiceProvider *provider;
	IHTMLDocument2 *page = NULL;
	BSTR url = NULL;
	if (FAILED(IUnknown_QueryInterface(site, &IID_IServiceProvider, (void **)&provider)))
		return NULL;
	if (FAILED(IServiceProvider_QueryService(provider, &sid_container_dispatch, &IID_IHTMLDocument2, (void **)&page)) || !page)
		IServiceProvider_QueryService(provider, &sid_host_security, &IID_IHTMLDocument2, (void **)&page);
	IServiceProvider_Release(provider);
	if (page)
	{
		IHTMLDocument2_get_URL(page, &url);
		IHTMLDocument2_Release(page);
	}
	return url;
}

static HRESULT WINAPI site_SetSite(IObjectWithSite *iface, IUnknown *site)
{
	http_request *self = site_self(iface);
	if (site)
		IUnknown_AddRef(site);
	if (self->site)
		IUnknown_Release(self->site);
	self->site = site;
	SysFreeString(self->site_url);
	self->site_url = site ? page_of_site(site) : NULL;
	return S_OK;
}

static HRESULT WINAPI site_GetSite(IObjectWithSite *iface, REFIID riid, void **out)
{
	http_request *self = site_self(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!self->site)
		return E_FAIL;
	return IUnknown_QueryInterface(self->site, riid, out);
}

static const IObjectWithSiteVtbl site_table = {
	site_QueryInterface, site_AddRef, site_Release, site_SetSite, site_GetSite,
};

#define HTTP_SAFETY_SUPPORTED (INTERFACESAFE_FOR_UNTRUSTED_CALLER | INTERFACESAFE_FOR_UNTRUSTED_DATA | INTERFACE_USES_SECURITY_MANAGER)

static HRESULT WINAPI safety_GetInterfaceSafetyOptions(IObjectSafety *iface, REFIID riid, DWORD *supported, DWORD *enabled)
{
	http_request *self = safety_self(iface);
	if (!supported || !enabled)
		return E_POINTER;
	*supported = HTTP_SAFETY_SUPPORTED;
	*enabled = self->safety_options;
	return S_OK;
}

static HRESULT WINAPI safety_SetInterfaceSafetyOptions(IObjectSafety *iface, REFIID riid, DWORD mask, DWORD enabled)
{
	http_request *self = safety_self(iface);
	if (mask & ~HTTP_SAFETY_SUPPORTED)
		return E_FAIL;
	self->safety_options = (self->safety_options & ~mask) | (enabled & mask);
	return S_OK;
}

static const IObjectSafetyVtbl safety_table = {
	safety_QueryInterface, safety_AddRef, safety_Release, safety_GetInterfaceSafetyOptions, safety_SetInterfaceSafetyOptions,
};

static HRESULT WINAPI errors_InterfaceSupportsErrorInfo(ISupportErrorInfo *iface, REFIID riid)
{
	return IsEqualGUID(riid, &IID_IXMLHTTPRequest) || IsEqualGUID(riid, &IID_IServerXMLHTTPRequest) || IsEqualGUID(riid, &IID_IServerXMLHTTPRequest2) ? S_OK : S_FALSE;
}

static const ISupportErrorInfoVtbl errors_table = {
	errors_QueryInterface, errors_AddRef, errors_Release, errors_InterfaceSupportsErrorInfo,
};

static HRESULT create(BOOL server, REFIID riid, void **out)
{
	http_request *self = mx_zalloc(sizeof(*self));
	HRESULT status;
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &http_table;
	self->site_vtbl = &site_table;
	self->safety_vtbl = &safety_table;
	self->errors_vtbl = &errors_table;
	self->callback_vtbl = &callback_table;
	self->negotiate_vtbl = &negotiate_table;
	mx_dispex_init(&self->dispex, self, FALSE);
	self->refs = 1;
	self->server = server;
	InitializeCriticalSection(&self->lock);
	status = http_QueryInterface((IServerXMLHTTPRequest2 *)self, riid, out);
	http_Release((IServerXMLHTTPRequest2 *)self);
	return status;
}

HRESULT mx_http_create(REFIID riid, void **out)
{
	return create(FALSE, riid, out);
}

HRESULT mx_server_http_create(REFIID riid, void **out)
{
	return create(TRUE, riid, out);
}
