/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "urlmon_private.h"

typedef struct
{
	IMoniker IMoniker_iface;
	IUriContainer IUriContainer_iface;
	LONG references;
	IUri *uri;
	WCHAR *url;
} url_moniker;

static const IMonikerVtbl moniker_vtable;

static inline url_moniker *from_moniker(IMoniker *iface)
{
	return CONTAINING_RECORD(iface, url_moniker, IMoniker_iface);
}

static inline url_moniker *from_container(IUriContainer *iface)
{
	return CONTAINING_RECORD(iface, url_moniker, IUriContainer_iface);
}

/* Null when the moniker is someone else's. */
static url_moniker *this_module_moniker(IMoniker *moniker)
{
	return moniker && moniker->lpVtbl == &moniker_vtable ? from_moniker(moniker) : NULL;
}

static HRESULT WINAPI moniker_query_interface(IMoniker *iface, REFIID iid, void **object)
{
	url_moniker *self = from_moniker(iface);
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IPersist) || IsEqualGUID(iid, &IID_IPersistStream) || IsEqualGUID(iid, &IID_IMoniker))
		*object = &self->IMoniker_iface;
	else if (IsEqualGUID(iid, &IID_IAsyncMoniker))
		*object = &self->IMoniker_iface;
	else if (IsEqualGUID(iid, &IID_IUriContainer))
		*object = &self->IUriContainer_iface;
	else
	{
		*object = NULL;
		return E_NOINTERFACE;
	}
	IUnknown_AddRef((IUnknown *)*object);
	return S_OK;
}

static ULONG WINAPI moniker_add_ref(IMoniker *iface)
{
	return InterlockedIncrement(&from_moniker(iface)->references);
}

static ULONG WINAPI moniker_release(IMoniker *iface)
{
	url_moniker *self = from_moniker(iface);
	ULONG left = InterlockedDecrement(&self->references);
	if (!left)
	{
		if (self->uri)
			IUri_Release(self->uri);
		SysFreeString(self->url);
		uri_free(self);
		urlmon_unlock_module();
	}
	return left;
}

static HRESULT WINAPI moniker_get_class_id(IMoniker *iface, CLSID *clsid)
{
	if (!clsid)
		return E_POINTER;
	*clsid = CLSID_StdURLMoniker;
	return S_OK;
}

static HRESULT WINAPI moniker_is_dirty(IMoniker *iface)
{
	return S_FALSE;
}

/* Persistent form: a byte count that includes the terminator, then the URL. */
static HRESULT WINAPI moniker_load(IMoniker *iface, IStream *stream)
{
	url_moniker *self = from_moniker(iface);
	WCHAR *text;
	ULONG size = 0, got = 0;
	IUri *uri = NULL;
	HRESULT result;
	if (!stream)
		return E_INVALIDARG;
	result = IStream_Read(stream, &size, sizeof(size), &got);
	if (FAILED(result))
		return result;
	if (got != sizeof(size) || size < sizeof(WCHAR))
		return E_FAIL;
	text = uri_alloc(size + sizeof(WCHAR));
	if (!text)
		return E_OUTOFMEMORY;
	result = IStream_Read(stream, text, size, &got);
	if (SUCCEEDED(result) && got != size)
		result = E_FAIL;
	if (SUCCEEDED(result))
		result = CreateUri(text, Uri_CREATE_FILE_USE_DOS_PATH | Uri_CREATE_ALLOW_RELATIVE, 0, &uri);
	if (SUCCEEDED(result))
	{
		if (self->uri)
			IUri_Release(self->uri);
		SysFreeString(self->url);
		self->uri = uri;
		self->url = SysAllocString(text);
	}
	uri_free(text);
	return result;
}

static HRESULT WINAPI moniker_save(IMoniker *iface, IStream *stream, BOOL clear_dirty)
{
	url_moniker *self = from_moniker(iface);
	ULONG size, wrote;
	HRESULT result;
	if (!stream)
		return E_INVALIDARG;
	if (!self->url)
		return E_UNEXPECTED;
	size = SysStringByteLen(self->url) + sizeof(WCHAR);
	result = IStream_Write(stream, &size, sizeof(size), &wrote);
	if (SUCCEEDED(result))
		result = IStream_Write(stream, self->url, size, &wrote);
	return result;
}

static HRESULT WINAPI moniker_get_size_max(IMoniker *iface, ULARGE_INTEGER *size)
{
	url_moniker *self = from_moniker(iface);
	if (!size)
		return E_INVALIDARG;
	size->QuadPart = sizeof(ULONG) + (self->url ? SysStringByteLen(self->url) + sizeof(WCHAR) : sizeof(WCHAR));
	return S_OK;
}

static HRESULT WINAPI moniker_bind_to_object(IMoniker *iface, IBindCtx *context, IMoniker *left, REFIID iid, void **object)
{
	url_moniker *self = from_moniker(iface);
	if (!object)
		return E_INVALIDARG;
	*object = NULL;
	if (!context)
		return E_INVALIDARG;
	if (!self->uri)
		return MK_E_SYNTAX;
	return urlmon_bind(iface, self->uri, context, TRUE, iid, object);
}

static HRESULT WINAPI moniker_bind_to_storage(IMoniker *iface, IBindCtx *context, IMoniker *left, REFIID iid, void **object)
{
	url_moniker *self = from_moniker(iface);
	if (!object)
		return E_INVALIDARG;
	*object = NULL;
	if (!context)
		return E_INVALIDARG;
	if (!self->uri)
		return MK_E_SYNTAX;
	return urlmon_bind(iface, self->uri, context, FALSE, IsEqualGUID(iid, &IID_IUnknown) ? &IID_IStream : iid, object);
}

static HRESULT WINAPI moniker_reduce(IMoniker *iface, IBindCtx *context, DWORD how_far, IMoniker **left, IMoniker **reduced)
{
	if (!reduced)
		return E_INVALIDARG;
	IMoniker_AddRef(iface);
	*reduced = iface;
	return MK_S_REDUCED_TO_SELF;
}

static HRESULT moniker_new(IUri *uri, IMoniker **out);

/* A URL on the right resolves against this one; anything else is the caller's composite to make. */
static HRESULT WINAPI moniker_compose_with(IMoniker *iface, IMoniker *right, BOOL only_if_not_generic, IMoniker **composite)
{
	url_moniker *self = from_moniker(iface);
	IUriContainer *container;
	IUri *relative = NULL, *final = NULL;
	HRESULT result;
	if (!composite)
		return E_INVALIDARG;
	*composite = NULL;
	if (!right)
		return E_INVALIDARG;
	if (FAILED(IMoniker_QueryInterface(right, &IID_IUriContainer, (void **)&container)))
		return MK_E_NEEDGENERIC;
	result = IUriContainer_GetIUri(container, &relative);
	IUriContainer_Release(container);
	if (FAILED(result))
		return result;
	if (!self->uri)
		final = relative;
	else
	{
		result = CoInternetCombineIUri(self->uri, relative, 0, &final, 0);
		IUri_Release(relative);
		if (FAILED(result))
			return result;
	}
	result = moniker_new(final, composite);
	IUri_Release(final);
	return result;
}

static HRESULT WINAPI moniker_enum(IMoniker *iface, BOOL forward, IEnumMoniker **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	return S_OK;
}

static HRESULT WINAPI moniker_is_equal(IMoniker *iface, IMoniker *other)
{
	url_moniker *self = from_moniker(iface);
	IBindCtx *context;
	LPOLESTR text = NULL;
	CLSID clsid;
	HRESULT result;
	if (!other)
		return E_INVALIDARG;
	result = IMoniker_GetClassID(other, &clsid);
	if (FAILED(result))
		return result;
	if (!IsEqualGUID(&clsid, &CLSID_StdURLMoniker))
		return S_FALSE;
	result = CreateBindCtx(0, &context);
	if (FAILED(result))
		return result;
	result = IMoniker_GetDisplayName(other, context, NULL, &text);
	IBindCtx_Release(context);
	if (FAILED(result))
		return result;
	result = !lstrcmpiW(text, self->url) ? S_OK : S_FALSE;
	CoTaskMemFree(text);
	return result;
}

static HRESULT WINAPI moniker_hash(IMoniker *iface, DWORD *hash)
{
	url_moniker *self = from_moniker(iface);
	const WCHAR *at;
	DWORD value = 0;
	if (!hash)
		return E_INVALIDARG;
	for (at = self->url; at && *at; at++)
		value = value * 37 + *at;
	*hash = value;
	return S_OK;
}

static HRESULT WINAPI moniker_is_running(IMoniker *iface, IBindCtx *context, IMoniker *left, IMoniker *newly)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI moniker_get_time(IMoniker *iface, IBindCtx *context, IMoniker *left, FILETIME *time)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI moniker_inverse(IMoniker *iface, IMoniker **out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI moniker_common_prefix(IMoniker *iface, IMoniker *other, IMoniker **prefix)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI moniker_relative_path_to(IMoniker *iface, IMoniker *other, IMoniker **path)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI moniker_get_display_name(IMoniker *iface, IBindCtx *context, IMoniker *left, LPOLESTR *name)
{
	url_moniker *self = from_moniker(iface);
	SIZE_T size;
	if (!name)
		return E_INVALIDARG;
	if (!self->url)
		return E_OUTOFMEMORY;
	size = SysStringByteLen(self->url) + sizeof(WCHAR);
	*name = CoTaskMemAlloc(size);
	if (!*name)
		return E_OUTOFMEMORY;
	memcpy(*name, self->url, size);
	return S_OK;
}

static HRESULT WINAPI moniker_parse_display_name(IMoniker *iface, IBindCtx *context, IMoniker *left, LPOLESTR name, ULONG *eaten, IMoniker **out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI moniker_is_system(IMoniker *iface, DWORD *type)
{
	if (!type)
		return E_INVALIDARG;
	*type = MKSYS_URLMONIKER;
	return S_OK;
}

static const IMonikerVtbl moniker_vtable = {
	moniker_query_interface, moniker_add_ref, moniker_release, moniker_get_class_id, moniker_is_dirty, moniker_load, moniker_save, moniker_get_size_max,
	moniker_bind_to_object, moniker_bind_to_storage, moniker_reduce, moniker_compose_with, moniker_enum, moniker_is_equal, moniker_hash, moniker_is_running,
	moniker_get_time, moniker_inverse, moniker_common_prefix, moniker_relative_path_to, moniker_get_display_name, moniker_parse_display_name, moniker_is_system,
};

static HRESULT WINAPI container_query_interface(IUriContainer *iface, REFIID iid, void **object)
{
	return moniker_query_interface(&from_container(iface)->IMoniker_iface, iid, object);
}

static ULONG WINAPI container_add_ref(IUriContainer *iface)
{
	return moniker_add_ref(&from_container(iface)->IMoniker_iface);
}

static ULONG WINAPI container_release(IUriContainer *iface)
{
	return moniker_release(&from_container(iface)->IMoniker_iface);
}

static HRESULT WINAPI container_get_iuri(IUriContainer *iface, IUri **uri)
{
	url_moniker *self = from_container(iface);
	if (!self->uri)
	{
		*uri = NULL;
		return S_FALSE;
	}
	IUri_AddRef(self->uri);
	*uri = self->uri;
	return S_OK;
}

static const IUriContainerVtbl container_vtable = {container_query_interface, container_add_ref, container_release, container_get_iuri};

static HRESULT moniker_new(IUri *uri, IMoniker **out)
{
	url_moniker *self = uri_alloc(sizeof(*self));
	HRESULT result = S_OK;
	if (!self)
		return E_OUTOFMEMORY;
	self->IMoniker_iface.lpVtbl = &moniker_vtable;
	self->IUriContainer_iface.lpVtbl = &container_vtable;
	self->references = 1;
	urlmon_lock_module();
	if (uri)
	{
		IUri_AddRef(uri);
		self->uri = uri;
		result = IUri_GetDisplayUri(uri, &self->url);
	}
	if (FAILED(result))
	{
		IMoniker_Release(&self->IMoniker_iface);
		return result;
	}
	*out = &self->IMoniker_iface;
	return S_OK;
}

HRESULT urlmon_create_moniker_object(IUnknown *outer, REFIID iid, void **object)
{
	IMoniker *moniker;
	HRESULT result;
	*object = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	result = moniker_new(NULL, &moniker);
	if (FAILED(result))
		return result;
	result = IMoniker_QueryInterface(moniker, iid, object);
	IMoniker_Release(moniker);
	return result;
}

/* The URI a context moniker stands for, for combining with a relative name. */
static HRESULT context_uri(IMoniker *context, IUri **uri)
{
	url_moniker *mine = this_module_moniker(context);
	IBindCtx *binding_context;
	LPOLESTR text = NULL;
	DWORD system = MKSYS_NONE;
	HRESULT result;
	*uri = NULL;
	if (mine)
	{
		if (!mine->uri)
			return E_UNEXPECTED;
		IUri_AddRef(mine->uri);
		*uri = mine->uri;
		return S_OK;
	}
	if (FAILED(IMoniker_IsSystemMoniker(context, &system)) || system != MKSYS_URLMONIKER)
		return S_FALSE;
	result = CreateBindCtx(0, &binding_context);
	if (FAILED(result))
		return result;
	result = IMoniker_GetDisplayName(context, binding_context, NULL, &text);
	IBindCtx_Release(binding_context);
	if (FAILED(result))
		return result;
	result = CreateUri(text, Uri_CREATE_FILE_USE_DOS_PATH, 0, uri);
	CoTaskMemFree(text);
	return result;
}

/* Each kind of moniker composes the same way it parses. */
static DWORD combine_flags_of(DWORD flags)
{
	if (flags == URL_MK_NO_CANONICALIZE)
		return URL_DONT_SIMPLIFY;
	return flags == URL_MK_LEGACY ? URL_FILE_USE_PATHURL : 0;
}

HRESULT WINAPI CreateURLMonikerEx2(IMoniker *context, IUri *uri, IMoniker **out, DWORD flags)
{
	IUri *base = NULL, *final = NULL;
	HRESULT result;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!uri)
		return E_INVALIDARG;
	if (flags != URL_MK_LEGACY && flags != URL_MK_UNIFORM && flags != URL_MK_NO_CANONICALIZE)
		return E_INVALIDARG;
	if (context)
	{
		result = context_uri(context, &base);
		if (FAILED(result))
			return result;
	}
	if (base)
	{
		result = CoInternetCombineIUri(base, uri, combine_flags_of(flags), &final, 0);
		IUri_Release(base);
		if (FAILED(result))
			return result;
	}
	else
	{
		final = uri;
		IUri_AddRef(final);
	}
	result = moniker_new(final, out);
	IUri_Release(final);
	return result;
}

/* Tries the readings a plain string can have, most specific first. */
static HRESULT parse_moniker_url(const WCHAR *url, DWORD kind, BOOL relative_only, IUri **uri)
{
	DWORD base = kind == URL_MK_LEGACY ? Uri_CREATE_FILE_USE_DOS_PATH : kind == URL_MK_NO_CANONICALIZE ? Uri_CREATE_NO_CANONICALIZE : 0;
	DWORD keep = base & ~Uri_CREATE_FILE_USE_DOS_PATH;
	HRESULT result = E_INVALIDARG;
	if (!relative_only)
	{
		result = CreateUri(url, base, 0, uri);
		if (FAILED(result))
			result = CreateUri(url, base | Uri_CREATE_ALLOW_IMPLICIT_FILE_SCHEME, 0, uri);
	}
	if (FAILED(result))
		result = CreateUri(url, keep | Uri_CREATE_ALLOW_RELATIVE, 0, uri);
	return result;
}

HRESULT WINAPI CreateURLMonikerEx(IMoniker *context, LPCWSTR url, IMoniker **out, DWORD flags)
{
	IUri *uri, *base = NULL;
	HRESULT result;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!url)
		return E_INVALIDARG;
	if (flags != URL_MK_LEGACY && flags != URL_MK_UNIFORM && flags != URL_MK_NO_CANONICALIZE)
		return E_INVALIDARG;
	if (context)
	{
		result = context_uri(context, &base);
		if (FAILED(result))
			return result;
	}
	result = parse_moniker_url(url, flags, base != NULL, &uri);
	if (FAILED(result))
	{
		if (base)
			IUri_Release(base);
		return result == E_INVALIDARG ? MK_E_SYNTAX : result;
	}
	if (base)
	{
		IUri *final = NULL;
		result = CoInternetCombineIUri(base, uri, combine_flags_of(flags), &final, 0);
		IUri_Release(uri);
		IUri_Release(base);
		if (FAILED(result))
			return result;
		uri = final;
	}
	result = moniker_new(uri, out);
	IUri_Release(uri);
	return result;
}

HRESULT WINAPI CreateURLMoniker(IMoniker *context, LPCWSTR url, IMoniker **out)
{
	return CreateURLMonikerEx(context, url, out, URL_MK_LEGACY);
}

HRESULT WINAPI MkParseDisplayNameEx(IBindCtx *context, LPCWSTR name, ULONG *eaten, IMoniker **out)
{
	HRESULT result;
	if (!context || !name || !eaten || !out)
		return E_INVALIDARG;
	if (wcschr(name, L':') > name && !(wcschr(name, L':') == name + 1))
	{
		result = CreateURLMoniker(NULL, name, out);
		if (SUCCEEDED(result))
		{
			*eaten = (ULONG)wcslen(name);
			return result;
		}
	}
	/* The class-based forms belong to OLE, if this one has them. */
	HMODULE ole = GetModuleHandleW(L"ole32.dll");
	HRESULT (WINAPI *parse)(IBindCtx *, LPCOLESTR, ULONG *, IMoniker **) = ole ? (void *)GetProcAddress(ole, "MkParseDisplayName") : NULL;
	return parse ? parse(context, name, eaten, out) : MK_E_SYNTAX;
}

HRESULT WINAPI IsAsyncMoniker(IMoniker *moniker)
{
	IUnknown *asynchronous;
	if (!moniker)
		return E_INVALIDARG;
	if (FAILED(IMoniker_QueryInterface(moniker, &IID_IAsyncMoniker, (void **)&asynchronous)))
		return S_FALSE;
	IUnknown_Release(asynchronous);
	return S_OK;
}

HRESULT WINAPI BindAsyncMoniker(IMoniker *moniker, DWORD options, IBindStatusCallback *callback, REFIID iid, void **object)
{
	IBindCtx *context = NULL;
	HRESULT result;
	if (!moniker || !object)
		return E_INVALIDARG;
	*object = NULL;
	result = CreateAsyncBindCtx(0, callback, NULL, &context);
	if (FAILED(result))
		return result;
	result = IMoniker_BindToObject(moniker, context, NULL, iid, object);
	IBindCtx_Release(context);
	return result;
}
