/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.Storage.AccessCache: StorageApplicationPermissions and its two lists, FutureAccessList and MostRecentlyUsedList.
 *
 * - Each list keeps, for a token, a file or folder by its path and the metadata the app gave. The lists belong to the app's package and
 *   last from one run to the next, kept in its data under AccessCache; a process of no package has none, as for ApplicationData.
 * - A token the app does not choose is a new GUID in braces. A token that names nothing is refused; an item that no longer exists is
 *   found and then fails as a missing file or folder.
 * - FutureAccessList holds up to 1000 items and refuses more. MostRecentlyUsedList holds 25, the most recent first: an item added again
 *   moves to the front with its token, and the oldest is dropped past 25, which ItemRemoved tells. Visibility in the system's own list
 *   changes nothing here. */
#include "object.h"
#include "../raven-winrt/winrt.h"

static const IID iid_permissions = {0x4391dfaa, 0xd033, 0x48f9, {0x80, 0x60, 0x3e, 0xc8, 0x47, 0xd2, 0xe3, 0xf1}};
static const IID iid_access_list = {0x2caff6ad, 0xde90, 0x47f5, {0xb2, 0xc3, 0xdd, 0x36, 0xc9, 0xfd, 0xd4, 0x53}};
static const IID iid_recent_list = {0x016239d5, 0x510d, 0x411e, {0x8c, 0xf1, 0xc3, 0xd1, 0xef, 0xfa, 0x4c, 0x33}};
static const IID iid_recent_list2 = {0xda481ea0, 0xed8d, 0x4731, {0xa1, 0xdb, 0xe4, 0x4e, 0xe2, 0x20, 0x40, 0x93}};
static const IID iid_removed_args = {0x59677e5c, 0x55be, 0x4c66, {0xba, 0x66, 0x5e, 0xae, 0xa7, 0x9d, 0x26, 0x31}};
/* IVectorView<AccessListEntry>, IIterable<AccessListEntry> and IIterator<AccessListEntry>. */
static const IID iid_entry_view = {0x42b49b8a, 0x3014, 0x5d27, {0x8f, 0x2c, 0x1e, 0xf5, 0xee, 0x89, 0xec, 0x00}};
static const IID iid_entry_iterable = {0x4995c2b0, 0x736b, 0x588d, {0xae, 0x42, 0x6f, 0x69, 0xb0, 0x25, 0xb3, 0x88}};
static const IID iid_entry_iterator = {0xd1a0a6c4, 0x889d, 0x519b, {0x85, 0x08, 0x26, 0x24, 0x1b, 0x32, 0x9b, 0x7e}};

#define FUTURE_MOST 1000
#define RECENT_MOST 25

/* AccessListEntry, as the app receives it: both strings are its own to delete. */
struct access_entry {
	HSTRING token, metadata;
};

/* One item of a list as it is kept. */
struct kept {
	WCHAR *token, *metadata, *path;
};

struct entries {
	struct kept *items;
	UINT32 count;
};

struct list {
	struct object base;
	const WCHAR *file;
	UINT32 most;
	struct handlers removed;
};

/* Every list is read, changed and written back under this lock. */
static SRWLOCK lock = SRWLOCK_INIT;

static WCHAR *copy_text(const WCHAR *text, UINT32 length)
{
	WCHAR *made = ALLOC(((SIZE_T)length + 1) * sizeof(WCHAR));
	if (made)
		memcpy(made, text, (SIZE_T)length * sizeof(WCHAR));
	return made;
}

static void free_entry(struct kept *entry)
{
	FREE(entry->token);
	FREE(entry->metadata);
	FREE(entry->path);
}

static void free_entries(struct entries *list)
{
	for (UINT32 at = 0; at < list->count; ++at)
		free_entry(&list->items[at]);
	FREE(list->items);
	list->items = NULL;
	list->count = 0;
}

/* Room for one more item at the end of a list. */
static BOOL grow(struct entries *list)
{
	const SIZE_T size = ((SIZE_T)list->count + 1) * sizeof *list->items;
	struct kept *grown = list->items ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, list->items, size) : ALLOC(size);
	if (grown)
		list->items = grown;
	return grown != NULL;
}

static HRESULT list_path(const struct list *self, WCHAR *path)
{
	HRESULT result = package_data_path(L"AccessCache", path);
	if (FAILED(result))
		return result;
	if (lstrlenW(path) + lstrlenW(self->file) + 2 >= MAX_PATH)
		return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
	lstrcatW(path, L"\\");
	lstrcatW(path, self->file);
	return S_OK;
}

/* The file holds, for each item, its token, metadata and path, each a count of characters and then the characters. */
static HRESULT load(const struct list *self, struct entries *out)
{
	WCHAR path[MAX_PATH];
	out->items = NULL;
	out->count = 0;
	HRESULT result = list_path(self, path);
	if (FAILED(result))
		return result;
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return GetLastError() == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(GetLastError());
	LARGE_INTEGER size;
	BYTE *bytes = NULL;
	DWORD read = 0;
	if (!GetFileSizeEx(file, &size) || size.QuadPart > 64 * 1024 * 1024)
		result = E_UNEXPECTED;
	else if (!(bytes = ALLOC(size.LowPart + 1)))
		result = E_OUTOFMEMORY;
	else if (!ReadFile(file, bytes, size.LowPart, &read, NULL) || read != size.LowPart)
		result = HRESULT_FROM_WIN32(GetLastError());
	CloseHandle(file);
	UINT32 at = 0;
	while (SUCCEEDED(result) && at < read) {
		struct kept entry = {0};
		WCHAR **fields[3] = {&entry.token, &entry.metadata, &entry.path};
		for (int field = 0; field < 3 && SUCCEEDED(result); ++field) {
			UINT32 length;
			if (read - at < sizeof length) {
				result = E_UNEXPECTED;
				break;
			}
			memcpy(&length, bytes + at, sizeof length);
			at += sizeof length;
			if (length > (read - at) / sizeof(WCHAR)) {
				result = E_UNEXPECTED;
				break;
			}
			if (!(*fields[field] = copy_text((const WCHAR *)(bytes + at), length)))
				result = E_OUTOFMEMORY;
			at += length * (UINT32)sizeof(WCHAR);
		}
		if (FAILED(result) || !grow(out)) {
			free_entry(&entry);
			if (SUCCEEDED(result))
				result = E_OUTOFMEMORY;
			break;
		}
		out->items[out->count++] = entry;
	}
	FREE(bytes);
	/* A file that cannot be read whole is a list of what could be read. */
	return result == E_UNEXPECTED ? S_OK : result;
}

static HRESULT save(const struct list *self, const struct entries *list)
{
	WCHAR path[MAX_PATH];
	HRESULT result = list_path(self, path);
	if (FAILED(result))
		return result;
	SIZE_T size = 0;
	for (UINT32 at = 0; at < list->count; ++at)
		size += 3 * sizeof(UINT32) + ((SIZE_T)lstrlenW(list->items[at].token) + lstrlenW(list->items[at].metadata) +
			lstrlenW(list->items[at].path)) * sizeof(WCHAR);
	BYTE *bytes = ALLOC(size + 1), *end = bytes;
	if (!bytes)
		return E_OUTOFMEMORY;
	for (UINT32 at = 0; at < list->count; ++at) {
		const WCHAR *fields[3] = {list->items[at].token, list->items[at].metadata, list->items[at].path};
		for (int field = 0; field < 3; ++field) {
			const UINT32 length = (UINT32)lstrlenW(fields[field]);
			memcpy(end, &length, sizeof length);
			memcpy(end + sizeof length, fields[field], (SIZE_T)length * sizeof(WCHAR));
			end += sizeof length + (SIZE_T)length * sizeof(WCHAR);
		}
	}
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD written = 0;
	if (file == INVALID_HANDLE_VALUE)
		result = HRESULT_FROM_WIN32(GetLastError());
	else {
		if (!WriteFile(file, bytes, (DWORD)size, &written, NULL) || written != size)
			result = HRESULT_FROM_WIN32(GetLastError());
		CloseHandle(file);
	}
	FREE(bytes);
	return result;
}

static struct list *list_of(void *iface) { return (struct list *)owner(iface); }

static UINT32 find_token(const struct entries *list, const WCHAR *token)
{
	for (UINT32 at = 0; at < list->count; ++at)
		if (!lstrcmpiW(list->items[at].token, token))
			return at;
	return list->count;
}

static UINT32 find_path(const struct entries *list, const WCHAR *path)
{
	for (UINT32 at = 0; at < list->count; ++at)
		if (!lstrcmpiW(list->items[at].path, path))
			return at;
	return list->count;
}

static const WCHAR *raw(HSTRING text) { return WindowsGetStringRawBuffer(text, NULL); }

/* ---- ItemRemovedEventArgs ---- */

struct removed_args {
	struct object base;
	struct kept entry;
};

static void destroy_removed_args(struct object *base) { free_entry(&((struct removed_args *)base)->entry); }

static HRESULT give_entry(const struct kept *entry, struct access_entry *out)
{
	out->token = out->metadata = NULL;
	HRESULT result = WindowsCreateString(entry->token, (UINT32)lstrlenW(entry->token), &out->token);
	if (SUCCEEDED(result))
		result = WindowsCreateString(entry->metadata, (UINT32)lstrlenW(entry->metadata), &out->metadata);
	if (FAILED(result)) {
		WindowsDeleteString(out->token);
		out->token = NULL;
	}
	return result;
}

static HRESULT WINAPI removed_entry(void *iface, struct access_entry *out)
{
	if (!out)
		return E_POINTER;
	return give_entry(&((struct removed_args *)owner(iface))->entry, out);
}

TABLE(removed_args_table, 0, INSPECTABLE, (const void *)removed_entry);
static const struct face_iid removed_args_iids[] = {{&iid_removed_args, 0}, {NULL, 0}};
static const struct kind removed_args_kind = {"ItemRemovedEventArgs", L"Windows.Storage.AccessCache.ItemRemovedEventArgs", removed_args_iids,
	destroy_removed_args};

/* Tells the list's ItemRemoved handlers of an item it dropped, which the arguments take over. */
static void tell_removed(struct list *self, struct kept *entry)
{
	static const void *const *const tables[] = {removed_args_table.slots};
	struct removed_args *args = make(sizeof *args, &removed_args_kind, tables, 1);
	if (!args) {
		free_entry(entry);
		return;
	}
	args->entry = *entry;
	handlers_raise(&self->removed, face_of(&self->base, 1), face_of(&args->base, 0));
	release(face_of(&args->base, 0));
}

/* ---- the entries ---- */

struct entry_view {
	struct object base;
	UINT32 count;
	struct kept *items;
};

struct entry_iterator {
	struct object base;
	struct entry_view *view;
	UINT32 at;
};

static void destroy_entry_view(struct object *base)
{
	struct entry_view *self = (struct entry_view *)base;
	struct entries list = {self->items, self->count};
	free_entries(&list);
}

static struct entry_view *view_of(void *iface) { return (struct entry_view *)owner(iface); }

static HRESULT WINAPI view_get_at(void *iface, UINT32 index, struct access_entry *out)
{
	struct entry_view *self = view_of(iface);
	if (!out)
		return E_POINTER;
	if (index >= self->count) {
		out->token = out->metadata = NULL;
		return E_BOUNDS;
	}
	return give_entry(&self->items[index], out);
}

static HRESULT WINAPI view_size(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = view_of(iface)->count;
	return S_OK;
}

static HRESULT WINAPI view_index_of(void *iface, struct access_entry value, UINT32 *index, boolean *found)
{
	struct entry_view *self = view_of(iface);
	if (!index || !found)
		return E_POINTER;
	*index = 0;
	*found = FALSE;
	for (UINT32 at = 0; at < self->count; ++at)
		if (!lstrcmpW(self->items[at].token, raw(value.token)) && !lstrcmpW(self->items[at].metadata, raw(value.metadata))) {
			*index = at;
			*found = TRUE;
			break;
		}
	return S_OK;
}

static HRESULT many(struct entry_view *self, UINT32 start, UINT32 capacity, struct access_entry *items, UINT32 *actual)
{
	if (!actual || (capacity && !items))
		return E_POINTER;
	*actual = 0;
	if (start > self->count)
		return E_BOUNDS;
	for (UINT32 at = start; at < self->count && *actual < capacity; ++at) {
		const HRESULT result = give_entry(&self->items[at], &items[*actual]);
		if (FAILED(result)) {
			while (*actual) {
				--*actual;
				WindowsDeleteString(items[*actual].token);
				WindowsDeleteString(items[*actual].metadata);
			}
			return result;
		}
		++*actual;
	}
	return S_OK;
}

static HRESULT WINAPI view_get_many(void *iface, UINT32 start, UINT32 capacity, struct access_entry *items, UINT32 *actual)
{
	return many(view_of(iface), start, capacity, items, actual);
}

static void destroy_entry_iterator(struct object *base) { release(face_of(&((struct entry_iterator *)base)->view->base, 0)); }

static struct entry_iterator *iterator_of(void *iface) { return (struct entry_iterator *)owner(iface); }

static HRESULT WINAPI iterator_current(void *iface, struct access_entry *out)
{
	struct entry_iterator *self = iterator_of(iface);
	return view_get_at(face_of(&self->view->base, 0), self->at, out);
}

static HRESULT WINAPI iterator_has_current(void *iface, boolean *out) { return answer_flag(out, iterator_of(iface)->at < iterator_of(iface)->view->count); }

static HRESULT WINAPI iterator_move_next(void *iface, boolean *out)
{
	struct entry_iterator *self = iterator_of(iface);
	if (self->at < self->view->count)
		++self->at;
	return answer_flag(out, self->at < self->view->count);
}

static HRESULT WINAPI iterator_get_many(void *iface, UINT32 capacity, struct access_entry *items, UINT32 *actual)
{
	struct entry_iterator *self = iterator_of(iface);
	const HRESULT result = many(self->view, self->at, capacity, items, actual);
	if (SUCCEEDED(result))
		self->at += *actual;
	return result;
}

TABLE(entry_iterator_table, 0, INSPECTABLE, (const void *)iterator_current, (const void *)iterator_has_current, (const void *)iterator_move_next,
	(const void *)iterator_get_many);
static const struct face_iid entry_iterator_iids[] = {{&iid_entry_iterator, 0}, {NULL, 0}};
static const struct kind entry_iterator_kind = {"AccessListEntryIterator", L"Windows.Foundation.Collections.IIterator`1<Windows.Storage.AccessCache.AccessListEntry>",
	entry_iterator_iids, destroy_entry_iterator};

static HRESULT WINAPI view_first(void *iface, void **out)
{
	static const void *const *const tables[] = {entry_iterator_table.slots};
	if (!out)
		return E_POINTER;
	struct entry_iterator *self = make(sizeof *self, &entry_iterator_kind, tables, 1);
	if (!(*out = self ? face_of(&self->base, 0) : NULL))
		return E_OUTOFMEMORY;
	self->view = view_of(iface);
	reference(face_of(&self->view->base, 0));
	return S_OK;
}

TABLE(entry_view_table, 0, INSPECTABLE, (const void *)view_get_at, (const void *)view_size, (const void *)view_index_of, (const void *)view_get_many);
TABLE(entry_iterable_table, 1, INSPECTABLE, (const void *)view_first);
static const struct face_iid entry_view_iids[] = {{&iid_entry_view, 0}, {&iid_entry_iterable, 1}, {NULL, 0}};
static const struct kind entry_view_kind = {"AccessListEntryView", L"Windows.Storage.AccessCache.AccessListEntryView", entry_view_iids,
	destroy_entry_view};

/* ---- the lists ---- */

/* Adds an item, or with a token replaces the one of that token; a token made here is answered in *made. */
static HRESULT put(void *iface, HSTRING token, void *item, HSTRING metadata, HSTRING *made)
{
	struct list *self = list_of(iface);
	struct entries list;
	struct kept entry = {0}, dropped = {0};
	WCHAR path[MAX_PATH], name[40];
	if (made)
		*made = NULL;
	if (!item || (!made && !WindowsGetStringLen(token)))
		return E_INVALIDARG;
	HRESULT result = path_of_any(item, path);
	if (FAILED(result))
		return result;
	if (!token) {
		GUID guid;
		if (FAILED(result = CoCreateGuid(&guid)))
			return result;
		StringFromGUID2(&guid, name, ARRAYSIZE(name));
	}
	const WCHAR *chosen = token ? raw(token) : name;
	AcquireSRWLockExclusive(&lock);
	result = load(self, &list);
	UINT32 at = list.count;
	if (SUCCEEDED(result)) {
		at = find_token(&list, chosen);
		/* The recent list keeps one entry for an item: adding it again moves it, with its token. */
		if (at == list.count && self->most == RECENT_MOST && !token && (at = find_path(&list, path)) < list.count)
			chosen = list.items[at].token;
		entry.token = copy_text(chosen, (UINT32)lstrlenW(chosen));
		entry.metadata = copy_text(raw(metadata), WindowsGetStringLen(metadata));
		entry.path = copy_text(path, (UINT32)lstrlenW(path));
		if (!entry.token || !entry.metadata || !entry.path)
			result = E_OUTOFMEMORY;
		else if (at == list.count && list.count >= self->most && self->most == FUTURE_MOST)
			result = E_BOUNDS;
	}
	if (SUCCEEDED(result) && made)
		result = WindowsCreateString(entry.token, (UINT32)lstrlenW(entry.token), made);
	if (SUCCEEDED(result)) {
		const BOOL front = self->most == RECENT_MOST;
		if (at < list.count) {
			free_entry(&list.items[at]);
			if (front) {
				memmove(list.items + 1, list.items, at * sizeof *list.items);
				at = 0;
			}
			list.items[at] = entry;
		} else {
			if (!grow(&list))
				result = E_OUTOFMEMORY;
			else {
				if (front) {
					memmove(list.items + 1, list.items, list.count * sizeof *list.items);
					list.items[0] = entry;
				} else
					list.items[list.count] = entry;
				++list.count;
				if (list.count > self->most)
					dropped = list.items[--list.count];
			}
		}
		if (SUCCEEDED(result)) {
			entry.token = entry.metadata = entry.path = NULL;
			result = save(self, &list);
		}
	}
	free_entries(&list);
	ReleaseSRWLockExclusive(&lock);
	free_entry(&entry);
	if (FAILED(result) && made) {
		WindowsDeleteString(*made);
		*made = NULL;
	}
	if (dropped.token) {
		if (SUCCEEDED(result))
			tell_removed(self, &dropped);
		else
			free_entry(&dropped);
	}
	return result;
}

static HRESULT WINAPI list_add_default(void *iface, void *item, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return put(iface, NULL, item, NULL, out);
}

static HRESULT WINAPI list_add(void *iface, void *item, HSTRING metadata, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return put(iface, NULL, item, metadata, out);
}

static HRESULT WINAPI list_replace_default(void *iface, HSTRING token, void *item) { return put(iface, token, item, NULL, NULL); }
static HRESULT WINAPI list_replace(void *iface, HSTRING token, void *item, HSTRING metadata) { return put(iface, token, item, metadata, NULL); }

/* What a token names: the item as a file, a folder or either, in an operation that has ended. */
enum { WANT_FILE = 1, WANT_FOLDER = 2 };

static HRESULT get(void *iface, HSTRING token, int want, void **out)
{
	struct list *self = list_of(iface);
	struct entries list;
	WCHAR path[MAX_PATH] = {0};
	if (!out)
		return E_POINTER;
	*out = NULL;
	AcquireSRWLockShared(&lock);
	HRESULT result = load(self, &list);
	if (SUCCEEDED(result)) {
		const UINT32 at = find_token(&list, raw(token));
		if (at == list.count || lstrlenW(list.items[at].path) >= MAX_PATH)
			result = E_INVALIDARG;
		else
			lstrcpyW(path, list.items[at].path);
	}
	free_entries(&list);
	ReleaseSRWLockShared(&lock);
	if (FAILED(result))
		return result;
	const DWORD attributes = GetFileAttributesW(path);
	const BOOL folder = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
	const IID *operation = want == WANT_FILE ? &iid_file_operation : want == WANT_FOLDER ? &iid_folder_operation : &iid_item_operation;
	union winrt_value value = {0};
	IUnknown *made = NULL;
	if (attributes == INVALID_FILE_ATTRIBUTES)
		result = HRESULT_FROM_WIN32(GetLastError() == ERROR_PATH_NOT_FOUND ? ERROR_PATH_NOT_FOUND : ERROR_FILE_NOT_FOUND);
	else if (want != (WANT_FILE | WANT_FOLDER) && folder != (want == WANT_FOLDER))
		result = E_INVALIDARG;
	else if (SUCCEEDED(result = folder ? make_folder(path, (void **)&made) : make_file(path, (void **)&made)) && want == (WANT_FILE | WANT_FOLDER)) {
		IUnknown *item = NULL;
		result = made->lpVtbl->QueryInterface(made, &iid_storage_item, (void **)&item);
		release(made);
		made = item;
	}
	value.object = made;
	const HRESULT done = winrt_async_done(operation, WINRT_RESULT_OBJECT, result, &value, out);
	if (made)
		release(made);
	return done;
}

static HRESULT WINAPI list_get_item(void *iface, HSTRING token, void **out) { return get(iface, token, WANT_FILE | WANT_FOLDER, out); }
static HRESULT WINAPI list_get_file(void *iface, HSTRING token, void **out) { return get(iface, token, WANT_FILE, out); }
static HRESULT WINAPI list_get_folder(void *iface, HSTRING token, void **out) { return get(iface, token, WANT_FOLDER, out); }
static HRESULT WINAPI list_get_item_options(void *iface, HSTRING token, UINT32 options, void **out) { return list_get_item(iface, token, out); }
static HRESULT WINAPI list_get_file_options(void *iface, HSTRING token, UINT32 options, void **out) { return list_get_file(iface, token, out); }
static HRESULT WINAPI list_get_folder_options(void *iface, HSTRING token, UINT32 options, void **out) { return list_get_folder(iface, token, out); }

static HRESULT WINAPI list_remove(void *iface, HSTRING token)
{
	struct list *self = list_of(iface);
	struct entries list;
	AcquireSRWLockExclusive(&lock);
	HRESULT result = load(self, &list);
	const UINT32 at = SUCCEEDED(result) ? find_token(&list, raw(token)) : 0;
	if (SUCCEEDED(result) && at < list.count) {
		free_entry(&list.items[at]);
		memmove(list.items + at, list.items + at + 1, (list.count - at - 1) * sizeof *list.items);
		--list.count;
		result = save(self, &list);
	}
	free_entries(&list);
	ReleaseSRWLockExclusive(&lock);
	return result;
}

static HRESULT WINAPI list_contains(void *iface, HSTRING token, boolean *out)
{
	struct entries list;
	if (!out)
		return E_POINTER;
	*out = FALSE;
	AcquireSRWLockShared(&lock);
	const HRESULT result = load(list_of(iface), &list);
	if (SUCCEEDED(result))
		*out = find_token(&list, raw(token)) < list.count;
	free_entries(&list);
	ReleaseSRWLockShared(&lock);
	return result;
}

static HRESULT WINAPI list_clear(void *iface)
{
	const struct entries empty = {NULL, 0};
	AcquireSRWLockExclusive(&lock);
	const HRESULT result = save(list_of(iface), &empty);
	ReleaseSRWLockExclusive(&lock);
	return result;
}

static HRESULT WINAPI list_check_access(void *iface, void *item, boolean *out)
{
	struct entries list;
	WCHAR path[MAX_PATH];
	if (!out)
		return E_POINTER;
	*out = FALSE;
	HRESULT result = path_of_any(item, path);
	if (FAILED(result))
		return result;
	AcquireSRWLockShared(&lock);
	result = load(list_of(iface), &list);
	if (SUCCEEDED(result))
		*out = find_path(&list, path) < list.count;
	free_entries(&list);
	ReleaseSRWLockShared(&lock);
	return result;
}

static HRESULT WINAPI list_entries(void *iface, void **out)
{
	static const void *const *const tables[] = {entry_view_table.slots, entry_iterable_table.slots};
	struct entries list;
	if (!out)
		return E_POINTER;
	*out = NULL;
	AcquireSRWLockShared(&lock);
	const HRESULT result = load(list_of(iface), &list);
	ReleaseSRWLockShared(&lock);
	if (FAILED(result))
		return result;
	struct entry_view *self = make(sizeof *self, &entry_view_kind, tables, 2);
	if (!self) {
		free_entries(&list);
		return E_OUTOFMEMORY;
	}
	self->items = list.items;
	self->count = list.count;
	*out = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT WINAPI list_maximum(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = list_of(iface)->most;
	return S_OK;
}

static HRESULT WINAPI recent_removed_add(void *iface, void *handler, INT64 *token) { return handlers_add(&list_of(iface)->removed, handler, token); }
static HRESULT WINAPI recent_removed_remove(void *iface, INT64 token) { return handlers_remove(&list_of(iface)->removed, token); }

static HRESULT WINAPI recent_add_visible(void *iface, void *item, HSTRING metadata, INT32 visibility, HSTRING *out)
{
	return list_add(iface, item, metadata, out);
}

static HRESULT WINAPI recent_replace_visible(void *iface, HSTRING token, void *item, HSTRING metadata, INT32 visibility)
{
	return list_replace(iface, token, item, metadata);
}

TABLE(access_list_table, 0, INSPECTABLE, (const void *)list_add_default, (const void *)list_add, (const void *)list_replace_default,
	(const void *)list_replace, (const void *)list_get_item, (const void *)list_get_file, (const void *)list_get_folder,
	(const void *)list_get_item_options, (const void *)list_get_file_options, (const void *)list_get_folder_options, (const void *)list_remove,
	(const void *)list_contains, (const void *)list_clear, (const void *)list_check_access, (const void *)list_entries, (const void *)list_maximum);
TABLE(recent_list_table, 1, INSPECTABLE, (const void *)recent_removed_add, (const void *)recent_removed_remove);
TABLE(recent_list2_table, 2, INSPECTABLE, (const void *)recent_add_visible, (const void *)recent_replace_visible);
static const struct face_iid future_iids[] = {{&iid_access_list, 0}, {NULL, 0}};
static const struct face_iid recent_iids[] = {{&iid_recent_list, 1}, {&iid_access_list, 0}, {&iid_recent_list2, 2}, {NULL, 0}};
static const struct kind future_kind = {"StorageItemAccessList", L"Windows.Storage.AccessCache.StorageItemAccessList", future_iids, NULL};
static const struct kind recent_kind = {"StorageItemMostRecentlyUsedList", L"Windows.Storage.AccessCache.StorageItemMostRecentlyUsedList",
	recent_iids, NULL};
static struct list future_list = {{{access_list_table.slots}, &future_kind, 1}, L"future", FUTURE_MOST};
static struct list recent_list = {{{access_list_table.slots, recent_list_table.slots, recent_list2_table.slots}, &recent_kind, 1}, L"recent",
	RECENT_MOST};

/* A list exists for an app of a package. */
static HRESULT give_list(struct list *list, size_t face, void **out)
{
	WCHAR path[MAX_PATH];
	if (!out)
		return E_POINTER;
	*out = NULL;
	const HRESULT result = package_data_path(L"AccessCache", path);
	if (FAILED(result))
		return result;
	reference(*out = face_of(&list->base, face));
	return S_OK;
}

static HRESULT WINAPI permissions_future(void *iface, void **out) { return give_list(&future_list, 0, out); }
static HRESULT WINAPI permissions_recent(void *iface, void **out) { return give_list(&recent_list, 1, out); }

TABLE(permissions_factory_table, 0, FACTORY);
TABLE(permissions_table, 1, INSPECTABLE, (const void *)permissions_future, (const void *)permissions_recent);
static const struct face_iid permissions_iids[] = {{&iid_factory, 0}, {&iid_permissions, 1}, {NULL, 0}};
static const struct kind permissions_kind = {"StorageApplicationPermissions", L"Windows.Storage.AccessCache.StorageApplicationPermissions",
	permissions_iids, NULL};
struct object storage_permissions_class = {{permissions_factory_table.slots, permissions_table.slots}, &permissions_kind, 1};
