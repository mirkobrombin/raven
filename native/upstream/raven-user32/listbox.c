/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include "user32_private.h"

/* The list box control, and the drop-down list of a combo box, from Microsoft's documentation of their messages. */

#define DEFAULT_COLUMN_WIDTH 150
#define MAX_ITEM_HEIGHT 255
#define TYPE_AHEAD_MS 1000
#define SCROLL_TIMER 1
#define SCROLL_TIMER_MS 50
#define TEXT_MARGIN 1

#ifndef LB_GETLISTBOXINFO
#define LB_GETLISTBOXINFO 0x01b2
#endif

typedef struct {
	WCHAR *text;
	ULONG_PTR data;
	int height;
	BOOL selected;
} list_item;

typedef struct {
	HWND owner;
	HFONT font;
	list_item *items;
	int count, capacity;
	int top, caret, anchor, selected;
	int item_height, column_width, horizontal_extent, horizontal_position;
	INT *tab_stops;
	int tab_count;
	LCID locale;
	BOOL multiple, extended, has_strings, owner_fixed, owner_variable, sorted, no_data, in_combo;
	BOOL vertical_bar, horizontal_bar;
	BOOL redraw, focused, captured, resizing, moved, tracking_bar;
	WCHAR typed[64];
	int typed_length;
	DWORD typed_at;
	short wheel_rest;
} list_state;

static list_state *state_of(HWND window)
{
	return (list_state *)GetWindowLongPtrW(window, 0);
}

static DWORD style_of(HWND window)
{
	return (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
}

static BOOL is_owner_draw(const list_state *state)
{
	return state->owner_fixed || state->owner_variable;
}

static void notify(HWND window, const list_state *state, WORD code)
{
	if (style_of(window) & LBS_NOTIFY || state->in_combo)
		SendMessageW(state->owner, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(window), code), (LPARAM)window);
}

static int height_of(const list_state *state, int index)
{
	return state->owner_variable && index < state->count ? state->items[index].height : state->item_height;
}

static int rows_per_column(HWND window, const list_state *state)
{
	RECT client;
	GetClientRect(window, &client);
	int rows = client.bottom / state->item_height;
	return rows > 0 ? rows : 1;
}

/* How many items fit whole from `top` down, and always at least one. */
static int items_per_page(HWND window, const list_state *state, int top)
{
	if (!state->owner_variable)
		return rows_per_column(window, state);
	RECT client;
	GetClientRect(window, &client);
	int used = 0, fitting = 0;
	for (int index = top; index < state->count; ++index) {
		used += state->items[index].height;
		if (used > client.bottom)
			break;
		++fitting;
	}
	return fitting ? fitting : 1;
}

static int visible_columns(HWND window, const list_state *state)
{
	RECT client;
	GetClientRect(window, &client);
	int columns = client.right / state->column_width;
	return columns > 0 ? columns : 1;
}

static int highest_top(HWND window, const list_state *state)
{
	if (style_of(window) & LBS_MULTICOLUMN) {
		int rows = rows_per_column(window, state);
		int columns = (state->count + rows - 1) / rows;
		int last = columns - visible_columns(window, state);
		return last > 0 ? last * rows : 0;
	}
	if (state->owner_variable) {
		RECT client;
		GetClientRect(window, &client);
		int used = 0, index = state->count;
		while (index > 0) {
			used += state->items[index - 1].height;
			if (used > client.bottom)
				break;
			--index;
		}
		return index < state->count ? index : (state->count ? state->count - 1 : 0);
	}
	int last = state->count - rows_per_column(window, state);
	return last > 0 ? last : 0;
}

static void item_rect(HWND window, const list_state *state, int index, RECT *rect)
{
	RECT client;
	GetClientRect(window, &client);
	if (style_of(window) & LBS_MULTICOLUMN) {
		int rows = rows_per_column(window, state);
		int column = index / rows - state->top / rows;
		rect->left = column * state->column_width;
		rect->right = rect->left + state->column_width;
		rect->top = (index % rows) * state->item_height;
		rect->bottom = rect->top + state->item_height;
		return;
	}
	int y = 0;
	if (state->owner_variable) {
		for (int at = state->top; at < index; ++at)
			y += state->items[at].height;
		for (int at = index; at < state->top; ++at)
			y -= state->items[at].height;
	} else
		y = (index - state->top) * state->item_height;
	int width = state->horizontal_extent > client.right ? state->horizontal_extent : client.right;
	rect->left = -state->horizontal_position;
	rect->right = rect->left + width;
	rect->top = y;
	rect->bottom = y + height_of(state, index);
}

static void update_scroll_bars(HWND window, const list_state *state)
{
	if (!state->redraw)
		return;
	DWORD style = style_of(window);
	SCROLLINFO info = {sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
	if (style & LBS_DISABLENOSCROLL)
		info.fMask |= SIF_DISABLENOSCROLL;
	RECT client;
	GetClientRect(window, &client);
	if (style & LBS_MULTICOLUMN) {
		int rows = rows_per_column(window, state);
		info.nMax = state->count ? (state->count + rows - 1) / rows - 1 : 0;
		info.nPage = visible_columns(window, state);
		info.nPos = state->top / rows;
		if (state->horizontal_bar)
			SetScrollInfo(window, SB_HORZ, &info, TRUE);
		return;
	}
	if (state->vertical_bar) {
		info.nMax = state->count ? state->count - 1 : 0;
		info.nPage = items_per_page(window, state, state->top);
		info.nPos = state->top;
		SetScrollInfo(window, SB_VERT, &info, TRUE);
	}
	if (state->horizontal_bar) {
		info.nMax = state->horizontal_extent ? state->horizontal_extent - 1 : 0;
		info.nPage = client.right;
		info.nPos = state->horizontal_position;
		SetScrollInfo(window, SB_HORZ, &info, TRUE);
	}
}

static void invalidate_all(HWND window, const list_state *state)
{
	if (state->redraw)
		InvalidateRect(window, NULL, TRUE);
}

static void invalidate_item(HWND window, const list_state *state, int index)
{
	if (!state->redraw || index < 0 || index >= state->count)
		return;
	RECT rect;
	item_rect(window, state, index, &rect);
	InvalidateRect(window, &rect, TRUE);
}

static void set_top(HWND window, list_state *state, int top)
{
	int highest = highest_top(window, state);
	if (top > highest)
		top = highest;
	if (top < 0)
		top = 0;
	if (style_of(window) & LBS_MULTICOLUMN) {
		int rows = rows_per_column(window, state);
		top -= top % rows;
	}
	if (top == state->top)
		return;
	state->top = top;
	update_scroll_bars(window, state);
	invalidate_all(window, state);
}

static void make_visible(HWND window, list_state *state, int index)
{
	if (index < 0 || index >= state->count)
		return;
	if (style_of(window) & LBS_MULTICOLUMN) {
		int rows = rows_per_column(window, state), columns = visible_columns(window, state);
		int column = index / rows, first = state->top / rows;
		if (column < first)
			set_top(window, state, column * rows);
		else if (column >= first + columns)
			set_top(window, state, (column - columns + 1) * rows);
		return;
	}
	if (index < state->top) {
		set_top(window, state, index);
		return;
	}
	if (!state->owner_variable) {
		int page = items_per_page(window, state, state->top);
		if (index >= state->top + page)
			set_top(window, state, index - page + 1);
		return;
	}
	int top = state->top;
	while (top < index && index >= top + items_per_page(window, state, top))
		++top;
	set_top(window, state, top);
}

static BOOL grow(list_state *state, int needed)
{
	if (needed <= state->capacity)
		return TRUE;
	int capacity = state->capacity ? state->capacity : 16;
	while (capacity < needed)
		capacity *= 2;
	list_item *items = state->items ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, state->items, capacity * sizeof(*items))
					: HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, capacity * sizeof(*items));
	if (!items)
		return FALSE;
	state->items = items;
	state->capacity = capacity;
	return TRUE;
}

static int compare_text(const list_state *state, const WCHAR *left, const WCHAR *right)
{
	return CompareStringW(state->locale, NORM_IGNORECASE, left, -1, right, -1) - CSTR_EQUAL;
}

/* The owner decides the order of items that have no text. */
static int compare_with_owner(HWND window, const list_state *state, int index, ULONG_PTR data)
{
	COMPAREITEMSTRUCT compare = {ODT_LISTBOX, (UINT)GetDlgCtrlID(window), window, (UINT)index, state->items[index].data, (UINT)-1, data, state->locale};
	return (int)SendMessageW(state->owner, WM_COMPAREITEM, compare.CtlID, (LPARAM)&compare);
}

/* Where a new item goes to keep the order: after the items equal to it. */
static int sorted_position(HWND window, const list_state *state, LPARAM value)
{
	int low = 0, high = state->count;
	while (low < high) {
		int middle = low + (high - low) / 2;
		int order = state->has_strings ? compare_text(state, state->items[middle].text, (const WCHAR *)value) : compare_with_owner(window, state, middle, value);
		if (order <= 0)
			low = middle + 1;
		else
			high = middle;
	}
	return low;
}

static void measure_variable(HWND window, const list_state *state, int index, list_item *item)
{
	MEASUREITEMSTRUCT measure = {ODT_LISTBOX, (UINT)GetDlgCtrlID(window), (UINT)index, 0, (UINT)state->item_height, 0};
	measure.itemData = state->has_strings ? (ULONG_PTR)item->text : item->data;
	SendMessageW(state->owner, WM_MEASUREITEM, measure.CtlID, (LPARAM)&measure);
	item->height = measure.itemHeight > 0 ? (int)measure.itemHeight : state->item_height;
}

static int insert_item(HWND window, list_state *state, int index, LPARAM value)
{
	if (index < 0 || index > state->count)
		return LB_ERR;
	WCHAR *text = NULL;
	if (state->has_strings) {
		const WCHAR *source = value ? (const WCHAR *)value : L"";
		size_t bytes = (lstrlenW(source) + 1) * sizeof(WCHAR);
		text = HeapAlloc(GetProcessHeap(), 0, bytes);
		if (!text) {
			notify(window, state, LBN_ERRSPACE);
			return LB_ERRSPACE;
		}
		CopyMemory(text, source, bytes);
	}
	if (!grow(state, state->count + 1)) {
		if (text)
			HeapFree(GetProcessHeap(), 0, text);
		notify(window, state, LBN_ERRSPACE);
		return LB_ERRSPACE;
	}
	MoveMemory(&state->items[index + 1], &state->items[index], (state->count - index) * sizeof(*state->items));
	list_item *item = &state->items[index];
	ZeroMemory(item, sizeof(*item));
	item->text = text;
	if (!state->has_strings && !state->no_data)
		item->data = (ULONG_PTR)value;
	++state->count;
	if (state->owner_variable)
		measure_variable(window, state, index, item);
	if (!state->multiple && state->selected >= index)
		++state->selected;
	if (state->count > 1 && state->caret >= index)
		++state->caret;
	if (state->anchor >= index)
		++state->anchor;
	update_scroll_bars(window, state);
	invalidate_all(window, state);
	return index;
}

static void release_item(HWND window, const list_state *state, int index)
{
	list_item *item = &state->items[index];
	if (is_owner_draw(state) || item->data) {
		DELETEITEMSTRUCT deleted = {ODT_LISTBOX, (UINT)GetDlgCtrlID(window), (UINT)index, window, item->data};
		SendMessageW(state->owner, WM_DELETEITEM, deleted.CtlID, (LPARAM)&deleted);
	}
	if (item->text)
		HeapFree(GetProcessHeap(), 0, item->text);
}

static int delete_item(HWND window, list_state *state, int index)
{
	if (index < 0 || index >= state->count)
		return LB_ERR;
	release_item(window, state, index);
	--state->count;
	MoveMemory(&state->items[index], &state->items[index + 1], (state->count - index) * sizeof(*state->items));
	if (!state->multiple) {
		if (state->selected == index)
			state->selected = -1;
		else if (state->selected > index)
			--state->selected;
	}
	if (state->caret > index || (state->caret == index && state->caret == state->count && state->caret > 0))
		--state->caret;
	if (state->anchor == index)
		state->anchor = -1;
	else if (state->anchor > index)
		--state->anchor;
	int highest = highest_top(window, state);
	if (state->top > highest)
		state->top = highest;
	update_scroll_bars(window, state);
	invalidate_all(window, state);
	return state->count;
}

static void reset_content(HWND window, list_state *state)
{
	for (int index = 0; index < state->count; ++index)
		release_item(window, state, index);
	state->count = 0;
	state->top = 0;
	state->caret = 0;
	state->anchor = -1;
	state->selected = -1;
	state->horizontal_position = 0;
	update_scroll_bars(window, state);
	invalidate_all(window, state);
}

/* ---- selection ---- */

static void set_item_selected(HWND window, list_state *state, int index, BOOL selected)
{
	if (state->items[index].selected == selected)
		return;
	state->items[index].selected = selected;
	invalidate_item(window, state, index);
}

static void select_range(HWND window, list_state *state, int first, int last, BOOL selected)
{
	if (first < 0)
		first = 0;
	if (last >= state->count)
		last = state->count - 1;
	for (int index = first; index <= last; ++index)
		set_item_selected(window, state, index, selected);
}

static void set_caret(HWND window, list_state *state, int index)
{
	if (state->caret == index)
		return;
	if (state->focused)
		invalidate_item(window, state, state->caret);
	state->caret = index;
	if (state->focused)
		invalidate_item(window, state, index);
}

static void set_single(HWND window, list_state *state, int index)
{
	if (state->selected == index)
		return;
	invalidate_item(window, state, state->selected);
	state->selected = index;
	invalidate_item(window, state, index);
}

/* Moves to an item as the keyboard or the mouse does it. */
static void pick_item(HWND window, list_state *state, int index, BOOL shift, BOOL control)
{
	if (index < 0 || index >= state->count)
		return;
	if (!state->multiple) {
		set_single(window, state, index);
		state->anchor = index;
	} else if (state->extended) {
		if (shift) {
			int from = state->anchor >= 0 ? state->anchor : state->caret;
			int low = from < index ? from : index, high = from < index ? index : from;
			if (!control)
				select_range(window, state, 0, state->count - 1, FALSE);
			select_range(window, state, low, high, TRUE);
			if (state->anchor < 0)
				state->anchor = from;
		} else if (control) {
			state->anchor = index;
		} else {
			select_range(window, state, 0, state->count - 1, FALSE);
			set_item_selected(window, state, index, TRUE);
			state->anchor = index;
		}
	}
	set_caret(window, state, index);
	make_visible(window, state, index);
}

/* ---- painting ---- */

static HBRUSH prepare_colors(HWND window, const list_state *state, HDC dc)
{
	HBRUSH brush = (HBRUSH)SendMessageW(state->owner ? state->owner : window, WM_CTLCOLORLISTBOX, (WPARAM)dc, (LPARAM)window);
	return brush ? brush : GetSysColorBrush(COLOR_WINDOW);
}

static void draw_owner_item(HWND window, const list_state *state, HDC dc, int index, const RECT *rect, UINT action)
{
	DRAWITEMSTRUCT item = {ODT_LISTBOX, (UINT)GetDlgCtrlID(window), (UINT)index, action, 0, window, dc, *rect, 0};
	if (index >= 0 && index < state->count) {
		item.itemData = state->items[index].data;
		if (state->multiple ? state->items[index].selected : state->selected == index)
			item.itemState |= ODS_SELECTED;
	}
	if (state->focused && state->caret == index)
		item.itemState |= ODS_FOCUS;
	if (!IsWindowEnabled(window))
		item.itemState |= ODS_DISABLED;
	SendMessageW(state->owner, WM_DRAWITEM, item.CtlID, (LPARAM)&item);
}

static void draw_text_item(HWND window, const list_state *state, HDC dc, int index, const RECT *rect, HBRUSH brush, COLORREF text_color)
{
	const list_item *item = &state->items[index];
	BOOL selected = state->multiple ? item->selected : state->selected == index;
	FillRect(dc, rect, selected ? GetSysColorBrush(COLOR_HIGHLIGHT) : brush);
	SetTextColor(dc, selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : text_color);
	if (item->text && *item->text) {
		int x = rect->left + TEXT_MARGIN, length = lstrlenW(item->text);
		if (style_of(window) & LBS_USETABSTOPS)
			TabbedTextOutW(dc, x, rect->top, item->text, length, state->tab_count, state->tab_stops, x);
		else
			ExtTextOutW(dc, x, rect->top, ETO_CLIPPED, rect, item->text, length, NULL);
	}
	if (state->focused && state->caret == index)
		DrawFocusRect(dc, rect);
}

static void paint(HWND window, list_state *state, HDC dc)
{
	RECT client;
	GetClientRect(window, &client);
	HGDIOBJ previous = state->font ? SelectObject(dc, state->font) : NULL;
	HBRUSH brush = prepare_colors(window, state, dc);
	COLORREF text_color = IsWindowEnabled(window) ? GetTextColor(dc) : GetSysColor(COLOR_GRAYTEXT);
	int mode = SetBkMode(dc, TRANSPARENT);
	int saved = SaveDC(dc);
	IntersectClipRect(dc, 0, 0, client.right, client.bottom);
	HRGN uncovered = CreateRectRgn(0, 0, client.right, client.bottom);
	for (int index = state->top; index < state->count; ++index) {
		RECT rect;
		item_rect(window, state, index, &rect);
		if (rect.left >= client.right || (rect.top >= client.bottom && !(style_of(window) & LBS_MULTICOLUMN)))
			break;
		if (rect.top >= client.bottom)
			continue;
		if (is_owner_draw(state))
			draw_owner_item(window, state, dc, index, &rect, ODA_DRAWENTIRE);
		else
			draw_text_item(window, state, dc, index, &rect, brush, text_color);
		if (uncovered) {
			HRGN item_region = CreateRectRgnIndirect(&rect);
			if (item_region) {
				CombineRgn(uncovered, uncovered, item_region, RGN_DIFF);
				DeleteObject(item_region);
			}
		}
	}
	if (uncovered) {
		FillRgn(dc, uncovered, brush);
		DeleteObject(uncovered);
	}
	/* An empty owner-drawn list still has to show that it holds the focus. */
	if (!state->count && state->focused && is_owner_draw(state)) {
		RECT rect = {0, 0, client.right, state->item_height};
		draw_owner_item(window, state, dc, -1, &rect, ODA_FOCUS);
	}
	RestoreDC(dc, saved);
	SetBkMode(dc, mode);
	if (previous)
		SelectObject(dc, previous);
}

/* ---- lookup ---- */

static int find_item(HWND window, const list_state *state, int start, LPARAM value, BOOL exact)
{
	if (state->no_data || !state->count)
		return LB_ERR;
	if (start < 0 || start >= state->count)
		start = -1;
	int prefix = state->has_strings && value ? lstrlenW((const WCHAR *)value) : 0;
	for (int step = 1; step <= state->count; ++step) {
		int index = (start + step) % state->count;
		const list_item *item = &state->items[index];
		if (state->has_strings) {
			if (!value)
				continue;
			if (exact ? !compare_text(state, item->text, (const WCHAR *)value)
				  : CompareStringW(state->locale, NORM_IGNORECASE, item->text, min(prefix, lstrlenW(item->text)), (const WCHAR *)value, prefix) == CSTR_EQUAL)
				return index;
		} else if (state->sorted ? !compare_with_owner(window, state, index, value) : item->data == (ULONG_PTR)value)
			return index;
	}
	return LB_ERR;
}

/* The item under a point, the nearest one when the point is outside; `outside` reports which it was. */
static int item_at_point(HWND window, const list_state *state, int x, int y, BOOL *outside)
{
	RECT client;
	GetClientRect(window, &client);
	*outside = x < 0 || x >= client.right || y < 0 || y >= client.bottom;
	if (!state->count) {
		*outside = TRUE;
		return -1;
	}
	if (style_of(window) & LBS_MULTICOLUMN) {
		int rows = rows_per_column(window, state);
		int column = x < 0 ? 0 : x / state->column_width;
		int row = y < 0 ? 0 : y / state->item_height;
		if (row >= rows)
			row = rows - 1;
		int index = state->top + column * rows + row;
		if (index >= state->count) {
			*outside = TRUE;
			index = state->count - 1;
		}
		return index;
	}
	if (y < 0)
		return state->top;
	int bottom = 0;
	for (int index = state->top; index < state->count; ++index) {
		bottom += height_of(state, index);
		if (y < bottom && y < client.bottom)
			return index;
		if (bottom >= client.bottom)
			return bottom > client.bottom && index > state->top && !state->owner_variable && y >= client.bottom ? index - 1 : index;
	}
	*outside = TRUE;
	return state->count - 1;
}

static BOOL add_directory_entry(HWND window, list_state *state, const WCHAR *text)
{
	LPARAM value = (LPARAM)text;
	int index = state->sorted ? sorted_position(window, state, value) : state->count;
	return insert_item(window, state, index, value) >= 0;
}

/* Fills the list with the files of a pattern, and the directories and drives the attributes ask for. */
static int list_directory(HWND window, list_state *state, UINT attributes, const WCHAR *pattern)
{
	int last = LB_ERR;
	WIN32_FIND_DATAW found;
	WCHAR entry[MAX_PATH + 4];
	if (!state->has_strings)
		return LB_ERR;
	if (pattern) {
		HANDLE search = FindFirstFileW(pattern, &found);
		if (search == INVALID_HANDLE_VALUE) {
			if (!(attributes & DDL_DRIVES))
				return LB_ERR;
		} else {
			do {
				DWORD file = found.dwFileAttributes;
				BOOL directory = (file & FILE_ATTRIBUTE_DIRECTORY) != 0;
				if (directory) {
					if (!(attributes & DDL_DIRECTORY) || !lstrcmpW(found.cFileName, L"."))
						continue;
					wsprintfW(entry, L"[%s]", found.cFileName);
				} else {
					if (attributes & DDL_EXCLUSIVE) {
						if (!(file & attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE)))
							continue;
					} else if (file & ~attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
						continue;
					lstrcpynW(entry, found.cFileName, MAX_PATH);
				}
				if (add_directory_entry(window, state, entry))
					last = state->count - 1;
			} while (FindNextFileW(search, &found));
			FindClose(search);
		}
	}
	if (attributes & DDL_DRIVES) {
		DWORD drives = GetLogicalDrives();
		for (int letter = 0; letter < 26; ++letter) {
			if (!(drives & (1u << letter)))
				continue;
			wsprintfW(entry, L"[-%c-]", L'a' + letter);
			if (add_directory_entry(window, state, entry))
				last = state->count - 1;
		}
	}
	return last;
}

/* ---- input ---- */

static void type_ahead(HWND window, list_state *state, WCHAR character)
{
	DWORD now = GetTickCount();
	if (now - state->typed_at > TYPE_AHEAD_MS || state->typed_length >= (int)(sizeof(state->typed) / sizeof(*state->typed)) - 1)
		state->typed_length = 0;
	state->typed_at = now;
	state->typed[state->typed_length++] = character;
	state->typed[state->typed_length] = 0;
	/* A new search starts after the caret, and a growing one stays on the item it found. */
	int start = state->typed_length == 1 ? state->caret : state->caret - 1;
	int index = find_item(window, state, start, (LPARAM)state->typed, FALSE);
	if (index != LB_ERR) {
		pick_item(window, state, index, FALSE, FALSE);
		notify(window, state, LBN_SELCHANGE);
	}
}

static void key_down(HWND window, list_state *state, WPARAM key)
{
	BOOL shift = GetKeyState(VK_SHIFT) < 0, control = GetKeyState(VK_CONTROL) < 0;
	BOOL columns = (style_of(window) & LBS_MULTICOLUMN) != 0;
	int rows = rows_per_column(window, state);
	int page = items_per_page(window, state, state->top);
	int target = -1;
	if (style_of(window) & LBS_WANTKEYBOARDINPUT) {
		LRESULT answer = SendMessageW(state->owner, WM_VKEYTOITEM, MAKEWPARAM(key, state->caret), (LPARAM)window);
		if (answer == -2)
			return;
		if (answer >= 0)
			target = (int)answer;
	}
	if (target < 0) {
		switch (key) {
		case VK_UP:
			target = state->caret - 1;
			break;
		case VK_DOWN:
			target = state->caret + 1;
			break;
		case VK_LEFT:
			if (columns)
				target = state->caret - rows;
			else {
				SendMessageW(window, WM_HSCROLL, SB_LINELEFT, 0);
				return;
			}
			break;
		case VK_RIGHT:
			if (columns)
				target = state->caret + rows;
			else {
				SendMessageW(window, WM_HSCROLL, SB_LINERIGHT, 0);
				return;
			}
			break;
		case VK_PRIOR:
			target = state->caret - (columns ? rows * visible_columns(window, state) : (page > 1 ? page - 1 : 1));
			if (target < 0)
				target = 0;
			break;
		case VK_NEXT:
			target = state->caret + (columns ? rows * visible_columns(window, state) : (page > 1 ? page - 1 : 1));
			if (target >= state->count)
				target = state->count - 1;
			break;
		case VK_HOME:
			target = 0;
			break;
		case VK_END:
			target = state->count - 1;
			break;
		case VK_SPACE:
			if (state->multiple && !state->extended && state->count) {
				set_item_selected(window, state, state->caret, !state->items[state->caret].selected);
				state->anchor = state->caret;
				notify(window, state, LBN_SELCHANGE);
			} else if (state->extended && control && state->count) {
				set_item_selected(window, state, state->caret, !state->items[state->caret].selected);
				state->anchor = state->caret;
				notify(window, state, LBN_SELCHANGE);
			}
			return;
		default:
			return;
		}
	}
	if (target < 0 || target >= state->count)
		return;
	pick_item(window, state, target, shift, control);
	/* A plain multiple-selection list moves only the caret. */
	if (!state->multiple || state->extended)
		notify(window, state, LBN_SELCHANGE);
}

static void mouse_down(HWND window, list_state *state, int x, int y, WPARAM keys)
{
	BOOL outside;
	int index = item_at_point(window, state, x, y, &outside);
	if (state->in_combo) {
		RECT client;
		GetClientRect(window, &client);
		if (x < 0 || y < 0 || x >= client.right || y >= client.bottom) {
			POINT point = {x, y};
			RECT frame;
			ClientToScreen(window, &point);
			GetWindowRect(window, &frame);
			if (PtInRect(&frame, point)) {
				/* The scroll bar of the drop-down: it needs the mouse for itself while it tracks. */
				LRESULT hit = SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
				state->tracking_bar = TRUE;
				ReleaseCapture();
				DefWindowProcW(window, WM_NCLBUTTONDOWN, hit, MAKELPARAM(point.x, point.y));
				SetCapture(window);
				state->tracking_bar = FALSE;
				return;
			}
			notify(window, state, LBN_SELCANCEL);
			return;
		}
		state->captured = TRUE;
		if (index >= 0)
			pick_item(window, state, index, FALSE, FALSE);
		return;
	}
	SetFocus(window);
	state->captured = TRUE;
	state->moved = FALSE;
	SetCapture(window);
	if (index < 0)
		return;
	if (state->multiple && !state->extended) {
		set_item_selected(window, state, index, !state->items[index].selected);
		state->anchor = index;
		set_caret(window, state, index);
		make_visible(window, state, index);
	} else if (state->extended && (keys & MK_CONTROL) && !(keys & MK_SHIFT)) {
		set_item_selected(window, state, index, !state->items[index].selected);
		state->anchor = index;
		set_caret(window, state, index);
		make_visible(window, state, index);
	} else
		pick_item(window, state, index, (keys & MK_SHIFT) != 0, (keys & MK_CONTROL) != 0);
	SetTimer(window, SCROLL_TIMER, SCROLL_TIMER_MS, NULL);
}

static void mouse_track(HWND window, list_state *state, int x, int y, WPARAM keys)
{
	BOOL outside;
	int index = item_at_point(window, state, x, y, &outside);
	if (index < 0)
		return;
	if (state->in_combo) {
		/* The drop-down follows the mouse, buttons or not. */
		if (!outside && index != state->selected) {
			set_single(window, state, index);
			set_caret(window, state, index);
		}
		return;
	}
	if (!state->captured || index == state->caret)
		return;
	/* A plain multiple-selection list toggles on the press alone. */
	if (state->multiple && !state->extended)
		return;
	pick_item(window, state, index, state->extended, (keys & MK_CONTROL) != 0);
	state->moved = TRUE;
}

static void mouse_up(HWND window, list_state *state, int x, int y)
{
	/* The press that opened a drop-down happened outside, so its release may come here after a drag. */
	if (state->in_combo) {
		RECT client;
		GetClientRect(window, &client);
		state->captured = FALSE;
		if (x >= 0 && y >= 0 && x < client.right && y < client.bottom)
			notify(window, state, LBN_SELCHANGE);
		return;
	}
	if (!state->captured)
		return;
	state->captured = FALSE;
	KillTimer(window, SCROLL_TIMER);
	ReleaseCapture();
	notify(window, state, LBN_SELCHANGE);
}

static void scroll_vertical(HWND window, list_state *state, UINT code)
{
	int page = items_per_page(window, state, state->top);
	switch (code) {
	case SB_LINEUP:
		set_top(window, state, state->top - 1);
		break;
	case SB_LINEDOWN:
		set_top(window, state, state->top + 1);
		break;
	case SB_PAGEUP:
		set_top(window, state, state->top - (page > 1 ? page - 1 : 1));
		break;
	case SB_PAGEDOWN:
		set_top(window, state, state->top + (page > 1 ? page - 1 : 1));
		break;
	case SB_TOP:
		set_top(window, state, 0);
		break;
	case SB_BOTTOM:
		set_top(window, state, state->count);
		break;
	case SB_THUMBPOSITION:
	case SB_THUMBTRACK: {
		SCROLLINFO info = {sizeof(info), SIF_TRACKPOS};
		if (GetScrollInfo(window, SB_VERT, &info))
			set_top(window, state, info.nTrackPos);
		break;
	}
	}
}

static void set_horizontal_position(HWND window, list_state *state, int position)
{
	RECT client;
	GetClientRect(window, &client);
	int highest = state->horizontal_extent - client.right;
	if (position > highest)
		position = highest;
	if (position < 0)
		position = 0;
	if (position == state->horizontal_position)
		return;
	state->horizontal_position = position;
	update_scroll_bars(window, state);
	invalidate_all(window, state);
}

static void scroll_horizontal(HWND window, list_state *state, UINT code)
{
	RECT client;
	GetClientRect(window, &client);
	if (style_of(window) & LBS_MULTICOLUMN) {
		int rows = rows_per_column(window, state), columns = visible_columns(window, state);
		switch (code) {
		case SB_LINELEFT:
			set_top(window, state, state->top - rows);
			break;
		case SB_LINERIGHT:
			set_top(window, state, state->top + rows);
			break;
		case SB_PAGELEFT:
			set_top(window, state, state->top - rows * columns);
			break;
		case SB_PAGERIGHT:
			set_top(window, state, state->top + rows * columns);
			break;
		case SB_LEFT:
			set_top(window, state, 0);
			break;
		case SB_RIGHT:
			set_top(window, state, state->count);
			break;
		case SB_THUMBPOSITION:
		case SB_THUMBTRACK: {
			SCROLLINFO info = {sizeof(info), SIF_TRACKPOS};
			if (GetScrollInfo(window, SB_HORZ, &info))
				set_top(window, state, info.nTrackPos * rows);
			break;
		}
		}
		return;
	}
	switch (code) {
	case SB_LINELEFT:
		set_horizontal_position(window, state, state->horizontal_position - 8);
		break;
	case SB_LINERIGHT:
		set_horizontal_position(window, state, state->horizontal_position + 8);
		break;
	case SB_PAGELEFT:
		set_horizontal_position(window, state, state->horizontal_position - client.right);
		break;
	case SB_PAGERIGHT:
		set_horizontal_position(window, state, state->horizontal_position + client.right);
		break;
	case SB_LEFT:
		set_horizontal_position(window, state, 0);
		break;
	case SB_RIGHT:
		set_horizontal_position(window, state, state->horizontal_extent);
		break;
	case SB_THUMBPOSITION:
	case SB_THUMBTRACK: {
		SCROLLINFO info = {sizeof(info), SIF_TRACKPOS};
		if (GetScrollInfo(window, SB_HORZ, &info))
			set_horizontal_position(window, state, info.nTrackPos);
		break;
	}
	}
}

/* ---- metrics ---- */

static int font_height(HWND window, const list_state *state)
{
	TEXTMETRICW metrics;
	HDC dc = GetDC(window);
	if (!dc)
		return 16;
	HGDIOBJ previous = state->font ? SelectObject(dc, state->font) : NULL;
	GetTextMetricsW(dc, &metrics);
	if (previous)
		SelectObject(dc, previous);
	ReleaseDC(window, dc);
	return metrics.tmHeight;
}

/* Tab stops are given in dialog units: a quarter of the average character width of the system font. */
static void set_tab_stops(HWND window, list_state *state, int count, const INT *stops)
{
	if (state->tab_stops)
		HeapFree(GetProcessHeap(), 0, state->tab_stops);
	state->tab_stops = NULL;
	state->tab_count = 0;
	int base = LOWORD(GetDialogBaseUnits());
	/* No array means the default stop, a single value a regular interval. */
	int default_stop = 32;
	if (!count) {
		stops = &default_stop;
		count = 1;
	}
	state->tab_stops = HeapAlloc(GetProcessHeap(), 0, count * sizeof(*state->tab_stops));
	if (!state->tab_stops)
		return;
	for (int index = 0; index < count; ++index)
		state->tab_stops[index] = MulDiv(stops[index], base, 4);
	state->tab_count = count;
	invalidate_all(window, state);
}

static void fit_integral_height(HWND window, list_state *state)
{
	DWORD style = style_of(window);
	if (state->resizing || state->in_combo || (style & (LBS_NOINTEGRALHEIGHT | LBS_MULTICOLUMN)) || state->owner_variable)
		return;
	RECT client;
	GetClientRect(window, &client);
	int extra = client.bottom % state->item_height;
	if (!extra || client.bottom < state->item_height)
		return;
	RECT frame;
	GetWindowRect(window, &frame);
	state->resizing = TRUE;
	SetWindowPos(window, NULL, 0, 0, frame.right - frame.left, frame.bottom - frame.top - extra, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
	state->resizing = FALSE;
}

static LRESULT create_state(HWND window, const CREATESTRUCTW *create)
{
	list_state *state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
	if (!state)
		return FALSE;
	SetWindowLongPtrW(window, 0, (LONG_PTR)state);
	DWORD style = create->style;
	state->owner = create->hwndParent;
	/* A simple combo box shows its list like any other; only the popup one tracks the mouse on its own. */
	state->in_combo = (style & LBS_COMBOBOX) && (style & WS_POPUP);
	state->owner_fixed = (style & LBS_OWNERDRAWFIXED) != 0;
	state->owner_variable = (style & LBS_OWNERDRAWVARIABLE) != 0;
	if (state->owner_variable)
		state->owner_fixed = FALSE;
	state->has_strings = !is_owner_draw(state) || (style & LBS_HASSTRINGS);
	state->sorted = (style & LBS_SORT) != 0;
	state->no_data = (style & LBS_NODATA) && state->owner_fixed && !state->has_strings && !state->sorted;
	/* The selection model is settled at creation; a later style change does not move it. */
	if (!(style & LBS_NOSEL)) {
		state->extended = (style & LBS_EXTENDEDSEL) != 0;
		state->multiple = state->extended || (style & LBS_MULTIPLESEL);
	}
	/* A bar that is not needed drops its style bit, so whether the list has one is settled here too. */
	state->vertical_bar = (style & WS_VSCROLL) != 0;
	state->horizontal_bar = (style & WS_HSCROLL) != 0;
	state->anchor = -1;
	state->selected = -1;
	state->column_width = DEFAULT_COLUMN_WIDTH;
	state->locale = GetUserDefaultLCID();
	state->redraw = !(style & LBS_NOREDRAW);
	state->item_height = font_height(window, state);
	if (state->item_height < 1)
		state->item_height = 1;
	return TRUE;
}

/* The owner of a fixed-height list is asked once for the height of all its items. */
static void measure_fixed(HWND window, list_state *state)
{
	if (!state->owner_fixed)
		return;
	MEASUREITEMSTRUCT measure = {ODT_LISTBOX, (UINT)GetDlgCtrlID(window), 0, 0, (UINT)state->item_height, 0};
	SendMessageW(state->owner, WM_MEASUREITEM, measure.CtlID, (LPARAM)&measure);
	if (measure.itemHeight > 0)
		state->item_height = measure.itemHeight > MAX_ITEM_HEIGHT ? MAX_ITEM_HEIGHT : (int)measure.itemHeight;
}

DWORD WINAPI GetListBoxInfo(HWND window)
{
	return (DWORD)SendMessageW(window, LB_GETLISTBOXINFO, 0, 0);
}

LRESULT WINAPI RavenListBoxProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	list_state *state = state_of(window);
	if (!state && message != WM_NCCREATE)
		return DefWindowProcW(window, message, wparam, lparam);
	switch (message) {
	case WM_NCCREATE:
		if (!create_state(window, (const CREATESTRUCTW *)lparam))
			return FALSE;
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_CREATE:
		measure_fixed(window, state);
		fit_integral_height(window, state);
		update_scroll_bars(window, state);
		return 0;
	case WM_NCDESTROY:
		reset_content(window, state);
		if (state->items)
			HeapFree(GetProcessHeap(), 0, state->items);
		if (state->tab_stops)
			HeapFree(GetProcessHeap(), 0, state->tab_stops);
		HeapFree(GetProcessHeap(), 0, state);
		SetWindowLongPtrW(window, 0, 0);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_SIZE:
		fit_integral_height(window, state);
		set_top(window, state, state->top);
		update_scroll_bars(window, state);
		invalidate_all(window, state);
		return 0;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = wparam ? (HDC)wparam : BeginPaint(window, &ps);
		if (state->redraw || wparam)
			paint(window, state, dc);
		if (!wparam)
			EndPaint(window, &ps);
		return 0;
	}
	case WM_PRINTCLIENT:
		paint(window, state, (HDC)wparam);
		return 0;
	case WM_ERASEBKGND:
		/* Painting covers every pixel of the client area, and erasing first would flash. */
		return 1;
	case WM_ENABLE:
		invalidate_all(window, state);
		return 0;
	case WM_GETDLGCODE:
		return DLGC_WANTARROWS | DLGC_WANTCHARS;
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		if (!is_owner_draw(state))
			state->item_height = font_height(window, state);
		fit_integral_height(window, state);
		update_scroll_bars(window, state);
		if (lparam)
			invalidate_all(window, state);
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_SETREDRAW:
		state->redraw = wparam != 0;
		if (wparam) {
			update_scroll_bars(window, state);
			InvalidateRect(window, NULL, TRUE);
		}
		return 0;
	case WM_MOUSEACTIVATE:
		if (state->in_combo)
			return MA_NOACTIVATE;
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_SETFOCUS:
		/* The focus stays with the combo box while its list is open. */
		if (state->in_combo) {
			if (wparam)
				SetFocus((HWND)wparam);
			return 0;
		}
		state->focused = TRUE;
		invalidate_item(window, state, state->caret);
		notify(window, state, LBN_SETFOCUS);
		return 0;
	case WM_KILLFOCUS:
		state->focused = FALSE;
		invalidate_item(window, state, state->caret);
		notify(window, state, LBN_KILLFOCUS);
		return 0;
	case WM_LBUTTONDOWN:
		mouse_down(window, state, (short)LOWORD(lparam), (short)HIWORD(lparam), wparam);
		return 0;
	case WM_LBUTTONDBLCLK:
		if (state->in_combo) {
			mouse_down(window, state, (short)LOWORD(lparam), (short)HIWORD(lparam), wparam);
			return 0;
		}
		SetFocus(window);
		notify(window, state, LBN_DBLCLK);
		return 0;
	case WM_MOUSEMOVE:
		mouse_track(window, state, (short)LOWORD(lparam), (short)HIWORD(lparam), wparam);
		return 0;
	case WM_LBUTTONUP:
		mouse_up(window, state, (short)LOWORD(lparam), (short)HIWORD(lparam));
		return 0;
	case WM_CAPTURECHANGED:
		/* Something else took the mouse from an open drop-down, which then has no way to close but this. */
		if (state->in_combo) {
			if (!state->tracking_bar && (HWND)lparam != window && IsWindowVisible(window))
				notify(window, state, LBN_SELCANCEL);
		} else if (state->captured) {
			state->captured = FALSE;
			KillTimer(window, SCROLL_TIMER);
		}
		return 0;
	case WM_TIMER:
		/* While dragged outside, the list keeps scrolling towards the mouse. */
		if (wparam == SCROLL_TIMER && state->captured) {
			POINT point;
			RECT client;
			GetCursorPos(&point);
			ScreenToClient(window, &point);
			GetClientRect(window, &client);
			if (point.y < 0 && state->caret > 0)
				pick_item(window, state, state->caret - 1, state->extended, FALSE);
			else if (point.y >= client.bottom && state->caret + 1 < state->count)
				pick_item(window, state, state->caret + 1, state->extended, FALSE);
		}
		return 0;
	case WM_KEYDOWN:
		key_down(window, state, wparam);
		return 0;
	case WM_CHAR:
		if (style_of(window) & LBS_WANTKEYBOARDINPUT && !state->has_strings) {
			LRESULT answer = SendMessageW(state->owner, WM_CHARTOITEM, MAKEWPARAM(wparam, state->caret), (LPARAM)window);
			if (answer >= 0 && answer < state->count) {
				pick_item(window, state, (int)answer, FALSE, FALSE);
				notify(window, state, LBN_SELCHANGE);
			}
			return 0;
		}
		if (state->has_strings && wparam >= L' ')
			type_ahead(window, state, (WCHAR)wparam);
		return 0;
	case WM_VSCROLL:
		scroll_vertical(window, state, LOWORD(wparam));
		return 0;
	case WM_HSCROLL:
		scroll_horizontal(window, state, LOWORD(wparam));
		return 0;
	case WM_MOUSEWHEEL: {
		UINT lines = 3;
		SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
		state->wheel_rest += (short)HIWORD(wparam);
		int notches = state->wheel_rest / WHEEL_DELTA;
		state->wheel_rest -= notches * WHEEL_DELTA;
		if (notches && lines) {
			int page = items_per_page(window, state, state->top);
			int step = lines == WHEEL_PAGESCROLL || (int)lines > page ? page : (int)lines;
			set_top(window, state, state->top - notches * step);
		}
		return 0;
	}
	case LB_ADDSTRING:
	case LB_ADDFILE: {
		int index = state->sorted ? sorted_position(window, state, lparam) : state->count;
		return insert_item(window, state, index, lparam);
	}
	case LB_INSERTSTRING: {
		int index = (int)wparam;
		return insert_item(window, state, index == -1 ? state->count : index, lparam);
	}
	case LB_DELETESTRING:
		return delete_item(window, state, (int)wparam);
	case LB_RESETCONTENT:
		reset_content(window, state);
		return 0;
	case LB_GETCOUNT:
		return state->count;
	case LB_INITSTORAGE:
		if (state->count + (int)wparam > state->capacity && !grow(state, state->count + (int)wparam))
			return LB_ERRSPACE;
		return state->capacity;
	case LB_SETCOUNT: {
		if (!state->no_data) {
			SetLastError(ERROR_SETCOUNT_ON_BAD_LB);
			return LB_ERR;
		}
		int wanted = (int)wparam;
		if (wanted < 0 || !grow(state, wanted))
			return LB_ERRSPACE;
		if (wanted > state->count)
			ZeroMemory(&state->items[state->count], (wanted - state->count) * sizeof(*state->items));
		state->count = wanted;
		if (state->top > highest_top(window, state))
			state->top = highest_top(window, state);
		if (state->selected >= wanted)
			state->selected = -1;
		if (state->caret >= wanted)
			state->caret = wanted ? wanted - 1 : 0;
		if (state->anchor >= wanted)
			state->anchor = -1;
		update_scroll_bars(window, state);
		invalidate_all(window, state);
		return 0;
	}
	case LB_GETTEXTLEN:
		if ((int)wparam < 0 || (int)wparam >= state->count || state->no_data)
			return LB_ERR;
		return state->has_strings ? lstrlenW(state->items[wparam].text) : (LRESULT)sizeof(ULONG_PTR);
	case LB_GETTEXT:
		if ((int)wparam < 0 || (int)wparam >= state->count || state->no_data || !lparam)
			return LB_ERR;
		if (!state->has_strings) {
			/* Items without text hand out their value in place of a string. */
			CopyMemory((void *)lparam, &state->items[wparam].data, sizeof(ULONG_PTR));
			return sizeof(ULONG_PTR);
		}
		lstrcpyW((WCHAR *)lparam, state->items[wparam].text);
		return lstrlenW(state->items[wparam].text);
	case LB_GETITEMDATA:
		if ((int)wparam < 0 || (int)wparam >= state->count)
			return LB_ERR;
		return state->no_data ? 0 : (LRESULT)state->items[wparam].data;
	case LB_SETITEMDATA:
		if ((int)wparam < 0 || (int)wparam >= state->count)
			return LB_ERR;
		if (!state->no_data)
			state->items[wparam].data = (ULONG_PTR)lparam;
		return TRUE;
	case LB_FINDSTRING:
		return find_item(window, state, (int)wparam, lparam, FALSE);
	case LB_FINDSTRINGEXACT:
		return find_item(window, state, (int)wparam, lparam, TRUE);
	case LB_SELECTSTRING: {
		if (state->multiple)
			return LB_ERR;
		int index = find_item(window, state, (int)wparam, lparam, FALSE);
		if (index != LB_ERR) {
			set_single(window, state, index);
			set_caret(window, state, index);
			make_visible(window, state, index);
		}
		return index;
	}
	case LB_GETCURSEL:
		if (state->multiple)
			return state->count ? state->caret : LB_ERR;
		return state->selected;
	case LB_SETCURSEL: {
		int index = (int)wparam;
		if (state->multiple || index >= state->count || index < -1)
			return LB_ERR;
		set_single(window, state, index);
		if (index < 0)
			return LB_ERR;
		set_caret(window, state, index);
		make_visible(window, state, index);
		return index;
	}
	case LB_GETSEL:
		if ((int)wparam < 0 || (int)wparam >= state->count)
			return LB_ERR;
		return state->multiple ? state->items[wparam].selected : state->selected == (int)wparam;
	case LB_SETSEL: {
		int index = (int)lparam;
		if (!state->multiple || index < -1 || index >= state->count)
			return LB_ERR;
		if (index == -1) {
			select_range(window, state, 0, state->count - 1, wparam != 0);
			return 0;
		}
		set_item_selected(window, state, index, wparam != 0);
		state->anchor = index;
		set_caret(window, state, index);
		if (wparam)
			make_visible(window, state, index);
		return 0;
	}
	case LB_GETSELCOUNT: {
		if (!state->multiple)
			return LB_ERR;
		int selected = 0;
		for (int index = 0; index < state->count; ++index)
			selected += state->items[index].selected != 0;
		return selected;
	}
	case LB_GETSELITEMS: {
		if (!state->multiple)
			return LB_ERR;
		int stored = 0;
		INT *out = (INT *)lparam;
		for (int index = 0; index < state->count && stored < (int)wparam; ++index)
			if (state->items[index].selected)
				out[stored++] = index;
		return stored;
	}
	case LB_SELITEMRANGE:
	case LB_SELITEMRANGEEX: {
		if (!state->multiple)
			return LB_ERR;
		if (message == LB_SELITEMRANGEEX) {
			int first = (int)wparam, last = (int)lparam;
			if (first <= last)
				select_range(window, state, first, last, TRUE);
			else
				select_range(window, state, last, first, FALSE);
			return 0;
		}
		/* The two ends are unsigned words, in either order. */
		int first = LOWORD(lparam), last = HIWORD(lparam);
		select_range(window, state, first < last ? first : last, first < last ? last : first, wparam != 0);
		return 0;
	}
	case LB_GETANCHORINDEX:
		return state->anchor;
	case LB_SETANCHORINDEX:
		if ((int)wparam < -1 || (int)wparam >= state->count)
			return LB_ERR;
		state->anchor = (int)wparam;
		return 0;
	case LB_GETCARETINDEX:
		return state->caret;
	case LB_SETCARETINDEX:
		if ((int)wparam < 0 || (int)wparam >= state->count)
			return LB_ERR;
		set_caret(window, state, (int)wparam);
		make_visible(window, state, (int)wparam);
		return 0;
	case LB_GETTOPINDEX:
		return state->top;
	case LB_SETTOPINDEX:
		if ((int)wparam < 0 || ((int)wparam >= state->count && state->count))
			return LB_ERR;
		set_top(window, state, (int)wparam);
		return 0;
	case LB_GETHORIZONTALEXTENT:
		return state->horizontal_extent;
	case LB_SETHORIZONTALEXTENT:
		if (style_of(window) & LBS_MULTICOLUMN)
			return 0;
		state->horizontal_extent = (int)wparam;
		set_horizontal_position(window, state, state->horizontal_position);
		update_scroll_bars(window, state);
		return 0;
	case LB_SETCOLUMNWIDTH:
		if ((int)wparam <= 0)
			return LB_ERR;
		state->column_width = (int)wparam;
		update_scroll_bars(window, state);
		invalidate_all(window, state);
		return 0;
	case LB_GETITEMHEIGHT:
		if (state->owner_variable && ((int)wparam < 0 || (int)wparam >= state->count))
			return state->count ? LB_ERR : state->item_height;
		return height_of(state, (int)wparam);
	case LB_SETITEMHEIGHT: {
		int height = LOWORD(lparam);
		if (height > MAX_ITEM_HEIGHT || height < 1)
			return LB_ERR;
		if (state->owner_variable) {
			if ((int)wparam < 0 || (int)wparam >= state->count)
				return LB_ERR;
			state->items[wparam].height = height;
		} else
			state->item_height = height;
		fit_integral_height(window, state);
		update_scroll_bars(window, state);
		invalidate_all(window, state);
		return 0;
	}
	case LB_GETITEMRECT: {
		int index = (int)wparam;
		if (index < 0 || (index >= state->count && (state->count || index)))
			return LB_ERR;
		RECT *rect = (RECT *)lparam, client;
		item_rect(window, state, index, rect);
		GetClientRect(window, &client);
		/* An item past the visible part still has a rectangle, and the answer says it is hidden. */
		return rect->top < client.bottom && rect->bottom > 0 && rect->left < client.right && rect->right > 0;
	}
	case LB_ITEMFROMPOINT: {
		BOOL outside;
		int index = item_at_point(window, state, (short)LOWORD(lparam), (short)HIWORD(lparam), &outside);
		return MAKELONG(index < 0 ? 0xffff : index, outside);
	}
	case LB_SETTABSTOPS:
		if (!(style_of(window) & LBS_USETABSTOPS))
			return FALSE;
		set_tab_stops(window, state, (int)wparam, (const INT *)lparam);
		return TRUE;
	case LB_GETLOCALE:
		return state->locale;
	case LB_SETLOCALE: {
		LCID previous = state->locale;
		if (!IsValidLocale((LCID)wparam, LCID_INSTALLED))
			return LB_ERR;
		state->locale = (LCID)wparam;
		return previous;
	}
	case LB_DIR:
		return list_directory(window, state, (UINT)wparam, (const WCHAR *)lparam);
	case LB_GETLISTBOXINFO:
		return style_of(window) & LBS_MULTICOLUMN ? rows_per_column(window, state) : items_per_page(window, state, state->top);
	}
	return DefWindowProcW(window, message, wparam, lparam);
}
