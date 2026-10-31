/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "urlmon_private.h"

/* One bind of a URL moniker: the client's IBindStatusCallback on one side, a bind protocol on
 * the other, and on top the object or stream the client asked for. */
typedef struct
{
	IStream IStream_iface;
	LONG references;
	IInternetProtocol *protocol;
	ULONGLONG position;
	BOOL finished;
} protocol_stream;

typedef struct
{
	IBinding IBinding_iface;
	IInternetProtocolSink IInternetProtocolSink_iface;
	IInternetBindInfo IInternetBindInfo_iface;
	IServiceProvider IServiceProvider_iface;
	LONG references;
	IBindStatusCallback *callback;
	IServiceProvider *client_services;
	IInternetProtocol *protocol;
	IUri *uri;
	IBindCtx *context;
	IMoniker *moniker;
	protocol_stream *stream;
	BINDINFO info;
	DWORD bind_flags;
	BOOL to_object;
	IID iid;
	IUnknown *result_object;
	WCHAR *mime;
	WCHAR *cache_file;
	BOOL started_data;
	BOOL stopped;
	HRESULT stop_result;
	DWORD stop_error;
	WCHAR *stop_text;
	ULONG last_value, last_limit;
	FORMATETC format;
	BOOL async;
} binding;

static inline binding *from_binding(IBinding *iface)
{
	return CONTAINING_RECORD(iface, binding, IBinding_iface);
}

static inline binding *from_sink(IInternetProtocolSink *iface)
{
	return CONTAINING_RECORD(iface, binding, IInternetProtocolSink_iface);
}

static inline binding *from_bind_info(IInternetBindInfo *iface)
{
	return CONTAINING_RECORD(iface, binding, IInternetBindInfo_iface);
}

static inline binding *from_services(IServiceProvider *iface)
{
	return CONTAINING_RECORD(iface, binding, IServiceProvider_iface);
}

static inline protocol_stream *from_stream(IStream *iface)
{
	return CONTAINING_RECORD(iface, protocol_stream, IStream_iface);
}

static HRESULT WINAPI stream_query_interface(IStream *iface, REFIID iid, void **object)
{
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_ISequentialStream) || IsEqualGUID(iid, &IID_IStream))
	{
		*object = iface;
		IStream_AddRef(iface);
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI stream_add_ref(IStream *iface)
{
	return InterlockedIncrement(&from_stream(iface)->references);
}

static ULONG WINAPI stream_release(IStream *iface)
{
	protocol_stream *self = from_stream(iface);
	ULONG left = InterlockedDecrement(&self->references);
	if (!left)
	{
		if (self->protocol)
		{
			IInternetProtocol_UnlockRequest(self->protocol);
			IInternetProtocol_Release(self->protocol);
		}
		uri_free(self);
		urlmon_unlock_module();
	}
	return left;
}

static HRESULT WINAPI stream_read(IStream *iface, void *buffer, ULONG wanted, ULONG *got)
{
	protocol_stream *self = from_stream(iface);
	ULONG served = 0;
	HRESULT result;
	if (got)
		*got = 0;
	if (!self->protocol)
		return S_FALSE;
	result = IInternetProtocol_Read(self->protocol, buffer, wanted, &served);
	self->position += served;
	if (got)
		*got = served;
	if (result == E_PENDING && served)
		return S_OK;
	if (result == S_FALSE && !served)
		self->finished = TRUE;
	/* Short counts are not the end: only a read that found nothing says so. */
	if (result == S_FALSE && served)
		return S_OK;
	return result;
}

static HRESULT WINAPI stream_write(IStream *iface, const void *buffer, ULONG count, ULONG *written)
{
	return STG_E_ACCESSDENIED;
}

static HRESULT WINAPI stream_seek(IStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *position)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI stream_set_size(IStream *iface, ULARGE_INTEGER size)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI stream_copy_to(IStream *iface, IStream *target, ULARGE_INTEGER count, ULARGE_INTEGER *read, ULARGE_INTEGER *written)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI stream_commit(IStream *iface, DWORD flags)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI stream_revert(IStream *iface)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI stream_lock_region(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER count, DWORD type)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI stream_unlock_region(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER count, DWORD type)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI stream_stat(IStream *iface, STATSTG *stat, DWORD flag)
{
	if (!stat)
		return E_POINTER;
	memset(stat, 0, sizeof(*stat));
	stat->type = STGTY_STREAM;
	stat->grfMode = STGM_READ;
	return S_OK;
}

static HRESULT WINAPI stream_clone(IStream *iface, IStream **out)
{
	return E_NOTIMPL;
}

static const IStreamVtbl stream_vtable = {
	stream_query_interface, stream_add_ref, stream_release, stream_read, stream_write, stream_seek, stream_set_size, stream_copy_to,
	stream_commit, stream_revert, stream_lock_region, stream_unlock_region, stream_stat, stream_clone,
};

static protocol_stream *stream_new(IInternetProtocol *protocol)
{
	protocol_stream *self = uri_alloc(sizeof(*self));
	if (self)
	{
		self->IStream_iface.lpVtbl = &stream_vtable;
		self->references = 1;
		IInternetProtocol_AddRef(protocol);
		IInternetProtocol_LockRequest(protocol, 0);
		self->protocol = protocol;
		urlmon_lock_module();
	}
	return self;
}

static HRESULT WINAPI binding_query_interface(IBinding *iface, REFIID iid, void **object)
{
	binding *self = from_binding(iface);
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IBinding))
		*object = &self->IBinding_iface;
	else if (IsEqualGUID(iid, &IID_IInternetProtocolSink))
		*object = &self->IInternetProtocolSink_iface;
	else if (IsEqualGUID(iid, &IID_IInternetBindInfo))
		*object = &self->IInternetBindInfo_iface;
	else if (IsEqualGUID(iid, &IID_IServiceProvider))
		*object = &self->IServiceProvider_iface;
	else if ((IsEqualGUID(iid, &IID_IWinInetInfo) || IsEqualGUID(iid, &IID_IWinInetHttpInfo)) && self->protocol)
		return IInternetProtocol_QueryInterface(self->protocol, iid, object);
	else
	{
		*object = NULL;
		return E_NOINTERFACE;
	}
	IUnknown_AddRef((IUnknown *)*object);
	return S_OK;
}

static ULONG WINAPI binding_add_ref(IBinding *iface)
{
	return InterlockedIncrement(&from_binding(iface)->references);
}

static ULONG WINAPI binding_release(IBinding *iface)
{
	binding *self = from_binding(iface);
	ULONG left = InterlockedDecrement(&self->references);
	if (!left)
	{
		if (self->protocol)
		{
			IInternetProtocol_Terminate(self->protocol, 0);
			IInternetProtocol_Release(self->protocol);
		}
		if (self->stream)
			IStream_Release(&self->stream->IStream_iface);
		if (self->callback)
			IBindStatusCallback_Release(self->callback);
		if (self->client_services)
			IServiceProvider_Release(self->client_services);
		if (self->context)
			IBindCtx_Release(self->context);
		if (self->moniker)
			IMoniker_Release(self->moniker);
		if (self->uri)
			IUri_Release(self->uri);
		if (self->result_object)
			IUnknown_Release(self->result_object);
		ReleaseBindInfo(&self->info);
		uri_free(self->mime);
		uri_free(self->cache_file);
		uri_free(self->stop_text);
		uri_free(self);
		urlmon_unlock_module();
	}
	return left;
}

static HRESULT WINAPI binding_abort(IBinding *iface)
{
	binding *self = from_binding(iface);
	if (self->stopped)
		return E_FAIL;
	if (!self->protocol)
		return E_FAIL;
	return IInternetProtocol_Abort(self->protocol, E_ABORT, 0);
}

static HRESULT WINAPI binding_suspend(IBinding *iface)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI binding_resume(IBinding *iface)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI binding_set_priority(IBinding *iface, LONG priority)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI binding_get_priority(IBinding *iface, LONG *priority)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI binding_get_bind_result(IBinding *iface, CLSID *protocol, DWORD *result, LPOLESTR *text, DWORD *reserved)
{
	binding *self = from_binding(iface);
	if (!protocol || !result || !text || reserved)
		return E_INVALIDARG;
	*protocol = CLSID_NULL;
	*result = self->stop_result;
	*text = NULL;
	return S_OK;
}

static const IBindingVtbl binding_vtable = {
	binding_query_interface, binding_add_ref, binding_release, binding_abort, binding_suspend, binding_resume, binding_set_priority, binding_get_priority, binding_get_bind_result,
};

static void data_available(binding *self, DWORD flags, ULONG value, ULONG limit)
{
	STGMEDIUM medium;
	memset(&medium, 0, sizeof(medium));
	if (self->stopped || self->to_object)
		return;
	if (!self->stream)
		self->stream = stream_new(self->protocol);
	if (!self->stream)
		return;
	self->format.dwAspect = DVASPECT_CONTENT;
	self->format.lindex = -1;
	self->format.tymed = (self->bind_flags & BINDF_NEEDFILE) && self->cache_file ? TYMED_FILE : TYMED_ISTREAM;
	if (self->format.tymed == TYMED_FILE)
	{
		medium.tymed = TYMED_FILE;
		medium.lpszFileName = self->cache_file;
	}
	else
	{
		medium.tymed = TYMED_ISTREAM;
		medium.pstm = &self->stream->IStream_iface;
	}
	/* Only for a client that copies the medium: this call's own reference stays ours. */
	medium.pUnkForRelease = (IUnknown *)&self->stream->IStream_iface;
	IBindStatusCallback_OnDataAvailable(self->callback, flags, value, &self->format, &medium);
}

static void make_object(binding *self)
{
	IPersistMoniker *persist = NULL;
	CLSID clsid;
	HRESULT result;
	if (self->result_object || !self->to_object)
		return;
	result = GetClassFileOrMime(NULL, NULL, NULL, 0, self->mime, 0, &clsid);
	if (FAILED(result))
	{
		self->stop_result = INET_E_CANNOT_INSTANTIATE_OBJECT;
		return;
	}
	IBindStatusCallback_OnProgress(self->callback, 0, 0, BINDSTATUS_CLASSIDAVAILABLE, NULL);
	result = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &self->iid, (void **)&self->result_object);
	if (FAILED(result))
	{
		self->stop_result = INET_E_CANNOT_INSTANTIATE_OBJECT;
		return;
	}
	IBindStatusCallback_OnProgress(self->callback, 0, 0, BINDSTATUS_BEGINSYNCOPERATION, NULL);
	if (SUCCEEDED(IUnknown_QueryInterface(self->result_object, &IID_IPersistMoniker, (void **)&persist)))
	{
		IPersistMoniker_Load(persist, TRUE, self->moniker, self->context, STGM_READ);
		IPersistMoniker_Release(persist);
	}
	IBindStatusCallback_OnProgress(self->callback, 0, 0, BINDSTATUS_ENDSYNCOPERATION, NULL);
	IBindStatusCallback_OnObjectAvailable(self->callback, &self->iid, self->result_object);
}

static HRESULT WINAPI sink_query_interface(IInternetProtocolSink *iface, REFIID iid, void **object)
{
	return binding_query_interface(&from_sink(iface)->IBinding_iface, iid, object);
}

static ULONG WINAPI sink_add_ref(IInternetProtocolSink *iface)
{
	return binding_add_ref(&from_sink(iface)->IBinding_iface);
}

static ULONG WINAPI sink_release(IInternetProtocolSink *iface)
{
	return binding_release(&from_sink(iface)->IBinding_iface);
}

static HRESULT WINAPI sink_switch(IInternetProtocolSink *iface, PROTOCOLDATA *data)
{
	binding *self = from_sink(iface);
	return self->protocol ? IInternetProtocol_Continue(self->protocol, data) : E_UNEXPECTED;
}

static HRESULT WINAPI sink_report_progress(IInternetProtocolSink *iface, ULONG code, LPCWSTR text)
{
	binding *self = from_sink(iface);
	if (self->stopped)
		return S_OK;
	switch (code)
	{
	case BINDSTATUS_MIMETYPEAVAILABLE:
	case BINDSTATUS_VERIFIEDMIMETYPEAVAILABLE:
		uri_free(self->mime);
		self->mime = text ? uri_text_copy(text) : NULL;
		self->format.cfFormat = text ? (CLIPFORMAT)RegisterClipboardFormatW(text) : 0;
		break;
	case BINDSTATUS_CACHEFILENAMEAVAILABLE:
		uri_free(self->cache_file);
		self->cache_file = text ? uri_text_copy(text) : NULL;
		break;
	default:
		break;
	}
	IBindStatusCallback_OnProgress(self->callback, 0, 0, code, text);
	return S_OK;
}

static HRESULT WINAPI sink_report_data(IInternetProtocolSink *iface, DWORD flags, ULONG value, ULONG limit)
{
	binding *self = from_sink(iface);
	BSTR url = NULL;
	if (self->stopped)
		return S_OK;
	self->last_value = value;
	self->last_limit = limit;
	IUri_GetDisplayUri(self->uri, &url);
	if (!self->started_data)
	{
		self->started_data = TRUE;
		IBindStatusCallback_OnProgress(self->callback, value, limit, BINDSTATUS_BEGINDOWNLOADDATA, url);
	}
	if (flags & BSCF_LASTDATANOTIFICATION)
		IBindStatusCallback_OnProgress(self->callback, value, limit, BINDSTATUS_ENDDOWNLOADDATA, url);
	else
		IBindStatusCallback_OnProgress(self->callback, value, limit, BINDSTATUS_DOWNLOADINGDATA, url);
	SysFreeString(url);
	data_available(self, flags, value, limit);
	return S_OK;
}

static HRESULT WINAPI sink_report_result(IInternetProtocolSink *iface, HRESULT result, DWORD error, LPCWSTR text)
{
	binding *self = from_sink(iface);
	if (self->stopped)
		return S_OK;
	self->stop_result = result;
	self->stop_error = error;
	self->stop_text = text ? uri_text_copy(text) : NULL;
	if (SUCCEEDED(result))
		make_object(self);
	self->stopped = TRUE;
	IBindStatusCallback_OnStopBinding(self->callback, self->stop_result, text);
	return S_OK;
}

static const IInternetProtocolSinkVtbl sink_vtable = {sink_query_interface, sink_add_ref, sink_release, sink_switch, sink_report_progress, sink_report_data, sink_report_result};

static HRESULT WINAPI bind_info_query_interface(IInternetBindInfo *iface, REFIID iid, void **object)
{
	return binding_query_interface(&from_bind_info(iface)->IBinding_iface, iid, object);
}

static ULONG WINAPI bind_info_add_ref(IInternetBindInfo *iface)
{
	return binding_add_ref(&from_bind_info(iface)->IBinding_iface);
}

static ULONG WINAPI bind_info_release(IInternetBindInfo *iface)
{
	return binding_release(&from_bind_info(iface)->IBinding_iface);
}

static HRESULT WINAPI bind_info_get(IInternetBindInfo *iface, DWORD *flags, BINDINFO *info)
{
	binding *self = from_bind_info(iface);
	if (!flags || !info)
		return E_INVALIDARG;
	*flags = self->bind_flags;
	return CopyBindInfo(&self->info, info);
}

static HRESULT WINAPI bind_info_get_string(IInternetBindInfo *iface, ULONG type, LPOLESTR *strings, ULONG count, ULONG *fetched)
{
	binding *self = from_bind_info(iface);
	WCHAR *copy;
	SIZE_T size;
	if (!strings || !fetched || !count)
		return E_INVALIDARG;
	*fetched = 0;
	switch (type)
	{
	case BINDSTRING_USER_AGENT:
		copy = urlmon_user_agent();
		break;
	case BINDSTRING_ACCEPT_MIMES:
		copy = uri_text_copy(L"*/*");
		break;
	case BINDSTRING_URL:
		copy = NULL;
		{
			BSTR url = NULL;
			if (SUCCEEDED(IUri_GetDisplayUri(self->uri, &url)))
			{
				copy = uri_text_copy(url);
				SysFreeString(url);
			}
		}
		break;
	default:
		return E_NOTIMPL;
	}
	if (!copy)
		return E_OUTOFMEMORY;
	size = (wcslen(copy) + 1) * sizeof(WCHAR);
	strings[0] = CoTaskMemAlloc(size);
	if (strings[0])
	{
		memcpy(strings[0], copy, size);
		*fetched = 1;
	}
	uri_free(copy);
	return *fetched ? S_OK : E_OUTOFMEMORY;
}

static const IInternetBindInfoVtbl bind_info_vtable = {bind_info_query_interface, bind_info_add_ref, bind_info_release, bind_info_get, bind_info_get_string};

static HRESULT WINAPI services_query_interface(IServiceProvider *iface, REFIID iid, void **object)
{
	return binding_query_interface(&from_services(iface)->IBinding_iface, iid, object);
}

static ULONG WINAPI services_add_ref(IServiceProvider *iface)
{
	return binding_add_ref(&from_services(iface)->IBinding_iface);
}

static ULONG WINAPI services_release(IServiceProvider *iface)
{
	return binding_release(&from_services(iface)->IBinding_iface);
}

static HRESULT WINAPI services_query(IServiceProvider *iface, REFGUID service, REFIID iid, void **object)
{
	binding *self = from_services(iface);
	*object = NULL;
	if (self->client_services && SUCCEEDED(IServiceProvider_QueryService(self->client_services, service, iid, object)))
		return S_OK;
	return IBindStatusCallback_QueryInterface(self->callback, iid, object);
}

static const IServiceProviderVtbl services_vtable = {services_query_interface, services_add_ref, services_release, services_query};

/* Waits for the end of a bind the caller wants finished on return, serving this thread's windows. */
static void run_until_done(binding *self)
{
	MSG message;
	while (!self->stopped)
	{
		MsgWaitForMultipleObjects(0, NULL, FALSE, INFINITE, QS_ALLINPUT);
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
}

HRESULT urlmon_bind(IMoniker *moniker, IUri *uri, IBindCtx *context, BOOL to_object, REFIID iid, void **result)
{
	IBindStatusCallback *callback;
	binding *self;
	HRESULT status;
	DWORD flags;
	*result = NULL;
	callback = urlmon_context_callback(context);
	if (!callback)
		return MK_E_SYNTAX;
	self = uri_alloc(sizeof(*self));
	if (!self)
	{
		IBindStatusCallback_Release(callback);
		return E_OUTOFMEMORY;
	}
	self->IBinding_iface.lpVtbl = &binding_vtable;
	self->IInternetProtocolSink_iface.lpVtbl = &sink_vtable;
	self->IInternetBindInfo_iface.lpVtbl = &bind_info_vtable;
	self->IServiceProvider_iface.lpVtbl = &services_vtable;
	self->references = 1;
	self->callback = callback;
	self->to_object = to_object;
	self->iid = *iid;
	IUri_AddRef(uri);
	self->uri = uri;
	IBindCtx_AddRef(context);
	self->context = context;
	IMoniker_AddRef(moniker);
	self->moniker = moniker;
	urlmon_lock_module();
	if (FAILED(IBindStatusCallback_QueryInterface(callback, &IID_IServiceProvider, (void **)&self->client_services)))
		self->client_services = NULL;
	self->info.cbSize = sizeof(self->info);
	status = IBindStatusCallback_GetBindInfo(callback, &self->bind_flags, &self->info);
	if (FAILED(status))
	{
		IBinding_Release(&self->IBinding_iface);
		return status;
	}
	self->async = (self->bind_flags & BINDF_ASYNCHRONOUS) != 0;
	status = urlmon_create_bind_protocol(&self->protocol);
	if (FAILED(status))
	{
		IBinding_Release(&self->IBinding_iface);
		return status;
	}
	status = IBindCtx_RegisterObjectParam(context, (WCHAR *)L"_BSCB_Binding_", (IUnknown *)&self->IBinding_iface);
	if (FAILED(status))
		status = S_OK;
	status = IBindStatusCallback_OnStartBinding(callback, 0, &self->IBinding_iface);
	if (FAILED(status))
	{
		IBinding_Abort(&self->IBinding_iface);
		IBindCtx_RevokeObjectParam(context, (WCHAR *)L"_BSCB_Binding_");
		IBinding_Release(&self->IBinding_iface);
		return status;
	}
	flags = PI_MIMEVERIFICATION;
	status = IInternetProtocolEx_StartEx((IInternetProtocolEx *)self->protocol, uri, &self->IInternetProtocolSink_iface, &self->IInternetBindInfo_iface, flags, 0);
	if (FAILED(status) && status != E_PENDING)
	{
		if (!self->stopped)
		{
			self->stopped = TRUE;
			self->stop_result = status;
			IBindStatusCallback_OnStopBinding(callback, status, NULL);
		}
		IBindCtx_RevokeObjectParam(context, (WCHAR *)L"_BSCB_Binding_");
		status = self->stop_result;
		IBinding_Release(&self->IBinding_iface);
		return status;
	}
	if (self->async && !self->stopped)
	{
		IBindCtx_RevokeObjectParam(context, (WCHAR *)L"_BSCB_Binding_");
		IBinding_Release(&self->IBinding_iface);
		return MK_S_ASYNCHRONOUS;
	}
	run_until_done(self);
	IBindCtx_RevokeObjectParam(context, (WCHAR *)L"_BSCB_Binding_");
	status = self->stop_result;
	if (SUCCEEDED(status))
	{
		if (self->to_object)
		{
			if (self->result_object)
				status = IUnknown_QueryInterface(self->result_object, iid, result);
		}
		else if (self->stream)
			status = IStream_QueryInterface(&self->stream->IStream_iface, iid, result);
		else
		{
			protocol_stream *stream = stream_new(self->protocol);
			if (stream)
			{
				status = IStream_QueryInterface(&stream->IStream_iface, iid, result);
				IStream_Release(&stream->IStream_iface);
			}
			else
				status = E_OUTOFMEMORY;
		}
	}
	IBinding_Release(&self->IBinding_iface);
	return status;
}
