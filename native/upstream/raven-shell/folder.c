/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven shell component: item id lists and the shell's file system namespace.
 *
 * An item id is opaque to programs, which only walk its size field. Ours holds the two bytes 'R' 'V' and then
 * text without a terminator: the absolute path for the first item of a list made at the desktop, a name below
 * the folder for the others. The host's SHGetPathFromIDListW reads the same form. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include "shell.h"

HRESULT raven_item_from_path(const WCHAR *path, REFIID iid, void **out);

/* Item id lists ---------------------------------------------------------------------------------------------- */

UINT WINAPI ILGetSize(PCUIDLIST_RELATIVE list)
{
	UINT size = 0;
	if (!list)
		return 0;
	for (const BYTE *at = (const BYTE *)list; ((const SHITEMID *)at)->cb; at += ((const SHITEMID *)at)->cb)
		size += ((const SHITEMID *)at)->cb;
	return size + sizeof(USHORT);
}

PUIDLIST_RELATIVE WINAPI ILGetNext(PCUIDLIST_RELATIVE list)
{
	if (!list || !list->mkid.cb)
		return NULL;
	PUIDLIST_RELATIVE next = (PUIDLIST_RELATIVE)((const BYTE *)list + list->mkid.cb);
	return next->mkid.cb ? next : NULL;
}

PUITEMID_CHILD WINAPI ILFindLastID(PCUIDLIST_RELATIVE list)
{
	if (!list)
		return NULL;
	PCUIDLIST_RELATIVE last = list;
	for (PCUIDLIST_RELATIVE next = ILGetNext(last); next; next = ILGetNext(last))
		last = next;
	return (PUITEMID_CHILD)last;
}

static ITEMIDLIST *make_list(UINT size)
{
	return CoTaskMemAlloc(size);
}

PIDLIST_RELATIVE WINAPI ILClone(PCUIDLIST_RELATIVE list)
{
	if (!list)
		return NULL;
	UINT size = ILGetSize(list);
	ITEMIDLIST *copy = make_list(size);
	if (copy)
		CopyMemory(copy, list, size);
	return (PIDLIST_RELATIVE)copy;
}

PITEMID_CHILD WINAPI ILCloneFirst(PCUIDLIST_RELATIVE list)
{
	if (!list)
		return NULL;
	UINT size = list->mkid.cb;
	ITEMIDLIST *copy = make_list(size + sizeof(USHORT));
	if (!copy)
		return NULL;
	CopyMemory(copy, list, size);
	*(USHORT *)((BYTE *)copy + size) = 0;
	return (PITEMID_CHILD)copy;
}

PIDLIST_ABSOLUTE WINAPI ILCombine(PCIDLIST_ABSOLUTE first, PCUIDLIST_RELATIVE second)
{
	if (!first)
		return (PIDLIST_ABSOLUTE)ILClone(second);
	if (!second)
		return (PIDLIST_ABSOLUTE)ILClone((PCUIDLIST_RELATIVE)first);
	UINT head = ILGetSize((PCUIDLIST_RELATIVE)first) - sizeof(USHORT), tail = ILGetSize(second);
	ITEMIDLIST *joined = make_list(head + tail);
	if (!joined)
		return NULL;
	CopyMemory(joined, first, head);
	CopyMemory((BYTE *)joined + head, second, tail);
	return (PIDLIST_ABSOLUTE)joined;
}

void WINAPI ILFree(PIDLIST_RELATIVE list)
{
	CoTaskMemFree(list);
}

static PIDLIST_RELATIVE list_of_text(const WCHAR *text)
{
	UINT length = lstrlenW(text) * sizeof(WCHAR), item = 4 + length;
	if (item > 0xfffd)
		return NULL;
	BYTE *bytes = (BYTE *)make_list(item + sizeof(USHORT));
	if (!bytes)
		return NULL;
	*(USHORT *)bytes = (USHORT)item;
	bytes[2] = 'R';
	bytes[3] = 'V';
	CopyMemory(bytes + 4, text, length);
	*(USHORT *)(bytes + item) = 0;
	return (PIDLIST_RELATIVE)bytes;
}

/* Appends the text of every item of the list to a path. */
static BOOL path_of_list(const WCHAR *base, PCUIDLIST_RELATIVE list, WCHAR *out, UINT capacity)
{
	copy_text(out, capacity, base);
	for (PCUIDLIST_RELATIVE at = list; at && at->mkid.cb; at = (PCUIDLIST_RELATIVE)((const BYTE *)at + at->mkid.cb)) {
		const BYTE *bytes = (const BYTE *)at;
		UINT size = at->mkid.cb;
		if (size < 4 || bytes[2] != 'R' || bytes[3] != 'V')
			return FALSE;
		UINT characters = (size - 4) / sizeof(WCHAR);
		WCHAR name[MAX_PATH];
		if (characters >= MAX_PATH)
			return FALSE;
		CopyMemory(name, bytes + 4, characters * sizeof(WCHAR));
		name[characters] = 0;
		/* The desktop's own items carry a whole path. */
		if (out[0] && !is_absolute(name))
			join_path(out, capacity, out, name);
		else
			copy_text(out, capacity, name);
	}
	return TRUE;
}

PIDLIST_ABSOLUTE WINAPI ILCreateFromPathW(PCWSTR path)
{
	return path ? (PIDLIST_ABSOLUTE)list_of_text(path) : NULL;
}

BOOL WINAPI ILIsEqual(PCIDLIST_ABSOLUTE first, PCIDLIST_ABSOLUTE second)
{
	WCHAR left[MAX_PATH], right[MAX_PATH];
	if (!first || !second)
		return first == second;
	if (!path_of_list(L"", (PCUIDLIST_RELATIVE)first, left, MAX_PATH) || !path_of_list(L"", (PCUIDLIST_RELATIVE)second, right, MAX_PATH))
		return FALSE;
	return !lstrcmpiW(left, right);
}

/* Enumeration ------------------------------------------------------------------------------------------------ */

typedef struct {
	IEnumIDList face;
	LONG references;
	HANDLE search;
	WIN32_FIND_DATAW found;
	BOOL pending;
	SHCONTF kinds;
	BOOL absolute; /* the desktop's items carry their whole path */
	WCHAR directory[MAX_PATH];
} folder_enum;

static BOOL enum_wanted(const folder_enum *self)
{
	const WIN32_FIND_DATAW *found = &self->found;
	if (!lstrcmpW(found->cFileName, L".") || !lstrcmpW(found->cFileName, L".."))
		return FALSE;
	if ((found->dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) && !(self->kinds & SHCONTF_INCLUDEHIDDEN))
		return FALSE;
	return self->kinds & (found->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? SHCONTF_FOLDERS : SHCONTF_NONFOLDERS) ? TRUE : FALSE;
}

static HRESULT WINAPI enum_query(IEnumIDList *face, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IEnumIDList)) {
		*out = face;
		IEnumIDList_AddRef(face);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI enum_reference(IEnumIDList *face)
{
	return InterlockedIncrement(&((folder_enum *)face)->references);
}

static ULONG WINAPI enum_release(IEnumIDList *face)
{
	folder_enum *self = (folder_enum *)face;
	LONG left = InterlockedDecrement(&self->references);
	if (!left) {
		if (self->search != INVALID_HANDLE_VALUE)
			FindClose(self->search);
		HeapFree(GetProcessHeap(), 0, self);
	}
	return left;
}

static void enum_advance(folder_enum *self)
{
	self->pending = self->search != INVALID_HANDLE_VALUE && FindNextFileW(self->search, &self->found);
}

static HRESULT WINAPI enum_next(IEnumIDList *face, ULONG wanted, PITEMID_CHILD *items, ULONG *fetched)
{
	folder_enum *self = (folder_enum *)face;
	ULONG count = 0;
	if (!items || (wanted > 1 && !fetched))
		return E_POINTER;
	while (count < wanted && self->pending) {
		if (enum_wanted(self)) {
			WCHAR text[MAX_PATH];
			if (self->absolute)
				join_path(text, MAX_PATH, self->directory, self->found.cFileName);
			else
				copy_text(text, MAX_PATH, self->found.cFileName);
			items[count] = (PITEMID_CHILD)list_of_text(text);
			if (!items[count])
				break;
			count++;
		}
		enum_advance(self);
	}
	if (fetched)
		*fetched = count;
	return count == wanted ? S_OK : S_FALSE;
}

static HRESULT WINAPI enum_skip(IEnumIDList *face, ULONG wanted)
{
	folder_enum *self = (folder_enum *)face;
	while (wanted && self->pending) {
		if (enum_wanted(self))
			wanted--;
		enum_advance(self);
	}
	return wanted ? S_FALSE : S_OK;
}

static void enum_open(folder_enum *self)
{
	WCHAR pattern[MAX_PATH];
	if (self->search != INVALID_HANDLE_VALUE)
		FindClose(self->search);
	join_path(pattern, MAX_PATH, self->directory, L"*");
	self->search = FindFirstFileW(pattern, &self->found);
	self->pending = self->search != INVALID_HANDLE_VALUE;
}

static HRESULT WINAPI enum_reset(IEnumIDList *face)
{
	enum_open((folder_enum *)face);
	return S_OK;
}

static HRESULT WINAPI enum_clone(IEnumIDList *face, IEnumIDList **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static const IEnumIDListVtbl enum_table = {enum_query, enum_reference, enum_release, enum_next, enum_skip, enum_reset, enum_clone};

/* Folders ---------------------------------------------------------------------------------------------------- */

typedef struct {
	IShellFolder face;
	LONG references;
	WCHAR path[MAX_PATH]; /* empty at the desktop, whose items name whole paths */
} shell_folder;

static HRESULT make_folder(const WCHAR *path, REFIID iid, void **out);

/* The directory that holds the desktop's own files. */
static void desktop_directory(WCHAR *out)
{
	static HRESULT (WINAPI *folder_path)(HWND, int, HANDLE, DWORD, LPWSTR);
	out[0] = 0;
	if (!folder_path) {
		HMODULE shell = LoadLibraryW(L"shell32.dll");
		folder_path = shell ? (void *)GetProcAddress(shell, "SHGetFolderPathW") : NULL;
	}
	if (folder_path)
		folder_path(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, out);
}

static HRESULT WINAPI folder_query(IShellFolder *face, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IShellFolder)) {
		*out = face;
		IShellFolder_AddRef(face);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI folder_reference(IShellFolder *face)
{
	return InterlockedIncrement(&((shell_folder *)face)->references);
}

static ULONG WINAPI folder_release(IShellFolder *face)
{
	shell_folder *self = (shell_folder *)face;
	LONG left = InterlockedDecrement(&self->references);
	if (!left)
		HeapFree(GetProcessHeap(), 0, self);
	return left;
}

static ULONG attributes_of_path(const WCHAR *path, ULONG wanted)
{
	DWORD found = GetFileAttributesW(path);
	ULONG result = SFGAO_CANCOPY | SFGAO_CANMOVE | SFGAO_CANRENAME | SFGAO_CANDELETE | SFGAO_HASPROPSHEET | SFGAO_FILESYSTEM;
	if (found == INVALID_FILE_ATTRIBUTES)
		return 0;
	if (found & FILE_ATTRIBUTE_DIRECTORY)
		result |= SFGAO_FOLDER | SFGAO_FILESYSANCESTOR | SFGAO_STORAGEANCESTOR | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE;
	else
		result |= SFGAO_STREAM;
	if (found & FILE_ATTRIBUTE_READONLY)
		result |= SFGAO_READONLY;
	if (found & FILE_ATTRIBUTE_HIDDEN)
		result |= SFGAO_HIDDEN;
	return result & wanted;
}

static HRESULT WINAPI folder_parse(IShellFolder *face, HWND window, IBindCtx *context, LPWSTR name, ULONG *eaten, PIDLIST_RELATIVE *out, ULONG *attributes)
{
	shell_folder *self = (shell_folder *)face;
	WCHAR full[MAX_PATH], base[MAX_PATH];
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!name)
		return E_INVALIDARG;
	if (self->path[0])
		copy_text(base, MAX_PATH, self->path);
	else
		desktop_directory(base);
	if (is_absolute(name))
		copy_text(full, MAX_PATH, name);
	else
		join_path(full, MAX_PATH, base, name);
	if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES)
		return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
	*out = list_of_text(self->path[0] && !is_absolute(name) ? name : full);
	if (!*out)
		return E_OUTOFMEMORY;
	if (eaten)
		*eaten = lstrlenW(name);
	if (attributes)
		*attributes = attributes_of_path(full, *attributes);
	return S_OK;
}

static HRESULT WINAPI folder_enumerate(IShellFolder *face, HWND window, SHCONTF kinds, IEnumIDList **out)
{
	shell_folder *self = (shell_folder *)face;
	if (!out)
		return E_INVALIDARG;
	folder_enum *walk = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*walk));
	if (!walk) {
		*out = NULL;
		return E_OUTOFMEMORY;
	}
	walk->face.lpVtbl = (IEnumIDListVtbl *)&enum_table;
	walk->references = 1;
	walk->search = INVALID_HANDLE_VALUE;
	walk->kinds = kinds;
	walk->absolute = !self->path[0];
	if (self->path[0])
		copy_text(walk->directory, MAX_PATH, self->path);
	else
		desktop_directory(walk->directory);
	enum_open(walk);
	*out = &walk->face;
	return S_OK;
}

static HRESULT WINAPI folder_bind(IShellFolder *face, PCUIDLIST_RELATIVE list, IBindCtx *context, REFIID iid, void **out)
{
	shell_folder *self = (shell_folder *)face;
	WCHAR full[MAX_PATH];
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!list)
		return E_INVALIDARG;
	if (!path_of_list(self->path, list, full, MAX_PATH))
		return E_INVALIDARG;
	if (IsEqualIID(iid, &IID_IShellItem))
		return raven_item_from_path(full, iid, out);
	DWORD found = GetFileAttributesW(full);
	if (found == INVALID_FILE_ATTRIBUTES)
		return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
	if (!(found & FILE_ATTRIBUTE_DIRECTORY))
		return E_NOINTERFACE;
	return make_folder(full, iid, out);
}

static HRESULT WINAPI folder_bind_storage(IShellFolder *face, PCUIDLIST_RELATIVE list, IBindCtx *context, REFIID iid, void **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT WINAPI folder_compare(IShellFolder *face, LPARAM column, PCUIDLIST_RELATIVE first, PCUIDLIST_RELATIVE second)
{
	WCHAR left[MAX_PATH], right[MAX_PATH];
	if (!path_of_list(L"", first, left, MAX_PATH) || !path_of_list(L"", second, right, MAX_PATH))
		return E_INVALIDARG;
	int order = lstrcmpiW(left, right);
	// The code of the result carries the order as a signed short.
	return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)(short)(order < 0 ? -1 : order > 0));
}

static HRESULT WINAPI folder_view(IShellFolder *face, HWND window, REFIID iid, void **out)
{
	if (out)
		*out = NULL;
	return E_NOINTERFACE;
}

static HRESULT WINAPI folder_attributes(IShellFolder *face, UINT count, PCUITEMID_CHILD_ARRAY items, SFGAOF *inout)
{
	shell_folder *self = (shell_folder *)face;
	WCHAR full[MAX_PATH];
	if (!inout || (count && !items))
		return E_INVALIDARG;
	ULONG result = *inout;
	if (!count) {
		WCHAR own[MAX_PATH];
		if (self->path[0])
			copy_text(own, MAX_PATH, self->path);
		else
			desktop_directory(own);
		result = attributes_of_path(own, result);
	}
	for (UINT index = 0; index < count; index++) {
		if (!path_of_list(self->path, (PCUIDLIST_RELATIVE)items[index], full, MAX_PATH))
			return E_INVALIDARG;
		result = attributes_of_path(full, result);
	}
	*inout = result;
	return S_OK;
}

static HRESULT WINAPI folder_ui_object(IShellFolder *face, HWND window, UINT count, PCUITEMID_CHILD_ARRAY items, REFIID iid, UINT *reserved, void **out)
{
	shell_folder *self = (shell_folder *)face;
	WCHAR full[MAX_PATH];
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (count != 1 || !items || !IsEqualIID(iid, &IID_IShellItem))
		return E_NOINTERFACE;
	if (!path_of_list(self->path, (PCUIDLIST_RELATIVE)items[0], full, MAX_PATH))
		return E_INVALIDARG;
	return raven_item_from_path(full, iid, out);
}

static HRESULT WINAPI folder_display_name(IShellFolder *face, PCUITEMID_CHILD item, SHGDNF flags, STRRET *out)
{
	shell_folder *self = (shell_folder *)face;
	WCHAR full[MAX_PATH];
	if (!out)
		return E_INVALIDARG;
	if (!path_of_list(self->path, (PCUIDLIST_RELATIVE)item, full, MAX_PATH))
		return E_INVALIDARG;
	if (!full[0])
		desktop_directory(full);
	const WCHAR *text = (flags & SHGDN_FORPARSING) && !(flags & SHGDN_INFOLDER) ? full : file_part(full);
	UINT bytes = (lstrlenW(text) + 1) * sizeof(WCHAR);
	WCHAR *copy = CoTaskMemAlloc(bytes);
	if (!copy)
		return E_OUTOFMEMORY;
	CopyMemory(copy, text, bytes);
	out->uType = STRRET_WSTR;
	out->pOleStr = copy;
	return S_OK;
}

static HRESULT WINAPI folder_rename(IShellFolder *face, HWND window, PCUITEMID_CHILD item, LPCWSTR name, SHGDNF flags, PITEMID_CHILD *out)
{
	shell_folder *self = (shell_folder *)face;
	WCHAR from[MAX_PATH], to[MAX_PATH];
	if (out)
		*out = NULL;
	if (!name || !path_of_list(self->path, (PCUIDLIST_RELATIVE)item, from, MAX_PATH))
		return E_INVALIDARG;
	copy_text(to, MAX_PATH, from);
	parent_directory(to);
	join_path(to, MAX_PATH, to, name);
	if (!MoveFileW(from, to))
		return HRESULT_FROM_WIN32(GetLastError());
	if (out) {
		*out = (PITEMID_CHILD)list_of_text(self->path[0] ? name : to);
		if (!*out)
			return E_OUTOFMEMORY;
	}
	return S_OK;
}

static const IShellFolderVtbl folder_table = {folder_query, folder_reference, folder_release, folder_parse, folder_enumerate, folder_bind,
	folder_bind_storage, folder_compare, folder_view, folder_attributes, folder_ui_object, folder_display_name, folder_rename};

static HRESULT make_folder(const WCHAR *path, REFIID iid, void **out)
{
	shell_folder *self = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->face.lpVtbl = (IShellFolderVtbl *)&folder_table;
	self->references = 1;
	copy_text(self->path, MAX_PATH, path);
	HRESULT result = IShellFolder_QueryInterface(&self->face, iid, out);
	IShellFolder_Release(&self->face);
	return result;
}

/* File properties -------------------------------------------------------------------------------------------- */

/* What the file system itself knows about a path, read when the store opens. Property handlers for file formats
 * do not exist here, so the store is read-only. */
static const GUID storage_properties = {0xb725f130, 0x47ef, 0x101a, {0xa5, 0xf1, 0x02, 0x60, 0x8c, 0x9e, 0xeb, 0xac}};
static const GUID file_name_property = {0x41cf5ae0, 0xf75a, 0x4806, {0xbd, 0x87, 0x59, 0xc7, 0xd9, 0x24, 0x8e, 0xb9}};
static const GUID path_properties = {0xe3e0584c, 0xb788, 0x4a5a, {0xbb, 0x20, 0x7f, 0x5a, 0x44, 0xc9, 0xac, 0xdd}};

enum { name_display, file_name, path_display, folder_display, file_size, date_modified, file_attributes, property_count };

typedef struct {
	IPropertyStore face;
	LONG references;
	WIN32_FILE_ATTRIBUTE_DATA data;
	WCHAR path[MAX_PATH];
	WCHAR folder[MAX_PATH];
} file_store;

static PROPERTYKEY property_key(int index)
{
	PROPERTYKEY key = {storage_properties, 0};
	switch (index) {
	case name_display: key.pid = 10; break;
	case file_name: key.fmtid = file_name_property; key.pid = 100; break;
	case path_display: key.fmtid = path_properties; key.pid = 7; break;
	case folder_display: key.fmtid = path_properties; key.pid = 6; break;
	case file_size: key.pid = 12; break;
	case date_modified: key.pid = 14; break;
	default: key.pid = 13; break;
	}
	return key;
}

static HRESULT WINAPI file_store_query(IPropertyStore *face, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IPropertyStore)) {
		*out = face;
		IPropertyStore_AddRef(face);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI file_store_reference(IPropertyStore *face)
{
	return InterlockedIncrement(&((file_store *)face)->references);
}

static ULONG WINAPI file_store_release(IPropertyStore *face)
{
	file_store *self = (file_store *)face;
	LONG left = InterlockedDecrement(&self->references);
	if (!left)
		HeapFree(GetProcessHeap(), 0, self);
	return left;
}

static HRESULT WINAPI file_store_count(IPropertyStore *face, DWORD *count)
{
	if (!count)
		return E_POINTER;
	*count = property_count;
	return S_OK;
}

static HRESULT WINAPI file_store_at(IPropertyStore *face, DWORD index, PROPERTYKEY *key)
{
	if (!key)
		return E_POINTER;
	if (index >= property_count)
		return E_INVALIDARG;
	*key = property_key(index);
	return S_OK;
}

static HRESULT text_value(PROPVARIANT *value, const WCHAR *text)
{
	UINT bytes = (lstrlenW(text) + 1) * sizeof(WCHAR);
	value->pwszVal = CoTaskMemAlloc(bytes);
	if (!value->pwszVal)
		return E_OUTOFMEMORY;
	CopyMemory(value->pwszVal, text, bytes);
	value->vt = VT_LPWSTR;
	return S_OK;
}

static HRESULT WINAPI file_store_get(IPropertyStore *face, REFPROPERTYKEY key, PROPVARIANT *value)
{
	file_store *self = (file_store *)face;
	if (!key || !value)
		return E_POINTER;
	ZeroMemory(value, sizeof(*value));
	for (int index = 0; index < property_count; index++) {
		PROPERTYKEY known = property_key(index);
		if (!IsEqualGUID(&known.fmtid, &key->fmtid) || known.pid != key->pid)
			continue;
		switch (index) {
		case name_display:
		case file_name:
			return text_value(value, file_part(self->path));
		case path_display:
			return text_value(value, self->path);
		case folder_display:
			return text_value(value, self->folder);
		case file_size:
			value->vt = VT_UI8;
			value->uhVal.LowPart = self->data.nFileSizeLow;
			value->uhVal.HighPart = self->data.nFileSizeHigh;
			return S_OK;
		case date_modified:
			value->vt = VT_FILETIME;
			value->filetime = self->data.ftLastWriteTime;
			return S_OK;
		default:
			value->vt = VT_UI4;
			value->ulVal = self->data.dwFileAttributes;
			return S_OK;
		}
	}
	return S_OK;
}

static HRESULT WINAPI file_store_set(IPropertyStore *face, REFPROPERTYKEY key, REFPROPVARIANT value)
{
	return STG_E_ACCESSDENIED;
}

static HRESULT WINAPI file_store_commit(IPropertyStore *face)
{
	return STG_E_ACCESSDENIED;
}

static const IPropertyStoreVtbl file_store_table = {file_store_query, file_store_reference, file_store_release, file_store_count, file_store_at,
	file_store_get, file_store_set, file_store_commit};

HRESULT WINAPI SHGetPropertyStoreFromParsingName(PCWSTR path, IBindCtx *context, GETPROPERTYSTOREFLAGS flags, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!path)
		return E_INVALIDARG;
	if (flags & GPS_READWRITE)
		return STG_E_ACCESSDENIED;
	file_store *self = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->face.lpVtbl = (IPropertyStoreVtbl *)&file_store_table;
	self->references = 1;
	copy_text(self->path, MAX_PATH, path);
	copy_text(self->folder, MAX_PATH, path);
	parent_directory(self->folder);
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &self->data)) {
		HRESULT failure = HRESULT_FROM_WIN32(GetLastError());
		HeapFree(GetProcessHeap(), 0, self);
		return failure;
	}
	HRESULT result = IPropertyStore_QueryInterface(&self->face, iid, out);
	IPropertyStore_Release(&self->face);
	return result;
}

/* "@library,-id" names a string resource; a text without the @ is the string itself. */
HRESULT WINAPI SHLoadIndirectString(PCWSTR source, PWSTR out, UINT capacity, void **reserved)
{
	if (!source || !out || !capacity)
		return E_INVALIDARG;
	if (source[0] != L'@') {
		copy_text(out, capacity, source);
		return S_OK;
	}
	WCHAR library[MAX_PATH];
	UINT length = 0;
	const WCHAR *at = source + 1;
	while (*at && *at != L',' && length + 1 < MAX_PATH)
		library[length++] = *at++;
	library[length] = 0;
	if (at[0] != L',' || at[1] != L'-')
		return E_INVALIDARG;
	UINT id = 0;
	for (at += 2; *at >= L'0' && *at <= L'9'; at++)
		id = id * 10 + (*at - L'0');
	HMODULE module = LoadLibraryExW(library, NULL, LOAD_LIBRARY_AS_DATAFILE);
	if (!module)
		return HRESULT_FROM_WIN32(GetLastError());
	int copied = LoadStringW(module, id, out, capacity);
	HRESULT result = copied > 0 ? S_OK : HRESULT_FROM_WIN32(ERROR_RESOURCE_NAME_NOT_FOUND);
	FreeLibrary(module);
	return result;
}

HRESULT WINAPI SHGetDesktopFolder(IShellFolder **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	return make_folder(L"", &IID_IShellFolder, (void **)out);
}
