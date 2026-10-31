/* SPDX-License-Identifier: MIT */
#include "ml.h"
#include <wchar.h>

/* Language tags and their locale identifiers, as the public locale tables list them. A plain language tag names the country
 * its language is first used in, except for the three families that keep a neutral identifier. */
static const struct
{
	LCID lcid;
	const WCHAR *tag;
} tags[] = {
	{0x0001, L"ar"}, {0x0401, L"ar-sa"}, {0x0801, L"ar-iq"}, {0x0c01, L"ar-eg"}, {0x1001, L"ar-ly"}, {0x1401, L"ar-dz"}, {0x1801, L"ar-ma"},
	{0x1c01, L"ar-tn"}, {0x2001, L"ar-om"}, {0x2401, L"ar-ye"}, {0x2801, L"ar-sy"}, {0x2c01, L"ar-jo"}, {0x3001, L"ar-lb"}, {0x3401, L"ar-kw"},
	{0x3801, L"ar-ae"}, {0x3c01, L"ar-bh"}, {0x4001, L"ar-qa"},
	{0x0402, L"bg"}, {0x0403, L"ca"},
	{0x0004, L"zh"}, {0x0404, L"zh-tw"}, {0x0804, L"zh-cn"}, {0x0c04, L"zh-hk"}, {0x1004, L"zh-sg"}, {0x1404, L"zh-mo"},
	{0x0405, L"cs"}, {0x0406, L"da"},
	{0x0407, L"de"}, {0x0807, L"de-ch"}, {0x0c07, L"de-at"}, {0x1007, L"de-lu"}, {0x1407, L"de-li"},
	{0x0408, L"el"},
	{0x0009, L"en"}, {0x0409, L"en-us"}, {0x0809, L"en-gb"}, {0x0c09, L"en-au"}, {0x1009, L"en-ca"}, {0x1409, L"en-nz"}, {0x1809, L"en-ie"},
	{0x1c09, L"en-za"}, {0x2009, L"en-jm"}, {0x2409, L"en"}, {0x2809, L"en-bz"}, {0x2c09, L"en-tt"},
	{0x040a, L"es"}, {0x080a, L"es-mx"}, {0x0c0a, L"es"}, {0x100a, L"es-gt"}, {0x140a, L"es-cr"}, {0x180a, L"es-pa"}, {0x1c0a, L"es-do"},
	{0x200a, L"es-ve"}, {0x240a, L"es-co"}, {0x280a, L"es-pe"}, {0x2c0a, L"es-ar"}, {0x300a, L"es-ec"}, {0x340a, L"es-cl"}, {0x380a, L"es-uy"},
	{0x3c0a, L"es-py"}, {0x400a, L"es-bo"}, {0x440a, L"es-sv"}, {0x480a, L"es-hn"}, {0x4c0a, L"es-ni"}, {0x500a, L"es-pr"},
	{0x040b, L"fi"},
	{0x040c, L"fr"}, {0x080c, L"fr-be"}, {0x0c0c, L"fr-ca"}, {0x100c, L"fr-ch"}, {0x140c, L"fr-lu"}, {0x180c, L"fr-mc"},
	{0x040d, L"he"}, {0x040e, L"hu"}, {0x040f, L"is"},
	{0x0410, L"it"}, {0x0810, L"it-ch"},
	{0x0411, L"ja"}, {0x0412, L"ko"},
	{0x0413, L"nl"}, {0x0813, L"nl-be"},
	{0x0414, L"no"}, {0x0814, L"no"},
	{0x0415, L"pl"}, {0x0416, L"pt-br"}, {0x0816, L"pt"},
	{0x0417, L"rm"}, {0x0418, L"ro"}, {0x0818, L"ro-mo"}, {0x0419, L"ru"}, {0x0819, L"ru-mo"},
	{0x041a, L"hr"}, {0x081a, L"sr"}, {0x0c1a, L"sr"},
	{0x041b, L"sk"}, {0x041c, L"sq"},
	{0x041d, L"sv"}, {0x081d, L"sv-fi"},
	{0x041e, L"th"}, {0x041f, L"tr"}, {0x0420, L"ur"}, {0x0421, L"id"}, {0x0422, L"uk"}, {0x0423, L"be"}, {0x0424, L"sl"}, {0x0425, L"et"},
	{0x0426, L"lv"}, {0x0427, L"lt"}, {0x0429, L"fa"}, {0x042a, L"vi"}, {0x042b, L"hy"}, {0x042c, L"az"}, {0x082c, L"az"}, {0x042d, L"eu"},
	{0x042e, L"sb"}, {0x042f, L"mk"}, {0x0430, L"sx"}, {0x0431, L"ts"}, {0x0432, L"tn"}, {0x0434, L"xh"}, {0x0435, L"zu"}, {0x0436, L"af"},
	{0x0437, L"ka"}, {0x0438, L"fo"}, {0x0439, L"hi"}, {0x043a, L"mt"}, {0x043c, L"gd"}, {0x043e, L"ms"}, {0x083e, L"ms"}, {0x043f, L"kk"},
	{0x0441, L"sw"}, {0x0443, L"uz"}, {0x0843, L"uz"}, {0x0444, L"tt"}, {0x0445, L"bn"}, {0x0446, L"pa"}, {0x0447, L"gu"}, {0x0449, L"ta"},
	{0x044a, L"te"}, {0x044b, L"kn"}, {0x044c, L"ml"}, {0x044e, L"mr"}, {0x044f, L"sa"}, {0x0457, L"kok"},
};

static WCHAR lower_ascii(WCHAR letter)
{
	return letter >= 'A' && letter <= 'Z' ? letter + 32 : letter;
}

static BOOL same_tag(const WCHAR *first, const WCHAR *second)
{
	for (; *first && *second; first++, second++)
		if (lower_ascii(*first) != lower_ascii(*second))
			return FALSE;
	return !*first && !*second;
}

HRESULT ml_tag_to_lcid(const WCHAR *tag, LCID *out)
{
	UINT index;
	for (index = 0; index < ARRAYSIZE(tags); index++)
		if (same_tag(tags[index].tag, tag))
		{
			*out = tags[index].lcid;
			return S_OK;
		}
	return E_FAIL;
}

BOOL ml_lcid_to_tag(LCID lcid, const WCHAR **tag)
{
	UINT index;
	for (index = 0; index < ARRAYSIZE(tags); index++)
		if (tags[index].lcid == lcid)
		{
			*tag = tags[index].tag;
			return TRUE;
		}
	return FALSE;
}

UINT ml_tag_count(void)
{
	return ARRAYSIZE(tags);
}

LCID ml_tag_lcid_at(UINT index)
{
	return tags[index].lcid;
}

/* The name comes in the system's own language: the locale data holds no others. */
void ml_tag_info(LCID lcid, RFC1766INFO *out)
{
	const WCHAR *tag = L"";
	UINT length;
	ml_lcid_to_tag(lcid, &tag);
	out->lcid = lcid;
	length = (UINT)wcslen(tag);
	memcpy(out->wszRfc1766, tag, (length + 1) * sizeof(WCHAR));
	if (!GetLocaleInfoW(lcid, LOCALE_SLANGUAGE, out->wszLocaleName, ARRAYSIZE(out->wszLocaleName)))
		memcpy(out->wszLocaleName, tag, (length + 1) * sizeof(WCHAR));
}

HRESULT WINAPI Rfc1766ToLcidW(LCID *out, LPCWSTR tag)
{
	if (!out || !tag)
		return E_INVALIDARG;
	return ml_tag_to_lcid(tag, out);
}

HRESULT WINAPI Rfc1766ToLcidA(LCID *out, LPCSTR tag)
{
	WCHAR wide[MAX_RFC1766_NAME + 1];
	if (!out || !tag)
		return E_INVALIDARG;
	if (!MultiByteToWideChar(CP_ACP, 0, tag, -1, wide, ARRAYSIZE(wide)))
		return E_INVALIDARG;
	return ml_tag_to_lcid(wide, out);
}

HRESULT WINAPI LcidToRfc1766W(LCID lcid, LPWSTR out, INT room)
{
	const WCHAR *tag;
	if (!out || room <= 0)
		return E_INVALIDARG;
	if (!ml_lcid_to_tag(lcid, &tag))
		return E_FAIL;
	if ((INT)wcslen(tag) >= room)
		return E_INVALIDARG;
	lstrcpyW(out, tag);
	return S_OK;
}

HRESULT WINAPI LcidToRfc1766A(LCID lcid, LPSTR out, INT room)
{
	WCHAR wide[MAX_RFC1766_NAME + 1];
	HRESULT status;
	if (!out || room <= 0)
		return E_INVALIDARG;
	status = LcidToRfc1766W(lcid, wide, ARRAYSIZE(wide));
	if (FAILED(status))
		return status;
	return WideCharToMultiByte(CP_ACP, 0, wide, -1, out, room, NULL, NULL) ? S_OK : E_INVALIDARG;
}
