/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "setup.h"
#include <shlobj.h>
#include <string.h>

void *setup_alloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

void *setup_realloc(void *memory, SIZE_T size)
{
	if (!memory)
		return setup_alloc(size);
	return HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, memory, size);
}

void setup_free(void *memory)
{
	if (memory)
		HeapFree(GetProcessHeap(), 0, memory);
}

WCHAR *setup_to_wide(const char *text)
{
	WCHAR *wide;
	int length;

	if (!text)
		return NULL;
	length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	wide = setup_alloc((SIZE_T)length * sizeof(WCHAR));
	if (wide)
		MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length);
	return wide;
}

char *setup_to_ansi(const WCHAR *text)
{
	char *ansi;
	int length;

	if (!text)
		return NULL;
	length = WideCharToMultiByte(CP_ACP, 0, text, -1, NULL, 0, NULL, NULL);
	ansi = setup_alloc(length);
	if (ansi)
		WideCharToMultiByte(CP_ACP, 0, text, -1, ansi, length, NULL, NULL);
	return ansi;
}

WCHAR *setup_join_path(const WCHAR *left, const WCHAR *right)
{
	SIZE_T left_length = left ? lstrlenW(left) : 0, right_length = right ? lstrlenW(right) : 0;
	WCHAR *path;

	while (right_length && (*right == '\\' || *right == '/')) {
		right++;
		right_length--;
	}
	while (left_length > 1 && right_length && (left[left_length - 1] == '\\' || left[left_length - 1] == '/'))
		left_length--;
	path = setup_alloc((left_length + right_length + 2) * sizeof(WCHAR));
	if (!path)
		return NULL;
	if (left_length)
		memcpy(path, left, left_length * sizeof(WCHAR));
	if (left_length && right_length)
		path[left_length++] = '\\';
	if (right_length)
		memcpy(path + left_length, right, right_length * sizeof(WCHAR));
	return path;
}

BOOL setup_make_directories(const WCHAR *path)
{
	WCHAR *copy = setup_copy_wide(path), *at;
	BOOL result;

	if (!copy)
		return FALSE;
	for (at = copy; *at; at++) {
		if ((*at == '\\' || *at == '/') && at > copy && at[-1] != ':' && at[-1] != '\\') {
			WCHAR saved = *at;
			*at = 0;
			CreateDirectoryW(copy, NULL);
			*at = saved;
		}
	}
	result = CreateDirectoryW(copy, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
	setup_free(copy);
	return result;
}

BOOL setup_give_wide(const WCHAR *text, WCHAR *buffer, DWORD size, DWORD *required)
{
	DWORD length = lstrlenW(text) + 1;

	if (required)
		*required = length;
	if (!buffer)
		return TRUE;
	if (size < length) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	memcpy(buffer, text, length * sizeof(WCHAR));
	return TRUE;
}

BOOL setup_wide_to_ansi_result(BOOL result, const WCHAR *text, char *buffer, DWORD size, DWORD *required)
{
	DWORD length;

	if (!result)
		return FALSE;
	length = WideCharToMultiByte(CP_ACP, 0, text, -1, NULL, 0, NULL, NULL);
	if (required)
		*required = length;
	if (!buffer)
		return TRUE;
	if (size < length) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	WideCharToMultiByte(CP_ACP, 0, text, -1, buffer, size, NULL, NULL);
	return TRUE;
}

/* Directory ids, documented in the INF DestinationDirs reference. */

static WCHAR *windows_child(const WCHAR *child)
{
	WCHAR directory[MAX_PATH];

	GetWindowsDirectoryW(directory, MAX_PATH);
	return setup_join_path(directory, child);
}

static WCHAR *system_child(const WCHAR *child)
{
	WCHAR directory[MAX_PATH];

	GetSystemDirectoryW(directory, MAX_PATH);
	return child ? setup_join_path(directory, child) : setup_copy_wide(directory);
}

WCHAR *setup_dir_id_path(HINF handle, int id)
{
	const WCHAR *custom = handle && handle != INVALID_HANDLE_VALUE ? setup_user_dir_id(handle, (DWORD)id) : NULL;
	WCHAR drive[MAX_PATH];

	if (custom)
		return setup_copy_wide(custom);
	switch (id) {
	case DIRID_NULL:
	case DIRID_ABSOLUTE:
	case DIRID_ABSOLUTE_16BIT:
		return setup_copy_wide(L"");
	case DIRID_SRCPATH:
		return handle ? setup_copy_wide(setup_inf_directory(handle)) : NULL;
	case DIRID_WINDOWS:
		return windows_child(NULL);
	case DIRID_SYSTEM:
		return system_child(NULL);
	case DIRID_DRIVERS:
		return system_child(L"drivers");
	case DIRID_INF:
		return windows_child(L"inf");
	case DIRID_HELP:
		return windows_child(L"help");
	case DIRID_FONTS:
		return windows_child(L"fonts");
	case DIRID_VIEWERS:
		return system_child(L"viewers");
	case DIRID_COLOR:
		return system_child(L"spool\\drivers\\color");
	case DIRID_APPS:
	case DIRID_BOOT:
	case 54:
		GetWindowsDirectoryW(drive, MAX_PATH);
		drive[3] = 0;
		return setup_copy_wide(drive);
	case DIRID_SHARED:
		return windows_child(NULL);
	case DIRID_SYSTEM16:
		return windows_child(L"system");
	case DIRID_SPOOL:
		return system_child(L"spool");
	case DIRID_SPOOLDRIVERS:
		return system_child(L"spool\\drivers");
	case DIRID_USERPROFILE: {
		DWORD length = GetEnvironmentVariableW(L"USERPROFILE", drive, MAX_PATH);
		return length && length < MAX_PATH ? setup_copy_wide(drive) : NULL;
	}
	case DIRID_PRINTPROCESSOR:
		return system_child(L"spool\\prtprocs");
	}
	/* The ids from 16384 name shell folders by their CSIDL. */
	if (id >= 0x4000 && id < DIRID_USER && SUCCEEDED(SHGetFolderPathW(NULL, id - 0x4000, NULL, 0, drive)))
		return setup_copy_wide(drive);
	/* Numbers nobody defined still resolve, to a place no installer would go looking at. */
	if (id >= 0 && id < DIRID_USER)
		return system_child(L"unknown");
	return NULL;
}

BOOL WINAPI SetupSetDirectoryIdW(HINF handle, DWORD id, PCWSTR path)
{
	if (!handle || handle == INVALID_HANDLE_VALUE) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (id && id < DIRID_USER) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!id)
		return TRUE;
	return setup_set_dir_id(handle, id, path ? path : L"");
}

BOOL WINAPI SetupSetDirectoryIdA(HINF handle, DWORD id, PCSTR path)
{
	WCHAR *wide = setup_to_wide(path);
	BOOL result = SetupSetDirectoryIdW(handle, id, wide);

	setup_free(wide);
	return result;
}

BOOL WINAPI SetupSetDirectoryIdExW(HINF handle, DWORD id, PCWSTR path, DWORD flags, DWORD reserved1, PVOID reserved2)
{
	return SetupSetDirectoryIdW(handle, id, path);
}

BOOL WINAPI SetupSetDirectoryIdExA(HINF handle, DWORD id, PCSTR path, DWORD flags, DWORD reserved1, PVOID reserved2)
{
	return SetupSetDirectoryIdA(handle, id, path);
}

WCHAR *setup_directory_from_line(HINF handle, INFCONTEXT *context, DWORD first)
{
	WCHAR *base, *joined;
	const WCHAR *subdir;
	INT id;

	if (!SetupGetIntField(context, first, &id))
		return NULL;
	base = setup_dir_id_path(handle, id);
	/* A user id nobody set up leaves the path relative to the current directory. */
	if (!base)
		base = setup_copy_wide(L"");
	if (!base)
		return NULL;
	subdir = SetupGetFieldCount(context) > first ? pSetupGetField(context, first + 1) : NULL;
	if (!subdir || !*subdir)
		return base;
	joined = setup_join_path(base, subdir);
	setup_free(base);
	return joined;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
