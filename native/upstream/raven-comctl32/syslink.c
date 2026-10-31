/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>

/* The link control. The markup is reduced once to plain text plus a table of which span belongs to which link; layout then
 * cuts that text into positioned pieces, and painting and hit testing only ever walk the pieces. */

#define NO_LINK (-1)
#define FOCUS_MARGIN 1

typedef struct {
	WCHAR id[MAX_LINKID_TEXT];
	WCHAR url[L_MAX_URL_LENGTH];
	UINT state;
	int first, last;
} link_record;

typedef struct {
	int start, length, link;
	RECT area;
} text_piece;

typedef struct {
	HWND window, parent;
	WCHAR *text;
	int length;
	link_record *links;
	int link_count;
	text_piece *pieces;
	int piece_count, piece_capacity;
	HFONT font, underline;
	int line_height, focus_link, press_link, hot_link;
	BOOL focused, tracking;
} link_state;

static link_state *state_of(HWND window)
{
	return (link_state *)GetWindowLongPtrW(window, 0);
}

static DWORD style_of(const link_state *state)
{
	return (DWORD)GetWindowLongPtrW(state->window, GWL_STYLE);
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

static HFONT font_of(const link_state *state)
{
	return state->font ? state->font : (HFONT)GetStockObject(SYSTEM_FONT);
}

static void rebuild_underline(link_state *state)
{
	LOGFONTW description;
	if (state->underline)
		DeleteObject(state->underline);
	state->underline = NULL;
	if (GetObjectW(font_of(state), sizeof(description), &description)) {
		description.lfUnderline = TRUE;
		state->underline = CreateFontIndirectW(&description);
	}
}

/* Markup */

static int same_prefix(const WCHAR *text, const WCHAR *lower)
{
	int length = 0;
	for (; lower[length]; length++) {
		WCHAR symbol = text[length];
		if (symbol >= 'A' && symbol <= 'Z')
			symbol += 'a' - 'A';
		if (symbol != lower[length])
			return 0;
	}
	return length;
}

static const WCHAR *read_attribute(const WCHAR *cursor, WCHAR *target, int capacity)
{
	WCHAR quote;
	int used = 0;
	while (*cursor == ' ' || *cursor == '\t')
		cursor++;
	if (*cursor != '=')
		return cursor;
	cursor++;
	while (*cursor == ' ' || *cursor == '\t')
		cursor++;
	quote = *cursor;
	if (quote == '"' || quote == '\'') {
		cursor++;
		while (*cursor && *cursor != quote) {
			if (used + 1 < capacity)
				target[used++] = *cursor;
			cursor++;
		}
		if (*cursor)
			cursor++;
	}
	else {
		while (*cursor && *cursor != ' ' && *cursor != '\t' && *cursor != '>') {
			if (used + 1 < capacity)
				target[used++] = *cursor;
			cursor++;
		}
	}
	target[used] = 0;
	return cursor;
}

/* Returns the character after an opening tag, or NULL when the text there is not a tag at all. */
static const WCHAR *read_open_tag(const WCHAR *cursor, link_record *link)
{
	int skip = same_prefix(cursor, L"<a");
	if (!skip || (cursor[skip] != '>' && cursor[skip] != ' ' && cursor[skip] != '\t'))
		return NULL;
	cursor += skip;
	for (;;) {
		while (*cursor == ' ' || *cursor == '\t')
			cursor++;
		if (*cursor == '>')
			return cursor + 1;
		if (!*cursor)
			return NULL;
		if ((skip = same_prefix(cursor, L"href")) != 0) {
			cursor = read_attribute(cursor + skip, link->url, L_MAX_URL_LENGTH);
		}
		else if ((skip = same_prefix(cursor, L"id")) != 0) {
			cursor = read_attribute(cursor + skip, link->id, MAX_LINKID_TEXT);
		}
		else {
			while (*cursor && *cursor != ' ' && *cursor != '\t' && *cursor != '>')
				cursor++;
		}
	}
}

static void parse_text(link_state *state, const WCHAR *source)
{
	int capacity = source ? lstrlenW(source) + 1 : 1, links = 0, used = 0;
	const WCHAR *cursor = source ? source : L"";
	BOOL literal_prefix = (style_of(state) & LWS_NOPREFIX) != 0;
	int open = NO_LINK;
	const WCHAR *scan;

	release(state->text);
	release(state->links);
	state->links = NULL;
	state->link_count = 0;
	state->focus_link = state->press_link = state->hot_link = NO_LINK;
	state->text = allocate(capacity * sizeof(WCHAR));
	if (!state->text) {
		state->length = 0;
		return;
	}
	for (scan = cursor; *scan; scan++) {
		if (*scan == '<')
			links++;
	}
	if (links)
		state->links = allocate(links * sizeof(link_record));

	while (*cursor) {
		if (*cursor == '<') {
			int skip;
			if (open == NO_LINK && state->links) {
				link_record candidate;
				const WCHAR *after;
				ZeroMemory(&candidate, sizeof(candidate));
				after = read_open_tag(cursor, &candidate);
				if (after) {
					open = state->link_count++;
					candidate.state = LIS_ENABLED;
					candidate.first = used;
					candidate.last = used;
					state->links[open] = candidate;
					cursor = after;
					continue;
				}
			}
			if (open != NO_LINK && (skip = same_prefix(cursor, L"</a>")) != 0) {
				state->links[open].last = used;
				open = NO_LINK;
				cursor += skip;
				continue;
			}
		}
		if (!literal_prefix && *cursor == '&') {
			cursor++;
			if (!*cursor)
				break;
		}
		state->text[used++] = *cursor++;
	}
	/* A link that is never closed runs to the end of the text. */
	if (open != NO_LINK)
		state->links[open].last = used;
	state->text[used] = 0;
	state->length = used;
}

/* Layout */

static int link_at(const link_state *state, int position)
{
	int index;
	for (index = 0; index < state->link_count; index++) {
		if (position >= state->links[index].first && position < state->links[index].last)
			return index;
	}
	return NO_LINK;
}

static void add_piece(link_state *state, int start, int length, int link, const RECT *area)
{
	text_piece *piece;
	if (state->piece_count == state->piece_capacity) {
		int capacity = state->piece_capacity ? state->piece_capacity * 2 : 16;
		text_piece *grown = allocate(capacity * sizeof(*grown));
		if (!grown)
			return;
		if (state->pieces)
			CopyMemory(grown, state->pieces, state->piece_count * sizeof(*grown));
		release(state->pieces);
		state->pieces = grown;
		state->piece_capacity = capacity;
	}
	piece = &state->pieces[state->piece_count++];
	piece->start = start;
	piece->length = length;
	piece->link = link;
	piece->area = *area;
}

/* Lays the text out in the given width and reports the size used. Words are never split: a word wider than the line takes
 * a line of its own and overflows it. */
static void layout_text(link_state *state, int width, SIZE *used)
{
	HDC dc = GetDC(state->window);
	HGDIOBJ old = SelectObject(dc, font_of(state));
	TEXTMETRICW metrics;
	BOOL ignore_return = (style_of(state) & LWS_IGNORERETURN) != 0;
	int x = 0, y = 0, position = 0, widest = 0;
	BOOL any = FALSE;

	GetTextMetricsW(dc, &metrics);
	state->line_height = metrics.tmHeight;
	state->piece_count = 0;

	while (position < state->length) {
		WCHAR symbol = state->text[position];
		int end = position, link;
		SIZE extent;

		if (symbol == '\n' || symbol == '\r') {
			if (symbol == '\r' && state->text[position + 1] == '\n')
				position++;
			position++;
			if (!ignore_return) {
				x = 0;
				y += state->line_height;
				any = TRUE;
			}
			continue;
		}
		/* A chunk ends at a link boundary, a line break, or after the spaces that follow a word. */
		link = link_at(state, position);
		while (end < state->length && state->text[end] != ' ' && state->text[end] != '\t' && state->text[end] != '\n' && state->text[end] != '\r' && link_at(state, end) == link)
			end++;
		while (end < state->length && (state->text[end] == ' ' || state->text[end] == '\t') && link_at(state, end) == link)
			end++;
		if (end == position)
			end++;
		GetTextExtentPoint32W(dc, state->text + position, end - position, &extent);
		if (x > 0 && x + extent.cx > width) {
			/* Spaces hanging at the end of the line do not push the word over. */
			int visible = end;
			SIZE trimmed;
			while (visible > position + 1 && (state->text[visible - 1] == ' ' || state->text[visible - 1] == '\t'))
				visible--;
			GetTextExtentPoint32W(dc, state->text + position, visible - position, &trimmed);
			if (x + trimmed.cx > width) {
				x = 0;
				y += state->line_height;
			}
		}
		{
			RECT area;
			SetRect(&area, x, y, x + extent.cx, y + state->line_height);
			add_piece(state, position, end - position, link, &area);
		}
		x += extent.cx;
		if (x > widest)
			widest = x;
		any = TRUE;
		position = end;
	}
	SelectObject(dc, old);
	ReleaseDC(state->window, dc);
	used->cx = widest;
	used->cy = any ? y + state->line_height : 0;
	if ((style_of(state) & LWS_RIGHT) && width > 0) {
		int index = 0;
		while (index < state->piece_count) {
			int row = state->pieces[index].area.top, stop = index, shift;
			while (stop < state->piece_count && state->pieces[stop].area.top == row)
				stop++;
			shift = width - state->pieces[stop - 1].area.right;
			if (shift > 0) {
				int walk;
				for (walk = index; walk < stop; walk++)
					OffsetRect(&state->pieces[walk].area, shift, 0);
			}
			index = stop;
		}
	}
}

static void relayout(link_state *state)
{
	RECT client;
	SIZE ignored;
	GetClientRect(state->window, &client);
	layout_text(state, client.right, &ignored);
}

static int link_from_point(const link_state *state, POINT at)
{
	int index;
	for (index = 0; index < state->piece_count; index++) {
		if (state->pieces[index].link != NO_LINK && PtInRect(&state->pieces[index].area, at))
			return state->pieces[index].link;
	}
	return NO_LINK;
}

/* Painting */

static COLORREF link_color(const link_state *state, const link_record *link, COLORREF plain)
{
	if (!IsWindowEnabled(state->window) || !(link->state & LIS_ENABLED))
		return GetSysColor(COLOR_GRAYTEXT);
	if (link->state & LIS_DEFAULTCOLORS)
		return plain;
	if (link->state & LIS_VISITED)
		return GetSysColor(COLOR_HIGHLIGHT);
	return GetSysColor(COLOR_HOTLIGHT);
}

static void draw_contents(link_state *state, HDC dc, const RECT *client)
{
	HBRUSH brush;
	COLORREF plain;
	HGDIOBJ old_font;
	NMCUSTOMDRAW draw;
	int old_mode, index;

	SetTextColor(dc, GetSysColor(IsWindowEnabled(state->window) ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT));
	SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
	brush = (HBRUSH)SendMessageW(state->parent, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)state->window);
	if (!brush)
		brush = GetSysColorBrush(COLOR_BTNFACE);
	if (!(style_of(state) & LWS_TRANSPARENT))
		FillRect(dc, client, brush);
	plain = GetTextColor(dc);
	old_mode = SetBkMode(dc, TRANSPARENT);
	old_font = SelectObject(dc, font_of(state));

	ZeroMemory(&draw, sizeof(draw));
	draw.hdr.hwndFrom = state->window;
	draw.hdr.idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	draw.hdr.code = NM_CUSTOMDRAW;
	draw.dwDrawStage = CDDS_PREPAINT;
	draw.hdc = dc;
	draw.rc = *client;
	SendMessageW(state->parent, WM_NOTIFY, draw.hdr.idFrom, (LPARAM)&draw);

	for (index = 0; index < state->piece_count; index++) {
		const text_piece *piece = &state->pieces[index];
		if (piece->link != NO_LINK) {
			const link_record *link = &state->links[piece->link];
			if (link->state & LIS_ENABLED)
				SelectObject(dc, state->underline ? state->underline : font_of(state));
			else
				SelectObject(dc, font_of(state));
			SetTextColor(dc, link_color(state, link, plain));
		}
		else {
			SelectObject(dc, font_of(state));
			SetTextColor(dc, plain);
		}
		{
			/* The space hanging at the end of a line must not carry the underline past the word. */
			int length = piece->length;
			if (index + 1 == state->piece_count || state->pieces[index + 1].area.top != piece->area.top) {
				while (length > 1 && state->text[piece->start + length - 1] == ' ')
					length--;
			}
			TextOutW(dc, piece->area.left, piece->area.top, state->text + piece->start, length);
		}
	}
	if (state->focused && state->focus_link != NO_LINK) {
		RECT mark;
		BOOL started = FALSE;
		for (index = 0; index < state->piece_count; index++) {
			if (state->pieces[index].link != state->focus_link)
				continue;
			if (!started) {
				mark = state->pieces[index].area;
				started = TRUE;
			}
			else if (state->pieces[index].area.top == mark.top) {
				mark.right = state->pieces[index].area.right;
			}
			else {
				DrawFocusRect(dc, &mark);
				mark = state->pieces[index].area;
			}
		}
		if (started) {
			SetTextColor(dc, RGB(0, 0, 0));
			SetBkColor(dc, RGB(255, 255, 255));
			InflateRect(&mark, 0, FOCUS_MARGIN - 1);
			DrawFocusRect(dc, &mark);
		}
	}
	SelectObject(dc, old_font);
	SetBkMode(dc, old_mode);
}

static void paint(link_state *state)
{
	PAINTSTRUCT info;
	RECT client;
	HDC dc = BeginPaint(state->window, &info);
	GetClientRect(state->window, &client);
	if (dc && !(style_of(state) & LWS_TRANSPARENT) && client.right > 0 && client.bottom > 0) {
		HDC memory = CreateCompatibleDC(dc);
		HBITMAP bitmap = memory ? CreateCompatibleBitmap(dc, client.right, client.bottom) : NULL;
		if (bitmap) {
			HGDIOBJ old = SelectObject(memory, bitmap);
			draw_contents(state, memory, &client);
			BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
			SelectObject(memory, old);
			DeleteObject(bitmap);
		}
		else {
			draw_contents(state, dc, &client);
		}
		if (memory)
			DeleteDC(memory);
	}
	else if (dc) {
		draw_contents(state, dc, &client);
	}
	EndPaint(state->window, &info);
}

/* Items and notifications */

static void fill_item(const link_state *state, int index, LITEM *item)
{
	const link_record *link = &state->links[index];
	item->iLink = index;
	if (item->mask & LIF_STATE)
		item->state = link->state & item->stateMask;
	if (item->mask & LIF_ITEMID)
		lstrcpynW(item->szID, link->id, MAX_LINKID_TEXT);
	if (item->mask & LIF_URL)
		lstrcpynW(item->szUrl, link->url, L_MAX_URL_LENGTH);
}

static int find_item(const link_state *state, const LITEM *item)
{
	int index;
	if (item->mask & LIF_ITEMINDEX)
		return item->iLink >= 0 && item->iLink < state->link_count ? item->iLink : NO_LINK;
	if (item->mask & LIF_ITEMID) {
		for (index = 0; index < state->link_count; index++) {
			if (!lstrcmpW(state->links[index].id, item->szID))
				return index;
		}
	}
	return NO_LINK;
}

static void set_focus_link(link_state *state, int index)
{
	if (state->focus_link != NO_LINK)
		state->links[state->focus_link].state &= ~LIS_FOCUSED;
	state->focus_link = index;
	if (index != NO_LINK)
		state->links[index].state |= LIS_FOCUSED;
	InvalidateRect(state->window, NULL, FALSE);
}

static void activate(link_state *state, int index, UINT code)
{
	NMLINK info;
	if (index == NO_LINK || index >= state->link_count)
		return;
	ZeroMemory(&info, sizeof(info));
	info.hdr.hwndFrom = state->window;
	info.hdr.idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	info.hdr.code = code;
	info.item.mask = LIF_ITEMINDEX | LIF_STATE | LIF_ITEMID | LIF_URL;
	info.item.stateMask = (UINT)~0u;
	fill_item(state, index, &info.item);
	SendMessageW(state->parent, WM_NOTIFY, info.hdr.idFrom, (LPARAM)&info);
}

static int next_enabled(const link_state *state, int from, int step)
{
	int index = from + step;
	for (; index >= 0 && index < state->link_count; index += step) {
		if (state->links[index].state & LIS_ENABLED)
			return index;
	}
	return NO_LINK;
}

static LRESULT ideal_size(link_state *state, WPARAM max_width, SIZE *size)
{
	RECT client;
	SIZE used;
	GetClientRect(state->window, &client);
	layout_text(state, max_width ? (int)max_width : client.right, &used);
	if (size)
		*size = used;
	relayout(state);
	if (!used.cy && size)
		return -1;
	return used.cy;
}

static LRESULT CALLBACK link_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	link_state *state = state_of(window);

	if (message == WM_NCCREATE) {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state = allocate(sizeof(*state));
		if (!state)
			return FALSE;
		state->window = window;
		state->parent = create->hwndParent;
		state->focus_link = state->press_link = state->hot_link = NO_LINK;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	if (!state)
		return DefWindowProcW(window, message, wparam, lparam);

	switch (message) {
	case WM_CREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		rebuild_underline(state);
		parse_text(state, create->lpszName);
		relayout(state);
		return 0;
	}
	case WM_NCDESTROY:
		SetWindowLongPtrW(window, 0, 0);
		release(state->text);
		release(state->links);
		release(state->pieces);
		if (state->underline)
			DeleteObject(state->underline);
		release(state);
		break;
	case WM_SETTEXT: {
		LRESULT result = DefWindowProcW(window, message, wparam, lparam);
		parse_text(state, (const WCHAR *)lparam);
		relayout(state);
		InvalidateRect(window, NULL, TRUE);
		return result;
	}
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		rebuild_underline(state);
		relayout(state);
		if (LOWORD(lparam))
			InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_SIZE:
		relayout(state);
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_ENABLE:
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
		if (wparam) {
			RECT client;
			GetClientRect(window, &client);
			draw_contents(state, (HDC)wparam, &client);
			return 0;
		}
		paint(state);
		return 0;
	case WM_PRINTCLIENT: {
		RECT client;
		GetClientRect(window, &client);
		draw_contents(state, (HDC)wparam, &client);
		return 0;
	}
	case WM_SETCURSOR: {
		POINT at;
		GetCursorPos(&at);
		ScreenToClient(window, &at);
		if (LOWORD(lparam) == HTCLIENT && link_from_point(state, at) != NO_LINK) {
			SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
			return TRUE;
		}
		break;
	}
	case WM_MOUSEMOVE: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		int hot = link_from_point(state, at);
		if (hot != state->hot_link) {
			if (state->hot_link != NO_LINK)
				state->links[state->hot_link].state &= ~LIS_HOTTRACK;
			if (hot != NO_LINK)
				state->links[hot].state |= LIS_HOTTRACK;
			state->hot_link = hot;
		}
		if (!state->tracking) {
			TRACKMOUSEEVENT track;
			track.cbSize = sizeof(track);
			track.dwFlags = TME_LEAVE;
			track.hwndTrack = window;
			track.dwHoverTime = 0;
			state->tracking = TrackMouseEvent(&track);
		}
		return 0;
	}
	case WM_MOUSELEAVE:
		state->tracking = FALSE;
		if (state->hot_link != NO_LINK)
			state->links[state->hot_link].state &= ~LIS_HOTTRACK;
		state->hot_link = NO_LINK;
		return 0;
	case WM_LBUTTONDOWN: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		int index = link_from_point(state, at);
		if (index != NO_LINK && !(state->links[index].state & LIS_ENABLED))
			index = NO_LINK;
		state->press_link = index;
		if (index != NO_LINK) {
			SetFocus(window);
			set_focus_link(state, index);
			SetCapture(window);
		}
		return 0;
	}
	case WM_LBUTTONUP: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		int press = state->press_link;
		state->press_link = NO_LINK;
		if (GetCapture() == window)
			ReleaseCapture();
		if (press != NO_LINK && link_from_point(state, at) == press)
			activate(state, press, NM_CLICK);
		return 0;
	}
	case WM_CAPTURECHANGED:
		state->press_link = NO_LINK;
		return 0;
	case WM_SETFOCUS:
		state->focused = TRUE;
		if (state->focus_link == NO_LINK && state->link_count) {
			BOOL back = GetKeyState(VK_SHIFT) < 0 && GetKeyState(VK_TAB) < 0;
			int first = next_enabled(state, back ? state->link_count : -1, back ? -1 : 1);
			if (first != NO_LINK)
				set_focus_link(state, first);
		}
		InvalidateRect(window, NULL, FALSE);
		return 0;
	case WM_KILLFOCUS:
		state->focused = FALSE;
		InvalidateRect(window, NULL, FALSE);
		return 0;
	case WM_GETDLGCODE: {
		LRESULT code = DLGC_WANTCHARS;
		const MSG *typed = (const MSG *)lparam;
		if (typed && typed->message == WM_KEYDOWN) {
			if (typed->wParam == VK_RETURN && state->focus_link != NO_LINK)
				code |= DLGC_WANTMESSAGE;
			if (typed->wParam == VK_TAB && next_enabled(state, state->focus_link, GetKeyState(VK_SHIFT) < 0 ? -1 : 1) != NO_LINK)
				code |= DLGC_WANTTAB;
		}
		return code;
	}
	case WM_KEYDOWN:
		if (wparam == VK_RETURN || wparam == VK_SPACE) {
			activate(state, state->focus_link, NM_RETURN);
		}
		else if (wparam == VK_TAB) {
			int next = next_enabled(state, state->focus_link, GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
			if (next != NO_LINK)
				set_focus_link(state, next);
		}
		return 0;
	case LM_HITTEST: {
		LHITTESTINFO *info = (LHITTESTINFO *)lparam;
		int index;
		if (!info)
			return FALSE;
		index = link_from_point(state, info->pt);
		if (index == NO_LINK)
			return FALSE;
		info->item.mask = LIF_ITEMINDEX | LIF_STATE | LIF_ITEMID | LIF_URL;
		info->item.stateMask = (UINT)~0u;
		fill_item(state, index, &info->item);
		return TRUE;
	}
	case LM_GETITEM: {
		LITEM *item = (LITEM *)lparam;
		int index;
		if (!item || (index = find_item(state, item)) == NO_LINK)
			return FALSE;
		fill_item(state, index, item);
		return TRUE;
	}
	case LM_SETITEM: {
		const LITEM *item = (const LITEM *)lparam;
		int index;
		if (!item || (index = find_item(state, item)) == NO_LINK)
			return FALSE;
		if (item->mask & LIF_STATE) {
			link_record *link = &state->links[index];
			UINT old_focus = link->state & LIS_FOCUSED;
			link->state = (link->state & ~item->stateMask) | (item->state & item->stateMask);
			if ((link->state & LIS_FOCUSED) && !old_focus)
				set_focus_link(state, index);
			else if (!(link->state & LIS_FOCUSED) && old_focus)
				state->focus_link = NO_LINK;
		}
		if ((item->mask & LIF_ITEMID) && (item->mask & LIF_ITEMINDEX))
			lstrcpynW(state->links[index].id, item->szID, MAX_LINKID_TEXT);
		if (item->mask & LIF_URL)
			lstrcpynW(state->links[index].url, item->szUrl, L_MAX_URL_LENGTH);
		InvalidateRect(window, NULL, FALSE);
		return TRUE;
	}
	/* LM_GETIDEALSIZE is the same message, told apart only by the SIZE pointer. */
	case LM_GETIDEALHEIGHT:
		return ideal_size(state, wparam, (SIZE *)lparam);
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void SYSLINK_Register(void)
{
	WNDCLASSW info;

	ZeroMemory(&info, sizeof(info));
	info.style = CS_GLOBALCLASS | CS_VREDRAW | CS_HREDRAW;
	info.lpfnWndProc = link_proc;
	info.cbWndExtra = sizeof(link_state *);
	info.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	info.lpszClassName = WC_LINK;
	RegisterClassW(&info);
}

void SYSLINK_Unregister(void)
{
	UnregisterClassW(WC_LINK, NULL);
}
