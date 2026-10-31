/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "docfile.h"
#include <string.h>

/* Property sets in the format of the public [MS-OLEPS] specification. A set is kept in memory as a
 * list and written whole to its stream at a commit. */

#define HEADER_BYTES 28
#define SECTION_ENTRY_BYTES 20
#define NAME_CHARS 32

static const GUID summary_id = { 0xf29f85e0, 0x4ff9, 0x1068, { 0xab, 0x91, 0x08, 0x00, 0x2b, 0x27, 0xb3, 0xd9 } };
static const GUID document_id = { 0xd5cdd502, 0x2e9c, 0x101b, { 0x93, 0x97, 0x08, 0x00, 0x2b, 0x2c, 0xf9, 0xae } };
static const GUID user_id = { 0xd5cdd505, 0x2e9c, 0x101b, { 0x93, 0x97, 0x08, 0x00, 0x2b, 0x2c, 0xf9, 0xae } };

static const WCHAR summary_name[] = L"\005SummaryInformation";
static const WCHAR document_name[] = L"\005DocumentSummaryInformation";
static const char name_alphabet[] = "abcdefghijklmnopqrstuvwxyz012345";

HRESULT WINAPI FmtIdToPropStgName(const FMTID *id, WCHAR *name)
{
	const BYTE *bytes = (const BYTE *)id;
	int at, bit = 0;

	if (!id || !name)
		return E_INVALIDARG;
	if (IsEqualGUID(id, &summary_id)) {
		lstrcpyW(name, summary_name);
		return S_OK;
	}
	if (IsEqualGUID(id, &document_id) || IsEqualGUID(id, &user_id)) {
		lstrcpyW(name, document_name);
		return S_OK;
	}
	name[0] = 5;
	for (at = 1; at <= 26; at++, bit += 5) {
		int byte = bit / 8, shift = bit % 8;
		int value = bytes[byte] >> shift;
		if (shift > 3 && byte + 1 < 16)
			value |= bytes[byte + 1] << (8 - shift);
		name[at] = name_alphabet[value & 31];
	}
	name[27] = 0;
	return S_OK;
}

HRESULT WINAPI PropStgNameToFmtId(const LPOLESTR name, FMTID *id)
{
	BYTE *bytes = (BYTE *)id;
	int at, bit = 0;

	if (!name || !id)
		return E_INVALIDARG;
	if (name[0] != 5)
		return STG_E_INVALIDNAME;
	if (!lstrcmpiW(name, summary_name)) {
		*id = summary_id;
		return S_OK;
	}
	if (!lstrcmpiW(name, document_name)) {
		*id = document_id;
		return S_OK;
	}
	if (lstrlenW(name) != 27)
		return STG_E_INVALIDNAME;
	memset(id, 0, sizeof(*id));
	for (at = 1; at <= 26; at++, bit += 5) {
		WCHAR unit = name[at];
		int value, byte = bit / 8, shift = bit % 8;
		if (unit >= 'a' && unit <= 'z')
			value = unit - 'a';
		else if (unit >= 'A' && unit <= 'Z')
			value = unit - 'A';
		else if (unit >= '0' && unit <= '5')
			value = unit - '0' + 26;
		else
			return STG_E_INVALIDNAME;
		bytes[byte] |= (BYTE)(value << shift);
		if (shift > 3 && byte + 1 < 16)
			bytes[byte + 1] |= (BYTE)(value >> (8 - shift));
	}
	return S_OK;
}

typedef struct property {
	PROPID id;
	WCHAR *name;
	PROPVARIANT value;
	struct property *next;
} property;

typedef struct property_store {
	IPropertyStorage IPropertyStorage_iface;
	LONG refs;
	IStream *stream;
	IStorage *contents_owner;
	CRITICAL_SECTION lock;
	FMTID id;
	CLSID clsid;
	DWORD flags;
	DWORD mode;
	UINT code_page;
	LCID locale;
	DWORD behavior;
	BOOL has_locale;
	BOOL has_behavior;
	BOOL dirty;
	PROPID next_id;
	property *first;
	/* The other section of a stream holding two, kept as it was read. */
	FMTID other_id;
	BYTE *other_section;
	DWORD other_size;
} property_store;

static property_store *store_of(IPropertyStorage *iface)
{
	return CONTAINING_RECORD(iface, property_store, IPropertyStorage_iface);
}

static BYTE *task_copy(const void *data, SIZE_T size)
{
	BYTE *copy = CoTaskMemAlloc(size ? size : 1);

	if (copy && size)
		memcpy(copy, data, size);
	return copy;
}

/* Values: deep copies whose memory belongs to the COM allocator, so that PropVariantClear frees
 * what ReadMultiple returns. */

static SIZE_T fixed_size(VARTYPE type)
{
	switch (type) {
	case VT_I1:
	case VT_UI1:
		return 1;
	case VT_I2:
	case VT_UI2:
	case VT_BOOL:
		return 2;
	case VT_I4:
	case VT_UI4:
	case VT_INT:
	case VT_UINT:
	case VT_R4:
	case VT_ERROR:
		return 4;
	case VT_I8:
	case VT_UI8:
	case VT_R8:
	case VT_CY:
	case VT_DATE:
	case VT_FILETIME:
		return 8;
	case VT_CLSID:
		return 16;
	default:
		return 0;
	}
}

static HRESULT copy_string_vector(VARTYPE type, const PROPVARIANT *from, PROPVARIANT *to)
{
	ULONG at, count = from->calpstr.cElems;
	void **target = CoTaskMemAlloc((count ? count : 1) * sizeof(void *));

	if (!target)
		return E_OUTOFMEMORY;
	to->calpstr.cElems = count;
	to->calpstr.pElems = (char **)target;
	for (at = 0; at < count; at++) {
		const void *source = from->calpstr.pElems[at];
		SIZE_T size = type == VT_LPSTR ? strlen(source) + 1 : (lstrlenW(source) + 1) * sizeof(WCHAR);
		target[at] = task_copy(source, size);
	}
	return S_OK;
}

static HRESULT copy_value(const PROPVARIANT *from, PROPVARIANT *to)
{
	VARTYPE base = from->vt & ~VT_VECTOR;

	*to = *from;
	if (from->vt & VT_VECTOR) {
		if (base == VT_LPSTR || base == VT_LPWSTR)
			return copy_string_vector(base, from, to);
		if (fixed_size(base)) {
			to->cal.pElems = (LONG *)task_copy(from->cal.pElems, from->cal.cElems * fixed_size(base));
			return to->cal.pElems ? S_OK : E_OUTOFMEMORY;
		}
		return STG_E_INVALIDPARAMETER;
	}
	switch (from->vt) {
	case VT_LPSTR:
		to->pszVal = (char *)task_copy(from->pszVal, from->pszVal ? strlen(from->pszVal) + 1 : 0);
		break;
	case VT_LPWSTR:
		to->pwszVal = (WCHAR *)task_copy(from->pwszVal, from->pwszVal ? (lstrlenW(from->pwszVal) + 1) * sizeof(WCHAR) : 0);
		break;
	case VT_BSTR:
		to->bstrVal = SysAllocStringLen(from->bstrVal, SysStringLen(from->bstrVal));
		break;
	case VT_BLOB:
		to->blob.pBlobData = task_copy(from->blob.pBlobData, from->blob.cbSize);
		break;
	case VT_CLSID:
		to->puuid = (CLSID *)task_copy(from->puuid, sizeof(CLSID));
		break;
	case VT_CF:
		to->pclipdata = (CLIPDATA *)task_copy(from->pclipdata, sizeof(CLIPDATA));
		if (to->pclipdata)
			to->pclipdata->pClipData = task_copy(from->pclipdata->pClipData, from->pclipdata->cbSize - sizeof(ULONG));
		break;
	default:
		break;
	}
	return S_OK;
}

/* Writing out. */

struct writer {
	BYTE *data;
	SIZE_T size;
	SIZE_T capacity;
	BOOL failed;
};

static void out_bytes(struct writer *out, const void *data, SIZE_T count)
{
	if (out->size + count > out->capacity) {
		SIZE_T capacity = (out->size + count) * 2;
		BYTE *grown = mem_realloc(out->data, capacity);
		if (!grown) {
			out->failed = TRUE;
			return;
		}
		out->data = grown;
		out->capacity = capacity;
	}
	if (data)
		memcpy(out->data + out->size, data, count);
	else
		memset(out->data + out->size, 0, count);
	out->size += count;
}

static void out_u32(struct writer *out, DWORD value)
{
	out_bytes(out, &value, 4);
}

static void out_align(struct writer *out)
{
	if (out->size % 4)
		out_bytes(out, NULL, 4 - out->size % 4);
}

static void out_ansi_string(struct writer *out, const char *text, UINT code_page)
{
	if (!text)
		text = "";
	if (code_page == 1200) {
		int chars = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
		WCHAR *wide = mem_alloc((SIZE_T)chars * sizeof(WCHAR));
		if (wide) {
			MultiByteToWideChar(CP_ACP, 0, text, -1, wide, chars);
			out_u32(out, (DWORD)chars * 2);
			out_bytes(out, wide, (SIZE_T)chars * 2);
			mem_free(wide);
		}
	} else {
		DWORD bytes = (DWORD)strlen(text) + 1;
		out_u32(out, bytes);
		out_bytes(out, text, bytes);
	}
	out_align(out);
}

static void out_wide_string(struct writer *out, const WCHAR *text)
{
	DWORD chars;

	if (!text)
		text = L"";
	chars = (DWORD)lstrlenW(text) + 1;
	out_u32(out, chars);
	out_bytes(out, text, (SIZE_T)chars * 2);
	out_align(out);
}

static void out_bstr(struct writer *out, BSTR text, UINT code_page)
{
	if (code_page == 1200) {
		DWORD chars = text ? SysStringLen(text) : 0;
		out_u32(out, (chars + 1) * 2);
		out_bytes(out, text, (SIZE_T)chars * 2);
		out_bytes(out, NULL, 2);
		out_align(out);
	} else {
		int bytes = text ? WideCharToMultiByte(code_page, 0, text, -1, NULL, 0, NULL, NULL) : 0;
		char *ansi = mem_alloc(bytes);
		if (ansi) {
			if (bytes)
				WideCharToMultiByte(code_page, 0, text, -1, ansi, bytes, NULL, NULL);
			out_u32(out, (DWORD)bytes);
			out_bytes(out, ansi, bytes);
			mem_free(ansi);
		}
		out_align(out);
	}
}

static HRESULT out_value(struct writer *out, const PROPVARIANT *value, UINT code_page)
{
	VARTYPE base = value->vt & ~VT_VECTOR;

	out_u32(out, value->vt);
	if (value->vt & VT_VECTOR) {
		ULONG at, count = value->cal.cElems;
		out_u32(out, count);
		if (base == VT_LPSTR) {
			for (at = 0; at < count; at++)
				out_ansi_string(out, value->calpstr.pElems[at], code_page);
		} else if (base == VT_LPWSTR) {
			for (at = 0; at < count; at++)
				out_wide_string(out, value->calpwstr.pElems[at]);
		} else if (fixed_size(base)) {
			out_bytes(out, value->cal.pElems, count * fixed_size(base));
			out_align(out);
		} else {
			return STG_E_INVALIDPARAMETER;
		}
		return S_OK;
	}
	switch (value->vt) {
	case VT_EMPTY:
	case VT_NULL:
		break;
	case VT_LPSTR:
		out_ansi_string(out, value->pszVal, code_page);
		break;
	case VT_LPWSTR:
		out_wide_string(out, value->pwszVal);
		break;
	case VT_BSTR:
		out_bstr(out, value->bstrVal, code_page);
		break;
	case VT_BLOB:
		out_u32(out, value->blob.cbSize);
		out_bytes(out, value->blob.pBlobData, value->blob.cbSize);
		out_align(out);
		break;
	case VT_CF:
		out_u32(out, value->pclipdata->cbSize);
		out_u32(out, (DWORD)value->pclipdata->ulClipFmt);
		out_bytes(out, value->pclipdata->pClipData, value->pclipdata->cbSize - sizeof(ULONG));
		out_align(out);
		break;
	case VT_CLSID:
		out_bytes(out, value->puuid, sizeof(CLSID));
		break;
	default:
		if (!fixed_size(value->vt))
			return STG_E_INVALIDPARAMETER;
		out_bytes(out, &value->bVal, fixed_size(value->vt));
		out_align(out);
		break;
	}
	return S_OK;
}

static void out_dictionary(struct writer *out, const property_store *store, DWORD named)
{
	const property *item;

	out_u32(out, named);
	for (item = store->first; item; item = item->next) {
		if (!item->name)
			continue;
		out_u32(out, item->id);
		if (store->code_page == 1200) {
			DWORD chars = (DWORD)lstrlenW(item->name) + 1;
			out_u32(out, chars);
			out_bytes(out, item->name, (SIZE_T)chars * 2);
			out_align(out);
		} else {
			int bytes = WideCharToMultiByte(store->code_page, 0, item->name, -1, NULL, 0, NULL, NULL);
			char *ansi = mem_alloc(bytes);
			if (ansi) {
				WideCharToMultiByte(store->code_page, 0, item->name, -1, ansi, bytes, NULL, NULL);
				out_u32(out, (DWORD)bytes);
				out_bytes(out, ansi, bytes);
				mem_free(ansi);
			}
		}
	}
	out_align(out);
}

static HRESULT build_section(const property_store *store, struct writer *out)
{
	const property *item;
	DWORD named = 0, count = 1, *table, used = 0, at;
	SIZE_T start = out->size, table_at;
	HRESULT status = S_OK;

	for (item = store->first; item; item = item->next) {
		count++;
		if (item->name)
			named++;
	}
	if (named)
		count++;
	if (store->has_locale)
		count++;
	if (store->has_behavior)
		count++;
	table = mem_alloc((SIZE_T)count * 2 * sizeof(DWORD));
	if (!table)
		return E_OUTOFMEMORY;
	out_u32(out, 0);
	out_u32(out, count);
	table_at = out->size;
	out_bytes(out, NULL, (SIZE_T)count * 8);
#define NEXT_ENTRY(pid) (table[used * 2] = (pid), table[used * 2 + 1] = (DWORD)(out->size - start), used++)
	if (named) {
		NEXT_ENTRY(PID_DICTIONARY);
		out_dictionary(out, store, named);
	}
	NEXT_ENTRY(PID_CODEPAGE);
	out_u32(out, VT_I2);
	out_u32(out, store->code_page & 0xffff);
	if (store->has_locale) {
		NEXT_ENTRY(PID_LOCALE);
		out_u32(out, VT_UI4);
		out_u32(out, store->locale);
	}
	if (store->has_behavior) {
		NEXT_ENTRY(PID_BEHAVIOR);
		out_u32(out, VT_UI4);
		out_u32(out, store->behavior);
	}
	for (item = store->first; SUCCEEDED(status) && item; item = item->next) {
		NEXT_ENTRY(item->id);
		status = out_value(out, &item->value, store->code_page);
	}
#undef NEXT_ENTRY
	if (out->failed)
		status = E_OUTOFMEMORY;
	if (SUCCEEDED(status)) {
		*(DWORD *)(out->data + start) = (DWORD)(out->size - start);
		for (at = 0; at < used * 2; at++)
			memcpy(out->data + table_at + (SIZE_T)at * 4, &table[at], 4);
	}
	mem_free(table);
	return status;
}

static HRESULT store_flush(property_store *store)
{
	struct writer out = { 0 };
	BYTE header[HEADER_BYTES] = { 0 };
	LARGE_INTEGER origin;
	ULARGE_INTEGER size;
	DWORD sections = store->other_section ? 2 : 1, offset;
	BOOL document_first = store->other_section && IsEqualGUID(&store->other_id, &document_id);
	HRESULT status;
	ULONG written;

	if (!store->dirty)
		return S_OK;
	header[0] = 0xfe;
	header[1] = 0xff;
	header[2] = store->behavior || (store->flags & PROPSETFLAG_CASE_SENSITIVE) ? 1 : 0;
	header[4] = 6;
	header[6] = 2;
	memcpy(header + 8, &store->clsid, sizeof(CLSID));
	memcpy(header + 24, &sections, 4);
	out_bytes(&out, header, sizeof(header));
	offset = HEADER_BYTES + sections * SECTION_ENTRY_BYTES;
	if (document_first) {
		out_bytes(&out, &store->other_id, sizeof(FMTID));
		out_u32(&out, offset);
		out_bytes(&out, &store->id, sizeof(FMTID));
		out_u32(&out, offset + store->other_size);
		out_bytes(&out, store->other_section, store->other_size);
		status = build_section(store, &out);
	} else {
		SIZE_T second_at;
		out_bytes(&out, &store->id, sizeof(FMTID));
		out_u32(&out, offset);
		if (store->other_section) {
			out_bytes(&out, &store->other_id, sizeof(FMTID));
			out_u32(&out, 0);
		}
		second_at = out.size - 4;
		status = build_section(store, &out);
		if (SUCCEEDED(status) && store->other_section) {
			DWORD other_at = (DWORD)out.size;
			memcpy(out.data + second_at, &other_at, 4);
			out_bytes(&out, store->other_section, store->other_size);
		}
	}
	if (out.failed)
		status = E_OUTOFMEMORY;
	if (SUCCEEDED(status)) {
		origin.QuadPart = 0;
		size.QuadPart = out.size;
		status = IStream_Seek(store->stream, origin, STREAM_SEEK_SET, NULL);
		if (SUCCEEDED(status))
			status = IStream_SetSize(store->stream, size);
		if (SUCCEEDED(status))
			status = IStream_Write(store->stream, out.data, (ULONG)out.size, &written);
		if (SUCCEEDED(status))
			status = IStream_Commit(store->stream, STGC_DEFAULT);
	}
	mem_free(out.data);
	if (SUCCEEDED(status))
		store->dirty = FALSE;
	return status;
}

/* Reading in. Every read is checked against the end of the section. */

struct reader {
	const BYTE *data;
	SIZE_T size;
	SIZE_T at;
	BOOL failed;
};

static DWORD in_u32(struct reader *in)
{
	DWORD value = 0;

	if (in->at + 4 > in->size) {
		in->failed = TRUE;
		return 0;
	}
	memcpy(&value, in->data + in->at, 4);
	in->at += 4;
	return value;
}

static const BYTE *in_bytes(struct reader *in, SIZE_T count)
{
	const BYTE *at = in->data + in->at;

	if (count > in->size - in->at) {
		in->failed = TRUE;
		return NULL;
	}
	in->at += count;
	return at;
}

static void in_align(struct reader *in)
{
	if (in->at % 4)
		in->at += 4 - in->at % 4;
	if (in->at > in->size) {
		in->at = in->size;
	}
}

static char *in_ansi_string(struct reader *in, UINT code_page)
{
	DWORD bytes = in_u32(in);
	const BYTE *data = in_bytes(in, bytes);
	char *text;

	if (!data)
		return NULL;
	in_align(in);
	if (code_page == 1200) {
		int chars = bytes / 2;
		int size = WideCharToMultiByte(CP_ACP, 0, (const WCHAR *)data, chars, NULL, 0, NULL, NULL);
		text = CoTaskMemAlloc(size + 1);
		if (text) {
			WideCharToMultiByte(CP_ACP, 0, (const WCHAR *)data, chars, text, size, NULL, NULL);
			text[size] = 0;
		}
		return text;
	}
	text = CoTaskMemAlloc(bytes + 1);
	if (text) {
		memcpy(text, data, bytes);
		text[bytes] = 0;
	}
	return text;
}

static WCHAR *in_wide_string(struct reader *in)
{
	DWORD chars = in_u32(in);
	const BYTE *data = chars > 0x7fffffff ? NULL : in_bytes(in, (SIZE_T)chars * 2);
	WCHAR *text;

	if (!data)
		return NULL;
	in_align(in);
	text = CoTaskMemAlloc(((SIZE_T)chars + 1) * 2);
	if (text) {
		memcpy(text, data, (SIZE_T)chars * 2);
		text[chars] = 0;
	}
	return text;
}

static HRESULT in_value(struct reader *in, PROPVARIANT *value, UINT code_page)
{
	DWORD type = in_u32(in);
	VARTYPE base = (VARTYPE)type & ~VT_VECTOR;

	PropVariantInit(value);
	value->vt = (VARTYPE)type;
	if (type & VT_VECTOR) {
		DWORD at, count = in_u32(in);
		if (count > in->size)
			return STG_E_INVALIDHEADER;
		if (base == VT_LPSTR || base == VT_LPWSTR) {
			void **items = CoTaskMemAlloc((count ? count : 1) * sizeof(void *));
			if (!items)
				return E_OUTOFMEMORY;
			value->calpstr.cElems = count;
			value->calpstr.pElems = (char **)items;
			for (at = 0; at < count; at++)
				items[at] = base == VT_LPSTR ? (void *)in_ansi_string(in, code_page) : (void *)in_wide_string(in);
		} else if (fixed_size(base)) {
			const BYTE *data = in_bytes(in, count * fixed_size(base));
			if (!data)
				return STG_E_INVALIDHEADER;
			value->cal.cElems = count;
			value->cal.pElems = (LONG *)task_copy(data, count * fixed_size(base));
			in_align(in);
		} else {
			value->vt = VT_EMPTY;
			return STG_E_INVALIDHEADER;
		}
		return in->failed ? STG_E_INVALIDHEADER : S_OK;
	}
	switch (type) {
	case VT_EMPTY:
	case VT_NULL:
		break;
	case VT_LPSTR:
		value->pszVal = in_ansi_string(in, code_page);
		break;
	case VT_LPWSTR:
		value->pwszVal = in_wide_string(in);
		break;
	case VT_BSTR: {
		char *text = in_ansi_string(in, code_page);
		if (text) {
			int chars = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
			value->bstrVal = SysAllocStringLen(NULL, chars ? chars - 1 : 0);
			if (value->bstrVal)
				MultiByteToWideChar(CP_ACP, 0, text, -1, value->bstrVal, chars);
			CoTaskMemFree(text);
		}
		break;
	}
	case VT_BLOB: {
		DWORD bytes = in_u32(in);
		const BYTE *data = in_bytes(in, bytes);
		if (data) {
			value->blob.cbSize = bytes;
			value->blob.pBlobData = task_copy(data, bytes);
		}
		in_align(in);
		break;
	}
	case VT_CF: {
		DWORD bytes = in_u32(in);
		DWORD format = in_u32(in);
		const BYTE *data = bytes >= 4 ? in_bytes(in, bytes - 4) : NULL;
		if (data) {
			value->pclipdata = CoTaskMemAlloc(sizeof(CLIPDATA));
			if (value->pclipdata) {
				value->pclipdata->cbSize = bytes;
				value->pclipdata->ulClipFmt = (LONG)format;
				value->pclipdata->pClipData = task_copy(data, bytes - 4);
			}
		}
		in_align(in);
		break;
	}
	case VT_CLSID: {
		const BYTE *data = in_bytes(in, sizeof(CLSID));
		if (data)
			value->puuid = (CLSID *)task_copy(data, sizeof(CLSID));
		break;
	}
	default: {
		const BYTE *data;
		if (!fixed_size((VARTYPE)type)) {
			value->vt = VT_EMPTY;
			return STG_E_INVALIDHEADER;
		}
		data = in_bytes(in, fixed_size((VARTYPE)type));
		if (data)
			memcpy(&value->bVal, data, fixed_size((VARTYPE)type));
		in_align(in);
		break;
	}
	}
	return in->failed ? STG_E_INVALIDHEADER : S_OK;
}

static void property_free(property *item)
{
	PropVariantClear(&item->value);
	mem_free(item->name);
	mem_free(item);
}

static void store_clear(property_store *store)
{
	while (store->first) {
		property *item = store->first;
		store->first = item->next;
		property_free(item);
	}
}

static property *find_by_id(const property_store *store, PROPID id)
{
	property *item;

	for (item = store->first; item; item = item->next) {
		if (item->id == id)
			return item;
	}
	return NULL;
}

static property *find_by_name(const property_store *store, const WCHAR *name)
{
	property *item;

	for (item = store->first; item; item = item->next) {
		if (item->name && ((store->flags & PROPSETFLAG_CASE_SENSITIVE) ? !lstrcmpW(item->name, name) : !lstrcmpiW(item->name, name)))
			return item;
	}
	return NULL;
}

static property *add_property(property_store *store, PROPID id)
{
	property *item = mem_alloc(sizeof(*item)), **link = &store->first;

	if (!item)
		return NULL;
	item->id = id;
	PropVariantInit(&item->value);
	while (*link)
		link = &(*link)->next;
	*link = item;
	if (id >= store->next_id && id < PID_MIN_READONLY)
		store->next_id = id + 1;
	return item;
}

static void read_dictionary(property_store *store, const BYTE *section, SIZE_T size, DWORD offset)
{
	struct reader in = { section, size, offset, FALSE };
	DWORD count = in_u32(&in), at;

	for (at = 0; at < count && !in.failed; at++) {
		PROPID id = in_u32(&in);
		DWORD length = in_u32(&in);
		property *item = find_by_id(store, id);
		WCHAR *name;
		if (store->code_page == 1200) {
			const BYTE *data = in_bytes(&in, (SIZE_T)length * 2);
			if (!data)
				return;
			name = mem_alloc(((SIZE_T)length + 1) * 2);
			if (name)
				memcpy(name, data, (SIZE_T)length * 2);
			in_align(&in);
		} else {
			const BYTE *data = in_bytes(&in, length);
			int chars;
			if (!data)
				return;
			chars = MultiByteToWideChar(store->code_page, 0, (const char *)data, length, NULL, 0);
			name = mem_alloc(((SIZE_T)chars + 1) * 2);
			if (name)
				MultiByteToWideChar(store->code_page, 0, (const char *)data, length, name, chars);
		}
		if (item && name && !item->name)
			item->name = name;
		else
			mem_free(name);
	}
}

static HRESULT parse_section(property_store *store, const BYTE *section, SIZE_T size)
{
	struct reader table = { section, size, 0, FALSE };
	DWORD count, at, dictionary_at = 0;
	HRESULT status = S_OK;

	in_u32(&table);
	count = in_u32(&table);
	if (table.failed || count > size / 8)
		return STG_E_INVALIDHEADER;
	/* The code page has to be known before any string is read. */
	for (at = 0; at < count; at++) {
		PROPID id = in_u32(&table);
		DWORD offset = in_u32(&table);
		if (id == PID_CODEPAGE && offset + 8 <= size) {
			WORD code_page = 0;
			memcpy(&code_page, section + offset + 4, 2);
			store->code_page = code_page;
		}
	}
	table.at = 8;
	for (at = 0; SUCCEEDED(status) && at < count; at++) {
		PROPID id = in_u32(&table);
		DWORD offset = in_u32(&table);
		struct reader in = { section, size, offset, FALSE };
		PROPVARIANT value;
		property *item;
		if (table.failed || offset >= size)
			return STG_E_INVALIDHEADER;
		if (id == PID_DICTIONARY) {
			dictionary_at = offset;
			continue;
		}
		if (id == PID_CODEPAGE)
			continue;
		status = in_value(&in, &value, store->code_page);
		if (FAILED(status))
			break;
		if (id == PID_LOCALE) {
			store->locale = value.ulVal;
			store->has_locale = TRUE;
			continue;
		}
		if (id == PID_BEHAVIOR) {
			store->behavior = value.ulVal;
			store->has_behavior = TRUE;
			if (store->behavior & 1)
				store->flags |= PROPSETFLAG_CASE_SENSITIVE;
			continue;
		}
		item = add_property(store, id);
		if (!item) {
			PropVariantClear(&value);
			return E_OUTOFMEMORY;
		}
		item->value = value;
	}
	if (SUCCEEDED(status) && dictionary_at)
		read_dictionary(store, section, size, dictionary_at);
	return status;
}

static HRESULT store_load(property_store *store)
{
	STATSTG stat;
	LARGE_INTEGER origin;
	BYTE *data;
	ULONG got = 0;
	DWORD sections, offset, at;
	HRESULT status = IStream_Stat(store->stream, &stat, STATFLAG_NONAME);

	if (FAILED(status))
		return status;
	if (stat.cbSize.QuadPart > 0x7fffffff)
		return STG_E_INVALIDHEADER;
	data = mem_alloc((SIZE_T)stat.cbSize.QuadPart);
	if (!data)
		return E_OUTOFMEMORY;
	origin.QuadPart = 0;
	status = IStream_Seek(store->stream, origin, STREAM_SEEK_SET, NULL);
	if (SUCCEEDED(status))
		status = IStream_Read(store->stream, data, stat.cbSize.LowPart, &got);
	store_clear(store);
	store->next_id = PID_FIRST_USABLE;
	store->code_page = CP_ACP;
	if (SUCCEEDED(status) && (got < HEADER_BYTES + SECTION_ENTRY_BYTES || data[0] != 0xfe || data[1] != 0xff))
		status = STG_E_INVALIDHEADER;
	if (SUCCEEDED(status)) {
		SIZE_T found = 0;
		memcpy(&store->clsid, data + 8, sizeof(CLSID));
		memcpy(&sections, data + 24, 4);
		if (sections > 2 || HEADER_BYTES + (SIZE_T)sections * SECTION_ENTRY_BYTES > got)
			status = STG_E_INVALIDHEADER;
		/* With two sections pick the one asked for and keep the other aside. */
		for (at = 0; SUCCEEDED(status) && at < sections; at++) {
			const BYTE *entry = data + HEADER_BYTES + (SIZE_T)at * SECTION_ENTRY_BYTES;
			memcpy(&offset, entry + 16, 4);
			if (offset >= got || sections == 1 || IsEqualGUID((const GUID *)entry, &store->id)) {
				if (offset < got)
					found = offset;
				if (sections == 1 && offset < got) {
					memcpy(&store->id, entry, sizeof(FMTID));
				}
			}
		}
		if (SUCCEEDED(status) && !found)
			status = STG_E_INVALIDHEADER;
		if (SUCCEEDED(status)) {
			mem_free(store->other_section);
			store->other_section = NULL;
			for (at = 0; at < sections; at++) {
				const BYTE *entry = data + HEADER_BYTES + (SIZE_T)at * SECTION_ENTRY_BYTES;
				DWORD other, length;
				memcpy(&other, entry + 16, 4);
				if (other == found || other + 4 > got)
					continue;
				memcpy(&length, data + other, 4);
				if (length > got - other)
					continue;
				store->other_section = mem_alloc(length);
				if (store->other_section) {
					memcpy(store->other_section, data + other, length);
					store->other_size = length;
					memcpy(&store->other_id, entry, sizeof(FMTID));
				}
			}
			status = parse_section(store, data + found, got - found);
		}
	}
	mem_free(data);
	return status;
}

/* IPropertyStorage. */

static HRESULT WINAPI store_QueryInterface(IPropertyStorage *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPropertyStorage)) {
		*out = iface;
		IPropertyStorage_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI store_AddRef(IPropertyStorage *iface)
{
	return InterlockedIncrement(&store_of(iface)->refs);
}

static ULONG WINAPI store_Release(IPropertyStorage *iface)
{
	property_store *store = store_of(iface);
	ULONG refs = InterlockedDecrement(&store->refs);

	if (refs)
		return refs;
	if ((store->mode & 3) != STGM_READ)
		store_flush(store);
	store_clear(store);
	IStream_Release(store->stream);
	if (store->contents_owner) {
		IStorage_Commit(store->contents_owner, STGC_DEFAULT);
		IStorage_Release(store->contents_owner);
	}
	mem_free(store->other_section);
	DeleteCriticalSection(&store->lock);
	mem_free(store);
	return 0;
}

static property *locate(const property_store *store, const PROPSPEC *spec)
{
	if (spec->ulKind == PRSPEC_LPWSTR)
		return find_by_name(store, spec->lpwstr);
	return find_by_id(store, spec->propid);
}

static HRESULT WINAPI store_ReadMultiple(IPropertyStorage *iface, ULONG count, const PROPSPEC *specs, PROPVARIANT *values)
{
	property_store *store = store_of(iface);
	HRESULT status = S_FALSE;
	ULONG at;

	if (!count)
		return S_FALSE;
	if (!specs || !values)
		return E_INVALIDARG;
	EnterCriticalSection(&store->lock);
	for (at = 0; at < count; at++) {
		property *item;
		PropVariantInit(&values[at]);
		if (specs[at].ulKind == PRSPEC_PROPID && specs[at].propid == PID_CODEPAGE) {
			values[at].vt = VT_I2;
			values[at].iVal = (SHORT)store->code_page;
			status = S_OK;
			continue;
		}
		if (specs[at].ulKind == PRSPEC_PROPID && specs[at].propid == PID_LOCALE && store->has_locale) {
			values[at].vt = VT_UI4;
			values[at].ulVal = store->locale;
			status = S_OK;
			continue;
		}
		if (specs[at].ulKind == PRSPEC_PROPID && specs[at].propid == PID_BEHAVIOR && store->has_behavior) {
			values[at].vt = VT_UI4;
			values[at].ulVal = store->behavior;
			status = S_OK;
			continue;
		}
		item = locate(store, &specs[at]);
		if (item) {
			HRESULT copied = copy_value(&item->value, &values[at]);
			if (FAILED(copied)) {
				status = copied;
				break;
			}
			status = S_OK;
		}
	}
	LeaveCriticalSection(&store->lock);
	return status;
}

static HRESULT replace_value(property *item, const PROPVARIANT *value)
{
	PROPVARIANT copy;
	HRESULT status = copy_value(value, &copy);

	if (FAILED(status))
		return status;
	PropVariantClear(&item->value);
	item->value = copy;
	return S_OK;
}

static HRESULT write_special(property_store *store, const PROPSPEC *spec, const PROPVARIANT *value)
{
	switch (spec->propid) {
	case PID_CODEPAGE:
		if (value->vt != VT_I2 && value->vt != VT_UI2)
			return STG_E_INVALIDPARAMETER;
		if (store->first)
			return STG_E_INVALIDPARAMETER;
		store->code_page = (WORD)value->iVal;
		return S_OK;
	case PID_LOCALE:
		if (value->vt != VT_I4 && value->vt != VT_UI4)
			return STG_E_INVALIDPARAMETER;
		store->locale = value->ulVal;
		store->has_locale = TRUE;
		return S_OK;
	case PID_BEHAVIOR:
		if (value->vt != VT_I4 && value->vt != VT_UI4)
			return STG_E_INVALIDPARAMETER;
		store->behavior = value->ulVal;
		store->has_behavior = TRUE;
		return S_OK;
	default:
		return STG_E_INVALIDPARAMETER;
	}
}

static HRESULT WINAPI store_WriteMultiple(IPropertyStorage *iface, ULONG count, const PROPSPEC *specs, const PROPVARIANT *values, PROPID first_name_id)
{
	property_store *store = store_of(iface);
	HRESULT status = S_OK;
	ULONG at;

	if (!count)
		return S_OK;
	if (!specs || !values)
		return E_INVALIDARG;
	if ((store->mode & 3) == STGM_READ)
		return STG_E_ACCESSDENIED;
	EnterCriticalSection(&store->lock);
	for (at = 0; SUCCEEDED(status) && at < count; at++) {
		const PROPSPEC *spec = &specs[at];
		property *item;
		if (spec->ulKind != PRSPEC_LPWSTR && spec->ulKind != PRSPEC_PROPID) {
			status = STG_E_INVALIDPARAMETER;
			break;
		}
		if (spec->ulKind == PRSPEC_PROPID) {
			if (spec->propid == PID_DICTIONARY || spec->propid == PID_ILLEGAL) {
				status = STG_E_INVALIDPARAMETER;
			} else if (spec->propid == PID_CODEPAGE || spec->propid == PID_LOCALE || spec->propid == PID_BEHAVIOR) {
				status = write_special(store, spec, &values[at]);
			} else if (spec->propid >= PID_MIN_READONLY) {
				status = STG_E_INVALIDPARAMETER;
			} else {
				item = find_by_id(store, spec->propid);
				if (!item)
					item = add_property(store, spec->propid);
				status = item ? replace_value(item, &values[at]) : E_OUTOFMEMORY;
			}
		} else {
			item = find_by_name(store, spec->lpwstr);
			if (!item) {
				if (first_name_id < PID_FIRST_USABLE) {
					status = STG_E_INVALIDPARAMETER;
					break;
				}
				if (first_name_id > store->next_id)
					store->next_id = first_name_id;
				item = add_property(store, store->next_id);
				if (item) {
					SIZE_T bytes = (lstrlenW(spec->lpwstr) + 1) * sizeof(WCHAR);
					item->name = mem_alloc(bytes);
					if (item->name)
						memcpy(item->name, spec->lpwstr, bytes);
				}
			}
			status = item ? replace_value(item, &values[at]) : E_OUTOFMEMORY;
		}
		store->dirty = TRUE;
	}
	LeaveCriticalSection(&store->lock);
	return status;
}

static void unlink_property(property_store *store, property *item)
{
	property **link = &store->first;

	while (*link != item)
		link = &(*link)->next;
	*link = item->next;
	property_free(item);
}

static HRESULT WINAPI store_DeleteMultiple(IPropertyStorage *iface, ULONG count, const PROPSPEC *specs)
{
	property_store *store = store_of(iface);
	HRESULT status = S_OK;
	ULONG at;

	if (!count)
		return S_OK;
	if (!specs)
		return E_INVALIDARG;
	if ((store->mode & 3) == STGM_READ)
		return STG_E_ACCESSDENIED;
	EnterCriticalSection(&store->lock);
	for (at = 0; at < count; at++) {
		property *item;
		if (specs[at].ulKind != PRSPEC_LPWSTR && specs[at].ulKind != PRSPEC_PROPID) {
			status = STG_E_INVALIDPARAMETER;
			continue;
		}
		if (specs[at].ulKind == PRSPEC_PROPID && specs[at].propid <= PID_CODEPAGE) {
			status = STG_E_INVALIDPARAMETER;
			continue;
		}
		item = locate(store, &specs[at]);
		if (item) {
			unlink_property(store, item);
			store->dirty = TRUE;
		}
	}
	LeaveCriticalSection(&store->lock);
	return status;
}

static HRESULT WINAPI store_ReadPropertyNames(IPropertyStorage *iface, ULONG count, const PROPID *ids, WCHAR **names)
{
	property_store *store = store_of(iface);
	HRESULT status = S_FALSE;
	ULONG at;

	if (!count)
		return S_FALSE;
	if (!ids || !names)
		return E_INVALIDARG;
	EnterCriticalSection(&store->lock);
	for (at = 0; at < count; at++) {
		property *item = find_by_id(store, ids[at]);
		names[at] = NULL;
		if (item && item->name) {
			names[at] = (WCHAR *)task_copy(item->name, (lstrlenW(item->name) + 1) * sizeof(WCHAR));
			status = S_OK;
		}
	}
	LeaveCriticalSection(&store->lock);
	return status;
}

static HRESULT WINAPI store_WritePropertyNames(IPropertyStorage *iface, ULONG count, const PROPID *ids, const LPOLESTR names[])
{
	property_store *store = store_of(iface);
	HRESULT status = S_OK;
	ULONG at;

	if (!count)
		return S_OK;
	if (!ids || !names)
		return E_INVALIDARG;
	if ((store->mode & 3) == STGM_READ)
		return STG_E_ACCESSDENIED;
	EnterCriticalSection(&store->lock);
	for (at = 0; SUCCEEDED(status) && at < count; at++) {
		SIZE_T bytes = (lstrlenW(names[at]) + 1) * sizeof(WCHAR);
		property *item = find_by_id(store, ids[at]);
		WCHAR *copy;
		if (!item)
			item = add_property(store, ids[at]);
		copy = mem_alloc(bytes);
		if (!item || !copy) {
			mem_free(copy);
			status = E_OUTOFMEMORY;
			break;
		}
		memcpy(copy, names[at], bytes);
		mem_free(item->name);
		item->name = copy;
		store->dirty = TRUE;
	}
	LeaveCriticalSection(&store->lock);
	return status;
}

static HRESULT WINAPI store_DeletePropertyNames(IPropertyStorage *iface, ULONG count, const PROPID *ids)
{
	property_store *store = store_of(iface);
	ULONG at;

	if (!count)
		return S_OK;
	if (!ids)
		return E_INVALIDARG;
	EnterCriticalSection(&store->lock);
	for (at = 0; at < count; at++) {
		property *item = find_by_id(store, ids[at]);
		if (item && item->name) {
			mem_free(item->name);
			item->name = NULL;
			store->dirty = TRUE;
		}
	}
	LeaveCriticalSection(&store->lock);
	return S_OK;
}

static HRESULT WINAPI store_Commit(IPropertyStorage *iface, DWORD flags)
{
	property_store *store = store_of(iface);
	HRESULT status;

	if ((store->mode & 3) == STGM_READ)
		return STG_E_ACCESSDENIED;
	EnterCriticalSection(&store->lock);
	status = store_flush(store);
	LeaveCriticalSection(&store->lock);
	return status;
}

static HRESULT WINAPI store_Revert(IPropertyStorage *iface)
{
	property_store *store = store_of(iface);
	HRESULT status = S_OK;

	EnterCriticalSection(&store->lock);
	if (store->dirty) {
		status = store_load(store);
		/* A set never committed reads back as empty. */
		if (status == STG_E_INVALIDHEADER) {
			store_clear(store);
			status = S_OK;
		}
		store->dirty = FALSE;
	}
	LeaveCriticalSection(&store->lock);
	return status;
}

typedef struct property_enum {
	IEnumSTATPROPSTG IEnumSTATPROPSTG_iface;
	LONG refs;
	IPropertyStorage *owner;
	STATPROPSTG *items;
	ULONG count;
	ULONG at;
} property_enum;

static const IEnumSTATPROPSTGVtbl property_enum_vtable;

static property_enum *property_enum_of(IEnumSTATPROPSTG *iface)
{
	return CONTAINING_RECORD(iface, property_enum, IEnumSTATPROPSTG_iface);
}

static HRESULT WINAPI property_enum_QueryInterface(IEnumSTATPROPSTG *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumSTATPROPSTG)) {
		*out = iface;
		IEnumSTATPROPSTG_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI property_enum_AddRef(IEnumSTATPROPSTG *iface)
{
	return InterlockedIncrement(&property_enum_of(iface)->refs);
}

static ULONG WINAPI property_enum_Release(IEnumSTATPROPSTG *iface)
{
	property_enum *object = property_enum_of(iface);
	ULONG refs = InterlockedDecrement(&object->refs);

	if (!refs) {
		ULONG at;
		for (at = 0; at < object->count; at++)
			mem_free(object->items[at].lpwstrName);
		mem_free(object->items);
		IPropertyStorage_Release(object->owner);
		mem_free(object);
	}
	return refs;
}

static HRESULT WINAPI property_enum_Next(IEnumSTATPROPSTG *iface, ULONG count, STATPROPSTG *out, ULONG *fetched)
{
	property_enum *object = property_enum_of(iface);
	ULONG done = 0;

	if (fetched)
		*fetched = 0;
	if (!out)
		return E_INVALIDARG;
	while (done < count && object->at < object->count) {
		out[done] = object->items[object->at++];
		if (out[done].lpwstrName)
			out[done].lpwstrName = (WCHAR *)task_copy(out[done].lpwstrName, (lstrlenW(out[done].lpwstrName) + 1) * sizeof(WCHAR));
		done++;
	}
	if (fetched)
		*fetched = done;
	return done == count ? S_OK : S_FALSE;
}

static HRESULT WINAPI property_enum_Skip(IEnumSTATPROPSTG *iface, ULONG count)
{
	property_enum *object = property_enum_of(iface);
	ULONG left = object->count - object->at;

	object->at += count < left ? count : left;
	return count <= left ? S_OK : S_FALSE;
}

static HRESULT WINAPI property_enum_Reset(IEnumSTATPROPSTG *iface)
{
	property_enum_of(iface)->at = 0;
	return S_OK;
}

static HRESULT property_enum_new(property_store *store, const property_enum *source, IEnumSTATPROPSTG **out)
{
	property_enum *object = mem_alloc(sizeof(*object));
	const property *item;
	ULONG count = 0;

	if (!object)
		return E_OUTOFMEMORY;
	for (item = store->first; item; item = item->next)
		count++;
	object->items = mem_alloc((SIZE_T)(count + 1) * sizeof(STATPROPSTG));
	if (!object->items) {
		mem_free(object);
		return E_OUTOFMEMORY;
	}
	for (item = store->first; item; item = item->next) {
		STATPROPSTG *entry = &object->items[object->count++];
		entry->propid = item->id;
		entry->vt = item->value.vt;
		if (item->name) {
			SIZE_T bytes = (lstrlenW(item->name) + 1) * sizeof(WCHAR);
			entry->lpwstrName = mem_alloc(bytes);
			if (entry->lpwstrName)
				memcpy(entry->lpwstrName, item->name, bytes);
		}
	}
	if (source)
		object->at = source->at < object->count ? source->at : object->count;
	object->IEnumSTATPROPSTG_iface.lpVtbl = &property_enum_vtable;
	object->refs = 1;
	object->owner = &store->IPropertyStorage_iface;
	IPropertyStorage_AddRef(object->owner);
	*out = &object->IEnumSTATPROPSTG_iface;
	return S_OK;
}

static HRESULT WINAPI property_enum_Clone(IEnumSTATPROPSTG *iface, IEnumSTATPROPSTG **out)
{
	property_enum *object = property_enum_of(iface);

	if (!out)
		return E_INVALIDARG;
	return property_enum_new(store_of(object->owner), object, out);
}

static const IEnumSTATPROPSTGVtbl property_enum_vtable = {
	property_enum_QueryInterface, property_enum_AddRef, property_enum_Release, property_enum_Next, property_enum_Skip,
	property_enum_Reset, property_enum_Clone,
};

static HRESULT WINAPI store_Enum(IPropertyStorage *iface, IEnumSTATPROPSTG **out)
{
	property_store *store = store_of(iface);
	HRESULT status;

	if (!out)
		return E_INVALIDARG;
	EnterCriticalSection(&store->lock);
	status = property_enum_new(store, NULL, out);
	LeaveCriticalSection(&store->lock);
	return status;
}

static HRESULT WINAPI store_SetTimes(IPropertyStorage *iface, const FILETIME *created, const FILETIME *accessed, const FILETIME *modified)
{
	return S_OK;
}

static HRESULT WINAPI store_SetClass(IPropertyStorage *iface, REFCLSID clsid)
{
	property_store *store = store_of(iface);

	if (!clsid)
		return E_INVALIDARG;
	if ((store->mode & 3) == STGM_READ)
		return STG_E_ACCESSDENIED;
	store->clsid = *clsid;
	store->dirty = TRUE;
	return S_OK;
}

static HRESULT WINAPI store_Stat(IPropertyStorage *iface, STATPROPSETSTG *stat)
{
	property_store *store = store_of(iface);
	STATSTG stream_stat;
	HRESULT status;

	if (!stat)
		return E_INVALIDARG;
	status = IStream_Stat(store->stream, &stream_stat, STATFLAG_NONAME);
	if (FAILED(status))
		return status;
	memset(stat, 0, sizeof(*stat));
	stat->fmtid = store->id;
	stat->clsid = store->clsid;
	stat->grfFlags = store->flags;
	stat->mtime = stream_stat.mtime;
	stat->ctime = stream_stat.ctime;
	stat->atime = stream_stat.atime;
	stat->dwOSVersion = 0x00020006;
	return S_OK;
}

static const IPropertyStorageVtbl store_vtable = {
	store_QueryInterface, store_AddRef, store_Release, store_ReadMultiple, store_WriteMultiple, store_DeleteMultiple,
	store_ReadPropertyNames, store_WritePropertyNames, store_DeletePropertyNames, store_Commit, store_Revert, store_Enum,
	store_SetTimes, store_SetClass, store_Stat,
};

static HRESULT store_new(IStream *stream, const FMTID *id, const CLSID *clsid, DWORD flags, DWORD mode, BOOL create, property_store **result)
{
	property_store *store = mem_alloc(sizeof(*store));
	HRESULT status = S_OK;

	if (!store)
		return E_OUTOFMEMORY;
	store->IPropertyStorage_iface.lpVtbl = &store_vtable;
	store->refs = 1;
	InitializeCriticalSection(&store->lock);
	store->id = *id;
	store->mode = mode;
	store->flags = flags;
	store->next_id = PID_FIRST_USABLE;
	store->code_page = flags & PROPSETFLAG_ANSI ? GetACP() : 1200;
	IStream_AddRef(stream);
	store->stream = stream;
	if (clsid)
		store->clsid = *clsid;
	if (create) {
		store->dirty = TRUE;
		if (flags & PROPSETFLAG_CASE_SENSITIVE) {
			store->behavior = 1;
			store->has_behavior = TRUE;
		}
	} else {
		status = store_load(store);
		store->id = *id;
	}
	if (FAILED(status)) {
		IPropertyStorage_Release(&store->IPropertyStorage_iface);
		return status;
	}
	*result = store;
	return S_OK;
}

HRESULT WINAPI StgCreatePropStg(IUnknown *owner, REFFMTID id, const CLSID *clsid, DWORD flags, DWORD reserved, IPropertyStorage **out)
{
	property_store *store;
	IStream *stream;
	HRESULT status;

	if (!owner || !id || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (reserved)
		return STG_E_INVALIDPARAMETER;
	if (flags & PROPSETFLAG_NONSIMPLE)
		return STG_E_INVALIDPARAMETER;
	status = IUnknown_QueryInterface(owner, &IID_IStream, (void **)&stream);
	if (FAILED(status))
		return status;
	status = store_new(stream, id, clsid, flags, STGM_READWRITE, TRUE, &store);
	IStream_Release(stream);
	if (SUCCEEDED(status))
		*out = &store->IPropertyStorage_iface;
	return status;
}

HRESULT WINAPI StgOpenPropStg(IUnknown *owner, REFFMTID id, DWORD flags, DWORD reserved, IPropertyStorage **out)
{
	property_store *store;
	IStream *stream;
	HRESULT status;

	if (!owner || !id || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (reserved)
		return STG_E_INVALIDPARAMETER;
	if (flags & PROPSETFLAG_NONSIMPLE)
		return STG_E_INVALIDPARAMETER;
	status = IUnknown_QueryInterface(owner, &IID_IStream, (void **)&stream);
	if (FAILED(status))
		return status;
	status = store_new(stream, id, NULL, flags, STGM_READWRITE, FALSE, &store);
	IStream_Release(stream);
	if (SUCCEEDED(status))
		*out = &store->IPropertyStorage_iface;
	return status;
}

/* IPropertySetStorage. */

typedef struct property_set {
	IPropertySetStorage IPropertySetStorage_iface;
	LONG refs;
	IStorage *storage;
} property_set;

static property_set *set_of(IPropertySetStorage *iface)
{
	return CONTAINING_RECORD(iface, property_set, IPropertySetStorage_iface);
}

static HRESULT WINAPI set_QueryInterface(IPropertySetStorage *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualIID(riid, &IID_IPropertySetStorage) || IsEqualIID(riid, &IID_IUnknown)) {
		*out = iface;
		IPropertySetStorage_AddRef(iface);
		return S_OK;
	}
	return IStorage_QueryInterface(set_of(iface)->storage, riid, out);
}

static ULONG WINAPI set_AddRef(IPropertySetStorage *iface)
{
	return InterlockedIncrement(&set_of(iface)->refs);
}

static ULONG WINAPI set_Release(IPropertySetStorage *iface)
{
	property_set *object = set_of(iface);
	ULONG refs = InterlockedDecrement(&object->refs);

	if (!refs) {
		IStorage_Release(object->storage);
		mem_free(object);
	}
	return refs;
}

static HRESULT WINAPI set_Create(IPropertySetStorage *iface, REFFMTID id, const CLSID *clsid, DWORD flags, DWORD mode, IPropertyStorage **out)
{
	property_set *object = set_of(iface);
	WCHAR name[NAME_CHARS];
	property_store *store;
	IStorage *contents = NULL;
	IStream *stream;
	HRESULT status;

	if (!id || !out)
		return E_INVALIDARG;
	*out = NULL;
	if ((mode & ~STGM_CREATE) != (STGM_READWRITE | STGM_SHARE_EXCLUSIVE))
		return STG_E_INVALIDFLAG;
	status = FmtIdToPropStgName(id, name);
	if (FAILED(status))
		return status;
	if (flags & PROPSETFLAG_NONSIMPLE) {
		status = IStorage_CreateStorage(object->storage, name, STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE | STGM_TRANSACTED, 0, 0, &contents);
		if (SUCCEEDED(status))
			status = IStorage_CreateStream(contents, L"CONTENTS", STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &stream);
	} else {
		status = IStorage_CreateStream(object->storage, name, STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &stream);
	}
	if (FAILED(status)) {
		if (contents)
			IStorage_Release(contents);
		return status;
	}
	status = store_new(stream, id, clsid, flags, mode & ~STGM_CREATE, TRUE, &store);
	IStream_Release(stream);
	if (FAILED(status)) {
		if (contents)
			IStorage_Release(contents);
		return status;
	}
	store->contents_owner = contents;
	*out = &store->IPropertyStorage_iface;
	return S_OK;
}

static HRESULT WINAPI set_Open(IPropertySetStorage *iface, REFFMTID id, DWORD mode, IPropertyStorage **out)
{
	property_set *object = set_of(iface);
	WCHAR name[NAME_CHARS];
	property_store *store;
	IStorage *contents = NULL;
	IStream *stream;
	DWORD flags = 0;
	HRESULT status;

	if (!id || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (mode != (STGM_READWRITE | STGM_SHARE_EXCLUSIVE) && mode != (STGM_READ | STGM_SHARE_EXCLUSIVE))
		return STG_E_INVALIDFLAG;
	status = FmtIdToPropStgName(id, name);
	if (FAILED(status))
		return status;
	status = IStorage_OpenStream(object->storage, name, NULL, mode, 0, &stream);
	if (status == STG_E_FILENOTFOUND) {
		status = IStorage_OpenStorage(object->storage, name, NULL, mode | STGM_TRANSACTED, NULL, 0, &contents);
		if (SUCCEEDED(status)) {
			status = IStorage_OpenStream(contents, L"CONTENTS", NULL, mode, 0, &stream);
			flags = PROPSETFLAG_NONSIMPLE;
		}
	}
	if (FAILED(status)) {
		if (contents)
			IStorage_Release(contents);
		return status;
	}
	status = store_new(stream, id, NULL, flags, mode, FALSE, &store);
	IStream_Release(stream);
	if (FAILED(status)) {
		if (contents)
			IStorage_Release(contents);
		return status;
	}
	if (store->code_page != 1200)
		store->flags |= PROPSETFLAG_ANSI;
	store->contents_owner = contents;
	*out = &store->IPropertyStorage_iface;
	return S_OK;
}

static HRESULT WINAPI set_Delete(IPropertySetStorage *iface, REFFMTID id)
{
	WCHAR name[NAME_CHARS];
	HRESULT status;

	if (!id)
		return E_INVALIDARG;
	status = FmtIdToPropStgName(id, name);
	if (FAILED(status))
		return status;
	return IStorage_DestroyElement(set_of(iface)->storage, name);
}

typedef struct set_enum {
	IEnumSTATPROPSETSTG IEnumSTATPROPSETSTG_iface;
	LONG refs;
	IEnumSTATSTG *names;
	IStorage *storage;
} set_enum;

static const IEnumSTATPROPSETSTGVtbl set_enum_vtable;

static set_enum *set_enum_of(IEnumSTATPROPSETSTG *iface)
{
	return CONTAINING_RECORD(iface, set_enum, IEnumSTATPROPSETSTG_iface);
}

static HRESULT WINAPI set_enum_QueryInterface(IEnumSTATPROPSETSTG *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumSTATPROPSETSTG)) {
		*out = iface;
		IEnumSTATPROPSETSTG_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI set_enum_AddRef(IEnumSTATPROPSETSTG *iface)
{
	return InterlockedIncrement(&set_enum_of(iface)->refs);
}

static ULONG WINAPI set_enum_Release(IEnumSTATPROPSETSTG *iface)
{
	set_enum *object = set_enum_of(iface);
	ULONG refs = InterlockedDecrement(&object->refs);

	if (!refs) {
		IEnumSTATSTG_Release(object->names);
		IStorage_Release(object->storage);
		mem_free(object);
	}
	return refs;
}

static HRESULT WINAPI set_enum_Next(IEnumSTATPROPSETSTG *iface, ULONG count, STATPROPSETSTG *out, ULONG *fetched)
{
	set_enum *object = set_enum_of(iface);
	ULONG done = 0;

	if (fetched)
		*fetched = 0;
	if (!out)
		return E_INVALIDARG;
	while (done < count) {
		STATSTG stat;
		FMTID id;
		if (IEnumSTATSTG_Next(object->names, 1, &stat, NULL) != S_OK)
			break;
		if (SUCCEEDED(PropStgNameToFmtId(stat.pwcsName, &id))) {
			IPropertySetStorage *sets;
			IPropertyStorage *store;
			memset(&out[done], 0, sizeof(out[done]));
			out[done].fmtid = id;
			out[done].mtime = stat.mtime;
			out[done].ctime = stat.ctime;
			out[done].atime = stat.atime;
			if (SUCCEEDED(IStorage_QueryInterface(object->storage, &IID_IPropertySetStorage, (void **)&sets))) {
				if (SUCCEEDED(IPropertySetStorage_Open(sets, &id, STGM_READ | STGM_SHARE_EXCLUSIVE, &store))) {
					STATPROPSETSTG info;
					if (SUCCEEDED(IPropertyStorage_Stat(store, &info))) {
						out[done].clsid = info.clsid;
						out[done].grfFlags = info.grfFlags;
					}
					IPropertyStorage_Release(store);
				}
				IPropertySetStorage_Release(sets);
			}
			done++;
		}
		CoTaskMemFree(stat.pwcsName);
	}
	if (fetched)
		*fetched = done;
	return done == count ? S_OK : S_FALSE;
}

static HRESULT WINAPI set_enum_Skip(IEnumSTATPROPSETSTG *iface, ULONG count)
{
	STATPROPSETSTG ignored;

	while (count) {
		HRESULT status = IEnumSTATPROPSETSTG_Next(iface, 1, &ignored, NULL);
		if (status != S_OK)
			return S_FALSE;
		count--;
	}
	return S_OK;
}

static HRESULT WINAPI set_enum_Reset(IEnumSTATPROPSETSTG *iface)
{
	return IEnumSTATSTG_Reset(set_enum_of(iface)->names);
}

static HRESULT WINAPI set_enum_Clone(IEnumSTATPROPSETSTG *iface, IEnumSTATPROPSETSTG **out)
{
	set_enum *object = set_enum_of(iface), *clone;
	HRESULT status;

	if (!out)
		return E_INVALIDARG;
	clone = mem_alloc(sizeof(*clone));
	if (!clone)
		return E_OUTOFMEMORY;
	status = IEnumSTATSTG_Clone(object->names, &clone->names);
	if (FAILED(status)) {
		mem_free(clone);
		return status;
	}
	clone->IEnumSTATPROPSETSTG_iface.lpVtbl = &set_enum_vtable;
	clone->refs = 1;
	clone->storage = object->storage;
	IStorage_AddRef(clone->storage);
	*out = &clone->IEnumSTATPROPSETSTG_iface;
	return S_OK;
}

static const IEnumSTATPROPSETSTGVtbl set_enum_vtable = {
	set_enum_QueryInterface, set_enum_AddRef, set_enum_Release, set_enum_Next, set_enum_Skip, set_enum_Reset, set_enum_Clone,
};

static HRESULT WINAPI set_Enum(IPropertySetStorage *iface, IEnumSTATPROPSETSTG **out)
{
	property_set *object = set_of(iface);
	set_enum *created;
	HRESULT status;

	if (!out)
		return E_INVALIDARG;
	created = mem_alloc(sizeof(*created));
	if (!created)
		return E_OUTOFMEMORY;
	status = IStorage_EnumElements(object->storage, 0, NULL, 0, &created->names);
	if (FAILED(status)) {
		mem_free(created);
		return status;
	}
	created->IEnumSTATPROPSETSTG_iface.lpVtbl = &set_enum_vtable;
	created->refs = 1;
	created->storage = object->storage;
	IStorage_AddRef(created->storage);
	*out = &created->IEnumSTATPROPSETSTG_iface;
	return S_OK;
}

static const IPropertySetStorageVtbl set_vtable = {
	set_QueryInterface, set_AddRef, set_Release, set_Create, set_Open, set_Delete, set_Enum,
};

HRESULT property_set_create(IStorage *storage, IPropertySetStorage **result)
{
	property_set *object = mem_alloc(sizeof(*object));

	if (!object)
		return E_OUTOFMEMORY;
	object->IPropertySetStorage_iface.lpVtbl = &set_vtable;
	object->refs = 1;
	object->storage = storage;
	IStorage_AddRef(storage);
	*result = &object->IPropertySetStorage_iface;
	return S_OK;
}

HRESULT WINAPI StgCreatePropSetStg(IStorage *storage, DWORD reserved, IPropertySetStorage **out)
{
	if (!storage || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (reserved)
		return STG_E_INVALIDPARAMETER;
	return property_set_create(storage, out);
}
