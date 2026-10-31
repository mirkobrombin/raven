/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>

/* The toolbar. Two costs are kept apart: a relayout places buttons with the cached button size, a recalc also
 * remeasures that size and the top margin. Messages that only move buttons around must not recalc, since
 * applications size buttons by hand between the two. */

#define DEFAULT_BITMAP_SIZE 16
#define DEFAULT_PADDING_X 7
#define DEFAULT_PADDING_Y 6
#define DEFAULT_SEPARATOR 8
#define WRAPPED_SEPARATOR_HEIGHT 5
#define DROP_ARROW_WIDTH 14
#define TOP_MARGIN 2
#define BOTTOM_MARGIN 2
#define TEXT_EXTRA_X 7
#define AUTOSIZE_EXTRA_X 4
#define LIST_EXTRA_X 8
#define TEXT_GAP_Y 1
#define IMAGE_LISTS 4
#define TIP_LENGTH 256
#define RESTORE_INFO_TEXT 128
#define STD_IMAGE_COUNT 15
#define VIEW_IMAGE_COUNT 12
#define HISTORY_IMAGE_COUNT 5
#define BUTTON_FACE_KEY RGB(192, 192, 192)

extern HMODULE COMCTL32_hModule;

typedef struct {
	int bitmap, command;
	BYTE state, style;
	DWORD_PTR data;
	INT_PTR string;
	WCHAR *text;
	int explicit_width;
	RECT area;
} toolbar_button;

/* A bitmap handed over by TB_ADDBITMAP. Bitmaps are kept so the managed list can be resliced at a new size. */
typedef struct {
	HINSTANCE module;
	UINT_PTR resource;
	HBITMAP bitmap;
	int wanted, start;
	BOOL builtin;
} bitmap_block;

typedef struct {
	HWND window, parent, tooltip;
	DWORD style, extended;
	toolbar_button *buttons;
	int count;
	WCHAR **strings;
	int string_count;
	bitmap_block *blocks;
	int block_count, next_bitmap;
	HIMAGELIST lists[IMAGE_LISTS], hot_list, disabled_list, pressed_list;
	BOOL list_is_ours;
	HFONT font, own_font;
	HTHEME theme;
	BOOL unicode;
	int bitmap_width, bitmap_height, button_width, button_height, padding_x, padding_y;
	int min_button_width, max_button_width, top_margin, indent, text_rows;
	int hot, pressed, press_is_arrow, rows, struct_size;
	DWORD draw_text_mask, draw_text_flags;
	TBINSERTMARK mark;
	COLORREF mark_color;
} toolbar_state;

static toolbar_state *state_of(HWND window)
{
	return (toolbar_state *)GetWindowLongPtrW(window, 0);
}

static void *allocate(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void release(void *block)
{
	HeapFree(GetProcessHeap(), 0, block);
}

static WCHAR *widen(const char *text)
{
	int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	WCHAR *copy = allocate(length * sizeof(WCHAR));
	if (copy)
		MultiByteToWideChar(CP_ACP, 0, text, -1, copy, length);
	return copy;
}

static WCHAR *duplicate(const WCHAR *text)
{
	WCHAR *copy = allocate((lstrlenW(text) + 1) * sizeof(WCHAR));
	if (copy)
		lstrcpyW(copy, text);
	return copy;
}

static WCHAR *copy_text(const void *text, BOOL wide)
{
	return wide ? duplicate(text) : widen(text);
}

static BOOL is_string_index(INT_PTR string)
{
	return string >= 0 && string <= 0xffff;
}

static const WCHAR *text_of(const toolbar_state *state, const toolbar_button *button)
{
	if (button->text)
		return button->text;
	if (is_string_index(button->string) && button->string < state->string_count)
		return state->strings[button->string];
	return NULL;
}

static BOOL shows_text(const toolbar_state *state, const toolbar_button *button)
{
	if (button->style & BTNS_SEP)
		return FALSE;
	if (state->text_rows <= 0 || !text_of(state, button))
		return FALSE;
	if (state->extended & TBSTYLE_EX_MIXEDBUTTONS)
		return (button->style & BTNS_SHOWTEXT) != 0;
	return TRUE;
}

static int index_of_command(const toolbar_state *state, int command)
{
	int index;
	for (index = 0; index < state->count; ++index)
		if (state->buttons[index].command == command)
			return index;
	return -1;
}

static HFONT font_of(const toolbar_state *state)
{
	return state->font ? state->font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static LRESULT notify_header(const toolbar_state *state, NMHDR *header, UINT code)
{
	header->hwndFrom = state->window;
	header->idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	header->code = code;
	return SendMessageW(state->parent, WM_NOTIFY, header->idFrom, (LPARAM)header);
}

static LRESULT notify_button(const toolbar_state *state, UINT code, int command)
{
	NMTOOLBARW notice = {0};
	notice.iItem = command;
	return notify_header(state, &notice.hdr, code);
}

static LRESULT notify_mouse(const toolbar_state *state, UINT code, int index, POINT point)
{
	NMMOUSE notice = {0};
	if (index >= 0) {
		notice.dwItemSpec = state->buttons[index].command;
		notice.dwItemData = state->buttons[index].data;
	} else {
		notice.dwItemSpec = (DWORD_PTR)-1;
	}
	notice.pt = point;
	notice.dwHitInfo = HTCLIENT;
	return notify_header(state, &notice.hdr, code);
}

static void measure_text(const toolbar_state *state, const WCHAR *text, SIZE *size)
{
	HDC dc = GetDC(state->window);
	HGDIOBJ old = SelectObject(dc, font_of(state));
	size->cx = size->cy = 0;
	GetTextExtentPoint32W(dc, text, lstrlenW(text), size);
	SelectObject(dc, old);
	ReleaseDC(state->window, dc);
}

static int font_height(const toolbar_state *state)
{
	TEXTMETRICW metrics;
	HDC dc = GetDC(state->window);
	HGDIOBJ old = SelectObject(dc, font_of(state));
	GetTextMetricsW(dc, &metrics);
	SelectObject(dc, old);
	ReleaseDC(state->window, dc);
	return metrics.tmHeight;
}

static int clamp_width(const toolbar_state *state, int width)
{
	if (state->min_button_width && width < state->min_button_width)
		width = state->min_button_width;
	if (state->max_button_width && width > state->max_button_width)
		width = state->max_button_width;
	return width;
}

static int width_of(const toolbar_state *state, const toolbar_button *button)
{
	int width;
	if (button->style & BTNS_SEP)
		return button->bitmap;
	if (button->explicit_width)
		return button->explicit_width;
	if ((button->style & BTNS_AUTOSIZE)) {
		width = state->bitmap_width + state->padding_x;
		if (shows_text(state, button)) {
			SIZE extent;
			measure_text(state, text_of(state, button), &extent);
			if (state->style & TBSTYLE_LIST)
				width += LIST_EXTRA_X + extent.cx;
			else
				width = max(width, extent.cx + TEXT_EXTRA_X + AUTOSIZE_EXTRA_X);
		}
	} else {
		width = state->button_width;
	}
	if ((button->style & BTNS_WHOLEDROPDOWN) || ((button->style & BTNS_DROPDOWN) && (state->extended & TBSTYLE_EX_DRAWDDARROWS)))
		width += DROP_ARROW_WIDTH;
	return clamp_width(state, width);
}

/* Breaks rows on the last separator of the row when there is one, else right before the button that does not fit. */
static void compute_wraps(toolbar_state *state)
{
	RECT client;
	int index = 0, x = 0, row_start = 0, last_separator = -1;
	GetClientRect(state->window, &client);
	for (index = 0; index < state->count; ++index)
		state->buttons[index].state &= ~TBSTATE_WRAP;
	index = 0;
	while (index < state->count) {
		toolbar_button *button = &state->buttons[index];
		int width;
		if (button->state & TBSTATE_HIDDEN) {
			++index;
			continue;
		}
		width = width_of(state, button);
		if (x && x + width > client.right) {
			if (last_separator >= row_start) {
				state->buttons[last_separator].state |= TBSTATE_WRAP;
				index = row_start = last_separator + 1;
			} else {
				int previous = index - 1;
				while (previous > row_start && (state->buttons[previous].state & TBSTATE_HIDDEN))
					--previous;
				state->buttons[previous].state |= TBSTATE_WRAP;
				row_start = index;
			}
			last_separator = -1;
			x = 0;
			continue;
		}
		if (button->style & BTNS_SEP)
			last_separator = index;
		x += width;
		++index;
	}
}

static void sync_tooltip_tools(const toolbar_state *state)
{
	int index;
	if (!state->tooltip)
		return;
	for (;;) {
		TTTOOLINFOW tool = {sizeof(tool)};
		if (!SendMessageW(state->tooltip, TTM_ENUMTOOLSW, 0, (LPARAM)&tool))
			break;
		SendMessageW(state->tooltip, TTM_DELTOOLW, 0, (LPARAM)&tool);
	}
	for (index = 0; index < state->count; ++index) {
		const toolbar_button *button = &state->buttons[index];
		TTTOOLINFOW tool = {sizeof(tool)};
		if (button->style & BTNS_SEP)
			continue;
		tool.hwnd = state->window;
		tool.uId = button->command;
		tool.rect = button->area;
		tool.lpszText = LPSTR_TEXTCALLBACKW;
		SendMessageW(state->tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool);
	}
}

static void relayout(toolbar_state *state)
{
	RECT client;
	int index, x = state->indent, y = state->top_margin;
	GetClientRect(state->window, &client);
	if (state->style & TBSTYLE_WRAPABLE)
		compute_wraps(state);
	state->rows = 1;
	for (index = 0; index < state->count; ++index) {
		toolbar_button *button = &state->buttons[index];
		int width;
		if (button->state & TBSTATE_HIDDEN) {
			SetRectEmpty(&button->area);
			continue;
		}
		width = width_of(state, button);
		if ((button->style & BTNS_SEP) && (button->state & TBSTATE_WRAP)) {
			BOOL vertical = (state->style & CCS_VERT) != 0;
			int height = vertical ? width : WRAPPED_SEPARATOR_HEIGHT;
			y += state->button_height;
			SetRect(&button->area, 0, y, vertical ? client.right : width, y + height);
			y += height;
			x = state->indent;
			++state->rows;
			continue;
		}
		SetRect(&button->area, x, y, x + width, y + state->button_height);
		x += width;
		if (button->state & TBSTATE_WRAP) {
			x = state->indent;
			y += state->button_height;
			++state->rows;
		}
	}
	sync_tooltip_tools(state);
	InvalidateRect(state->window, NULL, TRUE);
}

static void recalc(toolbar_state *state, BOOL pool_counts)
{
	int width = state->bitmap_width + state->padding_x, height = state->bitmap_height + state->padding_y, index;
	BOOL has_text = pool_counts;
	for (index = 0; index < state->count; ++index) {
		const toolbar_button *button = &state->buttons[index];
		if (!shows_text(state, button))
			continue;
		has_text = TRUE;
		if (!(state->style & TBSTYLE_LIST)) {
			SIZE extent;
			measure_text(state, text_of(state, button), &extent);
			width = max(width, extent.cx + TEXT_EXTRA_X);
		}
	}
	if (has_text && !(state->style & TBSTYLE_LIST) && state->text_rows > 0)
		height += font_height(state) + TEXT_GAP_Y;
	state->button_width = clamp_width(state, width);
	state->button_height = height;
	state->top_margin = (state->style & TBSTYLE_FLAT) ? 0 : TOP_MARGIN;
	relayout(state);
}

static void autosize(toolbar_state *state)
{
	RECT parent_area;
	int x = 0, y = 0, width, height, index, bottom = 0;
	UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
	if (state->style & TBSTYLE_WRAPABLE)
		compute_wraps(state);
	relayout(state);
	if (state->style & CCS_NORESIZE)
		return;
	for (index = 0; index < state->count; ++index)
		bottom = max(bottom, state->buttons[index].area.bottom);
	if (!bottom)
		bottom = state->top_margin + state->button_height;
	height = bottom + BOTTOM_MARGIN;
	{
		RECT frame = {0, 0, 0, 0};
		AdjustWindowRectEx(&frame, (DWORD)GetWindowLongPtrW(state->window, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(state->window, GWL_EXSTYLE));
		height += frame.bottom - frame.top;
	}
	GetClientRect(state->parent, &parent_area);
	width = parent_area.right;
	if (state->style & CCS_VERT) {
		RECT own;
		GetClientRect(state->window, &own);
		width = own.right ? own.right : state->button_width;
		height = parent_area.bottom;
	}
	if ((state->style & CCS_BOTTOM) == CCS_BOTTOM && !(state->style & CCS_VERT))
		y = parent_area.bottom - height;
	if ((state->style & (CCS_NOPARENTALIGN | CCS_NOMOVEY)) || !state->parent)
		flags |= SWP_NOMOVE;
	SetWindowPos(state->window, NULL, x, y, width, height, flags);
	relayout(state);
}

static void store_button(toolbar_state *state, toolbar_button *slot, const TBBUTTON *given, BOOL wide)
{
	memset(slot, 0, sizeof(*slot));
	slot->bitmap = given->iBitmap;
	slot->command = given->idCommand;
	slot->state = given->fsState;
	slot->style = given->fsStyle;
	slot->data = given->dwData;
	slot->string = given->iString;
	if (slot->style & BTNS_SEP) {
		if (slot->bitmap <= 0)
			slot->bitmap = DEFAULT_SEPARATOR;
	} else if (!is_string_index(given->iString) && given->iString != -1) {
		slot->text = copy_text((const void *)given->iString, wide);
	}
}

static const TBBUTTON *button_at(const toolbar_state *state, const void *list, int index)
{
	return (const TBBUTTON *)((const BYTE *)list + (SIZE_T)index * state->struct_size);
}

static BOOL add_buttons(toolbar_state *state, int count, int at, const void *list, BOOL wide)
{
	toolbar_button *grown;
	int index;
	BOOL measure = state->string_count > 0;
	if (count <= 0 || !list)
		return FALSE;
	grown = state->buttons ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, state->buttons, (state->count + count) * sizeof(*grown))
		: allocate(count * sizeof(*grown));
	if (!grown)
		return FALSE;
	state->buttons = grown;
	if (at < 0 || at > state->count)
		at = state->count;
	memmove(&grown[at + count], &grown[at], (state->count - at) * sizeof(*grown));
	for (index = 0; index < count; ++index) {
		TBBUTTON copy = {0};
		memcpy(&copy, button_at(state, list, index), min(state->struct_size, (int)sizeof(copy)));
		store_button(state, &grown[at + index], &copy, wide);
		if (text_of(state, &grown[at + index]))
			measure = TRUE;
	}
	state->count += count;
	if (state->hot >= at)
		state->hot += count;
	if (measure)
		recalc(state, FALSE);
	else
		relayout(state);
	return TRUE;
}

static BOOL delete_button(toolbar_state *state, int index)
{
	if (index < 0 || index >= state->count)
		return FALSE;
	notify_button(state, TBN_DELETINGBUTTON, state->buttons[index].command);
	release(state->buttons[index].text);
	--state->count;
	memmove(&state->buttons[index], &state->buttons[index + 1], (state->count - index) * sizeof(*state->buttons));
	if (state->hot == index)
		state->hot = -1;
	else if (state->hot > index)
		--state->hot;
	if (state->pressed == index)
		state->pressed = -1;
	else if (state->pressed > index)
		--state->pressed;
	relayout(state);
	return TRUE;
}

static void set_checked(toolbar_state *state, int index, BOOL checked)
{
	toolbar_button *button = &state->buttons[index];
	if (!checked) {
		button->state &= ~TBSTATE_CHECKED;
		return;
	}
	button->state |= TBSTATE_CHECKED;
	if ((button->style & BTNS_CHECKGROUP) == BTNS_CHECKGROUP) {
		int other;
		for (other = index - 1; other >= 0 && (state->buttons[other].style & BTNS_CHECKGROUP) == BTNS_CHECKGROUP; --other)
			state->buttons[other].state &= ~TBSTATE_CHECKED;
		for (other = index + 1; other < state->count && (state->buttons[other].style & BTNS_CHECKGROUP) == BTNS_CHECKGROUP; ++other)
			state->buttons[other].state &= ~TBSTATE_CHECKED;
		InvalidateRect(state->window, NULL, TRUE);
	}
}

/* 32-bit bitmaps carry their own transparency, older ones use a key colour. */
static void add_images(HIMAGELIST list, HBITMAP bitmap, BOOL builtin)
{
	BITMAP info;
	/* The built-in images are drawn on the key color, whatever depth loading them gives. */
	if (!builtin && GetObjectW(bitmap, sizeof(info), &info) && info.bmBitsPixel == 32)
		ImageList_Add(list, bitmap, NULL);
	else
		ImageList_AddMasked(list, bitmap, BUTTON_FACE_KEY);
}

/* Reslices every block; the count is never below what the application asked for at each add. */
static void rebuild_list(toolbar_state *state)
{
	HIMAGELIST list;
	int block;
	if (!state->list_is_ours)
		return;
	list = ImageList_Create(state->bitmap_width, state->bitmap_height, ILC_COLOR32 | ILC_MASK, 4, 4);
	if (!list)
		return;
	for (block = 0; block < state->block_count; ++block) {
		const bitmap_block *entry = &state->blocks[block];
		int before = ImageList_GetImageCount(list), wanted = entry->wanted;
		add_images(list, entry->bitmap, entry->builtin);
		if (entry->builtin)
			continue;
		if (wanted == 0)
			ImageList_SetImageCount(list, before + 1);
		else if (wanted > 0 && ImageList_GetImageCount(list) - before < wanted)
			ImageList_SetImageCount(list, before + wanted);
	}
	if (state->lists[0])
		ImageList_Destroy(state->lists[0]);
	state->lists[0] = list;
}

static const struct {
	UINT resource;
	int size, images;
} standard_bitmaps[] = {
	{120, 16, STD_IMAGE_COUNT}, {121, 24, STD_IMAGE_COUNT}, {120, 16, STD_IMAGE_COUNT}, {121, 24, STD_IMAGE_COUNT},
	{124, 16, VIEW_IMAGE_COUNT}, {125, 24, VIEW_IMAGE_COUNT}, {124, 16, VIEW_IMAGE_COUNT}, {125, 24, VIEW_IMAGE_COUNT},
	{130, 16, HISTORY_IMAGE_COUNT}, {131, 24, HISTORY_IMAGE_COUNT},
};

static int add_bitmap(toolbar_state *state, int wanted, const TBADDBITMAP *given)
{
	bitmap_block *grown;
	bitmap_block entry = {0};
	int block, start;
	BOOL standard;
	if (!given)
		return -1;
	if (given->hInst) {
		for (block = 0; block < state->block_count; ++block)
			if (state->blocks[block].module == given->hInst && state->blocks[block].resource == given->nID)
				return state->blocks[block].start;
	}
	standard = given->hInst == HINST_COMMCTRL;
	entry.module = given->hInst;
	entry.resource = given->nID;
	entry.wanted = wanted;
	if (standard) {
		if (given->nID >= ARRAYSIZE(standard_bitmaps))
			return -1;
		entry.bitmap = LoadImageW(COMCTL32_hModule, MAKEINTRESOURCEW(standard_bitmaps[given->nID].resource), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
		entry.builtin = TRUE;
		wanted = standard_bitmaps[given->nID].images;
		state->bitmap_width = state->bitmap_height = standard_bitmaps[given->nID].size;
	} else if (given->hInst) {
		entry.bitmap = LoadBitmapW(given->hInst, MAKEINTRESOURCEW(given->nID));
	} else {
		entry.bitmap = (HBITMAP)CopyImage((HANDLE)given->nID, IMAGE_BITMAP, 0, 0, 0);
	}
	if (!entry.bitmap)
		return -1;
	start = state->next_bitmap;
	entry.start = start;
	state->next_bitmap += wanted ? wanted : 1;
	grown = state->blocks ? HeapReAlloc(GetProcessHeap(), 0, state->blocks, (state->block_count + 1) * sizeof(*grown)) : allocate(sizeof(*grown));
	if (!grown) {
		DeleteObject(entry.bitmap);
		return -1;
	}
	state->blocks = grown;
	grown[state->block_count++] = entry;
	if (!state->lists[0])
		state->list_is_ours = TRUE;
	if (state->list_is_ours)
		rebuild_list(state);
	else
		add_images(state->lists[0], entry.bitmap, entry.builtin);
	if (standard)
		recalc(state, FALSE);
	else
		relayout(state);
	return start;
}

static void adopt_image_list(toolbar_state *state, HIMAGELIST list)
{
	int width = 1, height = 1, previous_width = state->button_width;
	if (list)
		ImageList_GetIconSize(list, &width, &height);
	if (width == state->bitmap_width && height == state->bitmap_height)
		return;
	state->bitmap_width = width;
	state->bitmap_height = height;
	recalc(state, FALSE);
	state->button_width = max(previous_width, state->button_width);
	relayout(state);
}

/* The double-NUL form takes each string whole; a resource splits on its first character and loses its last one. */
static int add_strings(toolbar_state *state, HINSTANCE module, LPARAM source, BOOL wide)
{
	WCHAR **grown;
	int first = state->string_count, count = 0, at;
	WCHAR *loaded = NULL;
	if (!HIWORD(source)) {
		const WCHAR *resource;
		int length;
		WCHAR *cursor, delimiter;
		if (!module || !GetModuleFileNameW(module, (WCHAR[2]){0}, 2))
			return -1;
		length = LoadStringW(module, (UINT)source, (WCHAR *)&resource, 0);
		if (length <= 1)
			return first;
		loaded = allocate(length * sizeof(WCHAR));
		if (!loaded)
			return -1;
		memcpy(loaded, resource, (length - 1) * sizeof(WCHAR));
		delimiter = loaded[0];
		for (cursor = loaded + 1; *cursor; ++cursor)
			if (*cursor == delimiter) {
				*cursor = 0;
				++count;
				if (cursor[1] == 0 && cursor + 1 >= loaded + length - 1)
					break;
			}
		if (!count) {
			release(loaded);
			return first;
		}
		grown = state->strings ? HeapReAlloc(GetProcessHeap(), 0, state->strings, (first + count) * sizeof(*grown)) : allocate(count * sizeof(*grown));
		if (!grown) {
			release(loaded);
			return -1;
		}
		state->strings = grown;
		cursor = loaded + 1;
		for (at = 0; at < count; ++at) {
			grown[first + at] = duplicate(cursor);
			cursor += lstrlenW(cursor) + 1;
		}
		release(loaded);
	} else {
		if (wide) {
			const WCHAR *cursor;
			for (cursor = (const WCHAR *)source; *cursor; cursor += lstrlenW(cursor) + 1)
				++count;
		} else {
			const char *cursor;
			for (cursor = (const char *)source; *cursor; cursor += lstrlenA(cursor) + 1)
				++count;
		}
		if (!count)
			return first;
		grown = state->strings ? HeapReAlloc(GetProcessHeap(), 0, state->strings, (first + count) * sizeof(*grown)) : allocate(count * sizeof(*grown));
		if (!grown)
			return -1;
		state->strings = grown;
		if (wide) {
			const WCHAR *cursor = (const WCHAR *)source;
			for (at = 0; at < count; ++at, cursor += lstrlenW(cursor) + 1)
				grown[first + at] = duplicate(cursor);
		} else {
			const char *cursor = (const char *)source;
			for (at = 0; at < count; ++at, cursor += lstrlenA(cursor) + 1)
				grown[first + at] = widen(cursor);
		}
	}
	state->string_count += count;
	recalc(state, TRUE);
	return first;
}

static int get_text_out(const WCHAR *text, void *out, int capacity, BOOL wide)
{
	if (wide) {
		int length = lstrlenW(text);
		if (out && capacity > 0)
			lstrcpynW(out, text, capacity);
		return length;
	} else {
		int length = WideCharToMultiByte(CP_ACP, 0, text, -1, NULL, 0, NULL, NULL) - 1;
		if (out && capacity > 0) {
			if (!WideCharToMultiByte(CP_ACP, 0, text, -1, out, capacity, NULL, NULL))
				((char *)out)[capacity - 1] = 0;
		}
		return length;
	}
}

static BOOL set_hot(toolbar_state *state, int index, DWORD reason)
{
	NMTBHOTITEM notice = {0};
	int previous = state->hot;
	if (index >= state->count)
		return FALSE;
	if (index < 0)
		index = -1;
	if (index == previous)
		return TRUE;
	notice.idOld = previous >= 0 ? state->buttons[previous].command : 0;
	notice.idNew = index >= 0 ? state->buttons[index].command : 0;
	notice.dwFlags = reason | (previous < 0 ? HICF_ENTERING : 0) | (index < 0 ? HICF_LEAVING : 0);
	if (notify_header(state, &notice.hdr, TBN_HOTITEMCHANGE))
		return FALSE;
	if (index >= 0 && !(state->buttons[index].state & TBSTATE_ENABLED))
		index = -1;
	state->hot = index;
	InvalidateRect(state->window, NULL, TRUE);
	return TRUE;
}

static int hit_test(const toolbar_state *state, POINT point)
{
	int index;
	for (index = 0; index < state->count; ++index) {
		const toolbar_button *button = &state->buttons[index];
		if (button->state & TBSTATE_HIDDEN)
			continue;
		if (PtInRect(&button->area, point))
			return (button->style & BTNS_SEP) ? -index - 1 : index;
	}
	return -state->count - 1;
}

static BOOL over_drop_arrow(const toolbar_state *state, const toolbar_button *button, POINT point)
{
	if ((button->style & BTNS_WHOLEDROPDOWN))
		return TRUE;
	if (!(button->style & BTNS_DROPDOWN))
		return FALSE;
	if (!(state->extended & TBSTYLE_EX_DRAWDDARROWS))
		return TRUE;
	return point.x >= button->area.right - DROP_ARROW_WIDTH;
}

static void mouse_down(toolbar_state *state, POINT point)
{
	int index = hit_test(state, point);
	toolbar_button *button;
	if (index < 0)
		return;
	button = &state->buttons[index];
	if (!(button->state & TBSTATE_ENABLED))
		return;
	if (notify_mouse(state, NM_LDOWN, index, point))
		return;
	if (over_drop_arrow(state, button, point)) {
		state->press_is_arrow = TRUE;
		state->pressed = index;
		InvalidateRect(state->window, NULL, TRUE);
		UpdateWindow(state->window);
		notify_button(state, TBN_DROPDOWN, button->command);
		state->pressed = -1;
		state->press_is_arrow = FALSE;
		InvalidateRect(state->window, NULL, TRUE);
		return;
	}
	state->pressed = index;
	button->state |= TBSTATE_PRESSED;
	SetCapture(state->window);
	notify_button(state, TBN_BEGINDRAG, button->command);
	InvalidateRect(state->window, NULL, TRUE);
}

static void mouse_up(toolbar_state *state, POINT point)
{
	int index = state->pressed, under;
	toolbar_button *button;
	if (index < 0)
		return;
	button = &state->buttons[index];
	under = hit_test(state, point);
	state->pressed = -1;
	button->state &= ~TBSTATE_PRESSED;
	if (GetCapture() == state->window)
		ReleaseCapture();
	notify_button(state, TBN_ENDDRAG, button->command);
	InvalidateRect(state->window, NULL, TRUE);
	if (under != index)
		return;
	if (button->style & BTNS_CHECK) {
		BOOL exclusive = (button->style & BTNS_CHECKGROUP) == BTNS_CHECKGROUP;
		set_checked(state, index, exclusive || !(button->state & TBSTATE_CHECKED));
	}
	notify_mouse(state, NM_CLICK, index, point);
	SendMessageW(state->parent, WM_COMMAND, MAKEWPARAM(button->command, BN_CLICKED), (LPARAM)state->window);
}

static void resolve_image(toolbar_state *state, toolbar_button *button, int *image)
{
	*image = button->bitmap;
	if (button->bitmap == I_IMAGECALLBACK) {
		NMTBDISPINFOW answer = {0};
		answer.dwMask = TBNF_IMAGE;
		answer.idCommand = button->command;
		answer.lParam = button->data;
		answer.iImage = I_IMAGECALLBACK;
		notify_header(state, &answer.hdr, TBN_GETDISPINFOW);
		*image = answer.iImage;
		if (answer.dwMask & TBNF_DI_SETITEM)
			button->bitmap = answer.iImage;
	}
}

static void draw_image(toolbar_state *state, HDC dc, toolbar_button *button, int x, int y, BOOL disabled, BOOL hot, BOOL pressed)
{
	int image;
	HIMAGELIST list;
	resolve_image(state, button, &image);
	if (image < 0 || image == I_IMAGENONE)
		return;
	list = state->lists[min(HIWORD(image), IMAGE_LISTS - 1)];
	if (disabled && state->disabled_list)
		list = state->disabled_list;
	else if (hot && state->hot_list)
		list = state->hot_list;
	else if (pressed && state->pressed_list)
		list = state->pressed_list;
	if (!list)
		return;
	if (disabled && list != state->disabled_list)
		ImageList_DrawEx(list, LOWORD(image), dc, x, y, 0, 0, CLR_NONE, GetSysColor(COLOR_BTNFACE), ILD_TRANSPARENT | ILD_BLEND50);
	else
		ImageList_Draw(list, LOWORD(image), dc, x, y, ILD_TRANSPARENT);
}

static void draw_drop_arrow(HDC dc, const RECT *area, BOOL disabled)
{
	HBRUSH brush = GetSysColorBrush(disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT);
	int center = (area->left + area->right) / 2, middle = (area->top + area->bottom) / 2 - 1, row;
	for (row = 0; row < 3; ++row) {
		RECT line = {center - 2 + row, middle + row, center + 3 - row, middle + row + 1};
		FillRect(dc, &line, brush);
	}
}

static void draw_button_frame(toolbar_state *state, HDC dc, const RECT *area, const toolbar_button *button, BOOL hot, BOOL pressed, BOOL checked, UINT custom)
{
	BOOL disabled = !(button->state & TBSTATE_ENABLED);
	if (custom & TBCDRF_NOEDGES)
		return;
	if (state->theme) {
		int look = disabled ? TS_DISABLED : pressed ? TS_PRESSED : checked ? (hot ? TS_HOTCHECKED : TS_CHECKED) : hot ? TS_HOT : TS_NORMAL;
		if (!(custom & TBCDRF_NOBACKGROUND) && (look != TS_NORMAL || !(state->style & TBSTYLE_FLAT)))
			DrawThemeBackground(state->theme, dc, TP_BUTTON, look, area, NULL);
		return;
	}
	if (state->style & TBSTYLE_FLAT) {
		if (pressed || checked)
			DrawEdge(dc, (RECT *)area, BDR_SUNKENOUTER, BF_RECT);
		else if (hot)
			DrawEdge(dc, (RECT *)area, BDR_RAISEDINNER, BF_RECT);
	} else {
		DrawEdge(dc, (RECT *)area, (pressed || checked) ? EDGE_SUNKEN : EDGE_RAISED, BF_SOFT | BF_RECT | BF_MIDDLE);
	}
}

static void draw_separator(const toolbar_state *state, HDC dc, const toolbar_button *button)
{
	RECT line = button->area;
	if (state->theme) {
		DrawThemeBackground(state->theme, dc, TP_SEPARATOR, TS_NORMAL, &line, NULL);
		return;
	}
	if (!(state->style & TBSTYLE_FLAT))
		return;
	if (button->state & TBSTATE_WRAP) {
		line.top = (line.top + line.bottom) / 2 - 1;
		line.bottom = line.top + 2;
		DrawEdge(dc, &line, EDGE_ETCHED, BF_TOP);
	} else {
		line.left = (line.left + line.right) / 2 - 1;
		line.right = line.left + 2;
		DrawEdge(dc, &line, EDGE_ETCHED, BF_LEFT);
	}
}

static void paint_button(toolbar_state *state, HDC dc, NMTBCUSTOMDRAW *custom, BOOL wants_items, int index)
{
	toolbar_button *button = &state->buttons[index];
	RECT area = button->area, content;
	BOOL disabled = !(button->state & TBSTATE_ENABLED), hot = state->hot == index;
	BOOL pressed = state->pressed == index || (button->state & TBSTATE_PRESSED);
	BOOL split = (button->style & BTNS_DROPDOWN) && (state->extended & TBSTYLE_EX_DRAWDDARROWS);
	BOOL whole = (button->style & BTNS_WHOLEDROPDOWN) != 0;
	BOOL checked = (button->state & TBSTATE_CHECKED) != 0;
	UINT answer = CDRF_DODEFAULT;
	int image_x, image_y;
	if (button->state & TBSTATE_HIDDEN)
		return;
	if (button->style & BTNS_SEP) {
		draw_separator(state, dc, button);
		return;
	}
	if (wants_items) {
		custom->nmcd.dwDrawStage = CDDS_ITEMPREPAINT;
		custom->nmcd.rc = area;
		custom->nmcd.dwItemSpec = button->command;
		custom->nmcd.lItemlParam = button->data;
		custom->nmcd.uItemState = (hot ? CDIS_HOT : 0) | (pressed ? CDIS_SELECTED : 0) | (disabled ? CDIS_DISABLED : 0) | ((button->state & TBSTATE_CHECKED) ? CDIS_CHECKED : 0);
		custom->clrText = GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT);
		custom->rcText = area;
		answer = (UINT)SendMessageW(state->parent, WM_NOTIFY, custom->nmcd.hdr.idFrom, (LPARAM)custom);
		if (answer & CDRF_SKIPDEFAULT)
			return;
		/* The program may have drawn a state itself and cleared it to leave the rest to the default drawing. */
		hot = (custom->nmcd.uItemState & CDIS_HOT) != 0;
		pressed = (custom->nmcd.uItemState & CDIS_SELECTED) != 0;
		checked = (custom->nmcd.uItemState & CDIS_CHECKED) != 0;
		SetTextColor(dc, custom->clrText);
	} else {
		SetTextColor(dc, GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
	}
	draw_button_frame(state, dc, &area, button, hot, pressed, checked, answer);
	content = area;
	if (split || whole) {
		RECT arrow = area;
		arrow.left = arrow.right - DROP_ARROW_WIDTH;
		if (split) {
			content.right = arrow.left;
			if (state->theme)
				DrawThemeBackground(state->theme, dc, TP_SPLITBUTTONDROPDOWN, disabled ? TS_DISABLED : hot ? TS_HOT : TS_NORMAL, &arrow, NULL);
		} else {
			content.right -= DROP_ARROW_WIDTH;
		}
		draw_drop_arrow(dc, &arrow, disabled);
	}
	if (pressed && !state->theme && !(answer & TBCDRF_NOOFFSET))
		OffsetRect(&content, 1, 1);
	if (state->style & TBSTYLE_LIST) {
		image_x = content.left + state->padding_x / 2;
		image_y = content.top + (content.bottom - content.top - state->bitmap_height) / 2;
	} else {
		image_x = content.left + (content.right - content.left - state->bitmap_width) / 2;
		image_y = content.top + state->padding_y / 2;
	}
	draw_image(state, dc, button, image_x, image_y, disabled, hot, pressed);
	if (shows_text(state, button)) {
		RECT text_area = content;
		UINT format = DT_SINGLELINE | DT_END_ELLIPSIS | ((button->style & BTNS_NOPREFIX) ? DT_NOPREFIX : 0);
		if (state->style & TBSTYLE_LIST) {
			text_area.left = image_x + state->bitmap_width + state->padding_x / 2;
			format |= DT_LEFT | DT_VCENTER;
		} else {
			text_area.top = image_y + state->bitmap_height + TEXT_GAP_Y;
			format |= DT_CENTER | DT_TOP;
		}
		format = (format & ~state->draw_text_mask) | (state->draw_text_flags & state->draw_text_mask);
		DrawTextW(dc, text_of(state, button), -1, &text_area, format);
	}
	if (wants_items && (answer & CDRF_NOTIFYPOSTPAINT)) {
		custom->nmcd.dwDrawStage = CDDS_ITEMPOSTPAINT;
		SendMessageW(state->parent, WM_NOTIFY, custom->nmcd.hdr.idFrom, (LPARAM)custom);
	}
}

static void paint(toolbar_state *state, HDC dc)
{
	NMTBCUSTOMDRAW custom = {0};
	RECT client;
	HGDIOBJ old;
	UINT answer;
	int index;
	GetClientRect(state->window, &client);
	if (state->style & TBSTYLE_TRANSPARENT)
		DrawThemeParentBackground(state->window, dc, &client);
	old = SelectObject(dc, font_of(state));
	SetBkMode(dc, TRANSPARENT);
	custom.nmcd.hdc = dc;
	custom.nmcd.rc = client;
	custom.nmcd.dwDrawStage = CDDS_PREPAINT;
	answer = (UINT)notify_header(state, &custom.nmcd.hdr, NM_CUSTOMDRAW);
	if (!(answer & CDRF_SKIPDEFAULT)) {
		for (index = 0; index < state->count; ++index) {
			RECT visible;
			if (IntersectRect(&visible, &state->buttons[index].area, &client))
				paint_button(state, dc, &custom, (answer & CDRF_NOTIFYITEMDRAW) != 0, index);
		}
		if (state->mark.iButton >= 0 && state->mark.iButton < state->count) {
			RECT line = state->buttons[state->mark.iButton].area;
			HBRUSH brush = CreateSolidBrush(state->mark_color);
			if (state->mark.dwFlags & TBIMHT_AFTER)
				line.left = line.right - 1;
			else
				line.right = line.left + 1;
			FillRect(dc, &line, brush);
			DeleteObject(brush);
		}
	}
	if (answer & CDRF_NOTIFYPOSTPAINT) {
		custom.nmcd.dwDrawStage = CDDS_POSTPAINT;
		custom.nmcd.rc = client;
		SendMessageW(state->parent, WM_NOTIFY, custom.nmcd.hdr.idFrom, (LPARAM)&custom);
	}
	SelectObject(dc, old);
}

/* Info tips come first; the fallback is what the application answers to the tooltip, then the button text. */
static LRESULT answer_tooltip(toolbar_state *state, NMTTDISPINFOW *question)
{
	int command = (int)question->hdr.idFrom, index = index_of_command(state, command);
	if (state->unicode) {
		static WCHAR tip[TIP_LENGTH];
		NMTBGETINFOTIPW info = {0};
		NMTTDISPINFOW relayed = *question;
		tip[0] = 0;
		info.pszText = tip;
		info.cchTextMax = ARRAYSIZE(tip);
		info.iItem = command;
		info.lParam = index >= 0 ? state->buttons[index].data : 0;
		notify_header(state, &info.hdr, TBN_GETINFOTIPW);
		if (info.pszText && info.pszText[0]) {
			question->lpszText = info.pszText;
			return 0;
		}
		SendMessageW(state->parent, WM_NOTIFY, question->hdr.idFrom, (LPARAM)&relayed);
		*question = relayed;
		if (question->lpszText && question->lpszText != LPSTR_TEXTCALLBACKW && question->lpszText[0])
			return 0;
		if (question->szText[0])
			return 0;
	} else {
		static char tip[TIP_LENGTH];
		NMTBGETINFOTIPA info = {0};
		NMTTDISPINFOA relayed = {0};
		const char *found = NULL;
		tip[0] = 0;
		info.pszText = tip;
		info.cchTextMax = ARRAYSIZE(tip);
		info.iItem = command;
		info.lParam = index >= 0 ? state->buttons[index].data : 0;
		notify_header(state, &info.hdr, TBN_GETINFOTIPA);
		if (info.pszText && info.pszText[0]) {
			found = info.pszText;
		} else {
			relayed.hdr = question->hdr;
			relayed.hdr.code = TTN_GETDISPINFOA;
			relayed.hinst = question->hinst;
			relayed.uFlags = question->uFlags;
			relayed.lParam = question->lParam;
			SendMessageW(state->parent, WM_NOTIFY, relayed.hdr.idFrom, (LPARAM)&relayed);
			if (relayed.hinst && IS_INTRESOURCE(relayed.lpszText)) {
				question->hinst = relayed.hinst;
				question->lpszText = (WCHAR *)relayed.lpszText;
				return 0;
			}
			found = (relayed.lpszText && relayed.lpszText != LPSTR_TEXTCALLBACKA && relayed.lpszText[0]) ? relayed.lpszText : relayed.szText[0] ? relayed.szText : NULL;
		}
		if (found) {
			MultiByteToWideChar(CP_ACP, 0, found, -1, question->szText, ARRAYSIZE(question->szText));
			question->szText[ARRAYSIZE(question->szText) - 1] = 0;
			question->lpszText = question->szText;
			return 0;
		}
	}
	if (index >= 0 && text_of(state, &state->buttons[index])) {
		lstrcpynW(question->szText, text_of(state, &state->buttons[index]), ARRAYSIZE(question->szText));
		question->lpszText = question->szText;
	}
	return 0;
}

static LRESULT forward_notify(toolbar_state *state, NMHDR *header)
{
	if (header->code == CBEN_ENDEDITW && !state->unicode) {
		const NMCBEENDEDITW *source = (const NMCBEENDEDITW *)header;
		NMCBEENDEDITA converted;
		converted.hdr = source->hdr;
		converted.hdr.code = CBEN_ENDEDITA;
		converted.fChanged = source->fChanged;
		converted.iNewSelection = source->iNewSelection;
		converted.iWhy = source->iWhy;
		WideCharToMultiByte(CP_ACP, 0, source->szText, -1, converted.szText, sizeof(converted.szText), NULL, NULL);
		return SendMessageW(state->parent, WM_NOTIFY, header->idFrom, (LPARAM)&converted);
	}
	return SendMessageW(state->parent, WM_NOTIFY, header->idFrom, (LPARAM)header);
}

/* The saved value is one command per button with whatever the parent wrote after it. */
static BOOL save_buttons(toolbar_state *state, const TBSAVEPARAMSW *params)
{
	NMTBSAVE notice = {0};
	HKEY key;
	int index;
	LONG failure;
	notice.iItem = -1;
	notice.cButtons = state->count;
	notice.cbData = state->count * sizeof(DWORD);
	notify_header(state, &notice.hdr, TBN_SAVE);
	if (!notice.pData) {
		notice.pData = allocate(notice.cbData);
		if (!notice.pData)
			return FALSE;
		notice.pCurrent = notice.pData;
	}
	for (index = 0; index < state->count; ++index) {
		const toolbar_button *button = &state->buttons[index];
		*notice.pCurrent++ = button->command;
		notice.iItem = index;
		notice.tbButton.iBitmap = button->bitmap;
		notice.tbButton.idCommand = button->command;
		notice.tbButton.fsState = button->state;
		notice.tbButton.fsStyle = button->style;
		notice.tbButton.dwData = button->data;
		notice.tbButton.iString = button->string;
		notify_header(state, &notice.hdr, TBN_SAVE);
	}
	failure = RegCreateKeyExW(params->hkr, params->pszSubKey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
	if (!failure) {
		failure = RegSetValueExW(key, params->pszValueName, 0, REG_BINARY, (const BYTE *)notice.pData, notice.cbData);
		RegCloseKey(key);
	}
	release(notice.pData);
	return !failure;
}

static void describe_restored(TBBUTTON *button, int command)
{
	memset(button, 0, sizeof(*button));
	if (command < 0) {
		button->iBitmap = DEFAULT_SEPARATOR;
		button->fsStyle = BTNS_SEP;
		button->fsState = command == -1 ? 0 : TBSTATE_HIDDEN;
	} else {
		button->iBitmap = -1;
		button->idCommand = command;
	}
}

static void refresh_from_parent(toolbar_state *state)
{
	int item = 0;
	notify_header(state, &(NMHDR){0}, TBN_BEGINADJUST);
	for (;;) {
		TBBUTTON found;
		int at;
		if (state->unicode) {
			WCHAR text[RESTORE_INFO_TEXT] = {0};
			NMTOOLBARW question = {0};
			question.iItem = item;
			question.cchText = RESTORE_INFO_TEXT;
			question.pszText = text;
			if (!notify_header(state, &question.hdr, TBN_GETBUTTONINFOW))
				break;
			found = question.tbButton;
		} else {
			char text[RESTORE_INFO_TEXT] = {0};
			NMTOOLBARA question = {0};
			question.iItem = item;
			question.cchText = RESTORE_INFO_TEXT;
			question.pszText = text;
			if (!notify_header(state, &question.hdr, TBN_GETBUTTONINFOA))
				break;
			found = question.tbButton;
		}
		at = index_of_command(state, found.idCommand);
		if (at >= 0) {
			toolbar_button *button = &state->buttons[at];
			release(button->text);
			button->text = NULL;
			store_button(state, button, &found, state->unicode);
		}
		++item;
	}
	notify_header(state, &(NMHDR){0}, TBN_ENDADJUST);
}

static BOOL restore_buttons(toolbar_state *state, const TBSAVEPARAMSW *params)
{
	NMTBRESTORE notice = {0};
	HKEY key;
	DWORD size = 0, type;
	int index, total;
	if (RegOpenKeyExW(params->hkr, params->pszSubKey, 0, KEY_QUERY_VALUE, &key))
		return FALSE;
	if (RegQueryValueExW(key, params->pszValueName, NULL, &type, NULL, &size) || type != REG_BINARY) {
		RegCloseKey(key);
		return FALSE;
	}
	notice.pData = allocate(size);
	if (!notice.pData) {
		RegCloseKey(key);
		return FALSE;
	}
	if (RegQueryValueExW(key, params->pszValueName, NULL, NULL, (BYTE *)notice.pData, &size)) {
		RegCloseKey(key);
		release(notice.pData);
		return FALSE;
	}
	RegCloseKey(key);
	notice.pCurrent = notice.pData;
	notice.cbData = size;
	notice.iItem = -1;
	notice.cButtons = size / sizeof(DWORD);
	notice.cbBytesPerRecord = sizeof(DWORD);
	if (notify_header(state, &notice.hdr, TBN_RESTORE)) {
		release(notice.pData);
		return FALSE;
	}
	total = notice.cButtons;
	while (state->count)
		delete_button(state, state->count - 1);
	for (index = 0; index < total; ++index) {
		int command = (int)*notice.pCurrent++;
		TBBUTTON button;
		describe_restored(&notice.tbButton, command);
		notice.iItem = index;
		notify_header(state, &notice.hdr, TBN_RESTORE);
		button = notice.tbButton;
		/* Only a pool index survives; a pointer from this notification is not kept. */
		if (!is_string_index(button.iString))
			button.iString = 0;
		add_buttons(state, 1, -1, &button, TRUE);
	}
	release(notice.pData);
	refresh_from_parent(state);
	relayout(state);
	return TRUE;
}

static int fill_button_info(toolbar_state *state, int index, TBBUTTONINFOW *info, BOOL wide)
{
	const toolbar_button *button = &state->buttons[index];
	if (info->dwMask & TBIF_COMMAND)
		info->idCommand = button->command;
	if (info->dwMask & TBIF_IMAGE)
		info->iImage = button->bitmap;
	if (info->dwMask & TBIF_LPARAM)
		info->lParam = button->data;
	if (info->dwMask & TBIF_SIZE)
		info->cx = (WORD)(button->area.right - button->area.left);
	if (info->dwMask & TBIF_STATE)
		info->fsState = button->state;
	if (info->dwMask & TBIF_STYLE)
		info->fsStyle = button->style;
	if ((info->dwMask & TBIF_TEXT) && info->pszText) {
		const WCHAR *text = text_of(state, button);
		get_text_out(text ? text : L"", info->pszText, info->cchText, wide);
	}
	return index;
}

static BOOL apply_button_info(toolbar_state *state, int index, const TBBUTTONINFOW *info, BOOL wide)
{
	toolbar_button *button = &state->buttons[index];
	if (info->dwMask & TBIF_COMMAND)
		button->command = info->idCommand;
	if (info->dwMask & TBIF_IMAGE)
		button->bitmap = info->iImage;
	if (info->dwMask & TBIF_LPARAM)
		button->data = info->lParam;
	if (info->dwMask & TBIF_SIZE)
		button->explicit_width = info->cx;
	if (info->dwMask & TBIF_STATE) {
		button->state = info->fsState;
	}
	if (info->dwMask & TBIF_STYLE)
		button->style = info->fsStyle;
	if ((info->dwMask & TBIF_TEXT) && info->pszText != LPSTR_TEXTCALLBACKW && info->pszText) {
		WCHAR *text = copy_text(info->pszText, wide);
		if (text) {
			release(button->text);
			button->text = text;
		}
	}
	relayout(state);
	return TRUE;
}

static void set_rows(toolbar_state *state, int wanted, BOOL larger, RECT *result)
{
	int visible = 0, per_row, index, in_row = 0, rows;
	for (index = 0; index < state->count; ++index) {
		state->buttons[index].state &= ~TBSTATE_WRAP;
		if (!(state->buttons[index].state & TBSTATE_HIDDEN))
			++visible;
	}
	if (wanted < 1)
		wanted = 1;
	per_row = visible ? (visible + wanted - 1) / wanted : 1;
	rows = visible ? (visible + per_row - 1) / per_row : 1;
	if (larger && rows < wanted && per_row > 1)
		--per_row;
	for (index = 0; index < state->count; ++index) {
		if (state->buttons[index].state & TBSTATE_HIDDEN)
			continue;
		if (++in_row == per_row && index < state->count - 1) {
			state->buttons[index].state |= TBSTATE_WRAP;
			in_row = 0;
		}
	}
	relayout(state);
	if (result) {
		SetRectEmpty(result);
		for (index = 0; index < state->count; ++index)
			UnionRect(result, result, &state->buttons[index].area);
	}
}

static void set_style(toolbar_state *state, DWORD style)
{
	BOOL wrap_changed = ((state->style ^ style) & TBSTYLE_WRAPABLE) != 0;
	BOOL was_wrapable = (state->style & TBSTYLE_WRAPABLE) != 0;
	state->style = style;
	if (wrap_changed && was_wrapable) {
		int index;
		for (index = 0; index < state->count; ++index)
			state->buttons[index].state &= ~TBSTATE_WRAP;
	}
	if (wrap_changed)
		recalc(state, FALSE);
	else
		relayout(state);
}

static void default_font(toolbar_state *state)
{
	LOGFONTW description;
	if (SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(description), &description, 0)) {
		state->own_font = CreateFontIndirectW(&description);
		state->font = state->own_font;
	}
}

static LRESULT button_flag_query(const toolbar_state *state, int command, BYTE flag)
{
	int index = index_of_command(state, command);
	return index < 0 ? -1 : (state->buttons[index].state & flag) != 0;
}

static LRESULT change_button_flag(toolbar_state *state, int command, BYTE flag, BOOL set)
{
	int index = index_of_command(state, command);
	if (index < 0)
		return FALSE;
	if (set)
		state->buttons[index].state |= flag;
	else
		state->buttons[index].state &= ~flag;
	InvalidateRect(state->window, NULL, TRUE);
	return TRUE;
}

static LRESULT CALLBACK toolbar_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	toolbar_state *state = state_of(window);
	BOOL wide = TRUE;
	if (!state && message != WM_NCCREATE)
		return DefWindowProcW(window, message, wparam, lparam);
	switch (message) {
	case WM_NCCREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state = allocate(sizeof(*state));
		if (!state)
			return FALSE;
		state->window = window;
		state->parent = create->hwndParent;
		state->style = create->style;
		if (state->style & TBSTYLE_FLAT)
			state->style |= TBSTYLE_TRANSPARENT;
		if (!(state->style & (CCS_TOP | CCS_NOMOVEY | CCS_BOTTOM)))
			state->style |= CCS_TOP;
		if (state->style != create->style)
			SetWindowLongPtrW(window, GWL_STYLE, state->style);
		state->bitmap_width = state->bitmap_height = DEFAULT_BITMAP_SIZE;
		state->padding_x = DEFAULT_PADDING_X;
		state->padding_y = DEFAULT_PADDING_Y;
		state->button_width = state->bitmap_width + state->padding_x;
		state->button_height = state->bitmap_height + state->padding_y;
		state->top_margin = (state->style & TBSTYLE_FLAT) ? 0 : TOP_MARGIN;
		state->hot = state->pressed = -1;
		state->rows = 1;
		state->text_rows = 1;
		state->struct_size = sizeof(TBBUTTON);
		state->mark.iButton = -1;
		state->mark_color = GetSysColor(COLOR_BTNTEXT);
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_CREATE:
		state->unicode = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_UNICODE;
		state->theme = OpenThemeData(window, L"Toolbar");
		default_font(state);
		if (state->style & TBSTYLE_TOOLTIPS)
			state->tooltip = CreateWindowExW(0, TOOLTIPS_CLASSW, NULL, WS_POPUP, 0, 0, 0, 0, window, NULL, NULL, NULL);
		autosize(state);
		return 0;
	case WM_DESTROY:
		CloseThemeData(state->theme);
		state->theme = NULL;
		return 0;
	case WM_NCDESTROY: {
		int index;
		for (index = 0; index < state->count; ++index)
			release(state->buttons[index].text);
		release(state->buttons);
		for (index = 0; index < state->string_count; ++index)
			release(state->strings[index]);
		release(state->strings);
		for (index = 0; index < state->block_count; ++index)
			DeleteObject(state->blocks[index].bitmap);
		release(state->blocks);
		if (state->list_is_ours && state->lists[0])
			ImageList_Destroy(state->lists[0]);
		if (state->own_font)
			DeleteObject(state->own_font);
		release(state);
		SetWindowLongPtrW(window, 0, 0);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_THEMECHANGED:
		CloseThemeData(state->theme);
		state->theme = OpenThemeData(window, L"Toolbar");
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_NOTIFYFORMAT:
		if (lparam == NF_REQUERY)
			state->unicode = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_UNICODE;
		return state->unicode ? NFR_UNICODE : NFR_ANSI;
	case WM_NOTIFY: {
		NMHDR *header = (NMHDR *)lparam;
		if (!header)
			return 0;
		if (header->code == TTN_GETDISPINFOW)
			return answer_tooltip(state, (NMTTDISPINFOW *)header);
		return forward_notify(state, header);
	}
	case WM_STYLECHANGED:
		if (wparam == (WPARAM)GWL_STYLE)
			set_style(state, ((const STYLESTRUCT *)lparam)->styleNew);
		return 0;
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		recalc(state, FALSE);
		return 1;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_SIZE:
		relayout(state);
		return 0;
	case WM_ERASEBKGND:
		if (!(state->style & TBSTYLE_TRANSPARENT)) {
			RECT client;
			GetClientRect(window, &client);
			FillRect((HDC)wparam, &client, GetSysColorBrush(COLOR_BTNFACE));
		}
		return 1;
	case WM_PAINT:
	case WM_PRINTCLIENT:
		if (wparam) {
			paint(state, (HDC)wparam);
		} else {
			PAINTSTRUCT painting;
			HDC dc = BeginPaint(window, &painting);
			paint(state, dc);
			EndPaint(window, &painting);
		}
		return 0;
	case WM_MOUSEMOVE: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		int index = hit_test(state, point);
		if (state->tooltip) {
			MSG relayed = {window, message, wparam, lparam};
			SendMessageW(state->tooltip, TTM_RELAYEVENT, 0, (LPARAM)&relayed);
		}
		if (index < 0)
			index = -1;
		if (index != state->hot) {
			TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
			set_hot(state, index, HICF_MOUSE);
			TrackMouseEvent(&track);
		}
		return 0;
	}
	case WM_MOUSELEAVE:
		set_hot(state, -1, HICF_MOUSE);
		return 0;
	case WM_LBUTTONDOWN: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		mouse_down(state, point);
		return 0;
	}
	case WM_LBUTTONUP: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		mouse_up(state, point);
		return 0;
	}
	case WM_CAPTURECHANGED:
		if (state->pressed >= 0) {
			state->buttons[state->pressed].state &= ~TBSTATE_PRESSED;
			state->pressed = -1;
			InvalidateRect(window, NULL, TRUE);
		}
		return 0;
	case WM_RBUTTONUP: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		int index = hit_test(state, point);
		if (!notify_mouse(state, NM_RCLICK, index >= 0 ? index : -1, point))
			return DefWindowProcW(window, message, wparam, lparam);
		return 0;
	}
	case WM_LBUTTONDBLCLK: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		int index = hit_test(state, point);
		notify_mouse(state, NM_DBLCLK, index >= 0 ? index : -1, point);
		return 0;
	}
	case WM_KEYDOWN: {
		NMKEY notice = {0};
		int step = 0;
		notice.nVKey = (UINT)wparam;
		notice.uFlags = HIWORD(lparam);
		if (notify_header(state, &notice.hdr, NM_KEYDOWN))
			return 0;
		if (wparam == VK_LEFT || wparam == VK_UP)
			step = -1;
		else if (wparam == VK_RIGHT || wparam == VK_DOWN)
			step = 1;
		if (step && state->count) {
			int next = state->hot < 0 ? (step > 0 ? 0 : state->count - 1) : state->hot + step, tries;
			for (tries = 0; tries < state->count && next >= 0 && next < state->count; ++tries, next += step) {
				const toolbar_button *button = &state->buttons[next];
				if ((button->state & TBSTATE_ENABLED) && !(button->state & TBSTATE_HIDDEN) && !(button->style & BTNS_SEP)) {
					set_hot(state, next, HICF_ARROWKEYS);
					break;
				}
			}
		} else if ((wparam == VK_RETURN || wparam == VK_SPACE) && state->hot >= 0) {
			SendMessageW(state->parent, WM_COMMAND, MAKEWPARAM(state->buttons[state->hot].command, BN_CLICKED), (LPARAM)window);
		}
		return 0;
	}
	case WM_GETDLGCODE:
		return DLGC_WANTARROWS | DLGC_WANTCHARS;
	case TB_BUTTONSTRUCTSIZE:
		state->struct_size = (int)wparam;
		return 0;
	case TB_ADDBUTTONSA:
		wide = FALSE;
		/* fall through */
	case TB_ADDBUTTONSW:
		return add_buttons(state, (int)wparam, -1, (const void *)lparam, wide);
	case TB_INSERTBUTTONA:
		wide = FALSE;
		/* fall through */
	case TB_INSERTBUTTONW:
		return add_buttons(state, 1, (int)wparam, (const void *)lparam, wide);
	case TB_DELETEBUTTON:
		return delete_button(state, (int)wparam);
	case TB_BUTTONCOUNT:
		return state->count;
	case TB_GETBUTTON: {
		TBBUTTON *out = (TBBUTTON *)lparam;
		const toolbar_button *button;
		if ((int)wparam < 0 || (int)wparam >= state->count || !out)
			return FALSE;
		button = &state->buttons[(int)wparam];
		memset(out, 0, min(state->struct_size, (int)sizeof(*out)));
		out->iBitmap = button->bitmap;
		out->idCommand = button->command;
		out->fsState = button->state;
		out->fsStyle = button->style;
		out->dwData = button->data;
		out->iString = button->string;
		return TRUE;
	}
	case TB_GETITEMRECT:
	case TB_GETRECT: {
		RECT *out = (RECT *)lparam;
		int index = message == TB_GETRECT ? index_of_command(state, (int)wparam) : (int)wparam;
		if (index < 0 || index >= state->count || !out)
			return FALSE;
		*out = state->buttons[index].area;
		return TRUE;
	}
	case TB_COMMANDTOINDEX:
		return index_of_command(state, (int)wparam);
	case TB_ENABLEBUTTON:
		return change_button_flag(state, (int)wparam, TBSTATE_ENABLED, LOWORD(lparam));
	case TB_INDETERMINATE:
		return change_button_flag(state, (int)wparam, TBSTATE_INDETERMINATE, LOWORD(lparam));
	case TB_PRESSBUTTON:
		return change_button_flag(state, (int)wparam, TBSTATE_PRESSED, LOWORD(lparam));
	case TB_MARKBUTTON:
		return change_button_flag(state, (int)wparam, TBSTATE_MARKED, LOWORD(lparam));
	case TB_HIDEBUTTON: {
		LRESULT done = change_button_flag(state, (int)wparam, TBSTATE_HIDDEN, LOWORD(lparam));
		if (done)
			relayout(state);
		return done;
	}
	case TB_CHECKBUTTON: {
		int index = index_of_command(state, (int)wparam);
		if (index < 0)
			return FALSE;
		set_checked(state, index, LOWORD(lparam));
		InvalidateRect(window, NULL, TRUE);
		return TRUE;
	}
	case TB_ISBUTTONENABLED:
		return button_flag_query(state, (int)wparam, TBSTATE_ENABLED);
	case TB_ISBUTTONCHECKED:
		return button_flag_query(state, (int)wparam, TBSTATE_CHECKED);
	case TB_ISBUTTONPRESSED:
		return button_flag_query(state, (int)wparam, TBSTATE_PRESSED);
	case TB_ISBUTTONHIDDEN:
		return button_flag_query(state, (int)wparam, TBSTATE_HIDDEN);
	case TB_ISBUTTONINDETERMINATE:
		return button_flag_query(state, (int)wparam, TBSTATE_INDETERMINATE);
	case TB_ISBUTTONHIGHLIGHTED:
		return button_flag_query(state, (int)wparam, TBSTATE_MARKED);
	case TB_GETSTATE: {
		int index = index_of_command(state, (int)wparam);
		return index < 0 ? -1 : state->buttons[index].state;
	}
	case TB_SETSTATE: {
		int index = index_of_command(state, (int)wparam);
		BYTE previous;
		if (index < 0)
			return FALSE;
		previous = state->buttons[index].state;
		state->buttons[index].state = (BYTE)LOWORD(lparam);
		if ((previous ^ (BYTE)LOWORD(lparam)) & TBSTATE_HIDDEN)
			recalc(state, FALSE);
		else
			InvalidateRect(window, NULL, TRUE);
		return TRUE;
	}
	case TB_MOVEBUTTON: {
		int from = (int)wparam, to = (int)lparam;
		toolbar_button moved;
		if (from < 0 || from >= state->count || to < 0 || to >= state->count)
			return FALSE;
		moved = state->buttons[from];
		if (from < to)
			memmove(&state->buttons[from], &state->buttons[from + 1], (to - from) * sizeof(moved));
		else
			memmove(&state->buttons[to + 1], &state->buttons[to], (from - to) * sizeof(moved));
		state->buttons[to] = moved;
		relayout(state);
		return TRUE;
	}
	case TB_SETCMDID:
		if ((int)wparam < 0 || (int)wparam >= state->count)
			return FALSE;
		state->buttons[(int)wparam].command = (int)lparam;
		return TRUE;
	case TB_GETBITMAP: {
		int index = index_of_command(state, (int)wparam);
		return index < 0 ? -1 : state->buttons[index].bitmap;
	}
	case TB_CHANGEBITMAP: {
		int index = index_of_command(state, (int)wparam);
		if (index < 0)
			return FALSE;
		state->buttons[index].bitmap = (int)lparam;
		InvalidateRect(window, NULL, TRUE);
		return TRUE;
	}
	case TB_GETBITMAPFLAGS:
		return 0;
	case TB_ADDBITMAP:
		return add_bitmap(state, (int)wparam, (const TBADDBITMAP *)lparam);
	case TB_LOADIMAGES: {
		TBADDBITMAP standard;
		standard.hInst = (HINSTANCE)lparam;
		standard.nID = wparam;
		return add_bitmap(state, 0, &standard);
	}
	case TB_REPLACEBITMAP: {
		const TBREPLACEBITMAP *replace = (const TBREPLACEBITMAP *)lparam;
		TBADDBITMAP added;
		if (!replace)
			return FALSE;
		added.hInst = replace->hInstNew;
		added.nID = replace->nIDNew;
		return add_bitmap(state, replace->nButtons, &added) >= 0;
	}
	case TB_SETIMAGELIST: {
		int which = min((int)wparam, IMAGE_LISTS - 1);
		HIMAGELIST previous = state->lists[which];
		state->lists[which] = (HIMAGELIST)lparam;
		if (which == 0) {
			state->list_is_ours = FALSE;
			adopt_image_list(state, (HIMAGELIST)lparam);
		}
		InvalidateRect(window, NULL, TRUE);
		return (LRESULT)previous;
	}
	case TB_GETIMAGELIST:
		return (LRESULT)state->lists[min((int)wparam, IMAGE_LISTS - 1)];
	case TB_SETHOTIMAGELIST: {
		HIMAGELIST previous = state->hot_list;
		state->hot_list = (HIMAGELIST)lparam;
		return (LRESULT)previous;
	}
	case TB_GETHOTIMAGELIST:
		return (LRESULT)state->hot_list;
	case TB_SETDISABLEDIMAGELIST: {
		HIMAGELIST previous = state->disabled_list;
		state->disabled_list = (HIMAGELIST)lparam;
		return (LRESULT)previous;
	}
	case TB_GETDISABLEDIMAGELIST:
		return (LRESULT)state->disabled_list;
	case TB_SETPRESSEDIMAGELIST: {
		HIMAGELIST previous = state->pressed_list;
		state->pressed_list = (HIMAGELIST)lparam;
		return (LRESULT)previous;
	}
	case TB_GETPRESSEDIMAGELIST:
		return (LRESULT)state->pressed_list;
	case TB_SETBITMAPSIZE: {
		int width = (short)LOWORD(lparam), height = (short)HIWORD(lparam);
		if (width != -1)
			state->bitmap_width = max(width, 1);
		if (height != -1)
			state->bitmap_height = max(height, 1);
		rebuild_list(state);
		recalc(state, FALSE);
		return TRUE;
	}
	case TB_SETBUTTONSIZE: {
		int width = (short)LOWORD(lparam), height = (short)HIWORD(lparam);
		state->button_width = max(width, state->bitmap_width + state->padding_x);
		state->button_height = max(height, state->bitmap_height + state->padding_y);
		relayout(state);
		return TRUE;
	}
	case TB_GETBUTTONSIZE:
		return MAKELONG(state->button_width, state->button_height);
	case TB_SETBUTTONWIDTH:
		state->min_button_width = (short)LOWORD(lparam);
		state->max_button_width = (short)HIWORD(lparam);
		relayout(state);
		return TRUE;
	case TB_SETPADDING: {
		LRESULT previous = MAKELONG(state->padding_x, state->padding_y);
		state->padding_x = (short)LOWORD(lparam);
		state->padding_y = (short)HIWORD(lparam);
		return previous;
	}
	case TB_GETPADDING:
		return MAKELONG(state->padding_x, state->padding_y);
	case TB_SETINDENT:
		state->indent = (int)wparam;
		relayout(state);
		return TRUE;
	case TB_SETMAXTEXTROWS:
		state->text_rows = (int)wparam;
		recalc(state, FALSE);
		return TRUE;
	case TB_GETTEXTROWS:
		return state->text_rows;
	case TB_ADDSTRINGA:
		wide = FALSE;
		/* fall through */
	case TB_ADDSTRINGW:
		return add_strings(state, (HINSTANCE)wparam, lparam, wide);
	case TB_GETSTRINGA:
		wide = FALSE;
		/* fall through */
	case TB_GETSTRINGW: {
		int which = HIWORD(wparam), capacity = LOWORD(wparam);
		if (which < 0 || which >= state->string_count)
			return -1;
		return get_text_out(state->strings[which], (void *)lparam, wide ? capacity / (int)sizeof(WCHAR) : capacity, wide);
	}
	case TB_GETBUTTONTEXTA:
		wide = FALSE;
		/* fall through */
	case TB_GETBUTTONTEXTW: {
		int index = index_of_command(state, (int)wparam);
		const WCHAR *text = index >= 0 ? text_of(state, &state->buttons[index]) : NULL;
		if (!text)
			return -1;
		return get_text_out(text, (void *)lparam, 0x7fff, wide);
	}
	case TB_GETBUTTONINFOA:
		wide = FALSE;
		/* fall through */
	case TB_GETBUTTONINFOW: {
		TBBUTTONINFOW *info = (TBBUTTONINFOW *)lparam;
		int index;
		if (!info || info->cbSize != (wide ? sizeof(TBBUTTONINFOW) : sizeof(TBBUTTONINFOA)))
			return -1;
		index = (info->dwMask & TBIF_BYINDEX) ? (int)wparam : index_of_command(state, (int)wparam);
		if (index < 0 || index >= state->count)
			return -1;
		return fill_button_info(state, index, info, wide);
	}
	case TB_SETBUTTONINFOA:
		wide = FALSE;
		/* fall through */
	case TB_SETBUTTONINFOW: {
		const TBBUTTONINFOW *info = (const TBBUTTONINFOW *)lparam;
		int index;
		if (!info || info->cbSize != (wide ? sizeof(TBBUTTONINFOW) : sizeof(TBBUTTONINFOA)))
			return FALSE;
		index = (info->dwMask & TBIF_BYINDEX) ? (int)wparam : index_of_command(state, (int)wparam);
		if (index < 0 || index >= state->count)
			return FALSE;
		return apply_button_info(state, index, info, wide);
	}
	case TB_AUTOSIZE:
		autosize(state);
		return 0;
	case TB_GETROWS:
		return state->rows;
	case TB_SETROWS:
		set_rows(state, LOWORD(wparam), HIWORD(wparam), (RECT *)lparam);
		return 0;
	case TB_GETMAXSIZE: {
		SIZE *out = (SIZE *)lparam;
		int index;
		if (!out)
			return FALSE;
		out->cx = out->cy = 0;
		for (index = 0; index < state->count; ++index) {
			out->cx = max(out->cx, state->buttons[index].area.right);
			out->cy = max(out->cy, state->buttons[index].area.bottom);
		}
		return TRUE;
	}
	case TB_GETIDEALSIZE: {
		SIZE *out = (SIZE *)lparam;
		int index;
		if (!out)
			return FALSE;
		if (wparam) {
			out->cy = 0;
			for (index = 0; index < state->count; ++index)
				out->cy = max(out->cy, state->buttons[index].area.bottom);
		} else {
			out->cx = 0;
			for (index = 0; index < state->count; ++index)
				out->cx = max(out->cx, state->buttons[index].area.right);
		}
		return TRUE;
	}
	case TB_GETSTYLE:
		return state->style;
	case TB_SETSTYLE:
		set_style(state, (DWORD)lparam);
		return 0;
	case TB_GETEXTENDEDSTYLE:
		return state->extended;
	case TB_SETEXTENDEDSTYLE: {
		DWORD previous = state->extended, mask = wparam ? (DWORD)wparam : ~0u;
		state->extended = (previous & ~mask) | ((DWORD)lparam & mask);
		if ((previous ^ state->extended) & TBSTYLE_EX_MIXEDBUTTONS)
			recalc(state, FALSE);
		else
			relayout(state);
		return previous;
	}
	case TB_GETHOTITEM:
		return state->hot;
	case TB_SETHOTITEM: {
		int previous = state->hot;
		set_hot(state, (int)wparam, HICF_OTHER);
		return previous;
	}
	case TB_GETANCHORHIGHLIGHT:
		return 0;
	case TB_SETANCHORHIGHLIGHT:
		return 0;
	case TB_HITTEST:
		return lparam ? hit_test(state, *(const POINT *)lparam) : -1;
	case TB_SETPARENT: {
		HWND previous = state->parent;
		state->parent = (HWND)wparam;
		return (LRESULT)previous;
	}
	case TB_GETTOOLTIPS:
		return (LRESULT)state->tooltip;
	case TB_SETTOOLTIPS:
		state->tooltip = (HWND)wparam;
		return 0;
	case TB_SETDRAWTEXTFLAGS: {
		DWORD previous = state->draw_text_flags;
		state->draw_text_mask = (DWORD)wparam;
		state->draw_text_flags = (previous & ~(DWORD)wparam) | ((DWORD)lparam & (DWORD)wparam);
		return previous;
	}
	case TB_GETINSERTMARK:
		if (lparam)
			*(TBINSERTMARK *)lparam = state->mark;
		return 0;
	case TB_SETINSERTMARK:
		if (lparam)
			state->mark = *(const TBINSERTMARK *)lparam;
		else
			state->mark.iButton = -1;
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case TB_GETINSERTMARKCOLOR:
		return state->mark_color;
	case TB_SETINSERTMARKCOLOR: {
		COLORREF previous = state->mark_color;
		state->mark_color = (COLORREF)lparam;
		InvalidateRect(window, NULL, TRUE);
		return previous;
	}
	case TB_INSERTMARKHITTEST: {
		const POINT *point = (const POINT *)wparam;
		TBINSERTMARK *out = (TBINSERTMARK *)lparam;
		int index;
		if (!point || !out)
			return FALSE;
		for (index = 0; index < state->count; ++index) {
			const RECT *area = &state->buttons[index].area;
			if (PtInRect(area, *point)) {
				out->iButton = index;
				out->dwFlags = point->x >= (area->left + area->right) / 2 ? TBIMHT_AFTER : 0;
				return TRUE;
			}
		}
		out->iButton = state->count ? state->count - 1 : -1;
		out->dwFlags = TBIMHT_AFTER | TBIMHT_BACKGROUND;
		return FALSE;
	}
	case TB_MAPACCELERATORA:
		wide = FALSE;
		/* fall through */
	case TB_MAPACCELERATORW: {
		int index;
		WCHAR wanted = (WCHAR)CharUpperW((WCHAR *)(ULONG_PTR)(wide ? (WCHAR)wparam : (WCHAR)(BYTE)wparam));
		UINT *out = (UINT *)lparam;
		for (index = 0; out && index < state->count; ++index) {
			const WCHAR *text = text_of(state, &state->buttons[index]);
			if (text) {
				const WCHAR *mark = wcschr(text, L'&');
				if (mark && (WCHAR)CharUpperW((WCHAR *)(ULONG_PTR)mark[1]) == wanted) {
					*out = state->buttons[index].command;
					return TRUE;
				}
			}
		}
		return FALSE;
	}
	case TB_SAVERESTOREA: {
		const TBSAVEPARAMSA *given = (const TBSAVEPARAMSA *)lparam;
		TBSAVEPARAMSW params;
		BOOL done;
		if (!given)
			return FALSE;
		params.hkr = given->hkr;
		params.pszSubKey = widen(given->pszSubKey);
		params.pszValueName = widen(given->pszValueName);
		done = wparam ? save_buttons(state, &params) : restore_buttons(state, &params);
		release((void *)params.pszSubKey);
		release((void *)params.pszValueName);
		return done;
	}
	case TB_SAVERESTOREW:
		if (!lparam)
			return FALSE;
		return wparam ? save_buttons(state, (const TBSAVEPARAMSW *)lparam) : restore_buttons(state, (const TBSAVEPARAMSW *)lparam);
	case TB_CUSTOMIZE:
		return 0;
	case TB_SETUNICODEFORMAT: {
		BOOL previous = state->unicode;
		state->unicode = wparam != 0;
		return previous;
	}
	case TB_GETUNICODEFORMAT:
		return state->unicode;
	/* This library is always version 6, whatever is asked for. */
	case CCM_SETVERSION:
	case CCM_GETVERSION:
		return 6;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void TOOLBAR_Register(void)
{
	WNDCLASSW definition = {0};
	definition.style = CS_GLOBALCLASS | CS_DBLCLKS;
	definition.lpfnWndProc = toolbar_procedure;
	definition.cbWndExtra = sizeof(void *);
	definition.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	definition.lpszClassName = TOOLBARCLASSNAMEW;
	RegisterClassW(&definition);
}

void TOOLBAR_Unregister(void)
{
	UnregisterClassW(TOOLBARCLASSNAMEW, NULL);
}

/* The themed build of the library registers the class under this name. */
void TOOLBARv6_Register(void)
{
	TOOLBAR_Register();
}

void TOOLBARv6_Unregister(void)
{
	TOOLBAR_Unregister();
}
