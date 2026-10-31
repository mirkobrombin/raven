/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>

/* Flat scroll bars were a look of their time. The documented fallback, the standard scroll bar, is what every program gets here. */

BOOL WINAPI InitializeFlatSB(HWND window)
{
	return TRUE;
}

HRESULT WINAPI UninitializeFlatSB(HWND window)
{
	return S_FALSE;
}

BOOL WINAPI FlatSB_GetScrollProp(HWND window, int property, LPINT value)
{
	if (value)
		*value = 0;
	return FALSE;
}

BOOL WINAPI FlatSB_SetScrollProp(HWND window, UINT property, INT_PTR value, BOOL redraw)
{
	return FALSE;
}

BOOL WINAPI FlatSB_EnableScrollBar(HWND window, int bar, UINT arrows)
{
	return EnableScrollBar(window, bar, arrows);
}

BOOL WINAPI FlatSB_ShowScrollBar(HWND window, int bar, BOOL show)
{
	return ShowScrollBar(window, bar, show);
}

BOOL WINAPI FlatSB_GetScrollRange(HWND window, int bar, LPINT low, LPINT high)
{
	return GetScrollRange(window, bar, low, high);
}

BOOL WINAPI FlatSB_GetScrollInfo(HWND window, int bar, LPSCROLLINFO info)
{
	return GetScrollInfo(window, bar, info);
}

int WINAPI FlatSB_GetScrollPos(HWND window, int bar)
{
	return GetScrollPos(window, bar);
}

int WINAPI FlatSB_SetScrollPos(HWND window, int bar, int position, BOOL redraw)
{
	return SetScrollPos(window, bar, position, redraw);
}

int WINAPI FlatSB_SetScrollInfo(HWND window, int bar, LPSCROLLINFO info, BOOL redraw)
{
	return SetScrollInfo(window, bar, info, redraw);
}

int WINAPI FlatSB_SetScrollRange(HWND window, int bar, int low, int high, BOOL redraw)
{
	return SetScrollRange(window, bar, low, high, redraw);
}

void FLATSB_Register(void)
{
}

void FLATSB_Unregister(void)
{
}

/* The native font control wraps a child and only passes on what it hears; it stays invisible in every program that still creates one. */
static LRESULT CALLBACK native_font_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message) {
	case WM_SETFONT:
	case WM_COMMAND:
	case WM_NOTIFY:
		if (message != WM_SETFONT)
			return SendMessageW(GetParent(window), message, wparam, lparam);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void NATIVEFONT_Register(void)
{
	WNDCLASSW definition = {0};
	definition.style = CS_GLOBALCLASS;
	definition.lpfnWndProc = native_font_procedure;
	definition.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	definition.lpszClassName = WC_NATIVEFONTCTLW;
	RegisterClassW(&definition);
}

void NATIVEFONT_Unregister(void)
{
	UnregisterClassW(WC_NATIVEFONTCTLW, NULL);
}
