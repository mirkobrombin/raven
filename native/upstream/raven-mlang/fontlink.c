/* SPDX-License-Identifier: MIT */
#include "ml.h"
#include <wchar.h>

/* Which code pages a piece of text fits, and which font carries them. The answers come from the system's own converters and
 * fonts. A mapped font is shared by everyone asking for the same thing and lives until the mapping is reset. */

static const struct
{
	UINT code_page;
	DWORD mask;
	BYTE charset;
} link_pages[] = {
	{1252, FS_LATIN1, ANSI_CHARSET},
	{1250, FS_LATIN2, EASTEUROPE_CHARSET},
	{1251, FS_CYRILLIC, RUSSIAN_CHARSET},
	{1253, FS_GREEK, GREEK_CHARSET},
	{1254, FS_TURKISH, TURKISH_CHARSET},
	{1255, FS_HEBREW, HEBREW_CHARSET},
	{1256, FS_ARABIC, ARABIC_CHARSET},
	{1257, FS_BALTIC, BALTIC_CHARSET},
	{1258, FS_VIETNAMESE, VIETNAMESE_CHARSET},
	{874, FS_THAI, THAI_CHARSET},
	{932, FS_JISJAPAN, SHIFTJIS_CHARSET},
	{936, FS_CHINESESIMP, GB2312_CHARSET},
	{949, FS_WANSUNG, HANGUL_CHARSET},
	{950, FS_CHINESETRAD, CHINESEBIG5_CHARSET},
};

static const struct
{
	SCRIPT_ID id;
	UINT code_page;
	const WCHAR *description;
	const WCHAR *fixed;
	const WCHAR *proportional;
} scripts[] = {
	{sidAsciiLatin, 1252, L"Latin", L"Courier New", L"Arial"},
	{sidLatin, 1252, L"Latin Extended", L"Courier New", L"Arial"},
	{sidGreek, 1253, L"Greek", L"Courier New", L"Arial"},
	{sidCyrillic, 1251, L"Cyrillic", L"Courier New", L"Arial"},
	{sidArmenian, 1200, L"Armenian", L"Courier New", L"Arial"},
	{sidHebrew, 1255, L"Hebrew", L"Courier New", L"Arial"},
	{sidArabic, 1256, L"Arabic", L"Courier New", L"Arial"},
	{sidDevanagari, 1200, L"Devanagari", L"Mangal", L"Mangal"},
	{sidThai, 874, L"Thai", L"Courier New", L"Tahoma"},
	{sidHangul, 949, L"Korean", L"GulimChe", L"Gulim"},
	{sidKana, 932, L"Japanese", L"MS Gothic", L"MS PGothic"},
	{sidBopomofo, 950, L"Traditional Chinese", L"MingLiU", L"PMingLiU"},
	{sidHan, 936, L"Simplified Chinese", L"NSimSun", L"SimSun"},
};

UINT ml_script_count(void)
{
	return ARRAYSIZE(scripts);
}

void ml_script_info(UINT index, SCRIPTINFO *out)
{
	out->ScriptId = scripts[index].id;
	out->uiCodePage = scripts[index].code_page;
	lstrcpyW(out->wszDescription, scripts[index].description);
	lstrcpyW(out->wszFixedWidthFont, scripts[index].fixed);
	lstrcpyW(out->wszProportionalFont, scripts[index].proportional);
}

static HRESULT WINAPI link_QueryInterface(IMLangFontLink *self, REFIID riid, void **out)
{
	return ml_object_query(CONTAINING_RECORD(self, ml_object, link), riid, out);
}

static ULONG WINAPI link_AddRef(IMLangFontLink *self)
{
	return ml_object_add_ref(CONTAINING_RECORD(self, ml_object, link));
}

static ULONG WINAPI link_Release(IMLangFontLink *self)
{
	return ml_object_release(CONTAINING_RECORD(self, ml_object, link));
}

static HRESULT WINAPI link2_QueryInterface(IMLangFontLink2 *self, REFIID riid, void **out)
{
	return ml_object_query(CONTAINING_RECORD(self, ml_object, link2), riid, out);
}

static ULONG WINAPI link2_AddRef(IMLangFontLink2 *self)
{
	return ml_object_add_ref(CONTAINING_RECORD(self, ml_object, link2));
}

static ULONG WINAPI link2_Release(IMLangFontLink2 *self)
{
	return ml_object_release(CONTAINING_RECORD(self, ml_object, link2));
}

static HRESULT WINAPI link_GetCharCodePages(void *self, WCHAR letter, DWORD *out)
{
	UINT index;
	DWORD mask = 0;
	if (!out)
		return E_INVALIDARG;
	for (index = 0; index < ARRAYSIZE(link_pages); index++)
	{
		char bytes[8];
		BOOL lost = FALSE;
		if (WideCharToMultiByte(link_pages[index].code_page, WC_NO_BEST_FIT_CHARS, &letter, 1, bytes, sizeof(bytes), NULL, &lost) && !lost)
			mask |= link_pages[index].mask;
	}
	*out = mask;
	return S_OK;
}

static HRESULT WINAPI link_GetStrCodePages(void *self, const WCHAR *text, LONG size, DWORD priority, DWORD *pages, LONG *fitted)
{
	DWORD gathered = 0;
	LONG index = 0;
	if (pages)
		*pages = 0;
	if (fitted)
		*fitted = 0;
	if (!text || size <= 0)
		return E_INVALIDARG;
	for (; index < size; index++)
	{
		DWORD mask, next;
		link_GetCharCodePages(self, text[index], &mask);
		next = index ? gathered & mask : mask;
		if (index && (!next || (next & priority) != (gathered & priority)))
			break;
		gathered = next;
	}
	if (pages)
		*pages = gathered;
	if (fitted)
		*fitted = index;
	return S_OK;
}

static HRESULT WINAPI link_CodePageToCodePages(void *self, UINT code_page, DWORD *out)
{
	UINT index;
	if (!out)
		return E_INVALIDARG;
	*out = 0;
	for (index = 0; index < ARRAYSIZE(link_pages); index++)
		if (link_pages[index].code_page == code_page)
		{
			*out = link_pages[index].mask;
			return S_OK;
		}
	return E_FAIL;
}

static HRESULT WINAPI link_CodePagesToCodePage(void *self, DWORD pages, UINT preferred, UINT *out)
{
	UINT index;
	DWORD mask;
	if (!out)
		return E_INVALIDARG;
	if (preferred && SUCCEEDED(link_CodePageToCodePages(self, preferred, &mask)) && (pages & mask))
	{
		*out = preferred;
		return S_OK;
	}
	for (index = 0; index < ARRAYSIZE(link_pages); index++)
		if (pages & link_pages[index].mask)
		{
			*out = link_pages[index].code_page;
			return S_OK;
		}
	*out = 0;
	return E_FAIL;
}

static HRESULT WINAPI link_GetFontCodePages(void *self, HDC context, HFONT font, DWORD *out)
{
	HGDIOBJ previous;
	FONTSIGNATURE signature;
	if (out)
		*out = 0;
	if (!context || !font || !(previous = SelectObject(context, font)))
		return E_FAIL;
	memset(&signature, 0, sizeof(signature));
	GetTextCharsetInfo(context, &signature, 0);
	SelectObject(context, previous);
	if (out)
		*out = signature.fsCsb[0];
	return S_OK;
}

/* The mapped fonts, kept for the whole process */

typedef struct mapped_font
{
	struct mapped_font *next;
	HFONT font;
	BYTE charset;
	LONG height;
	LONG weight;
	BYTE italic;
	WCHAR face[LF_FACESIZE];
} mapped_font;

static mapped_font *mapped_fonts;
static CRITICAL_SECTION map_lock;
static INIT_ONCE map_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_map(INIT_ONCE *once, void *parameter, void **context)
{
	InitializeCriticalSection(&map_lock);
	return TRUE;
}

static BYTE charset_of_mask(DWORD pages)
{
	UINT index;
	for (index = 0; index < ARRAYSIZE(link_pages); index++)
		if (pages & link_pages[index].mask)
			return link_pages[index].charset;
	return DEFAULT_CHARSET;
}

static HRESULT map_font(HDC context, DWORD pages, HFONT model, HFONT *out)
{
	LOGFONTW wanted;
	mapped_font *entry;
	BYTE charset = charset_of_mask(pages);
	HFONT font = NULL;
	if (!GetObjectW(model, sizeof(wanted), &wanted))
		return E_FAIL;
	InitOnceExecuteOnce(&map_once, init_map, NULL, NULL);
	EnterCriticalSection(&map_lock);
	for (entry = mapped_fonts; entry; entry = entry->next)
		if (entry->charset == charset && entry->height == wanted.lfHeight && entry->weight == wanted.lfWeight && entry->italic == wanted.lfItalic)
		{
			font = entry->font;
			break;
		}
	if (!font)
	{
		wanted.lfCharSet = charset;
		wanted.lfFaceName[0] = 0;
		font = CreateFontIndirectW(&wanted);
		if (font && (entry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*entry))))
		{
			entry->font = font;
			entry->charset = charset;
			entry->height = wanted.lfHeight;
			entry->weight = wanted.lfWeight;
			entry->italic = wanted.lfItalic;
			entry->next = mapped_fonts;
			mapped_fonts = entry;
		}
	}
	LeaveCriticalSection(&map_lock);
	if (!font)
		return E_FAIL;
	if (out)
		*out = font;
	return S_OK;
}

static HRESULT WINAPI link_MapFont(IMLangFontLink *self, HDC context, DWORD pages, HFONT model, HFONT *out)
{
	if (!context || !model)
		return E_FAIL;
	return map_font(context, pages, model, out);
}

static HRESULT WINAPI link2_MapFont(IMLangFontLink2 *self, HDC context, DWORD pages, WCHAR letter, HFONT *out)
{
	if (!context)
		return E_FAIL;
	if (!pages)
	{
		if (!out)
			return E_INVALIDARG;
		link_GetCharCodePages(self, letter, &pages);
	}
	return map_font(context, pages, GetCurrentObject(context, OBJ_FONT), out);
}

/* The caller's hold on a mapped font ends here; the font itself waits for the mapping to be reset. */
static HRESULT WINAPI link_ReleaseFont(void *self, HFONT font)
{
	mapped_font *entry;
	HRESULT status = E_FAIL;
	if (!font)
		return E_FAIL;
	InitOnceExecuteOnce(&map_once, init_map, NULL, NULL);
	EnterCriticalSection(&map_lock);
	for (entry = mapped_fonts; entry; entry = entry->next)
		if (entry->font == font)
			status = S_OK;
	LeaveCriticalSection(&map_lock);
	return status;
}

static HRESULT WINAPI link_ResetFontMapping(void *self)
{
	mapped_font *entry;
	InitOnceExecuteOnce(&map_once, init_map, NULL, NULL);
	EnterCriticalSection(&map_lock);
	while ((entry = mapped_fonts))
	{
		mapped_fonts = entry->next;
		DeleteObject(entry->font);
		HeapFree(GetProcessHeap(), 0, entry);
	}
	LeaveCriticalSection(&map_lock);
	return S_OK;
}

static HRESULT WINAPI link2_GetFontUnicodeRanges(IMLangFontLink2 *self, HDC context, UINT *count, UNICODERANGE *ranges)
{
	DWORD size;
	GLYPHSET *set;
	UINT index, wanted;
	if (!count)
		return E_INVALIDARG;
	if (!context)
		return E_FAIL;
	if (!(size = GetFontUnicodeRanges(context, NULL)))
		return E_FAIL;
	if (!(set = HeapAlloc(GetProcessHeap(), 0, size)))
		return E_OUTOFMEMORY;
	GetFontUnicodeRanges(context, set);
	wanted = ranges ? min(*count, set->cRanges) : set->cRanges;
	if (ranges)
		for (index = 0; index < wanted; index++)
		{
			ranges[index].wcFrom = set->ranges[index].wcLow;
			ranges[index].wcTo = (WCHAR)(set->ranges[index].wcLow + set->ranges[index].cGlyphs - 1);
		}
	*count = wanted;
	HeapFree(GetProcessHeap(), 0, set);
	return S_OK;
}

static HRESULT WINAPI link2_GetScriptFontInfo(IMLangFontLink2 *self, SCRIPT_ID script, DWORD flags, UINT *count, SCRIPTFONTINFO *fonts)
{
	UINT index;
	if (!count)
		return E_INVALIDARG;
	for (index = 0; index < ARRAYSIZE(scripts); index++)
		if (scripts[index].id == script)
		{
			UINT available = flags ? 1 : 2;
			if (fonts)
			{
				UINT room = *count, used = 0;
				if (room > used && !(flags & SCRIPTCONTF_PROPORTIONAL_FONT))
				{
					fonts[used].scripts = (SCRIPT_IDS)1 << script;
					lstrcpyW(fonts[used++].wszFont, scripts[index].fixed);
				}
				if (room > used && !(flags & SCRIPTCONTF_FIXED_FONT))
				{
					fonts[used].scripts = (SCRIPT_IDS)1 << script;
					lstrcpyW(fonts[used++].wszFont, scripts[index].proportional);
				}
				available = used;
			}
			*count = available;
			return S_OK;
		}
	*count = 0;
	return S_OK;
}

static HRESULT WINAPI link2_CodePageToScriptID(IMLangFontLink2 *self, UINT code_page, SCRIPT_ID *out)
{
	static const struct
	{
		UINT code_page;
		SCRIPT_ID script;
	} table[] = {
		{874, sidThai}, {932, sidKana}, {936, sidHan}, {949, sidHangul}, {950, sidBopomofo}, {1250, sidAsciiLatin}, {1251, sidCyrillic},
		{1252, sidAsciiLatin}, {1253, sidGreek}, {1254, sidAsciiLatin}, {1255, sidHebrew}, {1256, sidArabic}, {1257, sidAsciiLatin}, {1258, sidAsciiLatin},
	};
	UINT index;
	if (!out)
		return E_INVALIDARG;
	for (index = 0; index < ARRAYSIZE(table); index++)
		if (table[index].code_page == code_page)
		{
			*out = table[index].script;
			return S_OK;
		}
	return E_FAIL;
}

const IMLangFontLinkVtbl ml_link_vtbl = {
	link_QueryInterface,
	link_AddRef,
	link_Release,
	(void *)link_GetCharCodePages,
	(void *)link_GetStrCodePages,
	(void *)link_CodePageToCodePages,
	(void *)link_CodePagesToCodePage,
	(void *)link_GetFontCodePages,
	link_MapFont,
	(void *)link_ReleaseFont,
	(void *)link_ResetFontMapping,
};

const IMLangFontLink2Vtbl ml_link2_vtbl = {
	link2_QueryInterface,
	link2_AddRef,
	link2_Release,
	(void *)link_GetCharCodePages,
	(void *)link_GetStrCodePages,
	(void *)link_CodePageToCodePages,
	(void *)link_CodePagesToCodePage,
	(void *)link_GetFontCodePages,
	(void *)link_ReleaseFont,
	(void *)link_ResetFontMapping,
	link2_MapFont,
	link2_GetFontUnicodeRanges,
	link2_GetScriptFontInfo,
	link2_CodePageToScriptID,
};
