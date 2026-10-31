/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>

/* Four edit fields in one window, with dots painted in the gaps. Field text is authoritative: an empty field reads as zero and is blank. */

#define FIELDS 4
#define FIELD_DIGITS 3
#define SUBCLASS_ID 1

typedef struct {
	HWND window, parent;
	HWND edits[FIELDS];
	BYTE low[FIELDS], high[FIELDS];
	HFONT font;
	BOOL unicode;
} ipaddress_state;

static LRESULT CALLBACK field_procedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

static ipaddress_state *state_of(HWND window)
{
	return (ipaddress_state *)GetWindowLongPtrW(window, 0);
}

static int field_value(const ipaddress_state *state, int field)
{
	WCHAR text[FIELD_DIGITS + 1];
	int value = 0, index;
	if (!GetWindowTextW(state->edits[field], text, ARRAYSIZE(text)))
		return -1;
	for (index = 0; text[index]; ++index)
		value = value * 10 + (text[index] - L'0');
	return value;
}

static int index_of_field(const ipaddress_state *state, HWND edit)
{
	int field;
	for (field = 0; field < FIELDS; ++field)
		if (state->edits[field] == edit)
			return field;
	return -1;
}

static void set_field(const ipaddress_state *state, int field, int value)
{
	WCHAR text[FIELD_DIGITS + 1];
	if (value < 0)
		text[0] = 0;
	else
		wsprintfW(text, L"%d", value);
	SetWindowTextW(state->edits[field], text);
}

static void tell_parent(const ipaddress_state *state, WORD code)
{
	SendMessageW(state->parent, WM_COMMAND, MAKEWPARAM(GetWindowLongPtrW(state->window, GWLP_ID), code), (LPARAM)state->window);
}

/* Leaving a field applies its range and lets the program see, and rewrite, the value. */
static void settle_field(const ipaddress_state *state, int field)
{
	NMIPADDRESS notice;
	int value = field_value(state, field), settled = value;
	if (value >= 0) {
		if (value < state->low[field])
			settled = state->low[field];
		else if (value > state->high[field])
			settled = state->high[field];
	}
	notice.hdr.hwndFrom = state->window;
	notice.hdr.idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	notice.hdr.code = IPN_FIELDCHANGED;
	notice.iField = field;
	notice.iValue = settled;
	SendMessageW(state->parent, WM_NOTIFY, notice.hdr.idFrom, (LPARAM)&notice);
	settled = notice.iValue;
	if (settled >= 0 && settled != value)
		set_field(state, field, min(settled, 255));
}

static void focus_field(const ipaddress_state *state, int field)
{
	SetFocus(state->edits[field]);
	SendMessageW(state->edits[field], EM_SETSEL, 0, -1);
}

static int first_blank_or_zero(const ipaddress_state *state)
{
	int field;
	for (field = 0; field < FIELDS; ++field)
		if (field_value(state, field) < 0)
			return field;
	return 0;
}

static int read_address(const ipaddress_state *state, DWORD *address)
{
	int field, filled = 0;
	DWORD total = 0;
	for (field = 0; field < FIELDS; ++field) {
		int value = field_value(state, field);
		if (value >= 0)
			++filled;
		total = (total << 8) | (DWORD)(value < 0 ? 0 : min(value, 255));
	}
	*address = total;
	return filled;
}

static int format_address(const ipaddress_state *state, WCHAR *text)
{
	DWORD address;
	read_address(state, &address);
	return wsprintfW(text, L"%d.%d.%d.%d", FIRST_IPADDRESS(address), SECOND_IPADDRESS(address), THIRD_IPADDRESS(address), FOURTH_IPADDRESS(address));
}

static void layout(const ipaddress_state *state)
{
	RECT area;
	int field, width;
	GetClientRect(state->window, &area);
	width = area.right / FIELDS;
	for (field = 0; field < FIELDS; ++field)
		MoveWindow(state->edits[field], field * width + 1, 1, max(width - 6, 0), max((int)area.bottom - 2, 0), TRUE);
}

static void paint(const ipaddress_state *state, HDC dc)
{
	RECT area, dot;
	int field, width;
	GetClientRect(state->window, &area);
	FillRect(dc, &area, GetSysColorBrush(IsWindowEnabled(state->window) ? COLOR_WINDOW : COLOR_3DFACE));
	width = area.right / FIELDS;
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, GetSysColor(IsWindowEnabled(state->window) ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT));
	SelectObject(dc, state->font ? state->font : GetStockObject(DEFAULT_GUI_FONT));
	for (field = 0; field + 1 < FIELDS; ++field) {
		dot.left = (field + 1) * width - 6;
		dot.right = (field + 1) * width + 1;
		dot.top = area.top;
		dot.bottom = area.bottom;
		DrawTextW(dc, L".", 1, &dot, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}
}

static LRESULT CALLBACK field_procedure(HWND edit, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data)
{
	ipaddress_state *state = (ipaddress_state *)data;
	int field = index_of_field(state, edit);
	switch (message) {
	case WM_CHAR:
		if (wparam == L'.') {
			if (field + 1 < FIELDS && GetWindowTextLengthW(edit))
				focus_field(state, field + 1);
			return 0;
		}
		if (wparam >= L'0' && wparam <= L'9') {
			LRESULT result = DefSubclassProc(edit, message, wparam, lparam);
			if (GetWindowTextLengthW(edit) == FIELD_DIGITS && field + 1 < FIELDS)
				focus_field(state, field + 1);
			return result;
		}
		if (wparam == VK_BACK && !LOWORD(SendMessageW(edit, EM_GETSEL, 0, 0)) && !HIWORD(SendMessageW(edit, EM_GETSEL, 0, 0)) && field > 0) {
			SetFocus(state->edits[field - 1]);
			SendMessageW(state->edits[field - 1], EM_SETSEL, GetWindowTextLengthW(state->edits[field - 1]), -1);
			return 0;
		}
		if (wparam >= L' ')
			return 0;
		break;
	case WM_KEYDOWN: {
		DWORD selection = (DWORD)SendMessageW(edit, EM_GETSEL, 0, 0);
		int length = GetWindowTextLengthW(edit);
		if (wparam == VK_LEFT && !LOWORD(selection) && !HIWORD(selection) && field > 0) {
			SetFocus(state->edits[field - 1]);
			SendMessageW(state->edits[field - 1], EM_SETSEL, GetWindowTextLengthW(state->edits[field - 1]), -1);
			return 0;
		}
		if (wparam == VK_RIGHT && LOWORD(selection) == length && HIWORD(selection) == length && field + 1 < FIELDS) {
			SetFocus(state->edits[field + 1]);
			SendMessageW(state->edits[field + 1], EM_SETSEL, 0, 0);
			return 0;
		}
		break;
	}
	case WM_KILLFOCUS:
		settle_field(state, field);
		break;
	case WM_NCDESTROY:
		RemoveWindowSubclass(edit, field_procedure, id);
		break;
	}
	return DefSubclassProc(edit, message, wparam, lparam);
}

static LRESULT CALLBACK ipaddress_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	ipaddress_state *state = state_of(window);
	if (!state && message != WM_NCCREATE)
		return DefWindowProcW(window, message, wparam, lparam);
	switch (message) {
	case WM_NCCREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
		if (!state)
			return FALSE;
		state->window = window;
		state->parent = create->hwndParent;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_CREATE: {
		int field;
		state->unicode = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_UNICODE;
		for (field = 0; field < FIELDS; ++field) {
			state->high[field] = 255;
			state->edits[field] = CreateWindowExW(0, L"EDIT", NULL, WS_CHILD | WS_VISIBLE | ES_CENTER | ES_AUTOHSCROLL, 0, 0, 0, 0, window, NULL, NULL, NULL);
			if (!state->edits[field])
				return -1;
			SendMessageW(state->edits[field], EM_LIMITTEXT, FIELD_DIGITS, 0);
			SetWindowSubclass(state->edits[field], field_procedure, SUBCLASS_ID, (DWORD_PTR)state);
		}
		layout(state);
		return 0;
	}
	case WM_NCDESTROY:
		HeapFree(GetProcessHeap(), 0, state);
		SetWindowLongPtrW(window, 0, 0);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_SIZE:
		layout(state);
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
	case WM_SETFOCUS:
		focus_field(state, first_blank_or_zero(state));
		return 0;
	case WM_SETFONT: {
		int field;
		state->font = (HFONT)wparam;
		for (field = 0; field < FIELDS; ++field)
			SendMessageW(state->edits[field], WM_SETFONT, wparam, lparam);
		return 0;
	}
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_ENABLE: {
		int field;
		for (field = 0; field < FIELDS; ++field)
			EnableWindow(state->edits[field], (BOOL)wparam);
		InvalidateRect(window, NULL, TRUE);
		return 0;
	}
	case WM_COMMAND: {
		WORD code = HIWORD(wparam);
		if (code == EN_CHANGE || code == EN_SETFOCUS || code == EN_KILLFOCUS)
			tell_parent(state, code);
		return 0;
	}
	case WM_GETTEXTLENGTH: {
		WCHAR text[16];
		return format_address(state, text);
	}
	case WM_GETTEXT: {
		WCHAR text[16];
		int length = format_address(state, text);
		if (!wparam)
			return 0;
		length = min(length, (int)wparam - 1);
		memcpy((void *)lparam, text, length * sizeof(WCHAR));
		((WCHAR *)lparam)[length] = 0;
		return length;
	}
	case IPM_CLEARADDRESS: {
		int field;
		for (field = 0; field < FIELDS; ++field)
			set_field(state, field, -1);
		return TRUE;
	}
	case IPM_SETADDRESS: {
		DWORD address = (DWORD)lparam;
		int field;
		for (field = 0; field < FIELDS; ++field)
			set_field(state, field, (address >> (8 * (FIELDS - 1 - field))) & 255);
		return TRUE;
	}
	case IPM_GETADDRESS: {
		DWORD address;
		int filled = read_address(state, &address);
		if (lparam)
			*(DWORD *)lparam = address;
		return filled;
	}
	case IPM_SETRANGE:
		if (wparam >= FIELDS)
			return FALSE;
		state->low[wparam] = LOBYTE(LOWORD(lparam));
		state->high[wparam] = HIBYTE(LOWORD(lparam));
		return TRUE;
	case IPM_SETFOCUS:
		focus_field(state, wparam < FIELDS ? (int)wparam : first_blank_or_zero(state));
		return TRUE;
	case IPM_ISBLANK: {
		DWORD address;
		return !read_address(state, &address);
	}
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void IPADDRESS_Register(void)
{
	WNDCLASSW definition = {0};
	definition.style = CS_GLOBALCLASS | CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS;
	definition.lpfnWndProc = ipaddress_procedure;
	definition.cbWndExtra = sizeof(void *);
	definition.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_IBEAM);
	definition.lpszClassName = WC_IPADDRESSW;
	RegisterClassW(&definition);
}

void IPADDRESS_Unregister(void)
{
	UnregisterClassW(WC_IPADDRESSW, NULL);
}
