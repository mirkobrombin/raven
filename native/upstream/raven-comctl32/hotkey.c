/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>

/* The hot key control: it shows the key combination being pressed and refuses, by its rules, the ones it should not take. */

typedef struct {
	HWND owner;
	HFONT font;
	WORD key;
	BYTE modifiers;
	WORD forbidden;
	BYTE fallback;
	BOOL focused;
} hotkey_state;

static hotkey_state *state_of(HWND window)
{
	return (hotkey_state *)GetWindowLongPtrW(window, 0);
}

/* The rule that a set of modifiers falls under. */
static WORD rule_of(BYTE modifiers)
{
	BOOL shift = (modifiers & HOTKEYF_SHIFT) != 0, control = (modifiers & HOTKEYF_CONTROL) != 0, alt = (modifiers & HOTKEYF_ALT) != 0;
	if (shift && control && alt)
		return HKCOMB_SCA;
	if (shift && control)
		return HKCOMB_SC;
	if (shift && alt)
		return HKCOMB_SA;
	if (control && alt)
		return HKCOMB_CA;
	if (shift)
		return HKCOMB_S;
	if (control)
		return HKCOMB_C;
	return alt ? HKCOMB_A : HKCOMB_NONE;
}

static BYTE allowed_modifiers(const hotkey_state *state, BYTE modifiers)
{
	return state->forbidden & rule_of(modifiers) ? state->fallback : modifiers;
}

static BYTE held_modifiers(void)
{
	BYTE held = 0;
	if (GetKeyState(VK_SHIFT) < 0)
		held |= HOTKEYF_SHIFT;
	if (GetKeyState(VK_CONTROL) < 0)
		held |= HOTKEYF_CONTROL;
	if (GetKeyState(VK_MENU) < 0)
		held |= HOTKEYF_ALT;
	return held;
}

static void append_name(WCHAR *text, int capacity, UINT virtual_key, BOOL extended)
{
	int used = lstrlenW(text);
	LONG code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC) << 16 | (extended ? 1 << 24 : 0);
	if (used && used + 3 < capacity) {
		lstrcatW(text, L" + ");
		used += 3;
	}
	if (used < capacity - 1 && !GetKeyNameTextW(code, text + used, capacity - used) && virtual_key >= 'A' && virtual_key <= 'Z' && used + 1 < capacity) {
		text[used] = (WCHAR)virtual_key;
		text[used + 1] = 0;
	}
}

static void describe(const hotkey_state *state, WCHAR *text, int capacity)
{
	text[0] = 0;
	if (state->modifiers & HOTKEYF_CONTROL)
		append_name(text, capacity, VK_CONTROL, FALSE);
	if (state->modifiers & HOTKEYF_SHIFT)
		append_name(text, capacity, VK_SHIFT, FALSE);
	if (state->modifiers & HOTKEYF_ALT)
		append_name(text, capacity, VK_MENU, FALSE);
	if (state->key)
		append_name(text, capacity, state->key, (state->modifiers & HOTKEYF_EXT) != 0);
	if (!text[0] && capacity > 4)
		lstrcpyW(text, L"None");
}

static void place_caret(HWND window, const hotkey_state *state)
{
	if (!state->focused)
		return;
	WCHAR text[128];
	describe(state, text, 128);
	SIZE size = {0, 0};
	HDC dc = GetDC(window);
	if (dc) {
		HGDIOBJ previous = state->font ? SelectObject(dc, state->font) : NULL;
		GetTextExtentPoint32W(dc, text, lstrlenW(text), &size);
		if (previous)
			SelectObject(dc, previous);
		ReleaseDC(window, dc);
	}
	SetCaretPos(size.cx + 2, 1);
}

static void changed(HWND window, hotkey_state *state)
{
	InvalidateRect(window, NULL, TRUE);
	place_caret(window, state);
	if (state->owner)
		SendMessageW(state->owner, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(window), EN_CHANGE), (LPARAM)window);
}

static void paint(HWND window, hotkey_state *state, HDC dc)
{
	RECT client;
	GetClientRect(window, &client);
	BOOL enabled = IsWindowEnabled(window);
	FillRect(dc, &client, GetSysColorBrush(enabled ? COLOR_WINDOW : COLOR_3DFACE));
	WCHAR text[128];
	describe(state, text, 128);
	HGDIOBJ previous = state->font ? SelectObject(dc, state->font) : NULL;
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, GetSysColor(enabled ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT));
	TextOutW(dc, 2, 1, text, lstrlenW(text));
	if (previous)
		SelectObject(dc, previous);
}

static BOOL is_modifier(WPARAM key)
{
	return key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU;
}

static LRESULT WINAPI hotkey_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	hotkey_state *state = state_of(window);
	if (!state && message != WM_NCCREATE)
		return DefWindowProcW(window, message, wparam, lparam);
	switch (message) {
	case WM_NCCREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
		if (!state)
			return FALSE;
		state->owner = create->hwndParent;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) | WS_EX_CLIENTEDGE);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_NCDESTROY:
		HeapFree(GetProcessHeap(), 0, state);
		SetWindowLongPtrW(window, 0, 0);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = wparam ? (HDC)wparam : BeginPaint(window, &ps);
		paint(window, state, dc);
		if (!wparam)
			EndPaint(window, &ps);
		return 0;
	}
	case WM_ENABLE:
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		if (lparam)
			InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_GETDLGCODE:
		return DLGC_WANTCHARS | DLGC_WANTARROWS;
	case WM_SETFOCUS:
		state->focused = TRUE;
		CreateCaret(window, NULL, 1, 14);
		place_caret(window, state);
		ShowCaret(window);
		return 0;
	case WM_KILLFOCUS:
		state->focused = FALSE;
		DestroyCaret();
		return 0;
	case WM_LBUTTONDOWN:
		SetFocus(window);
		return 0;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		/* Tab and the like stay with the dialog; plain backspace, space and delete clear the control. */
		if (message == WM_KEYDOWN && !held_modifiers() && (wparam == VK_BACK || wparam == VK_SPACE || wparam == VK_DELETE || wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB)) {
			if (wparam == VK_BACK || wparam == VK_SPACE || wparam == VK_DELETE) {
				state->key = 0;
				state->modifiers = 0;
				changed(window, state);
			}
			return DefWindowProcW(window, message, wparam, lparam);
		}
		state->modifiers = allowed_modifiers(state, held_modifiers());
		if (lparam & 0x1000000)
			state->modifiers |= HOTKEYF_EXT;
		state->key = is_modifier(wparam) ? 0 : (WORD)wparam;
		changed(window, state);
		return 0;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		/* Letting go of the modifiers with no key leaves nothing chosen. */
		if (!state->key) {
			state->modifiers = allowed_modifiers(state, held_modifiers());
			InvalidateRect(window, NULL, TRUE);
			place_caret(window, state);
		}
		return 0;
	case WM_SYSCHAR:
		return 0;
	case HKM_SETHOTKEY:
		state->key = LOBYTE(LOWORD(wparam));
		state->modifiers = allowed_modifiers(state, HIBYTE(LOWORD(wparam)));
		InvalidateRect(window, NULL, TRUE);
		place_caret(window, state);
		return 0;
	case HKM_GETHOTKEY:
		return MAKEWORD(state->key, state->modifiers);
	case HKM_SETRULES:
		state->forbidden = (WORD)wparam;
		state->fallback = (BYTE)LOWORD(lparam);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void HOTKEY_Register(void)
{
	WNDCLASSW definition = {0};
	definition.style = CS_GLOBALCLASS;
	definition.lpfnWndProc = hotkey_procedure;
	definition.cbWndExtra = sizeof(void *);
	definition.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	definition.lpszClassName = HOTKEY_CLASSW;
	RegisterClassW(&definition);
}

void HOTKEY_Unregister(void)
{
	UnregisterClassW(HOTKEY_CLASSW, NULL);
}
