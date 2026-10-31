/* SPDX-License-Identifier: MIT */
#include "ps.h"
#include <wchar.h>

/* The system properties come from a table generated from the public key list, so there is nothing to load or refresh. A
 * description answers what the table knows: the rest of its questions get the defaults a plain text property has. */

typedef struct
{
	PROPERTYKEY key;
	const WCHAR *name;
	VARTYPE type;
} schema_entry;

static const schema_entry schema[] = {
#include "schema.inc"
};

#define PKEYSTR_LIMIT 50
#define GUID_TEXT_LIMIT 39

static const schema_entry *entry_by_key(REFPROPERTYKEY key)
{
	UINT index;
	for (index = 0; index < ARRAYSIZE(schema); index++)
		if (schema[index].key.pid == key->pid && IsEqualGUID(&schema[index].key.fmtid, &key->fmtid))
			return &schema[index];
	return NULL;
}

static const schema_entry *entry_by_name(const WCHAR *name)
{
	UINT index;
	for (index = 0; index < ARRAYSIZE(schema); index++)
		if (!lstrcmpiW(schema[index].name, name))
			return &schema[index];
	return NULL;
}

/* Descriptions */

typedef struct
{
	IPropertyDescription iface;
	LONG refs;
	const schema_entry *entry;
} description;

static description *from_description(IPropertyDescription *self)
{
	return CONTAINING_RECORD(self, description, iface);
}

static HRESULT WINAPI description_QueryInterface(IPropertyDescription *self, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IPropertyDescription))
	{
		*out = self;
		IPropertyDescription_AddRef(self);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI description_AddRef(IPropertyDescription *self)
{
	return InterlockedIncrement(&from_description(self)->refs);
}

static ULONG WINAPI description_Release(IPropertyDescription *self)
{
	description *object = from_description(self);
	LONG left = InterlockedDecrement(&object->refs);
	if (!left)
	{
		CoTaskMemFree(object);
		InterlockedDecrement(&ps_locks);
	}
	return left;
}

static HRESULT WINAPI description_GetPropertyKey(IPropertyDescription *self, PROPERTYKEY *out)
{
	*out = from_description(self)->entry->key;
	return S_OK;
}

static HRESULT WINAPI description_GetCanonicalName(IPropertyDescription *self, LPWSTR *out)
{
	return (*out = ps_copy_text(from_description(self)->entry->name)) ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI description_GetPropertyType(IPropertyDescription *self, VARTYPE *out)
{
	*out = from_description(self)->entry->type;
	return S_OK;
}

static HRESULT WINAPI description_GetDisplayName(IPropertyDescription *self, LPWSTR *out)
{
	const WCHAR *name = from_description(self)->entry->name, *dot = wcsrchr(name, '.');
	return (*out = ps_copy_text(dot ? dot + 1 : name)) ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI description_GetEditInvitation(IPropertyDescription *self, LPWSTR *out)
{
	*out = NULL;
	return E_FAIL;
}

static HRESULT WINAPI description_GetTypeFlags(IPropertyDescription *self, PROPDESC_TYPE_FLAGS mask, PROPDESC_TYPE_FLAGS *out)
{
	*out = (from_description(self)->entry->type & VT_VECTOR ? PDTF_MULTIPLEVALUES : PDTF_DEFAULT) & mask;
	return S_OK;
}

static HRESULT WINAPI description_GetViewFlags(IPropertyDescription *self, PROPDESC_VIEW_FLAGS *out)
{
	*out = PDVF_DEFAULT;
	return S_OK;
}

static HRESULT WINAPI description_GetDefaultColumnWidth(IPropertyDescription *self, UINT *out)
{
	*out = 20;
	return S_OK;
}

static HRESULT WINAPI description_GetDisplayType(IPropertyDescription *self, PROPDESC_DISPLAYTYPE *out)
{
	switch (from_description(self)->entry->type)
	{
	case VT_BOOL:
		*out = PDDT_BOOLEAN;
		break;
	case VT_UI8:
		*out = PDDT_NUMBER;
		break;
	case VT_FILETIME:
		*out = PDDT_DATETIME;
		break;
	default:
		*out = PDDT_STRING;
		break;
	}
	return S_OK;
}

static HRESULT WINAPI description_GetColumnState(IPropertyDescription *self, SHCOLSTATEF *out)
{
	*out = 0;
	return S_OK;
}

static HRESULT WINAPI description_GetGroupingRange(IPropertyDescription *self, PROPDESC_GROUPING_RANGE *out)
{
	*out = PDGR_DISCRETE;
	return S_OK;
}

static HRESULT WINAPI description_GetRelativeDescriptionType(IPropertyDescription *self, PROPDESC_RELATIVEDESCRIPTION_TYPE *out)
{
	*out = PDRDT_GENERAL;
	return S_OK;
}

static HRESULT WINAPI description_GetRelativeDescription(IPropertyDescription *self, REFPROPVARIANT first, REFPROPVARIANT second, LPWSTR *first_text, LPWSTR *second_text)
{
	HRESULT status = ps_render(first, first_text);
	if (FAILED(status))
		return status;
	status = ps_render(second, second_text);
	if (FAILED(status))
	{
		CoTaskMemFree(*first_text);
		*first_text = NULL;
	}
	return status;
}

static HRESULT WINAPI description_GetSortDescription(IPropertyDescription *self, PROPDESC_SORTDESCRIPTION *out)
{
	*out = PDSD_GENERAL;
	return S_OK;
}

static HRESULT WINAPI description_GetSortDescriptionLabel(IPropertyDescription *self, BOOL descending, LPWSTR *out)
{
	*out = NULL;
	return E_FAIL;
}

static HRESULT WINAPI description_GetAggregationType(IPropertyDescription *self, PROPDESC_AGGREGATION_TYPE *out)
{
	*out = PDAT_DEFAULT;
	return S_OK;
}

static HRESULT WINAPI description_GetConditionType(IPropertyDescription *self, PROPDESC_CONDITION_TYPE *type, CONDITION_OPERATION *operation)
{
	*type = PDCOT_NONE;
	*operation = COP_IMPLICIT;
	return S_OK;
}

static HRESULT WINAPI description_GetEnumTypeList(IPropertyDescription *self, REFIID riid, void **out)
{
	*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI description_CoerceToCanonicalValue(IPropertyDescription *self, PROPVARIANT *value)
{
	PROPVARIANT changed;
	VARTYPE type = from_description(self)->entry->type;
	HRESULT status;
	if (value->vt == type)
		return S_OK;
	status = PropVariantChangeType(&changed, value, 0, type);
	PropVariantClear(value);
	if (SUCCEEDED(status))
		*value = changed;
	return status;
}

static HRESULT WINAPI description_FormatForDisplay(IPropertyDescription *self, REFPROPVARIANT value, PROPDESC_FORMAT_FLAGS flags, LPWSTR *out)
{
	return ps_render(value, out);
}

static HRESULT WINAPI description_IsValueCanonical(IPropertyDescription *self, REFPROPVARIANT value)
{
	return value->vt == from_description(self)->entry->type ? S_OK : S_FALSE;
}

static const IPropertyDescriptionVtbl description_vtbl = {
	description_QueryInterface,
	description_AddRef,
	description_Release,
	description_GetPropertyKey,
	description_GetCanonicalName,
	description_GetPropertyType,
	description_GetDisplayName,
	description_GetEditInvitation,
	description_GetTypeFlags,
	description_GetViewFlags,
	description_GetDefaultColumnWidth,
	description_GetDisplayType,
	description_GetColumnState,
	description_GetGroupingRange,
	description_GetRelativeDescriptionType,
	description_GetRelativeDescription,
	description_GetSortDescription,
	description_GetSortDescriptionLabel,
	description_GetAggregationType,
	description_GetConditionType,
	description_GetEnumTypeList,
	description_CoerceToCanonicalValue,
	description_FormatForDisplay,
	description_IsValueCanonical,
};

static HRESULT description_create(const schema_entry *entry, REFIID riid, void **out)
{
	description *object;
	HRESULT status;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!entry)
		return TYPE_E_ELEMENTNOTFOUND;
	if (!(object = CoTaskMemAlloc(sizeof(*object))))
		return E_OUTOFMEMORY;
	object->iface.lpVtbl = &description_vtbl;
	object->refs = 1;
	object->entry = entry;
	InterlockedIncrement(&ps_locks);
	status = description_QueryInterface(&object->iface, riid, out);
	description_Release(&object->iface);
	return status;
}

/* Description lists */

typedef struct
{
	IPropertyDescriptionList iface;
	LONG refs;
	UINT count;
	const schema_entry *const *entries;
} description_list;

static description_list *from_list(IPropertyDescriptionList *self)
{
	return CONTAINING_RECORD(self, description_list, iface);
}

static HRESULT WINAPI list_QueryInterface(IPropertyDescriptionList *self, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IPropertyDescriptionList))
	{
		*out = self;
		IPropertyDescriptionList_AddRef(self);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI list_AddRef(IPropertyDescriptionList *self)
{
	return InterlockedIncrement(&from_list(self)->refs);
}

static ULONG WINAPI list_Release(IPropertyDescriptionList *self)
{
	description_list *object = from_list(self);
	LONG left = InterlockedDecrement(&object->refs);
	if (!left)
	{
		CoTaskMemFree((void *)object->entries);
		CoTaskMemFree(object);
		InterlockedDecrement(&ps_locks);
	}
	return left;
}

static HRESULT WINAPI list_GetCount(IPropertyDescriptionList *self, UINT *out)
{
	if (!out)
		return E_POINTER;
	*out = from_list(self)->count;
	return S_OK;
}

static HRESULT WINAPI list_GetAt(IPropertyDescriptionList *self, UINT index, REFIID riid, void **out)
{
	description_list *object = from_list(self);
	if (index >= object->count)
		return E_INVALIDARG;
	return description_create(object->entries[index], riid, out);
}

static const IPropertyDescriptionListVtbl list_vtbl = {
	list_QueryInterface,
	list_AddRef,
	list_Release,
	list_GetCount,
	list_GetAt,
};

static HRESULT list_create(const schema_entry **entries, UINT count, REFIID riid, void **out)
{
	description_list *object;
	HRESULT status;
	if (!(object = CoTaskMemAlloc(sizeof(*object))))
	{
		CoTaskMemFree(entries);
		return E_OUTOFMEMORY;
	}
	object->iface.lpVtbl = &list_vtbl;
	object->refs = 1;
	object->count = count;
	object->entries = entries;
	InterlockedIncrement(&ps_locks);
	status = list_QueryInterface(&object->iface, riid, out);
	list_Release(&object->iface);
	return status;
}

/* A property list string is "prop:" and names separated by semicolons, each with an optional view modifier in front. */
static HRESULT list_from_string(const WCHAR *text, REFIID riid, void **out)
{
	const schema_entry **entries;
	const WCHAR *scan;
	UINT room = 1, count = 0;
	if (!text || !out)
		return E_POINTER;
	*out = NULL;
	if (!wcsncmp(text, L"prop:", 5))
		text += 5;
	for (scan = text; *scan; scan++)
		room += *scan == ';';
	if (!(entries = CoTaskMemAlloc(room * sizeof(*entries))))
		return E_OUTOFMEMORY;
	for (scan = text; *scan;)
	{
		WCHAR name[128];
		UINT length = 0;
		const schema_entry *entry;
		while (*scan == ' ' || *scan == '*' || *scan == '~' || *scan == '#')
			scan++;
		while (*scan && *scan != ';')
		{
			if (length + 1 < ARRAYSIZE(name))
				name[length++] = *scan;
			scan++;
		}
		if (*scan)
			scan++;
		name[length] = 0;
		if (!length)
			continue;
		if (!(entry = entry_by_name(name)))
		{
			CoTaskMemFree(entries);
			return TYPE_E_ELEMENTNOTFOUND;
		}
		entries[count++] = entry;
	}
	return list_create(entries, count, riid, out);
}

static HRESULT list_all(REFIID riid, void **out)
{
	const schema_entry **entries;
	UINT index;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!(entries = CoTaskMemAlloc(ARRAYSIZE(schema) * sizeof(*entries))))
		return E_OUTOFMEMORY;
	for (index = 0; index < ARRAYSIZE(schema); index++)
		entries[index] = &schema[index];
	return list_create(entries, ARRAYSIZE(schema), riid, out);
}

/* The property system */

static HRESULT WINAPI system_QueryInterface(IPropertySystem *self, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IPropertySystem))
	{
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI system_AddRef(IPropertySystem *self)
{
	return 2;
}

static ULONG WINAPI system_Release(IPropertySystem *self)
{
	return 1;
}

static HRESULT WINAPI system_GetPropertyDescription(IPropertySystem *self, REFPROPERTYKEY key, REFIID riid, void **out)
{
	return description_create(entry_by_key(key), riid, out);
}

static HRESULT WINAPI system_GetPropertyDescriptionByName(IPropertySystem *self, LPCWSTR name, REFIID riid, void **out)
{
	if (!name)
		return E_POINTER;
	return description_create(entry_by_name(name), riid, out);
}

static HRESULT WINAPI system_GetPropertyDescriptionListFromString(IPropertySystem *self, LPCWSTR text, REFIID riid, void **out)
{
	return list_from_string(text, riid, out);
}

static HRESULT WINAPI system_EnumeratePropertyDescriptions(IPropertySystem *self, PROPDESC_ENUMFILTER filter, REFIID riid, void **out)
{
	return list_all(riid, out);
}

static HRESULT WINAPI system_FormatForDisplay(IPropertySystem *self, REFPROPERTYKEY key, REFPROPVARIANT value, PROPDESC_FORMAT_FLAGS flags, LPWSTR text, DWORD room)
{
	return PropVariantToString(value, text, room);
}

static HRESULT WINAPI system_FormatForDisplayAlloc(IPropertySystem *self, REFPROPERTYKEY key, REFPROPVARIANT value, PROPDESC_FORMAT_FLAGS flags, LPWSTR *out)
{
	return ps_render(value, out);
}

static HRESULT WINAPI system_RegisterPropertySchema(IPropertySystem *self, LPCWSTR path)
{
	return S_OK;
}

static HRESULT WINAPI system_UnregisterPropertySchema(IPropertySystem *self, LPCWSTR path)
{
	return S_OK;
}

static HRESULT WINAPI system_RefreshPropertySchema(IPropertySystem *self)
{
	return S_OK;
}

static const IPropertySystemVtbl system_vtbl = {
	system_QueryInterface,
	system_AddRef,
	system_Release,
	system_GetPropertyDescription,
	system_GetPropertyDescriptionByName,
	system_GetPropertyDescriptionListFromString,
	system_EnumeratePropertyDescriptions,
	system_FormatForDisplay,
	system_FormatForDisplayAlloc,
	system_RegisterPropertySchema,
	system_UnregisterPropertySchema,
	system_RefreshPropertySchema,
};

static IPropertySystem the_system = {&system_vtbl};

HRESULT ps_system_get(REFIID riid, void **out)
{
	return system_QueryInterface(&the_system, riid, out);
}

HRESULT WINAPI PSGetPropertySystem(REFIID riid, void **out)
{
	return ps_system_get(riid, out);
}

HRESULT WINAPI PSRegisterPropertySchema(PCWSTR path)
{
	return S_OK;
}

HRESULT WINAPI PSUnregisterPropertySchema(PCWSTR path)
{
	return S_OK;
}

HRESULT WINAPI PSRefreshPropertySchema(void)
{
	return S_OK;
}

HRESULT WINAPI PSGetPropertyDescription(REFPROPERTYKEY key, REFIID riid, void **out)
{
	return description_create(entry_by_key(key), riid, out);
}

HRESULT WINAPI PSGetPropertyDescriptionByName(LPCWSTR name, REFIID riid, void **out)
{
	return system_GetPropertyDescriptionByName(&the_system, name, riid, out);
}

HRESULT WINAPI PSGetPropertyDescriptionListFromString(LPCWSTR text, REFIID riid, void **out)
{
	return list_from_string(text, riid, out);
}

HRESULT WINAPI PSEnumeratePropertyDescriptions(PROPDESC_ENUMFILTER filter, REFIID riid, void **out)
{
	return list_all(riid, out);
}

HRESULT WINAPI PSGetPropertyKeyFromName(PCWSTR name, PROPERTYKEY *out)
{
	const schema_entry *entry;
	if (!name || !out)
		return E_POINTER;
	if (!(entry = entry_by_name(name)))
		return TYPE_E_ELEMENTNOTFOUND;
	*out = entry->key;
	return S_OK;
}

HRESULT WINAPI PSGetNameFromPropertyKey(REFPROPERTYKEY key, PWSTR *out)
{
	const schema_entry *entry;
	if (!key || !out)
		return E_POINTER;
	*out = NULL;
	if (!(entry = entry_by_key(key)))
		return TYPE_E_ELEMENTNOTFOUND;
	return (*out = ps_copy_text(entry->name)) ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI PSFormatForDisplay(REFPROPERTYKEY key, REFPROPVARIANT value, PROPDESC_FORMAT_FLAGS flags, LPWSTR text, DWORD room)
{
	return PropVariantToString(value, text, room);
}

HRESULT WINAPI PSFormatForDisplayAlloc(REFPROPERTYKEY key, REFPROPVARIANT value, PROPDESC_FORMAT_FLAGS flags, LPWSTR *out)
{
	return ps_render(value, out);
}

HRESULT WINAPI PSGetPropertyValue(IPropertyStore *store, IPropertyDescription *description, PROPVARIANT *out)
{
	PROPERTYKEY key;
	HRESULT status = IPropertyDescription_GetPropertyKey(description, &key);
	return FAILED(status) ? status : IPropertyStore_GetValue(store, &key, out);
}

HRESULT WINAPI PSSetPropertyValue(IPropertyStore *store, IPropertyDescription *description, REFPROPVARIANT value)
{
	PROPERTYKEY key;
	HRESULT status = IPropertyDescription_GetPropertyKey(description, &key);
	return FAILED(status) ? status : IPropertyStore_SetValue(store, &key, value);
}

/* Key text */

HRESULT WINAPI PSStringFromPropertyKey(REFPROPERTYKEY key, LPWSTR out, UINT room)
{
	WCHAR guid[GUID_TEXT_LIMIT], reversed[11];
	UINT count = 0, index;
	DWORD pid;
	if (!out)
		return E_POINTER;
	if (!key)
	{
		if (room >= PKEYSTR_LIMIT)
			out[0] = 0;
		return E_NOT_SUFFICIENT_BUFFER;
	}
	if (room <= GUID_TEXT_LIMIT + 1)
		return E_NOT_SUFFICIENT_BUFFER;
	pid = key->pid;
	do
	{
		reversed[count++] = '0' + pid % 10;
		pid /= 10;
	} while (pid);
	StringFromGUID2(&key->fmtid, guid, ARRAYSIZE(guid));
	memcpy(out, guid, 38 * sizeof(WCHAR));
	out[38] = ' ';
	if (GUID_TEXT_LIMIT + count + 1 > room)
	{
		/* Whatever fits of the digits is left behind in the order they were produced, past two terminators. */
		for (index = 0; index < room - GUID_TEXT_LIMIT; index++)
			out[GUID_TEXT_LIMIT + index] = reversed[index];
		out[GUID_TEXT_LIMIT] = 0;
		out[0] = 0;
		return E_NOT_SUFFICIENT_BUFFER;
	}
	for (index = 0; index < count; index++)
		out[GUID_TEXT_LIMIT + index] = reversed[count - 1 - index];
	out[GUID_TEXT_LIMIT + count] = 0;
	return S_OK;
}

static int hex_digit(WCHAR letter)
{
	if (letter >= '0' && letter <= '9')
		return letter - '0';
	if (letter >= 'a' && letter <= 'f')
		return letter - 'a' + 10;
	if (letter >= 'A' && letter <= 'F')
		return letter - 'A' + 10;
	return -1;
}

static BOOL read_hex(const WCHAR **scan, UINT digits, DWORD *out)
{
	const WCHAR *text = *scan;
	DWORD value = 0;
	while (digits--)
	{
		int digit = hex_digit(*text);
		if (digit < 0)
			return FALSE;
		value = value << 4 | digit;
		text++;
	}
	*scan = text;
	*out = value;
	return TRUE;
}

/* A field fills once its digits are read, so a caller sees how far a damaged key got. The bytes before a separator wait for
 * it: the two middle words and the second byte of the last group for their dashes, the final byte for the closing brace. */
static const WCHAR *read_key_guid(const WCHAR *scan, GUID *out)
{
	DWORD value;
	UINT index;
	if (*scan++ != '{')
		return NULL;
	if (!read_hex(&scan, 8, &value))
		return NULL;
	out->Data1 = value;
	if (*scan++ != '-' || !read_hex(&scan, 4, &value) || *scan != '-')
		return NULL;
	out->Data2 = (USHORT)value;
	scan++;
	if (!read_hex(&scan, 4, &value) || *scan != '-')
		return NULL;
	out->Data3 = (USHORT)value;
	scan++;
	for (index = 0; index < 8; index++)
	{
		if (!read_hex(&scan, 2, &value))
			return NULL;
		if (index == 1 && *scan++ != '-')
			return NULL;
		if (index == 7 && *scan != '}')
			return NULL;
		out->Data4[index] = (BYTE)value;
	}
	return scan + 1;
}

/* After the class comes the id, with a separator grammar that is odd but fixed: at most one comma takes one minus sign that
 * must touch the digits, and without a comma it takes two minus signs to negate and three to give up. */
static DWORD read_key_pid(const WCHAR *scan)
{
	BOOL negative = FALSE;
	DWORD value = 0;
	while (*scan == ' ')
		scan++;
	if (*scan == ',')
	{
		scan++;
		while (*scan == ' ')
			scan++;
		if (*scan == '-')
		{
			negative = TRUE;
			scan++;
		}
	}
	else
	{
		UINT signs = 0;
		while (*scan == '-')
		{
			signs++;
			scan++;
			while (*scan == ' ')
				scan++;
		}
		if (signs > 2)
			return 0;
		negative = signs == 2;
	}
	while (*scan >= '0' && *scan <= '9')
		value = value * 10 + (*scan++ - '0');
	return negative ? 0u - value : value;
}

HRESULT WINAPI PSPropertyKeyFromString(LPCWSTR text, PROPERTYKEY *out)
{
	const WCHAR *scan;
	if (!text || !out)
		return E_POINTER;
	memset(out, 0, sizeof(*out));
	if (!(scan = read_key_guid(text, &out->fmtid)) || !*scan)
		return E_INVALIDARG;
	{
		const WCHAR *rest = scan;
		while (*rest == ' ')
			rest++;
		if (!*rest)
			return E_INVALIDARG;
	}
	out->pid = read_key_pid(scan);
	return S_OK;
}
