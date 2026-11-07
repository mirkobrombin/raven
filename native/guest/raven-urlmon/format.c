/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "urlmon_private.h"

typedef struct
{
	IEnumFORMATETC IEnumFORMATETC_iface;
	LONG references;
	UINT count;
	UINT next;
	FORMATETC formats[];
} format_list;

static const IEnumFORMATETCVtbl format_list_vtable;

static inline format_list *from_enum(IEnumFORMATETC *iface)
{
	return CONTAINING_RECORD(iface, format_list, IEnumFORMATETC_iface);
}

static format_list *format_list_new(UINT count, const FORMATETC *formats, UINT next)
{
	format_list *self = uri_alloc(sizeof(*self) + count * sizeof(FORMATETC));
	if (self)
	{
		self->IEnumFORMATETC_iface.lpVtbl = &format_list_vtable;
		self->references = 1;
		self->count = count;
		self->next = next;
		memcpy(self->formats, formats, count * sizeof(FORMATETC));
		urlmon_lock_module();
	}
	return self;
}

static HRESULT WINAPI format_query_interface(IEnumFORMATETC *iface, REFIID iid, void **object)
{
	if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IEnumFORMATETC))
	{
		*object = iface;
		IEnumFORMATETC_AddRef(iface);
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI format_add_ref(IEnumFORMATETC *iface)
{
	return InterlockedIncrement(&from_enum(iface)->references);
}

static ULONG WINAPI format_release(IEnumFORMATETC *iface)
{
	format_list *self = from_enum(iface);
	ULONG left = InterlockedDecrement(&self->references);
	if (!left)
	{
		uri_free(self);
		urlmon_unlock_module();
	}
	return left;
}

static HRESULT WINAPI format_next(IEnumFORMATETC *iface, ULONG wanted, FORMATETC *out, ULONG *fetched)
{
	format_list *self = from_enum(iface);
	ULONG served = 0;
	if (!out)
		return E_INVALIDARG;
	while (served < wanted && self->next < self->count)
		out[served++] = self->formats[self->next++];
	if (fetched)
		*fetched = served;
	return served == wanted ? S_OK : S_FALSE;
}

static HRESULT WINAPI format_skip(IEnumFORMATETC *iface, ULONG count)
{
	format_list *self = from_enum(iface);
	UINT left = self->count - self->next;
	if (count > left)
	{
		self->next = self->count;
		return S_FALSE;
	}
	self->next += count;
	return S_OK;
}

static HRESULT WINAPI format_reset(IEnumFORMATETC *iface)
{
	from_enum(iface)->next = 0;
	return S_OK;
}

static HRESULT WINAPI format_clone(IEnumFORMATETC *iface, IEnumFORMATETC **out)
{
	format_list *self = from_enum(iface), *copy;
	if (!out)
		return E_INVALIDARG;
	copy = format_list_new(self->count, self->formats, self->next);
	if (!copy)
		return E_OUTOFMEMORY;
	*out = &copy->IEnumFORMATETC_iface;
	return S_OK;
}

static const IEnumFORMATETCVtbl format_list_vtable = {
	format_query_interface, format_add_ref, format_release, format_next, format_skip, format_reset, format_clone,
};

HRESULT WINAPI CreateFormatEnumerator(UINT count, FORMATETC *formats, IEnumFORMATETC **out)
{
	format_list *self;
	if (!out)
		return E_INVALIDARG;
	if (!count || !formats)
		return E_FAIL;
	self = format_list_new(count, formats, 0);
	if (!self)
		return E_OUTOFMEMORY;
	*out = &self->IEnumFORMATETC_iface;
	return S_OK;
}

static WCHAR format_param_name[] = L"_EnumFORMATETC_";

HRESULT WINAPI RegisterFormatEnumerator(LPBC context, IEnumFORMATETC *formats, DWORD reserved)
{
	if (!context || !formats)
		return E_INVALIDARG;
	return IBindCtx_RegisterObjectParam(context, format_param_name, (IUnknown *)formats);
}

HRESULT WINAPI RevokeFormatEnumerator(LPBC context, IEnumFORMATETC *formats)
{
	if (!context)
		return E_INVALIDARG;
	return IBindCtx_RevokeObjectParam(context, format_param_name);
}

void WINAPI ReleaseBindInfo(BINDINFO *info)
{
	DWORD size;
	if (!info || !info->cbSize)
		return;
	size = info->cbSize;
	CoTaskMemFree(info->szExtraInfo);
	ReleaseStgMedium(&info->stgmedData);
	if (size >= FIELD_OFFSET(BINDINFO, dwReserved))
	{
		CoTaskMemFree(info->szCustomVerb);
		info->szCustomVerb = NULL;
	}
	if (size >= FIELD_OFFSET(BINDINFO, pUnk) + sizeof(info->pUnk) && info->pUnk)
		IUnknown_Release(info->pUnk);
	memset(info, 0, size);
	info->cbSize = size;
}

HRESULT WINAPI CopyStgMedium(const STGMEDIUM *source, STGMEDIUM *target)
{
	if (!source || !target)
		return E_POINTER;
	*target = *source;
	switch (source->tymed)
	{
	case TYMED_NULL:
		break;
	case TYMED_FILE:
		if (source->lpszFileName)
		{
			SIZE_T size = (wcslen(source->lpszFileName) + 1) * sizeof(WCHAR);
			target->lpszFileName = CoTaskMemAlloc(size);
			if (!target->lpszFileName)
				return E_OUTOFMEMORY;
			memcpy(target->lpszFileName, source->lpszFileName, size);
		}
		break;
	case TYMED_ISTREAM:
		if (target->pstm)
			IStream_AddRef(target->pstm);
		break;
	case TYMED_ISTORAGE:
		if (target->pstg)
			IStorage_AddRef(target->pstg);
		break;
	case TYMED_HGLOBAL:
		if (source->hGlobal)
		{
			SIZE_T size = GlobalSize(source->hGlobal);
			void *from, *to;
			target->hGlobal = GlobalAlloc(GMEM_FIXED, size);
			if (!target->hGlobal)
				return E_OUTOFMEMORY;
			from = GlobalLock(source->hGlobal);
			to = GlobalLock(target->hGlobal);
			if (to && from)
				memcpy(to, from, size);
			if (to)
				GlobalUnlock(target->hGlobal);
			if (from)
				GlobalUnlock(source->hGlobal);
		}
		break;
	default:
		return E_NOTIMPL;
	}
	if (target->pUnkForRelease)
		IUnknown_AddRef(target->pUnkForRelease);
	return S_OK;
}

static WCHAR *task_copy(const WCHAR *text)
{
	SIZE_T size = (wcslen(text) + 1) * sizeof(WCHAR);
	WCHAR *copy = CoTaskMemAlloc(size);
	if (copy)
		memcpy(copy, text, size);
	return copy;
}

/* Only as much as both structures know about moves; the rest of the target ends up zero. */
HRESULT WINAPI CopyBindInfo(const BINDINFO *source, BINDINFO *target)
{
	DWORD size;
	HRESULT result;
	if (!source || !target)
		return E_POINTER;
	if (!source->cbSize || !target->cbSize)
		return E_INVALIDARG;
	size = target->cbSize;
	memset(target, 0, size);
	target->cbSize = size;
	memcpy(((BYTE *)target) + sizeof(DWORD), ((const BYTE *)source) + sizeof(DWORD), min(source->cbSize, size) - sizeof(DWORD) < 0x7fffffff && min(source->cbSize, size) > sizeof(DWORD) ? min(source->cbSize, size) - sizeof(DWORD) : 0);
	if (source->cbSize <= FIELD_OFFSET(BINDINFO, szExtraInfo) || size <= FIELD_OFFSET(BINDINFO, szExtraInfo))
		return S_OK;
	if (source->szExtraInfo)
	{
		target->szExtraInfo = task_copy(source->szExtraInfo);
		if (!target->szExtraInfo)
			return E_OUTOFMEMORY;
	}
	if (min(source->cbSize, size) > FIELD_OFFSET(BINDINFO, stgmedData))
	{
		result = CopyStgMedium(&source->stgmedData, &target->stgmedData);
		if (FAILED(result))
		{
			CoTaskMemFree(target->szExtraInfo);
			target->szExtraInfo = NULL;
			return result;
		}
	}
	if (min(source->cbSize, size) > FIELD_OFFSET(BINDINFO, szCustomVerb) && source->szCustomVerb)
	{
		target->szCustomVerb = task_copy(source->szCustomVerb);
		if (!target->szCustomVerb)
			return E_OUTOFMEMORY;
	}
	if (min(source->cbSize, size) > FIELD_OFFSET(BINDINFO, pUnk) && source->pUnk)
		IUnknown_AddRef(source->pUnk);
	if (min(source->cbSize, size) > FIELD_OFFSET(BINDINFO, securityAttributes) + sizeof(DWORD) && target->securityAttributes.lpSecurityDescriptor)
		target->securityAttributes.lpSecurityDescriptor = NULL;
	return S_OK;
}
