/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>

/* The progress bar. A change of position repaints at once, as setup programs that never pump messages still expect to be seen moving. */

#define DEFAULT_MARQUEE_MS 30
#define MARQUEE_TIMER 1
#define MARQUEE_CHUNKS 5
#define CHUNK_GAP 2

typedef struct {
	int low, high, position, step;
	COLORREF bar, background;
	HFONT font;
	int state;
	int marquee;
	BOOL marquee_on;
} progress_state;

static progress_state *state_of(HWND window)
{
	return (progress_state *)GetWindowLongPtrW(window, 0);
}

static DWORD style_of(HWND window)
{
	return (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
}

static int clamped(const progress_state *state, int value)
{
	if (value < state->low)
		return state->low;
	return value > state->high ? state->high : value;
}

static void repaint(HWND window, BOOL erase)
{
	InvalidateRect(window, NULL, erase);
	UpdateWindow(window);
}

static LRESULT move_to(HWND window, progress_state *state, int position)
{
	int previous = state->position;
	if (position != previous) {
		state->position = position;
		/* Growing draws over what is there; only shrinking has anything to wipe. */
		repaint(window, position < previous);
	}
	return previous;
}

static COLORREF bar_color(const progress_state *state)
{
	if (state->bar != CLR_DEFAULT)
		return state->bar;
	if (state->state == PBST_ERROR)
		return RGB(200, 40, 40);
	return state->state == PBST_PAUSED ? RGB(220, 180, 30) : GetSysColor(COLOR_HIGHLIGHT);
}

static void fill(HDC dc, int left, int top, int right, int bottom, COLORREF color)
{
	RECT rect = {left, top, right, bottom};
	SetBkColor(dc, color);
	ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rect, NULL, 0, NULL);
}

static void paint(HWND window, progress_state *state, HDC dc)
{
	RECT client;
	GetClientRect(window, &client);
	DWORD style = style_of(window);
	BOOL vertical = (style & PBS_VERTICAL) != 0, smooth = (style & PBS_SMOOTH) != 0;
	int length = vertical ? client.bottom : client.right, breadth = vertical ? client.right : client.bottom;
	int chunk = smooth ? length : (vertical ? client.right : client.bottom) * 2 / 3 + CHUNK_GAP;
	if (chunk < 1 + CHUNK_GAP)
		chunk = 1 + CHUNK_GAP;
	int from = 0, to;
	if (style & PBS_MARQUEE) {
		int span = MARQUEE_CHUNKS * chunk;
		from = length ? state->marquee % (length + span) - span : 0;
		to = from + span;
	} else {
		int range = state->high - state->low;
		to = range > 0 ? MulDiv(state->position - state->low, length, range) : 0;
	}
	COLORREF bar = bar_color(state);
	COLORREF background = state->background == CLR_DEFAULT ? GetSysColor(COLOR_3DFACE) : state->background;
	int saved = SaveDC(dc);
	/* Filled from the bottom when vertical, so x and y swap and the far end flips. */
	#define BAND(a, b, color) (vertical ? fill(dc, 0, length - (b), breadth, length - (a), color) : fill(dc, a, 0, b, breadth, color))
	int at = from > 0 ? from : 0;
	if (at > 0)
		BAND(0, at, background);
	if (smooth) {
		if (to > at)
			BAND(at, to, bar);
		if (to < length)
			BAND(to > at ? to : at, length, background);
	} else {
		int cursor = (style & PBS_MARQUEE) ? from : 0;
		for (; cursor < to && cursor < length; cursor += chunk) {
			int solid_end = cursor + chunk - CHUNK_GAP;
			if (solid_end > 0)
				BAND(cursor > 0 ? cursor : 0, solid_end < length ? solid_end : length, bar);
			if (solid_end + CHUNK_GAP > 0 && solid_end < length)
				BAND(solid_end > 0 ? solid_end : 0, solid_end + CHUNK_GAP < length ? solid_end + CHUNK_GAP : length, background);
		}
		if (cursor < length)
			BAND(cursor > 0 ? cursor : 0, length, background);
	}
	#undef BAND
	RestoreDC(dc, saved);
}

static LRESULT WINAPI progress_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	progress_state *state = state_of(window);
	if (!state && message != WM_NCCREATE)
		return DefWindowProcW(window, message, wparam, lparam);
	switch (message) {
	case WM_NCCREATE: {
		state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
		if (!state)
			return FALSE;
		state->high = 100;
		state->step = 10;
		state->bar = state->background = CLR_DEFAULT;
		state->state = PBST_NORMAL;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		/* The sunken edge belongs to the control, whatever edge it was created with. */
		DWORD extended = (DWORD)GetWindowLongPtrW(window, GWL_EXSTYLE);
		SetWindowLongPtrW(window, GWL_EXSTYLE, (extended & ~(DWORD)WS_EX_CLIENTEDGE) | WS_EX_STATICEDGE);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_NCDESTROY:
		HeapFree(GetProcessHeap(), 0, state);
		SetWindowLongPtrW(window, 0, 0);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_ERASEBKGND:
		/* Painting covers the whole client area. */
		return 1;
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
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		if (lparam)
			InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_TIMER:
		if (wparam == MARQUEE_TIMER) {
			state->marquee += 4;
			repaint(window, FALSE);
		}
		return 0;
	case PBM_DELTAPOS:
		return move_to(window, state, clamped(state, state->position + (int)wparam));
	case PBM_SETPOS:
		return move_to(window, state, clamped(state, (int)wparam));
	case PBM_GETPOS:
		return state->position;
	case PBM_SETRANGE:
	case PBM_SETRANGE32: {
		LRESULT previous = MAKELONG(state->low, state->high);
		state->low = message == PBM_SETRANGE ? (int)LOWORD(lparam) : (int)wparam;
		state->high = message == PBM_SETRANGE ? (int)HIWORD(lparam) : (int)lparam;
		if (state->position < state->low || state->position > state->high) {
			int position = clamped(state, state->position);
			move_to(window, state, position);
		} else
			InvalidateRect(window, NULL, TRUE);
		return previous;
	}
	case PBM_GETRANGE:
		if (lparam) {
			((PBRANGE *)lparam)->iLow = state->low;
			((PBRANGE *)lparam)->iHigh = state->high;
		}
		return wparam ? state->low : state->high;
	case PBM_SETSTEP: {
		int previous = state->step;
		state->step = (int)wparam;
		return previous;
	}
	case PBM_GETSTEP:
		return state->step;
	case PBM_STEPIT: {
		/* Passing either end carries on from the other. */
		int position = state->position + state->step, span = state->high - state->low;
		if (span <= 0)
			position = state->low;
		else if (position > state->high)
			position = (position - state->low) % span + state->low;
		else if (position < state->low)
			position = (position - state->low) % span + state->high;
		return move_to(window, state, clamped(state, position));
	}
	case PBM_SETBARCOLOR: {
		COLORREF previous = state->bar;
		state->bar = (COLORREF)lparam;
		InvalidateRect(window, NULL, TRUE);
		return previous;
	}
	case PBM_GETBARCOLOR:
		return state->bar;
	case PBM_SETBKCOLOR: {
		COLORREF previous = state->background;
		state->background = (COLORREF)lparam;
		InvalidateRect(window, NULL, TRUE);
		return previous;
	}
	case PBM_GETBKCOLOR:
		return state->background;
	case PBM_SETSTATE: {
		int previous = state->state;
		if (wparam == PBST_NORMAL || wparam == PBST_ERROR || wparam == PBST_PAUSED) {
			state->state = (int)wparam;
			InvalidateRect(window, NULL, FALSE);
		}
		return previous;
	}
	case PBM_GETSTATE:
		return state->state;
	case PBM_SETMARQUEE:
		state->marquee_on = wparam != 0;
		if (wparam)
			SetTimer(window, MARQUEE_TIMER, lparam ? (UINT)lparam : DEFAULT_MARQUEE_MS, NULL);
		else
			KillTimer(window, MARQUEE_TIMER);
		return wparam != 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void PROGRESS_Register(void)
{
	WNDCLASSW definition = {0};
	definition.style = CS_GLOBALCLASS | CS_VREDRAW | CS_HREDRAW;
	definition.lpfnWndProc = progress_procedure;
	definition.cbWndExtra = sizeof(void *);
	definition.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	definition.lpszClassName = PROGRESS_CLASSW;
	RegisterClassW(&definition);
}

void PROGRESS_Unregister(void)
{
	UnregisterClassW(PROGRESS_CLASSW, NULL);
}
