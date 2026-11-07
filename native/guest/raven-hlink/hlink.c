/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven hyperlink component: the flat functions of hlink.dll on the public header.
 *
 * Shortcut names, the special references a process sets and URL translation work. Hyperlink and browse context objects are not
 * provided: the functions that would make or navigate one answer E_NOTIMPL, which callers report as a link they cannot follow. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <urlmon.h>
#include <hlink.h>

#define SPECIAL_REFERENCES 8

static WCHAR *special[SPECIAL_REFERENCES];
static SRWLOCK special_lock = SRWLOCK_INIT;

static WCHAR *copy_text(const WCHAR *text)
{
	const SIZE_T size = (lstrlenW(text) + 1) * sizeof(WCHAR);
	WCHAR *copy = CoTaskMemAlloc(size);
	if (copy)
		CopyMemory(copy, text, size);
	return copy;
}

HRESULT WINAPI HlinkCreateFromMoniker(IMoniker *target, LPCWSTR location, LPCWSTR name, IHlinkSite *site, DWORD data, IUnknown *outer, REFIID iid, void **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

HRESULT WINAPI HlinkCreateFromString(LPCWSTR target, LPCWSTR location, LPCWSTR name, IHlinkSite *site, DWORD data, IUnknown *outer, REFIID iid, void **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

HRESULT WINAPI HlinkCreateFromData(IDataObject *source, IHlinkSite *site, DWORD data, IUnknown *outer, REFIID iid, void **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

HRESULT WINAPI HlinkCreateBrowseContext(IUnknown *outer, REFIID iid, void **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

HRESULT WINAPI HlinkClone(IHlink *link, REFIID iid, IHlinkSite *site, DWORD data, void **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

HRESULT WINAPI HlinkNavigate(IHlink *link, IHlinkFrame *frame, DWORD flags, LPBC context, IBindStatusCallback *callback, IHlinkBrowseContext *browse) { return E_NOTIMPL; }

HRESULT WINAPI HlinkOnNavigate(IHlinkFrame *frame, IHlinkBrowseContext *browse, DWORD flags, IMoniker *target, LPCWSTR location, LPCWSTR name, ULONG *id) { return E_NOTIMPL; }

HRESULT WINAPI HlinkUpdateStackItem(IHlinkFrame *frame, IHlinkBrowseContext *browse, ULONG id, IMoniker *target, LPCWSTR location, LPCWSTR name) { return E_NOTIMPL; }

HRESULT WINAPI HlinkParseDisplayName(LPBC context, LPCWSTR name, WINBOOL no_force, ULONG *eaten, IMoniker **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

HRESULT WINAPI HlinkSetSpecialReference(ULONG reference, LPCWSTR text)
{
	WCHAR *copy, *old;
	if (reference >= SPECIAL_REFERENCES || !text)
		return E_INVALIDARG;
	if (!(copy = copy_text(text)))
		return E_OUTOFMEMORY;
	AcquireSRWLockExclusive(&special_lock);
	old = special[reference];
	special[reference] = copy;
	ReleaseSRWLockExclusive(&special_lock);
	CoTaskMemFree(old);
	return S_OK;
}

/* What a process never set is the empty page a browser starts from. */
HRESULT WINAPI HlinkGetSpecialReference(ULONG reference, LPWSTR *out)
{
	HRESULT result = S_OK;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (reference >= SPECIAL_REFERENCES)
		return E_INVALIDARG;
	AcquireSRWLockShared(&special_lock);
	*out = copy_text(special[reference] ? special[reference] : L"about:blank");
	ReleaseSRWLockShared(&special_lock);
	if (!*out)
		result = E_OUTOFMEMORY;
	return result;
}

HRESULT WINAPI HlinkIsShortcut(LPCWSTR name)
{
	int length;
	if (!name)
		return E_INVALIDARG;
	length = lstrlenW(name);
	return length >= 4 && !lstrcmpiW(name + length - 4, L".url") ? S_OK : S_FALSE;
}

HRESULT WINAPI HlinkCreateShortcut(DWORD flags, IHlink *link, LPCWSTR directory, LPCWSTR file, LPWSTR *out, DWORD reserved)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

HRESULT WINAPI HlinkCreateShortcutFromString(DWORD flags, LPCWSTR target, LPCWSTR location, LPCWSTR directory, LPCWSTR file, LPWSTR *out, DWORD reserved)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

HRESULT WINAPI HlinkCreateShortcutFromMoniker(DWORD flags, IMoniker *target, LPCWSTR location, LPCWSTR directory, LPCWSTR file, LPWSTR *out, DWORD reserved)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

HRESULT WINAPI HlinkResolveShortcutToString(LPCWSTR file, LPWSTR *target, LPWSTR *location)
{
	if (target)
		*target = NULL;
	if (location)
		*location = NULL;
	return E_NOTIMPL;
}

/* An address is already what it names: nothing here rewrites one, which S_FALSE reports. */
HRESULT WINAPI HlinkTranslateURL(LPCWSTR url, DWORD flags, LPWSTR *out)
{
	if (out)
		*out = NULL;
	return url && out ? S_FALSE : E_INVALIDARG;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved) { return TRUE; }
