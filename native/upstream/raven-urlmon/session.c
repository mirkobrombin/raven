/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "urlmon_private.h"

/* One RegisterNameSpace or RegisterMimeFilter call. The newest wins, so entries go first. */
typedef struct registration
{
	struct registration *next;
	IClassFactory *factory;
	CLSID clsid;
	WCHAR name[];
} registration;

static CRITICAL_SECTION session_lock;
static registration *name_spaces;
static registration *mime_filters;
static char *user_agent;

static HRESULT list_add(registration **list, IClassFactory *factory, REFCLSID clsid, const WCHAR *name)
{
	SIZE_T length = wcslen(name) + 1;
	registration *entry = uri_alloc(sizeof(*entry) + length * sizeof(WCHAR));
	if (!entry)
		return E_OUTOFMEMORY;
	IClassFactory_AddRef(factory);
	entry->factory = factory;
	entry->clsid = *clsid;
	lstrcpyW(entry->name, name);
	EnterCriticalSection(&session_lock);
	entry->next = *list;
	*list = entry;
	LeaveCriticalSection(&session_lock);
	return S_OK;
}

/* Taking away something never registered is not an error to the caller. */
static HRESULT list_remove(registration **list, IClassFactory *factory, const WCHAR *name)
{
	registration **link, *found = NULL;
	EnterCriticalSection(&session_lock);
	for (link = list; *link; link = &(*link)->next)
		if ((*link)->factory == factory && !lstrcmpiW((*link)->name, name))
		{
			found = *link;
			*link = found->next;
			break;
		}
	LeaveCriticalSection(&session_lock);
	if (found)
	{
		IClassFactory_Release(found->factory);
		uri_free(found);
	}
	return S_OK;
}

static IClassFactory *list_find(registration *const *list, const WCHAR *name, CLSID *clsid)
{
	IClassFactory *factory = NULL;
	const registration *entry;
	EnterCriticalSection(&session_lock);
	for (entry = *list; entry; entry = entry->next)
		if (!lstrcmpiW(entry->name, name))
		{
			factory = entry->factory;
			IClassFactory_AddRef(factory);
			if (clsid)
				*clsid = entry->clsid;
			break;
		}
	LeaveCriticalSection(&session_lock);
	return factory;
}

/* The scheme of a URL, as a name to look up; empty if it has none. */
static BOOL scheme_of_url(const WCHAR *url, WCHAR *scheme, DWORD size)
{
	const WCHAR *colon;
	if (!url)
		return FALSE;
	colon = wcschr(url, L':');
	if (!colon || colon == url || (DWORD)(colon - url) >= size)
		return FALSE;
	memcpy(scheme, url, (colon - url) * sizeof(WCHAR));
	scheme[colon - url] = 0;
	return TRUE;
}

/* HKCR\PROTOCOLS\Handler\<scheme> names the CLSID of a handler some program registered. */
static HRESULT handler_clsid(const WCHAR *scheme, CLSID *clsid)
{
	WCHAR key_name[96], value[64];
	DWORD size = sizeof(value), type = 0;
	HKEY key;
	LONG status;
	if (wcslen(scheme) > 64)
		return MK_E_SYNTAX;
	lstrcpyW(key_name, L"PROTOCOLS\\Handler\\");
	lstrcatW(key_name, scheme);
	if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key_name, 0, KEY_READ, &key))
		return MK_E_SYNTAX;
	status = RegQueryValueExW(key, L"CLSID", NULL, &type, (BYTE *)value, &size);
	RegCloseKey(key);
	if (status || type != REG_SZ)
		return MK_E_SYNTAX;
	return CLSIDFromString(value, clsid);
}

HRESULT urlmon_protocol_factory(const WCHAR *url, BOOL *from_session, IClassFactory **factory, CLSID *clsid)
{
	WCHAR scheme[65];
	CLSID found;
	HRESULT result;
	*factory = NULL;
	if (from_session)
		*from_session = FALSE;
	if (!scheme_of_url(url, scheme, ARRAYSIZE(scheme)))
		return MK_E_SYNTAX;
	*factory = list_find(&name_spaces, scheme, clsid);
	if (*factory)
	{
		if (from_session)
			*from_session = TRUE;
		return S_OK;
	}
	if (urlmon_builtin_protocol(scheme, factory, clsid))
		return S_OK;
	result = handler_clsid(scheme, &found);
	if (FAILED(result))
		return result;
	if (clsid)
		*clsid = found;
	return CoGetClassObject(&found, CLSCTX_INPROC_SERVER, NULL, &IID_IClassFactory, (void **)factory);
}

static IInternetProtocolInfo *info_of_url(const WCHAR *url)
{
	IInternetProtocolInfo *info = NULL;
	IClassFactory *factory = NULL;
	IUnknown *created;
	WCHAR scheme[65];
	BOOL from_session = FALSE;
	/* What is built in here has no opinion beyond the defaults. */
	if (FAILED(urlmon_protocol_factory(url, &from_session, &factory, NULL)))
		return NULL;
	if (!from_session && scheme_of_url(url, scheme, ARRAYSIZE(scheme)))
	{
		IClassFactory *builtin = NULL;
		if (urlmon_builtin_protocol(scheme, &builtin, NULL))
		{
			IClassFactory_Release(builtin);
			IClassFactory_Release(factory);
			return NULL;
		}
	}
	/* A factory may serve the interface itself; otherwise an instance does. */
	if (FAILED(IClassFactory_QueryInterface(factory, &IID_IInternetProtocolInfo, (void **)&info)))
	{
		info = NULL;
		if (SUCCEEDED(IClassFactory_CreateInstance(factory, NULL, &IID_IUnknown, (void **)&created)))
		{
			if (FAILED(IUnknown_QueryInterface(created, &IID_IInternetProtocolInfo, (void **)&info)))
				info = NULL;
			IUnknown_Release(created);
		}
	}
	IClassFactory_Release(factory);
	return info;
}

IInternetProtocolInfo *urlmon_url_protocol_info(const WCHAR *url)
{
	return info_of_url(url);
}

IInternetProtocolInfo *urlmon_protocol_info(const WCHAR *scheme)
{
	WCHAR url[80];
	if (!scheme || wcslen(scheme) > 64)
		return NULL;
	lstrcpyW(url, scheme);
	lstrcatW(url, L":");
	return info_of_url(url);
}

IClassFactory *urlmon_mime_filter(const WCHAR *mime, CLSID *clsid)
{
	IClassFactory *factory = list_find(&mime_filters, mime, clsid);
	WCHAR key_name[256], value[64];
	DWORD size = sizeof(value), type = 0;
	CLSID found;
	HKEY key;
	LONG status;
	if (factory || !mime || wcslen(mime) > 200)
		return factory;
	lstrcpyW(key_name, L"PROTOCOLS\\Filter\\");
	lstrcatW(key_name, mime);
	if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key_name, 0, KEY_READ, &key))
		return NULL;
	status = RegQueryValueExW(key, L"CLSID", NULL, &type, (BYTE *)value, &size);
	RegCloseKey(key);
	if (status || type != REG_SZ || FAILED(CLSIDFromString(value, &found)))
		return NULL;
	if (FAILED(CoGetClassObject(&found, CLSCTX_INPROC_SERVER, NULL, &IID_IClassFactory, (void **)&factory)))
		return NULL;
	if (clsid)
		*clsid = found;
	return factory;
}

static HRESULT WINAPI session_query_interface(IInternetSession *iface, REFIID iid, void **object)
{
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInternetSession))
	{
		*object = iface;
		IInternetSession_AddRef(iface);
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI session_add_ref(IInternetSession *iface)
{
	urlmon_lock_module();
	return 2;
}

static ULONG WINAPI session_release(IInternetSession *iface)
{
	urlmon_unlock_module();
	return 1;
}

static HRESULT WINAPI session_register_name_space(IInternetSession *iface, IClassFactory *factory, REFCLSID clsid, LPCWSTR scheme, DWORD count, const LPCWSTR *patterns, DWORD reserved)
{
	if (!factory || !scheme)
		return E_INVALIDARG;
	return list_add(&name_spaces, factory, clsid, scheme);
}

static HRESULT WINAPI session_unregister_name_space(IInternetSession *iface, IClassFactory *factory, LPCWSTR scheme)
{
	if (!factory || !scheme)
		return E_INVALIDARG;
	return list_remove(&name_spaces, factory, scheme);
}

static HRESULT WINAPI session_register_mime_filter(IInternetSession *iface, IClassFactory *factory, REFCLSID clsid, LPCWSTR type)
{
	if (!factory || !type)
		return E_INVALIDARG;
	return list_add(&mime_filters, factory, clsid, type);
}

static HRESULT WINAPI session_unregister_mime_filter(IInternetSession *iface, IClassFactory *factory, LPCWSTR type)
{
	if (!factory || !type)
		return E_INVALIDARG;
	return list_remove(&mime_filters, factory, type);
}

static HRESULT WINAPI session_create_binding(IInternetSession *iface, LPBC context, LPCWSTR url, IUnknown *outer, IUnknown **unknown, IInternetProtocol **protocol, DWORD options)
{
	if (!protocol)
		return E_POINTER;
	return urlmon_create_bind_protocol(protocol);
}

static HRESULT WINAPI session_set_session_option(IInternetSession *iface, DWORD option, LPVOID buffer, DWORD length, DWORD reserved)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI session_get_session_option(IInternetSession *iface, DWORD option, LPVOID buffer, DWORD *length, DWORD reserved)
{
	return E_NOTIMPL;
}

static const IInternetSessionVtbl session_vtable = {
	session_query_interface, session_add_ref, session_release, session_register_name_space, session_unregister_name_space,
	session_register_mime_filter, session_unregister_mime_filter, session_create_binding, session_set_session_option, session_get_session_option,
};

static IInternetSession the_session = {&session_vtable};

HRESULT WINAPI CoInternetGetSession(DWORD mode, IInternetSession **session, DWORD reserved)
{
	if (mode)
		return E_INVALIDARG;
	if (!session)
		return E_POINTER;
	*session = &the_session;
	IInternetSession_AddRef(*session);
	return S_OK;
}

/* What the browser engine of this generation calls itself; Windows version from the process. */
static char *build_user_agent(void)
{
	static const char prefix[] = "Mozilla/4.0 (compatible; MSIE 8.0; Windows NT ";
	OSVERSIONINFOW version = {sizeof(version)};
	SYSTEM_INFO info;
	char *out = uri_alloc(sizeof(prefix) + 96);
	char *at;
	DWORD numbers[2], index;
	BOOL wow = FALSE;
	if (!out)
		return NULL;
	GetVersionExW(&version);
	numbers[0] = version.dwMajorVersion;
	numbers[1] = version.dwMinorVersion;
	lstrcpyA(out, prefix);
	at = out + lstrlenA(out);
	for (index = 0; index < 2; index++)
	{
		char digits[12];
		int used = 0;
		DWORD left = numbers[index];
		if (index)
			*at++ = '.';
		do
		{
			digits[used++] = (char)('0' + left % 10);
			left /= 10;
		}
		while (left);
		while (used)
			*at++ = digits[--used];
	}
	*at = 0;
	GetNativeSystemInfo(&info);
	if (sizeof(void *) == 8)
		lstrcatA(at, info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "; Win64; x64" : "; Win64");
	else if (IsWow64Process(GetCurrentProcess(), &wow) && wow)
		lstrcatA(at, "; WOW64");
	lstrcatA(at, ")");
	return out;
}

static const char *current_user_agent(void)
{
	if (!user_agent)
		user_agent = build_user_agent();
	return user_agent;
}

WCHAR *urlmon_user_agent(void)
{
	const char *agent;
	WCHAR *wide = NULL;
	int length;
	EnterCriticalSection(&session_lock);
	agent = current_user_agent();
	if (agent)
	{
		length = MultiByteToWideChar(CP_ACP, 0, agent, -1, NULL, 0);
		wide = uri_alloc(length * sizeof(WCHAR));
		if (wide)
			MultiByteToWideChar(CP_ACP, 0, agent, -1, wide, length);
	}
	LeaveCriticalSection(&session_lock);
	return wide;
}

HRESULT WINAPI ObtainUserAgentString(DWORD option, LPSTR out, DWORD *size)
{
	const char *agent;
	DWORD needed;
	HRESULT result = S_OK;
	if (!size || (!out && *size))
		return E_INVALIDARG;
	if (!out)
		return E_INVALIDARG;
	EnterCriticalSection(&session_lock);
	agent = current_user_agent();
	if (!agent)
		result = E_OUTOFMEMORY;
	else
	{
		needed = lstrlenA(agent) + 1;
		if (*size >= needed)
			lstrcpyA(out, agent);
		else
			result = E_OUTOFMEMORY;
		*size = needed;
	}
	LeaveCriticalSection(&session_lock);
	return result;
}

HRESULT WINAPI UrlMkGetSessionOption(DWORD option, LPVOID buffer, DWORD length, DWORD *used, DWORD reserved)
{
	const char *agent;
	HRESULT result = S_OK;
	DWORD needed;
	switch (option)
	{
	case URLMON_OPTION_USERAGENT:
		if (!used)
			return E_INVALIDARG;
		EnterCriticalSection(&session_lock);
		agent = current_user_agent();
		if (!agent)
			result = E_OUTOFMEMORY;
		else
		{
			needed = lstrlenA(agent) + 1;
			*used = needed;
			if (!buffer || length < needed)
				result = E_OUTOFMEMORY;
			else
				lstrcpyA(buffer, agent);
		}
		LeaveCriticalSection(&session_lock);
		return result;
	case URLMON_OPTION_URL_ENCODING:
		if (!buffer || length < sizeof(DWORD) || !used)
			return E_INVALIDARG;
		*(DWORD *)buffer = 0;
		*used = sizeof(DWORD);
		return S_OK;
	default:
		return E_INVALIDARG;
	}
}

HRESULT WINAPI UrlMkSetSessionOption(DWORD option, LPVOID buffer, DWORD length, DWORD reserved)
{
	char *copy;
	if (option != URLMON_OPTION_USERAGENT)
		return E_INVALIDARG;
	if (!buffer || !length)
		return E_INVALIDARG;
	copy = uri_alloc(length + 1);
	if (!copy)
		return E_OUTOFMEMORY;
	/* The length decides and the text may stop earlier: the first zero ends it. */
	memcpy(copy, buffer, length);
	copy[length] = 0;
	EnterCriticalSection(&session_lock);
	uri_free(user_agent);
	user_agent = copy;
	LeaveCriticalSection(&session_lock);
	return S_OK;
}

void urlmon_session_startup(void)
{
	InitializeCriticalSection(&session_lock);
}

void urlmon_session_shutdown(void)
{
	uri_free(user_agent);
	user_agent = NULL;
	DeleteCriticalSection(&session_lock);
}
