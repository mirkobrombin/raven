/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * The standard font object: a description, plus a GDI font realised on demand and shared
 * between objects that would realise the same one.
 */
#include "connpt.h"

/* Size unit of the description: ten-thousandths of a point. */
#define POINTS_UNIT 10000

struct shared_font
{
	struct shared_font *next;
	HFONT handle;
	LONG height;
	FONTDESC desc;
	WCHAR name[LF_FACESIZE];
	/* Objects using it, and every reference including those. The second may go negative: an
	 * object can outlive the references a caller released on its behalf. */
	LONG users, total;
};

static CRITICAL_SECTION font_lock;
static struct shared_font *font_head;
static LONG live_fonts;

void oa_font_startup(void)
{
	InitializeCriticalSection(&font_lock);
}

struct font
{
	IFont IFont_iface;
	IDispatch IDispatch_iface;
	IPersistStream IPersistStream_iface;
	IConnectionPointContainer IConnectionPointContainer_iface;
	IPersistPropertyBag IPersistPropertyBag_iface;
	LONG ref;
	FONTDESC desc;
	BSTR name;
	struct shared_font *shared;
	BOOL stale;
	LONG logical, himetric;
	HDC dc;
	IConnectionPoint *notify, *events;
};

static const IFontVtbl font_vtbl;
static const IDispatchVtbl font_dispatch_vtbl;
static const IPersistStreamVtbl font_stream_vtbl;
static const IConnectionPointContainerVtbl font_container_vtbl;
static const IPersistPropertyBagVtbl font_bag_vtbl;

static inline struct font *font_of(IFont *iface)
{
	return CONTAINING_RECORD(iface, struct font, IFont_iface);
}

static inline struct font *font_of_dispatch(IDispatch *iface)
{
	return CONTAINING_RECORD(iface, struct font, IDispatch_iface);
}

static inline struct font *font_of_stream(IPersistStream *iface)
{
	return CONTAINING_RECORD(iface, struct font, IPersistStream_iface);
}

static inline struct font *font_of_container(IConnectionPointContainer *iface)
{
	return CONTAINING_RECORD(iface, struct font, IConnectionPointContainer_iface);
}

static inline struct font *font_of_bag(IPersistPropertyBag *iface)
{
	return CONTAINING_RECORD(iface, struct font, IPersistPropertyBag_iface);
}

struct change
{
	DISPID id;
	const WCHAR *name;
};

static void notify_sink(IUnknown *sink, void *context)
{
	const struct change *change = context;
	IPropertyNotifySink_OnChanged((IPropertyNotifySink *)sink, change->id);
}

static void events_sink(IUnknown *sink, void *context)
{
	const struct change *change = context;
	DISPPARAMS params;
	VARIANTARG arg;
	IDispatch *dispatch = NULL;
	if (FAILED(IUnknown_QueryInterface(sink, &IID_IDispatch, (void **)&dispatch)))
		return;
	VariantInit(&arg);
	V_VT(&arg) = VT_BSTR;
	V_BSTR(&arg) = SysAllocString(change->name);
	params.rgvarg = &arg;
	params.rgdispidNamedArgs = NULL;
	params.cArgs = 1;
	params.cNamedArgs = 0;
	IDispatch_Invoke(dispatch, DISPID_FONT_CHANGED, &IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &params, NULL, NULL, NULL);
	VariantClear(&arg);
	IDispatch_Release(dispatch);
}

static void changed(struct font *font, DISPID id, const WCHAR *name)
{
	struct change change = {id, name};
	oa_connection_point_each(font->notify, notify_sink, &change);
	oa_connection_point_each(font->events, events_sink, &change);
}

static void unlink_if_unused(struct shared_font *shared)
{
	struct shared_font **link = &font_head;
	if (shared->users || shared->total)
		return;
	while (*link && *link != shared)
		link = &(*link)->next;
	if (*link)
		*link = shared->next;
	DeleteObject(shared->handle);
	HeapFree(GetProcessHeap(), 0, shared);
}

static void drop_shared(struct font *font)
{
	struct shared_font *shared = font->shared;
	if (!shared)
		return;
	font->shared = NULL;
	EnterCriticalSection(&font_lock);
	shared->users--;
	shared->total--;
	unlink_if_unused(shared);
	LeaveCriticalSection(&font_lock);
}

static LONG font_height(const struct font *font)
{
	LONGLONG size = font->desc.cySize.int64;
	/* Until a caller sets a ratio a font follows the display. */
	if (font->logical == 1 && font->himetric == 1)
	{
		HDC dc = GetDC(NULL);
		LONG dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
		if (dc)
			ReleaseDC(NULL, dc);
		return -(LONG)((size * dpi + 36 * POINTS_UNIT) / (72 * POINTS_UNIT));
	}
	size *= (LONGLONG)font->logical * 2540;
	return -(LONG)((size + (LONGLONG)font->himetric * 36 * POINTS_UNIT) / ((LONGLONG)font->himetric * 72 * POINTS_UNIT));
}

static void realize(struct font *font)
{
	struct shared_font *shared;
	LOGFONTW logfont;
	LONG height;
	if (font->shared && !font->stale)
		return;
	/* A changed font keeps its old handle valid until the new one is asked for. */
	drop_shared(font);
	font->stale = FALSE;
	height = font_height(font);
	EnterCriticalSection(&font_lock);
	for (shared = font_head; shared; shared = shared->next)
		if (shared->height == height && shared->desc.sWeight == font->desc.sWeight && shared->desc.sCharset == font->desc.sCharset && shared->desc.fItalic == font->desc.fItalic && shared->desc.fUnderline == font->desc.fUnderline && shared->desc.fStrikethrough == font->desc.fStrikethrough && !lstrcmpiW(shared->name, font->name))
		{
			shared->users++;
			shared->total++;
			font->shared = shared;
			LeaveCriticalSection(&font_lock);
			return;
		}
	memset(&logfont, 0, sizeof(logfont));
	logfont.lfHeight = height;
	logfont.lfWeight = font->desc.sWeight;
	logfont.lfItalic = (BYTE)font->desc.fItalic;
	logfont.lfUnderline = (BYTE)font->desc.fUnderline;
	logfont.lfStrikeOut = (BYTE)font->desc.fStrikethrough;
	logfont.lfCharSet = (BYTE)font->desc.sCharset;
	lstrcpynW(logfont.lfFaceName, font->name, LF_FACESIZE);
	shared = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*shared));
	if (shared)
	{
		shared->handle = CreateFontIndirectW(&logfont);
		shared->height = height;
		shared->desc = font->desc;
		lstrcpynW(shared->name, font->name, LF_FACESIZE);
		shared->users = 1;
		shared->total = 1;
		shared->next = font_head;
		font_head = shared;
		font->shared = shared;
	}
	LeaveCriticalSection(&font_lock);
}

/* Entries kept alive by unbalanced releases go when no font object is left. */
static void clear_unused(void)
{
	struct shared_font **link = &font_head;
	while (*link)
	{
		struct shared_font *shared = *link;
		if (shared->users)
		{
			link = &shared->next;
			continue;
		}
		*link = shared->next;
		DeleteObject(shared->handle);
		HeapFree(GetProcessHeap(), 0, shared);
	}
}

static HRESULT WINAPI font_QueryInterface(IFont *iface, REFIID riid, void **out)
{
	struct font *font = font_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IFont))
		*out = &font->IFont_iface;
	else if (IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_IFontDisp))
		*out = &font->IDispatch_iface;
	else if (IsEqualIID(riid, &IID_IPersist) || IsEqualIID(riid, &IID_IPersistStream))
		*out = &font->IPersistStream_iface;
	else if (IsEqualIID(riid, &IID_IConnectionPointContainer))
		*out = &font->IConnectionPointContainer_iface;
	else if (IsEqualIID(riid, &IID_IPersistPropertyBag))
		*out = &font->IPersistPropertyBag_iface;
	if (!*out)
		return E_NOINTERFACE;
	IUnknown_AddRef((IUnknown *)*out);
	return S_OK;
}

static ULONG WINAPI font_AddRef(IFont *iface)
{
	return InterlockedIncrement(&font_of(iface)->ref);
}

static ULONG WINAPI font_Release(IFont *iface)
{
	struct font *font = font_of(iface);
	ULONG count = InterlockedDecrement(&font->ref);
	if (!count)
	{
		drop_shared(font);
		SysFreeString(font->name);
		oa_connection_point_destroy(font->notify);
		oa_connection_point_destroy(font->events);
		HeapFree(GetProcessHeap(), 0, font);
		EnterCriticalSection(&font_lock);
		if (!--live_fonts)
			clear_unused();
		LeaveCriticalSection(&font_lock);
	}
	return count;
}

static HRESULT WINAPI font_get_Name(IFont *iface, BSTR *out)
{
	struct font *font = font_of(iface);
	if (!out)
		return E_POINTER;
	*out = font->name ? SysAllocString(font->name) : NULL;
	return S_OK;
}

static HRESULT WINAPI font_put_Name(IFont *iface, BSTR value)
{
	struct font *font = font_of(iface);
	BSTR copy;
	if (!value)
		return CTL_E_INVALIDPROPERTYVALUE;
	copy = SysAllocString(value);
	if (!copy)
		return E_OUTOFMEMORY;
	SysFreeString(font->name);
	font->name = copy;
	/* Symbol faces only exist in their own character set. */
	if (!lstrcmpiW(copy, L"Marlett") || !lstrcmpiW(copy, L"Symbol") || !lstrcmpiW(copy, L"Wingdings") || !lstrcmpiW(copy, L"Webdings"))
		font->desc.sCharset = SYMBOL_CHARSET;
	font->stale = TRUE;
	changed(font, DISPID_FONT_NAME, L"Name");
	return S_OK;
}

static HRESULT WINAPI font_get_Size(IFont *iface, CY *out)
{
	if (!out)
		return E_POINTER;
	*out = font_of(iface)->desc.cySize;
	return S_OK;
}

static HRESULT WINAPI font_put_Size(IFont *iface, CY value)
{
	struct font *font = font_of(iface);
	font->desc.cySize = value;
	font->stale = TRUE;
	changed(font, DISPID_FONT_SIZE, L"Size");
	return S_OK;
}

static HRESULT WINAPI font_get_Bold(IFont *iface, BOOL *out)
{
	if (!out)
		return E_POINTER;
	*out = font_of(iface)->desc.sWeight > FW_MEDIUM;
	return S_OK;
}

static HRESULT WINAPI font_put_Bold(IFont *iface, BOOL value)
{
	struct font *font = font_of(iface);
	font->desc.sWeight = value ? FW_BOLD : FW_NORMAL;
	font->stale = TRUE;
	changed(font, DISPID_FONT_BOLD, L"Bold");
	return S_OK;
}

static HRESULT WINAPI font_get_Italic(IFont *iface, BOOL *out)
{
	if (!out)
		return E_POINTER;
	*out = font_of(iface)->desc.fItalic;
	return S_OK;
}

static HRESULT WINAPI font_put_Italic(IFont *iface, BOOL value)
{
	struct font *font = font_of(iface);
	font->desc.fItalic = value;
	font->stale = TRUE;
	changed(font, DISPID_FONT_ITALIC, L"Italic");
	return S_OK;
}

static HRESULT WINAPI font_get_Underline(IFont *iface, BOOL *out)
{
	if (!out)
		return E_POINTER;
	*out = font_of(iface)->desc.fUnderline;
	return S_OK;
}

static HRESULT WINAPI font_put_Underline(IFont *iface, BOOL value)
{
	struct font *font = font_of(iface);
	font->desc.fUnderline = value;
	font->stale = TRUE;
	changed(font, DISPID_FONT_UNDER, L"Underline");
	return S_OK;
}

static HRESULT WINAPI font_get_Strikethrough(IFont *iface, BOOL *out)
{
	if (!out)
		return E_POINTER;
	*out = font_of(iface)->desc.fStrikethrough;
	return S_OK;
}

static HRESULT WINAPI font_put_Strikethrough(IFont *iface, BOOL value)
{
	struct font *font = font_of(iface);
	font->desc.fStrikethrough = value;
	font->stale = TRUE;
	changed(font, DISPID_FONT_STRIKE, L"Strikethrough");
	return S_OK;
}

static HRESULT WINAPI font_get_Weight(IFont *iface, short *out)
{
	if (!out)
		return E_POINTER;
	*out = font_of(iface)->desc.sWeight;
	return S_OK;
}

static HRESULT WINAPI font_put_Weight(IFont *iface, short value)
{
	struct font *font = font_of(iface);
	font->desc.sWeight = value;
	font->stale = TRUE;
	changed(font, DISPID_FONT_WEIGHT, L"Weight");
	return S_OK;
}

static HRESULT WINAPI font_get_Charset(IFont *iface, short *out)
{
	if (!out)
		return E_POINTER;
	*out = font_of(iface)->desc.sCharset;
	return S_OK;
}

static HRESULT WINAPI font_put_Charset(IFont *iface, short value)
{
	struct font *font = font_of(iface);
	font->desc.sCharset = value;
	font->stale = TRUE;
	changed(font, DISPID_FONT_CHARSET, L"Charset");
	return S_OK;
}

static HRESULT WINAPI font_get_hFont(IFont *iface, HFONT *out)
{
	struct font *font = font_of(iface);
	if (!out)
		return E_POINTER;
	realize(font);
	*out = font->shared ? font->shared->handle : NULL;
	return S_OK;
}

static HRESULT create_font(const FONTDESC *desc, const WCHAR *name, struct font **out);

static HRESULT WINAPI font_Clone(IFont *iface, IFont **out)
{
	struct font *font = font_of(iface), *copy;
	HRESULT status;
	if (!out)
		return E_POINTER;
	status = create_font(&font->desc, font->name, &copy);
	if (FAILED(status))
		return status;
	copy->logical = font->logical;
	copy->himetric = font->himetric;
	copy->dc = font->dc;
	*out = &copy->IFont_iface;
	return S_OK;
}

static HRESULT WINAPI font_IsEqual(IFont *iface, IFont *other)
{
	struct font *font = font_of(iface), *second;
	if (!other)
		return E_POINTER;
	if (other->lpVtbl != &font_vtbl)
		return S_FALSE;
	second = font_of(other);
	if (font->desc.cySize.int64 != second->desc.cySize.int64 || font->desc.sWeight != second->desc.sWeight || font->desc.sCharset != second->desc.sCharset || font->desc.fItalic != second->desc.fItalic || font->desc.fUnderline != second->desc.fUnderline || font->desc.fStrikethrough != second->desc.fStrikethrough)
		return S_FALSE;
	if (font->logical != second->logical || font->himetric != second->himetric)
		return S_FALSE;
	if (SysStringLen(font->name) != SysStringLen(second->name) || memcmp(font->name, second->name, SysStringByteLen(font->name)))
		return S_FALSE;
	return S_OK;
}

static HRESULT WINAPI font_SetRatio(IFont *iface, LONG logical, LONG himetric)
{
	struct font *font = font_of(iface);
	if (logical <= 0 || himetric <= 0)
		return E_FAIL;
	font->logical = logical;
	font->himetric = himetric;
	font->stale = TRUE;
	return S_OK;
}

static HRESULT WINAPI font_QueryTextMetrics(IFont *iface, TEXTMETRICOLE *out)
{
	struct font *font = font_of(iface);
	HDC dc;
	HGDIOBJ old;
	BOOL done;
	if (!out)
		return E_POINTER;
	realize(font);
	dc = GetDC(NULL);
	old = SelectObject(dc, font->shared->handle);
	done = GetTextMetricsW(dc, out);
	SelectObject(dc, old);
	ReleaseDC(NULL, dc);
	return done ? S_OK : E_FAIL;
}

static struct shared_font *find_shared(HFONT handle)
{
	struct shared_font *shared;
	for (shared = font_head; shared; shared = shared->next)
		if (shared->handle == handle)
			return shared;
	return NULL;
}

static HRESULT WINAPI font_AddRefHfont(IFont *iface, HFONT handle)
{
	struct shared_font *shared;
	HRESULT status = S_FALSE;
	if (!handle)
		return E_INVALIDARG;
	EnterCriticalSection(&font_lock);
	shared = find_shared(handle);
	if (shared)
	{
		shared->total++;
		status = S_OK;
	}
	LeaveCriticalSection(&font_lock);
	return status;
}

static HRESULT WINAPI font_ReleaseHfont(IFont *iface, HFONT handle)
{
	struct shared_font *shared;
	HRESULT status = S_FALSE;
	if (!handle)
		return E_INVALIDARG;
	EnterCriticalSection(&font_lock);
	shared = find_shared(handle);
	if (shared && shared->total > 0)
	{
		shared->total--;
		status = S_OK;
		unlink_if_unused(shared);
	}
	LeaveCriticalSection(&font_lock);
	return status;
}

static HRESULT WINAPI font_SetHdc(IFont *iface, HDC dc)
{
	font_of(iface)->dc = dc;
	return E_NOTIMPL;
}

static const IFontVtbl font_vtbl = {
	font_QueryInterface,
	font_AddRef,
	font_Release,
	font_get_Name,
	font_put_Name,
	font_get_Size,
	font_put_Size,
	font_get_Bold,
	font_put_Bold,
	font_get_Italic,
	font_put_Italic,
	font_get_Underline,
	font_put_Underline,
	font_get_Strikethrough,
	font_put_Strikethrough,
	font_get_Weight,
	font_put_Weight,
	font_get_Charset,
	font_put_Charset,
	font_get_hFont,
	font_Clone,
	font_IsEqual,
	font_SetRatio,
	font_QueryTextMetrics,
	font_AddRefHfont,
	font_ReleaseHfont,
	font_SetHdc,
};

static HRESULT WINAPI dispatch_QueryInterface(IDispatch *iface, REFIID riid, void **out)
{
	return font_QueryInterface(&font_of_dispatch(iface)->IFont_iface, riid, out);
}

static ULONG WINAPI dispatch_AddRef(IDispatch *iface)
{
	return font_AddRef(&font_of_dispatch(iface)->IFont_iface);
}

static ULONG WINAPI dispatch_Release(IDispatch *iface)
{
	return font_Release(&font_of_dispatch(iface)->IFont_iface);
}

static HRESULT WINAPI dispatch_GetTypeInfoCount(IDispatch *iface, UINT *out)
{
	if (!out)
		return E_POINTER;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI dispatch_GetTypeInfo(IDispatch *iface, UINT index, LCID lcid, ITypeInfo **out)
{
	ITypeLib *lib = NULL;
	HRESULT status;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (index)
		return E_FAIL;
	status = LoadTypeLib(L"stdole2.tlb", &lib);
	if (FAILED(status))
		return status;
	status = ITypeLib_GetTypeInfoOfGuid(lib, &IID_IFontDisp, out);
	ITypeLib_Release(lib);
	return status;
}

static HRESULT WINAPI dispatch_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids)
{
	ITypeInfo *info = NULL;
	HRESULT status;
	UINT index;
	if (!IsEqualIID(riid, &IID_NULL))
		return DISP_E_UNKNOWNINTERFACE;
	if (!names || !ids || !count)
		return E_INVALIDARG;
	status = dispatch_GetTypeInfo(iface, 0, lcid, &info);
	if (FAILED(status))
		return status;
	/* Properties take no named arguments: only the first name means anything. */
	for (index = 1; index < count; index++)
		ids[index] = DISPID_UNKNOWN;
	status = ITypeInfo_GetIDsOfNames(info, names, 1, ids);
	ITypeInfo_Release(info);
	return status;
}

static HRESULT WINAPI dispatch_Invoke(IDispatch *iface, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err)
{
	struct font *font = font_of_dispatch(iface);
	IFont *object = &font->IFont_iface;
	BOOL getting = (flags & DISPATCH_PROPERTYGET) != 0, putting = (flags & DISPATCH_PROPERTYPUT) != 0;
	VARTYPE type;
	VARIANT value;
	HRESULT status;
	if (!IsEqualIID(riid, &IID_NULL))
		return DISP_E_UNKNOWNINTERFACE;
	switch (id)
	{
	case DISPID_FONT_NAME:
		type = VT_BSTR;
		break;
	case DISPID_FONT_SIZE:
		type = VT_CY;
		break;
	case DISPID_FONT_BOLD:
	case DISPID_FONT_ITALIC:
	case DISPID_FONT_UNDER:
	case DISPID_FONT_STRIKE:
		type = VT_BOOL;
		break;
	case DISPID_FONT_WEIGHT:
	case DISPID_FONT_CHARSET:
		type = VT_I2;
		break;
	default:
		return DISP_E_MEMBERNOTFOUND;
	}
	if (putting)
	{
		if (!params)
			return DISP_E_PARAMNOTOPTIONAL;
		if (params->cArgs != 1)
			return DISP_E_BADPARAMCOUNT;
		VariantInit(&value);
		status = VariantChangeType(&value, &params->rgvarg[0], 0, type);
		if (FAILED(status))
		{
			if (arg_err)
				*arg_err = 0;
			return status;
		}
		switch (id)
		{
		case DISPID_FONT_NAME:
			status = IFont_put_Name(object, V_BSTR(&value));
			break;
		case DISPID_FONT_SIZE:
			status = IFont_put_Size(object, V_CY(&value));
			break;
		case DISPID_FONT_BOLD:
			status = IFont_put_Bold(object, V_BOOL(&value));
			break;
		case DISPID_FONT_ITALIC:
			status = IFont_put_Italic(object, V_BOOL(&value));
			break;
		case DISPID_FONT_UNDER:
			status = IFont_put_Underline(object, V_BOOL(&value));
			break;
		case DISPID_FONT_STRIKE:
			status = IFont_put_Strikethrough(object, V_BOOL(&value));
			break;
		case DISPID_FONT_WEIGHT:
			status = IFont_put_Weight(object, V_I2(&value));
			break;
		default:
			status = IFont_put_Charset(object, V_I2(&value));
			break;
		}
		VariantClear(&value);
		return status;
	}
	if (!getting)
		return DISP_E_MEMBERNOTFOUND;
	if (!result)
		return DISP_E_PARAMNOTOPTIONAL;
	VariantInit(result);
	V_VT(result) = type;
	switch (id)
	{
	case DISPID_FONT_NAME:
		return IFont_get_Name(object, &V_BSTR(result));
	case DISPID_FONT_SIZE:
		return IFont_get_Size(object, &V_CY(result));
	case DISPID_FONT_WEIGHT:
		return IFont_get_Weight(object, &V_I2(result));
	case DISPID_FONT_CHARSET:
		return IFont_get_Charset(object, &V_I2(result));
	default:
	{
		BOOL state = FALSE;
		status = id == DISPID_FONT_BOLD ? IFont_get_Bold(object, &state) : id == DISPID_FONT_ITALIC ? IFont_get_Italic(object, &state) : id == DISPID_FONT_UNDER ? IFont_get_Underline(object, &state) : IFont_get_Strikethrough(object, &state);
		V_BOOL(result) = state ? VARIANT_TRUE : VARIANT_FALSE;
		return status;
	}
	}
}

static const IDispatchVtbl font_dispatch_vtbl = {
	dispatch_QueryInterface,
	dispatch_AddRef,
	dispatch_Release,
	dispatch_GetTypeInfoCount,
	dispatch_GetTypeInfo,
	dispatch_GetIDsOfNames,
	dispatch_Invoke,
};

static HRESULT WINAPI stream_QueryInterface(IPersistStream *iface, REFIID riid, void **out)
{
	return font_QueryInterface(&font_of_stream(iface)->IFont_iface, riid, out);
}

static ULONG WINAPI stream_AddRef(IPersistStream *iface)
{
	return font_AddRef(&font_of_stream(iface)->IFont_iface);
}

static ULONG WINAPI stream_Release(IPersistStream *iface)
{
	return font_Release(&font_of_stream(iface)->IFont_iface);
}

static HRESULT WINAPI stream_GetClassID(IPersistStream *iface, CLSID *out)
{
	if (!out)
		return E_POINTER;
	*out = CLSID_StdFont;
	return S_OK;
}

static HRESULT WINAPI stream_IsDirty(IPersistStream *iface)
{
	return S_OK;
}

/* On stream: version, charset, style bits, weight, size in tenths of a thousandth of a point, then a counted ANSI name. */
#pragma pack(push, 1)
struct font_record
{
	BYTE version;
	WORD charset;
	BYTE style;
	WORD weight;
	DWORD size;
	BYTE name_length;
};
#pragma pack(pop)

#define STYLE_ITALIC 0x02
#define STYLE_UNDERLINE 0x04
#define STYLE_STRIKE 0x08

static HRESULT WINAPI stream_Load(IPersistStream *iface, IStream *source)
{
	struct font *font = font_of_stream(iface);
	struct font_record record;
	char text[256];
	WCHAR wide[256];
	ULONG got = 0;
	int length;
	BSTR name;
	if (FAILED(IStream_Read(source, &record, sizeof(record), &got)) || got != sizeof(record) || record.version != 1)
		return E_FAIL;
	got = 0;
	if (record.name_length && (FAILED(IStream_Read(source, text, record.name_length, &got)) || got != record.name_length))
		return E_FAIL;
	length = record.name_length ? MultiByteToWideChar(CP_ACP, 0, text, record.name_length, wide, ARRAYSIZE(wide) - 1) : 0;
	wide[length] = 0;
	name = SysAllocString(wide);
	if (!name)
		return E_OUTOFMEMORY;
	SysFreeString(font->name);
	font->name = name;
	font->desc.sCharset = record.charset;
	font->desc.sWeight = record.weight;
	font->desc.fItalic = (record.style & STYLE_ITALIC) != 0;
	font->desc.fUnderline = (record.style & STYLE_UNDERLINE) != 0;
	font->desc.fStrikethrough = (record.style & STYLE_STRIKE) != 0;
	font->desc.cySize.int64 = record.size;
	font->stale = TRUE;
	return S_OK;
}

static HRESULT WINAPI stream_Save(IPersistStream *iface, IStream *target, BOOL clear)
{
	struct font *font = font_of_stream(iface);
	struct font_record record;
	char text[256];
	int length = font->name ? WideCharToMultiByte(CP_ACP, 0, font->name, -1, text, sizeof(text), NULL, NULL) : 0;
	ULONG put = 0;
	if (length)
		length--;
	record.version = 1;
	record.charset = font->desc.sCharset;
	record.style = (font->desc.fItalic ? STYLE_ITALIC : 0) | (font->desc.fUnderline ? STYLE_UNDERLINE : 0) | (font->desc.fStrikethrough ? STYLE_STRIKE : 0);
	record.weight = font->desc.sWeight;
	record.size = (DWORD)font->desc.cySize.int64;
	record.name_length = (BYTE)length;
	if (FAILED(IStream_Write(target, &record, sizeof(record), &put)) || put != sizeof(record))
		return E_FAIL;
	if (length && (FAILED(IStream_Write(target, text, length, &put)) || put != (ULONG)length))
		return E_FAIL;
	return S_OK;
}

static HRESULT WINAPI stream_GetSizeMax(IPersistStream *iface, ULARGE_INTEGER *out)
{
	struct font *font = font_of_stream(iface);
	int length;
	if (!out)
		return E_POINTER;
	length = font->name ? WideCharToMultiByte(CP_ACP, 0, font->name, -1, NULL, 0, NULL, NULL) : 0;
	out->QuadPart = sizeof(struct font_record) + (length ? length - 1 : 0);
	return S_OK;
}

static const IPersistStreamVtbl font_stream_vtbl = {
	stream_QueryInterface,
	stream_AddRef,
	stream_Release,
	stream_GetClassID,
	stream_IsDirty,
	stream_Load,
	stream_Save,
	stream_GetSizeMax,
};

static HRESULT WINAPI container_QueryInterface(IConnectionPointContainer *iface, REFIID riid, void **out)
{
	return font_QueryInterface(&font_of_container(iface)->IFont_iface, riid, out);
}

static ULONG WINAPI container_AddRef(IConnectionPointContainer *iface)
{
	return font_AddRef(&font_of_container(iface)->IFont_iface);
}

static ULONG WINAPI container_Release(IConnectionPointContainer *iface)
{
	return font_Release(&font_of_container(iface)->IFont_iface);
}

static HRESULT WINAPI container_EnumConnectionPoints(IConnectionPointContainer *iface, IEnumConnectionPoints **out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI container_FindConnectionPoint(IConnectionPointContainer *iface, REFIID riid, IConnectionPoint **out)
{
	struct font *font = font_of_container(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (IsEqualIID(riid, &IID_IPropertyNotifySink))
		*out = font->notify;
	else if (IsEqualIID(riid, &IID_IFontEventsDisp))
		*out = font->events;
	else
		return CONNECT_E_NOCONNECTION;
	IConnectionPoint_AddRef(*out);
	return S_OK;
}

static const IConnectionPointContainerVtbl font_container_vtbl = {
	container_QueryInterface,
	container_AddRef,
	container_Release,
	container_EnumConnectionPoints,
	container_FindConnectionPoint,
};

static HRESULT WINAPI bag_QueryInterface(IPersistPropertyBag *iface, REFIID riid, void **out)
{
	return font_QueryInterface(&font_of_bag(iface)->IFont_iface, riid, out);
}

static ULONG WINAPI bag_AddRef(IPersistPropertyBag *iface)
{
	return font_AddRef(&font_of_bag(iface)->IFont_iface);
}

static ULONG WINAPI bag_Release(IPersistPropertyBag *iface)
{
	return font_Release(&font_of_bag(iface)->IFont_iface);
}

static HRESULT WINAPI bag_GetClassID(IPersistPropertyBag *iface, CLSID *out)
{
	if (!out)
		return E_POINTER;
	*out = CLSID_StdFont;
	return S_OK;
}

static HRESULT WINAPI bag_InitNew(IPersistPropertyBag *iface)
{
	return S_OK;
}

static HRESULT WINAPI bag_Load(IPersistPropertyBag *iface, IPropertyBag *bag, IErrorLog *log)
{
	struct font *font = font_of_bag(iface);
	static const struct
	{
		const WCHAR *name;
		VARTYPE type;
	} items[] = {
		{L"Name", VT_BSTR},
		{L"Size", VT_CY},
		{L"Charset", VT_I2},
		{L"Weight", VT_I2},
		{L"Underline", VT_BOOL},
		{L"Italic", VT_BOOL},
		{L"Strikethrough", VT_BOOL},
	};
	UINT index;
	for (index = 0; index < ARRAYSIZE(items); index++)
	{
		VARIANT value;
		VariantInit(&value);
		V_VT(&value) = items[index].type;
		if (FAILED(IPropertyBag_Read(bag, items[index].name, &value, log)))
			continue;
		switch (index)
		{
		case 0:
			IFont_put_Name(&font->IFont_iface, V_BSTR(&value));
			break;
		case 1:
			IFont_put_Size(&font->IFont_iface, V_CY(&value));
			break;
		case 2:
			IFont_put_Charset(&font->IFont_iface, V_I2(&value));
			break;
		case 3:
			IFont_put_Weight(&font->IFont_iface, V_I2(&value));
			break;
		case 4:
			IFont_put_Underline(&font->IFont_iface, V_BOOL(&value));
			break;
		case 5:
			IFont_put_Italic(&font->IFont_iface, V_BOOL(&value));
			break;
		default:
			IFont_put_Strikethrough(&font->IFont_iface, V_BOOL(&value));
			break;
		}
		VariantClear(&value);
	}
	return S_OK;
}

static HRESULT WINAPI bag_Save(IPersistPropertyBag *iface, IPropertyBag *bag, BOOL clear, BOOL save_all)
{
	struct font *font = font_of_bag(iface);
	VARIANT value;
	HRESULT status;
	VariantInit(&value);
	V_VT(&value) = VT_BSTR;
	V_BSTR(&value) = SysAllocString(font->name);
	status = IPropertyBag_Write(bag, L"Name", &value);
	VariantClear(&value);
	if (FAILED(status))
		return status;
	V_VT(&value) = VT_CY;
	V_CY(&value) = font->desc.cySize;
	IPropertyBag_Write(bag, L"Size", &value);
	V_VT(&value) = VT_I2;
	V_I2(&value) = font->desc.sCharset;
	IPropertyBag_Write(bag, L"Charset", &value);
	V_I2(&value) = font->desc.sWeight;
	IPropertyBag_Write(bag, L"Weight", &value);
	V_VT(&value) = VT_BOOL;
	V_BOOL(&value) = font->desc.fUnderline ? VARIANT_TRUE : VARIANT_FALSE;
	IPropertyBag_Write(bag, L"Underline", &value);
	V_BOOL(&value) = font->desc.fItalic ? VARIANT_TRUE : VARIANT_FALSE;
	IPropertyBag_Write(bag, L"Italic", &value);
	V_BOOL(&value) = font->desc.fStrikethrough ? VARIANT_TRUE : VARIANT_FALSE;
	IPropertyBag_Write(bag, L"Strikethrough", &value);
	return S_OK;
}

static const IPersistPropertyBagVtbl font_bag_vtbl = {
	bag_QueryInterface,
	bag_AddRef,
	bag_Release,
	bag_GetClassID,
	bag_InitNew,
	bag_Load,
	bag_Save,
};

static HRESULT create_font(const FONTDESC *desc, const WCHAR *name, struct font **out)
{
	struct font *font = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*font));
	if (!font)
		return E_OUTOFMEMORY;
	font->IFont_iface.lpVtbl = &font_vtbl;
	font->IDispatch_iface.lpVtbl = &font_dispatch_vtbl;
	font->IPersistStream_iface.lpVtbl = &font_stream_vtbl;
	font->IConnectionPointContainer_iface.lpVtbl = &font_container_vtbl;
	font->IPersistPropertyBag_iface.lpVtbl = &font_bag_vtbl;
	font->ref = 1;
	font->desc = *desc;
	font->desc.lpstrName = NULL;
	font->name = SysAllocString(name ? name : L"");
	font->logical = 1;
	font->himetric = 1;
	oa_connection_point_create((IUnknown *)&font->IFont_iface, &IID_IPropertyNotifySink, &font->notify);
	oa_connection_point_create((IUnknown *)&font->IFont_iface, &IID_IFontEventsDisp, &font->events);
	EnterCriticalSection(&font_lock);
	live_fonts++;
	LeaveCriticalSection(&font_lock);
	*out = font;
	return S_OK;
}

HRESULT WINAPI OleCreateFontIndirect(FONTDESC *desc, REFIID riid, void **out)
{
	FONTDESC standard;
	struct font *font;
	HRESULT status;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!desc)
	{
		memset(&standard, 0, sizeof(standard));
		standard.cbSizeofstruct = sizeof(standard);
		standard.lpstrName = (LPOLESTR)L"System";
		standard.cySize.int64 = 12 * POINTS_UNIT;
		standard.sWeight = FW_NORMAL;
		desc = &standard;
	}
	if (!desc->lpstrName)
		return CTL_E_INVALIDPROPERTYVALUE;
	status = create_font(desc, desc->lpstrName, &font);
	if (FAILED(status))
		return status;
	status = IFont_QueryInterface(&font->IFont_iface, riid, out);
	IFont_Release(&font->IFont_iface);
	return status;
}

/* What the class factories for the two standard objects hand out. */
HRESULT oa_create_std_font(REFIID riid, void **out)
{
	return OleCreateFontIndirect(NULL, riid, out);
}
