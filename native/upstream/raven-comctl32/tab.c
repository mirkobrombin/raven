/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>

/* The tab control. Tabs are laid out along a logical axis and mapped to the client area once, so the four placements share one layout. */

#define NO_ITEM (-1)
#define DEFAULT_PADDING_X 6
#define DEFAULT_PADDING_Y 3
#define MIN_TAB_CHARS 6
#define FIXED_ICON_SLACK 4
#define BUTTON_GAP 3
#define PANE_MARGIN 4
#define INSET 2
#define SCROLLER_ID 1
/* Owner-drawn tabs read the low 32 bits unless the application reserved more than a pointer. */
#define DRAW_DATA_BYTES 4

typedef struct {
	WCHAR *text;
	int image;
	DWORD state;
	RECT area;
	int row;
	int span;
	BYTE *data;
} tab_item;

typedef struct {
	HWND window, parent, scroller, tooltip;
	tab_item *items;
	int count;
	int selected, focused, hot;
	HFONT font;
	HIMAGELIST images;
	HTHEME theme;
	BOOL unicode, custom_extra, needs_scroller;
	SIZE_T extra;
	int padding_x, padding_y, min_width, set_width, set_height;
	int first_visible, rows, row_height;
	DWORD extended;
} tab_state;

static tab_state *state_of(HWND window)
{
	return (tab_state *)GetWindowLongPtrW(window, 0);
}

static DWORD style_of(const tab_state *state)
{
	return (DWORD)GetWindowLongPtrW(state->window, GWL_STYLE);
}

static void *allocate(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void release(void *block)
{
	HeapFree(GetProcessHeap(), 0, block);
}

static WCHAR *copy_text(const void *text, BOOL wide)
{
	WCHAR *copy;
	if (!text || text == (const void *)LPSTR_TEXTCALLBACKW)
		return NULL;
	if (wide) {
		copy = allocate((lstrlenW(text) + 1) * sizeof(WCHAR));
		if (copy)
			lstrcpyW(copy, text);
	} else {
		int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
		copy = allocate(length * sizeof(WCHAR));
		if (copy)
			MultiByteToWideChar(CP_ACP, 0, text, -1, copy, length);
	}
	return copy;
}

static LRESULT notify(const tab_state *state, UINT code)
{
	NMHDR notice;
	notice.hwndFrom = state->window;
	notice.idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	notice.code = code;
	return SendMessageW(state->parent, WM_NOTIFY, notice.idFrom, (LPARAM)&notice);
}

static HFONT font_of(const tab_state *state)
{
	return state->font ? state->font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static BOOL is_vertical(const tab_state *state)
{
	return (style_of(state) & TCS_VERTICAL) != 0;
}

/* The first row of a placement touches the pane, so it is measured from the far side of the tabs. */
static RECT to_client(const tab_state *state, const RECT *client, int along, int across, int length, int thickness)
{
	DWORD style = style_of(state);
	RECT area;
	if (style & TCS_VERTICAL) {
		int left = (style & TCS_RIGHT) ? client->right - across - thickness : across;
		SetRect(&area, left, along, left + thickness, along + length);
	} else {
		int top = (style & TCS_BOTTOM) ? client->bottom - across - thickness : across;
		SetRect(&area, along, top, along + length, top + thickness);
	}
	return area;
}

static int item_width(const tab_state *state, HDC dc, const tab_item *item, int default_min)
{
	DWORD style = style_of(state);
	int icon_width = 0, icon_height = 0, least = state->min_width < 0 ? default_min : state->min_width;
	if (state->images && item->image >= 0)
		ImageList_GetIconSize(state->images, &icon_width, &icon_height);
	if (style & TCS_FIXEDWIDTH) {
		if (state->images && item->image >= 0)
			return max(state->set_width, icon_width + FIXED_ICON_SLACK);
		return state->set_width;
	}
	if (item->text) {
		SIZE extent = {0};
		GetTextExtentPoint32W(dc, item->text, lstrlenW(item->text), &extent);
		if (icon_width)
			return max(extent.cx + icon_width + 3 * state->padding_x, least);
		return max(extent.cx + 2 * state->padding_x, least);
	}
	if (icon_width)
		return max(icon_width + 3 * state->padding_x, least);
	return least;
}

static void sync_scroller(tab_state *state, const RECT *client, int scroller_width)
{
	if (!state->needs_scroller) {
		if (state->scroller)
			ShowWindow(state->scroller, SW_HIDE);
		return;
	}
	if (!state->scroller)
		state->scroller = CreateWindowExW(0, UPDOWN_CLASSW, NULL, WS_CHILD | UDS_HORZ, 0, 0, 0, 0, state->window, (HMENU)(INT_PTR)SCROLLER_ID, NULL, NULL);
	if (!state->scroller)
		return;
	SendMessageW(state->scroller, UDM_SETRANGE32, 0, state->count - 1);
	SendMessageW(state->scroller, UDM_SETPOS32, 0, state->first_visible);
	{
		RECT area = to_client(state, client, client->right - scroller_width, 0, scroller_width, state->row_height);
		SetWindowPos(state->scroller, NULL, area.left, area.top, area.right - area.left, area.bottom - area.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	}
}

static void sync_tooltip_tools(const tab_state *state)
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
		TTTOOLINFOW tool = {sizeof(tool)};
		tool.hwnd = state->window;
		tool.uId = index;
		tool.rect = state->items[index].area;
		tool.lpszText = LPSTR_TEXTCALLBACKW;
		SendMessageW(state->tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool);
	}
}

/* Rotates rows so the selected one sits against the pane, the way a card file brings a tab forward. */
static int display_row(const tab_state *state, int row, int selected_row)
{
	if (style_of(state) & TCS_BUTTONS)
		return row;
	return (row - selected_row - 1 + 2 * state->rows) % state->rows;
}

static void layout(tab_state *state)
{
	DWORD style = style_of(state);
	RECT client;
	HDC dc;
	HGDIOBJ old;
	TEXTMETRICW metrics;
	int index, along, row, available, default_min, total = 0, gap, scroller_width, selected_row = 0;
	GetClientRect(state->window, &client);
	available = is_vertical(state) ? client.bottom : client.right;
	gap = (style & TCS_BUTTONS) ? BUTTON_GAP : 0;
	dc = GetDC(state->window);
	old = SelectObject(dc, font_of(state));
	GetTextMetricsW(dc, &metrics);
	default_min = metrics.tmAveCharWidth * MIN_TAB_CHARS + 2 * state->padding_x;
	state->row_height = state->set_height;
	if (!state->row_height) {
		int icon_width = 0, icon_height = 0;
		if (state->images)
			ImageList_GetIconSize(state->images, &icon_width, &icon_height);
		state->row_height = max(metrics.tmHeight, icon_height) + 2 * state->padding_y - 1;
	}
	for (index = 0; index < state->count; ++index) {
		state->items[index].span = item_width(state, dc, &state->items[index], default_min);
		total += state->items[index].span + gap;
	}
	SelectObject(dc, old);
	ReleaseDC(state->window, dc);
	state->rows = 1;
	along = row = 0;
	if (style & TCS_MULTILINE) {
		for (index = 0; index < state->count; ++index) {
			tab_item *item = &state->items[index];
			if (along && along + item->span > available) {
				++row;
				along = 0;
			}
			item->row = row;
			along += item->span + gap;
		}
		state->rows = row + 1;
	} else {
		for (index = 0; index < state->count; ++index)
			state->items[index].row = 0;
	}
	state->needs_scroller = !(style & TCS_MULTILINE) && total > available && state->count > 1 && available > 0;
	scroller_width = state->needs_scroller ? 2 * state->row_height : 0;
	if (!state->needs_scroller)
		state->first_visible = 0;
	if (state->selected >= 0)
		selected_row = state->items[state->selected].row;
	along = 0;
	if (state->needs_scroller)
		for (index = 0; index < state->first_visible; ++index)
			along -= state->items[index].span + gap;
	for (row = 0; row < state->rows; ++row) {
		int across = display_row(state, row, selected_row) * state->row_height, stretch = 0, members = 0, used = 0;
		if ((style & TCS_MULTILINE) && (style & TCS_RIGHTJUSTIFY) && !(style & TCS_FIXEDWIDTH) && state->rows > 1) {
			for (index = 0; index < state->count; ++index) {
				if (state->items[index].row == row) {
					used += state->items[index].span;
					++members;
				}
			}
			if (members)
				stretch = max(available - used, 0) / members;
		}
		if (style & TCS_MULTILINE)
			along = 0;
		for (index = 0; index < state->count; ++index) {
			tab_item *item = &state->items[index];
			if (item->row != row)
				continue;
			item->area = to_client(state, &client, along, across, item->span + stretch, state->row_height);
			along += item->span + stretch + gap;
		}
	}
	sync_scroller(state, &client, scroller_width);
	sync_tooltip_tools(state);
	InvalidateRect(state->window, NULL, TRUE);
}

static void reveal(tab_state *state, int index)
{
	RECT client;
	int gap = (style_of(state) & TCS_BUTTONS) ? BUTTON_GAP : 0, scroller, available, used = 0, at;
	if (!state->needs_scroller || index < 0)
		return;
	GetClientRect(state->window, &client);
	available = is_vertical(state) ? client.bottom : client.right;
	scroller = 2 * state->row_height;
	if (index < state->first_visible) {
		state->first_visible = index;
		layout(state);
		return;
	}
	for (at = state->first_visible; at <= index; ++at)
		used += state->items[at].span + gap;
	if (used <= available - scroller)
		return;
	while (state->first_visible < index && used > available - scroller) {
		used -= state->items[state->first_visible].span + gap;
		++state->first_visible;
	}
	layout(state);
}

static int hit_test(const tab_state *state, POINT point, UINT *flags)
{
	int index;
	UINT found = TCHT_NOWHERE;
	if (state->needs_scroller && state->scroller) {
		RECT cover;
		GetWindowRect(state->scroller, &cover);
		MapWindowPoints(HWND_DESKTOP, state->window, (POINT *)&cover, 2);
		if (PtInRect(&cover, point)) {
			if (flags)
				*flags = TCHT_NOWHERE;
			return NO_ITEM;
		}
	}
	for (index = 0; index < state->count; ++index) {
		const tab_item *item = &state->items[index];
		if (PtInRect(&item->area, point)) {
			found = TCHT_ONITEMLABEL;
			if (state->images && item->image >= 0) {
				int width, height;
				ImageList_GetIconSize(state->images, &width, &height);
				if (point.x < item->area.left + state->padding_x + width)
					found = TCHT_ONITEMICON;
			}
			if (flags)
				*flags = found;
			return index;
		}
	}
	if (flags)
		*flags = found;
	return NO_ITEM;
}

static void set_pressed(tab_state *state, int index, BOOL pressed)
{
	if (index < 0 || index >= state->count)
		return;
	if (pressed)
		state->items[index].state |= TCIS_BUTTONPRESSED;
	else
		state->items[index].state &= ~TCIS_BUTTONPRESSED;
}

static void repaint_after_selection(tab_state *state)
{
	if (style_of(state) & TCS_MULTILINE)
		layout(state);
	InvalidateRect(state->window, NULL, TRUE);
}

/* Selects without telling the parent, which is the contract of TCM_SETCURSEL. */
static int change_selection(tab_state *state, int index)
{
	int previous = state->selected;
	BOOL buttons = (style_of(state) & TCS_BUTTONS) != 0;
	if (index >= state->count)
		return NO_ITEM;
	if (index < 0)
		index = NO_ITEM;
	if (index == previous)
		return previous;
	set_pressed(state, previous, FALSE);
	state->selected = index;
	set_pressed(state, index, TRUE);
	if (!buttons || index == NO_ITEM)
		state->focused = index;
	reveal(state, index);
	repaint_after_selection(state);
	return previous;
}

/* What a click or TCM_SETCURFOCUS does: the parent may veto, and a veto leaves everything alone. */
static void change_focus(tab_state *state, int index)
{
	if (index >= state->count)
		return;
	if (style_of(state) & TCS_BUTTONS) {
		if (index < 0 || index == state->focused)
			return;
		notify(state, TCN_FOCUSCHANGE);
		if (state->focused != state->selected)
			set_pressed(state, state->focused, FALSE);
		state->focused = index;
		set_pressed(state, index, TRUE);
		reveal(state, index);
		InvalidateRect(state->window, NULL, TRUE);
		return;
	}
	if (index < 0) {
		if (state->focused == NO_ITEM)
			return;
		change_selection(state, NO_ITEM);
		notify(state, TCN_SELCHANGE);
		return;
	}
	if (index == state->focused)
		return;
	if (notify(state, TCN_SELCHANGING))
		return;
	change_selection(state, index);
	notify(state, TCN_SELCHANGE);
}

static void click_item(tab_state *state, int index)
{
	DWORD style = style_of(state);
	if ((style & TCS_BUTTONS) && (style & TCS_MULTISELECT) && (GetKeyState(VK_CONTROL) & 0x8000)) {
		state->items[index].state ^= TCIS_BUTTONPRESSED;
		InvalidateRect(state->window, NULL, TRUE);
		return;
	}
	if (index == state->selected)
		return;
	if (notify(state, TCN_SELCHANGING))
		return;
	change_selection(state, index);
	state->focused = index;
	notify(state, TCN_SELCHANGE);
}

static BOOL write_fields(tab_state *state, tab_item *item, const TCITEMW *given, BOOL wide)
{
	if (given->mask & TCIF_IMAGE)
		item->image = given->iImage;
	if (given->mask & TCIF_STATE)
		item->state = (item->state & ~given->dwStateMask) | (given->dwState & given->dwStateMask);
	if (given->mask & TCIF_PARAM)
		memcpy(item->data, &given->lParam, state->extra);
	if (given->mask & TCIF_TEXT) {
		WCHAR *text = copy_text(given->pszText, wide);
		if (given->pszText && given->pszText != LPSTR_TEXTCALLBACKW && !text)
			return FALSE;
		release(item->text);
		item->text = text;
	}
	return TRUE;
}

static int insert_item(tab_state *state, int index, const TCITEMW *given, BOOL wide)
{
	tab_item *grown;
	BYTE *data;
	if (index < 0 || !given)
		return -1;
	data = allocate(max(state->extra, sizeof(LPARAM)));
	if (!data)
		return -1;
	grown = state->items ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, state->items, (state->count + 1) * sizeof(*grown)) : allocate(sizeof(*grown));
	if (!grown) {
		release(data);
		return -1;
	}
	state->items = grown;
	index = min(index, state->count);
	memmove(&grown[index + 1], &grown[index], (state->count - index) * sizeof(*grown));
	memset(&grown[index], 0, sizeof(*grown));
	grown[index].image = -1;
	grown[index].data = data;
	write_fields(state, &grown[index], given, wide);
	++state->count;
	if (state->count == 1) {
		state->selected = state->focused = 0;
		set_pressed(state, 0, TRUE);
	} else {
		if (state->selected >= index)
			++state->selected;
		if (state->focused == NO_ITEM)
			state->focused = index;
		else if (state->focused >= index)
			++state->focused;
	}
	layout(state);
	return index;
}

static void forget_item(tab_item *item)
{
	release(item->text);
	release(item->data);
}

static BOOL delete_item(tab_state *state, int index)
{
	if (index < 0 || index >= state->count)
		return FALSE;
	forget_item(&state->items[index]);
	--state->count;
	memmove(&state->items[index], &state->items[index + 1], (state->count - index) * sizeof(*state->items));
	if (state->selected == index)
		state->selected = NO_ITEM;
	else if (state->selected > index)
		--state->selected;
	if (state->focused == index)
		state->focused = NO_ITEM;
	else if (state->focused > index)
		--state->focused;
	if (state->first_visible >= state->count)
		state->first_visible = max(state->count - 1, 0);
	layout(state);
	return TRUE;
}

static void delete_all(tab_state *state)
{
	int index;
	for (index = 0; index < state->count; ++index)
		forget_item(&state->items[index]);
	state->count = 0;
	state->selected = state->focused = NO_ITEM;
	state->first_visible = 0;
	layout(state);
}

static BOOL get_item(const tab_state *state, int index, TCITEMW *out, BOOL wide)
{
	const tab_item *item;
	if (index < 0 || index >= state->count) {
		if (out->mask & TCIF_STATE)
			out->dwState = 0;
		if (out->mask & TCIF_PARAM)
			out->lParam = 0;
		if (out->mask & TCIF_IMAGE)
			out->iImage = 0;
		return FALSE;
	}
	item = &state->items[index];
	if (out->mask & TCIF_STATE)
		out->dwState = item->state & out->dwStateMask;
	if (out->mask & TCIF_IMAGE)
		out->iImage = item->image;
	if (out->mask & TCIF_PARAM)
		memcpy(&out->lParam, item->data, state->extra);
	if ((out->mask & TCIF_TEXT) && out->pszText && out->cchTextMax > 0) {
		const WCHAR *text = item->text ? item->text : L"";
		if (wide) {
			lstrcpynW(out->pszText, text, out->cchTextMax);
		} else if (!WideCharToMultiByte(CP_ACP, 0, text, -1, (char *)out->pszText, out->cchTextMax, NULL, NULL)) {
			((char *)out->pszText)[out->cchTextMax - 1] = 0;
		}
	}
	return TRUE;
}

static void draw_vertical_text(const tab_state *state, HDC dc, const WCHAR *text, const RECT *area)
{
	LOGFONTW description;
	HFONT rotated;
	HGDIOBJ old;
	SIZE extent;
	int length = lstrlenW(text);
	BOOL right = (style_of(state) & TCS_RIGHT) != 0;
	GetObjectW(font_of(state), sizeof(description), &description);
	description.lfEscapement = description.lfOrientation = right ? 2700 : 900;
	rotated = CreateFontIndirectW(&description);
	old = SelectObject(dc, rotated);
	GetTextExtentPoint32W(dc, text, length, &extent);
	if (right)
		TextOutW(dc, (area->left + area->right + extent.cy) / 2, (area->top + area->bottom - extent.cx) / 2, text, length);
	else
		TextOutW(dc, (area->left + area->right - extent.cy) / 2, (area->top + area->bottom + extent.cx) / 2, text, length);
	SelectObject(dc, old);
	DeleteObject(rotated);
}

static void draw_content(const tab_state *state, HDC dc, const tab_item *item, RECT area)
{
	DWORD style = style_of(state);
	UINT format = DT_SINGLELINE | DT_VCENTER | DT_CENTER;
	if (is_vertical(state)) {
		if (item->text)
			draw_vertical_text(state, dc, item->text, &area);
		return;
	}
	if (state->images && item->image >= 0) {
		int width, height, left;
		ImageList_GetIconSize(state->images, &width, &height);
		left = area.left + state->padding_x;
		if (!(style & TCS_FORCEICONLEFT) && !(style & TCS_FORCELABELLEFT) && item->text) {
			SIZE extent;
			GetTextExtentPoint32W(dc, item->text, lstrlenW(item->text), &extent);
			left = max(area.left + (area.right - area.left - width - state->padding_x - extent.cx) / 2, left);
		}
		ImageList_Draw(state->images, item->image, dc, left, area.top + (area.bottom - area.top - height) / 2, ILD_TRANSPARENT);
		area.left = left + width + state->padding_x;
		format = DT_SINGLELINE | DT_VCENTER;
	}
	if (style & TCS_FORCELABELLEFT) {
		format = DT_SINGLELINE | DT_VCENTER;
		area.left += state->padding_x;
	}
	if (item->text)
		DrawTextW(dc, item->text, -1, &area, format);
}

static void draw_owner_item(const tab_state *state, HDC dc, int index, const RECT *area)
{
	const tab_item *item = &state->items[index];
	DRAWITEMSTRUCT owner = {0};
	int saved = SaveDC(dc);
	owner.CtlType = ODT_TAB;
	owner.CtlID = (UINT)GetWindowLongPtrW(state->window, GWLP_ID);
	owner.itemID = index;
	owner.itemAction = ODA_DRAWENTIRE;
	owner.itemState = index == state->selected ? ODS_SELECTED : 0;
	owner.hwndItem = state->window;
	owner.hDC = dc;
	owner.rcItem = *area;
	if (state->custom_extra && state->extra > sizeof(LPARAM))
		owner.itemData = (ULONG_PTR)item->data;
	else
		memcpy(&owner.itemData, item->data, min(state->extra, (SIZE_T)DRAW_DATA_BYTES));
	/* The frame belongs to the control: owner painting cannot reach it. */
	IntersectClipRect(dc, area->left + INSET, area->top + INSET, area->right - INSET, area->bottom - INSET);
	SendMessageW(state->parent, WM_DRAWITEM, owner.CtlID, (LPARAM)&owner);
	RestoreDC(dc, saved);
}

static void paint_item(tab_state *state, HDC dc, int index)
{
	tab_item *item = &state->items[index];
	DWORD style = style_of(state);
	BOOL selected = index == state->selected || (item->state & TCIS_BUTTONPRESSED);
	RECT area = item->area, frame = item->area;
	if (area.right <= area.left || area.bottom <= area.top)
		return;
	if (!(style & TCS_BUTTONS) && !selected) {
		if (style & TCS_VERTICAL) {
			if (style & TCS_RIGHT)
				frame.right -= INSET;
			else
				frame.left += INSET;
		} else if (style & TCS_BOTTOM) {
			frame.bottom -= INSET;
		} else {
			frame.top += INSET;
		}
	}
	if (style & TCS_BUTTONS) {
		if (style & TCS_FLATBUTTONS) {
			if (selected)
				DrawEdge(dc, &frame, BDR_SUNKENOUTER, BF_RECT);
		} else {
			FillRect(dc, &frame, GetSysColorBrush(COLOR_BTNFACE));
			DrawEdge(dc, &frame, selected ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT | BF_SOFT);
		}
	} else if (state->theme) {
		int look = !IsWindowEnabled(state->window) ? TIS_DISABLED : selected ? TIS_SELECTED : state->hot == index ? TIS_HOT : TIS_NORMAL;
		FillRect(dc, &frame, GetSysColorBrush(selected ? COLOR_WINDOW : COLOR_BTNFACE));
		DrawThemeBackground(state->theme, dc, TABP_TABITEM, look, &frame, NULL);
	} else {
		FillRect(dc, &frame, GetSysColorBrush(COLOR_BTNFACE));
		DrawEdge(dc, &frame, EDGE_RAISED, BF_SOFT | ((style & TCS_VERTICAL) ? ((style & TCS_RIGHT) ? BF_TOP | BF_RIGHT | BF_BOTTOM : BF_TOP | BF_LEFT | BF_BOTTOM)
			: (style & TCS_BOTTOM) ? BF_LEFT | BF_BOTTOM | BF_RIGHT : BF_LEFT | BF_TOP | BF_RIGHT));
	}
	if (item->state & TCIS_HIGHLIGHTED) {
		RECT mark = frame;
		InflateRect(&mark, -INSET, -INSET);
		FillRect(dc, &mark, GetSysColorBrush(COLOR_HIGHLIGHT));
		SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
	} else {
		SetTextColor(dc, GetSysColor(IsWindowEnabled(state->window) ? COLOR_BTNTEXT : COLOR_GRAYTEXT));
	}
	if (style & TCS_OWNERDRAWFIXED)
		draw_owner_item(state, dc, index, &area);
	else
		draw_content(state, dc, item, frame);
	if (index == state->focused && GetFocus() == state->window && !(style & TCS_FOCUSNEVER)) {
		RECT mark = frame;
		InflateRect(&mark, -INSET, -INSET);
		SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
		DrawFocusRect(dc, &mark);
	}
}

static RECT pane_rect(const tab_state *state)
{
	DWORD style = style_of(state);
	RECT pane;
	int depth = state->rows * state->row_height;
	GetClientRect(state->window, &pane);
	if (style & TCS_VERTICAL) {
		if (style & TCS_RIGHT)
			pane.right -= depth;
		else
			pane.left += depth;
	} else if (style & TCS_BOTTOM) {
		pane.bottom -= depth;
	} else {
		pane.top += depth;
	}
	return pane;
}

static void paint(tab_state *state, HDC dc)
{
	DWORD style = style_of(state);
	RECT client;
	HGDIOBJ old;
	int index;
	GetClientRect(state->window, &client);
	FillRect(dc, &client, GetSysColorBrush(COLOR_BTNFACE));
	old = SelectObject(dc, font_of(state));
	SetBkMode(dc, TRANSPARENT);
	if (state->needs_scroller && state->scroller) {
		RECT cover;
		GetWindowRect(state->scroller, &cover);
		MapWindowPoints(HWND_DESKTOP, state->window, (POINT *)&cover, 2);
		ExcludeClipRect(dc, cover.left, cover.top, cover.right, cover.bottom);
	}
	if (!(style & TCS_BUTTONS)) {
		RECT pane = pane_rect(state);
		if (state->theme) {
			FillRect(dc, &pane, GetSysColorBrush(COLOR_WINDOW));
			DrawThemeBackground(state->theme, dc, TABP_PANE, 0, &pane, NULL);
		} else {
			DrawEdge(dc, &pane, EDGE_RAISED, BF_RECT | BF_SOFT);
		}
	}
	for (index = 0; index < state->count; ++index)
		if (index != state->selected)
			paint_item(state, dc, index);
	if (state->selected >= 0)
		paint_item(state, dc, state->selected);
	SelectObject(dc, old);
}

/* wparam FALSE turns a window rectangle into the display rectangle, TRUE goes the other way. */
static void adjust_rect(const tab_state *state, BOOL to_window, RECT *area)
{
	DWORD style = style_of(state);
	int depth = state->rows * state->row_height, sign = to_window ? 1 : -1;
	if (!(style & TCS_BUTTONS))
		InflateRect(area, sign * PANE_MARGIN, sign * PANE_MARGIN);
	if (style & TCS_VERTICAL) {
		if (style & TCS_RIGHT)
			area->right += sign * depth;
		else
			area->left -= sign * depth;
	} else if (style & TCS_BOTTOM) {
		area->bottom += sign * depth;
	} else {
		area->top -= sign * depth;
	}
}

static void key_down(tab_state *state, WPARAM key, LPARAM flags)
{
	NMTCKEYDOWN notice;
	int target = state->focused, direction = 0;
	notice.hdr.hwndFrom = state->window;
	notice.hdr.idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	notice.hdr.code = TCN_KEYDOWN;
	notice.wVKey = (WORD)key;
	notice.flags = (UINT)flags;
	SendMessageW(state->parent, WM_NOTIFY, notice.hdr.idFrom, (LPARAM)&notice);
	if (key == VK_LEFT || key == VK_UP)
		direction = -1;
	else if (key == VK_RIGHT || key == VK_DOWN)
		direction = 1;
	if (direction) {
		target += direction;
		if (target >= 0 && target < state->count)
			change_focus(state, target);
	} else if ((key == VK_RETURN || key == VK_SPACE) && state->focused >= 0 && (style_of(state) & TCS_BUTTONS)) {
		click_item(state, state->focused);
	}
}

static LRESULT CALLBACK tab_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	tab_state *state = state_of(window);
	BOOL wide = TRUE;
	if (!state && message != WM_NCCREATE)
		return DefWindowProcW(window, message, wparam, lparam);
	switch (message) {
	case WM_NCCREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		HDC dc;
		DWORD style = create->style | WS_CLIPSIBLINGS;
		if (style & TCS_VERTICAL)
			style |= TCS_MULTILINE;
		SetWindowLongPtrW(window, GWL_STYLE, style);
		state = allocate(sizeof(*state));
		if (!state)
			return FALSE;
		state->window = window;
		state->parent = create->hwndParent;
		state->selected = state->focused = state->hot = NO_ITEM;
		state->extra = sizeof(LPARAM);
		state->padding_x = DEFAULT_PADDING_X;
		state->padding_y = DEFAULT_PADDING_Y;
		state->min_width = -1;
		state->rows = 1;
		dc = GetDC(window);
		state->set_width = GetDeviceCaps(dc, LOGPIXELSX);
		ReleaseDC(window, dc);
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_CREATE:
		state->unicode = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_UNICODE;
		state->theme = OpenThemeData(window, L"Tab");
		if (style_of(state) & TCS_TOOLTIPS)
			state->tooltip = CreateWindowExW(0, TOOLTIPS_CLASSW, NULL, WS_POPUP, 0, 0, 0, 0, window, NULL, NULL, NULL);
		layout(state);
		return 0;
	case WM_DESTROY:
		CloseThemeData(state->theme);
		state->theme = NULL;
		return 0;
	case WM_NCDESTROY: {
		int index;
		for (index = 0; index < state->count; ++index)
			forget_item(&state->items[index]);
		release(state->items);
		release(state);
		SetWindowLongPtrW(window, 0, 0);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_THEMECHANGED:
		CloseThemeData(state->theme);
		state->theme = OpenThemeData(window, L"Tab");
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_NOTIFYFORMAT:
		if (lparam == NF_REQUERY)
			state->unicode = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_UNICODE;
		return state->unicode ? NFR_UNICODE : NFR_ANSI;
	case WM_NOTIFY: {
		const NMHDR *notice = (const NMHDR *)lparam;
		if (state->tooltip && notice->hwndFrom == state->tooltip)
			return SendMessageW(state->parent, WM_NOTIFY, wparam, lparam);
		return 0;
	}
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		layout(state);
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_SIZE:
	case WM_STYLECHANGED:
		layout(state);
		return 0;
	case WM_HSCROLL:
	case WM_VSCROLL:
		if ((HWND)lparam == state->scroller && LOWORD(wparam) == SB_THUMBPOSITION) {
			state->first_visible = (short)HIWORD(wparam);
			layout(state);
		}
		return 0;
	case WM_ERASEBKGND:
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
	case WM_NCHITTEST: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		ScreenToClient(window, &point);
		if (hit_test(state, point, NULL) == NO_ITEM && !state->needs_scroller)
			return HTTRANSPARENT;
		return HTCLIENT;
	}
	case WM_LBUTTONDOWN: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		int index = hit_test(state, point, NULL);
		if (!(style_of(state) & TCS_FOCUSNEVER))
			SetFocus(window);
		if (index != NO_ITEM)
			click_item(state, index);
		return 0;
	}
	case WM_LBUTTONUP:
		notify(state, NM_CLICK);
		return 0;
	case WM_RBUTTONUP:
		notify(state, NM_RCLICK);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_MOUSEMOVE: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		int hot = hit_test(state, point, NULL);
		if (state->tooltip) {
			MSG relayed = {window, message, wparam, lparam};
			SendMessageW(state->tooltip, TTM_RELAYEVENT, 0, (LPARAM)&relayed);
		}
		if (hot != state->hot) {
			TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
			state->hot = hot;
			TrackMouseEvent(&track);
			InvalidateRect(window, NULL, TRUE);
		}
		return 0;
	}
	case WM_MOUSELEAVE:
		state->hot = NO_ITEM;
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
		InvalidateRect(window, NULL, TRUE);
		notify(state, message == WM_SETFOCUS ? NM_SETFOCUS : NM_KILLFOCUS);
		return 0;
	case WM_GETDLGCODE:
		return DLGC_WANTARROWS | DLGC_WANTCHARS;
	case WM_KEYDOWN:
		key_down(state, wparam, lparam);
		return 0;
	case TCM_GETIMAGELIST:
		return (LRESULT)state->images;
	case TCM_SETIMAGELIST: {
		HIMAGELIST previous = state->images;
		state->images = (HIMAGELIST)lparam;
		layout(state);
		return (LRESULT)previous;
	}
	case TCM_GETITEMCOUNT:
		return state->count;
	case TCM_GETITEMA:
		wide = FALSE;
		/* fall through */
	case TCM_GETITEMW:
		return lparam ? get_item(state, (int)wparam, (TCITEMW *)lparam, wide) : FALSE;
	case TCM_SETITEMA:
		wide = FALSE;
		/* fall through */
	case TCM_SETITEMW:
		if ((int)wparam < 0 || (int)wparam >= state->count || !lparam)
			return FALSE;
		if (!write_fields(state, &state->items[(int)wparam], (const TCITEMW *)lparam, wide))
			return FALSE;
		layout(state);
		return TRUE;
	case TCM_INSERTITEMA:
		wide = FALSE;
		/* fall through */
	case TCM_INSERTITEMW:
		return insert_item(state, (int)wparam, (const TCITEMW *)lparam, wide);
	case TCM_DELETEITEM:
		return delete_item(state, (int)wparam);
	case TCM_DELETEALLITEMS:
		delete_all(state);
		return TRUE;
	case TCM_GETITEMRECT:
		if ((int)wparam < 0 || (int)wparam >= state->count || !lparam)
			return FALSE;
		*(RECT *)lparam = state->items[(int)wparam].area;
		return TRUE;
	case TCM_GETCURSEL:
		return state->selected;
	case TCM_SETCURSEL:
		return change_selection(state, (int)wparam);
	case TCM_GETCURFOCUS:
		return state->focused;
	case TCM_SETCURFOCUS:
		change_focus(state, (int)wparam);
		return 0;
	case TCM_HITTEST: {
		TCHITTESTINFO *hit = (TCHITTESTINFO *)lparam;
		return hit ? hit_test(state, hit->pt, &hit->flags) : NO_ITEM;
	}
	case TCM_SETITEMEXTRA:
		if ((INT_PTR)wparam < 0 || state->count)
			return FALSE;
		state->extra = wparam;
		state->custom_extra = TRUE;
		return TRUE;
	case TCM_ADJUSTRECT:
		if (!lparam)
			return -1;
		adjust_rect(state, wparam != 0, (RECT *)lparam);
		return 0;
	case TCM_SETITEMSIZE: {
		LRESULT previous = MAKELONG(state->set_width, state->set_height ? state->set_height : state->row_height);
		state->set_width = (short)LOWORD(lparam);
		state->set_height = (short)HIWORD(lparam);
		layout(state);
		return previous;
	}
	case TCM_REMOVEIMAGE: {
		int removed = (int)wparam, index;
		if (!state->images || !ImageList_Remove(state->images, removed))
			return 0;
		for (index = 0; index < state->count; ++index) {
			if (state->items[index].image == removed)
				state->items[index].image = -1;
			else if (state->items[index].image > removed)
				--state->items[index].image;
		}
		layout(state);
		return 0;
	}
	case TCM_SETPADDING:
		state->padding_x = (short)LOWORD(lparam);
		state->padding_y = (short)HIWORD(lparam);
		return 0;
	case TCM_GETROWCOUNT:
		return state->rows;
	case TCM_GETTOOLTIPS:
		return (LRESULT)state->tooltip;
	case TCM_SETTOOLTIPS:
		state->tooltip = (HWND)wparam;
		return 0;
	case TCM_SETMINTABWIDTH: {
		int previous = state->min_width;
		state->min_width = (int)lparam;
		layout(state);
		return previous;
	}
	case TCM_DESELECTALL: {
		int index;
		for (index = 0; index < state->count; ++index)
			if (!wparam || index != state->focused)
				state->items[index].state &= ~TCIS_BUTTONPRESSED;
		InvalidateRect(window, NULL, TRUE);
		return 0;
	}
	case TCM_HIGHLIGHTITEM:
		if ((int)wparam < 0 || (int)wparam >= state->count)
			return FALSE;
		if (LOWORD(lparam))
			state->items[(int)wparam].state |= TCIS_HIGHLIGHTED;
		else
			state->items[(int)wparam].state &= ~TCIS_HIGHLIGHTED;
		InvalidateRect(window, NULL, TRUE);
		return TRUE;
	case TCM_SETEXTENDEDSTYLE: {
		DWORD previous = state->extended, mask = wparam ? (DWORD)wparam : ~0u;
		state->extended = (previous & ~mask) | ((DWORD)lparam & mask);
		return previous;
	}
	case TCM_GETEXTENDEDSTYLE:
		return state->extended;
	case TCM_SETUNICODEFORMAT: {
		BOOL previous = state->unicode;
		state->unicode = wparam != 0;
		return previous;
	}
	case TCM_GETUNICODEFORMAT:
		return state->unicode;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void TAB_Register(void)
{
	WNDCLASSW definition = {0};
	definition.style = CS_GLOBALCLASS | CS_DBLCLKS;
	definition.lpfnWndProc = tab_procedure;
	definition.cbWndExtra = sizeof(void *);
	definition.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	definition.lpszClassName = WC_TABCONTROLW;
	RegisterClassW(&definition);
}

void TAB_Unregister(void)
{
	UnregisterClassW(WC_TABCONTROLW, NULL);
}
