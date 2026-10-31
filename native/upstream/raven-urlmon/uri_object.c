/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "urlmon_private.h"

typedef struct
{
	IUri IUri_iface;
	IUriBuilderFactory IUriBuilderFactory_iface;
	IPersistStream IPersistStream_iface;
	IMarshal IMarshal_iface;
	IUnknown *outer;
	LONG references;
	BOOL ready;
	uri_data data;
} uri_object;

static inline uri_object *from_uri(IUri *iface)
{
	return CONTAINING_RECORD(iface, uri_object, IUri_iface);
}

static inline uri_object *from_factory(IUriBuilderFactory *iface)
{
	return CONTAINING_RECORD(iface, uri_object, IUriBuilderFactory_iface);
}

static inline uri_object *from_persist(IPersistStream *iface)
{
	return CONTAINING_RECORD(iface, uri_object, IPersistStream_iface);
}

static const IUriVtbl uri_vtable;

/* An IUri of this module, whatever wraps it; NULL for a foreign one. */
const uri_data *urlmon_uri_data(IUri *uri)
{
	if (uri && uri->lpVtbl == &uri_vtable && from_uri(uri)->ready)
		return &from_uri(uri)->data;
	return NULL;
}

static HRESULT WINAPI uri_query_interface(IUri *iface, REFIID iid, void **object)
{
	uri_object *self = from_uri(iface);
	if (!object)
		return E_POINTER;
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IUri))
		*object = &self->IUri_iface;
	else if (IsEqualGUID(iid, &IID_IUriBuilderFactory))
		*object = &self->IUriBuilderFactory_iface;
	else if (IsEqualGUID(iid, &IID_IPersistStream))
		*object = &self->IPersistStream_iface;
	else if (IsEqualGUID(iid, &IID_IMarshal))
		*object = &self->IMarshal_iface;
	else
	{
		*object = NULL;
		return E_NOINTERFACE;
	}
	IUnknown_AddRef((IUnknown *)*object);
	return S_OK;
}

static ULONG WINAPI uri_add_ref(IUri *iface)
{
	return InterlockedIncrement(&from_uri(iface)->references);
}

static ULONG WINAPI uri_release(IUri *iface)
{
	uri_object *self = from_uri(iface);
	ULONG left = InterlockedDecrement(&self->references);
	if (!left)
	{
		uri_clear(&self->data);
		uri_free(self);
		urlmon_unlock_module();
	}
	return left;
}

/* A hierarchical scheme needs its slashes to name a complete address. */
static BOOL has_absolute(const uri_data *data)
{
	if (!data->scheme)
		return TRUE;
	switch (data->scheme_type)
	{
	case URL_SCHEME_HTTP:
	case URL_SCHEME_HTTPS:
	case URL_SCHEME_FTP:
	case URL_SCHEME_GOPHER:
	case URL_SCHEME_FILE:
	case URL_SCHEME_NNTP:
	case URL_SCHEME_TELNET:
	case URL_SCHEME_WAIS:
		return data->has_authority;
	default:
		return TRUE;
	}
}

static WCHAR *join_range(const WCHAR *left, const WCHAR *right)
{
	SIZE_T length = wcslen(left);
	WCHAR *joined = uri_alloc((length + wcslen(right) + 1) * sizeof(WCHAR));
	if (joined)
	{
		lstrcpyW(joined, left);
		lstrcatW(joined, right);
	}
	return joined;
}

static const WCHAR *extension_of(const WCHAR *path)
{
	const WCHAR *slash = wcsrchr(path, L'/'), *back = wcsrchr(path, L'\\'), *dot;
	if (back && (!slash || back > slash))
		slash = back;
	dot = wcsrchr(slash ? slash : path, L'.');
	return dot;
}

static WCHAR *user_info_of(const uri_data *data)
{
	if (!data->user)
		return NULL;
	if (data->password)
	{
		WCHAR *left = join_range(data->user, L":"), *both;
		if (!left)
			return NULL;
		both = join_range(left, data->password);
		uri_free(left);
		return both;
	}
	return uri_text_copy(data->user);
}

static BOOL is_default_port(const uri_data *data)
{
	return (data->scheme_type == URL_SCHEME_HTTP && data->port == 80) || (data->scheme_type == URL_SCHEME_HTTPS && data->port == 443) ||
		(data->scheme_type == URL_SCHEME_FTP && data->port == 21);
}

static WCHAR *authority_of(const uri_data *data)
{
	WCHAR *info = user_info_of(data);
	SIZE_T size = (info ? wcslen(info) + 1 : 0) + (data->host ? wcslen(data->host) + 2 : 0) + (data->port_text ? wcslen(data->port_text) + 1 : 0) + 1;
	WCHAR *out = uri_alloc(size * sizeof(WCHAR));
	if (out)
	{
		if (info)
		{
			lstrcatW(out, info);
			lstrcatW(out, L"@");
		}
		if (data->host)
		{
			if (data->host_in_brackets)
				lstrcatW(out, L"[");
			lstrcatW(out, data->host);
			if (data->host_in_brackets)
				lstrcatW(out, L"]");
		}
		if (data->port_text && !(data->combined && is_default_port(data)))
		{
			lstrcatW(out, L":");
			lstrcatW(out, data->port_text);
		}
	}
	uri_free(info);
	return out;
}

/* The text of a property as a new string; S_FALSE when the URI does not have it. */
HRESULT urlmon_uri_property(const uri_data *data, Uri_PROPERTY property, WCHAR **value)
{
	const WCHAR *plain = NULL;
	WCHAR *made = NULL;
	BOOL present = TRUE;
	switch (property)
	{
	case Uri_PROPERTY_ABSOLUTE_URI:
		plain = data->absolute;
		present = has_absolute(data);
		if (!present)
			plain = L"";
		break;
	case Uri_PROPERTY_AUTHORITY:
		if (data->host || data->user)
			made = authority_of(data);
		present = made != NULL;
		break;
	case Uri_PROPERTY_DISPLAY_URI:
		plain = data->display;
		break;
	case Uri_PROPERTY_DOMAIN:
		if (data->host && (data->host_type == Uri_HOST_DNS || data->host_type == Uri_HOST_IDN))
			made = uri_domain_of(data->host);
		present = made != NULL;
		break;
	case Uri_PROPERTY_EXTENSION:
		plain = data->path ? extension_of(data->path) : NULL;
		present = plain != NULL;
		break;
	case Uri_PROPERTY_FRAGMENT:
		plain = data->fragment;
		present = plain != NULL;
		break;
	case Uri_PROPERTY_HOST:
		plain = data->host;
		present = plain != NULL;
		break;
	case Uri_PROPERTY_PASSWORD:
		plain = data->password;
		present = plain != NULL;
		break;
	case Uri_PROPERTY_PATH:
		plain = data->path;
		present = plain && (plain[0] || !data->scheme || data->combined);
		break;
	case Uri_PROPERTY_PATH_AND_QUERY:
		made = join_range(data->path ? data->path : L"", data->query ? data->query : L"");
		present = made && (made[0] || !data->scheme);
		break;
	case Uri_PROPERTY_QUERY:
		plain = data->query;
		present = plain != NULL;
		break;
	case Uri_PROPERTY_RAW_URI:
		plain = data->raw;
		break;
	case Uri_PROPERTY_SCHEME_NAME:
		plain = data->scheme;
		present = plain != NULL;
		break;
	case Uri_PROPERTY_USER_INFO:
		made = user_info_of(data);
		present = made != NULL;
		break;
	case Uri_PROPERTY_USER_NAME:
		plain = data->user;
		present = plain && (plain[0] || !data->password);
		break;
	default:
		return E_INVALIDARG;
	}
	if (made)
		*value = made;
	else
		*value = uri_text_copy(plain ? plain : L"");
	if (!*value)
		return E_OUTOFMEMORY;
	return present ? S_OK : S_FALSE;
}

static BOOL flags_fit(Uri_PROPERTY property, DWORD flags)
{
	if (flags & ~(Uri_DISPLAY_NO_FRAGMENT | Uri_PUNYCODE_IDN_HOST | Uri_DISPLAY_IDN_HOST))
		return FALSE;
	if ((flags & Uri_DISPLAY_NO_FRAGMENT) && property != Uri_PROPERTY_DISPLAY_URI)
		return FALSE;
	if ((flags & Uri_PUNYCODE_IDN_HOST) && property != Uri_PROPERTY_ABSOLUTE_URI && property != Uri_PROPERTY_DOMAIN && property != Uri_PROPERTY_HOST)
		return FALSE;
	if ((flags & Uri_DISPLAY_IDN_HOST) && property != Uri_PROPERTY_ABSOLUTE_URI && property != Uri_PROPERTY_DOMAIN && property != Uri_PROPERTY_HOST)
		return FALSE;
	return TRUE;
}

/* The complete address again with the host swapped for another spelling of it. */
static WCHAR *absolute_with_host(const uri_data *data, const WCHAR *host)
{
	SIZE_T size = wcslen(data->absolute) + wcslen(host) + 16;
	const WCHAR *found = data->host ? wcsstr(data->absolute, data->host) : NULL;
	WCHAR *out;
	if (!found)
		return uri_text_copy(data->absolute);
	out = uri_alloc(size * sizeof(WCHAR));
	if (out)
	{
		memcpy(out, data->absolute, (found - data->absolute) * sizeof(WCHAR));
		/* The short empty user info comes back in this form. */
		if (data->user && !data->user[0] && (!data->password || !data->password[0]))
			lstrcatW(out, data->password ? L":@" : L"@");
		lstrcatW(out, host);
		lstrcatW(out, found + wcslen(data->host));
	}
	return out;
}

static HRESULT WINAPI uri_get_property_bstr(IUri *iface, Uri_PROPERTY property, BSTR *value, DWORD flags)
{
	uri_object *self = from_uri(iface);
	WCHAR *text;
	HRESULT result;
	if (!self->ready)
		return E_UNEXPECTED;
	if (!value)
		return E_POINTER;
	*value = NULL;
	if (property == Uri_PROPERTY_ZONE)
	{
		*value = SysAllocStringLen(NULL, 0);
		return *value ? S_FALSE : E_OUTOFMEMORY;
	}
	if (property > Uri_PROPERTY_STRING_LAST)
		return E_INVALIDARG;
	if (!self->ready)
		return E_UNEXPECTED;
	if (!flags_fit(property, flags))
		return E_INVALIDARG;
	result = urlmon_uri_property(&self->data, property, &text);
	if (FAILED(result))
		return result;
	if ((flags & Uri_DISPLAY_NO_FRAGMENT) && self->data.fragment)
		text[wcslen(text) - wcslen(self->data.fragment)] = 0;
	if ((flags & (Uri_PUNYCODE_IDN_HOST | Uri_DISPLAY_IDN_HOST)) && self->data.host_type == Uri_HOST_IDN && result == S_OK)
	{
		WCHAR *host = flags & Uri_PUNYCODE_IDN_HOST ? uri_host_as_ascii(self->data.host) : uri_host_as_unicode(self->data.host), *changed = NULL;
		if (host)
		{
			if (property == Uri_PROPERTY_ABSOLUTE_URI || property == Uri_PROPERTY_DISPLAY_URI)
				changed = absolute_with_host(&self->data, host);
			else if (property == Uri_PROPERTY_DOMAIN)
				changed = uri_domain_of(host);
			else
				changed = uri_text_copy(host);
		}
		uri_free(host);
		if (changed)
		{
			uri_free(text);
			text = changed;
		}
	}
	*value = SysAllocString(text);
	uri_free(text);
	return *value ? result : E_OUTOFMEMORY;
}

static HRESULT WINAPI uri_get_property_length(IUri *iface, Uri_PROPERTY property, DWORD *length, DWORD flags)
{
	BSTR text;
	HRESULT result;
	if (!from_uri(iface)->ready)
		return E_UNEXPECTED;
	if (!length)
		return E_INVALIDARG;
	if (property > Uri_PROPERTY_STRING_LAST)
		return E_INVALIDARG;
	*length = 0;
	result = uri_get_property_bstr(iface, property, &text, flags);
	if (FAILED(result))
		return result;
	*length = SysStringLen(text);
	SysFreeString(text);
	return result;
}

static HRESULT WINAPI uri_get_property_dword(IUri *iface, Uri_PROPERTY property, DWORD *value, DWORD flags)
{
	uri_object *self = from_uri(iface);
	if (!self->ready)
		return E_UNEXPECTED;
	if (!value)
		return E_INVALIDARG;
	*value = 0;
	if (flags)
		return E_INVALIDARG;
	if (property >= Uri_PROPERTY_HOST_TYPE && !self->ready)
		return E_UNEXPECTED;
	switch (property)
	{
	case Uri_PROPERTY_HOST_TYPE:
		*value = self->data.host_type;
		return S_OK;
	case Uri_PROPERTY_PORT:
		if (!self->data.has_port)
			return S_FALSE;
		*value = self->data.port;
		return S_OK;
	case Uri_PROPERTY_SCHEME:
		*value = self->data.scheme_type;
		return S_OK;
	case Uri_PROPERTY_ZONE:
		*value = URLZONE_INVALID;
		return E_NOTIMPL;
	default:
		return E_INVALIDARG;
	}
}

static HRESULT WINAPI uri_has_property(IUri *iface, Uri_PROPERTY property, BOOL *has)
{
	uri_object *self = from_uri(iface);
	WCHAR *text;
	HRESULT result;
	if (!has)
		return E_INVALIDARG;
	if (!self->ready)
		return E_UNEXPECTED;
	switch (property)
	{
	case Uri_PROPERTY_HOST_TYPE:
		*has = TRUE;
		return S_OK;
	case Uri_PROPERTY_PORT:
		*has = self->data.has_port;
		return S_OK;
	case Uri_PROPERTY_SCHEME:
		*has = TRUE;
		return S_OK;
	case Uri_PROPERTY_ZONE:
		*has = FALSE;
		return S_OK;
	default:
		break;
	}
	*has = FALSE;
	if (property > Uri_PROPERTY_STRING_LAST)
		return E_INVALIDARG;
	result = urlmon_uri_property(&self->data, property, &text);
	if (FAILED(result))
		return result;
	uri_free(text);
	*has = result == S_OK;
	return S_OK;
}

#define STRING_GETTER(name, property) \
	static HRESULT WINAPI uri_get_##name(IUri *iface, BSTR *value) \
	{ \
		return uri_get_property_bstr(iface, property, value, 0); \
	}

STRING_GETTER(absolute, Uri_PROPERTY_ABSOLUTE_URI)
STRING_GETTER(authority, Uri_PROPERTY_AUTHORITY)
STRING_GETTER(display, Uri_PROPERTY_DISPLAY_URI)
STRING_GETTER(domain, Uri_PROPERTY_DOMAIN)
STRING_GETTER(extension, Uri_PROPERTY_EXTENSION)
STRING_GETTER(fragment, Uri_PROPERTY_FRAGMENT)
STRING_GETTER(host, Uri_PROPERTY_HOST)
STRING_GETTER(password, Uri_PROPERTY_PASSWORD)
STRING_GETTER(path, Uri_PROPERTY_PATH)
STRING_GETTER(path_and_query, Uri_PROPERTY_PATH_AND_QUERY)
STRING_GETTER(query, Uri_PROPERTY_QUERY)
STRING_GETTER(raw, Uri_PROPERTY_RAW_URI)
STRING_GETTER(scheme_name, Uri_PROPERTY_SCHEME_NAME)
STRING_GETTER(user_info, Uri_PROPERTY_USER_INFO)
STRING_GETTER(user_name, Uri_PROPERTY_USER_NAME)

#define DWORD_GETTER(name, property) \
	static HRESULT WINAPI uri_get_##name(IUri *iface, DWORD *value) \
	{ \
		return uri_get_property_dword(iface, property, value, 0); \
	}

DWORD_GETTER(host_type, Uri_PROPERTY_HOST_TYPE)
DWORD_GETTER(port, Uri_PROPERTY_PORT)
DWORD_GETTER(scheme, Uri_PROPERTY_SCHEME)
DWORD_GETTER(zone, Uri_PROPERTY_ZONE)

static HRESULT WINAPI uri_get_properties(IUri *iface, DWORD *mask)
{
	Uri_PROPERTY property;
	if (!mask)
		return E_INVALIDARG;
	if (!from_uri(iface)->ready)
		return E_UNEXPECTED;
	*mask = 0;
	for (property = Uri_PROPERTY_STRING_START; property <= Uri_PROPERTY_DWORD_LAST; property++)
	{
		BOOL has = FALSE;
		if (SUCCEEDED(uri_has_property(iface, property, &has)) && has)
			*mask |= 1u << property;
	}
	return S_OK;
}

static HRESULT WINAPI uri_is_equal(IUri *iface, IUri *other, BOOL *equal)
{
	uri_object *self = from_uri(iface);
	BSTR left = NULL, right = NULL;
	HRESULT result;
	if (!self->ready)
		return E_UNEXPECTED;
	if (!equal)
		return E_POINTER;
	*equal = FALSE;
	if (!other)
		return S_OK;
	if (!self->ready)
		return E_UNEXPECTED;
	if (other->lpVtbl != &uri_vtable)
	{
		/* Someone else's IUri can only be asked for its text. */
		uri_data parsed = {0};
		result = IUri_GetRawUri(other, &right);
		if (SUCCEEDED(result))
			result = uri_parse(right, Uri_CREATE_ALLOW_RELATIVE, &parsed);
		if (SUCCEEDED(result))
			*equal = urlmon_uris_match(&self->data, &parsed);
		uri_clear(&parsed);
		SysFreeString(right);
		return result;
	}
	if (!from_uri(other)->ready)
		return E_UNEXPECTED;
	result = uri_get_property_bstr(iface, Uri_PROPERTY_DISPLAY_URI, &left, 0);
	if (SUCCEEDED(result))
		result = uri_get_property_bstr(other, Uri_PROPERTY_DISPLAY_URI, &right, 0);
	if (SUCCEEDED(result))
	{
		const uri_data *a = &self->data, *b = &from_uri(other)->data;
		*equal = urlmon_uris_match(a, b);
		result = S_OK;
	}
	SysFreeString(left);
	SysFreeString(right);
	return result;
}

static const IUriVtbl uri_vtable = {
	uri_query_interface, uri_add_ref, uri_release, uri_get_property_bstr, uri_get_property_length, uri_get_property_dword, uri_has_property,
	uri_get_absolute, uri_get_authority, uri_get_display, uri_get_domain, uri_get_extension, uri_get_fragment, uri_get_host, uri_get_password,
	uri_get_path, uri_get_path_and_query, uri_get_query, uri_get_raw, uri_get_scheme_name, uri_get_user_info, uri_get_user_name,
	uri_get_host_type, uri_get_port, uri_get_scheme, uri_get_zone, uri_get_properties, uri_is_equal,
};

static HRESULT WINAPI factory_query_interface(IUriBuilderFactory *iface, REFIID iid, void **object)
{
	return uri_query_interface(&from_factory(iface)->IUri_iface, iid, object);
}

static ULONG WINAPI factory_add_ref(IUriBuilderFactory *iface)
{
	return uri_add_ref(&from_factory(iface)->IUri_iface);
}

static ULONG WINAPI factory_release(IUriBuilderFactory *iface)
{
	return uri_release(&from_factory(iface)->IUri_iface);
}

static HRESULT WINAPI factory_create_builder(IUriBuilderFactory *iface, DWORD flags, DWORD_PTR reserved, IUriBuilder **builder)
{
	if (!builder)
		return E_POINTER;
	if (flags || reserved)
	{
		*builder = NULL;
		return E_INVALIDARG;
	}
	return CreateIUriBuilder(NULL, 0, 0, builder);
}

static HRESULT WINAPI factory_create_initialized_builder(IUriBuilderFactory *iface, DWORD flags, DWORD_PTR reserved, IUriBuilder **builder)
{
	uri_object *self = from_factory(iface);
	if (!builder)
		return E_POINTER;
	*builder = NULL;
	if (!self->ready)
		return E_UNEXPECTED;
	if (flags || reserved)
		return E_INVALIDARG;
	if (!self->ready)
		return E_UNEXPECTED;
	return CreateIUriBuilder(&self->IUri_iface, 0, 0, builder);
}

static const IUriBuilderFactoryVtbl factory_vtable = {
	factory_query_interface, factory_add_ref, factory_release, factory_create_builder, factory_create_initialized_builder,
};

static HRESULT WINAPI persist_query_interface(IPersistStream *iface, REFIID iid, void **object)
{
	return uri_query_interface(&from_persist(iface)->IUri_iface, iid, object);
}

static ULONG WINAPI persist_add_ref(IPersistStream *iface)
{
	return uri_add_ref(&from_persist(iface)->IUri_iface);
}

static ULONG WINAPI persist_release(IPersistStream *iface)
{
	return uri_release(&from_persist(iface)->IUri_iface);
}

static HRESULT WINAPI persist_get_class_id(IPersistStream *iface, CLSID *clsid)
{
	if (!clsid)
		return E_INVALIDARG;
	*clsid = CLSID_CUri;
	return S_OK;
}

static HRESULT WINAPI persist_is_dirty(IPersistStream *iface)
{
	return S_FALSE;
}

/* Saved form: a size, six words of which the third holds the flags, how many properties follow,
 * and each one as id, byte count, value. The size counts two bytes more than what is there. */
#define PERSIST_HEADER_DWORDS 8

static const Uri_PROPERTY persisted[] = {
	Uri_PROPERTY_RAW_URI, Uri_PROPERTY_FRAGMENT, Uri_PROPERTY_HOST, Uri_PROPERTY_PASSWORD, Uri_PROPERTY_PATH, Uri_PROPERTY_PORT, Uri_PROPERTY_QUERY,
	Uri_PROPERTY_SCHEME_NAME, Uri_PROPERTY_USER_NAME,
};

/* Only hierarchical network URIs split up; for any other the raw text is all there is. */
static BOOL persists_parts(const uri_data *data)
{
	return data->scheme_type == URL_SCHEME_HTTP || data->scheme_type == URL_SCHEME_HTTPS || data->scheme_type == URL_SCHEME_FTP;
}

static HRESULT measure_persisted(uri_object *self, DWORD *size, DWORD *mask, DWORD *count)
{
	SIZE_T index;
	*size = 0;
	*mask = 0;
	*count = 0;
	for (index = 0; index < ARRAYSIZE(persisted); index++)
	{
		BOOL has = FALSE;
		HRESULT result;
		if (persisted[index] != Uri_PROPERTY_RAW_URI && !persists_parts(&self->data))
			continue;
		result = uri_has_property(&self->IUri_iface, persisted[index], &has);
		if (FAILED(result))
			return result;
		if (!has)
			continue;
		*mask |= 1u << persisted[index];
		(*count)++;
		if (persisted[index] == Uri_PROPERTY_PORT)
			*size += 3 * sizeof(DWORD);
		else
		{
			DWORD length;
			result = uri_get_property_length(&self->IUri_iface, persisted[index], &length, 0);
			if (FAILED(result))
				return result;
			*size += 2 * sizeof(DWORD) + (length + 1) * sizeof(WCHAR);
		}
	}
	return S_OK;
}

static HRESULT WINAPI persist_get_size_max(IPersistStream *iface, ULARGE_INTEGER *size)
{
	uri_object *self = from_persist(iface);
	DWORD body = 0, mask, count;
	HRESULT result;
	if (!size)
		return E_INVALIDARG;
	size->QuadPart = 0;
	if (self->ready)
	{
		result = measure_persisted(self, &body, &mask, &count);
		if (FAILED(result))
			return result;
	}
	size->QuadPart = (ULONGLONG)body + PERSIST_HEADER_DWORDS * sizeof(DWORD) + 2;
	return S_OK;
}

/* Everything after the leading size. A marshalled URI also spells out an empty path. */
static HRESULT write_persisted(uri_object *self, IStream *stream, DWORD mask, DWORD count, BOOL spell_empty_path)
{
	DWORD header[PERSIST_HEADER_DWORDS - 1] = {0};
	BOOL extra_path = spell_empty_path && persists_parts(&self->data) && !(mask & (1u << Uri_PROPERTY_PATH));
	HRESULT result;
	SIZE_T index;
	header[2] = uri_effective_flags(self->data.flags);
	header[6] = count + (extra_path ? 1 : 0);
	result = IStream_Write(stream, header, sizeof(header), NULL);
	for (index = 0; SUCCEEDED(result) && index < ARRAYSIZE(persisted); index++)
	{
		DWORD item[3];
		if (persisted[index] == Uri_PROPERTY_PATH && extra_path)
		{
			item[0] = Uri_PROPERTY_PATH;
			item[1] = sizeof(DWORD);
			item[2] = 0;
			result = IStream_Write(stream, item, sizeof(item), NULL);
			continue;
		}
		if (!(mask & (1u << persisted[index])))
			continue;
		item[0] = persisted[index];
		if (persisted[index] == Uri_PROPERTY_PORT)
		{
			item[1] = sizeof(DWORD);
			item[2] = self->data.port;
			result = IStream_Write(stream, item, sizeof(item), NULL);
		}
		else
		{
			BSTR text;
			result = uri_get_property_bstr(&self->IUri_iface, persisted[index], &text, 0);
			if (FAILED(result))
				break;
			item[1] = (SysStringLen(text) + 1) * sizeof(WCHAR);
			result = IStream_Write(stream, item, 2 * sizeof(DWORD), NULL);
			if (SUCCEEDED(result))
				result = IStream_Write(stream, text, item[1], NULL);
			SysFreeString(text);
		}
	}
	return result;
}

static HRESULT WINAPI persist_save(IPersistStream *iface, IStream *stream, BOOL clear)
{
	uri_object *self = from_persist(iface);
	DWORD body, mask, count, total;
	HRESULT result;
	if (!stream)
		return E_INVALIDARG;
	if (!self->ready)
		return E_UNEXPECTED;
	result = measure_persisted(self, &body, &mask, &count);
	if (FAILED(result))
		return result;
	total = body + PERSIST_HEADER_DWORDS * sizeof(DWORD) + 2;
	result = IStream_Write(stream, &total, sizeof(total), NULL);
	if (FAILED(result))
		return result;
	return write_persisted(self, stream, mask, count, FALSE);
}

/* Saved data is only trusted for its raw text and flags: parsing gives back everything else. */
static HRESULT read_persisted(uri_object *self, IStream *stream, DWORD limit)
{
	DWORD header[PERSIST_HEADER_DWORDS - 1], index;
	WCHAR *raw = NULL;
	HRESULT result;
	ULONG read;
	result = IStream_Read(stream, header, sizeof(header), &read);
	if (FAILED(result))
		return result;
	if (read != sizeof(header))
		return E_INVALIDARG;
	for (index = 0; SUCCEEDED(result) && index < header[6]; index++)
	{
		DWORD item[2];
		void *value;
		result = IStream_Read(stream, item, sizeof(item), &read);
		if (FAILED(result) || read != sizeof(item) || item[1] > limit)
		{
			result = FAILED(result) ? result : E_INVALIDARG;
			break;
		}
		value = uri_alloc(item[1] + sizeof(WCHAR));
		if (!value)
		{
			result = E_OUTOFMEMORY;
			break;
		}
		result = IStream_Read(stream, value, item[1], &read);
		if (item[0] == Uri_PROPERTY_RAW_URI && !raw)
			raw = value;
		else
			uri_free(value);
	}
	if (SUCCEEDED(result))
	{
		if (raw)
		{
			result = uri_parse(raw, header[2], &self->data);
			self->ready = SUCCEEDED(result);
		}
		else
			result = E_INVALIDARG;
	}
	uri_free(raw);
	return result;
}

static HRESULT WINAPI persist_load(IPersistStream *iface, IStream *stream)
{
	uri_object *self = from_persist(iface);
	DWORD total;
	HRESULT result;
	ULONG read;
	if (!stream)
		return E_INVALIDARG;
	if (self->ready)
		return E_UNEXPECTED;
	result = IStream_Read(stream, &total, sizeof(total), &read);
	if (FAILED(result))
		return result;
	if (read != sizeof(total) || total < PERSIST_HEADER_DWORDS * sizeof(DWORD))
		return E_INVALIDARG;
	return read_persisted(self, stream, total);
}

static const IPersistStreamVtbl persist_vtable = {
	persist_query_interface, persist_add_ref, persist_release, persist_get_class_id, persist_is_dirty, persist_load, persist_save, persist_get_size_max,
};


static inline uri_object *from_marshal(IMarshal *iface)
{
	return CONTAINING_RECORD(iface, uri_object, IMarshal_iface);
}

static HRESULT WINAPI marshal_query_interface(IMarshal *iface, REFIID iid, void **object)
{
	return uri_query_interface(&from_marshal(iface)->IUri_iface, iid, object);
}

static ULONG WINAPI marshal_add_ref(IMarshal *iface)
{
	return uri_add_ref(&from_marshal(iface)->IUri_iface);
}

static ULONG WINAPI marshal_release(IMarshal *iface)
{
	return uri_release(&from_marshal(iface)->IUri_iface);
}

/* A URI is its text: every copy that can see this process's memory or the local machine rebuilds it. */
static BOOL marshal_context_ok(DWORD context)
{
	return context == MSHCTX_LOCAL || context == MSHCTX_NOSHAREDMEM || context == MSHCTX_INPROC;
}

static HRESULT WINAPI marshal_get_class(IMarshal *iface, REFIID iid, void *object, DWORD context, void *data, DWORD flags, CLSID *clsid)
{
	if (!clsid || !marshal_context_ok(context))
		return E_INVALIDARG;
	*clsid = CLSID_CUri;
	return S_OK;
}

static HRESULT marshal_measure(uri_object *self, DWORD *body, DWORD *mask, DWORD *count, DWORD *persisted_size)
{
	HRESULT result = measure_persisted(self, body, mask, count);
	if (FAILED(result))
		return result;
	if (persists_parts(&self->data) && !(*mask & (1u << Uri_PROPERTY_PATH)))
		*body += 3 * sizeof(DWORD);
	*persisted_size = *body + PERSIST_HEADER_DWORDS * sizeof(DWORD) + 2;
	return S_OK;
}

/* Marshalled form: total size, context, size of the saved form, then that form without its own size. */
static HRESULT WINAPI marshal_get_size_max(IMarshal *iface, REFIID iid, void *object, DWORD context, void *data, DWORD flags, DWORD *size)
{
	uri_object *self = from_marshal(iface);
	DWORD body, mask, count, needed;
	HRESULT result;
	if (!size || !marshal_context_ok(context))
		return E_INVALIDARG;
	if (!self->ready)
		return E_UNEXPECTED;
	result = marshal_measure(self, &body, &mask, &count, &needed);
	if (FAILED(result))
		return result;
	*size = needed + 2 * sizeof(DWORD);
	return S_OK;
}

static HRESULT WINAPI marshal_marshal(IMarshal *iface, IStream *stream, REFIID iid, void *object, DWORD context, void *data, DWORD flags)
{
	uri_object *self = from_marshal(iface);
	DWORD body, mask, count, head[3];
	HRESULT result;
	if (!stream || !marshal_context_ok(context) || flags != MSHLFLAGS_NORMAL)
		return E_INVALIDARG;
	if (!self->ready)
		return E_UNEXPECTED;
	result = marshal_measure(self, &body, &mask, &count, &head[2]);
	if (FAILED(result))
		return result;
	head[0] = head[2] + 2 * sizeof(DWORD);
	head[1] = context;
	result = IStream_Write(stream, head, sizeof(head), NULL);
	if (FAILED(result))
		return result;
	return write_persisted(self, stream, mask, count, TRUE);
}

static HRESULT WINAPI marshal_unmarshal(IMarshal *iface, IStream *stream, REFIID iid, void **object)
{
	uri_object *self = from_marshal(iface);
	DWORD head[3];
	HRESULT result;
	ULONG read;
	if (!stream || !object)
		return E_INVALIDARG;
	*object = NULL;
	if (self->ready)
	{
		uri_clear(&self->data);
		memset(&self->data, 0, sizeof(self->data));
		self->ready = FALSE;
	}
	result = IStream_Read(stream, head, sizeof(head), &read);
	if (FAILED(result))
		return result;
	if (read != sizeof(head))
		return E_INVALIDARG;
	result = read_persisted(self, stream, head[0]);
	if (FAILED(result))
		return result;
	return uri_query_interface(&self->IUri_iface, iid, object);
}

static HRESULT WINAPI marshal_release_data(IMarshal *iface, IStream *stream)
{
	return S_OK;
}

static HRESULT WINAPI marshal_disconnect(IMarshal *iface, DWORD reserved)
{
	return S_OK;
}

static const IMarshalVtbl marshal_vtable = {
	marshal_query_interface, marshal_add_ref, marshal_release, marshal_get_class, marshal_get_size_max, marshal_marshal, marshal_unmarshal, marshal_release_data, marshal_disconnect,
};

static uri_object *new_uri(void)
{
	uri_object *self = uri_alloc(sizeof(*self));
	if (!self)
		return NULL;
	self->IUri_iface.lpVtbl = &uri_vtable;
	self->IMarshal_iface.lpVtbl = &marshal_vtable;
	self->IUriBuilderFactory_iface.lpVtbl = &factory_vtable;
	self->IPersistStream_iface.lpVtbl = &persist_vtable;
	self->references = 1;
	urlmon_lock_module();
	return self;
}

/* What was composed for a URI reads as the result itself, as its raw text. */
void urlmon_uri_settle_raw(IUri *uri)
{
	uri_object *self = from_uri(uri);
	WCHAR *raw = uri_text_copy(self->data.absolute);
	if (raw)
	{
		uri_free(self->data.raw);
		self->data.raw = raw;
	}
	self->data.combined = TRUE;
	if (!self->data.path)
		self->data.path = uri_text_copy(L"");
}

/* A relative text cannot say it has an authority: the builder states it afterwards. */
HRESULT urlmon_uri_set_authority(IUri *uri, const WCHAR *user, const WCHAR *password, const WCHAR *host, const WCHAR *path)
{
	uri_data *data = &from_uri(uri)->data;
	const WCHAR *values[] = {user ? user : L"", password, host, path ? path : L""};
	WCHAR **slots[] = {&data->user, &data->password, &data->host, &data->path};
	int index;
	for (index = 0; index < 4; index++)
	{
		WCHAR *copy = NULL;
		if (values[index])
		{
			copy = uri_text_copy(values[index]);
			if (!copy)
				return E_OUTOFMEMORY;
		}
		uri_free(*slots[index]);
		*slots[index] = copy;
	}
	data->has_authority = TRUE;
	data->show_user_info = user || password;
	return S_OK;
}

static BOOL same_text(const WCHAR *left, const WCHAR *right, BOOL fold)
{
	if (!left || !right)
		return !left && !right;
	return fold ? !lstrcmpiW(left, right) : !wcscmp(left, right);
}

/* The path as a comparison reads it. */
static WCHAR *comparable_path(const uri_data *data)
{
	BOOL file = data->scheme_type == URL_SCHEME_FILE;
	const WCHAR *path = data->path ? data->path : L"";
	WCHAR *copy = uri_alloc((wcslen(path) + 3) * sizeof(WCHAR)), *at, *read, *write;
	if (!copy)
		return NULL;
	if (file && path[0] && path[1] == L':')
		copy[0] = L'/';
	lstrcatW(copy, path);
	if (!copy[0])
		copy[0] = L'/';
	if (file)
		for (at = copy; *at; at++)
			if (*at == L'\\')
				*at = L'/';
	if (!urlmon_is_known(data) && !file)
		return copy;
	write = read = copy;
	if (*read == L'/')
		write = ++read;
	while (*read)
	{
		WCHAR *stop = read;
		SIZE_T length;
		while (*stop && *stop != L'/')
			stop++;
		length = stop - read;
		if ((length == 1 && read[0] == L'.') || (length == 2 && read[0] == L'.' && read[1] == L'.'))
		{
			if (length == 2 && write > copy + 1)
			{
				write--;
				while (write > copy + 1 && write[-1] != L'/')
					write--;
			}
			read = *stop ? stop + 1 : stop;
			continue;
		}
		memmove(write, read, length * sizeof(WCHAR));
		write += length;
		if (*stop)
			*write++ = *stop++;
		read = stop;
	}
	*write = 0;
	return copy;
}

BOOL urlmon_is_known(const uri_data *data)
{
	return data->scheme_type != URL_SCHEME_UNKNOWN && data->scheme_type != URL_SCHEME_WILDCARD;
}

BOOL urlmon_uris_match(const uri_data *left, const uri_data *right)
{
	BOOL known = urlmon_is_known(left), file = left->scheme_type == URL_SCHEME_FILE, match;
	const WCHAR *empty_user = L"";
	WCHAR *left_path, *right_path;
	if (!same_text(left->scheme, right->scheme, FALSE))
		return FALSE;
	if (!same_text(left->host && left->host[0] ? left->host : NULL, right->host && right->host[0] ? right->host : NULL, known))
		return FALSE;
	if (left->port != right->port)
		return FALSE;
	if (!same_text(left->user ? left->user : empty_user, right->user ? right->user : empty_user, FALSE) || !same_text(left->password, right->password, FALSE))
		return FALSE;
	if (!same_text(left->query, right->query, FALSE) || !same_text(left->fragment, right->fragment, FALSE))
		return FALSE;
	left_path = comparable_path(left);
	right_path = comparable_path(right);
	match = left_path && right_path && same_text(left_path, right_path, file);
	uri_free(left_path);
	uri_free(right_path);
	return match;
}

HRESULT urlmon_create_empty_uri(REFIID iid, void **object)
{
	uri_object *self = new_uri();
	HRESULT result;
	if (!self)
		return E_OUTOFMEMORY;
	result = uri_query_interface(&self->IUri_iface, iid, object);
	uri_release(&self->IUri_iface);
	return result;
}

HRESULT WINAPI CreateUri(LPCWSTR text, DWORD flags, DWORD_PTR reserved, IUri **uri)
{
	uri_object *self;
	HRESULT result;
	if (!uri)
		return E_INVALIDARG;
	*uri = NULL;
	if (!text || !uri_flags_valid(flags))
		return E_INVALIDARG;
	if ((flags & (Uri_CREATE_FILE_USE_DOS_PATH | Uri_CREATE_ALLOW_RELATIVE)) == (Uri_CREATE_FILE_USE_DOS_PATH | Uri_CREATE_ALLOW_RELATIVE))
		return E_INVALIDARG;
	self = new_uri();
	if (!self)
		return E_OUTOFMEMORY;
	result = uri_parse(text, flags, &self->data);
	if (FAILED(result))
	{
		uri_release(&self->IUri_iface);
		return result;
	}
	self->ready = TRUE;
	*uri = &self->IUri_iface;
	return S_OK;
}

HRESULT urlmon_create_uri_combined(const WCHAR *text, DWORD flags, IUri **uri)
{
	uri_object *self;
	HRESULT result;
	*uri = NULL;
	self = new_uri();
	if (!self)
		return E_OUTOFMEMORY;
	result = uri_parse(text, flags | URI_INTERNAL_COMBINED, &self->data);
	if (FAILED(result))
	{
		uri_release(&self->IUri_iface);
		return result;
	}
	self->ready = TRUE;
	*uri = &self->IUri_iface;
	return S_OK;
}

HRESULT WINAPI CreateUriWithFragment(LPCWSTR text, LPCWSTR fragment, DWORD flags, DWORD_PTR reserved, IUri **uri)
{
	WCHAR *both;
	const WCHAR *mark;
	HRESULT result;
	if (!uri)
		return E_INVALIDARG;
	if (!text)
	{
		*uri = NULL;
		return E_INVALIDARG;
	}
	if (!fragment)
		return CreateUri(text, flags, reserved, uri);
	*uri = NULL;
	if (wcschr(text, L'#'))
		return E_INVALIDARG;
	mark = fragment[0] == L'#' ? L"" : L"#";
	both = uri_alloc((wcslen(text) + wcslen(fragment) + 2) * sizeof(WCHAR));
	if (!both)
		return E_OUTOFMEMORY;
	lstrcpyW(both, text);
	lstrcatW(both, mark);
	lstrcatW(both, fragment);
	result = CreateUri(both, flags, reserved, uri);
	uri_free(both);
	return result;
}
