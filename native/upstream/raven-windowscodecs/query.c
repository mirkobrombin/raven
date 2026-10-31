/* SPDX-License-Identifier: MIT */
#include "wic.h"

/* A query reader stands at one place in the metadata: the root, a list of blocks, or inside one block's reader. A path
   walks down from there one segment at a time: [index], an optional schema with a colon, then a name or {type=value}. */
typedef struct
{
	IWICMetadataQueryWriter iface;
	LONG references;
	BOOL writable;
	IWICMetadataBlockReader *blocks;
	IWICMetadataReader *reader;
	WCHAR *location;
	GUID container;
} query;

static HRESULT query_make(IWICMetadataBlockReader *blocks, IWICMetadataReader *reader, const WCHAR *location, const GUID *container, query **out);

static query *query_of(IWICMetadataQueryWriter *iface)
{
	return WIC_CONTAINING(iface, query, iface);
}

typedef struct
{
	BOOL any_index;
	UINT index;
	PROPVARIANT schema, id;
	WCHAR *name;
	BOOL plain, has_index, escaped, schema_named, name_is_format;
} segment;

static void segment_clear(segment *part)
{
	PropVariantClear(&part->schema);
	PropVariantClear(&part->id);
	CoTaskMemFree(part->name);
}

/* One token up to the next delimiter, with the escapes taken out. A brace ends only at its own closing brace. */
static const WCHAR *read_token(const WCHAR *at, BOOL brace, WCHAR **out)
{
	const WCHAR *start = at;
	WCHAR *to;
	UINT length = 0;
	while (*at && (brace ? *at != '}' : *at != '/' && *at != ':')) {
		if (*at == '\\' && at[1])
			at++;
		at++;
		length++;
	}
	if (!(to = *out = CoTaskMemAlloc((length + 1) * sizeof(WCHAR))))
		return NULL;
	for (at = start; *at && (brace ? *at != '}' : *at != '/' && *at != ':'); at++) {
		if (*at == '\\' && at[1])
			at++;
		*to++ = *at;
	}
	*to = 0;
	return at;
}

static const struct
{
	const WCHAR *name;
	VARTYPE type;
} type_names[] = {
	{L"char", VT_I1}, {L"uchar", VT_UI1}, {L"short", VT_I2}, {L"ushort", VT_UI2}, {L"long", VT_I4}, {L"ulong", VT_UI4}, {L"int", VT_I4}, {L"uint", VT_UI4},
	{L"longlong", VT_I8}, {L"ulonglong", VT_UI8}, {L"float", VT_R4}, {L"double", VT_R8}, {L"str", VT_LPSTR}, {L"wstr", VT_LPWSTR}, {L"xmpstruct", VT_UNKNOWN},
	{L"xmpbag", VT_UNKNOWN}, {L"xmpseq", VT_UNKNOWN}, {L"xmpalt", VT_UNKNOWN}, {L"bool", VT_BOOL}, {L"guid", VT_CLSID},
};

static BOOL read_number(const WCHAR *text, BOOL *negative, ULONGLONG *number)
{
	*negative = *text == '-';
	if (*negative)
		text++;
	if (!*text)
		return FALSE;
	for (*number = 0; *text; text++) {
		if (*text < '0' || *text > '9')
			return FALSE;
		*number = *number * 10 + (*text - '0');
	}
	return TRUE;
}

static WCHAR *text_dup(const WCHAR *text)
{
	const SIZE_T size = (lstrlenW(text) + 1) * sizeof(WCHAR);
	WCHAR *copy = CoTaskMemAlloc(size);
	if (copy)
		memcpy(copy, text, size);
	return copy;
}

/* {type=value}; an empty brace asks for an item with no id at all. */
static const WCHAR *read_braced(const WCHAR *at, PROPVARIANT *out, HRESULT *result)
{
	WCHAR *type, *split;
	UINT index;
	ULONGLONG number;
	BOOL negative;
	at++;
	/* The type is taken as written: an escape in it makes a name no type has. */
	{
		const WCHAR *equals = at;
		SIZE_T length;
		while (*equals && *equals != '=' && *equals != '}')
			equals++;
		if (*equals == '}' && equals == at)
			return equals + 1;
		if (*equals != '=') {
			*result = WINCODEC_ERR_WRONGSTATE;
			return NULL;
		}
		length = (SIZE_T)(equals - at);
		if (!(type = CoTaskMemAlloc((length + 1) * sizeof(WCHAR)))) {
			*result = E_OUTOFMEMORY;
			return NULL;
		}
		memcpy(type, at, length * sizeof(WCHAR));
		type[length] = 0;
		if (!(at = read_token(equals + 1, TRUE, &split))) {
			CoTaskMemFree(type);
			*result = E_OUTOFMEMORY;
			return NULL;
		}
		if (*at != '}') {
			CoTaskMemFree(type);
			CoTaskMemFree(split);
			*result = WINCODEC_ERR_WRONGSTATE;
			return NULL;
		}
		at++;
	}
	for (index = 0; index < ARRAYSIZE(type_names); index++)
		if (!lstrcmpiW(type, type_names[index].name))
			break;
	CoTaskMemFree(type);
	if (index == ARRAYSIZE(type_names)) {
		*result = WINCODEC_ERR_WRONGSTATE;
		CoTaskMemFree(split);
		return NULL;
	}
	out->vt = type_names[index].type;
	if (out->vt == VT_LPWSTR)
		out->pwszVal = text_dup(split);
	else if (out->vt == VT_LPSTR) {
		const int bytes = WideCharToMultiByte(CP_ACP, 0, split, -1, NULL, 0, NULL, NULL);
		if ((out->pszVal = CoTaskMemAlloc(bytes)))
			WideCharToMultiByte(CP_ACP, 0, split, -1, out->pszVal, bytes, NULL, NULL);
	}
	else if (out->vt == VT_CLSID) {
		/* The opening brace of a guid would close the type's, so it comes escaped or not at all. */
		GUID *guid = CoTaskMemAlloc(sizeof(*guid));
		WCHAR braced[48] = L"{";
		lstrcpynW(braced + 1, split[0] == '{' ? split + 1 : split, 40);
		lstrcatW(braced, L"}");
		if (!guid || FAILED(CLSIDFromString(braced, guid))) {
			CoTaskMemFree(guid);
			out->vt = VT_EMPTY;
			*result = DISP_E_TYPEMISMATCH;
			CoTaskMemFree(split);
			return NULL;
		}
		out->puuid = guid;
	}
	else if (out->vt == VT_UNKNOWN)
		out->vt = VT_EMPTY;
	else if (!read_number(split, &negative, &number)) {
		out->vt = VT_EMPTY;
		*result = DISP_E_TYPEMISMATCH;
		CoTaskMemFree(split);
		return NULL;
	}
	else
		out->uhVal.QuadPart = negative ? (ULONGLONG)-(LONGLONG)number : number;
	CoTaskMemFree(split);
	return at;
}

static const WCHAR *read_segment(const WCHAR *at, segment *part, HRESULT *result)
{
	memset(part, 0, sizeof(*part));
	*result = S_OK;
	if (*at == '[') {
		at++;
		part->has_index = TRUE;
		if (*at == '*') {
			part->any_index = TRUE;
			at++;
		}
		else {
			if (*at < '0' || *at > '9') {
				*result = WINCODEC_ERR_INVALIDQUERYCHARACTER;
				return NULL;
			}
			for (; *at >= '0' && *at <= '9'; at++)
				part->index = part->index * 10 + (*at - '0');
		}
		if (*at != ']') {
			*result = WINCODEC_ERR_INVALIDQUERYCHARACTER;
			return NULL;
		}
		at++;
	}
	if (*at == '{') {
		if (!(at = read_braced(at, &part->id, result)))
			return NULL;
	}
	else {
		WCHAR *name;
		part->escaped = *at == '\\';
		if (!(at = read_token(at, FALSE, &name))) {
			*result = E_OUTOFMEMORY;
			return NULL;
		}
		part->name = name;
		part->plain = TRUE;
	}
	if (*at == ':') {
		/* What was read is the schema, by its short name or written in full. */
		if (part->plain) {
			part->schema.vt = VT_LPWSTR;
			part->schema.pwszVal = text_dup(part->name);
			part->schema_named = TRUE;
			CoTaskMemFree(part->name);
			part->name = NULL;
			part->plain = FALSE;
			part->escaped = FALSE;
		}
		else {
			part->schema = part->id;
			memset(&part->id, 0, sizeof(part->id));
		}
		at++;
		if (*at == '{') {
			if (!(at = read_braced(at, &part->id, result)))
				return NULL;
		}
		else {
			WCHAR *name;
			if (!(at = read_token(at, FALSE, &name))) {
				*result = E_OUTOFMEMORY;
				return NULL;
			}
			part->name = name;
			part->plain = TRUE;
		}
	}
	if (part->plain) {
		part->name_is_format = TRUE;
		part->id.vt = VT_LPWSTR;
		part->id.pwszVal = text_dup(part->name);
	}
	return at;
}

/* The nth reader of a block list with the format the segment names. */
static HRESULT find_block(IWICMetadataBlockReader *blocks, const segment *part, IWICMetadataReader **out)
{
	GUID wanted;
	UINT count = 0, index, seen = 0;
	if (part->id.vt == VT_CLSID)
		wanted = *part->id.puuid;
	else if (!part->plain || FAILED(WICMapShortNameToGuid(part->name, &wanted)))
		return WINCODEC_ERR_PROPERTYNOTFOUND;
	IWICMetadataBlockReader_GetCount(blocks, &count);
	for (index = 0; index < count; index++) {
		IWICMetadataReader *reader;
		GUID format;
		if (FAILED(IWICMetadataBlockReader_GetReaderByIndex(blocks, index, &reader)))
			continue;
		if (SUCCEEDED(IWICMetadataReader_GetMetadataFormat(reader, &format)) && IsEqualGUID(&format, &wanted) && seen++ == part->index) {
			*out = reader;
			return S_OK;
		}
		IWICMetadataReader_Release(reader);
	}
	return WINCODEC_ERR_PROPERTYNOTFOUND;
}

/* Paths also name a TIFF container by its short name, which the format map does not know. */
static BOOL name_to_format(const WCHAR *name, GUID *format)
{
	if (!lstrcmpW(name, L"tiff")) {
		*format = GUID_ContainerFormatTiff;
		return TRUE;
	}
	return SUCCEEDED(WICMapShortNameToGuid(name, format));
}

/* Inside a block a name of a format asks for the block held there; any other name is an item. A schema written as a name
   stands for its address only where the format has such names. */
static HRESULT prepare_ids(IWICMetadataReader *reader, segment *part)
{
	GUID format, *wanted;
	/* A path that stops at the slash asks for the item without an id. */
	if (part->plain && !part->schema_named && part->name && !*part->name) {
		PropVariantClear(&part->id);
		return S_OK;
	}
	if (part->name && part->name_is_format && part->schema.vt == VT_EMPTY && !part->escaped && name_to_format(part->name, &format)) {
		if (!(wanted = CoTaskMemAlloc(sizeof(*wanted))))
			return E_OUTOFMEMORY;
		*wanted = format;
		PropVariantClear(&part->id);
		part->id.vt = VT_CLSID;
		part->id.puuid = wanted;
	}
	else if (part->has_index)
		return E_INVALIDARG;
	if (part->schema_named && SUCCEEDED(IWICMetadataReader_GetMetadataFormat(reader, &format))
		&& (IsEqualGUID(&format, &GUID_MetadataFormatXMP) || IsEqualGUID(&format, &GUID_MetadataFormatXMPStruct))) {
		const WCHAR *address = wic_schema_of_name(part->schema.pwszVal);
		if (address) {
			WCHAR *copy = text_dup(address);
			CoTaskMemFree(part->schema.pwszVal);
			part->schema.pwszVal = copy;
		}
	}
	return S_OK;
}

static HRESULT read_from(IWICMetadataReader *reader, segment *part, PROPVARIANT *found)
{
	HRESULT result;
	if (FAILED(result = prepare_ids(reader, part)))
		return result;
	return IWICMetadataReader_GetValue(reader, &part->schema, &part->id, found);
}

static HRESULT WINAPI query_QueryInterface(IWICMetadataQueryWriter *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICMetadataQueryReader) || (query_of(iface)->writable && IsEqualGUID(riid, &IID_IWICMetadataQueryWriter))) {
		*out = iface;
		IWICMetadataQueryWriter_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI query_AddRef(IWICMetadataQueryWriter *iface)
{
	return InterlockedIncrement(&query_of(iface)->references);
}

static ULONG WINAPI query_Release(IWICMetadataQueryWriter *iface)
{
	query *self = query_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		if (self->blocks)
			IWICMetadataBlockReader_Release(self->blocks);
		if (self->reader)
			IWICMetadataReader_Release(self->reader);
		CoTaskMemFree(self->location);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI query_GetContainerFormat(IWICMetadataQueryWriter *iface, GUID *format)
{
	if (!format)
		return E_INVALIDARG;
	*format = query_of(iface)->container;
	return S_OK;
}

static HRESULT WINAPI query_GetLocation(IWICMetadataQueryWriter *iface, UINT room, WCHAR *out, UINT *actual)
{
	const WCHAR *location = query_of(iface)->location;
	const UINT length = lstrlenW(location) + 1;
	if (!actual)
		return E_INVALIDARG;
	if (out) {
		if (room < length)
			return WINCODEC_ERR_INSUFFICIENTBUFFER;
		memcpy(out, location, length * sizeof(WCHAR));
	}
	*actual = length;
	return S_OK;
}

static WCHAR *join_location(const WCHAR *base, const WCHAR *start, const WCHAR *end)
{
	const UINT base_length = !lstrcmpW(base, L"/") ? 0 : lstrlenW(base), added = (UINT)(end - start);
	WCHAR *joined = CoTaskMemAlloc((base_length + added + 1) * sizeof(WCHAR));
	if (joined) {
		memcpy(joined, base, base_length * sizeof(WCHAR));
		memcpy(joined + base_length, start, added * sizeof(WCHAR));
		joined[base_length + added] = 0;
	}
	return joined;
}

static HRESULT WINAPI query_GetMetadataByName(IWICMetadataQueryWriter *iface, const WCHAR *name, PROPVARIANT *value)
{
	query *self = query_of(iface);
	IWICMetadataReader *current = self->reader;
	const WCHAR *at = name;
	HRESULT result = S_OK;
	if (!name)
		return E_INVALIDARG;
	if (*at != '/')
		return WINCODEC_ERR_PROPERTYNOTSUPPORTED;
	if (current)
		IWICMetadataReader_AddRef(current);
	while (*at == '/') {
		segment part;
		IWICMetadataReader *next = NULL;
		if (!(at = read_segment(at + 1, &part, &result))) {
			segment_clear(&part);
			break;
		}
		if (*at && *at != '/') {
			segment_clear(&part);
			result = WINCODEC_ERR_INVALIDQUERYCHARACTER;
			at = NULL;
			break;
		}
		if (part.any_index && current) {
			segment_clear(&part);
			result = WINCODEC_ERR_REQUESTONLYVALIDATMETADATAROOT;
			at = NULL;
			break;
		}
		if (!current) {
			result = self->blocks ? find_block(self->blocks, &part, &next) : WINCODEC_ERR_PROPERTYNOTFOUND;
			segment_clear(&part);
		}
		else {
			PROPVARIANT found;
			PropVariantInit(&found);
			result = read_from(current, &part, &found);
			segment_clear(&part);
			if (SUCCEEDED(result) && found.vt == VT_UNKNOWN && found.punkVal
				&& SUCCEEDED(IUnknown_QueryInterface(found.punkVal, &IID_IWICMetadataReader, (void **)&next)))
				PropVariantClear(&found);
			else if (SUCCEEDED(result)) {
				/* An item ends the path; asking below it finds nothing. */
				IWICMetadataReader_Release(current);
				if (*at) {
					PropVariantClear(&found);
					return WINCODEC_ERR_PROPERTYNOTFOUND;
				}
				if (value)
					*value = found;
				else
					PropVariantClear(&found);
				return S_OK;
			}
		}
		if (FAILED(result)) {
			at = NULL;
			break;
		}
		if (current)
			IWICMetadataReader_Release(current);
		current = next;
	}
	if (!at || FAILED(result)) {
		if (current)
			IWICMetadataReader_Release(current);
		return result;
	}
	/* The path ended on a block: the answer is a query reader standing there. */
	{
		WCHAR *location = join_location(self->location, name, name + lstrlenW(name));
		query *nested = NULL;
		GUID format = self->container;
		if (current)
			IWICMetadataReader_GetMetadataFormat(current, &format);
		if (location && value && current && SUCCEEDED(result = query_make(NULL, current, location, &format, &nested))) {
			nested->writable = self->writable;
			value->vt = VT_UNKNOWN;
			value->punkVal = (IUnknown *)&nested->iface;
		}
		CoTaskMemFree(location);
		if (current)
			IWICMetadataReader_Release(current);
	}
	return result;
}

/* The names of what lies below, each by the path that reaches it. */
typedef struct
{
	IEnumString iface;
	LONG references;
	WCHAR **names;
	UINT count, at;
} name_list;

static name_list *names_of(IEnumString *iface)
{
	return WIC_CONTAINING(iface, name_list, iface);
}

static HRESULT WINAPI names_QueryInterface(IEnumString *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IEnumString)) {
		*out = iface;
		IEnumString_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI names_AddRef(IEnumString *iface)
{
	return InterlockedIncrement(&names_of(iface)->references);
}

static ULONG WINAPI names_Release(IEnumString *iface)
{
	name_list *self = names_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		UINT index;
		for (index = 0; index < self->count; index++)
			CoTaskMemFree(self->names[index]);
		wic_free(self->names);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI names_Next(IEnumString *iface, ULONG count, LPOLESTR *out, ULONG *fetched)
{
	name_list *self = names_of(iface);
	ULONG got = 0;
	if (!out || (count > 1 && !fetched))
		return E_INVALIDARG;
	while (got < count && self->at < self->count)
		out[got++] = text_dup(self->names[self->at++]);
	if (fetched)
		*fetched = got;
	return got == count ? S_OK : S_FALSE;
}

static HRESULT WINAPI names_Skip(IEnumString *iface, ULONG count)
{
	name_list *self = names_of(iface);
	const UINT left = self->count - self->at;
	self->at += min(count, left);
	return count <= left ? S_OK : S_FALSE;
}

static HRESULT WINAPI names_Reset(IEnumString *iface)
{
	names_of(iface)->at = 0;
	return S_OK;
}

static HRESULT WINAPI names_Clone(IEnumString *iface, IEnumString **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static const IEnumStringVtbl names_vtbl = {
	names_QueryInterface, names_AddRef, names_Release, names_Next, names_Skip, names_Reset, names_Clone,
};

static void write_decimal(WCHAR *to, ULONGLONG value)
{
	WCHAR digits[24];
	UINT used = 0;
	do {
		digits[used++] = (WCHAR)('0' + value % 10);
		value /= 10;
	} while (value);
	while (used)
		*to++ = digits[--used];
	*to = 0;
}

/* The path an item is reached by: its short format name when it is a block, else its id in braces. */
static WCHAR *item_path(const PROPVARIANT *id, const PROPVARIANT *value)
{
	WCHAR text[160];
	UINT index;
	lstrcpyW(text, L"/");
	if (value && value->vt == VT_UNKNOWN && value->punkVal) {
		IWICMetadataReader *reader;
		if (SUCCEEDED(IUnknown_QueryInterface(value->punkVal, &IID_IWICMetadataReader, (void **)&reader))) {
			GUID format;
			UINT length;
			IWICMetadataReader_GetMetadataFormat(reader, &format);
			IWICMetadataReader_Release(reader);
			if (SUCCEEDED(WICMapGuidToShortName(&format, ARRAYSIZE(text) - 1, text + 1, &length)))
				return text_dup(text);
		}
	}
	if (id->vt == VT_LPWSTR) {
		lstrcpynW(text + 1, id->pwszVal, 150);
		return text_dup(text);
	}
	if (id->vt == VT_EMPTY) {
		lstrcatW(text, L"{}");
		return text_dup(text);
	}
	for (index = 0; index < ARRAYSIZE(type_names); index++)
		if (type_names[index].type == id->vt) {
			lstrcatW(text, L"{");
			/* The unsigned and signed names for a width come first in the table's order of use. */
			lstrcatW(text, type_names[index].name);
			lstrcatW(text, L"=");
			write_decimal(text + lstrlenW(text), id->uhVal.QuadPart);
			lstrcatW(text, L"}");
			return text_dup(text);
		}
	return text_dup(L"/{}");
}

static HRESULT WINAPI query_GetEnumerator(IWICMetadataQueryWriter *iface, IEnumString **out)
{
	query *self = query_of(iface);
	name_list *list;
	UINT total = 0, index;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (self->reader)
		IWICMetadataReader_GetCount(self->reader, &total);
	else if (self->blocks)
		IWICMetadataBlockReader_GetCount(self->blocks, &total);
	if (!(list = wic_alloc(sizeof(*list))) || !(list->names = wic_alloc((total + 1) * sizeof(WCHAR *)))) {
		wic_free(list);
		return E_OUTOFMEMORY;
	}
	list->iface.lpVtbl = &names_vtbl;
	list->references = 1;
	for (index = 0; index < total; index++) {
		PROPVARIANT id, value;
		PropVariantInit(&id);
		PropVariantInit(&value);
		if (self->reader) {
			if (SUCCEEDED(IWICMetadataReader_GetValueByIndex(self->reader, index, NULL, &id, &value)))
				list->names[list->count++] = item_path(&id, self->writable ? NULL : &value);
		}
		else {
			IWICMetadataReader *reader;
			if (SUCCEEDED(IWICMetadataBlockReader_GetReaderByIndex(self->blocks, index, &reader))) {
				value.vt = VT_UNKNOWN;
				value.punkVal = (IUnknown *)reader;
				list->names[list->count++] = item_path(&id, &value);
			}
		}
		PropVariantClear(&id);
		PropVariantClear(&value);
	}
	*out = &list->iface;
	return S_OK;
}

/* Walks to the block that holds the path's last segment, making the blocks a writing path needs. *last gets what remains. */
static HRESULT walk_to_owner(query *self, const WCHAR *name, BOOL make, IWICMetadataWriter **owner, const WCHAR **last)
{
	IWICMetadataReader *current = self->reader;
	const WCHAR *at = name;
	HRESULT result = S_OK;
	if (*at != '/')
		return WINCODEC_ERR_PROPERTYNOTSUPPORTED;
	if (current)
		IWICMetadataReader_AddRef(current);
	for (;;) {
		segment part;
		const WCHAR *after = read_segment(at + 1, &part, &result);
		IWICMetadataReader *next = NULL;
		GUID format;
		if (!after) {
			segment_clear(&part);
			break;
		}
		if (!*after && current) {
			segment_clear(&part);
			result = IWICMetadataReader_QueryInterface(current, &IID_IWICMetadataWriter, (void **)owner);
			IWICMetadataReader_Release(current);
			*last = at + 1;
			return result;
		}
		if (!current) {
			result = self->blocks ? find_block(self->blocks, &part, &next) : WINCODEC_ERR_PROPERTYNOTFOUND;
			if (FAILED(result) && make && part.plain && name_to_format(part.name, &format)) {
				IWICMetadataWriter *made;
				if (SUCCEEDED(result = wic_metadata_writer_create(&format, NULL, 0, &made))) {
					result = IWICMetadataBlockWriter_AddWriter((IWICMetadataBlockWriter *)self->blocks, made);
					next = (IWICMetadataReader *)made;
				}
			}
		}
		else {
			PROPVARIANT found;
			const BOOL named = part.plain && name_to_format(part.name, &format);
			PropVariantInit(&found);
			result = read_from(current, &part, &found);
			if (SUCCEEDED(result) && found.vt == VT_UNKNOWN && found.punkVal)
				result = IUnknown_QueryInterface(found.punkVal, &IID_IWICMetadataReader, (void **)&next);
			else if (SUCCEEDED(result))
				result = WINCODEC_ERR_PROPERTYNOTFOUND;
			else if (make && named) {
				IWICMetadataWriter *made, *holder;
				if (SUCCEEDED(result = wic_metadata_writer_create(&format, NULL, 0, &made))) {
					PROPVARIANT held;
					held.vt = VT_UNKNOWN;
					held.punkVal = (IUnknown *)made;
					if (SUCCEEDED(result = IWICMetadataReader_QueryInterface(current, &IID_IWICMetadataWriter, (void **)&holder))) {
						result = IWICMetadataWriter_SetValue(holder, &part.schema, &part.id, &held);
						IWICMetadataWriter_Release(holder);
					}
					next = (IWICMetadataReader *)made;
				}
			}
			PropVariantClear(&found);
		}
		segment_clear(&part);
		if (current)
			IWICMetadataReader_Release(current);
		current = next;
		if (FAILED(result))
			break;
		if (!*after) {
			/* The path named a block at the root and nothing more. */
			if (current)
				IWICMetadataReader_Release(current);
			*owner = NULL;
			*last = NULL;
			return S_OK;
		}
		at = after;
	}
	if (current)
		IWICMetadataReader_Release(current);
	return result;
}

static HRESULT WINAPI query_SetMetadataByName(IWICMetadataQueryWriter *iface, const WCHAR *name, const PROPVARIANT *value)
{
	query *self = query_of(iface);
	IWICMetadataWriter *owner = NULL;
	const WCHAR *last = NULL;
	segment part;
	HRESULT result;
	if (!name || !value)
		return E_INVALIDARG;
	if (FAILED(result = walk_to_owner(self, name, TRUE, &owner, &last)))
		return result;
	if (!owner)
		return S_OK;
	if (read_segment(last, &part, &result) && SUCCEEDED(result = prepare_ids((IWICMetadataReader *)owner, &part)))
		result = IWICMetadataWriter_SetValue(owner, &part.schema, &part.id, value);
	segment_clear(&part);
	IWICMetadataWriter_Release(owner);
	return result;
}

static HRESULT WINAPI query_RemoveMetadataByName(IWICMetadataQueryWriter *iface, const WCHAR *name)
{
	query *self = query_of(iface);
	IWICMetadataWriter *owner = NULL;
	const WCHAR *last = NULL;
	segment part = {0};
	HRESULT result;
	if (!name)
		return E_INVALIDARG;
	if (!lstrcmpW(name, L"/") && !self->reader)
		return WINCODEC_ERR_INVALIDQUERYREQUEST;
	if (FAILED(result = walk_to_owner(self, name, FALSE, &owner, &last)))
		return result;
	if (!owner) {
		/* A whole block at the root goes from the list. */
		UINT count = 0, index, seen = 0;
		GUID wanted;
		const WCHAR *after = read_segment(name + 1, &part, &result);
		if (!after || !part.plain || !name_to_format(part.name, &wanted)) {
			segment_clear(&part);
			return after ? WINCODEC_ERR_PROPERTYNOTFOUND : result;
		}
		IWICMetadataBlockReader_GetCount(self->blocks, &count);
		for (index = 0; index < count; index++) {
			IWICMetadataReader *reader;
			GUID format;
			BOOL same;
			if (FAILED(IWICMetadataBlockReader_GetReaderByIndex(self->blocks, index, &reader)))
				continue;
			same = SUCCEEDED(IWICMetadataReader_GetMetadataFormat(reader, &format)) && IsEqualGUID(&format, &wanted);
			IWICMetadataReader_Release(reader);
			if (same && seen++ == part.index) {
				segment_clear(&part);
				return IWICMetadataBlockWriter_RemoveWriterByIndex((IWICMetadataBlockWriter *)self->blocks, index);
			}
		}
		segment_clear(&part);
		return WINCODEC_ERR_PROPERTYNOTFOUND;
	}
	if (!*last)
		result = WINCODEC_ERR_PROPERTYNOTFOUND;
	else if (read_segment(last, &part, &result) && SUCCEEDED(result = prepare_ids((IWICMetadataReader *)owner, &part)))
		result = IWICMetadataWriter_RemoveValue(owner, &part.schema, &part.id);
	else
		memset(&part, 0, sizeof(part));
	segment_clear(&part);
	IWICMetadataWriter_Release(owner);
	return result;
}

static const IWICMetadataQueryWriterVtbl query_vtbl = {
	query_QueryInterface, query_AddRef, query_Release, query_GetContainerFormat, query_GetLocation, query_GetMetadataByName, query_GetEnumerator,
	query_SetMetadataByName, query_RemoveMetadataByName,
};

static HRESULT query_make(IWICMetadataBlockReader *blocks, IWICMetadataReader *reader, const WCHAR *location, const GUID *container, query **out)
{
	query *self = wic_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &query_vtbl;
	self->references = 1;
	if ((self->blocks = blocks))
		IWICMetadataBlockReader_AddRef(blocks);
	if ((self->reader = reader))
		IWICMetadataReader_AddRef(reader);
	self->location = text_dup(location);
	self->container = *container;
	*out = self;
	return S_OK;
}

HRESULT wic_query_reader_create(IWICMetadataBlockReader *blocks, IWICMetadataQueryReader **out)
{
	GUID container;
	query *self;
	HRESULT result;
	if (!blocks || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (FAILED(result = IWICMetadataBlockReader_GetContainerFormat(blocks, &container)))
		return result;
	if (FAILED(result = query_make(blocks, NULL, L"/", &container, &self)))
		return result;
	*out = (IWICMetadataQueryReader *)&self->iface;
	return S_OK;
}

HRESULT wic_query_writer_create(IWICMetadataBlockWriter *blocks, IWICMetadataQueryWriter **out)
{
	GUID container;
	query *self;
	HRESULT result;
	if (!blocks || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (FAILED(result = IWICMetadataBlockWriter_GetContainerFormat(blocks, &container)))
		return result;
	if (FAILED(result = query_make((IWICMetadataBlockReader *)blocks, NULL, L"/", &container, &self)))
		return result;
	self->writable = TRUE;
	*out = &self->iface;
	return S_OK;
}

HRESULT wic_query_writer_over(IWICMetadataWriter *writer, const WCHAR *location, const GUID *container, IWICMetadataQueryWriter **out)
{
	query *self;
	HRESULT result;
	if (FAILED(result = query_make(NULL, (IWICMetadataReader *)writer, location, container, &self)))
		return result;
	self->writable = TRUE;
	*out = &self->iface;
	return S_OK;
}

/* A copy that can change, of the block the reader stands at. There is none to copy at the top of a file. */
HRESULT wic_query_writer_from_reader(IWICMetadataQueryReader *reader, const GUID *vendor, IWICMetadataQueryWriter **out)
{
	query *from;
	IWICMetadataWriter *writer;
	GUID format;
	HRESULT result;
	if (!reader || !out)
		return E_INVALIDARG;
	*out = NULL;
	from = (query *)reader;
	if (!from->reader || FAILED(IWICMetadataReader_GetMetadataFormat(from->reader, &format)) || !wic_metadata_format_writable(&format))
		return WINCODEC_ERR_UNEXPECTEDMETADATATYPE;
	if (FAILED(result = wic_metadata_writer_from_reader(from->reader, vendor, &writer)))
		return result;
	{
		const WCHAR *last = from->location, *at;
		for (at = from->location; *at; at++)
			if (*at == '/')
				last = at;
		result = wic_query_writer_over(writer, last, &from->container, out);
	}
	IWICMetadataWriter_Release(writer);
	return result;
}
