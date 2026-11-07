/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "advpack.h"

#define DELETE_IF_EMPTY 0x1
#define KEEP_SUBDIRECTORIES 0x2
#define KEEP_DIRECTORY 0x4
#define ALLOW_NETWORK 0x8

#define BACKUP_ADD 0x1
#define BACKUP_DELETE 0x2

#define COPY_NO_VERSION_CHECK 0x4
#define COPY_NO_OVERWRITE 0x10
#define COPY_REPLACE_ONLY 0x400

static HRESULT delete_tree(const WCHAR *path, DWORD flags)
{
	WIN32_FIND_DATAW found;
	WCHAR *pattern, *child;
	HRESULT result = S_OK;
	DWORD attributes = GetFileAttributesW(path);
	HANDLE search;
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return E_FAIL;
	if (attributes & FILE_ATTRIBUTE_READONLY)
		SetFileAttributesW(path, attributes & ~FILE_ATTRIBUTE_READONLY);
	if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
		return DeleteFileW(path) ? S_OK : E_FAIL;
	if (!(flags & DELETE_IF_EMPTY))
	{
		pattern = adv_join(path, L"*");
		if (!pattern)
			return E_OUTOFMEMORY;
		search = FindFirstFileW(pattern, &found);
		adv_free(pattern);
		if (search != INVALID_HANDLE_VALUE)
		{
			do
			{
				if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L".."))
					continue;
				if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (flags & KEEP_SUBDIRECTORIES))
					continue;
				child = adv_join(path, found.cFileName);
				if (!child || FAILED(delete_tree(child, flags & ~KEEP_DIRECTORY)))
					result = E_FAIL;
				adv_free(child);
			}
			while (FindNextFileW(search, &found));
			FindClose(search);
		}
	}
	if (SUCCEEDED(result) && !(flags & KEEP_DIRECTORY) && !RemoveDirectoryW(path))
		result = E_FAIL;
	return result;
}

HRESULT WINAPI DelNodeW(const WCHAR *path, DWORD flags)
{
	if (!path || !path[0])
		return E_FAIL;
	if (path[0] == L'\\' && path[1] == L'\\' && !(flags & ALLOW_NETWORK))
		return E_FAIL;
	return delete_tree(path, flags);
}

HRESULT WINAPI DelNodeA(const char *path, DWORD flags)
{
	WCHAR *wide = adv_wide(path);
	HRESULT result = DelNodeW(wide, flags);
	adv_free(wide);
	return result;
}

HRESULT WINAPI DelNodeRunDLL32W(HWND window, HINSTANCE instance, const WCHAR *line, INT show)
{
	WCHAR *copy, *fields[2] = {NULL};
	HRESULT result;
	if (!line)
		return E_FAIL;
	copy = adv_copy(line);
	if (!copy)
		return E_OUTOFMEMORY;
	adv_split(copy, fields, 2);
	result = DelNodeW(fields[0], fields[1] ? wcstol(fields[1], NULL, 10) : 0);
	adv_free(copy);
	return result;
}

HRESULT WINAPI DelNodeRunDLL32A(HWND window, HINSTANCE instance, const char *line, INT show)
{
	WCHAR *wide = adv_wide(line);
	HRESULT result = DelNodeRunDLL32W(window, instance, wide, show);
	adv_free(wide);
	return result;
}

/* Wanted names of a cabinet: the list as cut pieces, each with a found mark. */
typedef struct
{
	WCHAR *names;
	DWORD count;
	BOOL *found;
	const WCHAR *directory;
	BOOL extract;
} extract_run;

static BOOL is_blank(WCHAR character)
{
	return character == L' ' || character == L'\t';
}

/* Cuts "a:b : c" in place into consecutive strings, dropping blanks and empty pieces. */
static DWORD cut_list(WCHAR *list)
{
	WCHAR *read = list, *write = list, *end;
	DWORD count = 0;
	while (*read)
	{
		end = read;
		while (*end && *end != L':')
			end++;
		while (read < end && is_blank(*read))
			read++;
		while (end > read && is_blank(end[-1]))
			end--;
		if (end > read)
		{
			size_t length = end - read;
			WCHAR *next = end;
			while (*next && *next != L':')
				next++;
			next += *next ? 1 : 0;
			memmove(write, read, length * sizeof(WCHAR));
			write += length;
			*write++ = 0;
			count++;
			read = next;
			continue;
		}
		read = end;
		while (*read && *read != L':')
			read++;
		if (*read)
			read++;
	}
	*write = 0;
	return count;
}

static int wanted_index(extract_run *run, const WCHAR *name)
{
	const WCHAR *candidate = run->names;
	DWORD index;
	for (index = 0; index < run->count; index++)
	{
		if (!lstrcmpiW(candidate, name))
			return index;
		candidate += wcslen(candidate) + 1;
	}
	return -1;
}

static UINT CALLBACK extract_callback(void *context, UINT notification, UINT_PTR first, UINT_PTR second)
{
	extract_run *run = context;
	FILE_IN_CABINET_INFO_W *info;
	WCHAR *target, *parent;
	int index = -1;
	switch (notification)
	{
	case SPFILENOTIFY_FILEINCABINET:
		info = (FILE_IN_CABINET_INFO_W *)first;
		if (run->names)
		{
			index = wanted_index(run, info->NameInCabinet);
			if (index < 0)
				return FILEOP_SKIP;
			run->found[index] = TRUE;
		}
		if (!run->extract)
			return FILEOP_SKIP;
		target = adv_join(run->directory, info->NameInCabinet);
		if (!target || wcslen(target) >= MAX_PATH)
		{
			adv_free(target);
			return FILEOP_ABORT;
		}
		lstrcpyW(info->FullTargetName, target);
		parent = adv_parent(target);
		if (parent)
			SHCreateDirectoryExW(NULL, parent, NULL);
		adv_free(parent);
		adv_free(target);
		return FILEOP_DOIT;
	case SPFILENOTIFY_NEEDNEWCABINET:
		return ERROR_FILE_NOT_FOUND;
	}
	return NO_ERROR;
}

/* A cabinet missing a listed name extracts nothing, so a first pass only checks names. */
HRESULT adv_extract_cab(const WCHAR *cab, const WCHAR *directory, const WCHAR *list)
{
	extract_run run = {NULL, 0, NULL, NULL, FALSE};
	WCHAR *full_cab, *full_directory;
	HRESULT result = S_OK;
	DWORD index, attributes;
	full_cab = adv_full_path(cab);
	full_directory = adv_full_path(directory);
	if (!full_cab || !full_directory)
	{
		result = E_OUTOFMEMORY;
		goto done;
	}
	attributes = GetFileAttributesW(full_directory);
	if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
	{
		result = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
		goto done;
	}
	run.directory = full_directory;
	if (list)
	{
		size_t length = wcslen(list);
		run.names = adv_alloc((length + 2) * sizeof(WCHAR));
		if (!run.names)
		{
			result = E_OUTOFMEMORY;
			goto done;
		}
		memcpy(run.names, list, length * sizeof(WCHAR));
		run.count = cut_list(run.names);
		run.found = adv_alloc((run.count + 1) * sizeof(BOOL));
		if (!run.count || !run.found)
		{
			result = run.found ? E_FAIL : E_OUTOFMEMORY;
			goto done;
		}
		if (!SetupIterateCabinetW(full_cab, 0, extract_callback, &run))
		{
			result = adv_error(GetLastError());
			goto done;
		}
		for (index = 0; index < run.count; index++)
			if (!run.found[index])
			{
				result = E_FAIL;
				goto done;
			}
	}
	run.extract = TRUE;
	if (!SetupIterateCabinetW(full_cab, 0, extract_callback, &run))
		result = adv_error(GetLastError());
done:
	adv_free(run.found);
	adv_free(run.names);
	adv_free(full_directory);
	adv_free(full_cab);
	return result;
}

HRESULT WINAPI ExtractFilesW(const WCHAR *cab, const WCHAR *directory, DWORD flags, const WCHAR *list, void *reserved, DWORD reserved2)
{
	if (!cab || !directory)
		return E_INVALIDARG;
	return adv_extract_cab(cab, directory, list);
}

HRESULT WINAPI ExtractFilesA(const char *cab, const char *directory, DWORD flags, const char *list, void *reserved, DWORD reserved2)
{
	WCHAR *wide_cab = adv_wide(cab), *wide_directory = adv_wide(directory), *wide_list = adv_wide(list);
	HRESULT result = ExtractFilesW(wide_cab, wide_directory, flags, wide_list, reserved, reserved2);
	adv_free(wide_cab);
	adv_free(wide_directory);
	adv_free(wide_list);
	return result;
}

HRESULT WINAPI AdvInstallFileW(HWND window, const WCHAR *source_directory, const WCHAR *source_file, const WCHAR *target_directory, const WCHAR *target_file, DWORD flags, DWORD reserved)
{
	WCHAR *target_path;
	HRESULT result = S_OK;
	HSPFILEQ queue;
	void *context;
	BOOL exists;
	if (!source_directory || !source_file || !target_directory)
		return E_INVALIDARG;
	if (!target_file)
		target_file = source_file;
	target_path = adv_join(target_directory, target_file);
	if (!target_path)
		return E_OUTOFMEMORY;
	exists = GetFileAttributesW(target_path) != INVALID_FILE_ATTRIBUTES;
	adv_free(target_path);
	if (exists ? (flags & COPY_NO_OVERWRITE) != 0 : (flags & COPY_REPLACE_ONLY) != 0)
		return S_OK;
	queue = SetupOpenFileQueue();
	if (queue == INVALID_HANDLE_VALUE)
		return adv_error(GetLastError());
	if (!SetupQueueCopyW(queue, source_directory, NULL, source_file, NULL, NULL, target_directory, target_file, (flags & COPY_NO_VERSION_CHECK) ? 0 : SP_COPY_NEWER))
		result = adv_error(GetLastError());
	else
	{
		context = SetupInitDefaultQueueCallbackEx(window, INVALID_HANDLE_VALUE, 0, 0, NULL);
		if (!SetupCommitFileQueueW(window, queue, SetupDefaultQueueCallbackW, context))
			result = adv_error(GetLastError());
		SetupTermDefaultQueueCallback(context);
	}
	SetupCloseFileQueue(queue);
	return result;
}

HRESULT WINAPI AdvInstallFileA(HWND window, const char *source_directory, const char *source_file, const char *target_directory, const char *target_file, DWORD flags, DWORD reserved)
{
	WCHAR *a = adv_wide(source_directory), *b = adv_wide(source_file), *c = adv_wide(target_directory), *d = adv_wide(target_file);
	HRESULT result = AdvInstallFileW(window, a, b, c, d, flags, reserved);
	adv_free(a);
	adv_free(b);
	adv_free(c);
	adv_free(d);
	return result;
}

/* Entries live in the [backup] section of a protected <directory>\<base>.INI. */
HRESULT WINAPI AddDelBackupEntryW(const WCHAR *list, const WCHAR *directory, const WCHAR *base, DWORD flags)
{
	static const WCHAR marker[] = L"-1,0,0,0,0,0,-1";
	WCHAR *folder, *file, *path;
	const WCHAR *name;
	if (!list || !(flags & (BACKUP_ADD | BACKUP_DELETE)))
		return S_OK;
	if (directory)
		folder = adv_full_path(directory);
	else
	{
		folder = adv_alloc(MAX_PATH * sizeof(WCHAR));
		if (folder)
			GetWindowsDirectoryW(folder, MAX_PATH);
	}
	file = adv_alloc((wcslen(base) + 5) * sizeof(WCHAR));
	if (!folder || !file)
	{
		adv_free(folder);
		adv_free(file);
		return S_OK;
	}
	lstrcpyW(file, base);
	lstrcatW(file, L".INI");
	path = adv_join(folder, file);
	if (path)
	{
		if (GetFileAttributesW(folder) != INVALID_FILE_ATTRIBUTES)
		{
			SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
			for (name = list; *name; name += wcslen(name) + 1)
				WritePrivateProfileStringW(L"backup", name, (flags & BACKUP_ADD) ? marker : NULL, path);
			SetFileAttributesW(path, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY);
		}
		adv_free(path);
	}
	adv_free(file);
	adv_free(folder);
	return S_OK;
}

HRESULT WINAPI AddDelBackupEntryA(const char *list, const char *directory, const char *base, DWORD flags)
{
	WCHAR *wide_list = adv_wide_list(list), *wide_directory = adv_wide(directory), *wide_base = adv_wide(base);
	HRESULT result = wide_base ? AddDelBackupEntryW(wide_list, wide_directory, wide_base, flags) : E_INVALIDARG;
	adv_free(wide_list);
	adv_free(wide_directory);
	adv_free(wide_base);
	return result;
}

HRESULT WINAPI FileSaveMarkNotExistW(const WCHAR *list, const WCHAR *directory, const WCHAR *base)
{
	return AddDelBackupEntryW(list, directory, base, BACKUP_ADD);
}

HRESULT WINAPI FileSaveMarkNotExistA(const char *list, const char *directory, const char *base)
{
	return AddDelBackupEntryA(list, directory, base, BACKUP_ADD);
}
