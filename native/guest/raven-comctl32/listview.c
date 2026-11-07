/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>

/* The list view. Items are kept as records in display order and every view maps a record index to rectangles through one
 * geometry function, so hit testing, painting and scrolling cannot disagree. Owner data lists keep no records: selection
 * lives in a sorted range list and everything else is asked of the parent. Text is wide inside; ANSI exists only at the
 * message and notification edges. */

#ifndef LVS_EX_TRANSPARENTBKGND
#define LVS_EX_TRANSPARENTBKGND 0x00400000
#endif
#ifndef LVS_EX_HEADERINALLVIEWS
#define LVS_EX_HEADERINALLVIEWS 0x02000000
#endif
#ifndef LVFI_SUBSTRING
#define LVFI_SUBSTRING 0x0004
#endif

#define TEXT_LIMIT 260
#define NO_ITEM (-1)
#define ROW_PADDING 1
#define LABEL_PADDING 2
#define REPORT_MARGIN 2
#define ICON_MARGIN 2
#define HEADER_ITEM_TOP 3
#define DEFAULT_COLUMN_WIDTH 10
#define DEFAULT_LIST_WIDTH 128
#define EMPTY_LIST_WIDTH 8
#define ICON_LABEL_LINES 2
#define SEARCH_RESET_MS 1000
#define EDIT_TIMER 1
#define HEADER_ID 0
#define TIP_TOOL 1

#define IS_CALLBACK(text) ((const void *)(text) == (const void *)LPSTR_TEXTCALLBACKW)
#define NOTIFY_CODE(list, name) ((list)->unicode ? name##W : name##A)

typedef struct {
	WCHAR *text;
	int column;
	int image;
	UINT state;
} cell_record;

typedef struct {
	WCHAR *text;
	int image, indent, id, group;
	UINT state;
	LPARAM param;
	POINT at;
	BOOL placed;
	cell_record *cells;
	int cell_count;
} item_record;

typedef struct {
	int fmt, sub_item, image, left, right;
} column_record;

typedef struct {
	int first, last;
} index_range;

typedef struct {
	RECT bounds, state_icon, icon, label, select;
} item_rects;

typedef struct {
	const WCHAR *text;
	int image, indent;
	UINT state;
	LPARAM param;
	char raw[TEXT_LIMIT * sizeof(WCHAR)];
	WCHAR wide[TEXT_LIMIT];
} item_view;

typedef struct {
	HWND window, parent, header, edit, tips;
	WNDPROC edit_proc;
	int edit_item;
	item_record **items;
	int count, capacity;
	column_record *columns;
	int column_count;
	index_range *ranges;
	int range_count, range_capacity;
	int focus, mark, hot, selected_count, selected_column, next_id, next_slot;
	int view;
	UINT callback_mask;
	DWORD extended, last_style;
	BOOL created, owner_data, unicode, redraw, dirty, columns_dirty, in_layout, has_focus;
	int header_quiet, busy;
	BOOL dead;
	HFONT font, own_font, underline_font;
	HIMAGELIST normal_list, small_list, state_list, own_state_list;
	int normal_cx, normal_cy, small_cx, small_cy, state_cx, state_cy;
	int font_height, item_height, custom_height;
	int spacing_cx, spacing_cy;
	BOOL custom_spacing;
	int list_width;
	BOOL custom_list_width;
	int scroll_row, scroll_x, scroll_column;
	POINT origin;
	int rows_per_column;
	COLORREF back_color, text_color, text_back_color;
	HBITMAP back_image;
	int back_x, back_y;
	ULONG back_flags;
	HCURSOR hot_cursor;
	DWORD hover_time;
	WCHAR search[64];
	int search_length;
	DWORD search_tick;
	int press_item, press_button, tip_item;
	POINT press_at;
	BOOL press_was_selected, press_dragging, press_empty, press_on_label, press_keep;
	RECT band;
	BOOL band_active;
	UINT press_keys;
	RECT work_areas[LV_MAX_WORKAREAS];
	int work_area_count;
	BOOL group_view;
	WCHAR tip_text[TEXT_LIMIT];
} list_state;

static list_state *state_of(HWND window)
{
	return (list_state *)GetWindowLongPtrW(window, 0);
}

static DWORD style_of(const list_state *list)
{
	return (DWORD)GetWindowLongPtrW(list->window, GWL_STYLE);
}

static void *allocate(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void release(void *block)
{
	if (block)
		HeapFree(GetProcessHeap(), 0, block);
}

static WCHAR *copy_wide(const WCHAR *text)
{
	int length = lstrlenW(text) + 1;
	WCHAR *copy = allocate(length * sizeof(WCHAR));
	if (copy)
		CopyMemory(copy, text, length * sizeof(WCHAR));
	return copy;
}

static WCHAR *widen(const char *text)
{
	int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	WCHAR *copy = allocate(length * sizeof(WCHAR));
	if (copy)
		MultiByteToWideChar(CP_ACP, 0, text, -1, copy, length);
	return copy;
}

static void release_text(WCHAR *text)
{
	if (!IS_CALLBACK(text))
		release(text);
}

/* Stored text is NULL for empty and the callback sentinel for text the parent supplies. */
static WCHAR *store_text(const WCHAR *text)
{
	if (IS_CALLBACK(text))
		return LPSTR_TEXTCALLBACKW;
	return text && *text ? copy_wide(text) : NULL;
}

static int copy_text_out(const WCHAR *text, void *target, int capacity, BOOL wide)
{
	if (!target || capacity <= 0)
		return 0;
	if (wide) {
		lstrcpynW(target, text, capacity);
		return lstrlenW(target);
	}
	if (!WideCharToMultiByte(CP_ACP, 0, text, -1, target, capacity, NULL, NULL))
		((char *)target)[capacity - 1] = 0;
	return lstrlenA(target);
}

static HFONT font_of(const list_state *list)
{
	return list->font ? list->font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static LRESULT notify(list_state *list, NMHDR *header, UINT code)
{
	header->hwndFrom = list->window;
	header->idFrom = GetWindowLongPtrW(list->window, GWLP_ID);
	header->code = code;
	return SendMessageW(list->parent, WM_NOTIFY, header->idFrom, (LPARAM)header);
}

static LRESULT notify_plain(list_state *list, UINT code)
{
	NMHDR header;
	return notify(list, &header, code);
}

static LRESULT notify_item(list_state *list, UINT code, int index, UINT fresh, UINT old, UINT changed, LPARAM param)
{
	NMLISTVIEW info;
	ZeroMemory(&info, sizeof(info));
	info.iItem = index;
	info.uNewState = fresh;
	info.uOldState = old;
	info.uChanged = changed;
	info.lParam = param;
	return notify(list, &info.hdr, code);
}

static void mark_dirty(list_state *list)
{
	list->dirty = TRUE;
}

static void repaint(list_state *list)
{
	if (list->redraw)
		InvalidateRect(list->window, NULL, TRUE);
}

/* Owner data selection */

static BOOL range_contains(const list_state *list, int index)
{
	int low = 0, high = list->range_count - 1;
	while (low <= high) {
		int middle = (low + high) / 2;
		if (index < list->ranges[middle].first)
			high = middle - 1;
		else if (index > list->ranges[middle].last)
			low = middle + 1;
		else
			return TRUE;
	}
	return FALSE;
}

static BOOL range_room(list_state *list, int needed)
{
	index_range *grown;
	int capacity;
	if (needed <= list->range_capacity)
		return TRUE;
	capacity = needed * 2 + 8;
	grown = allocate(capacity * sizeof(*grown));
	if (!grown)
		return FALSE;
	if (list->ranges)
		CopyMemory(grown, list->ranges, list->range_count * sizeof(*grown));
	release(list->ranges);
	list->ranges = grown;
	list->range_capacity = capacity;
	return TRUE;
}

static void range_remove(list_state *list, int first, int last)
{
	int index = 0;
	while (index < list->range_count) {
		index_range *range = &list->ranges[index];
		if (range->last < first || range->first > last) {
			index++;
		}
		else if (range->first >= first && range->last <= last) {
			MoveMemory(range, range + 1, (list->range_count - index - 1) * sizeof(*range));
			list->range_count--;
		}
		else if (range->first < first && range->last > last) {
			int tail = range->last;
			if (!range_room(list, list->range_count + 1))
				return;
			range = &list->ranges[index];
			MoveMemory(range + 2, range + 1, (list->range_count - index - 1) * sizeof(*range));
			list->range_count++;
			range[0].last = first - 1;
			range[1].first = last + 1;
			range[1].last = tail;
			index += 2;
		}
		else if (range->first < first) {
			range->last = first - 1;
			index++;
		}
		else {
			range->first = last + 1;
			index++;
		}
	}
}

static void range_add(list_state *list, int first, int last)
{
	int index = 0;
	range_remove(list, first, last);
	if (!range_room(list, list->range_count + 1))
		return;
	while (index < list->range_count && list->ranges[index].first < first)
		index++;
	MoveMemory(&list->ranges[index + 1], &list->ranges[index], (list->range_count - index) * sizeof(index_range));
	list->ranges[index].first = first;
	list->ranges[index].last = last;
	list->range_count++;
	if (index + 1 < list->range_count && list->ranges[index].last + 1 == list->ranges[index + 1].first) {
		list->ranges[index].last = list->ranges[index + 1].last;
		MoveMemory(&list->ranges[index + 1], &list->ranges[index + 2], (list->range_count - index - 2) * sizeof(index_range));
		list->range_count--;
	}
	if (index > 0 && list->ranges[index - 1].last + 1 == list->ranges[index].first) {
		list->ranges[index - 1].last = list->ranges[index].last;
		MoveMemory(&list->ranges[index], &list->ranges[index + 1], (list->range_count - index - 1) * sizeof(index_range));
		list->range_count--;
	}
}

static int range_total(const list_state *list)
{
	int index, total = 0;
	for (index = 0; index < list->range_count; index++)
		total += list->ranges[index].last - list->ranges[index].first + 1;
	return total;
}

static void range_shift_insert(list_state *list, int at)
{
	int index;
	range_remove(list, at, at);
	for (index = 0; index < list->range_count; index++) {
		if (list->ranges[index].first >= at)
			list->ranges[index].first++;
		if (list->ranges[index].last >= at)
			list->ranges[index].last++;
	}
}

static void range_shift_delete(list_state *list, int at)
{
	int index;
	range_remove(list, at, at);
	for (index = 0; index < list->range_count; index++) {
		if (list->ranges[index].first > at)
			list->ranges[index].first--;
		if (list->ranges[index].last > at)
			list->ranges[index].last--;
	}
}

/* Stored state and text */

static UINT stored_state(const list_state *list, int index)
{
	UINT value;
	if (!list->owner_data)
		return list->items[index]->state;
	value = range_contains(list, index) ? LVIS_SELECTED : 0;
	if (index == list->focus)
		value |= LVIS_FOCUSED;
	return value;
}

static cell_record *find_cell(const item_record *item, int column)
{
	int index;
	for (index = 0; index < item->cell_count; index++) {
		if (item->cells[index].column == column)
			return &item->cells[index];
	}
	return NULL;
}

static cell_record *need_cell(item_record *item, int column)
{
	cell_record *cell = find_cell(item, column), *grown;
	if (cell)
		return cell;
	grown = allocate((item->cell_count + 1) * sizeof(*grown));
	if (!grown)
		return NULL;
	if (item->cells)
		CopyMemory(grown, item->cells, item->cell_count * sizeof(*grown));
	release(item->cells);
	item->cells = grown;
	cell = &grown[item->cell_count++];
	cell->column = column;
	cell->image = I_IMAGECALLBACK;
	return cell;
}

/* Resolves one item or subitem for display or for a query, asking the parent only for what is really callback data. */
static void view_item(list_state *list, int index, int sub, UINT ask, UINT state_mask, item_view *view)
{
	NMLVDISPINFOW info;
	item_record *item = list->owner_data ? NULL : list->items[index];
	cell_record *cell = item && sub > 0 ? find_cell(item, sub) : NULL;
	UINT need = 0;
	UINT state_need = state_mask & list->callback_mask;

	view->text = L"";
	view->image = sub > 0 ? I_IMAGECALLBACK : (item ? item->image : 0);
	view->indent = item && sub == 0 ? item->indent : 0;
	view->param = item ? item->param : 0;
	view->state = 0;
	if (sub == 0)
		view->state = stored_state(list, index);
	else if (cell)
		view->state = cell->state;
	view->state &= ~list->callback_mask;

	if (ask & LVIF_TEXT) {
		const WCHAR *text = !item ? LPSTR_TEXTCALLBACKW : sub == 0 ? item->text : cell ? cell->text : NULL;
		if (IS_CALLBACK(text))
			need |= LVIF_TEXT;
		else if (text)
			view->text = text;
	}
	if (ask & LVIF_IMAGE) {
		if (sub > 0) {
			if (cell)
				view->image = cell->image;
			if ((list->extended & LVS_EX_SUBITEMIMAGES) && view->image == I_IMAGECALLBACK)
				need |= LVIF_IMAGE;
		}
		else if (!item || item->image == I_IMAGECALLBACK) {
			need |= LVIF_IMAGE;
		}
	}
	if ((ask & LVIF_INDENT) && sub == 0 && (!item || item->indent == I_INDENTCALLBACK))
		need |= LVIF_INDENT;
	if ((ask & LVIF_PARAM) && !item)
		need |= LVIF_PARAM;
	if ((ask & LVIF_STATE) && state_need)
		need |= LVIF_STATE;
	if (!need)
		return;

	ZeroMemory(&info, sizeof(info));
	info.item.mask = need;
	info.item.iItem = index;
	info.item.iSubItem = sub;
	info.item.stateMask = state_need;
	info.item.iImage = view->image;
	info.item.iIndent = view->indent;
	info.item.lParam = view->param;
	info.item.pszText = list->unicode ? view->wide : (WCHAR *)view->raw;
	info.item.cchTextMax = TEXT_LIMIT;
	view->wide[0] = 0;
	view->raw[0] = 0;
	notify(list, &info.hdr, NOTIFY_CODE(list, LVN_GETDISPINFO));
	if (!list->owner_data && (index >= list->count || list->items[index] != item))
		return;

	if (need & LVIF_IMAGE)
		view->image = info.item.iImage;
	if (need & LVIF_INDENT)
		view->indent = info.item.iIndent == I_INDENTCALLBACK ? 0 : info.item.iIndent;
	if (need & LVIF_PARAM)
		view->param = info.item.lParam;
	if (need & LVIF_STATE)
		view->state |= info.item.state & state_need;
	if (need & LVIF_TEXT) {
		const void *answer = info.item.pszText;
		if (!answer || IS_CALLBACK(answer)) {
			view->text = L"";
		}
		else if (info.hdr.code == LVN_GETDISPINFOW) {
			view->text = answer;
		}
		else {
			/* A handler may rewrite the code: the format it answers in is the one it left there. */
			char narrow[sizeof(view->raw)];
			lstrcpynA(narrow, answer, sizeof(narrow));
			view->wide[0] = 0;
			MultiByteToWideChar(CP_ACP, 0, narrow, -1, view->wide, TEXT_LIMIT);
			view->wide[TEXT_LIMIT - 1] = 0;
			view->text = view->wide;
		}
	}
	if ((info.item.mask & LVIF_DI_SETITEM) && item) {
		if (need & LVIF_TEXT) {
			if (sub == 0) {
				release_text(item->text);
				item->text = store_text(view->text);
			}
			else if ((cell = need_cell(item, sub))) {
				release_text(cell->text);
				cell->text = store_text(view->text);
			}
		}
		if ((need & LVIF_IMAGE) && sub == 0)
			item->image = view->image;
	}
}

static int measure(const list_state *list, const WCHAR *text)
{
	SIZE size = { 0, 0 };
	HDC dc = GetDC(list->window);
	HGDIOBJ old = SelectObject(dc, font_of(list));
	GetTextExtentPoint32W(dc, text, lstrlenW(text), &size);
	SelectObject(dc, old);
	ReleaseDC(list->window, dc);
	return size.cx;
}

/* Layout */

static void refresh_columns(list_state *list)
{
	int index;
	if (!list->columns_dirty)
		return;
	list->columns_dirty = FALSE;
	for (index = 0; index < list->column_count; index++) {
		RECT area;
		SetRectEmpty(&area);
		if (list->header)
			SendMessageW(list->header, HDM_GETITEMRECT, index, (LPARAM)&area);
		list->columns[index].left = area.left;
		list->columns[index].right = area.right;
	}
}

static int total_width(list_state *list)
{
	int index, right = 0;
	refresh_columns(list);
	for (index = 0; index < list->column_count; index++) {
		if (list->columns[index].right > right)
			right = list->columns[index].right;
	}
	return right;
}

static int header_height(const list_state *list)
{
	RECT area;
	if (!list->header || list->view != LV_VIEW_DETAILS || (style_of(list) & LVS_NOCOLUMNHEADER))
		return 0;
	GetWindowRect(list->header, &area);
	return area.bottom - area.top;
}

static void refresh_metrics(list_state *list)
{
	TEXTMETRICW metrics;
	HDC dc = GetDC(list->window);
	HGDIOBJ old = SelectObject(dc, font_of(list));
	int height;
	GetTextMetricsW(dc, &metrics);
	SelectObject(dc, old);
	ReleaseDC(list->window, dc);
	list->font_height = metrics.tmHeight;

	height = metrics.tmHeight;
	if (list->small_cy > height)
		height = list->small_cy;
	if (list->state_cy > height)
		height = list->state_cy;
	height += ROW_PADDING;
	if (list->view == LV_VIEW_DETAILS && (list->extended & LVS_EX_GRIDLINES))
		height++;
	if (list->custom_height > 0)
		height = list->custom_height;
	list->item_height = height;
}

static void default_spacing(list_state *list)
{
	if (list->custom_spacing)
		return;
	list->spacing_cx = GetSystemMetrics(SM_CXICONSPACING) - GetSystemMetrics(SM_CXICON) + list->normal_cx;
	list->spacing_cy = GetSystemMetrics(SM_CYICONSPACING) - GetSystemMetrics(SM_CYICON) + list->normal_cy;
}

static int small_box_width(const list_state *list)
{
	return list->state_cx + list->small_cx + DEFAULT_LIST_WIDTH;
}

static void slot_position(list_state *list, int slot, const RECT *client, POINT *at)
{
	BOOL small = list->view == LV_VIEW_SMALLICON;
	int cx = small ? small_box_width(list) : list->spacing_cx;
	int cy = small ? list->item_height : list->spacing_cy;
	int extent_x = client->right - client->left - ICON_MARGIN, extent_y = client->bottom - client->top - ICON_MARGIN;

	cx = cx > 0 ? cx : 1;
	cy = cy > 0 ? cy : 1;
	if (style_of(list) & LVS_ALIGNLEFT) {
		int rows = extent_y / cy;
		rows = rows > 0 ? rows : 1;
		at->x = ICON_MARGIN + slot / rows * cx;
		at->y = ICON_MARGIN + slot % rows * cy;
	}
	else {
		int columns = extent_x / cx;
		columns = columns > 0 ? columns : 1;
		at->x = ICON_MARGIN + slot % columns * cx;
		at->y = ICON_MARGIN + slot / columns * cy;
	}
}

static void place_items(list_state *list, const RECT *client)
{
	int index;
	BOOL all = (style_of(list) & LVS_AUTOARRANGE) != 0;
	if (list->view != LV_VIEW_ICON && list->view != LV_VIEW_SMALLICON)
		return;
	if (list->owner_data)
		return;
	if (all)
		list->next_slot = 0;
	for (index = 0; index < list->count; index++) {
		item_record *item = list->items[index];
		if (all || !item->placed) {
			slot_position(list, list->next_slot++, client, &item->at);
			item->placed = TRUE;
		}
	}
}

static void refresh_list_width(list_state *list)
{
	int index, widest = 0;
	if (list->custom_list_width)
		return;
	if (!list->count) {
		list->list_width = EMPTY_LIST_WIDTH;
		return;
	}
	if (!list->owner_data && list->count < 4096) {
		for (index = 0; index < list->count; index++) {
			const WCHAR *text = list->items[index]->text;
			if (text && !IS_CALLBACK(text)) {
				int width = measure(list, text);
				if (width > widest)
					widest = width;
			}
		}
	}
	widest += list->state_cx + list->small_cx + 2 * LABEL_PADDING + 8;
	list->list_width = widest > DEFAULT_LIST_WIDTH ? widest : DEFAULT_LIST_WIDTH;
}

static void view_extent(list_state *list, SIZE *size)
{
	size->cx = size->cy = 0;
	if (list->view == LV_VIEW_DETAILS) {
		size->cx = total_width(list);
		size->cy = list->count * list->item_height;
	}
	else if (list->view == LV_VIEW_LIST) {
		int rows = list->rows_per_column > 0 ? list->rows_per_column : 1;
		size->cx = (list->count + rows - 1) / rows * list->list_width;
		size->cy = rows * list->item_height;
	}
	else if (!list->owner_data) {
		int index;
		BOOL small = list->view == LV_VIEW_SMALLICON;
		int cx = small ? small_box_width(list) : list->spacing_cx, cy = small ? list->item_height : list->spacing_cy;
		for (index = 0; index < list->count; index++) {
			const item_record *item = list->items[index];
			if (item->at.x + cx > size->cx)
				size->cx = item->at.x + cx;
			if (item->at.y + cy > size->cy)
				size->cy = item->at.y + cy;
		}
	}
}

static void clamp_scroll(list_state *list, const RECT *client)
{
	int height = client->bottom - client->top, width = client->right - client->left;
	if (list->view == LV_VIEW_DETAILS) {
		int visible = (height - header_height(list)) / (list->item_height > 0 ? list->item_height : 1);
		int top_limit = list->count - (visible > 0 ? visible : 1);
		int x_limit = total_width(list) - width;
		list->scroll_row = list->scroll_row > top_limit ? top_limit : list->scroll_row;
		list->scroll_row = list->scroll_row < 0 ? 0 : list->scroll_row;
		list->scroll_x = list->scroll_x > x_limit ? x_limit : list->scroll_x;
		list->scroll_x = list->scroll_x < 0 ? 0 : list->scroll_x;
	}
	else if (list->view == LV_VIEW_LIST) {
		int rows = list->rows_per_column > 0 ? list->rows_per_column : 1;
		int columns = (list->count + rows - 1) / rows;
		int shown = width / (list->list_width > 0 ? list->list_width : 1);
		int limit = columns - (shown > 0 ? shown : 1);
		list->scroll_column = list->scroll_column > limit ? limit : list->scroll_column;
		list->scroll_column = list->scroll_column < 0 ? 0 : list->scroll_column;
	}
}

static void refresh_scrollbars(list_state *list, const RECT *client)
{
	SCROLLINFO info;
	SIZE extent;
	int width = client->right - client->left, height = client->bottom - client->top;

	if (style_of(list) & LVS_NOSCROLL)
		return;
	view_extent(list, &extent);
	info.cbSize = sizeof(info);
	info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	info.nMin = 0;
	if (list->view == LV_VIEW_DETAILS) {
		int rows = (height - header_height(list)) / (list->item_height > 0 ? list->item_height : 1);
		info.nMax = list->count - 1;
		info.nPage = rows > 0 ? rows : 1;
		info.nPos = list->scroll_row;
		SetScrollInfo(list->window, SB_VERT, &info, TRUE);
		info.nMax = extent.cx - 1;
		info.nPage = width > 0 ? width : 0;
		info.nPos = list->scroll_x;
		SetScrollInfo(list->window, SB_HORZ, &info, TRUE);
	}
	else if (list->view == LV_VIEW_LIST) {
		int rows = list->rows_per_column > 0 ? list->rows_per_column : 1;
		int shown = width / (list->list_width > 0 ? list->list_width : 1);
		info.nMax = (list->count + rows - 1) / rows - 1;
		info.nPage = shown > 0 ? shown : 1;
		info.nPos = list->scroll_column;
		SetScrollInfo(list->window, SB_HORZ, &info, TRUE);
		info.nMax = 0;
		info.nPage = 0;
		info.nPos = 0;
		SetScrollInfo(list->window, SB_VERT, &info, TRUE);
	}
	else {
		info.nMax = extent.cy - 1;
		info.nPage = height > 0 ? height : 0;
		info.nPos = list->origin.y;
		SetScrollInfo(list->window, SB_VERT, &info, TRUE);
		info.nMax = extent.cx - 1;
		info.nPage = width > 0 ? width : 0;
		info.nPos = list->origin.x;
		SetScrollInfo(list->window, SB_HORZ, &info, TRUE);
	}
}

static void place_header(list_state *list, const RECT *client)
{
	HDLAYOUT layout;
	WINDOWPOS position;
	RECT area = *client;

	if (!list->header)
		return;
	if (list->view != LV_VIEW_DETAILS) {
		ShowWindow(list->header, SW_HIDE);
		return;
	}
	area.left -= list->scroll_x;
	layout.prc = &area;
	layout.pwpos = &position;
	if (!SendMessageW(list->header, HDM_LAYOUT, 0, (LPARAM)&layout))
		return;
	SetWindowPos(list->header, position.hwndInsertAfter, position.x, position.y, position.cx, position.cy, position.flags);
	if (style_of(list) & WS_VISIBLE)
		ShowWindow(list->header, (style_of(list) & LVS_NOCOLUMNHEADER) ? SW_HIDE : SW_SHOWNA);
}

static void ensure_layout(list_state *list)
{
	RECT client, again;
	int round;

	if (!list->dirty || list->in_layout || !list->created)
		return;
	list->in_layout = TRUE;
	list->dirty = FALSE;
	refresh_metrics(list);
	refresh_list_width(list);
	for (round = 0; round < 3; round++) {
		GetClientRect(list->window, &client);
		list->rows_per_column = client.bottom / (list->item_height > 0 ? list->item_height : 1);
		list->rows_per_column = list->rows_per_column > 0 ? list->rows_per_column : 1;
		place_header(list, &client);
		list->columns_dirty = TRUE;
		place_items(list, &client);
		clamp_scroll(list, &client);
		refresh_scrollbars(list, &client);
		GetClientRect(list->window, &again);
		if (EqualRect(&client, &again))
			break;
	}
	list->in_layout = FALSE;
	list->dirty = FALSE;
}

static int report_row_top(list_state *list, int index)
{
	return header_height(list) + (index - list->scroll_row) * list->item_height;
}

static void set_rect_empty_at(RECT *area, int x, int y)
{
	SetRect(area, x, y, x, y);
}

static void compute_rects(list_state *list, int index, int indent, item_rects *rects)
{
	ensure_layout(list);
	refresh_columns(list);
	if (indent < 0) {
		indent = 0;
		if (!list->owner_data && list->items[index]->indent != I_INDENTCALLBACK)
			indent = list->items[index]->indent;
	}

	if (list->view == LV_VIEW_DETAILS) {
		int top = report_row_top(list, index), bottom = top + list->item_height;
		int left = list->column_count ? list->columns[0].left - list->scroll_x : 0;
		int right = list->column_count ? list->columns[0].right - list->scroll_x : 0;
		int x = left + REPORT_MARGIN;

		SetRect(&rects->bounds, -list->scroll_x, top, total_width(list) - list->scroll_x, bottom);
		SetRect(&rects->state_icon, x, top, x + list->state_cx, bottom);
		x += list->state_cx + indent * list->small_cx;
		SetRect(&rects->icon, x, top, x + list->small_cx, bottom);
		x += list->small_cx;
		if (x > right && list->column_count)
			x = right;
		SetRect(&rects->label, x, top, right > x ? right : x, bottom);
		SetRect(&rects->select, rects->icon.left, top, rects->label.right, bottom);
	}
	else if (list->view == LV_VIEW_LIST || list->view == LV_VIEW_SMALLICON) {
		int x, y, width, limit;
		item_view view;

		if (list->view == LV_VIEW_LIST) {
			int rows = list->rows_per_column > 0 ? list->rows_per_column : 1;
			x = (index / rows - list->scroll_column) * list->list_width;
			y = (index % rows) * list->item_height;
			limit = x + list->list_width;
		}
		else {
			const item_record *item = list->owner_data ? NULL : list->items[index];
			x = (item ? item->at.x : 0) - list->origin.x;
			y = (item ? item->at.y : 0) - list->origin.y;
			limit = x + small_box_width(list);
		}
		SetRect(&rects->state_icon, x, y, x + list->state_cx, y + list->item_height);
		x += list->state_cx;
		SetRect(&rects->icon, x, y, x + list->small_cx, y + list->item_height);
		x += list->small_cx;
		view_item(list, index, 0, LVIF_TEXT, 0, &view);
		width = measure(list, view.text) + 2 * LABEL_PADDING;
		SetRect(&rects->label, x, y, x + width > limit && limit > x ? limit : x + width, y + list->item_height);
		SetRect(&rects->bounds, rects->state_icon.left, y, list->view == LV_VIEW_LIST ? limit : rects->label.right, y + list->item_height);
		SetRect(&rects->select, rects->icon.left, y, rects->label.right, y + list->item_height);
	}
	else {
		const item_record *item = list->owner_data ? NULL : list->items[index];
		int x = (item ? item->at.x : 0) - list->origin.x;
		int y = (item ? item->at.y : 0) - list->origin.y;
		int box = list->spacing_cx > list->normal_cx ? list->spacing_cx : list->normal_cx;
		int icon_left = x + (box - list->normal_cx) / 2;
		int icon_bottom = y + list->normal_cy + ICON_MARGIN;
		int lines = (index == list->focus) ? 8 : ICON_LABEL_LINES;
		RECT text;
		item_view view;
		HDC dc;
		HGDIOBJ old;

		SetRect(&rects->icon, icon_left, y + ICON_MARGIN, icon_left + list->normal_cx, icon_bottom);
		SetRect(&rects->state_icon, icon_left - list->state_cx, icon_bottom - list->state_cy, icon_left, icon_bottom);
		view_item(list, index, 0, LVIF_TEXT, 0, &view);
		SetRect(&text, 0, 0, box - 2 * LABEL_PADDING, 0);
		dc = GetDC(list->window);
		old = SelectObject(dc, font_of(list));
		DrawTextW(dc, view.text, -1, &text, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_CENTER);
		SelectObject(dc, old);
		ReleaseDC(list->window, dc);
		if (text.bottom > lines * list->font_height)
			text.bottom = lines * list->font_height;
		if (text.bottom < list->font_height)
			text.bottom = list->font_height;
		SetRect(&rects->label, x, icon_bottom, x + box, icon_bottom + text.bottom + 2);
		if (*view.text) {
			int width = text.right + 2 * LABEL_PADDING;
			if (width < box) {
				rects->label.left = x + (box - width) / 2;
				rects->label.right = rects->label.left + width;
			}
		}
		else {
			set_rect_empty_at(&rects->label, x + box / 2, icon_bottom);
		}
		UnionRect(&rects->bounds, &rects->icon, &rects->label);
		rects->bounds.left = x;
		rects->bounds.right = x + box;
		UnionRect(&rects->select, &rects->icon, &rects->label);
	}
}

static void invalidate_item(list_state *list, int index)
{
	item_rects rects;
	RECT row;
	if (!list->redraw || !IsWindowVisible(list->window))
		return;
	if (list->view == LV_VIEW_LIST || list->view == LV_VIEW_DETAILS) {
		RECT client;
		GetClientRect(list->window, &client);
		ensure_layout(list);
		if (list->view == LV_VIEW_DETAILS) {
			row.top = report_row_top(list, index);
			row.bottom = row.top + list->item_height;
			row.left = 0;
			row.right = client.right;
			InvalidateRect(list->window, &row, FALSE);
			return;
		}
	}
	compute_rects(list, index, -1, &rects);
	UnionRect(&row, &rects.bounds, &rects.select);
	InflateRect(&row, 1, 1);
	InvalidateRect(list->window, &row, TRUE);
}

/* Selection and focus */

static BOOL change_state(list_state *list, int index, UINT mask, UINT value, BOOL always_ask)
{
	UINT old = stored_state(list, index), fresh = (old & ~mask) | (value & mask);
	item_record *item = list->owner_data ? NULL : list->items[index];
	LPARAM param = item ? item->param : 0;

	if (fresh == old && !always_ask)
		return TRUE;
	if (item) {
		if (notify_item(list, LVN_ITEMCHANGING, index, fresh, old, LVIF_STATE, param))
			return FALSE;
		if (index >= list->count || list->items[index] != item)
			return FALSE;
	}
	if (fresh == old)
		return TRUE;

	if ((fresh & LVIS_SELECTED) && !(old & LVIS_SELECTED) && (style_of(list) & LVS_SINGLESEL)) {
		if (list->owner_data) {
			while (list->range_count) {
				int other = list->ranges[0].first;
				range_remove(list, other, other);
				notify_item(list, LVN_ITEMCHANGED, other, other == list->focus ? LVIS_FOCUSED : 0, (other == list->focus ? LVIS_FOCUSED : 0) | LVIS_SELECTED, LVIF_STATE, 0);
				invalidate_item(list, other);
			}
		}
		else {
			int other;
			for (other = 0; other < list->count && list->selected_count; other++) {
				if (other != index && (list->items[other]->state & LVIS_SELECTED))
					change_state(list, other, LVIS_SELECTED, 0, FALSE);
			}
		}
	}
	if ((fresh & LVIS_FOCUSED) && !(old & LVIS_FOCUSED)) {
		if (list->focus != NO_ITEM && list->focus != index)
			change_state(list, list->focus, LVIS_FOCUSED, 0, FALSE);
		list->focus = index;
		if (list->mark == NO_ITEM)
			list->mark = index;
	}
	else if (!(fresh & LVIS_FOCUSED) && (old & LVIS_FOCUSED)) {
		invalidate_item(list, index);
		list->focus = NO_ITEM;
	}
	if (item && (index >= list->count || list->items[index] != item))
		return FALSE;

	if (list->owner_data) {
		if ((fresh & LVIS_SELECTED) && !(old & LVIS_SELECTED))
			range_add(list, index, index);
		else if (!(fresh & LVIS_SELECTED) && (old & LVIS_SELECTED))
			range_remove(list, index, index);
	}
	else {
		item->state = fresh;
		if ((fresh & LVIS_SELECTED) && !(old & LVIS_SELECTED))
			list->selected_count++;
		else if (!(fresh & LVIS_SELECTED) && (old & LVIS_SELECTED))
			list->selected_count--;
	}
	notify_item(list, LVN_ITEMCHANGED, index, fresh, old, LVIF_STATE, param);
	if (index < list->count)
		invalidate_item(list, index);
	return TRUE;
}

static int selected_total(const list_state *list)
{
	return list->owner_data ? range_total(list) : list->selected_count;
}

static void clear_selection(list_state *list)
{
	if (!selected_total(list))
		return;
	if (list->owner_data) {
		list->range_count = 0;
		notify_item(list, LVN_ITEMCHANGED, NO_ITEM, 0, LVIS_SELECTED, LVIF_STATE, 0);
		repaint(list);
	}
	else {
		int index;
		for (index = 0; index < list->count && list->selected_count; index++) {
			if (list->items[index]->state & LVIS_SELECTED)
				change_state(list, index, LVIS_SELECTED, 0, FALSE);
		}
	}
}

static BOOL set_state_all(list_state *list, UINT mask, UINT value)
{
	BOOL success = TRUE;
	int index;

	if ((mask & LVIS_FOCUSED) && (value & LVIS_FOCUSED))
		return FALSE;
	if ((mask & LVIS_SELECTED) && (value & LVIS_SELECTED) && (style_of(list) & LVS_SINGLESEL))
		return FALSE;
	if (mask == LVIS_SELECTED && !value && !selected_total(list))
		return TRUE;

	if (list->owner_data) {
		if (mask & LVIS_FOCUSED) {
			if (list->focus != NO_ITEM) {
				change_state(list, list->focus, LVIS_FOCUSED, 0, FALSE);
				notify_item(list, LVN_ITEMCHANGED, NO_ITEM, 0, LVIS_FOCUSED, LVIF_STATE, 0);
			}
		}
		if (mask & LVIS_SELECTED) {
			if (value & LVIS_SELECTED) {
				list->range_count = 0;
				if (list->count)
					range_add(list, 0, list->count - 1);
				notify_item(list, LVN_ITEMCHANGED, NO_ITEM, LVIS_SELECTED, 0, LVIF_STATE, 0);
			}
			else if (range_total(list)) {
				NMLVCACHEHINT hint;
				ZeroMemory(&hint, sizeof(hint));
				hint.iFrom = 0;
				hint.iTo = list->count - 1;
				notify(list, &hint.hdr, LVN_ODCACHEHINT);
				list->range_count = 0;
				notify_item(list, LVN_ITEMCHANGED, NO_ITEM, 0, LVIS_SELECTED, LVIF_STATE, 0);
			}
		}
		if (mask & ~(LVIS_FOCUSED | LVIS_SELECTED))
			notify_item(list, LVN_ITEMCHANGED, NO_ITEM, value & mask & ~(LVIS_FOCUSED | LVIS_SELECTED), 0, LVIF_STATE, 0);
		repaint(list);
		return TRUE;
	}
	for (index = 0; index < list->count; index++) {
		if (!change_state(list, index, mask, value, TRUE))
			success = FALSE;
	}
	return success;
}

/* Moves the caret the way the keyboard and the mouse both need: a span from the anchor, with the target last so its
 * selection and focus travel in one notification. */
static void select_span(list_state *list, int target, BOOL keep_others, BOOL extend)
{
	BOOL single = (style_of(list) & LVS_SINGLESEL) != 0;
	int anchor = extend && !single && list->mark != NO_ITEM ? list->mark : target;
	int low = anchor < target ? anchor : target, high = anchor < target ? target : anchor;

	if (list->owner_data) {
		if (!keep_others)
			clear_selection(list);
		if (high > low) {
			NMLVODSTATECHANGE change;
			range_add(list, low, high);
			ZeroMemory(&change, sizeof(change));
			change.iFrom = low;
			change.iTo = high;
			change.uNewState = LVIS_SELECTED;
			notify(list, &change.hdr, LVN_ODSTATECHANGED);
			repaint(list);
		}
	}
	else {
		int index;
		for (index = 0; index < list->count; index++) {
			BOOL inside = index >= low && index <= high;
			if (index == target)
				continue;
			if (inside)
				change_state(list, index, LVIS_SELECTED, LVIS_SELECTED, FALSE);
			else if (!keep_others && (list->items[index]->state & LVIS_SELECTED))
				change_state(list, index, LVIS_SELECTED, 0, FALSE);
		}
	}
	if (target < list->count)
		change_state(list, target, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED, FALSE);
	if (!extend)
		list->mark = target;
}

/* Scrolling */

static void note_scroll(list_state *list, UINT code, int dx, int dy)
{
	NMLVSCROLL info;
	ZeroMemory(&info, sizeof(info));
	info.dx = dx;
	info.dy = dy;
	notify(list, &info.hdr, code);
}

static void end_edit(list_state *list, BOOL commit);

static void scrolled(list_state *list)
{
	RECT client;
	if (list->edit)
		end_edit(list, FALSE);
	GetClientRect(list->window, &client);
	place_header(list, &client);
	refresh_scrollbars(list, &client);
	repaint(list);
}

static BOOL scroll_view(list_state *list, int dx, int dy)
{
	RECT client;
	ensure_layout(list);
	GetClientRect(list->window, &client);
	if (list->view == LV_VIEW_DETAILS) {
		if (dy && list->item_height > 0)
			list->scroll_row += dy > 0 ? (dy + list->item_height - 1) / list->item_height : -((-dy + list->item_height - 1) / list->item_height);
		list->scroll_x += dx;
		clamp_scroll(list, &client);
	}
	else if (list->view == LV_VIEW_LIST) {
		if (dx && list->list_width > 0)
			list->scroll_column += dx > 0 ? (dx + list->list_width - 1) / list->list_width : -((-dx + list->list_width - 1) / list->list_width);
		clamp_scroll(list, &client);
	}
	else {
		list->origin.x += dx;
		list->origin.y += dy;
	}
	scrolled(list);
	return TRUE;
}

static BOOL item_is_visible(list_state *list, int index)
{
	item_rects rects;
	RECT client, common;
	if (index < 0 || index >= list->count)
		return FALSE;
	compute_rects(list, index, -1, &rects);
	GetClientRect(list->window, &client);
	client.top += header_height(list);
	return IntersectRect(&common, &client, &rects.bounds);
}

static BOOL ensure_visible(list_state *list, int index, BOOL partial)
{
	item_rects rects;
	RECT client;
	if (index < 0 || index >= list->count)
		return FALSE;
	compute_rects(list, index, -1, &rects);
	GetClientRect(list->window, &client);
	client.top += header_height(list);

	if (list->view == LV_VIEW_DETAILS) {
		int visible = (client.bottom - client.top) / (list->item_height > 0 ? list->item_height : 1);
		if (partial && item_is_visible(list, index))
			return TRUE;
		if (index < list->scroll_row)
			list->scroll_row = index;
		else if (index >= list->scroll_row + (visible > 0 ? visible : 1))
			list->scroll_row = index - (visible > 0 ? visible : 1) + 1;
		else
			return TRUE;
	}
	else if (list->view == LV_VIEW_LIST) {
		int rows = list->rows_per_column > 0 ? list->rows_per_column : 1;
		int shown = client.right / (list->list_width > 0 ? list->list_width : 1);
		int column = index / rows;
		if (column < list->scroll_column)
			list->scroll_column = column;
		else if (column >= list->scroll_column + (shown > 0 ? shown : 1))
			list->scroll_column = column - (shown > 0 ? shown : 1) + 1;
		else
			return TRUE;
	}
	else {
		BOOL moved = FALSE;
		if (partial && item_is_visible(list, index))
			return TRUE;
		if (rects.bounds.left < client.left) {
			list->origin.x += rects.bounds.left - client.left;
			moved = TRUE;
		}
		else if (rects.bounds.right > client.right) {
			list->origin.x += rects.bounds.right - client.right;
			moved = TRUE;
		}
		if (rects.bounds.top < client.top) {
			list->origin.y += rects.bounds.top - client.top;
			moved = TRUE;
		}
		else if (rects.bounds.bottom > client.bottom) {
			list->origin.y += rects.bounds.bottom - client.bottom;
			moved = TRUE;
		}
		if (!moved)
			return TRUE;
	}
	scrolled(list);
	return TRUE;
}

static void scroll_message(list_state *list, int bar, WORD code)
{
	SCROLLINFO info;
	int position, line, page;
	BOOL vertical = bar == SB_VERT;

	ensure_layout(list);
	info.cbSize = sizeof(info);
	info.fMask = SIF_ALL;
	if (!GetScrollInfo(list->window, bar, &info))
		return;
	position = info.nPos;
	page = info.nPage > 0 ? (int)info.nPage : 1;
	line = list->view == LV_VIEW_DETAILS && vertical ? 1 : list->view == LV_VIEW_LIST ? 1 : 16;
	switch (code) {
	case SB_LINEUP: position -= line; break;
	case SB_LINEDOWN: position += line; break;
	case SB_PAGEUP: position -= page; break;
	case SB_PAGEDOWN: position += page; break;
	case SB_THUMBTRACK:
	case SB_THUMBPOSITION: position = info.nTrackPos; break;
	case SB_TOP: position = info.nMin; break;
	case SB_BOTTOM: position = info.nMax; break;
	default: return;
	}
	if (position > info.nMax - page + 1)
		position = info.nMax - page + 1;
	if (position < info.nMin)
		position = info.nMin;
	if (position == info.nPos)
		return;
	note_scroll(list, LVN_BEGINSCROLL, 0, 0);
	if (list->view == LV_VIEW_DETAILS) {
		if (vertical)
			list->scroll_row = position;
		else
			list->scroll_x = position;
	}
	else if (list->view == LV_VIEW_LIST) {
		list->scroll_column = position;
	}
	else if (vertical) {
		list->origin.y = position;
	}
	else {
		list->origin.x = position;
	}
	scrolled(list);
	note_scroll(list, LVN_ENDSCROLL, 0, 0);
}

/* Hit testing */

static int hit_test(list_state *list, POINT at, UINT *flags, int *subitem)
{
	RECT client;
	int index = NO_ITEM, first = 0, last = list->count - 1, walk;
	item_rects rects;

	ensure_layout(list);
	GetClientRect(list->window, &client);
	*flags = 0;
	if (subitem)
		*subitem = list->view == LV_VIEW_DETAILS ? NO_ITEM : 0;
	if (at.x < client.left)
		*flags |= LVHT_TOLEFT;
	else if (at.x >= client.right)
		*flags |= LVHT_TORIGHT;
	if (at.y < client.top)
		*flags |= LVHT_ABOVE;
	else if (at.y >= client.bottom)
		*flags |= LVHT_BELOW;

	if (list->view == LV_VIEW_DETAILS) {
		int column;
		refresh_columns(list);
		if (subitem) {
			for (column = 0; column < list->column_count; column++) {
				int left = list->columns[column].left - list->scroll_x, right = list->columns[column].right - list->scroll_x;
				if (at.x >= left && at.x < right) {
					*subitem = column;
					break;
				}
			}
		}
		if (*flags)
			return NO_ITEM;
		if (at.y >= header_height(list) && list->item_height > 0) {
			int row = (at.y - header_height(list)) / list->item_height + list->scroll_row;
			if (row < list->count)
				first = last = row;
			else
				first = 0, last = -1;
		}
		else {
			first = 0, last = -1;
		}
	}
	else if (*flags) {
		return NO_ITEM;
	}
	else if (list->view == LV_VIEW_LIST && list->item_height > 0 && list->list_width > 0) {
		int rows = list->rows_per_column > 0 ? list->rows_per_column : 1;
		int row = at.y / list->item_height, column = at.x / list->list_width + list->scroll_column;
		int candidate = column * rows + row;
		if (row < rows && candidate < list->count)
			first = last = candidate;
		else
			first = 0, last = -1;
	}

	for (walk = last; walk >= first; walk--) {
		POINT probe = at;
		compute_rects(list, walk, -1, &rects);
		if (list->view == LV_VIEW_DETAILS) {
			if (list->extended & LVS_EX_FULLROWSELECT) {
				if (PtInRect(&rects.bounds, probe)) {
					*flags = PtInRect(&rects.state_icon, probe) && list->state_list ? LVHT_ONITEMSTATEICON : LVHT_ONITEM;
					index = walk;
					break;
				}
				continue;
			}
			if (subitem && *subitem > 0 && !PtInRect(&rects.select, probe)) {
				*flags = LVHT_ONITEMLABEL;
				return walk;
			}
		}
		if (list->state_list && PtInRect(&rects.state_icon, probe)) {
			*flags = LVHT_ONITEMSTATEICON;
			index = walk;
			break;
		}
		if (PtInRect(&rects.icon, probe)) {
			*flags = LVHT_ONITEMICON;
			index = walk;
			break;
		}
		if (PtInRect(&rects.label, probe)) {
			*flags = LVHT_ONITEMLABEL;
			index = walk;
			break;
		}
		if (list->view == LV_VIEW_DETAILS && subitem && *subitem == 0 && PtInRect(&rects.bounds, probe)) {
			*flags = LVHT_ONITEMLABEL;
			return walk;
		}
	}
	if (index == NO_ITEM && !*flags)
		*flags = LVHT_NOWHERE;
	return index;
}

/* Edit control */

static LRESULT CALLBACK label_edit_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	list_state *list = state_of(GetParent(window));
	WNDPROC previous = list ? list->edit_proc : DefWindowProcW;

	switch (message) {
	case WM_GETDLGCODE:
		return DLGC_WANTALLKEYS;
	case WM_KEYDOWN:
		if (list && list->edit == window) {
			if (wparam == VK_RETURN) {
				end_edit(list, TRUE);
				return 0;
			}
			if (wparam == VK_ESCAPE) {
				end_edit(list, FALSE);
				return 0;
			}
		}
		break;
	case WM_CHAR:
		if (wparam == VK_RETURN || wparam == VK_ESCAPE)
			return 0;
		break;
	}
	return CallWindowProcW(previous, window, message, wparam, lparam);
}

static BOOL set_item(list_state *list, const LVITEMW *item);

static void end_edit(list_state *list, BOOL commit)
{
	HWND edit = list->edit;
	int index = list->edit_item;
	NMLVDISPINFOW info;
	WCHAR text[TEXT_LIMIT];
	char narrow[TEXT_LIMIT * 2];
	BOOL had_focus;
	item_record *item;

	if (!edit)
		return;
	list->edit = NULL;
	had_focus = GetFocus() == edit;
	text[0] = 0;
	GetWindowTextW(edit, text, TEXT_LIMIT);
	ShowWindow(edit, SW_HIDE);
	item = !list->owner_data && index < list->count ? list->items[index] : NULL;

	ZeroMemory(&info, sizeof(info));
	info.item.mask = LVIF_TEXT;
	info.item.iItem = index;
	info.item.lParam = item ? item->param : 0;
	if (commit) {
		if (list->unicode) {
			info.item.pszText = text;
		}
		else {
			if (!WideCharToMultiByte(CP_ACP, 0, text, -1, narrow, sizeof(narrow), NULL, NULL))
				narrow[sizeof(narrow) - 1] = 0;
			info.item.pszText = (WCHAR *)narrow;
		}
		info.item.cchTextMax = TEXT_LIMIT;
	}
	list->busy++;
	if (notify(list, &info.hdr, NOTIFY_CODE(list, LVN_ENDLABELEDIT)) && commit && item && index < list->count && list->items[index] == item && !IS_CALLBACK(item->text)) {
		LVITEMW change;
		ZeroMemory(&change, sizeof(change));
		change.mask = LVIF_TEXT;
		change.iItem = index;
		change.pszText = text;
		set_item(list, &change);
	}
	if (had_focus && !list->dead)
		SetFocus(list->window);
	DestroyWindow(edit);
	list->busy--;
}

static HWND begin_edit(list_state *list, int index)
{
	NMLVDISPINFOW info;
	item_view view;
	item_rects rects;
	HWND edit, focused = GetFocus();
	RECT area;
	item_record *item;

	if (list->edit) {
		BOOL was_focused = focused == list->edit;
		end_edit(list, FALSE);
		if (was_focused)
			focused = list->window;
	}
	if (index < 0 || index >= list->count)
		return NULL;
	if (focused != list->window)
		return NULL;
	item = list->owner_data ? NULL : list->items[index];

	view_item(list, index, 0, LVIF_TEXT, 0, &view);
	ensure_visible(list, index, FALSE);
	compute_rects(list, index, -1, &rects);
	area = rects.label;
	if (area.right - area.left < 40)
		area.right = area.left + 40;
	if (area.bottom - area.top < list->font_height + 4)
		area.bottom = area.top + list->font_height + 4;

	edit = CreateWindowExW(0, L"Edit", view.text, WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_LEFT, area.left, area.top, area.right - area.left, area.bottom - area.top, list->window, NULL, GetModuleHandleW(NULL), NULL);
	if (!edit)
		return NULL;
	SendMessageW(edit, WM_SETFONT, (WPARAM)font_of(list), FALSE);
	SendMessageW(edit, EM_SETLIMITTEXT, TEXT_LIMIT - 1, 0);
	SendMessageW(edit, EM_SETSEL, 0, -1);

	ZeroMemory(&info, sizeof(info));
	info.item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_STATE;
	info.item.iItem = index;
	info.item.lParam = item ? item->param : 0;
	info.item.state = stored_state(list, index);
	info.item.pszText = (WCHAR *)view.text;
	{
		char narrow[TEXT_LIMIT * 2];
		if (!list->unicode) {
			if (!WideCharToMultiByte(CP_ACP, 0, view.text, -1, narrow, sizeof(narrow), NULL, NULL))
				narrow[sizeof(narrow) - 1] = 0;
			info.item.pszText = (WCHAR *)narrow;
		}
		list->edit = edit;
		list->edit_item = index;
		if (notify(list, &info.hdr, NOTIFY_CODE(list, LVN_BEGINLABELEDIT))) {
			list->edit = NULL;
			DestroyWindow(edit);
			return NULL;
		}
	}
	if (list->edit != edit)
		return NULL;
	list->edit_proc = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)label_edit_proc);
	/* The header stays first in the child order while a label is being edited. */
	SetWindowPos(edit, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	ShowWindow(edit, SW_SHOW);
	SetFocus(edit);
	return edit;
}

/* Header */

static void ensure_header(list_state *list)
{
	DWORD style, own;
	if (list->header || !list->created)
		return;
	own = style_of(list);
	style = WS_CHILD | HDS_HORZ | HDS_DRAGDROP | HDS_FULLDRAG;
	if (!(own & LVS_NOSORTHEADER))
		style |= HDS_BUTTONS;
	if (own & LVS_NOCOLUMNHEADER)
		style |= HDS_HIDDEN;
	list->header = CreateWindowExW(0, WC_HEADERW, NULL, style, 0, 0, 0, 0, list->window, (HMENU)(INT_PTR)HEADER_ID, GetModuleHandleW(NULL), NULL);
	if (!list->header)
		return;
	SendMessageW(list->header, WM_SETFONT, (WPARAM)list->font, TRUE);
	if (list->small_list)
		SendMessageW(list->header, HDM_SETIMAGELIST, 0, (LPARAM)list->small_list);
	mark_dirty(list);
}

static BOOL column_to_header(const LVCOLUMNW *column, HDITEMW *item)
{
	ZeroMemory(item, sizeof(*item));
	if (column->mask & LVCF_FMT) {
		item->mask |= HDI_FORMAT;
		item->fmt = HDF_STRING;
		if (column->fmt & LVCFMT_RIGHT)
			item->fmt |= HDF_RIGHT;
		else if (column->fmt & LVCFMT_CENTER)
			item->fmt |= HDF_CENTER;
		if (column->fmt & LVCFMT_BITMAP_ON_RIGHT)
			item->fmt |= HDF_BITMAP_ON_RIGHT;
		if ((column->fmt & LVCFMT_COL_HAS_IMAGES) || (column->fmt & LVCFMT_IMAGE))
			item->fmt |= HDF_IMAGE;
	}
	if (column->mask & LVCF_WIDTH) {
		item->mask |= HDI_WIDTH;
		item->cxy = column->cx;
	}
	if (column->mask & LVCF_TEXT) {
		item->mask |= HDI_TEXT;
		item->pszText = column->pszText;
		item->cchTextMax = column->pszText ? lstrlenW(column->pszText) : 0;
	}
	if (column->mask & LVCF_IMAGE) {
		item->mask |= HDI_IMAGE | HDI_FORMAT;
		item->iImage = column->iImage;
		item->fmt |= HDF_IMAGE;
	}
	if (column->mask & LVCF_ORDER) {
		item->mask |= HDI_ORDER;
		item->iOrder = column->iOrder;
	}
	return TRUE;
}

static int insert_column(list_state *list, int index, const LVCOLUMNW *column)
{
	HDITEMW item;
	column_record *grown;
	int walk, cell;

	if (index < 0)
		return -1;
	if (index > list->column_count)
		index = list->column_count;
	ensure_header(list);
	if (!list->header)
		return -1;
	grown = allocate((list->column_count + 1) * sizeof(*grown));
	if (!grown)
		return -1;
	column_to_header(column, &item);
	if (!(item.mask & HDI_WIDTH)) {
		item.mask |= HDI_WIDTH;
		item.cxy = DEFAULT_COLUMN_WIDTH;
	}
	if (!(item.mask & HDI_FORMAT)) {
		item.mask |= HDI_FORMAT;
		item.fmt = HDF_LEFT | HDF_STRING;
	}
	list->header_quiet++;
	index = (int)SendMessageW(list->header, HDM_INSERTITEMW, index, (LPARAM)&item);
	list->header_quiet--;
	if (index < 0) {
		release(grown);
		return -1;
	}
	if (list->columns) {
		CopyMemory(grown, list->columns, index * sizeof(*grown));
		CopyMemory(grown + index + 1, list->columns + index, (list->column_count - index) * sizeof(*grown));
	}
	release(list->columns);
	list->columns = grown;
	grown[index].fmt = (column->mask & LVCF_FMT) ? column->fmt : LVCFMT_LEFT;
	grown[index].sub_item = (column->mask & LVCF_SUBITEM) ? column->iSubItem : index;
	grown[index].image = (column->mask & LVCF_IMAGE) ? column->iImage : 0;
	list->column_count++;
	if (!list->owner_data) {
		for (walk = 0; walk < list->count; walk++) {
			item_record *record = list->items[walk];
			for (cell = 0; cell < record->cell_count; cell++) {
				if (record->cells[cell].column >= index)
					record->cells[cell].column++;
			}
		}
	}
	list->columns_dirty = TRUE;
	mark_dirty(list);
	repaint(list);
	return index;
}

static BOOL delete_column(list_state *list, int index)
{
	int walk;
	if (index < 0 || index >= list->column_count || !list->header)
		return FALSE;
	list->header_quiet++;
	SendMessageW(list->header, HDM_DELETEITEM, index, 0);
	list->header_quiet--;
	MoveMemory(list->columns + index, list->columns + index + 1, (list->column_count - index - 1) * sizeof(*list->columns));
	list->column_count--;
	if (!list->owner_data) {
		for (walk = 0; walk < list->count; walk++) {
			item_record *record = list->items[walk];
			int cell = 0;
			if (index == 0) {
				cell_record *next = find_cell(record, 1);
				release_text(record->text);
				record->text = next ? next->text : NULL;
				if (next)
					next->text = NULL;
			}
			else if (find_cell(record, index)) {
				release_text(find_cell(record, index)->text);
				find_cell(record, index)->text = NULL;
			}
			while (cell < record->cell_count) {
				cell_record *entry = &record->cells[cell];
				if (entry->column == (index ? index : 1)) {
					MoveMemory(entry, entry + 1, (record->cell_count - cell - 1) * sizeof(*entry));
					record->cell_count--;
					continue;
				}
				if (entry->column > index)
					entry->column--;
				cell++;
			}
		}
	}
	list->columns_dirty = TRUE;
	mark_dirty(list);
	repaint(list);
	return TRUE;
}

static BOOL set_column(list_state *list, int index, const LVCOLUMNW *column)
{
	HDITEMW item;
	BOOL done = TRUE;
	if (index < 0 || index >= list->column_count || !list->header)
		return FALSE;
	column_to_header(column, &item);
	if (column->mask & LVCF_FMT)
		list->columns[index].fmt = column->fmt;
	if (column->mask & LVCF_SUBITEM)
		list->columns[index].sub_item = column->iSubItem;
	if (column->mask & LVCF_IMAGE)
		list->columns[index].image = column->iImage;
	if (item.mask) {
		list->header_quiet++;
		done = (BOOL)SendMessageW(list->header, HDM_SETITEMW, index, (LPARAM)&item);
		list->header_quiet--;
	}
	list->columns_dirty = TRUE;
	mark_dirty(list);
	repaint(list);
	return done;
}

static BOOL get_column(list_state *list, int index, LVCOLUMNW *column, BOOL wide)
{
	HDITEMW item;
	WCHAR text[TEXT_LIMIT];
	const column_record *record;
	if (index < 0 || index >= list->column_count || !list->header || !column)
		return FALSE;
	record = &list->columns[index];
	ZeroMemory(&item, sizeof(item));
	item.mask = HDI_WIDTH | HDI_ORDER;
	if (column->mask & LVCF_TEXT) {
		item.mask |= HDI_TEXT;
		text[0] = 0;
		item.pszText = text;
		item.cchTextMax = TEXT_LIMIT;
	}
	if (!SendMessageW(list->header, HDM_GETITEMW, index, (LPARAM)&item))
		return FALSE;
	if (column->mask & LVCF_FMT)
		column->fmt = record->fmt;
	if (column->mask & LVCF_WIDTH)
		column->cx = item.cxy;
	if (column->mask & LVCF_TEXT)
		copy_text_out(text, column->pszText, column->cchTextMax, wide);
	if (column->mask & LVCF_IMAGE)
		column->iImage = record->image;
	if (column->mask & LVCF_ORDER)
		column->iOrder = item.iOrder;
	if (column->mask & LVCF_SUBITEM)
		column->iSubItem = record->sub_item;
	return TRUE;
}

static int measure_cell(list_state *list, int index, int column)
{
	item_view view;
	int width;
	view_item(list, index, column, LVIF_TEXT, 0, &view);
	width = measure(list, view.text) + 2 * LABEL_PADDING;
	if (column == 0)
		width += REPORT_MARGIN + list->state_cx + list->small_cx;
	return width;
}

static BOOL set_column_width(list_state *list, int column, int width)
{
	if (list->view == LV_VIEW_LIST) {
		if (column != 0 && column != -1)
			return FALSE;
		if (width < 0) {
			list->custom_list_width = FALSE;
			refresh_list_width(list);
		}
		else {
			list->custom_list_width = TRUE;
			list->list_width = width;
		}
		mark_dirty(list);
		repaint(list);
		return TRUE;
	}
	if (list->view != LV_VIEW_DETAILS)
		return FALSE;
	if (column < 0 || column >= list->column_count || !list->header)
		return FALSE;
	if (width == LVSCW_AUTOSIZE || width == LVSCW_AUTOSIZE_USEHEADER) {
		int widest = 0, index, scan = list->count;
		HDITEMW item;
		WCHAR text[TEXT_LIMIT];
		if (list->owner_data && scan > 256)
			scan = 256;
		for (index = 0; index < scan; index++) {
			int cell = measure_cell(list, index, column);
			if (cell > widest)
				widest = cell;
		}
		if (width == LVSCW_AUTOSIZE_USEHEADER) {
			RECT client;
			int right;
			text[0] = 0;
			ZeroMemory(&item, sizeof(item));
			item.mask = HDI_TEXT;
			item.pszText = text;
			item.cchTextMax = TEXT_LIMIT;
			SendMessageW(list->header, HDM_GETITEMW, column, (LPARAM)&item);
			right = measure(list, text) + 12;
			if (right > widest)
				widest = right;
			refresh_columns(list);
			GetClientRect(list->window, &client);
			if (column == list->column_count - 1 && client.right > list->columns[column].left && client.right - list->columns[column].left > widest)
				widest = client.right - list->columns[column].left;
		}
		width = widest;
	}
	{
		HDITEMW item;
		ZeroMemory(&item, sizeof(item));
		item.mask = HDI_WIDTH;
		item.cxy = width;
		list->header_quiet++;
		SendMessageW(list->header, HDM_SETITEMW, column, (LPARAM)&item);
		list->header_quiet--;
	}
	list->columns_dirty = TRUE;
	mark_dirty(list);
	repaint(list);
	return TRUE;
}

/* Items */

static BOOL ensure_capacity(list_state *list)
{
	item_record **grown;
	int capacity;
	if (list->count < list->capacity)
		return TRUE;
	capacity = list->capacity ? list->capacity * 2 : 32;
	grown = allocate(capacity * sizeof(*grown));
	if (!grown)
		return FALSE;
	if (list->items)
		CopyMemory(grown, list->items, list->count * sizeof(*grown));
	release(list->items);
	list->items = grown;
	list->capacity = capacity;
	return TRUE;
}

static int sorted_position(list_state *list, const WCHAR *text)
{
	DWORD style = style_of(list);
	int index;
	if (!(style & (LVS_SORTASCENDING | LVS_SORTDESCENDING)) || !text || IS_CALLBACK(text))
		return -1;
	for (index = 0; index < list->count; index++) {
		const WCHAR *other = list->items[index]->text;
		int order;
		if (IS_CALLBACK(other))
			continue;
		order = lstrcmpW(text, other ? other : L"");
		if (style & LVS_SORTDESCENDING)
			order = -order;
		if (order < 0)
			return index;
	}
	return list->count;
}

static int insert_item(list_state *list, const LVITEMW *source)
{
	item_record *item;
	int index;
	UINT wanted = source && (source->mask & LVIF_STATE) ? source->state : 0;

	if (!source || source->iSubItem != 0)
		return -1;
	index = source->iItem;
	if (index < 0)
		return -1;
	if (index > list->count)
		index = list->count;

	if (list->owner_data) {
		range_shift_insert(list, index);
		list->count++;
		if (list->focus >= index)
			list->focus++;
		if (list->mark >= index)
			list->mark++;
		mark_dirty(list);
		repaint(list);
		if (wanted & (LVIS_FOCUSED | LVIS_SELECTED))
			change_state(list, index, LVIS_FOCUSED | LVIS_SELECTED, wanted, FALSE);
		return index;
	}

	if (!ensure_capacity(list))
		return -1;
	item = allocate(sizeof(*item));
	if (!item)
		return -1;
	if (source->mask & LVIF_TEXT) {
		int position = sorted_position(list, source->pszText);
		item->text = store_text(source->pszText);
		if (position >= 0)
			index = position;
	}
	if (source->mask & LVIF_IMAGE)
		item->image = source->iImage;
	if (source->mask & LVIF_INDENT)
		item->indent = source->iIndent;
	if (source->mask & LVIF_PARAM)
		item->param = source->lParam;
	item->group = I_GROUPIDNONE;
	if (source->mask & LVIF_GROUPID)
		item->group = source->iGroupId;
	item->id = list->next_id++;
	item->state = wanted & ~(LVIS_FOCUSED | LVIS_SELECTED);
	if (list->extended & LVS_EX_CHECKBOXES)
		item->state = (item->state & ~LVIS_STATEIMAGEMASK) | INDEXTOSTATEIMAGEMASK(1);

	MoveMemory(list->items + index + 1, list->items + index, (list->count - index) * sizeof(*list->items));
	list->items[index] = item;
	list->count++;
	if (list->focus >= index)
		list->focus++;
	if (list->mark >= index)
		list->mark++;
	if (list->edit && list->edit_item >= index)
		list->edit_item++;
	mark_dirty(list);

	if (wanted & (LVIS_FOCUSED | LVIS_SELECTED)) {
		UINT bits = wanted & (LVIS_FOCUSED | LVIS_SELECTED);
		change_state(list, index, bits, bits, FALSE);
		if (index >= list->count || list->items[index] != item)
			return index;
	}
	{
		NMLISTVIEW info;
		ZeroMemory(&info, sizeof(info));
		info.iItem = index;
		info.lParam = item->param;
		notify(list, &info.hdr, LVN_INSERTITEM);
	}
	repaint(list);
	return index;
}

static BOOL set_subitem(list_state *list, const LVITEMW *source, item_record *item)
{
	cell_record *cell;
	if (source->mask & LVIF_PARAM)
		return FALSE;
	if (source->iSubItem < 0)
		return FALSE;
	if (!(source->mask & (LVIF_TEXT | LVIF_IMAGE | LVIF_STATE)))
		return TRUE;
	cell = need_cell(item, source->iSubItem);
	if (!cell)
		return FALSE;
	if (source->mask & LVIF_TEXT) {
		release_text(cell->text);
		cell->text = store_text(source->pszText);
	}
	if (source->mask & LVIF_IMAGE)
		cell->image = source->iImage;
	if (source->mask & LVIF_STATE)
		cell->state = (cell->state & ~source->stateMask) | (source->state & source->stateMask);
	invalidate_item(list, source->iItem);
	return TRUE;
}

static BOOL set_item(list_state *list, const LVITEMW *source)
{
	item_record *item;
	int index = source->iItem;
	UINT changed = 0;
	const WCHAR *text = source->pszText;

	if (list->owner_data || index < 0 || index >= list->count)
		return FALSE;
	item = list->items[index];
	if (source->iSubItem)
		return set_subitem(list, source, item);

	if (source->mask & LVIF_TEXT) {
		if (IS_CALLBACK(text) || IS_CALLBACK(item->text))
			changed |= LVIF_TEXT;
		else if (lstrcmpW(text ? text : L"", item->text ? item->text : L""))
			changed |= LVIF_TEXT;
	}
	if ((source->mask & LVIF_IMAGE) && source->iImage != item->image)
		changed |= LVIF_IMAGE;
	if ((source->mask & LVIF_PARAM) && source->lParam != item->param)
		changed |= LVIF_PARAM;
	if ((source->mask & LVIF_INDENT) && source->iIndent != item->indent)
		changed |= LVIF_INDENT;
	if (source->mask & LVIF_GROUPID)
		item->group = source->iGroupId;

	if (changed) {
		LPARAM param = (changed & LVIF_PARAM) ? source->lParam : item->param;
		if (notify_item(list, LVN_ITEMCHANGING, index, 0, 0, changed, param))
			return FALSE;
		if (index >= list->count || list->items[index] != item)
			return FALSE;
		if (changed & LVIF_TEXT) {
			release_text(item->text);
			item->text = store_text(text);
			mark_dirty(list);
		}
		if (changed & LVIF_IMAGE)
			item->image = source->iImage;
		if (changed & LVIF_PARAM)
			item->param = source->lParam;
		if (changed & LVIF_INDENT)
			item->indent = source->iIndent;
		notify_item(list, LVN_ITEMCHANGED, index, 0, 0, changed, param);
		if (index >= list->count || list->items[index] != item)
			return TRUE;
		invalidate_item(list, index);
	}
	if (source->mask & LVIF_STATE)
		return change_state(list, index, source->stateMask, source->state, FALSE);
	return TRUE;
}

static BOOL get_item(list_state *list, LVITEMW *target, BOOL wide)
{
	item_view view;
	int index, sub;
	UINT ask;
	BOOL want_state;

	if (!target)
		return FALSE;
	index = target->iItem;
	sub = target->iSubItem;
	if (index < 0 || index >= list->count || sub < 0)
		return FALSE;
	if (target->mask & LVIF_NORECOMPUTE) {
		if (target->mask & LVIF_TEXT) {
			const WCHAR *text = list->owner_data ? LPSTR_TEXTCALLBACKW : sub == 0 ? list->items[index]->text : (find_cell(list->items[index], sub) ? find_cell(list->items[index], sub)->text : NULL);
			if (IS_CALLBACK(text)) {
				target->pszText = LPSTR_TEXTCALLBACKW;
				return TRUE;
			}
		}
	}
	ask = target->mask & (LVIF_TEXT | LVIF_IMAGE | LVIF_INDENT | LVIF_PARAM | LVIF_STATE);
	want_state = (target->mask & LVIF_STATE) != 0;
	view_item(list, index, sub, ask, want_state ? target->stateMask : 0, &view);
	if (target->mask & LVIF_TEXT) {
		if (target->pszText)
			copy_text_out(view.text, target->pszText, target->cchTextMax, wide);
	}
	if (target->mask & LVIF_IMAGE) {
		if (sub == 0 || (list->extended & LVS_EX_SUBITEMIMAGES))
			target->iImage = view.image;
	}
	if (target->mask & LVIF_PARAM)
		target->lParam = view.param;
	if (target->mask & LVIF_INDENT)
		target->iIndent = sub == 0 ? view.indent : 0;
	if (want_state)
		target->state = view.state & target->stateMask;
	if ((target->mask & LVIF_GROUPID) && !list->owner_data)
		target->iGroupId = list->items[index]->group;
	return TRUE;
}

static void free_item(item_record *item)
{
	int cell;
	release_text(item->text);
	for (cell = 0; cell < item->cell_count; cell++)
		release_text(item->cells[cell].text);
	release(item->cells);
	release(item);
}

static BOOL delete_item(list_state *list, int index)
{
	item_record *item;
	BOOL had_focus;

	if (index < 0 || index >= list->count)
		return FALSE;
	if (list->edit && list->edit_item == index)
		end_edit(list, FALSE);
	if (index >= list->count)
		return FALSE;
	had_focus = list->focus == index;

	if (list->owner_data) {
		NMLISTVIEW info;
		ZeroMemory(&info, sizeof(info));
		info.iItem = index;
		notify(list, &info.hdr, LVN_DELETEITEM);
		range_shift_delete(list, index);
		list->count--;
		if (had_focus)
			list->focus = NO_ITEM;
		else if (list->focus > index)
			list->focus--;
	}
	else {
		NMLISTVIEW info;
		item = list->items[index];
		/* The parent sees the doomed item without focus. */
		if (had_focus) {
			list->focus = NO_ITEM;
			item->state &= ~LVIS_FOCUSED;
		}
		ZeroMemory(&info, sizeof(info));
		info.iItem = index;
		info.lParam = item->param;
		notify(list, &info.hdr, LVN_DELETEITEM);
		if (index >= list->count || list->items[index] != item)
			return TRUE;
		if (item->state & LVIS_SELECTED)
			list->selected_count--;
		MoveMemory(list->items + index, list->items + index + 1, (list->count - index - 1) * sizeof(*list->items));
		list->count--;
		free_item(item);
		if (list->focus > index)
			list->focus--;
	}
	if (list->mark == index)
		list->mark = NO_ITEM;
	else if (list->mark > index)
		list->mark--;
	if (list->edit && list->edit_item > index)
		list->edit_item--;
	if (list->hot == index)
		list->hot = NO_ITEM;
	else if (list->hot > index)
		list->hot--;
	mark_dirty(list);
	if (had_focus && list->count)
		change_state(list, index < list->count ? index : list->count - 1, LVIS_FOCUSED, LVIS_FOCUSED, FALSE);
	repaint(list);
	return TRUE;
}

static BOOL delete_all(list_state *list, BOOL destroying)
{
	if (list->edit)
		end_edit(list, FALSE);
	if (list->owner_data) {
		if (!destroying) {
			NMLISTVIEW info;
			ZeroMemory(&info, sizeof(info));
			info.iItem = NO_ITEM;
			notify(list, &info.hdr, LVN_DELETEALLITEMS);
		}
		list->count = 0;
		list->range_count = 0;
	}
	else {
		NMLISTVIEW info;
		BOOL quiet;
		ZeroMemory(&info, sizeof(info));
		info.iItem = NO_ITEM;
		quiet = notify(list, &info.hdr, LVN_DELETEALLITEMS) != 0;
		while (list->count) {
			int index = list->count - 1;
			item_record *item = list->items[index];
			if (!quiet) {
				ZeroMemory(&info, sizeof(info));
				info.iItem = index;
				info.lParam = item->param;
				notify(list, &info.hdr, LVN_DELETEITEM);
				if (list->count <= index || list->items[index] != item)
					continue;
			}
			list->count = index;
			free_item(item);
		}
		list->selected_count = 0;
		list->next_slot = 0;
	}
	list->focus = list->mark = list->hot = NO_ITEM;
	list->scroll_row = list->scroll_x = list->scroll_column = 0;
	list->origin.x = list->origin.y = 0;
	mark_dirty(list);
	if (!destroying)
		repaint(list);
	return TRUE;
}

/* Sorting */

typedef struct {
	list_state *list;
	PFNLVCOMPARE compare;
	LPARAM data;
	BOOL by_index;
} sort_job;

static int sort_order(const sort_job *job, int first, int second)
{
	if (job->by_index)
		return job->compare(first, second, job->data);
	return job->compare(job->list->items[first]->param, job->list->items[second]->param, job->data);
}

static void merge_sort(const sort_job *job, int *values, int *scratch, int count)
{
	int middle = count / 2, left = 0, right = middle, out = 0;
	if (count < 2)
		return;
	merge_sort(job, values, scratch, middle);
	merge_sort(job, values + middle, scratch, count - middle);
	while (left < middle && right < count) {
		if (sort_order(job, values[right], values[left]) < 0)
			scratch[out++] = values[right++];
		else
			scratch[out++] = values[left++];
	}
	while (left < middle)
		scratch[out++] = values[left++];
	while (right < count)
		scratch[out++] = values[right++];
	CopyMemory(values, scratch, count * sizeof(int));
}

static BOOL sort_items(list_state *list, PFNLVCOMPARE compare, LPARAM data, BOOL by_index)
{
	sort_job job;
	int *values, *scratch, index;
	item_record **ordered, *focused, *marked;

	if (list->owner_data || !compare)
		return FALSE;
	if (list->count < 2)
		return TRUE;
	values = allocate(list->count * sizeof(int));
	scratch = allocate(list->count * sizeof(int));
	ordered = allocate(list->count * sizeof(*ordered));
	if (!values || !scratch || !ordered) {
		release(values);
		release(scratch);
		release(ordered);
		return FALSE;
	}
	for (index = 0; index < list->count; index++)
		values[index] = index;
	job.list = list;
	job.compare = compare;
	job.data = data;
	job.by_index = by_index;
	merge_sort(&job, values, scratch, list->count);

	focused = list->focus != NO_ITEM ? list->items[list->focus] : NULL;
	marked = list->mark != NO_ITEM ? list->items[list->mark] : NULL;
	for (index = 0; index < list->count; index++)
		ordered[index] = list->items[values[index]];
	CopyMemory(list->items, ordered, list->count * sizeof(*ordered));
	for (index = 0; index < list->count; index++) {
		if (list->items[index] == focused)
			list->focus = index;
		if (list->items[index] == marked)
			list->mark = index;
	}
	release(values);
	release(scratch);
	release(ordered);
	mark_dirty(list);
	repaint(list);
	return TRUE;
}

/* Lookup */

static int find_item(list_state *list, int start, const LVFINDINFOW *info, BOOL wide)
{
	int index, passes, step;
	if (!info)
		return -1;
	if (list->owner_data) {
		NMLVFINDITEMW request;
		ZeroMemory(&request, sizeof(request));
		request.iStart = start;
		request.lvfi = *info;
		if (list->unicode) {
			LVFINDINFOW copy = *info;
			WCHAR *text = NULL;
			if (!wide && (info->flags & (LVFI_STRING | LVFI_PARTIAL | LVFI_SUBSTRING)) && info->psz)
				copy.psz = text = widen((const char *)info->psz);
			request.lvfi = copy;
			index = (int)notify(list, &request.hdr, LVN_ODFINDITEMW);
			release(text);
			return index;
		}
		if (wide && (info->flags & (LVFI_STRING | LVFI_PARTIAL | LVFI_SUBSTRING)) && info->psz) {
			char narrow[TEXT_LIMIT * 2];
			if (!WideCharToMultiByte(CP_ACP, 0, info->psz, -1, narrow, sizeof(narrow), NULL, NULL))
				narrow[sizeof(narrow) - 1] = 0;
			request.lvfi.psz = (LPCWSTR)narrow;
			return (int)notify(list, &request.hdr, LVN_ODFINDITEMA);
		}
		return (int)notify(list, &request.hdr, LVN_ODFINDITEMA);
	}

	index = start + 1 < 0 ? 0 : start + 1;
	passes = (info->flags & LVFI_WRAP) ? 2 : 1;
	for (step = 0; step < passes; step++) {
		int limit = step ? (start + 1 < list->count ? start + 1 : list->count) : list->count;
		for (; index < limit; index++) {
			if (info->flags & LVFI_PARAM) {
				if (list->items[index]->param == info->lParam)
					return index;
				continue;
			}
			if ((info->flags & (LVFI_STRING | LVFI_PARTIAL | LVFI_SUBSTRING)) && info->psz) {
				item_view view;
				int length, compared;
				BOOL exact = (info->flags & LVFI_STRING) && !(info->flags & (LVFI_PARTIAL | LVFI_SUBSTRING));
				const WCHAR *needle = info->psz;
				view_item(list, index, 0, LVIF_TEXT, 0, &view);
				length = lstrlenW(needle);
				if (exact) {
					if (!lstrcmpiW(view.text, needle))
						return index;
					continue;
				}
				compared = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, view.text, lstrlenW(view.text) < length ? lstrlenW(view.text) : length, needle, length);
				if (compared == CSTR_EQUAL)
					return index;
			}
		}
		index = 0;
	}
	return -1;
}

static int neighbour(list_state *list, int from, UINT direction)
{
	item_rects base, other;
	int index, best = NO_ITEM, best_score = 0x7fffffff;
	POINT origin;
	if (list->view == LV_VIEW_DETAILS) {
		if (direction == LVNI_ABOVE)
			return from > 0 ? from - 1 : NO_ITEM;
		if (direction == LVNI_BELOW)
			return from + 1 < list->count ? from + 1 : NO_ITEM;
		return NO_ITEM;
	}
	if (list->view == LV_VIEW_LIST) {
		int rows = list->rows_per_column > 0 ? list->rows_per_column : 1, target = from;
		ensure_layout(list);
		rows = list->rows_per_column > 0 ? list->rows_per_column : 1;
		if (direction == LVNI_ABOVE)
			target = from - 1;
		else if (direction == LVNI_BELOW)
			target = from + 1;
		else if (direction == LVNI_TOLEFT)
			target = from - rows;
		else if (direction == LVNI_TORIGHT)
			target = from + rows;
		return target >= 0 && target < list->count ? target : NO_ITEM;
	}
	compute_rects(list, from, -1, &base);
	origin.x = base.bounds.left;
	origin.y = base.bounds.top;
	for (index = 0; index < list->count; index++) {
		int along, across, score;
		if (index == from)
			continue;
		compute_rects(list, index, -1, &other);
		if (direction == LVNI_ABOVE) {
			along = origin.y - other.bounds.top;
			across = other.bounds.left - origin.x;
		}
		else if (direction == LVNI_BELOW) {
			along = other.bounds.top - origin.y;
			across = other.bounds.left - origin.x;
		}
		else if (direction == LVNI_TOLEFT) {
			along = origin.x - other.bounds.left;
			across = other.bounds.top - origin.y;
		}
		else {
			along = other.bounds.left - origin.x;
			across = other.bounds.top - origin.y;
		}
		if (along <= 0)
			continue;
		across = across < 0 ? -across : across;
		score = along + 4 * across;
		if (score < best_score) {
			best_score = score;
			best = index;
		}
	}
	return best;
}

static int next_item(list_state *list, int start, UINT flags)
{
	UINT want = 0, direction = flags & LVNI_DIRECTIONMASK;
	int index;

	if (flags & LVNI_CUT)
		want |= LVIS_CUT;
	if (flags & LVNI_DROPHILITED)
		want |= LVIS_DROPHILITED;
	if (flags & LVNI_FOCUSED)
		want |= LVIS_FOCUSED;
	if (flags & LVNI_SELECTED)
		want |= LVIS_SELECTED;

	if (want == LVIS_FOCUSED && !direction && !(flags & LVNI_PREVIOUS))
		return list->focus > start ? list->focus : NO_ITEM;
	if (direction) {
		if (start < 0 || start >= list->count)
			return NO_ITEM;
		index = start;
		while ((index = neighbour(list, index, direction)) != NO_ITEM) {
			if ((stored_state(list, index) & want) == want)
				return index;
		}
		return NO_ITEM;
	}
	if (flags & LVNI_PREVIOUS) {
		for (index = start - 1; index >= 0; index--) {
			if ((stored_state(list, index) & want) == want)
				return index;
		}
		return NO_ITEM;
	}
	for (index = start + 1 < 0 ? 0 : start + 1; index < list->count; index++) {
		if ((stored_state(list, index) & want) == want)
			return index;
	}
	return NO_ITEM;
}

static DWORD approximate_rect(list_state *list, int count, int cx, int cy)
{
	ensure_layout(list);
	if (count == -1)
		count = list->count;
	if (list->view == LV_VIEW_DETAILS) {
		ensure_header(list);
		return MAKELONG(total_width(list), header_height(list) + count * list->item_height);
	}
	if (list->view == LV_VIEW_LIST) {
		int rows, height = cy == 0xffff ? 0 : cy;
		RECT client;
		GetClientRect(list->window, &client);
		rows = (height ? height : client.bottom) / (list->item_height > 0 ? list->item_height : 1);
		rows = rows > 0 ? rows : 1;
		return MAKELONG((count + rows - 1) / rows * list->list_width, (count < rows ? count : rows) * list->item_height);
	}
	else {
		BOOL small = list->view == LV_VIEW_SMALLICON;
		int box_x = small ? small_box_width(list) : list->spacing_cx, box_y = small ? list->item_height : list->spacing_cy;
		int fit, columns, rows;
		box_x = box_x > 0 ? box_x : 1;
		fit = cx == 0xffff ? count : cx / box_x;
		fit = fit > 0 ? fit : 1;
		columns = count > 1 ? (count < fit ? count : fit) : 1;
		rows = (count + columns - 1) / columns;
		return MAKELONG(columns * box_x + ICON_MARGIN, rows * box_y + ICON_MARGIN);
	}
}

/* Painting */

static void ensure_check_list(list_state *list)
{
	int size = GetSystemMetrics(SM_CXSMICON), index;
	HDC screen, dc;
	HBITMAP bitmap;
	HIMAGELIST images;

	if (list->own_state_list)
		return;
	screen = GetDC(list->window);
	dc = CreateCompatibleDC(screen);
	bitmap = CreateCompatibleBitmap(screen, size, size);
	images = ImageList_Create(size, size, ILC_COLOR32, 2, 1);
	ReleaseDC(list->window, screen);
	for (index = 0; index < 2 && images; index++) {
		RECT cell = { 0, 0, size, size };
		RECT box = { 1, 1, size - 1, size - 1 };
		HGDIOBJ old = SelectObject(dc, bitmap);
		HTHEME theme = OpenThemeData(list->window, L"BUTTON");
		FillRect(dc, &cell, GetSysColorBrush(COLOR_WINDOW));
		if (theme) {
			DrawThemeBackground(theme, dc, 3 /* BP_CHECKBOX */, index ? 5 /* CBS_CHECKEDNORMAL */ : 1 /* CBS_UNCHECKEDNORMAL */, &box, NULL);
			CloseThemeData(theme);
		}
		else {
			DrawFrameControl(dc, &box, DFC_BUTTON, DFCS_BUTTONCHECK | DFCS_FLAT | (index ? DFCS_CHECKED : 0));
		}
		SelectObject(dc, old);
		ImageList_Add(images, bitmap, NULL);
	}
	DeleteObject(bitmap);
	DeleteDC(dc);
	list->own_state_list = images;
	list->state_list = images;
	list->state_cx = list->state_cy = 0;
	if (images)
		ImageList_GetIconSize(images, &list->state_cx, &list->state_cy);
	mark_dirty(list);
}

static void fill_color(HDC dc, const RECT *area, COLORREF color)
{
	COLORREF previous = SetBkColor(dc, color);
	ExtTextOutW(dc, 0, 0, ETO_OPAQUE, area, NULL, 0, NULL);
	SetBkColor(dc, previous);
}

static BOOL is_real_color(COLORREF color)
{
	return color != CLR_NONE && color != CLR_DEFAULT;
}

static LRESULT custom_draw(list_state *list, NMLVCUSTOMDRAW *draw, DWORD stage)
{
	draw->nmcd.dwDrawStage = stage;
	return notify(list, &draw->nmcd.hdr, NM_CUSTOMDRAW);
}

static void draw_image(HIMAGELIST images, int image, HDC dc, int x, int y, UINT flags)
{
	if (images && image >= 0)
		ImageList_Draw(images, image, dc, x, y, flags);
}

static void draw_label(list_state *list, HDC dc, const WCHAR *text, const RECT *area, UINT format, COLORREF ink)
{
	RECT inner = *area;
	if (!*text)
		return;
	inner.left += LABEL_PADDING;
	inner.right -= LABEL_PADDING;
	if (is_real_color(ink))
		SetTextColor(dc, ink);
	else
		SetTextColor(dc, list->text_color);
	DrawTextW(dc, text, -1, &inner, format | DT_NOPREFIX);
}

static UINT image_flags(const list_state *list, UINT state, BOOL highlighted)
{
	UINT flags = ILD_TRANSPARENT | (state & LVIS_OVERLAYMASK);
	(void)list;
	if (state & LVIS_CUT)
		flags |= ILD_BLEND50;
	else if (highlighted)
		flags |= ILD_SELECTED;
	return flags;
}

static void draw_main_cell(list_state *list, HDC dc, int index, const item_rects *rects, const item_view *view, NMLVCUSTOMDRAW *draw, BOOL highlighted, BOOL full_row)
{
	HIMAGELIST icons = list->view == LV_VIEW_ICON ? list->normal_list : list->small_list;
	RECT fill = rects->label;
	UINT format;

	if (list->state_list) {
		int picture = (int)((view->state & LVIS_STATEIMAGEMASK) >> 12);
		if (picture)
			draw_image(list->state_list, list->own_state_list == list->state_list ? picture - 1 : picture - 1, dc, rects->state_icon.left, rects->state_icon.top + (rects->state_icon.bottom - rects->state_icon.top - list->state_cy) / 2, ILD_TRANSPARENT);
	}
	if (icons && view->image != I_IMAGENONE) {
		int height = list->view == LV_VIEW_ICON ? list->normal_cy : list->small_cy;
		draw_image(icons, view->image, dc, rects->icon.left, rects->icon.top + (rects->icon.bottom - rects->icon.top - height) / 2, image_flags(list, view->state, highlighted));
	}
	if (!full_row && is_real_color(draw->clrTextBk) && (list->view == LV_VIEW_DETAILS || *view->text || highlighted))
		fill_color(dc, &fill, draw->clrTextBk);
	if (list->view == LV_VIEW_ICON) {
		format = DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS;
	}
	else {
		format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
		(void)index;
	}
	draw_label(list, dc, view->text, &rects->label, format, draw->clrText);
}

static void draw_sub_cell(list_state *list, HDC dc, int index, int column, const RECT *cell, NMLVCUSTOMDRAW *draw, BOOL full_row)
{
	item_view view;
	RECT area = *cell;
	UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
	int fmt = list->columns[column].fmt & LVCFMT_JUSTIFYMASK;

	view_item(list, index, column, LVIF_TEXT | LVIF_IMAGE, 0, &view);
	if (fmt == LVCFMT_RIGHT)
		format |= DT_RIGHT;
	else if (fmt == LVCFMT_CENTER)
		format |= DT_CENTER;
	(void)full_row;
	if ((list->extended & LVS_EX_SUBITEMIMAGES) && list->small_list && view.image >= 0) {
		draw_image(list->small_list, view.image, dc, area.left + REPORT_MARGIN, area.top + (area.bottom - area.top - list->small_cy) / 2, ILD_TRANSPARENT);
		area.left += REPORT_MARGIN + list->small_cx;
	}
	draw_label(list, dc, view.text, &area, format, draw->clrText);
}

static void draw_item(list_state *list, HDC dc, int index, const RECT *clip, DWORD pre_answer)
{
	NMLVCUSTOMDRAW draw;
	item_view view;
	item_rects rects;
	RECT common;
	BOOL selected, highlighted, focused_window = list->has_focus, full_row, show;
	DWORD answer = 0;
	HGDIOBJ old_font = NULL;
	UINT state;
	int indent;

	view_item(list, index, 0, LVIF_TEXT | LVIF_IMAGE | LVIF_INDENT | LVIF_PARAM | LVIF_STATE, LVIS_SELECTED | LVIS_FOCUSED | LVIS_CUT | LVIS_DROPHILITED | LVIS_OVERLAYMASK | LVIS_STATEIMAGEMASK, &view);
	if (!list->owner_data && index >= list->count)
		return;
	indent = list->view == LV_VIEW_DETAILS ? view.indent : 0;
	compute_rects(list, index, indent, &rects);
	UnionRect(&common, &rects.bounds, &rects.select);
	if (clip && !IntersectRect(&common, &common, clip))
		return;

	state = view.state;
	selected = (state & LVIS_SELECTED) != 0;
	show = focused_window || (style_of(list) & LVS_SHOWSELALWAYS);
	highlighted = (selected && show) || (state & LVIS_DROPHILITED);
	full_row = list->view == LV_VIEW_DETAILS && (list->extended & LVS_EX_FULLROWSELECT);

	ZeroMemory(&draw, sizeof(draw));
	draw.nmcd.hdc = dc;
	draw.nmcd.dwItemSpec = index;
	draw.nmcd.lItemlParam = view.param;
	draw.nmcd.rc = rects.bounds;
	if (selected)
		draw.nmcd.uItemState |= CDIS_SELECTED;
	if (index == list->focus)
		draw.nmcd.uItemState |= CDIS_FOCUS;
	if (index == list->hot)
		draw.nmcd.uItemState |= CDIS_HOT;
	draw.iSubItem = 0;
	draw.clrText = list->text_color;
	draw.clrTextBk = list->text_back_color;
	if (highlighted && (focused_window || (state & LVIS_DROPHILITED))) {
		draw.clrText = GetSysColor(COLOR_HIGHLIGHTTEXT);
		draw.clrTextBk = GetSysColor(COLOR_HIGHLIGHT);
	}
	else if (highlighted) {
		draw.clrText = GetSysColor(COLOR_WINDOWTEXT);
		draw.clrTextBk = GetSysColor(COLOR_3DFACE);
	}

	if (pre_answer & CDRF_NOTIFYITEMDRAW)
		answer = (DWORD)custom_draw(list, &draw, CDDS_ITEMPREPAINT);
	if (list->dead || (!list->owner_data && index >= list->count))
		return;
	if (answer & CDRF_NEWFONT)
		old_font = GetCurrentObject(dc, OBJ_FONT);

	if (!(answer & CDRF_SKIPDEFAULT)) {
		if (list->view == LV_VIEW_DETAILS && (style_of(list) & LVS_OWNERDRAWFIXED)) {
			DRAWITEMSTRUCT owner;
			ZeroMemory(&owner, sizeof(owner));
			owner.CtlType = ODT_LISTVIEW;
			owner.CtlID = (UINT)GetWindowLongPtrW(list->window, GWLP_ID);
			owner.itemID = index;
			owner.itemAction = ODA_DRAWENTIRE;
			if (selected)
				owner.itemState |= ODS_SELECTED;
			if (index == list->focus && focused_window)
				owner.itemState |= ODS_FOCUS;
			owner.hwndItem = list->window;
			owner.hDC = dc;
			owner.rcItem = rects.bounds;
			owner.itemData = view.param;
			SendMessageW(list->parent, WM_DRAWITEM, owner.CtlID, (LPARAM)&owner);
		}
		else if (list->view == LV_VIEW_DETAILS) {
			int column;
			if (full_row && is_real_color(draw.clrTextBk))
				fill_color(dc, &rects.bounds, draw.clrTextBk);
			refresh_columns(list);
			for (column = 0; column < list->column_count; column++) {
				RECT cell;
				DWORD sub_answer = 0;
				SetRect(&cell, list->columns[column].left - list->scroll_x, rects.bounds.top, list->columns[column].right - list->scroll_x, rects.bounds.bottom);
				draw.iSubItem = column;
				if (selected && !focused_window && (style_of(list) & LVS_SHOWSELALWAYS) && column)
					draw.clrTextBk = GetSysColor(COLOR_3DFACE);
				if (answer & CDRF_NOTIFYSUBITEMDRAW) {
					sub_answer = (DWORD)custom_draw(list, &draw, CDDS_ITEMPREPAINT | CDDS_SUBITEM);
					if (list->dead || (!list->owner_data && index >= list->count))
						return;
				}
				if (!(sub_answer & CDRF_SKIPDEFAULT)) {
					if (column == 0) {
						draw_main_cell(list, dc, index, &rects, &view, &draw, highlighted, full_row);
					}
					else {
						if (!full_row && is_real_color(draw.clrTextBk) && !highlighted)
							fill_color(dc, &cell, draw.clrTextBk);
						{
							COLORREF ink = draw.clrText;
							if (highlighted && !full_row)
								draw.clrText = list->text_color;
							draw_sub_cell(list, dc, index, column, &cell, &draw, full_row);
							draw.clrText = ink;
						}
					}
				}
				if (answer & CDRF_NOTIFYSUBITEMDRAW)
					custom_draw(list, &draw, CDDS_ITEMPOSTPAINT | CDDS_SUBITEM);
				if (list->dead || (!list->owner_data && index >= list->count))
					return;
			}
			draw.iSubItem = 0;
		}
		else {
			draw_main_cell(list, dc, index, &rects, &view, &draw, highlighted, FALSE);
		}
		if (index == list->focus && focused_window && !(style_of(list) & LVS_OWNERDRAWFIXED)) {
			RECT mark = full_row ? rects.bounds : rects.label;
			SetTextColor(dc, RGB(0, 0, 0));
			SetBkColor(dc, RGB(255, 255, 255));
			DrawFocusRect(dc, &mark);
		}
	}
	if (old_font)
		SelectObject(dc, old_font);
	if (answer & CDRF_NOTIFYPOSTPAINT)
		custom_draw(list, &draw, CDDS_ITEMPOSTPAINT);
}

static void draw_grid(list_state *list, HDC dc, const RECT *client)
{
	HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DFACE));
	HGDIOBJ old = SelectObject(dc, pen);
	int top = header_height(list), y, column;
	if (list->item_height > 0) {
		for (y = top + list->item_height - 1; y < client->bottom; y += list->item_height) {
			MoveToEx(dc, 0, y, NULL);
			LineTo(dc, client->right, y);
		}
	}
	refresh_columns(list);
	for (column = 0; column < list->column_count; column++) {
		int x = list->columns[column].right - list->scroll_x - 1;
		MoveToEx(dc, x, top, NULL);
		LineTo(dc, x, client->bottom);
	}
	SelectObject(dc, old);
	DeleteObject(pen);
}

static void visible_span(list_state *list, const RECT *client, int *first, int *last)
{
	*first = 0;
	*last = list->count - 1;
	if (list->view == LV_VIEW_DETAILS && list->item_height > 0) {
		int rows = (client->bottom - header_height(list)) / list->item_height + 1;
		*first = list->scroll_row;
		*last = *first + rows;
	}
	else if (list->view == LV_VIEW_LIST && list->list_width > 0) {
		int rows = list->rows_per_column > 0 ? list->rows_per_column : 1;
		*first = list->scroll_column * rows;
		*last = (list->scroll_column + client->right / list->list_width + 1) * rows;
	}
	if (*last > list->count - 1)
		*last = list->count - 1;
}

static void draw_items(list_state *list, HDC dc, const RECT *clip)
{
	NMLVCUSTOMDRAW draw;
	RECT client;
	DWORD answer;
	int first, last, index;
	HGDIOBJ old_font;
	int old_mode;
	COLORREF old_text, old_back;

	ensure_layout(list);
	if ((list->extended & LVS_EX_CHECKBOXES) && !list->own_state_list)
		ensure_check_list(list);
	ensure_layout(list);
	GetClientRect(list->window, &client);
	old_font = SelectObject(dc, font_of(list));
	old_mode = SetBkMode(dc, TRANSPARENT);
	old_text = GetTextColor(dc);
	old_back = GetBkColor(dc);

	ZeroMemory(&draw, sizeof(draw));
	draw.nmcd.hdc = dc;
	draw.nmcd.rc = client;
	draw.clrText = list->text_color;
	draw.clrTextBk = list->text_back_color;
	answer = (DWORD)custom_draw(list, &draw, CDDS_PREPAINT);
	if (!list->dead && !(answer & CDRF_SKIPDEFAULT)) {
		visible_span(list, &client, &first, &last);
		if (list->owner_data && first <= last) {
			NMLVCACHEHINT hint;
			ZeroMemory(&hint, sizeof(hint));
			hint.iFrom = first;
			hint.iTo = last;
			notify(list, &hint.hdr, LVN_ODCACHEHINT);
		}
		if (list->view == LV_VIEW_DETAILS && (list->extended & LVS_EX_GRIDLINES))
			draw_grid(list, dc, &client);
		for (index = first; index <= last && index < list->count && !list->dead; index++)
			draw_item(list, dc, index, clip, answer);
		if (!list->dead && list->band_active)
			DrawFocusRect(dc, &list->band);
		if (!list->dead && (answer & CDRF_NOTIFYPOSTPAINT))
			custom_draw(list, &draw, CDDS_POSTPAINT);
	}
	SetBkColor(dc, old_back);
	SetTextColor(dc, old_text);
	SetBkMode(dc, old_mode);
	SelectObject(dc, old_font);
}

static void draw_back_image(list_state *list, HDC dc, const RECT *client)
{
	BITMAP info;
	HDC memory;
	HGDIOBJ old;
	if (!list->back_image || !GetObjectW(list->back_image, sizeof(info), &info))
		return;
	memory = CreateCompatibleDC(dc);
	old = SelectObject(memory, list->back_image);
	if (list->back_flags & LVBKIF_STYLE_TILE) {
		int x, y;
		for (y = 0; y < client->bottom; y += info.bmHeight > 0 ? info.bmHeight : client->bottom + 1) {
			for (x = 0; x < client->right; x += info.bmWidth > 0 ? info.bmWidth : client->right + 1)
				BitBlt(dc, x, y, info.bmWidth, info.bmHeight, memory, 0, 0, SRCCOPY);
		}
	}
	else {
		int x = (client->right - info.bmWidth) * list->back_x / 100;
		int y = (client->bottom - info.bmHeight) * list->back_y / 100;
		BitBlt(dc, x, y, info.bmWidth, info.bmHeight, memory, 0, 0, SRCCOPY);
	}
	SelectObject(memory, old);
	DeleteDC(memory);
}

static void exclude_header(const list_state *list, HDC dc)
{
	RECT area;
	POINT corner;
	if (!list->header || !IsWindowVisible(list->header))
		return;
	GetWindowRect(list->header, &area);
	corner.x = area.left;
	corner.y = area.top;
	ScreenToClient(list->window, &corner);
	ExcludeClipRect(dc, corner.x, corner.y, corner.x + area.right - area.left, corner.y + area.bottom - area.top);
}

static void paint(list_state *list)
{
	PAINTSTRUCT info;
	RECT client;
	HDC dc;

	/* Painting the header first keeps its pixels out of the erase below, even for a subclass that never validates. */
	if (list->header && list->redraw && IsWindowVisible(list->header))
		UpdateWindow(list->header);
	dc = BeginPaint(list->window, &info);
	if (dc && list->redraw) {
		GetClientRect(list->window, &client);
		ensure_layout(list);
		exclude_header(list, dc);
		if (list->back_color == CLR_NONE && !list->back_image) {
			draw_items(list, dc, &info.rcPaint);
		}
		else {
			HDC memory = CreateCompatibleDC(dc);
			HBITMAP bitmap = memory ? CreateCompatibleBitmap(dc, client.right, client.bottom) : NULL;
			if (bitmap) {
				HGDIOBJ old = SelectObject(memory, bitmap);
				fill_color(memory, &client, list->back_color == CLR_NONE ? GetSysColor(COLOR_WINDOW) : list->back_color);
				draw_back_image(list, memory, &client);
				draw_items(list, memory, &info.rcPaint);
				if (!list->dead)
					BitBlt(dc, info.rcPaint.left, info.rcPaint.top, info.rcPaint.right - info.rcPaint.left, info.rcPaint.bottom - info.rcPaint.top, memory, info.rcPaint.left, info.rcPaint.top, SRCCOPY);
				SelectObject(memory, old);
				DeleteObject(bitmap);
			}
			else {
				draw_items(list, dc, &info.rcPaint);
			}
			if (memory)
				DeleteDC(memory);
		}
	}
	if (!list->dead)
		EndPaint(list->window, &info);
}

static HIMAGELIST create_drag_image(list_state *list, int index, POINT *at)
{
	item_rects rects;
	item_view view;
	HDC screen, memory;
	HBITMAP bitmap;
	HGDIOBJ old_bitmap, old_font;
	HIMAGELIST images;
	RECT area;
	int width, height;

	if (index < 0 || index >= list->count || !at)
		return NULL;
	compute_rects(list, index, -1, &rects);
	view_item(list, index, 0, LVIF_TEXT | LVIF_IMAGE, 0, &view);
	UnionRect(&area, &rects.icon, &rects.label);
	width = area.right - area.left;
	height = area.bottom - area.top;
	if (width <= 0)
		width = 1;
	if (height <= 0)
		height = 1;
	screen = GetDC(list->window);
	memory = CreateCompatibleDC(screen);
	bitmap = CreateCompatibleBitmap(screen, width, height);
	ReleaseDC(list->window, screen);
	old_bitmap = SelectObject(memory, bitmap);
	old_font = SelectObject(memory, font_of(list));
	SetRect(&area, 0, 0, width, height);
	FillRect(memory, &area, GetSysColorBrush(COLOR_WINDOW));
	SetBkMode(memory, TRANSPARENT);
	draw_image(list->view == LV_VIEW_ICON ? list->normal_list : list->small_list, view.image, memory, rects.icon.left - rects.icon.left, 0, ILD_TRANSPARENT);
	OffsetRect(&rects.label, -rects.icon.left, -rects.icon.top);
	draw_label(list, memory, view.text, &rects.label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, list->text_color);
	SelectObject(memory, old_font);
	SelectObject(memory, old_bitmap);
	DeleteDC(memory);
	images = ImageList_Create(width, height, ILC_COLOR32, 1, 0);
	if (images)
		ImageList_Add(images, bitmap, NULL);
	DeleteObject(bitmap);
	at->x = rects.icon.left;
	at->y = rects.icon.top;
	return images;
}

/* Input */

static void send_activate(list_state *list, UINT code, int index, int sub, POINT at, UINT keys)
{
	NMITEMACTIVATE info;
	ZeroMemory(&info, sizeof(info));
	info.iItem = index;
	info.iSubItem = sub;
	info.ptAction = at;
	info.uKeyFlags = keys;
	if (index >= 0 && !list->owner_data && index < list->count)
		info.lParam = list->items[index]->param;
	notify(list, &info.hdr, code);
}

static void toggle_check(list_state *list, int index)
{
	UINT state = stored_state(list, index);
	UINT picture = (state & LVIS_STATEIMAGEMASK) >> 12;
	change_state(list, index, LVIS_STATEIMAGEMASK, INDEXTOSTATEIMAGEMASK(picture == 2 ? 1 : 2), FALSE);
}

static void band_select(list_state *list)
{
	int index;
	for (index = 0; index < list->count; index++) {
		item_rects rects;
		RECT common;
		BOOL inside;
		compute_rects(list, index, -1, &rects);
		inside = IntersectRect(&common, &rects.select, &list->band);
		if (inside != ((stored_state(list, index) & LVIS_SELECTED) != 0))
			change_state(list, index, LVIS_SELECTED, inside ? LVIS_SELECTED : 0, FALSE);
	}
}

static void button_down(list_state *list, int button, POINT at, UINT keys)
{
	UINT flags;
	int sub, index;
	BOOL ctrl = (keys & MK_CONTROL) != 0, shift = (keys & MK_SHIFT) != 0;
	BOOL single = (style_of(list) & LVS_SINGLESEL) != 0;

	if (list->edit)
		end_edit(list, TRUE);
	KillTimer(list->window, EDIT_TIMER);
	if (GetFocus() != list->window)
		SetFocus(list->window);
	if (list->dead)
		return;
	index = hit_test(list, at, &flags, &sub);
	list->press_item = index;
	list->press_button = button;
	list->press_at = at;
	list->press_keys = keys;
	list->press_dragging = FALSE;
	list->press_empty = index == NO_ITEM;
	list->press_was_selected = FALSE;
	list->press_on_label = (flags & LVHT_ONITEMLABEL) != 0;
	list->press_keep = FALSE;

	if (index == NO_ITEM) {
		if (!ctrl && !shift && (button == VK_LBUTTON || selected_total(list) < 2 || button == VK_RBUTTON))
			clear_selection(list);
	}
	else if (button == VK_LBUTTON && (flags & LVHT_ONITEMSTATEICON) && (list->extended & LVS_EX_CHECKBOXES)) {
		toggle_check(list, index);
		list->press_item = NO_ITEM;
		list->press_empty = FALSE;
		list->press_keep = TRUE;
	}
	else {
		UINT state = stored_state(list, index);
		list->press_was_selected = (state & LVIS_SELECTED) != 0;
		if (shift && !single) {
			select_span(list, index, ctrl, TRUE);
		}
		else if (ctrl && !single && button == VK_LBUTTON) {
			change_state(list, index, LVIS_SELECTED | LVIS_FOCUSED, (state & LVIS_SELECTED ? 0 : LVIS_SELECTED) | LVIS_FOCUSED, FALSE);
			list->mark = index;
		}
		else if (state & LVIS_SELECTED) {
			/* Keep the group until the button comes up, a drag carries all of it. */
			change_state(list, index, LVIS_FOCUSED, LVIS_FOCUSED, FALSE);
			list->mark = index;
			list->press_keep = selected_total(list) > 1;
		}
		else {
			select_span(list, index, FALSE, FALSE);
		}
		if (index < list->count)
			ensure_visible(list, index, TRUE);
	}
	if (!list->dead)
		SetCapture(list->window);
}

static void button_up(list_state *list, int button, POINT at, UINT keys)
{
	int index = list->press_item, sub;
	UINT flags;
	BOOL was_band = list->band_active, dragged = list->press_dragging;

	if (list->press_button != button)
		return;
	list->press_button = 0;
	if (GetCapture() == list->window)
		ReleaseCapture();
	if (was_band) {
		list->band_active = FALSE;
		repaint(list);
	}
	if (dragged)
		return;
	if (list->press_keep && index != NO_ITEM && index < list->count && !(keys & (MK_CONTROL | MK_SHIFT)))
		select_span(list, index, FALSE, FALSE);
	if (list->dead)
		return;
	hit_test(list, at, &flags, &sub);
	if (sub < 0)
		sub = 0;
	send_activate(list, button == VK_LBUTTON ? NM_CLICK : NM_RCLICK, index, index == NO_ITEM ? 0 : sub, at, keys);
	if (list->dead)
		return;
	if (button == VK_RBUTTON) {
		POINT screen = at;
		ClientToScreen(list->window, &screen);
		SendMessageW(list->window, WM_CONTEXTMENU, (WPARAM)list->window, MAKELPARAM(screen.x, screen.y));
		return;
	}
	if (index == NO_ITEM)
		return;
	if ((list->extended & LVS_EX_ONECLICKACTIVATE) || ((list->extended & LVS_EX_TWOCLICKACTIVATE) && list->press_was_selected))
		send_activate(list, LVN_ITEMACTIVATE, index, sub, at, keys);
	else if ((style_of(list) & LVS_EDITLABELS) && list->press_was_selected && list->press_on_label && !(keys & (MK_CONTROL | MK_SHIFT)))
		SetTimer(list->window, EDIT_TIMER, GetDoubleClickTime(), NULL);
}

static void mouse_move(list_state *list, POINT at, UINT keys)
{
	BOOL tracked_ext = (list->extended & (LVS_EX_TRACKSELECT | LVS_EX_ONECLICKACTIVATE | LVS_EX_TWOCLICKACTIVATE | LVS_EX_UNDERLINEHOT | LVS_EX_UNDERLINECOLD)) != 0;

	if (list->press_button && (keys & (MK_LBUTTON | MK_RBUTTON))) {
		int dx = at.x - list->press_at.x, dy = at.y - list->press_at.y;
		dx = dx < 0 ? -dx : dx;
		dy = dy < 0 ? -dy : dy;
		if (list->band_active) {
			RECT old = list->band;
			SetRect(&list->band, min(at.x, list->press_at.x), min(at.y, list->press_at.y), max(at.x, list->press_at.x), max(at.y, list->press_at.y));
			band_select(list);
			InvalidateRect(list->window, &old, FALSE);
			InvalidateRect(list->window, &list->band, FALSE);
			return;
		}
		if (!list->press_dragging && (dx >= GetSystemMetrics(SM_CXDRAG) || dy >= GetSystemMetrics(SM_CYDRAG))) {
			if (list->press_item != NO_ITEM) {
				NMLISTVIEW info;
				list->press_dragging = TRUE;
				ReleaseCapture();
				ZeroMemory(&info, sizeof(info));
				info.iItem = list->press_item;
				info.ptAction = list->press_at;
				if (!list->owner_data && list->press_item < list->count)
					info.lParam = list->items[list->press_item]->param;
				if (!(style_of(list) & LVS_NOSCROLL) || TRUE)
					notify(list, &info.hdr, list->press_button == VK_LBUTTON ? LVN_BEGINDRAG : LVN_BEGINRDRAG);
			}
			else if (list->press_empty && list->press_button == VK_LBUTTON && !(style_of(list) & LVS_SINGLESEL)) {
				list->band_active = TRUE;
				list->press_dragging = TRUE;
				SetRectEmpty(&list->band);
			}
		}
		if (list->band_active || list->press_dragging)
			return;
	}
	if (tracked_ext) {
		TRACKMOUSEEVENT track;
		UINT flags;
		int sub, index = hit_test(list, at, &flags, &sub);
		track.cbSize = sizeof(track);
		track.dwFlags = TME_LEAVE | ((list->extended & (LVS_EX_TRACKSELECT | LVS_EX_ONECLICKACTIVATE)) ? TME_HOVER : 0);
		track.hwndTrack = list->window;
		track.dwHoverTime = list->hover_time;
		TrackMouseEvent(&track);
		if (index != list->hot) {
			NMLISTVIEW info;
			int old = list->hot;
			ZeroMemory(&info, sizeof(info));
			info.iItem = index;
			info.ptAction = at;
			if (!notify(list, &info.hdr, LVN_HOTTRACK)) {
				list->hot = index;
				if (old != NO_ITEM && old < list->count)
					invalidate_item(list, old);
				if (index != NO_ITEM)
					invalidate_item(list, index);
			}
		}
	}
	if (list->tips && (list->extended & (LVS_EX_INFOTIP | LVS_EX_LABELTIP))) {
		UINT flags;
		int sub, index = hit_test(list, at, &flags, &sub);
		MSG relay;
		ZeroMemory(&relay, sizeof(relay));
		relay.hwnd = list->window;
		relay.message = WM_MOUSEMOVE;
		relay.wParam = keys;
		relay.lParam = MAKELPARAM(at.x, at.y);
		if (index != list->tip_item) {
			list->tip_item = index;
			SendMessageW(list->tips, TTM_POP, 0, 0);
		}
		SendMessageW(list->tips, TTM_RELAYEVENT, 0, (LPARAM)&relay);
	}
}

static void key_down(list_state *list, WPARAM key, LPARAM data)
{
	NMLVKEYDOWN info;
	BOOL shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
	int current = list->focus, target = NO_ITEM, page;
	RECT client;

	ZeroMemory(&info, sizeof(info));
	info.wVKey = (WORD)key;
	info.flags = (UINT)(data >> 16);
	notify(list, &info.hdr, LVN_KEYDOWN);
	if (list->dead)
		return;
	ensure_layout(list);
	GetClientRect(list->window, &client);
	page = list->view == LV_VIEW_DETAILS && list->item_height > 0 ? (client.bottom - header_height(list)) / list->item_height : list->rows_per_column;
	page = page > 1 ? page - 1 : 1;

	switch (key) {
	case VK_RETURN: {
		POINT at = { 0, 0 };
		notify_plain(list, NM_RETURN);
		if (!list->dead && current != NO_ITEM)
			send_activate(list, LVN_ITEMACTIVATE, current, 0, at, 0);
		return;
	}
	case VK_SPACE:
		if (current == NO_ITEM)
			return;
		if (ctrl) {
			UINT state = stored_state(list, current);
			change_state(list, current, LVIS_SELECTED, state & LVIS_SELECTED ? 0 : LVIS_SELECTED, FALSE);
			list->mark = current;
		}
		else if (list->extended & LVS_EX_CHECKBOXES) {
			toggle_check(list, current);
		}
		return;
	case VK_UP:
		target = current == NO_ITEM ? 0 : neighbour(list, current, LVNI_ABOVE);
		break;
	case VK_DOWN:
		target = current == NO_ITEM ? 0 : neighbour(list, current, LVNI_BELOW);
		break;
	case VK_LEFT:
		if (list->view == LV_VIEW_DETAILS) {
			scroll_view(list, -16, 0);
			return;
		}
		target = current == NO_ITEM ? 0 : neighbour(list, current, LVNI_TOLEFT);
		break;
	case VK_RIGHT:
		if (list->view == LV_VIEW_DETAILS) {
			scroll_view(list, 16, 0);
			return;
		}
		target = current == NO_ITEM ? 0 : neighbour(list, current, LVNI_TORIGHT);
		break;
	case VK_HOME:
		target = 0;
		break;
	case VK_END:
		target = list->count - 1;
		break;
	case VK_PRIOR:
		target = current == NO_ITEM ? 0 : (current - page < 0 ? 0 : current - page);
		break;
	case VK_NEXT:
		target = current == NO_ITEM ? 0 : (current + page >= list->count ? list->count - 1 : current + page);
		break;
	default:
		return;
	}
	if (target == NO_ITEM || target < 0 || target >= list->count)
		return;
	if (ctrl && !shift)
		change_state(list, target, LVIS_FOCUSED, LVIS_FOCUSED, FALSE);
	else
		select_span(list, target, ctrl, shift);
	if (!list->dead && target < list->count)
		ensure_visible(list, target, FALSE);
}

static void char_typed(list_state *list, WCHAR symbol)
{
	DWORD now = GetTickCount();
	LVFINDINFOW info;
	int found, start;

	if (symbol < 32 || !list->count)
		return;
	if (now - list->search_tick > SEARCH_RESET_MS)
		list->search_length = 0;
	list->search_tick = now;
	if (list->search_length < (int)(sizeof(list->search) / sizeof(WCHAR)) - 1) {
		list->search[list->search_length++] = symbol;
		list->search[list->search_length] = 0;
	}
	ZeroMemory(&info, sizeof(info));
	info.flags = LVFI_STRING | LVFI_PARTIAL | LVFI_WRAP;
	info.psz = list->search;
	start = list->search_length == 1 ? list->focus : list->focus - 1;
	found = find_item(list, start, &info, TRUE);
	if (found >= 0 && found < list->count) {
		select_span(list, found, FALSE, FALSE);
		ensure_visible(list, found, FALSE);
	}
}

/* Forwarded header traffic */

static LRESULT forward_header(list_state *list, NMHEADERW *info)
{
	HDITEMA narrow;
	NMHEADERA converted;
	char text[TEXT_LIMIT * 2];
	UINT code = info->hdr.code;
	UINT ansi;

	switch (code) {
	case HDN_ITEMCHANGINGW: ansi = HDN_ITEMCHANGINGA; break;
	case HDN_ITEMCHANGEDW: ansi = HDN_ITEMCHANGEDA; break;
	case HDN_ITEMCLICKW: ansi = HDN_ITEMCLICKA; break;
	case HDN_DIVIDERDBLCLICKW: ansi = HDN_DIVIDERDBLCLICKA; break;
	case HDN_ENDTRACKW: ansi = HDN_ENDTRACKA; break;
	case HDN_TRACKW: ansi = HDN_TRACKA; break;
	default: ansi = code; break;
	}
	if (list->unicode || ansi == code)
		return SendMessageW(list->parent, WM_NOTIFY, info->hdr.idFrom, (LPARAM)info);

	/* The sender sees the converted code, like the ANSI parent. */
	info->hdr.code = ansi;
	if (info->pitem && (info->pitem->mask & HDI_TEXT) && info->pitem->pszText && info->pitem->pszText != LPSTR_TEXTCALLBACKW) {
		CopyMemory(&narrow, info->pitem, sizeof(narrow));
		if (!WideCharToMultiByte(CP_ACP, 0, info->pitem->pszText, -1, text, sizeof(text), NULL, NULL))
			text[sizeof(text) - 1] = 0;
		narrow.pszText = text;
		narrow.cchTextMax = sizeof(text);
		CopyMemory(&converted, info, sizeof(converted));
		converted.pitem = &narrow;
		return SendMessageW(list->parent, WM_NOTIFY, info->hdr.idFrom, (LPARAM)&converted);
	}
	return SendMessageW(list->parent, WM_NOTIFY, info->hdr.idFrom, (LPARAM)info);
}

static LRESULT header_notify(list_state *list, NMHEADERW *info)
{
	UINT code = info->hdr.code;
	list->columns_dirty = TRUE;
	switch (code) {
	case HDN_ITEMCHANGINGA:
	case HDN_ITEMCHANGINGW:
		if (list->header_quiet)
			return 0;
		return forward_header(list, info);
	case HDN_ITEMCHANGEDA:
	case HDN_ITEMCHANGEDW:
		mark_dirty(list);
		repaint(list);
		if (list->header_quiet)
			return 0;
		return forward_header(list, info);
	case HDN_ITEMCLICKA:
	case HDN_ITEMCLICKW: {
		NMLISTVIEW click;
		if (GetFocus() != list->window)
			SetFocus(list->window);
		ZeroMemory(&click, sizeof(click));
		click.iItem = NO_ITEM;
		click.iSubItem = info->iItem;
		notify(list, &click.hdr, LVN_COLUMNCLICK);
		return forward_header(list, info);
	}
	case HDN_DIVIDERDBLCLICKA:
	case HDN_DIVIDERDBLCLICKW:
		if (info->iItem >= 0 && info->iItem < list->column_count)
			set_column_width(list, info->iItem, LVSCW_AUTOSIZE);
		return forward_header(list, info);
	case HDN_TRACKA:
	case HDN_TRACKW:
	case HDN_ENDTRACKA:
	case HDN_ENDTRACKW:
		mark_dirty(list);
		repaint(list);
		return forward_header(list, info);
	case HDN_BEGINTRACKA:
	case HDN_BEGINTRACKW:
		return 0;
	case HDN_BEGINDRAG:
		return !(list->extended & LVS_EX_HEADERDRAGDROP);
	case HDN_ENDDRAG:
		mark_dirty(list);
		repaint(list);
		return forward_header(list, info);
	case HDN_FILTERCHANGE:
		return forward_header(list, info);
	case HDN_ITEMDBLCLICKA:
	case HDN_ITEMDBLCLICKW:
	case HDN_BEGINFILTEREDIT:
	case HDN_ENDFILTEREDIT:
	case HDN_ITEMSTATEICONCLICK:
	case HDN_ITEMKEYDOWN:
		return 0;
	}
	/* Custom draw and clicks on the header belong to the same parent that owns the list. */
	if ((int)code < 0 && code >= (UINT)NM_LAST)
		return SendMessageW(list->parent, WM_NOTIFY, info->hdr.idFrom, (LPARAM)info);
	return 0;
}

static void ensure_tips(list_state *list)
{
	TTTOOLINFOW tool;
	if (list->tips || !(list->extended & (LVS_EX_INFOTIP | LVS_EX_LABELTIP)))
		return;
	list->tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, list->window, NULL, GetModuleHandleW(NULL), NULL);
	if (!list->tips)
		return;
	ZeroMemory(&tool, sizeof(tool));
	tool.cbSize = sizeof(tool);
	tool.uFlags = TTF_SUBCLASS;
	tool.hwnd = list->window;
	tool.uId = TIP_TOOL;
	GetClientRect(list->window, &tool.rect);
	tool.lpszText = LPSTR_TEXTCALLBACKW;
	SendMessageW(list->tips, TTM_ADDTOOLW, 0, (LPARAM)&tool);
}

static LRESULT tip_notify(list_state *list, NMHDR *header)
{
	if (header->code == TTN_GETDISPINFOW) {
		NMTTDISPINFOW *info = (NMTTDISPINFOW *)header;
		item_view view;
		info->lpszText = list->tip_text;
		list->tip_text[0] = 0;
		if (list->tip_item == NO_ITEM || list->tip_item >= list->count)
			return 0;
		view_item(list, list->tip_item, 0, LVIF_TEXT, 0, &view);
		if (list->extended & LVS_EX_INFOTIP) {
			NMLVGETINFOTIPW tip;
			char narrow[TEXT_LIMIT * 2];
			ZeroMemory(&tip, sizeof(tip));
			tip.dwFlags = LVGIT_UNFOLDED;
			tip.iItem = list->tip_item;
			narrow[0] = 0;
			tip.pszText = list->unicode ? list->tip_text : (WCHAR *)narrow;
			tip.cchTextMax = TEXT_LIMIT;
			notify(list, &tip.hdr, NOTIFY_CODE(list, LVN_GETINFOTIP));
			if (!list->unicode)
				MultiByteToWideChar(CP_ACP, 0, narrow, -1, list->tip_text, TEXT_LIMIT);
		}
		if (!list->tip_text[0] && (list->extended & LVS_EX_LABELTIP)) {
			item_rects rects;
			compute_rects(list, list->tip_item, -1, &rects);
			if (measure(list, view.text) + 2 * LABEL_PADDING > rects.label.right - rects.label.left)
				lstrcpynW(list->tip_text, view.text, TEXT_LIMIT);
		}
	}
	return 0;
}

/* Styles */

static void apply_style(list_state *list, DWORD old, DWORD fresh)
{
	DWORD changed = old ^ fresh;
	list->last_style = fresh;
	if (changed & LVS_TYPEMASK) {
		list->view = fresh & LVS_TYPEMASK;
		list->scroll_row = list->scroll_x = list->scroll_column = 0;
		list->origin.x = list->origin.y = 0;
		if (list->view == LV_VIEW_DETAILS)
			ensure_header(list);
	}
	if ((changed & LVS_NOCOLUMNHEADER) && list->header) {
		LONG_PTR style = GetWindowLongPtrW(list->header, GWL_STYLE);
		if (fresh & LVS_NOCOLUMNHEADER)
			SetWindowLongPtrW(list->header, GWL_STYLE, style | HDS_HIDDEN);
		else
			SetWindowLongPtrW(list->header, GWL_STYLE, style & ~HDS_HIDDEN);
	}
	if (changed & 0xffff) {
		mark_dirty(list);
		repaint(list);
	}
}

static void sync_style(list_state *list)
{
	DWORD now;
	if (!list->created)
		return;
	now = style_of(list);
	if (now != list->last_style)
		apply_style(list, list->last_style, now);
}

static DWORD set_extended(list_state *list, DWORD mask, DWORD value)
{
	DWORD old = list->extended, wanted;
	mask = mask ? mask : (DWORD)~0u;
	wanted = (old & ~mask) | (value & mask);
	list->extended = wanted;

	if ((wanted & LVS_EX_CHECKBOXES) != (old & LVS_EX_CHECKBOXES)) {
		if (wanted & LVS_EX_CHECKBOXES) {
			int index;
			ensure_check_list(list);
			if (!list->owner_data) {
				for (index = 0; index < list->count; index++)
					list->items[index]->state = (list->items[index]->state & ~LVIS_STATEIMAGEMASK) | INDEXTOSTATEIMAGEMASK(1);
			}
		}
		else if (list->own_state_list) {
			if (list->state_list == list->own_state_list) {
				list->state_list = NULL;
				list->state_cx = list->state_cy = 0;
			}
			ImageList_Destroy(list->own_state_list);
			list->own_state_list = NULL;
		}
	}
	if (wanted & (LVS_EX_HEADERDRAGDROP | LVS_EX_GRIDLINES | LVS_EX_FULLROWSELECT | LVS_EX_HEADERINALLVIEWS))
		ensure_header(list);
	if (wanted & LVS_EX_TRANSPARENTBKGND)
		list->back_color = CLR_NONE;
	if (wanted & (LVS_EX_INFOTIP | LVS_EX_LABELTIP))
		ensure_tips(list);
	if (old != wanted) {
		mark_dirty(list);
		repaint(list);
	}
	return old;
}

static HIMAGELIST set_image_list(list_state *list, int which, HIMAGELIST images)
{
	HIMAGELIST old = NULL;
	int cx = 0, cy = 0;
	if (images)
		ImageList_GetIconSize(images, &cx, &cy);
	switch (which) {
	case LVSIL_NORMAL:
		old = list->normal_list;
		list->normal_list = images;
		list->normal_cx = cx;
		list->normal_cy = cy;
		default_spacing(list);
		break;
	case LVSIL_SMALL:
		old = list->small_list;
		list->small_list = images;
		list->small_cx = cx;
		list->small_cy = cy;
		if (list->header)
			SendMessageW(list->header, HDM_SETIMAGELIST, 0, (LPARAM)images);
		break;
	case LVSIL_STATE:
		old = list->state_list == list->own_state_list ? NULL : list->state_list;
		if (list->own_state_list && images != list->own_state_list) {
			ImageList_Destroy(list->own_state_list);
			list->own_state_list = NULL;
		}
		list->state_list = images;
		list->state_cx = cx;
		list->state_cy = cy;
		break;
	default:
		return NULL;
	}
	mark_dirty(list);
	repaint(list);
	return old;
}

static void destroy_lists(list_state *list)
{
	if (list->own_state_list)
		ImageList_Destroy(list->own_state_list);
	if (style_of(list) & LVS_SHAREIMAGELISTS)
		return;
	if (list->normal_list)
		ImageList_Destroy(list->normal_list);
	if (list->small_list)
		ImageList_Destroy(list->small_list);
	if (list->state_list && list->state_list != list->own_state_list)
		ImageList_Destroy(list->state_list);
}

static BOOL set_back_image(list_state *list, const LVBKIMAGEW *image)
{
	HBITMAP previous = list->back_image;
	HBITMAP wanted;
	if (!image)
		return FALSE;
	wanted = (image->ulFlags & LVBKIF_SOURCE_MASK) == LVBKIF_SOURCE_HBITMAP ? image->hbm : NULL;
	list->back_image = NULL;
	if (previous)
		DeleteObject(previous);
	list->back_flags = image->ulFlags;
	list->back_x = image->xOffsetPercent;
	list->back_y = image->yOffsetPercent;
	repaint(list);
	if (!wanted || wanted == previous)
		return FALSE;
	list->back_image = wanted;
	return TRUE;
}

static LRESULT set_icon_spacing(list_state *list, LPARAM value)
{
	DWORD old = MAKELONG(list->spacing_cx, list->spacing_cy);
	if (value == -1) {
		list->custom_spacing = FALSE;
		default_spacing(list);
	}
	else {
		if (LOWORD(value))
			list->spacing_cx = LOWORD(value);
		if (HIWORD(value))
			list->spacing_cy = HIWORD(value);
		list->custom_spacing = TRUE;
	}
	mark_dirty(list);
	return old;
}

static LRESULT rect_query(list_state *list, int index, RECT *target)
{
	item_rects rects;
	int kind;
	if (!target || index < 0 || index >= list->count)
		return FALSE;
	kind = target->left;
	compute_rects(list, index, -1, &rects);
	switch (kind) {
	case LVIR_BOUNDS:
		*target = rects.bounds;
		break;
	case LVIR_ICON:
		*target = rects.icon;
		break;
	case LVIR_LABEL:
		*target = rects.label;
		break;
	case LVIR_SELECTBOUNDS:
		*target = rects.select;
		break;
	default:
		return FALSE;
	}
	return TRUE;
}

static LRESULT subitem_rect(list_state *list, int index, RECT *target)
{
	int sub, kind;
	if (!target || list->view != LV_VIEW_DETAILS)
		return FALSE;
	sub = target->top;
	kind = target->left;
	ensure_layout(list);
	refresh_columns(list);
	if (index == NO_ITEM) {
		/* The header asks in one-based column numbers. */
		if (!sub)
			return FALSE;
		if (sub > 0 && sub < list->column_count) {
			target->left = list->columns[sub].left - list->scroll_x;
			target->right = list->columns[sub].right - list->scroll_x;
		}
		else {
			target->left = target->right = 0;
		}
		target->top = HEADER_ITEM_TOP;
		target->bottom = HEADER_ITEM_TOP + list->item_height;
		return TRUE;
	}
	if (sub == 0) {
		item_rects rects;
		if (index < 0 || index >= list->count)
			return FALSE;
		compute_rects(list, index, -1, &rects);
		if (kind == LVIR_ICON)
			*target = rects.icon;
		else if (kind == LVIR_LABEL)
			*target = rects.label;
		else
			*target = rects.bounds;
		return TRUE;
	}
	if (sub < 0 || sub >= list->column_count)
		return FALSE;
	target->top = report_row_top(list, index);
	target->bottom = target->top + list->item_height;
	target->left = list->columns[sub].left - list->scroll_x;
	target->right = kind == LVIR_ICON ? target->left : list->columns[sub].right - list->scroll_x;
	return TRUE;
}

static LRESULT string_width(list_state *list, const WCHAR *text)
{
	if (!text)
		return 0;
	return measure(list, text) + 12;
}

static BOOL set_item_position(list_state *list, int index, int x, int y)
{
	if (index < 0 || index >= list->count)
		return FALSE;
	if (list->view != LV_VIEW_ICON && list->view != LV_VIEW_SMALLICON)
		return FALSE;
	if (list->owner_data)
		return FALSE;
	list->items[index]->at.x = x + list->origin.x;
	list->items[index]->at.y = y + list->origin.y;
	list->items[index]->placed = TRUE;
	mark_dirty(list);
	repaint(list);
	return TRUE;
}

/* Message procedure */

static LRESULT with_wide_item(list_state *list, UINT message, WPARAM wparam, const LVITEMA *source)
{
	LVITEMW copy;
	WCHAR *text = NULL;
	LRESULT result;

	if (!source)
		return message == LVM_INSERTITEMA ? -1 : FALSE;
	CopyMemory(&copy, source, sizeof(copy));
	if ((message == LVM_SETITEMTEXTA || (source->mask & LVIF_TEXT)) && source->pszText && !IS_CALLBACK(source->pszText)) {
		text = widen(source->pszText);
		copy.pszText = text;
	}
	if (message == LVM_INSERTITEMA) {
		result = insert_item(list, &copy);
	}
	else if (message == LVM_SETITEMA) {
		result = set_item(list, &copy);
	}
	else {
		copy.mask = LVIF_TEXT;
		copy.iItem = (int)wparam;
		result = set_item(list, &copy);
	}
	release(text);
	return result;
}

static LRESULT with_wide_column(list_state *list, UINT message, WPARAM wparam, const LVCOLUMNA *source)
{
	LVCOLUMNW copy;
	WCHAR *text = NULL;
	LRESULT result;

	if (!source)
		return message == LVM_INSERTCOLUMNA ? -1 : FALSE;
	CopyMemory(&copy, source, sizeof(copy));
	if ((source->mask & LVCF_TEXT) && source->pszText) {
		text = widen(source->pszText);
		copy.pszText = text;
	}
	result = message == LVM_INSERTCOLUMNA ? insert_column(list, (int)wparam, &copy) : set_column(list, (int)wparam, &copy);
	release(text);
	return result;
}

static LRESULT with_wide_find(list_state *list, WPARAM wparam, const LVFINDINFOA *source)
{
	LVFINDINFOW copy;
	WCHAR *text = NULL;
	LRESULT result;
	if (!source)
		return -1;
	CopyMemory(&copy, source, sizeof(copy));
	if ((source->flags & (LVFI_STRING | LVFI_PARTIAL | LVFI_SUBSTRING)) && source->psz) {
		text = widen(source->psz);
		copy.psz = text;
	}
	result = find_item(list, (int)wparam, &copy, TRUE);
	release(text);
	return result;
}

static LRESULT list_create(list_state *list, HWND window, const CREATESTRUCTW *create)
{
	list->unicode = SendMessageW(list->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_UNICODE;
	list->view = create->style & LVS_TYPEMASK;
	list->owner_data = (create->style & LVS_OWNERDATA) != 0;
	list->last_style = create->style;
	list->created = TRUE;
	default_spacing(list);
	refresh_metrics(list);
	if (list->view == LV_VIEW_DETAILS)
		ensure_header(list);
	if ((create->style & LVS_OWNERDRAWFIXED) && list->view == LV_VIEW_DETAILS) {
		MEASUREITEMSTRUCT measured;
		ZeroMemory(&measured, sizeof(measured));
		measured.CtlType = ODT_LISTVIEW;
		measured.CtlID = (UINT)GetWindowLongPtrW(window, GWLP_ID);
		measured.itemHeight = list->font_height;
		SendMessageW(list->parent, WM_MEASUREITEM, measured.CtlID, (LPARAM)&measured);
		list->custom_height = measured.itemHeight;
	}
	mark_dirty(list);
	return 0;
}

static void free_state(list_state *list)
{
	release(list->items);
	release(list->columns);
	release(list->ranges);
	if (list->own_font)
		DeleteObject(list->own_font);
	if (list->underline_font)
		DeleteObject(list->underline_font);
	if (list->back_image)
		DeleteObject(list->back_image);
	release(list);
}

static LRESULT list_message(list_state *list, HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message) {
	/* This library is always version 6, whatever is asked for. */
	case CCM_SETVERSION:
	case CCM_GETVERSION:
		return 6;
	case WM_CREATE:
		return list_create(list, window, (const CREATESTRUCTW *)lparam);
	case WM_DESTROY:
		delete_all(list, list->owner_data);
		list->dead = TRUE;
		destroy_lists(list);
		return 0;
	case WM_PAINT:
		if (wparam) {
			if (list->redraw)
				draw_items(list, (HDC)wparam, NULL);
			return 0;
		}
		paint(list);
		return 0;
	case WM_PRINTCLIENT:
		draw_items(list, (HDC)wparam, NULL);
		return 0;
	case WM_ERASEBKGND: {
		RECT client;
		int saved;
		if (!list->redraw)
			return TRUE;
		if (list->extended & LVS_EX_TRANSPARENTBKGND) {
			SendMessageW(list->parent, WM_PRINTCLIENT, wparam, PRF_ERASEBKGND);
			return TRUE;
		}
		if (list->back_color == CLR_NONE) {
			SendMessageW(list->parent, WM_ERASEBKGND, wparam, 0);
			return TRUE;
		}
		GetClientRect(window, &client);
		saved = SaveDC((HDC)wparam);
		exclude_header(list, (HDC)wparam);
		fill_color((HDC)wparam, &client, list->back_color);
		RestoreDC((HDC)wparam, saved);
		return TRUE;
	}
	case WM_SETREDRAW:
		list->redraw = wparam != 0;
		if (list->redraw) {
			mark_dirty(list);
			InvalidateRect(window, NULL, TRUE);
		}
		return 0;
	case WM_SIZE:
		mark_dirty(list);
		ensure_layout(list);
		repaint(list);
		return 0;
	case WM_SHOWWINDOW:
		if (wparam && list->view == LV_VIEW_DETAILS)
			ensure_header(list);
		mark_dirty(list);
		break;
	case WM_SETFONT:
		list->font = (HFONT)wparam;
		if (list->header)
			SendMessageW(list->header, WM_SETFONT, wparam, lparam);
		if (list->underline_font) {
			DeleteObject(list->underline_font);
			list->underline_font = NULL;
		}
		mark_dirty(list);
		if (lparam)
			repaint(list);
		return 0;
	case WM_GETFONT:
		return (LRESULT)list->font;
	case WM_GETDLGCODE:
		return DLGC_WANTCHARS | DLGC_WANTARROWS;
	case WM_SETFOCUS:
		list->has_focus = TRUE;
		notify_plain(list, NM_SETFOCUS);
		repaint(list);
		return 0;
	case WM_KILLFOCUS:
		list->has_focus = FALSE;
		notify_plain(list, NM_KILLFOCUS);
		repaint(list);
		return 0;
	case WM_NOTIFYFORMAT:
		if (lparam == NF_REQUERY)
			list->unicode = SendMessageW(list->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_UNICODE;
		return list->unicode ? NFR_UNICODE : NFR_ANSI;
	case WM_NOTIFY: {
		NMHDR *header = (NMHDR *)lparam;
		if (list->header && header->hwndFrom == list->header)
			return header_notify(list, (NMHEADERW *)header);
		if (list->tips && header->hwndFrom == list->tips)
			return tip_notify(list, header);
		return 0;
	}
	case WM_COMMAND:
		if (list->edit && (HWND)lparam == list->edit) {
			if (HIWORD(wparam) == EN_KILLFOCUS) {
				end_edit(list, TRUE);
				return 0;
			}
			return SendMessageW(list->parent, WM_COMMAND, wparam, lparam);
		}
		return 0;
	case WM_TIMER:
		if (wparam == EDIT_TIMER) {
			KillTimer(window, EDIT_TIMER);
			if (list->focus != NO_ITEM)
				begin_edit(list, list->focus);
			return 0;
		}
		break;
	case WM_STYLECHANGING:
		if (wparam == GWL_STYLE) {
			STYLESTRUCT *change = (STYLESTRUCT *)lparam;
			change->styleNew = (change->styleNew & ~LVS_OWNERDATA) | (change->styleOld & LVS_OWNERDATA);
		}
		return 0;
	case WM_STYLECHANGED:
		if (wparam == GWL_STYLE && list->created) {
			const STYLESTRUCT *change = (const STYLESTRUCT *)lparam;
			if (list->last_style != change->styleNew)
				apply_style(list, list->last_style, change->styleNew);
		}
		return 0;
	case WM_SYSCOLORCHANGE:
	case WM_THEMECHANGED:
		repaint(list);
		return 0;
	case WM_VSCROLL:
		scroll_message(list, SB_VERT, LOWORD(wparam));
		return 0;
	case WM_HSCROLL:
		scroll_message(list, SB_HORZ, LOWORD(wparam));
		return 0;
	case WM_MOUSEWHEEL: {
		int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
		UINT lines = 3;
		SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
		if (!notches)
			return 0;
		note_scroll(list, LVN_BEGINSCROLL, 0, 0);
		if (list->view == LV_VIEW_LIST)
			scroll_view(list, -notches * list->list_width, 0);
		else
			scroll_view(list, 0, -notches * (int)lines * list->item_height);
		if (!list->dead)
			note_scroll(list, LVN_ENDSCROLL, 0, 0);
		return 0;
	}
	case WM_KEYDOWN:
		key_down(list, wparam, lparam);
		return 0;
	case WM_CHAR:
		char_typed(list, (WCHAR)wparam);
		return 0;
	case WM_LBUTTONDOWN: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		button_down(list, VK_LBUTTON, at, (UINT)wparam);
		return 0;
	}
	case WM_RBUTTONDOWN: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		button_down(list, VK_RBUTTON, at, (UINT)wparam);
		return 0;
	}
	case WM_LBUTTONUP: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		button_up(list, VK_LBUTTON, at, (UINT)wparam);
		return 0;
	}
	case WM_RBUTTONUP: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		button_up(list, VK_RBUTTON, at, (UINT)wparam);
		return 0;
	}
	case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDBLCLK: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		UINT flags;
		int sub, index;
		KillTimer(window, EDIT_TIMER);
		index = hit_test(list, at, &flags, &sub);
		send_activate(list, message == WM_LBUTTONDBLCLK ? NM_DBLCLK : NM_RDBLCLK, index, sub < 0 ? 0 : sub, at, (UINT)wparam);
		if (!list->dead && message == WM_LBUTTONDBLCLK && index != NO_ITEM && !(list->extended & LVS_EX_ONECLICKACTIVATE))
			send_activate(list, LVN_ITEMACTIVATE, index, sub < 0 ? 0 : sub, at, (UINT)wparam);
		return 0;
	}
	case WM_MOUSEMOVE: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		mouse_move(list, at, (UINT)wparam);
		return 0;
	}
	case WM_MOUSELEAVE:
		if (list->hot != NO_ITEM) {
			int old = list->hot;
			list->hot = NO_ITEM;
			if (old < list->count)
				invalidate_item(list, old);
		}
		return 0;
	case WM_MOUSEHOVER: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		UINT flags;
		int sub, index;
		if (notify_plain(list, NM_HOVER))
			return 0;
		if (!list->dead && (list->extended & (LVS_EX_TRACKSELECT | LVS_EX_ONECLICKACTIVATE))) {
			index = hit_test(list, at, &flags, &sub);
			if (index != NO_ITEM)
				select_span(list, index, FALSE, FALSE);
		}
		return 0;
	}
	case WM_CAPTURECHANGED:
		if ((HWND)lparam != window && list->press_button) {
			list->press_button = 0;
			if (list->band_active) {
				list->band_active = FALSE;
				repaint(list);
			}
		}
		return 0;
	case WM_SETCURSOR:
		if ((list->extended & (LVS_EX_ONECLICKACTIVATE | LVS_EX_TWOCLICKACTIVATE)) && list->hot != NO_ITEM && (HWND)wparam == window) {
			SetCursor(list->hot_cursor ? list->hot_cursor : LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
			return TRUE;
		}
		break;

	case LVM_GETBKCOLOR:
		return list->back_color;
	case LVM_SETBKCOLOR:
		list->back_color = (COLORREF)lparam;
		list->extended &= ~LVS_EX_TRANSPARENTBKGND;
		return TRUE;
	case LVM_GETIMAGELIST:
		return (LRESULT)(wparam == LVSIL_NORMAL ? list->normal_list : wparam == LVSIL_SMALL ? list->small_list : wparam == LVSIL_STATE ? list->state_list : NULL);
	case LVM_SETIMAGELIST:
		return (LRESULT)set_image_list(list, (int)wparam, (HIMAGELIST)lparam);
	case LVM_GETITEMCOUNT:
		return list->count;
	case LVM_GETITEMA:
		return get_item(list, (LVITEMW *)lparam, FALSE);
	case LVM_GETITEMW:
		return get_item(list, (LVITEMW *)lparam, TRUE);
	case LVM_SETITEMA:
	case LVM_INSERTITEMA:
		return with_wide_item(list, message, wparam, (const LVITEMA *)lparam);
	case LVM_SETITEMW:
		return lparam ? set_item(list, (const LVITEMW *)lparam) : FALSE;
	case LVM_INSERTITEMW:
		return insert_item(list, (const LVITEMW *)lparam);
	case LVM_DELETEITEM:
		return delete_item(list, (int)wparam);
	case LVM_DELETEALLITEMS:
		return delete_all(list, FALSE);
	case LVM_GETCALLBACKMASK:
		return list->callback_mask;
	case LVM_SETCALLBACKMASK:
		list->callback_mask = (UINT)wparam;
		return TRUE;
	case LVM_GETNEXTITEM:
		return next_item(list, (int)wparam, (UINT)LOWORD(lparam));
	case LVM_GETNEXTITEMINDEX: {
		LVITEMINDEX *index = (LVITEMINDEX *)wparam;
		if (!index)
			return FALSE;
		index->iItem = next_item(list, index->iItem, (UINT)LOWORD(lparam));
		return index->iItem != NO_ITEM;
	}
	case LVM_FINDITEMA:
		return with_wide_find(list, wparam, (const LVFINDINFOA *)lparam);
	case LVM_FINDITEMW:
		return find_item(list, (int)wparam, (const LVFINDINFOW *)lparam, TRUE);
	case LVM_GETITEMRECT:
		return rect_query(list, (int)wparam, (RECT *)lparam);
	case LVM_SETITEMPOSITION:
		return set_item_position(list, (int)wparam, (short)LOWORD(lparam), (short)HIWORD(lparam));
	case LVM_SETITEMPOSITION32: {
		const POINT *at = (const POINT *)lparam;
		return at ? set_item_position(list, (int)wparam, at->x, at->y) : FALSE;
	}
	case LVM_GETITEMPOSITION: {
		POINT *at = (POINT *)lparam;
		item_rects rects;
		if (!at || (int)wparam < 0 || (int)wparam >= list->count)
			return FALSE;
		compute_rects(list, (int)wparam, -1, &rects);
		if (list->view == LV_VIEW_DETAILS) {
			at->x = rects.state_icon.left;
			at->y = rects.bounds.top;
		}
		else if ((list->view == LV_VIEW_ICON || list->view == LV_VIEW_SMALLICON) && !list->owner_data) {
			at->x = list->items[wparam]->at.x - list->origin.x;
			at->y = list->items[wparam]->at.y - list->origin.y;
		}
		else {
			at->x = rects.bounds.left;
			at->y = rects.bounds.top;
		}
		return TRUE;
	}
	case LVM_GETSTRINGWIDTHA: {
		WCHAR *text = lparam ? widen((const char *)lparam) : NULL;
		LRESULT width = string_width(list, text);
		release(text);
		return width;
	}
	case LVM_GETSTRINGWIDTHW:
		return string_width(list, (const WCHAR *)lparam);
	case LVM_HITTEST: {
		LVHITTESTINFO *info = (LVHITTESTINFO *)lparam;
		int index, sub;
		if (!info)
			return -1;
		index = hit_test(list, info->pt, &info->flags, &sub);
		info->iItem = index;
		info->iSubItem = 0;
		if (list->view == LV_VIEW_DETAILS && !(list->extended & LVS_EX_FULLROWSELECT) && index != NO_ITEM && sub > 0) {
			index = info->iItem = NO_ITEM;
			info->flags = LVHT_NOWHERE;
		}
		return index;
	}
	case LVM_SUBITEMHITTEST: {
		LVHITTESTINFO *info = (LVHITTESTINFO *)lparam;
		int sub = 0, index;
		if (!info)
			return -1;
		index = hit_test(list, info->pt, &info->flags, &sub);
		info->iItem = sub == NO_ITEM && list->view == LV_VIEW_DETAILS ? NO_ITEM : index;
		info->iSubItem = sub;
		return info->iItem;
	}
	case LVM_ENSUREVISIBLE:
		return ensure_visible(list, (int)wparam, lparam != 0);
	case LVM_SCROLL:
		note_scroll(list, LVN_BEGINSCROLL, (int)wparam, (int)lparam);
		scroll_view(list, (int)wparam, (int)lparam);
		if (!list->dead)
			note_scroll(list, LVN_ENDSCROLL, (int)wparam, (int)lparam);
		return TRUE;
	case LVM_REDRAWITEMS: {
		int first = (int)wparam, last = (int)lparam, index;
		if (first < 0)
			first = 0;
		if (last >= list->count)
			last = list->count - 1;
		for (index = first; index <= last; index++)
			invalidate_item(list, index);
		return TRUE;
	}
	case LVM_UPDATE:
		if ((int)wparam < 0 || (int)wparam >= list->count)
			return FALSE;
		if (style_of(list) & LVS_AUTOARRANGE)
			mark_dirty(list);
		invalidate_item(list, (int)wparam);
		if (list->redraw)
			UpdateWindow(window);
		return TRUE;
	case LVM_ARRANGE: {
		int index;
		if (list->view != LV_VIEW_ICON && list->view != LV_VIEW_SMALLICON)
			return FALSE;
		if (!list->owner_data) {
			for (index = 0; index < list->count; index++)
				list->items[index]->placed = FALSE;
		}
		list->next_slot = 0;
		list->origin.x = list->origin.y = 0;
		mark_dirty(list);
		repaint(list);
		return TRUE;
	}
	case LVM_EDITLABELA:
	case LVM_EDITLABELW:
		return (LRESULT)begin_edit(list, (int)wparam);
	case LVM_GETEDITCONTROL:
		return (LRESULT)list->edit;
	case LVM_CANCELEDITLABEL:
		if (list->edit)
			end_edit(list, TRUE);
		return TRUE;
	case LVM_GETCOLUMNA:
		return get_column(list, (int)wparam, (LVCOLUMNW *)lparam, FALSE);
	case LVM_GETCOLUMNW:
		return get_column(list, (int)wparam, (LVCOLUMNW *)lparam, TRUE);
	case LVM_SETCOLUMNA:
	case LVM_INSERTCOLUMNA:
		return with_wide_column(list, message, wparam, (const LVCOLUMNA *)lparam);
	case LVM_SETCOLUMNW:
		return lparam ? set_column(list, (int)wparam, (const LVCOLUMNW *)lparam) : FALSE;
	case LVM_INSERTCOLUMNW:
		return lparam ? insert_column(list, (int)wparam, (const LVCOLUMNW *)lparam) : -1;
	case LVM_DELETECOLUMN:
		return delete_column(list, (int)wparam);
	case LVM_GETCOLUMNWIDTH:
		if (list->view == LV_VIEW_LIST) {
			ensure_layout(list);
			return list->list_width;
		}
		if (list->view == LV_VIEW_DETAILS && list->header) {
			HDITEMW item;
			ZeroMemory(&item, sizeof(item));
			item.mask = HDI_WIDTH;
			if (SendMessageW(list->header, HDM_GETITEMW, wparam, (LPARAM)&item))
				return item.cxy;
		}
		return 0;
	case LVM_SETCOLUMNWIDTH:
		return set_column_width(list, (int)wparam, (short)LOWORD(lparam));
	case LVM_GETHEADER:
		return (LRESULT)list->header;
	case LVM_CREATEDRAGIMAGE:
		return (LRESULT)create_drag_image(list, (int)wparam, (POINT *)lparam);
	case LVM_GETVIEWRECT: {
		RECT *area = (RECT *)lparam;
		SIZE extent;
		if (!area)
			return FALSE;
		ensure_layout(list);
		view_extent(list, &extent);
		if (list->view == LV_VIEW_DETAILS) {
			area->left = -list->scroll_x;
			area->top = list->count ? header_height(list) - list->scroll_row * list->item_height : 0;
			area->right = area->left + extent.cx;
			area->bottom = list->count ? area->top + extent.cy : 0;
		}
		else if (list->view == LV_VIEW_LIST) {
			area->left = -list->scroll_column * list->list_width;
			area->top = 0;
			area->right = area->left + extent.cx;
			area->bottom = extent.cy;
		}
		else {
			area->left = -list->origin.x;
			area->top = -list->origin.y;
			area->right = area->left + extent.cx;
			area->bottom = area->top + extent.cy;
		}
		return TRUE;
	}
	case LVM_GETTEXTCOLOR:
		return list->text_color;
	case LVM_SETTEXTCOLOR:
		list->text_color = (COLORREF)lparam;
		return TRUE;
	case LVM_GETTEXTBKCOLOR:
		return list->text_back_color;
	case LVM_SETTEXTBKCOLOR:
		list->text_back_color = (COLORREF)lparam;
		return TRUE;
	case LVM_GETTOPINDEX:
		ensure_layout(list);
		if (list->view == LV_VIEW_DETAILS)
			return list->scroll_row;
		if (list->view == LV_VIEW_LIST)
			return list->scroll_column * (list->rows_per_column > 0 ? list->rows_per_column : 1);
		return 0;
	case LVM_GETCOUNTPERPAGE: {
		RECT client;
		if (!list->created)
			return 0;
		ensure_layout(list);
		GetClientRect(window, &client);
		if (list->view == LV_VIEW_DETAILS) {
			int rows = (client.bottom - header_height(list)) / (list->item_height > 0 ? list->item_height : 1);
			return rows > 0 ? rows : 0;
		}
		if (list->view == LV_VIEW_LIST) {
			int shown = client.right / (list->list_width > 0 ? list->list_width : 1);
			return (shown > 0 ? shown : 1) * list->rows_per_column;
		}
		return list->count;
	}
	case LVM_GETORIGIN: {
		POINT *at = (POINT *)lparam;
		if (!at)
			return FALSE;
		at->x = at->y = 0;
		if (list->view == LV_VIEW_ICON || list->view == LV_VIEW_SMALLICON)
			*at = list->origin;
		return TRUE;
	}
	case LVM_SETITEMSTATE: {
		const LVITEMW *item = (const LVITEMW *)lparam;
		if (!item)
			return FALSE;
		if ((int)wparam == NO_ITEM)
			return set_state_all(list, item->stateMask, item->state);
		if ((int)wparam < 0 || (int)wparam >= list->count)
			return FALSE;
		return change_state(list, (int)wparam, item->stateMask, item->state, FALSE);
	}
	case LVM_GETITEMSTATE: {
		item_view view;
		if ((int)wparam < 0 || (int)wparam >= list->count)
			return 0;
		view_item(list, (int)wparam, 0, LVIF_STATE, (UINT)lparam, &view);
		return view.state & (UINT)lparam;
	}
	case LVM_GETITEMTEXTA:
	case LVM_GETITEMTEXTW: {
		LVITEMW *item = (LVITEMW *)lparam;
		item_view view;
		BOOL wide = message == LVM_GETITEMTEXTW;
		if (!item || (int)wparam < 0 || (int)wparam >= list->count || item->iSubItem < 0)
			return 0;
		view_item(list, (int)wparam, item->iSubItem, LVIF_TEXT, 0, &view);
		return copy_text_out(view.text, item->pszText, item->cchTextMax, wide);
	}
	case LVM_SETITEMTEXTA:
		return lparam ? with_wide_item(list, message, wparam, (const LVITEMA *)lparam) : FALSE;
	case LVM_SETITEMTEXTW: {
		LVITEMW copy;
		if (!lparam)
			return FALSE;
		CopyMemory(&copy, (const void *)lparam, sizeof(copy));
		copy.mask = LVIF_TEXT;
		copy.iItem = (int)wparam;
		return set_item(list, &copy);
	}
	case LVM_SETITEMCOUNT:
		if (list->owner_data) {
			int count = (int)wparam;
			if (count < 0)
				count = 0;
			if (count < list->count && count >= 0 && list->count > 0)
				range_remove(list, count, list->count - 1);
			list->count = count;
			if (list->focus >= count)
				list->focus = NO_ITEM;
			if (!count)
				list->mark = NO_ITEM;
			mark_dirty(list);
			repaint(list);
		}
		return TRUE;
	case LVM_SORTITEMS:
		return sort_items(list, (PFNLVCOMPARE)lparam, (LPARAM)wparam, FALSE);
	case LVM_SORTITEMSEX:
		return sort_items(list, (PFNLVCOMPARE)lparam, (LPARAM)wparam, TRUE);
	case LVM_GETSELECTEDCOUNT:
		return selected_total(list);
	case LVM_GETITEMSPACING:
		ensure_layout(list);
		if (wparam)
			return MAKELONG(small_box_width(list), list->item_height);
		return MAKELONG(list->spacing_cx, list->spacing_cy);
	case LVM_GETISEARCHSTRINGW:
		if (lparam)
			lstrcpyW((WCHAR *)lparam, list->search);
		return list->search_length;
	case LVM_GETISEARCHSTRINGA:
		if (lparam)
			WideCharToMultiByte(CP_ACP, 0, list->search, -1, (char *)lparam, sizeof(list->search), NULL, NULL);
		return list->search_length;
	case LVM_SETICONSPACING:
		return set_icon_spacing(list, lparam);
	case LVM_SETEXTENDEDLISTVIEWSTYLE:
		return set_extended(list, (DWORD)wparam, (DWORD)lparam);
	case LVM_GETEXTENDEDLISTVIEWSTYLE:
		return list->extended;
	case LVM_GETSUBITEMRECT:
		return subitem_rect(list, (int)wparam, (RECT *)lparam);
	case LVM_SETCOLUMNORDERARRAY:
		if (!list->header)
			return FALSE;
		list->header_quiet++;
		{
			LRESULT done = SendMessageW(list->header, HDM_SETORDERARRAY, wparam, lparam);
			list->header_quiet--;
			list->columns_dirty = TRUE;
			mark_dirty(list);
			repaint(list);
			return done;
		}
	case LVM_GETCOLUMNORDERARRAY:
		return list->header ? SendMessageW(list->header, HDM_GETORDERARRAY, wparam, lparam) : 0;
	case LVM_SETHOTITEM: {
		int old = list->hot;
		list->hot = (int)wparam;
		return old;
	}
	case LVM_GETHOTITEM:
		return list->hot;
	case LVM_SETHOTCURSOR: {
		HCURSOR old = list->hot_cursor;
		list->hot_cursor = (HCURSOR)lparam;
		return (LRESULT)old;
	}
	case LVM_GETHOTCURSOR:
		return (LRESULT)(list->hot_cursor ? list->hot_cursor : LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
	case LVM_APPROXIMATEVIEWRECT:
		return approximate_rect(list, (short)wparam, LOWORD(lparam), HIWORD(lparam));
	case LVM_SETWORKAREAS: {
		int count = (int)wparam > LV_MAX_WORKAREAS ? LV_MAX_WORKAREAS : (int)wparam;
		if (lparam && count > 0) {
			CopyMemory(list->work_areas, (const void *)lparam, count * sizeof(RECT));
			list->work_area_count = count;
		}
		else {
			list->work_area_count = 0;
		}
		return TRUE;
	}
	case LVM_GETWORKAREAS:
		if (lparam && (int)wparam > 0)
			CopyMemory((void *)lparam, list->work_areas, ((int)wparam < list->work_area_count ? (int)wparam : list->work_area_count) * sizeof(RECT));
		return TRUE;
	case LVM_GETNUMBEROFWORKAREAS:
		if (lparam)
			*(UINT *)lparam = list->work_area_count;
		return TRUE;
	case LVM_GETSELECTIONMARK:
		return list->mark;
	case LVM_SETSELECTIONMARK: {
		int old = list->mark;
		if ((int)lparam == NO_ITEM || ((int)lparam >= 0 && (int)lparam < list->count))
			list->mark = (int)lparam;
		return old;
	}
	case LVM_SETHOVERTIME: {
		DWORD old = list->hover_time;
		list->hover_time = (DWORD)lparam;
		return old;
	}
	case LVM_GETHOVERTIME:
		return list->hover_time;
	case LVM_SETTOOLTIPS: {
		HWND old = list->tips;
		list->tips = (HWND)wparam;
		return (LRESULT)old;
	}
	case LVM_GETTOOLTIPS:
		return (LRESULT)list->tips;
	case LVM_SETUNICODEFORMAT: {
		BOOL old = list->unicode;
		list->unicode = wparam != 0;
		return old;
	}
	case LVM_GETUNICODEFORMAT:
		return list->unicode;
	case LVM_SETBKIMAGEA:
	case LVM_SETBKIMAGEW:
		return set_back_image(list, (const LVBKIMAGEW *)lparam);
	case LVM_GETBKIMAGEA:
	case LVM_GETBKIMAGEW: {
		LVBKIMAGEW *image = (LVBKIMAGEW *)lparam;
		if (!image)
			return FALSE;
		image->ulFlags = list->back_flags;
		image->hbm = list->back_image;
		image->xOffsetPercent = list->back_x;
		image->yOffsetPercent = list->back_y;
		return TRUE;
	}
	case LVM_SETSELECTEDCOLUMN:
		list->selected_column = (int)wparam;
		return TRUE;
	case LVM_GETSELECTEDCOLUMN:
		return list->selected_column;
	case LVM_SETVIEW:
		if (wparam > LV_VIEW_LIST)
			return -1;
		if ((int)wparam != list->view) {
			list->view = (int)wparam;
			list->scroll_row = list->scroll_x = list->scroll_column = 0;
			list->origin.x = list->origin.y = 0;
			if (list->view == LV_VIEW_DETAILS)
				ensure_header(list);
			mark_dirty(list);
			repaint(list);
		}
		return 1;
	case LVM_GETVIEW:
		return list->view;
	case LVM_MAPINDEXTOID:
		if (list->owner_data || (int)wparam < 0 || (int)wparam >= list->count)
			return -1;
		return list->items[wparam]->id;
	case LVM_MAPIDTOINDEX: {
		int index;
		if (list->owner_data)
			return -1;
		for (index = 0; index < list->count; index++) {
			if (list->items[index]->id == (int)wparam)
				return index;
		}
		return -1;
	}
	case LVM_ISITEMVISIBLE:
		return item_is_visible(list, (int)wparam);
	case LVM_ENABLEGROUPVIEW: {
		BOOL old = list->group_view;
		list->group_view = wparam != 0;
		return old == list->group_view ? 0 : 1;
	}
	case LVM_ISGROUPVIEWENABLED:
		return list->group_view;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

static LRESULT CALLBACK list_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	list_state *list = state_of(window);
	LRESULT result;

	if (message == WM_NCCREATE) {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		list = allocate(sizeof(*list));
		if (!list)
			return FALSE;
		list->window = window;
		list->parent = create->hwndParent;
		list->focus = list->mark = list->hot = list->tip_item = NO_ITEM;
		list->edit_item = NO_ITEM;
		list->selected_column = NO_ITEM;
		list->hover_time = HOVER_DEFAULT;
		list->redraw = TRUE;
		list->dirty = TRUE;
		list->columns_dirty = TRUE;
		list->back_color = GetSysColor(COLOR_WINDOW);
		list->text_color = GetSysColor(COLOR_WINDOWTEXT);
		list->text_back_color = CLR_DEFAULT;
		SetWindowLongPtrW(window, 0, (LONG_PTR)list);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	if (!list)
		return DefWindowProcW(window, message, wparam, lparam);
	if (message == WM_NCDESTROY) {
		SetWindowLongPtrW(window, 0, 0);
		list->dead = TRUE;
		if (!list->busy)
			free_state(list);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	if (message != WM_STYLECHANGING && message != WM_STYLECHANGED)
		sync_style(list);
	list->busy++;
	result = list_message(list, window, message, wparam, lparam);
	if (!--list->busy && list->dead && !state_of(window))
		free_state(list);
	return result;
}

void LISTVIEW_Register(void)
{
	WNDCLASSW info;

	ZeroMemory(&info, sizeof(info));
	info.style = CS_GLOBALCLASS | CS_DBLCLKS;
	info.lpfnWndProc = list_proc;
	info.cbWndExtra = sizeof(list_state *);
	info.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	info.lpszClassName = WC_LISTVIEWW;
	RegisterClassW(&info);
}

void LISTVIEW_Unregister(void)
{
	UnregisterClassW(WC_LISTVIEWW, NULL);
}
