/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven shell component: the file dialog window.
 *
 * One resizable window serves opening, saving and choosing folders: a bar with back, forward and up, the folder's address and a search
 * field; a navigation tree with the user's folders and the drives, whose folders open as they are expanded; the folder's contents in a
 * list view with name, date, type and size, sorted by any column and shown as details, a list or large icons; and below, the file name,
 * the file types and the buttons. The common controls come from comctl32, loaded when the dialog first opens, so programs that never
 * show it never load that library. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>
#include <string.h>
#include "shell.h"

#define ID_ADDRESS 0x101
#define ID_LIST 0x102
#define ID_NAME 0x104
#define ID_TYPE 0x106
#define ID_UP 0x107
#define ID_NAME_LABEL 0x109
#define ID_BACK 0x110
#define ID_FORWARD 0x111
#define ID_SEARCH 0x112
#define ID_TREE 0x113
#define ID_NEW_FOLDER 0x114
#define ID_VIEW 0x115
#define ID_STATUS 0x116
#define ID_CHECK 0x120

#define HISTORY 32
#define TYPE_CACHE 16

enum { ICON_FOLDER, ICON_FILE, ICON_DRIVE, ICON_COMPUTER, ICON_STAR, ICON_COUNT };
enum { SHOW_DETAILS, SHOW_LIST, SHOW_ICONS, SHOW_COUNT };
enum { COLUMN_NAME, COLUMN_DATE, COLUMN_TYPE, COLUMN_SIZE, COLUMN_COUNT };

typedef struct {
	WCHAR name[MAX_PATH];
	BOOL directory;
	ULONGLONG size;
	FILETIME written;
	WCHAR type[64];
} Entry;

/* The comctl32 functions the window needs, found when the library is loaded. */
typedef struct {
	BOOL (WINAPI *init)(const INITCOMMONCONTROLSEX *);
	HIMAGELIST (WINAPI *create)(int, int, UINT, int, int);
	int (WINAPI *add_masked)(HIMAGELIST, HBITMAP, COLORREF);
	BOOL (WINAPI *destroy)(HIMAGELIST);
} Controls;

typedef struct {
	DialogState *state;
	HWND window, address, search, tree, list, name, types;
	HIMAGELIST small_icons, large_icons;
	Entry *entries;
	UINT count, capacity;
	int sort_column;
	BOOL descending;
	int view;
	BOOL filling, activated;
	WCHAR back[HISTORY][MAX_PATH];
	UINT back_count;
	WCHAR forward[HISTORY][MAX_PATH];
	UINT forward_count;
	WCHAR typed_spec[128];
	struct { WCHAR extension[16]; WCHAR type[64]; } types_seen[TYPE_CACHE];
	UINT types_next;
} Browser;

static Controls controls;

static BOOL load_controls(void)
{
	HMODULE library;
	INITCOMMONCONTROLSEX classes = {sizeof classes, ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES};
	if (controls.init)
		return TRUE;
	if (!(library = LoadLibraryW(L"comctl32.dll")))
		return FALSE;
	controls.create = (void *)GetProcAddress(library, "ImageList_Create");
	controls.add_masked = (void *)GetProcAddress(library, "ImageList_AddMasked");
	controls.destroy = (void *)GetProcAddress(library, "ImageList_Destroy");
	controls.init = (void *)GetProcAddress(library, "InitCommonControlsEx");
	if (!controls.create || !controls.add_masked || !controls.destroy || !controls.init) {
		controls.init = NULL;
		return FALSE;
	}
	controls.init(&classes);
	return TRUE;
}

/* Icons ----------------------------------------------------------------------------------------------------- */

/* A point of an icon drawn on a 16 unit grid, at the size asked for. */
static POINT at(int x, int y, int size, int left)
{
	POINT point = {left + x * size / 16, y * size / 16};
	return point;
}

static void shape(HDC dc, const int *points, int count, int size, int left, COLORREF fill, COLORREF line)
{
	POINT corners[12];
	HBRUSH brush = CreateSolidBrush(fill);
	HPEN pen = CreatePen(PS_SOLID, 1, line);
	HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);
	int index;
	for (index = 0; index < count; ++index)
		corners[index] = at(points[index * 2], points[index * 2 + 1], size, left);
	Polygon(dc, corners, count);
	SelectObject(dc, old_brush);
	SelectObject(dc, old_pen);
	DeleteObject(brush);
	DeleteObject(pen);
}

static void draw_icon(HDC dc, int kind, int size, int left)
{
	static const int folder_back[] = {1, 3, 6, 3, 7, 4, 15, 4, 15, 14, 1, 14};
	static const int folder_front[] = {1, 6, 15, 6, 15, 14, 1, 14};
	static const int page[] = {3, 1, 10, 1, 13, 4, 13, 15, 3, 15};
	static const int corner[] = {10, 1, 10, 4, 13, 4};
	static const int drive_body[] = {1, 5, 15, 5, 15, 12, 1, 12};
	static const int drive_top[] = {1, 5, 15, 5, 15, 7, 1, 7};
	static const int drive_light[] = {11, 9, 13, 9, 13, 10, 11, 10};
	static const int monitor[] = {1, 2, 15, 2, 15, 11, 1, 11};
	static const int screen[] = {2, 3, 14, 3, 14, 10, 2, 10};
	static const int stand[] = {6, 11, 10, 11, 11, 14, 5, 14};
	static const int star[] = {8, 1, 10, 6, 15, 6, 11, 9, 13, 15, 8, 11, 3, 15, 5, 9, 1, 6, 6, 6};
	int line;
	switch (kind) {
	case ICON_FOLDER:
		shape(dc, folder_back, 6, size, left, RGB(226, 176, 58), RGB(176, 128, 28));
		shape(dc, folder_front, 4, size, left, RGB(252, 214, 104), RGB(196, 148, 40));
		break;
	case ICON_FILE:
		shape(dc, page, 5, size, left, RGB(255, 255, 255), RGB(118, 118, 118));
		shape(dc, corner, 3, size, left, RGB(222, 222, 222), RGB(118, 118, 118));
		for (line = 7; line <= 12; line += 2)
			shape(dc, (const int[]){5, line, 11, line, 11, line, 5, line}, 4, size, left, RGB(146, 168, 204), RGB(146, 168, 204));
		break;
	case ICON_DRIVE:
		shape(dc, drive_body, 4, size, left, RGB(196, 198, 204), RGB(96, 98, 108));
		shape(dc, drive_top, 4, size, left, RGB(226, 228, 232), RGB(96, 98, 108));
		shape(dc, drive_light, 4, size, left, RGB(64, 200, 92), RGB(40, 150, 64));
		break;
	case ICON_COMPUTER:
		shape(dc, monitor, 4, size, left, RGB(70, 72, 82), RGB(50, 52, 60));
		shape(dc, screen, 4, size, left, RGB(72, 140, 214), RGB(72, 140, 214));
		shape(dc, stand, 4, size, left, RGB(120, 122, 132), RGB(80, 82, 92));
		break;
	case ICON_STAR:
		shape(dc, star, 10, size, left, RGB(70, 140, 220), RGB(40, 100, 180));
		break;
	}
}

/* The icons of one size, drawn on a magenta ground that the image list masks out. */
static HIMAGELIST make_icons(int size)
{
	HIMAGELIST list = controls.create(size, size, ILC_COLOR24 | ILC_MASK, ICON_COUNT, 0);
	HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
	HBITMAP strip = CreateCompatibleBitmap(screen, size * ICON_COUNT, size);
	HGDIOBJ old = SelectObject(dc, strip);
	RECT all = {0, 0, size * ICON_COUNT, size};
	HBRUSH ground = CreateSolidBrush(RGB(255, 0, 255));
	int kind;
	FillRect(dc, &all, ground);
	DeleteObject(ground);
	for (kind = 0; kind < ICON_COUNT; ++kind)
		draw_icon(dc, kind, size, kind * size);
	SelectObject(dc, old);
	if (list)
		controls.add_masked(list, strip, RGB(255, 0, 255));
	DeleteObject(strip);
	DeleteDC(dc);
	ReleaseDC(NULL, screen);
	return list;
}

/* Text ------------------------------------------------------------------------------------------------------ */

static WCHAR fold(WCHAR c)
{
	return c >= L'A' && c <= L'Z' ? (WCHAR)(c + 32) : c;
}

static BOOL contains_folded(const WCHAR *text, const WCHAR *part)
{
	size_t length = lstrlenW(part), index;
	if (!length)
		return TRUE;
	for (; *text; ++text) {
		for (index = 0; index < length && text[index] && fold(text[index]) == fold(part[index]); ++index)
			;
		if (index == length)
			return TRUE;
	}
	return FALSE;
}

/* A count with thousands separators, as Explorer shows sizes in kilobytes. */
static void format_count(WCHAR *out, ULONGLONG value)
{
	WCHAR digits[32];
	int count = 0, index, at = 0;
	do {
		digits[count++] = (WCHAR)(L'0' + value % 10);
		value /= 10;
	} while (value);
	for (index = count - 1; index >= 0; --index) {
		out[at++] = digits[index];
		if (index && index % 3 == 0)
			out[at++] = L',';
	}
	out[at] = 0;
}

static void format_date(WCHAR *out, size_t capacity, const FILETIME *written)
{
	FILETIME local;
	SYSTEMTIME time;
	int length;
	out[0] = 0;
	if (!FileTimeToLocalFileTime(written, &local) || !FileTimeToSystemTime(&local, &time))
		return;
	length = GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &time, NULL, out, (int)capacity);
	if (length > 0 && (size_t)length < capacity - 1) {
		out[length - 1] = L' ';
		GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &time, NULL, out + length, (int)(capacity - length));
	}
}

/* The registered description of a file's extension, else the extension itself followed by "File". */
static void type_name(Browser *browser, const WCHAR *name, WCHAR *out)
{
	const WCHAR *dot = NULL, *scan;
	WCHAR class_name[128];
	LONG size;
	UINT index;
	for (scan = name; *scan; ++scan)
		if (*scan == L'.')
			dot = scan;
	if (!dot || !dot[1] || lstrlenW(dot) >= 16) {
		copy_text(out, 64, L"File");
		return;
	}
	for (index = 0; index < TYPE_CACHE; ++index)
		if (!lstrcmpiW(browser->types_seen[index].extension, dot)) {
			copy_text(out, 64, browser->types_seen[index].type);
			return;
		}
	out[0] = 0;
	size = sizeof class_name;
	if (!RegQueryValueW(HKEY_CLASSES_ROOT, dot, class_name, &size) && class_name[0]) {
		size = 64 * sizeof(WCHAR);
		if (RegQueryValueW(HKEY_CLASSES_ROOT, class_name, out, &size))
			out[0] = 0;
	}
	if (!out[0]) {
		size_t length = 0;
		for (scan = dot + 1; *scan && length < 40; ++scan)
			out[length++] = *scan >= L'a' && *scan <= L'z' ? (WCHAR)(*scan - 32) : *scan;
		out[length] = 0;
		lstrcatW(out, L" File");
	}
	index = browser->types_next++ % TYPE_CACHE;
	copy_text(browser->types_seen[index].extension, 16, dot);
	copy_text(browser->types_seen[index].type, 64, out);
}

/* The folder list ------------------------------------------------------------------------------------------- */

static const WCHAR *active_spec(const Browser *browser)
{
	const DialogState *state = browser->state;
	if (browser->typed_spec[0])
		return browser->typed_spec;
	if (state->filter_count && state->filter_index >= 1 && state->filter_index <= state->filter_count)
		return state->filters[state->filter_index - 1].spec;
	return L"*";
}

static Browser *sorting;

static int __cdecl compare_entries(const void *left, const void *right)
{
	const Entry *a = left, *b = right;
	int order = 0;
	if (a->directory != b->directory)
		return a->directory ? -1 : 1;
	switch (sorting->sort_column) {
	case COLUMN_DATE:
		order = CompareFileTime(&a->written, &b->written);
		break;
	case COLUMN_TYPE:
		order = lstrcmpiW(a->type, b->type);
		break;
	case COLUMN_SIZE:
		order = a->size < b->size ? -1 : a->size > b->size;
		break;
	}
	if (!order)
		order = lstrcmpiW(a->name, b->name);
	return sorting->descending ? -order : order;
}

static BOOL add_entry(Browser *browser, const WIN32_FIND_DATAW *found)
{
	Entry *entry;
	if (browser->count == browser->capacity) {
		const UINT capacity = browser->capacity ? browser->capacity * 2 : 128;
		Entry *grown = browser->entries ? HeapReAlloc(GetProcessHeap(), 0, browser->entries, capacity * sizeof(Entry))
			: HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(Entry));
		if (!grown)
			return FALSE;
		browser->entries = grown;
		browser->capacity = capacity;
	}
	entry = &browser->entries[browser->count++];
	copy_text(entry->name, MAX_PATH, found->cFileName);
	entry->directory = (found->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	entry->size = (ULONGLONG)found->nFileSizeHigh << 32 | found->nFileSizeLow;
	entry->written = found->ftLastWriteTime;
	if (entry->directory)
		copy_text(entry->type, 64, L"File folder");
	else
		type_name(browser, entry->name, entry->type);
	return TRUE;
}

static void read_folder(Browser *browser)
{
	WCHAR pattern[MAX_PATH + 4], search[128];
	WIN32_FIND_DATAW found;
	HANDLE handle;
	const WCHAR *spec = active_spec(browser);
	const BOOL folders_only = (browser->state->options & FOS_PICKFOLDERS) != 0;
	browser->count = 0;
	GetWindowTextW(browser->search, search, 128);
	join_path(pattern, MAX_PATH + 4, browser->state->dir, L"*");
	handle = FindFirstFileW(pattern, &found);
	if (handle == INVALID_HANDLE_VALUE)
		return;
	do {
		const BOOL directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (!lstrcmpW(found.cFileName, L".") || !lstrcmpW(found.cFileName, L".."))
			continue;
		if (found.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)
			continue;
		if (!directory && (folders_only || !match_spec(spec, found.cFileName)))
			continue;
		if (!contains_folded(found.cFileName, search))
			continue;
		if (!add_entry(browser, &found))
			break;
	} while (FindNextFileW(handle, &found));
	FindClose(handle);
}

static void set_cell(HWND list, int item, int column, const WCHAR *text)
{
	LVITEMW cell = {0};
	cell.iSubItem = column;
	cell.pszText = (WCHAR *)text;
	SendMessageW(list, LVM_SETITEMTEXTW, item, (LPARAM)&cell);
}

static void show_status(Browser *browser)
{
	WCHAR text[64];
	const LRESULT selected = SendMessageW(browser->list, LVM_GETSELECTEDCOUNT, 0, 0);
	if (selected > 0)
		wsprintfW(text, browser->count == 1 ? L"%u item, %u selected" : L"%u items, %u selected", browser->count, (UINT)selected);
	else
		wsprintfW(text, browser->count == 1 ? L"%u item" : L"%u items", browser->count);
	SetDlgItemTextW(browser->window, ID_STATUS, text);
}

/* Shows the entries in their order; the header arrow marks the sorted column. */
static void show_entries(Browser *browser)
{
	UINT index;
	int column;
	HWND header = (HWND)SendMessageW(browser->list, LVM_GETHEADER, 0, 0);
	browser->filling = TRUE;
	SendMessageW(browser->list, WM_SETREDRAW, FALSE, 0);
	SendMessageW(browser->list, LVM_DELETEALLITEMS, 0, 0);
	if (browser->count > 1) {
		sorting = browser;
		qsort(browser->entries, browser->count, sizeof(Entry), compare_entries);
	}
	for (index = 0; index < browser->count; ++index) {
		const Entry *entry = &browser->entries[index];
		WCHAR text[64];
		LVITEMW item = {0};
		int row;
		item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
		item.iItem = (int)index;
		item.pszText = (WCHAR *)entry->name;
		item.iImage = entry->directory ? ICON_FOLDER : ICON_FILE;
		item.lParam = index;
		row = (int)SendMessageW(browser->list, LVM_INSERTITEMW, 0, (LPARAM)&item);
		if (row < 0)
			continue;
		format_date(text, 64, &entry->written);
		set_cell(browser->list, row, COLUMN_DATE, text);
		set_cell(browser->list, row, COLUMN_TYPE, entry->type);
		if (!entry->directory) {
			format_count(text, (entry->size + 1023) / 1024);
			lstrcatW(text, L" KB");
			set_cell(browser->list, row, COLUMN_SIZE, text);
		}
	}
	for (column = 0; header && column < COLUMN_COUNT; ++column) {
		HDITEMW heading = {0};
		heading.mask = HDI_FORMAT;
		if (!SendMessageW(header, HDM_GETITEMW, column, (LPARAM)&heading))
			continue;
		heading.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
		if (column == browser->sort_column)
			heading.fmt |= browser->descending ? HDF_SORTDOWN : HDF_SORTUP;
		SendMessageW(header, HDM_SETITEMW, column, (LPARAM)&heading);
	}
	SendMessageW(browser->list, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(browser->list, NULL, TRUE);
	if (header)
		InvalidateRect(header, NULL, TRUE);
	browser->filling = FALSE;
	show_status(browser);
}

static void refresh(Browser *browser)
{
	read_folder(browser);
	show_entries(browser);
}

static void update_buttons(Browser *browser)
{
	WCHAR parent[MAX_PATH];
	copy_text(parent, MAX_PATH, browser->state->dir);
	parent_directory(parent);
	EnableWindow(GetDlgItem(browser->window, ID_BACK), browser->back_count > 0);
	EnableWindow(GetDlgItem(browser->window, ID_FORWARD), browser->forward_count > 0);
	EnableWindow(GetDlgItem(browser->window, ID_UP), lstrcmpiW(parent, browser->state->dir) != 0);
}

static void push(WCHAR (*stack)[MAX_PATH], UINT *count, const WCHAR *path)
{
	if (*count == HISTORY) {
		memmove(stack[0], stack[1], (HISTORY - 1) * sizeof stack[0]);
		--*count;
	}
	copy_text(stack[(*count)++], MAX_PATH, path);
}

static BOOL is_directory(const WCHAR *path)
{
	const DWORD attributes = GetFileAttributesW(path);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* Opens a folder; a visit the user makes is remembered for the back button, and forgets where forward led. */
static BOOL navigate(Browser *browser, const WCHAR *dir, BOOL remember)
{
	WCHAR full[MAX_PATH];
	if (!GetFullPathNameW(dir, MAX_PATH, full, NULL))
		copy_text(full, MAX_PATH, dir);
	if (!is_directory(full))
		return FALSE;
	if (remember && lstrcmpiW(full, browser->state->dir)) {
		push(browser->back, &browser->back_count, browser->state->dir);
		browser->forward_count = 0;
	}
	copy_text(browser->state->dir, MAX_PATH, full);
	browser->typed_spec[0] = 0;
	SetWindowTextW(browser->address, full);
	browser->filling = TRUE;
	SetWindowTextW(browser->search, L"");
	browser->filling = FALSE;
	refresh(browser);
	update_buttons(browser);
	return TRUE;
}

static void go_back(Browser *browser, BOOL forward)
{
	WCHAR (*from)[MAX_PATH] = forward ? browser->forward : browser->back;
	UINT *from_count = forward ? &browser->forward_count : &browser->back_count;
	WCHAR (*to)[MAX_PATH] = forward ? browser->back : browser->forward;
	UINT *to_count = forward ? &browser->back_count : &browser->forward_count;
	WCHAR target[MAX_PATH];
	if (!*from_count)
		return;
	copy_text(target, MAX_PATH, from[--*from_count]);
	push(to, to_count, browser->state->dir);
	navigate(browser, target, FALSE);
}

/* The navigation tree --------------------------------------------------------------------------------------- */

static WCHAR *copy_path(const WCHAR *path)
{
	const size_t length = lstrlenW(path) + 1;
	WCHAR *copy = HeapAlloc(GetProcessHeap(), 0, length * sizeof(WCHAR));
	if (copy)
		memcpy(copy, path, length * sizeof(WCHAR));
	return copy;
}

/* A node for a folder carries its path and promises children until it is expanded and found to have none. */
static HTREEITEM add_node(HWND tree, HTREEITEM parent, const WCHAR *label, int icon, const WCHAR *path)
{
	TVINSERTSTRUCTW insert = {0};
	insert.hParent = parent;
	insert.hInsertAfter = TVI_LAST;
	insert.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_CHILDREN;
	insert.item.pszText = (WCHAR *)label;
	insert.item.iImage = insert.item.iSelectedImage = icon;
	insert.item.lParam = (LPARAM)(path ? copy_path(path) : NULL);
	insert.item.cChildren = 1;
	return (HTREEITEM)SendMessageW(tree, TVM_INSERTITEMW, 0, (LPARAM)&insert);
}

static int __cdecl compare_names(const void *left, const void *right)
{
	return lstrcmpiW(left, right);
}

static void expand_node(HWND tree, HTREEITEM node, const WCHAR *path)
{
	WCHAR pattern[MAX_PATH + 4], (*names)[MAX_PATH] = NULL;
	WIN32_FIND_DATAW found;
	HANDLE handle;
	UINT count = 0, capacity = 0, index;
	TVITEMW item = {0};
	if (SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)node))
		return;
	join_path(pattern, MAX_PATH + 4, path, L"*");
	handle = FindFirstFileW(pattern, &found);
	if (handle != INVALID_HANDLE_VALUE) {
		do {
			if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (found.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) ||
				!lstrcmpW(found.cFileName, L".") || !lstrcmpW(found.cFileName, L".."))
				continue;
			if (count == capacity) {
				void *grown;
				capacity = capacity ? capacity * 2 : 32;
				grown = names ? HeapReAlloc(GetProcessHeap(), 0, names, capacity * sizeof names[0]) : HeapAlloc(GetProcessHeap(), 0, capacity * sizeof names[0]);
				if (!grown)
					break;
				names = grown;
			}
			copy_text(names[count++], MAX_PATH, found.cFileName);
		} while (FindNextFileW(handle, &found));
		FindClose(handle);
	}
	if (count > 1)
		qsort(names, count, sizeof names[0], compare_names);
	for (index = 0; index < count; ++index) {
		WCHAR child[MAX_PATH];
		join_path(child, MAX_PATH, path, names[index]);
		add_node(tree, node, names[index], ICON_FOLDER, child);
	}
	if (names)
		HeapFree(GetProcessHeap(), 0, names);
	if (!count) {
		item.mask = TVIF_CHILDREN;
		item.hItem = node;
		item.cChildren = 0;
		SendMessageW(tree, TVM_SETITEMW, 0, (LPARAM)&item);
	}
}

static void fill_tree(Browser *browser)
{
	static const struct { const WCHAR *label, *folder; } places[] = {
		{L"Desktop", L"Desktop"}, {L"Documents", L"Documents"}, {L"Downloads", L"Downloads"},
		{L"Music", L"Music"}, {L"Pictures", L"Pictures"}, {L"Videos", L"Videos"}};
	WCHAR profile[MAX_PATH], path[MAX_PATH];
	HTREEITEM quick, computer;
	DWORD drives;
	UINT index;
	quick = add_node(browser->tree, TVI_ROOT, L"Quick access", ICON_STAR, NULL);
	if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH))
		for (index = 0; index < ARRAYSIZE(places); ++index) {
			join_path(path, MAX_PATH, profile, places[index].folder);
			if (is_directory(path))
				add_node(browser->tree, quick, places[index].label, ICON_FOLDER, path);
		}
	computer = add_node(browser->tree, TVI_ROOT, L"This PC", ICON_COMPUTER, NULL);
	drives = GetLogicalDrives();
	if (!drives)
		drives = 1u << 2;
	for (index = 0; index < 26; ++index) {
		WCHAR root[4] = {(WCHAR)(L'A' + index), L':', L'\\', 0}, volume[64], label[96];
		if (!(drives & (1u << index)))
			continue;
		if (!GetVolumeInformationW(root, volume, 64, NULL, NULL, NULL, NULL, 0) || !volume[0])
			copy_text(volume, 64, L"Local Disk");
		wsprintfW(label, L"%s (%c:)", volume, root[0]);
		add_node(browser->tree, computer, label, ICON_DRIVE, root);
	}
	SendMessageW(browser->tree, TVM_EXPAND, TVE_EXPAND, (LPARAM)quick);
	SendMessageW(browser->tree, TVM_EXPAND, TVE_EXPAND, (LPARAM)computer);
}

/* Layout ---------------------------------------------------------------------------------------------------- */

static void place(HWND window, int id, int x, int y, int width, int height)
{
	HWND control = GetDlgItem(window, id);
	if (control)
		MoveWindow(control, x, y, width < 0 ? 0 : width, height < 0 ? 0 : height, TRUE);
}

static void layout(Browser *browser)
{
	const DialogState *state = browser->state;
	const int margin = 8, button = 88, row = 24, gap = 8, tree_width = 190, search_width = 200, type_width = 230;
	RECT client;
	int width, height, buttons_y, name_y, checks_y, main_top = 72, main_bottom, name_right;
	UINT check;
	GetClientRect(browser->window, &client);
	width = client.right;
	height = client.bottom;
	place(browser->window, ID_BACK, margin, margin, 30, row);
	place(browser->window, ID_FORWARD, margin + 32, margin, 30, row);
	place(browser->window, ID_UP, margin + 64, margin, 30, row);
	place(browser->window, ID_ADDRESS, margin + 102, margin + 1, width - margin * 2 - 102 - search_width - gap, row - 2);
	place(browser->window, ID_SEARCH, width - margin - search_width, margin + 1, search_width, row - 2);
	place(browser->window, ID_NEW_FOLDER, margin, margin + row + gap, 100, row);
	place(browser->window, ID_VIEW, width - margin - 130, margin + row + gap, 130, row);
	buttons_y = height - margin - 26;
	name_y = buttons_y - gap - row;
	checks_y = name_y - gap - (int)state->check_count * 20;
	main_bottom = (state->check_count ? checks_y : name_y) - gap;
	place(browser->window, ID_TREE, margin, main_top, tree_width, main_bottom - main_top);
	place(browser->window, ID_LIST, margin + tree_width + gap / 2, main_top, width - margin * 2 - tree_width - gap / 2, main_bottom - main_top);
	for (check = 0; check < state->check_count; ++check)
		place(browser->window, ID_CHECK + check, margin + tree_width + gap / 2, checks_y + (int)check * 20, 320, 18);
	name_right = state->filter_count && !(state->options & FOS_PICKFOLDERS) ? width - margin - type_width - gap : width - margin;
	place(browser->window, ID_NAME_LABEL, margin + tree_width + gap / 2, name_y + 4, 80, row - 6);
	place(browser->window, ID_NAME, margin + tree_width + gap / 2 + 84, name_y, name_right - (margin + tree_width + gap / 2 + 84), row);
	place(browser->window, ID_TYPE, width - margin - type_width, name_y, type_width, 220);
	place(browser->window, ID_STATUS, margin, buttons_y + 6, 260, 18);
	place(browser->window, IDOK, width - margin - button * 2 - gap, buttons_y, button, 26);
	place(browser->window, IDCANCEL, width - margin - button, buttons_y, button, 26);
}

static HWND add_control(Browser *browser, DWORD extended, const WCHAR *klass, const WCHAR *text, DWORD style, int id)
{
	HWND control = CreateWindowExW(extended, klass, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, browser->window, (HMENU)(INT_PTR)id,
		GetModuleHandleW(NULL), NULL);
	if (control)
		SendMessageW(control, WM_SETFONT, SendMessageW(browser->window, WM_GETFONT, 0, 0), FALSE);
	return control;
}

static void set_view(Browser *browser, int view)
{
	static const WCHAR *labels[] = {L"View: Details", L"View: List", L"View: Large icons"};
	static const DWORD styles[] = {LVS_REPORT, LVS_LIST, LVS_ICON};
	const LONG_PTR style = GetWindowLongPtrW(browser->list, GWL_STYLE);
	browser->view = view;
	SetWindowLongPtrW(browser->list, GWL_STYLE, (style & ~(LONG_PTR)LVS_TYPEMASK) | styles[view]);
	SetDlgItemTextW(browser->window, ID_VIEW, labels[view]);
	InvalidateRect(browser->list, NULL, TRUE);
}

static void create_controls(Browser *browser)
{
	static const struct { const WCHAR *title; int width; int format; } columns[COLUMN_COUNT] = {
		{L"Name", 250, LVCFMT_LEFT}, {L"Date modified", 140, LVCFMT_LEFT}, {L"Type", 130, LVCFMT_LEFT}, {L"Size", 80, LVCFMT_RIGHT}};
	DialogState *state = browser->state;
	const BOOL folders = (state->options & FOS_PICKFOLDERS) != 0, many = (state->options & FOS_ALLOWMULTISELECT) && !state->save;
	UINT index;
	add_control(browser, 0, L"Button", L"\x2190", WS_TABSTOP | BS_PUSHBUTTON, ID_BACK);
	add_control(browser, 0, L"Button", L"\x2192", WS_TABSTOP | BS_PUSHBUTTON, ID_FORWARD);
	add_control(browser, 0, L"Button", L"\x2191", WS_TABSTOP | BS_PUSHBUTTON, ID_UP);
	browser->address = add_control(browser, WS_EX_CLIENTEDGE, L"Edit", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_ADDRESS);
	browser->search = add_control(browser, WS_EX_CLIENTEDGE, L"Edit", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_SEARCH);
	SendMessageW(browser->search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search");
	if (state->save || folders)
		add_control(browser, 0, L"Button", L"New folder", WS_TABSTOP | BS_PUSHBUTTON, ID_NEW_FOLDER);
	add_control(browser, 0, L"Button", L"View: Details", WS_TABSTOP | BS_PUSHBUTTON, ID_VIEW);
	browser->tree = add_control(browser, WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"", WS_TABSTOP | TVS_HASBUTTONS | TVS_SHOWSELALWAYS |
		TVS_LINESATROOT, ID_TREE);
	browser->list = add_control(browser, WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_TABSTOP | WS_CLIPCHILDREN | LVS_REPORT | LVS_SHOWSELALWAYS |
		LVS_SHAREIMAGELISTS | (many ? 0 : LVS_SINGLESEL), ID_LIST);
	SendMessageW(browser->list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
	for (index = 0; index < COLUMN_COUNT; ++index) {
		LVCOLUMNW column = {0};
		column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
		column.pszText = (WCHAR *)columns[index].title;
		column.cx = columns[index].width;
		column.fmt = columns[index].format;
		column.iSubItem = (int)index;
		SendMessageW(browser->list, LVM_INSERTCOLUMNW, index, (LPARAM)&column);
	}
	browser->small_icons = make_icons(16);
	browser->large_icons = make_icons(32);
	SendMessageW(browser->list, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)browser->small_icons);
	SendMessageW(browser->list, LVM_SETIMAGELIST, LVSIL_NORMAL, (LPARAM)browser->large_icons);
	SendMessageW(browser->tree, TVM_SETIMAGELIST, TVSIL_NORMAL, (LPARAM)browser->small_icons);
	for (index = 0; index < state->check_count; ++index) {
		HWND check = add_control(browser, 0, L"Button", state->checks[index].label, WS_TABSTOP | BS_AUTOCHECKBOX, (int)(ID_CHECK + index));
		SendMessageW(check, BM_SETCHECK, state->checks[index].checked ? BST_CHECKED : BST_UNCHECKED, 0);
	}
	add_control(browser, 0, L"Static", state->name_label[0] ? state->name_label : folders ? L"Folder:" : L"File name:", SS_LEFT,
		ID_NAME_LABEL);
	browser->name = add_control(browser, WS_EX_CLIENTEDGE, L"Edit", state->name, WS_TABSTOP | ES_AUTOHSCROLL, ID_NAME);
	browser->types = add_control(browser, 0, L"ComboBox", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, ID_TYPE);
	for (index = 0; index < state->filter_count; ++index) {
		WCHAR entry[64 + 256 + 4];
		const Filter *filter = &state->filters[index];
		copy_text(entry, 64, filter->name);
		/* Explorer shows the patterns after the name unless the name already lists them. */
		if (!contains_folded(filter->name, filter->spec) && lstrlenW(filter->spec) < 200) {
			lstrcatW(entry, L" (");
			lstrcatW(entry, filter->spec);
			lstrcatW(entry, L")");
		}
		SendMessageW(browser->types, CB_ADDSTRING, 0, (LPARAM)entry);
	}
	if (state->filter_count && !folders) {
		if (state->filter_index < 1 || state->filter_index > state->filter_count)
			state->filter_index = 1;
		SendMessageW(browser->types, CB_SETCURSEL, state->filter_index - 1, 0);
	} else {
		ShowWindow(browser->types, SW_HIDE);
	}
	add_control(browser, 0, L"Static", L"", SS_LEFT | SS_NOPREFIX, ID_STATUS);
	add_control(browser, 0, L"Button", state->ok_label[0] ? state->ok_label : folders ? L"Select Folder" : state->save ? L"Save" : L"Open",
		WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
	add_control(browser, 0, L"Button", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
}

/* Choosing ---------------------------------------------------------------------------------------------------- */

static void say(const Browser *browser, const WCHAR *text, UINT flags)
{
	MessageBoxW(browser->window, text, browser->state->title[0] ? browser->state->title : L"File", flags);
}

/* The extension a saved name gets when it has none: the default one, else the first of the selected file type ("*.txt"). */
static void add_extension(const Browser *browser, WCHAR *full)
{
	const DialogState *state = browser->state;
	WCHAR extension[16];
	const WCHAR *spec;
	if (wcschr(file_part(full), L'.'))
		return;
	copy_text(extension, 16, state->defext);
	spec = state->filter_count && state->filter_index >= 1 && state->filter_index <= state->filter_count ?
		state->filters[state->filter_index - 1].spec : L"*";
	if (!extension[0] && spec[0] == L'*' && spec[1] == L'.' && spec[2] && spec[2] != L'*' && spec[2] != L'?') {
		size_t length = 0;
		while (spec[2 + length] && spec[2 + length] != L';' && spec[2 + length] != L'*' && spec[2 + length] != L'?' && length < 15)
			extension[length] = spec[2 + length], ++length;
		extension[length] = 0;
	}
	if (extension[0] && lstrlenW(full) + lstrlenW(extension) + 2 < MAX_PATH) {
		lstrcatW(full, L".");
		lstrcatW(full, extension);
	}
}

/* The names typed in the file name field: one name, or several in quotes when more than one file may be chosen. */
static UINT split_names(const WCHAR *text, WCHAR (*names)[MAX_PATH], UINT capacity)
{
	UINT count = 0;
	while (*text == L' ')
		++text;
	if (*text != L'"') {
		copy_text(names[0], MAX_PATH, text);
		return text[0] ? 1 : 0;
	}
	while (*text && count < capacity) {
		const WCHAR *end;
		size_t length;
		while (*text == L' ')
			++text;
		if (*text != L'"')
			break;
		end = wcschr(text + 1, L'"');
		length = end ? (size_t)(end - text - 1) : (size_t)lstrlenW(text + 1);
		if (length >= MAX_PATH)
			length = MAX_PATH - 1;
		memcpy(names[count], text + 1, length * sizeof(WCHAR));
		names[count][length] = 0;
		if (length)
			++count;
		if (!end)
			break;
		text = end + 1;
	}
	return count;
}

static BOOL accept(Browser *browser)
{
	DialogState *state = browser->state;
	WCHAR text[MAX_PATH * 4], (*names)[MAX_PATH] = state->results, full[MAX_PATH + 16];
	const BOOL folders = (state->options & FOS_PICKFOLDERS) != 0;
	const UINT capacity = (state->options & FOS_ALLOWMULTISELECT) && !state->save ? MAX_RESULTS : 1;
	UINT count, index;
	DWORD attributes;
	GetWindowTextW(browser->name, text, MAX_PATH * 4);
	count = split_names(text, names, capacity);
	if (!count) {
		if (!folders)
			return FALSE;
		copy_text(state->result, MAX_PATH, state->dir);
		copy_text(state->results[0], MAX_PATH, state->dir);
		state->result_count = 1;
		return TRUE;
	}
	if (count == 1 && (wcschr(names[0], L'*') || wcschr(names[0], L'?'))) {
		copy_text(browser->typed_spec, 128, file_part(names[0]));
		refresh(browser);
		return FALSE;
	}
	for (index = 0; index < count; ++index) {
		if (is_absolute(names[index]))
			copy_text(full, MAX_PATH, names[index]);
		else
			join_path(full, MAX_PATH, state->dir, names[index]);
		attributes = GetFileAttributesW(full);
		if (count == 1 && attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
			if (folders) {
				copy_text(state->results[0], MAX_PATH, full);
				break;
			}
			SetWindowTextW(browser->name, L"");
			navigate(browser, full, TRUE);
			return FALSE;
		}
		if (!folders && state->save && attributes == INVALID_FILE_ATTRIBUTES) {
			add_extension(browser, full);
			attributes = GetFileAttributesW(full);
		}
		if (state->save) {
			if (attributes != INVALID_FILE_ATTRIBUTES && (state->options & FOS_OVERWRITEPROMPT)) {
				WCHAR message[MAX_PATH + 80];
				copy_text(message, MAX_PATH + 80, file_part(full));
				lstrcatW(message, L" already exists.\nDo you want to replace it?");
				if (MessageBoxW(browser->window, message, state->title[0] ? state->title : L"Confirm Save As",
						MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
					return FALSE;
			}
		} else if (attributes == INVALID_FILE_ATTRIBUTES && (state->options & FOS_FILEMUSTEXIST)) {
			WCHAR message[MAX_PATH + 80];
			copy_text(message, MAX_PATH + 80, file_part(full));
			lstrcatW(message, L"\nFile not found.\nCheck the file name and try again.");
			say(browser, message, MB_OK | MB_ICONWARNING);
			return FALSE;
		}
		copy_text(state->results[index], MAX_PATH, full);
	}
	state->result_count = count;
	copy_text(state->result, MAX_PATH, state->results[0]);
	return TRUE;
}

/* The names of the selected files go to the file name field: one as it is, several each in quotes. */
static void take_selection(Browser *browser)
{
	WCHAR text[MAX_PATH * 4];
	int item = -1;
	UINT files = 0;
	const BOOL folders = (browser->state->options & FOS_PICKFOLDERS) != 0;
	text[0] = 0;
	while ((item = (int)SendMessageW(browser->list, LVM_GETNEXTITEM, item, LVNI_SELECTED)) >= 0) {
		LVITEMW row = {0};
		const Entry *entry;
		row.mask = LVIF_PARAM;
		row.iItem = item;
		if (!SendMessageW(browser->list, LVM_GETITEMW, 0, (LPARAM)&row) || (UINT)row.lParam >= browser->count)
			continue;
		entry = &browser->entries[row.lParam];
		if (entry->directory != folders)
			continue;
		if (lstrlenW(text) + lstrlenW(entry->name) + 4 >= MAX_PATH * 4)
			break;
		if (files == 1) {
			WCHAR first[MAX_PATH];
			copy_text(first, MAX_PATH, text);
			lstrcpyW(text, L"\"");
			lstrcatW(text, first);
			lstrcatW(text, L"\"");
		}
		if (files) {
			lstrcatW(text, L" \"");
			lstrcatW(text, entry->name);
			lstrcatW(text, L"\"");
		} else {
			lstrcatW(text, entry->name);
		}
		++files;
	}
	if (files)
		SetWindowTextW(browser->name, text);
	show_status(browser);
}

static void open_item(Browser *browser, int item)
{
	LVITEMW row = {0};
	WCHAR path[MAX_PATH];
	row.mask = LVIF_PARAM;
	row.iItem = item;
	if (item < 0 || !SendMessageW(browser->list, LVM_GETITEMW, 0, (LPARAM)&row) || (UINT)row.lParam >= browser->count)
		return;
	if (browser->entries[row.lParam].directory) {
		join_path(path, MAX_PATH, browser->state->dir, browser->entries[row.lParam].name);
		navigate(browser, path, TRUE);
		return;
	}
	SetWindowTextW(browser->name, browser->entries[row.lParam].name);
	SendMessageW(browser->window, WM_COMMAND, IDOK, 0);
}

static void new_folder(Browser *browser)
{
	WCHAR name[64], path[MAX_PATH];
	UINT attempt;
	for (attempt = 1; attempt < 100; ++attempt) {
		if (attempt == 1)
			copy_text(name, 64, L"New folder");
		else
			wsprintfW(name, L"New folder (%u)", attempt);
		join_path(path, MAX_PATH, browser->state->dir, name);
		if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
			continue;
		if (!CreateDirectoryW(path, NULL)) {
			say(browser, L"The folder could not be created.", MB_OK | MB_ICONWARNING);
			return;
		}
		refresh(browser);
		{
			LVFINDINFOW find = {0};
			int item;
			find.flags = LVFI_STRING;
			find.psz = name;
			item = (int)SendMessageW(browser->list, LVM_FINDITEMW, (WPARAM)-1, (LPARAM)&find);
			if (item >= 0) {
				LVITEMW state = {0};
				state.stateMask = state.state = LVIS_SELECTED | LVIS_FOCUSED;
				SendMessageW(browser->list, LVM_SETITEMSTATE, item, (LPARAM)&state);
				SendMessageW(browser->list, LVM_ENSUREVISIBLE, item, FALSE);
			}
		}
		return;
	}
}

static void go_to_address(Browser *browser)
{
	WCHAR text[MAX_PATH], full[MAX_PATH];
	GetWindowTextW(browser->address, text, MAX_PATH);
	if (is_absolute(text))
		copy_text(full, MAX_PATH, text);
	else
		join_path(full, MAX_PATH, browser->state->dir, text);
	if (navigate(browser, full, TRUE)) {
		SetFocus(browser->list);
		return;
	}
	if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) {
		SetWindowTextW(browser->name, full);
		SetWindowTextW(browser->address, browser->state->dir);
		SendMessageW(browser->window, WM_COMMAND, IDOK, 0);
		return;
	}
	{
		WCHAR message[MAX_PATH + 96];
		copy_text(message, MAX_PATH, text);
		lstrcatW(message, L"\nThe path does not exist. Check the spelling and try again.");
		say(browser, message, MB_OK | MB_ICONWARNING);
	}
}

static LRESULT notify(Browser *browser, NMHDR *header)
{
	if (header->idFrom == ID_LIST) {
		switch (header->code) {
		case LVN_ITEMCHANGED: {
			const NMLISTVIEW *change = (const NMLISTVIEW *)header;
			if (!browser->filling && ((change->uNewState ^ change->uOldState) & LVIS_SELECTED))
				take_selection(browser);
			return 0;
		}
		case NM_DBLCLK:
			open_item(browser, ((const NMITEMACTIVATE *)header)->iItem);
			return 0;
		case LVN_COLUMNCLICK: {
			const int column = ((const NMLISTVIEW *)header)->iSubItem;
			browser->descending = column == browser->sort_column ? !browser->descending : FALSE;
			browser->sort_column = column;
			show_entries(browser);
			return 0;
		}
		case LVN_KEYDOWN: {
			const WORD key = ((const NMLVKEYDOWN *)header)->wVKey;
			if (key == VK_BACK)
				SendMessageW(browser->window, WM_COMMAND, ID_UP, 0);
			else if (key == VK_F5)
				refresh(browser);
			return 0;
		}
		}
	} else if (header->idFrom == ID_TREE) {
		switch (header->code) {
		case TVN_ITEMEXPANDINGW: {
			const NMTREEVIEWW *change = (const NMTREEVIEWW *)header;
			if ((change->action & TVE_EXPAND) && change->itemNew.lParam)
				expand_node(browser->tree, change->itemNew.hItem, (const WCHAR *)change->itemNew.lParam);
			return FALSE;
		}
		case TVN_SELCHANGEDW: {
			const NMTREEVIEWW *change = (const NMTREEVIEWW *)header;
			if ((change->action == TVC_BYMOUSE || change->action == TVC_BYKEYBOARD) && change->itemNew.lParam)
				navigate(browser, (const WCHAR *)change->itemNew.lParam, TRUE);
			return 0;
		}
		case TVN_DELETEITEMW: {
			const NMTREEVIEWW *change = (const NMTREEVIEWW *)header;
			if (change->itemOld.lParam)
				HeapFree(GetProcessHeap(), 0, (void *)change->itemOld.lParam);
			return 0;
		}
		}
	}
	return 0;
}

/* The dialog opens at a comfortable size over its owner, kept within the work area. */
static void center(HWND window, HWND owner)
{
	RECT area, over;
	int width = 900, height = 560, x, y;
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
	if (!owner || !GetWindowRect(owner, &over))
		over = area;
	if (width > area.right - area.left)
		width = area.right - area.left;
	if (height > area.bottom - area.top)
		height = area.bottom - area.top;
	x = over.left + (over.right - over.left - width) / 2;
	y = over.top + (over.bottom - over.top - height) / 2;
	x = x < area.left ? area.left : x + width > area.right ? area.right - width : x;
	y = y < area.top ? area.top : y + height > area.bottom ? area.bottom - height : y;
	SetWindowPos(window, NULL, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

static INT_PTR CALLBACK dialog_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	Browser *browser = (Browser *)GetWindowLongPtrW(window, DWLP_USER);
	switch (message) {
	case WM_INITDIALOG:
		browser = (Browser *)lparam;
		browser->window = window;
		SetWindowLongPtrW(window, DWLP_USER, (LONG_PTR)browser);
		create_controls(browser);
		fill_tree(browser);
		center(window, GetParent(window));
		layout(browser);
		if (!navigate(browser, browser->state->dir, FALSE)) {
			WCHAR current[MAX_PATH];
			GetCurrentDirectoryW(MAX_PATH, current);
			navigate(browser, current, FALSE);
		}
		SendMessageW(window, WM_NEXTDLGCTL, (WPARAM)(browser->state->options & FOS_PICKFOLDERS ? browser->list : browser->name), TRUE);
		return FALSE;
	case WM_SETFOCUS:
		/* The file name field takes the keyboard with its text selected when the dialog first gets it, so typing a name and Enter
		   saves; the dialog manager, which has no saved focus yet, would give it to the first control. Later it restores what had it. */
		if (!browser || browser->activated)
			return FALSE;
		browser->activated = TRUE;
		SendMessageW(window, WM_NEXTDLGCTL, (WPARAM)(browser->state->options & FOS_PICKFOLDERS ? browser->list : browser->name), TRUE);
		return TRUE;
	case WM_SIZE:
		if (browser)
			layout(browser);
		return TRUE;
	case WM_GETMINMAXINFO:
		((MINMAXINFO *)lparam)->ptMinTrackSize.x = 600;
		((MINMAXINFO *)lparam)->ptMinTrackSize.y = 400;
		return TRUE;
	case WM_NOTIFY:
		if (!browser)
			return FALSE;
		SetWindowLongPtrW(window, DWLP_MSGRESULT, notify(browser, (NMHDR *)lparam));
		return TRUE;
	case WM_COMMAND:
		if (!browser)
			return FALSE;
		switch (LOWORD(wparam)) {
		case IDOK: {
			/* Enter acts on the field that has the keyboard: the address goes there, a folder in the list opens. */
			HWND focus = GetFocus();
			if (lparam == 0 && focus == browser->address) {
				go_to_address(browser);
				return TRUE;
			}
			if (lparam == 0 && focus == browser->search) {
				SetFocus(browser->list);
				return TRUE;
			}
			if (lparam == 0 && focus == browser->list && SendMessageW(browser->list, LVM_GETSELECTEDCOUNT, 0, 0) == 1) {
				const int item = (int)SendMessageW(browser->list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
				LVITEMW row = {0};
				row.mask = LVIF_PARAM;
				row.iItem = item;
				if (SendMessageW(browser->list, LVM_GETITEMW, 0, (LPARAM)&row) && (UINT)row.lParam < browser->count &&
					browser->entries[row.lParam].directory && !(browser->state->options & FOS_PICKFOLDERS)) {
					open_item(browser, item);
					return TRUE;
				}
			}
			if (accept(browser)) {
				UINT check;
				for (check = 0; check < browser->state->check_count; ++check)
					browser->state->checks[check].checked = IsDlgButtonChecked(window, ID_CHECK + check) == BST_CHECKED;
				browser->state->has_result = TRUE;
				EndDialog(window, IDOK);
			}
			return TRUE;
		}
		case IDCANCEL:
			EndDialog(window, IDCANCEL);
			return TRUE;
		case ID_BACK:
			go_back(browser, FALSE);
			return TRUE;
		case ID_FORWARD:
			go_back(browser, TRUE);
			return TRUE;
		case ID_UP: {
			WCHAR dir[MAX_PATH];
			copy_text(dir, MAX_PATH, browser->state->dir);
			parent_directory(dir);
			navigate(browser, dir, TRUE);
			return TRUE;
		}
		case ID_NEW_FOLDER:
			new_folder(browser);
			return TRUE;
		case ID_VIEW:
			set_view(browser, (browser->view + 1) % SHOW_COUNT);
			return TRUE;
		case ID_SEARCH:
			if (HIWORD(wparam) == EN_CHANGE && !browser->filling)
				refresh(browser);
			return TRUE;
		case ID_TYPE:
			if (HIWORD(wparam) == CBN_SELCHANGE) {
				const LRESULT selected = SendMessageW(browser->types, CB_GETCURSEL, 0, 0);
				if (selected >= 0)
					browser->state->filter_index = (UINT)selected + 1;
				browser->typed_spec[0] = 0;
				refresh(browser);
			}
			return TRUE;
		}
		return FALSE;
	}
	return FALSE;
}

BOOL run_dialog(HWND owner, DialogState *state)
{
	/* A template with the frame alone: the controls are made in pixels once the dialog exists. */
	WORD *buffer, *at;
	const WCHAR *title = state->title[0] ? state->title : (state->options & FOS_PICKFOLDERS) ? L"Select Folder" : state->save ? L"Save As" : L"Open";
	Browser *browser;
	INT_PTR answer;
	if (!load_controls())
		return FALSE;
	buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1024);
	browser = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Browser));
	if (!buffer || !browser) {
		if (buffer)
			HeapFree(GetProcessHeap(), 0, buffer);
		if (browser)
			HeapFree(GetProcessHeap(), 0, browser);
		return FALSE;
	}
	at = buffer;
	*(DWORD *)at = DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
	at += 4; /* style, extended style */
	*at++ = 0; /* no controls */
	*at++ = 0; *at++ = 0; *at++ = 400; *at++ = 260;
	*at++ = 0; *at++ = 0; /* no menu, the dialog class */
	lstrcpyW((WCHAR *)at, title);
	at += lstrlenW(title) + 1;
	*at++ = 8;
	lstrcpyW((WCHAR *)at, L"MS Shell Dlg");
	browser->state = state;
	state->has_result = FALSE;
	state->result_count = 0;
	if (!state->dir[0])
		GetCurrentDirectoryW(MAX_PATH, state->dir);
	answer = DialogBoxIndirectParamW(GetModuleHandleW(NULL), (LPCDLGTEMPLATEW)buffer, owner, dialog_proc, (LPARAM)browser);
	if (browser->small_icons)
		controls.destroy(browser->small_icons);
	if (browser->large_icons)
		controls.destroy(browser->large_icons);
	if (browser->entries)
		HeapFree(GetProcessHeap(), 0, browser->entries);
	HeapFree(GetProcessHeap(), 0, browser);
	HeapFree(GetProcessHeap(), 0, buffer);
	return answer == IDOK && state->has_result;
}
