/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "../raven-winrt/winrt.h"
#include "../raven-appx/appxpackaging.h"

#define CALL(object, method, ...) ((object)->lpVtbl->method((object), __VA_ARGS__))
#define RECORD L".raven-package"

static HRESULT file_error(void)
{
	return HRESULT_FROM_WIN32(GetLastError());
}

static BOOL safe_name(const WCHAR *name, BOOL subdirectories)
{
	if (!name || !*name)
		return FALSE;
	const WCHAR *part = name;
	for (const WCHAR *at = name;; ++at) {
		const WCHAR c = *at;
		if (!c || c == L'\\' || c == L'/') {
			if (at == part || at[-1] == L'.' || at[-1] == L' ' || (c && !subdirectories))
				return FALSE;
			const WCHAR *end = part;
			while (end < at && *end != L'.')
				++end;
			const int length = (int)(end - part);
			if (length == 3 && (CompareStringOrdinal(part, 3, L"CON", 3, TRUE) == CSTR_EQUAL ||
					CompareStringOrdinal(part, 3, L"PRN", 3, TRUE) == CSTR_EQUAL ||
					CompareStringOrdinal(part, 3, L"AUX", 3, TRUE) == CSTR_EQUAL ||
					CompareStringOrdinal(part, 3, L"NUL", 3, TRUE) == CSTR_EQUAL))
				return FALSE;
			if (length == 4 && part[3] >= L'1' && part[3] <= L'9' && (CompareStringOrdinal(part, 3, L"COM", 3, TRUE) == CSTR_EQUAL ||
					CompareStringOrdinal(part, 3, L"LPT", 3, TRUE) == CSTR_EQUAL))
				return FALSE;
			if (!c)
				return TRUE;
			part = at + 1;
		} else if (c < 32 || c == L':' || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c == L'?' || c == L'*') {
			return FALSE;
		}
	}
}

static BOOL join_path(WCHAR *path, const WCHAR *directory, const WCHAR *relative)
{
	const int length = lstrlenW(directory);
	if (length + 1 + lstrlenW(relative) >= MAX_PATH)
		return FALSE;
	lstrcpyW(path, directory);
	path[length] = L'\\';
	lstrcpyW(path + length + 1, relative);
	for (WCHAR *at = path + length + 1; *at; ++at)
		if (*at == L'/')
			*at = L'\\';
	return TRUE;
}

static HRESULT write_bytes(HANDLE file, const void *bytes, DWORD size)
{
	DWORD written;
	if (!WriteFile(file, bytes, size, &written, NULL))
		return file_error();
	return written == size ? S_OK : HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
}

static HRESULT write_record(HANDLE file, const WCHAR *value, char delimiter)
{
	char utf8[4 * MAX_PATH];
	const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, utf8, sizeof utf8, NULL, NULL);
	if (!length)
		return file_error();
	utf8[length - 1] = delimiter;
	return write_bytes(file, utf8, length);
}

static HRESULT extract_file(IStream *stream, const WCHAR *stage, const WCHAR *name, BYTE *buffer, struct winrt_async *operation)
{
	WCHAR path[MAX_PATH];
	if (!safe_name(name, TRUE) || !join_path(path, stage, name))
		return APPX_E_CORRUPT_CONTENT;
	for (WCHAR *at = path + lstrlenW(stage) + 1; *at; ++at) {
		if (*at != L'\\')
			continue;
		*at = 0;
		if (!CreateDirectoryW(path, NULL)) {
			const DWORD error = GetLastError();
			const DWORD attributes = GetFileAttributesW(path);
			if (error != ERROR_ALREADY_EXISTS || attributes == INVALID_FILE_ATTRIBUTES ||
				(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != FILE_ATTRIBUTE_DIRECTORY)
				return HRESULT_FROM_WIN32(error);
		}
		*at = L'\\';
	}
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return file_error();
	HRESULT result = S_OK;
	for (;;) {
		ULONG count = 0;
		if (winrt_async_canceled(operation)) {
			result = E_ABORT;
			break;
		}
		result = CALL(stream, Read, buffer, 65536, &count);
		if (FAILED(result) || !count)
			break;
		result = write_bytes(file, buffer, count);
		if (FAILED(result))
			break;
	}
	CloseHandle(file);
	return SUCCEEDED(result) ? S_OK : result;
}

static void remove_stage(const WCHAR *directory)
{
	WCHAR path[MAX_PATH];
	if (!join_path(path, directory, L"*"))
		return;
	WIN32_FIND_DATAW entry;
	HANDLE files = FindFirstFileW(path, &entry);
	if (files != INVALID_HANDLE_VALUE) {
		do {
			if (!lstrcmpW(entry.cFileName, L".") || !lstrcmpW(entry.cFileName, L"..") ||
				!join_path(path, directory, entry.cFileName))
				continue;
			if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
					RemoveDirectoryW(path);
				else
					remove_stage(path);
			} else {
				DeleteFileW(path);
			}
		} while (FindNextFileW(files, &entry));
		FindClose(files);
	}
	RemoveDirectoryW(directory);
}

static HRESULT extract_package(IAppxPackageReader *reader, const WCHAR *stage, struct winrt_async *operation)
{
	BYTE *buffer = WINRT_ALLOC(65536);
	if (!buffer)
		return E_OUTOFMEMORY;
	IAppxFilesEnumerator *files = NULL;
	HRESULT result = CALL(reader, GetPayloadFiles, &files);
	BOOL present = FALSE;
	if (SUCCEEDED(result))
		result = CALL(files, GetHasCurrent, &present);
	while (SUCCEEDED(result) && present) {
		IAppxFile *file = NULL;
		WCHAR *name = NULL;
		IStream *stream = NULL;
		result = CALL(files, GetCurrent, &file);
		if (SUCCEEDED(result))
			result = CALL(file, GetName, &name);
		if (SUCCEEDED(result))
			result = CALL(file, GetStream, &stream);
		if (SUCCEEDED(result))
			result = extract_file(stream, stage, name, buffer, operation);
		CoTaskMemFree(name);
		winrt_put(stream);
		winrt_put(file);
		if (SUCCEEDED(result))
			result = CALL(files, MoveNext, &present);
	}
	IAppxFile *manifest = NULL;
	IStream *stream = NULL;
	if (SUCCEEDED(result))
		result = CALL(reader, GetFootprintFile, APPX_FOOTPRINT_FILE_TYPE_MANIFEST, &manifest);
	if (SUCCEEDED(result))
		result = CALL(manifest, GetStream, &stream);
	if (SUCCEEDED(result))
		result = extract_file(stream, stage, L"AppxManifest.xml", buffer, operation);
	winrt_put(stream);
	winrt_put(manifest);
	IAppxFile *map = NULL;
	stream = NULL;
	if (SUCCEEDED(result))
		result = CALL(reader, GetFootprintFile, APPX_FOOTPRINT_FILE_TYPE_BLOCKMAP, &map);
	if (SUCCEEDED(result))
		result = CALL(map, GetStream, &stream);
	if (SUCCEEDED(result))
		result = extract_file(stream, stage, L"AppxBlockMap.xml", buffer, operation);
	winrt_put(stream);
	winrt_put(map);
	winrt_put(files);
	WINRT_FREE(buffer);
	return result;
}

static HRESULT write_applications(IAppxManifestReader *manifest, const WCHAR *stage, HANDLE record, const WCHAR *family)
{
	IAppxManifestApplicationsEnumerator *applications = NULL;
	HRESULT result = CALL(manifest, GetApplications, &applications);
	BOOL present = FALSE, launchable = FALSE;
	if (SUCCEEDED(result))
		result = CALL(applications, GetHasCurrent, &present);
	while (SUCCEEDED(result) && present) {
		IAppxManifestApplication *app = NULL;
		WCHAR *name = NULL, *aumid = NULL;
		result = CALL(applications, GetCurrent, &app);
		/* An application starts as a program: a full-trust desktop program, or an app of the Windows Runtime whose program runs its view
		 * with CoreApplication. One made only of web pages names no program and cannot start. */
		if (SUCCEEDED(result))
			result = CALL(app, GetStringValue, L"Executable", &name);
		if (SUCCEEDED(result) && name) {
			result = CALL(app, GetAppUserModelId, &aumid);
			WCHAR path[MAX_PATH];
			const int prefix = lstrlenW(family);
			if (SUCCEEDED(result) && (!safe_name(name, TRUE) || !join_path(path, stage, name) || !safe_name(aumid, FALSE) ||
					lstrlenW(aumid) <= prefix + 1 || aumid[prefix] != L'!' ||
					CompareStringOrdinal(aumid, prefix, family, prefix, TRUE) != CSTR_EQUAL))
				result = APPX_E_INVALID_MANIFEST;
			if (SUCCEEDED(result)) {
				DWORD attributes = GetFileAttributesW(path);
				if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
					result = APPX_E_INVALID_MANIFEST;
			}
			if (SUCCEEDED(result))
				result = write_record(record, aumid, '\t');
			if (SUCCEEDED(result))
				result = write_record(record, name, '\n');
			launchable = SUCCEEDED(result);
		}
		CoTaskMemFree(aumid);
		CoTaskMemFree(name);
		winrt_put(app);
		if (SUCCEEDED(result))
			result = CALL(applications, MoveNext, &present);
	}
	winrt_put(applications);
	return SUCCEEDED(result) && !launchable ? HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) : result;
}

HRESULT install_msix(IAppxPackageReader *reader, struct winrt_async *operation)
{
	IAppxManifestReader *manifest = NULL;
	IAppxManifestPackageId *id = NULL;
	WCHAR *name = NULL, *family = NULL, *publisher = NULL, *resource = NULL;
	HRESULT result = CALL(reader, GetManifest, &manifest);
	if (SUCCEEDED(result))
		result = CALL(manifest, GetPackageId, &id);
	if (SUCCEEDED(result))
		result = CALL(id, GetPackageFullName, &name);
	if (SUCCEEDED(result))
		result = CALL(id, GetPackageFamilyName, &family);
	if (SUCCEEDED(result))
		result = CALL(id, GetPublisher, &publisher);
	if (SUCCEEDED(result))
		result = CALL(id, GetResourceId, &resource);
	if (SUCCEEDED(result) && (!safe_name(name, FALSE) || !safe_name(family, FALSE) || !publisher || !*publisher ||
			(resource && *resource && !safe_name(resource, FALSE))))
		result = APPX_E_INVALID_MANIFEST;

	WCHAR stage[MAX_PATH], temp[MAX_PATH], random[64] = L".raven-msix-";
	BOOL staged = FALSE;
	if (SUCCEEDED(result)) {
		GUID guid;
		result = CoCreateGuid(&guid);
		DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
		if (SUCCEEDED(result) && (!length || length >= ARRAYSIZE(temp) || !StringFromGUID2(&guid, random + 12, 52)))
			result = E_FAIL;
		if (SUCCEEDED(result) && temp[length - 1] == L'\\')
			temp[length - 1] = 0;
		if (SUCCEEDED(result) && !join_path(stage, temp, random))
			result = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
		if (SUCCEEDED(result)) {
			staged = CreateDirectoryW(stage, NULL);
			if (!staged)
				result = file_error();
		}
	}
	if (SUCCEEDED(result))
		result = extract_package(reader, stage, operation);
	if (SUCCEEDED(result)) {
		WCHAR path[MAX_PATH];
		HANDLE record = join_path(path, stage, RECORD) ? CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL)
			: INVALID_HANDLE_VALUE;
		if (record == INVALID_HANDLE_VALUE)
			result = E_FAIL;
		else {
			result = write_record(record, family, '\n');
			if (SUCCEEDED(result))
				result = write_record(record, publisher, '\n');
			if (SUCCEEDED(result))
				result = write_record(record, resource ? resource : L"", '\n');
			if (SUCCEEDED(result))
				result = write_applications(manifest, stage, record, family);
			CloseHandle(record);
		}
	}
	if (SUCCEEDED(result)) {
		typedef HRESULT (WINAPI *publish_package)(const WCHAR *, const WCHAR *);
		HMODULE host = GetModuleHandleW(L"ravenhost.dll");
		publish_package publish = host ? (publish_package)GetProcAddress(host, "RavenPublishPackage") : NULL;
		result = winrt_async_canceled(operation) ? E_ABORT : publish ? publish(stage, name) : E_FAIL;
	}
	if (staged)
		remove_stage(stage);
	CoTaskMemFree(resource);
	CoTaskMemFree(publisher);
	CoTaskMemFree(family);
	CoTaskMemFree(name);
	winrt_put(id);
	winrt_put(manifest);
	return result;
}
