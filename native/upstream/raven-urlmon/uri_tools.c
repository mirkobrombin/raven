/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "urlmon_private.h"
#include <strsafe.h>

#define COMBINE_FLAGS_OF(flags) \
	((((flags) & URL_DONT_SIMPLIFY) ? Uri_CREATE_NO_CANONICALIZE : 0) | \
	 (((flags) & URL_FILE_USE_PATHURL) ? Uri_CREATE_FILE_USE_DOS_PATH : 0) | \
	 (((flags) & URL_DONT_UNESCAPE_EXTRA_INFO) ? Uri_CREATE_NO_DECODE_EXTRA_INFO : 0))

static WCHAR *join3(const WCHAR *a, const WCHAR *b, const WCHAR *c)
{
	WCHAR *out = uri_alloc((wcslen(a) + wcslen(b) + wcslen(c) + 1) * sizeof(WCHAR));
	if (out)
	{
		lstrcpyW(out, a);
		lstrcatW(out, b);
		lstrcatW(out, c);
	}
	return out;
}

/* Where the directory part of a path ends, keeping "/c:/" whole on file paths. */
static SIZE_T directory_end(const WCHAR *path)
{
	const WCHAR *slash = wcsrchr(path, L'/');
	return slash ? slash - path + 1 : 0;
}

static BOOL has_drive_root(const WCHAR *path)
{
	return path[0] == L'/' && ((path[1] >= L'a' && path[1] <= L'z') || (path[1] >= L'A' && path[1] <= L'Z')) && path[2] == L':';
}

/* A moniker path merges inside what follows "::" and keeps backslashes before it. */
static WCHAR *merge_mk(const WCHAR *base, const WCHAR *relative)
{
	const WCHAR *store = wcsstr(base, L"::");
	WCHAR *clean, *at, *out, *merged;
	clean = uri_text_copy(relative);
	if (!clean)
		return NULL;
	for (at = clean; *at; at++)
		if (*at == L'\\')
			*at = L'/';
	if (store)
	{
		WCHAR *head = uri_text_range(base, store + 2);
		WCHAR *tail = uri_text_copy(store + 2);
		if (!head || !tail)
		{
			uri_free(head);
			uri_free(tail);
			uri_free(clean);
			return NULL;
		}
		for (at = tail; *at; at++)
			if (*at == L'\\')
				*at = L'/';
		if (clean[0] == L'/')
			merged = join3(head, clean, L"");
		else
		{
			tail[directory_end(tail)] = 0;
			out = join3(head, tail, clean);
			merged = out;
		}
		uri_free(head);
		uri_free(tail);
		uri_free(clean);
		return merged;
	}
	if (clean[0] == L'/')
	{
		/* Nothing is left of the store but an '@' tag. */
		const WCHAR *colon = wcschr(base, L':');
		WCHAR *head = base[0] == L'@' && colon ? uri_text_range(base, colon + 1) : uri_text_copy(L"");
		merged = head ? join3(head, clean, L"") : NULL;
		uri_free(head);
		uri_free(clean);
		return merged;
	}
	{
		WCHAR *head = uri_text_range(base, base + directory_end(base));
		merged = head ? join3(head, clean, L"") : NULL;
		uri_free(head);
		uri_free(clean);
		return merged;
	}
}

static WCHAR *merge_path(const uri_data *base, const uri_data *relative)
{
	const WCHAR *path = base->path ? base->path : L"", *rel = relative->path;
	WCHAR *directory, *merged;
	if (base->scheme_type == URL_SCHEME_MK)
		return merge_mk(path, rel);
	if (rel[0] == L'/')
	{
		if (base->scheme_type == URL_SCHEME_FILE && has_drive_root(path))
		{
			directory = uri_text_range(path, path + 3);
			merged = directory ? join3(directory, rel, L"") : NULL;
			uri_free(directory);
			return merged;
		}
		return uri_text_copy(rel);
	}
	directory = uri_text_range(path, path + directory_end(path));
	if (!directory)
		return NULL;
	if (!directory[0] && (base->host || base->has_authority))
		merged = join3(L"/", rel, L"");
	else
		merged = join3(directory, rel, L"");
	uri_free(directory);
	return merged;
}

/* Normalizes a merged file path so ".." never climbs above its drive. */
static void trim_drive_climbs(WCHAR *path)
{
	WCHAR *rest;
	if (!has_drive_root(path) || path[3] != L'/')
		return;
	rest = path + 4;
	while (rest[0] == L'.' && rest[1] == L'.' && rest[2] == L'/')
		memmove(rest, rest + 3, (wcslen(rest + 3) + 1) * sizeof(WCHAR));
}

HRESULT urlmon_combine(const uri_data *base, const uri_data *relative, DWORD url_flags, IUri **result)
{
	const WCHAR *query, *fragment;
	WCHAR *path = NULL, *text, *out, number[12];
	DWORD create = COMBINE_FLAGS_OF(url_flags);
	const uri_data *authority = relative->host && relative->has_authority ? relative : base;
	BOOL no_simplify = (url_flags & URL_DONT_SIMPLIFY) != 0;
	HRESULT status;
	SIZE_T size;
	fragment = relative->fragment;
	if (relative->path[0])
	{
		query = relative->query;
		if (authority == relative)
			path = uri_text_copy(relative->path);
		else
			path = merge_path(base, relative);
	}
	else
	{
		query = relative->query ? relative->query : base->query;
		if (authority == relative)
			path = uri_text_copy(L"/");
		else
			path = uri_text_copy(base->path ? base->path : L"");
	}
	if (!path)
		return E_OUTOFMEMORY;
	if (base->scheme_type == URL_SCHEME_FILE)
		trim_drive_climbs(path);
	size = wcslen(path) + 64;
	size += base->scheme ? wcslen(base->scheme) : 0;
	size += authority->user ? wcslen(authority->user) : 0;
	size += authority->password ? wcslen(authority->password) : 0;
	size += authority->host ? wcslen(authority->host) : 0;
	size += authority->port_text ? wcslen(authority->port_text) : 0;
	size += query ? wcslen(query) : 0;
	size += fragment ? wcslen(fragment) : 0;
	text = uri_alloc(size * sizeof(WCHAR));
	if (!text)
	{
		uri_free(path);
		return E_OUTOFMEMORY;
	}
	out = text;
	if (base->scheme)
	{
		lstrcpyW(out, base->scheme);
		lstrcatW(out, L":");
	}
	if (base->has_authority || authority == relative)
	{
		lstrcatW(out, L"//");
		if (authority->user)
		{
			lstrcatW(out, authority->user);
			if (authority->password)
			{
				lstrcatW(out, L":");
				lstrcatW(out, authority->password);
			}
			lstrcatW(out, L"@");
		}
		if (authority->host)
		{
			if (authority->host_in_brackets)
				lstrcatW(out, L"[");
			lstrcatW(out, authority->host);
			if (authority->host_in_brackets)
				lstrcatW(out, L"]");
		}
		if (authority->port_text)
		{
			lstrcatW(out, L":");
			lstrcatW(out, authority->port_text);
		}
		else if (no_simplify && authority->has_port && authority->port)
		{
			DWORD left = authority->port;
			int used = 0;
			WCHAR reversed[12], *put = number;
			do
			{
				reversed[used++] = (WCHAR)(L'0' + left % 10);
				left /= 10;
			}
			while (left);
			*put++ = L':';
			while (used)
				*put++ = reversed[--used];
			*put = 0;
			lstrcatW(out, number);
		}
	}
	lstrcatW(out, path);
	uri_free(path);
	if (query)
		lstrcatW(out, query);
	if (fragment)
		lstrcatW(out, fragment);
	if (!base->scheme)
		create |= Uri_CREATE_ALLOW_RELATIVE;
	status = urlmon_create_uri_combined(text, create | Uri_CREATE_NO_DECODE_EXTRA_INFO, result);
	uri_free(text);
	if (SUCCEEDED(status))
		urlmon_uri_settle_raw(*result);
	return status;
}

HRESULT WINAPI CoInternetCombineIUri(IUri *base, IUri *relative, DWORD flags, IUri **result, DWORD_PTR reserved)
{
	const uri_data *base_data, *relative_data;
	IInternetProtocolInfo *info;
	if (!result)
		return E_INVALIDARG;
	*result = NULL;
	if (!base || !relative)
		return E_INVALIDARG;
	base_data = urlmon_uri_data(base);
	relative_data = urlmon_uri_data(relative);
	if (!base_data || !relative_data)
		return E_NOTIMPL;
	info = urlmon_protocol_info(base_data->scheme);
	if (info)
	{
		WCHAR buffer[INTERNET_MAX_URL_LENGTH + 1];
		DWORD length = 0;
		HRESULT status = IInternetProtocolInfo_CombineUrl(info, base_data->absolute, relative_data->raw, flags, buffer, ARRAYSIZE(buffer), &length, 0);
		IInternetProtocolInfo_Release(info);
		if (status != INET_E_DEFAULT_ACTION)
			return SUCCEEDED(status) ? CreateUri(buffer, Uri_CREATE_ALLOW_RELATIVE, 0, result) : status;
	}
	if (relative_data->scheme)
	{
		IUri_AddRef(relative);
		*result = relative;
		return S_OK;
	}
	return urlmon_combine(base_data, relative_data, flags, result);
}

HRESULT WINAPI CoInternetCombineUrlEx(IUri *base, LPCWSTR relative, DWORD flags, IUri **result, DWORD_PTR reserved)
{
	const uri_data *base_data;
	IInternetProtocolInfo *info;
	IUri *parsed;
	DWORD create = Uri_CREATE_ALLOW_RELATIVE | Uri_CREATE_ALLOW_IMPLICIT_FILE_SCHEME;
	HRESULT status;
	if (!result)
		return E_POINTER;
	*result = NULL;
	if (!relative)
		return E_UNEXPECTED;
	if (!base)
		return E_INVALIDARG;
	base_data = urlmon_uri_data(base);
	if (!base_data)
		return E_NOTIMPL;
	info = urlmon_protocol_info(base_data->scheme);
	if (info)
	{
		WCHAR buffer[INTERNET_MAX_URL_LENGTH + 1];
		DWORD length = 0;
		status = IInternetProtocolInfo_CombineUrl(info, base_data->absolute, relative, flags, buffer, ARRAYSIZE(buffer), &length, 0);
		IInternetProtocolInfo_Release(info);
		if (status != INET_E_DEFAULT_ACTION)
			return SUCCEEDED(status) ? CreateUri(buffer, Uri_CREATE_ALLOW_RELATIVE, 0, result) : status;
	}
	create |= COMBINE_FLAGS_OF(flags) & ~Uri_CREATE_FILE_USE_DOS_PATH;
	status = CreateUri(relative, create, 0, &parsed);
	if (FAILED(status))
		return status;
	status = CoInternetCombineIUri(base, parsed, flags, result, 0);
	IUri_Release(parsed);
	return status;
}

static BOOL is_hierarchical_known(const uri_data *data)
{
	switch (data->scheme_type)
	{
	case URL_SCHEME_HTTP:
	case URL_SCHEME_HTTPS:
	case URL_SCHEME_FTP:
	case URL_SCHEME_GOPHER:
	case URL_SCHEME_NNTP:
	case URL_SCHEME_TELNET:
	case URL_SCHEME_WAIS:
		return TRUE;
	default:
		return FALSE;
	}
}

/* Walks the text after the host, dropping dot segments. */
static void drop_dot_segments(WCHAR *text, SIZE_T prefix)
{
	WCHAR *tail = text + prefix, *read, *write, *stop = tail, *scan;
	while (*stop && *stop != L'?' && *stop != L'#')
		stop++;
	write = read = tail;
	if (*read == L'/')
		write = ++read;
	while (read < stop)
	{
		WCHAR *end = read;
		SIZE_T length;
		while (end < stop && *end != L'/')
			end++;
		length = end - read;
		if (length == 1 && read[0] == L'.')
		{
			read = end < stop ? end + 1 : end;
			continue;
		}
		if (length == 2 && read[0] == L'.' && read[1] == L'.')
		{
			if (write > tail + 1)
			{
				write--;
				while (write > tail + 1 && write[-1] != L'/')
					write--;
			}
			read = end < stop ? end + 1 : end;
			continue;
		}
		memmove(write, read, length * sizeof(WCHAR));
		write += length;
		if (end < stop)
			*write++ = L'/';
		read = end < stop ? end + 1 : end;
	}
	scan = write;
	memmove(scan, stop, (wcslen(stop) + 1) * sizeof(WCHAR));
}

static WCHAR *make_canonical(const uri_data *data, DWORD flags)
{
	const WCHAR *origin = data->absolute[0] ? data->absolute : data->raw, *at;
	SIZE_T prefix = 0;
	WCHAR *start = uri_text_copy(origin), *copy = uri_alloc((wcslen(origin) * 3 + 2) * sizeof(WCHAR)), *out = copy;
	if (!copy || !start)
	{
		uri_free(start);
		uri_free(copy);
		return NULL;
	}
	if (data->has_authority && data->scheme)
	{
		const WCHAR *slash = wcsstr(start, L"://");
		if (slash)
			slash = wcschr(slash + 3, L'/');
		if (slash)
			prefix = slash - start;
	}
	/* Dots written as escapes are not dots: simplify before anything gets unescaped. */
	if (!(flags & (URL_DONT_SIMPLIFY | URL_NO_META)))
		drop_dot_segments(start, prefix);
	for (at = start; *at; at++)
	{
		WCHAR character = *at;
		BOOL escaped = FALSE;
		if ((flags & URL_UNESCAPE) && character == L'%' && (SIZE_T)(at - start) >= prefix)
		{
			WCHAR high = at[1], low = high ? at[2] : 0;
			int a = high >= L'0' && high <= L'9' ? high - L'0' : (high | 32) >= L'a' && (high | 32) <= L'f' ? (high | 32) - L'a' + 10 : -1;
			int b = low >= L'0' && low <= L'9' ? low - L'0' : (low | 32) >= L'a' && (low | 32) <= L'f' ? (low | 32) - L'a' + 10 : -1;
			if (a >= 0 && b >= 0)
			{
				character = (WCHAR)(a * 16 + b);
				at += 2;
				escaped = (flags & URL_ESCAPE_UNSAFE) != 0;
				if (escaped)
				{
					/* An escape asked to stay safe keeps its text. */
					*out++ = L'%';
					*out++ = high;
					*out++ = low;
					continue;
				}
			}
		}
		if ((flags & URL_ESCAPE_PERCENT) && character == L'%')
		{
			lstrcpyW(out, L"%25");
			out += 3;
		}
		else if (((flags & URL_ESCAPE_SPACES_ONLY) && character == L' ') ||
			((flags & URL_ESCAPE_UNSAFE) && (character == L'<' || character == L'>' || character == L'|' || character == L'"' || character == L'^' || character == L'`' || character == L'{' || character == L'}')))
		{
			static const WCHAR digits[] = L"0123456789ABCDEF";
			*out++ = L'%';
			*out++ = digits[character >> 4];
			*out++ = digits[character & 15];
		}
		else
			*out++ = character;
	}
	*out = 0;
	uri_free(start);
	return copy;
}

static WCHAR *root_document(const uri_data *data)
{
	WCHAR *out;
	SIZE_T size;
	if (!is_hierarchical_known(data) || !data->has_authority)
		return uri_text_copy(L"");
	size = wcslen(data->scheme) + wcslen(data->host) + (data->port_text ? wcslen(data->port_text) : 0) + 16;
	out = uri_alloc(size * sizeof(WCHAR));
	if (out)
	{
		lstrcpyW(out, data->scheme);
		lstrcatW(out, L"://");
		if (data->host_in_brackets)
			lstrcatW(out, L"[");
		lstrcatW(out, data->host);
		if (data->host_in_brackets)
			lstrcatW(out, L"]");
		if (data->port_text)
		{
			lstrcatW(out, L":");
			lstrcatW(out, data->port_text);
		}
		lstrcatW(out, L"/");
	}
	return out;
}

static BOOL is_hex_digit(WCHAR value)
{
	return (value >= L'0' && value <= L'9') || ((value | 32) >= L'a' && (value | 32) <= L'f');
}

static int hex_digit(WCHAR value)
{
	return value <= L'9' ? value - L'0' : (value | 32) - L'a' + 10;
}

static WCHAR *file_path_of(const uri_data *data)
{
	const WCHAR *path = data->path;
	WCHAR *out, *at;
	if (data->flags & Uri_CREATE_FILE_USE_DOS_PATH && path[0] != L'/')
	{
		/* A DOS path keeps what follows '#' in its text; it never names a file. */
		out = uri_text_copy(path);
		if (out && (at = wcschr(out, L'#')))
			*at = 0;
		return out;
	}
	if (data->host && data->host[0])
	{
		out = join3(L"\\\\", data->host, path);
	}
	else
	{
		if (has_drive_root(path))
			path++;
		out = uri_text_copy(path);
	}
	if (out)
	{
		WCHAR *write = out;
		for (at = out; *at; at++)
		{
			WCHAR high = at[0] == L'%' ? at[1] : 0, low = high ? at[2] : 0;
			if (is_hex_digit(high) && is_hex_digit(low))
			{
				*write++ = (WCHAR)(hex_digit(high) * 16 + hex_digit(low));
				at += 2;
			}
			else
				*write++ = *at == L'/' ? L'\\' : *at;
		}
		*write = 0;
	}
	return out;
}

HRESULT WINAPI CoInternetParseIUri(IUri *uri, PARSEACTION action, DWORD flags, LPWSTR buffer, DWORD size, DWORD *used, DWORD_PTR reserved)
{
	const uri_data *data;
	IInternetProtocolInfo *info;
	WCHAR *text = NULL;
	SIZE_T length;
	HRESULT status = S_OK;
	if (!used)
		return E_POINTER;
	if (!uri)
	{
		*used = 0;
		return E_INVALIDARG;
	}
	if (!buffer || !size)
	{
		*used = 0;
		return E_INVALIDARG;
	}
	data = urlmon_uri_data(uri);
	if (!data)
	{
		*used = 0;
		return E_NOTIMPL;
	}
	info = urlmon_protocol_info(data->scheme);
	if (info)
	{
		status = IInternetProtocolInfo_ParseUrl(info, data->absolute, action, flags, buffer, size, used, 0);
		IInternetProtocolInfo_Release(info);
		if (status != INET_E_DEFAULT_ACTION)
			return status;
		status = S_OK;
	}
	switch (action)
	{
	case PARSE_CANONICALIZE:
		text = make_canonical(data, flags);
		break;
	case PARSE_FRIENDLY:
		text = uri_text_copy(data->display);
		break;
	case PARSE_ROOTDOCUMENT:
		text = root_document(data);
		break;
	case PARSE_DOCUMENT:
		if (is_hierarchical_known(data) && data->has_authority)
		{
			text = uri_text_copy(data->absolute);
			if (text && data->fragment)
				text[wcslen(text) - wcslen(data->fragment)] = 0;
		}
		else
			text = uri_text_copy(L"");
		break;
	case PARSE_PATH_FROM_URL:
		if (data->scheme_type != URL_SCHEME_FILE)
		{
			*used = 0;
			buffer[0] = 0;
			return E_INVALIDARG;
		}
		text = file_path_of(data);
		break;
	case PARSE_URL_FROM_PATH:
		text = uri_text_copy(data->scheme ? (data->has_authority ? data->absolute : L"") : data->raw);
		break;
	case PARSE_SCHEMA:
		text = uri_text_copy(data->scheme ? data->scheme : L"");
		break;
	case PARSE_SITE:
		text = uri_text_copy(data->host ? data->host : L"");
		break;
	case PARSE_DOMAIN:
		text = data->host && data->host_type == Uri_HOST_DNS ? uri_domain_of(data->host) : NULL;
		if (!text)
			text = uri_text_copy(L"");
		break;
	case PARSE_LOCATION:
	case PARSE_ANCHOR:
		text = uri_text_copy(data->fragment ? data->fragment : L"");
		break;
	case PARSE_SECURITY_URL:
	case PARSE_MIME:
	case PARSE_SERVER:
	case PARSE_SECURITY_DOMAIN:
		*used = 0;
		buffer[0] = 0;
		return E_FAIL;
	case PARSE_ENCODE:
	case PARSE_DECODE:
	default:
		*used = 0;
		buffer[0] = 0;
		return E_INVALIDARG;
	}
	if (!text)
		return E_OUTOFMEMORY;
	length = wcslen(text);
	/* A bare request to unescape reports nothing rather than a size. */
	if (length + 1 > size && action == PARSE_CANONICALIZE && flags == URL_UNESCAPE)
	{
		*used = 0;
		buffer[0] = 0;
	}
	else if (length + 1 > size)
	{
		*used = (DWORD)length;
		buffer[0] = 0;
		status = STRSAFE_E_INSUFFICIENT_BUFFER;
	}
	else
	{
		lstrcpyW(buffer, text);
		*used = (DWORD)length;
	}
	uri_free(text);
	return status;
}
