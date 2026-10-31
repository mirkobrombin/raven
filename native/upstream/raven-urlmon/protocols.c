/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "urlmon_private.h"

/* The protocols served here. All share one object: what differs is how the bytes arrive.
 * file and mk read on the caller's thread. The network ones let a worker do the blocking
 * wininet calls and keep what it read in a queue, so Read never blocks the apartment. */
typedef enum
{
	KIND_FILE,
	KIND_MK,
	KIND_HTTP,
	KIND_HTTPS,
	KIND_FTP,
} protocol_kind;

typedef struct chunk
{
	struct chunk *next;
	DWORD size;
	DWORD used;
	BYTE data[];
} chunk;

typedef struct
{
	IInternetProtocolEx IInternetProtocolEx_iface;
	IInternetPriority IInternetPriority_iface;
	IWinInetHttpInfo IWinInetHttpInfo_iface;
	LONG references;
	IUnknown *outer;
	IUnknown IUnknown_inner;
	protocol_kind kind;
	IInternetProtocolSink *sink;
	IInternetBindInfo *bind_info;
	DWORD bind_flags;
	LONG priority;
	/* file */
	HANDLE file;
	/* mk */
	IStream *stream;
	/* network */
	CRITICAL_SECTION lock;
	HANDLE worker;
	HINTERNET session, connection, request;
	chunk *first, *last;
	BOOL finished;
	HRESULT final_result;
	DWORD final_error;
	WCHAR *final_text;
	BOOL aborted;
	BOOL reported_first;
	BOOL headers_reported;
	BOOL result_reported;
	ULONGLONG received, expected;
	WCHAR *url;
	IUri *uri;
	WCHAR *post_headers;
	BINDINFO info;
	BOOL have_info;
	void *post_data;
	DWORD post_size;
} protocol_object;

static inline protocol_object *from_protocol(IInternetProtocolEx *iface)
{
	return CONTAINING_RECORD(iface, protocol_object, IInternetProtocolEx_iface);
}

static inline protocol_object *from_priority(IInternetPriority *iface)
{
	return CONTAINING_RECORD(iface, protocol_object, IInternetPriority_iface);
}

static inline protocol_object *from_http_info(IWinInetHttpInfo *iface)
{
	return CONTAINING_RECORD(iface, protocol_object, IWinInetHttpInfo_iface);
}

static inline protocol_object *from_inner(IUnknown *iface)
{
	return CONTAINING_RECORD(iface, protocol_object, IUnknown_inner);
}

static BOOL is_network(const protocol_object *self)
{
	return self->kind == KIND_HTTP || self->kind == KIND_HTTPS || self->kind == KIND_FTP;
}

static void release_sink(protocol_object *self)
{
	if (self->sink)
		IInternetProtocolSink_Release(self->sink);
	self->sink = NULL;
	if (self->bind_info)
		IInternetBindInfo_Release(self->bind_info);
	self->bind_info = NULL;
}

static void close_network(protocol_object *self)
{
	if (self->request)
		InternetCloseHandle(self->request);
	if (self->connection)
		InternetCloseHandle(self->connection);
	if (self->session)
		InternetCloseHandle(self->session);
	self->request = self->connection = self->session = NULL;
}

/* Waits out the worker before anything it uses goes away. */
static void stop_worker(protocol_object *self)
{
	if (!self->worker)
		return;
	EnterCriticalSection(&self->lock);
	self->aborted = TRUE;
	LeaveCriticalSection(&self->lock);
	/* Closing the handles unblocks whatever call it is in. */
	close_network(self);
	WaitForSingleObject(self->worker, INFINITE);
	CloseHandle(self->worker);
	self->worker = NULL;
}

static void close_everything(protocol_object *self)
{
	chunk *item;
	stop_worker(self);
	close_network(self);
	if (self->file && self->file != INVALID_HANDLE_VALUE)
		CloseHandle(self->file);
	self->file = NULL;
	if (self->stream)
		IStream_Release(self->stream);
	self->stream = NULL;
	while ((item = self->first))
	{
		self->first = item->next;
		uri_free(item);
	}
	self->last = NULL;
	if (self->have_info)
		ReleaseBindInfo(&self->info);
	self->have_info = FALSE;
	if (self->uri)
		IUri_Release(self->uri);
	self->uri = NULL;
	uri_free(self->url);
	self->url = NULL;
	uri_free(self->post_headers);
	self->post_headers = NULL;
	uri_free(self->final_text);
	self->final_text = NULL;
}

static HRESULT WINAPI inner_query_interface(IUnknown *iface, REFIID iid, void **object)
{
	protocol_object *self = from_inner(iface);
	if (IsEqualGUID(iid, &IID_IUnknown))
		*object = &self->IUnknown_inner;
	else if (IsEqualGUID(iid, &IID_IInternetProtocolRoot) || IsEqualGUID(iid, &IID_IInternetProtocol) || IsEqualGUID(iid, &IID_IInternetProtocolEx))
		*object = &self->IInternetProtocolEx_iface;
	else if (IsEqualGUID(iid, &IID_IInternetPriority))
		*object = &self->IInternetPriority_iface;
	else if ((self->kind == KIND_HTTP || self->kind == KIND_HTTPS) && (IsEqualGUID(iid, &IID_IWinInetInfo) || IsEqualGUID(iid, &IID_IWinInetHttpInfo)))
		*object = &self->IWinInetHttpInfo_iface;
	else
	{
		*object = NULL;
		return E_NOINTERFACE;
	}
	IUnknown_AddRef((IUnknown *)*object);
	return S_OK;
}

static ULONG WINAPI inner_add_ref(IUnknown *iface)
{
	return InterlockedIncrement(&from_inner(iface)->references);
}

static ULONG WINAPI inner_release(IUnknown *iface)
{
	protocol_object *self = from_inner(iface);
	ULONG left = InterlockedDecrement(&self->references);
	if (!left)
	{
		close_everything(self);
		release_sink(self);
		DeleteCriticalSection(&self->lock);
		uri_free(self);
		urlmon_unlock_module();
	}
	return left;
}

static const IUnknownVtbl inner_vtable = {inner_query_interface, inner_add_ref, inner_release};

static HRESULT WINAPI protocol_query_interface(IInternetProtocolEx *iface, REFIID iid, void **object)
{
	return IUnknown_QueryInterface(from_protocol(iface)->outer, iid, object);
}

static ULONG WINAPI protocol_add_ref(IInternetProtocolEx *iface)
{
	return IUnknown_AddRef(from_protocol(iface)->outer);
}

static ULONG WINAPI protocol_release(IInternetProtocolEx *iface)
{
	return IUnknown_Release(from_protocol(iface)->outer);
}

static void take_sink(protocol_object *self, IInternetProtocolSink *sink, IInternetBindInfo *info)
{
	release_sink(self);
	IInternetProtocolSink_AddRef(sink);
	self->sink = sink;
	IInternetBindInfo_AddRef(info);
	self->bind_info = info;
}

static void report_result(protocol_object *self, HRESULT result, DWORD error, const WCHAR *text)
{
	if (self->result_reported || !self->sink)
		return;
	self->result_reported = TRUE;
	IInternetProtocolSink_ReportResult(self->sink, result, error, text);
}

static void gather_bind_info(protocol_object *self);

/* Starting a protocol that already holds its file only announces the data again. */
static HRESULT start_file(protocol_object *self, IUri *uri)
{
	WCHAR path[MAX_PATH * 2];
	DWORD length = 0, high = 0, low;
	WCHAR *found_mime = NULL;
	HRESULT result;
	gather_bind_info(self);
	if (!(self->bind_flags & BINDF_FROMURLMON))
		IInternetProtocolSink_ReportProgress(self->sink, BINDSTATUS_DIRECTBIND, NULL);
	if (self->file)
	{
		low = GetFileSize(self->file, &high);
		IInternetProtocolSink_ReportData(self->sink, BSCF_FIRSTDATANOTIFICATION | BSCF_LASTDATANOTIFICATION, low, low);
		return S_OK;
	}
	IInternetProtocolSink_ReportProgress(self->sink, BINDSTATUS_SENDINGREQUEST, L"");
	result = CoInternetParseIUri(uri, PARSE_PATH_FROM_URL, 0, path, ARRAYSIZE(path), &length, 0);
	if (FAILED(result))
	{
		report_result(self, INET_E_RESOURCE_NOT_FOUND, ERROR_FILE_NOT_FOUND, NULL);
		return INET_E_RESOURCE_NOT_FOUND;
	}
	self->file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (self->file == INVALID_HANDLE_VALUE)
	{
		DWORD error = GetLastError();
		self->file = NULL;
		report_result(self, INET_E_RESOURCE_NOT_FOUND, error, NULL);
		return INET_E_RESOURCE_NOT_FOUND;
	}
	IInternetProtocolSink_ReportProgress(self->sink, BINDSTATUS_CACHEFILENAMEAVAILABLE, path);
	if (SUCCEEDED(FindMimeFromData(NULL, path, NULL, 0, NULL, 0, &found_mime, 0)))
	{
		IInternetProtocolSink_ReportProgress(self->sink, self->bind_flags & BINDF_FROMURLMON ? BINDSTATUS_VERIFIEDMIMETYPEAVAILABLE : BINDSTATUS_MIMETYPEAVAILABLE, found_mime);
		CoTaskMemFree(found_mime);
	}
	low = GetFileSize(self->file, &high);
	self->expected = ((ULONGLONG)high << 32) | low;
	IInternetProtocolSink_ReportData(self->sink, BSCF_FIRSTDATANOTIFICATION | BSCF_LASTDATANOTIFICATION, low, low);
	report_result(self, S_OK, 0, NULL);
	return S_OK;
}

/* mk:@Class:Name asks a stream of that class's external object. */
static HRESULT start_mk(protocol_object *self, IUri *uri)
{
	BSTR path = NULL;
	IParseDisplayName *parser = NULL;
	IMoniker *moniker = NULL;
	IBindCtx *context = NULL;
	const WCHAR *colon, *end;
	WCHAR *class_part = NULL;
	CLSID clsid;
	ULONG eaten = 0;
	HRESULT result;
	WCHAR *mime = NULL;
	IInternetProtocolSink_ReportProgress(self->sink, BINDSTATUS_DIRECTBIND, NULL);
	result = IUri_GetPath(uri, &path);
	if (FAILED(result))
		return result;
	/* @ProgID:path, with the scheme already gone. */
	if (path[0] != L'@' || !(colon = wcschr(path, L':')))
	{
		SysFreeString(path);
		report_result(self, INET_E_RESOURCE_NOT_FOUND, ERROR_INVALID_PARAMETER, NULL);
		return INET_E_RESOURCE_NOT_FOUND;
	}
	end = colon;
	class_part = uri_text_range(path + 1, end);
	if (!class_part)
		result = E_OUTOFMEMORY;
	else
		result = CLSIDFromProgID(class_part, &clsid);
	uri_free(class_part);
	if (SUCCEEDED(result))
		result = CoGetClassObject(&clsid, CLSCTX_INPROC_SERVER, NULL, &IID_IParseDisplayName, (void **)&parser);
	if (SUCCEEDED(result))
		result = CreateBindCtx(0, &context);
	if (SUCCEEDED(result))
		result = IParseDisplayName_ParseDisplayName(parser, context, (WCHAR *)colon, &eaten, &moniker);
	if (SUCCEEDED(result))
		result = IMoniker_BindToStorage(moniker, context, NULL, &IID_IStream, (void **)&self->stream);
	if (parser)
		IParseDisplayName_Release(parser);
	if (moniker)
		IMoniker_Release(moniker);
	if (context)
		IBindCtx_Release(context);
	if (FAILED(result))
	{
		SysFreeString(path);
		report_result(self, INET_E_RESOURCE_NOT_FOUND, result, NULL);
		return INET_E_RESOURCE_NOT_FOUND;
	}
	if (SUCCEEDED(FindMimeFromData(NULL, path, NULL, 0, NULL, 0, &mime, 0)))
	{
		IInternetProtocolSink_ReportProgress(self->sink, BINDSTATUS_MIMETYPEAVAILABLE, mime);
		CoTaskMemFree(mime);
	}
	SysFreeString(path);
	IInternetProtocolSink_ReportData(self->sink, BSCF_FIRSTDATANOTIFICATION | BSCF_LASTDATANOTIFICATION | BSCF_DATAFULLYAVAILABLE, 0, 0);
	report_result(self, S_OK, 0, NULL);
	return S_OK;
}

static void wake_apartment(protocol_object *self)
{
	PROTOCOLDATA data = {PI_FORCE_ASYNC, 0, NULL, 0};
	IInternetProtocolSink_Switch(self->sink, &data);
}

static void worker_finish(protocol_object *self, HRESULT result, DWORD error, const WCHAR *text)
{
	EnterCriticalSection(&self->lock);
	self->finished = TRUE;
	self->final_result = result;
	self->final_error = error;
	self->final_text = text ? uri_text_copy(text) : NULL;
	LeaveCriticalSection(&self->lock);
	wake_apartment(self);
}

static BOOL still_wanted(protocol_object *self)
{
	BOOL wanted;
	EnterCriticalSection(&self->lock);
	wanted = !self->aborted;
	LeaveCriticalSection(&self->lock);
	return wanted;
}

static HRESULT connect_network(protocol_object *self, DWORD *error)
{
	static const WCHAR *const http_types[] = {L"*/*", NULL};
	BSTR host = NULL, path = NULL, user = NULL, password = NULL;
	WCHAR *agent = urlmon_user_agent();
	DWORD port = 0, service, flags = INTERNET_FLAG_KEEP_CONNECTION, status = 0, size = sizeof(status), head;
	const WCHAR *verb = L"GET";
	HRESULT result;
	self->session = InternetOpenW(agent ? agent : L"", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	uri_free(agent);
	if (!self->session)
		goto network_failed;
	IUri_GetHost(self->uri, &host);
	IUri_GetPathAndQuery(self->uri, &path);
	IUri_GetUserName(self->uri, &user);
	IUri_GetPassword(self->uri, &password);
	IUri_GetPort(self->uri, &port);
	service = self->kind == KIND_FTP ? INTERNET_SERVICE_FTP : INTERNET_SERVICE_HTTP;
	if (self->kind == KIND_HTTPS)
		flags |= INTERNET_FLAG_SECURE;
	if (self->bind_flags & BINDF_NOWRITECACHE)
		flags |= INTERNET_FLAG_NO_CACHE_WRITE;
	if (self->bind_flags & BINDF_GETNEWESTVERSION)
		flags |= INTERNET_FLAG_RELOAD;
	self->connection = InternetConnectW(self->session, host, (INTERNET_PORT)port, user && user[0] ? user : NULL, password && password[0] ? password : NULL, service, 0, 0);
	if (!self->connection)
		goto network_failed;
	IInternetProtocolSink_ReportProgress(self->sink, BINDSTATUS_CONNECTING, host);
	if (self->kind == KIND_FTP)
	{
		/* Looked up here: a wininet without FTP must only fail this URL, not end the program. */
		HMODULE wininet = GetModuleHandleW(L"wininet.dll");
		HINTERNET (WINAPI *open_file)(HINTERNET, LPCWSTR, DWORD, DWORD, DWORD_PTR) = wininet ? (void *)GetProcAddress(wininet, "FtpOpenFileW") : NULL;
		DWORD (WINAPI *file_size)(HINTERNET, DWORD *) = wininet ? (void *)GetProcAddress(wininet, "FtpGetFileSize") : NULL;
		if (!open_file || !file_size)
		{
			*error = ERROR_NOT_SUPPORTED;
			result = INET_E_CANNOT_CONNECT;
			goto done;
		}
		self->request = open_file(self->connection, path, GENERIC_READ, FTP_TRANSFER_TYPE_BINARY | flags, 0);
		if (!self->request)
			goto network_failed;
		head = 0;
		self->expected = file_size(self->request, &head);
		result = S_OK;
		goto done;
	}
	if (self->info.dwBindVerb == BINDVERB_POST)
		verb = L"POST";
	else if (self->info.dwBindVerb == BINDVERB_PUT)
		verb = L"PUT";
	else if (self->info.dwBindVerb == BINDVERB_CUSTOM && self->info.szCustomVerb)
		verb = self->info.szCustomVerb;
	self->request = HttpOpenRequestW(self->connection, verb, path, NULL, NULL, http_types, flags, 0);
	if (!self->request)
		goto network_failed;
	IInternetProtocolSink_ReportProgress(self->sink, BINDSTATUS_SENDINGREQUEST, L"");
	if (!HttpSendRequestW(self->request, self->post_headers, self->post_headers ? (DWORD)-1 : 0, self->post_data, self->post_size))
		goto network_failed;
	if (HttpQueryInfoW(self->request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, NULL) && status >= 400)
	{
		*error = status;
		result = INET_E_DOWNLOAD_FAILURE;
		goto done;
	}
	result = S_OK;
	goto done;
network_failed:
	*error = GetLastError();
	result = *error == ERROR_INTERNET_NAME_NOT_RESOLVED ? INET_E_RESOURCE_NOT_FOUND :
		*error == ERROR_INTERNET_CANNOT_CONNECT ? INET_E_CANNOT_CONNECT : INET_E_DOWNLOAD_FAILURE;
done:
	SysFreeString(host);
	SysFreeString(path);
	SysFreeString(user);
	SysFreeString(password);
	return result;
}

static DWORD WINAPI network_worker(void *argument)
{
	protocol_object *self = argument;
	DWORD error = 0, read = 0;
	HRESULT result = connect_network(self, &error);
	if (FAILED(result))
	{
		worker_finish(self, result, error, NULL);
		return 0;
	}
	/* Headers can only be looked at once the request has answered. */
	wake_apartment(self);
	while (still_wanted(self))
	{
		chunk *item = uri_alloc(sizeof(*item) + 0x10000);
		if (!item)
		{
			worker_finish(self, E_OUTOFMEMORY, ERROR_OUTOFMEMORY, NULL);
			return 0;
		}
		if (!InternetReadFile(self->request, item->data, 0x10000, &read))
		{
			error = GetLastError();
			uri_free(item);
			worker_finish(self, INET_E_DOWNLOAD_FAILURE, error, NULL);
			return 0;
		}
		if (!read)
		{
			uri_free(item);
			worker_finish(self, S_OK, 0, NULL);
			return 0;
		}
		item->size = read;
		EnterCriticalSection(&self->lock);
		if (self->last)
			self->last->next = item;
		else
			self->first = item;
		self->last = item;
		self->received += read;
		LeaveCriticalSection(&self->lock);
		wake_apartment(self);
	}
	return 0;
}

static void gather_bind_info(protocol_object *self)
{
	DWORD count = 0;
	const WCHAR *agents[2] = {NULL, NULL};
	self->info.cbSize = sizeof(self->info);
	if (SUCCEEDED(IInternetBindInfo_GetBindInfo(self->bind_info, &self->bind_flags, &self->info)))
		self->have_info = TRUE;
	if (self->have_info && self->info.dwBindVerb != BINDVERB_GET && self->info.stgmedData.tymed == TYMED_HGLOBAL)
	{
		self->post_data = self->info.stgmedData.hGlobal;
		self->post_size = self->info.cbstgmedData;
	}
	(void)count;
	(void)agents;
}

/* Whoever binds may add headers of their own. */
static void gather_extra_headers(protocol_object *self)
{
	IHttpNegotiate *negotiate = NULL;
	IServiceProvider *provider = NULL;
	if (SUCCEEDED(IInternetProtocolSink_QueryInterface(self->sink, &IID_IServiceProvider, (void **)&provider)))
	{
		if (SUCCEEDED(IServiceProvider_QueryService(provider, &IID_IHttpNegotiate, &IID_IHttpNegotiate, (void **)&negotiate)))
		{
			WCHAR *added = NULL;
			if (SUCCEEDED(IHttpNegotiate_BeginningTransaction(negotiate, self->url, NULL, 0, &added)) && added)
			{
				self->post_headers = uri_text_copy(added);
				CoTaskMemFree(added);
			}
			IHttpNegotiate_Release(negotiate);
		}
		IServiceProvider_Release(provider);
	}
}

static HRESULT start_network(protocol_object *self)
{
	gather_bind_info(self);
	gather_extra_headers(self);
	IInternetProtocolSink_ReportProgress(self->sink, BINDSTATUS_FINDINGRESOURCE, L"");
	self->worker = CreateThread(NULL, 0, network_worker, self, 0, NULL);
	if (!self->worker)
		return HRESULT_FROM_WIN32(GetLastError());
	return S_OK;
}

static HRESULT WINAPI protocol_start_ex(IInternetProtocolEx *iface, IUri *uri, IInternetProtocolSink *sink, IInternetBindInfo *info, DWORD flags, HANDLE_PTR reserved)
{
	protocol_object *self = from_protocol(iface);
	static const struct
	{
		protocol_kind kind;
		DWORD scheme;
	} expected[] = {{KIND_FILE, URL_SCHEME_FILE}, {KIND_MK, URL_SCHEME_MK}, {KIND_HTTP, URL_SCHEME_HTTP}, {KIND_HTTPS, URL_SCHEME_HTTPS}, {KIND_FTP, URL_SCHEME_FTP}};
	DWORD scheme = 0, index;
	BSTR absolute = NULL;
	HRESULT result;
	if (!uri || !sink || !info)
		return E_INVALIDARG;
	IUri_GetScheme(uri, &scheme);
	for (index = 0; index < ARRAYSIZE(expected); index++)
		if (expected[index].kind == self->kind && expected[index].scheme != scheme)
			return MK_E_SYNTAX;
	take_sink(self, sink, info);
	IUri_AddRef(uri);
	self->uri = uri;
	result = IUri_GetAbsoluteUri(uri, &absolute);
	if (FAILED(result))
		return result;
	self->url = uri_text_copy(absolute);
	SysFreeString(absolute);
	switch (self->kind)
	{
	case KIND_FILE:
		return start_file(self, uri);
	case KIND_MK:
		return start_mk(self, uri);
	default:
		return start_network(self);
	}
}

static HRESULT WINAPI protocol_start(IInternetProtocolEx *iface, LPCWSTR url, IInternetProtocolSink *sink, IInternetBindInfo *info, DWORD flags, HANDLE_PTR reserved)
{
	IUri *uri;
	HRESULT result;
	if (!url || !url[0] || !sink || !info)
		return E_INVALIDARG;
	result = CreateUri(url, Uri_CREATE_FILE_USE_DOS_PATH, 0, &uri);
	if (FAILED(result))
		return MK_E_SYNTAX;
	result = protocol_start_ex(iface, uri, sink, info, flags, reserved);
	IUri_Release(uri);
	return result;
}

static WCHAR *query_header(protocol_object *self, DWORD option)
{
	DWORD size = 0;
	WCHAR *text;
	HttpQueryInfoW(self->request, option, NULL, &size, NULL);
	if (!size)
		return NULL;
	text = uri_alloc(size + sizeof(WCHAR));
	if (text && !HttpQueryInfoW(self->request, option, text, &size, NULL))
	{
		uri_free(text);
		text = NULL;
	}
	return text;
}

/* Runs on the apartment thread: says what the worker found since last time. */
/* Lets the client see the answer's headers, the way it saw the request's. */
static void report_response(protocol_object *self)
{
	IServiceProvider *provider;
	IHttpNegotiate *negotiate;
	DWORD status = 0, size = sizeof(status);
	WCHAR *headers;
	if (FAILED(IInternetProtocolSink_QueryInterface(self->sink, &IID_IServiceProvider, (void **)&provider)))
		return;
	if (SUCCEEDED(IServiceProvider_QueryService(provider, &IID_IHttpNegotiate, &IID_IHttpNegotiate, (void **)&negotiate)))
	{
		WCHAR *added = NULL;
		HttpQueryInfoW(self->request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, NULL);
		headers = query_header(self, HTTP_QUERY_RAW_HEADERS_CRLF);
		IHttpNegotiate_OnResponse(negotiate, status, headers, NULL, &added);
		if (added)
			CoTaskMemFree(added);
		uri_free(headers);
		IHttpNegotiate_Release(negotiate);
	}
	IServiceProvider_Release(provider);
}

static HRESULT WINAPI protocol_continue(IInternetProtocolEx *iface, PROTOCOLDATA *data)
{
	protocol_object *self = from_protocol(iface);
	BOOL finished;
	ULONGLONG received;
	DWORD flags;
	if (!is_network(self) || !self->sink)
		return S_OK;
	if (!self->headers_reported && self->request)
	{
		WCHAR *type;
		self->headers_reported = TRUE;
		if (self->kind != KIND_FTP)
		{
			report_response(self);
			WCHAR *length = query_header(self, HTTP_QUERY_CONTENT_LENGTH);
			if (length)
			{
				const WCHAR *at;
				ULONGLONG value = 0;
				for (at = length; *at >= L'0' && *at <= L'9'; at++)
					value = value * 10 + (*at - L'0');
				self->expected = value;
				uri_free(length);
			}
			type = query_header(self, HTTP_QUERY_CONTENT_TYPE);
			if (type)
			{
				WCHAR *stop = wcschr(type, L';');
				if (stop)
					*stop = 0;
				IInternetProtocolSink_ReportProgress(self->sink, BINDSTATUS_MIMETYPEAVAILABLE, type);
				uri_free(type);
			}
		}
	}
	EnterCriticalSection(&self->lock);
	finished = self->finished;
	received = self->received;
	LeaveCriticalSection(&self->lock);
	if (received)
	{
		flags = self->reported_first ? BSCF_INTERMEDIATEDATANOTIFICATION : BSCF_FIRSTDATANOTIFICATION;
		self->reported_first = TRUE;
		if (finished)
			flags = (flags & BSCF_FIRSTDATANOTIFICATION) | BSCF_LASTDATANOTIFICATION | BSCF_DATAFULLYAVAILABLE;
		IInternetProtocolSink_ReportData(self->sink, flags, (ULONG)received, (ULONG)(self->expected ? self->expected : received));
	}
	if (finished && self->sink)
		report_result(self, self->final_result, self->final_error, self->final_text);
	return S_OK;
}

static HRESULT WINAPI protocol_abort(IInternetProtocolEx *iface, HRESULT reason, DWORD options)
{
	protocol_object *self = from_protocol(iface);
	stop_worker(self);
	report_result(self, reason, ERROR_SUCCESS, NULL);
	return S_OK;
}

static HRESULT WINAPI protocol_terminate(IInternetProtocolEx *iface, DWORD options)
{
	protocol_object *self = from_protocol(iface);
	/* An opened file stays readable after this, until the protocol goes away. */
	if (self->kind != KIND_FILE)
		close_everything(self);
	release_sink(self);
	return S_OK;
}

static HRESULT WINAPI protocol_suspend(IInternetProtocolEx *iface)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI protocol_resume(IInternetProtocolEx *iface)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI protocol_read(IInternetProtocolEx *iface, void *buffer, ULONG wanted, ULONG *got)
{
	protocol_object *self = from_protocol(iface);
	ULONG served = 0;
	HRESULT result = S_OK;
	if (got)
		*got = 0;
	if (!buffer)
		return E_POINTER;
	switch (self->kind)
	{
	case KIND_FILE:
		if (!self->file)
			return INET_E_DATA_NOT_AVAILABLE;
		if (!ReadFile(self->file, buffer, wanted, &served, NULL))
			return INET_E_DOWNLOAD_FAILURE;
		if (served < wanted)
			result = S_FALSE;
		break;
	case KIND_MK:
		if (!self->stream)
			return INET_E_DATA_NOT_AVAILABLE;
		result = IStream_Read(self->stream, buffer, wanted, &served);
		if (FAILED(result))
			return result;
		break;
	default:
		EnterCriticalSection(&self->lock);
		while (served < wanted && self->first)
		{
			chunk *item = self->first;
			DWORD take = min(wanted - served, item->size - item->used);
			memcpy((BYTE *)buffer + served, item->data + item->used, take);
			served += take;
			item->used += take;
			if (item->used == item->size)
			{
				self->first = item->next;
				if (!self->first)
					self->last = NULL;
				uri_free(item);
			}
		}
		if (!served && wanted)
			result = self->finished ? (FAILED(self->final_result) ? self->final_result : S_FALSE) : E_PENDING;
		else if (served < wanted)
			result = self->finished ? S_FALSE : E_PENDING;
		LeaveCriticalSection(&self->lock);
		if (got)
			*got = served;
		return result;
	}
	if (got)
		*got = served;
	return served == wanted ? S_OK : S_FALSE;
}

static HRESULT WINAPI protocol_seek(IInternetProtocolEx *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *position)
{
	return E_FAIL;
}

static HRESULT WINAPI protocol_lock_request(IInternetProtocolEx *iface, DWORD options)
{
	return S_OK;
}

static HRESULT WINAPI protocol_unlock_request(IInternetProtocolEx *iface)
{
	return S_OK;
}

static const IInternetProtocolExVtbl protocol_vtable = {
	protocol_query_interface, protocol_add_ref, protocol_release, protocol_start, protocol_continue, protocol_abort, protocol_terminate, protocol_suspend,
	protocol_resume, protocol_read, protocol_seek, protocol_lock_request, protocol_unlock_request, protocol_start_ex,
};

static HRESULT WINAPI priority_query_interface(IInternetPriority *iface, REFIID iid, void **object)
{
	return IUnknown_QueryInterface(from_priority(iface)->outer, iid, object);
}

static ULONG WINAPI priority_add_ref(IInternetPriority *iface)
{
	return IUnknown_AddRef(from_priority(iface)->outer);
}

static ULONG WINAPI priority_release(IInternetPriority *iface)
{
	return IUnknown_Release(from_priority(iface)->outer);
}

static HRESULT WINAPI priority_set(IInternetPriority *iface, LONG priority)
{
	from_priority(iface)->priority = priority;
	return S_OK;
}

static HRESULT WINAPI priority_get(IInternetPriority *iface, LONG *priority)
{
	if (!priority)
		return E_POINTER;
	*priority = from_priority(iface)->priority;
	return S_OK;
}

static const IInternetPriorityVtbl priority_vtable = {priority_query_interface, priority_add_ref, priority_release, priority_set, priority_get};

static HRESULT WINAPI http_info_query_interface(IWinInetHttpInfo *iface, REFIID iid, void **object)
{
	return IUnknown_QueryInterface(from_http_info(iface)->outer, iid, object);
}

static ULONG WINAPI http_info_add_ref(IWinInetHttpInfo *iface)
{
	return IUnknown_AddRef(from_http_info(iface)->outer);
}

static ULONG WINAPI http_info_release(IWinInetHttpInfo *iface)
{
	return IUnknown_Release(from_http_info(iface)->outer);
}

static HRESULT WINAPI http_info_query_option(IWinInetHttpInfo *iface, DWORD option, void *buffer, DWORD *size)
{
	protocol_object *self = from_http_info(iface);
	if (!self->request)
		return E_FAIL;
	return InternetQueryOptionW(self->request, option, buffer, size) ? S_OK : S_FALSE;
}

static HRESULT WINAPI http_info_query_info(IWinInetHttpInfo *iface, DWORD option, void *buffer, DWORD *size, DWORD *flags, DWORD *reserved)
{
	protocol_object *self = from_http_info(iface);
	if (!self->request)
		return E_FAIL;
	return HttpQueryInfoA(self->request, option, buffer, size, flags) ? S_OK : S_FALSE;
}

static const IWinInetHttpInfoVtbl http_info_vtable = {http_info_query_interface, http_info_add_ref, http_info_release, http_info_query_option, http_info_query_info};

static HRESULT protocol_new(protocol_kind kind, IUnknown *outer, REFIID iid, void **object)
{
	protocol_object *self;
	HRESULT result;
	*object = NULL;
	if (outer && !IsEqualGUID(iid, &IID_IUnknown))
		return CLASS_E_NOAGGREGATION;
	self = uri_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->IInternetProtocolEx_iface.lpVtbl = &protocol_vtable;
	self->IInternetPriority_iface.lpVtbl = &priority_vtable;
	self->IWinInetHttpInfo_iface.lpVtbl = &http_info_vtable;
	self->IUnknown_inner.lpVtbl = &inner_vtable;
	self->references = 1;
	self->outer = outer ? outer : &self->IUnknown_inner;
	self->kind = kind;
	InitializeCriticalSection(&self->lock);
	urlmon_lock_module();
	result = IUnknown_QueryInterface(&self->IUnknown_inner, iid, object);
	IUnknown_Release(&self->IUnknown_inner);
	return result;
}

HRESULT urlmon_create_file_protocol(IUnknown *outer, REFIID iid, void **object)
{
	return protocol_new(KIND_FILE, outer, iid, object);
}

HRESULT urlmon_create_mk_protocol(IUnknown *outer, REFIID iid, void **object)
{
	return protocol_new(KIND_MK, outer, iid, object);
}

HRESULT urlmon_create_http_protocol(IUnknown *outer, REFIID iid, void **object)
{
	return protocol_new(KIND_HTTP, outer, iid, object);
}

HRESULT urlmon_create_https_protocol(IUnknown *outer, REFIID iid, void **object)
{
	return protocol_new(KIND_HTTPS, outer, iid, object);
}

HRESULT urlmon_create_ftp_protocol(IUnknown *outer, REFIID iid, void **object)
{
	return protocol_new(KIND_FTP, outer, iid, object);
}
