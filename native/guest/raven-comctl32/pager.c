/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>
#include <stddef.h>

/* The pager keeps its scroll buttons in the non-client area, so the client area is exactly the window onto the child. It
 * also stands between the child and the parent for notifications: children always talk Unicode to it, and an ANSI parent
 * gets them translated by the table at the bottom of this file. */

#define DEFAULT_BUTTON 12
#define MINIMUM_BUTTON 5
#define REPEAT_TIMER 1
#define HOVER_TIMER 2
#define FIRST_DELAY 400
#define NEXT_DELAY 50
#define HOVER_DELAY 100

enum { BUTTON_FIRST, BUTTON_LAST, BUTTON_NONE = -1 };

typedef struct {
	HWND window, parent, child;
	COLORREF background;
	int border, button, position, content;
	BOOL forward_mouse, ansi_parent, tracking;
	int hot, pressed;
	char *reply;
} pager_state;

static pager_state *state_of(HWND window)
{
	return (pager_state *)GetWindowLongPtrW(window, 0);
}

static BOOL is_horizontal(const pager_state *state)
{
	return (GetWindowLongPtrW(state->window, GWL_STYLE) & PGS_HORZ) != 0;
}

static LRESULT notify_parent(const pager_state *state, NMHDR *header, UINT code)
{
	header->hwndFrom = state->window;
	header->idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	header->code = code;
	return SendMessageW(state->parent, WM_NOTIFY, header->idFrom, (LPARAM)header);
}

static int view_extent(const pager_state *state)
{
	RECT client;
	GetClientRect(state->window, &client);
	return is_horizontal(state) ? client.right : client.bottom;
}

static int furthest_position(const pager_state *state)
{
	int range = state->content - view_extent(state);
	return range > 0 ? range : 0;
}

static BOOL button_shown(const pager_state *state, int which)
{
	if (!state->child)
		return FALSE;
	return which == BUTTON_FIRST ? state->position > 0 : state->position < furthest_position(state);
}

static DWORD button_state(const pager_state *state, int which)
{
	if (!button_shown(state, which))
		return PGF_INVISIBLE;
	if (state->pressed == which)
		return PGF_DEPRESSED;
	if (state->hot == which)
		return PGF_HOT;
	return PGF_NORMAL;
}

/* The window rectangle in window coordinates, shrunk by the border. */
static void band_of(const pager_state *state, RECT *band)
{
	GetWindowRect(state->window, band);
	OffsetRect(band, -band->left, -band->top);
	if (GetWindowLongPtrW(state->window, GWL_STYLE) & WS_BORDER)
		InflateRect(band, -GetSystemMetrics(SM_CXBORDER), -GetSystemMetrics(SM_CYBORDER));
}

static void button_rect(const pager_state *state, int which, RECT *area)
{
	band_of(state, area);
	if (is_horizontal(state)) {
		if (which == BUTTON_FIRST)
			area->right = area->left + state->button;
		else
			area->left = area->right - state->button;
	}
	else {
		if (which == BUTTON_FIRST)
			area->bottom = area->top + state->button;
		else
			area->top = area->bottom - state->button;
	}
}

static int button_at(const pager_state *state, POINT screen)
{
	RECT frame, area;
	POINT at;
	int which;
	GetWindowRect(state->window, &frame);
	at.x = screen.x - frame.left;
	at.y = screen.y - frame.top;
	for (which = BUTTON_FIRST; which <= BUTTON_LAST; which++) {
		button_rect(state, which, &area);
		if (button_shown(state, which) && PtInRect(&area, at))
			return which;
	}
	return BUTTON_NONE;
}

/* Asks the parent how big the child wants to be, on the axis that scrolls. */
static void measure_content(pager_state *state, const RECT *view)
{
	NMPGCALCSIZE size;
	ZeroMemory(&size, sizeof(size));
	if (is_horizontal(state)) {
		size.dwFlag = PGF_CALCWIDTH;
		size.iWidth = view->right - view->left;
		notify_parent(state, &size.hdr, PGN_CALCSIZE);
		state->content = size.iWidth;
	}
	else {
		size.dwFlag = PGF_CALCHEIGHT;
		size.iHeight = view->bottom - view->top;
		notify_parent(state, &size.hdr, PGN_CALCSIZE);
		state->content = size.iHeight;
	}
}

static void place_child(pager_state *state)
{
	RECT client;
	if (!state->child)
		return;
	GetClientRect(state->window, &client);
	if (is_horizontal(state)) {
		int width = state->content > client.right ? state->content : client.right;
		SetWindowPos(state->child, 0, -state->position, 0, width, client.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
	}
	else {
		int height = state->content > client.bottom ? state->content : client.bottom;
		SetWindowPos(state->child, 0, 0, -state->position, client.right, height, SWP_NOZORDER | SWP_NOACTIVATE);
	}
}

static void paint_frame(pager_state *state);

static void refresh_frame(pager_state *state)
{
	SetWindowPos(state->window, 0, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	place_child(state);
	paint_frame(state);
}

static void set_position(pager_state *state, int position)
{
	int range;
	if (position < 0)
		position = 0;
	range = furthest_position(state);
	if (state->child && position > range)
		position = range;
	if (position == state->position)
		return;
	state->position = position;
	refresh_frame(state);
	/* The buttons that came and went changed the view, which can move the limit under the position just set. */
	range = furthest_position(state);
	if (state->position > range) {
		state->position = range;
		refresh_frame(state);
	}
}

static void scroll_by_button(pager_state *state, int which)
{
	NMPGSCROLL scroll;
	RECT frame;
	BOOL horizontal = is_horizontal(state);
	int page = view_extent(state);

	ZeroMemory(&scroll, sizeof(scroll));
	scroll.fwKeys = (GetKeyState(VK_SHIFT) < 0 ? PGK_SHIFT : 0) | (GetKeyState(VK_CONTROL) < 0 ? PGK_CONTROL : 0) | (GetKeyState(VK_MENU) < 0 ? PGK_MENU : 0);
	GetClientRect(state->window, &frame);
	scroll.rcParent = frame;
	if (horizontal) {
		scroll.iDir = which == BUTTON_FIRST ? PGF_SCROLLLEFT : PGF_SCROLLRIGHT;
		scroll.iXpos = state->position;
	}
	else {
		scroll.iDir = which == BUTTON_FIRST ? PGF_SCROLLUP : PGF_SCROLLDOWN;
		scroll.iYpos = state->position;
	}
	scroll.iScroll = page;
	notify_parent(state, &scroll.hdr, PGN_SCROLL);
	if (scroll.iScroll > 0)
		set_position(state, state->position + (which == BUTTON_FIRST ? -scroll.iScroll : scroll.iScroll));
}

static void draw_button(const pager_state *state, HDC dc, int which)
{
	static const UINT arrows[2][2] = { { DFCS_SCROLLUP, DFCS_SCROLLDOWN }, { DFCS_SCROLLLEFT, DFCS_SCROLLRIGHT } };
	DWORD look = button_state(state, which);
	RECT area;
	UINT flags;
	if (look == PGF_INVISIBLE)
		return;
	button_rect(state, which, &area);
	flags = arrows[is_horizontal(state)][which] | DFCS_FLAT;
	if (look == PGF_DEPRESSED)
		flags |= DFCS_PUSHED;
	if (look == PGF_HOT)
		flags &= ~DFCS_FLAT;
	DrawFrameControl(dc, &area, DFC_SCROLL, flags);
}

static void paint_frame(pager_state *state)
{
	HDC dc = GetWindowDC(state->window);
	if (!dc)
		return;
	draw_button(state, dc, BUTTON_FIRST);
	draw_button(state, dc, BUTTON_LAST);
	ReleaseDC(state->window, dc);
}

static void set_hot(pager_state *state, int which)
{
	if (state->hot == which)
		return;
	state->hot = which;
	paint_frame(state);
}

static void end_press(pager_state *state)
{
	if (state->pressed == BUTTON_NONE)
		return;
	state->pressed = BUTTON_NONE;
	KillTimer(state->window, REPEAT_TIMER);
	if (GetCapture() == state->window)
		ReleaseCapture();
	paint_frame(state);
}

static void track_leave(pager_state *state, DWORD extra)
{
	TRACKMOUSEEVENT track;
	if (state->tracking)
		return;
	track.cbSize = sizeof(track);
	track.dwFlags = TME_LEAVE | extra;
	track.hwndTrack = state->window;
	track.dwHoverTime = 0;
	state->tracking = TrackMouseEvent(&track);
}

/* Notification translation */

enum text_shape {
	SHAPE_POINTER,	/* the text pointer sits in the structure, which has the same layout for both character sets */
	SHAPE_HEADER_ITEM,	/* the text pointer sits in the HDITEM the structure points to */
	SHAPE_TOOLTIP,
	SHAPE_FORMAT,
	SHAPE_COMBO_END,
	SHAPE_COMBO_DRAG
};

typedef struct {
	UINT wide, narrow;
	enum text_shape shape;
	int text, limit, mask;
	UINT bit;
	BOOL returns_text, empty_for_null;
} notify_rule;

#define NO_FIELD (-1)
#define POINTER_RULE(w, a, type, field, max, mask_field, mask_bit, out, empty) \
	{ w, a, SHAPE_POINTER, offsetof(type, field), max, mask_field, mask_bit, out, empty }

static const notify_rule rules[] = {
	POINTER_RULE(CBEN_GETDISPINFOW, CBEN_GETDISPINFOA, NMCOMBOBOXEXW, ceItem.pszText, offsetof(NMCOMBOBOXEXW, ceItem.cchTextMax), offsetof(NMCOMBOBOXEXW, ceItem.mask), CBEIF_TEXT, TRUE, TRUE),
	POINTER_RULE(CBEN_INSERTITEM, CBEN_INSERTITEM, NMCOMBOBOXEXW, ceItem.pszText, NO_FIELD, offsetof(NMCOMBOBOXEXW, ceItem.mask), CBEIF_TEXT, FALSE, FALSE),
	POINTER_RULE(CBEN_DELETEITEM, CBEN_DELETEITEM, NMCOMBOBOXEXW, ceItem.pszText, NO_FIELD, offsetof(NMCOMBOBOXEXW, ceItem.mask), CBEIF_TEXT, FALSE, FALSE),
	{ CBEN_ENDEDITW, CBEN_ENDEDITA, SHAPE_COMBO_END, 0, NO_FIELD, NO_FIELD, 0, FALSE, FALSE },
	{ CBEN_DRAGBEGINW, CBEN_DRAGBEGINA, SHAPE_COMBO_DRAG, 0, NO_FIELD, NO_FIELD, 0, FALSE, FALSE },
	POINTER_RULE(DTN_USERSTRINGW, DTN_USERSTRINGA, NMDATETIMESTRINGW, pszUserString, NO_FIELD, NO_FIELD, 0, FALSE, FALSE),
	POINTER_RULE(DTN_WMKEYDOWNW, DTN_WMKEYDOWNA, NMDATETIMEWMKEYDOWNW, pszFormat, NO_FIELD, NO_FIELD, 0, FALSE, FALSE),
	POINTER_RULE(DTN_FORMATQUERYW, DTN_FORMATQUERYA, NMDATETIMEFORMATQUERYW, pszFormat, NO_FIELD, NO_FIELD, 0, FALSE, FALSE),
	{ DTN_FORMATW, DTN_FORMATA, SHAPE_FORMAT, 0, NO_FIELD, NO_FIELD, 0, TRUE, FALSE },
	POINTER_RULE(HDN_GETDISPINFOW, HDN_GETDISPINFOA, NMHDDISPINFOW, pszText, offsetof(NMHDDISPINFOW, cchTextMax), offsetof(NMHDDISPINFOW, mask), HDI_TEXT, TRUE, FALSE),
	{ HDN_BEGINTRACKW, HDN_BEGINTRACKA, SHAPE_HEADER_ITEM, 0, NO_FIELD, NO_FIELD, 0, FALSE, FALSE },
	{ HDN_DIVIDERDBLCLICKW, HDN_DIVIDERDBLCLICKA, SHAPE_HEADER_ITEM, 0, NO_FIELD, NO_FIELD, 0, FALSE, FALSE },
	{ HDN_ENDTRACKW, HDN_ENDTRACKA, SHAPE_HEADER_ITEM, 0, NO_FIELD, NO_FIELD, 0, FALSE, FALSE },
	{ HDN_ITEMCHANGEDW, HDN_ITEMCHANGEDA, SHAPE_HEADER_ITEM, 0, NO_FIELD, NO_FIELD, 0, FALSE, FALSE },
	{ HDN_ITEMCHANGINGW, HDN_ITEMCHANGINGA, SHAPE_HEADER_ITEM, 0, NO_FIELD, NO_FIELD, 0, FALSE, FALSE },
	{ HDN_ITEMCLICKW, HDN_ITEMCLICKA, SHAPE_HEADER_ITEM, 0, NO_FIELD, NO_FIELD, 0, FALSE, FALSE },
	{ HDN_ITEMDBLCLICKW, HDN_ITEMDBLCLICKA, SHAPE_HEADER_ITEM, 0, NO_FIELD, NO_FIELD, 0, FALSE, FALSE },
	{ HDN_TRACKW, HDN_TRACKA, SHAPE_HEADER_ITEM, 0, NO_FIELD, NO_FIELD, 0, FALSE, FALSE },
	POINTER_RULE(LVN_BEGINLABELEDITW, LVN_BEGINLABELEDITA, NMLVDISPINFOW, item.pszText, offsetof(NMLVDISPINFOW, item.cchTextMax), offsetof(NMLVDISPINFOW, item.mask), LVIF_TEXT, FALSE, FALSE),
	POINTER_RULE(LVN_ENDLABELEDITW, LVN_ENDLABELEDITA, NMLVDISPINFOW, item.pszText, offsetof(NMLVDISPINFOW, item.cchTextMax), offsetof(NMLVDISPINFOW, item.mask), LVIF_TEXT, FALSE, FALSE),
	POINTER_RULE(LVN_GETDISPINFOW, LVN_GETDISPINFOA, NMLVDISPINFOW, item.pszText, offsetof(NMLVDISPINFOW, item.cchTextMax), offsetof(NMLVDISPINFOW, item.mask), LVIF_TEXT, TRUE, TRUE),
	POINTER_RULE(LVN_SETDISPINFOW, LVN_SETDISPINFOA, NMLVDISPINFOW, item.pszText, offsetof(NMLVDISPINFOW, item.cchTextMax), offsetof(NMLVDISPINFOW, item.mask), LVIF_TEXT, FALSE, FALSE),
	POINTER_RULE(LVN_GETINFOTIPW, LVN_GETINFOTIPA, NMLVGETINFOTIPW, pszText, offsetof(NMLVGETINFOTIPW, cchTextMax), NO_FIELD, 0, TRUE, FALSE),
	POINTER_RULE(LVN_ODFINDITEMW, LVN_ODFINDITEMA, NMLVFINDITEMW, lvfi.psz, NO_FIELD, offsetof(NMLVFINDITEMW, lvfi.flags), LVFI_STRING | LVFI_PARTIAL, FALSE, FALSE),
	POINTER_RULE(LVN_INCREMENTALSEARCHW, LVN_INCREMENTALSEARCHA, NMLVFINDITEMW, lvfi.psz, NO_FIELD, offsetof(NMLVFINDITEMW, lvfi.flags), LVFI_STRING | LVFI_PARTIAL, FALSE, FALSE),
	POINTER_RULE(TBN_GETBUTTONINFOW, TBN_GETBUTTONINFOA, NMTOOLBARW, pszText, offsetof(NMTOOLBARW, cchText), NO_FIELD, 0, TRUE, FALSE),
	POINTER_RULE(TBN_GETDISPINFOW, TBN_GETDISPINFOA, NMTBDISPINFOW, pszText, offsetof(NMTBDISPINFOW, cchText), offsetof(NMTBDISPINFOW, dwMask), TBNF_TEXT, TRUE, FALSE),
	POINTER_RULE(TBN_GETINFOTIPW, TBN_GETINFOTIPA, NMTBGETINFOTIPW, pszText, offsetof(NMTBGETINFOTIPW, cchTextMax), NO_FIELD, 0, TRUE, FALSE),
	{ TTN_GETDISPINFOW, TTN_GETDISPINFOA, SHAPE_TOOLTIP, 0, NO_FIELD, NO_FIELD, 0, TRUE, FALSE },
	POINTER_RULE(TVN_BEGINDRAGW, TVN_BEGINDRAGA, NMTREEVIEWW, itemNew.pszText, offsetof(NMTREEVIEWW, itemNew.cchTextMax), offsetof(NMTREEVIEWW, itemNew.mask), TVIF_TEXT, FALSE, FALSE),
	POINTER_RULE(TVN_BEGINRDRAGW, TVN_BEGINRDRAGA, NMTREEVIEWW, itemNew.pszText, offsetof(NMTREEVIEWW, itemNew.cchTextMax), offsetof(NMTREEVIEWW, itemNew.mask), TVIF_TEXT, FALSE, FALSE),
	POINTER_RULE(TVN_DELETEITEMW, TVN_DELETEITEMA, NMTREEVIEWW, itemOld.pszText, offsetof(NMTREEVIEWW, itemOld.cchTextMax), offsetof(NMTREEVIEWW, itemOld.mask), TVIF_TEXT, FALSE, FALSE),
	POINTER_RULE(TVN_ITEMEXPANDEDW, TVN_ITEMEXPANDEDA, NMTREEVIEWW, itemNew.pszText, offsetof(NMTREEVIEWW, itemNew.cchTextMax), offsetof(NMTREEVIEWW, itemNew.mask), TVIF_TEXT, FALSE, FALSE),
	POINTER_RULE(TVN_ITEMEXPANDINGW, TVN_ITEMEXPANDINGA, NMTREEVIEWW, itemNew.pszText, offsetof(NMTREEVIEWW, itemNew.cchTextMax), offsetof(NMTREEVIEWW, itemNew.mask), TVIF_TEXT, FALSE, FALSE),
	POINTER_RULE(TVN_SELCHANGEDW, TVN_SELCHANGEDA, NMTREEVIEWW, itemNew.pszText, offsetof(NMTREEVIEWW, itemNew.cchTextMax), offsetof(NMTREEVIEWW, itemNew.mask), TVIF_TEXT, FALSE, FALSE),
	POINTER_RULE(TVN_SELCHANGINGW, TVN_SELCHANGINGA, NMTREEVIEWW, itemNew.pszText, offsetof(NMTREEVIEWW, itemNew.cchTextMax), offsetof(NMTREEVIEWW, itemNew.mask), TVIF_TEXT, FALSE, FALSE),
	POINTER_RULE(TVN_BEGINLABELEDITW, TVN_BEGINLABELEDITA, NMTVDISPINFOW, item.pszText, offsetof(NMTVDISPINFOW, item.cchTextMax), offsetof(NMTVDISPINFOW, item.mask), TVIF_TEXT, FALSE, FALSE),
	POINTER_RULE(TVN_ENDLABELEDITW, TVN_ENDLABELEDITA, NMTVDISPINFOW, item.pszText, offsetof(NMTVDISPINFOW, item.cchTextMax), offsetof(NMTVDISPINFOW, item.mask), TVIF_TEXT, FALSE, FALSE),
	POINTER_RULE(TVN_GETDISPINFOW, TVN_GETDISPINFOA, NMTVDISPINFOW, item.pszText, offsetof(NMTVDISPINFOW, item.cchTextMax), offsetof(NMTVDISPINFOW, item.mask), TVIF_TEXT, TRUE, TRUE),
	POINTER_RULE(TVN_SETDISPINFOW, TVN_SETDISPINFOA, NMTVDISPINFOW, item.pszText, offsetof(NMTVDISPINFOW, item.cchTextMax), offsetof(NMTVDISPINFOW, item.mask), TVIF_TEXT, FALSE, FALSE),
	POINTER_RULE(TVN_GETINFOTIPW, TVN_GETINFOTIPA, NMTVGETINFOTIPW, pszText, offsetof(NMTVGETINFOTIPW, cchTextMax), NO_FIELD, 0, TRUE, FALSE),
};

static const notify_rule *rule_for(UINT code)
{
	int index;
	for (index = 0; index < (int)(sizeof(rules) / sizeof(rules[0])); index++) {
		if (rules[index].wide == code)
			return &rules[index];
	}
	return NULL;
}

static char *narrow_copy(const WCHAR *text, int extra)
{
	int size = text ? WideCharToMultiByte(CP_ACP, 0, text, -1, NULL, 0, NULL, NULL) : 1;
	char *copy = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + extra + 1);
	if (copy && text)
		WideCharToMultiByte(CP_ACP, 0, text, -1, copy, size, NULL, NULL);
	return copy;
}

static void widen_into(const char *text, WCHAR *target, int capacity)
{
	if (target && capacity > 0 && !MultiByteToWideChar(CP_ACP, 0, text, -1, target, capacity))
		target[capacity - 1] = 0;
}

static LRESULT forward_pointer(const pager_state *state, const notify_rule *rule, NMHDR *header, WPARAM id)
{
	char *base = (char *)header;
	WCHAR **slot = (WCHAR **)(base + rule->text);
	WCHAR *original = *slot;
	int *limit = rule->limit != NO_FIELD ? (int *)(base + rule->limit) : NULL;
	int capacity = limit ? *limit : 0;
	char *copy;
	LRESULT result;

	if (rule->mask != NO_FIELD && !(*(UINT *)(base + rule->mask) & rule->bit)) {
		header->code = rule->narrow;
		result = SendMessageW(state->parent, WM_NOTIFY, id, (LPARAM)header);
		header->code = rule->wide;
		return result;
	}
	if (!original && !rule->empty_for_null) {
		copy = NULL;
	}
	else {
		copy = narrow_copy(original, capacity > 0 ? capacity * 3 : 0);
		if (!copy)
			return 0;
	}
	*slot = (WCHAR *)copy;
	header->code = rule->narrow;
	result = SendMessageW(state->parent, WM_NOTIFY, id, (LPARAM)header);
	header->code = rule->wide;
	if (rule->returns_text) {
		const char *answer = (const char *)*slot;
		if (answer && original)
			widen_into(answer, original, capacity);
		else if (answer) {
			/* The parent handed back its own string and there is nowhere of the child's to put it, so it lives in the
			 * pager until the next notification. */
			pager_state *owner = state_of(state->window);
			int size = MultiByteToWideChar(CP_ACP, 0, answer, -1, NULL, 0);
			if (owner->reply)
				HeapFree(GetProcessHeap(), 0, owner->reply);
			owner->reply = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size * sizeof(WCHAR) + sizeof(WCHAR));
			if (owner->reply)
				widen_into(answer, (WCHAR *)owner->reply, size);
			original = (WCHAR *)owner->reply;
		}
	}
	*slot = original;
	if (copy)
		HeapFree(GetProcessHeap(), 0, copy);
	return result;
}

static LRESULT forward_header_item(const pager_state *state, const notify_rule *rule, NMHEADERW *info, WPARAM id)
{
	HDITEMW *item = info->pitem;
	WCHAR *original = NULL;
	char *copy = NULL;
	LRESULT result;
	if (item && (item->mask & HDI_TEXT)) {
		original = item->pszText;
		copy = narrow_copy(original, 0);
		item->pszText = (WCHAR *)copy;
	}
	info->hdr.code = rule->narrow;
	result = SendMessageW(state->parent, WM_NOTIFY, id, (LPARAM)info);
	info->hdr.code = rule->wide;
	if (copy) {
		item->pszText = original;
		HeapFree(GetProcessHeap(), 0, copy);
	}
	return result;
}

static LRESULT forward_tooltip(const pager_state *state, const notify_rule *rule, NMTTDISPINFOW *info, WPARAM id)
{
	NMTTDISPINFOA narrow;
	LRESULT result;
	ZeroMemory(&narrow, sizeof(narrow));
	narrow.hdr = info->hdr;
	narrow.hdr.code = rule->narrow;
	narrow.lpszText = narrow.szText;
	narrow.hinst = info->hinst;
	narrow.uFlags = info->uFlags;
	narrow.lParam = info->lParam;
	if (info->szText[0])
		WideCharToMultiByte(CP_ACP, 0, info->szText, -1, narrow.szText, sizeof(narrow.szText), NULL, NULL);
	result = SendMessageW(state->parent, WM_NOTIFY, id, (LPARAM)&narrow);
	info->hinst = narrow.hinst;
	info->uFlags = narrow.uFlags;
	info->lParam = narrow.lParam;
	if (narrow.hinst && IS_INTRESOURCE(narrow.lpszText)) {
		info->lpszText = (WCHAR *)narrow.lpszText;
	}
	else if (narrow.lpszText) {
		widen_into(narrow.lpszText, info->szText, ARRAYSIZE(info->szText));
		info->lpszText = info->szText;
	}
	return result;
}

static LRESULT forward_format(const pager_state *state, const notify_rule *rule, NMDATETIMEFORMATW *info, WPARAM id)
{
	NMDATETIMEFORMATA narrow;
	char *format = narrow_copy(info->pszFormat, 0);
	LRESULT result;
	ZeroMemory(&narrow, sizeof(narrow));
	narrow.nmhdr = info->nmhdr;
	narrow.nmhdr.code = rule->narrow;
	narrow.pszFormat = format;
	narrow.st = info->st;
	narrow.pszDisplay = narrow.szDisplay;
	result = SendMessageW(state->parent, WM_NOTIFY, id, (LPARAM)&narrow);
	if (narrow.pszDisplay) {
		widen_into(narrow.pszDisplay, info->szDisplay, ARRAYSIZE(info->szDisplay));
		info->pszDisplay = info->szDisplay;
	}
	if (format)
		HeapFree(GetProcessHeap(), 0, format);
	return result;
}

static LRESULT forward_combo_end(const pager_state *state, const notify_rule *rule, NMCBEENDEDITW *info, WPARAM id)
{
	NMCBEENDEDITA narrow;
	ZeroMemory(&narrow, sizeof(narrow));
	narrow.hdr = info->hdr;
	narrow.hdr.code = rule->narrow;
	narrow.fChanged = info->fChanged;
	narrow.iNewSelection = info->iNewSelection;
	narrow.iWhy = info->iWhy;
	WideCharToMultiByte(CP_ACP, 0, info->szText, -1, narrow.szText, sizeof(narrow.szText), NULL, NULL);
	return SendMessageW(state->parent, WM_NOTIFY, id, (LPARAM)&narrow);
}

static LRESULT forward_combo_drag(const pager_state *state, const notify_rule *rule, NMCBEDRAGBEGINW *info, WPARAM id)
{
	NMCBEDRAGBEGINA narrow;
	ZeroMemory(&narrow, sizeof(narrow));
	narrow.hdr = info->hdr;
	narrow.hdr.code = rule->narrow;
	narrow.iItemid = info->iItemid;
	WideCharToMultiByte(CP_ACP, 0, info->szText, -1, narrow.szText, sizeof(narrow.szText), NULL, NULL);
	return SendMessageW(state->parent, WM_NOTIFY, id, (LPARAM)&narrow);
}

static LRESULT forward_notify(const pager_state *state, WPARAM id, NMHDR *header)
{
	const notify_rule *rule;
	if (!state->ansi_parent || !header || !(rule = rule_for(header->code)))
		return SendMessageW(state->parent, WM_NOTIFY, id, (LPARAM)header);
	switch (rule->shape) {
	case SHAPE_POINTER:
		return forward_pointer(state, rule, header, id);
	case SHAPE_HEADER_ITEM:
		return forward_header_item(state, rule, (NMHEADERW *)header, id);
	case SHAPE_TOOLTIP:
		return forward_tooltip(state, rule, (NMTTDISPINFOW *)header, id);
	case SHAPE_FORMAT:
		return forward_format(state, rule, (NMDATETIMEFORMATW *)header, id);
	case SHAPE_COMBO_END:
		return forward_combo_end(state, rule, (NMCBEENDEDITW *)header, id);
	case SHAPE_COMBO_DRAG:
		return forward_combo_drag(state, rule, (NMCBEDRAGBEGINW *)header, id);
	}
	return 0;
}

static void query_parent_format(pager_state *state)
{
	LRESULT format = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)state->window, NF_QUERY);
	state->ansi_parent = format == NFR_ANSI;
}

/* Window procedure */

static LRESULT calculate_frame(pager_state *state, NCCALCSIZE_PARAMS *sizes)
{
	RECT *view = &sizes->rgrc[0];
	BOOL horizontal = is_horizontal(state);
	BOOL first, last;
	if (GetWindowLongPtrW(state->window, GWL_STYLE) & WS_BORDER)
		InflateRect(view, -GetSystemMetrics(SM_CXBORDER), -GetSystemMetrics(SM_CYBORDER));
	if (state->child)
		measure_content(state, view);
	first = state->child && state->position > 0;
	{
		int extent = horizontal ? view->right - view->left : view->bottom - view->top;
		if (first)
			extent -= state->button;
		last = state->child && state->position < state->content - extent;
	}
	if (horizontal) {
		if (first)
			view->left += state->button;
		if (last)
			view->right -= state->button;
		if (view->right < view->left)
			view->right = view->left;
	}
	else {
		if (first)
			view->top += state->button;
		if (last)
			view->bottom -= state->button;
		if (view->bottom < view->top)
			view->bottom = view->top;
	}
	return 0;
}

static LRESULT CALLBACK pager_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	pager_state *state = state_of(window);

	if (message == WM_NCCREATE) {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
		if (!state)
			return FALSE;
		state->window = window;
		state->parent = create->hwndParent;
		state->background = GetSysColor(COLOR_BTNFACE);
		state->button = DEFAULT_BUTTON;
		state->hot = state->pressed = BUTTON_NONE;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	if (!state)
		return DefWindowProcW(window, message, wparam, lparam);

	switch (message) {
	case WM_CREATE:
		query_parent_format(state);
		return 0;
	case WM_NCDESTROY:
		SetWindowLongPtrW(window, 0, 0);
		if (state->reply)
			HeapFree(GetProcessHeap(), 0, state->reply);
		HeapFree(GetProcessHeap(), 0, state);
		break;
	case PGM_SETCHILD:
		state->child = (HWND)lparam;
		state->position = 0;
		refresh_frame(state);
		return 0;
	case PGM_RECALCSIZE:
		refresh_frame(state);
		set_position(state, state->position);
		return 0;
	case PGM_FORWARDMOUSE:
		state->forward_mouse = (BOOL)wparam;
		return 0;
	case PGM_GETPOS:
		return state->position;
	case PGM_SETPOS:
		set_position(state, (int)lparam);
		return 0;
	case PGM_GETBKCOLOR:
		return state->background;
	case PGM_SETBKCOLOR: {
		COLORREF old = state->background;
		state->background = (COLORREF)lparam;
		InvalidateRect(window, NULL, TRUE);
		return old;
	}
	case PGM_GETBORDER:
		return state->border;
	case PGM_SETBORDER: {
		int old = state->border;
		state->border = (int)lparam;
		return old;
	}
	case PGM_GETBUTTONSIZE:
		return state->button;
	case PGM_SETBUTTONSIZE: {
		int old = state->button;
		state->button = (int)lparam < MINIMUM_BUTTON ? MINIMUM_BUTTON : (int)lparam;
		refresh_frame(state);
		return old;
	}
	case PGM_GETBUTTONSTATE:
		return button_state(state, lparam == PGB_TOPORLEFT ? BUTTON_FIRST : BUTTON_LAST);
	case PGM_GETDROPTARGET:
		if (lparam)
			*(void **)lparam = NULL;
		return 0;
	case WM_NOTIFYFORMAT:
		if (lparam == NF_REQUERY) {
			query_parent_format(state);
			return state->ansi_parent ? NFR_ANSI : NFR_UNICODE;
		}
		if (lparam == NF_QUERY)
			return NFR_UNICODE;
		return 0;
	case WM_NOTIFY:
		return forward_notify(state, wparam, (NMHDR *)lparam);
	case WM_COMMAND:
		return SendMessageW(state->parent, message, wparam, lparam);
	case WM_NCCALCSIZE:
		if (wparam)
			return calculate_frame(state, (NCCALCSIZE_PARAMS *)lparam);
		break;
	case WM_NCPAINT:
		DefWindowProcW(window, message, wparam, lparam);
		paint_frame(state);
		return 0;
	case WM_NCHITTEST: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		if (button_at(state, at) != BUTTON_NONE)
			return HTCAPTION + 100;
		return DefWindowProcW(window, message, wparam, lparam) == HTNOWHERE ? HTNOWHERE : HTCLIENT;
	}
	case WM_NCLBUTTONDOWN:
	case WM_NCLBUTTONDBLCLK: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		int which = button_at(state, at);
		if (which == BUTTON_NONE || (GetWindowLongPtrW(window, GWL_STYLE) & PGS_AUTOSCROLL))
			return 0;
		state->pressed = which;
		SetCapture(window);
		scroll_by_button(state, which);
		paint_frame(state);
		SetTimer(window, REPEAT_TIMER, FIRST_DELAY, NULL);
		return 0;
	}
	case WM_NCMOUSEMOVE: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		int which = button_at(state, at);
		set_hot(state, which);
		if (which != BUTTON_NONE) {
			track_leave(state, TME_NONCLIENT);
			if (GetWindowLongPtrW(window, GWL_STYLE) & PGS_AUTOSCROLL)
				SetTimer(window, HOVER_TIMER, HOVER_DELAY, NULL);
		}
		return 0;
	}
	case WM_NCMOUSELEAVE:
	case WM_MOUSELEAVE:
		state->tracking = FALSE;
		KillTimer(window, HOVER_TIMER);
		set_hot(state, BUTTON_NONE);
		return 0;
	case WM_LBUTTONUP:
	case WM_CAPTURECHANGED:
		end_press(state);
		return 0;
	case WM_MOUSEMOVE:
		if (state->forward_mouse && state->child) {
			POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
			MapWindowPoints(window, state->child, &at, 1);
			return SendMessageW(state->child, message, wparam, MAKELPARAM(at.x, at.y));
		}
		return 0;
	case WM_TIMER:
		if (wparam == REPEAT_TIMER && state->pressed != BUTTON_NONE) {
			int which = state->pressed;
			POINT at;
			GetCursorPos(&at);
			SetTimer(window, REPEAT_TIMER, NEXT_DELAY, NULL);
			if (button_at(state, at) == which)
				scroll_by_button(state, which);
			if (!button_shown(state, which))
				end_press(state);
			return 0;
		}
		if (wparam == HOVER_TIMER) {
			POINT at;
			int which;
			GetCursorPos(&at);
			which = button_at(state, at);
			if (which == BUTTON_NONE)
				KillTimer(window, HOVER_TIMER);
			else
				scroll_by_button(state, which);
			return 0;
		}
		break;
	case WM_SIZE:
		place_child(state);
		set_position(state, state->position);
		return 0;
	case WM_ERASEBKGND: {
		RECT client;
		HBRUSH brush = CreateSolidBrush(state->background);
		GetClientRect(window, &client);
		FillRect((HDC)wparam, &client, brush);
		DeleteObject(brush);
		return 1;
	}
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void PAGER_Register(void)
{
	WNDCLASSW info;

	ZeroMemory(&info, sizeof(info));
	info.style = CS_GLOBALCLASS | CS_DBLCLKS | CS_SAVEBITS;
	info.lpfnWndProc = pager_proc;
	info.cbWndExtra = sizeof(pager_state *);
	info.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	info.lpszClassName = WC_PAGESCROLLERW;
	RegisterClassW(&info);
}

void PAGER_Unregister(void)
{
	UnregisterClassW(WC_PAGESCROLLERW, NULL);
}
