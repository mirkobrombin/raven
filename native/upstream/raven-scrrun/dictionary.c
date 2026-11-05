/* SPDX-License-Identifier: MIT */
#include "scrrun.h"

/* Scripting.Dictionary: items by key, in the order they were added. Keys are strings (compared exactly or, in text mode, without
 * regard to case), numbers (by value) or objects (by identity). A hash over the keys finds them; it is built again when items go. */

typedef struct {
	VARIANT key, item;
	ULONG hash;
	LONG next;
} entry;

typedef struct {
	sr_object base;
	entry *entries;
	LONG count, room;
	LONG *buckets;
	LONG bucket_count;
	LONG compare_mode;
} dictionary;

enum {
	DICT_ADD = 1, DICT_COUNT, DICT_EXISTS, DICT_ITEMS, DICT_REMOVE, DICT_REMOVEALL, DICT_KEYS, DICT_KEY, DICT_COMPAREMODE, DICT_HASHVAL,
};

static const sr_member members[] = {
	{ L"Item", DISPID_VALUE, 1 }, { L"Add", DICT_ADD, 2 }, { L"Count", DICT_COUNT, 0 }, { L"Exists", DICT_EXISTS, 1 }, { L"Items", DICT_ITEMS, 0 },
	{ L"Remove", DICT_REMOVE, 1 }, { L"RemoveAll", DICT_REMOVEALL, 0 }, { L"Keys", DICT_KEYS, 0 }, { L"Key", DICT_KEY, 1 },
	{ L"CompareMode", DICT_COMPAREMODE, 0 }, { L"HashVal", DICT_HASHVAL, 1 }, { L"_NewEnum", DISPID_NEWENUM, 0 }, { NULL },
};

static BOOL is_number(VARTYPE type)
{
	switch (type) {
	case VT_I1: case VT_I2: case VT_I4: case VT_I8: case VT_UI1: case VT_UI2: case VT_UI4: case VT_UI8: case VT_INT: case VT_UINT:
	case VT_R4: case VT_R8: case VT_CY: case VT_DECIMAL: case VT_DATE: case VT_BOOL:
		return TRUE;
	}
	return FALSE;
}

static double number_of(const VARIANT *v)
{
	VARIANT d;
	VariantInit(&d);
	return SUCCEEDED(VariantChangeType(&d, (VARIANT *)v, 0, VT_R8)) ? V_R8(&d) : 0;
}

static IUnknown *identity(const VARIANT *v)
{
	IUnknown *unknown = NULL;
	if ((V_VT(v) == VT_DISPATCH || V_VT(v) == VT_UNKNOWN) && V_UNKNOWN(v) &&
		SUCCEEDED(IUnknown_QueryInterface(V_UNKNOWN(v), &IID_IUnknown, (void **)&unknown)))
		IUnknown_Release(unknown);
	return unknown;
}

static WCHAR fold(WCHAR c) { return (WCHAR)(ULONG_PTR)CharLowerW((LPWSTR)(ULONG_PTR)c); }

static ULONG hash_of(const dictionary *d, const VARIANT *key)
{
	ULONG hash = 2166136261u;
	if (V_VT(key) == VT_BSTR) {
		for (const WCHAR *c = V_BSTR(key); c && *c; c++)
			hash = (hash ^ (d->compare_mode ? fold(*c) : *c)) * 16777619u;
	} else if (is_number(V_VT(key))) {
		double value = number_of(key);
		if (value == 0)
			value = 0;
		const BYTE *bytes = (const BYTE *)&value;
		for (UINT i = 0; i < sizeof(value); i++)
			hash = (hash ^ bytes[i]) * 16777619u;
	} else {
		ULONG_PTR pointer = (ULONG_PTR)identity(key);
		hash ^= (ULONG)pointer ^ (ULONG)(pointer >> 16);
	}
	return hash;
}

static BOOL same_key(const dictionary *d, const VARIANT *a, const VARIANT *b)
{
	if (V_VT(a) == VT_BSTR || V_VT(b) == VT_BSTR) {
		if (V_VT(a) != VT_BSTR || V_VT(b) != VT_BSTR)
			return FALSE;
		const WCHAR *x = V_BSTR(a) ? V_BSTR(a) : L"", *y = V_BSTR(b) ? V_BSTR(b) : L"";
		return d->compare_mode ? CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, x, -1, y, -1) == CSTR_EQUAL : !lstrcmpW(x, y);
	}
	if (is_number(V_VT(a)) || is_number(V_VT(b)))
		return is_number(V_VT(a)) && is_number(V_VT(b)) && number_of(a) == number_of(b);
	if (V_VT(a) == VT_EMPTY || V_VT(a) == VT_NULL)
		return V_VT(a) == V_VT(b);
	return identity(a) == identity(b);
}

static void rebuild(dictionary *d)
{
	LONG wanted = 16;
	while (wanted < d->count * 2)
		wanted *= 2;
	if (wanted != d->bucket_count) {
		sr_free(d->buckets);
		d->buckets = sr_alloc(wanted * sizeof(LONG));
		d->bucket_count = wanted;
	}
	for (LONG i = 0; i < d->bucket_count; i++)
		d->buckets[i] = -1;
	for (LONG i = 0; i < d->count; i++) {
		LONG *head = &d->buckets[d->entries[i].hash & (d->bucket_count - 1)];
		d->entries[i].next = *head;
		*head = i;
	}
}

static LONG find(const dictionary *d, const VARIANT *key)
{
	if (!d->bucket_count)
		return -1;
	for (LONG i = d->buckets[hash_of(d, key) & (d->bucket_count - 1)]; i >= 0; i = d->entries[i].next)
		if (same_key(d, &d->entries[i].key, key))
			return i;
	return -1;
}

/* A key is kept as its value: the default member of an object that has one stands for it only when asked for by value. */
static HRESULT key_of(VARIANT *in, VARIANT *out)
{
	VariantInit(out);
	if (!in)
		return DISP_E_PARAMNOTFOUND;
	if (V_VT(in) & VT_ARRAY)
		return DISP_E_TYPEMISMATCH;
	return VariantCopyInd(out, in);
}

static LONG add(dictionary *d, VARIANT *key, VARIANT *item)
{
	if (d->count == d->room) {
		d->room = d->room ? d->room * 2 : 8;
		d->entries = sr_grow(d->entries, d->room * sizeof(entry));
	}
	entry *e = &d->entries[d->count];
	e->key = *key;
	VariantInit(key);
	VariantInit(&e->item);
	if (item)
		VariantCopyInd(&e->item, item);
	e->hash = hash_of(d, &e->key);
	d->count++;
	if (d->count * 2 > d->bucket_count)
		rebuild(d);
	else {
		LONG *head = &d->buckets[e->hash & (d->bucket_count - 1)];
		e->next = *head;
		*head = d->count - 1;
	}
	return d->count - 1;
}

static void remove_at(dictionary *d, LONG index)
{
	VariantClear(&d->entries[index].key);
	VariantClear(&d->entries[index].item);
	memmove(&d->entries[index], &d->entries[index + 1], (d->count - index - 1) * sizeof(entry));
	d->count--;
	rebuild(d);
}

static HRESULT array_of(dictionary *d, BOOL keys, VARIANT *result)
{
	SAFEARRAYBOUND bound = { (ULONG)d->count, 0 };
	SAFEARRAY *array = SafeArrayCreate(VT_VARIANT, 1, &bound);
	if (!array)
		return E_OUTOFMEMORY;
	for (LONG i = 0; i < d->count; i++)
		SafeArrayPutElement(array, &i, keys ? &d->entries[i].key : &d->entries[i].item);
	if (!result) {
		SafeArrayDestroy(array);
		return S_OK;
	}
	V_VT(result) = VT_ARRAY | VT_VARIANT;
	V_ARRAY(result) = array;
	return S_OK;
}

static HRESULT dictionary_invoke(sr_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	dictionary *d = (dictionary *)self;
	BOOL put = sr_is_put(flags);
	VARIANT key;
	HRESULT hr = S_OK;
	LONG at;
	switch (id) {
	case DISPID_VALUE:
		if (FAILED(hr = key_of(sr_arg(params, flags, 0), &key)))
			return hr;
		at = find(d, &key);
		if (put) {
			VARIANT *value = sr_put_value(params);
			if (at < 0)
				add(d, &key, value);
			else {
				VariantClear(&d->entries[at].item);
				hr = VariantCopyInd(&d->entries[at].item, value);
			}
		} else {
			/* Reading an item that is not there adds it, empty. */
			if (at < 0)
				at = add(d, &key, NULL);
			if (result)
				hr = VariantCopy(result, &d->entries[at].item);
		}
		VariantClear(&key);
		return hr;
	case DICT_ADD:
		if (FAILED(hr = key_of(sr_arg(params, flags, 0), &key)))
			return hr;
		if (find(d, &key) >= 0)
			hr = sr_error(SR_E_KEY_EXISTS);
		else
			add(d, &key, sr_arg(params, flags, 1));
		VariantClear(&key);
		return hr;
	case DICT_COUNT:
		return sr_return_long(result, d->count);
	case DICT_EXISTS:
	case DICT_HASHVAL:
		if (FAILED(hr = key_of(sr_arg(params, flags, 0), &key)))
			return hr;
		hr = id == DICT_EXISTS ? sr_return_bool(result, find(d, &key) >= 0) : sr_return_long(result, (LONG)(hash_of(d, &key) % 1201));
		VariantClear(&key);
		return hr;
	case DICT_ITEMS:
	case DICT_KEYS:
		return array_of(d, id == DICT_KEYS, result);
	case DICT_REMOVE:
		if (FAILED(hr = key_of(sr_arg(params, flags, 0), &key)))
			return hr;
		at = find(d, &key);
		VariantClear(&key);
		if (at < 0)
			return sr_error(SR_E_ELEMENT_NOT_FOUND);
		remove_at(d, at);
		return S_OK;
	case DICT_REMOVEALL:
		while (d->count)
			remove_at(d, d->count - 1);
		return S_OK;
	case DICT_KEY: {
		/* Key(old) = new renames an item. */
		if (!put)
			return DISP_E_MEMBERNOTFOUND;
		VARIANT renamed;
		if (FAILED(hr = key_of(sr_arg(params, flags, 0), &key)))
			return hr;
		if (FAILED(hr = key_of(sr_put_value(params), &renamed))) {
			VariantClear(&key);
			return hr;
		}
		at = find(d, &key);
		if (at < 0)
			hr = sr_error(SR_E_ELEMENT_NOT_FOUND);
		else if (find(d, &renamed) >= 0)
			hr = sr_error(SR_E_KEY_EXISTS);
		else {
			VariantClear(&d->entries[at].key);
			d->entries[at].key = renamed;
			VariantInit(&renamed);
			d->entries[at].hash = hash_of(d, &d->entries[at].key);
			rebuild(d);
		}
		VariantClear(&key);
		VariantClear(&renamed);
		return hr;
	}
	case DICT_COMPAREMODE:
		if (!put)
			return sr_return_long(result, d->compare_mode);
		/* The mode changes only while the dictionary is empty. */
		if (d->count)
			return sr_error(SR_E_ARGUMENT);
		return sr_long(sr_put_value(params), &d->compare_mode);
	case DISPID_NEWENUM: {
		VARIANT *keys = sr_alloc((d->count ? d->count : 1) * sizeof(VARIANT));
		for (LONG i = 0; i < d->count; i++)
			keys[i] = d->entries[i].key;
		hr = sr_return_enum(result, keys, (ULONG)d->count);
		sr_free(keys);
		return hr;
	}
	}
	return DISP_E_MEMBERNOTFOUND;
}

static void dictionary_destroy(sr_object *self)
{
	dictionary *d = (dictionary *)self;
	for (LONG i = 0; i < d->count; i++) {
		VariantClear(&d->entries[i].key);
		VariantClear(&d->entries[i].item);
	}
	sr_free(d->entries);
	sr_free(d->buckets);
}

static const sr_class dictionary_class = { members, dictionary_invoke, dictionary_destroy };

IDispatch *sr_dictionary_new(void)
{
	dictionary *d = sr_object_new(&dictionary_class, sizeof(dictionary));
	rebuild(d);
	return sr_dispatch(d);
}
