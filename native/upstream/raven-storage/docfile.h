/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#ifndef RAVEN_DOCFILE_H
#define RAVEN_DOCFILE_H

#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <objidl.h>

/* The compound file of a storage is held as a tree of elements, each carrying its whole sector
 * chain as an array. The file's allocation tables are only read to fill those arrays and are
 * written anew from them at every flush, so nothing has to be freed or relinked while the file is
 * being changed and an element can go back to an earlier state by restoring its array. */

enum element_kind {
	KIND_STORAGE = 1,
	KIND_STREAM = 2,
	KIND_ROOT = 5,
};

typedef struct element {
	WCHAR name[32];
	BYTE kind;
	BOOL in_mini;
	BOOL chain_ready;
	CLSID clsid;
	DWORD state_bits;
	FILETIME created;
	FILETIME modified;
	DWORD start;
	ULONGLONG size;
	DWORD *chain;
	DWORD chain_count;
	DWORD chain_capacity;
	struct element *parent;
	struct element *first_child;
	struct element *next;
	DWORD index;
} element;

struct snapshot_link {
	element *tree;
	struct snapshot_link *next;
};

typedef struct volume {
	LONG refs;
	CRITICAL_SECTION lock;
	ILockBytes *bytes;
	BOOL writable;
	BOOL dirty;
	DWORD sector_shift;
	DWORD sector_size;
	DWORD major;
	element *root;
	/* The tables as last read or written. */
	DWORD *fat;
	DWORD fat_count;
	DWORD *mini_fat;
	DWORD mini_count;
	/* Where free sectors are looked for next, and the first sector past the allocated ones. */
	DWORD fat_cursor;
	DWORD mini_cursor;
	DWORD end_sector;
	DWORD mini_end;
	/* Open transactions: a sector stamped before the current epoch may belong to one of their
	 * snapshots and is copied instead of overwritten. */
	DWORD shadows;
	DWORD epoch;
	DWORD *stamps;
	DWORD stamp_count;
	DWORD *mini_stamps;
	DWORD mini_stamp_count;
	struct snapshot_link *snapshots;
} volume;

void *mem_alloc(SIZE_T size);
void *mem_realloc(void *block, SIZE_T size);
void mem_free(void *block);

HRESULT volume_create(ILockBytes *bytes, DWORD sector_size, volume **result);
HRESULT volume_load(ILockBytes *bytes, BOOL writable, volume **result);
void volume_add_ref(volume *vol);
void volume_release(volume *vol);
HRESULT volume_flush(volume *vol);
BOOL looks_like_docfile(const BYTE *header, ULONG count);

element *element_new(const WCHAR *name, BYTE kind);
void element_free_tree(element *item);
element *element_find(element *storage, const WCHAR *name);
void element_insert(element *storage, element *child);
void element_remove(element *child);
int element_compare_names(const WCHAR *left, const WCHAR *right);
element *element_clone(volume *vol, element *item, HRESULT *status);
void element_restore(element *item, element *saved);

HRESULT element_read(volume *vol, element *item, ULONGLONG offset, void *buffer, ULONG count, ULONG *done);
HRESULT element_write(volume *vol, element *item, ULONGLONG offset, const void *buffer, ULONG count, ULONG *done);
HRESULT element_resize(volume *vol, element *item, ULONGLONG size);

void snapshot_register(volume *vol, element *tree);
void snapshot_forget(volume *vol, element *tree);

HRESULT file_bytes_open(const WCHAR *path, DWORD mode, BOOL create, ILockBytes **result);
HRESULT property_set_create(IStorage *storage, IPropertySetStorage **result);
HRESULT storage_open_root(volume *vol, const WCHAR *path, DWORD mode, BOOL delete_on_release, IStorage **result);

#endif
