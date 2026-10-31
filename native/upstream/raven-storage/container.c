/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "docfile.h"
#include <string.h>

/* The on-disk format is the one of the public [MS-CFB] specification. */

#define FREE_SECTOR 0xffffffffu
#define END_OF_CHAIN 0xfffffffeu
#define FAT_SECTOR 0xfffffffdu
#define DIFAT_SECTOR 0xfffffffcu
#define NO_STREAM 0xffffffffu
/* Free on disk, but still holding the data of an open transaction's snapshot. */
#define RESERVED_SECTOR 0xfffffff0u

#define MINI_SIZE 64
#define MINI_CUTOFF 4096
#define HEADER_SIZE 512
#define HEADER_DIFAT 109
#define ENTRY_SIZE 128

static const BYTE signature[8] = { 0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1 };

void *mem_alloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1);
}

void *mem_realloc(void *block, SIZE_T size)
{
	if (!block)
		return mem_alloc(size);
	return HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, block, size ? size : 1);
}

void mem_free(void *block)
{
	if (block)
		HeapFree(GetProcessHeap(), 0, block);
}

static DWORD get32(const BYTE *at)
{
	return at[0] | at[1] << 8 | at[2] << 16 | (DWORD)at[3] << 24;
}

static WORD get16(const BYTE *at)
{
	return (WORD)(at[0] | at[1] << 8);
}

static void put32(BYTE *at, DWORD value)
{
	at[0] = (BYTE)value;
	at[1] = (BYTE)(value >> 8);
	at[2] = (BYTE)(value >> 16);
	at[3] = (BYTE)(value >> 24);
}

static void put16(BYTE *at, WORD value)
{
	at[0] = (BYTE)value;
	at[1] = (BYTE)(value >> 8);
}

BOOL looks_like_docfile(const BYTE *header, ULONG count)
{
	return count >= 8 && !memcmp(header, signature, 8);
}

static ULONGLONG sector_offset(const volume *vol, DWORD sector)
{
	return ((ULONGLONG)sector + 1) << vol->sector_shift;
}

static HRESULT read_at(volume *vol, ULONGLONG offset, void *buffer, ULONG count)
{
	ULARGE_INTEGER at;
	ULONG got = 0;
	HRESULT status;

	at.QuadPart = offset;
	status = ILockBytes_ReadAt(vol->bytes, at, buffer, count, &got);
	if (FAILED(status))
		return status;
	/* Sectors handed out but never written yet lie past the end of the file. */
	if (got < count)
		memset((BYTE *)buffer + got, 0, count - got);
	return S_OK;
}

static HRESULT write_at(volume *vol, ULONGLONG offset, const void *buffer, ULONG count)
{
	ULARGE_INTEGER at;
	ULONG done = 0;
	HRESULT status;

	at.QuadPart = offset;
	status = ILockBytes_WriteAt(vol->bytes, at, buffer, count, &done);
	if (FAILED(status))
		return status;
	return done == count ? S_OK : STG_E_MEDIUMFULL;
}

static WCHAR upper_unit(WCHAR unit)
{
	if (unit < 0x80)
		return unit >= 'a' && unit <= 'z' ? unit - 'a' + 'A' : unit;
	return (WCHAR)(ULONG_PTR)CharUpperW((WCHAR *)(ULONG_PTR)unit);
}

int element_compare_names(const WCHAR *left, const WCHAR *right)
{
	int left_length = lstrlenW(left), right_length = lstrlenW(right), at;

	/* The directory order of [MS-CFB]: shorter names first, then by upper case code unit. */
	if (left_length != right_length)
		return left_length - right_length;
	for (at = 0; at < left_length; at++) {
		WCHAR a = upper_unit(left[at]), b = upper_unit(right[at]);
		if (a != b)
			return a - b;
	}
	return 0;
}

element *element_new(const WCHAR *name, BYTE kind)
{
	element *item = mem_alloc(sizeof(*item));

	if (!item)
		return NULL;
	lstrcpynW(item->name, name, 32);
	item->kind = kind;
	item->start = END_OF_CHAIN;
	item->chain_ready = TRUE;
	GetSystemTimeAsFileTime(&item->created);
	item->modified = item->created;
	if (kind == KIND_STREAM) {
		item->in_mini = TRUE;
		memset(&item->created, 0, sizeof(item->created));
		memset(&item->modified, 0, sizeof(item->modified));
	}
	return item;
}

void element_free_tree(element *item)
{
	element *child, *next;

	if (!item)
		return;
	for (child = item->first_child; child; child = next) {
		next = child->next;
		element_free_tree(child);
	}
	mem_free(item->chain);
	mem_free(item);
}

element *element_find(element *storage, const WCHAR *name)
{
	element *child;

	for (child = storage->first_child; child; child = child->next) {
		if (!element_compare_names(child->name, name))
			return child;
	}
	return NULL;
}

void element_insert(element *storage, element *child)
{
	element **link = &storage->first_child;

	while (*link && element_compare_names((*link)->name, child->name) < 0)
		link = &(*link)->next;
	child->next = *link;
	*link = child;
	child->parent = storage;
}

void element_remove(element *child)
{
	element **link = &child->parent->first_child;

	while (*link != child)
		link = &(*link)->next;
	*link = child->next;
	child->next = NULL;
	child->parent = NULL;
}

/* Fills the element's chain from the tables on first use. */
static HRESULT ready_chain(volume *vol, element *item)
{
	const DWORD *table;
	DWORD limit, unit, sector, count = 0;
	ULONGLONG wanted;

	if (item->chain_ready)
		return S_OK;
	table = item->in_mini ? vol->mini_fat : vol->fat;
	limit = item->in_mini ? vol->mini_count : vol->fat_count;
	unit = item->in_mini ? MINI_SIZE : vol->sector_size;
	wanted = (item->size + unit - 1) / unit;
	sector = item->start;
	if (wanted > limit)
		return STG_E_DOCFILECORRUPT;
	item->chain = mem_alloc((SIZE_T)(wanted ? wanted : 1) * sizeof(DWORD));
	if (!item->chain)
		return E_OUTOFMEMORY;
	item->chain_capacity = (DWORD)(wanted ? wanted : 1);
	while (count < wanted) {
		if (sector >= limit)
			return STG_E_DOCFILECORRUPT;
		item->chain[count++] = sector;
		sector = table[sector];
	}
	item->chain_count = count;
	item->chain_ready = TRUE;
	return S_OK;
}

static HRESULT ensure_stamp(DWORD **stamps, DWORD *count, DWORD index)
{
	DWORD *grown, size;

	if (index < *count)
		return S_OK;
	size = index + 1 > *count * 2 ? index + 1 : *count * 2;
	if (size < 64)
		size = 64;
	grown = mem_realloc(*stamps, (SIZE_T)size * sizeof(DWORD));
	if (!grown)
		return E_OUTOFMEMORY;
	*stamps = grown;
	*count = size;
	return S_OK;
}

static HRESULT allocate_sector(volume *vol, DWORD *result)
{
	static const BYTE zero[4096];
	HRESULT status;
	DWORD sector;
	BOOL reused = FALSE;

	while (vol->fat_cursor < vol->fat_count && vol->fat[vol->fat_cursor] != FREE_SECTOR)
		vol->fat_cursor++;
	if (vol->fat_cursor < vol->fat_count && vol->fat_cursor < vol->end_sector) {
		sector = vol->fat_cursor++;
		reused = TRUE;
	} else {
		sector = vol->end_sector++;
		/* Its slot in the table still reads as free: the search must not hand it out again. */
		vol->fat_cursor = vol->end_sector;
	}
	status = ensure_stamp(&vol->stamps, &vol->stamp_count, sector);
	if (FAILED(status))
		return status;
	vol->stamps[sector] = vol->epoch;
	if (reused) {
		status = write_at(vol, sector_offset(vol, sector), zero, vol->sector_size);
		if (FAILED(status))
			return status;
	}
	*result = sector;
	return S_OK;
}

static HRESULT allocate_mini(volume *vol, DWORD *result)
{
	HRESULT status;
	DWORD sector;

	while (vol->mini_cursor < vol->mini_count && vol->mini_fat[vol->mini_cursor] != FREE_SECTOR)
		vol->mini_cursor++;
	if (vol->mini_cursor < vol->mini_count && vol->mini_cursor < vol->mini_end) {
		sector = vol->mini_cursor++;
	} else {
		sector = vol->mini_end++;
		/* Its slot in the table still reads as free: the search must not hand it out again. */
		vol->mini_cursor = vol->mini_end;
		status = element_resize(vol, vol->root, (ULONGLONG)vol->mini_end * MINI_SIZE);
		if (FAILED(status))
			return status;
	}
	status = ensure_stamp(&vol->mini_stamps, &vol->mini_stamp_count, sector);
	if (FAILED(status))
		return status;
	vol->mini_stamps[sector] = vol->epoch;
	*result = sector;
	return S_OK;
}

static HRESULT set_chain_length(volume *vol, element *item, DWORD count)
{
	HRESULT status;

	if (count > item->chain_capacity) {
		DWORD size = count > item->chain_capacity * 2 ? count : item->chain_capacity * 2;
		DWORD *grown = mem_realloc(item->chain, (SIZE_T)size * sizeof(DWORD));
		if (!grown)
			return E_OUTOFMEMORY;
		item->chain = grown;
		item->chain_capacity = size;
	}
	while (item->chain_count < count) {
		DWORD sector;
		status = item->in_mini ? allocate_mini(vol, &sector) : allocate_sector(vol, &sector);
		if (FAILED(status))
			return status;
		item->chain[item->chain_count++] = sector;
	}
	item->chain_count = count;
	return S_OK;
}

static HRESULT read_regular(volume *vol, element *item, ULONGLONG offset, BYTE *buffer, ULONG count)
{
	while (count) {
		DWORD index = (DWORD)(offset >> vol->sector_shift), inner = (DWORD)offset & (vol->sector_size - 1);
		ULONG run = vol->sector_size - inner;
		HRESULT status;

		/* Consecutive sectors are read at once, which large streams nearly always are. */
		while (run < count && index + 1 < item->chain_count && item->chain[index + 1] == item->chain[index] + 1) {
			run += vol->sector_size;
			index++;
		}
		if (run > count)
			run = count;
		status = read_at(vol, sector_offset(vol, item->chain[(DWORD)(offset >> vol->sector_shift)]) + inner, buffer, run);
		if (FAILED(status))
			return status;
		buffer += run;
		offset += run;
		count -= run;
	}
	return S_OK;
}

static HRESULT copy_sector(volume *vol, element *item, DWORD index)
{
	BYTE *data = mem_alloc(vol->sector_size);
	HRESULT status;
	DWORD fresh;

	if (!data)
		return E_OUTOFMEMORY;
	status = read_at(vol, sector_offset(vol, item->chain[index]), data, vol->sector_size);
	if (SUCCEEDED(status))
		status = allocate_sector(vol, &fresh);
	if (SUCCEEDED(status))
		status = write_at(vol, sector_offset(vol, fresh), data, vol->sector_size);
	if (SUCCEEDED(status))
		item->chain[index] = fresh;
	mem_free(data);
	return status;
}

static BOOL sector_is_shared(const volume *vol, DWORD sector)
{
	return vol->shadows && (sector >= vol->stamp_count || vol->stamps[sector] != vol->epoch);
}

static BOOL mini_is_shared(const volume *vol, DWORD sector)
{
	return vol->shadows && (sector >= vol->mini_stamp_count || vol->mini_stamps[sector] != vol->epoch);
}

static HRESULT write_regular(volume *vol, element *item, ULONGLONG offset, const BYTE *buffer, ULONG count)
{
	while (count) {
		DWORD first = (DWORD)(offset >> vol->sector_shift), index = first, inner = (DWORD)offset & (vol->sector_size - 1);
		ULONG run = vol->sector_size - inner;
		HRESULT status;

		if (sector_is_shared(vol, item->chain[first])) {
			status = copy_sector(vol, item, first);
			if (FAILED(status))
				return status;
		}
		while (run < count && index + 1 < item->chain_count && item->chain[index + 1] == item->chain[index] + 1 &&
				!sector_is_shared(vol, item->chain[index + 1])) {
			run += vol->sector_size;
			index++;
		}
		if (run > count)
			run = count;
		status = write_at(vol, sector_offset(vol, item->chain[first]) + inner, buffer, run);
		if (FAILED(status))
			return status;
		buffer += run;
		offset += run;
		count -= run;
	}
	return S_OK;
}

static HRESULT read_mini(volume *vol, element *item, ULONGLONG offset, BYTE *buffer, ULONG count)
{
	HRESULT status = ready_chain(vol, vol->root);

	while (SUCCEEDED(status) && count) {
		DWORD index = (DWORD)(offset / MINI_SIZE), inner = (DWORD)(offset % MINI_SIZE);
		ULONG run = MINI_SIZE - inner;

		if (run > count)
			run = count;
		status = read_regular(vol, vol->root, (ULONGLONG)item->chain[index] * MINI_SIZE + inner, buffer, run);
		buffer += run;
		offset += run;
		count -= run;
	}
	return status;
}

static HRESULT write_mini(volume *vol, element *item, ULONGLONG offset, const BYTE *buffer, ULONG count)
{
	HRESULT status = ready_chain(vol, vol->root);

	while (SUCCEEDED(status) && count) {
		DWORD index = (DWORD)(offset / MINI_SIZE), inner = (DWORD)(offset % MINI_SIZE);
		ULONG run = MINI_SIZE - inner;

		if (run > count)
			run = count;
		if (mini_is_shared(vol, item->chain[index])) {
			BYTE data[MINI_SIZE];
			DWORD fresh;
			status = read_regular(vol, vol->root, (ULONGLONG)item->chain[index] * MINI_SIZE, data, MINI_SIZE);
			if (SUCCEEDED(status))
				status = allocate_mini(vol, &fresh);
			if (SUCCEEDED(status))
				status = write_regular(vol, vol->root, (ULONGLONG)fresh * MINI_SIZE, data, MINI_SIZE);
			if (FAILED(status))
				return status;
			item->chain[index] = fresh;
		}
		status = write_regular(vol, vol->root, (ULONGLONG)item->chain[index] * MINI_SIZE + inner, buffer, run);
		buffer += run;
		offset += run;
		count -= run;
	}
	return status;
}

HRESULT element_read(volume *vol, element *item, ULONGLONG offset, void *buffer, ULONG count, ULONG *done)
{
	HRESULT status = ready_chain(vol, item);

	*done = 0;
	if (FAILED(status))
		return status;
	if (offset >= item->size || !count)
		return S_OK;
	if (count > item->size - offset)
		count = (ULONG)(item->size - offset);
	status = item->in_mini ? read_mini(vol, item, offset, buffer, count) : read_regular(vol, item, offset, buffer, count);
	if (SUCCEEDED(status))
		*done = count;
	return status;
}

/* Moves the data between the mini stream and regular sectors when a stream crosses the cutoff. */
static HRESULT change_storage_class(volume *vol, element *item, BOOL to_mini, ULONGLONG size)
{
	ULONG keep = (ULONG)(item->size < size ? item->size : size), done = 0;
	BYTE *data = mem_alloc(keep);
	HRESULT status;

	if (!data)
		return E_OUTOFMEMORY;
	status = element_read(vol, item, 0, data, keep, &done);
	if (SUCCEEDED(status)) {
		DWORD unit = to_mini ? MINI_SIZE : vol->sector_size;
		item->chain_count = 0;
		item->in_mini = to_mini;
		status = set_chain_length(vol, item, (DWORD)((size + unit - 1) / unit));
	}
	if (SUCCEEDED(status)) {
		item->size = size;
		status = to_mini ? write_mini(vol, item, 0, data, keep) : write_regular(vol, item, 0, data, keep);
	}
	mem_free(data);
	return status;
}

static HRESULT zero_range(volume *vol, element *item, ULONGLONG from, ULONGLONG to)
{
	static const BYTE zero[MINI_SIZE];
	HRESULT status = S_OK;

	while (SUCCEEDED(status) && from < to) {
		ULONG run = to - from > MINI_SIZE ? MINI_SIZE : (ULONG)(to - from);
		status = item->in_mini ? write_mini(vol, item, from, zero, run) : write_regular(vol, item, from, zero, run);
		from += run;
	}
	return status;
}

HRESULT element_resize(volume *vol, element *item, ULONGLONG size)
{
	BOOL want_mini = item->kind == KIND_STREAM && size < MINI_CUTOFF;
	ULONGLONG old_size = item->size;
	DWORD unit;
	HRESULT status = ready_chain(vol, item);

	if (FAILED(status))
		return status;
	vol->dirty = TRUE;
	if (item->kind == KIND_STREAM && want_mini != item->in_mini) {
		status = change_storage_class(vol, item, want_mini, size);
		if (FAILED(status))
			return status;
	} else {
		unit = item->in_mini ? MINI_SIZE : vol->sector_size;
		if ((size + unit - 1) / unit > 0xfffffff0u)
			return STG_E_MEDIUMFULL;
		status = set_chain_length(vol, item, (DWORD)((size + unit - 1) / unit));
		if (FAILED(status))
			return status;
		item->size = size;
	}
	if (item->kind == KIND_STREAM && size > old_size) {
		/* What a shrink left behind in the last sector must not come back. */
		unit = item->in_mini ? MINI_SIZE : vol->sector_size;
		if (old_size % unit && want_mini == item->in_mini) {
			ULONGLONG tail = old_size + unit - old_size % unit;
			status = zero_range(vol, item, old_size, tail < size ? tail : size);
		}
	}
	return status;
}

HRESULT element_write(volume *vol, element *item, ULONGLONG offset, const void *buffer, ULONG count, ULONG *done)
{
	HRESULT status = ready_chain(vol, item);

	*done = 0;
	if (FAILED(status))
		return status;
	if (!count)
		return S_OK;
	if (offset + count > item->size) {
		status = element_resize(vol, item, offset + count);
		if (FAILED(status))
			return status;
	}
	vol->dirty = TRUE;
	status = item->in_mini ? write_mini(vol, item, offset, buffer, count) : write_regular(vol, item, offset, buffer, count);
	if (SUCCEEDED(status))
		*done = count;
	return status;
}

element *element_clone(volume *vol, element *item, HRESULT *status)
{
	element *copy = mem_alloc(sizeof(*copy)), *child, *last = NULL;

	if (!copy) {
		*status = E_OUTOFMEMORY;
		return NULL;
	}
	*status = ready_chain(vol, item);
	if (FAILED(*status)) {
		mem_free(copy);
		return NULL;
	}
	*copy = *item;
	copy->parent = NULL;
	copy->next = NULL;
	copy->first_child = NULL;
	copy->chain = NULL;
	copy->chain_capacity = copy->chain_count;
	if (item->chain_count) {
		copy->chain = mem_alloc((SIZE_T)item->chain_count * sizeof(DWORD));
		if (!copy->chain) {
			mem_free(copy);
			*status = E_OUTOFMEMORY;
			return NULL;
		}
		memcpy(copy->chain, item->chain, (SIZE_T)item->chain_count * sizeof(DWORD));
	}
	for (child = item->first_child; child; child = child->next) {
		element *cloned = element_clone(vol, child, status);
		if (!cloned) {
			element_free_tree(copy);
			return NULL;
		}
		cloned->parent = copy;
		if (last)
			last->next = cloned;
		else
			copy->first_child = cloned;
		last = cloned;
	}
	return copy;
}

/* Gives the element the state of its saved copy, keeping its identity and its place in the tree. */
void element_restore(element *item, element *saved)
{
	element *child, *next, *parent = item->parent, *sibling = item->next;

	for (child = item->first_child; child; child = next) {
		next = child->next;
		element_free_tree(child);
	}
	mem_free(item->chain);
	*item = *saved;
	item->parent = parent;
	item->next = sibling;
	item->chain = NULL;
	item->first_child = NULL;
	item->chain_capacity = item->chain_count;
	if (saved->chain_count) {
		item->chain = mem_alloc((SIZE_T)saved->chain_count * sizeof(DWORD));
		if (item->chain)
			memcpy(item->chain, saved->chain, (SIZE_T)saved->chain_count * sizeof(DWORD));
	}
	{
		HRESULT status;
		element *last = NULL;
		for (child = saved->first_child; child; child = child->next) {
			element *cloned = element_clone(NULL, child, &status);
			if (!cloned)
				break;
			cloned->parent = item;
			if (last)
				last->next = cloned;
			else
				item->first_child = cloned;
			last = cloned;
		}
	}
}

void snapshot_register(volume *vol, element *tree)
{
	struct snapshot_link *link = mem_alloc(sizeof(*link));

	if (link) {
		link->tree = tree;
		link->next = vol->snapshots;
		vol->snapshots = link;
	}
	vol->shadows++;
	vol->epoch++;
}

void snapshot_forget(volume *vol, element *tree)
{
	struct snapshot_link **link = &vol->snapshots;

	while (*link) {
		if ((*link)->tree == tree) {
			struct snapshot_link *gone = *link;
			*link = gone->next;
			mem_free(gone);
			break;
		}
		link = &(*link)->next;
	}
	vol->shadows--;
	vol->epoch++;
}

/* Directory reading. Links are walked with every index visited at most once, so a damaged file
 * cannot loop or attach an entry twice. */
static HRESULT attach_children(const BYTE *directory, DWORD count, BYTE *seen, DWORD index, element *storage, int depth);

static WORD entry_name_bytes(const BYTE *entry)
{
	WORD bytes = get16(entry + 64);

	return bytes > 64 ? 64 : bytes;
}

static element *read_entry(const BYTE *entry)
{
	element *item = mem_alloc(sizeof(*item));
	WORD bytes = entry_name_bytes(entry), at;

	if (!item)
		return NULL;
	for (at = 0; at + 1 < bytes && at < 62; at += 2)
		item->name[at / 2] = get16(entry + at);
	item->kind = entry[66];
	memcpy(&item->clsid, entry + 80, sizeof(CLSID));
	item->state_bits = get32(entry + 96);
	item->created.dwLowDateTime = get32(entry + 100);
	item->created.dwHighDateTime = get32(entry + 104);
	item->modified.dwLowDateTime = get32(entry + 108);
	item->modified.dwHighDateTime = get32(entry + 112);
	item->start = get32(entry + 116);
	item->size = get32(entry + 120) | (ULONGLONG)get32(entry + 124) << 32;
	return item;
}

static HRESULT attach_children(const BYTE *directory, DWORD count, BYTE *seen, DWORD index, element *storage, int depth)
{
	const BYTE *entry;
	element *item;
	HRESULT status;

	if (index == NO_STREAM)
		return S_OK;
	if (index >= count || seen[index] || depth > 4096)
		return STG_E_DOCFILECORRUPT;
	seen[index] = 1;
	entry = directory + (SIZE_T)index * ENTRY_SIZE;
	status = attach_children(directory, count, seen, get32(entry + 68), storage, depth + 1);
	if (FAILED(status))
		return status;
	if (entry[66] == KIND_STORAGE || entry[66] == KIND_STREAM) {
		item = read_entry(entry);
		if (!item)
			return E_OUTOFMEMORY;
		element_insert(storage, item);
		status = attach_children(directory, count, seen, get32(entry + 76), item, depth + 1);
		if (FAILED(status))
			return status;
	}
	return attach_children(directory, count, seen, get32(entry + 72), storage, depth + 1);
}

static void prepare_loaded(volume *vol, element *item)
{
	element *child;

	/* Version 3 files may have anything in the high half of a size. */
	if (vol->major == 3)
		item->size &= 0xffffffffu;
	item->chain_ready = FALSE;
	item->in_mini = item->kind == KIND_STREAM && item->size < MINI_CUTOFF;
	if (item->kind == KIND_STORAGE) {
		item->chain_ready = TRUE;
		item->size = 0;
	}
	for (child = item->first_child; child; child = child->next)
		prepare_loaded(vol, child);
}

static HRESULT read_chain_bytes(volume *vol, DWORD start, BYTE **result, DWORD *sectors)
{
	DWORD count = 0, sector = start, capacity = 16;
	BYTE *data = mem_alloc((SIZE_T)capacity << vol->sector_shift);

	if (!data)
		return E_OUTOFMEMORY;
	while (sector != END_OF_CHAIN && sector != FREE_SECTOR) {
		HRESULT status;
		if (sector >= vol->fat_count || count >= vol->fat_count) {
			mem_free(data);
			return STG_E_DOCFILECORRUPT;
		}
		if (count == capacity) {
			BYTE *grown = mem_realloc(data, (SIZE_T)(capacity * 2) << vol->sector_shift);
			if (!grown) {
				mem_free(data);
				return E_OUTOFMEMORY;
			}
			data = grown;
			capacity *= 2;
		}
		status = read_at(vol, sector_offset(vol, sector), data + ((SIZE_T)count << vol->sector_shift), vol->sector_size);
		if (FAILED(status)) {
			mem_free(data);
			return status;
		}
		count++;
		sector = vol->fat[sector];
	}
	*result = data;
	*sectors = count;
	return S_OK;
}

static HRESULT load_fat(volume *vol, const BYTE *header)
{
	DWORD fat_sectors = get32(header + 44), next = get32(header + 68), left = get32(header + 72);
	DWORD per_sector = vol->sector_size / 4, slot, count = 0, *list;
	HRESULT status = S_OK;

	if (fat_sectors > 0x3ffffff)
		return STG_E_DOCFILECORRUPT;
	list = mem_alloc((SIZE_T)(fat_sectors + 1) * sizeof(DWORD));
	if (!list)
		return E_OUTOFMEMORY;
	for (slot = 0; slot < HEADER_DIFAT && count < fat_sectors; slot++)
		list[count++] = get32(header + 76 + slot * 4);
	while (SUCCEEDED(status) && count < fat_sectors && next != END_OF_CHAIN && left--) {
		BYTE *block = mem_alloc(vol->sector_size);
		if (!block) {
			status = E_OUTOFMEMORY;
			break;
		}
		status = read_at(vol, sector_offset(vol, next), block, vol->sector_size);
		for (slot = 0; SUCCEEDED(status) && slot + 1 < per_sector && count < fat_sectors; slot++)
			list[count++] = get32(block + slot * 4);
		next = get32(block + (per_sector - 1) * 4);
		mem_free(block);
	}
	if (SUCCEEDED(status) && count < fat_sectors)
		status = STG_E_DOCFILECORRUPT;
	if (SUCCEEDED(status)) {
		vol->fat_count = fat_sectors * per_sector;
		vol->fat = mem_alloc((SIZE_T)(vol->fat_count ? vol->fat_count : 1) * sizeof(DWORD));
		if (!vol->fat)
			status = E_OUTOFMEMORY;
	}
	if (SUCCEEDED(status)) {
		BYTE *block = mem_alloc(vol->sector_size);
		if (!block)
			status = E_OUTOFMEMORY;
		for (slot = 0; SUCCEEDED(status) && slot < fat_sectors; slot++) {
			DWORD at;
			status = read_at(vol, sector_offset(vol, list[slot]), block, vol->sector_size);
			for (at = 0; SUCCEEDED(status) && at < per_sector; at++)
				vol->fat[slot * per_sector + at] = get32(block + at * 4);
		}
		mem_free(block);
	}
	mem_free(list);
	return status;
}

static HRESULT load_mini_fat(volume *vol, const BYTE *header)
{
	BYTE *data;
	DWORD sectors, at;
	DWORD start = get32(header + 60);
	HRESULT status;

	if (!get32(header + 64) || start == END_OF_CHAIN) {
		vol->mini_fat = mem_alloc(sizeof(DWORD));
		return vol->mini_fat ? S_OK : E_OUTOFMEMORY;
	}
	status = read_chain_bytes(vol, start, &data, &sectors);
	if (FAILED(status))
		return status;
	vol->mini_count = sectors * (vol->sector_size / 4);
	vol->mini_fat = mem_alloc((SIZE_T)(vol->mini_count ? vol->mini_count : 1) * sizeof(DWORD));
	if (vol->mini_fat) {
		for (at = 0; at < vol->mini_count; at++)
			vol->mini_fat[at] = get32(data + (SIZE_T)at * 4);
	} else {
		status = E_OUTOFMEMORY;
	}
	mem_free(data);
	return status;
}

static HRESULT load_directory(volume *vol, const BYTE *header)
{
	BYTE *data, *seen;
	DWORD sectors, count;
	HRESULT status = read_chain_bytes(vol, get32(header + 48), &data, &sectors);

	if (FAILED(status))
		return status;
	count = sectors * (vol->sector_size / ENTRY_SIZE);
	seen = mem_alloc(count ? count : 1);
	if (!count || !seen) {
		mem_free(data);
		mem_free(seen);
		return count ? E_OUTOFMEMORY : STG_E_DOCFILECORRUPT;
	}
	vol->root = read_entry(data);
	if (!vol->root) {
		status = E_OUTOFMEMORY;
	} else {
		vol->root->kind = KIND_ROOT;
		lstrcpyW(vol->root->name, L"Root Entry");
		seen[0] = 1;
		status = attach_children(data, count, seen, get32(data + 76), vol->root, 0);
	}
	if (SUCCEEDED(status))
		prepare_loaded(vol, vol->root);
	mem_free(seen);
	mem_free(data);
	return status;
}

static DWORD table_high_water(const DWORD *table, DWORD count)
{
	while (count && table[count - 1] == FREE_SECTOR)
		count--;
	return count;
}

static volume *volume_new(ILockBytes *bytes, BOOL writable)
{
	volume *vol = mem_alloc(sizeof(*vol));

	if (!vol)
		return NULL;
	vol->refs = 1;
	InitializeCriticalSection(&vol->lock);
	ILockBytes_AddRef(bytes);
	vol->bytes = bytes;
	vol->writable = writable;
	vol->epoch = 1;
	return vol;
}

HRESULT volume_load(ILockBytes *bytes, BOOL writable, volume **result)
{
	BYTE header[HEADER_SIZE];
	ULARGE_INTEGER at;
	ULONG got = 0;
	WORD shift;
	volume *vol;
	HRESULT status;

	at.QuadPart = 0;
	status = ILockBytes_ReadAt(bytes, at, header, sizeof(header), &got);
	if (FAILED(status))
		return status;
	if (got < sizeof(header) || !looks_like_docfile(header, got))
		return STG_E_FILEALREADYEXISTS;
	shift = get16(header + 30);
	if ((get16(header + 26) != 3 && get16(header + 26) != 4) || (shift != 9 && shift != 12) || get16(header + 32) != 6)
		return STG_E_OLDFORMAT;
	vol = volume_new(bytes, writable);
	if (!vol)
		return E_OUTOFMEMORY;
	vol->major = get16(header + 26);
	vol->sector_shift = shift;
	vol->sector_size = 1u << shift;
	status = load_fat(vol, header);
	if (SUCCEEDED(status))
		status = load_mini_fat(vol, header);
	if (SUCCEEDED(status))
		status = load_directory(vol, header);
	if (FAILED(status)) {
		volume_release(vol);
		return status;
	}
	vol->end_sector = table_high_water(vol->fat, vol->fat_count);
	vol->mini_end = table_high_water(vol->mini_fat, vol->mini_count);
	/* The mini stream may be longer than the sectors its table describes. */
	if (vol->root->size / MINI_SIZE > vol->mini_end)
		vol->mini_end = (DWORD)(vol->root->size / MINI_SIZE);
	*result = vol;
	return S_OK;
}

HRESULT volume_create(ILockBytes *bytes, DWORD sector_size, volume **result)
{
	volume *vol = volume_new(bytes, TRUE);
	ULARGE_INTEGER empty;
	HRESULT status;

	if (!vol)
		return E_OUTOFMEMORY;
	vol->sector_size = sector_size;
	vol->sector_shift = sector_size == 4096 ? 12 : 9;
	vol->major = sector_size == 4096 ? 4 : 3;
	vol->root = element_new(L"Root Entry", KIND_ROOT);
	vol->fat = mem_alloc(sizeof(DWORD));
	vol->mini_fat = mem_alloc(sizeof(DWORD));
	if (!vol->root || !vol->fat || !vol->mini_fat) {
		volume_release(vol);
		return E_OUTOFMEMORY;
	}
	memset(&vol->root->created, 0, sizeof(FILETIME));
	empty.QuadPart = 0;
	ILockBytes_SetSize(bytes, empty);
	vol->dirty = TRUE;
	status = volume_flush(vol);
	if (FAILED(status)) {
		volume_release(vol);
		return status;
	}
	*result = vol;
	return S_OK;
}

void volume_add_ref(volume *vol)
{
	InterlockedIncrement(&vol->refs);
}

void volume_release(volume *vol)
{
	if (InterlockedDecrement(&vol->refs))
		return;
	element_free_tree(vol->root);
	while (vol->snapshots) {
		struct snapshot_link *gone = vol->snapshots;
		vol->snapshots = gone->next;
		mem_free(gone);
	}
	mem_free(vol->fat);
	mem_free(vol->mini_fat);
	mem_free(vol->stamps);
	mem_free(vol->mini_stamps);
	ILockBytes_Release(vol->bytes);
	DeleteCriticalSection(&vol->lock);
	mem_free(vol);
}

/* The flush: every chain goes into new tables, the directory is laid out afresh, and the header
 * comes last. */
struct table {
	DWORD *items;
	DWORD count;
	DWORD cursor;
};

static BOOL table_grow(struct table *table, DWORD count)
{
	DWORD at, *grown;

	if (count <= table->count)
		return TRUE;
	grown = mem_realloc(table->items, (SIZE_T)count * sizeof(DWORD));
	if (!grown)
		return FALSE;
	table->items = grown;
	for (at = table->count; at < count; at++)
		grown[at] = FREE_SECTOR;
	table->count = count;
	return TRUE;
}

static BOOL table_take(struct table *table, DWORD *result)
{
	while (table->cursor < table->count && table->items[table->cursor] != FREE_SECTOR)
		table->cursor++;
	if (table->cursor == table->count && !table_grow(table, table->count + 1))
		return FALSE;
	*result = table->cursor++;
	return TRUE;
}

static void link_chain(struct table *table, const DWORD *chain, DWORD count)
{
	DWORD at;

	for (at = 0; at < count; at++)
		if (chain[at] < table->count)
			table->items[chain[at]] = at + 1 < count ? chain[at + 1] : END_OF_CHAIN;
}

static HRESULT link_tree(volume *vol, element *item, struct table *regular, struct table *mini, DWORD *entries)
{
	element *child;
	HRESULT status = ready_chain(vol, item);

	if (FAILED(status))
		return status;
	item->index = (*entries)++;
	if (item->kind != KIND_STORAGE) {
		link_chain(item->in_mini ? mini : regular, item->chain, item->chain_count);
		item->start = item->chain_count ? item->chain[0] : END_OF_CHAIN;
	}
	for (child = item->first_child; child; child = child->next) {
		status = link_tree(vol, child, regular, mini, entries);
		if (FAILED(status))
			return status;
	}
	return S_OK;
}

static void reserve_snapshot(element *item, struct table *regular, struct table *mini)
{
	struct table *table = item->in_mini ? mini : regular;
	element *child;
	DWORD at;

	if (item->kind != KIND_STORAGE) {
		for (at = 0; at < item->chain_count; at++)
			if (item->chain[at] < table->count && table->items[item->chain[at]] == FREE_SECTOR)
				table->items[item->chain[at]] = RESERVED_SECTOR;
	}
	for (child = item->first_child; child; child = child->next)
		reserve_snapshot(child, regular, mini);
}

/* Lays the siblings out as a perfectly balanced search tree. Its last level is red and all the
 * others black, which makes it a valid red-black tree. */
static DWORD place_siblings(BYTE *directory, element **items, int count, int depth, int red_depth)
{
	int middle = count / 2;
	BYTE *entry;

	if (!count)
		return NO_STREAM;
	entry = directory + (SIZE_T)items[middle]->index * ENTRY_SIZE;
	entry[67] = depth == red_depth ? 0 : 1;
	put32(entry + 68, place_siblings(directory, items, middle, depth + 1, red_depth));
	put32(entry + 72, place_siblings(directory, items + middle + 1, count - middle - 1, depth + 1, red_depth));
	return items[middle]->index;
}

static HRESULT write_entries(BYTE *directory, element *item)
{
	BYTE *entry = directory + (SIZE_T)item->index * ENTRY_SIZE;
	int length = lstrlenW(item->name), at, count = 0, depth = 0;
	element *child, **items;

	for (at = 0; at < length; at++)
		put16(entry + at * 2, item->name[at]);
	put16(entry + 64, (WORD)((length + 1) * 2));
	entry[66] = item->kind;
	if (item->kind == KIND_ROOT)
		entry[67] = 1;
	memcpy(entry + 80, &item->clsid, sizeof(CLSID));
	put32(entry + 96, item->state_bits);
	put32(entry + 100, item->created.dwLowDateTime);
	put32(entry + 104, item->created.dwHighDateTime);
	put32(entry + 108, item->modified.dwLowDateTime);
	put32(entry + 112, item->modified.dwHighDateTime);
	put32(entry + 116, item->kind == KIND_STORAGE ? 0 : item->start);
	put32(entry + 120, (DWORD)item->size);
	put32(entry + 124, (DWORD)(item->size >> 32));
	for (child = item->first_child; child; child = child->next)
		count++;
	if (!count)
		return S_OK;
	items = mem_alloc((SIZE_T)count * sizeof(*items));
	if (!items)
		return E_OUTOFMEMORY;
	for (child = item->first_child, at = 0; child; child = child->next)
		items[at++] = child;
	while ((1 << (depth + 1)) <= count)
		depth++;
	/* A full tree has no partial last level to colour. */
	put32(entry + 76, place_siblings(directory, items, count, 0, (1 << (depth + 1)) - 1 == count ? -1 : depth));
	mem_free(items);
	for (child = item->first_child; child; child = child->next) {
		HRESULT status = write_entries(directory, child);
		if (FAILED(status))
			return status;
	}
	return S_OK;
}

static HRESULT write_sectors_chain(volume *vol, struct table *regular, const BYTE *data, DWORD sectors, DWORD *first)
{
	DWORD at, previous = END_OF_CHAIN;

	*first = END_OF_CHAIN;
	for (at = 0; at < sectors; at++) {
		DWORD sector;
		HRESULT status;
		if (!table_take(regular, &sector))
			return E_OUTOFMEMORY;
		regular->items[sector] = END_OF_CHAIN;
		if (previous != END_OF_CHAIN)
			regular->items[previous] = sector;
		else
			*first = sector;
		status = write_at(vol, sector_offset(vol, sector), data + ((SIZE_T)at << vol->sector_shift), vol->sector_size);
		if (FAILED(status))
			return status;
		previous = sector;
	}
	return S_OK;
}

static void clear_sibling_slots(BYTE *directory, DWORD count)
{
	DWORD at;

	for (at = 0; at < count; at++) {
		BYTE *entry = directory + (SIZE_T)at * ENTRY_SIZE;
		put32(entry + 68, NO_STREAM);
		put32(entry + 72, NO_STREAM);
		put32(entry + 76, NO_STREAM);
	}
}

HRESULT volume_flush(volume *vol)
{
	struct table regular = { 0 }, mini = { 0 };
	struct snapshot_link *link;
	BYTE *directory = NULL, *mini_bytes = NULL, *block = NULL, header[4096];
	DWORD entries = 0, per_sector = vol->sector_size / 4, directory_sectors, mini_sectors, first_directory, first_mini = END_OF_CHAIN;
	DWORD fat_sectors = 0, difat_sectors = 0, *fat_list = NULL, first_difat = END_OF_CHAIN, at;
	HRESULT status;
	ULARGE_INTEGER size;

	if (!vol->dirty)
		return S_OK;
	status = ready_chain(vol, vol->root);
	if (FAILED(status))
		return status;
	if (!table_grow(&regular, vol->end_sector) || !table_grow(&mini, vol->mini_end)) {
		status = E_OUTOFMEMORY;
		goto done;
	}
	status = link_tree(vol, vol->root, &regular, &mini, &entries);
	if (FAILED(status))
		goto done;
	/* Mini sectors nothing uses at the tail come off the mini stream. */
	vol->mini_end = table_high_water(mini.items, mini.count);
	for (link = vol->snapshots; link; link = link->next)
		reserve_snapshot(link->tree, &regular, &mini);
	if (table_high_water(mini.items, mini.count) > vol->mini_end)
		vol->mini_end = table_high_water(mini.items, mini.count);
	mini.count = vol->mini_end;
	status = element_resize(vol, vol->root, (ULONGLONG)vol->mini_end * MINI_SIZE);
	if (FAILED(status))
		goto done;
	/* That may have allocated or dropped sectors of the mini stream itself. */
	if (!table_grow(&regular, vol->end_sector)) {
		status = E_OUTOFMEMORY;
		goto done;
	}
	for (at = 0; at < regular.count; at++)
		if (regular.items[at] != RESERVED_SECTOR)
			regular.items[at] = FREE_SECTOR;
	{
		DWORD counted = 0;
		status = link_tree(vol, vol->root, &regular, &mini, &counted);
		if (FAILED(status))
			goto done;
	}

	directory_sectors = (entries * ENTRY_SIZE + vol->sector_size - 1) >> vol->sector_shift;
	if (!directory_sectors)
		directory_sectors = 1;
	directory = mem_alloc((SIZE_T)directory_sectors << vol->sector_shift);
	if (!directory) {
		status = E_OUTOFMEMORY;
		goto done;
	}
	clear_sibling_slots(directory, directory_sectors * (vol->sector_size / ENTRY_SIZE));
	status = write_entries(directory, vol->root);
	if (FAILED(status))
		goto done;
	status = write_sectors_chain(vol, &regular, directory, directory_sectors, &first_directory);
	if (FAILED(status))
		goto done;

	mini_sectors = (vol->mini_end + per_sector - 1) / per_sector;
	if (mini_sectors) {
		mini_bytes = mem_alloc((SIZE_T)mini_sectors << vol->sector_shift);
		if (!mini_bytes) {
			status = E_OUTOFMEMORY;
			goto done;
		}
		for (at = 0; at < mini_sectors * per_sector; at++) {
			DWORD value = at < mini.count ? mini.items[at] : FREE_SECTOR;
			put32(mini_bytes + (SIZE_T)at * 4, value == RESERVED_SECTOR ? FREE_SECTOR : value);
		}
		status = write_sectors_chain(vol, &regular, mini_bytes, mini_sectors, &first_mini);
		if (FAILED(status))
			goto done;
	}

	/* The tables have to describe the sectors they occupy themselves. */
	for (;;) {
		DWORD wanted_difat = fat_sectors > HEADER_DIFAT ? (fat_sectors - HEADER_DIFAT + per_sector - 2) / (per_sector - 1) : 0;
		DWORD total = table_high_water(regular.items, regular.count);
		DWORD sector;
		if (fat_sectors * per_sector >= total && wanted_difat == difat_sectors)
			break;
		if (!table_take(&regular, &sector)) {
			status = E_OUTOFMEMORY;
			goto done;
		}
		if (wanted_difat > difat_sectors) {
			regular.items[sector] = DIFAT_SECTOR;
			difat_sectors++;
		} else {
			regular.items[sector] = FAT_SECTOR;
			fat_sectors++;
		}
	}
	fat_list = mem_alloc((SIZE_T)(fat_sectors + difat_sectors + 1) * sizeof(DWORD));
	block = mem_alloc(vol->sector_size);
	if (!fat_list || !block) {
		status = E_OUTOFMEMORY;
		goto done;
	}
	{
		DWORD found = 0;
		for (at = 0; at < regular.count && found < fat_sectors; at++)
			if (regular.items[at] == FAT_SECTOR)
				fat_list[found++] = at;
	}
	for (at = 0; SUCCEEDED(status) && at < fat_sectors; at++) {
		DWORD slot;
		for (slot = 0; slot < per_sector; slot++) {
			DWORD index = at * per_sector + slot;
			DWORD value = index < regular.count ? regular.items[index] : FREE_SECTOR;
			put32(block + slot * 4, value == RESERVED_SECTOR ? FREE_SECTOR : value);
		}
		status = write_at(vol, sector_offset(vol, fat_list[at]), block, vol->sector_size);
	}
	if (FAILED(status))
		goto done;
	if (difat_sectors) {
		DWORD listed = HEADER_DIFAT, found = 0, *difat_list = fat_list + fat_sectors;
		for (at = 0; at < regular.count && found < difat_sectors; at++)
			if (regular.items[at] == DIFAT_SECTOR)
				difat_list[found++] = at;
		first_difat = difat_list[0];
		for (at = 0; SUCCEEDED(status) && at < difat_sectors; at++) {
			DWORD slot;
			for (slot = 0; slot + 1 < per_sector; slot++, listed++)
				put32(block + slot * 4, listed < fat_sectors ? fat_list[listed] : FREE_SECTOR);
			put32(block + (per_sector - 1) * 4, at + 1 < difat_sectors ? difat_list[at + 1] : END_OF_CHAIN);
			status = write_at(vol, sector_offset(vol, difat_list[at]), block, vol->sector_size);
		}
		if (FAILED(status))
			goto done;
	}

	memset(header, 0, sizeof(header));
	memcpy(header, signature, 8);
	put16(header + 24, 0x3e);
	put16(header + 26, (WORD)vol->major);
	put16(header + 28, 0xfffe);
	put16(header + 30, (WORD)vol->sector_shift);
	put16(header + 32, 6);
	put32(header + 40, vol->major == 4 ? directory_sectors : 0);
	put32(header + 44, fat_sectors);
	put32(header + 48, first_directory);
	put32(header + 56, MINI_CUTOFF);
	put32(header + 60, first_mini);
	put32(header + 64, mini_sectors);
	put32(header + 68, first_difat);
	put32(header + 72, difat_sectors);
	for (at = 0; at < HEADER_DIFAT; at++)
		put32(header + 76 + at * 4, at < fat_sectors ? fat_list[at] : FREE_SECTOR);
	status = write_at(vol, 0, header, vol->sector_size);
	if (FAILED(status))
		goto done;

	/* What was written is now the file's state; reserved sectors stay out of reach until their
	 * snapshot is gone. */
	mem_free(vol->fat);
	vol->fat = regular.items;
	vol->fat_count = regular.count;
	vol->fat_cursor = 0;
	regular.items = NULL;
	mem_free(vol->mini_fat);
	vol->mini_fat = mini.items;
	vol->mini_count = mini.count;
	vol->mini_cursor = 0;
	mini.items = NULL;
	vol->end_sector = table_high_water(vol->fat, vol->fat_count);
	size.QuadPart = sector_offset(vol, vol->end_sector);
	ILockBytes_SetSize(vol->bytes, size);
	ILockBytes_Flush(vol->bytes);
	vol->epoch++;
	vol->dirty = FALSE;
done:
	mem_free(regular.items);
	mem_free(mini.items);
	mem_free(directory);
	mem_free(mini_bytes);
	mem_free(block);
	mem_free(fat_list);
	return status;
}
