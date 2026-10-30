/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include "user32_private.h"

/* The button control, from Microsoft's documentation of its styles, messages and notifications. */

#define CHECK_SIZE 13
#define CHECK_GAP 4

#ifndef BS_PUSHBOX
#define BS_PUSHBOX 0x0000000a
#endif

#ifndef BS_SPLITBUTTON
#define BS_SPLITBUTTON 0x0000000c
#define BS_DEFSPLITBUTTON 0x0000000d
#define BS_COMMANDLINK 0x0000000e
#define BS_DEFCOMMANDLINK 0x0000000f
#endif

#ifndef BM_SETDONTCLICK
#define BM_SETDONTCLICK 0x00f8
#endif

typedef struct {
	HFONT font;
	HANDLE image;
	BOOL image_is_icon;
	UINT check;
	BOOL highlighted, focused, captured, hide_focus, hide_prefix, down_by_key, no_click;
} button_state;

static button_state *state_of(HWND window)
{
	return (button_state *)GetWindowLongPtrW(window, 0);
}

static UINT kind_of(DWORD style)
{
	return style & BS_TYPEMASK;
}

static BOOL is_check_kind(UINT kind)
{
	switch (kind) {
	case BS_CHECKBOX:
	case BS_AUTOCHECKBOX:
	case BS_3STATE:
	case BS_AUTO3STATE:
	case BS_RADIOBUTTON:
	case BS_AUTORADIOBUTTON:
		return TRUE;
	}
	return FALSE;
}

static BOOL is_radio_kind(UINT kind)
{
	return kind == BS_RADIOBUTTON || kind == BS_AUTORADIOBUTTON;
}

static BOOL is_default_kind(UINT kind)
{
	return kind == BS_DEFPUSHBUTTON || kind == BS_DEFSPLITBUTTON || kind == BS_DEFCOMMANDLINK;
}

/* Split buttons and command links act as push buttons, and without common controls 6 they look like them too. */
static BOOL is_push_kind(UINT kind)
{
	return kind == BS_PUSHBUTTON || kind == BS_PUSHBOX || kind == BS_SPLITBUTTON || kind == BS_COMMANDLINK || is_default_kind(kind);
}

/* The two-state and three-state boxes also take +, = and - from the keyboard. */
static BOOL is_box_kind(UINT kind)
{
	return kind == BS_CHECKBOX || kind == BS_AUTOCHECKBOX || kind == BS_3STATE || kind == BS_AUTO3STATE;
}

static HBRUSH parent_brush(HWND window, HDC dc, UINT message)
{
	HWND parent = GetParent(window);
	HBRUSH brush = (HBRUSH)SendMessageW(parent ? parent : window, message, (WPARAM)dc, (LPARAM)window);
	return brush ? brush : GetSysColorBrush(COLOR_3DFACE);
}

static void notify(HWND window, WORD code)
{
	HWND parent = GetParent(window);
	if (parent)
		SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(window), code), (LPARAM)window);
}

/* The label's area in the client, with the alignment styles turned into format flags. */
static UINT label_format(DWORD style, UINT kind, const button_state *state)
{
	UINT format = 0, horizontal = style & BS_CENTER, vertical = style & BS_VCENTER;
	if (!horizontal)
		horizontal = is_push_kind(kind) ? BS_CENTER : BS_LEFT;
	if (!vertical)
		vertical = BS_VCENTER;
	if (kind == BS_GROUPBOX)
		vertical = BS_TOP;
	if (horizontal == BS_CENTER)
		format |= DT_CENTER;
	else if (horizontal == BS_RIGHT)
		format |= DT_RIGHT;
	if (style & BS_MULTILINE)
		format |= DT_WORDBREAK;
	else {
		format |= DT_SINGLELINE;
		if (vertical == BS_VCENTER)
			format |= DT_VCENTER;
		else if (vertical == BS_BOTTOM)
			format |= DT_BOTTOM;
	}
	if (state->hide_prefix)
		format |= DT_HIDEPREFIX;
	return format;
}

static void image_size(HANDLE image, BOOL is_icon, int *out_width, int *out_height)
{
	int width = 0, height = 0;
	if (!is_icon) {
		BITMAP bitmap;
		if (GetObjectW(image, sizeof(bitmap), &bitmap)) {
			width = bitmap.bmWidth;
			height = bitmap.bmHeight;
		}
	} else {
		ICONINFO info;
		if (GetIconInfo((HICON)image, &info)) {
			BITMAP bitmap;
			if (GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(bitmap), &bitmap)) {
				width = bitmap.bmWidth;
				height = info.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2;
			}
			if (info.hbmColor)
				DeleteObject(info.hbmColor);
			if (info.hbmMask)
				DeleteObject(info.hbmMask);
		}
	}
	*out_width = width;
	*out_height = height;
}

static void draw_label(HWND window, HDC dc, DWORD style, UINT kind, const RECT *area, button_state *state)
{
	RECT text_area = *area;
	int length = GetWindowTextLengthW(window);
	if (state->image) {
		/* With an image style the image replaces the text; without one the two sit side by side. */
		BOOL image_only = (style & (BS_ICON | BS_BITMAP)) != 0 || !length;
		int width, height;
		image_size(state->image, state->image_is_icon, &width, &height);
		int x = image_only ? area->left + (area->right - area->left - width) / 2 : area->left + 2;
		int y = area->top + (area->bottom - area->top - height) / 2;
		UINT flags = (state->image_is_icon ? DST_ICON : DST_BITMAP) | (IsWindowEnabled(window) ? 0 : DSS_DISABLED);
		DrawStateW(dc, NULL, NULL, (LPARAM)state->image, 0, x, y, width, height, flags);
		if (image_only)
			return;
		text_area.left = x + width + 2;
	}
	if (!length)
		return;
	WCHAR small_text[256], *text = length < 256 ? small_text : HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * (length + 1));
	if (!text)
		return;
	GetWindowTextW(window, text, length + 1);
	RECT box = text_area;
	UINT format = label_format(style, kind, state);
	/* A multiline label is centered by hand: DT_VCENTER only applies to a single line. */
	if (style & BS_MULTILINE) {
		RECT measured = box;
		int height = DrawTextW(dc, text, -1, &measured, format | DT_CALCRECT);
		if ((style & BS_VCENTER) == BS_VCENTER || !(style & BS_VCENTER))
			box.top += (box.bottom - box.top - height) / 2;
		else if ((style & BS_VCENTER) == BS_BOTTOM)
			box.top = box.bottom - height;
	}
	int mode = SetBkMode(dc, TRANSPARENT);
	if (!IsWindowEnabled(window))
		SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
	DrawTextW(dc, text, -1, &box, format);
	SetBkMode(dc, mode);
	if (text != small_text)
		HeapFree(GetProcessHeap(), 0, text);
}

static void paint_push(HWND window, HDC dc, DWORD style, UINT kind, const RECT *client, button_state *state)
{
	RECT area = *client;
	/* The parent is told, but push buttons always use the system colors. */
	parent_brush(window, dc, WM_CTLCOLORBTN);
	if (is_default_kind(kind)) {
		FrameRect(dc, &area, GetSysColorBrush(COLOR_WINDOWFRAME));
		InflateRect(&area, -1, -1);
	}
	UINT state_flags = DFCS_BUTTONPUSH | (style & BS_FLAT ? DFCS_FLAT : 0) | (state->highlighted || state->check == BST_CHECKED ? DFCS_PUSHED : 0);
	DrawFrameControl(dc, &area, DFC_BUTTON, state_flags);
	RECT label = area;
	if (state->highlighted)
		OffsetRect(&label, 1, 1);
	SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
	draw_label(window, dc, style, kind, &label, state);
	if (state->focused && !state->hide_focus) {
		RECT focus = area;
		InflateRect(&focus, -3, -3);
		DrawFocusRect(dc, &focus);
	}
}

static void paint_check(HWND window, HDC dc, DWORD style, UINT kind, const RECT *client, button_state *state)
{
	FillRect(dc, client, parent_brush(window, dc, WM_CTLCOLORSTATIC));
	RECT box = *client, label = *client;
	BOOL right = style & BS_LEFTTEXT;
	int top = (client->bottom - CHECK_SIZE) / 2;
	if ((style & BS_VCENTER) == BS_TOP)
		top = 0;
	else if ((style & BS_VCENTER) == BS_BOTTOM)
		top = client->bottom - CHECK_SIZE;
	box.top = top;
	box.bottom = top + CHECK_SIZE;
	if (right) {
		box.left = client->right - CHECK_SIZE;
		label.right -= CHECK_SIZE + CHECK_GAP;
	} else {
		box.right = box.left + CHECK_SIZE;
		label.left += CHECK_SIZE + CHECK_GAP;
	}
	UINT frame;
	if (is_radio_kind(kind))
		frame = DFCS_BUTTONRADIO;
	else if ((kind == BS_3STATE || kind == BS_AUTO3STATE) && state->check == BST_INDETERMINATE)
		frame = DFCS_BUTTON3STATE;
	else
		frame = DFCS_BUTTONCHECK;
	if (state->check != BST_UNCHECKED)
		frame |= DFCS_CHECKED;
	if (state->highlighted)
		frame |= DFCS_PUSHED;
	if (!IsWindowEnabled(window))
		frame |= DFCS_INACTIVE;
	if (style & BS_FLAT)
		frame |= DFCS_FLAT;
	DrawFrameControl(dc, &box, DFC_BUTTON, frame);
	draw_label(window, dc, style, kind, &label, state);
	if (state->focused && !state->hide_focus && GetWindowTextLengthW(window)) {
		WCHAR text[256];
		GetWindowTextW(window, text, 256);
		RECT measure = label;
		UINT format = label_format(style, kind, state);
		DrawTextW(dc, text, -1, &measure, format | DT_CALCRECT);
		int height = measure.bottom - measure.top;
		measure.top = client->top + (client->bottom - height) / 2;
		measure.bottom = measure.top + height;
		InflateRect(&measure, 1, 1);
		if (measure.left < label.left)
			measure.left = label.left;
		DrawFocusRect(dc, &measure);
	}
}

static void paint_group(HWND window, HDC dc, DWORD style, const RECT *client, button_state *state)
{
	TEXTMETRICW metrics;
	GetTextMetricsW(dc, &metrics);
	HBRUSH brush = parent_brush(window, dc, WM_CTLCOLORSTATIC);
	RECT frame = *client;
	frame.top += metrics.tmHeight / 2;
	DrawEdge(dc, &frame, EDGE_ETCHED, BF_RECT | (style & BS_FLAT ? BF_FLAT | BF_MONO : 0));
	int length = GetWindowTextLengthW(window);
	if (!length)
		return;
	WCHAR small_text[256], *text = length < 256 ? small_text : HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * (length + 1));
	if (!text)
		return;
	GetWindowTextW(window, text, length + 1);
	RECT label = {client->left + 8, client->top, client->right - 8, client->top + metrics.tmHeight};
	RECT measure = label;
	UINT format = label_format(style, BS_GROUPBOX, state) | DT_SINGLELINE;
	DrawTextW(dc, text, -1, &measure, format | DT_CALCRECT);
	if ((style & BS_CENTER) == BS_CENTER)
		OffsetRect(&measure, (label.right - label.left - (measure.right - measure.left)) / 2, 0);
	else if ((style & BS_CENTER) == BS_RIGHT)
		OffsetRect(&measure, label.right - measure.right, 0);
	RECT clear = {measure.left - 2, measure.top, measure.right + 2, measure.bottom};
	FillRect(dc, &clear, brush);
	int mode = SetBkMode(dc, TRANSPARENT);
	if (!IsWindowEnabled(window))
		SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
	DrawTextW(dc, text, -1, &measure, format & ~(UINT)(DT_CENTER | DT_RIGHT));
	SetBkMode(dc, mode);
	if (text != small_text)
		HeapFree(GetProcessHeap(), 0, text);
}

static void paint_owner(HWND window, HDC dc, const RECT *client, button_state *state, UINT action)
{
	DRAWITEMSTRUCT item = {ODT_BUTTON, (UINT)GetDlgCtrlID(window), 0, action, 0, window, dc, *client, 0};
	if (state->highlighted)
		item.itemState |= ODS_SELECTED;
	if (state->focused)
		item.itemState |= ODS_FOCUS;
	if (!IsWindowEnabled(window))
		item.itemState |= ODS_DISABLED;
	if (state->check == BST_CHECKED)
		item.itemState |= ODS_CHECKED;
	if (state->hide_focus)
		item.itemState |= ODS_NOFOCUSRECT;
	if (state->hide_prefix)
		item.itemState |= ODS_NOACCEL;
	parent_brush(window, dc, WM_CTLCOLORBTN);
	SendMessageW(GetParent(window), WM_DRAWITEM, item.CtlID, (LPARAM)&item);
}

static void paint(HWND window, HDC dc, button_state *state, UINT action)
{
	DWORD style = (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
	UINT kind = kind_of(style);
	RECT client;
	GetClientRect(window, &client);
	HGDIOBJ previous = state->font ? SelectObject(dc, state->font) : NULL;
	int saved = SaveDC(dc);
	IntersectClipRect(dc, client.left, client.top, client.right, client.bottom);
	SetBkColor(dc, GetSysColor(COLOR_3DFACE));
	SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
	if (is_push_kind(kind) || (is_check_kind(kind) && (style & BS_PUSHLIKE))) {
		paint_push(window, dc, style, is_check_kind(kind) ? BS_PUSHBUTTON : kind, &client, state);
		kind = BS_PUSHBUTTON;
	}
	switch (kind) {
	case BS_PUSHBUTTON:
		break;
	case BS_GROUPBOX:
		paint_group(window, dc, style, &client, state);
		break;
	case BS_OWNERDRAW:
		paint_owner(window, dc, &client, state, action);
		break;
	case BS_USERBUTTON:
		SendMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(window), BN_PAINT), (LPARAM)window);
		break;
	default:
		paint_check(window, dc, style, kind, &client, state);
		break;
	}
	RestoreDC(dc, saved);
	if (previous)
		SelectObject(dc, previous);
}

static void redraw(HWND window, button_state *state, UINT action)
{
	if (!IsWindowVisible(window))
		return;
	HDC dc = GetDC(window);
	if (dc) {
		paint(window, dc, state, action);
		ReleaseDC(window, dc);
	}
}

static void set_highlight(HWND window, button_state *state, BOOL highlighted)
{
	if (state->highlighted == highlighted)
		return;
	state->highlighted = highlighted;
	redraw(window, state, ODA_SELECT);
	if ((DWORD)GetWindowLongPtrW(window, GWL_STYLE) & BS_NOTIFY)
		notify(window, highlighted ? BN_HILITE : BN_UNHILITE);
}

static void set_check(HWND window, button_state *state, UINT value)
{
	UINT kind = kind_of((DWORD)GetWindowLongPtrW(window, GWL_STYLE));
	if (is_radio_kind(kind))
		value = value ? BST_CHECKED : BST_UNCHECKED;
	else if (kind != BS_3STATE && kind != BS_AUTO3STATE)
		value = value ? BST_CHECKED : BST_UNCHECKED;
	else if (value > BST_INDETERMINATE)
		value = BST_INDETERMINATE;
	if (state->check == value)
		return;
	state->check = value;
	/* A checked radio button is what tabbing lands on in its group. */
	if (is_radio_kind(kind)) {
		LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
		SetWindowLongPtrW(window, GWL_STYLE, value ? style | WS_TABSTOP : style & ~(LONG_PTR)WS_TABSTOP);
	}
	redraw(window, state, ODA_SELECT);
}

/* An automatic radio button clears the rest of its group, which runs from one WS_GROUP sibling to the next. */
static void uncheck_group(HWND window)
{
	HWND first = window;
	while (!(GetWindowLongPtrW(first, GWL_STYLE) & WS_GROUP)) {
		HWND previous = GetWindow(first, GW_HWNDPREV);
		if (!previous)
			break;
		first = previous;
	}
	for (HWND sibling = first; sibling; sibling = GetWindow(sibling, GW_HWNDNEXT)) {
		if (sibling != first && (GetWindowLongPtrW(sibling, GWL_STYLE) & WS_GROUP))
			break;
		if (sibling != window && (SendMessageW(sibling, WM_GETDLGCODE, 0, 0) & DLGC_RADIOBUTTON))
			SendMessageW(sibling, BM_SETCHECK, BST_UNCHECKED, 0);
	}
}

static void click(HWND window, button_state *state)
{
	UINT kind = kind_of((DWORD)GetWindowLongPtrW(window, GWL_STYLE));
	switch (kind) {
	case BS_AUTOCHECKBOX:
		set_check(window, state, state->check == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED);
		break;
	case BS_AUTO3STATE:
		set_check(window, state, (state->check + 1) % 3);
		break;
	case BS_AUTORADIOBUTTON:
		set_check(window, state, BST_CHECKED);
		uncheck_group(window);
		break;
	}
	notify(window, BN_CLICKED);
}

LRESULT WINAPI RavenButtonProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	button_state *state = state_of(window);
	if (!state && message != WM_NCCREATE)
		return DefWindowProcW(window, message, wparam, lparam);
	DWORD style = (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
	UINT kind = kind_of(style);
	switch (message) {
	case WM_NCCREATE:
		state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
		if (!state)
			return FALSE;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		/* The obsolete user button lives on as a push button. */
		if (kind == BS_USERBUTTON)
			SetWindowLongPtrW(window, GWL_STYLE, (style & ~(DWORD)BS_TYPEMASK) | BS_PUSHBUTTON);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_NCDESTROY:
		HeapFree(GetProcessHeap(), 0, state);
		SetWindowLongPtrW(window, 0, 0);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_GETDLGCODE:
		if (kind == BS_SPLITBUTTON)
			return DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON | DLGC_WANTARROWS;
		if (kind == BS_DEFSPLITBUTTON)
			return DLGC_BUTTON | DLGC_DEFPUSHBUTTON | DLGC_WANTARROWS;
		if (is_default_kind(kind))
			return DLGC_BUTTON | DLGC_DEFPUSHBUTTON;
		if (is_push_kind(kind))
			return DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON;
		if (is_radio_kind(kind))
			return DLGC_BUTTON | DLGC_RADIOBUTTON;
		return kind == BS_GROUPBOX ? DLGC_STATIC : DLGC_BUTTON;
	case WM_NCHITTEST:
		return kind == BS_GROUPBOX ? HTTRANSPARENT : DefWindowProcW(window, message, wparam, lparam);
	case WM_ERASEBKGND:
		/* Painting covers every other kind, but an owner may draw less than the whole button. */
		if (kind == BS_OWNERDRAW) {
			RECT client;
			GetClientRect(window, &client);
			FillRect((HDC)wparam, &client, parent_brush(window, (HDC)wparam, WM_CTLCOLORBTN));
		}
		return 1;
	case WM_CHAR:
		if (is_box_kind(kind)) {
			if (wparam == L'+' || wparam == L'=') {
				SendMessageW(window, BM_SETCHECK, BST_CHECKED, 0);
				return 0;
			}
			if (wparam == L'-') {
				SendMessageW(window, BM_SETCHECK, BST_UNCHECKED, 0);
				return 0;
			}
		}
		break;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = wparam ? (HDC)wparam : BeginPaint(window, &ps);
		paint(window, dc, state, ODA_DRAWENTIRE);
		if (!wparam)
			EndPaint(window, &ps);
		return 0;
	}
	case WM_PRINTCLIENT:
		paint(window, (HDC)wparam, state, ODA_DRAWENTIRE);
		return 0;
	case WM_ENABLE:
		InvalidateRect(window, NULL, TRUE);
		if (!wparam && (style & BS_NOTIFY))
			notify(window, BN_DISABLE);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_SETFOCUS:
		state->focused = TRUE;
		redraw(window, state, ODA_FOCUS);
		if (style & BS_NOTIFY)
			notify(window, BN_SETFOCUS);
		/* Arrowing onto a radio button selects it; a mouse press is still waiting for its release. */
		if (is_radio_kind(kind) && !state->captured && state->check == BST_UNCHECKED)
			click(window, state);
		return 0;
	case WM_KILLFOCUS:
		state->focused = FALSE;
		if (state->captured) {
			state->captured = FALSE;
			ReleaseCapture();
		}
		state->highlighted = FALSE;
		state->down_by_key = FALSE;
		redraw(window, state, ODA_FOCUS);
		if (style & BS_NOTIFY)
			notify(window, BN_KILLFOCUS);
		return 0;
	case WM_LBUTTONDBLCLK:
		if ((style & BS_NOTIFY) || is_radio_kind(kind) || kind == BS_OWNERDRAW || kind == BS_USERBUTTON) {
			notify(window, BN_DOUBLECLICKED);
			return 0;
		}
		/* Without notifications a double click is one more click. */
		/* fall through */
	case WM_LBUTTONDOWN:
		SetCapture(window);
		state->captured = TRUE;
		SetFocus(window);
		set_highlight(window, state, TRUE);
		return 0;
	case WM_MOUSEMOVE:
		if (state->captured) {
			POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
			RECT client;
			GetClientRect(window, &client);
			set_highlight(window, state, PtInRect(&client, point));
		}
		return 0;
	case WM_LBUTTONUP:
		if (state->captured) {
			POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
			RECT client;
			GetClientRect(window, &client);
			BOOL inside = PtInRect(&client, point);
			state->captured = FALSE;
			set_highlight(window, state, FALSE);
			ReleaseCapture();
			if (inside && !state->no_click)
				click(window, state);
		}
		return 0;
	case WM_CAPTURECHANGED:
		if (state->captured) {
			state->captured = FALSE;
			set_highlight(window, state, FALSE);
		}
		return 0;
	case WM_KEYDOWN:
		if (wparam == VK_SPACE && !(lparam & 0x40000000)) {
			state->down_by_key = TRUE;
			set_highlight(window, state, TRUE);
			return 0;
		}
		break;
	case WM_KEYUP:
		if (wparam == VK_SPACE && state->down_by_key) {
			state->down_by_key = FALSE;
			set_highlight(window, state, FALSE);
			click(window, state);
			return 0;
		}
		/* Any other key but tab cancels a press in progress without a click. */
		if (wparam != VK_TAB && state->down_by_key) {
			state->down_by_key = FALSE;
			set_highlight(window, state, FALSE);
		}
		break;
	case WM_SYSKEYUP:
		if (wparam != VK_TAB && state->captured) {
			state->captured = FALSE;
			set_highlight(window, state, FALSE);
			ReleaseCapture();
		}
		break;
	case WM_SETTEXT: {
		LRESULT result = DefWindowProcW(window, message, wparam, lparam);
		InvalidateRect(window, NULL, TRUE);
		return result;
	}
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		if (lparam)
			InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_UPDATEUISTATE: {
		LRESULT result = DefWindowProcW(window, message, wparam, lparam);
		LRESULT cues = SendMessageW(window, WM_QUERYUISTATE, 0, 0);
		state->hide_focus = (cues & UISF_HIDEFOCUS) != 0;
		state->hide_prefix = (cues & UISF_HIDEACCEL) != 0;
		InvalidateRect(window, NULL, TRUE);
		return result;
	}
	case BM_GETCHECK:
		return is_check_kind(kind) ? state->check : BST_UNCHECKED;
	case BM_SETCHECK:
		if (is_check_kind(kind))
			set_check(window, state, (UINT)wparam);
		return 0;
	case BM_GETSTATE:
		return state->check | (state->highlighted ? BST_PUSHED : 0) | (state->focused ? BST_FOCUS : 0);
	case BM_SETSTATE:
		set_highlight(window, state, wparam != 0);
		return 0;
	case BM_GETIMAGE:
		return (wparam == IMAGE_ICON) == state->image_is_icon ? (LRESULT)state->image : 0;
	case BM_SETIMAGE: {
		if (wparam != IMAGE_BITMAP && wparam != IMAGE_ICON)
			return 0;
		HANDLE previous = state->image;
		state->image = (HANDLE)lparam;
		state->image_is_icon = wparam == IMAGE_ICON;
		InvalidateRect(window, NULL, TRUE);
		return (LRESULT)previous;
	}
	case BM_SETSTYLE: {
		/* The type changes with the look: a dialog moves the default this way. */
		LONG_PTR updated = (style & ~(DWORD)BS_TYPEMASK) | (wparam & BS_TYPEMASK);
		SetWindowLongPtrW(window, GWL_STYLE, updated);
		if (lparam)
			InvalidateRect(window, NULL, TRUE);
		return 0;
	}
	case BM_CLICK:
		if (!IsWindowEnabled(window))
			return 0;
		SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, 0);
		SendMessageW(window, WM_LBUTTONUP, 0, 0);
		return 0;
	case BM_SETDONTCLICK:
		state->no_click = wparam != 0;
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}
