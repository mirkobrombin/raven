/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "urlmon_private.h"

static WCHAR callback_param[] = L"_BSCB_Holder_";

/* Interface used to spot our own holder when something else finds it in a context. */
const IID IID_IBindStatusCallbackHolder = {0x79eac9cc, 0xbaf9, 0x11ce, {0x8c, 0x82, 0x00, 0xaa, 0x00, 0x4b, 0xa9, 0x0b}};

/* What sits in the bind context instead of the client's callback: it passes everything on, and
 * answers the interfaces a binder looks for whichever way the client serves them. */
typedef struct
{
	IBindStatusCallbackEx IBindStatusCallbackEx_iface;
	IServiceProvider IServiceProvider_iface;
	IHttpNegotiate2 IHttpNegotiate2_iface;
	IAuthenticate IAuthenticate_iface;
	IInternetBindInfo IInternetBindInfo_iface;
	LONG references;
	IBindStatusCallback *callback;
	IServiceProvider *services;
} holder;

static inline holder *from_callback(IBindStatusCallbackEx *iface)
{
	return CONTAINING_RECORD(iface, holder, IBindStatusCallbackEx_iface);
}

static inline holder *from_services(IServiceProvider *iface)
{
	return CONTAINING_RECORD(iface, holder, IServiceProvider_iface);
}

static inline holder *from_negotiate(IHttpNegotiate2 *iface)
{
	return CONTAINING_RECORD(iface, holder, IHttpNegotiate2_iface);
}

static inline holder *from_authenticate(IAuthenticate *iface)
{
	return CONTAINING_RECORD(iface, holder, IAuthenticate_iface);
}

static inline holder *from_holder_info(IInternetBindInfo *iface)
{
	return CONTAINING_RECORD(iface, holder, IInternetBindInfo_iface);
}

/* The client's answer for an interface: its service provider first, then the callback itself. */
static HRESULT client_service(holder *self, REFIID iid, void **object)
{
	*object = NULL;
	if (self->services && SUCCEEDED(IServiceProvider_QueryService(self->services, iid, iid, object)))
		return S_OK;
	return IBindStatusCallback_QueryInterface(self->callback, iid, object);
}

static HRESULT WINAPI callback_query_interface(IBindStatusCallbackEx *iface, REFIID iid, void **object)
{
	holder *self = from_callback(iface);
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IBindStatusCallback) || IsEqualGUID(iid, &IID_IBindStatusCallbackEx) || IsEqualGUID(iid, &IID_IBindStatusCallbackHolder))
		*object = &self->IBindStatusCallbackEx_iface;
	else if (IsEqualGUID(iid, &IID_IServiceProvider))
		*object = &self->IServiceProvider_iface;
	else if (IsEqualGUID(iid, &IID_IHttpNegotiate) || IsEqualGUID(iid, &IID_IHttpNegotiate2))
		*object = &self->IHttpNegotiate2_iface;
	else if (IsEqualGUID(iid, &IID_IAuthenticate))
		*object = &self->IAuthenticate_iface;
	else if (IsEqualGUID(iid, &IID_IInternetBindInfo))
		*object = &self->IInternetBindInfo_iface;
	else
		return IBindStatusCallback_QueryInterface(self->callback, iid, object);
	IUnknown_AddRef((IUnknown *)*object);
	return S_OK;
}

static ULONG WINAPI callback_add_ref(IBindStatusCallbackEx *iface)
{
	return InterlockedIncrement(&from_callback(iface)->references);
}

static ULONG WINAPI callback_release(IBindStatusCallbackEx *iface)
{
	holder *self = from_callback(iface);
	ULONG left = InterlockedDecrement(&self->references);
	if (!left)
	{
		if (self->services)
			IServiceProvider_Release(self->services);
		IBindStatusCallback_Release(self->callback);
		uri_free(self);
		urlmon_unlock_module();
	}
	return left;
}

static HRESULT WINAPI callback_on_start(IBindStatusCallbackEx *iface, DWORD reserved, IBinding *binding)
{
	return IBindStatusCallback_OnStartBinding(from_callback(iface)->callback, 0xff, binding);
}

static HRESULT WINAPI callback_get_priority(IBindStatusCallbackEx *iface, LONG *priority)
{
	if (priority)
		*priority = THREAD_PRIORITY_NORMAL;
	return S_OK;
}

static HRESULT WINAPI callback_on_low_resource(IBindStatusCallbackEx *iface, DWORD reserved)
{
	return S_OK;
}

static HRESULT WINAPI callback_on_progress(IBindStatusCallbackEx *iface, ULONG value, ULONG limit, ULONG code, LPCWSTR text)
{
	return IBindStatusCallback_OnProgress(from_callback(iface)->callback, value, limit, code, text);
}

static HRESULT WINAPI callback_on_stop(IBindStatusCallbackEx *iface, HRESULT status, LPCWSTR text)
{
	return IBindStatusCallback_OnStopBinding(from_callback(iface)->callback, status, text);
}

static HRESULT WINAPI callback_get_bind_info(IBindStatusCallbackEx *iface, DWORD *flags, BINDINFO *info)
{
	IBindStatusCallbackEx *modern;
	holder *self = from_callback(iface);
	HRESULT result;
	if (SUCCEEDED(IBindStatusCallback_QueryInterface(self->callback, &IID_IBindStatusCallbackEx, (void **)&modern)))
	{
		DWORD ex = 0, reserved = 0;
		result = IBindStatusCallbackEx_GetBindInfoEx(modern, flags, info, &ex, &reserved);
		IBindStatusCallbackEx_Release(modern);
		return result;
	}
	return IBindStatusCallback_GetBindInfo(self->callback, flags, info);
}

static HRESULT WINAPI callback_on_data(IBindStatusCallbackEx *iface, DWORD flags, DWORD size, FORMATETC *format, STGMEDIUM *medium)
{
	return IBindStatusCallback_OnDataAvailable(from_callback(iface)->callback, flags, size, format, medium);
}

static HRESULT WINAPI callback_on_object(IBindStatusCallbackEx *iface, REFIID iid, IUnknown *object)
{
	return IBindStatusCallback_OnObjectAvailable(from_callback(iface)->callback, iid, object);
}

static HRESULT WINAPI callback_get_bind_info_ex(IBindStatusCallbackEx *iface, DWORD *flags, BINDINFO *info, DWORD *ex, DWORD *reserved)
{
	holder *self = from_callback(iface);
	IBindStatusCallbackEx *modern;
	HRESULT result;
	if (SUCCEEDED(IBindStatusCallback_QueryInterface(self->callback, &IID_IBindStatusCallbackEx, (void **)&modern)))
	{
		result = IBindStatusCallbackEx_GetBindInfoEx(modern, flags, info, ex, reserved);
		IBindStatusCallbackEx_Release(modern);
		return result;
	}
	return IBindStatusCallback_GetBindInfo(self->callback, flags, info);
}

static const IBindStatusCallbackExVtbl callback_vtable = {
	callback_query_interface, callback_add_ref, callback_release, callback_on_start, callback_get_priority, callback_on_low_resource, callback_on_progress,
	callback_on_stop, callback_get_bind_info, callback_on_data, callback_on_object, callback_get_bind_info_ex,
};

static HRESULT WINAPI services_query_interface(IServiceProvider *iface, REFIID iid, void **object)
{
	return callback_query_interface(&from_services(iface)->IBindStatusCallbackEx_iface, iid, object);
}

static ULONG WINAPI services_add_ref(IServiceProvider *iface)
{
	return callback_add_ref(&from_services(iface)->IBindStatusCallbackEx_iface);
}

static ULONG WINAPI services_release(IServiceProvider *iface)
{
	return callback_release(&from_services(iface)->IBindStatusCallbackEx_iface);
}

static HRESULT WINAPI services_query(IServiceProvider *iface, REFGUID service, REFIID iid, void **object)
{
	holder *self = from_services(iface);
	*object = NULL;
	if (IsEqualGUID(service, &IID_IHttpNegotiate) || IsEqualGUID(service, &IID_IHttpNegotiate2))
		return callback_query_interface(&self->IBindStatusCallbackEx_iface, iid, object);
	if (IsEqualGUID(service, &IID_IAuthenticate))
		return callback_query_interface(&self->IBindStatusCallbackEx_iface, iid, object);
	if (self->services && SUCCEEDED(IServiceProvider_QueryService(self->services, service, iid, object)))
		return S_OK;
	return IBindStatusCallback_QueryInterface(self->callback, iid, object);
}

static const IServiceProviderVtbl services_vtable = {services_query_interface, services_add_ref, services_release, services_query};

static HRESULT WINAPI negotiate_query_interface(IHttpNegotiate2 *iface, REFIID iid, void **object)
{
	return callback_query_interface(&from_negotiate(iface)->IBindStatusCallbackEx_iface, iid, object);
}

static ULONG WINAPI negotiate_add_ref(IHttpNegotiate2 *iface)
{
	return callback_add_ref(&from_negotiate(iface)->IBindStatusCallbackEx_iface);
}

static ULONG WINAPI negotiate_release(IHttpNegotiate2 *iface)
{
	return callback_release(&from_negotiate(iface)->IBindStatusCallbackEx_iface);
}

static HRESULT WINAPI negotiate_begin(IHttpNegotiate2 *iface, LPCWSTR url, LPCWSTR headers, DWORD reserved, LPWSTR *added)
{
	IHttpNegotiate *client;
	HRESULT result = S_OK;
	if (added)
		*added = NULL;
	if (SUCCEEDED(client_service(from_negotiate(iface), &IID_IHttpNegotiate, (void **)&client)))
	{
		result = IHttpNegotiate_BeginningTransaction(client, url, headers, reserved, added);
		IHttpNegotiate_Release(client);
	}
	return result;
}

static HRESULT WINAPI negotiate_response(IHttpNegotiate2 *iface, DWORD code, LPCWSTR response, LPCWSTR request, LPWSTR *added)
{
	IHttpNegotiate *client;
	HRESULT result = S_OK;
	if (added)
		*added = NULL;
	if (SUCCEEDED(client_service(from_negotiate(iface), &IID_IHttpNegotiate, (void **)&client)))
	{
		result = IHttpNegotiate_OnResponse(client, code, response, request, added);
		IHttpNegotiate_Release(client);
	}
	return result;
}

static HRESULT WINAPI negotiate_root_id(IHttpNegotiate2 *iface, BYTE *id, DWORD *size, DWORD_PTR reserved)
{
	IHttpNegotiate2 *client;
	HRESULT result = E_FAIL;
	if (SUCCEEDED(client_service(from_negotiate(iface), &IID_IHttpNegotiate2, (void **)&client)))
	{
		result = IHttpNegotiate2_GetRootSecurityId(client, id, size, reserved);
		IHttpNegotiate2_Release(client);
	}
	return result;
}

static const IHttpNegotiate2Vtbl negotiate_vtable = {negotiate_query_interface, negotiate_add_ref, negotiate_release, negotiate_begin, negotiate_response, negotiate_root_id};

static HRESULT WINAPI authenticate_query_interface(IAuthenticate *iface, REFIID iid, void **object)
{
	return callback_query_interface(&from_authenticate(iface)->IBindStatusCallbackEx_iface, iid, object);
}

static ULONG WINAPI authenticate_add_ref(IAuthenticate *iface)
{
	return callback_add_ref(&from_authenticate(iface)->IBindStatusCallbackEx_iface);
}

static ULONG WINAPI authenticate_release(IAuthenticate *iface)
{
	return callback_release(&from_authenticate(iface)->IBindStatusCallbackEx_iface);
}

static HRESULT WINAPI authenticate_authenticate(IAuthenticate *iface, HWND *window, LPWSTR *user, LPWSTR *password)
{
	IAuthenticate *client;
	HRESULT result = E_FAIL;
	if (SUCCEEDED(client_service(from_authenticate(iface), &IID_IAuthenticate, (void **)&client)))
	{
		result = IAuthenticate_Authenticate(client, window, user, password);
		IAuthenticate_Release(client);
	}
	return result;
}

static const IAuthenticateVtbl authenticate_vtable = {authenticate_query_interface, authenticate_add_ref, authenticate_release, authenticate_authenticate};

static HRESULT WINAPI holder_info_query_interface(IInternetBindInfo *iface, REFIID iid, void **object)
{
	return callback_query_interface(&from_holder_info(iface)->IBindStatusCallbackEx_iface, iid, object);
}

static ULONG WINAPI holder_info_add_ref(IInternetBindInfo *iface)
{
	return callback_add_ref(&from_holder_info(iface)->IBindStatusCallbackEx_iface);
}

static ULONG WINAPI holder_info_release(IInternetBindInfo *iface)
{
	return callback_release(&from_holder_info(iface)->IBindStatusCallbackEx_iface);
}

static HRESULT WINAPI holder_info_get(IInternetBindInfo *iface, DWORD *flags, BINDINFO *info)
{
	return callback_get_bind_info(&from_holder_info(iface)->IBindStatusCallbackEx_iface, flags, info);
}

/* Strings only come from a client that has its own IInternetBindInfo. */
static HRESULT WINAPI holder_info_get_string(IInternetBindInfo *iface, ULONG type, LPOLESTR *strings, ULONG count, ULONG *fetched)
{
	IInternetBindInfo *client;
	HRESULT result;
	result = IBindStatusCallback_QueryInterface(from_holder_info(iface)->callback, &IID_IInternetBindInfo, (void **)&client);
	if (FAILED(result))
		return result;
	result = IInternetBindInfo_GetBindString(client, type, strings, count, fetched);
	IInternetBindInfo_Release(client);
	return result;
}

static const IInternetBindInfoVtbl holder_info_vtable = {holder_info_query_interface, holder_info_add_ref, holder_info_release, holder_info_get, holder_info_get_string};

static holder *holder_new(IBindStatusCallback *callback)
{
	holder *self = uri_alloc(sizeof(*self));
	if (self)
	{
		self->IBindStatusCallbackEx_iface.lpVtbl = &callback_vtable;
		self->IServiceProvider_iface.lpVtbl = &services_vtable;
		self->IHttpNegotiate2_iface.lpVtbl = &negotiate_vtable;
		self->IAuthenticate_iface.lpVtbl = &authenticate_vtable;
		self->IInternetBindInfo_iface.lpVtbl = &holder_info_vtable;
		self->references = 1;
		IBindStatusCallback_AddRef(callback);
		self->callback = callback;
		if (FAILED(IBindStatusCallback_QueryInterface(callback, &IID_IServiceProvider, (void **)&self->services)))
			self->services = NULL;
		urlmon_lock_module();
	}
	return self;
}

IBindStatusCallback *urlmon_context_callback(IBindCtx *context)
{
	IUnknown *found = NULL;
	IBindStatusCallback *callback = NULL;
	if (FAILED(IBindCtx_GetObjectParam(context, callback_param, &found)))
		return NULL;
	if (FAILED(IUnknown_QueryInterface(found, &IID_IBindStatusCallback, (void **)&callback)))
		callback = NULL;
	IUnknown_Release(found);
	return callback;
}

/* What the client gave, from whatever the context holds: the holder, or something set directly. */
static IBindStatusCallback *current_client(IBindCtx *context)
{
	IBindStatusCallback *found = urlmon_context_callback(context), *inner = NULL;
	if (!found)
		return NULL;
	if (SUCCEEDED(IBindStatusCallback_QueryInterface(found, &IID_IBindStatusCallbackHolder, (void **)&inner)))
	{
		IBindStatusCallback *client = from_callback((IBindStatusCallbackEx *)inner)->callback;
		IBindStatusCallback_AddRef(client);
		IBindStatusCallback_Release(inner);
		IBindStatusCallback_Release(found);
		return client;
	}
	return found;
}

/* A context keeps one holder for its whole life: a new client just takes the old one's place. */
HRESULT WINAPI RegisterBindStatusCallback(IBindCtx *context, IBindStatusCallback *callback, IBindStatusCallback **previous, DWORD reserved)
{
	IBindStatusCallback *found, *inner = NULL, *before;
	holder *wrapped;
	HRESULT result;
	if (!context || !callback)
		return E_INVALIDARG;
	if (previous)
		*previous = NULL;
	found = urlmon_context_callback(context);
	if (found && SUCCEEDED(IBindStatusCallback_QueryInterface(found, &IID_IBindStatusCallbackHolder, (void **)&inner)))
	{
		wrapped = from_callback((IBindStatusCallbackEx *)inner);
		before = wrapped->callback;
		IBindStatusCallback_AddRef(callback);
		wrapped->callback = callback;
		if (wrapped->services)
			IServiceProvider_Release(wrapped->services);
		if (FAILED(IBindStatusCallback_QueryInterface(callback, &IID_IServiceProvider, (void **)&wrapped->services)))
			wrapped->services = NULL;
		if (previous)
			*previous = before;
		else
			IBindStatusCallback_Release(before);
		IBindStatusCallback_Release(inner);
		IBindStatusCallback_Release(found);
		return S_OK;
	}
	wrapped = holder_new(callback);
	if (!wrapped)
	{
		if (found)
			IBindStatusCallback_Release(found);
		return E_OUTOFMEMORY;
	}
	if (found)
	{
		IBindCtx_RevokeObjectParam(context, callback_param);
		if (previous)
			*previous = found;
		else
			IBindStatusCallback_Release(found);
	}
	result = IBindCtx_RegisterObjectParam(context, callback_param, (IUnknown *)&wrapped->IBindStatusCallbackEx_iface);
	IBindStatusCallbackEx_Release(&wrapped->IBindStatusCallbackEx_iface);
	return result;
}

HRESULT WINAPI RevokeBindStatusCallback(IBindCtx *context, IBindStatusCallback *callback)
{
	IBindStatusCallback *current;
	if (!context || !callback)
		return E_INVALIDARG;
	current = current_client(context);
	if (current)
	{
		if (current == callback)
			IBindCtx_RevokeObjectParam(context, callback_param);
		IBindStatusCallback_Release(current);
	}
	return S_OK;
}

/* An ordinary bind context that also says it is an asynchronous one. */
typedef struct
{
	IBindCtx IBindCtx_iface;
	LONG references;
	IBindCtx *inner;
} async_context;

static inline async_context *from_context(IBindCtx *iface)
{
	return CONTAINING_RECORD(iface, async_context, IBindCtx_iface);
}

static HRESULT WINAPI context_query_interface(IBindCtx *iface, REFIID iid, void **object)
{
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IBindCtx) || IsEqualGUID(iid, &IID_IAsyncBindCtx))
	{
		*object = iface;
		IBindCtx_AddRef(iface);
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI context_add_ref(IBindCtx *iface)
{
	return InterlockedIncrement(&from_context(iface)->references);
}

static ULONG WINAPI context_release(IBindCtx *iface)
{
	async_context *self = from_context(iface);
	ULONG left = InterlockedDecrement(&self->references);
	if (!left)
	{
		IBindCtx_Release(self->inner);
		uri_free(self);
		urlmon_unlock_module();
	}
	return left;
}

static HRESULT WINAPI context_register_bound(IBindCtx *iface, IUnknown *object)
{
	return IBindCtx_RegisterObjectBound(from_context(iface)->inner, object);
}

static HRESULT WINAPI context_revoke_bound(IBindCtx *iface, IUnknown *object)
{
	return IBindCtx_RevokeObjectBound(from_context(iface)->inner, object);
}

static HRESULT WINAPI context_release_bound(IBindCtx *iface)
{
	return IBindCtx_ReleaseBoundObjects(from_context(iface)->inner);
}

static HRESULT WINAPI context_set_options(IBindCtx *iface, BIND_OPTS *options)
{
	return IBindCtx_SetBindOptions(from_context(iface)->inner, options);
}

static HRESULT WINAPI context_get_options(IBindCtx *iface, BIND_OPTS *options)
{
	return IBindCtx_GetBindOptions(from_context(iface)->inner, options);
}

static HRESULT WINAPI context_get_rot(IBindCtx *iface, IRunningObjectTable **table)
{
	return IBindCtx_GetRunningObjectTable(from_context(iface)->inner, table);
}

static HRESULT WINAPI context_register_param(IBindCtx *iface, LPOLESTR key, IUnknown *object)
{
	return IBindCtx_RegisterObjectParam(from_context(iface)->inner, key, object);
}

static HRESULT WINAPI context_get_param(IBindCtx *iface, LPOLESTR key, IUnknown **object)
{
	return IBindCtx_GetObjectParam(from_context(iface)->inner, key, object);
}

static HRESULT WINAPI context_enum_params(IBindCtx *iface, IEnumString **out)
{
	return IBindCtx_EnumObjectParam(from_context(iface)->inner, out);
}

static HRESULT WINAPI context_revoke_param(IBindCtx *iface, LPOLESTR key)
{
	return IBindCtx_RevokeObjectParam(from_context(iface)->inner, key);
}

static const IBindCtxVtbl context_vtable = {
	context_query_interface, context_add_ref, context_release, context_register_bound, context_revoke_bound, context_release_bound, context_set_options, context_get_options,
	context_get_rot, context_register_param, context_get_param, context_enum_params, context_revoke_param,
};

static HRESULT prepare_context(IBindCtx *context, IBindStatusCallback *callback, IEnumFORMATETC *formats)
{
	BIND_OPTS options = {sizeof(options)};
	HRESULT result;
	options.grfMode = STGM_READWRITE | STGM_SHARE_EXCLUSIVE;
	options.grfFlags = BIND_MAYBOTHERUSER;
	options.dwTickCountDeadline = 0;
	IBindCtx_SetBindOptions(context, &options);
	if (callback)
	{
		result = RegisterBindStatusCallback(context, callback, NULL, 0);
		if (FAILED(result))
			return result;
	}
	if (formats)
		return RegisterFormatEnumerator(context, formats, 0);
	return S_OK;
}

HRESULT WINAPI CreateAsyncBindCtx(DWORD reserved, IBindStatusCallback *callback, IEnumFORMATETC *formats, IBindCtx **out)
{
	IBindCtx *context;
	HRESULT result;
	if (!callback || !out)
		return E_INVALIDARG;
	*out = NULL;
	result = CreateBindCtx(0, &context);
	if (FAILED(result))
		return result;
	result = prepare_context(context, callback, formats);
	if (FAILED(result))
	{
		IBindCtx_Release(context);
		return result;
	}
	*out = context;
	return S_OK;
}

HRESULT WINAPI CreateAsyncBindCtxEx(IBindCtx *base, DWORD options, IBindStatusCallback *callback, IEnumFORMATETC *formats, IBindCtx **out, DWORD reserved)
{
	async_context *self;
	IBindCtx *context;
	HRESULT result;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (reserved)
		return E_INVALIDARG;
	if (base)
	{
		IBindCtx_AddRef(base);
		context = base;
		result = S_OK;
	}
	else
		result = CreateBindCtx(0, &context);
	if (FAILED(result))
		return result;
	result = prepare_context(context, callback, formats);
	if (FAILED(result))
	{
		IBindCtx_Release(context);
		return result;
	}
	self = uri_alloc(sizeof(*self));
	if (!self)
	{
		IBindCtx_Release(context);
		return E_OUTOFMEMORY;
	}
	self->IBindCtx_iface.lpVtbl = &context_vtable;
	self->references = 1;
	self->inner = context;
	urlmon_lock_module();
	*out = &self->IBindCtx_iface;
	return S_OK;
}

HRESULT WINAPI HlinkSimpleNavigateToString(LPCWSTR target, LPCWSTR location, LPCWSTR frame, IUnknown *host, IBindCtx *context, IBindStatusCallback *callback, DWORD flags, DWORD reserved)
{
	SHELLEXECUTEINFOW info = {sizeof(info)};
	if (!target)
		return E_INVALIDARG;
	info.lpFile = target;
	info.lpParameters = location;
	info.nShow = SW_SHOWNORMAL;
	info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_FLAG_DDEWAIT;
	return urlmon_shell_execute(&info) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

HRESULT WINAPI HlinkNavigateString(IUnknown *host, LPCWSTR target)
{
	return HlinkSimpleNavigateToString(target, NULL, NULL, host, NULL, NULL, 0, 0);
}

HRESULT WINAPI HlinkSimpleNavigateToMoniker(IMoniker *moniker, LPCWSTR location, LPCWSTR frame, IUnknown *host, IBindCtx *context, IBindStatusCallback *callback, DWORD flags, DWORD reserved)
{
	LPOLESTR name = NULL;
	IBindCtx *own = NULL;
	HRESULT result = CreateBindCtx(0, &own);
	if (FAILED(result))
		return result;
	result = IMoniker_GetDisplayName(moniker, own, NULL, &name);
	IBindCtx_Release(own);
	if (FAILED(result))
		return result;
	result = HlinkSimpleNavigateToString(name, location, frame, host, context, callback, flags, reserved);
	CoTaskMemFree(name);
	return result;
}
