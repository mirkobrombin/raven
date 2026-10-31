/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "urlmon_private.h"

/* Copies a result text the way the plain URL helpers report it. */
static HRESULT give_text(const WCHAR *text, WCHAR *buffer, DWORD size, DWORD *used)
{
	DWORD length = (DWORD)wcslen(text);
	if (used)
		*used = length;
	if (length + 1 > size)
	{
		if (used)
			*used = length + 1;
		return E_POINTER;
	}
	memcpy(buffer, text, (length + 1) * sizeof(WCHAR));
	return S_OK;
}

static HRESULT parse_schema(const WCHAR *url, WCHAR *buffer, DWORD size, DWORD *used)
{
	const WCHAR *colon = wcschr(url, L':');
	WCHAR *scheme;
	HRESULT result;
	if (!colon)
		return give_text(L"", buffer, size, used);
	scheme = uri_text_range(url, colon);
	if (!scheme)
		return E_OUTOFMEMORY;
	result = give_text(scheme, buffer, size, used);
	uri_free(scheme);
	return result;
}

/* The authority of a hierarchical URL. */
static HRESULT parse_domain(const WCHAR *url, WCHAR *buffer, DWORD size, DWORD *used)
{
	const WCHAR *colon = wcschr(url, L':'), *start, *end;
	WCHAR *domain;
	HRESULT result;
	if (!colon)
		return E_FAIL;
	start = colon + 1;
	if (start[0] == L'/' && start[1] == L'/')
	{
		start += 2;
		end = start;
		while (*end && *end != L'/' && *end != L'\\' && *end != L'?' && *end != L'#')
			end++;
		/* What looks like a drive after the slashes of file: is not an authority. */
		if (end > start && wcschr(start, L':') && wcschr(start, L':') < end)
			end = start;
	}
	else
		end = start;
	domain = uri_text_range(start, end);
	if (!domain)
		return E_OUTOFMEMORY;
	result = give_text(domain, buffer, size, used);
	uri_free(domain);
	return result;
}

static BOOL is_network_scheme(const WCHAR *url)
{
	static const WCHAR *const schemes[] = {L"http:", L"https:", L"ftp:"};
	DWORD index;
	for (index = 0; index < ARRAYSIZE(schemes); index++)
		if (!_wcsnicmp(url, schemes[index], wcslen(schemes[index])))
			return TRUE;
	return FALSE;
}

static HRESULT parse_root_document(const WCHAR *url, WCHAR *buffer, DWORD size, DWORD *used)
{
	const WCHAR *end;
	WCHAR *root;
	HRESULT result;
	if (!is_network_scheme(url))
		return E_FAIL;
	end = wcsstr(url, L"://");
	if (!end)
		return E_FAIL;
	end += 3;
	while (*end && *end != L'/' && *end != L'?' && *end != L'#')
		end++;
	root = uri_text_range(url, end);
	if (!root)
		return E_OUTOFMEMORY;
	result = give_text(root, buffer, size, used);
	uri_free(root);
	return result;
}

/* Default handling of each parse action; the caller already gave the protocol its chance. */
static HRESULT default_parse(const WCHAR *url, PARSEACTION action, DWORD flags, WCHAR *buffer, DWORD size, DWORD *used)
{
	DWORD length = size;
	HRESULT result;
	switch (action)
	{
	case PARSE_CANONICALIZE:
		result = UrlCanonicalizeW(url, buffer, &length, flags);
		if (result == E_POINTER && used)
			*used = length;
		else if (SUCCEEDED(result) && used)
			*used = length;
		return result;
	case PARSE_ENCODE:
	case PARSE_UNESCAPE:
		result = UrlUnescapeW((WCHAR *)url, buffer, &length, flags);
		if (used)
			*used = length;
		return result;
	case PARSE_PATH_FROM_URL:
		if (_wcsnicmp(url, L"file:", 5))
			return E_INVALIDARG;
		result = PathCreateFromUrlW(url, buffer, &length, flags);
		if (used)
			*used = length;
		return result;
	case PARSE_SCHEMA:
		return parse_schema(url, buffer, size, used);
	case PARSE_SECURITY_DOMAIN:
	case PARSE_DOMAIN:
		if (!_wcsnicmp(url, L"file:", 5))
			return give_text(L"", buffer, size, used);
		if (!is_network_scheme(url) && !wcschr(url, L':'))
			return E_FAIL;
		if (!is_network_scheme(url))
			return E_FAIL;
		return parse_domain(url, buffer, size, used);
	case PARSE_ROOTDOCUMENT:
		return parse_root_document(url, buffer, size, used);
	case PARSE_SECURITY_URL:
	case PARSE_MIME:
	case PARSE_SERVER:
	case PARSE_LOCATION:
	case PARSE_SITE:
	case PARSE_ANCHOR:
	case PARSE_FRIENDLY:
	case PARSE_DOCUMENT:
	case PARSE_DECODE:
	case PARSE_URL_FROM_PATH:
	default:
		return E_FAIL;
	}
}

/* res: and about: are their own security context; anything else without a protocol's say has none. */
static HRESULT default_security_url(const WCHAR *url, WCHAR *buffer, DWORD size, DWORD *used)
{
	if (!_wcsnicmp(url, L"res:", 4) || !_wcsnicmp(url, L"about:", 6))
		return give_text(url, buffer, size, used);
	return E_FAIL;
}

HRESULT WINAPI CoInternetParseUrl(LPCWSTR url, PARSEACTION action, DWORD flags, LPWSTR buffer, DWORD size, DWORD *used, DWORD reserved)
{
	IInternetProtocolInfo *info = urlmon_url_protocol_info(url);
	HRESULT result = INET_E_DEFAULT_ACTION;
	if (info)
	{
		result = IInternetProtocolInfo_ParseUrl(info, url, action, flags, buffer, size, used, 0);
		IInternetProtocolInfo_Release(info);
		if (result == S_OK || result == S_FALSE)
			return result;
	}
	if (action == PARSE_SECURITY_URL)
		return default_security_url(url, buffer, size, used);
	return default_parse(url, action, flags, buffer, size, used);
}

HRESULT WINAPI CoInternetCombineUrl(LPCWSTR base, LPCWSTR relative, DWORD flags, LPWSTR buffer, DWORD size, DWORD *used, DWORD reserved)
{
	IInternetProtocolInfo *info = urlmon_url_protocol_info(base);
	DWORD length = size;
	HRESULT result;
	if (info)
	{
		result = IInternetProtocolInfo_CombineUrl(info, base, relative, flags, buffer, size, used, 0);
		IInternetProtocolInfo_Release(info);
		if (SUCCEEDED(result))
			return result;
	}
	result = UrlCombineW(base, relative, buffer, &length, flags);
	if (used)
		*used = length;
	return result;
}

HRESULT WINAPI CoInternetCompareUrl(LPCWSTR left, LPCWSTR right, DWORD flags)
{
	IInternetProtocolInfo *info = urlmon_url_protocol_info(left);
	HRESULT result;
	if (info)
	{
		result = IInternetProtocolInfo_CompareUrl(info, left, right, flags);
		IInternetProtocolInfo_Release(info);
		if (result == S_OK || result == S_FALSE)
			return result;
	}
	return !lstrcmpW(left, right) ? S_OK : S_FALSE;
}

HRESULT WINAPI CoInternetQueryInfo(LPCWSTR url, QUERYOPTION option, DWORD flags, LPVOID buffer, DWORD size, DWORD *used, DWORD reserved)
{
	IInternetProtocolInfo *info = urlmon_url_protocol_info(url);
	HRESULT result = E_FAIL;
	if (info)
	{
		result = IInternetProtocolInfo_QueryInfo(info, url, option, flags, buffer, size, used, 0);
		IInternetProtocolInfo_Release(info);
		if (result != E_FAIL && result != INET_E_DEFAULT_ACTION && result != E_NOTIMPL && result != INET_E_QUERYOPTION_UNKNOWN)
			return result;
	}
	switch (option)
	{
	case QUERY_USES_NETWORK:
		if (!buffer || size < sizeof(DWORD))
			return E_FAIL;
		*(DWORD *)buffer = 0;
		if (used)
			*used = sizeof(DWORD);
		return S_OK;
	default:
		return E_FAIL;
	}
}

HRESULT WINAPI IsValidURL(LPBC context, LPCWSTR url, DWORD reserved)
{
	if (!url || reserved)
		return E_INVALIDARG;
	return S_OK;
}

HRESULT WINAPI CoInternetGetProtocolFlags(LPCWSTR url, DWORD *flags, DWORD reserved)
{
	if (flags)
		*flags = 0;
	return E_NOTIMPL;
}

BOOL WINAPI IsLoggingEnabledA(LPCSTR url)
{
	return FALSE;
}

BOOL WINAPI IsLoggingEnabledW(LPCWSTR url)
{
	return FALSE;
}

BOOL WINAPI IsProtectedModeURL(LPCWSTR url)
{
	return FALSE;
}

HRESULT WINAPI IsInternetESCEnabledLocal(void)
{
	return S_FALSE;
}

HRESULT WINAPI LogSqmBits(DWORD id, DWORD bits)
{
	return S_OK;
}

HRESULT WINAPI LogSqmIncrement(DWORD id, DWORD amount)
{
	return S_OK;
}

HRESULT WINAPI LogSqmUXCommandOffsetInternal(DWORD a, DWORD b, DWORD c, DWORD d)
{
	return S_OK;
}

HRESULT WINAPI MapUriToBrowserEmulationState(DWORD a, DWORD b, DWORD c)
{
	return E_NOTIMPL;
}

HRESULT WINAPI CoInternetGetBrowserProfile(DWORD profile)
{
	return E_NOTIMPL;
}

HRESULT WINAPI FlushUrlmonZonesCache(void)
{
	return S_OK;
}

HRESULT WINAPI ShouldShowIntranetWarningSecband(DWORD zone)
{
	return S_FALSE;
}

HRESULT WINAPI GetIUriPriv(IUri *uri, void **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

/* Features: the defaults this generation of the browser settled on, overridable by the registry once. */
#define FEATURE_COUNT ((DWORD)FEATURE_ENTRY_COUNT)

static const struct
{
	const WCHAR *name;
	BOOL enabled;
} feature_table[] = {
	{L"FEATURE_OBJECT_CACHING", TRUE}, {L"FEATURE_ZONE_ELEVATION", FALSE}, {L"FEATURE_MIME_HANDLING", FALSE}, {L"FEATURE_MIME_SNIFFING", FALSE},
	{L"FEATURE_WINDOW_RESTRICTIONS", FALSE}, {L"FEATURE_WEBOC_POPUPMANAGEMENT", FALSE}, {L"FEATURE_BEHAVIORS", TRUE}, {L"FEATURE_DISABLE_MK_PROTOCOL", TRUE},
	{L"FEATURE_LOCALMACHINE_LOCKDOWN", FALSE}, {L"FEATURE_SECURITYBAND", FALSE}, {L"FEATURE_RESTRICT_ACTIVEXINSTALL", FALSE}, {L"FEATURE_VALIDATE_NAVIGATE_URL", FALSE},
	{L"FEATURE_RESTRICT_FILEDOWNLOAD", FALSE}, {L"FEATURE_ADDON_MANAGEMENT", FALSE}, {L"FEATURE_PROTOCOL_LOCKDOWN", FALSE}, {L"FEATURE_HTTP_USERNAME_PASSWORD_DISABLE", FALSE},
	{L"FEATURE_SAFE_BINDTOOBJECT", FALSE}, {L"FEATURE_UNC_SAVEDFILECHECK", FALSE}, {L"FEATURE_GET_URL_DOM_FILEPATH_UNENCODED", TRUE}, {L"FEATURE_TABBED_BROWSING", FALSE},
	{L"FEATURE_SSLUX", FALSE}, {L"FEATURE_DISABLE_NAVIGATION_SOUNDS", FALSE}, {L"FEATURE_DISABLE_LEGACY_COMPRESSION", TRUE}, {L"FEATURE_FORCE_ADDR_AND_STATUS", FALSE},
	{L"FEATURE_XMLHTTP", TRUE}, {L"FEATURE_DISABLE_TELNET_PROTOCOL", FALSE}, {L"FEATURE_FEEDS", FALSE}, {L"FEATURE_BLOCK_INPUT_PROMPTS", FALSE},
};

static BYTE feature_known[ARRAYSIZE(feature_table)];
static BYTE feature_value[ARRAYSIZE(feature_table)];

static BOOL read_feature(HKEY root, const WCHAR *name, BOOL *value)
{
	static const WCHAR base[] = L"Software\\Microsoft\\Internet Explorer\\Main\\FeatureControl\\";
	WCHAR path[128], module[MAX_PATH], *file, *at;
	const WCHAR *names[2];
	DWORD data, size, type, index;
	HKEY key;
	BOOL found = FALSE;
	lstrcpyW(path, base);
	lstrcatW(path, name);
	if (RegOpenKeyExW(root, path, 0, KEY_READ, &key))
		return FALSE;
	GetModuleFileNameW(NULL, module, ARRAYSIZE(module));
	file = module;
	for (at = module; *at; at++)
		if (*at == L'\\' || *at == L'/')
			file = at + 1;
	names[0] = file;
	names[1] = L"*";
	for (index = 0; index < 2 && !found; index++)
	{
		size = sizeof(data);
		if (!RegQueryValueExW(key, names[index], NULL, &type, (BYTE *)&data, &size) && type == REG_DWORD)
		{
			*value = data != 0;
			found = TRUE;
		}
	}
	RegCloseKey(key);
	return found;
}

/* The first answer sticks: later registry changes reach only other processes. */
static BOOL feature_is_on(DWORD feature)
{
	if (!feature_known[feature])
	{
		BOOL value = feature_table[feature].enabled;
		if (!read_feature(HKEY_CURRENT_USER, feature_table[feature].name, &value))
			read_feature(HKEY_LOCAL_MACHINE, feature_table[feature].name, &value);
		feature_value[feature] = (BYTE)value;
		feature_known[feature] = TRUE;
	}
	return feature_value[feature];
}

HRESULT WINAPI CoInternetIsFeatureEnabled(INTERNETFEATURELIST feature, DWORD flags)
{
	if ((DWORD)feature >= ARRAYSIZE(feature_table))
		return E_FAIL;
	if (flags != GET_FEATURE_FROM_PROCESS && flags != GET_FEATURE_FROM_THREAD && flags != GET_FEATURE_FROM_THREAD_LOCALMACHINE &&
		flags != GET_FEATURE_FROM_THREAD_INTRANET && flags != GET_FEATURE_FROM_THREAD_TRUSTED && flags != GET_FEATURE_FROM_THREAD_INTERNET && flags != GET_FEATURE_FROM_THREAD_RESTRICTED && flags != GET_FEATURE_FROM_REGISTRY)
		return E_NOTIMPL;
	if (flags != GET_FEATURE_FROM_PROCESS)
		return E_NOTIMPL;
	return feature_is_on(feature) ? S_OK : S_FALSE;
}

HRESULT WINAPI CoInternetSetFeatureEnabled(INTERNETFEATURELIST feature, DWORD flags, BOOL enable)
{
	if ((DWORD)feature >= ARRAYSIZE(feature_table))
		return E_FAIL;
	if (flags != SET_FEATURE_ON_PROCESS)
		return E_NOTIMPL;
	feature_known[feature] = TRUE;
	feature_value[feature] = (BYTE)(enable != FALSE);
	return S_OK;
}

HRESULT WINAPI CoInternetIsFeatureEnabledForUrl(INTERNETFEATURELIST feature, DWORD flags, LPCWSTR url, IInternetSecurityManager *manager)
{
	DWORD policy = 0;
	HRESULT result;
	if (!url || (DWORD)feature >= ARRAYSIZE(feature_table))
		return E_INVALIDARG;
	result = CoInternetIsFeatureEnabled(feature, flags);
	if (result != S_OK || !manager)
		return result;
	(void)policy;
	return result;
}

HRESULT WINAPI CoInternetIsFeatureZoneElevationEnabled(LPCWSTR from, LPCWSTR to, IInternetSecurityManager *manager, DWORD flags)
{
	if (!to)
		return E_INVALIDARG;
	return CoInternetIsFeatureEnabled(FEATURE_ZONE_ELEVATION, flags) == S_OK ? S_OK : S_FALSE;
}
