/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "setup.h"
#include <string.h>

/* Where an INF says files live and go: [SourceDisksNames], [SourceDisksFiles], [DestinationDirs],
 * with the platform decorated forms taking precedence. */

#if defined(_M_AMD64) || defined(__x86_64__)
#define PLATFORM_SUFFIX L".amd64"
#else
#define PLATFORM_SUFFIX L".x86"
#endif

static BOOL find_decorated(HINF handle, const WCHAR *base, const WCHAR *key, INFCONTEXT *context)
{
	WCHAR name[64];

	lstrcpyW(name, base);
	lstrcatW(name, PLATFORM_SUFFIX);
	if (SetupFindFirstLineW(handle, name, key, context))
		return TRUE;
	return SetupFindFirstLineW(handle, base, key, context);
}

BOOL setup_source_location(HINF handle, const WCHAR *file, UINT *id, WCHAR **subdir)
{
	INFCONTEXT context;
	INT value = 0;
	const WCHAR *path;

	if (!find_decorated(handle, L"SourceDisksFiles", file, &context)) {
		SetLastError(ERROR_LINE_NOT_FOUND);
		return FALSE;
	}
	if (!SetupGetIntField(&context, 1, &value)) {
		SetLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	*id = value;
	path = SetupGetFieldCount(&context) >= 2 ? pSetupGetField(&context, 2) : NULL;
	if (subdir)
		*subdir = setup_copy_wide(path ? path : L"");
	SetLastError(0);
	return TRUE;
}

WCHAR *setup_source_info(HINF handle, UINT id, UINT what)
{
	WCHAR key[16];
	INFCONTEXT context;
	const WCHAR *field;
	DWORD index;

	wsprintfW(key, L"%u", id);
	if (!find_decorated(handle, L"SourceDisksNames", key, &context)) {
		SetLastError(ERROR_LINE_NOT_FOUND);
		return NULL;
	}
	switch (what) {
	case SRCINFO_DESCRIPTION:
		index = 1;
		break;
	case SRCINFO_TAGFILE:
		index = 2;
		break;
	case SRCINFO_PATH:
		index = 4;
		break;
	case SRCINFO_TAGFILE2:
		index = 5;
		break;
	case SRCINFO_FLAGS:
		index = 3;
		break;
	default:
		SetLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	field = SetupGetFieldCount(&context) >= index ? pSetupGetField(&context, index) : NULL;
	return setup_copy_wide(field ? field : L"");
}

BOOL WINAPI SetupGetSourceFileLocationW(HINF handle, PINFCONTEXT line, PCWSTR file, PUINT id, PWSTR buffer, DWORD size, PDWORD required)
{
	const WCHAR *name = file;
	WCHAR *subdir = NULL;
	BOOL result;

	/* A context names the file by the line's target field, whichever name the caller passed. */
	if (line) {
		name = pSetupGetField(line, 1);
		if (!name) {
			SetLastError(ERROR_LINE_NOT_FOUND);
			return FALSE;
		}
	}
	if (!name) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!setup_source_location(handle, name, id, &subdir))
		return FALSE;
	result = setup_give_wide(subdir, buffer, size, required);
	setup_free(subdir);
	if (result)
		SetLastError(0);
	return result;
}

BOOL WINAPI SetupGetSourceFileLocationA(HINF handle, PINFCONTEXT line, PCSTR file, PUINT id, PSTR buffer, DWORD size, PDWORD required)
{
	WCHAR *wide = setup_to_wide(file), text[MAX_PATH];
	BOOL result = SetupGetSourceFileLocationW(handle, line, wide, id, text, ARRAYSIZE(text), NULL);

	setup_free(wide);
	return setup_wide_to_ansi_result(result, text, buffer, size, required);
}

BOOL WINAPI SetupGetSourceFileSizeW(HINF handle, PINFCONTEXT line, PCWSTR file, PCWSTR section, PDWORD size, UINT rounding)
{
	const WCHAR *name = file;
	INFCONTEXT context;
	INT value;

	if (line)
		name = pSetupGetField(line, 1);
	if (!name || !find_decorated(handle, L"SourceDisksFiles", name, &context)) {
		SetLastError(ERROR_LINE_NOT_FOUND);
		return FALSE;
	}
	if (!SetupGetIntField(&context, 3, &value)) {
		SetLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	*size = rounding ? (((DWORD)value + rounding - 1) / rounding) * rounding : (DWORD)value;
	return TRUE;
}

BOOL WINAPI SetupGetSourceFileSizeA(HINF handle, PINFCONTEXT line, PCSTR file, PCSTR section, PDWORD size, UINT rounding)
{
	WCHAR *wide_file = setup_to_wide(file), *wide_section = setup_to_wide(section);
	BOOL result = SetupGetSourceFileSizeW(handle, line, wide_file, wide_section, size, rounding);

	setup_free(wide_file);
	setup_free(wide_section);
	return result;
}

BOOL WINAPI SetupGetSourceInfoW(HINF handle, UINT id, UINT what, PWSTR buffer, DWORD size, PDWORD required)
{
	WCHAR *text = setup_source_info(handle, id, what);
	BOOL result;

	if (!text)
		return FALSE;
	result = setup_give_wide(text, buffer, size, required);
	setup_free(text);
	return result;
}

BOOL WINAPI SetupGetSourceInfoA(HINF handle, UINT id, UINT what, PSTR buffer, DWORD size, PDWORD required)
{
	WCHAR *text = setup_source_info(handle, id, what);
	BOOL result;

	if (!text)
		return FALSE;
	result = setup_wide_to_ansi_result(TRUE, text, buffer, size, required);
	setup_free(text);
	return result;
}

/* A section name of NULL means the default. Without any entry, files go to the system directory. */
WCHAR *setup_section_target(HINF handle, const WCHAR *section)
{
	INFCONTEXT context;
	WCHAR *directory;

	if ((section && SetupFindFirstLineW(handle, L"DestinationDirs", section, &context)) ||
			SetupFindFirstLineW(handle, L"DestinationDirs", L"DefaultDestDir", &context)) {
		directory = setup_directory_from_line(handle, &context, 1);
		if (directory)
			return directory;
	}
	return setup_dir_id_path(handle, DIRID_DEFAULT);
}

BOOL WINAPI SetupGetTargetPathW(HINF handle, PINFCONTEXT line, PCWSTR section, PWSTR buffer, DWORD size, PDWORD required)
{
	WCHAR *directory = NULL;
	BOOL result;

	if (line) {
		INFCONTEXT *context = line;
		const WCHAR *line_section = NULL;
		WCHAR name[256];
		UINT at = context->Section, length;
		/* The context says which section; the sections are enumerable by the same index. */
		if (SetupEnumInfSectionsW(context->CurrentInf, at, name, ARRAYSIZE(name), &length))
			line_section = name;
		directory = setup_section_target(handle, line_section);
	} else {
		directory = setup_section_target(handle, section);
	}
	if (!directory) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	result = setup_give_wide(directory, buffer, size, required);
	setup_free(directory);
	if (result)
		SetLastError(0);
	return result;
}

BOOL WINAPI SetupGetTargetPathA(HINF handle, PINFCONTEXT line, PCSTR section, PSTR buffer, DWORD size, PDWORD required)
{
	WCHAR *wide = setup_to_wide(section), text[MAX_PATH * 2];
	BOOL result = SetupGetTargetPathW(handle, line, wide, text, ARRAYSIZE(text), NULL);

	setup_free(wide);
	return setup_wide_to_ansi_result(result, text, buffer, size, required);
}

/* Information about INF files as a block: the header, then each file's name and its [Version]. */

static WCHAR *flat_version(HINF handle, DWORD *chars)
{
	WCHAR *text = NULL;
	SIZE_T used = 0;
	INFCONTEXT context;

	if (SetupFindFirstLineW(handle, L"Version", NULL, &context)) {
		do {
			const WCHAR *key = pSetupGetField(&context, 0), *value = pSetupGetField(&context, 1);
			SIZE_T key_length = key ? lstrlenW(key) : 0, value_length = value ? lstrlenW(value) : 0;
			WCHAR *grown;

			if (!key)
				continue;
			grown = setup_realloc(text, (used + key_length + value_length + 3) * sizeof(WCHAR));
			if (!grown)
				break;
			text = grown;
			memcpy(text + used, key, (key_length + 1) * sizeof(WCHAR));
			used += key_length + 1;
			if (value_length)
				memcpy(text + used, value, value_length * sizeof(WCHAR));
			used += value_length + 1;
		} while (SetupFindNextLine(&context, &context));
	}
	if (!text)
		text = setup_alloc(sizeof(WCHAR) * 2);
	else
		used++;
	*chars = (DWORD)used;
	return text;
}

/* The block stores names as wide text; the offset table starts it. */
static BOOL fill_information(HINF handle, PSP_INF_INFORMATION buffer, DWORD size, DWORD *required)
{
	const WCHAR *path = setup_inf_path(handle);
	DWORD version_chars = 0, name_chars = lstrlenW(path) + 1, total;
	WCHAR *version = flat_version(handle, &version_chars);
	BYTE *out;

	if (!version) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	total = FIELD_OFFSET(SP_INF_INFORMATION, VersionData) + (name_chars + version_chars) * sizeof(WCHAR);
	if (required)
		*required = total;
	if (!buffer) {
		setup_free(version);
		if (size >= total) {
			SetLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		return TRUE;
	}
	if (size < total) {
		setup_free(version);
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	buffer->InfStyle = INF_STYLE_WIN4;
	buffer->InfCount = 1;
	out = buffer->VersionData;
	memcpy(out, path, name_chars * sizeof(WCHAR));
	memcpy(out + name_chars * sizeof(WCHAR), version, version_chars * sizeof(WCHAR));
	setup_free(version);
	return TRUE;
}

BOOL WINAPI SetupGetInfInformationW(LPCVOID spec, DWORD search, PSP_INF_INFORMATION buffer, DWORD size, PDWORD required)
{
	HINF handle = INVALID_HANDLE_VALUE;
	BOOL opened = FALSE, result;
	WCHAR full[MAX_PATH * 2], directory[MAX_PATH];

	if (!spec) {
		SetLastError(search == INFINFO_INF_SPEC_IS_HINF ? ERROR_INVALID_HANDLE : ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	switch (search) {
	case INFINFO_INF_SPEC_IS_HINF:
		handle = (HINF)spec;
		break;
	case INFINFO_INF_NAME_IS_ABSOLUTE:
		lstrcpynW(full, spec, ARRAYSIZE(full));
		break;
	case INFINFO_DEFAULT_SEARCH:
	case INFINFO_REVERSE_DEFAULT_SEARCH:
	case INFINFO_INF_PATH_LIST_SEARCH: {
		const WCHAR *name = spec;
		WCHAR *first, *second;

		GetWindowsDirectoryW(directory, MAX_PATH);
		first = setup_join_path(directory, L"inf");
		GetSystemDirectoryW(directory, MAX_PATH);
		second = setup_copy_wide(directory);
		if (search == INFINFO_REVERSE_DEFAULT_SEARCH) {
			WCHAR *swap = first;
			first = second;
			second = swap;
		}
		{
			WCHAR *one = setup_join_path(first, name), *two = setup_join_path(second, name);
			if (GetFileAttributesW(one) != INVALID_FILE_ATTRIBUTES)
				lstrcpynW(full, one, ARRAYSIZE(full));
			else
				lstrcpynW(full, two, ARRAYSIZE(full));
			setup_free(one);
			setup_free(two);
		}
		setup_free(first);
		setup_free(second);
		break;
	}
	default:
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (handle == INVALID_HANDLE_VALUE) {
		handle = SetupOpenInfFileW(full, NULL, INF_STYLE_WIN4 | INF_STYLE_OLDNT, NULL);
		if (handle == INVALID_HANDLE_VALUE)
			return FALSE;
		opened = TRUE;
	}
	result = fill_information(handle, buffer, size, required);
	if (opened)
		SetupCloseInfFile(handle);
	return result;
}

BOOL WINAPI SetupGetInfInformationA(LPCVOID spec, DWORD search, PSP_INF_INFORMATION buffer, DWORD size, PDWORD required)
{
	WCHAR *wide = search == INFINFO_INF_SPEC_IS_HINF ? NULL : setup_to_wide(spec);
	BOOL result = SetupGetInfInformationW(search == INFINFO_INF_SPEC_IS_HINF ? spec : wide, search, buffer, size, required);

	setup_free(wide);
	return result;
}

BOOL WINAPI SetupQueryInfFileInformationW(PSP_INF_INFORMATION info, UINT index, PWSTR buffer, DWORD size, PDWORD required)
{
	if (!info || index >= info->InfCount) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return setup_give_wide((const WCHAR *)info->VersionData, buffer, size, required);
}

BOOL WINAPI SetupQueryInfFileInformationA(PSP_INF_INFORMATION info, UINT index, PSTR buffer, DWORD size, PDWORD required)
{
	if (!info || index >= info->InfCount) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return setup_wide_to_ansi_result(TRUE, (const WCHAR *)info->VersionData, buffer, size, required);
}

BOOL WINAPI SetupQueryInfVersionInformationW(PSP_INF_INFORMATION info, UINT index, PCWSTR key, PWSTR buffer, DWORD size, PDWORD required)
{
	const WCHAR *at;

	if (!info || index >= info->InfCount) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	at = (const WCHAR *)info->VersionData;
	at += lstrlenW(at) + 1;
	while (*at) {
		const WCHAR *value = at + lstrlenW(at) + 1;
		if (key && !lstrcmpiW(at, key))
			return setup_give_wide(value, buffer, size, required);
		at = value + lstrlenW(value) + 1;
	}
	SetLastError(ERROR_INVALID_DATA);
	return FALSE;
}

BOOL WINAPI SetupQueryInfVersionInformationA(PSP_INF_INFORMATION info, UINT index, PCSTR key, PSTR buffer, DWORD size, PDWORD required)
{
	WCHAR *wide = setup_to_wide(key), text[MAX_PATH * 2];
	BOOL result = SetupQueryInfVersionInformationW(info, index, wide, text, ARRAYSIZE(text), NULL);

	setup_free(wide);
	return setup_wide_to_ansi_result(result, text, buffer, size, required);
}

BOOL WINAPI SetupQueryInfOriginalFileInformationW(PSP_INF_INFORMATION info, UINT index, PSP_ALTPLATFORM_INFO platform, PSP_ORIGINAL_FILE_INFO_W original)
{
	const WCHAR *path, *name;

	if (!info || index >= info->InfCount || !original || original->cbSize != sizeof(*original)) {
		SetLastError(ERROR_INVALID_USER_BUFFER);
		return FALSE;
	}
	path = (const WCHAR *)info->VersionData;
	name = wcsrchr(path, '\\');
	lstrcpynW(original->OriginalInfName, name ? name + 1 : path, MAX_PATH);
	original->OriginalCatalogName[0] = 0;
	return TRUE;
}

BOOL WINAPI SetupQueryInfOriginalFileInformationA(PSP_INF_INFORMATION info, UINT index, PSP_ALTPLATFORM_INFO platform, PSP_ORIGINAL_FILE_INFO_A original)
{
	SP_ORIGINAL_FILE_INFO_W wide;

	if (!original || original->cbSize != sizeof(*original)) {
		SetLastError(ERROR_INVALID_USER_BUFFER);
		return FALSE;
	}
	wide.cbSize = sizeof(wide);
	if (!SetupQueryInfOriginalFileInformationW(info, index, platform, &wide))
		return FALSE;
	WideCharToMultiByte(CP_ACP, 0, wide.OriginalInfName, -1, original->OriginalInfName, MAX_PATH, NULL, NULL);
	original->OriginalCatalogName[0] = 0;
	return TRUE;
}

BOOL WINAPI SetupGetInfFileListW(PCWSTR directory, DWORD style, PWSTR buffer, DWORD size, PDWORD required)
{
	WCHAR windows[MAX_PATH], *pattern, *base = NULL;
	WIN32_FIND_DATAW found;
	HANDLE search;
	DWORD total = 1, used = 0;

	if (!(style & (INF_STYLE_OLDNT | INF_STYLE_WIN4))) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!directory) {
		GetWindowsDirectoryW(windows, MAX_PATH);
		base = setup_join_path(windows, L"inf");
		directory = base;
	}
	{
		DWORD attributes = GetFileAttributesW(directory);

		if (!*directory || (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))) {
			setup_free(base);
			SetLastError(ERROR_DIRECTORY);
			return FALSE;
		}
		if (attributes == INVALID_FILE_ATTRIBUTES) {
			setup_free(base);
			if (required)
				*required = 1;
			if (buffer && size)
				buffer[0] = 0;
			SetLastError(ERROR_PATH_NOT_FOUND);
			return TRUE;
		}
	}
	pattern = setup_join_path(directory, L"*.inf");
	search = FindFirstFileW(pattern, &found);
	setup_free(pattern);
	if (search != INVALID_HANDLE_VALUE) {
		do {
			WCHAR *full = setup_join_path(directory, found.cFileName);
			HINF handle = full ? SetupOpenInfFileW(full, NULL, INF_STYLE_OLDNT | INF_STYLE_WIN4, NULL) : INVALID_HANDLE_VALUE;
			setup_free(full);
			if (handle != INVALID_HANDLE_VALUE) {
				DWORD length = lstrlenW(found.cFileName) + 1;
				BOOL wanted = style & (setup_inf_is_win4(handle) ? INF_STYLE_WIN4 : INF_STYLE_OLDNT);
				SetupCloseInfFile(handle);
				if (wanted) {
					if (buffer && used + length + 1 <= size)
						memcpy(buffer + used, found.cFileName, length * sizeof(WCHAR));
					used += length;
					total += length;
				}
			}
		} while (FindNextFileW(search, &found));
		FindClose(search);
	}
	setup_free(base);
	if (required)
		*required = total;
	if (buffer && size < total) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	if (buffer)
		buffer[used] = 0;
	SetLastError(0);
	return TRUE;
}

BOOL WINAPI SetupGetInfFileListA(PCSTR directory, DWORD style, PSTR buffer, DWORD size, PDWORD required)
{
	WCHAR *wide = setup_to_wide(directory), *list = NULL;
	DWORD chars = 0, ansi;
	BOOL result = SetupGetInfFileListW(wide, style, NULL, 0, &chars);

	if (result) {
		list = setup_alloc((SIZE_T)chars * sizeof(WCHAR));
		result = list && SetupGetInfFileListW(wide, style, list, chars, &chars);
	}
	setup_free(wide);
	if (!result) {
		setup_free(list);
		return FALSE;
	}
	ansi = WideCharToMultiByte(CP_ACP, 0, list, chars, NULL, 0, NULL, NULL);
	if (required)
		*required = ansi;
	if (buffer) {
		if (size < ansi) {
			setup_free(list);
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		WideCharToMultiByte(CP_ACP, 0, list, chars, buffer, size, NULL, NULL);
	}
	setup_free(list);
	return TRUE;
}
