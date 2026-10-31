/* SPDX-License-Identifier: MIT */
#include "ps.h"

/* One list of keys behind the store and its serialized form. Serialization follows the published property store format
 * (one storage per format id, 'SPS1' signature), and carries the value types that format calls simple. */

typedef struct
{
	PROPERTYKEY key;
	PROPVARIANT value;
	PSC_STATE state;
} item;

typedef struct
{
	IPropertyStoreCache cache;
	IPersistSerializedPropStorage serialized;
	LONG refs;
	CRITICAL_SECTION lock;
	item *items;
	DWORD count;
	DWORD room;
} store;

#define SIGNATURE 0x53505331

static store *from_cache(IPropertyStoreCache *self)
{
	return CONTAINING_RECORD(self, store, cache);
}

static store *from_serialized(IPersistSerializedPropStorage *self)
{
	return CONTAINING_RECORD(self, store, serialized);
}

static HRESULT WINAPI cache_QueryInterface(IPropertyStoreCache *self, REFIID riid, void **out)
{
	store *object = from_cache(self);
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IPropertyStore) || IsEqualGUID(riid, &IID_IPropertyStoreCache))
		*out = &object->cache;
	else if (IsEqualGUID(riid, &IID_IPersistSerializedPropStorage))
		*out = &object->serialized;
	else
	{
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&object->refs);
	return S_OK;
}

static ULONG WINAPI cache_AddRef(IPropertyStoreCache *self)
{
	return InterlockedIncrement(&from_cache(self)->refs);
}

static void drop_items(store *object)
{
	DWORD index;
	for (index = 0; index < object->count; index++)
		PropVariantClear(&object->items[index].value);
	object->count = 0;
}

static ULONG WINAPI cache_Release(IPropertyStoreCache *self)
{
	store *object = from_cache(self);
	LONG left = InterlockedDecrement(&object->refs);
	if (!left)
	{
		drop_items(object);
		CoTaskMemFree(object->items);
		DeleteCriticalSection(&object->lock);
		CoTaskMemFree(object);
		InterlockedDecrement(&ps_locks);
	}
	return left;
}

static HRESULT WINAPI cache_GetCount(IPropertyStoreCache *self, DWORD *out)
{
	if (!out)
		return E_POINTER;
	*out = from_cache(self)->count;
	return S_OK;
}

static HRESULT WINAPI cache_GetAt(IPropertyStoreCache *self, DWORD index, PROPERTYKEY *out)
{
	store *object = from_cache(self);
	HRESULT status = E_INVALIDARG;
	if (!out)
		return E_POINTER;
	EnterCriticalSection(&object->lock);
	if (index < object->count)
	{
		*out = object->items[index].key;
		status = S_OK;
	}
	LeaveCriticalSection(&object->lock);
	return status;
}

static item *find_item(store *object, REFPROPERTYKEY key)
{
	DWORD index;
	for (index = 0; index < object->count; index++)
		if (object->items[index].key.pid == key->pid && IsEqualGUID(&object->items[index].key.fmtid, &key->fmtid))
			return &object->items[index];
	return NULL;
}

static HRESULT WINAPI cache_GetValueAndState(IPropertyStoreCache *self, REFPROPERTYKEY key, PROPVARIANT *value, PSC_STATE *state)
{
	store *object = from_cache(self);
	item *found;
	HRESULT status = TYPE_E_ELEMENTNOTFOUND;
	if (!key)
		return E_POINTER;
	if (value)
		PropVariantInit(value);
	if (state)
		*state = PSC_NORMAL;
	EnterCriticalSection(&object->lock);
	if ((found = find_item(object, key)))
	{
		status = value ? PropVariantCopy(value, &found->value) : S_OK;
		if (state)
			*state = found->state;
	}
	LeaveCriticalSection(&object->lock);
	return status;
}

static HRESULT WINAPI cache_GetValue(IPropertyStoreCache *self, REFPROPERTYKEY key, PROPVARIANT *value)
{
	HRESULT status;
	if (!value)
		return E_POINTER;
	status = cache_GetValueAndState(self, key, value, NULL);
	return status == TYPE_E_ELEMENTNOTFOUND ? S_OK : status;
}

static HRESULT WINAPI cache_GetState(IPropertyStoreCache *self, REFPROPERTYKEY key, PSC_STATE *state)
{
	if (!state)
		return E_POINTER;
	return cache_GetValueAndState(self, key, NULL, state);
}

static HRESULT WINAPI cache_SetState(IPropertyStoreCache *self, REFPROPERTYKEY key, PSC_STATE state)
{
	store *object = from_cache(self);
	item *found;
	HRESULT status = TYPE_E_ELEMENTNOTFOUND;
	if (!key)
		return E_POINTER;
	EnterCriticalSection(&object->lock);
	if ((found = find_item(object, key)))
	{
		found->state = state;
		status = S_OK;
	}
	LeaveCriticalSection(&object->lock);
	return status;
}

static HRESULT put_item(store *object, REFPROPERTYKEY key, const PROPVARIANT *value, const PSC_STATE *state)
{
	item *found = find_item(object, key);
	PROPVARIANT copy;
	HRESULT status;
	PropVariantInit(&copy);
	status = PropVariantCopy(&copy, value);
	if (FAILED(status))
		return status;
	if (!found)
	{
		if (object->count == object->room)
		{
			DWORD room = object->room ? object->room * 2 : 8;
			item *grown = CoTaskMemRealloc(object->items, room * sizeof(item));
			if (!grown)
			{
				PropVariantClear(&copy);
				return E_OUTOFMEMORY;
			}
			object->items = grown;
			object->room = room;
		}
		found = &object->items[object->count++];
		found->key = *key;
		found->state = PSC_NORMAL;
		PropVariantInit(&found->value);
	}
	PropVariantClear(&found->value);
	found->value = copy;
	if (state)
		found->state = *state;
	return S_OK;
}

static HRESULT WINAPI cache_SetValue(IPropertyStoreCache *self, REFPROPERTYKEY key, REFPROPVARIANT value)
{
	store *object = from_cache(self);
	HRESULT status;
	if (!key || !value)
		return E_POINTER;
	EnterCriticalSection(&object->lock);
	status = put_item(object, key, value, NULL);
	LeaveCriticalSection(&object->lock);
	return status;
}

static HRESULT WINAPI cache_SetValueAndState(IPropertyStoreCache *self, REFPROPERTYKEY key, const PROPVARIANT *value, PSC_STATE state)
{
	store *object = from_cache(self);
	HRESULT status;
	if (!key || !value)
		return E_POINTER;
	EnterCriticalSection(&object->lock);
	status = put_item(object, key, value, &state);
	LeaveCriticalSection(&object->lock);
	return status;
}

static HRESULT WINAPI cache_Commit(IPropertyStoreCache *self)
{
	return S_OK;
}

static const IPropertyStoreCacheVtbl cache_vtbl = {
	cache_QueryInterface,
	cache_AddRef,
	cache_Release,
	cache_GetCount,
	cache_GetAt,
	cache_GetValue,
	cache_SetValue,
	cache_Commit,
	cache_GetState,
	cache_GetValueAndState,
	cache_SetState,
	cache_SetValueAndState,
};

/* Serialization */

static HRESULT WINAPI serialized_QueryInterface(IPersistSerializedPropStorage *self, REFIID riid, void **out)
{
	return cache_QueryInterface(&from_serialized(self)->cache, riid, out);
}

static ULONG WINAPI serialized_AddRef(IPersistSerializedPropStorage *self)
{
	return cache_AddRef(&from_serialized(self)->cache);
}

static ULONG WINAPI serialized_Release(IPersistSerializedPropStorage *self)
{
	return cache_Release(&from_serialized(self)->cache);
}

static HRESULT WINAPI serialized_SetFlags(IPersistSerializedPropStorage *self, PERSIST_SPROPSTORE_FLAGS flags)
{
	return S_OK;
}

/* How many bytes a value takes on the wire after its type word and padding; zero when the format cannot carry it here. */
static DWORD value_size(const PROPVARIANT *value)
{
	switch (value->vt)
	{
	case VT_EMPTY:
	case VT_NULL:
		return 0;
	case VT_BOOL:
	case VT_I2:
	case VT_UI2:
	case VT_I4:
	case VT_UI4:
	case VT_R4:
		return 4;
	case VT_I8:
	case VT_UI8:
	case VT_R8:
	case VT_FILETIME:
		return 8;
	case VT_CLSID:
		return sizeof(CLSID);
	case VT_LPWSTR:
	{
		DWORD chars = value->pwszVal ? lstrlenW(value->pwszVal) + 1 : 1;
		return 4 + (chars * 2 + 3 & ~3u);
	}
	}
	return 0;
}

static BOOL value_carried(VARTYPE type)
{
	switch (type)
	{
	case VT_EMPTY:
	case VT_NULL:
	case VT_BOOL:
	case VT_I2:
	case VT_UI2:
	case VT_I4:
	case VT_UI4:
	case VT_R4:
	case VT_I8:
	case VT_UI8:
	case VT_R8:
	case VT_FILETIME:
	case VT_CLSID:
	case VT_LPWSTR:
		return TRUE;
	}
	return FALSE;
}

static BYTE *put_dword(BYTE *out, DWORD value)
{
	memcpy(out, &value, 4);
	return out + 4;
}

static BYTE *write_value(BYTE *out, const PROPVARIANT *value)
{
	DWORD size = value_size(value), chars;
	out = put_dword(out, value->vt);
	memset(out, 0, size);
	switch (value->vt)
	{
	case VT_EMPTY:
	case VT_NULL:
		break;
	case VT_CLSID:
		memcpy(out, value->puuid, sizeof(CLSID));
		break;
	case VT_LPWSTR:
		chars = value->pwszVal ? lstrlenW(value->pwszVal) + 1 : 1;
		put_dword(out, chars);
		if (value->pwszVal)
			memcpy(out + 4, value->pwszVal, chars * 2);
		break;
	default:
		memcpy(out, &value->bVal, size);
		break;
	}
	return out + size;
}

static HRESULT WINAPI serialized_GetPropertyStorage(IPersistSerializedPropStorage *self, SERIALIZEDPROPSTORAGE **out, DWORD *size)
{
	store *object = from_serialized(self);
	BYTE *data = NULL, *cursor;
	DWORD index, total = 0;
	if (!out || !size)
		return E_POINTER;
	*out = NULL;
	*size = 0;
	EnterCriticalSection(&object->lock);
	/* Each carried item goes in a storage of its own: readers take any sequence of storages, repeated format ids included. */
	for (index = 0; index < object->count; index++)
		if (value_carried(object->items[index].value.vt))
			total += 4 + 4 + 16 + 4 + 4 + 1 + 4 + value_size(&object->items[index].value) + 4;
	if (total)
	{
		total += 4;
		if (!(data = CoTaskMemAlloc(total)))
		{
			LeaveCriticalSection(&object->lock);
			return E_OUTOFMEMORY;
		}
		cursor = data;
		for (index = 0; index < object->count; index++)
		{
			const item *entry = &object->items[index];
			DWORD payload;
			if (!value_carried(entry->value.vt))
				continue;
			payload = 4 + 4 + 1 + 4 + value_size(&entry->value);
			cursor = put_dword(cursor, 4 + 4 + 16 + payload + 4);
			cursor = put_dword(cursor, SIGNATURE);
			memcpy(cursor, &entry->key.fmtid, 16);
			cursor += 16;
			cursor = put_dword(cursor, payload);
			cursor = put_dword(cursor, entry->key.pid);
			*cursor++ = 0;
			cursor = write_value(cursor, &entry->value);
			cursor = put_dword(cursor, 0);
		}
		put_dword(cursor, 0);
	}
	LeaveCriticalSection(&object->lock);
	*out = (SERIALIZEDPROPSTORAGE *)data;
	*size = total;
	return S_OK;
}

static DWORD get_dword(const BYTE *data)
{
	DWORD value;
	memcpy(&value, data, 4);
	return value;
}

static HRESULT read_value(store *object, REFPROPERTYKEY key, const BYTE *data, DWORD size)
{
	PROPVARIANT value;
	VARTYPE type;
	DWORD chars;
	HRESULT status;
	if (size < 4)
		return E_INVALIDARG;
	type = (VARTYPE)get_dword(data);
	data += 4;
	size -= 4;
	PropVariantInit(&value);
	value.vt = type;
	switch (type)
	{
	case VT_EMPTY:
	case VT_NULL:
		break;
	case VT_BOOL:
	case VT_I2:
	case VT_UI2:
	case VT_I4:
	case VT_UI4:
	case VT_R4:
		if (size < 4)
			return E_INVALIDARG;
		memcpy(&value.bVal, data, 4);
		break;
	case VT_I8:
	case VT_UI8:
	case VT_R8:
	case VT_FILETIME:
		if (size < 8)
			return E_INVALIDARG;
		memcpy(&value.bVal, data, 8);
		break;
	case VT_CLSID:
		if (size < sizeof(CLSID))
			return E_INVALIDARG;
		value.puuid = (CLSID *)data;
		break;
	case VT_LPWSTR:
		if (size < 4)
			return E_INVALIDARG;
		chars = get_dword(data);
		if (!chars || chars > (size - 4) / 2 || *(const WCHAR *)(data + 4 + (chars - 1) * 2))
			return E_INVALIDARG;
		value.pwszVal = (WCHAR *)(data + 4);
		break;
	default:
		return S_OK;
	}
	/* The copy made by the store owns everything; the borrowed pointers above are not cleared. */
	status = put_item(object, key, &value, NULL);
	return status;
}

static HRESULT WINAPI serialized_SetPropertyStorage(IPersistSerializedPropStorage *self, PCUSERIALIZEDPROPSTORAGE source, DWORD size)
{
	store *object = from_serialized(self);
	const BYTE *data = (const BYTE *)source;
	HRESULT status = S_OK;
	if (!source && size)
		return E_POINTER;
	EnterCriticalSection(&object->lock);
	drop_items(object);
	while (SUCCEEDED(status) && size >= 4)
	{
		DWORD storage = get_dword(data);
		PROPERTYKEY key;
		const BYTE *values;
		DWORD left;
		if (!storage)
			break;
		if (storage < 4 + 4 + 16 + 4 || storage > size || get_dword(data + 4) != SIGNATURE)
		{
			status = E_INVALIDARG;
			break;
		}
		memcpy(&key.fmtid, data + 8, 16);
		values = data + 24;
		left = storage - 24;
		while (left >= 4)
		{
			DWORD entry = get_dword(values);
			if (!entry)
				break;
			if (entry < 9 || entry > left)
			{
				status = E_INVALIDARG;
				break;
			}
			key.pid = get_dword(values + 4);
			status = read_value(object, &key, values + 9, entry - 9);
			if (FAILED(status))
				break;
			values += entry;
			left -= entry;
		}
		data += storage;
		size -= storage;
	}
	LeaveCriticalSection(&object->lock);
	return status;
}

static const IPersistSerializedPropStorageVtbl serialized_vtbl = {
	serialized_QueryInterface,
	serialized_AddRef,
	serialized_Release,
	serialized_SetFlags,
	serialized_SetPropertyStorage,
	serialized_GetPropertyStorage,
};

HRESULT ps_store_create(REFIID riid, void **out)
{
	store *object;
	HRESULT status;
	*out = NULL;
	if (!(object = CoTaskMemAlloc(sizeof(*object))))
		return E_OUTOFMEMORY;
	memset(object, 0, sizeof(*object));
	object->cache.lpVtbl = &cache_vtbl;
	object->serialized.lpVtbl = &serialized_vtbl;
	object->refs = 1;
	InitializeCriticalSection(&object->lock);
	InterlockedIncrement(&ps_locks);
	status = cache_QueryInterface(&object->cache, riid, out);
	cache_Release(&object->cache);
	return status;
}

HRESULT WINAPI PSCreateMemoryPropertyStore(REFIID riid, void **out)
{
	return ps_store_create(riid, out);
}

/* The plain unknown handed out for an object's store: a separate identity that answers for the object behind it. */
typedef struct
{
	IUnknown unknown;
	LONG refs;
	IUnknown *inner;
} wrapper;

static HRESULT WINAPI wrapper_QueryInterface(IUnknown *self, REFIID riid, void **out)
{
	wrapper *object = CONTAINING_RECORD(self, wrapper, unknown);
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown))
	{
		*out = self;
		InterlockedIncrement(&object->refs);
		return S_OK;
	}
	return IUnknown_QueryInterface(object->inner, riid, out);
}

static ULONG WINAPI wrapper_AddRef(IUnknown *self)
{
	return InterlockedIncrement(&CONTAINING_RECORD(self, wrapper, unknown)->refs);
}

static ULONG WINAPI wrapper_Release(IUnknown *self)
{
	wrapper *object = CONTAINING_RECORD(self, wrapper, unknown);
	LONG left = InterlockedDecrement(&object->refs);
	if (!left)
	{
		IUnknown_Release(object->inner);
		CoTaskMemFree(object);
		InterlockedDecrement(&ps_locks);
	}
	return left;
}

static const IUnknownVtbl wrapper_vtbl = {wrapper_QueryInterface, wrapper_AddRef, wrapper_Release};

HRESULT WINAPI PSCreatePropertyStoreFromObject(IUnknown *source, DWORD mode, REFIID riid, void **out)
{
	IPropertyStore *found;
	wrapper *object;
	HRESULT status;
	if (!source || !out)
		return E_POINTER;
	*out = NULL;
	status = IUnknown_QueryInterface(source, &IID_IPropertyStore, (void **)&found);
	if (FAILED(status))
		return status;
	if (!IsEqualGUID(riid, &IID_IUnknown))
	{
		status = IPropertyStore_QueryInterface(found, riid, out);
		IPropertyStore_Release(found);
		return status;
	}
	if (!(object = CoTaskMemAlloc(sizeof(*object))))
	{
		IPropertyStore_Release(found);
		return E_OUTOFMEMORY;
	}
	object->unknown.lpVtbl = &wrapper_vtbl;
	object->refs = 1;
	object->inner = (IUnknown *)found;
	InterlockedIncrement(&ps_locks);
	*out = &object->unknown;
	return S_OK;
}
