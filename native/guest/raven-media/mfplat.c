/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven Media Foundation component: mfplat.dll, the samples, media types and buffers media libraries carry their data in.
 *
 * These objects only hold attributes and bytes, so they are complete without a media pipeline behind them. */
#define INITGUID
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <propvarutil.h>
#include <strmif.h>
#include <stdint.h>
#include <string.h>

static const GUID format_wave_format_ex = {0x05589f81, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
static const GUID subtype_adpcm = {0x00000002, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

typedef enum attribute_kind {
	ATTRIBUTE_U32,
	ATTRIBUTE_U64,
	ATTRIBUTE_DOUBLE,
	ATTRIBUTE_GUID,
	ATTRIBUTE_STRING,
	ATTRIBUTE_BLOB,
	ATTRIBUTE_UNKNOWN
} attribute_kind;

typedef struct attribute {
	GUID key;
	attribute_kind kind;
	union {
		UINT32 u32;
		UINT64 u64;
		double real;
		GUID guid;
		WCHAR *string;
		struct {
			UINT32 size;
			UINT8 *data;
		} blob;
		IUnknown *unknown;
	} value;
} attribute;

typedef struct mf_object {
	union {
		IMFMediaType media_type;
		IMFSample sample;
	};
	LONG ref;
	/* Guards the attributes and the sample fields. A temporary object that never leaves one call has none. */
	CRITICAL_SECTION lock;
	attribute *attributes;
	UINT32 attribute_count, attribute_capacity;
	DWORD sample_flags;
	LONGLONG sample_time, sample_duration;
	BOOL has_sample_time, has_sample_duration;
	IMFMediaBuffer **buffers;
	DWORD buffer_count, buffer_capacity;
} mf_object;

typedef struct media_buffer {
	IMFMediaBuffer iface;
	LONG ref;
	BYTE *allocation, *data;
	DWORD max_length, current_length, lock_count;
} media_buffer;

static BOOL guid_equal(REFGUID left, REFGUID right)
{
	return !memcmp(left, right, sizeof(GUID));
}

static size_t wide_length(const WCHAR *text)
{
	const WCHAR *end = text;
	while (*end)
		++end;
	return (size_t)(end - text);
}

static BOOL wide_equal(const WCHAR *left, const WCHAR *right)
{
	while (*left && *left == *right) {
		++left;
		++right;
	}
	return *left == *right;
}

static void attribute_clear(attribute *item)
{
	switch (item->kind) {
	case ATTRIBUTE_STRING:
		LocalFree(item->value.string);
		break;
	case ATTRIBUTE_BLOB:
		LocalFree(item->value.blob.data);
		break;
	case ATTRIBUTE_UNKNOWN:
		if (item->value.unknown)
			IUnknown_Release(item->value.unknown);
		break;
	default:
		break;
	}
	memset(item, 0, sizeof(*item));
}

static mf_object *object_from_type(IMFMediaType *iface) { return (mf_object *)iface; }
static mf_object *object_from_sample(IMFSample *iface) { return (mf_object *)iface; }

static attribute *attribute_find(mf_object *object, REFGUID key)
{
	for (UINT32 i = 0; i < object->attribute_count; ++i)
		if (guid_equal(&object->attributes[i].key, key))
			return &object->attributes[i];
	return NULL;
}

static HRESULT attribute_store(mf_object *object, REFGUID key, const attribute *value)
{
	attribute copy = *value;
	switch (value->kind) {
	case ATTRIBUTE_STRING: {
		const size_t size = (wide_length(value->value.string) + 1) * sizeof(WCHAR);
		copy.value.string = LocalAlloc(LMEM_FIXED, size);
		if (!copy.value.string)
			return E_OUTOFMEMORY;
		memcpy(copy.value.string, value->value.string, size);
		break;
	}
	case ATTRIBUTE_BLOB:
		copy.value.blob.data = NULL;
		if (value->value.blob.size) {
			copy.value.blob.data = LocalAlloc(LMEM_FIXED, value->value.blob.size);
			if (!copy.value.blob.data)
				return E_OUTOFMEMORY;
			memcpy(copy.value.blob.data, value->value.blob.data, value->value.blob.size);
		}
		break;
	case ATTRIBUTE_UNKNOWN:
		if (copy.value.unknown)
			IUnknown_AddRef(copy.value.unknown);
		break;
	default:
		break;
	}
	attribute *slot = attribute_find(object, key);
	if (!slot) {
		if (object->attribute_count == object->attribute_capacity) {
			const UINT32 capacity = object->attribute_capacity ? object->attribute_capacity * 2 : 8;
			if (capacity < object->attribute_capacity || capacity > SIZE_MAX / sizeof(attribute)) {
				attribute_clear(&copy);
				return E_OUTOFMEMORY;
			}
			attribute *items = LocalAlloc(LMEM_FIXED, capacity * sizeof(*items));
			if (!items) {
				attribute_clear(&copy);
				return E_OUTOFMEMORY;
			}
			if (object->attribute_count)
				memcpy(items, object->attributes, object->attribute_count * sizeof(*items));
			LocalFree(object->attributes);
			object->attributes = items;
			object->attribute_capacity = capacity;
		}
		slot = &object->attributes[object->attribute_count++];
		memset(slot, 0, sizeof(*slot));
	} else {
		attribute_clear(slot);
	}
	*slot = copy;
	slot->key = *key;
	return S_OK;
}

static HRESULT attribute_get_item(mf_object *object, REFGUID key, PROPVARIANT *value)
{
	attribute *item = attribute_find(object, key);
	if (!item)
		return MF_E_ATTRIBUTENOTFOUND;
	/* A caller passes no value to ask only whether the key exists. */
	if (!value)
		return S_OK;
	PropVariantInit(value);
	switch (item->kind) {
	case ATTRIBUTE_U32:
		value->vt = VT_UI4;
		value->ulVal = item->value.u32;
		break;
	case ATTRIBUTE_U64:
		value->vt = VT_UI8;
		value->uhVal.QuadPart = item->value.u64;
		break;
	case ATTRIBUTE_DOUBLE:
		value->vt = VT_R8;
		value->dblVal = item->value.real;
		break;
	case ATTRIBUTE_GUID:
		value->vt = VT_CLSID;
		value->puuid = CoTaskMemAlloc(sizeof(GUID));
		if (!value->puuid)
			return E_OUTOFMEMORY;
		*value->puuid = item->value.guid;
		break;
	case ATTRIBUTE_STRING: {
		const size_t size = (wide_length(item->value.string) + 1) * sizeof(WCHAR);
		value->vt = VT_LPWSTR;
		value->pwszVal = CoTaskMemAlloc(size);
		if (!value->pwszVal)
			return E_OUTOFMEMORY;
		memcpy(value->pwszVal, item->value.string, size);
		break;
	}
	case ATTRIBUTE_BLOB:
		value->vt = VT_VECTOR | VT_UI1;
		value->caub.cElems = item->value.blob.size;
		value->caub.pElems = NULL;
		if (item->value.blob.size) {
			value->caub.pElems = CoTaskMemAlloc(item->value.blob.size);
			if (!value->caub.pElems) {
				PropVariantInit(value);
				return E_OUTOFMEMORY;
			}
			memcpy(value->caub.pElems, item->value.blob.data, item->value.blob.size);
		}
		break;
	case ATTRIBUTE_UNKNOWN:
		value->vt = VT_UNKNOWN;
		value->punkVal = item->value.unknown;
		if (value->punkVal)
			IUnknown_AddRef(value->punkVal);
		break;
	}
	return S_OK;
}

static HRESULT attribute_from_variant(mf_object *object, REFGUID key, REFPROPVARIANT value)
{
	if (!value)
		return E_POINTER;
	attribute item;
	memset(&item, 0, sizeof(item));
	switch (value->vt) {
	case VT_UI4:
		item.kind = ATTRIBUTE_U32;
		item.value.u32 = value->ulVal;
		break;
	case VT_UI8:
		item.kind = ATTRIBUTE_U64;
		item.value.u64 = value->uhVal.QuadPart;
		break;
	case VT_R8:
		item.kind = ATTRIBUTE_DOUBLE;
		item.value.real = value->dblVal;
		break;
	case VT_CLSID:
		if (!value->puuid)
			return E_INVALIDARG;
		item.kind = ATTRIBUTE_GUID;
		item.value.guid = *value->puuid;
		break;
	case VT_LPWSTR:
		if (!value->pwszVal)
			return E_INVALIDARG;
		item.kind = ATTRIBUTE_STRING;
		item.value.string = value->pwszVal;
		break;
	case VT_BLOB:
		if (value->blob.cbSize && !value->blob.pBlobData)
			return E_INVALIDARG;
		item.kind = ATTRIBUTE_BLOB;
		item.value.blob.size = value->blob.cbSize;
		item.value.blob.data = value->blob.pBlobData;
		break;
	case VT_VECTOR | VT_UI1:
		if (value->caub.cElems && !value->caub.pElems)
			return E_INVALIDARG;
		item.kind = ATTRIBUTE_BLOB;
		item.value.blob.size = value->caub.cElems;
		item.value.blob.data = value->caub.pElems;
		break;
	case VT_UNKNOWN:
		item.kind = ATTRIBUTE_UNKNOWN;
		item.value.unknown = value->punkVal;
		break;
	default:
		return MF_E_INVALIDTYPE;
	}
	return attribute_store(object, key, &item);
}

static MF_ATTRIBUTE_TYPE attribute_type(attribute_kind kind)
{
	switch (kind) {
	case ATTRIBUTE_U32: return MF_ATTRIBUTE_UINT32;
	case ATTRIBUTE_U64: return MF_ATTRIBUTE_UINT64;
	case ATTRIBUTE_DOUBLE: return MF_ATTRIBUTE_DOUBLE;
	case ATTRIBUTE_GUID: return MF_ATTRIBUTE_GUID;
	case ATTRIBUTE_STRING: return MF_ATTRIBUTE_STRING;
	case ATTRIBUTE_BLOB: return MF_ATTRIBUTE_BLOB;
	case ATTRIBUTE_UNKNOWN: return MF_ATTRIBUTE_IUNKNOWN;
	}
	return (MF_ATTRIBUTE_TYPE)0;
}

static HRESULT attribute_get_scalar(mf_object *object, REFGUID key, attribute_kind kind, void *value)
{
	if (!value)
		return E_POINTER;
	attribute *item = attribute_find(object, key);
	if (!item)
		return MF_E_ATTRIBUTENOTFOUND;
	if (item->kind != kind)
		return MF_E_INVALIDTYPE;
	switch (kind) {
	case ATTRIBUTE_U32: *(UINT32 *)value = item->value.u32; break;
	case ATTRIBUTE_U64: *(UINT64 *)value = item->value.u64; break;
	case ATTRIBUTE_DOUBLE: *(double *)value = item->value.real; break;
	case ATTRIBUTE_GUID: *(GUID *)value = item->value.guid; break;
	default: return MF_E_INVALIDTYPE;
	}
	return S_OK;
}

static BOOL attribute_equal_value(const attribute *left, const attribute *right)
{
	if (left->kind != right->kind)
		return FALSE;
	switch (left->kind) {
	case ATTRIBUTE_U32: return left->value.u32 == right->value.u32;
	case ATTRIBUTE_U64: return left->value.u64 == right->value.u64;
	case ATTRIBUTE_DOUBLE: return left->value.real == right->value.real;
	case ATTRIBUTE_GUID: return guid_equal(&left->value.guid, &right->value.guid);
	case ATTRIBUTE_STRING: return wide_equal(left->value.string, right->value.string);
	case ATTRIBUTE_BLOB:
		return left->value.blob.size == right->value.blob.size &&
			(!left->value.blob.size || !memcmp(left->value.blob.data, right->value.blob.data, left->value.blob.size));
	case ATTRIBUTE_UNKNOWN: return left->value.unknown == right->value.unknown;
	}
	return FALSE;
}

static HRESULT attr_get_item_type(mf_object *object, REFGUID key, MF_ATTRIBUTE_TYPE *type)
{
	if (!type)
		return E_POINTER;
	attribute *item = attribute_find(object, key);
	if (!item)
		return MF_E_ATTRIBUTENOTFOUND;
	*type = attribute_type(item->kind);
	return S_OK;
}

static HRESULT attr_compare_item(mf_object *object, REFGUID key, REFPROPVARIANT value, BOOL *result)
{
	if (!result)
		return E_POINTER;
	mf_object temporary;
	memset(&temporary, 0, sizeof(temporary));
	const HRESULT status = attribute_from_variant(&temporary, key, value);
	if (FAILED(status))
		return status;
	attribute candidate;
	candidate = temporary.attributes[0];
	attribute *item = attribute_find(object, key);
	*result = item && attribute_equal_value(item, &candidate);
	attribute_clear(&candidate);
	LocalFree(temporary.attributes);
	return S_OK;
}

static HRESULT attr_compare(mf_object *object, IMFAttributes *theirs, MF_ATTRIBUTES_MATCH_TYPE mode, BOOL *result)
{
	if (!theirs || !result)
		return E_POINTER;
	*result = TRUE;
	UINT32 count = 0;
	HRESULT status = IMFAttributes_GetCount(theirs, &count);
	if (FAILED(status))
		return status;
	if (mode == MF_ATTRIBUTES_MATCH_OUR_ITEMS || mode == MF_ATTRIBUTES_MATCH_ALL_ITEMS ||
		(mode == MF_ATTRIBUTES_MATCH_SMALLER && object->attribute_count <= count)) {
		for (UINT32 i = 0; i < object->attribute_count; ++i) {
			attribute *item = &object->attributes[i];
			PROPVARIANT value;
			PropVariantInit(&value);
			status = IMFAttributes_GetItem(theirs, &item->key, &value);
			if (FAILED(status)) {
				*result = FALSE;
				return S_OK;
			}
			mf_object temporary;
			memset(&temporary, 0, sizeof(temporary));
			status = attribute_from_variant(&temporary, &item->key, &value);
			PropVariantClear(&value);
			if (FAILED(status) || !attribute_equal_value(item, &temporary.attributes[0])) {
				*result = FALSE;
				if (temporary.attribute_count)
					attribute_clear(&temporary.attributes[0]);
				LocalFree(temporary.attributes);
				return S_OK;
			}
			attribute_clear(&temporary.attributes[0]);
			LocalFree(temporary.attributes);
		}
	}
	if (mode == MF_ATTRIBUTES_MATCH_THEIR_ITEMS || mode == MF_ATTRIBUTES_MATCH_ALL_ITEMS || mode == MF_ATTRIBUTES_MATCH_INTERSECTION ||
		(mode == MF_ATTRIBUTES_MATCH_SMALLER && object->attribute_count > count)) {
		for (UINT32 i = 0; i < count; ++i) {
			GUID key;
			PROPVARIANT value;
			PropVariantInit(&value);
			status = IMFAttributes_GetItemByIndex(theirs, i, &key, &value);
			if (FAILED(status))
				return status;
			attribute *item = attribute_find(object, &key);
			if (!item && mode == MF_ATTRIBUTES_MATCH_INTERSECTION) {
				PropVariantClear(&value);
				continue;
			}
			if (!item) {
				PropVariantClear(&value);
				*result = FALSE;
				return S_OK;
			}
			mf_object temporary;
			memset(&temporary, 0, sizeof(temporary));
			status = attribute_from_variant(&temporary, &key, &value);
			PropVariantClear(&value);
			const BOOL equal = SUCCEEDED(status) && attribute_equal_value(item, &temporary.attributes[0]);
			if (temporary.attribute_count)
				attribute_clear(&temporary.attributes[0]);
			LocalFree(temporary.attributes);
			if (!equal) {
				*result = FALSE;
				return S_OK;
			}
		}
	}
	return S_OK;
}

static HRESULT attr_get_string_length(mf_object *object, REFGUID key, UINT32 *length)
{
	if (!length)
		return E_POINTER;
	attribute *item = attribute_find(object, key);
	if (!item)
		return MF_E_ATTRIBUTENOTFOUND;
	if (item->kind != ATTRIBUTE_STRING)
		return MF_E_INVALIDTYPE;
	*length = (UINT32)wide_length(item->value.string);
	return S_OK;
}

static HRESULT attr_get_string(mf_object *object, REFGUID key, LPWSTR value, UINT32 size, UINT32 *length)
{
	attribute *item = attribute_find(object, key);
	if (!item)
		return MF_E_ATTRIBUTENOTFOUND;
	if (item->kind != ATTRIBUTE_STRING)
		return MF_E_INVALIDTYPE;
	const UINT32 chars = (UINT32)wide_length(item->value.string);
	if (length)
		*length = chars;
	if (!value || size <= chars)
		return MF_E_BUFFERTOOSMALL;
	memcpy(value, item->value.string, (chars + 1) * sizeof(WCHAR));
	return S_OK;
}

static HRESULT attr_get_allocated_string(mf_object *object, REFGUID key, LPWSTR *value, UINT32 *length)
{
	if (!value)
		return E_POINTER;
	*value = NULL;
	attribute *item = attribute_find(object, key);
	if (!item)
		return MF_E_ATTRIBUTENOTFOUND;
	if (item->kind != ATTRIBUTE_STRING)
		return MF_E_INVALIDTYPE;
	const size_t bytes = (wide_length(item->value.string) + 1) * sizeof(WCHAR);
	*value = CoTaskMemAlloc(bytes);
	if (!*value)
		return E_OUTOFMEMORY;
	memcpy(*value, item->value.string, bytes);
	if (length)
		*length = (UINT32)(bytes / sizeof(WCHAR) - 1);
	return S_OK;
}

static HRESULT attr_get_blob_size(mf_object *object, REFGUID key, UINT32 *size)
{
	if (!size)
		return E_POINTER;
	attribute *item = attribute_find(object, key);
	if (!item)
		return MF_E_ATTRIBUTENOTFOUND;
	if (item->kind != ATTRIBUTE_BLOB)
		return MF_E_INVALIDTYPE;
	*size = item->value.blob.size;
	return S_OK;
}

static HRESULT attr_get_blob(mf_object *object, REFGUID key, UINT8 *value, UINT32 size, UINT32 *written)
{
	attribute *item = attribute_find(object, key);
	if (!item)
		return MF_E_ATTRIBUTENOTFOUND;
	if (item->kind != ATTRIBUTE_BLOB)
		return MF_E_INVALIDTYPE;
	if (written)
		*written = item->value.blob.size;
	if (size < item->value.blob.size || (!value && item->value.blob.size))
		return MF_E_BUFFERTOOSMALL;
	if (item->value.blob.size)
		memcpy(value, item->value.blob.data, item->value.blob.size);
	return S_OK;
}

static HRESULT attr_get_allocated_blob(mf_object *object, REFGUID key, UINT8 **value, UINT32 *size)
{
	if (!value)
		return E_POINTER;
	*value = NULL;
	attribute *item = attribute_find(object, key);
	if (!item)
		return MF_E_ATTRIBUTENOTFOUND;
	if (item->kind != ATTRIBUTE_BLOB)
		return MF_E_INVALIDTYPE;
	if (item->value.blob.size) {
		*value = CoTaskMemAlloc(item->value.blob.size);
		if (!*value)
			return E_OUTOFMEMORY;
		memcpy(*value, item->value.blob.data, item->value.blob.size);
	}
	if (size)
		*size = item->value.blob.size;
	return S_OK;
}

static HRESULT attr_get_unknown(mf_object *object, REFGUID key, REFIID iid, LPVOID *value)
{
	if (!value)
		return E_POINTER;
	*value = NULL;
	attribute *item = attribute_find(object, key);
	if (!item)
		return MF_E_ATTRIBUTENOTFOUND;
	if (item->kind != ATTRIBUTE_UNKNOWN)
		return MF_E_INVALIDTYPE;
	return item->value.unknown ? IUnknown_QueryInterface(item->value.unknown, iid, value) : E_NOINTERFACE;
}

static HRESULT attr_delete(mf_object *object, REFGUID key)
{
	for (UINT32 i = 0; i < object->attribute_count; ++i)
		if (guid_equal(&object->attributes[i].key, key)) {
			attribute_clear(&object->attributes[i]);
			if (i + 1 < object->attribute_count)
				memmove(&object->attributes[i], &object->attributes[i + 1],
					(object->attribute_count - i - 1) * sizeof(attribute));
			--object->attribute_count;
			break;
		}
	return S_OK;
}

static HRESULT attr_delete_all(mf_object *object)
{
	while (object->attribute_count)
		attribute_clear(&object->attributes[--object->attribute_count]);
	return S_OK;
}

static HRESULT attr_get_count(mf_object *object, UINT32 *count)
{
	if (!count)
		return E_POINTER;
	*count = object->attribute_count;
	return S_OK;
}

static HRESULT attr_get_index(mf_object *object, UINT32 index, GUID *key, PROPVARIANT *value)
{
	if (!key)
		return E_POINTER;
	if (index >= object->attribute_count)
		return E_INVALIDARG;
	*key = object->attributes[index].key;
	return attribute_get_item(object, key, value);
}

static HRESULT attr_copy_all(mf_object *object, IMFAttributes *destination)
{
	if (!destination)
		return E_POINTER;
	for (UINT32 i = 0; i < object->attribute_count; ++i) {
		PROPVARIANT value;
		PropVariantInit(&value);
		HRESULT status = attribute_get_item(object, &object->attributes[i].key, &value);
		if (SUCCEEDED(status))
			status = IMFAttributes_SetItem(destination, &object->attributes[i].key, &value);
		PropVariantClear(&value);
		if (FAILED(status))
			return status;
	}
	return S_OK;
}

static ULONG object_addref(mf_object *object) { return (ULONG)InterlockedIncrement(&object->ref); }

static ULONG object_release(mf_object *object)
{
	const LONG ref = InterlockedDecrement(&object->ref);
	if (ref)
		return (ULONG)ref;
	for (UINT32 i = 0; i < object->attribute_count; ++i)
		attribute_clear(&object->attributes[i]);
	LocalFree(object->attributes);
	for (DWORD i = 0; i < object->buffer_count; ++i)
		IMFMediaBuffer_Release(object->buffers[i]);
	LocalFree(object->buffers);
	DeleteCriticalSection(&object->lock);
	LocalFree(object);
	return 0;
}

#define GUARDED(object_expr, call) \
	{ mf_object *o = (object_expr); EnterCriticalSection(&o->lock); const HRESULT status = (call); LeaveCriticalSection(&o->lock); return status; }

/* A call into another store runs on a copy of this one, with no lock held: two stores that name each other never wait for each other. */
static HRESULT attr_snapshot(mf_object *object, mf_object *copy)
{
	HRESULT status = S_OK;
	memset(copy, 0, sizeof(*copy));
	EnterCriticalSection(&object->lock);
	for (UINT32 i = 0; SUCCEEDED(status) && i < object->attribute_count; ++i)
		status = attribute_store(copy, &object->attributes[i].key, &object->attributes[i]);
	LeaveCriticalSection(&object->lock);
	return status;
}

static void attr_snapshot_free(mf_object *copy)
{
	for (UINT32 i = 0; i < copy->attribute_count; ++i)
		attribute_clear(&copy->attributes[i]);
	LocalFree(copy->attributes);
}

#define SNAPSHOTTED(object_expr, call) \
	{ mf_object copy, *o = &copy; HRESULT status = attr_snapshot((object_expr), o); if (SUCCEEDED(status)) status = (call); attr_snapshot_free(o); return status; }

#define ATTRIBUTE_WRAPPERS(prefix, iface_type, object_expr) \
static HRESULT WINAPI prefix##_GetItem(iface_type *self, REFGUID key, PROPVARIANT *value) GUARDED((object_expr), attribute_get_item(o, key, value)) \
static HRESULT WINAPI prefix##_GetItemType(iface_type *self, REFGUID key, MF_ATTRIBUTE_TYPE *type) GUARDED((object_expr), attr_get_item_type(o, key, type)) \
static HRESULT WINAPI prefix##_CompareItem(iface_type *self, REFGUID key, REFPROPVARIANT value, WINBOOL *result) GUARDED((object_expr), attr_compare_item(o, key, value, result)) \
static HRESULT WINAPI prefix##_Compare(iface_type *self, IMFAttributes *theirs, MF_ATTRIBUTES_MATCH_TYPE mode, WINBOOL *result) SNAPSHOTTED((object_expr), attr_compare(o, theirs, mode, result)) \
static HRESULT WINAPI prefix##_GetUINT32(iface_type *self, REFGUID key, UINT32 *value) GUARDED((object_expr), attribute_get_scalar(o, key, ATTRIBUTE_U32, value)) \
static HRESULT WINAPI prefix##_GetUINT64(iface_type *self, REFGUID key, UINT64 *value) GUARDED((object_expr), attribute_get_scalar(o, key, ATTRIBUTE_U64, value)) \
static HRESULT WINAPI prefix##_GetDouble(iface_type *self, REFGUID key, double *value) GUARDED((object_expr), attribute_get_scalar(o, key, ATTRIBUTE_DOUBLE, value)) \
static HRESULT WINAPI prefix##_GetGUID(iface_type *self, REFGUID key, GUID *value) GUARDED((object_expr), attribute_get_scalar(o, key, ATTRIBUTE_GUID, value)) \
static HRESULT WINAPI prefix##_GetStringLength(iface_type *self, REFGUID key, UINT32 *length) GUARDED((object_expr), attr_get_string_length(o, key, length)) \
static HRESULT WINAPI prefix##_GetString(iface_type *self, REFGUID key, LPWSTR value, UINT32 size, UINT32 *length) GUARDED((object_expr), attr_get_string(o, key, value, size, length)) \
static HRESULT WINAPI prefix##_GetAllocatedString(iface_type *self, REFGUID key, LPWSTR *value, UINT32 *length) GUARDED((object_expr), attr_get_allocated_string(o, key, value, length)) \
static HRESULT WINAPI prefix##_GetBlobSize(iface_type *self, REFGUID key, UINT32 *size) GUARDED((object_expr), attr_get_blob_size(o, key, size)) \
static HRESULT WINAPI prefix##_GetBlob(iface_type *self, REFGUID key, UINT8 *value, UINT32 size, UINT32 *written) GUARDED((object_expr), attr_get_blob(o, key, value, size, written)) \
static HRESULT WINAPI prefix##_GetAllocatedBlob(iface_type *self, REFGUID key, UINT8 **value, UINT32 *size) GUARDED((object_expr), attr_get_allocated_blob(o, key, value, size)) \
static HRESULT WINAPI prefix##_GetUnknown(iface_type *self, REFGUID key, REFIID iid, LPVOID *value) GUARDED((object_expr), attr_get_unknown(o, key, iid, value)) \
static HRESULT WINAPI prefix##_SetItem(iface_type *self, REFGUID key, REFPROPVARIANT value) GUARDED((object_expr), attribute_from_variant(o, key, value)) \
static HRESULT WINAPI prefix##_DeleteItem(iface_type *self, REFGUID key) GUARDED((object_expr), attr_delete(o, key)) \
static HRESULT WINAPI prefix##_DeleteAllItems(iface_type *self) GUARDED((object_expr), attr_delete_all(o)) \
static HRESULT WINAPI prefix##_SetUINT32(iface_type *self, REFGUID key, UINT32 value) { attribute item = {.kind = ATTRIBUTE_U32, .value.u32 = value}; GUARDED((object_expr), attribute_store(o, key, &item)) } \
static HRESULT WINAPI prefix##_SetUINT64(iface_type *self, REFGUID key, UINT64 value) { attribute item = {.kind = ATTRIBUTE_U64, .value.u64 = value}; GUARDED((object_expr), attribute_store(o, key, &item)) } \
static HRESULT WINAPI prefix##_SetDouble(iface_type *self, REFGUID key, double value) { attribute item = {.kind = ATTRIBUTE_DOUBLE, .value.real = value}; GUARDED((object_expr), attribute_store(o, key, &item)) } \
static HRESULT WINAPI prefix##_SetGUID(iface_type *self, REFGUID key, REFGUID value) { attribute item = {.kind = ATTRIBUTE_GUID, .value.guid = *value}; GUARDED((object_expr), attribute_store(o, key, &item)) } \
static HRESULT WINAPI prefix##_SetString(iface_type *self, REFGUID key, LPCWSTR value) { if (!value) return E_INVALIDARG; attribute item = {.kind = ATTRIBUTE_STRING, .value.string = (WCHAR *)value}; GUARDED((object_expr), attribute_store(o, key, &item)) } \
static HRESULT WINAPI prefix##_SetBlob(iface_type *self, REFGUID key, const UINT8 *value, UINT32 size) { if (size && !value) return E_INVALIDARG; attribute item = {.kind = ATTRIBUTE_BLOB, .value.blob = {size, (UINT8 *)value}}; GUARDED((object_expr), attribute_store(o, key, &item)) } \
static HRESULT WINAPI prefix##_SetUnknown(iface_type *self, REFGUID key, IUnknown *value) { attribute item = {.kind = ATTRIBUTE_UNKNOWN, .value.unknown = value}; GUARDED((object_expr), attribute_store(o, key, &item)) } \
static HRESULT WINAPI prefix##_LockStore(iface_type *self) { EnterCriticalSection(&(object_expr)->lock); return S_OK; } \
static HRESULT WINAPI prefix##_UnlockStore(iface_type *self) { LeaveCriticalSection(&(object_expr)->lock); return S_OK; } \
static HRESULT WINAPI prefix##_GetCount(iface_type *self, UINT32 *count) GUARDED((object_expr), attr_get_count(o, count)) \
static HRESULT WINAPI prefix##_GetItemByIndex(iface_type *self, UINT32 index, GUID *key, PROPVARIANT *value) GUARDED((object_expr), attr_get_index(o, index, key, value)) \
static HRESULT WINAPI prefix##_CopyAllItems(iface_type *self, IMFAttributes *destination) SNAPSHOTTED((object_expr), attr_copy_all(o, destination))

ATTRIBUTE_WRAPPERS(type, IMFMediaType, object_from_type(self))
ATTRIBUTE_WRAPPERS(sample, IMFSample, object_from_sample(self))

static HRESULT WINAPI type_QueryInterface(IMFMediaType *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (guid_equal(iid, &IID_IUnknown) || guid_equal(iid, &IID_IMFAttributes) || guid_equal(iid, &IID_IMFMediaType)) {
		*out = self;
		object_addref(object_from_type(self));
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI type_AddRef(IMFMediaType *self) { return object_addref(object_from_type(self)); }
static ULONG WINAPI type_Release(IMFMediaType *self) { return object_release(object_from_type(self)); }

static HRESULT WINAPI type_GetMajorType(IMFMediaType *self, GUID *major)
{
	return type_GetGUID(self, &MF_MT_MAJOR_TYPE, major);
}

static HRESULT WINAPI type_IsCompressedFormat(IMFMediaType *self, WINBOOL *compressed)
{
	if (!compressed)
		return E_POINTER;
	const HRESULT status = type_GetUINT32(self, &MF_MT_COMPRESSED, (UINT32 *)compressed);
	if (status == MF_E_ATTRIBUTENOTFOUND) {
		*compressed = FALSE;
		return S_OK;
	}
	return status;
}

static HRESULT WINAPI type_IsEqual(IMFMediaType *self, IMFMediaType *other, DWORD *flags)
{
	if (!other)
		return E_POINTER;
	BOOL equal = FALSE;
	const HRESULT status = type_Compare(self, (IMFAttributes *)other, MF_ATTRIBUTES_MATCH_ALL_ITEMS, &equal);
	if (FAILED(status))
		return status;
	if (flags)
		*flags = equal ? MF_MEDIATYPE_EQUAL_MAJOR_TYPES | MF_MEDIATYPE_EQUAL_FORMAT_TYPES | MF_MEDIATYPE_EQUAL_FORMAT_DATA |
			MF_MEDIATYPE_EQUAL_FORMAT_USER_DATA : 0;
	return equal ? S_OK : S_FALSE;
}

static HRESULT WINAPI type_GetRepresentation(IMFMediaType *self, GUID representation, LPVOID *out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!guid_equal(&representation, &format_wave_format_ex) && !guid_equal(&representation, &AM_MEDIA_TYPE_REPRESENTATION))
		return MF_E_UNSUPPORTED_REPRESENTATION;
	AM_MEDIA_TYPE *media = CoTaskMemAlloc(sizeof(*media));
	if (!media)
		return E_OUTOFMEMORY;
	memset(media, 0, sizeof(*media));
	WAVEFORMATEX *format;
	UINT32 size;
	HRESULT status = MFCreateWaveFormatExFromMFMediaType(self, &format, &size, MFWaveFormatExConvertFlag_Normal);
	if (SUCCEEDED(status))
		status = type_GetGUID(self, &MF_MT_SUBTYPE, &media->subtype);
	if (FAILED(status)) {
		CoTaskMemFree(format);
		CoTaskMemFree(media);
		return status;
	}
	media->majortype = MFMediaType_Audio;
	media->bFixedSizeSamples = TRUE;
	media->lSampleSize = format->nBlockAlign;
	media->formattype = format_wave_format_ex;
	media->cbFormat = size;
	media->pbFormat = (BYTE *)format;
	*out = media;
	return S_OK;
}

static HRESULT WINAPI type_FreeRepresentation(IMFMediaType *self, GUID representation, LPVOID value)
{
	(void)self;
	(void)representation;
	if (value)
		CoTaskMemFree(((AM_MEDIA_TYPE *)value)->pbFormat);
	CoTaskMemFree(value);
	return S_OK;
}

static IMFMediaTypeVtbl media_type_vtable = {
	type_QueryInterface, type_AddRef, type_Release,
	type_GetItem, type_GetItemType, type_CompareItem, type_Compare, type_GetUINT32, type_GetUINT64, type_GetDouble, type_GetGUID,
	type_GetStringLength, type_GetString, type_GetAllocatedString, type_GetBlobSize, type_GetBlob, type_GetAllocatedBlob, type_GetUnknown,
	type_SetItem, type_DeleteItem, type_DeleteAllItems, type_SetUINT32, type_SetUINT64, type_SetDouble, type_SetGUID, type_SetString,
	type_SetBlob, type_SetUnknown, type_LockStore, type_UnlockStore, type_GetCount, type_GetItemByIndex, type_CopyAllItems,
	type_GetMajorType, type_IsCompressedFormat, type_IsEqual, type_GetRepresentation, type_FreeRepresentation
};

static HRESULT WINAPI sample_QueryInterface(IMFSample *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (guid_equal(iid, &IID_IUnknown) || guid_equal(iid, &IID_IMFAttributes) || guid_equal(iid, &IID_IMFSample)) {
		*out = self;
		object_addref(object_from_sample(self));
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG WINAPI sample_AddRef(IMFSample *self) { return object_addref(object_from_sample(self)); }
static ULONG WINAPI sample_Release(IMFSample *self) { return object_release(object_from_sample(self)); }
static HRESULT WINAPI sample_GetSampleFlags(IMFSample *self, DWORD *flags)
{
	if (!flags)
		return E_POINTER;
	*flags = object_from_sample(self)->sample_flags;
	return S_OK;
}
static HRESULT WINAPI sample_SetSampleFlags(IMFSample *self, DWORD flags) { object_from_sample(self)->sample_flags = flags; return S_OK; }
static HRESULT WINAPI sample_GetSampleTime(IMFSample *self, LONGLONG *time)
{
	if (!time)
		return E_POINTER;
	mf_object *object = object_from_sample(self);
	EnterCriticalSection(&object->lock);
	const BOOL known = object->has_sample_time;
	if (known)
		*time = object->sample_time;
	LeaveCriticalSection(&object->lock);
	return known ? S_OK : MF_E_NO_SAMPLE_TIMESTAMP;
}
static HRESULT WINAPI sample_SetSampleTime(IMFSample *self, LONGLONG time)
{
	mf_object *object = object_from_sample(self);
	EnterCriticalSection(&object->lock);
	object->sample_time = time;
	object->has_sample_time = TRUE;
	LeaveCriticalSection(&object->lock);
	return S_OK;
}
static HRESULT WINAPI sample_GetSampleDuration(IMFSample *self, LONGLONG *duration)
{
	if (!duration)
		return E_POINTER;
	mf_object *object = object_from_sample(self);
	EnterCriticalSection(&object->lock);
	const BOOL known = object->has_sample_duration;
	if (known)
		*duration = object->sample_duration;
	LeaveCriticalSection(&object->lock);
	return known ? S_OK : MF_E_NO_SAMPLE_DURATION;
}
static HRESULT WINAPI sample_SetSampleDuration(IMFSample *self, LONGLONG duration)
{
	mf_object *object = object_from_sample(self);
	EnterCriticalSection(&object->lock);
	object->sample_duration = duration;
	object->has_sample_duration = TRUE;
	LeaveCriticalSection(&object->lock);
	return S_OK;
}
static HRESULT WINAPI sample_GetBufferCount(IMFSample *self, DWORD *count)
{
	if (!count)
		return E_POINTER;
	*count = object_from_sample(self)->buffer_count;
	return S_OK;
}

static HRESULT buffers_get(mf_object *object, DWORD index, IMFMediaBuffer **buffer)
{
	if (index >= object->buffer_count)
		return MF_E_INVALIDINDEX;
	*buffer = object->buffers[index];
	IMFMediaBuffer_AddRef(*buffer);
	return S_OK;
}

static HRESULT buffers_add(mf_object *object, IMFMediaBuffer *buffer)
{
	if (object->buffer_count == object->buffer_capacity) {
		const DWORD capacity = object->buffer_capacity ? object->buffer_capacity * 2 : 4;
		if (capacity < object->buffer_capacity || capacity > SIZE_MAX / sizeof(*object->buffers))
			return E_OUTOFMEMORY;
		IMFMediaBuffer **buffers = LocalAlloc(LMEM_FIXED, capacity * sizeof(*buffers));
		if (!buffers)
			return E_OUTOFMEMORY;
		if (object->buffer_count)
			memcpy(buffers, object->buffers, object->buffer_count * sizeof(*buffers));
		LocalFree(object->buffers);
		object->buffers = buffers;
		object->buffer_capacity = capacity;
	}
	IMFMediaBuffer_AddRef(buffer);
	object->buffers[object->buffer_count++] = buffer;
	return S_OK;
}

static HRESULT buffers_remove(mf_object *object, DWORD index)
{
	if (index >= object->buffer_count)
		return MF_E_INVALIDINDEX;
	IMFMediaBuffer_Release(object->buffers[index]);
	if (index + 1 < object->buffer_count)
		memmove(&object->buffers[index], &object->buffers[index + 1], (object->buffer_count - index - 1) * sizeof(*object->buffers));
	--object->buffer_count;
	return S_OK;
}

static HRESULT buffers_remove_all(mf_object *object)
{
	while (object->buffer_count) {
		const DWORD index = --object->buffer_count;
		IMFMediaBuffer_Release(object->buffers[index]);
	}
	return S_OK;
}

static HRESULT buffers_total_length(mf_object *object, DWORD *length)
{
	*length = 0;
	for (DWORD i = 0; i < object->buffer_count; ++i) {
		DWORD current;
		const HRESULT status = IMFMediaBuffer_GetCurrentLength(object->buffers[i], &current);
		if (FAILED(status))
			return status;
		if (current > UINT32_MAX - *length)
			return E_OUTOFMEMORY;
		*length += current;
	}
	return S_OK;
}

static HRESULT buffers_copy(mf_object *object, IMFMediaBuffer *destination)
{
	BYTE *target;
	DWORD target_max, target_length;
	HRESULT status = IMFMediaBuffer_Lock(destination, &target, &target_max, &target_length);
	if (FAILED(status))
		return status;
	DWORD offset = 0;
	for (DWORD i = 0; i < object->buffer_count && SUCCEEDED(status); ++i) {
		BYTE *source;
		DWORD max_length, current;
		status = IMFMediaBuffer_Lock(object->buffers[i], &source, &max_length, &current);
		if (FAILED(status))
			break;
		if (current > target_max - offset)
			status = MF_E_BUFFERTOOSMALL;
		else {
			if (current)
				memcpy(target + offset, source, current);
			offset += current;
		}
		const HRESULT unlock = IMFMediaBuffer_Unlock(object->buffers[i]);
		if (SUCCEEDED(status))
			status = unlock;
	}
	if (SUCCEEDED(status))
		status = IMFMediaBuffer_SetCurrentLength(destination, offset);
	const HRESULT unlock = IMFMediaBuffer_Unlock(destination);
	return FAILED(status) ? status : unlock;
}

static HRESULT buffers_contiguous(mf_object *object, IMFMediaBuffer **out);

static HRESULT WINAPI sample_GetBufferByIndex(IMFSample *self, DWORD index, IMFMediaBuffer **buffer)
{
	if (!buffer)
		return E_POINTER;
	*buffer = NULL;
	GUARDED(object_from_sample(self), buffers_get(o, index, buffer))
}
static HRESULT WINAPI sample_ConvertToContiguousBuffer(IMFSample *self, IMFMediaBuffer **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	GUARDED(object_from_sample(self), buffers_contiguous(o, out))
}
static HRESULT WINAPI sample_AddBuffer(IMFSample *self, IMFMediaBuffer *buffer)
{
	if (!buffer)
		return E_POINTER;
	GUARDED(object_from_sample(self), buffers_add(o, buffer))
}
static HRESULT WINAPI sample_RemoveBufferByIndex(IMFSample *self, DWORD index) GUARDED(object_from_sample(self), buffers_remove(o, index))
static HRESULT WINAPI sample_RemoveAllBuffers(IMFSample *self) GUARDED(object_from_sample(self), buffers_remove_all(o))
static HRESULT WINAPI sample_GetTotalLength(IMFSample *self, DWORD *length)
{
	if (!length)
		return E_POINTER;
	GUARDED(object_from_sample(self), buffers_total_length(o, length))
}
static HRESULT WINAPI sample_CopyToBuffer(IMFSample *self, IMFMediaBuffer *destination)
{
	if (!destination)
		return E_POINTER;
	GUARDED(object_from_sample(self), buffers_copy(o, destination))
}

static IMFSampleVtbl sample_vtable = {
	sample_QueryInterface, sample_AddRef, sample_Release,
	sample_GetItem, sample_GetItemType, sample_CompareItem, sample_Compare, sample_GetUINT32, sample_GetUINT64, sample_GetDouble, sample_GetGUID,
	sample_GetStringLength, sample_GetString, sample_GetAllocatedString, sample_GetBlobSize, sample_GetBlob, sample_GetAllocatedBlob, sample_GetUnknown,
	sample_SetItem, sample_DeleteItem, sample_DeleteAllItems, sample_SetUINT32, sample_SetUINT64, sample_SetDouble, sample_SetGUID, sample_SetString,
	sample_SetBlob, sample_SetUnknown, sample_LockStore, sample_UnlockStore, sample_GetCount, sample_GetItemByIndex, sample_CopyAllItems,
	sample_GetSampleFlags, sample_SetSampleFlags, sample_GetSampleTime, sample_SetSampleTime, sample_GetSampleDuration, sample_SetSampleDuration,
	sample_GetBufferCount, sample_GetBufferByIndex, sample_ConvertToContiguousBuffer, sample_AddBuffer, sample_RemoveBufferByIndex,
	sample_RemoveAllBuffers, sample_GetTotalLength, sample_CopyToBuffer
};

static HRESULT WINAPI buffer_QueryInterface(IMFMediaBuffer *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (guid_equal(iid, &IID_IUnknown) || guid_equal(iid, &IID_IMFMediaBuffer)) {
		*out = self;
		InterlockedIncrement(&((media_buffer *)self)->ref);
		return S_OK;
	}
	return E_NOINTERFACE;
}
static ULONG WINAPI buffer_AddRef(IMFMediaBuffer *self) { return (ULONG)InterlockedIncrement(&((media_buffer *)self)->ref); }
static ULONG WINAPI buffer_Release(IMFMediaBuffer *self)
{
	media_buffer *buffer = (media_buffer *)self;
	const LONG ref = InterlockedDecrement(&buffer->ref);
	if (!ref) {
		LocalFree(buffer->allocation);
		LocalFree(buffer);
	}
	return (ULONG)ref;
}
static HRESULT WINAPI buffer_Lock(IMFMediaBuffer *self, BYTE **data, DWORD *max_length, DWORD *current_length)
{
	if (!data)
		return E_POINTER;
	media_buffer *buffer = (media_buffer *)self;
	*data = buffer->data;
	if (max_length)
		*max_length = buffer->max_length;
	if (current_length)
		*current_length = buffer->current_length;
	InterlockedIncrement((volatile LONG *)&buffer->lock_count);
	return S_OK;
}
static HRESULT WINAPI buffer_Unlock(IMFMediaBuffer *self)
{
	media_buffer *buffer = (media_buffer *)self;
	if (!buffer->lock_count)
		return MF_E_INVALIDREQUEST;
	InterlockedDecrement((volatile LONG *)&buffer->lock_count);
	return S_OK;
}
static HRESULT WINAPI buffer_GetCurrentLength(IMFMediaBuffer *self, DWORD *length)
{
	if (!length)
		return E_POINTER;
	*length = ((media_buffer *)self)->current_length;
	return S_OK;
}
static HRESULT WINAPI buffer_SetCurrentLength(IMFMediaBuffer *self, DWORD length)
{
	media_buffer *buffer = (media_buffer *)self;
	if (length > buffer->max_length)
		return E_INVALIDARG;
	buffer->current_length = length;
	return S_OK;
}
static HRESULT WINAPI buffer_GetMaxLength(IMFMediaBuffer *self, DWORD *length)
{
	if (!length)
		return E_POINTER;
	*length = ((media_buffer *)self)->max_length;
	return S_OK;
}

static IMFMediaBufferVtbl media_buffer_vtable = {
	buffer_QueryInterface, buffer_AddRef, buffer_Release, buffer_Lock, buffer_Unlock, buffer_GetCurrentLength,
	buffer_SetCurrentLength, buffer_GetMaxLength
};

static volatile LONG startup_count;

__declspec(dllexport) HRESULT WINAPI MFStartup(ULONG version, DWORD flags)
{
	if (version != MF_VERSION)
		return MF_E_BAD_STARTUP_VERSION;
	if (flags != MFSTARTUP_FULL && flags != MFSTARTUP_NOSOCKET)
		return E_INVALIDARG;
	InterlockedIncrement(&startup_count);
	return S_OK;
}

__declspec(dllexport) HRESULT WINAPI MFShutdown(void)
{
	LONG current = InterlockedCompareExchange(&startup_count, 0, 0);
	while (current > 0) {
		const LONG previous = InterlockedCompareExchange(&startup_count, current - 1, current);
		if (previous == current)
			return S_OK;
		current = previous;
	}
	return MF_E_SHUTDOWN;
}

static HRESULT create_buffer(DWORD max_length, DWORD alignment, IMFMediaBuffer **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if ((alignment & (alignment - 1)) || alignment > 4096)
		return E_INVALIDARG;
	media_buffer *buffer = LocalAlloc(LPTR, sizeof(*buffer));
	if (!buffer)
		return E_OUTOFMEMORY;
	buffer->iface.lpVtbl = &media_buffer_vtable;
	buffer->ref = 1;
	buffer->max_length = max_length;
	if (max_length) {
		const DWORD align = alignment;
		if (max_length > UINT32_MAX - align + 1) {
			LocalFree(buffer);
			return E_INVALIDARG;
		}
		buffer->allocation = LocalAlloc(LPTR, max_length + align - 1);
		if (!buffer->allocation) {
			LocalFree(buffer);
			return E_OUTOFMEMORY;
		}
		buffer->data = (BYTE *)(((uintptr_t)buffer->allocation + align - 1) & ~(uintptr_t)(align - 1));
	}
	*out = &buffer->iface;
	return S_OK;
}

static HRESULT buffers_contiguous(mf_object *object, IMFMediaBuffer **out)
{
	if (!object->buffer_count)
		return E_UNEXPECTED;
	if (object->buffer_count == 1)
		return buffers_get(object, 0, out);
	DWORD total;
	HRESULT status = buffers_total_length(object, &total);
	if (FAILED(status))
		return status;
	status = create_buffer(total, 1, out);
	if (FAILED(status))
		return status;
	status = buffers_copy(object, *out);
	if (SUCCEEDED(status)) {
		buffers_remove_all(object);
		status = buffers_add(object, *out);
	}
	if (FAILED(status)) {
		IMFMediaBuffer_Release(*out);
		*out = NULL;
	}
	return status;
}

static mf_object *create_object(void)
{
	mf_object *object = LocalAlloc(LPTR, sizeof(*object));
	if (object) {
		object->ref = 1;
		InitializeCriticalSection(&object->lock);
	}
	return object;
}

__declspec(dllexport) HRESULT WINAPI MFCreateSample(IMFSample **sample)
{
	if (!sample)
		return E_POINTER;
	*sample = NULL;
	mf_object *object = create_object();
	if (!object)
		return E_OUTOFMEMORY;
	object->sample.lpVtbl = &sample_vtable;
	*sample = &object->sample;
	return S_OK;
}

__declspec(dllexport) HRESULT WINAPI MFCreateMediaType(IMFMediaType **type)
{
	if (!type)
		return E_POINTER;
	*type = NULL;
	mf_object *object = create_object();
	if (!object)
		return E_OUTOFMEMORY;
	object->media_type.lpVtbl = &media_type_vtable;
	*type = &object->media_type;
	return S_OK;
}

__declspec(dllexport) HRESULT WINAPI MFCreateMemoryBuffer(DWORD max_length, IMFMediaBuffer **buffer)
{
	return create_buffer(max_length, 1, buffer);
}

__declspec(dllexport) HRESULT WINAPI MFCreateAlignedMemoryBuffer(DWORD max_length, DWORD alignment, IMFMediaBuffer **buffer)
{
	/* The argument is a mask such as MF_16_BYTE_ALIGNMENT, one less than the alignment. */
	return alignment == UINT32_MAX ? E_INVALIDARG : create_buffer(max_length, alignment + 1, buffer);
}

__declspec(dllexport) HRESULT WINAPI MFInitMediaTypeFromWaveFormatEx(IMFMediaType *type, const WAVEFORMATEX *format, UINT32 size)
{
	if (!type || !format || size < sizeof(WAVEFORMATEX))
		return E_INVALIDARG;
	if (format->cbSize > size - sizeof(WAVEFORMATEX))
		return E_INVALIDARG;
	const BOOL extensible = format->wFormatTag == WAVE_FORMAT_EXTENSIBLE;
	GUID subtype;
	DWORD channel_mask = 0, valid_bits = 0;
	if (extensible) {
		if (size < sizeof(WAVEFORMATEXTENSIBLE) || format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
			return E_INVALIDARG;
		const WAVEFORMATEXTENSIBLE *extended = (const WAVEFORMATEXTENSIBLE *)format;
		if (!guid_equal(&extended->SubFormat, &MFAudioFormat_PCM) &&
			!guid_equal(&extended->SubFormat, &MFAudioFormat_Float))
			return MF_E_INVALIDMEDIATYPE;
		subtype = extended->SubFormat;
		channel_mask = extended->dwChannelMask;
		valid_bits = extended->Samples.wValidBitsPerSample;
	} else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		subtype = MFAudioFormat_Float;
	else if (format->wFormatTag == WAVE_FORMAT_PCM)
		subtype = MFAudioFormat_PCM;
	else if (format->wFormatTag == WAVE_FORMAT_ADPCM)
		subtype = subtype_adpcm;
	else
		return MF_E_INVALIDMEDIATYPE;
	const GUID audio_major = MFMediaType_Audio;
	HRESULT status = IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &audio_major);
	if (FAILED(status))
		return status;
	status = IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, &subtype);
	if (SUCCEEDED(status)) status = IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, format->nChannels);
	if (SUCCEEDED(status)) status = IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, format->nSamplesPerSec);
	if (SUCCEEDED(status)) status = IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, format->nAvgBytesPerSec);
	if (SUCCEEDED(status)) status = IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, format->nBlockAlign);
	if (SUCCEEDED(status)) status = IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, format->wBitsPerSample);
	if (SUCCEEDED(status) && extensible)
		status = IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_VALID_BITS_PER_SAMPLE, valid_bits);
	if (SUCCEEDED(status) && extensible)
		status = IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_CHANNEL_MASK, channel_mask);
	if (FAILED(status))
		return status;
	if (format->cbSize && !extensible)
		status = IMFMediaType_SetBlob(type, &MF_MT_USER_DATA, (const UINT8 *)(format + 1), format->cbSize);
	else
		status = IMFMediaType_DeleteItem(type, &MF_MT_USER_DATA);
	if (SUCCEEDED(status) && !extensible)
		status = IMFMediaType_DeleteItem(type, &MF_MT_AUDIO_CHANNEL_MASK);
	if (SUCCEEDED(status) && !extensible)
		status = IMFMediaType_DeleteItem(type, &MF_MT_AUDIO_VALID_BITS_PER_SAMPLE);
	return status;
}

__declspec(dllexport) HRESULT WINAPI MFCreateWaveFormatExFromMFMediaType(IMFMediaType *type, WAVEFORMATEX **format,
	UINT32 *size, UINT32 flags)
{
	if (!type || !format || !size)
		return E_POINTER;
	*format = NULL;
	*size = 0;
	GUID major, subtype;
	UINT32 channels, rate, bits, alignment, average;
	if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major)) || !guid_equal(&major, &MFMediaType_Audio) ||
		FAILED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)) ||
		FAILED(IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &channels)) ||
		FAILED(IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) ||
		FAILED(IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)))
		return MF_E_INVALIDMEDIATYPE;
	if (!channels || channels > UINT16_MAX || !rate || bits > UINT16_MAX)
		return MF_E_INVALIDMEDIATYPE;
	const UINT32 sample_bytes = (bits + 7) / 8;
	if (!sample_bytes || channels > UINT32_MAX / sample_bytes)
		return MF_E_INVALIDMEDIATYPE;
	if (FAILED(IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, &alignment)))
		alignment = channels * sample_bytes;
	if (!alignment || rate > UINT32_MAX / alignment)
		return MF_E_INVALIDMEDIATYPE;
	average = rate * alignment;
	IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &average);
	if (alignment > UINT16_MAX)
		return MF_E_INVALIDMEDIATYPE;
	UINT32 extra = 0;
	UINT8 *user_data = NULL;
	if (guid_equal(&subtype, &subtype_adpcm)) {
		HRESULT status = IMFMediaType_GetBlobSize(type, &MF_MT_USER_DATA, &extra);
		if (FAILED(status))
			return MF_E_INVALIDMEDIATYPE;
		if (extra) {
			user_data = CoTaskMemAlloc(extra);
			if (!user_data)
				return E_OUTOFMEMORY;
			status = IMFMediaType_GetBlob(type, &MF_MT_USER_DATA, user_data, extra, NULL);
			if (FAILED(status)) {
				CoTaskMemFree(user_data);
				return status;
			}
		}
	}
	if (extra > UINT16_MAX || extra > UINT32_MAX - sizeof(WAVEFORMATEX)) {
		CoTaskMemFree(user_data);
		return MF_E_INVALIDMEDIATYPE;
	}
	UINT32 valid_bits = bits, channel_mask = channels == 1 ? SPEAKER_FRONT_CENTER : channels < 32 ? (1u << channels) - 1 : 0;
	IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_VALID_BITS_PER_SAMPLE, &valid_bits);
	IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_CHANNEL_MASK, &channel_mask);
	/* A plain WAVEFORMATEX cannot say where more than two channels go or that some sample bits are padding. */
	const BOOL extensible = !guid_equal(&subtype, &subtype_adpcm) &&
		(flags == MFWaveFormatExConvertFlag_ForceExtensible || channels > 2 || valid_bits != bits);
	if (extensible)
		extra = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	const DWORD cb = (DWORD)sizeof(WAVEFORMATEX) + extra;
	WAVEFORMATEX *result = CoTaskMemAlloc(cb);
	if (!result) {
		CoTaskMemFree(user_data);
		return E_OUTOFMEMORY;
	}
	memset(result, 0, cb);
	result->wFormatTag = guid_equal(&subtype, &MFAudioFormat_PCM) ? WAVE_FORMAT_PCM :
		guid_equal(&subtype, &MFAudioFormat_Float) ? WAVE_FORMAT_IEEE_FLOAT :
		guid_equal(&subtype, &subtype_adpcm) ? WAVE_FORMAT_ADPCM : WAVE_FORMAT_UNKNOWN;
	if (result->wFormatTag == WAVE_FORMAT_UNKNOWN) {
		CoTaskMemFree(user_data);
		CoTaskMemFree(result);
		return MF_E_INVALIDMEDIATYPE;
	}
	result->nChannels = (WORD)channels;
	result->nSamplesPerSec = rate;
	result->wBitsPerSample = (WORD)bits;
	result->nBlockAlign = (WORD)alignment;
	result->nAvgBytesPerSec = average;
	result->cbSize = (WORD)extra;
	if (extensible) {
		WAVEFORMATEXTENSIBLE *extended = (WAVEFORMATEXTENSIBLE *)result;
		result->wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		extended->Samples.wValidBitsPerSample = (WORD)valid_bits;
		extended->dwChannelMask = channel_mask;
		extended->SubFormat = subtype;
	} else if (extra)
		memcpy(result + 1, user_data, extra);
	CoTaskMemFree(user_data);
	*format = result;
	*size = cb;
	return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	(void)instance;
	(void)reason;
	(void)reserved;
	return TRUE;
}
