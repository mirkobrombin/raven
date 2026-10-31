/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "docfile.h"
#include <string.h>

#define ACCESS_BITS 0x3
#define SHARE_BITS 0x70
#define KNOWN_BITS (ACCESS_BITS | SHARE_BITS | STGM_CREATE | STGM_CONVERT | STGM_TRANSACTED | STGM_PRIORITY | STGM_NOSCRATCH | \
	STGM_NOSNAPSHOT | STGM_DIRECT_SWMR | STGM_DELETEONRELEASE | STGM_SIMPLE)
#define STATE_WRITABLE(mode) (((mode) & ACCESS_BITS) != STGM_READ)

/* What an open storage or stream is: a handle in the tree of what is open, which lets a parent
 * invalidate everything below it when it goes away or changes under them. */
typedef struct handle {
	struct handle *parent;
	struct handle *children;
	struct handle *sibling;
	element *item;
	volume *vol;
	BOOL reverted;
	BOOL is_stream;
} handle;

typedef struct storage {
	IStorage IStorage_iface;
	LONG refs;
	handle base;
	DWORD mode;
	BOOL is_root;
	element *saved;
	WCHAR *path;
	BOOL delete_on_release;
} storage;

typedef struct stream {
	IStream IStream_iface;
	LONG refs;
	handle base;
	DWORD mode;
	ULONGLONG position;
} stream;

typedef struct element_enum {
	IEnumSTATSTG IEnumSTATSTG_iface;
	LONG refs;
	storage *owner;
	WCHAR after[32];
} element_enum;

static const IStorageVtbl storage_vtable;
static const IStreamVtbl stream_vtable;
static const IEnumSTATSTGVtbl enum_vtable;

static storage *storage_of(IStorage *iface)
{
	return CONTAINING_RECORD(iface, storage, IStorage_iface);
}

static stream *stream_of(IStream *iface)
{
	return CONTAINING_RECORD(iface, stream, IStream_iface);
}

static element_enum *enum_of(IEnumSTATSTG *iface)
{
	return CONTAINING_RECORD(iface, element_enum, IEnumSTATSTG_iface);
}

static void handle_attach(handle *child, handle *parent)
{
	child->parent = parent;
	child->sibling = parent->children;
	parent->children = child;
}

static void handle_detach(handle *child)
{
	handle **link;

	if (!child->parent)
		return;
	link = &child->parent->children;
	while (*link != child)
		link = &(*link)->sibling;
	*link = child->sibling;
	child->parent = NULL;
	child->sibling = NULL;
}

static void drop_saved(storage *object)
{
	if (!object->saved)
		return;
	snapshot_forget(object->base.vol, object->saved);
	element_free_tree(object->saved);
	object->saved = NULL;
}

static void invalidate_children(handle *owner);

static void invalidate(handle *item)
{
	invalidate_children(item);
	if (!item->is_stream) {
		storage *object = CONTAINING_RECORD(item, storage, base);
		if (object->saved) {
			element_restore(item->item, object->saved);
			drop_saved(object);
		}
	}
	item->reverted = TRUE;
	item->item = NULL;
	volume_release(item->vol);
	item->vol = NULL;
}

static void invalidate_children(handle *owner)
{
	while (owner->children) {
		handle *child = owner->children;
		handle_detach(child);
		invalidate(child);
	}
}

static void invalidate_opened(handle *owner, const element *target)
{
	handle *child = owner->children, *next;

	for (; child; child = next) {
		next = child->sibling;
		if (child->item == target) {
			handle_detach(child);
			invalidate(child);
		}
	}
}

static BOOL is_opened(const handle *owner, const element *target)
{
	const handle *child;

	for (child = owner->children; child; child = child->sibling) {
		if (child->item == target)
			return TRUE;
	}
	return FALSE;
}

static BOOL valid_name(const WCHAR *name)
{
	const WCHAR *at;

	if (!name || !name[0] || lstrlenW(name) >= 32)
		return FALSE;
	for (at = name; *at; at++) {
		if (*at == '/' || *at == '\\' || *at == ':' || *at == '!')
			return FALSE;
	}
	return TRUE;
}

static HRESULT child_mode_status(DWORD mode, BOOL is_stream)
{
	if ((mode & ~KNOWN_BITS) || (mode & ACCESS_BITS) == ACCESS_BITS)
		return STG_E_INVALIDFLAG;
	if (mode & STGM_DELETEONRELEASE)
		return STG_E_INVALIDFUNCTION;
	if (is_stream && (mode & STGM_TRANSACTED))
		return STG_E_INVALIDFUNCTION;
	if ((mode & SHARE_BITS) != STGM_SHARE_EXCLUSIVE)
		return STG_E_INVALIDFLAG;
	if (mode & STGM_PRIORITY)
		return STG_E_INVALIDFUNCTION;
	return S_OK;
}

static void fill_stat(const element *item, DWORD mode, STATSTG *stat, DWORD flag, const WCHAR *name)
{
	memset(stat, 0, sizeof(*stat));
	if (!(flag & STATFLAG_NONAME) && name) {
		SIZE_T bytes = (lstrlenW(name) + 1) * sizeof(WCHAR);
		stat->pwcsName = CoTaskMemAlloc(bytes);
		if (stat->pwcsName)
			memcpy(stat->pwcsName, name, bytes);
	}
	stat->type = item->kind == KIND_STREAM ? STGTY_STREAM : STGTY_STORAGE;
	stat->cbSize.QuadPart = item->kind == KIND_STREAM ? item->size : 0;
	stat->mtime = item->modified;
	stat->ctime = item->created;
	stat->grfMode = mode;
	stat->clsid = item->clsid;
	stat->grfStateBits = item->state_bits;
}

/* Streams. */

static HRESULT WINAPI stream_QueryInterface(IStream *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ISequentialStream) || IsEqualIID(riid, &IID_IStream)) {
		*out = iface;
		IStream_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI stream_AddRef(IStream *iface)
{
	return InterlockedIncrement(&stream_of(iface)->refs);
}

static ULONG WINAPI stream_Release(IStream *iface)
{
	stream *object = stream_of(iface);
	ULONG refs = InterlockedDecrement(&object->refs);

	if (refs)
		return refs;
	if (object->base.vol) {
		volume *vol = object->base.vol;
		EnterCriticalSection(&vol->lock);
		handle_detach(&object->base);
		LeaveCriticalSection(&vol->lock);
		volume_release(vol);
	}
	mem_free(object);
	return 0;
}

static HRESULT WINAPI stream_Read(IStream *iface, void *buffer, ULONG count, ULONG *read)
{
	stream *object = stream_of(iface);
	volume *vol = object->base.vol;
	HRESULT status;
	ULONG done = 0;

	if (object->base.reverted)
		return STG_E_REVERTED;
	if (!buffer)
		return STG_E_INVALIDPOINTER;
	EnterCriticalSection(&vol->lock);
	status = element_read(vol, object->base.item, object->position, buffer, count, &done);
	object->position += done;
	LeaveCriticalSection(&vol->lock);
	if (read)
		*read = done;
	return status;
}

static HRESULT WINAPI stream_Write(IStream *iface, const void *buffer, ULONG count, ULONG *written)
{
	stream *object = stream_of(iface);
	volume *vol = object->base.vol;
	HRESULT status;
	ULONG done = 0;

	if (written)
		*written = 0;
	if (object->base.reverted)
		return STG_E_REVERTED;
	if (!STATE_WRITABLE(object->mode))
		return STG_E_ACCESSDENIED;
	if (!buffer)
		return STG_E_INVALIDPOINTER;
	EnterCriticalSection(&vol->lock);
	status = element_write(vol, object->base.item, object->position, buffer, count, &done);
	object->position += done;
	LeaveCriticalSection(&vol->lock);
	if (written)
		*written = done;
	return status;
}

static HRESULT WINAPI stream_Seek(IStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *position)
{
	stream *object = stream_of(iface);
	LONGLONG base;

	if (position)
		position->QuadPart = 0;
	if (object->base.reverted)
		return STG_E_REVERTED;
	switch (origin) {
	case STREAM_SEEK_SET:
		base = 0;
		break;
	case STREAM_SEEK_CUR:
		base = (LONGLONG)object->position;
		break;
	case STREAM_SEEK_END:
		base = (LONGLONG)object->base.item->size;
		break;
	default:
		return STG_E_INVALIDFUNCTION;
	}
	if (base + move.QuadPart < 0)
		return STG_E_INVALIDFUNCTION;
	object->position = (ULONGLONG)(base + move.QuadPart);
	if (position)
		position->QuadPart = object->position;
	return S_OK;
}

static HRESULT WINAPI stream_SetSize(IStream *iface, ULARGE_INTEGER size)
{
	stream *object = stream_of(iface);
	volume *vol = object->base.vol;
	HRESULT status;

	if (object->base.reverted)
		return STG_E_REVERTED;
	if (!STATE_WRITABLE(object->mode))
		return STG_E_ACCESSDENIED;
	EnterCriticalSection(&vol->lock);
	status = element_resize(vol, object->base.item, size.QuadPart);
	LeaveCriticalSection(&vol->lock);
	return status;
}

static HRESULT WINAPI stream_CopyTo(IStream *iface, IStream *target, ULARGE_INTEGER count, ULARGE_INTEGER *read, ULARGE_INTEGER *written)
{
	stream *object = stream_of(iface);
	ULONGLONG left = count.QuadPart, total_read = 0, total_written = 0;
	BYTE *block;
	HRESULT status = S_OK;

	if (object->base.reverted)
		return STG_E_REVERTED;
	if (!target)
		return STG_E_INVALIDPOINTER;
	block = mem_alloc(65536);
	if (!block)
		return E_OUTOFMEMORY;
	while (left) {
		ULONG got = 0, put = 0;
		status = IStream_Read(iface, block, left > 65536 ? 65536 : (ULONG)left, &got);
		if (FAILED(status) || !got)
			break;
		total_read += got;
		status = IStream_Write(target, block, got, &put);
		total_written += put;
		if (FAILED(status) || put != got)
			break;
		left -= got;
	}
	mem_free(block);
	if (read)
		read->QuadPart = total_read;
	if (written)
		written->QuadPart = total_written;
	return status;
}

static HRESULT WINAPI stream_Commit(IStream *iface, DWORD flags)
{
	return stream_of(iface)->base.reverted ? STG_E_REVERTED : S_OK;
}

static HRESULT WINAPI stream_Revert(IStream *iface)
{
	return stream_of(iface)->base.reverted ? STG_E_REVERTED : S_OK;
}

static HRESULT WINAPI stream_LockRegion(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER count, DWORD type)
{
	return STG_E_INVALIDFUNCTION;
}

static HRESULT WINAPI stream_UnlockRegion(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER count, DWORD type)
{
	return STG_E_INVALIDFUNCTION;
}

static HRESULT WINAPI stream_Stat(IStream *iface, STATSTG *stat, DWORD flag)
{
	stream *object = stream_of(iface);

	if (object->base.reverted)
		return STG_E_REVERTED;
	if (!stat)
		return STG_E_INVALIDPOINTER;
	EnterCriticalSection(&object->base.vol->lock);
	fill_stat(object->base.item, object->mode, stat, flag, object->base.item->name);
	LeaveCriticalSection(&object->base.vol->lock);
	return S_OK;
}

static stream *stream_new(handle *parent, element *item, DWORD mode)
{
	stream *object = mem_alloc(sizeof(*object));

	if (!object)
		return NULL;
	object->IStream_iface.lpVtbl = &stream_vtable;
	object->refs = 1;
	object->base.item = item;
	object->base.vol = parent->vol;
	object->base.is_stream = TRUE;
	object->mode = mode;
	volume_add_ref(parent->vol);
	handle_attach(&object->base, parent);
	return object;
}

static HRESULT WINAPI stream_Clone(IStream *iface, IStream **out)
{
	stream *object = stream_of(iface), *clone;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (object->base.reverted)
		return STG_E_REVERTED;
	EnterCriticalSection(&object->base.vol->lock);
	clone = stream_new(object->base.parent, object->base.item, object->mode);
	LeaveCriticalSection(&object->base.vol->lock);
	if (!clone)
		return E_OUTOFMEMORY;
	clone->position = object->position;
	*out = &clone->IStream_iface;
	return S_OK;
}

static const IStreamVtbl stream_vtable = {
	stream_QueryInterface, stream_AddRef, stream_Release, stream_Read, stream_Write, stream_Seek, stream_SetSize,
	stream_CopyTo, stream_Commit, stream_Revert, stream_LockRegion, stream_UnlockRegion, stream_Stat, stream_Clone,
};

/* Element enumeration. It remembers the last name it returned, so elements may come and go while
 * it runs. */

static HRESULT enum_new(storage *owner, const WCHAR *after, IEnumSTATSTG **out)
{
	element_enum *object = mem_alloc(sizeof(*object));

	if (!object)
		return E_OUTOFMEMORY;
	object->IEnumSTATSTG_iface.lpVtbl = &enum_vtable;
	object->refs = 1;
	object->owner = owner;
	if (after)
		lstrcpyW(object->after, after);
	IStorage_AddRef(&owner->IStorage_iface);
	*out = &object->IEnumSTATSTG_iface;
	return S_OK;
}

static HRESULT WINAPI enum_QueryInterface(IEnumSTATSTG *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumSTATSTG)) {
		*out = iface;
		IEnumSTATSTG_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI enum_AddRef(IEnumSTATSTG *iface)
{
	return InterlockedIncrement(&enum_of(iface)->refs);
}

static ULONG WINAPI enum_Release(IEnumSTATSTG *iface)
{
	element_enum *object = enum_of(iface);
	ULONG refs = InterlockedDecrement(&object->refs);

	if (!refs) {
		IStorage_Release(&object->owner->IStorage_iface);
		mem_free(object);
	}
	return refs;
}

static element *enum_following(element_enum *object)
{
	element *child;

	for (child = object->owner->base.item->first_child; child; child = child->next) {
		if (!object->after[0] || element_compare_names(child->name, object->after) > 0)
			return child;
	}
	return NULL;
}

static HRESULT WINAPI enum_Next(IEnumSTATSTG *iface, ULONG count, STATSTG *out, ULONG *fetched)
{
	element_enum *object = enum_of(iface);
	ULONG done = 0;

	if (fetched)
		*fetched = 0;
	if (!out || (count > 1 && !fetched))
		return E_INVALIDARG;
	if (object->owner->base.reverted)
		return STG_E_REVERTED;
	EnterCriticalSection(&object->owner->base.vol->lock);
	while (done < count) {
		element *child = enum_following(object);
		if (!child)
			break;
		fill_stat(child, 0, &out[done++], STATFLAG_DEFAULT, child->name);
		lstrcpyW(object->after, child->name);
	}
	LeaveCriticalSection(&object->owner->base.vol->lock);
	if (fetched)
		*fetched = done;
	return done == count ? S_OK : S_FALSE;
}

static HRESULT WINAPI enum_Skip(IEnumSTATSTG *iface, ULONG count)
{
	element_enum *object = enum_of(iface);
	ULONG done = 0;

	if (object->owner->base.reverted)
		return STG_E_REVERTED;
	EnterCriticalSection(&object->owner->base.vol->lock);
	while (done < count) {
		element *child = enum_following(object);
		if (!child)
			break;
		lstrcpyW(object->after, child->name);
		done++;
	}
	LeaveCriticalSection(&object->owner->base.vol->lock);
	return done == count ? S_OK : S_FALSE;
}

static HRESULT WINAPI enum_Reset(IEnumSTATSTG *iface)
{
	element_enum *object = enum_of(iface);

	if (object->owner->base.reverted)
		return STG_E_REVERTED;
	object->after[0] = 0;
	return S_OK;
}

static HRESULT WINAPI enum_Clone(IEnumSTATSTG *iface, IEnumSTATSTG **out)
{
	element_enum *object = enum_of(iface);

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (object->owner->base.reverted)
		return STG_E_REVERTED;
	return enum_new(object->owner, object->after, out);
}

static const IEnumSTATSTGVtbl enum_vtable = {
	enum_QueryInterface, enum_AddRef, enum_Release, enum_Next, enum_Skip, enum_Reset, enum_Clone,
};

/* Storages. */

static HRESULT start_transaction(storage *object)
{
	HRESULT status = S_OK;

	object->saved = element_clone(object->base.vol, object->base.item, &status);
	if (!object->saved)
		return status;
	snapshot_register(object->base.vol, object->saved);
	return S_OK;
}

static storage *storage_new(volume *vol, element *item, DWORD mode)
{
	storage *object = mem_alloc(sizeof(*object));

	if (!object)
		return NULL;
	object->IStorage_iface.lpVtbl = &storage_vtable;
	object->refs = 1;
	object->base.item = item;
	object->base.vol = vol;
	object->mode = mode;
	volume_add_ref(vol);
	return object;
}

static HRESULT WINAPI storage_QueryInterface(IStorage *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IStorage)) {
		*out = iface;
		IStorage_AddRef(iface);
		return S_OK;
	}
	if (IsEqualIID(riid, &IID_IPropertySetStorage))
		return property_set_create(iface, (IPropertySetStorage **)out);
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI storage_AddRef(IStorage *iface)
{
	return InterlockedIncrement(&storage_of(iface)->refs);
}

static ULONG WINAPI storage_Release(IStorage *iface)
{
	storage *object = storage_of(iface);
	ULONG refs = InterlockedDecrement(&object->refs);

	if (refs)
		return refs;
	if (object->base.vol) {
		volume *vol = object->base.vol;
		EnterCriticalSection(&vol->lock);
		invalidate_children(&object->base);
		handle_detach(&object->base);
		if (object->saved) {
			element_restore(object->base.item, object->saved);
			drop_saved(object);
		}
		if (object->is_root && vol->writable)
			volume_flush(vol);
		LeaveCriticalSection(&vol->lock);
		volume_release(vol);
	}
	if (object->delete_on_release && object->path)
		DeleteFileW(object->path);
	mem_free(object->path);
	mem_free(object);
	return 0;
}

static HRESULT WINAPI storage_CreateStream(IStorage *iface, const WCHAR *name, DWORD mode, DWORD reserved1, DWORD reserved2, IStream **out)
{
	storage *object = storage_of(iface);
	element *existing;
	stream *created;
	volume *vol = object->base.vol;
	HRESULT status;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (reserved1 || reserved2)
		return STG_E_INVALIDPARAMETER;
	if (!valid_name(name))
		return STG_E_INVALIDNAME;
	status = child_mode_status(mode, TRUE);
	if (FAILED(status))
		return status;
	if (object->base.reverted)
		return STG_E_REVERTED;
	if ((object->mode & STGM_SIMPLE) && (mode & STGM_CREATE))
		return STG_E_INVALIDFLAG;
	if (!STATE_WRITABLE(mode) && !STATE_WRITABLE(object->mode))
		return STG_E_ACCESSDENIED;
	if (!STATE_WRITABLE(object->mode))
		return STG_E_ACCESSDENIED;
	EnterCriticalSection(&vol->lock);
	existing = element_find(object->base.item, name);
	if (existing) {
		if (!(mode & STGM_CREATE)) {
			LeaveCriticalSection(&vol->lock);
			return STG_E_FILEALREADYEXISTS;
		}
		invalidate_opened(&object->base, existing);
		element_remove(existing);
		element_free_tree(existing);
	}
	existing = element_new(name, KIND_STREAM);
	created = existing ? stream_new(&object->base, existing, mode & ~STGM_CREATE) : NULL;
	if (created) {
		element_insert(object->base.item, existing);
		vol->dirty = TRUE;
		GetSystemTimeAsFileTime(&object->base.item->modified);
	} else {
		element_free_tree(existing);
		status = E_OUTOFMEMORY;
	}
	LeaveCriticalSection(&vol->lock);
	if (created)
		*out = &created->IStream_iface;
	return status;
}

static HRESULT WINAPI storage_OpenStream(IStorage *iface, const WCHAR *name, void *reserved1, DWORD mode, DWORD reserved2, IStream **out)
{
	storage *object = storage_of(iface);
	volume *vol = object->base.vol;
	HRESULT status;
	element *found;
	stream *opened = NULL;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (reserved1 || reserved2)
		return STG_E_INVALIDPARAMETER;
	if (!name)
		return STG_E_INVALIDNAME;
	status = child_mode_status(mode, TRUE);
	if (FAILED(status))
		return status;
	if (object->base.reverted)
		return STG_E_REVERTED;
	if (STATE_WRITABLE(mode) && !STATE_WRITABLE(object->mode))
		return STG_E_ACCESSDENIED;
	EnterCriticalSection(&vol->lock);
	found = lstrlenW(name) < 32 ? element_find(object->base.item, name) : NULL;
	if (!found || found->kind != KIND_STREAM) {
		status = STG_E_FILENOTFOUND;
	} else if (is_opened(&object->base, found)) {
		status = STG_E_ACCESSDENIED;
	} else {
		opened = stream_new(&object->base, found, mode);
		if (!opened)
			status = E_OUTOFMEMORY;
	}
	LeaveCriticalSection(&vol->lock);
	if (opened)
		*out = &opened->IStream_iface;
	return status;
}

static HRESULT child_storage(storage *object, element *item, DWORD mode, IStorage **out)
{
	storage *child = storage_new(object->base.vol, item, mode);
	HRESULT status = S_OK;

	if (!child)
		return E_OUTOFMEMORY;
	handle_attach(&child->base, &object->base);
	if (mode & STGM_TRANSACTED) {
		status = start_transaction(child);
		if (FAILED(status)) {
			IStorage_Release(&child->IStorage_iface);
			return status;
		}
	}
	*out = &child->IStorage_iface;
	return S_OK;
}

static HRESULT WINAPI storage_CreateStorage(IStorage *iface, const WCHAR *name, DWORD mode, DWORD reserved1, DWORD reserved2, IStorage **out)
{
	storage *object = storage_of(iface);
	volume *vol = object->base.vol;
	element *existing;
	HRESULT status;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (reserved1 || reserved2)
		return STG_E_INVALIDPARAMETER;
	if (!valid_name(name))
		return STG_E_INVALIDNAME;
	status = child_mode_status(mode, FALSE);
	if (FAILED(status))
		return status;
	if (object->base.reverted)
		return STG_E_REVERTED;
	if (object->mode & STGM_SIMPLE)
		return STG_E_INVALIDFUNCTION;
	EnterCriticalSection(&vol->lock);
	existing = element_find(object->base.item, name);
	if (!STATE_WRITABLE(object->mode)) {
		status = existing && existing->kind == KIND_STREAM ? STG_E_FILEALREADYEXISTS : STG_E_ACCESSDENIED;
	} else if (existing && !(mode & STGM_CREATE)) {
		status = STG_E_FILEALREADYEXISTS;
	} else {
		if (existing) {
			invalidate_opened(&object->base, existing);
			element_remove(existing);
			element_free_tree(existing);
		}
		existing = element_new(name, KIND_STORAGE);
		if (!existing) {
			status = E_OUTOFMEMORY;
		} else {
			element_insert(object->base.item, existing);
			vol->dirty = TRUE;
			GetSystemTimeAsFileTime(&object->base.item->modified);
			status = child_storage(object, existing, mode & ~STGM_CREATE, out);
		}
	}
	LeaveCriticalSection(&vol->lock);
	return status;
}

static HRESULT WINAPI storage_OpenStorage(IStorage *iface, const WCHAR *name, IStorage *priority, DWORD mode, SNB exclude, DWORD reserved, IStorage **out)
{
	storage *object = storage_of(iface);
	volume *vol = object->base.vol;
	element *found;
	HRESULT status;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (!name)
		return STG_E_INVALIDNAME;
	if (priority || exclude || reserved)
		return STG_E_INVALIDPARAMETER;
	status = child_mode_status(mode, FALSE);
	if (FAILED(status))
		return status;
	if (object->base.reverted)
		return STG_E_REVERTED;
	if (STATE_WRITABLE(mode) && !STATE_WRITABLE(object->mode))
		return STG_E_ACCESSDENIED;
	EnterCriticalSection(&vol->lock);
	found = lstrlenW(name) < 32 ? element_find(object->base.item, name) : NULL;
	if (!found || found->kind == KIND_STREAM)
		status = STG_E_FILENOTFOUND;
	else if (is_opened(&object->base, found))
		status = STG_E_ACCESSDENIED;
	else
		status = child_storage(object, found, mode, out);
	LeaveCriticalSection(&vol->lock);
	return status;
}

static BOOL in_exclusions(const WCHAR *name, SNB exclude)
{
	if (!exclude)
		return FALSE;
	for (; *exclude; exclude++) {
		if (!element_compare_names(*exclude, name))
			return TRUE;
	}
	return FALSE;
}

/* The target may be any storage, so the copy goes through the interfaces. */
static HRESULT copy_into(IStorage *source, IStorage *target, DWORD exclude_count, const IID *exclude_ids, SNB exclude)
{
	BOOL skip_storages = FALSE, skip_streams = FALSE;
	IEnumSTATSTG *names;
	STATSTG stat;
	HRESULT status;
	DWORD at;

	for (at = 0; at < exclude_count; at++) {
		if (IsEqualIID(&exclude_ids[at], &IID_IStorage))
			skip_storages = TRUE;
		else if (IsEqualIID(&exclude_ids[at], &IID_IStream))
			skip_streams = TRUE;
	}
	status = IStorage_Stat(source, &stat, STATFLAG_NONAME);
	if (FAILED(status))
		return status;
	status = IStorage_SetClass(target, &stat.clsid);
	if (FAILED(status))
		return status;
	status = IStorage_EnumElements(source, 0, NULL, 0, &names);
	if (FAILED(status))
		return status;
	while (IEnumSTATSTG_Next(names, 1, &stat, NULL) == S_OK && SUCCEEDED(status)) {
		if (in_exclusions(stat.pwcsName, exclude)) {
			/* Skipped by name. */
		} else if (stat.type == STGTY_STORAGE && !skip_storages) {
			IStorage *from, *to;
			status = IStorage_OpenStorage(source, stat.pwcsName, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, NULL, 0, &from);
			if (SUCCEEDED(status)) {
				status = IStorage_CreateStorage(target, stat.pwcsName, STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &to);
				if (status == STG_E_FILEALREADYEXISTS)
					status = IStorage_OpenStorage(target, stat.pwcsName, NULL, STGM_WRITE | STGM_SHARE_EXCLUSIVE, NULL, 0, &to);
				if (SUCCEEDED(status)) {
					status = copy_into(from, to, exclude_count, exclude_ids, NULL);
					IStorage_Release(to);
				}
				IStorage_Release(from);
			}
		} else if (stat.type == STGTY_STREAM && !skip_streams) {
			IStream *from, *to;
			status = IStorage_OpenStream(source, stat.pwcsName, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &from);
			if (SUCCEEDED(status)) {
				status = IStorage_CreateStream(target, stat.pwcsName, STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &to);
				if (SUCCEEDED(status)) {
					ULARGE_INTEGER all;
					all.QuadPart = ~0ull;
					status = IStream_CopyTo(from, to, all, NULL, NULL);
					IStream_Release(to);
				}
				IStream_Release(from);
			}
		}
		CoTaskMemFree(stat.pwcsName);
	}
	IEnumSTATSTG_Release(names);
	return status;
}

static BOOL is_below(const element *item, const element *ancestor)
{
	for (; item; item = item->parent) {
		if (item == ancestor)
			return TRUE;
	}
	return FALSE;
}

static HRESULT WINAPI storage_CopyTo(IStorage *iface, DWORD exclude_count, const IID *exclude_ids, SNB exclude, IStorage *target)
{
	storage *object = storage_of(iface);

	if (!target)
		return STG_E_INVALIDPOINTER;
	if (object->base.reverted)
		return STG_E_REVERTED;
	if (target->lpVtbl == &storage_vtable) {
		storage *other = storage_of(target);
		if (other->base.reverted)
			return STG_E_REVERTED;
		if (other->base.vol == object->base.vol && is_below(other->base.item, object->base.item))
			return STG_E_ACCESSDENIED;
	}
	return copy_into(iface, target, exclude_count, exclude_ids, exclude);
}

static HRESULT WINAPI storage_MoveElementTo(IStorage *iface, const WCHAR *name, IStorage *target, const WCHAR *new_name, DWORD flags)
{
	storage *object = storage_of(iface);
	element *found;
	HRESULT status;
	BOOL is_stream;

	if (!name || !new_name)
		return STG_E_INVALIDNAME;
	if (!target)
		return STG_E_INVALIDPOINTER;
	if (flags != STGMOVE_MOVE && flags != STGMOVE_COPY)
		return STG_E_INVALIDFUNCTION;
	if (object->base.reverted)
		return STG_E_REVERTED;
	if (!STATE_WRITABLE(object->mode) && flags == STGMOVE_MOVE)
		return STG_E_ACCESSDENIED;
	EnterCriticalSection(&object->base.vol->lock);
	found = lstrlenW(name) < 32 ? element_find(object->base.item, name) : NULL;
	is_stream = found && found->kind == KIND_STREAM;
	LeaveCriticalSection(&object->base.vol->lock);
	if (!found)
		return STG_E_FILENOTFOUND;
	if (is_stream) {
		IStream *from, *to;
		status = IStorage_OpenStream(iface, name, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &from);
		if (FAILED(status))
			return status;
		status = IStorage_CreateStream(target, new_name, STGM_FAILIFTHERE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &to);
		if (SUCCEEDED(status)) {
			ULARGE_INTEGER all;
			all.QuadPart = ~0ull;
			status = IStream_CopyTo(from, to, all, NULL, NULL);
			IStream_Release(to);
		}
		IStream_Release(from);
	} else {
		IStorage *from, *to;
		status = IStorage_OpenStorage(iface, name, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, NULL, 0, &from);
		if (FAILED(status))
			return status;
		status = IStorage_CreateStorage(target, new_name, STGM_FAILIFTHERE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &to);
		if (SUCCEEDED(status)) {
			status = copy_into(from, to, 0, NULL, NULL);
			IStorage_Release(to);
		}
		IStorage_Release(from);
	}
	if (SUCCEEDED(status) && flags == STGMOVE_MOVE)
		status = IStorage_DestroyElement(iface, name);
	return status;
}

static HRESULT WINAPI storage_Commit(IStorage *iface, DWORD flags)
{
	storage *object = storage_of(iface);
	volume *vol = object->base.vol;
	HRESULT status = S_OK;

	if (object->base.reverted)
		return STG_E_REVERTED;
	if (!STATE_WRITABLE(object->mode))
		return object->saved ? STG_E_ACCESSDENIED : S_OK;
	EnterCriticalSection(&vol->lock);
	if (object->saved)
		drop_saved(object);
	if (object->is_root)
		status = volume_flush(vol);
	if (SUCCEEDED(status) && (object->mode & STGM_TRANSACTED))
		status = start_transaction(object);
	if (!object->is_root)
		vol->dirty = TRUE;
	LeaveCriticalSection(&vol->lock);
	return status;
}

static HRESULT WINAPI storage_Revert(IStorage *iface)
{
	storage *object = storage_of(iface);
	volume *vol = object->base.vol;

	if (object->base.reverted)
		return STG_E_REVERTED;
	if (!object->saved)
		return S_OK;
	EnterCriticalSection(&vol->lock);
	invalidate_children(&object->base);
	element_restore(object->base.item, object->saved);
	vol->epoch++;
	LeaveCriticalSection(&vol->lock);
	return S_OK;
}

static HRESULT WINAPI storage_EnumElements(IStorage *iface, DWORD reserved1, void *reserved2, DWORD reserved3, IEnumSTATSTG **out)
{
	storage *object = storage_of(iface);

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (object->base.reverted)
		return STG_E_REVERTED;
	return enum_new(object, NULL, out);
}

static HRESULT WINAPI storage_DestroyElement(IStorage *iface, const WCHAR *name)
{
	storage *object = storage_of(iface);
	volume *vol = object->base.vol;
	element *found;
	HRESULT status = S_OK;

	if (!name)
		return STG_E_INVALIDPOINTER;
	if (object->base.reverted)
		return STG_E_REVERTED;
	if (!STATE_WRITABLE(object->mode))
		return STG_E_ACCESSDENIED;
	EnterCriticalSection(&vol->lock);
	found = lstrlenW(name) < 32 ? element_find(object->base.item, name) : NULL;
	if (found) {
		invalidate_opened(&object->base, found);
		element_remove(found);
		element_free_tree(found);
		vol->dirty = TRUE;
		GetSystemTimeAsFileTime(&object->base.item->modified);
	} else {
		status = STG_E_FILENOTFOUND;
	}
	LeaveCriticalSection(&vol->lock);
	return status;
}

static HRESULT WINAPI storage_RenameElement(IStorage *iface, const WCHAR *name, const WCHAR *new_name)
{
	storage *object = storage_of(iface);
	volume *vol = object->base.vol;
	element *found;
	HRESULT status = S_OK;

	if (!name || !valid_name(new_name))
		return STG_E_INVALIDNAME;
	if (object->base.reverted)
		return STG_E_REVERTED;
	if (!STATE_WRITABLE(object->mode))
		return STG_E_ACCESSDENIED;
	EnterCriticalSection(&vol->lock);
	found = lstrlenW(name) < 32 ? element_find(object->base.item, name) : NULL;
	if (!found) {
		status = STG_E_FILENOTFOUND;
	} else if (element_find(object->base.item, new_name)) {
		status = STG_E_FILEALREADYEXISTS;
	} else if (is_opened(&object->base, found)) {
		status = STG_E_ACCESSDENIED;
	} else {
		element_remove(found);
		lstrcpyW(found->name, new_name);
		element_insert(object->base.item, found);
		vol->dirty = TRUE;
	}
	LeaveCriticalSection(&vol->lock);
	return status;
}

static HRESULT WINAPI storage_SetElementTimes(IStorage *iface, const WCHAR *name, const FILETIME *created, const FILETIME *accessed, const FILETIME *modified)
{
	storage *object = storage_of(iface);
	volume *vol = object->base.vol;
	element *found;
	HRESULT status = S_OK;

	if (object->base.reverted)
		return STG_E_REVERTED;
	if (!STATE_WRITABLE(object->mode))
		return STG_E_ACCESSDENIED;
	EnterCriticalSection(&vol->lock);
	found = name ? element_find(object->base.item, name) : object->base.item;
	if (found) {
		if (created)
			found->created = *created;
		if (modified)
			found->modified = *modified;
		vol->dirty = TRUE;
	} else {
		status = STG_E_FILENOTFOUND;
	}
	LeaveCriticalSection(&vol->lock);
	return status;
}

static HRESULT WINAPI storage_SetClass(IStorage *iface, REFCLSID clsid)
{
	storage *object = storage_of(iface);

	if (object->base.reverted)
		return STG_E_REVERTED;
	if (!STATE_WRITABLE(object->mode))
		return STG_E_ACCESSDENIED;
	EnterCriticalSection(&object->base.vol->lock);
	object->base.item->clsid = *clsid;
	object->base.vol->dirty = TRUE;
	LeaveCriticalSection(&object->base.vol->lock);
	return S_OK;
}

static HRESULT WINAPI storage_SetStateBits(IStorage *iface, DWORD bits, DWORD mask)
{
	storage *object = storage_of(iface);

	if (object->base.reverted)
		return STG_E_REVERTED;
	if (!STATE_WRITABLE(object->mode))
		return STG_E_ACCESSDENIED;
	EnterCriticalSection(&object->base.vol->lock);
	object->base.item->state_bits = (object->base.item->state_bits & ~mask) | (bits & mask);
	object->base.vol->dirty = TRUE;
	LeaveCriticalSection(&object->base.vol->lock);
	return S_OK;
}

static HRESULT WINAPI storage_Stat(IStorage *iface, STATSTG *stat, DWORD flag)
{
	storage *object = storage_of(iface);

	if (!stat)
		return STG_E_INVALIDPOINTER;
	if (object->base.reverted)
		return STG_E_REVERTED;
	if (flag & ~(STATFLAG_NONAME | STATFLAG_NOOPEN))
		return STG_E_INVALIDFLAG;
	EnterCriticalSection(&object->base.vol->lock);
	fill_stat(object->base.item, object->mode, stat, flag, object->is_root ? object->path : object->base.item->name);
	LeaveCriticalSection(&object->base.vol->lock);
	return S_OK;
}

static const IStorageVtbl storage_vtable = {
	storage_QueryInterface, storage_AddRef, storage_Release, storage_CreateStream, storage_OpenStream, storage_CreateStorage,
	storage_OpenStorage, storage_CopyTo, storage_MoveElementTo, storage_Commit, storage_Revert, storage_EnumElements,
	storage_DestroyElement, storage_RenameElement, storage_SetElementTimes, storage_SetClass, storage_SetStateBits, storage_Stat,
};

HRESULT storage_open_root(volume *vol, const WCHAR *path, DWORD mode, BOOL delete_on_release, IStorage **result)
{
	storage *object = storage_new(vol, vol->root, mode);
	HRESULT status = S_OK;

	if (!object)
		return E_OUTOFMEMORY;
	object->is_root = TRUE;
	object->delete_on_release = delete_on_release;
	if (path) {
		SIZE_T bytes = (lstrlenW(path) + 1) * sizeof(WCHAR);
		object->path = mem_alloc(bytes);
		if (object->path)
			memcpy(object->path, path, bytes);
	}
	if (mode & STGM_TRANSACTED)
		status = start_transaction(object);
	if (FAILED(status)) {
		IStorage_Release(&object->IStorage_iface);
		return status;
	}
	*result = &object->IStorage_iface;
	return S_OK;
}

/* The entry points. */

static HRESULT create_mode_status(DWORD mode)
{
	if ((mode & ~KNOWN_BITS) || (mode & ACCESS_BITS) == ACCESS_BITS || (mode & SHARE_BITS) > STGM_SHARE_DENY_NONE)
		return STG_E_INVALIDFLAG;
	if ((mode & ACCESS_BITS) == STGM_READ || (mode & STGM_PRIORITY) || ((mode & STGM_CREATE) && (mode & STGM_CONVERT)))
		return STG_E_INVALIDFLAG;
	if (!(mode & STGM_TRANSACTED) && !(mode & STGM_DIRECT_SWMR) && (mode & SHARE_BITS) != STGM_SHARE_EXCLUSIVE)
		return STG_E_INVALIDFLAG;
	return S_OK;
}

static HRESULT open_mode_status(DWORD mode)
{
	if ((mode & ~KNOWN_BITS) || (mode & ACCESS_BITS) == ACCESS_BITS || (mode & SHARE_BITS) > STGM_SHARE_DENY_NONE)
		return STG_E_INVALIDFLAG;
	if (mode & STGM_PRIORITY) {
		if (mode & STGM_DELETEONRELEASE)
			return STG_E_INVALIDFUNCTION;
		return mode & ~STGM_PRIORITY ? STG_E_INVALIDFLAG : S_OK;
	}
	if (mode & (STGM_CREATE | STGM_CONVERT))
		return STG_E_INVALIDFLAG;
	if (mode & STGM_DELETEONRELEASE)
		return STG_E_INVALIDFUNCTION;
	if (mode & (STGM_TRANSACTED | STGM_DIRECT_SWMR))
		return S_OK;
	if ((mode & SHARE_BITS) == STGM_SHARE_EXCLUSIVE)
		return S_OK;
	return (mode & ACCESS_BITS) == STGM_READ && (mode & SHARE_BITS) == STGM_SHARE_DENY_WRITE ? S_OK : STG_E_INVALIDFLAG;
}

static HRESULT temp_path(WCHAR *path)
{
	WCHAR folder[MAX_PATH];

	if (!GetTempPathW(MAX_PATH, folder) || !GetTempFileNameW(folder, L"STO", 0, path))
		return STG_E_INSUFFICIENTMEMORY;
	return S_OK;
}

static HRESULT create_on_file(const WCHAR *name, DWORD mode, DWORD sector_size, IStorage **out)
{
	WCHAR path[MAX_PATH * 2], full[MAX_PATH * 2];
	BOOL temporary = !name;
	ILockBytes *bytes;
	volume *vol;
	HRESULT status;

	if (temporary) {
		status = temp_path(path);
		if (FAILED(status))
			return status;
		name = path;
		mode |= STGM_CREATE;
	}
	if (!GetFullPathNameW(name, ARRAYSIZE(full), full, NULL))
		lstrcpynW(full, name, ARRAYSIZE(full));
	status = file_bytes_open(full, mode, TRUE, &bytes);
	if (FAILED(status))
		return status;
	status = volume_create(bytes, sector_size, &vol);
	ILockBytes_Release(bytes);
	if (SUCCEEDED(status)) {
		status = storage_open_root(vol, full, mode & ~(STGM_CREATE | STGM_CONVERT), temporary || (mode & STGM_DELETEONRELEASE), out);
		volume_release(vol);
	}
	if (FAILED(status))
		DeleteFileW(full);
	return status;
}

HRESULT WINAPI StgCreateDocfile(const WCHAR *name, DWORD mode, DWORD reserved, IStorage **out)
{
	HRESULT status;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (reserved)
		return STG_E_INVALIDPARAMETER;
	status = create_mode_status(mode);
	if (FAILED(status))
		return status;
	return create_on_file(name, mode, 512, out);
}

static HRESULT cast_result(IStorage *storage, REFIID riid, void **out)
{
	HRESULT status = IStorage_QueryInterface(storage, riid, out);

	IStorage_Release(storage);
	return status;
}

HRESULT WINAPI StgCreateStorageEx(const WCHAR *name, DWORD mode, DWORD format, DWORD attributes, STGOPTIONS *options, PSECURITY_DESCRIPTOR security, REFIID riid, void **out)
{
	DWORD sector_size = 512;
	IStorage *created;
	HRESULT status;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (format == STGFMT_FILE || format == STGFMT_ANY || format == STGFMT_NATIVE)
		return STG_E_INVALIDPARAMETER;
	if (format != STGFMT_DOCFILE && format != STGFMT_STORAGE)
		return STG_E_INVALIDPARAMETER;
	if (security)
		return STG_E_INVALIDPARAMETER;
	if (options) {
		if (options->ulSectorSize && options->ulSectorSize != 512 && options->ulSectorSize != 4096)
			return STG_E_INVALIDPARAMETER;
		if (options->ulSectorSize)
			sector_size = options->ulSectorSize;
	}
	status = create_mode_status(mode);
	if (FAILED(status))
		return status;
	status = create_on_file(name, mode, sector_size, &created);
	if (FAILED(status))
		return status;
	return cast_result(created, riid, out);
}

static HRESULT open_on_file(const WCHAR *name, DWORD mode, IStorage **out)
{
	WCHAR full[MAX_PATH * 2];
	ILockBytes *bytes;
	volume *vol;
	HRESULT status;

	if (!GetFullPathNameW(name, ARRAYSIZE(full), full, NULL))
		lstrcpynW(full, name, ARRAYSIZE(full));
	status = file_bytes_open(full, mode, FALSE, &bytes);
	if (FAILED(status))
		return status;
	status = volume_load(bytes, STATE_WRITABLE(mode), &vol);
	ILockBytes_Release(bytes);
	if (FAILED(status))
		return status;
	status = storage_open_root(vol, full, mode, FALSE, out);
	volume_release(vol);
	return status;
}

HRESULT WINAPI StgOpenStorage(const WCHAR *name, IStorage *priority, DWORD mode, SNB exclude, DWORD reserved, IStorage **out)
{
	HRESULT status;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (reserved)
		return STG_E_INVALIDPARAMETER;
	status = open_mode_status(mode);
	if (FAILED(status))
		return status;
	if (!name && !priority)
		return STG_E_INVALIDNAME;
	if (priority) {
		STATSTG stat;
		status = IStorage_Stat(priority, &stat, STATFLAG_DEFAULT);
		IStorage_Release(priority);
		if (FAILED(status))
			return status;
		if (!name)
			name = stat.pwcsName;
		status = open_on_file(name, mode, out);
		CoTaskMemFree(stat.pwcsName);
	} else {
		status = open_on_file(name, mode, out);
	}
	if (SUCCEEDED(status) && exclude) {
		/* The named elements of an opened storage read as empty; they are simply destroyed in a
		 * transaction nobody is going to commit unless the caller wants that. */
		for (; *exclude; exclude++)
			IStorage_DestroyElement(*out, *exclude);
	}
	return status;
}

HRESULT WINAPI StgOpenStorageEx(const WCHAR *name, DWORD mode, DWORD format, DWORD attributes, STGOPTIONS *options, void *reserved, REFIID riid, void **out)
{
	IStorage *opened;
	HRESULT status;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (reserved)
		return STG_E_INVALIDPARAMETER;
	if (format == STGFMT_FILE)
		return STG_E_INVALIDPARAMETER;
	if (format != STGFMT_DOCFILE && format != STGFMT_STORAGE && format != STGFMT_ANY && format != STGFMT_NATIVE)
		return STG_E_INVALIDPARAMETER;
	if (!name)
		return STG_E_INVALIDNAME;
	status = open_mode_status(mode);
	if (FAILED(status))
		return status;
	status = open_on_file(name, mode, &opened);
	if (FAILED(status))
		return status;
	return cast_result(opened, riid, out);
}

HRESULT WINAPI StgCreateDocfileOnILockBytes(ILockBytes *bytes, DWORD mode, DWORD reserved, IStorage **out)
{
	volume *vol;
	HRESULT status;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (!bytes)
		return STG_E_INVALIDPOINTER;
	if (reserved)
		return STG_E_INVALIDPARAMETER;
	status = create_mode_status(mode);
	if (FAILED(status))
		return status;
	if (StgIsStorageILockBytes(bytes) == S_OK && !(mode & STGM_CREATE))
		return STG_E_FILEALREADYEXISTS;
	status = volume_create(bytes, 512, &vol);
	if (FAILED(status))
		return status;
	status = storage_open_root(vol, NULL, mode & ~(STGM_CREATE | STGM_CONVERT), FALSE, out);
	volume_release(vol);
	return status;
}

HRESULT WINAPI StgOpenStorageOnILockBytes(ILockBytes *bytes, IStorage *priority, DWORD mode, SNB exclude, DWORD reserved, IStorage **out)
{
	volume *vol;
	HRESULT status;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (!bytes)
		return STG_E_INVALIDPOINTER;
	if (priority || exclude || reserved)
		return STG_E_INVALIDPARAMETER;
	status = open_mode_status(mode);
	if (FAILED(status))
		return status;
	status = volume_load(bytes, STATE_WRITABLE(mode), &vol);
	if (status == STG_E_FILEALREADYEXISTS)
		status = STG_E_FILEALREADYEXISTS;
	if (FAILED(status))
		return status;
	status = storage_open_root(vol, NULL, mode, FALSE, out);
	volume_release(vol);
	return status;
}

HRESULT WINAPI StgIsStorageILockBytes(ILockBytes *bytes)
{
	BYTE header[8];
	ULARGE_INTEGER at;
	ULONG got = 0;
	HRESULT status;

	if (!bytes)
		return STG_E_INVALIDPOINTER;
	at.QuadPart = 0;
	status = ILockBytes_ReadAt(bytes, at, header, sizeof(header), &got);
	if (FAILED(status))
		return status;
	return looks_like_docfile(header, got) ? S_OK : S_FALSE;
}

HRESULT WINAPI StgIsStorageFile(const WCHAR *name)
{
	BYTE header[8];
	DWORD got = 0;
	HANDLE file;

	if (!name)
		return STG_E_INVALIDNAME;
	file = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return STG_E_FILENOTFOUND;
	if (!ReadFile(file, header, sizeof(header), &got, NULL))
		got = 0;
	CloseHandle(file);
	return looks_like_docfile(header, got) ? S_OK : S_FALSE;
}

HRESULT WINAPI StgSetTimes(const WCHAR *name, const FILETIME *created, const FILETIME *accessed, const FILETIME *modified)
{
	IStorage *opened;
	HRESULT status = StgOpenStorage(name, NULL, STGM_READWRITE | STGM_SHARE_EXCLUSIVE, NULL, 0, &opened);

	if (FAILED(status))
		return status;
	status = IStorage_SetElementTimes(opened, NULL, created, accessed, modified);
	IStorage_Release(opened);
	return status;
}
