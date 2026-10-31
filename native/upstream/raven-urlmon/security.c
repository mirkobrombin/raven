/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "urlmon_private.h"

#define ZONES_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\Zones"
#define ZONE_MAP_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\ZoneMap"

/* Zone of a scheme without further rules, from ZoneMap\ProtocolDefaults when the key has it. */
static DWORD zone_of_scheme(const WCHAR *scheme)
{
	static const HKEY roots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
	DWORD value, size, type, index;
	HKEY key;
	for (index = 0; index < ARRAYSIZE(roots); index++)
	{
		if (RegOpenKeyExW(roots[index], ZONE_MAP_KEY L"\\ProtocolDefaults", 0, KEY_READ, &key))
			continue;
		size = sizeof(value);
		if (!RegQueryValueExW(key, scheme, NULL, &type, (BYTE *)&value, &size) && type == REG_DWORD)
		{
			RegCloseKey(key);
			return value;
		}
		RegCloseKey(key);
	}
	if (!lstrcmpiW(scheme, L"file") || !lstrcmpiW(scheme, L"mk") || !lstrcmpiW(scheme, L"res") || !lstrcmpiW(scheme, L"about"))
		return !lstrcmpiW(scheme, L"about") ? URLZONE_LOCAL_MACHINE : URLZONE_LOCAL_MACHINE;
	return URLZONE_INTERNET;
}

/* What a protocol says stands in for the URL when judging it. */
static HRESULT security_url(const WCHAR *url, WCHAR **out)
{
	WCHAR buffer[INTERNET_MAX_URL_LENGTH];
	DWORD used = 0;
	IInternetProtocolInfo *info = urlmon_url_protocol_info(url);
	SIZE_T size;
	*out = NULL;
	if (info)
	{
		HRESULT result = IInternetProtocolInfo_ParseUrl(info, url, PARSE_SECURITY_URL, 0, buffer, ARRAYSIZE(buffer), &used, 0);
		IInternetProtocolInfo_Release(info);
		if (result == S_OK)
		{
			buffer[ARRAYSIZE(buffer) - 1] = 0;
			if (wcschr(buffer, L':'))
			{
				/* Nested: mapping continues with what the protocol pointed at. */
				return security_url(buffer, out);
			}
		}
	}
	size = (wcslen(url) + 1) * sizeof(WCHAR);
	*out = CoTaskMemAlloc(size);
	if (!*out)
		return E_OUTOFMEMORY;
	memcpy(*out, url, size);
	return S_OK;
}

static HRESULT scheme_part(const WCHAR *url, WCHAR **scheme)
{
	const WCHAR *colon = wcschr(url, L':');
	*scheme = NULL;
	if (!colon)
		return E_INVALIDARG;
	/* C:\... is a path on a disk and that means a file. */
	if (colon == url + 1)
		*scheme = uri_text_copy(L"file");
	else
		*scheme = uri_text_range(url, colon);
	return *scheme ? S_OK : E_OUTOFMEMORY;
}

static HRESULT map_url(const WCHAR *url, DWORD *zone, DWORD flags)
{
	WCHAR *plain = NULL, *scheme = NULL;
	HRESULT result = security_url(url, &plain);
	if (FAILED(result))
		return result;
	result = scheme_part(plain, &scheme);
	if (SUCCEEDED(result))
		*zone = zone_of_scheme(scheme);
	else if (result == E_INVALIDARG)
		*zone = URLZONE_INVALID;
	uri_free(scheme);
	CoTaskMemFree(plain);
	return result;
}

HRESULT WINAPI CoInternetGetSecurityUrl(LPCWSTR url, LPWSTR *out, PSUACTION action, DWORD reserved)
{
	HRESULT result;
	if (!url || !out)
		return E_INVALIDARG;
	result = security_url(url, out);
	if (FAILED(result))
		return result;
	if (action == PSU_DEFAULT)
	{
		/* Only as much as is needed to identify the site. */
		WCHAR *at = wcschr(*out, L':');
		if (at && at[1] == L'/' && at[2] == L'/')
		{
			WCHAR *end = at + 3;
			while (*end && *end != L'/' && *end != L'\\' && *end != L'?' && *end != L'#')
				end++;
			*end = 0;
		}
	}
	return S_OK;
}

HRESULT WINAPI CoInternetGetSecurityUrlEx(IUri *uri, IUri **out, PSUACTION action, DWORD_PTR reserved)
{
	BSTR absolute = NULL;
	WCHAR *text = NULL;
	HRESULT result;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!uri)
		return E_INVALIDARG;
	result = IUri_GetDisplayUri(uri, &absolute);
	if (FAILED(result))
		return result;
	result = CoInternetGetSecurityUrl(absolute, &text, action, 0);
	SysFreeString(absolute);
	if (FAILED(result))
		return result;
	result = CreateUri(text, Uri_CREATE_ALLOW_RELATIVE, 0, out);
	CoTaskMemFree(text);
	return result;
}

HRESULT WINAPI CompareSecurityIds(BYTE *left, DWORD left_size, BYTE *right, DWORD right_size, DWORD reserved)
{
	if (!left || !right || left_size < sizeof(DWORD) || right_size < sizeof(DWORD))
		return E_INVALIDARG;
	return left_size == right_size && !memcmp(left, right, left_size) ? S_OK : S_FALSE;
}

typedef struct
{
	IInternetSecurityManagerEx2 IInternetSecurityManagerEx2_iface;
	LONG references;
	IInternetSecurityMgrSite *site;
	IInternetSecurityManager *custom;
} security_manager;

static inline security_manager *from_manager(IInternetSecurityManagerEx2 *iface)
{
	return CONTAINING_RECORD(iface, security_manager, IInternetSecurityManagerEx2_iface);
}

static HRESULT WINAPI manager_query_interface(IInternetSecurityManagerEx2 *iface, REFIID iid, void **object)
{
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInternetSecurityManager) || IsEqualGUID(iid, &IID_IInternetSecurityManagerEx) || IsEqualGUID(iid, &IID_IInternetSecurityManagerEx2))
	{
		*object = iface;
		IInternetSecurityManagerEx2_AddRef(iface);
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI manager_add_ref(IInternetSecurityManagerEx2 *iface)
{
	return InterlockedIncrement(&from_manager(iface)->references);
}

static ULONG WINAPI manager_release(IInternetSecurityManagerEx2 *iface)
{
	security_manager *self = from_manager(iface);
	ULONG left = InterlockedDecrement(&self->references);
	if (!left)
	{
		if (self->site)
			IInternetSecurityMgrSite_Release(self->site);
		if (self->custom)
			IInternetSecurityManager_Release(self->custom);
		uri_free(self);
		urlmon_unlock_module();
	}
	return left;
}

static HRESULT WINAPI manager_set_site(IInternetSecurityManagerEx2 *iface, IInternetSecurityMgrSite *site)
{
	security_manager *self = from_manager(iface);
	IServiceProvider *provider = NULL;
	if (self->site)
		IInternetSecurityMgrSite_Release(self->site);
	if (self->custom)
		IInternetSecurityManager_Release(self->custom);
	self->site = site;
	self->custom = NULL;
	if (site)
	{
		IInternetSecurityMgrSite_AddRef(site);
		if (SUCCEEDED(IInternetSecurityMgrSite_QueryInterface(site, &IID_IServiceProvider, (void **)&provider)))
		{
			if (FAILED(IServiceProvider_QueryService(provider, &SID_SInternetSecurityManager, &IID_IInternetSecurityManager, (void **)&self->custom)))
				self->custom = NULL;
			IServiceProvider_Release(provider);
		}
	}
	return S_OK;
}

static HRESULT WINAPI manager_get_site(IInternetSecurityManagerEx2 *iface, IInternetSecurityMgrSite **site)
{
	security_manager *self = from_manager(iface);
	if (!site)
		return E_INVALIDARG;
	*site = self->site;
	if (self->site)
		IInternetSecurityMgrSite_AddRef(self->site);
	return S_OK;
}

static HRESULT WINAPI manager_map_url(IInternetSecurityManagerEx2 *iface, LPCWSTR url, DWORD *zone, DWORD flags)
{
	security_manager *self = from_manager(iface);
	HRESULT result;
	if (self->custom)
	{
		result = IInternetSecurityManager_MapUrlToZone(self->custom, url, zone, flags);
		if (result != INET_E_DEFAULT_ACTION)
			return result;
	}
	if (!url)
	{
		if (zone)
			*zone = URLZONE_INVALID;
		return E_INVALIDARG;
	}
	if (flags)
		return E_INVALIDARG;
	return map_url(url, zone, flags);
}

static HRESULT WINAPI manager_get_id(IInternetSecurityManagerEx2 *iface, LPCWSTR url, BYTE *id, DWORD *size, DWORD_PTR reserved)
{
	security_manager *self = from_manager(iface);
	WCHAR *plain = NULL, *at, *end;
	DWORD zone = 0, needed;
	HRESULT result;
	int length;
	if (self->custom)
	{
		result = IInternetSecurityManager_GetSecurityId(self->custom, url, id, size, reserved);
		if (result != INET_E_DEFAULT_ACTION)
			return result;
	}
	if (!url || !id || !size || !*size || reserved)
		return E_INVALIDARG;
	result = map_url(url, &zone, 0);
	if (FAILED(result))
		return result == E_INVALIDARG ? E_INVALIDARG : result;
	result = security_url(url, &plain);
	if (FAILED(result))
		return result;
	/* The id: scheme:host (scheme alone with no host), then the zone. */
	at = wcschr(plain, L':');
	if (at && at == plain + 1)
	{
		CoTaskMemFree(plain);
		plain = CoTaskMemAlloc(sizeof(L"file:"));
		if (!plain)
			return E_OUTOFMEMORY;
		lstrcpyW(plain, L"file:");
		at = plain + 4;
		end = at + 1;
	}
	else if (at && at[1] == L'/' && at[2] == L'/')
	{
		WCHAR *from = at + 3, *to = at + 1;
		end = from;
		while (*end && *end != L'/' && *end != L'\\' && *end != L'?' && *end != L'#')
			end++;
		memmove(to, from, (end - from) * sizeof(WCHAR));
		end = to + (end - from);
	}
	else if (at)
		end = at + 1;
	else
		end = plain + wcslen(plain);
	*end = 0;
	/* A port does not change the site. */
	if (at && (end = wcschr(at + 1, L':')))
		*end = 0;
	for (at = plain; *at; at++)
		if (*at >= L'A' && *at <= L'Z')
			*at += 32;
	length = WideCharToMultiByte(CP_ACP, 0, plain, -1, NULL, 0, NULL, NULL) - 1;
	needed = length + sizeof(DWORD);
	if (*size < needed)
	{
		*size = needed;
		CoTaskMemFree(plain);
		return E_NOT_SUFFICIENT_BUFFER;
	}
	WideCharToMultiByte(CP_ACP, 0, plain, -1, (char *)id, length + 1, NULL, NULL);
	memcpy(id + length, &zone, sizeof(DWORD));
	*size = needed;
	CoTaskMemFree(plain);
	return S_OK;
}

static BOOL read_policy(DWORD zone, DWORD action, DWORD *policy)
{
	static const HKEY roots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
	WCHAR path[128], name[16];
	DWORD index, value, size, type;
	HKEY key;
	wsprintfW(path, ZONES_KEY L"\\%u", zone);
	wsprintfW(name, L"%X", action);
	for (index = 0; index < ARRAYSIZE(roots); index++)
	{
		if (RegOpenKeyExW(roots[index], path, 0, KEY_READ, &key))
			continue;
		size = sizeof(value);
		if (!RegQueryValueExW(key, name, NULL, &type, (BYTE *)&value, &size) && type == REG_DWORD)
		{
			RegCloseKey(key);
			*policy = value;
			return TRUE;
		}
		RegCloseKey(key);
	}
	return FALSE;
}

static HRESULT WINAPI manager_process_action(IInternetSecurityManagerEx2 *iface, LPCWSTR url, DWORD action, BYTE *policy, DWORD size, BYTE *context, DWORD context_size, DWORD flags, DWORD reserved)
{
	security_manager *self = from_manager(iface);
	DWORD zone, value = URLPOLICY_DISALLOW;
	HRESULT result;
	if (self->custom)
	{
		result = IInternetSecurityManager_ProcessUrlAction(self->custom, url, action, policy, size, context, context_size, flags, reserved);
		if (result != INET_E_DEFAULT_ACTION)
			return result;
	}
	if (!url || (flags & (PUAF_ENFORCERESTRICTED)) || reserved)
		return E_INVALIDARG;
	result = map_url(url, &zone, 0);
	if (FAILED(result))
		return result;
	if (!read_policy(zone, action, &value))
		value = zone == URLZONE_LOCAL_MACHINE ? URLPOLICY_ALLOW : URLPOLICY_DISALLOW;
	if (policy && size >= sizeof(DWORD))
		*(DWORD *)policy = value;
	if (policy && size && size < sizeof(DWORD))
		return E_INVALIDARG;
	switch (GetUrlPolicyPermissions(value))
	{
	case URLPOLICY_ALLOW:
		return S_OK;
	case URLPOLICY_QUERY:
		return S_FALSE;
	default:
		return S_FALSE;
	}
}

static HRESULT WINAPI manager_query_policy(IInternetSecurityManagerEx2 *iface, LPCWSTR url, REFGUID key, BYTE **policy, DWORD *size, BYTE *context, DWORD context_size, DWORD reserved)
{
	security_manager *self = from_manager(iface);
	if (self->custom)
	{
		HRESULT result = IInternetSecurityManager_QueryCustomPolicy(self->custom, url, key, policy, size, context, context_size, reserved);
		if (result != INET_E_DEFAULT_ACTION)
			return result;
	}
	if (!url)
		return E_INVALIDARG;
	return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

static HRESULT WINAPI manager_set_mapping(IInternetSecurityManagerEx2 *iface, DWORD zone, LPCWSTR pattern, DWORD flags)
{
	security_manager *self = from_manager(iface);
	if (self->custom)
	{
		HRESULT result = IInternetSecurityManager_SetZoneMapping(self->custom, zone, pattern, flags);
		if (result != INET_E_DEFAULT_ACTION)
			return result;
	}
	return E_NOTIMPL;
}

static HRESULT WINAPI manager_get_mappings(IInternetSecurityManagerEx2 *iface, DWORD zone, IEnumString **patterns, DWORD flags)
{
	security_manager *self = from_manager(iface);
	if (self->custom)
	{
		HRESULT result = IInternetSecurityManager_GetZoneMappings(self->custom, zone, patterns, flags);
		if (result != INET_E_DEFAULT_ACTION)
			return result;
	}
	return E_NOTIMPL;
}

static HRESULT WINAPI manager_process_action_ex(IInternetSecurityManagerEx2 *iface, LPCWSTR url, DWORD action, BYTE *policy, DWORD size, BYTE *context, DWORD context_size, DWORD flags, DWORD reserved, DWORD *result_flags)
{
	if (result_flags)
		*result_flags = 0;
	return manager_process_action(iface, url, action, policy, size, context, context_size, flags, reserved);
}

static HRESULT WINAPI manager_map_url_ex2(IInternetSecurityManagerEx2 *iface, IUri *uri, DWORD *zone, DWORD flags, LPWSTR *mapped, DWORD *ignored)
{
	BSTR display = NULL;
	HRESULT result;
	if (!zone)
		return E_INVALIDARG;
	if (!uri)
	{
		*zone = URLZONE_INVALID;
		return E_INVALIDARG;
	}
	if (flags & ~MUTZ_ACCEPT_WILDCARD_SCHEME)
		return E_INVALIDARG;
	result = IUri_GetDisplayUri(uri, &display);
	if (FAILED(result))
		return result;
	result = map_url(display, zone, 0);
	SysFreeString(display);
	if (mapped)
		*mapped = NULL;
	if (ignored)
		*ignored = 0;
	return result;
}

static HRESULT WINAPI manager_process_action_ex2(IInternetSecurityManagerEx2 *iface, IUri *uri, DWORD action, BYTE *policy, DWORD size, BYTE *context, DWORD context_size, DWORD flags, DWORD_PTR reserved, DWORD *result_flags)
{
	BSTR display = NULL;
	HRESULT result;
	if (!uri)
		return E_INVALIDARG;
	result = IUri_GetDisplayUri(uri, &display);
	if (FAILED(result))
		return result;
	result = manager_process_action_ex(iface, display, action, policy, size, context, context_size, flags, 0, result_flags);
	SysFreeString(display);
	return result;
}

static HRESULT WINAPI manager_get_id_ex2(IInternetSecurityManagerEx2 *iface, IUri *uri, BYTE *id, DWORD *size, DWORD_PTR reserved)
{
	BSTR display = NULL;
	HRESULT result;
	if (reserved || !uri)
		return E_INVALIDARG;
	result = IUri_GetDisplayUri(uri, &display);
	if (FAILED(result))
		return result;
	result = manager_get_id(iface, display, id, size, 0);
	SysFreeString(display);
	return result;
}

static HRESULT WINAPI manager_query_policy_ex2(IInternetSecurityManagerEx2 *iface, IUri *uri, REFGUID key, BYTE **policy, DWORD *size, BYTE *context, DWORD context_size, DWORD_PTR reserved)
{
	BSTR display = NULL;
	HRESULT result;
	if (!uri)
		return E_INVALIDARG;
	result = IUri_GetDisplayUri(uri, &display);
	if (FAILED(result))
		return result;
	result = manager_query_policy(iface, display, key, policy, size, context, context_size, 0);
	SysFreeString(display);
	return result;
}

static const IInternetSecurityManagerEx2Vtbl manager_vtable = {
	manager_query_interface, manager_add_ref, manager_release, manager_set_site, manager_get_site, manager_map_url, manager_get_id, manager_process_action,
	manager_query_policy, manager_set_mapping, manager_get_mappings, manager_process_action_ex, manager_map_url_ex2, manager_process_action_ex2, manager_get_id_ex2, manager_query_policy_ex2,
};

HRESULT urlmon_create_security_manager(IUnknown *outer, REFIID iid, void **object)
{
	security_manager *self;
	HRESULT result;
	*object = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	self = uri_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->IInternetSecurityManagerEx2_iface.lpVtbl = &manager_vtable;
	self->references = 1;
	urlmon_lock_module();
	result = manager_query_interface(&self->IInternetSecurityManagerEx2_iface, iid, object);
	IInternetSecurityManagerEx2_Release(&self->IInternetSecurityManagerEx2_iface);
	return result;
}

HRESULT WINAPI CoInternetCreateSecurityManager(IServiceProvider *provider, IInternetSecurityManager **out, DWORD reserved)
{
	return urlmon_create_security_manager(NULL, &IID_IInternetSecurityManager, (void **)out);
}

typedef struct
{
	IInternetZoneManagerEx2 IInternetZoneManagerEx2_iface;
	LONG references;
	struct enum_entry
	{
		BOOL in_use;
	} enums[8];
} zone_manager;

static inline zone_manager *from_zones(IInternetZoneManagerEx2 *iface)
{
	return CONTAINING_RECORD(iface, zone_manager, IInternetZoneManagerEx2_iface);
}

static HRESULT WINAPI zones_query_interface(IInternetZoneManagerEx2 *iface, REFIID iid, void **object)
{
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInternetZoneManager) || IsEqualGUID(iid, &IID_IInternetZoneManagerEx) || IsEqualGUID(iid, &IID_IInternetZoneManagerEx2))
	{
		*object = iface;
		IInternetZoneManagerEx2_AddRef(iface);
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI zones_add_ref(IInternetZoneManagerEx2 *iface)
{
	return InterlockedIncrement(&from_zones(iface)->references);
}

static ULONG WINAPI zones_release(IInternetZoneManagerEx2 *iface)
{
	zone_manager *self = from_zones(iface);
	ULONG left = InterlockedDecrement(&self->references);
	if (!left)
	{
		uri_free(self);
		urlmon_unlock_module();
	}
	return left;
}

static void read_zone_text(HKEY key, const WCHAR *name, WCHAR *out, DWORD chars)
{
	DWORD size = chars * sizeof(WCHAR), type = 0;
	out[0] = 0;
	if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)out, &size) || type != REG_SZ)
		out[0] = 0;
	out[chars - 1] = 0;
}

static void read_zone_dword(HKEY key, const WCHAR *name, DWORD *out)
{
	DWORD size = sizeof(*out), type = 0, value;
	size = sizeof(value);
	if (!RegQueryValueExW(key, name, NULL, &type, (BYTE *)&value, &size) && type == REG_DWORD)
		*out = value;
}

static HRESULT WINAPI zones_get_attributes(IInternetZoneManagerEx2 *iface, DWORD zone, ZONEATTRIBUTES *attributes)
{
	WCHAR path[128];
	HKEY key;
	if (!attributes)
		return E_INVALIDARG;
	wsprintfW(path, ZONES_KEY L"\\%u", zone);
	if (zone > URLZONE_UNTRUSTED)
		return E_FAIL;
	memset(attributes, 0, sizeof(*attributes));
	attributes->cbSize = sizeof(*attributes);
	if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key) && RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key))
		return S_OK;
	read_zone_text(key, L"DisplayName", attributes->szDisplayName, ARRAYSIZE(attributes->szDisplayName));
	read_zone_text(key, L"Description", attributes->szDescription, ARRAYSIZE(attributes->szDescription));
	read_zone_text(key, L"Icon", attributes->szIconPath, ARRAYSIZE(attributes->szIconPath));
	read_zone_dword(key, L"MinLevel", &attributes->dwTemplateMinLevel);
	read_zone_dword(key, L"RecommendedLevel", &attributes->dwTemplateRecommended);
	read_zone_dword(key, L"CurrentLevel", &attributes->dwTemplateCurrentLevel);
	read_zone_dword(key, L"Flags", &attributes->dwFlags);
	RegCloseKey(key);
	return S_OK;
}

static HRESULT WINAPI zones_set_attributes(IInternetZoneManagerEx2 *iface, DWORD zone, ZONEATTRIBUTES *attributes)
{
	WCHAR path[128];
	HKEY key;
	if (!attributes)
		return E_INVALIDARG;
	if (zone > URLZONE_UNTRUSTED)
		return E_FAIL;
	wsprintfW(path, ZONES_KEY L"\\%u", zone);
	if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL))
		return E_FAIL;
	RegSetValueExW(key, L"DisplayName", 0, REG_SZ, (BYTE *)attributes->szDisplayName, (DWORD)(wcslen(attributes->szDisplayName) + 1) * sizeof(WCHAR));
	RegSetValueExW(key, L"Description", 0, REG_SZ, (BYTE *)attributes->szDescription, (DWORD)(wcslen(attributes->szDescription) + 1) * sizeof(WCHAR));
	RegSetValueExW(key, L"Icon", 0, REG_SZ, (BYTE *)attributes->szIconPath, (DWORD)(wcslen(attributes->szIconPath) + 1) * sizeof(WCHAR));
	RegSetValueExW(key, L"MinLevel", 0, REG_DWORD, (BYTE *)&attributes->dwTemplateMinLevel, sizeof(DWORD));
	RegSetValueExW(key, L"RecommendedLevel", 0, REG_DWORD, (BYTE *)&attributes->dwTemplateRecommended, sizeof(DWORD));
	RegSetValueExW(key, L"CurrentLevel", 0, REG_DWORD, (BYTE *)&attributes->dwTemplateCurrentLevel, sizeof(DWORD));
	RegSetValueExW(key, L"Flags", 0, REG_DWORD, (BYTE *)&attributes->dwFlags, sizeof(DWORD));
	RegCloseKey(key);
	return S_OK;
}

static HRESULT WINAPI zones_get_custom_policy(IInternetZoneManagerEx2 *iface, DWORD zone, REFGUID key, BYTE **policy, DWORD *size, URLZONEREG region)
{
	return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

static HRESULT WINAPI zones_set_custom_policy(IInternetZoneManagerEx2 *iface, DWORD zone, REFGUID key, BYTE *policy, DWORD size, URLZONEREG region)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI zones_get_action_policy(IInternetZoneManagerEx2 *iface, DWORD zone, DWORD action, BYTE *policy, DWORD size, URLZONEREG region)
{
	DWORD value = 0;
	if (!policy || !size)
		return E_INVALIDARG;
	if (!read_policy(zone, action, &value))
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	if (size < sizeof(DWORD))
		return E_INVALIDARG;
	*(DWORD *)policy = value;
	return S_OK;
}

static HRESULT WINAPI zones_set_action_policy(IInternetZoneManagerEx2 *iface, DWORD zone, DWORD action, BYTE *policy, DWORD size, URLZONEREG region)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI zones_prompt(IInternetZoneManagerEx2 *iface, DWORD action, HWND parent, LPCWSTR url, LPCWSTR text, DWORD flags)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI zones_log(IInternetZoneManagerEx2 *iface, DWORD action, LPCWSTR url, LPCWSTR text, DWORD flags)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI zones_create_enumerator(IInternetZoneManagerEx2 *iface, DWORD *handle, DWORD *count, DWORD flags)
{
	zone_manager *self = from_zones(iface);
	DWORD index;
	if (!handle || !count || flags)
	{
		if (handle)
			*handle = 0;
		if (count)
			*count = 0;
		return E_INVALIDARG;
	}
	for (index = 0; index < ARRAYSIZE(self->enums); index++)
		if (!self->enums[index].in_use)
		{
			self->enums[index].in_use = TRUE;
			*handle = index + 1;
			*count = URLZONE_UNTRUSTED + 1;
			return S_OK;
		}
	*handle = 0;
	*count = 0;
	return E_OUTOFMEMORY;
}

static HRESULT WINAPI zones_get_at(IInternetZoneManagerEx2 *iface, DWORD handle, DWORD index, DWORD *zone)
{
	zone_manager *self = from_zones(iface);
	if (!zone)
		return E_INVALIDARG;
	if (!handle || handle > ARRAYSIZE(self->enums) || !self->enums[handle - 1].in_use || index > URLZONE_UNTRUSTED)
	{
		*zone = URLZONE_INVALID;
		return E_INVALIDARG;
	}
	*zone = index;
	return S_OK;
}

static HRESULT WINAPI zones_destroy_enumerator(IInternetZoneManagerEx2 *iface, DWORD handle)
{
	zone_manager *self = from_zones(iface);
	if (!handle || handle > ARRAYSIZE(self->enums) || !self->enums[handle - 1].in_use)
		return E_INVALIDARG;
	self->enums[handle - 1].in_use = FALSE;
	return S_OK;
}

static HRESULT WINAPI zones_copy_template(IInternetZoneManagerEx2 *iface, DWORD level, DWORD zone, DWORD reserved)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI zones_get_action_policy_ex(IInternetZoneManagerEx2 *iface, DWORD zone, DWORD action, BYTE *policy, DWORD size, URLZONEREG region, DWORD flags)
{
	if (flags)
		return E_NOTIMPL;
	return zones_get_action_policy(iface, zone, action, policy, size, region);
}

static HRESULT WINAPI zones_set_action_policy_ex(IInternetZoneManagerEx2 *iface, DWORD zone, DWORD action, BYTE *policy, DWORD size, URLZONEREG region, DWORD flags)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI zones_get_attributes_ex(IInternetZoneManagerEx2 *iface, DWORD zone, ZONEATTRIBUTES *attributes, DWORD flags)
{
	return zones_get_attributes(iface, zone, attributes);
}

static HRESULT WINAPI zones_get_security_state(IInternetZoneManagerEx2 *iface, DWORD zone, BOOL respect_policy, DWORD *state, BOOL *encountered)
{
	if (state)
		*state = 0;
	return S_OK;
}

static HRESULT WINAPI zones_get_ie_security_state(IInternetZoneManagerEx2 *iface, BOOL respect_policy, DWORD *state, BOOL *encountered, BOOL no_cache)
{
	if (state)
		*state = 0;
	return S_OK;
}

static HRESULT WINAPI zones_fix_settings(IInternetZoneManagerEx2 *iface)
{
	return E_NOTIMPL;
}

static const IInternetZoneManagerEx2Vtbl zones_vtable = {
	zones_query_interface, zones_add_ref, zones_release, zones_get_attributes, zones_set_attributes, zones_get_custom_policy, zones_set_custom_policy, zones_get_action_policy, zones_set_action_policy,
	zones_prompt, zones_log, zones_create_enumerator, zones_get_at, zones_destroy_enumerator, zones_copy_template, zones_get_action_policy_ex, zones_set_action_policy_ex, zones_get_attributes_ex, zones_get_security_state,
	zones_get_ie_security_state, zones_fix_settings,
};

HRESULT urlmon_create_zone_manager(IUnknown *outer, REFIID iid, void **object)
{
	zone_manager *self;
	HRESULT result;
	*object = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	self = uri_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->IInternetZoneManagerEx2_iface.lpVtbl = &zones_vtable;
	self->references = 1;
	urlmon_lock_module();
	result = zones_query_interface(&self->IInternetZoneManagerEx2_iface, iid, object);
	IInternetZoneManagerEx2_Release(&self->IInternetZoneManagerEx2_iface);
	return result;
}

HRESULT WINAPI CoInternetCreateZoneManager(IServiceProvider *provider, IInternetZoneManager **out, DWORD reserved)
{
	return urlmon_create_zone_manager(NULL, &IID_IInternetZoneManager, (void **)out);
}
