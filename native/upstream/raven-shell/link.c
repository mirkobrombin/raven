/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven shell component: the shell link object (CLSID_ShellLink).
 *
 * A link is created in memory through IShellLinkW and written or read through IPersistFile in the binary format of
 * [MS-SHLLINK]: the header, the local base path in a link information block, and the string data in UTF-16. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shobjidl.h>
#include <stddef.h>
#include <string.h>

#define LINK_HAS_INFO 0x2
#define LINK_HAS_NAME 0x4
#define LINK_HAS_RELATIVE_PATH 0x8
#define LINK_HAS_WORKING_DIR 0x10
#define LINK_HAS_ARGUMENTS 0x20
#define LINK_HAS_ICON_LOCATION 0x40
#define LINK_UNICODE 0x80
#define LINK_INFO_LOCAL 0x1

#define TEXT_MAX 1024

typedef struct {
	const IShellLinkWVtbl *link_vtbl;
	const IPersistFileVtbl *file_vtbl;
	LONG refs;
	WCHAR path[MAX_PATH], relative[MAX_PATH], directory[MAX_PATH], icon[MAX_PATH], current[MAX_PATH];
	WCHAR arguments[TEXT_MAX], description[TEXT_MAX];
	int icon_index;
	int show;
	WORD hotkey;
	BOOL dirty;
} LinkObject;

static const GUID link_class = {0x00021401, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

static BOOL same_guid(const GUID *a, const GUID *b)
{
	const BYTE *left = (const BYTE *)a, *right = (const BYTE *)b;
	size_t index;
	for (index = 0; index < sizeof(GUID); ++index)
		if (left[index] != right[index])
			return FALSE;
	return TRUE;
}

static void set_text(WCHAR *to, size_t capacity, const WCHAR *from)
{
	size_t length = from ? lstrlenW(from) : 0;
	if (length >= capacity)
		length = capacity - 1;
	if (length)
		memcpy(to, from, length * sizeof(WCHAR));
	to[length] = 0;
}

static HRESULT get_text(const WCHAR *from, WCHAR *to, int capacity)
{
	size_t length = lstrlenW(from);
	if (!to || capacity <= 0)
		return E_INVALIDARG;
	if (length >= (size_t)capacity) {
		memcpy(to, from, (capacity - 1) * sizeof(WCHAR));
		to[capacity - 1] = 0;
		return S_FALSE;
	}
	memcpy(to, from, (length + 1) * sizeof(WCHAR));
	return S_OK;
}

#define LINK_OF_LINK(self) ((LinkObject *)(self))
#define LINK_OF_FILE(self) ((LinkObject *)((char *)(self) - offsetof(LinkObject, file_vtbl)))

/* IUnknown -------------------------------------------------------------------------------------------------- */

static HRESULT link_query(LinkObject *link, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IShellLinkW)) {
		*out = &link->link_vtbl;
	} else if (same_guid(iid, &IID_IPersistFile) || same_guid(iid, &IID_IPersist)) {
		*out = &link->file_vtbl;
	} else {
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&link->refs);
	return S_OK;
}

static ULONG link_add_ref(LinkObject *link)
{
	return (ULONG)InterlockedIncrement(&link->refs);
}

static ULONG link_release(LinkObject *link)
{
	LONG left = InterlockedDecrement(&link->refs);
	if (!left)
		HeapFree(GetProcessHeap(), 0, link);
	return (ULONG)left;
}

static HRESULT WINAPI sl_query(IShellLinkW *self, REFIID iid, void **out) { return link_query(LINK_OF_LINK(self), iid, out); }
static ULONG WINAPI sl_add_ref(IShellLinkW *self) { return link_add_ref(LINK_OF_LINK(self)); }
static ULONG WINAPI sl_release(IShellLinkW *self) { return link_release(LINK_OF_LINK(self)); }

/* IShellLinkW ----------------------------------------------------------------------------------------------- */

static HRESULT WINAPI sl_get_path(IShellLinkW *self, LPWSTR file, int capacity, WIN32_FIND_DATAW *data, DWORD flags)
{
	LinkObject *link = LINK_OF_LINK(self);
	if (data) {
		memset(data, 0, sizeof(*data));
		data->dwFileAttributes = GetFileAttributesW(link->path);
		if (data->dwFileAttributes == INVALID_FILE_ATTRIBUTES)
			data->dwFileAttributes = 0;
		set_text(data->cFileName, MAX_PATH, link->path);
	}
	return get_text(link->path, file, capacity) == E_INVALIDARG ? E_INVALIDARG : S_OK;
}

static HRESULT WINAPI sl_get_id_list(IShellLinkW *self, PIDLIST_ABSOLUTE *list)
{
	if (list)
		*list = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI sl_set_id_list(IShellLinkW *self, PCIDLIST_ABSOLUTE list) { return E_NOTIMPL; }

static HRESULT WINAPI sl_get_description(IShellLinkW *self, LPWSTR text, int capacity)
{
	return get_text(LINK_OF_LINK(self)->description, text, capacity) == E_INVALIDARG ? E_INVALIDARG : S_OK;
}

static HRESULT WINAPI sl_set_description(IShellLinkW *self, LPCWSTR text)
{
	LinkObject *link = LINK_OF_LINK(self);
	set_text(link->description, TEXT_MAX, text);
	link->dirty = TRUE;
	return S_OK;
}

static HRESULT WINAPI sl_get_directory(IShellLinkW *self, LPWSTR text, int capacity)
{
	return get_text(LINK_OF_LINK(self)->directory, text, capacity) == E_INVALIDARG ? E_INVALIDARG : S_OK;
}

static HRESULT WINAPI sl_set_directory(IShellLinkW *self, LPCWSTR text)
{
	LinkObject *link = LINK_OF_LINK(self);
	set_text(link->directory, MAX_PATH, text);
	link->dirty = TRUE;
	return S_OK;
}

static HRESULT WINAPI sl_get_arguments(IShellLinkW *self, LPWSTR text, int capacity)
{
	return get_text(LINK_OF_LINK(self)->arguments, text, capacity) == E_INVALIDARG ? E_INVALIDARG : S_OK;
}

static HRESULT WINAPI sl_set_arguments(IShellLinkW *self, LPCWSTR text)
{
	LinkObject *link = LINK_OF_LINK(self);
	set_text(link->arguments, TEXT_MAX, text);
	link->dirty = TRUE;
	return S_OK;
}

static HRESULT WINAPI sl_get_hotkey(IShellLinkW *self, WORD *key)
{
	if (!key)
		return E_POINTER;
	*key = LINK_OF_LINK(self)->hotkey;
	return S_OK;
}

static HRESULT WINAPI sl_set_hotkey(IShellLinkW *self, WORD key)
{
	LinkObject *link = LINK_OF_LINK(self);
	link->hotkey = key;
	link->dirty = TRUE;
	return S_OK;
}

static HRESULT WINAPI sl_get_show(IShellLinkW *self, int *show)
{
	if (!show)
		return E_POINTER;
	*show = LINK_OF_LINK(self)->show;
	return S_OK;
}

static HRESULT WINAPI sl_set_show(IShellLinkW *self, int show)
{
	LinkObject *link = LINK_OF_LINK(self);
	link->show = show;
	link->dirty = TRUE;
	return S_OK;
}

static HRESULT WINAPI sl_get_icon(IShellLinkW *self, LPWSTR text, int capacity, int *index)
{
	LinkObject *link = LINK_OF_LINK(self);
	if (index)
		*index = link->icon_index;
	return get_text(link->icon, text, capacity) == E_INVALIDARG ? E_INVALIDARG : S_OK;
}

static HRESULT WINAPI sl_set_icon(IShellLinkW *self, LPCWSTR text, int index)
{
	LinkObject *link = LINK_OF_LINK(self);
	set_text(link->icon, MAX_PATH, text);
	link->icon_index = index;
	link->dirty = TRUE;
	return S_OK;
}

static HRESULT WINAPI sl_set_relative(IShellLinkW *self, LPCWSTR text, DWORD reserved)
{
	LinkObject *link = LINK_OF_LINK(self);
	set_text(link->relative, MAX_PATH, text);
	link->dirty = TRUE;
	return S_OK;
}

static HRESULT WINAPI sl_resolve(IShellLinkW *self, HWND window, DWORD flags)
{
	LinkObject *link = LINK_OF_LINK(self);
	if (!link->path[0])
		return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
	return GetFileAttributesW(link->path) == INVALID_FILE_ATTRIBUTES ? HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) : S_OK;
}

static HRESULT WINAPI sl_set_path(IShellLinkW *self, LPCWSTR text)
{
	LinkObject *link = LINK_OF_LINK(self);
	if (!text)
		return E_INVALIDARG;
	set_text(link->path, MAX_PATH, text);
	link->dirty = TRUE;
	return S_OK;
}

static const IShellLinkWVtbl link_vtbl = {
	sl_query, sl_add_ref, sl_release, sl_get_path, sl_get_id_list, sl_set_id_list, sl_get_description, sl_set_description,
	sl_get_directory, sl_set_directory, sl_get_arguments, sl_set_arguments, sl_get_hotkey, sl_set_hotkey, sl_get_show, sl_set_show,
	sl_get_icon, sl_set_icon, sl_set_relative, sl_resolve, sl_set_path,
};

/* The file format ------------------------------------------------------------------------------------------- */

typedef struct {
	BYTE *data;
	DWORD size, capacity;
	BOOL failed;
} Buffer;

static void put_bytes(Buffer *buffer, const void *bytes, DWORD count)
{
	if (buffer->failed)
		return;
	if (buffer->size + count > buffer->capacity) {
		DWORD capacity = buffer->capacity ? buffer->capacity * 2 : 1024;
		BYTE *grown;
		while (capacity < buffer->size + count)
			capacity *= 2;
		grown = buffer->data ? HeapReAlloc(GetProcessHeap(), 0, buffer->data, capacity) : HeapAlloc(GetProcessHeap(), 0, capacity);
		if (!grown) {
			buffer->failed = TRUE;
			return;
		}
		buffer->data = grown;
		buffer->capacity = capacity;
	}
	memcpy(buffer->data + buffer->size, bytes, count);
	buffer->size += count;
}

static void put_dword(Buffer *buffer, DWORD value) { put_bytes(buffer, &value, 4); }
static void put_word(Buffer *buffer, WORD value) { put_bytes(buffer, &value, 2); }

static void put_string(Buffer *buffer, const WCHAR *text)
{
	WORD length = (WORD)lstrlenW(text);
	put_word(buffer, length);
	put_bytes(buffer, text, length * sizeof(WCHAR));
}

/* The link information block for a local target: the volume, then the base path in the system code page. */
static void put_link_info(Buffer *buffer, const WCHAR *path)
{
	char ansi[MAX_PATH * 2];
	int length = WideCharToMultiByte(CP_ACP, 0, path, -1, ansi, sizeof(ansi), NULL, NULL);
	DWORD volume_size = 0x11, header = 0x1c, volume_at, base_at, suffix_at, total;
	DWORD type = GetDriveTypeW(NULL) == DRIVE_REMOVABLE ? DRIVE_REMOVABLE : DRIVE_FIXED;
	if (length <= 0)
		ansi[0] = 0, length = 1;
	volume_at = header;
	base_at = volume_at + volume_size;
	suffix_at = base_at + (DWORD)length;
	total = suffix_at + 1;
	put_dword(buffer, total);
	put_dword(buffer, header);
	put_dword(buffer, LINK_INFO_LOCAL);
	put_dword(buffer, volume_at);
	put_dword(buffer, base_at);
	put_dword(buffer, 0);
	put_dword(buffer, suffix_at);
	put_dword(buffer, volume_size);
	put_dword(buffer, type);
	put_dword(buffer, 0);
	put_dword(buffer, 0x10);
	put_bytes(buffer, "\0", 1);
	put_bytes(buffer, ansi, (DWORD)length);
	put_bytes(buffer, "\0", 1);
}

static HRESULT save_link(LinkObject *link, const WCHAR *file)
{
	Buffer buffer = {0};
	DWORD flags = LINK_UNICODE, attributes;
	HANDLE handle;
	DWORD written;
	BOOL ok;
	static const BYTE terminal[4] = {0, 0, 0, 0};
	if (link->path[0])
		flags |= LINK_HAS_INFO;
	if (link->description[0])
		flags |= LINK_HAS_NAME;
	if (link->relative[0])
		flags |= LINK_HAS_RELATIVE_PATH;
	if (link->directory[0])
		flags |= LINK_HAS_WORKING_DIR;
	if (link->arguments[0])
		flags |= LINK_HAS_ARGUMENTS;
	if (link->icon[0])
		flags |= LINK_HAS_ICON_LOCATION;
	attributes = link->path[0] ? GetFileAttributesW(link->path) : 0;
	if (attributes == INVALID_FILE_ATTRIBUTES)
		attributes = 0;
	put_dword(&buffer, 0x4c);
	put_bytes(&buffer, &link_class, sizeof(GUID));
	put_dword(&buffer, flags);
	put_dword(&buffer, attributes);
	put_dword(&buffer, 0); put_dword(&buffer, 0); /* creation */
	put_dword(&buffer, 0); put_dword(&buffer, 0); /* access */
	put_dword(&buffer, 0); put_dword(&buffer, 0); /* write */
	put_dword(&buffer, 0); /* size of the target */
	put_dword(&buffer, (DWORD)link->icon_index);
	put_dword(&buffer, (DWORD)(link->show ? link->show : SW_SHOWNORMAL));
	put_word(&buffer, link->hotkey);
	put_word(&buffer, 0);
	put_dword(&buffer, 0);
	put_dword(&buffer, 0);
	if (flags & LINK_HAS_INFO)
		put_link_info(&buffer, link->path);
	if (flags & LINK_HAS_NAME)
		put_string(&buffer, link->description);
	if (flags & LINK_HAS_RELATIVE_PATH)
		put_string(&buffer, link->relative);
	if (flags & LINK_HAS_WORKING_DIR)
		put_string(&buffer, link->directory);
	if (flags & LINK_HAS_ARGUMENTS)
		put_string(&buffer, link->arguments);
	if (flags & LINK_HAS_ICON_LOCATION)
		put_string(&buffer, link->icon);
	put_bytes(&buffer, terminal, 4);
	if (buffer.failed) {
		if (buffer.data)
			HeapFree(GetProcessHeap(), 0, buffer.data);
		return E_OUTOFMEMORY;
	}
	handle = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		HeapFree(GetProcessHeap(), 0, buffer.data);
		return HRESULT_FROM_WIN32(GetLastError());
	}
	ok = WriteFile(handle, buffer.data, buffer.size, &written, NULL) && written == buffer.size;
	CloseHandle(handle);
	HeapFree(GetProcessHeap(), 0, buffer.data);
	if (!ok)
		return HRESULT_FROM_WIN32(GetLastError());
	set_text(link->current, MAX_PATH, file);
	link->dirty = FALSE;
	return S_OK;
}

typedef struct {
	const BYTE *data;
	DWORD size, at;
	BOOL failed;
} Reader;

static DWORD take_dword(Reader *reader)
{
	DWORD value = 0;
	if (reader->at + 4 > reader->size) {
		reader->failed = TRUE;
		return 0;
	}
	memcpy(&value, reader->data + reader->at, 4);
	reader->at += 4;
	return value;
}

static void take_string(Reader *reader, WCHAR *to, size_t capacity, BOOL unicode)
{
	WORD count;
	DWORD bytes;
	if (reader->at + 2 > reader->size) {
		reader->failed = TRUE;
		return;
	}
	memcpy(&count, reader->data + reader->at, 2);
	reader->at += 2;
	bytes = unicode ? (DWORD)count * 2 : count;
	if (reader->at + bytes > reader->size) {
		reader->failed = TRUE;
		return;
	}
	if (unicode) {
		size_t keep = count < capacity - 1 ? count : capacity - 1;
		memcpy(to, reader->data + reader->at, keep * sizeof(WCHAR));
		to[keep] = 0;
	} else {
		int length = MultiByteToWideChar(CP_ACP, 0, (const char *)reader->data + reader->at, count, to, (int)capacity - 1);
		to[length > 0 ? length : 0] = 0;
	}
	reader->at += bytes;
}

static HRESULT load_link(LinkObject *link, const WCHAR *file)
{
	HANDLE handle = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD size, read = 0;
	BYTE *bytes;
	Reader reader;
	DWORD flags, info_size, header_size, info_flags, base_at, suffix_at;
	BOOL unicode;
	if (handle == INVALID_HANDLE_VALUE)
		return HRESULT_FROM_WIN32(GetLastError());
	size = GetFileSize(handle, NULL);
	if (size < 0x4c || size > 1 << 20) {
		CloseHandle(handle);
		return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
	}
	bytes = HeapAlloc(GetProcessHeap(), 0, size);
	if (!bytes || !ReadFile(handle, bytes, size, &read, NULL) || read != size) {
		CloseHandle(handle);
		if (bytes)
			HeapFree(GetProcessHeap(), 0, bytes);
		return bytes ? HRESULT_FROM_WIN32(GetLastError()) : E_OUTOFMEMORY;
	}
	CloseHandle(handle);
	reader.data = bytes;
	reader.size = size;
	reader.at = 0;
	reader.failed = FALSE;
	if (take_dword(&reader) != 0x4c || !same_guid((const GUID *)(bytes + 4), &link_class)) {
		HeapFree(GetProcessHeap(), 0, bytes);
		return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
	}
	memcpy(&flags, bytes + 20, 4);
	memcpy(&link->icon_index, bytes + 56, 4);
	memcpy(&link->show, bytes + 60, 4);
	memcpy(&link->hotkey, bytes + 64, 2);
	reader.at = 0x4c;
	unicode = (flags & LINK_UNICODE) != 0;
	link->path[0] = link->relative[0] = link->directory[0] = link->icon[0] = link->arguments[0] = link->description[0] = 0;
	if (flags & 0x1) {
		WORD id_size = 0;
		if (reader.at + 2 <= size)
			memcpy(&id_size, bytes + reader.at, 2);
		reader.at += 2 + id_size;
	}
	if (flags & LINK_HAS_INFO) {
		DWORD start = reader.at;
		info_size = take_dword(&reader);
		header_size = take_dword(&reader);
		info_flags = take_dword(&reader);
		take_dword(&reader);
		base_at = take_dword(&reader);
		take_dword(&reader);
		suffix_at = take_dword(&reader);
		(void)header_size;
		if (!reader.failed && (info_flags & LINK_INFO_LOCAL) && start + base_at < size) {
			char combined[MAX_PATH * 2];
			const char *base = (const char *)bytes + start + base_at;
			const char *suffix = suffix_at && start + suffix_at < size ? (const char *)bytes + start + suffix_at : "";
			size_t base_length = 0, suffix_length = 0, limit = size - (start + base_at);
			while (base_length < limit && base[base_length])
				++base_length;
			while (suffix[suffix_length] && start + suffix_at + suffix_length < size)
				++suffix_length;
			if (base_length + suffix_length < sizeof(combined) - 1) {
				memcpy(combined, base, base_length);
				memcpy(combined + base_length, suffix, suffix_length);
				combined[base_length + suffix_length] = 0;
				MultiByteToWideChar(CP_ACP, 0, combined, -1, link->path, MAX_PATH);
			}
		}
		reader.at = start + info_size;
	}
	if (flags & LINK_HAS_NAME)
		take_string(&reader, link->description, TEXT_MAX, unicode);
	if (flags & LINK_HAS_RELATIVE_PATH)
		take_string(&reader, link->relative, MAX_PATH, unicode);
	if (flags & LINK_HAS_WORKING_DIR)
		take_string(&reader, link->directory, MAX_PATH, unicode);
	if (flags & LINK_HAS_ARGUMENTS)
		take_string(&reader, link->arguments, TEXT_MAX, unicode);
	if (flags & LINK_HAS_ICON_LOCATION)
		take_string(&reader, link->icon, MAX_PATH, unicode);
	HeapFree(GetProcessHeap(), 0, bytes);
	if (reader.failed)
		return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
	set_text(link->current, MAX_PATH, file);
	link->dirty = FALSE;
	return S_OK;
}

/* IPersistFile ---------------------------------------------------------------------------------------------- */

static HRESULT WINAPI pf_query(IPersistFile *self, REFIID iid, void **out) { return link_query(LINK_OF_FILE(self), iid, out); }
static ULONG WINAPI pf_add_ref(IPersistFile *self) { return link_add_ref(LINK_OF_FILE(self)); }
static ULONG WINAPI pf_release(IPersistFile *self) { return link_release(LINK_OF_FILE(self)); }

static HRESULT WINAPI pf_class_id(IPersistFile *self, CLSID *id)
{
	if (!id)
		return E_POINTER;
	*id = link_class;
	return S_OK;
}

static HRESULT WINAPI pf_is_dirty(IPersistFile *self) { return LINK_OF_FILE(self)->dirty ? S_OK : S_FALSE; }

static HRESULT WINAPI pf_load(IPersistFile *self, LPCOLESTR file, DWORD mode)
{
	if (!file)
		return E_INVALIDARG;
	return load_link(LINK_OF_FILE(self), file);
}

static HRESULT WINAPI pf_save(IPersistFile *self, LPCOLESTR file, BOOL remember)
{
	LinkObject *link = LINK_OF_FILE(self);
	const WCHAR *target = file ? file : link->current;
	HRESULT result;
	if (!target || !*target)
		return E_INVALIDARG;
	result = save_link(link, target);
	if (SUCCEEDED(result) && !remember && file)
		link->current[0] = 0;
	return result;
}

static HRESULT WINAPI pf_save_completed(IPersistFile *self, LPCOLESTR file) { return S_OK; }

static HRESULT WINAPI pf_current_file(IPersistFile *self, LPOLESTR *file)
{
	LinkObject *link = LINK_OF_FILE(self);
	size_t bytes;
	WCHAR *copy;
	if (!file)
		return E_POINTER;
	*file = NULL;
	if (!link->current[0])
		return S_FALSE;
	bytes = (lstrlenW(link->current) + 1) * sizeof(WCHAR);
	copy = CoTaskMemAlloc(bytes);
	if (!copy)
		return E_OUTOFMEMORY;
	memcpy(copy, link->current, bytes);
	*file = copy;
	return S_OK;
}

static const IPersistFileVtbl file_vtbl = {
	pf_query, pf_add_ref, pf_release, pf_class_id, pf_is_dirty, pf_load, pf_save, pf_save_completed, pf_current_file,
};

HRESULT raven_make_shell_link(REFIID iid, void **out)
{
	LinkObject *link = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(LinkObject));
	HRESULT result;
	if (!link)
		return E_OUTOFMEMORY;
	link->link_vtbl = &link_vtbl;
	link->file_vtbl = &file_vtbl;
	link->refs = 1;
	link->show = SW_SHOWNORMAL;
	result = link_query(link, iid, out);
	link_release(link);
	return result;
}
