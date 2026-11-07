/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>

/* The most recently used lists: a registry key with one value per item, named by a letter, and "MRUList" holding
 * those letters from the newest to the oldest. The shell's run and search histories are kept this way. */

#define LIST_BINARY 0x0001
#define LIST_CACHEWRITE 0x0002
#define MOST_ITEMS 26

typedef struct {
	DWORD size;
	UINT most;
	UINT flags;
	HKEY key;
	LPCWSTR sub_key;
	void *compare;
} list_info_w;

typedef struct {
	DWORD size;
	UINT most;
	UINT flags;
	HKEY key;
	LPCSTR sub_key;
	void *compare;
} list_info_a;

typedef int (CALLBACK *compare_text_w)(LPCWSTR, LPCWSTR);
typedef int (CALLBACK *compare_text_a)(LPCSTR, LPCSTR);
typedef int (CALLBACK *compare_data)(LPCVOID, LPCVOID, DWORD);

typedef struct {
	BYTE *data;
	DWORD size;
} list_item;

typedef struct {
	UINT most;
	UINT flags;
	HKEY key;
	void *compare;
	BOOL narrow;
	WCHAR order[MOST_ITEMS + 1];
	int count;
	list_item items[MOST_ITEMS];
} recent_list;

static WCHAR *to_wide(LPCSTR text)
{
	int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	WCHAR *wide = HeapAlloc(GetProcessHeap(), 0, length * sizeof(WCHAR));
	if (wide)
		MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length);
	return wide;
}

static char *to_narrow(LPCWSTR text)
{
	int length = WideCharToMultiByte(CP_ACP, 0, text, -1, NULL, 0, NULL, NULL);
	char *narrow = HeapAlloc(GetProcessHeap(), 0, length);
	if (narrow)
		WideCharToMultiByte(CP_ACP, 0, text, -1, narrow, length, NULL, NULL);
	return narrow;
}

static void save_order(const recent_list *list)
{
	RegSetValueExW(list->key, L"MRUList", 0, REG_SZ, (const BYTE *)list->order, (list->count + 1) * sizeof(WCHAR));
}

static void save_item(const recent_list *list, int slot)
{
	WCHAR name[2] = {(WCHAR)(L'a' + slot), 0};
	RegSetValueExW(list->key, name, 0, list->flags & LIST_BINARY ? REG_BINARY : REG_SZ, list->items[slot].data, list->items[slot].size);
}

static void load(recent_list *list)
{
	DWORD type, bytes = sizeof(list->order) - sizeof(WCHAR);
	WCHAR stored[MOST_ITEMS + 1] = {0};
	if (RegQueryValueExW(list->key, L"MRUList", NULL, &type, (BYTE *)stored, &bytes) != ERROR_SUCCESS || type != REG_SZ)
		return;
	for (const WCHAR *letter = stored; *letter && (UINT)list->count < list->most; ++letter) {
		int slot = *letter - L'a';
		if (slot < 0 || (UINT)slot >= list->most || list->items[slot].data)
			continue;
		WCHAR name[2] = {*letter, 0};
		DWORD size = 0;
		if (RegQueryValueExW(list->key, name, NULL, &type, NULL, &size) != ERROR_SUCCESS || !size)
			continue;
		BYTE *data = HeapAlloc(GetProcessHeap(), 0, size);
		if (!data)
			continue;
		if (RegQueryValueExW(list->key, name, NULL, &type, data, &size) != ERROR_SUCCESS) {
			HeapFree(GetProcessHeap(), 0, data);
			continue;
		}
		list->items[slot].data = data;
		list->items[slot].size = size;
		list->order[list->count++] = *letter;
	}
	list->order[list->count] = 0;
}

static HANDLE create_list(UINT most, UINT flags, HKEY key, LPCWSTR sub_key, void *compare, BOOL narrow)
{
	if (!key || !sub_key)
		return NULL;
	recent_list *list = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*list));
	if (!list)
		return NULL;
	list->most = most > MOST_ITEMS ? MOST_ITEMS : most;
	list->flags = flags;
	list->compare = compare;
	list->narrow = narrow;
	if (RegCreateKeyExW(key, sub_key, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &list->key, NULL) != ERROR_SUCCESS) {
		HeapFree(GetProcessHeap(), 0, list);
		return NULL;
	}
	load(list);
	return list;
}

/* The two extra arguments of the lazy forms have no documented effect. */
HANDLE WINAPI CreateMRUListLazyW(const list_info_w *info, DWORD unused1, DWORD unused2, DWORD unused3)
{
	if (!info)
		return NULL;
	return create_list(info->most, info->flags, info->key, info->sub_key, info->compare, FALSE);
}

HANDLE WINAPI CreateMRUListLazyA(const list_info_a *info, DWORD unused1, DWORD unused2, DWORD unused3)
{
	if (!info || !info->key || !info->sub_key)
		return NULL;
	WCHAR *sub_key = to_wide(info->sub_key);
	HANDLE list = sub_key ? create_list(info->most, info->flags, info->key, sub_key, info->compare, TRUE) : NULL;
	if (sub_key)
		HeapFree(GetProcessHeap(), 0, sub_key);
	return list;
}

HANDLE WINAPI CreateMRUListW(const list_info_w *info)
{
	return CreateMRUListLazyW(info, 0, 0, 0);
}

HANDLE WINAPI CreateMRUListA(const list_info_a *info)
{
	return CreateMRUListLazyA(info, 0, 0, 0);
}

void WINAPI FreeMRUList(HANDLE handle)
{
	recent_list *list = handle;
	if (!list)
		return;
	if (list->flags & LIST_CACHEWRITE) {
		save_order(list);
		for (int index = 0; index < list->count; ++index)
			save_item(list, list->order[index] - L'a');
	}
	for (int slot = 0; slot < MOST_ITEMS; ++slot)
		if (list->items[slot].data)
			HeapFree(GetProcessHeap(), 0, list->items[slot].data);
	RegCloseKey(list->key);
	HeapFree(GetProcessHeap(), 0, list);
}

static BOOL same_item(const recent_list *list, const list_item *item, const void *data, DWORD size)
{
	if (list->flags & LIST_BINARY) {
		if (list->compare)
			return !((compare_data)list->compare)(data, item->data, size);
		if (item->size != size)
			return FALSE;
		const BYTE *left = data, *right = item->data;
		for (DWORD index = 0; index < size; ++index)
			if (left[index] != right[index])
				return FALSE;
		return TRUE;
	}
	if (!list->narrow)
		return list->compare ? !((compare_text_w)list->compare)(data, (LPCWSTR)item->data) : !lstrcmpiW(data, (LPCWSTR)item->data);
	char *left = to_narrow(data), *right = to_narrow((LPCWSTR)item->data);
	BOOL same = left && right && (list->compare ? !((compare_text_a)list->compare)(left, right) : !lstrcmpiA(left, right));
	if (left)
		HeapFree(GetProcessHeap(), 0, left);
	if (right)
		HeapFree(GetProcessHeap(), 0, right);
	return same;
}

/* Where an item stands from the newest, or -1. */
static int find_position(const recent_list *list, const void *data, DWORD size)
{
	for (int position = 0; position < list->count; ++position)
		if (same_item(list, &list->items[list->order[position] - L'a'], data, size))
			return position;
	return -1;
}

static void move_to_front(recent_list *list, int position)
{
	WCHAR letter = list->order[position];
	MoveMemory(list->order + 1, list->order, position * sizeof(WCHAR));
	list->order[0] = letter;
}

INT WINAPI AddMRUData(HANDLE handle, LPCVOID data, DWORD size)
{
	recent_list *list = handle;
	if (!list || !data || !list->most)
		return -1;
	int position = find_position(list, data, size), slot;
	if (position >= 0) {
		slot = list->order[position] - L'a';
		move_to_front(list, position);
	} else {
		/* The next free letter, or the oldest item's when the list is full. */
		if ((UINT)list->count < list->most) {
			for (slot = 0; list->items[slot].data; ++slot)
				;
			list->order[list->count++] = (WCHAR)(L'a' + slot);
			list->order[list->count] = 0;
		} else
			slot = list->order[list->count - 1] - L'a';
		BYTE *copy = HeapAlloc(GetProcessHeap(), 0, size);
		if (!copy)
			return -1;
		CopyMemory(copy, data, size);
		if (list->items[slot].data)
			HeapFree(GetProcessHeap(), 0, list->items[slot].data);
		list->items[slot].data = copy;
		list->items[slot].size = size;
		move_to_front(list, list->count - 1);
		if (!(list->flags & LIST_CACHEWRITE))
			save_item(list, slot);
	}
	if (!(list->flags & LIST_CACHEWRITE))
		save_order(list);
	return slot;
}

INT WINAPI AddMRUStringW(HANDLE handle, LPCWSTR text)
{
	if (!handle)
		return -1;
	if (!text)
		return 0;
	return AddMRUData(handle, text, (lstrlenW(text) + 1) * sizeof(WCHAR));
}

INT WINAPI AddMRUStringA(HANDLE handle, LPCSTR text)
{
	if (!handle)
		return -1;
	if (!text)
		return 0;
	WCHAR *wide = to_wide(text);
	if (!wide)
		return -1;
	INT slot = AddMRUStringW(handle, wide);
	HeapFree(GetProcessHeap(), 0, wide);
	return slot;
}

INT WINAPI FindMRUData(HANDLE handle, LPCVOID data, DWORD size, LPINT slot)
{
	recent_list *list = handle;
	if (!list || !data)
		return -1;
	int position = find_position(list, data, size);
	if (position >= 0 && slot)
		*slot = list->order[position] - L'a';
	return position;
}

INT WINAPI FindMRUStringW(HANDLE handle, LPCWSTR text, LPINT slot)
{
	return text ? FindMRUData(handle, text, (lstrlenW(text) + 1) * sizeof(WCHAR), slot) : -1;
}

INT WINAPI FindMRUStringA(HANDLE handle, LPCSTR text, LPINT slot)
{
	if (!text)
		return -1;
	WCHAR *wide = to_wide(text);
	if (!wide)
		return -1;
	INT position = FindMRUStringW(handle, wide, slot);
	HeapFree(GetProcessHeap(), 0, wide);
	return position;
}

/* With no buffer or a negative position it tells how many items there are. */
static const list_item *enumerated(recent_list *list, INT position, LPVOID buffer, INT *result)
{
	*result = -1;
	if (!list)
		return NULL;
	if (position < 0 || !buffer) {
		*result = list->count;
		return NULL;
	}
	if (position >= list->count)
		return NULL;
	return &list->items[list->order[position] - L'a'];
}

INT WINAPI EnumMRUListW(HANDLE handle, INT position, LPVOID buffer, DWORD size)
{
	recent_list *list = handle;
	INT result;
	const list_item *item = enumerated(list, position, buffer, &result);
	if (!item)
		return result;
	if (list->flags & LIST_BINARY) {
		CopyMemory(buffer, item->data, item->size < size ? item->size : size);
		return (INT)item->size;
	}
	lstrcpynW(buffer, (LPCWSTR)item->data, size);
	return lstrlenW((LPCWSTR)item->data);
}

INT WINAPI EnumMRUListA(HANDLE handle, INT position, LPVOID buffer, DWORD size)
{
	recent_list *list = handle;
	INT result;
	const list_item *item = enumerated(list, position, buffer, &result);
	if (!item)
		return result;
	if (list->flags & LIST_BINARY) {
		CopyMemory(buffer, item->data, item->size < size ? item->size : size);
		return (INT)item->size;
	}
	char *narrow = to_narrow((LPCWSTR)item->data);
	if (!narrow)
		return -1;
	lstrcpynA(buffer, narrow, size);
	result = lstrlenA(narrow);
	HeapFree(GetProcessHeap(), 0, narrow);
	return result;
}

BOOL WINAPI DelMRUString(HANDLE handle, INT position)
{
	recent_list *list = handle;
	if (!list || position < 0 || position >= list->count)
		return FALSE;
	int slot = list->order[position] - L'a';
	WCHAR name[2] = {list->order[position], 0};
	HeapFree(GetProcessHeap(), 0, list->items[slot].data);
	list->items[slot].data = NULL;
	list->items[slot].size = 0;
	MoveMemory(list->order + position, list->order + position + 1, (list->count - position) * sizeof(WCHAR));
	list->count--;
	if (!(list->flags & LIST_CACHEWRITE)) {
		RegDeleteValueW(list->key, name);
		save_order(list);
	}
	return TRUE;
}
