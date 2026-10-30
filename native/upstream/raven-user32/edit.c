/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include "user32_private.h"

/* The edit control, from Microsoft's documentation of its styles, messages and notification order. */

#define DEFAULT_LIMIT 30000
#define SINGLE_LINE_MAX 0x7ffffffe
#define MULTI_LINE_MAX 0xffffffffu
#define DEFAULT_TAB_UNITS 32
#define SCROLL_TIMER 1
#define SCROLL_TIMER_MS 100
#define MENU_SELECT_ALL 0x00b1

#ifndef ES_COMBO
#define ES_COMBO 0x00000200
#endif

typedef struct {
	int start, length, breaks, width;
} edit_line;

typedef struct {
	HWND owner;
	HFONT font;
	HLOCAL handle;
	WCHAR *text;
	UINT length, capacity, limit;
	edit_line *lines;
	int line_count, line_capacity;
	UINT anchor, caret;
	int first_line, x_offset, wanted_x, max_width;
	int line_height, char_width;
	int margin_left, margin_right;
	RECT format;
	BOOL format_set, vertical_bar, horizontal_bar;
	WCHAR password;
	INT *tab_stops;
	int tab_count;
	EDITWORDBREAKPROCW word_break;
	BOOL multiline, modified, focused, captured, dialog_mode, soft_breaks, handle_given;
	WCHAR *undo_text;
	UINT undo_removed, undo_position, undo_inserted;
	BOOL undo_merge;
	BOOL partial_layout;
	UINT layout_from;
	int wheel_rest;
} edit_state;

static edit_state *state_of(HWND window)
{
	return (edit_state *)GetWindowLongPtrW(window, 0);
}

static DWORD style_of(HWND window)
{
	return (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
}

static void notify(HWND window, const edit_state *state, WORD code)
{
	if (state->owner)
		SendMessageW(state->owner, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(window), code), (LPARAM)window);
}

static BOOL is_scrollable_x(HWND window)
{
	return (style_of(window) & ES_AUTOHSCROLL) != 0;
}

/* The buffer is a local allocation, because a multiline control's owner may ask for its handle. */
static BOOL reserve(edit_state *state, UINT characters)
{
	if (characters < state->capacity)
		return TRUE;
	UINT capacity = state->capacity ? state->capacity : 64;
	while (capacity <= characters)
		capacity *= 2;
	HLOCAL handle = state->handle;
	if (handle) {
		LocalUnlock(handle);
		HLOCAL grown = LocalReAlloc(handle, capacity * sizeof(WCHAR), LMEM_MOVEABLE);
		if (!grown) {
			state->text = LocalLock(handle);
			return FALSE;
		}
		handle = grown;
	} else {
		handle = LocalAlloc(LMEM_MOVEABLE | LMEM_ZEROINIT, capacity * sizeof(WCHAR));
		if (!handle)
			return FALSE;
	}
	state->handle = handle;
	state->text = LocalLock(handle);
	state->capacity = capacity;
	return state->text != NULL;
}

/* ---- measuring ---- */

static HDC measuring_dc(HWND window, const edit_state *state, HGDIOBJ *previous)
{
	HDC dc = GetDC(window);
	*previous = dc && state->font ? SelectObject(dc, state->font) : NULL;
	return dc;
}

static void release_measuring_dc(HWND window, HDC dc, HGDIOBJ previous)
{
	if (previous)
		SelectObject(dc, previous);
	ReleaseDC(window, dc);
}

static int next_tab(const edit_state *state, int x)
{
	if (!state->tab_count) {
		int step = MulDiv(DEFAULT_TAB_UNITS, LOWORD(GetDialogBaseUnits()), 4);
		if (step < 1)
			step = 1;
		return (x / step + 1) * step;
	}
	for (int index = 0; index < state->tab_count; ++index)
		if (state->tab_stops[index] > x)
			return state->tab_stops[index];
	/* Past the last stop the interval between the last two continues, or the single value repeats. */
	int last = state->tab_stops[state->tab_count - 1];
	int step = state->tab_count > 1 ? last - state->tab_stops[state->tab_count - 2] : last;
	if (step < 1)
		step = state->char_width;
	return last + ((x - last) / step + 1) * step;
}

static int run_width(HDC dc, const edit_state *state, const WCHAR *text, int count)
{
	SIZE size = {0, 0};
	if (count <= 0)
		return 0;
	if (state->password) {
		GetTextExtentPoint32W(dc, &state->password, 1, &size);
		return size.cx * count;
	}
	GetTextExtentPoint32W(dc, text, count, &size);
	return size.cx;
}

/* How far a run of a line reaches, with x counted from the line's own start so tabs land the same anywhere. */
static int advance(HDC dc, const edit_state *state, const WCHAR *text, int count, int x)
{
	if (state->password)
		return x + run_width(dc, state, text, count);
	int from = 0;
	for (int index = 0; index < count; ++index) {
		if (text[index] != L'\t' || !state->multiline)
			continue;
		x += run_width(dc, state, text + from, index - from);
		x = next_tab(state, x);
		from = index + 1;
	}
	return x + run_width(dc, state, text + from, count - from);
}

static void draw_run(HDC dc, const edit_state *state, const WCHAR *text, int count, int origin, int x, int y)
{
	if (count <= 0)
		return;
	if (state->password) {
		int width = run_width(dc, state, &state->password, 1);
		for (int index = 0; index < count; ++index)
			TextOutW(dc, origin + x + index * width, y, &state->password, 1);
		return;
	}
	int from = 0;
	for (int index = 0; index < count; ++index) {
		if (text[index] != L'\t' || !state->multiline)
			continue;
		if (index > from)
			TextOutW(dc, origin + x, y, text + from, index - from);
		x += run_width(dc, state, text + from, index - from);
		x = next_tab(state, x);
		from = index + 1;
	}
	if (count > from)
		TextOutW(dc, origin + x, y, text + from, count - from);
}

/* ---- layout ---- */

static BOOL add_line(edit_state *state, int start, int length, int breaks, int width)
{
	if (state->line_count == state->line_capacity) {
		int capacity = state->line_capacity ? state->line_capacity * 2 : 16;
		edit_line *lines = state->lines ? HeapReAlloc(GetProcessHeap(), 0, state->lines, capacity * sizeof(*lines))
						: HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(*lines));
		if (!lines)
			return FALSE;
		state->lines = lines;
		state->line_capacity = capacity;
	}
	edit_line *line = &state->lines[state->line_count++];
	line->start = start;
	line->length = length;
	line->breaks = breaks;
	line->width = width;
	if (width > state->max_width)
		state->max_width = width;
	return TRUE;
}

static int break_length(const WCHAR *text, UINT at, UINT length)
{
	if (at >= length)
		return 0;
	if (text[at] == L'\r' && at + 1 < length && text[at + 1] == L'\n')
		return 2;
	if (text[at] == L'\r' && at + 2 < length && text[at + 1] == L'\r' && text[at + 2] == L'\n')
		return 3;
	return text[at] == L'\n' ? 1 : 0;
}

static BOOL is_space(WCHAR character)
{
	return character == L' ' || character == L'\t';
}

/* How many characters of a stretch fit a width, found by halving since tabs rule out a single measure. */
static int fitting(HDC dc, const edit_state *state, const WCHAR *text, int count, int width)
{
	BOOL tabs = FALSE;
	for (int index = 0; index < count && !tabs; ++index)
		tabs = text[index] == L'\t';
	if (!tabs && !state->password) {
		int fit = 0;
		SIZE size;
		if (GetTextExtentExPointW(dc, text, count, width, &fit, NULL, &size))
			return fit > 0 ? fit : 1;
	}
	int low = 1, high = count;
	while (low < high) {
		int middle = low + (high - low + 1) / 2;
		if (advance(dc, state, text, middle, 0) <= width)
			low = middle;
		else
			high = middle - 1;
	}
	return low;
}

static int wrap_point(const edit_state *state, const WCHAR *text, int count, int fit)
{
	if (state->word_break) {
		int found = state->word_break((WCHAR *)text, fit, count, WB_LEFT);
		return found > 0 ? found : fit;
	}
	/* The break stays after the spaces, which hang at the end of the line they follow. */
	for (int at = fit; at > 0; --at)
		if (is_space(text[at - 1]))
			return at;
	return fit;
}

static int line_of(const edit_state *state, UINT index)
{
	int low = 0, high = state->line_count - 1;
	while (low < high) {
		int middle = low + (high - low + 1) / 2;
		if ((UINT)state->lines[middle].start <= index)
			low = middle;
		else
			high = middle - 1;
	}
	return low;
}

static void layout(HWND window, edit_state *state)
{
	HGDIOBJ previous;
	HDC dc = measuring_dc(window, state, &previous);
	if (!dc)
		return;
	/* An edit leaves the lines before it alone, except that the last of them may pull a word back. */
	int keep = 0;
	if (state->partial_layout && state->multiline && state->line_count) {
		keep = line_of(state, state->layout_from) - 1;
		if (keep < 0)
			keep = 0;
	}
	state->partial_layout = FALSE;
	state->line_count = keep;
	state->max_width = 0;
	for (int line = 0; line < keep; ++line)
		if (state->lines[line].width > state->max_width)
			state->max_width = state->lines[line].width;
	if (!state->multiline) {
		add_line(state, 0, (int)state->length, 0, advance(dc, state, state->text, (int)state->length, 0));
		release_measuring_dc(window, dc, previous);
		return;
	}
	BOOL wrap = !is_scrollable_x(window);
	int available = state->format.right - state->format.left;
	UINT at = keep ? (UINT)state->lines[keep].start : 0;
	for (;;) {
		UINT end = at;
		int breaks = 0;
		while (end < state->length && !(breaks = break_length(state->text, end, state->length)))
			++end;
		UINT from = at;
		while (wrap && available > 0 && end > from) {
			int count = (int)(end - from);
			int width = advance(dc, state, state->text + from, count, 0);
			if (width <= available)
				break;
			int fit = fitting(dc, state, state->text + from, count, available);
			int taken = wrap_point(state, state->text + from, count, fit);
			add_line(state, (int)from, taken, 0, advance(dc, state, state->text + from, taken, 0));
			from += taken;
		}
		add_line(state, (int)from, (int)(end - from), breaks, advance(dc, state, state->text + from, (int)(end - from), 0));
		if (end >= state->length)
			break;
		at = end + breaks;
	}
	release_measuring_dc(window, dc, previous);
}

/* Where a line begins on screen, its alignment and the horizontal scroll applied. */
static int line_origin(HWND window, const edit_state *state, int line)
{
	DWORD style = style_of(window);
	int available = state->format.right - state->format.left, width = state->lines[line].width;
	int x = state->format.left - state->x_offset;
	if ((style & ES_RIGHT) && width < available)
		x = state->format.right - width;
	else if ((style & ES_CENTER) && width < available)
		x = state->format.left + (available - width) / 2;
	return x;
}

static int line_y(const edit_state *state, int line)
{
	return state->format.top + (line - state->first_line) * state->line_height;
}

static void position_of(HWND window, const edit_state *state, UINT index, int *x, int *y)
{
	int line = line_of(state, index);
	const edit_line *entry = &state->lines[line];
	HGDIOBJ previous;
	HDC dc = measuring_dc(window, state, &previous);
	int offset = (int)index - entry->start;
	if (offset > entry->length)
		offset = entry->length;
	*x = line_origin(window, state, line) + (dc ? advance(dc, state, state->text + entry->start, offset, 0) : 0);
	*y = line_y(state, line);
	if (dc)
		release_measuring_dc(window, dc, previous);
}

/* The character of a line nearest an x: whichever side of a character's middle it falls decides. */
static UINT character_in_line(HWND window, const edit_state *state, int line, int x)
{
	const edit_line *entry = &state->lines[line];
	const WCHAR *text = state->text + entry->start;
	int relative = x - line_origin(window, state, line);
	HGDIOBJ previous;
	HDC dc = measuring_dc(window, state, &previous);
	if (!dc)
		return (UINT)entry->start;
	int low = 0, high = entry->length;
	while (low < high) {
		int middle = low + (high - low) / 2;
		if (advance(dc, state, text, middle + 1, 0) > relative)
			high = middle;
		else
			low = middle + 1;
	}
	if (low < entry->length) {
		int before = advance(dc, state, text, low, 0), after = advance(dc, state, text, low + 1, 0);
		if (relative >= (before + after + 1) / 2)
			++low;
	}
	release_measuring_dc(window, dc, previous);
	return (UINT)(entry->start + low);
}

static UINT character_at(HWND window, const edit_state *state, int x, int y, int *out_line)
{
	int height = state->line_height > 0 ? state->line_height : 1, distance = y - state->format.top;
	int line = 0;
	if (state->multiline) {
		line = state->first_line + (distance >= 0 ? distance / height : (distance - height + 1) / height);
		if (line < 0)
			line = 0;
		if (line >= state->line_count)
			line = state->line_count - 1;
	}
	if (out_line)
		*out_line = line;
	return character_in_line(window, state, line, x);
}

static int page_lines(const edit_state *state)
{
	int lines = (state->format.bottom - state->format.top) / (state->line_height > 0 ? state->line_height : 1);
	return lines > 0 ? lines : 1;
}

static void update_scroll_bars(HWND window, const edit_state *state)
{
	if (!state->multiline)
		return;
	SCROLLINFO info = {sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
	if (state->vertical_bar) {
		info.nMax = state->line_count > 0 ? state->line_count - 1 : 0;
		info.nPage = page_lines(state);
		info.nPos = state->first_line;
		SetScrollInfo(window, SB_VERT, &info, TRUE);
	}
	if (state->horizontal_bar) {
		info.nMax = state->max_width;
		info.nPage = state->format.right - state->format.left;
		info.nPos = state->x_offset;
		SetScrollInfo(window, SB_HORZ, &info, TRUE);
	}
}

static void place_caret(HWND window, const edit_state *state)
{
	if (!state->focused)
		return;
	int x, y;
	position_of(window, state, state->caret, &x, &y);
	SetCaretPos(x, y);
}

/* The format rectangle follows the client area until the application sets it: borders inset it, height permitting. */
static void default_format(HWND window, edit_state *state)
{
	RECT client;
	GetClientRect(window, &client);
	DWORD style = style_of(window);
	int inset = 0;
	if ((DWORD)GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_CLIENTEDGE)
		inset = 1;
	else if (style & WS_BORDER)
		inset = GetSystemMetrics(SM_CYBORDER) + 1;
	state->format.left = client.left + inset + state->margin_left;
	state->format.right = client.right - inset - state->margin_right;
	if (state->format.right < state->format.left)
		state->format.right = state->format.left;
	state->format.top = client.bottom >= state->line_height + 2 * inset ? inset : 0;
	state->format.bottom = client.bottom - state->format.top;
	if (!state->multiline || state->format.bottom - state->format.top < state->line_height) {
		int height = client.bottom - state->format.top;
		state->format.bottom = state->format.top + (height < state->line_height ? height : state->line_height);
	}
	if (state->multiline)
		state->format.bottom -= (state->format.bottom - state->format.top) % (state->line_height > 0 ? state->line_height : 1);
}

static void scroll_caret_into_view(HWND window, edit_state *state)
{
	int line = line_of(state, state->caret);
	if (state->multiline) {
		int page = page_lines(state);
		if (line < state->first_line)
			state->first_line = line;
		else if (line >= state->first_line + page)
			state->first_line = line - page + 1;
	}
	if (!is_scrollable_x(window) && state->multiline)
		return;
	const edit_line *entry = &state->lines[line];
	HGDIOBJ previous;
	HDC dc = measuring_dc(window, state, &previous);
	if (!dc)
		return;
	int x = advance(dc, state, state->text + entry->start, (int)state->caret - entry->start, 0);
	release_measuring_dc(window, dc, previous);
	int available = state->format.right - state->format.left;
	if (x < state->x_offset)
		state->x_offset = x - available / 3;
	else if (x >= state->x_offset + available)
		state->x_offset = x - available + available / 3;
	if (state->x_offset < 0)
		state->x_offset = 0;
}

static void refresh(HWND window, edit_state *state)
{
	update_scroll_bars(window, state);
	InvalidateRect(window, NULL, TRUE);
	place_caret(window, state);
}

static void sync_with_buffer(HWND window, edit_state *state)
{
	/* The owner of the handle may have written through it. */
	if (!state->handle_given || !state->text)
		return;
	UINT length = lstrlenW(state->text);
	if (length != state->length) {
		state->length = length;
		if (state->anchor > length)
			state->anchor = length;
		if (state->caret > length)
			state->caret = length;
		layout(window, state);
	}
}

/* ---- selection and editing ---- */

static UINT selection_low(const edit_state *state)
{
	return state->anchor < state->caret ? state->anchor : state->caret;
}

static UINT selection_high(const edit_state *state)
{
	return state->anchor < state->caret ? state->caret : state->anchor;
}

static void set_selection(HWND window, edit_state *state, UINT anchor, UINT caret)
{
	if (anchor > state->length)
		anchor = state->length;
	if (caret > state->length)
		caret = state->length;
	if (state->anchor == anchor && state->caret == caret)
		return;
	state->anchor = anchor;
	state->caret = caret;
	state->undo_merge = FALSE;
	InvalidateRect(window, NULL, FALSE);
	place_caret(window, state);
}

static void forget_undo(edit_state *state)
{
	if (state->undo_text)
		HeapFree(GetProcessHeap(), 0, state->undo_text);
	state->undo_text = NULL;
	state->undo_removed = 0;
	state->undo_inserted = 0;
	state->undo_merge = FALSE;
}

/* Replaces a range. The record for undo holds what left and how much came in, and successive typing grows one record. */
static BOOL replace_range(HWND window, edit_state *state, UINT low, UINT high, const WCHAR *insert, UINT count, BOOL record, BOOL merge)
{
	UINT removed = high - low;
	if (!reserve(state, state->length - removed + count))
		return FALSE;
	if (!state->partial_layout || low < state->layout_from)
		state->layout_from = low;
	state->partial_layout = TRUE;
	if (record) {
		BOOL can_merge = merge && state->undo_merge && !removed && low == state->undo_position + state->undo_inserted;
		if (can_merge)
			state->undo_inserted += count;
		else {
			forget_undo(state);
			if (removed) {
				state->undo_text = HeapAlloc(GetProcessHeap(), 0, removed * sizeof(WCHAR));
				if (state->undo_text) {
					CopyMemory(state->undo_text, state->text + low, removed * sizeof(WCHAR));
					state->undo_removed = removed;
				}
			}
			state->undo_position = low;
			state->undo_inserted = count;
		}
		state->undo_merge = merge;
	}
	MoveMemory(state->text + low + count, state->text + high, (state->length - high) * sizeof(WCHAR));
	if (count)
		CopyMemory(state->text + low, insert, count * sizeof(WCHAR));
	state->length = state->length - removed + count;
	state->text[state->length] = 0;
	state->anchor = state->caret = low + count;
	return TRUE;
}

static void after_change(HWND window, edit_state *state)
{
	layout(window, state);
	scroll_caret_into_view(window, state);
	notify(window, state, EN_UPDATE);
	refresh(window, state);
	if (IsWindowVisible(window))
		UpdateWindow(window);
	notify(window, state, EN_CHANGE);
}

/* Does the text still fit where a control without the automatic scroll styles has to show all of it? */
static BOOL fits_without_scrolling(HWND window, const edit_state *state)
{
	if (!state->multiline)
		return is_scrollable_x(window) || state->max_width <= state->format.right - state->format.left;
	return (style_of(window) & ES_AUTOVSCROLL) || state->line_count <= page_lines(state);
}

static void resize_insertion(edit_state *state, UINT low, const WCHAR *text, UINT *kept, UINT wanted, BOOL record)
{
	MoveMemory(state->text + low + wanted, state->text + low + *kept, (state->length - low - *kept + 1) * sizeof(WCHAR));
	if (wanted > *kept)
		CopyMemory(state->text + low + *kept, text + *kept, (wanted - *kept) * sizeof(WCHAR));
	state->length = state->length - *kept + wanted;
	if (record)
		state->undo_inserted = state->undo_inserted - *kept + wanted;
	*kept = wanted;
	state->anchor = state->caret = low + wanted;
	if (!state->partial_layout || low < state->layout_from)
		state->layout_from = low;
	state->partial_layout = TRUE;
}

/* The user's insertion: cut down to the limit and to what the styles can show, reporting what was refused. */
static void insert_text(HWND window, edit_state *state, const WCHAR *text, UINT count, BOOL merge, BOOL record)
{
	if (!state->multiline)
		for (UINT index = 0; index < count; ++index)
			if (text[index] == L'\r' || text[index] == L'\n') {
				count = index;
				break;
			}
	UINT low = selection_low(state), high = selection_high(state);
	UINT room = state->limit > state->length - (high - low) ? state->limit - (state->length - (high - low)) : 0;
	BOOL refused = FALSE;
	if (count > room) {
		count = room;
		refused = TRUE;
	}
	if (!count && low == high) {
		if (refused)
			notify(window, state, EN_MAXTEXT);
		return;
	}
	if (!replace_range(window, state, low, high, text, count, record, merge)) {
		notify(window, state, EN_ERRSPACE);
		return;
	}
	layout(window, state);
	if (count && !fits_without_scrolling(window, state)) {
		/* The longest start of the insertion that the control can still show. */
		UINT fits = 0, too_many = count, kept = count;
		while (too_many - fits > 1) {
			UINT middle = fits + (too_many - fits) / 2;
			resize_insertion(state, low, text, &kept, middle, record);
			layout(window, state);
			if (fits_without_scrolling(window, state))
				fits = middle;
			else
				too_many = middle;
		}
		resize_insertion(state, low, text, &kept, fits, record);
		layout(window, state);
		refused = TRUE;
	}
	state->modified = TRUE;
	after_change(window, state);
	if (refused)
		notify(window, state, EN_MAXTEXT);
}

static void delete_selection(HWND window, edit_state *state)
{
	if (state->anchor == state->caret)
		return;
	replace_range(window, state, selection_low(state), selection_high(state), L"", 0, TRUE, FALSE);
	state->modified = TRUE;
	after_change(window, state);
}

static void delete_range(HWND window, edit_state *state, UINT low, UINT high)
{
	if (low >= high)
		return;
	state->anchor = low;
	state->caret = high;
	delete_selection(window, state);
}

/* Undoing swaps the text with the record, so a second undo brings back what the first took away. */
static BOOL undo(HWND window, edit_state *state)
{
	if (!state->undo_text && !state->undo_inserted)
		return FALSE;
	UINT position = state->undo_position, inserted = state->undo_inserted, removed = state->undo_removed;
	WCHAR *restored = state->undo_text;
	WCHAR *taken = inserted ? HeapAlloc(GetProcessHeap(), 0, inserted * sizeof(WCHAR)) : NULL;
	if (inserted && !taken)
		return FALSE;
	if (inserted)
		CopyMemory(taken, state->text + position, inserted * sizeof(WCHAR));
	state->undo_text = NULL;
	replace_range(window, state, position, position + inserted, restored ? restored : L"", removed, FALSE, FALSE);
	if (restored)
		HeapFree(GetProcessHeap(), 0, restored);
	state->undo_text = taken;
	state->undo_removed = inserted;
	state->undo_inserted = removed;
	state->undo_position = position;
	state->undo_merge = FALSE;
	state->anchor = position;
	state->caret = position + removed;
	state->modified = TRUE;
	after_change(window, state);
	return TRUE;
}

static BOOL copy_selection(HWND window, const edit_state *state)
{
	if (state->password || state->anchor == state->caret)
		return FALSE;
	UINT low = selection_low(state), count = selection_high(state) - low;
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (count + 1) * sizeof(WCHAR));
	if (!memory)
		return FALSE;
	WCHAR *copy = GlobalLock(memory);
	CopyMemory(copy, state->text + low, count * sizeof(WCHAR));
	copy[count] = 0;
	GlobalUnlock(memory);
	if (!OpenClipboard(window)) {
		GlobalFree(memory);
		return FALSE;
	}
	EmptyClipboard();
	SetClipboardData(CF_UNICODETEXT, memory);
	CloseClipboard();
	return TRUE;
}

static void paste(HWND window, edit_state *state)
{
	if (!OpenClipboard(window))
		return;
	HANDLE memory = GetClipboardData(CF_UNICODETEXT);
	const WCHAR *text = memory ? GlobalLock(memory) : NULL;
	if (text) {
		insert_text(window, state, text, lstrlenW(text), FALSE, TRUE);
		GlobalUnlock(memory);
	}
	CloseClipboard();
}

/* ---- word movement ---- */

static BOOL is_delimiter(const edit_state *state, UINT index)
{
	WCHAR character = state->text[index];
	if (state->word_break)
		return state->word_break(state->text, (int)index, (int)state->length, WB_ISDELIMITER);
	return is_space(character) || character == L'\r' || character == L'\n';
}

static UINT word_left(const edit_state *state, UINT index)
{
	if (state->word_break)
		return (UINT)state->word_break(state->text, (int)index, (int)state->length, WB_LEFT);
	while (index > 0 && is_delimiter(state, index - 1))
		--index;
	while (index > 0 && !is_delimiter(state, index - 1))
		--index;
	return index;
}

static UINT word_right(const edit_state *state, UINT index)
{
	if (state->word_break)
		return (UINT)state->word_break(state->text, (int)index, (int)state->length, WB_RIGHT);
	while (index < state->length && !is_delimiter(state, index))
		++index;
	while (index < state->length && is_delimiter(state, index))
		++index;
	return index;
}

/* ---- scrolling ---- */

static int scroll_lines(HWND window, edit_state *state, int delta)
{
	int highest = state->line_count - page_lines(state);
	if (highest < 0)
		highest = 0;
	int target = state->first_line + delta;
	if (target > highest)
		target = highest;
	if (target < 0)
		target = 0;
	int moved = target - state->first_line;
	if (!moved)
		return 0;
	state->first_line = target;
	refresh(window, state);
	return moved;
}

static void set_x_offset(HWND window, edit_state *state, int offset)
{
	int highest = state->max_width - (state->format.right - state->format.left);
	if (offset > highest)
		offset = highest;
	if (offset < 0)
		offset = 0;
	if (offset == state->x_offset)
		return;
	state->x_offset = offset;
	refresh(window, state);
}

static LRESULT vertical_scroll(HWND window, edit_state *state, UINT code, BOOL notifying)
{
	int moved = 0;
	switch (code) {
	case SB_LINEUP:
		moved = scroll_lines(window, state, -1);
		break;
	case SB_LINEDOWN:
		moved = scroll_lines(window, state, 1);
		break;
	case SB_PAGEUP:
		moved = scroll_lines(window, state, -page_lines(state));
		break;
	case SB_PAGEDOWN:
		moved = scroll_lines(window, state, page_lines(state));
		break;
	case SB_TOP:
		moved = scroll_lines(window, state, -state->line_count);
		break;
	case SB_BOTTOM:
		moved = scroll_lines(window, state, state->line_count);
		break;
	case SB_THUMBPOSITION:
	case SB_THUMBTRACK: {
		SCROLLINFO info = {sizeof(info), SIF_TRACKPOS};
		if (GetScrollInfo(window, SB_VERT, &info))
			moved = scroll_lines(window, state, info.nTrackPos - state->first_line);
		break;
	}
	}
	if (moved && notifying)
		notify(window, state, EN_VSCROLL);
	return moved ? MAKELRESULT(moved, TRUE) : 0;
}

static void horizontal_scroll(HWND window, edit_state *state, UINT code)
{
	if (style_of(window) & (ES_CENTER | ES_RIGHT))
		return;
	int before = state->x_offset, page = state->format.right - state->format.left;
	switch (code) {
	case SB_LINELEFT:
		set_x_offset(window, state, state->x_offset - state->char_width);
		break;
	case SB_LINERIGHT:
		set_x_offset(window, state, state->x_offset + state->char_width);
		break;
	case SB_PAGELEFT:
		set_x_offset(window, state, state->x_offset - page);
		break;
	case SB_PAGERIGHT:
		set_x_offset(window, state, state->x_offset + page);
		break;
	case SB_LEFT:
		set_x_offset(window, state, 0);
		break;
	case SB_RIGHT:
		set_x_offset(window, state, state->max_width);
		break;
	case SB_THUMBPOSITION:
	case SB_THUMBTRACK: {
		SCROLLINFO info = {sizeof(info), SIF_TRACKPOS};
		if (GetScrollInfo(window, SB_HORZ, &info))
			set_x_offset(window, state, info.nTrackPos);
		break;
	}
	}
	if (before != state->x_offset)
		notify(window, state, EN_HSCROLL);
}

/* ---- painting ---- */

static void paint_line(HWND window, edit_state *state, HDC dc, int line, HBRUSH highlight, COLORREF text_color, UINT low, UINT high)
{
	const edit_line *entry = &state->lines[line];
	const WCHAR *text = state->text + entry->start;
	int origin = line_origin(window, state, line), y = line_y(state, line);
	int from = (int)low - entry->start, to = (int)high - entry->start;
	if (from < 0)
		from = 0;
	if (to > entry->length)
		to = entry->length;
	if (!highlight || from >= to) {
		SetTextColor(dc, text_color);
		draw_run(dc, state, text, entry->length, origin, 0, y);
		return;
	}
	int start_x = advance(dc, state, text, from, 0), end_x = advance(dc, state, text, to, 0);
	SetTextColor(dc, text_color);
	draw_run(dc, state, text, from, origin, 0, y);
	RECT band = {origin + start_x, y, origin + end_x, y + state->line_height};
	FillRect(dc, &band, highlight);
	SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
	draw_run(dc, state, text + from, to - from, origin, start_x, y);
	SetTextColor(dc, text_color);
	draw_run(dc, state, text + to, entry->length - to, origin, end_x, y);
}

static void paint(HWND window, edit_state *state, HDC dc)
{
	DWORD style = style_of(window);
	RECT client;
	GetClientRect(window, &client);
	HGDIOBJ previous = state->font ? SelectObject(dc, state->font) : NULL;
	/* A control that cannot be typed into is coloured as static text. */
	BOOL enabled = IsWindowEnabled(window);
	UINT color_message = !enabled || (style & ES_READONLY) ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT;
	HBRUSH brush = state->owner ? (HBRUSH)SendMessageW(state->owner, color_message, (WPARAM)dc, (LPARAM)window) : NULL;
	if (!brush)
		brush = (HBRUSH)DefWindowProcW(window, color_message, (WPARAM)dc, (LPARAM)window);
	FillRect(dc, &client, brush);
	COLORREF text_color = enabled ? GetTextColor(dc) : GetSysColor(COLOR_GRAYTEXT);
	int mode = SetBkMode(dc, TRANSPARENT);
	int saved = SaveDC(dc);
	IntersectClipRect(dc, state->format.left, state->format.top, state->format.right, state->format.bottom);
	HBRUSH highlight = state->focused || (style & ES_NOHIDESEL) ? GetSysColorBrush(COLOR_HIGHLIGHT) : NULL;
	UINT low = selection_low(state), high = selection_high(state);
	int last = state->first_line + page_lines(state) + 1;
	for (int line = state->first_line; line < state->line_count && line < last; ++line)
		paint_line(window, state, dc, line, low == high ? NULL : highlight, text_color, low, high);
	RestoreDC(dc, saved);
	SetBkMode(dc, mode);
	if (previous)
		SelectObject(dc, previous);
}

/* ---- input ---- */

static void move_caret(HWND window, edit_state *state, UINT index, BOOL extend)
{
	set_selection(window, state, extend ? state->anchor : index, index);
	scroll_caret_into_view(window, state);
	refresh(window, state);
}

static void move_vertically(HWND window, edit_state *state, int delta, BOOL extend)
{
	int x, y, line = line_of(state, state->caret) + delta;
	position_of(window, state, state->caret, &x, &y);
	if (state->wanted_x < 0)
		state->wanted_x = x;
	if (line < 0)
		line = 0;
	if (line >= state->line_count)
		line = state->line_count - 1;
	UINT index = character_in_line(window, state, line, state->wanted_x);
	int wanted = state->wanted_x;
	move_caret(window, state, index, extend);
	state->wanted_x = wanted;
}

static void step_back(HWND window, edit_state *state, BOOL extend)
{
	if (!extend && state->anchor != state->caret) {
		move_caret(window, state, selection_low(state), FALSE);
		return;
	}
	UINT index = state->caret;
	if (!index)
		return;
	/* A line break is one step. */
	if (index >= 2 && state->text[index - 2] == L'\r' && state->text[index - 1] == L'\n')
		index -= 2;
	else
		--index;
	move_caret(window, state, index, extend);
}

static void step_forward(HWND window, edit_state *state, BOOL extend)
{
	if (!extend && state->anchor != state->caret) {
		move_caret(window, state, selection_high(state), FALSE);
		return;
	}
	UINT index = state->caret;
	if (index >= state->length)
		return;
	int breaks = break_length(state->text, index, state->length);
	move_caret(window, state, index + (breaks ? breaks : 1), extend);
}

static void delete_forward(HWND window, edit_state *state)
{
	if (state->anchor != state->caret) {
		delete_selection(window, state);
		return;
	}
	if (state->caret >= state->length)
		return;
	int breaks = break_length(state->text, state->caret, state->length);
	delete_range(window, state, state->caret, state->caret + (breaks ? breaks : 1));
}

static void delete_backward(HWND window, edit_state *state)
{
	if (state->anchor != state->caret) {
		delete_selection(window, state);
		return;
	}
	UINT index = state->caret;
	if (!index)
		return;
	UINT low = index >= 2 && state->text[index - 2] == L'\r' && state->text[index - 1] == L'\n' ? index - 2 : index - 1;
	delete_range(window, state, low, index);
}

static BOOL combo_wants_key(HWND window, const edit_state *state, WPARAM key)
{
	if (!(style_of(window) & ES_COMBO) || !state->owner)
		return FALSE;
	switch (key) {
	case VK_UP:
	case VK_DOWN:
	case VK_F4:
		return TRUE;
	case VK_RETURN:
	case VK_ESCAPE:
	case VK_PRIOR:
	case VK_NEXT:
		return SendMessageW(state->owner, CB_GETDROPPEDSTATE, 0, 0) != 0;
	}
	return FALSE;
}

static void key_down(HWND window, edit_state *state, WPARAM key)
{
	BOOL shift = GetKeyState(VK_SHIFT) < 0, control = GetKeyState(VK_CONTROL) < 0;
	BOOL read_only = (style_of(window) & ES_READONLY) != 0;
	if (combo_wants_key(window, state, key)) {
		SendMessageW(state->owner, WM_KEYDOWN, key, 0);
		return;
	}
	if (key != VK_UP && key != VK_DOWN && key != VK_PRIOR && key != VK_NEXT)
		state->wanted_x = -1;
	switch (key) {
	case VK_LEFT:
		if (control)
			move_caret(window, state, word_left(state, state->caret), shift);
		else
			step_back(window, state, shift);
		break;
	case VK_RIGHT:
		if (control)
			move_caret(window, state, word_right(state, state->caret), shift);
		else
			step_forward(window, state, shift);
		break;
	case VK_UP:
		if (state->multiline)
			move_vertically(window, state, -1, shift);
		else
			step_back(window, state, shift);
		break;
	case VK_DOWN:
		if (state->multiline)
			move_vertically(window, state, 1, shift);
		else
			step_forward(window, state, shift);
		break;
	case VK_PRIOR:
		if (state->multiline) {
			scroll_lines(window, state, -page_lines(state));
			move_vertically(window, state, -page_lines(state), shift);
		}
		break;
	case VK_NEXT:
		if (state->multiline) {
			scroll_lines(window, state, page_lines(state));
			move_vertically(window, state, page_lines(state), shift);
		}
		break;
	case VK_HOME: {
		const edit_line *entry = &state->lines[line_of(state, state->caret)];
		move_caret(window, state, control ? 0 : (UINT)entry->start, shift);
		break;
	}
	case VK_END: {
		const edit_line *entry = &state->lines[line_of(state, state->caret)];
		move_caret(window, state, control ? state->length : (UINT)(entry->start + entry->length), shift);
		break;
	}
	case VK_DELETE:
		if (read_only)
			break;
		if (shift)
			SendMessageW(window, WM_CUT, 0, 0);
		else if (control && state->anchor == state->caret)
			delete_range(window, state, state->caret, (UINT)(state->lines[line_of(state, state->caret)].start + state->lines[line_of(state, state->caret)].length));
		else
			delete_forward(window, state);
		break;
	case VK_INSERT:
		if (control)
			SendMessageW(window, WM_COPY, 0, 0);
		else if (shift && !read_only)
			SendMessageW(window, WM_PASTE, 0, 0);
		break;
	case VK_RETURN:
		/* Within a dialog, and short of ES_WANTRETURN, Enter belongs to the default button. */
		if (state->dialog_mode && !(state->multiline && (style_of(window) & ES_WANTRETURN))) {
			DWORD def = (DWORD)SendMessageW(state->owner, DM_GETDEFID, 0, 0);
			if (HIWORD(def) == DC_HASDEFID) {
				HWND button = GetDlgItem(state->owner, LOWORD(def));
				if (button) {
					SendMessageW(button, BM_SETSTATE, TRUE, 0);
					PostMessageW(button, BM_CLICK, 0, 0);
					SendMessageW(button, BM_SETSTATE, FALSE, 0);
				}
			}
		}
		break;
	case VK_ESCAPE:
		if (state->multiline && state->dialog_mode && state->owner)
			PostMessageW(state->owner, WM_CLOSE, 0, 0);
		break;
	case VK_TAB:
		if (state->multiline && state->dialog_mode && state->owner)
			SendMessageW(state->owner, WM_NEXTDLGCTL, shift, 0);
		break;
	}
}

static void character(HWND window, edit_state *state, WCHAR typed)
{
	DWORD style = style_of(window);
	BOOL read_only = (style & ES_READONLY) != 0, control = GetKeyState(VK_CONTROL) < 0;
	switch (typed) {
	case 1:
		set_selection(window, state, 0, state->length);
		return;
	case 3:
		SendMessageW(window, WM_COPY, 0, 0);
		return;
	case 22:
		if (!read_only)
			SendMessageW(window, WM_PASTE, 0, 0);
		return;
	case 24:
		if (!read_only)
			SendMessageW(window, WM_CUT, 0, 0);
		return;
	case 26:
		if (!read_only)
			undo(window, state);
		return;
	case VK_BACK:
		if (!read_only)
			delete_backward(window, state);
		return;
	case 127:
		if (!read_only && state->anchor == state->caret)
			delete_range(window, state, word_left(state, state->caret), state->caret);
		return;
	case L'\r':
		if (state->multiline && !read_only && (!state->dialog_mode || (style & ES_WANTRETURN) || control))
			insert_text(window, state, L"\r\n", 2, TRUE, TRUE);
		return;
	case L'\n':
		if (state->multiline && !read_only && control)
			insert_text(window, state, L"\r\n", 2, TRUE, TRUE);
		return;
	case L'\t':
		if (state->multiline && !read_only && !state->dialog_mode)
			insert_text(window, state, L"\t", 1, TRUE, TRUE);
		return;
	}
	if (typed < L' ' || read_only)
		return;
	if ((style & ES_NUMBER) && (typed < L'0' || typed > L'9')) {
		MessageBeep(MB_OK);
		return;
	}
	WCHAR value = typed;
	if (style & ES_LOWERCASE)
		CharLowerBuffW(&value, 1);
	else if (style & ES_UPPERCASE)
		CharUpperBuffW(&value, 1);
	insert_text(window, state, &value, 1, TRUE, TRUE);
}

static void mouse_down(HWND window, edit_state *state, int x, int y, WPARAM keys)
{
	SetFocus(window);
	SetCapture(window);
	state->captured = TRUE;
	state->wanted_x = -1;
	UINT index = character_at(window, state, x, y, NULL);
	set_selection(window, state, keys & MK_SHIFT ? state->anchor : index, index);
	scroll_caret_into_view(window, state);
	refresh(window, state);
	SetTimer(window, SCROLL_TIMER, SCROLL_TIMER_MS, NULL);
}

static void select_word_at(HWND window, edit_state *state, int x, int y, BOOL extend)
{
	UINT index = character_at(window, state, x, y, NULL);
	if (index < state->length && is_delimiter(state, index)) {
		set_selection(window, state, extend ? state->anchor : index, index);
		return;
	}
	UINT low = index, high = index;
	while (low > 0 && !is_delimiter(state, low - 1))
		--low;
	while (high < state->length && !is_delimiter(state, high))
		++high;
	set_selection(window, state, extend ? state->anchor : low, high);
}

static void context_menu(HWND window, edit_state *state, LPARAM lparam)
{
	HMENU menu = CreatePopupMenu();
	if (!menu)
		return;
	DWORD style = style_of(window);
	BOOL read_only = (style & ES_READONLY) != 0, selected = state->anchor != state->caret;
	AppendMenuW(menu, MF_STRING | (state->undo_text || state->undo_inserted ? 0 : MF_GRAYED), WM_UNDO, L"&Undo");
	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(menu, MF_STRING | (selected && !read_only && !state->password ? 0 : MF_GRAYED), WM_CUT, L"Cu&t");
	AppendMenuW(menu, MF_STRING | (selected && !state->password ? 0 : MF_GRAYED), WM_COPY, L"&Copy");
	AppendMenuW(menu, MF_STRING | (!read_only && IsClipboardFormatAvailable(CF_UNICODETEXT) ? 0 : MF_GRAYED), WM_PASTE, L"&Paste");
	AppendMenuW(menu, MF_STRING | (selected && !read_only ? 0 : MF_GRAYED), WM_CLEAR, L"&Delete");
	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(menu, MF_STRING | (state->length ? 0 : MF_GRAYED), MENU_SELECT_ALL, L"Select &All");
	POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
	if (lparam == -1) {
		point.x = 0;
		point.y = 0;
		ClientToScreen(window, &point);
	}
	SetFocus(window);
	UINT chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, 0, window, NULL);
	DestroyMenu(menu);
	if (chosen == MENU_SELECT_ALL)
		set_selection(window, state, 0, state->length);
	else if (chosen)
		SendMessageW(window, chosen, 0, 0);
}

/* ---- metrics ---- */

static void apply_font(HWND window, edit_state *state)
{
	TEXTMETRICW metrics;
	HGDIOBJ previous;
	HDC dc = measuring_dc(window, state, &previous);
	if (!dc)
		return;
	GetTextMetricsW(dc, &metrics);
	state->line_height = metrics.tmHeight > 0 ? metrics.tmHeight : 16;
	state->char_width = metrics.tmAveCharWidth > 0 ? metrics.tmAveCharWidth : 8;
	release_measuring_dc(window, dc, previous);
}

/* By default the margins only leave room for how far the font's characters overhang their cells. */
static int font_overhang(HWND window, const edit_state *state)
{
	HGDIOBJ previous;
	HDC dc = measuring_dc(window, state, &previous);
	if (!dc)
		return 0;
	ABC widths[95];
	int overhang = 0;
	if (GetCharABCWidthsW(dc, 32, 126, widths))
		for (int index = 0; index < 95; ++index) {
			if (-widths[index].abcA > overhang)
				overhang = -widths[index].abcA;
			if (-widths[index].abcC > overhang)
				overhang = -widths[index].abcC;
		}
	release_measuring_dc(window, dc, previous);
	return overhang;
}

static void set_tab_stops(edit_state *state, int count, const INT *stops)
{
	if (state->tab_stops)
		HeapFree(GetProcessHeap(), 0, state->tab_stops);
	state->tab_stops = NULL;
	state->tab_count = 0;
	if (!count)
		return;
	state->tab_stops = HeapAlloc(GetProcessHeap(), 0, count * sizeof(*state->tab_stops));
	if (!state->tab_stops)
		return;
	int base = LOWORD(GetDialogBaseUnits());
	for (int index = 0; index < count; ++index)
		state->tab_stops[index] = MulDiv(stops[index], base, 4);
	state->tab_count = count;
}

static void reflow(HWND window, edit_state *state)
{
	if (!state->format_set)
		default_format(window, state);
	layout(window, state);
	if (state->first_line >= state->line_count)
		state->first_line = state->line_count - 1;
	scroll_caret_into_view(window, state);
	refresh(window, state);
}

static BOOL set_format_rect(HWND window, edit_state *state, const RECT *rect, BOOL redraw)
{
	if (!state->multiline)
		return FALSE;
	/* A rectangle too small to hold a line or two characters does not take effect. */
	if (rect && (rect->right - rect->left < 2 * state->char_width || rect->bottom - rect->top < state->line_height)) {
		if (redraw)
			InvalidateRect(window, NULL, TRUE);
		return TRUE;
	}
	if (rect) {
		state->format = *rect;
		state->format_set = TRUE;
	} else
		state->format_set = FALSE;
	reflow(window, state);
	if (!redraw)
		ValidateRect(window, NULL);
	return TRUE;
}

static void set_whole_text(HWND window, edit_state *state, const WCHAR *text)
{
	UINT count = text ? lstrlenW(text) : 0;
	if (!state->multiline)
		for (UINT index = 0; index < count; ++index)
			if (text[index] == L'\r' || text[index] == L'\n') {
				count = index;
				break;
			}
	forget_undo(state);
	if (!reserve(state, count)) {
		notify(window, state, EN_ERRSPACE);
		return;
	}
	if (count)
		CopyMemory(state->text, text, count * sizeof(WCHAR));
	state->text[count] = 0;
	state->length = count;
	state->anchor = state->caret = 0;
	state->first_line = 0;
	state->x_offset = 0;
	state->modified = FALSE;
	after_change(window, state);
}

/* The text with the soft breaks that EM_FMTLINES asks to show, written into `out` when there is one. */
static UINT copy_text(const edit_state *state, WCHAR *out, UINT capacity)
{
	UINT written = 0;
	for (int line = 0; line < state->line_count; ++line) {
		const edit_line *entry = &state->lines[line];
		UINT count = (UINT)entry->length + (UINT)entry->breaks;
		BOOL soft = state->soft_breaks && !entry->breaks && line + 1 < state->line_count;
		for (UINT index = 0; index < count; ++index) {
			if (out && written + 1 >= capacity)
				return written;
			if (out)
				out[written] = state->text[entry->start + index];
			++written;
		}
		if (soft) {
			static const WCHAR break_text[] = L"\r\r\n";
			for (int index = 0; index < 3; ++index) {
				if (out && written + 1 >= capacity)
					return written;
				if (out)
					out[written] = break_text[index];
				++written;
			}
		}
	}
	return written;
}

LRESULT WINAPI RavenEditProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	edit_state *state = state_of(window);
	if (!state && message != WM_NCCREATE)
		return DefWindowProcW(window, message, wparam, lparam);
	if (state)
		sync_with_buffer(window, state);
	switch (message) {
	case WM_NCCREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
		if (!state)
			return FALSE;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		state->owner = create->hwndParent;
		state->limit = DEFAULT_LIMIT;
		state->wanted_x = -1;
		DWORD style = create->style;
		state->multiline = (style & ES_MULTILINE) != 0;
		state->vertical_bar = (style & WS_VSCROLL) != 0;
		state->horizontal_bar = (style & WS_HSCROLL) != 0;
		if (state->multiline) {
			/* Multiline controls settle on some combinations of the styles at creation. */
			style &= ~(DWORD)ES_PASSWORD;
			if (style & WS_VSCROLL)
				style |= ES_AUTOVSCROLL;
			if (style & WS_HSCROLL)
				style |= ES_AUTOHSCROLL;
			if (style & (ES_CENTER | ES_RIGHT))
				style &= ~(DWORD)ES_AUTOHSCROLL;
		} else {
			if (style & ES_PASSWORD)
				state->password = L'*';
		}
		SetWindowLongPtrW(window, GWL_STYLE, style);
		if (!reserve(state, 0))
			return FALSE;
		state->text[0] = 0;
		apply_font(window, state);
		default_format(window, state);
		layout(window, state);
		return DefWindowProcW(window, message, wparam, (LPARAM)&(CREATESTRUCTW){.lpszName = NULL});
	}
	case WM_CREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		if (create->lpszName && !(IS_INTRESOURCE(create->lpszName))) {
			UINT count = lstrlenW(create->lpszName);
			if (!state->multiline)
				for (UINT index = 0; index < count; ++index)
					if (create->lpszName[index] == L'\r' || create->lpszName[index] == L'\n') {
						count = index;
						break;
					}
			if (reserve(state, count)) {
				CopyMemory(state->text, create->lpszName, count * sizeof(WCHAR));
				state->text[count] = 0;
				state->length = count;
			}
		}
		layout(window, state);
		update_scroll_bars(window, state);
		return 0;
	}
	case WM_NCDESTROY:
		forget_undo(state);
		if (state->handle) {
			LocalUnlock(state->handle);
			LocalFree(state->handle);
		}
		if (state->lines)
			HeapFree(GetProcessHeap(), 0, state->lines);
		if (state->tab_stops)
			HeapFree(GetProcessHeap(), 0, state->tab_stops);
		HeapFree(GetProcessHeap(), 0, state);
		SetWindowLongPtrW(window, 0, 0);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_SIZE:
		reflow(window, state);
		return 0;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = wparam ? (HDC)wparam : BeginPaint(window, &ps);
		paint(window, state, dc);
		if (!wparam)
			EndPaint(window, &ps);
		return 0;
	}
	case WM_PRINTCLIENT:
		paint(window, state, (HDC)wparam);
		return 0;
	case WM_ERASEBKGND:
		/* Painting fills everything with the colour the parent chooses, once. */
		return 1;
	case WM_ENABLE:
		InvalidateRect(window, NULL, TRUE);
		return (LRESULT)IsWindowEnabled(window);
	case WM_GETDLGCODE: {
		LRESULT code = DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS;
		if (state->multiline)
			code |= DLGC_WANTALLKEYS;
		if (lparam) {
			const MSG *pending = (const MSG *)lparam;
			/* Being asked with a message in hand means a dialog manager is feeding this control. */
			if (state->multiline)
				state->dialog_mode = TRUE;
			if (pending->message == WM_SYSCHAR && pending->wParam == VK_BACK)
				code |= DLGC_WANTMESSAGE;
			if (pending->message == WM_KEYDOWN && pending->wParam == VK_RETURN && (style_of(window) & ES_COMBO) && SendMessageW(state->owner, CB_GETDROPPEDSTATE, 0, 0))
				code |= DLGC_WANTMESSAGE;
			if (pending->message == WM_KEYDOWN && pending->wParam == VK_ESCAPE && (style_of(window) & ES_COMBO) && SendMessageW(state->owner, CB_GETDROPPEDSTATE, 0, 0))
				code |= DLGC_WANTMESSAGE;
		}
		return code;
	}
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		apply_font(window, state);
		state->margin_left = state->margin_right = font_overhang(window, state);
		state->format_set = FALSE;
		reflow(window, state);
		if (state->focused) {
			DestroyCaret();
			CreateCaret(window, NULL, 1, state->line_height);
			place_caret(window, state);
			ShowCaret(window);
		}
		if (!lparam)
			ValidateRect(window, NULL);
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_SETTEXT:
		set_whole_text(window, state, (const WCHAR *)lparam);
		return TRUE;
	case WM_GETTEXT: {
		if (!wparam)
			return 0;
		UINT written = copy_text(state, (WCHAR *)lparam, (UINT)wparam);
		((WCHAR *)lparam)[written] = 0;
		return written;
	}
	case WM_GETTEXTLENGTH:
		return copy_text(state, NULL, 0);
	case WM_SETFOCUS:
		state->focused = TRUE;
		CreateCaret(window, NULL, 1, state->line_height);
		place_caret(window, state);
		ShowCaret(window);
		InvalidateRect(window, NULL, FALSE);
		notify(window, state, EN_SETFOCUS);
		return 0;
	case WM_KILLFOCUS:
		state->focused = FALSE;
		DestroyCaret();
		InvalidateRect(window, NULL, FALSE);
		notify(window, state, EN_KILLFOCUS);
		return 0;
	case WM_LBUTTONDOWN:
		mouse_down(window, state, (short)LOWORD(lparam), (short)HIWORD(lparam), wparam);
		return 0;
	case WM_LBUTTONDBLCLK:
		SetFocus(window);
		select_word_at(window, state, (short)LOWORD(lparam), (short)HIWORD(lparam), (wparam & MK_SHIFT) != 0);
		return 0;
	case WM_MOUSEMOVE:
		if (state->captured) {
			UINT index = character_at(window, state, (short)LOWORD(lparam), (short)HIWORD(lparam), NULL);
			set_selection(window, state, state->anchor, index);
		}
		return 0;
	case WM_LBUTTONUP:
		if (state->captured) {
			state->captured = FALSE;
			KillTimer(window, SCROLL_TIMER);
			ReleaseCapture();
		}
		return 0;
	case WM_CAPTURECHANGED:
		if (state->captured) {
			state->captured = FALSE;
			KillTimer(window, SCROLL_TIMER);
		}
		return 0;
	case WM_TIMER:
		if (wparam == SCROLL_TIMER && state->captured) {
			/* The drag continues past the edge the mouse has left. */
			POINT point;
			GetCursorPos(&point);
			ScreenToClient(window, &point);
			if (state->multiline) {
				if (point.y < state->format.top)
					scroll_lines(window, state, -1);
				else if (point.y >= state->format.bottom)
					scroll_lines(window, state, 1);
			}
			UINT index = character_at(window, state, point.x, point.y, NULL);
			set_selection(window, state, state->anchor, index);
			scroll_caret_into_view(window, state);
			refresh(window, state);
		}
		return 0;
	case WM_RBUTTONUP:
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_CONTEXTMENU:
		context_menu(window, state, lparam);
		return 0;
	case WM_KEYDOWN:
		key_down(window, state, wparam);
		return 0;
	case WM_CHAR:
		character(window, state, (WCHAR)wparam);
		return 1;
	case WM_SYSKEYDOWN:
		if (wparam == VK_BACK && (lparam & 0x20000000)) {
			undo(window, state);
			return 0;
		}
		if ((wparam == VK_UP || wparam == VK_DOWN) && combo_wants_key(window, state, wparam)) {
			SendMessageW(state->owner, WM_SYSKEYDOWN, wparam, lparam);
			return 0;
		}
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_SYSCHAR:
		if (wparam == VK_BACK)
			return TRUE;
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_VSCROLL:
		vertical_scroll(window, state, LOWORD(wparam), TRUE);
		return 0;
	case WM_HSCROLL:
		horizontal_scroll(window, state, LOWORD(wparam));
		return 0;
	case WM_MOUSEWHEEL: {
		if (!state->multiline)
			return 0;
		UINT lines = 3;
		SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
		state->wheel_rest += (short)HIWORD(wparam);
		int notches = state->wheel_rest / WHEEL_DELTA;
		state->wheel_rest -= notches * WHEEL_DELTA;
		if (notches && scroll_lines(window, state, -notches * (lines == WHEEL_PAGESCROLL ? page_lines(state) : (int)lines)))
			notify(window, state, EN_VSCROLL);
		return 0;
	}
	case WM_COPY:
		return copy_selection(window, state);
	case WM_CUT:
		if (state->anchor != state->caret && copy_selection(window, state))
			delete_selection(window, state);
		return 0;
	case WM_CLEAR:
		delete_selection(window, state);
		return 0;
	case WM_PASTE:
		paste(window, state);
		return 0;
	case WM_UNDO:
	case EM_UNDO:
		return undo(window, state);
	case EM_CANUNDO:
		return state->undo_text != NULL || state->undo_inserted != 0;
	case EM_EMPTYUNDOBUFFER:
		forget_undo(state);
		return 0;
	case EM_GETSEL: {
		UINT low = selection_low(state), high = selection_high(state);
		if (wparam)
			*(DWORD *)wparam = low;
		if (lparam)
			*(DWORD *)lparam = high;
		return low > 0xffff || high > 0xffff ? -1 : MAKELONG(low, high);
	}
	case EM_SETSEL: {
		int start = (int)wparam, end = (int)lparam;
		/* A negative start drops the selection where the caret is; a negative end means the end of the text. */
		if (start < 0)
			set_selection(window, state, state->caret, state->caret);
		else
			set_selection(window, state, (UINT)start, end < 0 ? state->length : (UINT)end);
		return 0;
	}
	case EM_REPLACESEL:
		insert_text(window, state, lparam ? (const WCHAR *)lparam : L"", lparam ? lstrlenW((const WCHAR *)lparam) : 0, FALSE, wparam != 0);
		if (!wparam && !state->undo_text)
			forget_undo(state);
		return 1;
	case EM_GETMODIFY:
		return state->modified;
	case EM_SETMODIFY:
		state->modified = wparam != 0;
		return 0;
	case EM_GETLIMITTEXT:
		return state->limit;
	case EM_SETLIMITTEXT:
		state->limit = wparam ? (UINT)wparam : (state->multiline ? MULTI_LINE_MAX : SINGLE_LINE_MAX);
		return 0;
	case EM_GETLINECOUNT:
		return state->multiline ? state->line_count : 1;
	case EM_LINEINDEX: {
		if (!state->multiline)
			return 0;
		int line = (int)wparam == -1 ? line_of(state, state->caret) : (int)wparam;
		return line < 0 || line >= state->line_count ? -1 : state->lines[line].start;
	}
	case EM_LINEFROMCHAR:
		if (!state->multiline)
			return 0;
		return line_of(state, (int)wparam == -1 ? selection_low(state) : (UINT)wparam > state->length ? state->length : (UINT)wparam);
	case EM_LINELENGTH: {
		if (!state->multiline)
			return state->length;
		if ((int)wparam == -1) {
			/* What remains of the touched lines around the selection. */
			int first = line_of(state, selection_low(state)), last = line_of(state, selection_high(state));
			int before = (int)selection_low(state) - state->lines[first].start;
			int after = state->lines[last].start + state->lines[last].length - (int)selection_high(state);
			return before + after;
		}
		if ((UINT)wparam > state->length)
			return 0;
		return state->lines[line_of(state, (UINT)wparam)].length;
	}
	case EM_GETLINE: {
		int line = state->multiline ? (int)wparam : 0;
		if (line < 0 || line >= state->line_count)
			return 0;
		WCHAR *out = (WCHAR *)lparam;
		int capacity = *(const WORD *)out, count = state->lines[line].length;
		if (count > capacity)
			count = capacity;
		if (count)
			CopyMemory(out, state->text + state->lines[line].start, count * sizeof(WCHAR));
		return count;
	}
	case EM_POSFROMCHAR: {
		UINT index = (UINT)wparam;
		if (index > state->length || (index == state->length && state->length))
			return -1;
		int x, y;
		position_of(window, state, index, &x, &y);
		return MAKELONG((short)x, (short)y);
	}
	case EM_CHARFROMPOS: {
		int line;
		UINT index = character_at(window, state, (short)LOWORD(lparam), (short)HIWORD(lparam), &line);
		return MAKELONG(index, line);
	}
	case EM_GETFIRSTVISIBLELINE:
		if (state->multiline)
			return state->first_line;
		{
			/* A single line reports the first character the scroll leaves in view. */
			UINT index = character_at(window, state, state->format.left, state->format.top, NULL);
			return index;
		}
	case EM_LINESCROLL:
		if (!state->multiline)
			return FALSE;
		if (!(style_of(window) & (ES_CENTER | ES_RIGHT)))
			set_x_offset(window, state, state->x_offset + (int)wparam * state->char_width);
		scroll_lines(window, state, (int)lparam);
		return TRUE;
	case EM_SCROLL:
		return state->multiline ? vertical_scroll(window, state, (UINT)wparam, FALSE) : 0;
	case EM_SCROLLCARET:
		scroll_caret_into_view(window, state);
		refresh(window, state);
		return 1;
	case EM_GETTHUMB:
		return state->first_line;
	case EM_GETHANDLE:
		if (!state->multiline)
			return 0;
		state->handle_given = TRUE;
		return (LRESULT)state->handle;
	case EM_SETHANDLE: {
		if (!state->multiline || !wparam)
			return 0;
		/* The old buffer belongs to whoever takes it back with EM_GETHANDLE, and is not freed here. */
		if (state->handle)
			LocalUnlock(state->handle);
		state->handle = (HLOCAL)wparam;
		state->text = LocalLock(state->handle);
		state->capacity = (UINT)(LocalSize(state->handle) / sizeof(WCHAR));
		state->length = lstrlenW(state->text);
		state->handle_given = TRUE;
		state->anchor = state->caret = 0;
		state->first_line = state->x_offset = 0;
		state->modified = FALSE;
		forget_undo(state);
		reflow(window, state);
		return 0;
	}
	case EM_GETRECT:
		*(RECT *)lparam = state->format;
		return 0;
	case EM_SETRECT:
		return set_format_rect(window, state, (const RECT *)lparam, TRUE);
	case EM_SETRECTNP:
		return set_format_rect(window, state, (const RECT *)lparam, FALSE);
	case EM_GETMARGINS:
		return MAKELONG(state->margin_left, state->margin_right);
	case EM_SETMARGINS:
		if (wparam & EC_LEFTMARGIN)
			state->margin_left = LOWORD(lparam) == EC_USEFONTINFO ? font_overhang(window, state) : LOWORD(lparam);
		if (wparam & EC_RIGHTMARGIN)
			state->margin_right = HIWORD(lparam) == EC_USEFONTINFO ? font_overhang(window, state) : HIWORD(lparam);
		state->format_set = FALSE;
		reflow(window, state);
		return 0;
	case EM_GETPASSWORDCHAR:
		return state->password;
	case EM_SETPASSWORDCHAR: {
		DWORD style = style_of(window);
		state->password = (WCHAR)wparam;
		SetWindowLongPtrW(window, GWL_STYLE, wparam ? style | ES_PASSWORD : style & ~(DWORD)ES_PASSWORD);
		layout(window, state);
		refresh(window, state);
		return 1;
	}
	case EM_SETREADONLY: {
		DWORD style = style_of(window);
		SetWindowLongPtrW(window, GWL_STYLE, wparam ? style | ES_READONLY : style & ~(DWORD)ES_READONLY);
		InvalidateRect(window, NULL, TRUE);
		return 1;
	}
	case EM_SETTABSTOPS:
		if (!state->multiline)
			return FALSE;
		set_tab_stops(state, (int)wparam, (const INT *)lparam);
		layout(window, state);
		return TRUE;
	case EM_FMTLINES:
		if (!state->multiline)
			return wparam != 0;
		state->soft_breaks = wparam != 0;
		return wparam != 0;
	case EM_GETWORDBREAKPROC:
		return (LRESULT)state->word_break;
	case EM_SETWORDBREAKPROC:
		state->word_break = (EDITWORDBREAKPROCW)lparam;
		layout(window, state);
		refresh(window, state);
		return 0;
	case EM_GETIMESTATUS:
	case EM_SETIMESTATUS:
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}
