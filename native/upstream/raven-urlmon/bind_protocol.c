/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "urlmon_private.h"

/* Stands between a binder and the protocol that serves the URL: it finds the handler, brings
 * calls from other threads back to the thread that started, looks at the first bytes to settle
 * the type, and puts a MIME filter in the way when one exists. */
#define SNIFF_SIZE 2048
#define WM_BIND_TASK (WM_USER + 0x60)

typedef enum
{
	TASK_CONTINUE,
	TASK_PROGRESS,
	TASK_DATA,
	TASK_RESULT,
} task_kind;

typedef struct bind_task
{
	struct bind_task *next;
	task_kind kind;
	PROTOCOLDATA data;
	ULONG code;
	ULONG value;
	ULONG limit;
	HRESULT result;
	WCHAR *text;
} bind_task;

typedef struct
{
	IInternetProtocolEx IInternetProtocolEx_iface;
	IInternetBindInfo IInternetBindInfo_iface;
	IInternetPriority IInternetPriority_iface;
	IServiceProvider IServiceProvider_iface;
	IInternetProtocolSink IInternetProtocolSink_iface;
	LONG references;
	IUnknown *inner_unknown;
	IInternetProtocol *inner;
	IInternetProtocolSink *sink;
	IInternetBindInfo *bind_info;
	IServiceProvider *services;
	IInternetProtocol *filter;
	IInternetProtocolSink *filter_sink;
	LONG priority;
	DWORD start_flags;
	DWORD thread;
	HWND window;
	CRITICAL_SECTION lock;
	bind_task *first, *last;
	WCHAR *url;
	WCHAR *mime;
	BOOL verify_mime;
	BOOL sniffed;
	BOOL ended;
	BYTE *head;
	DWORD head_size, head_used;
	ULONG last_value, last_limit, last_flags;
	HRESULT pending_result;
	BOOL has_pending_result;
} bind_protocol;

static inline bind_protocol *from_protocol(IInternetProtocolEx *iface)
{
	return CONTAINING_RECORD(iface, bind_protocol, IInternetProtocolEx_iface);
}

static inline bind_protocol *from_bind_info(IInternetBindInfo *iface)
{
	return CONTAINING_RECORD(iface, bind_protocol, IInternetBindInfo_iface);
}

static inline bind_protocol *from_priority(IInternetPriority *iface)
{
	return CONTAINING_RECORD(iface, bind_protocol, IInternetPriority_iface);
}

static inline bind_protocol *from_services(IServiceProvider *iface)
{
	return CONTAINING_RECORD(iface, bind_protocol, IServiceProvider_iface);
}

static inline bind_protocol *from_sink(IInternetProtocolSink *iface)
{
	return CONTAINING_RECORD(iface, bind_protocol, IInternetProtocolSink_iface);
}

static void deliver_progress(bind_protocol *self, ULONG code, const WCHAR *text);
static void deliver_data(bind_protocol *self, ULONG flags, ULONG value, ULONG limit);
static void deliver_result(bind_protocol *self, HRESULT result, ULONG error, const WCHAR *text);

static HRESULT WINAPI protocol_query_interface(IInternetProtocolEx *iface, REFIID iid, void **object)
{
	bind_protocol *self = from_protocol(iface);
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInternetProtocolRoot) || IsEqualGUID(iid, &IID_IInternetProtocol) || IsEqualGUID(iid, &IID_IInternetProtocolEx))
		*object = &self->IInternetProtocolEx_iface;
	else if (IsEqualGUID(iid, &IID_IInternetBindInfo))
		*object = &self->IInternetBindInfo_iface;
	else if (IsEqualGUID(iid, &IID_IInternetPriority))
		*object = &self->IInternetPriority_iface;
	else if (IsEqualGUID(iid, &IID_IServiceProvider))
		*object = &self->IServiceProvider_iface;
	else if (IsEqualGUID(iid, &IID_IInternetProtocolSink))
		*object = &self->IInternetProtocolSink_iface;
	else if ((IsEqualGUID(iid, &IID_IWinInetInfo) || IsEqualGUID(iid, &IID_IWinInetHttpInfo)) && self->inner)
		return IInternetProtocol_QueryInterface(self->inner, iid, object);
	else
	{
		*object = NULL;
		return E_NOINTERFACE;
	}
	IUnknown_AddRef((IUnknown *)*object);
	return S_OK;
}

static ULONG WINAPI protocol_add_ref(IInternetProtocolEx *iface)
{
	return InterlockedIncrement(&from_protocol(iface)->references);
}

static void clear_binding_state(bind_protocol *self)
{
	if (self->filter)
		IInternetProtocol_Release(self->filter);
	self->filter = NULL;
	if (self->filter_sink)
		IInternetProtocolSink_Release(self->filter_sink);
	self->filter_sink = NULL;
	if (self->inner)
		IInternetProtocol_Release(self->inner);
	self->inner = NULL;
	if (self->inner_unknown)
		IUnknown_Release(self->inner_unknown);
	self->inner_unknown = NULL;
	if (self->sink)
		IInternetProtocolSink_Release(self->sink);
	self->sink = NULL;
	if (self->bind_info)
		IInternetBindInfo_Release(self->bind_info);
	self->bind_info = NULL;
	if (self->services)
		IServiceProvider_Release(self->services);
	self->services = NULL;
}

static ULONG WINAPI protocol_release(IInternetProtocolEx *iface)
{
	bind_protocol *self = from_protocol(iface);
	ULONG left = InterlockedDecrement(&self->references);
	if (!left)
	{
		bind_task *task;
		clear_binding_state(self);
		if (self->window)
			DestroyWindow(self->window);
		while ((task = self->first))
		{
			self->first = task->next;
			uri_free(task->text);
			uri_free(task);
		}
		DeleteCriticalSection(&self->lock);
		uri_free(self->head);
		uri_free(self->mime);
		uri_free(self->url);
		uri_free(self);
		urlmon_unlock_module();
	}
	return left;
}

static LRESULT CALLBACK task_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	bind_protocol *self;
	bind_task *task;
	if (message != WM_BIND_TASK)
		return DefWindowProcW(window, message, wparam, lparam);
	self = (bind_protocol *)lparam;
	for (;;)
	{
		EnterCriticalSection(&self->lock);
		task = self->first;
		if (task)
		{
			self->first = task->next;
			if (!self->first)
				self->last = NULL;
		}
		LeaveCriticalSection(&self->lock);
		if (!task)
			break;
		switch (task->kind)
		{
		case TASK_CONTINUE:
			if (self->inner)
				IInternetProtocol_Continue(self->inner, &task->data);
			break;
		case TASK_PROGRESS:
			deliver_progress(self, task->code, task->text);
			break;
		case TASK_DATA:
			deliver_data(self, task->code, task->value, task->limit);
			break;
		case TASK_RESULT:
			deliver_result(self, task->result, task->code, task->text);
			break;
		}
		uri_free(task->text);
		uri_free(task);
	}
	IInternetProtocolEx_Release(&self->IInternetProtocolEx_iface);
	return 0;
}

static HWND task_window(void)
{
	static const WCHAR class_name[] = L"URL Moniker Notification Window";
	static ATOM registered;
	WNDCLASSEXW window_class = {sizeof(window_class)};
	if (!registered)
	{
		window_class.lpfnWndProc = task_window_proc;
		window_class.hInstance = urlmon_instance;
		window_class.lpszClassName = class_name;
		registered = RegisterClassExW(&window_class);
	}
	return CreateWindowExW(0, class_name, class_name, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, urlmon_instance, NULL);
}

/* Only the thread that started may talk to the binder. */
static BOOL on_other_thread(const bind_protocol *self)
{
	return self->window && GetCurrentThreadId() != self->thread;
}

static void post_task(bind_protocol *self, bind_task *task)
{
	EnterCriticalSection(&self->lock);
	if (self->last)
		self->last->next = task;
	else
		self->first = task;
	self->last = task;
	LeaveCriticalSection(&self->lock);
	IInternetProtocolEx_AddRef(&self->IInternetProtocolEx_iface);
	PostMessageW(self->window, WM_BIND_TASK, 0, (LPARAM)self);
}

static bind_task *new_task(task_kind kind, const WCHAR *text)
{
	bind_task *task = uri_alloc(sizeof(*task));
	if (task)
	{
		task->kind = kind;
		if (text)
			task->text = uri_text_copy(text);
	}
	return task;
}

static void report_mime(bind_protocol *self, const WCHAR *mime, BOOL verified)
{
	IClassFactory *factory;
	CLSID clsid;
	if (self->sink)
		IInternetProtocolSink_ReportProgress(self->sink, verified ? BINDSTATUS_VERIFIEDMIMETYPEAVAILABLE : BINDSTATUS_MIMETYPEAVAILABLE, mime);
	if (self->filter || !mime || !self->sink)
		return;
	factory = urlmon_mime_filter(mime, &clsid);
	if (factory)
	{
		IUnknown *created = NULL;
		if (SUCCEEDED(IClassFactory_CreateInstance(factory, NULL, &IID_IUnknown, (void **)&created)))
		{
			PROTOCOLFILTERDATA data = {sizeof(data), &self->IInternetProtocolSink_iface, self->inner, NULL, 0};
			WCHAR guid[40];
			StringFromGUID2(&clsid, guid, ARRAYSIZE(guid));
			if (SUCCEEDED(IUnknown_QueryInterface(created, &IID_IInternetProtocol, (void **)&self->filter)))
			{
				if (FAILED(IUnknown_QueryInterface(created, &IID_IInternetProtocolSink, (void **)&self->filter_sink)))
					self->filter_sink = NULL;
				IInternetProtocolSink_ReportProgress(self->sink, BINDSTATUS_LOADINGMIMEHANDLER, NULL);
				IInternetProtocolSink_ReportProgress(self->sink, BINDSTATUS_FILTERREPORTMIMETYPE, mime);
				IInternetProtocol_Start(self->filter, mime, &self->IInternetProtocolSink_iface, &self->IInternetBindInfo_iface, PI_FILTER_MODE | PI_FORCE_ASYNC, (HANDLE_PTR)&data);
			}
			IUnknown_Release(created);
		}
		IClassFactory_Release(factory);
	}
}

static void deliver_progress(bind_protocol *self, ULONG code, const WCHAR *text)
{
	if (!self->sink)
		return;
	switch (code)
	{
	case BINDSTATUS_MIMETYPEAVAILABLE:
		uri_free(self->mime);
		self->mime = text ? uri_text_copy(text) : NULL;
		if (!self->verify_mime)
			report_mime(self, self->mime, FALSE);
		break;
	case BINDSTATUS_VERIFIEDMIMETYPEAVAILABLE:
		self->verify_mime = FALSE;
		self->sniffed = TRUE;
		uri_free(self->mime);
		self->mime = text ? uri_text_copy(text) : NULL;
		report_mime(self, self->mime, FALSE);
		break;
	default:
		IInternetProtocolSink_ReportProgress(self->sink, code, text);
		break;
	}
}

/* Keeps some first bytes aside, lets them decide the type, then hands them out through Read. */
static void sniff_head(bind_protocol *self)
{
	WCHAR *found = NULL;
	ULONG got = 0;
	HRESULT result;
	if (self->sniffed)
		return;
	if (!self->head)
		self->head = uri_alloc(SNIFF_SIZE);
	if (!self->head)
		return;
	while (self->head_size < SNIFF_SIZE)
	{
		result = IInternetProtocol_Read(self->inner, self->head + self->head_size, SNIFF_SIZE - self->head_size, &got);
		if (got)
			self->head_size += got;
		if (result != S_OK || !got)
			break;
	}
	self->sniffed = TRUE;
	self->verify_mime = FALSE;
	if (SUCCEEDED(FindMimeFromData(NULL, self->url, self->head, self->head_size, self->mime, 0, &found, 0)))
	{
		uri_free(self->mime);
		self->mime = uri_text_copy(found);
		CoTaskMemFree(found);
	}
	report_mime(self, self->mime, FALSE);
}

static void deliver_data(bind_protocol *self, ULONG flags, ULONG value, ULONG limit)
{
	if (!self->sink)
		return;
	if (self->verify_mime)
	{
		if (!(flags & BSCF_LASTDATANOTIFICATION) && self->head_size < SNIFF_SIZE && value < SNIFF_SIZE && (limit == 0 || value < limit))
		{
			/* Too little to judge yet: try again with more. */
			self->last_flags |= flags;
			return;
		}
		sniff_head(self);
	}
	else if (!self->sniffed)
		self->sniffed = TRUE;
	if (self->last_flags & BSCF_FIRSTDATANOTIFICATION)
		flags |= BSCF_FIRSTDATANOTIFICATION;
	self->last_flags = 0;
	self->last_value = value;
	self->last_limit = limit;
	if (self->filter_sink)
		IInternetProtocolSink_ReportData(self->filter_sink, flags, value, limit);
	else
		IInternetProtocolSink_ReportData(self->sink, flags, value, limit);
}

static void deliver_result(bind_protocol *self, HRESULT result, ULONG error, const WCHAR *text)
{
	if (self->verify_mime && SUCCEEDED(result))
		sniff_head(self);
	if (self->sink && !self->ended)
	{
		self->ended = TRUE;
		IInternetProtocolSink_ReportResult(self->sink, result, error, text);
	}
}

static HRESULT WINAPI sink_query_interface(IInternetProtocolSink *iface, REFIID iid, void **object)
{
	return protocol_query_interface(&from_sink(iface)->IInternetProtocolEx_iface, iid, object);
}

static ULONG WINAPI sink_add_ref(IInternetProtocolSink *iface)
{
	return protocol_add_ref(&from_sink(iface)->IInternetProtocolEx_iface);
}

static ULONG WINAPI sink_release(IInternetProtocolSink *iface)
{
	return protocol_release(&from_sink(iface)->IInternetProtocolEx_iface);
}

static HRESULT WINAPI sink_switch(IInternetProtocolSink *iface, PROTOCOLDATA *data)
{
	bind_protocol *self = from_sink(iface);
	bind_task *task;
	if (!data)
		return E_INVALIDARG;
	if (self->window)
	{
		task = new_task(TASK_CONTINUE, NULL);
		if (!task)
			return E_OUTOFMEMORY;
		task->data = *data;
		post_task(self, task);
		return S_OK;
	}
	return self->sink ? IInternetProtocolSink_Switch(self->sink, data) : E_UNEXPECTED;
}

static HRESULT WINAPI sink_report_progress(IInternetProtocolSink *iface, ULONG code, LPCWSTR text)
{
	bind_protocol *self = from_sink(iface);
	if (on_other_thread(self))
	{
		bind_task *task = new_task(TASK_PROGRESS, text);
		if (!task)
			return E_OUTOFMEMORY;
		task->code = code;
		post_task(self, task);
		return S_OK;
	}
	deliver_progress(self, code, text);
	return S_OK;
}

static HRESULT WINAPI sink_report_data(IInternetProtocolSink *iface, DWORD flags, ULONG value, ULONG limit)
{
	bind_protocol *self = from_sink(iface);
	if (on_other_thread(self))
	{
		bind_task *task = new_task(TASK_DATA, NULL);
		if (!task)
			return E_OUTOFMEMORY;
		task->code = flags;
		task->value = value;
		task->limit = limit;
		post_task(self, task);
		return S_OK;
	}
	deliver_data(self, flags, value, limit);
	return S_OK;
}

static HRESULT WINAPI sink_report_result(IInternetProtocolSink *iface, HRESULT result, DWORD error, LPCWSTR text)
{
	bind_protocol *self = from_sink(iface);
	if (on_other_thread(self))
	{
		bind_task *task = new_task(TASK_RESULT, text);
		if (!task)
			return E_OUTOFMEMORY;
		task->result = result;
		task->code = error;
		post_task(self, task);
		return S_OK;
	}
	deliver_result(self, result, error, text);
	return S_OK;
}

static const IInternetProtocolSinkVtbl sink_vtable = {sink_query_interface, sink_add_ref, sink_release, sink_switch, sink_report_progress, sink_report_data, sink_report_result};

static HRESULT start_inner(bind_protocol *self, IUri *uri, IInternetProtocolSink *sink, IInternetBindInfo *info, DWORD flags, HANDLE_PTR reserved)
{
	IInternetProtocolEx *modern = NULL;
	IClassFactory *factory = NULL;
	BOOL from_session = FALSE;
	CLSID clsid = {0};
	BSTR url = NULL;
	HRESULT result;
	if (self->inner)
		return E_UNEXPECTED;
	result = IUri_GetDisplayUri(uri, &url);
	if (FAILED(result))
		return result;
	self->url = uri_text_copy(url);
	self->start_flags = flags;
	self->thread = GetCurrentThreadId();
	self->verify_mime = (flags & PI_MIMEVERIFICATION) != 0;
	IInternetProtocolSink_AddRef(sink);
	self->sink = sink;
	IInternetBindInfo_AddRef(info);
	self->bind_info = info;
	if (FAILED(IInternetProtocolSink_QueryInterface(sink, &IID_IServiceProvider, (void **)&self->services)))
		self->services = NULL;
	if (!(flags & PI_APARTMENTTHREADED) || (flags & PI_FORCE_ASYNC) || TRUE)
		self->window = task_window();
	result = urlmon_protocol_factory(url, &from_session, &factory, &clsid);
	SysFreeString(url);
	if (FAILED(result))
	{
		deliver_result(self, result, 0, NULL);
		return result;
	}
	result = IClassFactory_CreateInstance(factory, NULL, &IID_IUnknown, (void **)&self->inner_unknown);
	IClassFactory_Release(factory);
	if (SUCCEEDED(result))
		result = IUnknown_QueryInterface(self->inner_unknown, &IID_IInternetProtocol, (void **)&self->inner);
	if (FAILED(result))
	{
		deliver_result(self, result, 0, NULL);
		return result;
	}
	if (SUCCEEDED(IInternetProtocol_QueryInterface(self->inner, &IID_IInternetProtocolEx, (void **)&modern)))
	{
		result = IInternetProtocolEx_StartEx(modern, uri, &self->IInternetProtocolSink_iface, &self->IInternetBindInfo_iface, flags & ~PI_MIMEVERIFICATION, reserved);
		IInternetProtocolEx_Release(modern);
	}
	else
	{
		BSTR display = NULL;
		result = IUri_GetDisplayUri(uri, &display);
		if (SUCCEEDED(result))
			result = IInternetProtocol_Start(self->inner, display, &self->IInternetProtocolSink_iface, &self->IInternetBindInfo_iface, flags & ~PI_MIMEVERIFICATION, reserved);
		SysFreeString(display);
	}
	return result;
}

static HRESULT WINAPI protocol_start_ex(IInternetProtocolEx *iface, IUri *uri, IInternetProtocolSink *sink, IInternetBindInfo *info, DWORD flags, HANDLE_PTR reserved)
{
	if (!uri || !sink || !info)
		return E_INVALIDARG;
	return start_inner(from_protocol(iface), uri, sink, info, flags, reserved);
}

static HRESULT WINAPI protocol_start(IInternetProtocolEx *iface, LPCWSTR url, IInternetProtocolSink *sink, IInternetBindInfo *info, DWORD flags, HANDLE_PTR reserved)
{
	IUri *uri;
	HRESULT result;
	if (!url || !sink || !info)
		return E_INVALIDARG;
	result = CreateUri(url, Uri_CREATE_FILE_USE_DOS_PATH, 0, &uri);
	if (FAILED(result))
		return result;
	result = start_inner(from_protocol(iface), uri, sink, info, flags, reserved);
	IUri_Release(uri);
	return result;
}

static HRESULT WINAPI protocol_continue(IInternetProtocolEx *iface, PROTOCOLDATA *data)
{
	bind_protocol *self = from_protocol(iface);
	return self->inner ? IInternetProtocol_Continue(self->inner, data) : E_UNEXPECTED;
}

static HRESULT WINAPI protocol_abort(IInternetProtocolEx *iface, HRESULT reason, DWORD options)
{
	bind_protocol *self = from_protocol(iface);
	return self->inner ? IInternetProtocol_Abort(self->inner, reason, options) : E_UNEXPECTED;
}

static HRESULT WINAPI protocol_terminate(IInternetProtocolEx *iface, DWORD options)
{
	bind_protocol *self = from_protocol(iface);
	if (self->filter)
		IInternetProtocol_Terminate(self->filter, 0);
	if (self->inner)
		IInternetProtocol_Terminate(self->inner, options);
	clear_binding_state(self);
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
	bind_protocol *self = from_protocol(iface);
	ULONG served = 0, more = 0;
	HRESULT result = S_OK;
	if (self->filter)
		return IInternetProtocol_Read(self->filter, buffer, wanted, got);
	if (self->head_used < self->head_size)
	{
		served = min(wanted, self->head_size - self->head_used);
		memcpy(buffer, self->head + self->head_used, served);
		self->head_used += served;
	}
	if (served < wanted)
	{
		if (!self->inner)
			result = served ? S_FALSE : E_FAIL;
		else
		{
			result = IInternetProtocol_Read(self->inner, (BYTE *)buffer + served, wanted - served, &more);
			served += more;
			if (served && FAILED(result))
				result = S_OK;
		}
	}
	if (got)
		*got = served;
	return result;
}

static HRESULT WINAPI protocol_seek(IInternetProtocolEx *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *position)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI protocol_lock_request(IInternetProtocolEx *iface, DWORD options)
{
	bind_protocol *self = from_protocol(iface);
	return self->inner ? IInternetProtocol_LockRequest(self->inner, options) : S_OK;
}

static HRESULT WINAPI protocol_unlock_request(IInternetProtocolEx *iface)
{
	bind_protocol *self = from_protocol(iface);
	return self->inner ? IInternetProtocol_UnlockRequest(self->inner) : S_OK;
}

static const IInternetProtocolExVtbl protocol_vtable = {
	protocol_query_interface, protocol_add_ref, protocol_release, protocol_start, protocol_continue, protocol_abort, protocol_terminate, protocol_suspend,
	protocol_resume, protocol_read, protocol_seek, protocol_lock_request, protocol_unlock_request, protocol_start_ex,
};

static HRESULT WINAPI bind_info_query_interface(IInternetBindInfo *iface, REFIID iid, void **object)
{
	return protocol_query_interface(&from_bind_info(iface)->IInternetProtocolEx_iface, iid, object);
}

static ULONG WINAPI bind_info_add_ref(IInternetBindInfo *iface)
{
	return protocol_add_ref(&from_bind_info(iface)->IInternetProtocolEx_iface);
}

static ULONG WINAPI bind_info_release(IInternetBindInfo *iface)
{
	return protocol_release(&from_bind_info(iface)->IInternetProtocolEx_iface);
}

static HRESULT WINAPI bind_info_get(IInternetBindInfo *iface, DWORD *flags, BINDINFO *info)
{
	bind_protocol *self = from_bind_info(iface);
	return self->bind_info ? IInternetBindInfo_GetBindInfo(self->bind_info, flags, info) : E_UNEXPECTED;
}

static HRESULT WINAPI bind_info_get_string(IInternetBindInfo *iface, ULONG type, LPOLESTR *strings, ULONG count, ULONG *fetched)
{
	bind_protocol *self = from_bind_info(iface);
	return self->bind_info ? IInternetBindInfo_GetBindString(self->bind_info, type, strings, count, fetched) : E_UNEXPECTED;
}

static const IInternetBindInfoVtbl bind_info_vtable = {bind_info_query_interface, bind_info_add_ref, bind_info_release, bind_info_get, bind_info_get_string};

static HRESULT WINAPI priority_query_interface(IInternetPriority *iface, REFIID iid, void **object)
{
	return protocol_query_interface(&from_priority(iface)->IInternetProtocolEx_iface, iid, object);
}

static ULONG WINAPI priority_add_ref(IInternetPriority *iface)
{
	return protocol_add_ref(&from_priority(iface)->IInternetProtocolEx_iface);
}

static ULONG WINAPI priority_release(IInternetPriority *iface)
{
	return protocol_release(&from_priority(iface)->IInternetProtocolEx_iface);
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

static HRESULT WINAPI services_query_interface(IServiceProvider *iface, REFIID iid, void **object)
{
	return protocol_query_interface(&from_services(iface)->IInternetProtocolEx_iface, iid, object);
}

static ULONG WINAPI services_add_ref(IServiceProvider *iface)
{
	return protocol_add_ref(&from_services(iface)->IInternetProtocolEx_iface);
}

static ULONG WINAPI services_release(IServiceProvider *iface)
{
	return protocol_release(&from_services(iface)->IInternetProtocolEx_iface);
}

static HRESULT WINAPI services_query(IServiceProvider *iface, REFGUID service, REFIID iid, void **object)
{
	bind_protocol *self = from_services(iface);
	*object = NULL;
	return self->services ? IServiceProvider_QueryService(self->services, service, iid, object) : E_NOINTERFACE;
}

static const IServiceProviderVtbl services_vtable = {services_query_interface, services_add_ref, services_release, services_query};

HRESULT urlmon_create_bind_protocol(IInternetProtocol **protocol)
{
	bind_protocol *self = uri_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->IInternetProtocolEx_iface.lpVtbl = &protocol_vtable;
	self->IInternetBindInfo_iface.lpVtbl = &bind_info_vtable;
	self->IInternetPriority_iface.lpVtbl = &priority_vtable;
	self->IServiceProvider_iface.lpVtbl = &services_vtable;
	self->IInternetProtocolSink_iface.lpVtbl = &sink_vtable;
	self->references = 1;
	InitializeCriticalSection(&self->lock);
	urlmon_lock_module();
	*protocol = (IInternetProtocol *)&self->IInternetProtocolEx_iface;
	return S_OK;
}
