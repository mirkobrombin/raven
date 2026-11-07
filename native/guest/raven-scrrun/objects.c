/* SPDX-License-Identifier: MIT */
#include "scrrun.h"

/* The File, Folder and Drive objects of the file system object and their collections. Each holds a full path; what it tells is read
 * from the file system when asked. */

typedef struct {
	sr_object base;
	WCHAR *path;
} path_object;

enum {
	ITEM_PATH = DISPID_VALUE, ITEM_NAME = 1, ITEM_ATTRIBUTES, ITEM_DATECREATED, ITEM_DATELASTACCESSED, ITEM_DATELASTMODIFIED, ITEM_DRIVE,
	ITEM_PARENTFOLDER, ITEM_SHORTNAME, ITEM_SHORTPATH, ITEM_SIZE, ITEM_TYPE, ITEM_COPY, ITEM_DELETE, ITEM_MOVE, ITEM_OPENASTEXTSTREAM,
	ITEM_FILES, ITEM_SUBFOLDERS, ITEM_ISROOTFOLDER, ITEM_CREATETEXTFILE,
};

static const sr_member file_members[] = {
	{ L"Path", ITEM_PATH, 0 }, { L"Name", ITEM_NAME, 0 }, { L"Attributes", ITEM_ATTRIBUTES, 0 }, { L"DateCreated", ITEM_DATECREATED, 0 },
	{ L"DateLastAccessed", ITEM_DATELASTACCESSED, 0 }, { L"DateLastModified", ITEM_DATELASTMODIFIED, 0 }, { L"Drive", ITEM_DRIVE, 0 },
	{ L"ParentFolder", ITEM_PARENTFOLDER, 0 }, { L"ShortName", ITEM_SHORTNAME, 0 }, { L"ShortPath", ITEM_SHORTPATH, 0 }, { L"Size", ITEM_SIZE, 0 },
	{ L"Type", ITEM_TYPE, 0 }, { L"Copy", ITEM_COPY, 2 }, { L"Delete", ITEM_DELETE, 1 }, { L"Move", ITEM_MOVE, 1 },
	{ L"OpenAsTextStream", ITEM_OPENASTEXTSTREAM, 2 }, { NULL },
};

static const sr_member folder_members[] = {
	{ L"Path", ITEM_PATH, 0 }, { L"Name", ITEM_NAME, 0 }, { L"Attributes", ITEM_ATTRIBUTES, 0 }, { L"DateCreated", ITEM_DATECREATED, 0 },
	{ L"DateLastAccessed", ITEM_DATELASTACCESSED, 0 }, { L"DateLastModified", ITEM_DATELASTMODIFIED, 0 }, { L"Drive", ITEM_DRIVE, 0 },
	{ L"ParentFolder", ITEM_PARENTFOLDER, 0 }, { L"ShortName", ITEM_SHORTNAME, 0 }, { L"ShortPath", ITEM_SHORTPATH, 0 }, { L"Size", ITEM_SIZE, 0 },
	{ L"Type", ITEM_TYPE, 0 }, { L"Copy", ITEM_COPY, 2 }, { L"Delete", ITEM_DELETE, 1 }, { L"Move", ITEM_MOVE, 1 }, { L"Files", ITEM_FILES, 0 },
	{ L"SubFolders", ITEM_SUBFOLDERS, 0 }, { L"IsRootFolder", ITEM_ISROOTFOLDER, 0 }, { L"CreateTextFile", ITEM_CREATETEXTFILE, 3 }, { NULL },
};

static void path_destroy(sr_object *self) { sr_free(((path_object *)self)->path); }

static BOOL attributes_data(const WCHAR *path, WIN32_FILE_ATTRIBUTE_DATA *data)
{
	return GetFileAttributesExW(path, GetFileExInfoStandard, data);
}

static WCHAR *drive_root(const WCHAR *path)
{
	WCHAR *root = sr_alloc(MAX_PATH * sizeof(WCHAR));
	if (path[0] && path[1] == L':')
		wsprintfW(root, L"%c:\\", path[0]);
	else
		lstrcpynW(root, path, MAX_PATH);
	return root;
}

/* The total size of a folder's tree. */
static ULONGLONG tree_size(const WCHAR *path)
{
	WCHAR *pattern = sr_join(path, L"*");
	WIN32_FIND_DATAW found;
	HANDLE find = FindFirstFileW(pattern, &found);
	sr_free(pattern);
	ULONGLONG total = 0;
	if (find == INVALID_HANDLE_VALUE)
		return 0;
	do {
		if (!lstrcmpW(found.cFileName, L".") || !lstrcmpW(found.cFileName, L".."))
			continue;
		if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			WCHAR *child = sr_join(path, found.cFileName);
			total += tree_size(child);
			sr_free(child);
		} else
			total += ((ULONGLONG)found.nFileSizeHigh << 32) | found.nFileSizeLow;
	} while (FindNextFileW(find, &found));
	FindClose(find);
	return total;
}

static HRESULT return_size(VARIANT *result, ULONGLONG size)
{
	/* A size that fits a Long is one; larger ones are Doubles. */
	if (size <= 0x7fffffff)
		return sr_return_long(result, (LONG)size);
	return sr_return_double(result, (double)size);
}

static HRESULT short_path(const WCHAR *path, BOOL name_only, VARIANT *result)
{
	WCHAR small[MAX_PATH];
	DWORD length = GetShortPathNameW(path, small, MAX_PATH);
	if (!length || length >= MAX_PATH)
		lstrcpynW(small, path, MAX_PATH);
	if (!name_only)
		return sr_return_text(result, small);
	WCHAR *name = sr_file_name(small);
	HRESULT hr = sr_return_text(result, name);
	sr_free(name);
	return hr;
}

static HRESULT type_name(const WCHAR *path, BOOL folder, VARIANT *result)
{
	if (folder)
		return sr_return_text(result, L"File folder");
	WCHAR *name = sr_file_name(path), *dot = NULL, text[MAX_PATH + 8];
	for (WCHAR *c = name; *c; c++)
		if (*c == L'.')
			dot = c;
	if (dot && dot[1]) {
		lstrcpynW(text, dot + 1, MAX_PATH);
		CharUpperW(text);
		lstrcatW(text, L" File");
	} else
		lstrcpyW(text, L"File");
	sr_free(name);
	return sr_return_text(result, text);
}

/* What File and Folder share. */
static HRESULT item_invoke(path_object *item, BOOL folder, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	BOOL put = sr_is_put(flags);
	HRESULT hr = S_OK;
	BSTR text = NULL;
	BOOL flag;
	LONG number;
	if (put && id != ITEM_NAME && id != ITEM_ATTRIBUTES)
		return DISP_E_MEMBERNOTFOUND;
	switch (id) {
	case ITEM_PATH:
		return sr_return_text(result, item->path);
	case ITEM_NAME: {
		WCHAR *name = sr_file_name(item->path);
		if (!put) {
			hr = sr_return_text(result, name);
			sr_free(name);
			return hr;
		}
		sr_free(name);
		if (FAILED(hr = sr_text(sr_put_value(params), &text)))
			return hr;
		WCHAR *parent = sr_parent_name(item->path), *renamed = sr_join(parent, text);
		if (MoveFileW(item->path, renamed)) {
			sr_free(item->path);
			item->path = renamed;
			renamed = NULL;
		} else
			hr = sr_last_error();
		sr_free(parent);
		sr_free(renamed);
		SysFreeString(text);
		return hr;
	}
	case ITEM_ATTRIBUTES: {
		DWORD a = GetFileAttributesW(item->path);
		if (a == INVALID_FILE_ATTRIBUTES)
			return sr_last_error();
		if (!put)
			return sr_return_long(result, (LONG)(a & 0x1ff7));
		/* Only the read-only, hidden, system and archive bits can be changed. */
		if (FAILED(hr = sr_long(sr_put_value(params), &number)))
			return hr;
		DWORD changeable = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;
		DWORD wanted = (a & ~changeable) | ((DWORD)number & changeable);
		return SetFileAttributesW(item->path, wanted & ~FILE_ATTRIBUTE_DIRECTORY) ? S_OK : sr_last_error();
	}
	case ITEM_DATECREATED:
	case ITEM_DATELASTACCESSED:
	case ITEM_DATELASTMODIFIED:
		if (!attributes_data(item->path, &data))
			return sr_last_error();
		return sr_return_date(result, id == ITEM_DATECREATED ? &data.ftCreationTime : id == ITEM_DATELASTACCESSED ? &data.ftLastAccessTime :
			&data.ftLastWriteTime);
	case ITEM_DRIVE: {
		WCHAR *root = drive_root(item->path);
		hr = sr_return_object(result, sr_drive_new(root));
		sr_free(root);
		return hr;
	}
	case ITEM_PARENTFOLDER: {
		WCHAR *parent = sr_parent_name(item->path);
		hr = *parent ? sr_return_object(result, sr_folder_new(parent)) : S_OK;
		sr_free(parent);
		return hr;
	}
	case ITEM_SHORTNAME:
	case ITEM_SHORTPATH:
		return short_path(item->path, id == ITEM_SHORTNAME, result);
	case ITEM_SIZE:
		if (folder)
			return return_size(result, tree_size(item->path));
		if (!attributes_data(item->path, &data))
			return sr_last_error();
		return return_size(result, ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow);
	case ITEM_TYPE:
		return type_name(item->path, folder, result);
	case ITEM_COPY:
		if (FAILED(hr = sr_text(sr_arg(params, flags, 0), &text)))
			return hr;
		if (SUCCEEDED(hr = sr_bool_or(sr_arg(params, flags, 1), TRUE, &flag)))
			hr = folder ? sr_copy_folders(item->path, text, flag) : sr_copy_files(item->path, text, flag);
		SysFreeString(text);
		return hr;
	case ITEM_DELETE:
		if (FAILED(hr = sr_bool_or(sr_arg(params, flags, 0), FALSE, &flag)))
			return hr;
		return folder ? sr_delete_folders(item->path, flag) : sr_delete_files(item->path, flag);
	case ITEM_MOVE: {
		if (FAILED(hr = sr_text(sr_arg(params, flags, 0), &text)))
			return hr;
		/* The object follows the item to where it goes: into the destination when that is a folder or ends with a separator. */
		WCHAR *name = sr_file_name(item->path);
		UINT length = SysStringLen(text);
		DWORD a = GetFileAttributesW(text);
		BOOL into = (length && (text[length - 1] == L'\\' || text[length - 1] == L'/')) ||
			(a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY));
		WCHAR *target = into ? sr_join(text, name) : sr_copy(text), *moved = sr_full_path(target);
		hr = folder ? sr_move_folders(item->path, text) : sr_move_files(item->path, text);
		if (SUCCEEDED(hr)) {
			sr_free(item->path);
			item->path = moved;
			moved = NULL;
		}
		sr_free(moved);
		sr_free(target);
		sr_free(name);
		SysFreeString(text);
		return hr;
	}
	}
	return DISP_E_MEMBERNOTFOUND;
}

static HRESULT file_invoke(sr_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	path_object *file = (path_object *)self;
	if (id == ITEM_OPENASTEXTSTREAM) {
		LONG mode, format;
		HRESULT hr;
		if (FAILED(hr = sr_long_or(sr_arg(params, flags, 0), SR_FOR_READING, &mode)) || FAILED(hr = sr_long_or(sr_arg(params, flags, 1), 0, &format)))
			return hr;
		return sr_open_text(file->path, mode, FALSE, format, TRUE, result);
	}
	return item_invoke(file, FALSE, id, flags, params, result);
}

static const sr_class file_class = { file_members, file_invoke, path_destroy };

static IDispatch *path_new(const sr_class *cls, const WCHAR *path)
{
	path_object *object = sr_object_new(cls, sizeof(path_object));
	object->path = sr_full_path(path);
	/* A folder's path ends with no separator but a root's. */
	UINT length = lstrlenW(object->path);
	while (length > 3 && (object->path[length - 1] == L'\\' || object->path[length - 1] == L'/'))
		object->path[--length] = 0;
	return sr_dispatch(object);
}

IDispatch *sr_file_new(const WCHAR *path) { return path_new(&file_class, path); }

/* Files and SubFolders: the items of one folder, by name, counted and enumerated in the order the file system gives them. */

typedef struct {
	sr_object base;
	WCHAR *path;
	BOOL folders;
} listing;

enum { LIST_COUNT = 1, LIST_ITEM = DISPID_VALUE, LIST_ADD = 2 };

static const sr_member listing_members[] = {
	{ L"Count", LIST_COUNT, 0 }, { L"Item", LIST_ITEM, 1 }, { L"Add", LIST_ADD, 1 }, { L"_NewEnum", DISPID_NEWENUM, 0 }, { NULL },
};

/* The items, as File or Folder objects; how many there are. */
static ULONG list_items(listing *l, VARIANT **out)
{
	WCHAR *pattern = sr_join(l->path, L"*");
	WIN32_FIND_DATAW found;
	HANDLE find = FindFirstFileW(pattern, &found);
	sr_free(pattern);
	ULONG count = 0, room = 0;
	*out = NULL;
	if (find == INVALID_HANDLE_VALUE)
		return 0;
	do {
		BOOL folder = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (folder != l->folders || !lstrcmpW(found.cFileName, L".") || !lstrcmpW(found.cFileName, L".."))
			continue;
		if (count == room) {
			room = room ? room * 2 : 16;
			*out = sr_grow(*out, room * sizeof(VARIANT));
		}
		WCHAR *child = sr_join(l->path, found.cFileName);
		V_VT(&(*out)[count]) = VT_DISPATCH;
		V_DISPATCH(&(*out)[count]) = folder ? sr_folder_new(child) : sr_file_new(child);
		sr_free(child);
		count++;
	} while (FindNextFileW(find, &found));
	FindClose(find);
	return count;
}

static void free_items(VARIANT *items, ULONG count)
{
	for (ULONG i = 0; i < count; i++)
		VariantClear(&items[i]);
	sr_free(items);
}

static HRESULT listing_invoke(sr_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	listing *l = (listing *)self;
	VARIANT *items;
	ULONG count;
	BSTR name = NULL;
	HRESULT hr;
	if (sr_is_put(flags))
		return DISP_E_MEMBERNOTFOUND;
	switch (id) {
	case LIST_COUNT:
		count = list_items(l, &items);
		free_items(items, count);
		return sr_return_long(result, (LONG)count);
	case DISPID_NEWENUM:
		count = list_items(l, &items);
		hr = sr_return_enum(result, items, count);
		free_items(items, count);
		return hr;
	case LIST_ITEM:
	case LIST_ADD: {
		if (FAILED(hr = sr_text(sr_arg(params, flags, 0), &name)))
			return hr;
		WCHAR *child = sr_join(l->path, name);
		DWORD a = GetFileAttributesW(child);
		if (id == LIST_ADD) {
			if (!l->folders)
				hr = DISP_E_MEMBERNOTFOUND;
			else if (a != INVALID_FILE_ATTRIBUTES)
				hr = sr_error(SR_E_FILE_EXISTS);
			else if (!CreateDirectoryW(child, NULL))
				hr = sr_last_error();
			else
				hr = sr_return_object(result, sr_folder_new(child));
		} else if (a == INVALID_FILE_ATTRIBUTES || ((a & FILE_ATTRIBUTE_DIRECTORY) != 0) != l->folders)
			hr = sr_error(l->folders ? SR_E_PATH_NOT_FOUND : SR_E_FILE_NOT_FOUND);
		else
			hr = sr_return_object(result, l->folders ? sr_folder_new(child) : sr_file_new(child));
		sr_free(child);
		SysFreeString(name);
		return hr;
	}
	}
	return DISP_E_MEMBERNOTFOUND;
}

static void listing_destroy(sr_object *self) { sr_free(((listing *)self)->path); }

static const sr_class listing_class = { listing_members, listing_invoke, listing_destroy };

static IDispatch *listing_new(const WCHAR *path, BOOL folders)
{
	listing *l = sr_object_new(&listing_class, sizeof(listing));
	l->path = sr_copy(path);
	l->folders = folders;
	return sr_dispatch(l);
}

static HRESULT folder_invoke(sr_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	path_object *folder = (path_object *)self;
	HRESULT hr;
	switch (id) {
	case ITEM_FILES:
	case ITEM_SUBFOLDERS:
		if (sr_is_put(flags))
			return DISP_E_MEMBERNOTFOUND;
		return sr_return_object(result, listing_new(folder->path, id == ITEM_SUBFOLDERS));
	case ITEM_ISROOTFOLDER: {
		WCHAR *parent = sr_parent_name(folder->path);
		hr = sr_return_bool(result, !*parent);
		sr_free(parent);
		return hr;
	}
	case ITEM_CREATETEXTFILE: {
		BSTR name;
		BOOL overwrite, unicode;
		if (FAILED(hr = sr_text(sr_arg(params, flags, 0), &name)))
			return hr;
		if (SUCCEEDED(hr = sr_bool_or(sr_arg(params, flags, 1), TRUE, &overwrite)) && SUCCEEDED(hr = sr_bool_or(sr_arg(params, flags, 2), FALSE, &unicode))) {
			WCHAR *path = sr_join(folder->path, name);
			hr = sr_open_text(path, SR_FOR_WRITING, TRUE, unicode ? -1 : 0, overwrite, result);
			sr_free(path);
		}
		SysFreeString(name);
		return hr;
	}
	}
	return item_invoke(folder, TRUE, id, flags, params, result);
}

static const sr_class folder_class = { folder_members, folder_invoke, path_destroy };

IDispatch *sr_folder_new(const WCHAR *path) { return path_new(&folder_class, path); }

/* Drive and Drives. */

enum {
	DRIVE_PATH = DISPID_VALUE, DRIVE_AVAILABLESPACE = 1, DRIVE_DRIVELETTER, DRIVE_DRIVETYPE, DRIVE_FILESYSTEM, DRIVE_FREESPACE, DRIVE_ISREADY,
	DRIVE_ROOTFOLDER, DRIVE_SERIALNUMBER, DRIVE_SHARENAME, DRIVE_TOTALSIZE, DRIVE_VOLUMENAME,
};

static const sr_member drive_members[] = {
	{ L"Path", DRIVE_PATH, 0 }, { L"AvailableSpace", DRIVE_AVAILABLESPACE, 0 }, { L"DriveLetter", DRIVE_DRIVELETTER, 0 }, { L"DriveType", DRIVE_DRIVETYPE, 0 },
	{ L"FileSystem", DRIVE_FILESYSTEM, 0 }, { L"FreeSpace", DRIVE_FREESPACE, 0 }, { L"IsReady", DRIVE_ISREADY, 0 }, { L"RootFolder", DRIVE_ROOTFOLDER, 0 },
	{ L"SerialNumber", DRIVE_SERIALNUMBER, 0 }, { L"ShareName", DRIVE_SHARENAME, 0 }, { L"TotalSize", DRIVE_TOTALSIZE, 0 },
	{ L"VolumeName", DRIVE_VOLUMENAME, 0 }, { NULL },
};

static HRESULT drive_invoke(sr_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	const WCHAR *root = ((path_object *)self)->path;
	WCHAR volume[MAX_PATH + 1] = L"", system[MAX_PATH + 1] = L"", text[8];
	DWORD serial = 0;
	ULARGE_INTEGER available, total, free;
	BOOL put = sr_is_put(flags);
	if (put && id != DRIVE_VOLUMENAME)
		return DISP_E_MEMBERNOTFOUND;
	switch (id) {
	case DRIVE_PATH:
		lstrcpynW(text, root, 3);
		return sr_return_text(result, root[1] == L':' ? text : root);
	case DRIVE_DRIVELETTER:
		lstrcpynW(text, root, 2);
		return sr_return_text(result, root[1] == L':' ? text : L"");
	case DRIVE_DRIVETYPE: {
		/* Unknown 0, Removable 1, Fixed 2, Network 3, CD-ROM 4, RAM disk 5. */
		static const LONG kinds[] = { 0, 0, 1, 2, 3, 4, 5 };
		UINT type = GetDriveTypeW(root);
		return sr_return_long(result, type < ARRAYSIZE(kinds) ? kinds[type] : 0);
	}
	case DRIVE_ISREADY:
		return sr_return_bool(result, GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, NULL, 0));
	case DRIVE_ROOTFOLDER:
		return sr_return_object(result, sr_folder_new(root));
	case DRIVE_SHARENAME:
		return sr_return_text(result, root[1] == L':' ? L"" : root);
	case DRIVE_AVAILABLESPACE:
	case DRIVE_FREESPACE:
	case DRIVE_TOTALSIZE:
		if (!GetDiskFreeSpaceExW(root, &available, &total, &free))
			return sr_error(SR_E_DISK_NOT_READY);
		return sr_return_double(result, (double)(id == DRIVE_AVAILABLESPACE ? available.QuadPart : id == DRIVE_FREESPACE ? free.QuadPart :
			total.QuadPart));
	case DRIVE_FILESYSTEM:
	case DRIVE_SERIALNUMBER:
	case DRIVE_VOLUMENAME:
		if (put) {
			BSTR name;
			HRESULT hr = sr_text(sr_put_value(params), &name);
			if (SUCCEEDED(hr) && !SetVolumeLabelW(root, name))
				hr = sr_last_error();
			SysFreeString(name);
			return hr;
		}
		if (!GetVolumeInformationW(root, volume, MAX_PATH, &serial, NULL, NULL, system, MAX_PATH))
			return sr_error(SR_E_DISK_NOT_READY);
		return id == DRIVE_SERIALNUMBER ? sr_return_long(result, (LONG)serial) : sr_return_text(result, id == DRIVE_FILESYSTEM ? system : volume);
	}
	return DISP_E_MEMBERNOTFOUND;
}

static const sr_class drive_class = { drive_members, drive_invoke, path_destroy };

IDispatch *sr_drive_new(const WCHAR *root)
{
	path_object *drive = sr_object_new(&drive_class, sizeof(path_object));
	drive->path = sr_copy(root);
	return sr_dispatch(drive);
}

static const sr_member drives_members[] = { { L"Count", LIST_COUNT, 0 }, { L"Item", LIST_ITEM, 1 }, { L"_NewEnum", DISPID_NEWENUM, 0 }, { NULL } };

static HRESULT drives_invoke(sr_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	WCHAR roots[4 * 26 + 2];
	DWORD length = GetLogicalDriveStringsW(ARRAYSIZE(roots), roots);
	VARIANT items[26];
	ULONG count = 0;
	HRESULT hr;
	if (sr_is_put(flags))
		return DISP_E_MEMBERNOTFOUND;
	if (length >= ARRAYSIZE(roots))
		length = 0;
	if (id == LIST_ITEM) {
		BSTR letter;
		if (FAILED(hr = sr_text(sr_arg(params, flags, 0), &letter)))
			return hr;
		for (WCHAR *root = roots; root < roots + length && *root; root += lstrlenW(root) + 1)
			if (letter[0] && (root[0] | 0x20) == (letter[0] | 0x20)) {
				SysFreeString(letter);
				return sr_return_object(result, sr_drive_new(root));
			}
		SysFreeString(letter);
		return sr_error(68);
	}
	for (WCHAR *root = roots; root < roots + length && *root && count < 26; root += lstrlenW(root) + 1) {
		V_VT(&items[count]) = VT_DISPATCH;
		V_DISPATCH(&items[count++]) = sr_drive_new(root);
	}
	if (id == LIST_COUNT)
		hr = sr_return_long(result, (LONG)count);
	else if (id == DISPID_NEWENUM)
		hr = sr_return_enum(result, items, count);
	else
		hr = DISP_E_MEMBERNOTFOUND;
	for (ULONG i = 0; i < count; i++)
		VariantClear(&items[i]);
	return hr;
}

static const sr_class drives_class = { drives_members, drives_invoke, NULL };

IDispatch *sr_drives_new(void) { return sr_dispatch(sr_object_new(&drives_class, sizeof(sr_object))); }
