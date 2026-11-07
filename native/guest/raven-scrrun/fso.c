/* SPDX-License-Identifier: MIT */
#include "scrrun.h"

/* Scripting.FileSystemObject: paths taken apart and put together, files and folders copied, moved and deleted (with wildcards in
 * the last part of a path), text files opened, and the File, Folder and Drive objects of the paths it is given. */

/* Paths. */

static BOOL is_separator(WCHAR c) { return c == L'\\' || c == L'/'; }

WCHAR *sr_full_path(const WCHAR *path)
{
	DWORD size = GetFullPathNameW(*path ? path : L".", 0, NULL, NULL);
	if (!size)
		return sr_copy(path);
	WCHAR *full = sr_alloc((size + 1) * sizeof(WCHAR));
	GetFullPathNameW(*path ? path : L".", size + 1, full, NULL);
	return full;
}

/* The length of a path without the separators that end it, keeping the one of a root such as C:\ . */
static UINT trimmed_length(const WCHAR *path)
{
	UINT length = lstrlenW(path);
	while (length > 1 && is_separator(path[length - 1]) && !(length == 3 && path[1] == L':'))
		length--;
	return length;
}

/* Where the drive part ends: C: or \\server\share. */
static UINT drive_length(const WCHAR *path)
{
	if (path[0] && path[1] == L':')
		return 2;
	if (is_separator(path[0]) && is_separator(path[1])) {
		UINT i = 2, parts = 0;
		while (path[i] && parts < 2) {
			if (is_separator(path[i]))
				parts++;
			if (parts < 2)
				i++;
		}
		return i;
	}
	return 0;
}

static WCHAR *copy_n(const WCHAR *text, UINT length)
{
	WCHAR *copy = sr_alloc((length + 1) * sizeof(WCHAR));
	memcpy(copy, text, length * sizeof(WCHAR));
	return copy;
}

WCHAR *sr_file_name(const WCHAR *path)
{
	UINT length = trimmed_length(path), start = length;
	if (length <= drive_length(path))
		return sr_copy(L"");
	while (start > 0 && !is_separator(path[start - 1]) && path[start - 1] != L':')
		start--;
	return copy_n(path + start, length - start);
}

WCHAR *sr_parent_name(const WCHAR *path)
{
	UINT length = trimmed_length(path), drive = drive_length(path);
	if (length <= drive)
		return sr_copy(L"");
	while (length > 0 && !is_separator(path[length - 1]) && path[length - 1] != L':')
		length--;
	if (!length)
		return sr_copy(L"");
	/* The parent keeps the separator of a root and drops any other. */
	while (length > drive + 1 && is_separator(path[length - 1]))
		length--;
	return copy_n(path, length);
}

WCHAR *sr_join(const WCHAR *path, const WCHAR *name)
{
	UINT a = lstrlenW(path), b = lstrlenW(name);
	BOOL separate = a && !is_separator(path[a - 1]) && path[a - 1] != L':' && !is_separator(name[0]);
	WCHAR *joined = sr_alloc((a + b + 2) * sizeof(WCHAR));
	memcpy(joined, path, a * sizeof(WCHAR));
	if (separate)
		joined[a++] = L'\\';
	memcpy(joined + a, name, b * sizeof(WCHAR));
	return joined;
}

static WCHAR *base_name(const WCHAR *path, BOOL extension)
{
	WCHAR *name = sr_file_name(path);
	WCHAR *dot = NULL;
	for (WCHAR *c = name; *c; c++)
		if (*c == L'.')
			dot = c;
	if (extension) {
		WCHAR *result = sr_copy(dot ? dot + 1 : L"");
		sr_free(name);
		return result;
	}
	if (dot)
		*dot = 0;
	return name;
}

static BOOL has_wildcards(const WCHAR *text)
{
	for (; *text; text++)
		if (*text == L'*' || *text == L'?')
			return TRUE;
	return FALSE;
}

static DWORD attributes_of(const WCHAR *path) { return GetFileAttributesW(path); }

static BOOL file_exists(const WCHAR *path)
{
	DWORD a = attributes_of(path);
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL folder_exists(const WCHAR *path)
{
	DWORD a = attributes_of(path);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Operations over the files or folders a spec names. */

typedef HRESULT (*each_fn)(const WCHAR *path, const WCHAR *name, const WCHAR *destination, BOOL flag);

/* Calls each for every match of spec, files or folders; the error of a spec that matches nothing. */
static HRESULT for_matches(const WCHAR *spec, BOOL folders, const WCHAR *destination, BOOL flag, each_fn each)
{
	if (!has_wildcards(spec)) {
		if (folders ? !folder_exists(spec) : !file_exists(spec))
			return sr_error(folders ? SR_E_PATH_NOT_FOUND : SR_E_FILE_NOT_FOUND);
		WCHAR *name = sr_file_name(spec);
		HRESULT hr = each(spec, name, destination, flag);
		sr_free(name);
		return hr;
	}
	WIN32_FIND_DATAW found;
	HANDLE find = FindFirstFileW(spec, &found);
	if (find == INVALID_HANDLE_VALUE)
		return sr_error(folders ? SR_E_PATH_NOT_FOUND : SR_E_FILE_NOT_FOUND);
	WCHAR *parent = sr_parent_name(spec);
	HRESULT hr = S_OK;
	BOOL any = FALSE;
	do {
		BOOL folder = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (folder != folders || !lstrcmpW(found.cFileName, L".") || !lstrcmpW(found.cFileName, L".."))
			continue;
		any = TRUE;
		WCHAR *path = sr_join(parent, found.cFileName);
		hr = each(path, found.cFileName, destination, flag);
		sr_free(path);
	} while (SUCCEEDED(hr) && FindNextFileW(find, &found));
	FindClose(find);
	sr_free(parent);
	if (SUCCEEDED(hr) && !any)
		hr = sr_error(folders ? SR_E_PATH_NOT_FOUND : SR_E_FILE_NOT_FOUND);
	return hr;
}

/* The destination of one item: inside the destination when it is a folder (or ends with a separator), the destination itself
 * otherwise. */
static WCHAR *target_of(const WCHAR *destination, const WCHAR *name, BOOL into)
{
	UINT length = lstrlenW(destination);
	if (into || (length && is_separator(destination[length - 1])) || folder_exists(destination))
		return sr_join(destination, name);
	return sr_copy(destination);
}

static HRESULT copy_one_file(const WCHAR *path, const WCHAR *name, const WCHAR *destination, BOOL overwrite)
{
	WCHAR *target = target_of(destination, name, FALSE);
	HRESULT hr = S_OK;
	if (file_exists(target) && !overwrite)
		hr = sr_error(SR_E_FILE_EXISTS);
	else if (!CopyFileW(path, target, FALSE))
		hr = sr_last_error();
	sr_free(target);
	return hr;
}

static HRESULT move_one(const WCHAR *path, const WCHAR *name, const WCHAR *destination, BOOL folder)
{
	UINT length = lstrlenW(destination);
	WCHAR *target = folder && !(length && is_separator(destination[length - 1])) && !folder_exists(destination) ? sr_copy(destination) :
		target_of(destination, name, FALSE);
	HRESULT hr = S_OK;
	if (attributes_of(target) != INVALID_FILE_ATTRIBUTES)
		hr = sr_error(SR_E_FILE_EXISTS);
	else if (!MoveFileExW(path, target, MOVEFILE_COPY_ALLOWED))
		hr = sr_last_error();
	sr_free(target);
	return hr;
}

static HRESULT move_one_file(const WCHAR *path, const WCHAR *name, const WCHAR *destination, BOOL flag)
{
	return move_one(path, name, destination, FALSE);
}

static HRESULT move_one_folder(const WCHAR *path, const WCHAR *name, const WCHAR *destination, BOOL flag)
{
	return move_one(path, name, destination, TRUE);
}

static HRESULT delete_one_file(const WCHAR *path, const WCHAR *name, const WCHAR *destination, BOOL force)
{
	DWORD a = attributes_of(path);
	if (a & FILE_ATTRIBUTE_READONLY) {
		if (!force)
			return sr_error(SR_E_PERMISSION);
		SetFileAttributesW(path, a & ~FILE_ATTRIBUTE_READONLY);
	}
	return DeleteFileW(path) ? S_OK : sr_last_error();
}

/* A folder's whole tree: copied into target, or deleted. */
static HRESULT copy_tree(const WCHAR *source, const WCHAR *target, BOOL overwrite)
{
	if (!folder_exists(target) && !CreateDirectoryW(target, NULL))
		return sr_last_error();
	WCHAR *pattern = sr_join(source, L"*");
	WIN32_FIND_DATAW found;
	HANDLE find = FindFirstFileW(pattern, &found);
	sr_free(pattern);
	if (find == INVALID_HANDLE_VALUE)
		return S_OK;
	HRESULT hr = S_OK;
	do {
		if (!lstrcmpW(found.cFileName, L".") || !lstrcmpW(found.cFileName, L".."))
			continue;
		WCHAR *from = sr_join(source, found.cFileName), *to = sr_join(target, found.cFileName);
		if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			hr = copy_tree(from, to, overwrite);
		else if (file_exists(to) && !overwrite)
			hr = sr_error(SR_E_FILE_EXISTS);
		else if (!CopyFileW(from, to, FALSE))
			hr = sr_last_error();
		sr_free(from);
		sr_free(to);
	} while (SUCCEEDED(hr) && FindNextFileW(find, &found));
	FindClose(find);
	return hr;
}

static HRESULT delete_tree(const WCHAR *path, BOOL force)
{
	WCHAR *pattern = sr_join(path, L"*");
	WIN32_FIND_DATAW found;
	HANDLE find = FindFirstFileW(pattern, &found);
	sr_free(pattern);
	HRESULT hr = S_OK;
	if (find != INVALID_HANDLE_VALUE) {
		do {
			if (!lstrcmpW(found.cFileName, L".") || !lstrcmpW(found.cFileName, L".."))
				continue;
			WCHAR *child = sr_join(path, found.cFileName);
			hr = found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? delete_tree(child, force) : delete_one_file(child, NULL, NULL, force);
			sr_free(child);
		} while (SUCCEEDED(hr) && FindNextFileW(find, &found));
		FindClose(find);
	}
	if (FAILED(hr))
		return hr;
	DWORD a = attributes_of(path);
	if (a & FILE_ATTRIBUTE_READONLY) {
		if (!force)
			return sr_error(SR_E_PERMISSION);
		SetFileAttributesW(path, a & ~FILE_ATTRIBUTE_READONLY);
	}
	return RemoveDirectoryW(path) ? S_OK : sr_last_error();
}

static HRESULT copy_one_folder(const WCHAR *path, const WCHAR *name, const WCHAR *destination, BOOL overwrite)
{
	UINT length = lstrlenW(destination);
	WCHAR *target = length && is_separator(destination[length - 1]) ? sr_join(destination, name) : sr_copy(destination);
	HRESULT hr = copy_tree(path, target, overwrite);
	sr_free(target);
	return hr;
}

static HRESULT delete_one_folder(const WCHAR *path, const WCHAR *name, const WCHAR *destination, BOOL force)
{
	return delete_tree(path, force);
}

HRESULT sr_copy_files(const WCHAR *spec, const WCHAR *destination, BOOL overwrite)
{
	return for_matches(spec, FALSE, destination, overwrite, copy_one_file);
}

HRESULT sr_move_files(const WCHAR *spec, const WCHAR *destination) { return for_matches(spec, FALSE, destination, FALSE, move_one_file); }
HRESULT sr_delete_files(const WCHAR *spec, BOOL force) { return for_matches(spec, FALSE, NULL, force, delete_one_file); }

HRESULT sr_copy_folders(const WCHAR *spec, const WCHAR *destination, BOOL overwrite)
{
	return for_matches(spec, TRUE, destination, overwrite, copy_one_folder);
}

HRESULT sr_move_folders(const WCHAR *spec, const WCHAR *destination) { return for_matches(spec, TRUE, destination, FALSE, move_one_folder); }
HRESULT sr_delete_folders(const WCHAR *spec, BOOL force) { return for_matches(spec, TRUE, NULL, force, delete_one_folder); }

/* The object. */

enum {
	FSO_BUILDPATH = 1, FSO_COPYFILE, FSO_COPYFOLDER, FSO_CREATEFOLDER, FSO_CREATETEXTFILE, FSO_DELETEFILE, FSO_DELETEFOLDER, FSO_DRIVEEXISTS,
	FSO_FILEEXISTS, FSO_FOLDEREXISTS, FSO_GETABSOLUTEPATHNAME, FSO_GETBASENAME, FSO_GETDRIVE, FSO_GETDRIVENAME, FSO_GETEXTENSIONNAME,
	FSO_GETFILE, FSO_GETFILENAME, FSO_GETFILEVERSION, FSO_GETFOLDER, FSO_GETPARENTFOLDERNAME, FSO_GETSPECIALFOLDER, FSO_GETSTANDARDSTREAM,
	FSO_GETTEMPNAME, FSO_MOVEFILE, FSO_MOVEFOLDER, FSO_OPENTEXTFILE, FSO_DRIVES,
};

static const sr_member members[] = {
	{ L"BuildPath", FSO_BUILDPATH, 2 }, { L"CopyFile", FSO_COPYFILE, 3 }, { L"CopyFolder", FSO_COPYFOLDER, 3 }, { L"CreateFolder", FSO_CREATEFOLDER, 1 },
	{ L"CreateTextFile", FSO_CREATETEXTFILE, 3 }, { L"DeleteFile", FSO_DELETEFILE, 2 }, { L"DeleteFolder", FSO_DELETEFOLDER, 2 },
	{ L"DriveExists", FSO_DRIVEEXISTS, 1 }, { L"FileExists", FSO_FILEEXISTS, 1 }, { L"FolderExists", FSO_FOLDEREXISTS, 1 },
	{ L"GetAbsolutePathName", FSO_GETABSOLUTEPATHNAME, 1 }, { L"GetBaseName", FSO_GETBASENAME, 1 }, { L"GetDrive", FSO_GETDRIVE, 1 },
	{ L"GetDriveName", FSO_GETDRIVENAME, 1 }, { L"GetExtensionName", FSO_GETEXTENSIONNAME, 1 }, { L"GetFile", FSO_GETFILE, 1 },
	{ L"GetFileName", FSO_GETFILENAME, 1 }, { L"GetFileVersion", FSO_GETFILEVERSION, 1 }, { L"GetFolder", FSO_GETFOLDER, 1 },
	{ L"GetParentFolderName", FSO_GETPARENTFOLDERNAME, 1 }, { L"GetSpecialFolder", FSO_GETSPECIALFOLDER, 1 },
	{ L"GetStandardStream", FSO_GETSTANDARDSTREAM, 2 }, { L"GetTempName", FSO_GETTEMPNAME, 0 }, { L"MoveFile", FSO_MOVEFILE, 2 },
	{ L"MoveFolder", FSO_MOVEFOLDER, 2 }, { L"OpenTextFile", FSO_OPENTEXTFILE, 4 }, { L"Drives", FSO_DRIVES, 0 }, { NULL },
};

static HRESULT file_version(const WCHAR *path, VARIANT *result)
{
	DWORD ignored, size = GetFileVersionInfoSizeW(path, &ignored);
	if (!size)
		return file_exists(path) ? sr_return_text(result, L"") : sr_error(SR_E_FILE_NOT_FOUND);
	BYTE *data = sr_alloc(size);
	VS_FIXEDFILEINFO *info = NULL;
	UINT length = 0;
	WCHAR text[64] = L"";
	if (GetFileVersionInfoW(path, 0, size, data) && VerQueryValueW(data, L"\\", (void **)&info, &length) && info)
		wsprintfW(text, L"%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS),
			LOWORD(info->dwFileVersionLS));
	sr_free(data);
	return sr_return_text(result, text);
}

static HRESULT special_folder(LONG which, VARIANT *result)
{
	WCHAR path[MAX_PATH + 1];
	UINT length;
	switch (which) {
	case 0: length = GetWindowsDirectoryW(path, MAX_PATH); break;
	case 1: length = GetSystemDirectoryW(path, MAX_PATH); break;
	case 2: length = GetTempPathW(MAX_PATH, path); break;
	default: return sr_error(SR_E_ARGUMENT);
	}
	if (!length || length > MAX_PATH)
		return sr_last_error();
	path[trimmed_length(path)] = 0;
	return sr_return_object(result, sr_folder_new(path));
}

static HRESULT standard_stream(LONG which, BOOL unicode, VARIANT *result)
{
	static const DWORD handles[] = { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
	if (which < 0 || which > 2)
		return sr_error(SR_E_ARGUMENT);
	HANDLE handle = GetStdHandle(handles[which]);
	if (!handle || handle == INVALID_HANDLE_VALUE)
		return sr_error(SR_E_BAD_FILE_MODE);
	return sr_return_object(result, sr_text_stream_new(handle, FALSE, which ? SR_FOR_WRITING : SR_FOR_READING, unicode));
}

/* A drive by letter, by root or by a path on it. */
static HRESULT drive_of(const WCHAR *spec, BOOL exists_only, VARIANT *result)
{
	WCHAR root[MAX_PATH + 2];
	UINT length = drive_length(spec);
	if (!length && spec[0] && !spec[1])
		wsprintfW(root, L"%c:\\", spec[0]);
	else if (length && length < MAX_PATH) {
		lstrcpynW(root, spec, length + 1);
		lstrcatW(root, L"\\");
	} else
		return exists_only ? sr_return_bool(result, FALSE) : sr_error(SR_E_ARGUMENT);
	UINT type = GetDriveTypeW(root);
	if (exists_only)
		return sr_return_bool(result, type != DRIVE_NO_ROOT_DIR && type != DRIVE_UNKNOWN);
	if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN)
		return sr_error(68);
	return sr_return_object(result, sr_drive_new(root));
}

static HRESULT fso_invoke(sr_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	BSTR first = NULL, second = NULL;
	WCHAR *text = NULL;
	LONG number = 0, format = 0;
	BOOL flag = FALSE, create = FALSE;
	HRESULT hr = S_OK;
	if (sr_is_put(flags))
		return DISP_E_MEMBERNOTFOUND;
	if (id != FSO_GETTEMPNAME && id != FSO_DRIVES && id != FSO_GETSPECIALFOLDER && id != FSO_GETSTANDARDSTREAM &&
		FAILED(hr = sr_text(sr_arg(params, flags, 0), &first)))
		return hr;
	switch (id) {
	case FSO_BUILDPATH:
		if (SUCCEEDED(hr = sr_text(sr_arg(params, flags, 1), &second))) {
			text = sr_join(first, second);
			hr = sr_return_text(result, text);
		}
		break;
	case FSO_COPYFILE:
	case FSO_COPYFOLDER:
		if (SUCCEEDED(hr = sr_text(sr_arg(params, flags, 1), &second)) && SUCCEEDED(hr = sr_bool_or(sr_arg(params, flags, 2), TRUE, &flag)))
			hr = id == FSO_COPYFILE ? sr_copy_files(first, second, flag) : sr_copy_folders(first, second, flag);
		break;
	case FSO_MOVEFILE:
	case FSO_MOVEFOLDER:
		if (SUCCEEDED(hr = sr_text(sr_arg(params, flags, 1), &second)))
			hr = id == FSO_MOVEFILE ? sr_move_files(first, second) : sr_move_folders(first, second);
		break;
	case FSO_DELETEFILE:
	case FSO_DELETEFOLDER:
		if (SUCCEEDED(hr = sr_bool_or(sr_arg(params, flags, 1), FALSE, &flag)))
			hr = id == FSO_DELETEFILE ? sr_delete_files(first, flag) : sr_delete_folders(first, flag);
		break;
	case FSO_CREATEFOLDER:
		if (attributes_of(first) != INVALID_FILE_ATTRIBUTES)
			hr = sr_error(SR_E_FILE_EXISTS);
		else if (!CreateDirectoryW(first, NULL))
			hr = sr_last_error();
		else
			hr = sr_return_object(result, sr_folder_new(first));
		break;
	case FSO_CREATETEXTFILE:
		if (SUCCEEDED(hr = sr_bool_or(sr_arg(params, flags, 1), TRUE, &flag)) && SUCCEEDED(hr = sr_bool_or(sr_arg(params, flags, 2), FALSE, &create)))
			hr = sr_open_text(first, SR_FOR_WRITING, TRUE, create ? -1 : 0, flag, result);
		break;
	case FSO_OPENTEXTFILE:
		if (SUCCEEDED(hr = sr_long_or(sr_arg(params, flags, 1), SR_FOR_READING, &number)) &&
			SUCCEEDED(hr = sr_bool_or(sr_arg(params, flags, 2), FALSE, &create)) && SUCCEEDED(hr = sr_long_or(sr_arg(params, flags, 3), 0, &format)))
			hr = sr_open_text(first, number, create, format, TRUE, result);
		break;
	case FSO_DRIVEEXISTS:
		hr = drive_of(first, TRUE, result);
		break;
	case FSO_FILEEXISTS:
		hr = sr_return_bool(result, file_exists(first));
		break;
	case FSO_FOLDEREXISTS:
		hr = sr_return_bool(result, folder_exists(first));
		break;
	case FSO_GETABSOLUTEPATHNAME:
		text = sr_full_path(first);
		hr = sr_return_text(result, text);
		break;
	case FSO_GETBASENAME:
	case FSO_GETEXTENSIONNAME:
		text = base_name(first, id == FSO_GETEXTENSIONNAME);
		hr = sr_return_text(result, text);
		break;
	case FSO_GETFILENAME:
		text = sr_file_name(first);
		hr = sr_return_text(result, text);
		break;
	case FSO_GETPARENTFOLDERNAME:
		text = sr_parent_name(first);
		hr = sr_return_text(result, text);
		break;
	case FSO_GETDRIVENAME:
		text = sr_alloc((drive_length(first) + 1) * sizeof(WCHAR));
		lstrcpynW(text, first, drive_length(first) + 1);
		hr = sr_return_text(result, text);
		break;
	case FSO_GETDRIVE:
		hr = drive_of(first, FALSE, result);
		break;
	case FSO_GETFILE:
		hr = file_exists(first) ? sr_return_object(result, sr_file_new(first)) : sr_error(SR_E_FILE_NOT_FOUND);
		break;
	case FSO_GETFOLDER:
		hr = folder_exists(first) ? sr_return_object(result, sr_folder_new(first)) : sr_error(SR_E_PATH_NOT_FOUND);
		break;
	case FSO_GETFILEVERSION:
		hr = file_version(first, result);
		break;
	case FSO_GETSPECIALFOLDER:
		if (SUCCEEDED(hr = sr_long(sr_arg(params, flags, 0), &number)))
			hr = special_folder(number, result);
		break;
	case FSO_GETSTANDARDSTREAM:
		if (SUCCEEDED(hr = sr_long(sr_arg(params, flags, 0), &number)) && SUCCEEDED(hr = sr_bool_or(sr_arg(params, flags, 1), FALSE, &flag)))
			hr = standard_stream(number, flag, result);
		break;
	case FSO_GETTEMPNAME: {
		WCHAR name[16];
		wsprintfW(name, L"rad%05X.tmp", (GetTickCount() ^ (GetCurrentProcessId() << 8) ^ (ULONG)(ULONG_PTR)&name) & 0xfffff);
		hr = sr_return_text(result, name);
		break;
	}
	case FSO_DRIVES:
		hr = sr_return_object(result, sr_drives_new());
		break;
	default:
		hr = DISP_E_MEMBERNOTFOUND;
	}
	SysFreeString(first);
	SysFreeString(second);
	sr_free(text);
	return hr;
}

static const sr_class fso_class = { members, fso_invoke, NULL };

IDispatch *sr_file_system_new(void) { return sr_dispatch(sr_object_new(&fso_class, sizeof(sr_object))); }
