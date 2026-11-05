/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <objidl.h>

/* The data object OleGetClipboard hands out: a view of the clipboard as it is when the object is asked, so every call opens the
 * clipboard, takes what it holds in the format asked for and closes it again. Data is given as global memory the caller owns. */

static BOOL same_iid(REFIID left, REFIID right)
{
	return !memcmp(left, right, sizeof(IID));
}

/* --- The formats on the clipboard, listed when the enumerator is made. --- */

typedef struct
{
	IEnumFORMATETC iface;
	LONG refs;
	ULONG count, next;
	FORMATETC formats[];
} format_list;

static const IEnumFORMATETCVtbl format_vtbl;

static format_list *new_format_list(const FORMATETC *formats, ULONG count, ULONG next)
{
	format_list *self = HeapAlloc(GetProcessHeap(), 0, sizeof(*self) + count * sizeof(FORMATETC));
	if (!self)
		return NULL;
	self->iface.lpVtbl = &format_vtbl;
	self->refs = 1;
	self->count = count;
	self->next = next;
	if (count)
		memcpy(self->formats, formats, count * sizeof(FORMATETC));
	return self;
}

static HRESULT WINAPI formats_QueryInterface(IEnumFORMATETC *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_iid(iid, &IID_IUnknown) || same_iid(iid, &IID_IEnumFORMATETC))
	{
		*out = iface;
		IEnumFORMATETC_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI formats_AddRef(IEnumFORMATETC *iface)
{
	return InterlockedIncrement(&((format_list *)iface)->refs);
}

static ULONG WINAPI formats_Release(IEnumFORMATETC *iface)
{
	const LONG refs = InterlockedDecrement(&((format_list *)iface)->refs);
	if (!refs)
		HeapFree(GetProcessHeap(), 0, iface);
	return refs;
}

static HRESULT WINAPI formats_Next(IEnumFORMATETC *iface, ULONG wanted, FORMATETC *out, ULONG *fetched)
{
	format_list *self = (format_list *)iface;
	ULONG given = 0;
	if (!out || (wanted > 1 && !fetched))
		return E_INVALIDARG;
	while (given < wanted && self->next < self->count)
		out[given++] = self->formats[self->next++];
	if (fetched)
		*fetched = given;
	return given == wanted ? S_OK : S_FALSE;
}

static HRESULT WINAPI formats_Skip(IEnumFORMATETC *iface, ULONG count)
{
	format_list *self = (format_list *)iface;
	const ULONG left = self->count - self->next;
	self->next += count < left ? count : left;
	return count <= left ? S_OK : S_FALSE;
}

static HRESULT WINAPI formats_Reset(IEnumFORMATETC *iface)
{
	((format_list *)iface)->next = 0;
	return S_OK;
}

static HRESULT WINAPI formats_Clone(IEnumFORMATETC *iface, IEnumFORMATETC **out)
{
	format_list *self = (format_list *)iface, *copy;
	if (!out)
		return E_POINTER;
	copy = new_format_list(self->formats, self->count, self->next);
	*out = copy ? &copy->iface : NULL;
	return copy ? S_OK : E_OUTOFMEMORY;
}

static const IEnumFORMATETCVtbl format_vtbl = {formats_QueryInterface, formats_AddRef, formats_Release, formats_Next, formats_Skip,
	formats_Reset, formats_Clone};

/* --- The data object. --- */

#define CLIPBOARD_CANT_OPEN ((HRESULT)0x800401d0)

static HRESULT WINAPI data_QueryInterface(IDataObject *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_iid(iid, &IID_IUnknown) || same_iid(iid, &IID_IDataObject))
	{
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

/* One object serves every caller, as the clipboard is one. */
static ULONG WINAPI data_AddRef(IDataObject *iface)
{
	return 2;
}

static ULONG WINAPI data_Release(IDataObject *iface)
{
	return 1;
}

static HRESULT check_format(const FORMATETC *format)
{
	if (!format)
		return E_INVALIDARG;
	if (format->lindex != -1)
		return DV_E_LINDEX;
	if (!(format->tymed & TYMED_HGLOBAL))
		return DV_E_TYMED;
	if (format->dwAspect != DVASPECT_CONTENT)
		return DV_E_DVASPECT;
	return IsClipboardFormatAvailable(format->cfFormat) ? S_OK : DV_E_FORMATETC;
}

static HRESULT WINAPI data_GetData(IDataObject *iface, FORMATETC *format, STGMEDIUM *medium)
{
	HRESULT result;
	HANDLE source;
	HGLOBAL copy = NULL;
	if (!medium)
		return E_INVALIDARG;
	memset(medium, 0, sizeof(*medium));
	result = check_format(format);
	if (FAILED(result))
		return result;
	if (!OpenClipboard(NULL))
		return CLIPBOARD_CANT_OPEN;
	source = GetClipboardData(format->cfFormat);
	if (source)
	{
		const SIZE_T size = GlobalSize(source);
		const void *bytes = GlobalLock(source);
		copy = bytes ? GlobalAlloc(GMEM_MOVEABLE, size) : NULL;
		if (copy)
		{
			memcpy(GlobalLock(copy), bytes, size);
			GlobalUnlock(copy);
		}
		if (bytes)
			GlobalUnlock(source);
	}
	CloseClipboard();
	if (!copy)
		return source ? E_OUTOFMEMORY : DV_E_FORMATETC;
	medium->tymed = TYMED_HGLOBAL;
	medium->hGlobal = copy;
	return S_OK;
}

static HRESULT WINAPI data_GetDataHere(IDataObject *iface, FORMATETC *format, STGMEDIUM *medium)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI data_QueryGetData(IDataObject *iface, FORMATETC *format)
{
	return check_format(format);
}

static HRESULT WINAPI data_GetCanonicalFormatEtc(IDataObject *iface, FORMATETC *format, FORMATETC *out)
{
	if (!format || !out)
		return E_INVALIDARG;
	*out = *format;
	out->ptd = NULL;
	return DATA_S_SAMEFORMATETC;
}

static HRESULT WINAPI data_SetData(IDataObject *iface, FORMATETC *format, STGMEDIUM *medium, BOOL release)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI data_EnumFormatEtc(IDataObject *iface, DWORD direction, IEnumFORMATETC **out)
{
	FORMATETC formats[64];
	ULONG count = 0;
	UINT format = 0;
	format_list *list;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (direction != DATADIR_GET)
		return E_NOTIMPL;
	if (!OpenClipboard(NULL))
		return CLIPBOARD_CANT_OPEN;
	while (count < ARRAYSIZE(formats) && (format = EnumClipboardFormats(format)))
	{
		FORMATETC *entry = &formats[count++];
		entry->cfFormat = (CLIPFORMAT)format;
		entry->ptd = NULL;
		entry->dwAspect = DVASPECT_CONTENT;
		entry->lindex = -1;
		entry->tymed = TYMED_HGLOBAL;
	}
	CloseClipboard();
	list = new_format_list(formats, count, 0);
	if (!list)
		return E_OUTOFMEMORY;
	*out = &list->iface;
	return S_OK;
}

static HRESULT WINAPI data_DAdvise(IDataObject *iface, FORMATETC *format, DWORD flags, IAdviseSink *sink, DWORD *connection)
{
	return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT WINAPI data_DUnadvise(IDataObject *iface, DWORD connection)
{
	return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT WINAPI data_EnumDAdvise(IDataObject *iface, IEnumSTATDATA **out)
{
	return OLE_E_ADVISENOTSUPPORTED;
}

static const IDataObjectVtbl data_vtbl = {data_QueryInterface, data_AddRef, data_Release, data_GetData, data_GetDataHere,
	data_QueryGetData, data_GetCanonicalFormatEtc, data_SetData, data_EnumFormatEtc, data_DAdvise, data_DUnadvise, data_EnumDAdvise};
static IDataObject clipboard_object = {(IDataObjectVtbl *)&data_vtbl};

HRESULT WINAPI OleGetClipboard(IDataObject **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = &clipboard_object;
	return S_OK;
}
