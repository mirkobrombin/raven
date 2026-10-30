/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include "user32_private.h"

/* The combo box control, from Microsoft's documentation of its parts, messages and notification order. */

#define LIST_ID 1000
#define EDIT_ID 1001
#define BORDER 2
#define FIELD_INSET 3
#define MIN_VISIBLE 30

#ifndef ES_COMBO
#define ES_COMBO 0x00000200
#endif

#ifndef CB_GETCOMBOBOXINFO
#define CB_GETCOMBOBOXINFO 0x0164
#endif

typedef struct {
	HWND owner, list, edit;
	HFONT font;
	int field_height, drop_height, drop_width;
	BOOL dropped, pressed, extended_ui, focused, resizing, keyed, setting_text, no_field_text;
	int chosen_before;
} combo_state;

static combo_state *state_of(HWND window)
{
	return (combo_state *)GetWindowLongPtrW(window, 0);
}

static UINT kind_of(HWND window)
{
	return (DWORD)GetWindowLongPtrW(window, GWL_STYLE) & 3;
}

static void notify(HWND window, const combo_state *state, WORD code)
{
	SendMessageW(state->owner, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(window), code), (LPARAM)window);
}

static void field_rect(HWND window, const combo_state *state, RECT *rect)
{
	RECT client;
	GetClientRect(window, &client);
	rect->left = FIELD_INSET;
	rect->top = FIELD_INSET;
	rect->right = client.right - FIELD_INSET;
	rect->bottom = state->field_height - FIELD_INSET;
	if (kind_of(window) != CBS_SIMPLE)
		rect->right = client.right - BORDER - GetSystemMetrics(SM_CXVSCROLL);
	if (rect->right < rect->left)
		rect->right = rect->left;
}

static void button_rect(HWND window, const combo_state *state, RECT *rect)
{
	RECT client;
	GetClientRect(window, &client);
	rect->right = client.right - BORDER;
	rect->left = rect->right - GetSystemMetrics(SM_CXVSCROLL);
	rect->top = BORDER;
	rect->bottom = state->field_height - BORDER;
	if (rect->left < BORDER)
		rect->left = BORDER;
}

static int font_height(HWND window, const combo_state *state)
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

/* Lays the children out and keeps the window itself to the height of its closed form. */
static void arrange(HWND window, combo_state *state)
{
	RECT client, field;
	GetClientRect(window, &client);
	field_rect(window, state, &field);
	if (state->edit)
		SetWindowPos(state->edit, NULL, field.left, field.top, field.right - field.left, field.bottom - field.top, SWP_NOACTIVATE | SWP_NOZORDER);
	if (kind_of(window) == CBS_SIMPLE) {
		SetWindowPos(state->list, NULL, 0, state->field_height, client.right, client.bottom - state->field_height, SWP_NOACTIVATE | SWP_NOZORDER);
		return;
	}
	if (client.bottom != state->field_height && !state->resizing) {
		RECT frame;
		GetWindowRect(window, &frame);
		state->resizing = TRUE;
		SetWindowPos(window, NULL, 0, 0, frame.right - frame.left, state->field_height, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
		state->resizing = FALSE;
	}
}

/* The selected item's text goes to the field; the application is not told, as the text did not come from typing. */
static void show_selection(HWND window, combo_state *state)
{
	int selected = (int)SendMessageW(state->list, LB_GETCURSEL, 0, 0);
	if (state->edit) {
		WCHAR small_text[256], *text = small_text;
		text[0] = 0;
		if (selected >= 0) {
			int length = (int)SendMessageW(state->list, LB_GETTEXTLEN, selected, 0);
			if (length >= 256)
				text = HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
			if (text)
				SendMessageW(state->list, LB_GETTEXT, selected, (LPARAM)text);
		}
		if (text) {
			state->setting_text = TRUE;
			SendMessageW(state->edit, WM_SETTEXT, 0, (LPARAM)text);
			state->setting_text = FALSE;
			if (state->focused)
				SendMessageW(state->edit, EM_SETSEL, 0, -1);
			if (text != small_text)
				HeapFree(GetProcessHeap(), 0, text);
		}
	}
	RECT field;
	field_rect(window, state, &field);
	InvalidateRect(window, &field, TRUE);
}

static void place_drop_down(HWND window, combo_state *state)
{
	RECT frame;
	GetWindowRect(window, &frame);
	int width = frame.right - frame.left;
	if (state->drop_width > width)
		width = state->drop_width;
	int item = (int)SendMessageW(state->list, LB_GETITEMHEIGHT, 0, 0);
	int count = (int)SendMessageW(state->list, LB_GETCOUNT, 0, 0);
	int border = 2 * GetSystemMetrics(SM_CYBORDER);
	int wanted = state->drop_height - (frame.bottom - frame.top);
	int contents = (count ? count : 1) * item + border;
	/* An unset height still has to show something. */
	if (wanted < item + border)
		wanted = contents < (MIN_VISIBLE * item + border) ? contents : MIN_VISIBLE * item + border;
	if (!((DWORD)GetWindowLongPtrW(window, GWL_STYLE) & CBS_NOINTEGRALHEIGHT))
		wanted = wanted - (wanted - border) % item;
	if (wanted > contents)
		wanted = contents;
	int y = frame.bottom;
	if (y + wanted > GetSystemMetrics(SM_CYSCREEN) && frame.top - wanted >= 0)
		y = frame.top - wanted;
	SetWindowPos(state->list, HWND_TOPMOST, frame.left, y, width, wanted, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void open_list(HWND window, combo_state *state)
{
	if (state->dropped || kind_of(window) == CBS_SIMPLE)
		return;
	notify(window, state, CBN_DROPDOWN);
	state->dropped = TRUE;
	state->chosen_before = (int)SendMessageW(state->list, LB_GETCURSEL, 0, 0);
	place_drop_down(window, state);
	SendMessageW(state->list, LB_SETCARETINDEX, state->chosen_before >= 0 ? state->chosen_before : 0, 0);
	SetCapture(state->list);
	RECT button;
	button_rect(window, state, &button);
	InvalidateRect(window, &button, TRUE);
}

static void close_list(HWND window, combo_state *state)
{
	if (!state->dropped)
		return;
	state->dropped = FALSE;
	state->pressed = FALSE;
	if (GetCapture() == state->list)
		ReleaseCapture();
	ShowWindow(state->list, SW_HIDE);
	RECT button;
	button_rect(window, state, &button);
	InvalidateRect(window, &button, TRUE);
}

/* The user settled on the highlighted item: ok, closed, and changed if it is a new one. */
static void commit(HWND window, combo_state *state)
{
	int selected = (int)SendMessageW(state->list, LB_GETCURSEL, 0, 0);
	BOOL changed = selected != state->chosen_before;
	BOOL was_open = state->dropped;
	notify(window, state, CBN_SELENDOK);
	close_list(window, state);
	if (was_open)
		notify(window, state, CBN_CLOSEUP);
	show_selection(window, state);
	if (changed)
		notify(window, state, CBN_SELCHANGE);
	state->chosen_before = selected;
}

static void cancel(HWND window, combo_state *state)
{
	if (!state->dropped)
		return;
	SendMessageW(state->list, LB_SETCURSEL, state->chosen_before, 0);
	notify(window, state, CBN_SELENDCANCEL);
	close_list(window, state);
	notify(window, state, CBN_CLOSEUP);
	show_selection(window, state);
}

/* A movement key steps the selection with the list open or closed, and tells the application at once. */
static void forward_key(HWND window, combo_state *state, UINT message, WPARAM wparam)
{
	int before = (int)SendMessageW(state->list, LB_GETCURSEL, 0, 0);
	state->keyed = TRUE;
	SendMessageW(state->list, message, wparam, 0);
	state->keyed = FALSE;
	int after = (int)SendMessageW(state->list, LB_GETCURSEL, 0, 0);
	if (after == before)
		return;
	show_selection(window, state);
	notify(window, state, CBN_SELENDOK);
	notify(window, state, CBN_SELCHANGE);
	state->chosen_before = after;
}

static BOOL key_down(HWND window, combo_state *state, WPARAM key)
{
	BOOL alt = GetKeyState(VK_MENU) < 0;
	switch (key) {
	case VK_F4:
		if (state->extended_ui)
			return FALSE;
		break;
	case VK_UP:
	case VK_DOWN:
		if (alt)
			break;
		if (key == VK_DOWN && state->extended_ui && !state->dropped) {
			open_list(window, state);
			return TRUE;
		}
		forward_key(window, state, WM_KEYDOWN, key);
		return TRUE;
	case VK_PRIOR:
	case VK_NEXT:
	case VK_HOME:
	case VK_END:
		if (state->edit && !state->dropped)
			return FALSE;
		forward_key(window, state, WM_KEYDOWN, key);
		return TRUE;
	case VK_RETURN:
		if (!state->dropped)
			return FALSE;
		commit(window, state);
		return TRUE;
	case VK_ESCAPE:
		if (!state->dropped)
			return FALSE;
		cancel(window, state);
		return TRUE;
	default:
		return FALSE;
	}
	if (state->dropped)
		commit(window, state);
	else
		open_list(window, state);
	return TRUE;
}

static void paint_field(HWND window, combo_state *state, HDC dc)
{
	RECT field;
	field_rect(window, state, &field);
	HGDIOBJ previous = state->font ? SelectObject(dc, state->font) : NULL;
	HBRUSH brush = (HBRUSH)SendMessageW(state->owner, WM_CTLCOLORLISTBOX, (WPARAM)dc, (LPARAM)window);
	if (!brush)
		brush = GetSysColorBrush(COLOR_WINDOW);
	BOOL highlighted = state->focused && !state->dropped;
	int selected = (int)SendMessageW(state->list, LB_GETCURSEL, 0, 0);
	DWORD style = (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
	if (style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) {
		DRAWITEMSTRUCT item = {ODT_COMBOBOX, (UINT)GetDlgCtrlID(window), (UINT)selected, ODA_DRAWENTIRE, ODS_COMBOBOXEDIT, window, dc, field, 0};
		if (selected >= 0)
			item.itemData = SendMessageW(state->list, LB_GETITEMDATA, selected, 0);
		if (highlighted)
			item.itemState |= ODS_SELECTED | ODS_FOCUS;
		if (!IsWindowEnabled(window))
			item.itemState |= ODS_DISABLED;
		FillRect(dc, &field, brush);
		SendMessageW(state->owner, WM_DRAWITEM, item.CtlID, (LPARAM)&item);
	} else {
		FillRect(dc, &field, highlighted ? GetSysColorBrush(COLOR_HIGHLIGHT) : brush);
		if (!IsWindowEnabled(window))
			SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
		else if (highlighted)
			SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
		if (selected >= 0) {
			int length = (int)SendMessageW(state->list, LB_GETTEXTLEN, selected, 0);
			WCHAR small_text[256], *text = length < 256 ? small_text : HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
			if (text) {
				SendMessageW(state->list, LB_GETTEXT, selected, (LPARAM)text);
				RECT area = field;
				area.left += 1;
				int mode = SetBkMode(dc, TRANSPARENT);
				DrawTextW(dc, text, -1, &area, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
				SetBkMode(dc, mode);
				if (text != small_text)
					HeapFree(GetProcessHeap(), 0, text);
			}
		}
		if (highlighted)
			DrawFocusRect(dc, &field);
	}
	if (previous)
		SelectObject(dc, previous);
}

static void paint(HWND window, combo_state *state, HDC dc)
{
	RECT client, box;
	GetClientRect(window, &client);
	box = client;
	box.bottom = state->field_height;
	HBRUSH brush = (HBRUSH)SendMessageW(state->owner, state->edit ? WM_CTLCOLOREDIT : WM_CTLCOLORLISTBOX, (WPARAM)dc, (LPARAM)window);
	FillRect(dc, &box, brush ? brush : GetSysColorBrush(COLOR_WINDOW));
	if (kind_of(window) != CBS_SIMPLE) {
		RECT button;
		button_rect(window, state, &button);
		DrawFrameControl(dc, &button, DFC_SCROLL, DFCS_SCROLLCOMBOBOX | (state->pressed ? DFCS_PUSHED : 0) | (IsWindowEnabled(window) ? 0 : DFCS_INACTIVE));
	}
	if (!state->edit)
		paint_field(window, state, dc);
	DrawEdge(dc, &box, EDGE_SUNKEN, BF_RECT);
}

static void forward_child_notification(HWND window, combo_state *state, WPARAM wparam, LPARAM lparam)
{
	HWND child = (HWND)lparam;
	WORD code = HIWORD(wparam);
	if (child == state->edit) {
		switch (code) {
		case EN_SETFOCUS:
			if (!state->focused) {
				state->focused = TRUE;
				notify(window, state, CBN_SETFOCUS);
			}
			break;
		case EN_KILLFOCUS: {
			HWND now = GetFocus();
			if (now != window && now != state->list) {
				cancel(window, state);
				state->focused = FALSE;
				notify(window, state, CBN_KILLFOCUS);
			}
			break;
		}
		case EN_CHANGE:
			if (state->setting_text)
				break;
			/* What the user types deselects the list unless it names an item. */
			{
				int length = GetWindowTextLengthW(state->edit);
				WCHAR small_text[256], *text = length < 256 ? small_text : HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
				if (text) {
					GetWindowTextW(state->edit, text, length + 1);
					int found = (int)SendMessageW(state->list, LB_FINDSTRINGEXACT, -1, (LPARAM)text);
					SendMessageW(state->list, LB_SETCURSEL, found, 0);
					if (found >= 0)
						SendMessageW(state->list, LB_SETTOPINDEX, found, 0);
					if (text != small_text)
						HeapFree(GetProcessHeap(), 0, text);
				}
			}
			notify(window, state, CBN_EDITCHANGE);
			break;
		case EN_UPDATE:
			if (!state->setting_text)
				notify(window, state, CBN_EDITUPDATE);
			break;
		case EN_ERRSPACE:
			notify(window, state, CBN_ERRSPACE);
			break;
		}
		return;
	}
	if (child != state->list)
		return;
	switch (code) {
	case LBN_SELCHANGE:
		if (state->keyed)
			break;
		commit(window, state);
		break;
	case LBN_SELCANCEL:
		cancel(window, state);
		break;
	case LBN_DBLCLK:
		notify(window, state, CBN_DBLCLK);
		break;
	case LBN_ERRSPACE:
		notify(window, state, CBN_ERRSPACE);
		break;
	}
}

static LRESULT create_children(HWND window, combo_state *state, const CREATESTRUCTW *create)
{
	DWORD style = create->style;
	UINT kind = style & 3;
	DWORD list_style = WS_VSCROLL | WS_CLIPSIBLINGS | LBS_COMBOBOX | LBS_NOTIFY;
	if (style & CBS_OWNERDRAWFIXED)
		list_style |= LBS_OWNERDRAWFIXED;
	if (style & CBS_OWNERDRAWVARIABLE)
		list_style |= LBS_OWNERDRAWVARIABLE;
	if (style & CBS_SORT)
		list_style |= LBS_SORT;
	if (style & CBS_HASSTRINGS)
		list_style |= LBS_HASSTRINGS;
	if (style & CBS_DISABLENOSCROLL)
		list_style |= LBS_DISABLENOSCROLL;
	if (style & CBS_NOINTEGRALHEIGHT)
		list_style |= LBS_NOINTEGRALHEIGHT;
	DWORD list_ex = 0;
	if (kind == CBS_SIMPLE) {
		list_style |= WS_CHILD | WS_VISIBLE | LBS_NOINTEGRALHEIGHT;
		list_ex = WS_EX_CLIENTEDGE;
	} else {
		list_style |= WS_POPUP | WS_BORDER;
		list_ex = WS_EX_TOPMOST | WS_EX_NOACTIVATE;
	}
	state->list = CreateWindowExW(list_ex, L"ComboLBox", NULL, list_style, 0, state->field_height, create->cx, 0, window, kind == CBS_SIMPLE ? (HMENU)(ULONG_PTR)LIST_ID : NULL, create->hInstance, NULL);
	if (!state->list)
		return -1;
	/* For a popup that parameter would name a menu, so its identifier is set apart. */
	SetWindowLongPtrW(state->list, GWLP_ID, LIST_ID);
	if (kind != CBS_DROPDOWNLIST) {
		DWORD edit_style = WS_CHILD | WS_VISIBLE | ES_COMBO | ES_LEFT | ES_NOHIDESEL;
		if (style & CBS_AUTOHSCROLL)
			edit_style |= ES_AUTOHSCROLL;
		if (style & CBS_OEMCONVERT)
			edit_style |= ES_OEMCONVERT;
		if (style & CBS_LOWERCASE)
			edit_style |= ES_LOWERCASE;
		if (style & CBS_UPPERCASE)
			edit_style |= ES_UPPERCASE;
		if (style & WS_DISABLED)
			edit_style |= WS_DISABLED;
		state->edit = CreateWindowExW(0, L"Edit", create->lpszName, edit_style, 0, 0, 0, 0, window, (HMENU)(ULONG_PTR)EDIT_ID, create->hInstance, NULL);
		if (!state->edit)
			return -1;
	}
	return 0;
}

/* The owner of an owner-drawn combo box is asked for the height of the selection field. */
static void measure_field(HWND window, combo_state *state, DWORD style)
{
	if (!(style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)))
		return;
	MEASUREITEMSTRUCT measure = {ODT_COMBOBOX, (UINT)GetDlgCtrlID(window), (UINT)-1, 0, (UINT)(state->field_height - 2 * FIELD_INSET), 0};
	SendMessageW(state->owner, WM_MEASUREITEM, measure.CtlID, (LPARAM)&measure);
	if (measure.itemHeight > 0)
		state->field_height = (int)measure.itemHeight + 2 * FIELD_INSET;
}

/* The list reports drawing with its own identity, and the application expects the combo box's. */
static LRESULT forward_owner_item(HWND window, combo_state *state, UINT message, LPARAM lparam)
{
	UINT id = (UINT)GetDlgCtrlID(window);
	switch (message) {
	case WM_DRAWITEM: {
		DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lparam;
		item->CtlType = ODT_COMBOBOX;
		item->CtlID = id;
		item->hwndItem = window;
		break;
	}
	case WM_MEASUREITEM: {
		MEASUREITEMSTRUCT *item = (MEASUREITEMSTRUCT *)lparam;
		item->CtlType = ODT_COMBOBOX;
		item->CtlID = id;
		break;
	}
	case WM_DELETEITEM: {
		DELETEITEMSTRUCT *item = (DELETEITEMSTRUCT *)lparam;
		item->CtlType = ODT_COMBOBOX;
		item->CtlID = id;
		item->hwndItem = window;
		break;
	}
	case WM_COMPAREITEM: {
		COMPAREITEMSTRUCT *item = (COMPAREITEMSTRUCT *)lparam;
		item->CtlType = ODT_COMBOBOX;
		item->CtlID = id;
		item->hwndItem = window;
		break;
	}
	}
	return SendMessageW(state->owner, message, id, lparam);
}

BOOL WINAPI GetComboBoxInfo(HWND window, PCOMBOBOXINFO info)
{
	return (BOOL)SendMessageW(window, CB_GETCOMBOBOXINFO, 0, (LPARAM)info);
}

LRESULT WINAPI RavenComboProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	combo_state *state = state_of(window);
	if (!state && message != WM_NCCREATE)
		return DefWindowProcW(window, message, wparam, lparam);
	switch (message) {
	case WM_NCCREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
		if (!state)
			return FALSE;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		state->owner = create->hwndParent;
		state->drop_height = create->cy;
		state->chosen_before = -1;
		/* The scroll bars of the styles belong to the list, not to the field. */
		SetWindowLongPtrW(window, GWL_STYLE, GetWindowLongPtrW(window, GWL_STYLE) & ~(LONG_PTR)(WS_VSCROLL | WS_HSCROLL));
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_CREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state->field_height = font_height(window, state) + 2 + 2 * FIELD_INSET;
		measure_field(window, state, create->style);
		if (create_children(window, state, create) < 0)
			return -1;
		arrange(window, state);
		return 0;
	}
	case WM_NCDESTROY:
		HeapFree(GetProcessHeap(), 0, state);
		SetWindowLongPtrW(window, 0, 0);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_DESTROY:
		if (state->list && kind_of(window) != CBS_SIMPLE)
			DestroyWindow(state->list);
		state->list = NULL;
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_SIZE: {
		if (!state->list)
			return 0;
		if (!state->resizing && kind_of(window) != CBS_SIMPLE) {
			RECT frame;
			GetWindowRect(window, &frame);
			/* A taller size is a new drop-down height; the window keeps its closed height either way. */
			if (frame.bottom - frame.top > state->field_height)
				state->drop_height = frame.bottom - frame.top;
		}
		arrange(window, state);
		return 0;
	}
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
		return 1;
	case WM_ENABLE:
		if (state->edit)
			EnableWindow(state->edit, (BOOL)wparam);
		EnableWindow(state->list, (BOOL)wparam);
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_GETDLGCODE:
		if (lparam) {
			const MSG *pending = (const MSG *)lparam;
			if (pending->message == WM_KEYDOWN && (pending->wParam == VK_RETURN || pending->wParam == VK_ESCAPE) && state->dropped)
				return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTMESSAGE;
		}
		return DLGC_WANTARROWS | DLGC_WANTCHARS;
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		SendMessageW(state->list, WM_SETFONT, wparam, lparam);
		if (state->edit)
			SendMessageW(state->edit, WM_SETFONT, wparam, lparam);
		state->field_height = font_height(window, state) + 2 + 2 * FIELD_INSET;
		measure_field(window, state, (DWORD)GetWindowLongPtrW(window, GWL_STYLE));
		arrange(window, state);
		if (lparam)
			InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_SETFOCUS:
		if (state->edit) {
			SetFocus(state->edit);
			SendMessageW(state->edit, EM_SETSEL, 0, -1);
			return 0;
		}
		if (!state->focused) {
			state->focused = TRUE;
			notify(window, state, CBN_SETFOCUS);
		}
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_KILLFOCUS:
		if (state->edit)
			return 0;
		if ((HWND)wparam != state->list)
			cancel(window, state);
		state->focused = FALSE;
		notify(window, state, CBN_KILLFOCUS);
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		RECT button;
		button_rect(window, state, &button);
		SetFocus(state->edit ? state->edit : window);
		if (kind_of(window) == CBS_SIMPLE)
			return 0;
		if (kind_of(window) == CBS_DROPDOWN && !PtInRect(&button, point))
			return 0;
		if (state->dropped)
			cancel(window, state);
		else {
			state->pressed = TRUE;
			open_list(window, state);
		}
		return 0;
	}
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (key_down(window, state, wparam))
			return 0;
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_CHAR:
		if (!state->edit)
			forward_key(window, state, WM_CHAR, wparam);
		else
			SendMessageW(state->edit, WM_CHAR, wparam, lparam);
		return 0;
	case WM_MOUSEWHEEL:
		if (!state->dropped) {
			forward_key(window, state, WM_KEYDOWN, (short)HIWORD(wparam) > 0 ? VK_UP : VK_DOWN);
			return 0;
		}
		return SendMessageW(state->list, message, wparam, lparam);
	case WM_COMMAND:
		forward_child_notification(window, state, wparam, lparam);
		return 0;
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX:
	case WM_CTLCOLORSTATIC:
		return SendMessageW(state->owner, message, wparam, lparam);
	case WM_DRAWITEM:
	case WM_MEASUREITEM:
	case WM_DELETEITEM:
	case WM_COMPAREITEM:
		return forward_owner_item(window, state, message, lparam);
	case WM_GETTEXTLENGTH:
		if (state->edit)
			return SendMessageW(state->edit, message, wparam, lparam);
		{
			int selected = (int)SendMessageW(state->list, LB_GETCURSEL, 0, 0);
			return selected >= 0 ? SendMessageW(state->list, LB_GETTEXTLEN, selected, 0) : 0;
		}
	case WM_GETTEXT:
		if (state->edit)
			return SendMessageW(state->edit, message, wparam, lparam);
		{
			int selected = (int)SendMessageW(state->list, LB_GETCURSEL, 0, 0);
			WCHAR *out = (WCHAR *)lparam;
			if (!wparam)
				return 0;
			out[0] = 0;
			if (selected < 0)
				return 0;
			int length = (int)SendMessageW(state->list, LB_GETTEXTLEN, selected, 0);
			WCHAR *text = HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
			if (!text)
				return 0;
			SendMessageW(state->list, LB_GETTEXT, selected, (LPARAM)text);
			lstrcpynW(out, text, (int)wparam);
			HeapFree(GetProcessHeap(), 0, text);
			return lstrlenW(out);
		}
	case WM_SETTEXT:
		if (state->edit)
			return SendMessageW(state->edit, message, wparam, lparam);
		{
			/* A field without an edit control can only show an item of the list. */
			int found = (int)SendMessageW(state->list, LB_FINDSTRINGEXACT, -1, lparam);
			SendMessageW(state->list, LB_SETCURSEL, found, 0);
			show_selection(window, state);
			return found >= 0 ? TRUE : CB_ERR;
		}
	case CB_ADDSTRING:
		return SendMessageW(state->list, LB_ADDSTRING, wparam, lparam);
	case CB_INSERTSTRING:
		return SendMessageW(state->list, LB_INSERTSTRING, wparam, lparam);
	case CB_DELETESTRING: {
		int selected = (int)SendMessageW(state->list, LB_GETCURSEL, 0, 0);
		LRESULT result = SendMessageW(state->list, LB_DELETESTRING, wparam, lparam);
		if (selected == (int)wparam)
			show_selection(window, state);
		return result;
	}
	case CB_RESETCONTENT:
		SendMessageW(state->list, LB_RESETCONTENT, 0, 0);
		if (state->edit) {
			state->setting_text = TRUE;
			SendMessageW(state->edit, WM_SETTEXT, 0, (LPARAM)L"");
			state->setting_text = FALSE;
		}
		state->chosen_before = -1;
		InvalidateRect(window, NULL, TRUE);
		return CB_OKAY;
	case CB_DIR:
		return SendMessageW(state->list, LB_DIR, wparam, lparam);
	case CB_FINDSTRING:
		return SendMessageW(state->list, LB_FINDSTRING, wparam, lparam);
	case CB_FINDSTRINGEXACT:
		return SendMessageW(state->list, LB_FINDSTRINGEXACT, wparam, lparam);
	case CB_GETCOUNT:
		return SendMessageW(state->list, LB_GETCOUNT, 0, 0);
	case CB_GETCURSEL:
		return SendMessageW(state->list, LB_GETCURSEL, 0, 0);
	case CB_SETCURSEL: {
		LRESULT result = SendMessageW(state->list, LB_SETCURSEL, wparam, 0);
		state->chosen_before = (int)SendMessageW(state->list, LB_GETCURSEL, 0, 0);
		show_selection(window, state);
		return result;
	}
	case CB_SELECTSTRING: {
		LRESULT result = SendMessageW(state->list, LB_SELECTSTRING, wparam, lparam);
		if (result != CB_ERR) {
			state->chosen_before = (int)result;
			show_selection(window, state);
		}
		return result;
	}
	case CB_GETITEMDATA:
		return SendMessageW(state->list, LB_GETITEMDATA, wparam, 0);
	case CB_SETITEMDATA:
		return SendMessageW(state->list, LB_SETITEMDATA, wparam, lparam);
	case CB_GETLBTEXT:
		return SendMessageW(state->list, LB_GETTEXT, wparam, lparam);
	case CB_GETLBTEXTLEN:
		return SendMessageW(state->list, LB_GETTEXTLEN, wparam, 0);
	case CB_GETTOPINDEX:
		return SendMessageW(state->list, LB_GETTOPINDEX, 0, 0);
	case CB_SETTOPINDEX:
		return SendMessageW(state->list, LB_SETTOPINDEX, wparam, 0) == LB_ERR ? CB_ERR : 0;
	case CB_INITSTORAGE:
		return SendMessageW(state->list, LB_INITSTORAGE, wparam, lparam);
	case CB_GETLOCALE:
		return SendMessageW(state->list, LB_GETLOCALE, 0, 0);
	case CB_SETLOCALE:
		return SendMessageW(state->list, LB_SETLOCALE, wparam, 0);
	case CB_GETHORIZONTALEXTENT:
		return SendMessageW(state->list, LB_GETHORIZONTALEXTENT, 0, 0);
	case CB_SETHORIZONTALEXTENT:
		return SendMessageW(state->list, LB_SETHORIZONTALEXTENT, wparam, 0);
	case CB_GETITEMHEIGHT:
		if ((int)wparam == -1)
			return state->field_height - 2 * FIELD_INSET;
		return SendMessageW(state->list, LB_GETITEMHEIGHT, wparam, 0);
	case CB_SETITEMHEIGHT:
		if ((int)wparam == -1) {
			if ((int)lparam < 1 || (int)lparam > 32767)
				return CB_ERR;
			state->field_height = (int)lparam + 2 * FIELD_INSET;
			arrange(window, state);
			InvalidateRect(window, NULL, TRUE);
			return 0;
		}
		return SendMessageW(state->list, LB_SETITEMHEIGHT, wparam, lparam);
	case CB_LIMITTEXT:
		if (!state->edit)
			return CB_ERR;
		SendMessageW(state->edit, EM_LIMITTEXT, wparam, 0);
		return TRUE;
	case CB_GETEDITSEL: {
		if (!state->edit)
			return CB_ERR;
		LRESULT range = SendMessageW(state->edit, EM_GETSEL, wparam, lparam);
		return range;
	}
	case CB_SETEDITSEL:
		if (!state->edit)
			return CB_ERR;
		SendMessageW(state->edit, EM_SETSEL, (short)LOWORD(lparam), (short)HIWORD(lparam));
		return TRUE;
	case CB_SHOWDROPDOWN:
		if (wparam)
			open_list(window, state);
		else if (state->dropped) {
			close_list(window, state);
			notify(window, state, CBN_CLOSEUP);
		}
		return TRUE;
	case CB_GETDROPPEDSTATE:
		return state->dropped;
	case CB_GETDROPPEDCONTROLRECT: {
		RECT *rect = (RECT *)lparam;
		GetWindowRect(window, rect);
		if (state->drop_width > rect->right - rect->left)
			rect->right = rect->left + state->drop_width;
		rect->bottom = rect->top + (kind_of(window) == CBS_SIMPLE || state->drop_height > state->field_height ? state->drop_height : state->field_height);
		return CB_OKAY;
	}
	case CB_GETDROPPEDWIDTH: {
		RECT frame;
		GetWindowRect(window, &frame);
		return state->drop_width > frame.right - frame.left ? state->drop_width : frame.right - frame.left;
	}
	case CB_SETDROPPEDWIDTH: {
		RECT frame;
		GetWindowRect(window, &frame);
		state->drop_width = (int)wparam;
		return state->drop_width > frame.right - frame.left ? state->drop_width : frame.right - frame.left;
	}
	case CB_SETEXTENDEDUI:
		state->extended_ui = wparam != 0;
		return CB_OKAY;
	case CB_GETEXTENDEDUI:
		return state->extended_ui;
	case CB_GETCOMBOBOXINFO: {
		COMBOBOXINFO *info = (COMBOBOXINFO *)lparam;
		if (!info || info->cbSize != sizeof(*info)) {
			SetLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		field_rect(window, state, &info->rcItem);
		button_rect(window, state, &info->rcButton);
		info->stateButton = kind_of(window) == CBS_SIMPLE ? STATE_SYSTEM_INVISIBLE : (state->pressed ? STATE_SYSTEM_PRESSED : 0);
		info->hwndCombo = window;
		info->hwndItem = state->edit ? state->edit : window;
		info->hwndList = state->list;
		return TRUE;
	}
	}
	return DefWindowProcW(window, message, wparam, lparam);
}
