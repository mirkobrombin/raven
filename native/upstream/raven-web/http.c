/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The classes of Windows.Web.Http that programs make: HttpClient, which sends requests through a filter (the base protocol filter
 * unless the program gives its own) after adding its default headers; HttpBaseProtocolFilter with its cache control; HttpMethod;
 * HttpRequestMessage and HttpResponseMessage. A response is any status the server answers; only the methods that return content
 * (GetStringAsync, GetBufferAsync, GetInputStreamAsync) and EnsureSuccessStatusCode fail on a status outside 2xx, with the HRESULT of
 * the status in FACILITY_HTTP, as documented. With HttpCompletionOption.ResponseContentRead, the default, the body is read before the
 * operation completes; with ResponseHeadersRead it completes once the head has arrived. */
#define INITGUID
#include "web.h"
#include <urlmon.h>

typedef __x_ABI_CWindows_CWeb_CHttp_CHttpCompletionOption completion_option;
typedef __x_ABI_CWindows_CWeb_CHttp_CHttpStatusCode status_code;
typedef __x_ABI_CWindows_CWeb_CHttp_CHttpVersion http_version;
typedef __x_ABI_CWindows_CWeb_CHttp_CHttpResponseMessageSource message_source;
typedef __x_ABI_CWindows_CWeb_CHttp_CFilters_CHttpCacheReadBehavior read_behavior;
typedef __x_ABI_CWindows_CWeb_CHttp_CFilters_CHttpCacheWriteBehavior write_behavior;
typedef __x_ABI_CWindows_CWeb_CHttp_CFilters_CHttpCookieUsageBehavior cookie_usage;

/* Methods. */

struct method {
	struct winrt_object base;
	struct winrt_interface method, stringable;
	HSTRING name;
};

static void method_destroy(struct winrt_object *object) { WindowsDeleteString(((struct method *)object)->name); }
static HRESULT STDMETHODCALLTYPE method_name(void *iface, HSTRING *out) { return WindowsDuplicateString(((struct method *)winrt_object_of(iface))->name, out); }

static const __x_ABI_CWindows_CWeb_CHttp_CIHttpMethodVtbl method_table = {WINRT_INSPECTABLE, (void *)method_name};
static const __x_ABI_CWindows_CFoundation_CIStringableVtbl method_stringable_table = {WINRT_INSPECTABLE, (void *)method_name};
static const struct winrt_entry method_entries[] = {
	WINRT_ENTRY(struct method, method, IID___x_ABI_CWindows_CWeb_CHttp_CIHttpMethod),
	WINRT_ENTRY(struct method, stringable, IID___x_ABI_CWindows_CFoundation_CIStringable), WINRT_END};
static const struct winrt_class method_kind = {L"Windows.Web.Http.HttpMethod", method_entries, method_destroy};

/* A method name is a token (RFC 9110, 9.1). */
static HRESULT method_new(const WCHAR *name, UINT32 length, http_method **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!length)
		return E_INVALIDARG;
	for (UINT32 index = 0; index < length; ++index)
		if (name[index] <= ' ' || name[index] >= 0x7f || name[index] == '(' || name[index] == ')' || name[index] == ',' || name[index] == '/' ||
			name[index] == ':' || name[index] == ';' || name[index] == '<' || name[index] == '>' || name[index] == '=' || name[index] == '?' ||
			name[index] == '@' || name[index] == '[' || name[index] == ']' || name[index] == '{' || name[index] == '}' || name[index] == '"' ||
			name[index] == '\\')
			return E_INVALIDARG;
	struct method *self = winrt_new(&method_kind, sizeof(*self), NULL);
	if (!self)
		return E_OUTOFMEMORY;
	self->method.table = &method_table;
	self->stringable.table = &method_stringable_table;
	const HRESULT result = winrt_string_n(name, length, &self->name);
	if (FAILED(result)) {
		winrt_release(&self->method);
		return result;
	}
	*out = (void *)&self->method;
	return S_OK;
}

static HRESULT standard_method(const WCHAR *name, http_method **out) { return method_new(name, lstrlenW(name), out); }

/* Request messages. */

struct request {
	struct winrt_object base;
	struct winrt_interface request, closable, stringable;
	CRITICAL_SECTION lock;
	http_method *method;
	uri_class *uri;
	http_content *content;
	struct headers *headers;
};

static struct request *request_of(void *iface) { return (struct request *)winrt_object_of(iface); }

static void request_destroy(struct winrt_object *object)
{
	struct request *self = (struct request *)object;
	winrt_put(self->method);
	winrt_put(self->uri);
	winrt_put(self->content);
	headers_release(self->headers);
	DeleteCriticalSection(&self->lock);
}

/* Gets or sets one of the request's interface members under its lock. */
static HRESULT get_member(CRITICAL_SECTION *lock, void *member, void **out)
{
	if (!out)
		return E_POINTER;
	EnterCriticalSection(lock);
	*out = *(void **)member;
	if (*out)
		((IUnknown *)*out)->lpVtbl->AddRef(*out);
	LeaveCriticalSection(lock);
	return S_OK;
}

static HRESULT put_member(CRITICAL_SECTION *lock, void *member, void *value)
{
	if (value)
		((IUnknown *)value)->lpVtbl->AddRef(value);
	EnterCriticalSection(lock);
	void *old = *(void **)member;
	*(void **)member = value;
	LeaveCriticalSection(lock);
	winrt_put(old);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE request_get_content(void *iface, http_content **out) { return get_member(&request_of(iface)->lock, &request_of(iface)->content, (void **)out); }
static HRESULT STDMETHODCALLTYPE request_put_content(void *iface, http_content *value) { return put_member(&request_of(iface)->lock, &request_of(iface)->content, value); }

static HRESULT STDMETHODCALLTYPE request_headers(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = headers_interface(request_of(iface)->headers);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE request_get_method(void *iface, http_method **out) { return get_member(&request_of(iface)->lock, &request_of(iface)->method, (void **)out); }

static HRESULT STDMETHODCALLTYPE request_put_method(void *iface, http_method *value)
{
	if (!value)
		return E_INVALIDARG;
	return put_member(&request_of(iface)->lock, &request_of(iface)->method, value);
}

static HRESULT STDMETHODCALLTYPE request_properties(void *iface, void **out)
{
	/* The property bag of a request, for filters to pass data along, is not provided yet. */
	if (!out)
		return E_POINTER;
	*out = NULL;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE request_get_uri(void *iface, uri_class **out) { return get_member(&request_of(iface)->lock, &request_of(iface)->uri, (void **)out); }
static HRESULT STDMETHODCALLTYPE request_put_uri(void *iface, uri_class *value) { return put_member(&request_of(iface)->lock, &request_of(iface)->uri, value); }

static HRESULT STDMETHODCALLTYPE request_transport(void *iface, void **out)
{
	/* HttpTransportInformation (the server's certificate and its errors) is not provided yet. */
	if (!out)
		return E_POINTER;
	*out = NULL;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE request_close(void *iface) { return S_OK; }

/* "GET https://host/path" */
static HRESULT STDMETHODCALLTYPE request_string(void *iface, HSTRING *out)
{
	HSTRING method = NULL, uri = NULL, space = NULL, partial = NULL;
	http_method *verb = NULL;
	uri_class *target = NULL;
	if (!out)
		return E_POINTER;
	request_get_method(iface, &verb);
	request_get_uri(iface, &target);
	HRESULT result = verb ? verb->lpVtbl->get_Method(verb, &method) : S_OK;
	if (SUCCEEDED(result) && target)
		result = uri_text(target, &uri);
	if (SUCCEEDED(result))
		result = winrt_string(L" ", &space);
	if (SUCCEEDED(result))
		result = WindowsConcatString(method, space, &partial);
	if (SUCCEEDED(result))
		result = WindowsConcatString(partial, uri, out);
	WindowsDeleteString(method);
	WindowsDeleteString(uri);
	WindowsDeleteString(space);
	WindowsDeleteString(partial);
	winrt_put(verb);
	winrt_put(target);
	return result;
}

static const __x_ABI_CWindows_CWeb_CHttp_CIHttpRequestMessageVtbl request_table = {WINRT_INSPECTABLE, (void *)request_get_content,
	(void *)request_put_content, (void *)request_headers, (void *)request_get_method, (void *)request_put_method,
	(void *)request_properties, (void *)request_get_uri, (void *)request_put_uri, (void *)request_transport};
static const __x_ABI_CWindows_CFoundation_CIClosableVtbl request_closable_table = {WINRT_INSPECTABLE, (void *)request_close};
static const __x_ABI_CWindows_CFoundation_CIStringableVtbl request_stringable_table = {WINRT_INSPECTABLE, (void *)request_string};
static const struct winrt_entry request_entries[] = {
	WINRT_ENTRY(struct request, request, IID___x_ABI_CWindows_CWeb_CHttp_CIHttpRequestMessage),
	WINRT_ENTRY(struct request, closable, IID___x_ABI_CWindows_CFoundation_CIClosable),
	WINRT_ENTRY(struct request, stringable, IID___x_ABI_CWindows_CFoundation_CIStringable), WINRT_END};
static const struct winrt_class request_kind = {L"Windows.Web.Http.HttpRequestMessage", request_entries, request_destroy};

static HRESULT request_new(http_method *method, uri_class *uri, request_message **out)
{
	struct request *self = winrt_new(&request_kind, sizeof(*self), NULL);
	if (!self)
		return E_OUTOFMEMORY;
	self->request.table = &request_table;
	self->closable.table = &request_closable_table;
	self->stringable.table = &request_stringable_table;
	InitializeCriticalSection(&self->lock);
	HRESULT result = headers_new(HEADERS_REQUEST, &self->headers);
	if (SUCCEEDED(result) && !method)
		result = standard_method(L"GET", &self->method);
	else if (SUCCEEDED(result)) {
		method->lpVtbl->AddRef(method);
		self->method = method;
	}
	if (FAILED(result)) {
		winrt_release(&self->request);
		return result;
	}
	if (uri)
		uri->lpVtbl->AddRef(uri);
	self->uri = uri;
	*out = (void *)&self->request;
	return S_OK;
}

HRESULT request_parts(request_message *request, HSTRING *method, uri_class **uri, http_content **content)
{
	http_method *verb = NULL;
	HRESULT result = request->lpVtbl->get_Method(request, &verb);
	if (SUCCEEDED(result) && !verb)
		result = E_INVALIDARG;
	if (SUCCEEDED(result))
		result = verb->lpVtbl->get_Method(verb, method);
	winrt_put(verb);
	if (SUCCEEDED(result))
		result = request->lpVtbl->get_RequestUri(request, uri);
	if (SUCCEEDED(result))
		result = request->lpVtbl->get_Content(request, content);
	return result;
}

void request_set_uri(request_message *request, uri_class *uri) { request->lpVtbl->put_RequestUri(request, uri); }

/* Response messages. */

struct response {
	struct winrt_object base;
	struct winrt_interface response, closable, stringable;
	CRITICAL_SECTION lock;
	INT32 status, version, source;
	HSTRING reason;
	struct headers *headers;
	http_content *content;
	request_message *request;
};

static struct response *response_of(void *iface) { return (struct response *)winrt_object_of(iface); }

static void response_destroy(struct winrt_object *object)
{
	struct response *self = (struct response *)object;
	WindowsDeleteString(self->reason);
	headers_release(self->headers);
	winrt_put(self->content);
	winrt_put(self->request);
	DeleteCriticalSection(&self->lock);
}

static HRESULT STDMETHODCALLTYPE response_get_content(void *iface, http_content **out) { return get_member(&response_of(iface)->lock, &response_of(iface)->content, (void **)out); }
static HRESULT STDMETHODCALLTYPE response_put_content(void *iface, http_content *value) { return put_member(&response_of(iface)->lock, &response_of(iface)->content, value); }

static HRESULT STDMETHODCALLTYPE response_headers(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = headers_interface(response_of(iface)->headers);
	return S_OK;
}

static BOOL success_status(INT32 status) { return status >= 200 && status <= 299; }

static HRESULT STDMETHODCALLTYPE response_is_success(void *iface, boolean *out)
{
	if (!out)
		return E_POINTER;
	*out = success_status(response_of(iface)->status);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE response_get_reason(void *iface, HSTRING *out)
{
	struct response *self = response_of(iface);
	if (!out)
		return E_POINTER;
	EnterCriticalSection(&self->lock);
	const HRESULT result = WindowsDuplicateString(self->reason, out);
	LeaveCriticalSection(&self->lock);
	return result;
}

static HRESULT STDMETHODCALLTYPE response_put_reason(void *iface, HSTRING value)
{
	struct response *self = response_of(iface);
	HSTRING copy = NULL;
	const HRESULT result = WindowsDuplicateString(value, &copy);
	if (FAILED(result))
		return result;
	EnterCriticalSection(&self->lock);
	HSTRING old = self->reason;
	self->reason = copy;
	LeaveCriticalSection(&self->lock);
	WindowsDeleteString(old);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE response_get_request(void *iface, request_message **out) { return get_member(&response_of(iface)->lock, &response_of(iface)->request, (void **)out); }
static HRESULT STDMETHODCALLTYPE response_put_request(void *iface, request_message *value) { return put_member(&response_of(iface)->lock, &response_of(iface)->request, value); }

static HRESULT STDMETHODCALLTYPE response_get_source(void *iface, message_source *out)
{
	if (!out)
		return E_POINTER;
	*out = response_of(iface)->source;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE response_put_source(void *iface, message_source value)
{
	response_of(iface)->source = value;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE response_get_status(void *iface, status_code *out)
{
	if (!out)
		return E_POINTER;
	*out = response_of(iface)->status;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE response_put_status(void *iface, status_code value)
{
	response_of(iface)->status = value;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE response_get_version(void *iface, http_version *out)
{
	if (!out)
		return E_POINTER;
	*out = response_of(iface)->version;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE response_put_version(void *iface, http_version value)
{
	response_of(iface)->version = value;
	return S_OK;
}

static HRESULT status_error(INT32 status) { return MAKE_HRESULT(SEVERITY_ERROR, FACILITY_HTTP, status); }

static HRESULT STDMETHODCALLTYPE response_ensure_success(void *iface, response_message **out)
{
	struct response *self = response_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!success_status(self->status))
		return status_error(self->status);
	winrt_add_ref(&self->response);
	*out = (void *)&self->response;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE response_close(void *iface)
{
	http_content *content = NULL;
	__x_ABI_CWindows_CFoundation_CIClosable *closable = NULL;
	response_get_content(iface, &content);
	if (content && SUCCEEDED(content->lpVtbl->QueryInterface(content, &IID___x_ABI_CWindows_CFoundation_CIClosable, (void **)&closable))) {
		closable->lpVtbl->Close(closable);
		closable->lpVtbl->Release(closable);
	}
	winrt_put(content);
	return S_OK;
}

/* "StatusCode: 200, ReasonPhrase: 'OK', Version: 2" */
static HRESULT STDMETHODCALLTYPE response_string(void *iface, HSTRING *out)
{
	struct response *self = response_of(iface);
	static const WCHAR status[] = L"StatusCode: ", reason_label[] = L", ReasonPhrase: '", version[] = L"', Version: ";
	if (!out)
		return E_POINTER;
	EnterCriticalSection(&self->lock);
	UINT32 reason_length;
	const WCHAR *reason = winrt_text(self->reason, &reason_length);
	WCHAR *text = WINRT_ALLOC((ARRAYSIZE(status) + ARRAYSIZE(reason_label) + ARRAYSIZE(version) + reason_length + 40) * sizeof(WCHAR));
	HRESULT result = text ? S_OK : E_OUTOFMEMORY;
	if (text) {
		WCHAR *at = text;
		memcpy(at, status, sizeof(status) - sizeof(WCHAR));
		at += ARRAYSIZE(status) - 1;
		at += winrt_decimal((UINT64)self->status, at);
		memcpy(at, reason_label, sizeof(reason_label) - sizeof(WCHAR));
		at += ARRAYSIZE(reason_label) - 1;
		memcpy(at, reason, reason_length * sizeof(WCHAR));
		at += reason_length;
		memcpy(at, version, sizeof(version) - sizeof(WCHAR));
		at += ARRAYSIZE(version) - 1;
		at += winrt_decimal((UINT64)self->version, at);
		result = winrt_string_n(text, (UINT32)(at - text), out);
	}
	LeaveCriticalSection(&self->lock);
	WINRT_FREE(text);
	return result;
}

static const __x_ABI_CWindows_CWeb_CHttp_CIHttpResponseMessageVtbl response_table = {WINRT_INSPECTABLE, (void *)response_get_content,
	(void *)response_put_content, (void *)response_headers, (void *)response_is_success, (void *)response_get_reason,
	(void *)response_put_reason, (void *)response_get_request, (void *)response_put_request, (void *)response_get_source,
	(void *)response_put_source, (void *)response_get_status, (void *)response_put_status, (void *)response_get_version,
	(void *)response_put_version, (void *)response_ensure_success};
static const __x_ABI_CWindows_CFoundation_CIClosableVtbl response_closable_table = {WINRT_INSPECTABLE, (void *)response_close};
static const __x_ABI_CWindows_CFoundation_CIStringableVtbl response_stringable_table = {WINRT_INSPECTABLE, (void *)response_string};
static const struct winrt_entry response_entries[] = {
	WINRT_ENTRY(struct response, response, IID___x_ABI_CWindows_CWeb_CHttp_CIHttpResponseMessage),
	WINRT_ENTRY(struct response, closable, IID___x_ABI_CWindows_CFoundation_CIClosable),
	WINRT_ENTRY(struct response, stringable, IID___x_ABI_CWindows_CFoundation_CIStringable), WINRT_END};
static const struct winrt_class response_kind = {L"Windows.Web.Http.HttpResponseMessage", response_entries, response_destroy};

HRESULT response_new(INT32 status, response_message **out)
{
	struct response *self = winrt_new(&response_kind, sizeof(*self), NULL);
	if (!self)
		return E_OUTOFMEMORY;
	self->response.table = &response_table;
	self->closable.table = &response_closable_table;
	self->stringable.table = &response_stringable_table;
	InitializeCriticalSection(&self->lock);
	self->status = status;
	self->version = HttpVersion_Http11;
	self->source = HttpResponseMessageSource_Network;
	const HRESULT result = headers_new(HEADERS_RESPONSE, &self->headers);
	if (FAILED(result)) {
		winrt_release(&self->response);
		return result;
	}
	*out = (void *)&self->response;
	return S_OK;
}

void response_set_head(response_message *response, HSTRING reason, INT32 version, request_message *request, struct headers *headers)
{
	struct response *self = response_of(response);
	response_put_reason(response, reason);
	self->version = version;
	response_put_request(response, request);
	headers_interface(headers);
	headers_release(self->headers);
	self->headers = headers;
}

void response_set_content(response_message *response, http_content *content) { response_put_content(response, content); }

/* The base protocol filter and its cache control. */

struct cache_control {
	struct winrt_object base;
	struct winrt_interface control;
	LONG read, write;
};

static struct cache_control *cache_of(void *iface) { return (struct cache_control *)winrt_object_of(iface); }

static HRESULT STDMETHODCALLTYPE cache_get_read(void *iface, read_behavior *out)
{
	if (!out)
		return E_POINTER;
	*out = InterlockedCompareExchange(&cache_of(iface)->read, 0, 0);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE cache_put_read(void *iface, read_behavior value)
{
	if (value < HttpCacheReadBehavior_Default || value > HttpCacheReadBehavior_NoCache)
		return E_INVALIDARG;
	InterlockedExchange(&cache_of(iface)->read, value);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE cache_get_write(void *iface, write_behavior *out)
{
	if (!out)
		return E_POINTER;
	*out = InterlockedCompareExchange(&cache_of(iface)->write, 0, 0);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE cache_put_write(void *iface, write_behavior value)
{
	if (value != HttpCacheWriteBehavior_Default && value != HttpCacheWriteBehavior_NoCache)
		return E_INVALIDARG;
	InterlockedExchange(&cache_of(iface)->write, value);
	return S_OK;
}

static const __x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpCacheControlVtbl cache_table = {WINRT_INSPECTABLE, (void *)cache_get_read,
	(void *)cache_put_read, (void *)cache_get_write, (void *)cache_put_write};
static const struct winrt_entry cache_entries[] = {
	WINRT_ENTRY(struct cache_control, control, IID___x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpCacheControl), WINRT_END};
static const struct winrt_class cache_kind = {L"Windows.Web.Http.Filters.HttpCacheControl", cache_entries, NULL};

struct filter {
	struct winrt_object base;
	struct winrt_interface filter, filter2, filter3, filter4, filter5, http, closable;
	struct cache_control *cache;
	LONG allow_redirect, allow_ui, decompress, use_proxy, max_connections, max_version, cookies;
};

static struct filter *filter_of(void *iface) { return (struct filter *)winrt_object_of(iface); }

static void filter_destroy(struct winrt_object *object) { winrt_release(&((struct filter *)object)->cache->control); }

static HRESULT get_flag(LONG *flag, boolean *out)
{
	if (!out)
		return E_POINTER;
	*out = InterlockedCompareExchange(flag, 0, 0) != 0;
	return S_OK;
}

static HRESULT put_flag(LONG *flag, boolean value)
{
	InterlockedExchange(flag, value != 0);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE filter_get_redirect(void *iface, boolean *out) { return get_flag(&filter_of(iface)->allow_redirect, out); }
static HRESULT STDMETHODCALLTYPE filter_put_redirect(void *iface, boolean value) { return put_flag(&filter_of(iface)->allow_redirect, value); }
static HRESULT STDMETHODCALLTYPE filter_get_ui(void *iface, boolean *out) { return get_flag(&filter_of(iface)->allow_ui, out); }
static HRESULT STDMETHODCALLTYPE filter_put_ui(void *iface, boolean value) { return put_flag(&filter_of(iface)->allow_ui, value); }
static HRESULT STDMETHODCALLTYPE filter_get_decompression(void *iface, boolean *out) { return get_flag(&filter_of(iface)->decompress, out); }
static HRESULT STDMETHODCALLTYPE filter_put_decompression(void *iface, boolean value) { return put_flag(&filter_of(iface)->decompress, value); }
static HRESULT STDMETHODCALLTYPE filter_get_proxy(void *iface, boolean *out) { return get_flag(&filter_of(iface)->use_proxy, out); }
static HRESULT STDMETHODCALLTYPE filter_put_proxy(void *iface, boolean value) { return put_flag(&filter_of(iface)->use_proxy, value); }

static HRESULT STDMETHODCALLTYPE filter_cache(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	winrt_add_ref(&filter_of(iface)->cache->control);
	*out = &filter_of(iface)->cache->control;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE filter_not_provided(void *iface, void **out)
{
	/* Cookies managed through the filter, client certificates, ignorable certificate errors and credentials are not provided yet:
	 * WinINet's own cookie store and certificate checks apply. */
	if (!out)
		return E_POINTER;
	*out = NULL;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE filter_put_not_provided(void *iface, void *value) { return E_NOTIMPL; }

static HRESULT STDMETHODCALLTYPE filter_get_connections(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = (UINT32)InterlockedCompareExchange(&filter_of(iface)->max_connections, 0, 0);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE filter_put_connections(void *iface, UINT32 value)
{
	if (!value)
		return E_INVALIDARG;
	InterlockedExchange(&filter_of(iface)->max_connections, (LONG)value);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE filter_get_version(void *iface, http_version *out)
{
	if (!out)
		return E_POINTER;
	*out = InterlockedCompareExchange(&filter_of(iface)->max_version, 0, 0);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE filter_put_version(void *iface, http_version value)
{
	if (value < HttpVersion_None || value > HttpVersion_Http20)
		return E_INVALIDARG;
	InterlockedExchange(&filter_of(iface)->max_version, value);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE filter_get_cookies(void *iface, cookie_usage *out)
{
	if (!out)
		return E_POINTER;
	*out = InterlockedCompareExchange(&filter_of(iface)->cookies, 0, 0);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE filter_put_cookies(void *iface, cookie_usage value)
{
	if (value != HttpCookieUsageBehavior_Default && value != HttpCookieUsageBehavior_NoCookies)
		return E_INVALIDARG;
	InterlockedExchange(&filter_of(iface)->cookies, value);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE filter_add_validation(void *iface, void *handler, EventRegistrationToken *token)
{
	/* Custom validation of server certificates is not provided yet. */
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE filter_remove_validation(void *iface, EventRegistrationToken token) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE filter_clear_authentication(void *iface) { return S_OK; }

static HRESULT STDMETHODCALLTYPE filter_user(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return S_OK;
}

/* Sending: the filter's settings when the request is made, and the exchange. */
struct send {
	struct transport_settings settings;
	request_message *request;
};

static void free_send(void *context)
{
	struct send *send = context;
	winrt_put(send->request);
	WINRT_FREE(send);
}

static HRESULT send_work(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct send *send = context;
	struct exchange *exchange = exchange_open();
	if (!exchange)
		return E_OUTOFMEMORY;
	const HRESULT outcome = exchange_send(exchange, &send->settings, send->request, (response_message **)&result->object);
	/* On success the response's content holds the exchange. */
	if (FAILED(outcome))
		exchange_close(exchange);
	return outcome;
}

static HRESULT STDMETHODCALLTYPE filter_send(void *iface, request_message *request, response_operation **out)
{
	struct filter *self = filter_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!request)
		return E_INVALIDARG;
	struct send *send = WINRT_ALLOC(sizeof(*send));
	if (!send)
		return E_OUTOFMEMORY;
	send->settings.allow_redirect = InterlockedCompareExchange(&self->allow_redirect, 0, 0) != 0;
	send->settings.decompress = InterlockedCompareExchange(&self->decompress, 0, 0) != 0;
	send->settings.use_cookies = InterlockedCompareExchange(&self->cookies, 0, 0) == HttpCookieUsageBehavior_Default;
	/* The cache behaviours, as WinINet's request flags. */
	const LONG read = InterlockedCompareExchange(&self->cache->read, 0, 0), write = InterlockedCompareExchange(&self->cache->write, 0, 0);
	send->settings.cache_flags = (read == HttpCacheReadBehavior_MostRecent ? INTERNET_FLAG_RESYNCHRONIZE : 0) |
		(read == HttpCacheReadBehavior_OnlyFromCache ? INTERNET_FLAG_FROM_CACHE : 0) |
		(read == HttpCacheReadBehavior_NoCache ? INTERNET_FLAG_RELOAD | INTERNET_FLAG_PRAGMA_NOCACHE : 0) |
		(write == HttpCacheWriteBehavior_NoCache ? INTERNET_FLAG_NO_CACHE_WRITE : 0);
	request->lpVtbl->AddRef(request);
	send->request = request;
	return winrt_async_start(&IID___FIAsyncOperationWithProgress_2_Windows__CWeb__CHttp__CHttpResponseMessage_HttpProgress, TRUE,
		WINRT_RESULT_OBJECT, send_work, send, free_send, (void **)out);
}

static HRESULT STDMETHODCALLTYPE filter_close(void *iface) { return S_OK; }

static const __x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpBaseProtocolFilterVtbl filter_table = {WINRT_INSPECTABLE,
	(void *)filter_get_redirect, (void *)filter_put_redirect, (void *)filter_get_ui, (void *)filter_put_ui,
	(void *)filter_get_decompression, (void *)filter_put_decompression, (void *)filter_cache, (void *)filter_not_provided,
	(void *)filter_not_provided, (void *)filter_put_not_provided, (void *)filter_not_provided, (void *)filter_get_connections,
	(void *)filter_put_connections, (void *)filter_not_provided, (void *)filter_put_not_provided, (void *)filter_not_provided,
	(void *)filter_put_not_provided, (void *)filter_get_proxy, (void *)filter_put_proxy};
static const __x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpBaseProtocolFilter2Vtbl filter2_table = {WINRT_INSPECTABLE,
	(void *)filter_get_version, (void *)filter_put_version};
static const __x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpBaseProtocolFilter3Vtbl filter3_table = {WINRT_INSPECTABLE,
	(void *)filter_get_cookies, (void *)filter_put_cookies};
static const __x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpBaseProtocolFilter4Vtbl filter4_table = {WINRT_INSPECTABLE,
	(void *)filter_add_validation, (void *)filter_remove_validation, (void *)filter_clear_authentication};
static const __x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpBaseProtocolFilter5Vtbl filter5_table = {WINRT_INSPECTABLE, (void *)filter_user};
static const __x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpFilterVtbl http_filter_table = {WINRT_INSPECTABLE, (void *)filter_send};
static const __x_ABI_CWindows_CFoundation_CIClosableVtbl filter_closable_table = {WINRT_INSPECTABLE, (void *)filter_close};
static const struct winrt_entry filter_entries[] = {
	WINRT_ENTRY(struct filter, filter, IID___x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpBaseProtocolFilter),
	WINRT_ENTRY(struct filter, filter2, IID___x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpBaseProtocolFilter2),
	WINRT_ENTRY(struct filter, filter3, IID___x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpBaseProtocolFilter3),
	WINRT_ENTRY(struct filter, filter4, IID___x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpBaseProtocolFilter4),
	WINRT_ENTRY(struct filter, filter5, IID___x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpBaseProtocolFilter5),
	WINRT_ENTRY(struct filter, http, IID___x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpFilter),
	WINRT_ENTRY(struct filter, closable, IID___x_ABI_CWindows_CFoundation_CIClosable), WINRT_END};
static const struct winrt_class filter_kind = {L"Windows.Web.Http.Filters.HttpBaseProtocolFilter", filter_entries, filter_destroy};

/* A new filter with the documented defaults: redirections followed, decompression on, the cache's default behaviours, the system's
 * proxy, six connections per server, HTTP/2 at most, cookies kept. */
static HRESULT filter_new(void **out)
{
	struct filter *self = winrt_new(&filter_kind, sizeof(*self), NULL);
	if (!self)
		return E_OUTOFMEMORY;
	self->filter.table = &filter_table;
	self->filter2.table = &filter2_table;
	self->filter3.table = &filter3_table;
	self->filter4.table = &filter4_table;
	self->filter5.table = &filter5_table;
	self->http.table = &http_filter_table;
	self->closable.table = &filter_closable_table;
	self->allow_redirect = self->allow_ui = self->decompress = self->use_proxy = 1;
	self->max_connections = 6;
	self->max_version = HttpVersion_Http20;
	self->cookies = HttpCookieUsageBehavior_Default;
	if (!(self->cache = winrt_new(&cache_kind, sizeof(*self->cache), NULL))) {
		winrt_release(&self->filter);
		return E_OUTOFMEMORY;
	}
	self->cache->control.table = &cache_table;
	*out = &self->filter;
	return S_OK;
}

/* The client. */

struct client {
	struct winrt_object base;
	struct winrt_interface client, closable, stringable;
	http_filter *filter;
	struct headers *defaults;
};

static struct client *client_of(void *iface) { return (struct client *)winrt_object_of(iface); }

static void client_destroy(struct winrt_object *object)
{
	struct client *self = (struct client *)object;
	winrt_put(self->filter);
	headers_release(self->defaults);
}

/* What the operation answers: the response, or its content as a buffer, a stream or a string. */
enum answer {
	ANSWER_RESPONSE,
	ANSWER_BUFFER,
	ANSWER_STREAM,
	ANSWER_STRING
};

struct exchange_call {
	struct client *client;
	request_message *request;
	completion_option option;
	enum answer answer;
};

static void free_call(void *context)
{
	struct exchange_call *call = context;
	winrt_release(&call->client->client);
	winrt_put(call->request);
	WINRT_FREE(call);
}

/* Adds the client's default headers that the request does not set itself. */
struct merge {
	request_message *request;
	__FIMap_2_HSTRING_HSTRING *map;
};

static HRESULT merge_default(void *context, HSTRING key, HSTRING value)
{
	struct merge *merge = context;
	boolean present = FALSE, replaced;
	HRESULT result = merge->map->lpVtbl->HasKey(merge->map, key, &present);
	if (SUCCEEDED(result) && !present)
		result = merge->map->lpVtbl->Insert(merge->map, key, value, &replaced);
	return result;
}

static HRESULT merge_defaults(struct client *client, request_message *request)
{
	IInspectable *headers = NULL;
	struct merge merge = {request, NULL};
	HRESULT result = request->lpVtbl->get_Headers(request, (void *)&headers);
	if (SUCCEEDED(result))
		result = headers->lpVtbl->QueryInterface(headers, &IID___FIMap_2_HSTRING_HSTRING, (void **)&merge.map);
	if (SUCCEEDED(result))
		result = winrt_map_each(headers_map(client->defaults), merge_default, &merge);
	winrt_put(merge.map);
	winrt_put(headers);
	return result;
}

static HRESULT call_work(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct exchange_call *call = context;
	response_operation *sending = NULL;
	response_message *response = NULL;
	http_content *content = NULL;
	HRESULT outcome = merge_defaults(call->client, call->request);
	if (SUCCEEDED(outcome))
		outcome = call->client->filter->lpVtbl->SendRequestAsync(call->client->filter, call->request, &sending);
	if (SUCCEEDED(outcome))
		outcome = winrt_wait(sending, TRUE, &response);
	if (SUCCEEDED(outcome) && call->answer != ANSWER_RESPONSE) {
		response_message *checked = NULL;
		outcome = response->lpVtbl->EnsureSuccessStatusCode(response, &checked);
		winrt_put(checked);
	}
	if (SUCCEEDED(outcome))
		outcome = response->lpVtbl->get_Content(response, &content);
	if (SUCCEEDED(outcome) && !content)
		outcome = E_UNEXPECTED;
	if (SUCCEEDED(outcome) && call->answer == ANSWER_RESPONSE && call->option == HttpCompletionOption_ResponseContentRead) {
		__FIAsyncOperationWithProgress_2_UINT64_UINT64 *buffering = NULL;
		UINT64 length;
		if (SUCCEEDED(outcome = content->lpVtbl->BufferAllAsync(content, &buffering)))
			outcome = winrt_wait(buffering, TRUE, &length);
		winrt_put(buffering);
	}
	if (SUCCEEDED(outcome)) {
		void *reading = NULL;
		switch (call->answer) {
		case ANSWER_RESPONSE:
			response->lpVtbl->AddRef(response);
			result->object = response;
			break;
		case ANSWER_BUFFER:
			if (SUCCEEDED(outcome = content->lpVtbl->ReadAsBufferAsync(content, (void *)&reading)))
				outcome = winrt_wait(reading, TRUE, &result->object);
			break;
		case ANSWER_STREAM:
			if (SUCCEEDED(outcome = content->lpVtbl->ReadAsInputStreamAsync(content, (void *)&reading)))
				outcome = winrt_wait(reading, TRUE, &result->object);
			break;
		case ANSWER_STRING:
			if (SUCCEEDED(outcome = content->lpVtbl->ReadAsStringAsync(content, (void *)&reading)))
				outcome = winrt_wait(reading, TRUE, &result->string);
			break;
		}
		winrt_put(reading);
	}
	winrt_put(content);
	winrt_put(response);
	winrt_put(sending);
	return outcome;
}

static HRESULT start_call(void *iface, request_message *request, completion_option option, enum answer answer, void **out)
{
	static const IID *operations[] = {&IID___FIAsyncOperationWithProgress_2_Windows__CWeb__CHttp__CHttpResponseMessage_HttpProgress,
		&IID___FIAsyncOperationWithProgress_2_Windows__CStorage__CStreams__CIBuffer_HttpProgress,
		&IID___FIAsyncOperationWithProgress_2_Windows__CStorage__CStreams__CIInputStream_HttpProgress,
		&IID___FIAsyncOperationWithProgress_2_HSTRING_HttpProgress};
	struct client *self = client_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!request)
		return E_INVALIDARG;
	if (option != HttpCompletionOption_ResponseContentRead && option != HttpCompletionOption_ResponseHeadersRead)
		return E_INVALIDARG;
	struct exchange_call *call = WINRT_ALLOC(sizeof(*call));
	if (!call)
		return E_OUTOFMEMORY;
	winrt_add_ref(&self->client);
	request->lpVtbl->AddRef(request);
	call->client = self;
	call->request = request;
	call->option = option;
	call->answer = answer;
	return winrt_async_start(operations[answer], TRUE, answer == ANSWER_STRING ? WINRT_RESULT_STRING : WINRT_RESULT_OBJECT, call_work, call,
		free_call, out);
}

/* A request of the method to the URI, with optional content, sent as the answer says. */
static HRESULT send_to(void *iface, const WCHAR *verb, uri_class *uri, http_content *content, completion_option option, enum answer answer,
	void **out)
{
	http_method *method = NULL;
	request_message *request = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!uri)
		return E_INVALIDARG;
	HRESULT result = standard_method(verb, &method);
	if (SUCCEEDED(result))
		result = request_new(method, uri, &request);
	if (SUCCEEDED(result) && content)
		result = request->lpVtbl->put_Content(request, content);
	if (SUCCEEDED(result))
		result = start_call(iface, request, option, answer, out);
	winrt_put(request);
	winrt_put(method);
	return result;
}

static HRESULT STDMETHODCALLTYPE client_delete(void *iface, uri_class *uri, void **out)
{
	return send_to(iface, L"DELETE", uri, NULL, HttpCompletionOption_ResponseContentRead, ANSWER_RESPONSE, out);
}

static HRESULT STDMETHODCALLTYPE client_get(void *iface, uri_class *uri, void **out)
{
	return send_to(iface, L"GET", uri, NULL, HttpCompletionOption_ResponseContentRead, ANSWER_RESPONSE, out);
}

static HRESULT STDMETHODCALLTYPE client_get_option(void *iface, uri_class *uri, completion_option option, void **out)
{
	return send_to(iface, L"GET", uri, NULL, option, ANSWER_RESPONSE, out);
}

static HRESULT STDMETHODCALLTYPE client_get_buffer(void *iface, uri_class *uri, void **out)
{
	return send_to(iface, L"GET", uri, NULL, HttpCompletionOption_ResponseContentRead, ANSWER_BUFFER, out);
}

static HRESULT STDMETHODCALLTYPE client_get_stream(void *iface, uri_class *uri, void **out)
{
	return send_to(iface, L"GET", uri, NULL, HttpCompletionOption_ResponseHeadersRead, ANSWER_STREAM, out);
}

static HRESULT STDMETHODCALLTYPE client_get_string(void *iface, uri_class *uri, void **out)
{
	return send_to(iface, L"GET", uri, NULL, HttpCompletionOption_ResponseContentRead, ANSWER_STRING, out);
}

static HRESULT STDMETHODCALLTYPE client_post(void *iface, uri_class *uri, http_content *content, void **out)
{
	return send_to(iface, L"POST", uri, content, HttpCompletionOption_ResponseContentRead, ANSWER_RESPONSE, out);
}

static HRESULT STDMETHODCALLTYPE client_put(void *iface, uri_class *uri, http_content *content, void **out)
{
	return send_to(iface, L"PUT", uri, content, HttpCompletionOption_ResponseContentRead, ANSWER_RESPONSE, out);
}

static HRESULT STDMETHODCALLTYPE client_send(void *iface, request_message *request, void **out)
{
	return start_call(iface, request, HttpCompletionOption_ResponseContentRead, ANSWER_RESPONSE, out);
}

static HRESULT STDMETHODCALLTYPE client_send_option(void *iface, request_message *request, completion_option option, void **out)
{
	return start_call(iface, request, option, ANSWER_RESPONSE, out);
}

static HRESULT STDMETHODCALLTYPE client_defaults(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = headers_interface(client_of(iface)->defaults);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE client_close(void *iface) { return S_OK; }
static HRESULT STDMETHODCALLTYPE client_string(void *iface, HSTRING *out) { return winrt_class_name(iface, out); }

static const __x_ABI_CWindows_CWeb_CHttp_CIHttpClientVtbl client_table = {WINRT_INSPECTABLE, (void *)client_delete, (void *)client_get,
	(void *)client_get_option, (void *)client_get_buffer, (void *)client_get_stream, (void *)client_get_string, (void *)client_post,
	(void *)client_put, (void *)client_send, (void *)client_send_option, (void *)client_defaults};
static const __x_ABI_CWindows_CFoundation_CIClosableVtbl client_closable_table = {WINRT_INSPECTABLE, (void *)client_close};
static const __x_ABI_CWindows_CFoundation_CIStringableVtbl client_stringable_table = {WINRT_INSPECTABLE, (void *)client_string};
static const struct winrt_entry client_entries[] = {
	WINRT_ENTRY(struct client, client, IID___x_ABI_CWindows_CWeb_CHttp_CIHttpClient),
	WINRT_ENTRY(struct client, closable, IID___x_ABI_CWindows_CFoundation_CIClosable),
	WINRT_ENTRY(struct client, stringable, IID___x_ABI_CWindows_CFoundation_CIStringable), WINRT_END};
static const struct winrt_class client_kind = {L"Windows.Web.Http.HttpClient", client_entries, client_destroy};

static HRESULT client_new(http_filter *filter, void **out)
{
	struct client *self = winrt_new(&client_kind, sizeof(*self), NULL);
	if (!self)
		return E_OUTOFMEMORY;
	self->client.table = &client_table;
	self->closable.table = &client_closable_table;
	self->stringable.table = &client_stringable_table;
	HRESULT result = headers_new(HEADERS_REQUEST, &self->defaults);
	if (SUCCEEDED(result) && filter) {
		filter->lpVtbl->AddRef(filter);
		self->filter = filter;
	} else if (SUCCEEDED(result)) {
		void *base = NULL;
		if (SUCCEEDED(result = filter_new(&base))) {
			self->filter = winrt_get(winrt_object_of(base), &IID___x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpFilter);
			winrt_put(base);
		}
	}
	if (FAILED(result)) {
		winrt_release(&self->client);
		return result;
	}
	*out = &self->client;
	return S_OK;
}

/* Factories: IActivationFactory and the class's factory or statics interfaces. */

struct factory {
	struct winrt_object base;
	struct winrt_interface activation, first, second;
	HRESULT (*activate)(IInspectable **out);
};

static HRESULT STDMETHODCALLTYPE factory_activate(void *iface, IInspectable **out)
{
	struct factory *self = (struct factory *)winrt_object_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	return self->activate ? self->activate(out) : E_NOTIMPL;
}

static HRESULT activate_client(IInspectable **out) { return client_new(NULL, (void **)out); }
static HRESULT activate_filter(IInspectable **out) { return filter_new((void **)out); }
static HRESULT activate_request(IInspectable **out) { return request_new(NULL, NULL, (request_message **)out); }

static HRESULT activate_response(IInspectable **out)
{
	response_message *response = NULL;
	HRESULT result = response_new(HttpStatusCode_Ok, &response);
	if (SUCCEEDED(result))
		response_put_reason(response, NULL);
	*out = (IInspectable *)response;
	return result;
}

static HRESULT STDMETHODCALLTYPE client_factory_create(void *iface, http_filter *filter, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return filter ? client_new(filter, out) : E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE method_factory_create(void *iface, HSTRING name, http_method **out)
{
	UINT32 length;
	const WCHAR *text = winrt_text(name, &length);
	return method_new(text, length, out);
}

static HRESULT STDMETHODCALLTYPE method_delete(void *iface, http_method **out) { return standard_method(L"DELETE", out); }
static HRESULT STDMETHODCALLTYPE method_get(void *iface, http_method **out) { return standard_method(L"GET", out); }
static HRESULT STDMETHODCALLTYPE method_head(void *iface, http_method **out) { return standard_method(L"HEAD", out); }
static HRESULT STDMETHODCALLTYPE method_options(void *iface, http_method **out) { return standard_method(L"OPTIONS", out); }
static HRESULT STDMETHODCALLTYPE method_patch(void *iface, http_method **out) { return standard_method(L"PATCH", out); }
static HRESULT STDMETHODCALLTYPE method_post(void *iface, http_method **out) { return standard_method(L"POST", out); }
static HRESULT STDMETHODCALLTYPE method_put(void *iface, http_method **out) { return standard_method(L"PUT", out); }

static HRESULT STDMETHODCALLTYPE request_factory_create(void *iface, http_method *method, uri_class *uri, request_message **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return method && uri ? request_new(method, uri, out) : E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE response_factory_create(void *iface, status_code status, response_message **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return response_new(status, out);
}

static const IActivationFactoryVtbl activation_table = {WINRT_INSPECTABLE, (void *)factory_activate};
static const __x_ABI_CWindows_CWeb_CHttp_CIHttpClientFactoryVtbl client_factory_table = {WINRT_INSPECTABLE, (void *)client_factory_create};
static const __x_ABI_CWindows_CWeb_CHttp_CIHttpMethodFactoryVtbl method_factory_table = {WINRT_INSPECTABLE, (void *)method_factory_create};
static const __x_ABI_CWindows_CWeb_CHttp_CIHttpMethodStaticsVtbl method_statics_table = {WINRT_INSPECTABLE, (void *)method_delete,
	(void *)method_get, (void *)method_head, (void *)method_options, (void *)method_patch, (void *)method_post, (void *)method_put};
static const __x_ABI_CWindows_CWeb_CHttp_CIHttpRequestMessageFactoryVtbl request_factory_table = {WINRT_INSPECTABLE,
	(void *)request_factory_create};
static const __x_ABI_CWindows_CWeb_CHttp_CIHttpResponseMessageFactoryVtbl response_factory_table = {WINRT_INSPECTABLE,
	(void *)response_factory_create};

static const struct winrt_entry factory_entries[] = {
	WINRT_ENTRY(struct factory, activation, IID_IActivationFactory), WINRT_ENTRY(struct factory, first, WINRT_OWN),
	WINRT_ENTRY(struct factory, second, WINRT_OWN_2), WINRT_END};

static const struct {
	const WCHAR *name;
	HRESULT (*activate)(IInspectable **out);
	const IID *first, *second;
	const void *first_table, *second_table;
} classes[] = {
	{L"Windows.Web.Http.HttpClient", activate_client, &IID___x_ABI_CWindows_CWeb_CHttp_CIHttpClientFactory, NULL, &client_factory_table},
	{L"Windows.Web.Http.Filters.HttpBaseProtocolFilter", activate_filter},
	{L"Windows.Web.Http.HttpMethod", NULL, &IID___x_ABI_CWindows_CWeb_CHttp_CIHttpMethodFactory,
		&IID___x_ABI_CWindows_CWeb_CHttp_CIHttpMethodStatics, &method_factory_table, &method_statics_table},
	{L"Windows.Web.Http.HttpRequestMessage", activate_request, &IID___x_ABI_CWindows_CWeb_CHttp_CIHttpRequestMessageFactory, NULL,
		&request_factory_table},
	{L"Windows.Web.Http.HttpResponseMessage", activate_response, &IID___x_ABI_CWindows_CWeb_CHttp_CIHttpResponseMessageFactory, NULL,
		&response_factory_table}};

static struct winrt_class factory_kinds[ARRAYSIZE(classes)];

__declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
	UINT32 length;
	const WCHAR *text = winrt_text(name, &length);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (CompareStringOrdinal(text, (int)length, L"Windows.Foundation.Uri", -1, FALSE) == CSTR_EQUAL)
		return uri_factory(&IID_IActivationFactory, (void **)out);
	if (CompareStringOrdinal(text, (int)length, L"Windows.Web.Http.Headers.HttpDateOrDeltaHeaderValue", -1, FALSE) == CSTR_EQUAL)
		return date_or_delta_statics(&IID_IActivationFactory, (void **)out);
	for (UINT32 index = 0; index < ARRAYSIZE(classes); ++index) {
		if (CompareStringOrdinal(text, (int)length, classes[index].name, -1, FALSE) != CSTR_EQUAL)
			continue;
		/* Each class's factory answers with its name. */
		factory_kinds[index].name = classes[index].name;
		factory_kinds[index].entries = factory_entries;
		struct factory *self = winrt_new_own(&factory_kinds[index], sizeof(*self), classes[index].first, classes[index].second, NULL);
		if (!self)
			return E_OUTOFMEMORY;
		self->activation.table = &activation_table;
		self->first.table = classes[index].first_table;
		self->second.table = classes[index].second_table;
		self->activate = classes[index].activate;
		*out = (void *)&self->activation;
		return S_OK;
	}
	return CLASS_E_CLASSNOTAVAILABLE;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return winrt_in_use() ? S_FALSE : S_OK; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
