/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "urlmon_private.h"

enum
{
	PART_FRAGMENT,
	PART_HOST,
	PART_PASSWORD,
	PART_PATH,
	PART_QUERY,
	PART_SCHEME,
	PART_USER,
	PART_COUNT
};

typedef struct
{
	IUriBuilder IUriBuilder_iface;
	LONG references;
	IUri *base;
	DWORD base_flags;
	WCHAR *parts[PART_COUNT];
	BOOL changed[PART_COUNT];
	BOOL port_changed, has_port;
	DWORD port;
	BOOL modified;
} uri_builder;

static inline uri_builder *from_builder(IUriBuilder *iface)
{
	return CONTAINING_RECORD(iface, uri_builder, IUriBuilder_iface);
}

static HRESULT WINAPI builder_query_interface(IUriBuilder *iface, REFIID iid, void **object)
{
	if (!object)
		return E_POINTER;
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IUriBuilder))
	{
		*object = iface;
		IUriBuilder_AddRef(iface);
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI builder_add_ref(IUriBuilder *iface)
{
	return InterlockedIncrement(&from_builder(iface)->references);
}

static void forget_parts(uri_builder *self)
{
	int index;
	for (index = 0; index < PART_COUNT; index++)
	{
		uri_free(self->parts[index]);
		self->parts[index] = NULL;
		self->changed[index] = FALSE;
	}
	self->port_changed = FALSE;
	self->has_port = FALSE;
	self->port = 0;
	self->modified = FALSE;
}

static ULONG WINAPI builder_release(IUriBuilder *iface)
{
	uri_builder *self = from_builder(iface);
	ULONG left = InterlockedDecrement(&self->references);
	if (!left)
	{
		forget_parts(self);
		if (self->base)
			IUri_Release(self->base);
		uri_free(self);
		urlmon_unlock_module();
	}
	return left;
}

/* A base part as it reads in the URI, or the text set over it. */
static const WCHAR *part_value(uri_builder *self, int part)
{
	const uri_data *data;
	if (self->changed[part])
		return self->parts[part];
	data = urlmon_uri_data(self->base);
	if (!data)
		return NULL;
	switch (part)
	{
	case PART_FRAGMENT:
		return data->fragment;
	case PART_HOST:
		return data->host && data->host[0] ? data->host : NULL;
	case PART_PASSWORD:
		return data->password;
	case PART_PATH:
		return data->path && data->path[0] ? data->path : NULL;
	case PART_QUERY:
		return data->query;
	case PART_SCHEME:
		return data->scheme;
	case PART_USER:
		return data->user && data->user[0] ? data->user : NULL;
	}
	return NULL;
}

static HRESULT get_part(uri_builder *self, int part, DWORD *length, LPCWSTR *value)
{
	const WCHAR *text;
	if (!length)
	{
		if (value)
			*value = NULL;
		return E_POINTER;
	}
	if (!value)
	{
		*length = 0;
		return E_POINTER;
	}
	text = part_value(self, part);
	*value = text;
	*length = text ? (DWORD)wcslen(text) : 0;
	return text ? S_OK : S_FALSE;
}

/* Sets a part; when it takes a mark ('#' or '?') the mark is added as needed. */
static HRESULT set_part(uri_builder *self, int part, const WCHAR *value, WCHAR mark)
{
	WCHAR *copy = NULL;
	if (value && (value[0] || mark || part == PART_HOST))
	{
		BOOL add = mark && value[0] != mark;
		copy = uri_alloc((wcslen(value) + 2) * sizeof(WCHAR));
		if (!copy)
			return E_OUTOFMEMORY;
		if (add)
			copy[0] = mark;
		lstrcatW(copy, value);
	}
	uri_free(self->parts[part]);
	self->parts[part] = copy;
	self->changed[part] = TRUE;
	self->modified = TRUE;
	return S_OK;
}

static void add_text(WCHAR **out, const WCHAR *text)
{
	if (text)
	{
		lstrcpyW(*out, text);
		*out += wcslen(text);
	}
}

/* The whole text the parts spell. */
static HRESULT compose_text(uri_builder *self, WCHAR **text, BOOL *has_scheme)
{
	const WCHAR *scheme = part_value(self, PART_SCHEME), *host = part_value(self, PART_HOST), *user = part_value(self, PART_USER),
		*password = part_value(self, PART_PASSWORD), *path = part_value(self, PART_PATH), *query = part_value(self, PART_QUERY),
		*fragment = part_value(self, PART_FRAGMENT);
	const uri_data *data = urlmon_uri_data(self->base);
	DWORD port = 0;
	BOOL show_port = FALSE, wrap_host;
	WCHAR number[16], *out;
	SIZE_T size;
	if (self->port_changed)
	{
		show_port = self->has_port;
		port = self->port;
	}
	else if (data && data->port_text)
	{
		show_port = TRUE;
		port = data->port;
	}
	/* One colon can be a name the caller insists on; an IPv6 address has more. */
	wrap_host = host && host[0] != L'[' && wcschr(host, L':') && wcschr(wcschr(host, L':') + 1, L':');
	size = 32;
	if (scheme)
		size += wcslen(scheme);
	if (host)
		size += wcslen(host);
	if (user)
		size += wcslen(user);
	if (password)
		size += wcslen(password);
	if (path)
		size += wcslen(path);
	if (query)
		size += wcslen(query);
	if (fragment)
		size += wcslen(fragment);
	*text = uri_alloc(size * sizeof(WCHAR));
	if (!*text)
		return E_OUTOFMEMORY;
	*has_scheme = scheme != NULL;
	out = *text;
	if (scheme)
	{
		add_text(&out, scheme);
		add_text(&out, L":");
	}
	if (host || user || password)
	{
		BOOL slashes = scheme && (!(data && data->host_after_scheme) || host == NULL || self->changed[PART_SCHEME]);
		if (slashes)
			add_text(&out, L"//");
		if (user || password)
		{
			add_text(&out, user);
			if (password)
			{
				add_text(&out, L":");
				add_text(&out, password);
			}
			add_text(&out, L"@");
		}
		if (wrap_host)
			add_text(&out, L"[");
		add_text(&out, host);
		if (wrap_host)
			add_text(&out, L"]");
	}
	else if (scheme && data && data->has_authority && !self->changed[PART_HOST])
		add_text(&out, L"//");
	if (show_port)
	{
		DWORD left = port;
		int used = 0, at = 0;
		WCHAR reversed[12];
		do
		{
			reversed[used++] = (WCHAR)(L'0' + left % 10);
			left /= 10;
		}
		while (left);
		number[at++] = L':';
		while (used)
			number[at++] = reversed[--used];
		number[at] = 0;
		add_text(&out, number);
	}
	if (path)
	{
		if (host && path[0] != L'/')
			add_text(&out, L"/");
		add_text(&out, path);
	}
	add_text(&out, query);
	add_text(&out, fragment);
	return S_OK;
}

/* CreateUriSimple comes through the same door but settles its raw text like the others. */
#define BUILD_SIMPLE 0x40000000u

/* A part may not hold what would end it once written into the text. */
static BOOL part_fits(const WCHAR *text, const WCHAR *stops)
{
	for (; text && *text; text++)
		if (wcschr(stops, *text))
			return FALSE;
	return TRUE;
}

static HRESULT build(uri_builder *self, DWORD flags, DWORD builder_flags, DWORD allow_encoding, IUri **uri)
{
	const WCHAR *host, *user, *password;
	BOOL has_scheme = FALSE;
	WCHAR *text;
	HRESULT result;
	BOOL keep_raw = (flags == (DWORD)-1 && !(builder_flags & BUILD_SIMPLE)) || (builder_flags & UriBuilder_USE_ORIGINAL_FLAGS);
	if (!uri)
		return E_POINTER;
	*uri = NULL;
	if (!self->modified && self->base && !(builder_flags & ~BUILD_SIMPLE) && (flags == (DWORD)-1 || uri_effective_flags(flags) == uri_effective_flags(self->base_flags)))
	{
		IUri_AddRef(self->base);
		*uri = self->base;
		return S_OK;
	}
	if (allow_encoding && (self->modified || !self->base))
		return E_NOTIMPL;
	if ((builder_flags & UriBuilder_USE_ORIGINAL_FLAGS) && self->base)
		flags = self->base_flags;
	if (!self->base && !self->modified)
		return INET_E_INVALID_URL;
	host = part_value(self, PART_HOST);
	user = part_value(self, PART_USER);
	password = part_value(self, PART_PASSWORD);
	if (!part_value(self, PART_SCHEME) && !self->base && !host && !part_value(self, PART_PATH) && !part_value(self, PART_QUERY) && !part_value(self, PART_FRAGMENT))
		return INET_E_INVALID_URL;
	if (!host && (user || password) && part_value(self, PART_SCHEME))
		return INET_E_INVALID_URL;
	if (flags != (DWORD)-1 && !uri_flags_valid(flags))
		return E_INVALIDARG;
	if (!part_fits(user, L":@/?#") || !part_fits(password, L"@/?#") || !part_fits(host, L"/?#") ||
		!part_fits(part_value(self, PART_PATH), L"?#") || !part_fits(part_value(self, PART_QUERY), L"#"))
		return INET_E_INVALID_URL;
	if (!part_value(self, PART_SCHEME) && !(flags != (DWORD)-1 && (flags & (Uri_CREATE_ALLOW_RELATIVE | Uri_CREATE_ALLOW_IMPLICIT_WILDCARD_SCHEME | Uri_CREATE_ALLOW_IMPLICIT_FILE_SCHEME))) && !(flags == (DWORD)-1 && (self->base_flags & Uri_CREATE_ALLOW_RELATIVE)))
		return INET_E_INVALID_URL;
	if (flags == (DWORD)-1)
		flags = self->base_flags;
	if ((user || password) && part_value(self, PART_SCHEME) && !lstrcmpiW(part_value(self, PART_SCHEME), L"file"))
		return INET_E_INVALID_URL;
	result = compose_text(self, &text, &has_scheme);
	if (FAILED(result))
		return result;
	result = urlmon_create_uri_combined(text, flags, uri);
	uri_free(text);
	if (result == E_INVALIDARG)
		result = INET_E_INVALID_URL;
	if (SUCCEEDED(result) && !keep_raw)
		urlmon_uri_settle_raw(*uri);
	if (SUCCEEDED(result) && !has_scheme && (user || password || host))
		result = urlmon_uri_set_authority(*uri, user, password, host, part_value(self, PART_PATH));
	return result;
}

static HRESULT WINAPI builder_create_uri_simple(IUriBuilder *iface, DWORD allow_encoding, DWORD_PTR reserved, IUri **uri)
{
	return build(from_builder(iface), (DWORD)-1, BUILD_SIMPLE, allow_encoding, uri);
}

static HRESULT WINAPI builder_create_uri(IUriBuilder *iface, DWORD flags, DWORD allow_encoding, DWORD_PTR reserved, IUri **uri)
{
	return build(from_builder(iface), flags, 0, allow_encoding, uri);
}

static HRESULT WINAPI builder_create_uri_with_flags(IUriBuilder *iface, DWORD flags, DWORD builder_flags, DWORD allow_encoding, DWORD_PTR reserved, IUri **uri)
{
	return build(from_builder(iface), flags, builder_flags, allow_encoding, uri);
}

static HRESULT WINAPI builder_get_iuri(IUriBuilder *iface, IUri **uri)
{
	uri_builder *self = from_builder(iface);
	if (!uri)
		return E_POINTER;
	*uri = self->base;
	if (self->base)
		IUri_AddRef(self->base);
	return S_OK;
}

static HRESULT WINAPI builder_set_iuri(IUriBuilder *iface, IUri *uri)
{
	uri_builder *self = from_builder(iface);
	const uri_data *data = NULL;
	/* The same base keeps what was already changed over it. */
	if (uri == self->base)
		return S_OK;
	if (uri)
	{
		data = urlmon_uri_data(uri);
		if (!data)
			return E_NOTIMPL;
		IUri_AddRef(uri);
	}
	if (self->base)
		IUri_Release(self->base);
	self->base = uri;
	self->base_flags = data ? data->flags : 0;
	forget_parts(self);
	return S_OK;
}

#define PART_ACCESSORS(name, part, mark) \
	static HRESULT WINAPI builder_get_##name(IUriBuilder *iface, DWORD *length, LPCWSTR *value) \
	{ \
		return get_part(from_builder(iface), part, length, value); \
	} \
	static HRESULT WINAPI builder_set_##name(IUriBuilder *iface, LPCWSTR value) \
	{ \
		return set_part(from_builder(iface), part, value, mark); \
	}

PART_ACCESSORS(fragment, PART_FRAGMENT, L'#')
PART_ACCESSORS(password, PART_PASSWORD, 0)
PART_ACCESSORS(path, PART_PATH, 0)
PART_ACCESSORS(query, PART_QUERY, L'?')
PART_ACCESSORS(user_name, PART_USER, 0)

static HRESULT WINAPI builder_get_scheme_name(IUriBuilder *iface, DWORD *length, LPCWSTR *value)
{
	return get_part(from_builder(iface), PART_SCHEME, length, value);
}

static HRESULT WINAPI builder_set_scheme_name(IUriBuilder *iface, LPCWSTR value)
{
	if (!value || !value[0])
		return E_INVALIDARG;
	return set_part(from_builder(iface), PART_SCHEME, value, 0);
}

static HRESULT WINAPI builder_get_host(IUriBuilder *iface, DWORD *length, LPCWSTR *value)
{
	return get_part(from_builder(iface), PART_HOST, length, value);
}

/* A host can't be taken away with a set. */
static HRESULT WINAPI builder_set_host(IUriBuilder *iface, LPCWSTR value)
{
	if (!value)
		return E_INVALIDARG;
	return set_part(from_builder(iface), PART_HOST, value, 0);
}

static HRESULT WINAPI builder_get_port(IUriBuilder *iface, BOOL *has, DWORD *port)
{
	uri_builder *self = from_builder(iface);
	const uri_data *data;
	if (!has)
	{
		if (port)
			*port = 0;
		return E_POINTER;
	}
	if (!port)
	{
		*has = FALSE;
		return E_POINTER;
	}
	/* Only a port set here counts as had; the base's still reads back. */
	if (self->port_changed)
	{
		*has = self->has_port;
		*port = self->port;
		return S_OK;
	}
	data = urlmon_uri_data(self->base);
	*has = FALSE;
	*port = data && data->has_port ? data->port : 0;
	return S_OK;
}

static HRESULT WINAPI builder_set_port(IUriBuilder *iface, BOOL has, DWORD port)
{
	uri_builder *self = from_builder(iface);
	self->port_changed = TRUE;
	self->has_port = has;
	self->port = port;
	self->modified = TRUE;
	return S_OK;
}

static HRESULT WINAPI builder_remove_properties(IUriBuilder *iface, DWORD mask)
{
	static const DWORD refused = Uri_HAS_ABSOLUTE_URI | Uri_HAS_DISPLAY_URI | Uri_HAS_RAW_URI | Uri_HAS_HOST_TYPE | Uri_HAS_SCHEME | Uri_HAS_SCHEME_NAME | Uri_HAS_ZONE;
	static const struct { DWORD mask; int part; } map[] = {
		{Uri_HAS_FRAGMENT, PART_FRAGMENT}, {Uri_HAS_HOST, PART_HOST}, {Uri_HAS_PASSWORD, PART_PASSWORD}, {Uri_HAS_PATH, PART_PATH},
		{Uri_HAS_QUERY, PART_QUERY}, {Uri_HAS_USER_NAME, PART_USER},
	};
	uri_builder *self = from_builder(iface);
	SIZE_T index;
	if (mask & refused)
		return E_INVALIDARG;
	for (index = 0; index < ARRAYSIZE(map); index++)
		if (mask & map[index].mask)
			set_part(self, map[index].part, NULL, 0);
	if (mask & Uri_HAS_PORT)
		builder_set_port(iface, FALSE, 0);
	return S_OK;
}

static HRESULT WINAPI builder_has_been_modified(IUriBuilder *iface, BOOL *modified)
{
	if (!modified)
		return E_POINTER;
	*modified = from_builder(iface)->modified;
	return S_OK;
}

static const IUriBuilderVtbl builder_vtable = {
	builder_query_interface, builder_add_ref, builder_release, builder_create_uri_simple, builder_create_uri, builder_create_uri_with_flags,
	builder_get_iuri, builder_set_iuri, builder_get_fragment, builder_get_host, builder_get_password, builder_get_path, builder_get_port,
	builder_get_query, builder_get_scheme_name, builder_get_user_name, builder_set_fragment, builder_set_host, builder_set_password,
	builder_set_path, builder_set_port, builder_set_query, builder_set_scheme_name, builder_set_user_name, builder_remove_properties,
	builder_has_been_modified,
};

HRESULT WINAPI CreateIUriBuilder(IUri *uri, DWORD flags, DWORD_PTR reserved, IUriBuilder **builder)
{
	uri_builder *self;
	const uri_data *data = NULL;
	if (!builder)
		return E_POINTER;
	if (uri)
	{
		data = urlmon_uri_data(uri);
		if (!data)
		{
			*builder = NULL;
			return E_NOTIMPL;
		}
	}
	self = uri_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->IUriBuilder_iface.lpVtbl = &builder_vtable;
	self->references = 1;
	if (uri)
	{
		IUri_AddRef(uri);
		self->base = uri;
		self->base_flags = data->flags;
	}
	urlmon_lock_module();
	*builder = &self->IUriBuilder_iface;
	return S_OK;
}
