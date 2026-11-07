/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_MLANG_ML_H
#define RAVEN_MLANG_ML_H

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <mlang.h>

#define CP_UNICODE 1200
#define CP_UNICODE_SWAPPED 1201

typedef struct
{
	UINT code_page;
	UINT family;
	DWORD flags;
	const WCHAR *description;
	const WCHAR *web_charset;
	const WCHAR *header_charset;
	const WCHAR *body_charset;
	const WCHAR *fixed_font;
	const WCHAR *proportional_font;
	BYTE gdi_charset;
} ml_page;

extern const ml_page ml_pages[];
extern const UINT ml_page_count;
extern LONG ml_locks;

const ml_page *ml_page_of(UINT code_page);
BOOL ml_page_convertible(UINT code_page);
HRESULT ml_to_unicode(UINT code_page, const char *source, UINT *source_size, WCHAR *target, UINT *target_size);
HRESULT ml_from_unicode(UINT code_page, const WCHAR *source, UINT *source_size, char *target, UINT *target_size);
HRESULT ml_convert(UINT from, UINT to, const BYTE *source, UINT *source_size, BYTE *target, UINT *target_size);

HRESULT ml_tag_to_lcid(const WCHAR *tag, LCID *out);
BOOL ml_lcid_to_tag(LCID lcid, const WCHAR **tag);
UINT ml_tag_count(void);
LCID ml_tag_lcid_at(UINT index);
void ml_tag_info(LCID lcid, RFC1766INFO *out);

/* One object answers every interface; the global font link object is the same shape with less on show. */
typedef struct
{
	const IMultiLanguageVtbl *language;
	const IMultiLanguage3Vtbl *language3;
	const IMLangFontLinkVtbl *link;
	const IMLangFontLink2Vtbl *link2;
	LONG refs;
	BOOL global;
} ml_object;

extern const IMLangFontLinkVtbl ml_link_vtbl;
extern const IMLangFontLink2Vtbl ml_link2_vtbl;

HRESULT ml_object_query(ml_object *self, REFIID riid, void **out);
ULONG ml_object_add_ref(ml_object *self);
ULONG ml_object_release(ml_object *self);
HRESULT ml_language_create(REFIID riid, void **out);
HRESULT ml_convert_charset_create(REFIID riid, void **out);

#endif
