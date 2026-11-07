/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "urlmon_private.h"

/* The callback behind the blocking helpers: what the caller's own callback does not supply
 * (bind flags, where the bytes go) happens here. */
typedef enum
{
	SINK_FILE,
	SINK_CACHE,
	SINK_STREAM,
} sink_mode;

typedef struct
{
	IBindStatusCallback IBindStatusCallback_iface;
	IServiceProvider IServiceProvider_iface;
	LONG references;
	IBindStatusCallback *client;
	sink_mode mode;
	HANDLE file;
	WCHAR *temporary;
	WCHAR *target;
	DWORD client_size;
	WCHAR *cache_out;
	HRESULT result;
	IStream *pull;
	BOOL pull_mode;
	BOOL file_opened;
} download_sink;

static inline download_sink *from_callback(IBindStatusCallback *iface)
{
	return CONTAINING_RECORD(iface, download_sink, IBindStatusCallback_iface);
}

static inline download_sink *from_services(IServiceProvider *iface)
{
	return CONTAINING_RECORD(iface, download_sink, IServiceProvider_iface);
}

static HRESULT WINAPI sink_query_interface(IBindStatusCallback *iface, REFIID iid, void **object)
{
	download_sink *self = from_callback(iface);
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IBindStatusCallback))
		*object = &self->IBindStatusCallback_iface;
	else if (IsEqualGUID(iid, &IID_IServiceProvider))
		*object = &self->IServiceProvider_iface;
	else
	{
		*object = NULL;
		return E_NOINTERFACE;
	}
	IUnknown_AddRef((IUnknown *)*object);
	return S_OK;
}

static ULONG WINAPI sink_add_ref(IBindStatusCallback *iface)
{
	return InterlockedIncrement(&from_callback(iface)->references);
}

static ULONG WINAPI sink_release(IBindStatusCallback *iface)
{
	download_sink *self = from_callback(iface);
	ULONG left = InterlockedDecrement(&self->references);
	if (!left)
	{
		if (self->file && self->file != INVALID_HANDLE_VALUE)
			CloseHandle(self->file);
		if (self->client)
			IBindStatusCallback_Release(self->client);
		if (self->pull)
			IStream_Release(self->pull);
		uri_free(self->temporary);
		uri_free(self->target);
		uri_free(self);
		urlmon_unlock_module();
	}
	return left;
}

static HRESULT WINAPI sink_on_start(IBindStatusCallback *iface, DWORD reserved, IBinding *binding)
{
	download_sink *self = from_callback(iface);
	return self->client ? IBindStatusCallback_OnStartBinding(self->client, reserved, binding) : S_OK;
}

static HRESULT WINAPI sink_get_priority(IBindStatusCallback *iface, LONG *priority)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI sink_on_low_resource(IBindStatusCallback *iface, DWORD reserved)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI sink_on_progress(IBindStatusCallback *iface, ULONG value, ULONG limit, ULONG code, LPCWSTR text)
{
	download_sink *self = from_callback(iface);
	HRESULT result = S_OK;
	if (self->client)
		result = IBindStatusCallback_OnProgress(self->client, value, limit, code, text);
	return result == E_ABORT ? E_ABORT : S_OK;
}

static HRESULT WINAPI sink_on_stop(IBindStatusCallback *iface, HRESULT status, LPCWSTR text)
{
	download_sink *self = from_callback(iface);
	self->result = status;
	return self->client ? IBindStatusCallback_OnStopBinding(self->client, status, text) : S_OK;
}

static HRESULT WINAPI sink_get_bind_info(IBindStatusCallback *iface, DWORD *flags, BINDINFO *info)
{
	download_sink *self = from_callback(iface);
	DWORD client_flags = 0;
	HRESULT result = S_OK;
	if (self->client)
	{
		result = IBindStatusCallback_GetBindInfo(self->client, &client_flags, info);
		if (FAILED(result))
			return result;
	}
	else
	{
		DWORD size = info->cbSize;
		memset(info, 0, size);
		info->cbSize = size;
	}
	*flags = client_flags | BINDF_PULLDATA | BINDF_ASYNCHRONOUS * 0;
	return result;
}

/* Copies out whatever the stream has now. */
static HRESULT write_available(download_sink *self, IStream *stream)
{
	BYTE buffer[8192];
	ULONG got, wrote;
	HRESULT result;
	if (!self->file)
	{
		WCHAR directory[MAX_PATH], name[MAX_PATH];
		if (self->mode == SINK_FILE)
		{
			GetTempPathW(ARRAYSIZE(directory), directory);
			GetTempFileNameW(directory, L"url", 0, name);
			self->temporary = uri_text_copy(name);
		}
		else
		{
			GetTempPathW(ARRAYSIZE(directory), directory);
			GetTempFileNameW(directory, L"cac", 0, name);
			self->temporary = uri_text_copy(name);
		}
		if (!self->temporary)
			return E_OUTOFMEMORY;
		self->file = CreateFileW(self->temporary, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (self->file == INVALID_HANDLE_VALUE)
		{
			self->file = NULL;
			return HRESULT_FROM_WIN32(GetLastError());
		}
	}
	for (;;)
	{
		got = 0;
		result = IStream_Read(stream, buffer, sizeof(buffer), &got);
		if (got && !WriteFile(self->file, buffer, got, &wrote, NULL))
			return HRESULT_FROM_WIN32(GetLastError());
		if (result != S_OK)
			return result == E_PENDING || result == S_FALSE ? S_OK : result;
		if (!got)
			return S_OK;
	}
}

static HRESULT WINAPI sink_on_data(IBindStatusCallback *iface, DWORD flags, DWORD size, FORMATETC *format, STGMEDIUM *medium)
{
	download_sink *self = from_callback(iface);
	if (self->mode == SINK_STREAM)
	{
		if (medium->tymed == TYMED_ISTREAM && !self->pull)
		{
			self->pull = medium->pstm;
			IStream_AddRef(self->pull);
		}
		return self->client && self->pull_mode ? IBindStatusCallback_OnDataAvailable(self->client, flags, size, format, medium) : S_OK;
	}
	if (medium->tymed == TYMED_ISTREAM)
		return write_available(self, medium->pstm);
	return S_OK;
}

static HRESULT WINAPI sink_on_object(IBindStatusCallback *iface, REFIID iid, IUnknown *object)
{
	return S_OK;
}

static const IBindStatusCallbackVtbl sink_vtable = {
	sink_query_interface, sink_add_ref, sink_release, sink_on_start, sink_get_priority, sink_on_low_resource, sink_on_progress, sink_on_stop, sink_get_bind_info, sink_on_data, sink_on_object,
};

static HRESULT WINAPI services_query_interface(IServiceProvider *iface, REFIID iid, void **object)
{
	return sink_query_interface(&from_services(iface)->IBindStatusCallback_iface, iid, object);
}

static ULONG WINAPI services_add_ref(IServiceProvider *iface)
{
	return sink_add_ref(&from_services(iface)->IBindStatusCallback_iface);
}

static ULONG WINAPI services_release(IServiceProvider *iface)
{
	return sink_release(&from_services(iface)->IBindStatusCallback_iface);
}

static HRESULT WINAPI services_query(IServiceProvider *iface, REFGUID service, REFIID iid, void **object)
{
	download_sink *self = from_services(iface);
	IServiceProvider *provider = NULL;
	HRESULT result = E_NOINTERFACE;
	*object = NULL;
	if (!self->client)
		return E_NOINTERFACE;
	if (SUCCEEDED(IBindStatusCallback_QueryInterface(self->client, &IID_IServiceProvider, (void **)&provider)))
	{
		result = IServiceProvider_QueryService(provider, service, iid, object);
		IServiceProvider_Release(provider);
	}
	if (FAILED(result))
		result = IBindStatusCallback_QueryInterface(self->client, iid, object);
	return result;
}

static const IServiceProviderVtbl services_vtable = {services_query_interface, services_add_ref, services_release, services_query};

static download_sink *sink_new(IBindStatusCallback *client, sink_mode mode)
{
	download_sink *self = uri_alloc(sizeof(*self));
	if (self)
	{
		self->IBindStatusCallback_iface.lpVtbl = &sink_vtable;
		self->IServiceProvider_iface.lpVtbl = &services_vtable;
		self->references = 1;
		self->mode = mode;
		if (client)
			IBindStatusCallback_AddRef(client);
		self->client = client;
		urlmon_lock_module();
	}
	return self;
}

/* Runs one blocking bind through the sink, then returns what the caller will read, if anything. */
static HRESULT run_bind(LPCWSTR url, download_sink *sink, IStream **stream)
{
	IMoniker *moniker = NULL;
	IBindCtx *context = NULL;
	IStream *result_stream = NULL;
	HRESULT result;
	if (stream)
		*stream = NULL;
	result = CreateURLMoniker(NULL, url, &moniker);
	if (SUCCEEDED(result))
		result = CreateBindCtx(0, &context);
	if (SUCCEEDED(result))
		result = RegisterBindStatusCallback(context, &sink->IBindStatusCallback_iface, NULL, 0);
	if (SUCCEEDED(result))
		result = IMoniker_BindToStorage(moniker, context, NULL, &IID_IStream, (void **)&result_stream);
	if (SUCCEEDED(result) && result_stream)
	{
		/* The last bytes can arrive after the last notification. */
		if (sink->mode != SINK_STREAM)
			write_available(sink, result_stream);
		if (stream)
		{
			*stream = result_stream;
			result_stream = NULL;
		}
	}
	if (result_stream)
		IStream_Release(result_stream);
	if (context)
		IBindCtx_Release(context);
	if (moniker)
		IMoniker_Release(moniker);
	return result;
}

HRESULT WINAPI URLDownloadToFileW(LPUNKNOWN caller, LPCWSTR url, LPCWSTR target, DWORD reserved, LPBINDSTATUSCALLBACK callback)
{
	download_sink *sink;
	HRESULT result;
	if (!url || !target)
		return E_INVALIDARG;
	sink = sink_new(callback, SINK_FILE);
	if (!sink)
		return E_OUTOFMEMORY;
	result = run_bind(url, sink, NULL);
	if (sink->file)
	{
		CloseHandle(sink->file);
		sink->file = NULL;
	}
	if (SUCCEEDED(result) && sink->temporary)
	{
		if (!MoveFileExW(sink->temporary, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
			result = HRESULT_FROM_WIN32(GetLastError());
	}
	else if (sink->temporary)
		DeleteFileW(sink->temporary);
	IBindStatusCallback_Release(&sink->IBindStatusCallback_iface);
	return result;
}

static WCHAR *wide_of(const char *text)
{
	int length;
	WCHAR *wide;
	if (!text)
		return NULL;
	length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	wide = uri_alloc(length * sizeof(WCHAR));
	if (wide)
		MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length);
	return wide;
}

HRESULT WINAPI URLDownloadToFileA(LPUNKNOWN caller, LPCSTR url, LPCSTR target, DWORD reserved, LPBINDSTATUSCALLBACK callback)
{
	WCHAR *wide_url = wide_of(url), *wide_target = wide_of(target);
	HRESULT result = URLDownloadToFileW(caller, wide_url, wide_target, reserved, callback);
	uri_free(wide_url);
	uri_free(wide_target);
	return result;
}

HRESULT WINAPI URLDownloadToCacheFileW(LPUNKNOWN caller, LPCWSTR url, LPWSTR out, DWORD size, DWORD reserved, LPBINDSTATUSCALLBACK callback)
{
	download_sink *sink;
	HRESULT result;
	if (!url || !out)
		return E_INVALIDARG;
	sink = sink_new(callback, SINK_CACHE);
	if (!sink)
		return E_OUTOFMEMORY;
	result = run_bind(url, sink, NULL);
	if (sink->file)
	{
		CloseHandle(sink->file);
		sink->file = NULL;
	}
	if (SUCCEEDED(result))
	{
		if (!sink->temporary)
			result = E_FAIL;
		else if (wcslen(sink->temporary) + 1 > size)
			result = E_OUTOFMEMORY;
		else
			lstrcpyW(out, sink->temporary);
	}
	else if (sink->temporary)
		DeleteFileW(sink->temporary);
	IBindStatusCallback_Release(&sink->IBindStatusCallback_iface);
	return result;
}

HRESULT WINAPI URLDownloadToCacheFileA(LPUNKNOWN caller, LPCSTR url, LPSTR out, DWORD size, DWORD reserved, LPBINDSTATUSCALLBACK callback)
{
	WCHAR *wide_url = wide_of(url), *wide_out;
	HRESULT result;
	wide_out = uri_alloc(size * sizeof(WCHAR));
	if (!wide_out)
	{
		uri_free(wide_url);
		return E_OUTOFMEMORY;
	}
	result = URLDownloadToCacheFileW(caller, wide_url, wide_out, size, reserved, callback);
	if (SUCCEEDED(result))
		WideCharToMultiByte(CP_ACP, 0, wide_out, -1, out, size, NULL, NULL);
	uri_free(wide_url);
	uri_free(wide_out);
	return result;
}

HRESULT WINAPI URLOpenBlockingStreamW(LPUNKNOWN caller, LPCWSTR url, LPSTREAM *stream, DWORD reserved, LPBINDSTATUSCALLBACK callback)
{
	download_sink *sink;
	HRESULT result;
	if (!url || !stream)
		return E_INVALIDARG;
	sink = sink_new(callback, SINK_STREAM);
	if (!sink)
		return E_OUTOFMEMORY;
	result = run_bind(url, sink, stream);
	IBindStatusCallback_Release(&sink->IBindStatusCallback_iface);
	return result;
}

HRESULT WINAPI URLOpenBlockingStreamA(LPUNKNOWN caller, LPCSTR url, LPSTREAM *stream, DWORD reserved, LPBINDSTATUSCALLBACK callback)
{
	WCHAR *wide_url = wide_of(url);
	HRESULT result = URLOpenBlockingStreamW(caller, wide_url, stream, reserved, callback);
	uri_free(wide_url);
	return result;
}

HRESULT WINAPI URLOpenStreamW(LPUNKNOWN caller, LPCWSTR url, DWORD reserved, LPBINDSTATUSCALLBACK callback)
{
	IStream *stream = NULL;
	download_sink *sink;
	HRESULT result;
	if (!url)
		return E_INVALIDARG;
	sink = sink_new(callback, SINK_STREAM);
	if (!sink)
		return E_OUTOFMEMORY;
	sink->pull_mode = TRUE;
	result = run_bind(url, sink, &stream);
	if (stream)
		IStream_Release(stream);
	IBindStatusCallback_Release(&sink->IBindStatusCallback_iface);
	return result;
}

HRESULT WINAPI URLOpenStreamA(LPUNKNOWN caller, LPCSTR url, DWORD reserved, LPBINDSTATUSCALLBACK callback)
{
	WCHAR *wide_url = wide_of(url);
	HRESULT result = URLOpenStreamW(caller, wide_url, reserved, callback);
	uri_free(wide_url);
	return result;
}

HRESULT WINAPI URLOpenPullStreamW(LPUNKNOWN caller, LPCWSTR url, DWORD reserved, LPBINDSTATUSCALLBACK callback)
{
	return URLOpenStreamW(caller, url, reserved, callback);
}
