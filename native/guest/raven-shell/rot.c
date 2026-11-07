/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <objidl.h>

/* The running object table of this process and the item moniker that names entries in it. Nothing outside the
 * process looks into a table here, so both stay plain objects with no marshalling. */

typedef struct rot_entry
{
	struct rot_entry *next;
	DWORD cookie;
	IUnknown *object;
	IMoniker *moniker;
	FILETIME changed;
} rot_entry;

static rot_entry *entries;
static DWORD next_cookie = 1;
static SRWLOCK entries_lock = SRWLOCK_INIT;

static BOOL same_iid(REFIID left, REFIID right)
{
	return !memcmp(left, right, sizeof(IID));
}

/* --- The enumerator: a copy of the monikers at the time it was asked for. --- */

typedef struct
{
	IEnumMoniker iface;
	LONG refs;
	IMoniker **items;
	ULONG count, at;
} moniker_list;

static moniker_list *list_of(IEnumMoniker *iface)
{
	return CONTAINING_RECORD(iface, moniker_list, iface);
}

static HRESULT WINAPI list_QueryInterface(IEnumMoniker *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!same_iid(riid, &IID_IUnknown) && !same_iid(riid, &IID_IEnumMoniker))
		return E_NOINTERFACE;
	*out = iface;
	IEnumMoniker_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI list_AddRef(IEnumMoniker *iface)
{
	return InterlockedIncrement(&list_of(iface)->refs);
}

static ULONG WINAPI list_Release(IEnumMoniker *iface)
{
	moniker_list *self = list_of(iface);
	ULONG left = InterlockedDecrement(&self->refs), index;
	if (left)
		return left;
	for (index = 0; index < self->count; index++)
		IMoniker_Release(self->items[index]);
	HeapFree(GetProcessHeap(), 0, self->items);
	HeapFree(GetProcessHeap(), 0, self);
	return 0;
}

static HRESULT WINAPI list_Next(IEnumMoniker *iface, ULONG wanted, IMoniker **out, ULONG *fetched)
{
	moniker_list *self = list_of(iface);
	ULONG done = 0;
	if (!out)
		return E_POINTER;
	while (done < wanted && self->at < self->count)
	{
		out[done] = self->items[self->at++];
		IMoniker_AddRef(out[done++]);
	}
	if (fetched)
		*fetched = done;
	return done == wanted ? S_OK : S_FALSE;
}

static HRESULT WINAPI list_Skip(IEnumMoniker *iface, ULONG count)
{
	moniker_list *self = list_of(iface);
	if (count > self->count - self->at)
	{
		self->at = self->count;
		return S_FALSE;
	}
	self->at += count;
	return S_OK;
}

static HRESULT WINAPI list_Reset(IEnumMoniker *iface)
{
	list_of(iface)->at = 0;
	return S_OK;
}

static HRESULT make_list(IMoniker **items, ULONG count, ULONG at, IEnumMoniker **out);

static HRESULT WINAPI list_Clone(IEnumMoniker *iface, IEnumMoniker **out)
{
	moniker_list *self = list_of(iface);
	return make_list(self->items, self->count, self->at, out);
}

static const IEnumMonikerVtbl list_vtbl = {list_QueryInterface, list_AddRef, list_Release, list_Next, list_Skip, list_Reset, list_Clone};

static HRESULT make_list(IMoniker **items, ULONG count, ULONG at, IEnumMoniker **out)
{
	moniker_list *self = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *self);
	ULONG index;
	if (!self)
		return E_OUTOFMEMORY;
	self->items = HeapAlloc(GetProcessHeap(), 0, (count + 1) * sizeof(IMoniker *));
	if (!self->items)
	{
		HeapFree(GetProcessHeap(), 0, self);
		return E_OUTOFMEMORY;
	}
	for (index = 0; index < count; index++)
	{
		self->items[index] = items[index];
		IMoniker_AddRef(items[index]);
	}
	self->iface.lpVtbl = &list_vtbl;
	self->refs = 1;
	self->count = count;
	self->at = at;
	*out = &self->iface;
	return S_OK;
}

/* --- The table. --- */

static HRESULT WINAPI table_QueryInterface(IRunningObjectTable *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!same_iid(riid, &IID_IUnknown) && !same_iid(riid, &IID_IRunningObjectTable))
		return E_NOINTERFACE;
	*out = iface;
	return S_OK;
}

static ULONG WINAPI table_AddRef(IRunningObjectTable *iface)
{
	return 2;
}

static ULONG WINAPI table_Release(IRunningObjectTable *iface)
{
	return 1;
}

static rot_entry *find_entry(IMoniker *moniker)
{
	rot_entry *entry;
	for (entry = entries; entry; entry = entry->next)
		if (IMoniker_IsEqual(entry->moniker, moniker) == S_OK)
			return entry;
	return NULL;
}

static HRESULT WINAPI table_Register(IRunningObjectTable *iface, DWORD flags, IUnknown *object, IMoniker *moniker, DWORD *cookie)
{
	rot_entry *entry;
	BOOL taken;
	if (!object || !moniker || !cookie)
		return E_INVALIDARG;
	entry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *entry);
	if (!entry)
		return E_OUTOFMEMORY;
	IUnknown_AddRef(object);
	IMoniker_AddRef(moniker);
	entry->object = object;
	entry->moniker = moniker;
	GetSystemTimeAsFileTime(&entry->changed);
	AcquireSRWLockExclusive(&entries_lock);
	taken = find_entry(moniker) != NULL;
	entry->cookie = next_cookie++;
	entry->next = entries;
	entries = entry;
	*cookie = entry->cookie;
	ReleaseSRWLockExclusive(&entries_lock);
	return taken ? MK_S_MONIKERALREADYREGISTERED : S_OK;
}

static HRESULT WINAPI table_Revoke(IRunningObjectTable *iface, DWORD cookie)
{
	rot_entry **link, *found = NULL;
	AcquireSRWLockExclusive(&entries_lock);
	for (link = &entries; *link; link = &(*link)->next)
		if ((*link)->cookie == cookie)
		{
			found = *link;
			*link = found->next;
			break;
		}
	ReleaseSRWLockExclusive(&entries_lock);
	if (!found)
		return E_INVALIDARG;
	IMoniker_Release(found->moniker);
	IUnknown_Release(found->object);
	HeapFree(GetProcessHeap(), 0, found);
	return S_OK;
}

static HRESULT WINAPI table_IsRunning(IRunningObjectTable *iface, IMoniker *moniker)
{
	HRESULT status;
	if (!moniker)
		return E_INVALIDARG;
	AcquireSRWLockShared(&entries_lock);
	status = find_entry(moniker) ? S_OK : S_FALSE;
	ReleaseSRWLockShared(&entries_lock);
	return status;
}

static HRESULT WINAPI table_GetObject(IRunningObjectTable *iface, IMoniker *moniker, IUnknown **out)
{
	rot_entry *entry;
	if (!moniker || !out)
		return E_INVALIDARG;
	*out = NULL;
	AcquireSRWLockShared(&entries_lock);
	entry = find_entry(moniker);
	if (entry)
	{
		*out = entry->object;
		IUnknown_AddRef(*out);
	}
	ReleaseSRWLockShared(&entries_lock);
	return entry ? S_OK : MK_E_UNAVAILABLE;
}

static HRESULT WINAPI table_NoteChangeTime(IRunningObjectTable *iface, DWORD cookie, FILETIME *when)
{
	rot_entry *entry;
	HRESULT status = E_INVALIDARG;
	if (!when)
		return E_INVALIDARG;
	AcquireSRWLockExclusive(&entries_lock);
	for (entry = entries; entry; entry = entry->next)
		if (entry->cookie == cookie)
		{
			entry->changed = *when;
			status = S_OK;
			break;
		}
	ReleaseSRWLockExclusive(&entries_lock);
	return status;
}

static HRESULT WINAPI table_GetTimeOfLastChange(IRunningObjectTable *iface, IMoniker *moniker, FILETIME *when)
{
	rot_entry *entry;
	if (!moniker || !when)
		return E_INVALIDARG;
	AcquireSRWLockShared(&entries_lock);
	entry = find_entry(moniker);
	if (entry)
		*when = entry->changed;
	ReleaseSRWLockShared(&entries_lock);
	return entry ? S_OK : MK_E_UNAVAILABLE;
}

static HRESULT WINAPI table_EnumRunning(IRunningObjectTable *iface, IEnumMoniker **out)
{
	rot_entry *entry;
	IMoniker **items;
	ULONG count = 0, index = 0;
	HRESULT status;
	if (!out)
		return E_INVALIDARG;
	AcquireSRWLockShared(&entries_lock);
	for (entry = entries; entry; entry = entry->next)
		count++;
	items = HeapAlloc(GetProcessHeap(), 0, (count + 1) * sizeof(IMoniker *));
	if (items)
		for (entry = entries; entry; entry = entry->next)
			items[index++] = entry->moniker;
	status = items ? make_list(items, count, 0, out) : E_OUTOFMEMORY;
	ReleaseSRWLockShared(&entries_lock);
	HeapFree(GetProcessHeap(), 0, items);
	return status;
}

static const IRunningObjectTableVtbl table_vtbl = {table_QueryInterface, table_AddRef, table_Release, table_Register, table_Revoke,
	table_IsRunning, table_GetObject, table_NoteChangeTime, table_GetTimeOfLastChange, table_EnumRunning};
static IRunningObjectTable table = {&table_vtbl};

HRESULT WINAPI GetRunningObjectTable(DWORD reserved, IRunningObjectTable **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (reserved)
		return E_UNEXPECTED;
	*out = &table;
	return S_OK;
}

/* --- The item moniker: a delimiter and a name, equal to another by both, regardless of case. --- */

typedef struct
{
	IMoniker iface;
	LONG refs;
	WCHAR *delimiter, *item;
} item_moniker;

/* {00000304-0000-0000-C000-000000000046}, CLSID_ItemMoniker. */
static const CLSID item_class = {0x00000304, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

static item_moniker *item_of(IMoniker *iface)
{
	return CONTAINING_RECORD(iface, item_moniker, iface);
}

static WCHAR *copy_text(const WCHAR *text)
{
	SIZE_T size = (lstrlenW(text) + 1) * sizeof(WCHAR);
	WCHAR *copy = HeapAlloc(GetProcessHeap(), 0, size);
	if (copy)
		memcpy(copy, text, size);
	return copy;
}

static HRESULT WINAPI item_QueryInterface(IMoniker *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!same_iid(riid, &IID_IUnknown) && !same_iid(riid, &IID_IPersist) && !same_iid(riid, &IID_IPersistStream) && !same_iid(riid, &IID_IMoniker))
		return E_NOINTERFACE;
	*out = iface;
	IMoniker_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI item_AddRef(IMoniker *iface)
{
	return InterlockedIncrement(&item_of(iface)->refs);
}

static ULONG WINAPI item_Release(IMoniker *iface)
{
	item_moniker *self = item_of(iface);
	ULONG left = InterlockedDecrement(&self->refs);
	if (left)
		return left;
	HeapFree(GetProcessHeap(), 0, self->delimiter);
	HeapFree(GetProcessHeap(), 0, self->item);
	HeapFree(GetProcessHeap(), 0, self);
	return 0;
}

static HRESULT WINAPI item_GetClassID(IMoniker *iface, CLSID *out)
{
	if (!out)
		return E_POINTER;
	*out = item_class;
	return S_OK;
}

static HRESULT WINAPI item_IsDirty(IMoniker *iface)
{
	return S_FALSE;
}

static HRESULT WINAPI item_Load(IMoniker *iface, IStream *stream)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI item_Save(IMoniker *iface, IStream *stream, BOOL clear)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI item_GetSizeMax(IMoniker *iface, ULARGE_INTEGER *out)
{
	return E_NOTIMPL;
}

/* Binding an item goes through the container to its left, and no moniker here composes one. */
static HRESULT WINAPI item_BindToObject(IMoniker *iface, IBindCtx *context, IMoniker *left, REFIID riid, void **out)
{
	if (out)
		*out = NULL;
	return left ? E_NOTIMPL : E_INVALIDARG;
}

static HRESULT WINAPI item_BindToStorage(IMoniker *iface, IBindCtx *context, IMoniker *left, REFIID riid, void **out)
{
	if (out)
		*out = NULL;
	return left ? E_NOTIMPL : E_INVALIDARG;
}

static HRESULT WINAPI item_Reduce(IMoniker *iface, IBindCtx *context, DWORD how, IMoniker **left, IMoniker **out)
{
	if (!out)
		return E_POINTER;
	*out = iface;
	IMoniker_AddRef(iface);
	return MK_S_REDUCED_TO_SELF;
}

static HRESULT WINAPI item_ComposeWith(IMoniker *iface, IMoniker *right, BOOL only_if_not_generic, IMoniker **out)
{
	if (out)
		*out = NULL;
	return only_if_not_generic ? MK_E_NEEDGENERIC : E_NOTIMPL;
}

static HRESULT WINAPI item_Enum(IMoniker *iface, BOOL forward, IEnumMoniker **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return S_OK;
}

static HRESULT WINAPI item_IsEqual(IMoniker *iface, IMoniker *other)
{
	item_moniker *self = item_of(iface), *given;
	CLSID kind;
	if (!other)
		return E_INVALIDARG;
	if (FAILED(IMoniker_GetClassID(other, &kind)) || !same_iid(&kind, &item_class))
		return S_FALSE;
	given = item_of(other);
	return !lstrcmpiW(self->delimiter, given->delimiter) && !lstrcmpiW(self->item, given->item) ? S_OK : S_FALSE;
}

static HRESULT WINAPI item_Hash(IMoniker *iface, DWORD *out)
{
	const WCHAR *at;
	DWORD hash = 0;
	if (!out)
		return E_POINTER;
	for (at = item_of(iface)->item; *at; at++)
		hash = hash * 31 + (DWORD)(ULONG_PTR)CharUpperW((WCHAR *)(ULONG_PTR)*at);
	*out = hash;
	return S_OK;
}

static HRESULT WINAPI item_IsRunning(IMoniker *iface, IBindCtx *context, IMoniker *left, IMoniker *newly)
{
	return table_IsRunning(&table, iface);
}

static HRESULT WINAPI item_GetTimeOfLastChange(IMoniker *iface, IBindCtx *context, IMoniker *left, FILETIME *out)
{
	return table_GetTimeOfLastChange(&table, iface, out);
}

static HRESULT WINAPI item_Inverse(IMoniker *iface, IMoniker **out)
{
	if (out)
		*out = NULL;
	return MK_E_NOINVERSE;
}

static HRESULT WINAPI item_CommonPrefixWith(IMoniker *iface, IMoniker *other, IMoniker **out)
{
	if (out)
		*out = NULL;
	return MK_E_NOPREFIX;
}

static HRESULT WINAPI item_RelativePathTo(IMoniker *iface, IMoniker *other, IMoniker **out)
{
	if (out)
		*out = NULL;
	return MK_E_NOTBINDABLE;
}

static HRESULT WINAPI item_GetDisplayName(IMoniker *iface, IBindCtx *context, IMoniker *left, LPOLESTR *out)
{
	item_moniker *self = item_of(iface);
	SIZE_T length;
	if (!out)
		return E_POINTER;
	length = lstrlenW(self->delimiter) + lstrlenW(self->item) + 1;
	*out = CoTaskMemAlloc(length * sizeof(WCHAR));
	if (!*out)
		return E_OUTOFMEMORY;
	lstrcpyW(*out, self->delimiter);
	lstrcatW(*out, self->item);
	return S_OK;
}

static HRESULT WINAPI item_ParseDisplayName(IMoniker *iface, IBindCtx *context, IMoniker *left, LPOLESTR name, ULONG *eaten, IMoniker **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI item_IsSystemMoniker(IMoniker *iface, DWORD *out)
{
	if (!out)
		return E_POINTER;
	*out = MKSYS_ITEMMONIKER;
	return S_OK;
}

static const IMonikerVtbl item_vtbl = {item_QueryInterface, item_AddRef, item_Release, item_GetClassID, item_IsDirty, item_Load, item_Save,
	item_GetSizeMax, item_BindToObject, item_BindToStorage, item_Reduce, item_ComposeWith, item_Enum, item_IsEqual, item_Hash, item_IsRunning,
	item_GetTimeOfLastChange, item_Inverse, item_CommonPrefixWith, item_RelativePathTo, item_GetDisplayName, item_ParseDisplayName,
	item_IsSystemMoniker};

HRESULT WINAPI CreateItemMoniker(LPCOLESTR delimiter, LPCOLESTR item, IMoniker **out);

/* A file moniker names its path the way an item moniker names its item. */
HRESULT WINAPI CreateFileMoniker(LPCOLESTR path, IMoniker **out)
{
	return CreateItemMoniker(L"", path, out);
}

HRESULT WINAPI CreateItemMoniker(LPCOLESTR delimiter, LPCOLESTR item, IMoniker **out)
{
	item_moniker *self;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!item)
		return MK_E_SYNTAX;
	self = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *self);
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &item_vtbl;
	self->refs = 1;
	self->delimiter = copy_text(delimiter ? delimiter : L"");
	self->item = copy_text(item);
	if (!self->delimiter || !self->item)
	{
		item_Release(&self->iface);
		return E_OUTOFMEMORY;
	}
	*out = &self->iface;
	return S_OK;
}

/* --- The global interface table: interfaces a thread leaves for the other threads of the process, under a cookie. Every call here
 * reaches its object directly, whatever its apartment, so an entry is the object itself rather than a marshalled reference. --- */

typedef struct git_entry
{
	struct git_entry *next;
	DWORD cookie;
	IUnknown *object;
} git_entry;

static git_entry *git_entries;
static DWORD git_next_cookie = 0x100;
static SRWLOCK git_lock = SRWLOCK_INIT;

static const IID git_iid = {0x00000146, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static HRESULT WINAPI git_QueryInterface(IGlobalInterfaceTable *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_iid(iid, &IID_IUnknown) || same_iid(iid, &git_iid))
	{
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

/* The table lives as long as the process. */
static ULONG WINAPI git_AddRef(IGlobalInterfaceTable *iface)
{
	return 2;
}

static ULONG WINAPI git_Release(IGlobalInterfaceTable *iface)
{
	return 1;
}

static HRESULT WINAPI git_RegisterInterfaceInGlobal(IGlobalInterfaceTable *iface, IUnknown *object, REFIID iid, DWORD *cookie)
{
	git_entry *entry;
	IUnknown *held;
	HRESULT result;
	if (!cookie)
		return E_INVALIDARG;
	*cookie = 0;
	if (!object || !iid)
		return E_INVALIDARG;
	result = IUnknown_QueryInterface(object, iid, (void **)&held);
	if (FAILED(result))
		return result;
	entry = HeapAlloc(GetProcessHeap(), 0, sizeof *entry);
	if (!entry)
	{
		IUnknown_Release(held);
		return E_OUTOFMEMORY;
	}
	entry->object = held;
	AcquireSRWLockExclusive(&git_lock);
	entry->cookie = git_next_cookie++;
	entry->next = git_entries;
	git_entries = entry;
	ReleaseSRWLockExclusive(&git_lock);
	*cookie = entry->cookie;
	return S_OK;
}

static HRESULT WINAPI git_RevokeInterfaceFromGlobal(IGlobalInterfaceTable *iface, DWORD cookie)
{
	git_entry **link, *entry = NULL;
	AcquireSRWLockExclusive(&git_lock);
	for (link = &git_entries; *link; link = &(*link)->next)
		if ((*link)->cookie == cookie)
		{
			entry = *link;
			*link = entry->next;
			break;
		}
	ReleaseSRWLockExclusive(&git_lock);
	if (!entry)
		return E_INVALIDARG;
	IUnknown_Release(entry->object);
	HeapFree(GetProcessHeap(), 0, entry);
	return S_OK;
}

static HRESULT WINAPI git_GetInterfaceFromGlobal(IGlobalInterfaceTable *iface, DWORD cookie, REFIID iid, void **out)
{
	git_entry *entry;
	HRESULT result = E_INVALIDARG;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	AcquireSRWLockShared(&git_lock);
	for (entry = git_entries; entry; entry = entry->next)
		if (entry->cookie == cookie)
		{
			result = IUnknown_QueryInterface(entry->object, iid, out);
			break;
		}
	ReleaseSRWLockShared(&git_lock);
	return result;
}

static IGlobalInterfaceTableVtbl git_vtbl = {git_QueryInterface, git_AddRef, git_Release, git_RegisterInterfaceInGlobal,
	git_RevokeInterfaceFromGlobal, git_GetInterfaceFromGlobal};
static IGlobalInterfaceTable global_table = {&git_vtbl};

HRESULT raven_make_global_interface_table(REFIID iid, void **out)
{
	return git_QueryInterface(&global_table, iid, out);
}
