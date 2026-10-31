/* SPDX-License-Identifier: MIT */
#include "ml.h"
#include <wchar.h>

UINT ml_script_count(void);
void ml_script_info(UINT index, SCRIPTINFO *out);

/* Enumerators for code pages, language tags and scripts walk a list fixed when they are made, and all follow one contract. */

typedef enum
{
	list_pages,
	list_tags,
	list_scripts,
} list_kind;

typedef struct enumerator
{
	const void *vtbl;
	LONG refs;
	list_kind kind;
	UINT position;
	UINT count;
	DWORD flags;
} enumerator;

static UINT list_size(list_kind kind, DWORD flags)
{
	UINT index, count = 0;
	switch (kind)
	{
	case list_tags:
		return ml_tag_count();
	case list_scripts:
		return ml_script_count();
	default:
		for (index = 0; index < ml_page_count; index++)
			if (!flags || (ml_pages[index].flags & flags))
				count++;
		return count;
	}
}

static void fill_page(const ml_page *row, MIMECPINFO *out)
{
	memset(out, 0, sizeof(*out));
	out->dwFlags = row->flags;
	out->uiCodePage = row->code_page;
	out->uiFamilyCodePage = row->family;
	out->bGDICharset = row->gdi_charset;
	lstrcpyW(out->wszDescription, row->description);
	lstrcpyW(out->wszWebCharset, row->web_charset);
	lstrcpyW(out->wszHeaderCharset, row->header_charset);
	lstrcpyW(out->wszBodyCharset, row->body_charset);
	lstrcpyW(out->wszFixedWidthFont, row->fixed_font);
	lstrcpyW(out->wszProportionalFont, row->proportional_font);
}

static const ml_page *page_at(DWORD flags, UINT position)
{
	UINT index;
	for (index = 0; index < ml_page_count; index++)
		if (!flags || (ml_pages[index].flags & flags))
			if (!position--)
				return &ml_pages[index];
	return NULL;
}

static HRESULT WINAPI enumerator_QueryInterface(enumerator *self, REFIID riid, void **out);
static ULONG WINAPI enumerator_AddRef(enumerator *self)
{
	return InterlockedIncrement(&self->refs);
}

static ULONG WINAPI enumerator_Release(enumerator *self)
{
	LONG left = InterlockedDecrement(&self->refs);
	if (!left)
		HeapFree(GetProcessHeap(), 0, self);
	return left;
}

static HRESULT enumerator_create(list_kind kind, DWORD flags, void **out);

static HRESULT WINAPI enumerator_Clone(enumerator *self, void **out)
{
	HRESULT status;
	if (!out)
		return E_POINTER;
	status = enumerator_create(self->kind, self->flags, out);
	if (SUCCEEDED(status))
		((enumerator *)*out)->position = self->position;
	return status;
}

static HRESULT WINAPI enumerator_Next(enumerator *self, ULONG wanted, void *items, ULONG *fetched)
{
	ULONG done = 0;
	if (!items)
		return E_FAIL;
	for (; done < wanted && self->position < self->count; done++, self->position++)
	{
		switch (self->kind)
		{
		case list_tags:
			ml_tag_info(ml_tag_lcid_at(self->position), (RFC1766INFO *)items + done);
			break;
		case list_scripts:
			ml_script_info(self->position, (SCRIPTINFO *)items + done);
			break;
		default:
			fill_page(page_at(self->flags, self->position), (MIMECPINFO *)items + done);
			break;
		}
	}
	if (fetched)
		*fetched = done;
	return done ? S_OK : S_FALSE;
}

static HRESULT WINAPI enumerator_Reset(enumerator *self)
{
	self->position = 0;
	return S_OK;
}

static HRESULT WINAPI enumerator_Skip(enumerator *self, ULONG count)
{
	if (count > self->count - self->position)
	{
		self->position = self->count;
		return S_FALSE;
	}
	self->position += count;
	return S_OK;
}

static const struct
{
	void *QueryInterface, *AddRef, *Release, *Clone, *Next, *Reset, *Skip;
} enumerator_vtbl = {
	enumerator_QueryInterface,
	enumerator_AddRef,
	enumerator_Release,
	enumerator_Clone,
	enumerator_Next,
	enumerator_Reset,
	enumerator_Skip,
};

static const IID *const list_iids[] = {&IID_IEnumCodePage, &IID_IEnumRfc1766, &IID_IEnumScript};

static HRESULT WINAPI enumerator_QueryInterface(enumerator *self, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, list_iids[self->kind]))
	{
		*out = self;
		enumerator_AddRef(self);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static HRESULT enumerator_create(list_kind kind, DWORD flags, void **out)
{
	enumerator *self = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &enumerator_vtbl;
	self->refs = 1;
	self->kind = kind;
	self->flags = flags;
	self->count = list_size(kind, flags);
	*out = self;
	return S_OK;
}

/* The charset converter */

typedef struct
{
	IMLangConvertCharset iface;
	LONG refs;
	UINT source;
	UINT target;
	DWORD property;
} converter;

#define CONVERTER(pointer) CONTAINING_RECORD(pointer, converter, iface)

static HRESULT WINAPI converter_QueryInterface(IMLangConvertCharset *self, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IMLangConvertCharset))
	{
		*out = self;
		InterlockedIncrement(&CONVERTER(self)->refs);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI converter_AddRef(IMLangConvertCharset *self)
{
	return InterlockedIncrement(&CONVERTER(self)->refs);
}

static ULONG WINAPI converter_Release(IMLangConvertCharset *self)
{
	LONG left = InterlockedDecrement(&CONVERTER(self)->refs);
	if (!left)
		HeapFree(GetProcessHeap(), 0, CONVERTER(self));
	return left;
}

static HRESULT WINAPI converter_Initialize(IMLangConvertCharset *self, UINT source, UINT target, DWORD property)
{
	CONVERTER(self)->source = source;
	CONVERTER(self)->target = target;
	CONVERTER(self)->property = property;
	return source == CP_ACP ? S_FALSE : S_OK;
}

static HRESULT WINAPI converter_GetSourceCodePage(IMLangConvertCharset *self, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = CONVERTER(self)->source;
	return S_OK;
}

static HRESULT WINAPI converter_GetDestinationCodePage(IMLangConvertCharset *self, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = CONVERTER(self)->target;
	return S_OK;
}

static HRESULT WINAPI converter_GetProperty(IMLangConvertCharset *self, DWORD *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = CONVERTER(self)->property;
	return S_OK;
}

static HRESULT WINAPI converter_DoConversion(IMLangConvertCharset *self, BYTE *source, UINT *source_size, BYTE *target, UINT *target_size)
{
	return ml_convert(CONVERTER(self)->source, CONVERTER(self)->target, source, source_size, target, target_size);
}

static HRESULT WINAPI converter_DoConversionToUnicode(IMLangConvertCharset *self, CHAR *source, UINT *source_size, WCHAR *target, UINT *target_size)
{
	return ml_to_unicode(CONVERTER(self)->source, source, source_size, target, target_size);
}

static HRESULT WINAPI converter_DoConversionFromUnicode(IMLangConvertCharset *self, WCHAR *source, UINT *source_size, CHAR *target, UINT *target_size)
{
	return ml_from_unicode(CONVERTER(self)->target, source, source_size, target, target_size);
}

static const IMLangConvertCharsetVtbl converter_vtbl = {
	converter_QueryInterface,
	converter_AddRef,
	converter_Release,
	converter_Initialize,
	converter_GetSourceCodePage,
	converter_GetDestinationCodePage,
	converter_GetProperty,
	converter_DoConversion,
	converter_DoConversionToUnicode,
	converter_DoConversionFromUnicode,
};

HRESULT ml_convert_charset_create(REFIID riid, void **out)
{
	converter *self = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
	HRESULT status;
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = (IMLangConvertCharsetVtbl *)&converter_vtbl;
	self->refs = 1;
	status = converter_QueryInterface(&self->iface, riid, out);
	converter_Release(&self->iface);
	return status;
}

/* The language object */

#define OBJECT1(pointer) CONTAINING_RECORD(pointer, ml_object, language)
#define OBJECT3(pointer) CONTAINING_RECORD(pointer, ml_object, language3)

HRESULT ml_object_query(ml_object *self, REFIID riid, void **out)
{
	void *found = NULL;
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown))
		found = self->global ? (void *)&self->link : (void *)&self->language;
	else if (IsEqualGUID(riid, &IID_IMLangFontLink) || IsEqualGUID(riid, &IID_IMLangCodePages))
		found = &self->link;
	else if (!self->global)
	{
		if (IsEqualGUID(riid, &IID_IMultiLanguage))
			found = &self->language;
		else if (IsEqualGUID(riid, &IID_IMultiLanguage2) || IsEqualGUID(riid, &IID_IMultiLanguage3))
			found = &self->language3;
		else if (IsEqualGUID(riid, &IID_IMLangFontLink2))
			found = &self->link2;
	}
	*out = found;
	if (!found)
		return E_NOINTERFACE;
	ml_object_add_ref(self);
	return S_OK;
}

ULONG ml_object_add_ref(ml_object *self)
{
	return InterlockedIncrement(&self->refs);
}

ULONG ml_object_release(ml_object *self)
{
	LONG left = InterlockedDecrement(&self->refs);
	if (!left)
		HeapFree(GetProcessHeap(), 0, self);
	return left;
}

static HRESULT WINAPI ml1_QueryInterface(IMultiLanguage *self, REFIID riid, void **out)
{
	return ml_object_query(OBJECT1(self), riid, out);
}

static ULONG WINAPI ml1_AddRef(IMultiLanguage *self)
{
	return ml_object_add_ref(OBJECT1(self));
}

static ULONG WINAPI ml1_Release(IMultiLanguage *self)
{
	return ml_object_release(OBJECT1(self));
}

static HRESULT WINAPI ml3_QueryInterface(IMultiLanguage3 *self, REFIID riid, void **out)
{
	return ml_object_query(OBJECT3(self), riid, out);
}

static ULONG WINAPI ml3_AddRef(IMultiLanguage3 *self)
{
	return ml_object_add_ref(OBJECT3(self));
}

static ULONG WINAPI ml3_Release(IMultiLanguage3 *self)
{
	return ml_object_release(OBJECT3(self));
}

static HRESULT WINAPI ml_GetNumberOfCodePageInfo(void *self, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = ml_page_count;
	return S_OK;
}

static HRESULT WINAPI ml3_GetCodePageInfo(void *self, UINT code_page, LANGID language, MIMECPINFO *out)
{
	const ml_page *row;
	if (!out)
		return E_INVALIDARG;
	if (!(row = ml_page_of(code_page)))
		return E_FAIL;
	fill_page(row, out);
	return S_OK;
}

static HRESULT WINAPI ml1_GetCodePageInfo(void *self, UINT code_page, MIMECPINFO *out)
{
	return ml3_GetCodePageInfo(self, code_page, 0, out);
}

static HRESULT WINAPI ml_GetFamilyCodePage(void *self, UINT code_page, UINT *out)
{
	const ml_page *row;
	if (!out)
		return E_INVALIDARG;
	if (!(row = ml_page_of(code_page)))
		return E_FAIL;
	*out = row->family;
	return S_OK;
}

static HRESULT WINAPI ml3_EnumCodePages(void *self, DWORD flags, LANGID language, IEnumCodePage **out)
{
	if (!out)
		return E_INVALIDARG;
	return enumerator_create(list_pages, flags, (void **)out);
}

static HRESULT WINAPI ml1_EnumCodePages(void *self, DWORD flags, IEnumCodePage **out)
{
	return ml3_EnumCodePages(self, flags, 0, out);
}

static WCHAR lower_ascii(WCHAR letter)
{
	return letter >= 'A' && letter <= 'Z' ? letter + 32 : letter;
}

static BOOL same_name(const WCHAR *first, const WCHAR *second)
{
	for (; *first && *second; first++, second++)
		if (lower_ascii(*first) != lower_ascii(*second))
			return FALSE;
	return !*first && !*second;
}

static HRESULT WINAPI ml_GetCharsetInfo(void *self, BSTR charset, MIMECSETINFO *out)
{
	static const struct
	{
		const WCHAR *name;
		UINT code_page;
	} aliases[] = {
		{L"ascii", 20127},
		{L"iso8859-1", 28591},
		{L"latin1", 28591},
		{L"utf8", 65001},
	};
	const ml_page *row = NULL;
	UINT index;
	if (!charset || !out)
		return E_INVALIDARG;
	for (index = 0; index < ml_page_count && !row; index++)
		if (same_name(ml_pages[index].web_charset, charset) || same_name(ml_pages[index].header_charset, charset) ||
			same_name(ml_pages[index].body_charset, charset))
			row = &ml_pages[index];
	for (index = 0; index < ARRAYSIZE(aliases) && !row; index++)
		if (same_name(aliases[index].name, charset))
			row = ml_page_of(aliases[index].code_page);
	if (!row)
		return E_FAIL;
	out->uiCodePage = row->family;
	out->uiInternetEncoding = row->code_page;
	for (index = 0; charset[index] && index < ARRAYSIZE(out->wszCharset) - 1; index++)
		out->wszCharset[index] = lower_ascii(charset[index]);
	out->wszCharset[index] = 0;
	return S_OK;
}

static HRESULT WINAPI ml_IsConvertible(void *self, DWORD source, DWORD target)
{
	return ml_page_convertible(source) && ml_page_convertible(target) ? S_OK : S_FALSE;
}

static HRESULT WINAPI ml_ConvertString(void *self, DWORD *mode, DWORD from, DWORD to, BYTE *source, UINT *source_size, BYTE *target, UINT *target_size)
{
	return ml_convert(from, to, source, source_size, target, target_size);
}

static HRESULT WINAPI ml_ConvertStringToUnicode(void *self, DWORD *mode, DWORD encoding, CHAR *source, UINT *source_size, WCHAR *target, UINT *target_size)
{
	return ml_to_unicode(encoding, source, source_size, target, target_size);
}

static HRESULT WINAPI ml_ConvertStringFromUnicode(void *self, DWORD *mode, DWORD encoding, WCHAR *source, UINT *source_size, CHAR *target, UINT *target_size)
{
	return ml_from_unicode(encoding, source, source_size, target, target_size);
}

static HRESULT WINAPI ml_ConvertStringReset(void *self)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI ml_GetRfc1766FromLcid(void *self, LCID lcid, BSTR *out)
{
	const WCHAR *tag;
	if (!out)
		return E_INVALIDARG;
	if (!ml_lcid_to_tag(lcid, &tag))
		return E_FAIL;
	*out = SysAllocString(tag);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI ml_GetLcidFromRfc1766(void *self, LCID *out, BSTR tag)
{
	if (!out || !tag)
		return E_INVALIDARG;
	return ml_tag_to_lcid(tag, out);
}

static HRESULT WINAPI ml3_EnumRfc1766(void *self, LANGID language, IEnumRfc1766 **out)
{
	if (!out)
		return E_INVALIDARG;
	return enumerator_create(list_tags, 0, (void **)out);
}

static HRESULT WINAPI ml1_EnumRfc1766(void *self, IEnumRfc1766 **out)
{
	return ml3_EnumRfc1766(self, 0, out);
}

static HRESULT WINAPI ml3_GetRfc1766Info(void *self, LCID lcid, LANGID language, RFC1766INFO *out)
{
	const WCHAR *tag;
	if (!out)
		return E_INVALIDARG;
	if (!ml_lcid_to_tag(lcid, &tag))
		return E_FAIL;
	ml_tag_info(lcid, out);
	return S_OK;
}

static HRESULT WINAPI ml1_GetRfc1766Info(void *self, LCID lcid, RFC1766INFO *out)
{
	return ml3_GetRfc1766Info(self, lcid, 0, out);
}

static HRESULT WINAPI ml_CreateConvertCharset(void *self, UINT source, UINT target, DWORD property, IMLangConvertCharset **out)
{
	HRESULT status;
	if (!out)
		return E_INVALIDARG;
	status = ml_convert_charset_create(&IID_IMLangConvertCharset, (void **)out);
	return FAILED(status) ? status : IMLangConvertCharset_Initialize(*out, source, target, property);
}

/* A whole stream at once, in a buffer that grows; streams here are short texts. */
static HRESULT read_stream(IStream *stream, BYTE **bytes, ULONG *size)
{
	ULONG room = 4096, used = 0, got = 0;
	BYTE *data = HeapAlloc(GetProcessHeap(), 0, room);
	HRESULT status;
	if (!data)
		return E_OUTOFMEMORY;
	for (;;)
	{
		status = IStream_Read(stream, data + used, room - used, &got);
		if (FAILED(status))
		{
			HeapFree(GetProcessHeap(), 0, data);
			return status;
		}
		if (!got)
			break;
		used += got;
		if (used == room)
		{
			BYTE *more = HeapReAlloc(GetProcessHeap(), 0, data, room * 2);
			if (!more)
			{
				HeapFree(GetProcessHeap(), 0, data);
				return E_OUTOFMEMORY;
			}
			data = more;
			room *= 2;
		}
	}
	*bytes = data;
	*size = used;
	return S_OK;
}

static HRESULT rewind_stream(IStream *stream)
{
	LARGE_INTEGER zero;
	zero.QuadPart = 0;
	return IStream_Seek(stream, zero, STREAM_SEEK_SET, NULL);
}

static HRESULT WINAPI ml_ConvertStringInIStream(void *self, DWORD *mode, DWORD flag, WCHAR *fallback, DWORD from, DWORD to, IStream *in, IStream *out)
{
	BYTE *source, *target = NULL;
	ULONG source_size;
	UINT used, target_size = 0;
	HRESULT status;
	if (!in || !out)
		return E_INVALIDARG;
	if (FAILED(status = read_stream(in, &source, &source_size)))
		return status;
	used = source_size;
	status = ml_convert(from, to, source, &used, NULL, &target_size);
	if (SUCCEEDED(status) && !(target = HeapAlloc(GetProcessHeap(), 0, target_size ? target_size : 1)))
		status = E_OUTOFMEMORY;
	if (SUCCEEDED(status))
	{
		used = source_size;
		status = ml_convert(from, to, source, &used, target, &target_size);
	}
	if (SUCCEEDED(status))
		status = IStream_Write(out, target, target_size, NULL);
	HeapFree(GetProcessHeap(), 0, source);
	if (target)
		HeapFree(GetProcessHeap(), 0, target);
	return status;
}

static HRESULT WINAPI ml_ConvertStringToUnicodeEx(void *self, DWORD *mode, DWORD encoding, CHAR *source, UINT *source_size, WCHAR *target, UINT *target_size, DWORD flag, WCHAR *fallback)
{
	return ml_to_unicode(encoding, source, source_size, target, target_size);
}

static HRESULT WINAPI ml_ConvertStringFromUnicodeEx(void *self, DWORD *mode, DWORD encoding, WCHAR *source, UINT *source_size, CHAR *target, UINT *target_size, DWORD flag, WCHAR *fallback)
{
	return ml_from_unicode(encoding, source, source_size, target, target_size);
}

static BOOL valid_utf8(const BYTE *text, INT size, BOOL *any_high)
{
	INT index = 0;
	*any_high = FALSE;
	while (index < size)
	{
		BYTE lead = text[index++];
		INT trail = lead < 0x80 ? 0 : (lead & 0xe0) == 0xc0 ? 1 : (lead & 0xf0) == 0xe0 ? 2 : (lead & 0xf8) == 0xf0 ? 3 : -1;
		if (trail < 0)
			return FALSE;
		if (trail)
			*any_high = TRUE;
		while (trail--)
			if (index >= size || (text[index++] & 0xc0) != 0x80)
				return FALSE;
	}
	return TRUE;
}

/* Marks and validity decide; failing those the caller's preference, then the western page, are the honest guess. */
static HRESULT detect_bytes(DWORD preferred, const BYTE *text, INT size, DetectEncodingInfo *found, INT *scores)
{
	UINT code_page;
	BOOL high;
	if (!text || !found || !scores || *scores <= 0)
		return E_INVALIDARG;
	if (size >= 2 && text[0] == 0xff && text[1] == 0xfe)
		code_page = CP_UNICODE;
	else if (size >= 2 && text[0] == 0xfe && text[1] == 0xff)
		code_page = CP_UNICODE_SWAPPED;
	else if (valid_utf8(text, size, &high) && high)
		code_page = CP_UTF8;
	else if (preferred && ml_page_of(preferred))
		code_page = preferred;
	else
		code_page = 1252;
	found->nLangID = 0;
	found->nCodePage = code_page;
	found->nDocPercent = 100;
	found->nConfidence = 100;
	*scores = 1;
	return S_OK;
}

static HRESULT WINAPI ml_DetectInputCodepage(void *self, DWORD flag, DWORD preferred, CHAR *source, INT *size, DetectEncodingInfo *found, INT *scores)
{
	if (!size)
		return E_INVALIDARG;
	return detect_bytes(preferred, (const BYTE *)source, *size, found, scores);
}

static HRESULT WINAPI ml_DetectCodepageInIStream(void *self, DWORD flag, DWORD preferred, IStream *stream, DetectEncodingInfo *found, INT *scores)
{
	BYTE *bytes;
	ULONG size;
	HRESULT status;
	if (!stream)
		return E_INVALIDARG;
	if (FAILED(status = read_stream(stream, &bytes, &size)))
		return status;
	status = detect_bytes(preferred, bytes, size, found, scores);
	HeapFree(GetProcessHeap(), 0, bytes);
	rewind_stream(stream);
	return status;
}

static HRESULT WINAPI ml_ValidateCodePage(void *self, UINT code_page, HWND window)
{
	return ml_page_convertible(code_page) ? S_OK : S_FALSE;
}

static HRESULT WINAPI ml_GetCodePageDescription(void *self, UINT code_page, LCID lcid, LPWSTR out, int room)
{
	const ml_page *row;
	if (!out || room <= 0)
		return E_INVALIDARG;
	if (!(row = ml_page_of(code_page)))
		return E_INVALIDARG;
	lstrcpynW(out, row->description, room);
	return S_OK;
}

static HRESULT WINAPI ml_IsCodePageInstallable(void *self, UINT code_page)
{
	return ml_page_convertible(code_page) ? S_OK : S_FALSE;
}

static HRESULT WINAPI ml_SetMimeDBSource(void *self, MIMECONTF source)
{
	return S_OK;
}

static HRESULT WINAPI ml_GetNumberOfScripts(void *self, UINT *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = ml_script_count();
	return S_OK;
}

static HRESULT WINAPI ml_EnumScripts(void *self, DWORD flags, LANGID language, IEnumScript **out)
{
	if (!out)
		return E_INVALIDARG;
	return enumerator_create(list_scripts, 0, (void **)out);
}

static HRESULT WINAPI ml_ValidateCodePageEx(void *self, UINT code_page, HWND window, DWORD control)
{
	return ml_page_convertible(code_page) ? S_OK : S_FALSE;
}

static BOOL page_holds(UINT code_page, const WCHAR *text, UINT size)
{
	BOOL lost = FALSE;
	if (!size)
		return TRUE;
	return WideCharToMultiByte(code_page, WC_NO_BEST_FIT_CHARS, text, size, NULL, 0, NULL, &lost) > 0 && !lost;
}

static void add_detected(UINT *list, UINT *used, UINT room, UINT code_page)
{
	UINT index;
	for (index = 0; index < *used; index++)
		if (list[index] == code_page)
			return;
	if (*used < room)
		list[(*used)++] = code_page;
}

/* The pages asked about that hold the whole text, then the three Unicode forms, which hold anything. */
static HRESULT WINAPI ml_DetectOutboundCodePage(void *self, DWORD flags, LPCWSTR text, UINT size, UINT *preferred, UINT preferred_count, UINT *detected, UINT *detected_count, WCHAR *special)
{
	UINT index, used = 0;
	if (!detected || !detected_count || !*detected_count)
		return E_INVALIDARG;
	for (index = 0; preferred && index < preferred_count; index++)
		if (preferred[index] == CP_UNICODE || preferred[index] == CP_UTF8 || preferred[index] == CP_UTF7 || page_holds(preferred[index], text, size))
			add_detected(detected, &used, *detected_count, preferred[index]);
	add_detected(detected, &used, *detected_count, CP_UTF8);
	add_detected(detected, &used, *detected_count, CP_UTF7);
	add_detected(detected, &used, *detected_count, CP_UNICODE);
	*detected_count = used;
	return S_OK;
}

static HRESULT WINAPI ml_DetectOutboundCodePageInIStream(void *self, DWORD flags, IStream *stream, UINT *preferred, UINT preferred_count, UINT *detected, UINT *detected_count, WCHAR *special)
{
	BYTE *bytes;
	ULONG size;
	HRESULT status;
	if (!stream || !detected || !detected_count || !*detected_count)
		return E_INVALIDARG;
	if (FAILED(status = read_stream(stream, &bytes, &size)))
		return status;
	status = ml_DetectOutboundCodePage(self, flags, (const WCHAR *)bytes, size / 2, preferred, preferred_count, detected, detected_count, special);
	HeapFree(GetProcessHeap(), 0, bytes);
	rewind_stream(stream);
	return status;
}

static const IMultiLanguageVtbl ml1_vtbl = {
	ml1_QueryInterface,
	ml1_AddRef,
	ml1_Release,
	(void *)ml_GetNumberOfCodePageInfo,
	(void *)ml1_GetCodePageInfo,
	(void *)ml_GetFamilyCodePage,
	(void *)ml1_EnumCodePages,
	(void *)ml_GetCharsetInfo,
	(void *)ml_IsConvertible,
	(void *)ml_ConvertString,
	(void *)ml_ConvertStringToUnicode,
	(void *)ml_ConvertStringFromUnicode,
	(void *)ml_ConvertStringReset,
	(void *)ml_GetRfc1766FromLcid,
	(void *)ml_GetLcidFromRfc1766,
	(void *)ml1_EnumRfc1766,
	(void *)ml1_GetRfc1766Info,
	(void *)ml_CreateConvertCharset,
};

static const IMultiLanguage3Vtbl ml3_vtbl = {
	ml3_QueryInterface,
	ml3_AddRef,
	ml3_Release,
	(void *)ml_GetNumberOfCodePageInfo,
	(void *)ml3_GetCodePageInfo,
	(void *)ml_GetFamilyCodePage,
	(void *)ml3_EnumCodePages,
	(void *)ml_GetCharsetInfo,
	(void *)ml_IsConvertible,
	(void *)ml_ConvertString,
	(void *)ml_ConvertStringToUnicode,
	(void *)ml_ConvertStringFromUnicode,
	(void *)ml_ConvertStringReset,
	(void *)ml_GetRfc1766FromLcid,
	(void *)ml_GetLcidFromRfc1766,
	(void *)ml3_EnumRfc1766,
	(void *)ml3_GetRfc1766Info,
	(void *)ml_CreateConvertCharset,
	(void *)ml_ConvertStringInIStream,
	(void *)ml_ConvertStringToUnicodeEx,
	(void *)ml_ConvertStringFromUnicodeEx,
	(void *)ml_DetectCodepageInIStream,
	(void *)ml_DetectInputCodepage,
	(void *)ml_ValidateCodePage,
	(void *)ml_GetCodePageDescription,
	(void *)ml_IsCodePageInstallable,
	(void *)ml_SetMimeDBSource,
	(void *)ml_GetNumberOfScripts,
	(void *)ml_EnumScripts,
	(void *)ml_ValidateCodePageEx,
	(void *)ml_DetectOutboundCodePage,
	(void *)ml_DetectOutboundCodePageInIStream,
};

static ml_object *object_create(BOOL global)
{
	ml_object *self = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
	if (!self)
		return NULL;
	self->language = &ml1_vtbl;
	self->language3 = &ml3_vtbl;
	self->link = &ml_link_vtbl;
	self->link2 = &ml_link2_vtbl;
	self->refs = 1;
	self->global = global;
	return self;
}

HRESULT ml_language_create(REFIID riid, void **out)
{
	ml_object *self = object_create(FALSE);
	HRESULT status;
	if (!self)
		return E_OUTOFMEMORY;
	status = ml_object_query(self, riid, out);
	ml_object_release(self);
	return status;
}

/* Never freed: its first reference belongs to the process. */
HRESULT WINAPI GetGlobalFontLinkObject(void **out)
{
	static ml_object *volatile shared;
	if (!out)
		return E_INVALIDARG;
	if (!shared)
	{
		ml_object *self = object_create(TRUE);
		if (!self)
			return E_OUTOFMEMORY;
		if (InterlockedCompareExchangePointer((void *volatile *)&shared, self, NULL))
			HeapFree(GetProcessHeap(), 0, self);
	}
	*out = (void *)&shared->link;
	ml_object_add_ref(shared);
	return S_OK;
}

/* The flat conversion functions keep their counts in signed integers of the same size. */

HRESULT WINAPI ConvertINetString(DWORD *mode, DWORD from, DWORD to, LPCSTR source, LPINT source_size, LPSTR target, LPINT target_size)
{
	return ml_convert(from, to, (const BYTE *)source, (UINT *)source_size, (BYTE *)target, (UINT *)target_size);
}

HRESULT WINAPI ConvertINetMultiByteToUnicode(DWORD *mode, DWORD encoding, LPCSTR source, LPINT source_size, LPWSTR target, LPINT target_size)
{
	return ml_to_unicode(encoding, source, (UINT *)source_size, target, (UINT *)target_size);
}

HRESULT WINAPI ConvertINetUnicodeToMultiByte(DWORD *mode, DWORD encoding, LPCWSTR source, LPINT source_size, LPSTR target, LPINT target_size)
{
	return ml_from_unicode(encoding, source, (UINT *)source_size, target, (UINT *)target_size);
}

HRESULT WINAPI ConvertINetReset(void)
{
	return S_OK;
}

HRESULT WINAPI IsConvertINetStringAvailable(DWORD from, DWORD to)
{
	return ml_IsConvertible(NULL, from, to);
}
