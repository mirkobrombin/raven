/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "urlmon_private.h"

static const WCHAR mime_plain[] = L"text/plain", mime_binary[] = L"application/octet-stream", mime_html[] = L"text/html", mime_xml[] = L"text/xml";

typedef BOOL (*signature_test)(const BYTE *data, DWORD size);

static BOOL has_prefix(const BYTE *data, DWORD size, const char *prefix, DWORD length, DWORD required)
{
	return size >= required && !memcmp(data, prefix, length);
}

static char lower_byte(BYTE value)
{
	return value >= 'A' && value <= 'Z' ? (char)(value + 32) : (char)value;
}

static BOOL is_jpeg(const BYTE *data, DWORD size)
{
	return has_prefix(data, size, "\xff\xd8", 2, 3);
}

static BOOL is_gif(const BYTE *data, DWORD size)
{
	if (size < 6 || lower_byte(data[0]) != 'g' || lower_byte(data[1]) != 'i' || lower_byte(data[2]) != 'f' || data[3] != '8')
		return FALSE;
	return (data[4] == '7' || data[4] == '9') && lower_byte(data[5]) == 'a';
}

static BOOL is_bitmap(const BYTE *data, DWORD size)
{
	return size >= 14 && data[0] == 'B' && data[1] == 'M' && !data[6] && !data[7] && !data[8] && !data[9];
}

static BOOL is_png(const BYTE *data, DWORD size)
{
	return has_prefix(data, size, "\x89PNG\r\n\x1a\n", 8, 8);
}

static BOOL is_tiff(const BYTE *data, DWORD size)
{
	return has_prefix(data, size, "MM\0*", 4, 5) || has_prefix(data, size, "II*", 3, 4);
}

static BOOL is_riff(const BYTE *data, DWORD size, const char *kind)
{
	return size > 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, kind, 4);
}

static BOOL is_avi(const BYTE *data, DWORD size)
{
	return is_riff(data, size, "AVI ");
}

static BOOL is_wave(const BYTE *data, DWORD size)
{
	return is_riff(data, size, "WAVE");
}

static BOOL is_mpeg(const BYTE *data, DWORD size)
{
	return size > 4 && !data[0] && !data[1] && data[2] == 1 && (data[3] == 0xb3 || data[3] == 0xba);
}

static BOOL is_gzip(const BYTE *data, DWORD size)
{
	return has_prefix(data, size, "\x1f\x8b", 2, 3);
}

static BOOL is_zip(const BYTE *data, DWORD size)
{
	return has_prefix(data, size, "PK", 2, 3);
}

static BOOL is_java(const BYTE *data, DWORD size)
{
	return has_prefix(data, size, "\xca\xfe\xba\xbe", 4, 5);
}

static BOOL is_pdf(const BYTE *data, DWORD size)
{
	return has_prefix(data, size, "%PDF", 4, 5);
}

static BOOL is_executable(const BYTE *data, DWORD size)
{
	return has_prefix(data, size, "MZ", 2, 3);
}

static BOOL is_rich_text(const BYTE *data, DWORD size)
{
	return has_prefix(data, size, "{\\rtf", 5, 6);
}

static BOOL is_postscript(const BYTE *data, DWORD size)
{
	return has_prefix(data, size, "%!", 2, 3);
}

static BOOL is_sound(const BYTE *data, DWORD size)
{
	return has_prefix(data, size, ".snd", 4, 5);
}

static const struct
{
	signature_test test;
	const WCHAR *mime;
} signatures[] = {
	{is_gif, L"image/gif"}, {is_jpeg, L"image/pjpeg"}, {is_bitmap, L"image/bmp"}, {is_png, L"image/x-png"}, {is_tiff, L"image/tiff"},
	{is_avi, L"video/avi"}, {is_wave, L"audio/wav"}, {is_mpeg, L"video/mpeg"}, {is_gzip, L"application/x-gzip-compressed"},
	{is_zip, L"application/x-zip-compressed"}, {is_java, L"application/java"}, {is_pdf, L"application/pdf"},
	{is_executable, L"application/x-msdownload"}, {is_rich_text, L"text/richtext"}, {is_postscript, L"application/postscript"},
	{is_sound, L"audio/basic"},
};

/* Whichever markup appears first decides; a tag must be followed by something to count. */
static const WCHAR *markup_of(const BYTE *data, DWORD size)
{
	static const struct
	{
		const char *tag;
		DWORD length, trailing;
		const WCHAR *mime;
	} tags[] = {{"<html", 5, 1, mime_html}, {"<head", 5, 1, mime_html}, {"<body", 5, 1, mime_html}, {"<title", 6, 1, mime_html}, {"<?xml", 5, 2, mime_xml}};
	DWORD at, tag, index;
	for (at = 0; at < size; at++)
	{
		if (data[at] != '<')
			continue;
		for (tag = 0; tag < ARRAYSIZE(tags); tag++)
		{
			if (at + tags[tag].length + tags[tag].trailing > size)
				continue;
			for (index = 0; index < tags[tag].length; index++)
				if (lower_byte(data[at + index]) != tags[tag].tag[index])
					break;
			if (index == tags[tag].length)
				return tags[tag].mime;
		}
	}
	return NULL;
}

static const WCHAR *signature_of(const BYTE *data, DWORD size)
{
	DWORD index;
	for (index = 0; index < ARRAYSIZE(signatures); index++)
		if (signatures[index].test(data, size))
			return signatures[index].mime;
	return NULL;
}

/* Control characters mean binary; the last byte is not held against the data. */
static const WCHAR *generic_of(const BYTE *data, DWORD size)
{
	DWORD at;
	for (at = 0; at + 1 < size; at++)
		if (data[at] < 0x20 && data[at] != '\t' && data[at] != '\n' && data[at] != '\r')
			return mime_binary;
	return mime_plain;
}

static BOOL is_generic(const WCHAR *mime)
{
	return !lstrcmpiW(mime, mime_plain) || !lstrcmpiW(mime, mime_binary);
}

/* A type sniffing can recognise by itself: an extension cannot overrule what sniffing said of it. */
static BOOL is_sniffable(const WCHAR *mime)
{
	DWORD index;
	if (is_generic(mime) || !lstrcmpiW(mime, mime_html) || !lstrcmpiW(mime, mime_xml) || !lstrcmpiW(mime, L"image/jpeg"))
		return TRUE;
	for (index = 0; index < ARRAYSIZE(signatures); index++)
		if (!lstrcmpiW(mime, signatures[index].mime))
			return TRUE;
	return FALSE;
}

/* Windows keeps these in HKCR; a prefix may not have them yet. */
static const WCHAR *extension_fallback(const WCHAR *extension)
{
	static const struct
	{
		const WCHAR *extension, *mime;
	} known[] = {
		{L".htm", mime_html}, {L".html", mime_html}, {L".xml", mime_xml}, {L".txt", mime_plain}, {L".css", L"text/css"}, {L".js", L"application/javascript"},
		{L".gif", L"image/gif"}, {L".jpg", L"image/jpeg"}, {L".jpeg", L"image/jpeg"}, {L".png", L"image/png"}, {L".bmp", L"image/bmp"}, {L".tif", L"image/tiff"},
		{L".pdf", L"application/pdf"}, {L".zip", L"application/x-zip-compressed"}, {L".exe", L"application/x-msdownload"}, {L".dll", L"application/x-msdownload"},
		{L".wav", L"audio/wav"}, {L".avi", L"video/avi"}, {L".mpg", L"video/mpeg"}, {L".mpeg", L"video/mpeg"},
	};
	DWORD index;
	for (index = 0; index < ARRAYSIZE(known); index++)
		if (!lstrcmpiW(extension, known[index].extension))
			return known[index].mime;
	return NULL;
}

/* The content type recorded for the file name at the end of a URL's path. */
static WCHAR *mime_of_url(const WCHAR *url, HRESULT *failure)
{
	WCHAR *copy, *extension, *at, value[128];
	const WCHAR *name, *fallback;
	DWORD size = sizeof(value), type = 0;
	WCHAR *result = NULL;
	HKEY key;
	copy = uri_text_copy(url);
	if (!copy)
	{
		*failure = E_OUTOFMEMORY;
		return NULL;
	}
	*failure = E_FAIL;
	for (at = copy; *at; at++)
		if (*at == L'?' || *at == L'#')
		{
			*at = 0;
			break;
		}
	name = copy;
	for (at = copy; *at; at++)
		if (*at == L'/' || *at == L'\\')
			name = at + 1;
	extension = NULL;
	for (at = (WCHAR *)name; *at; at++)
		if (*at == L'.')
			extension = at;
	if (!extension)
	{
		*failure = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
		if (!wcschr(copy, L'/') || wcsstr(copy, L"%"))
			*failure = E_FAIL;
		uri_free(copy);
		return NULL;
	}
	if (!RegOpenKeyExW(HKEY_CLASSES_ROOT, extension, 0, KEY_READ, &key))
	{
		if (!RegQueryValueExW(key, L"Content Type", NULL, &type, (BYTE *)value, &size) && type == REG_SZ)
			result = uri_text_copy(value);
		RegCloseKey(key);
	}
	if (!result)
	{
		fallback = extension_fallback(extension);
		if (fallback)
			result = uri_text_copy(fallback);
		else
			*failure = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
	}
	uri_free(copy);
	return result;
}

static HRESULT return_mime(const WCHAR *mime, WCHAR **out)
{
	SIZE_T size = (wcslen(mime) + 1) * sizeof(WCHAR);
	*out = CoTaskMemAlloc(size);
	if (!*out)
		return E_OUTOFMEMORY;
	memcpy(*out, mime, size);
	return S_OK;
}

/* A proposal carrying parameters is known by what stands before them. */
static HRESULT return_proposal(const WCHAR *proposed, WCHAR **out)
{
	const WCHAR *end = wcschr(proposed, L';');
	WCHAR *bare;
	HRESULT result;
	if (!end)
		return return_mime(proposed, out);
	bare = uri_text_range(proposed, end);
	if (!bare)
		return E_OUTOFMEMORY;
	result = return_mime(bare, out);
	uri_free(bare);
	return result;
}

HRESULT WINAPI FindMimeFromData(LPBC context, LPCWSTR url, LPVOID buffer, DWORD size, LPCWSTR proposed, DWORD flags, LPWSTR *out, DWORD reserved)
{
	const BYTE *data = buffer;
	const WCHAR *found = NULL;
	WCHAR *from_url;
	HRESULT result, failure;
	if (!out || (!url && !buffer))
		return E_INVALIDARG;
	if (!data || !size)
	{
		if (proposed)
			return return_proposal(proposed, out);
		if (!data && url)
		{
			from_url = mime_of_url(url, &failure);
			if (!from_url)
				return failure;
			result = return_mime(from_url, out);
			uri_free(from_url);
			return result;
		}
		return E_FAIL;
	}
	/* An image proposal is only ever overruled by another image signature, not by markup. */
	if (proposed && !wcsncmp(proposed, L"image/", 6))
	{
		found = signature_of(data, size);
		if (!found)
		{
			found = generic_of(data, size);
			if (found == mime_binary)
				found = proposed;
		}
		return return_mime(found, out);
	}
	found = markup_of(data, size);
	if (!found)
		found = signature_of(data, size);
	if (!found)
		found = generic_of(data, size);
	if (!is_generic(found))
		return return_mime(found, out);
	if (proposed && !is_generic(proposed))
		return return_proposal(proposed, out);
	if (url)
	{
		from_url = mime_of_url(url, &failure);
		if (from_url)
		{
			if (!is_sniffable(from_url))
				found = from_url;
			result = return_mime(found, out);
			uri_free(from_url);
			return result;
		}
	}
	return return_mime(found, out);
}

HRESULT WINAPI GetClassFileOrMime(LPBC context, LPCWSTR name, LPVOID buffer, DWORD size, LPCWSTR mime, DWORD reserved, CLSID *clsid)
{
	WCHAR key_name[256], value[64];
	DWORD length = sizeof(value), type = 0;
	HKEY key;
	LONG status;
	if (!clsid || !mime || wcslen(mime) > 200)
		return E_INVALIDARG;
	lstrcpyW(key_name, L"MIME\\Database\\Content Type\\");
	lstrcatW(key_name, mime);
	if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key_name, 0, KEY_READ, &key))
		return MK_E_CANTOPENFILE;
	status = RegQueryValueExW(key, L"CLSID", NULL, &type, (BYTE *)value, &length);
	RegCloseKey(key);
	if (status || type != REG_SZ)
		return MK_E_CANTOPENFILE;
	return CLSIDFromString(value, clsid);
}
