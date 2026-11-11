/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven shell component: SHGetFileInfo, what the shell shows of a file, a folder or a drive.
 *
 * A file manager lists its items with the shell's image lists: it asks for the list once and for each item only the index of its picture
 * in it (SHGFI_SYSICONINDEX). The lists are the pictures the file dialog draws, so a folder, a file and a drive look the same everywhere.
 * A type is named as the class registered for the extension names it; an extension nothing claims is named as Windows names it ("TXT
 * File"). */
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include "shell.h"

static int kind_of(const WCHAR *path, BOOL directory)
{
	const size_t length = lstrlenW(path);
	if (length >= 2 && length <= 3 && path[1] == L':' && (length == 2 || path[2] == L'\\' || path[2] == L'/'))
		return ICON_DRIVE;
	return directory ? ICON_FOLDER : ICON_FILE;
}

/* The name of a path's last part; a drive keeps its root. */
static void display_name(const WCHAR *path, WCHAR *out, size_t capacity)
{
	WCHAR copy[MAX_PATH];
	size_t length;
	copy_text(copy, MAX_PATH, path);
	length = lstrlenW(copy);
	while (length > 3 && (copy[length - 1] == L'\\' || copy[length - 1] == L'/'))
		copy[--length] = 0;
	copy_text(out, capacity, length > 3 ? file_part(copy) : copy);
}

/* The registered name of a class: the default value of its key under HKEY_CLASSES_ROOT. */
static BOOL class_name(const WCHAR *key, WCHAR *out, DWORD bytes)
{
	DWORD type = 0;
	return RegGetValueW(HKEY_CLASSES_ROOT, key, NULL, RRF_RT_REG_SZ, &type, out, &bytes) == ERROR_SUCCESS && out[0];
}

/* The pictures a registered type names in its DefaultIcon ("C:\Program Files\7-Zip\7z.dll,0"), added to both image lists at the same
   index the first time a file of the type is shown, as Windows adds them to the system image list. */
#define TYPE_ICONS 128

static struct { WCHAR source[MAX_PATH]; int index, image; } type_icons[TYPE_ICONS];
static int type_icon_count;
static SRWLOCK type_icon_lock = SRWLOCK_INIT;

static int registered_icon(const WCHAR *program)
{
	WCHAR key[MAX_PATH + 16], value[MAX_PATH + 16], *comma = NULL, *at;
	int index = 0, image = -1, slot;
	HICON large = NULL, small = NULL;
	if (lstrlenW(program) >= MAX_PATH)
		return -1;
	lstrcpyW(key, program);
	lstrcatW(key, L"\\DefaultIcon");
	if (!class_name(key, value, sizeof value))
		return -1;
	for (at = value; *at; ++at)
		if (*at == L',')
			comma = at;
	if (comma) {
		*comma = 0;
		const WCHAR *digit = comma + 1;
		const BOOL negative = *digit == L'-';
		for (digit += negative; *digit >= L'0' && *digit <= L'9'; ++digit)
			index = index * 10 + (*digit - L'0');
		if (negative)
			index = -index;
	}
	/* A path the registry quotes is the same path. */
	if (value[0] == L'"') {
		size_t length = lstrlenW(value);
		if (length > 1 && value[length - 1] == L'"')
			value[length - 1] = 0;
		MoveMemory(value, value + 1, lstrlenW(value) * sizeof(WCHAR));
	}
	AcquireSRWLockExclusive(&type_icon_lock);
	for (slot = 0; slot < type_icon_count; ++slot)
		if (type_icons[slot].index == index && !lstrcmpiW(type_icons[slot].source, value)) {
			image = type_icons[slot].image;
			break;
		}
	if (slot == type_icon_count && type_icon_count < TYPE_ICONS && shell_icons(value, index, &large, &small, 1) == 1 && large && small) {
		int (WINAPI *replace)(HIMAGELIST, int, HICON) = (void *)GetProcAddress(LoadLibraryW(L"comctl32.dll"), "ImageList_ReplaceIcon");
		HIMAGELIST big = system_icons(FALSE), little = system_icons(TRUE);
		if (replace && big && little) {
			image = replace(big, -1, large);
			if (image < 0 || replace(little, -1, small) != image)
				image = -1;
		}
		if (image >= 0) {
			lstrcpyW(type_icons[slot].source, value);
			type_icons[slot].index = index;
			type_icons[slot].image = image;
			++type_icon_count;
		}
	}
	if (large)
		DestroyIcon(large);
	if (small)
		DestroyIcon(small);
	ReleaseSRWLockExclusive(&type_icon_lock);
	return image;
}

/* The picture of a file: its type's registered one, or the plain page. */
static int file_image(const WCHAR *name)
{
	const WCHAR *dot = NULL, *at;
	WCHAR program[MAX_PATH];
	int image;
	for (at = name; *at; ++at)
		if (*at == L'.')
			dot = at;
	if (!dot || !dot[1] || !class_name(dot, program, sizeof program))
		return ICON_FILE;
	image = registered_icon(program);
	return image >= 0 ? image : ICON_FILE;
}

static void type_name(const WCHAR *name, int kind, WCHAR *out, size_t capacity)
{
	const WCHAR *dot = NULL, *at;
	WCHAR program[MAX_PATH], text[80];
	size_t used = 0;
	if (kind != ICON_FILE) {
		copy_text(out, capacity, kind == ICON_DRIVE ? L"Local Disk" : L"File folder");
		return;
	}
	for (at = name; *at; ++at)
		if (*at == L'.')
			dot = at;
	if (!dot || !dot[1]) {
		copy_text(out, capacity, L"File");
		return;
	}
	if (class_name(dot, program, sizeof program) && class_name(program, text, sizeof text)) {
		copy_text(out, capacity, text);
		return;
	}
	for (at = dot + 1; *at && used + 6 < 80; ++at)
		text[used++] = *at >= L'a' && *at <= L'z' ? (WCHAR)(*at - 32) : *at;
	lstrcpyW(text + used, L" File");
	copy_text(out, capacity, text);
}

DWORD_PTR WINAPI SHGetFileInfoW(LPCWSTR path, DWORD attributes, SHFILEINFOW *info, UINT size, UINT flags)
{
	WCHAR resolved[MAX_PATH];
	HIMAGELIST list = NULL;
	BOOL directory;
	int kind;
	if (!path)
		return 0;
	if (flags & SHGFI_PIDL) {
		/* The host reads the item list form Raven's shell builds. */
		BOOL (WINAPI *path_of)(PCIDLIST_ABSOLUTE, LPWSTR) = (void *)GetProcAddress(LoadLibraryW(L"shell32.dll"), "SHGetPathFromIDListW");
		if (!path_of || !path_of((PCIDLIST_ABSOLUTE)path, resolved))
			return 0;
		path = resolved;
	}
	if (flags & SHGFI_USEFILEATTRIBUTES) {
		directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	} else {
		const DWORD found = GetFileAttributesW(path);
		if (found == INVALID_FILE_ATTRIBUTES)
			return 0;
		directory = (found & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}
	kind = kind_of(path, directory);
	if (flags & (SHGFI_SYSICONINDEX | SHGFI_ICON)) {
		list = system_icons((flags & SHGFI_SMALLICON) != 0);
		if (!list)
			return 0;
	}
	if (!info)
		return (flags & SHGFI_SYSICONINDEX) ? (DWORD_PTR)list : 0;
	if (flags & (SHGFI_SYSICONINDEX | SHGFI_ICON)) {
		WCHAR name[MAX_PATH];
		display_name(path, name, MAX_PATH);
		info->iIcon = kind == ICON_FILE ? file_image(name) : kind;
	}
	if (flags & SHGFI_ICON) {
		HICON (WINAPI *get_icon)(HIMAGELIST, int, UINT) = (void *)GetProcAddress(LoadLibraryW(L"comctl32.dll"), "ImageList_GetIcon");
		info->hIcon = get_icon ? get_icon(list, info->iIcon, ILD_NORMAL) : NULL;
	}
	if (flags & SHGFI_ATTRIBUTES)
		/* Part of the file system, can be copied, moved, linked, renamed and deleted; a folder also holds other items. */
		info->dwAttributes = SFGAO_FILESYSTEM | SFGAO_CANCOPY | SFGAO_CANMOVE | SFGAO_CANLINK | SFGAO_CANRENAME | SFGAO_CANDELETE |
			(directory ? SFGAO_FOLDER : 0);
	if (flags & SHGFI_DISPLAYNAME)
		display_name(path, info->szDisplayName, MAX_PATH);
	if (flags & SHGFI_TYPENAME) {
		WCHAR name[MAX_PATH];
		display_name(path, name, MAX_PATH);
		type_name(name, kind, info->szTypeName, 80);
	}
	return (flags & SHGFI_SYSICONINDEX) ? (DWORD_PTR)list : 1;
}

DWORD_PTR WINAPI SHGetFileInfoA(LPCSTR path, DWORD attributes, SHFILEINFOA *info, UINT size, UINT flags)
{
	WCHAR wide[MAX_PATH];
	SHFILEINFOW answer;
	DWORD_PTR result;
	if (!path)
		return 0;
	if (flags & SHGFI_PIDL)
		result = SHGetFileInfoW((LPCWSTR)path, attributes, info ? &answer : NULL, sizeof answer, flags);
	else if (!MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH))
		return 0;
	else
		result = SHGetFileInfoW(wide, attributes, info ? &answer : NULL, sizeof answer, flags);
	if (!result || !info)
		return result;
	if (flags & (SHGFI_SYSICONINDEX | SHGFI_ICON))
		info->iIcon = answer.iIcon;
	if (flags & SHGFI_ICON)
		info->hIcon = answer.hIcon;
	if (flags & SHGFI_ATTRIBUTES)
		info->dwAttributes = answer.dwAttributes;
	if (flags & SHGFI_DISPLAYNAME)
		WideCharToMultiByte(CP_ACP, 0, answer.szDisplayName, -1, info->szDisplayName, MAX_PATH, NULL, NULL);
	if (flags & SHGFI_TYPENAME)
		WideCharToMultiByte(CP_ACP, 0, answer.szTypeName, -1, info->szTypeName, 80, NULL, NULL);
	return result;
}
