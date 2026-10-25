/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The ScrollBar control: a scroll bar of its own window (SBS_HORZ or SBS_VERT), or a size box, which tells its parent of every move
 * through WM_HSCROLL and WM_VSCROLL. Its range, page and position are set through the SBM_ messages, which SetScrollInfo and the other
 * scroll bar functions send for SB_CTL.
 *
 * This is Raven code on the public Windows headers. */
#include <user32.h>

#define LINE_TIMER 0x7f00

typedef struct {
	SCROLLINFO info;
	UINT arrows;   /* ESB_DISABLE_* flags set through SBM_ENABLE_ARROWS */
	int pressed;   /* the part the button went down on: one of the HT values below, or 0 */
	int drag_offset;
	BOOL in_part;
} ScrollState;

enum { PART_NONE, PART_LOW_ARROW, PART_HIGH_ARROW, PART_LOW_PAGE, PART_HIGH_PAGE, PART_THUMB };

static ScrollState *state_of(HWND window)
{
	return (ScrollState *)GetWindowLongPtrW(window, 0);
}

static BOOL vertical(HWND window)
{
	return (GetWindowLongW(window, GWL_STYLE) & SBS_VERT) != 0;
}

static int highest_position(const ScrollState *state)
{
	const int page = state->info.nPage ? (int)state->info.nPage - 1 : 0;
	return max(state->info.nMin, state->info.nMax - page);
}

/* The bar's parts along its length: the two arrows, the track between them and the thumb on it. */
typedef struct {
	RECT bar, low_arrow, high_arrow;
	int track_start, track_end, thumb_start, thumb_end;
	BOOL has_thumb;
} Layout;

static Layout layout_of(HWND window, const ScrollState *state)
{
	Layout layout;
	const BOOL down = vertical(window);
	GetClientRect(window, &layout.bar);
	const int length = down ? layout.bar.bottom : layout.bar.right;
	const int breadth = down ? layout.bar.right : layout.bar.bottom;
	int arrow = min(down ? GetSystemMetrics(SM_CYVSCROLL) : GetSystemMetrics(SM_CXHSCROLL), length / 2);
	if (arrow < 0)
		arrow = 0;
	layout.low_arrow = layout.bar;
	layout.high_arrow = layout.bar;
	if (down) {
		layout.low_arrow.bottom = arrow;
		layout.high_arrow.top = length - arrow;
	} else {
		layout.low_arrow.right = arrow;
		layout.high_arrow.left = length - arrow;
	}
	layout.track_start = arrow;
	layout.track_end = length - arrow;
	const int track = layout.track_end - layout.track_start;
	const int range = state->info.nMax - state->info.nMin + 1;
	const int smallest = max(breadth / 2, 8);
	int thumb = state->info.nPage && range > 0 ? MulDiv(track, (int)state->info.nPage, range) : (down ? GetSystemMetrics(SM_CYVTHUMB) : GetSystemMetrics(SM_CXHTHUMB));
	thumb = max(thumb, smallest);
	layout.has_thumb = IsWindowEnabled(window) && track >= thumb && state->info.nMax > state->info.nMin &&
		(!state->info.nPage || (int)state->info.nPage <= state->info.nMax - state->info.nMin) && (state->arrows & ESB_DISABLE_BOTH) != ESB_DISABLE_BOTH;
	const int positions = highest_position(state) - state->info.nMin;
	const int shown = state->pressed == PART_THUMB ? state->info.nTrackPos : state->info.nPos;
	const int offset = positions > 0 ? MulDiv(track - thumb, shown - state->info.nMin, positions) : 0;
	layout.thumb_start = layout.track_start + max(0, min(offset, track - thumb));
	layout.thumb_end = layout.thumb_start + thumb;
	return layout;
}

static RECT span(HWND window, const Layout *layout, int from, int to)
{
	RECT part = layout->bar;
	if (vertical(window)) {
		part.top = from;
		part.bottom = to;
	} else {
		part.left = from;
		part.right = to;
	}
	return part;
}

static int hit(HWND window, const ScrollState *state, POINT point)
{
	const Layout layout = layout_of(window, state);
	const int along = vertical(window) ? point.y : point.x;
	if (!PtInRect(&layout.bar, point))
		return PART_NONE;
	if (along < layout.track_start)
		return PART_LOW_ARROW;
	if (along >= layout.track_end)
		return PART_HIGH_ARROW;
	if (!layout.has_thumb)
		return PART_NONE;
	if (along < layout.thumb_start)
		return PART_LOW_PAGE;
	if (along >= layout.thumb_end)
		return PART_HIGH_PAGE;
	return PART_THUMB;
}

static void paint(HWND window, HDC dc, const ScrollState *state)
{
	const LONG style = GetWindowLongW(window, GWL_STYLE);
	RECT client;
	GetClientRect(window, &client);
	if (style & (SBS_SIZEBOX | SBS_SIZEGRIP)) {
		FillRect(dc, &client, GetSysColorBrush(COLOR_BTNFACE));
		if (style & SBS_SIZEGRIP)
			DrawFrameControl(dc, &client, DFC_SCROLL, DFCS_SCROLLSIZEGRIP);
		return;
	}
	const Layout layout = layout_of(window, state);
	const BOOL down = vertical(window), enabled = IsWindowEnabled(window);
	const BOOL low_off = !enabled || (state->arrows & ESB_DISABLE_LTUP) || !layout.has_thumb;
	const BOOL high_off = !enabled || (state->arrows & ESB_DISABLE_RTDN) || !layout.has_thumb;
	RECT low = layout.low_arrow, high = layout.high_arrow;
	DrawFrameControl(dc, &low, DFC_SCROLL, (down ? DFCS_SCROLLUP : DFCS_SCROLLLEFT) | (low_off ? DFCS_INACTIVE : 0) |
		(state->pressed == PART_LOW_ARROW && state->in_part ? DFCS_PUSHED | DFCS_FLAT : 0));
	DrawFrameControl(dc, &high, DFC_SCROLL, (down ? DFCS_SCROLLDOWN : DFCS_SCROLLRIGHT) | (high_off ? DFCS_INACTIVE : 0) |
		(state->pressed == PART_HIGH_ARROW && state->in_part ? DFCS_PUSHED | DFCS_FLAT : 0));
	/* The track in the scroll bar colour, darker where a page click is held, and the thumb raised on it. */
	RECT track = span(window, &layout, layout.track_start, layout.track_end);
	FillRect(dc, &track, GetSysColorBrush(COLOR_SCROLLBAR));
	if (!layout.has_thumb)
		return;
	if (state->in_part && (state->pressed == PART_LOW_PAGE || state->pressed == PART_HIGH_PAGE)) {
		RECT held = state->pressed == PART_LOW_PAGE ? span(window, &layout, layout.track_start, layout.thumb_start) :
			span(window, &layout, layout.thumb_end, layout.track_end);
		FillRect(dc, &held, GetSysColorBrush(COLOR_3DDKSHADOW));
	}
	RECT thumb = span(window, &layout, layout.thumb_start, layout.thumb_end);
	FillRect(dc, &thumb, GetSysColorBrush(COLOR_BTNFACE));
	DrawEdge(dc, &thumb, EDGE_RAISED, BF_RECT);
}

static void notify(HWND window, UINT code, int position)
{
	const HWND parent = GetParent(window) ? GetParent(window) : GetWindow(window, GW_OWNER);
	if (parent)
		SendMessageW(parent, vertical(window) ? WM_VSCROLL : WM_HSCROLL, MAKEWPARAM(code, (WORD)position), (LPARAM)window);
}

/* What a held part does once: the line and page moves the parent makes. */
static void act(HWND window, int part)
{
	static const UINT codes[] = {0, SB_LINEUP, SB_LINEDOWN, SB_PAGEUP, SB_PAGEDOWN};
	if (part >= PART_LOW_ARROW && part <= PART_HIGH_PAGE)
		notify(window, codes[part], 0);
}

static int set_info(HWND window, ScrollState *state, const SCROLLINFO *info, BOOL redraw)
{
	if (info->fMask & SIF_RANGE) {
		state->info.nMin = info->nMin;
		state->info.nMax = max(info->nMin, info->nMax);
	}
	if (info->fMask & SIF_PAGE)
		state->info.nPage = min(info->nPage, (UINT)(state->info.nMax - state->info.nMin + 1));
	if (info->fMask & SIF_POS)
		state->info.nPos = info->nPos;
	state->info.nPos = max(state->info.nMin, min(state->info.nPos, highest_position(state)));
	state->info.nTrackPos = state->info.nPos;
	if (redraw)
		InvalidateRect(window, NULL, TRUE);
	return state->info.nPos;
}

static LRESULT procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam, BOOL unicode)
{
	ScrollState *state = state_of(window);
	if (!state && message != WM_NCCREATE && message != WM_CREATE)
		return unicode ? DefWindowProcW(window, message, wparam, lparam) : DefWindowProcA(window, message, wparam, lparam);
	switch (message) {
	case WM_NCCREATE:
		state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *state);
		if (!state)
			return FALSE;
		state->info.cbSize = sizeof state->info;
		state->info.nMax = 100;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		break;
	case WM_NCDESTROY:
		SetWindowLongPtrW(window, 0, 0);
		HeapFree(GetProcessHeap(), 0, state);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
	case WM_PRINTCLIENT: {
		PAINTSTRUCT paint_info;
		HDC dc = wparam ? (HDC)wparam : BeginPaint(window, &paint_info);
		paint(window, dc, state);
		if (!wparam)
			EndPaint(window, &paint_info);
		return 0;
	}
	case WM_ENABLE:
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK: {
		const POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		if (GetWindowLongW(window, GWL_STYLE) & (SBS_SIZEBOX | SBS_SIZEGRIP))
			return 0;
		state->pressed = hit(window, state, point);
		state->in_part = TRUE;
		if (state->pressed == PART_NONE)
			return 0;
		SetCapture(window);
		if (GetWindowLongW(window, GWL_STYLE) & WS_TABSTOP)
			SetFocus(window);
		if (state->pressed == PART_THUMB) {
			const Layout layout = layout_of(window, state);
			state->drag_offset = (vertical(window) ? point.y : point.x) - layout.thumb_start;
			state->info.nTrackPos = state->info.nPos;
		} else {
			act(window, state->pressed);
			SetTimer(window, LINE_TIMER, 400, NULL);
		}
		InvalidateRect(window, NULL, TRUE);
		return 0;
	}
	case WM_MOUSEMOVE:
		if (state->pressed == PART_THUMB) {
			const POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
			const Layout layout = layout_of(window, state);
			const int room = (layout.track_end - layout.track_start) - (layout.thumb_end - layout.thumb_start);
			const int along = (vertical(window) ? point.y : point.x) - state->drag_offset - layout.track_start;
			const int positions = highest_position(state) - state->info.nMin;
			const int position = room > 0 ? state->info.nMin + MulDiv(max(0, min(along, room)), positions, room) : state->info.nMin;
			if (position != state->info.nTrackPos) {
				state->info.nTrackPos = position;
				notify(window, SB_THUMBTRACK, position);
				InvalidateRect(window, NULL, TRUE);
			}
		} else if (state->pressed) {
			const POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
			const BOOL inside = hit(window, state, point) == state->pressed;
			if (inside != state->in_part) {
				state->in_part = inside;
				InvalidateRect(window, NULL, TRUE);
			}
		}
		return 0;
	case WM_TIMER:
		if (wparam == LINE_TIMER && state->pressed && state->pressed != PART_THUMB) {
			POINT point;
			GetCursorPos(&point);
			ScreenToClient(window, &point);
			if (hit(window, state, point) == state->pressed)
				act(window, state->pressed);
			SetTimer(window, LINE_TIMER, 50, NULL);
		}
		return 0;
	case WM_LBUTTONUP:
	case WM_CAPTURECHANGED:
		if (state->pressed) {
			const int part = state->pressed;
			state->pressed = PART_NONE;
			KillTimer(window, LINE_TIMER);
			if (message == WM_LBUTTONUP)
				ReleaseCapture();
			if (part == PART_THUMB)
				notify(window, SB_THUMBPOSITION, state->info.nTrackPos);
			notify(window, SB_ENDSCROLL, 0);
			InvalidateRect(window, NULL, TRUE);
		}
		return 0;
	case WM_GETDLGCODE:
		return DLGC_WANTARROWS;
	case WM_KEYDOWN: {
		UINT code;
		switch (wparam) {
		case VK_UP: case VK_LEFT: code = SB_LINEUP; break;
		case VK_DOWN: case VK_RIGHT: code = SB_LINEDOWN; break;
		case VK_PRIOR: code = SB_PAGEUP; break;
		case VK_NEXT: code = SB_PAGEDOWN; break;
		case VK_HOME: code = SB_TOP; break;
		case VK_END: code = SB_BOTTOM; break;
		default: return 0;
		}
		notify(window, code, 0);
		notify(window, SB_ENDSCROLL, 0);
		return 0;
	}
	case SBM_SETPOS: {
		const int previous = state->info.nPos;
		SCROLLINFO info = {sizeof info, SIF_POS};
		info.nPos = (int)wparam;
		set_info(window, state, &info, lparam != 0);
		return previous;
	}
	case SBM_GETPOS:
		return state->info.nPos;
	case SBM_SETRANGE:
	case SBM_SETRANGEREDRAW: {
		const int previous = state->info.nPos;
		SCROLLINFO info = {sizeof info, SIF_RANGE};
		info.nMin = (int)wparam;
		info.nMax = (int)lparam;
		set_info(window, state, &info, message == SBM_SETRANGEREDRAW);
		return previous;
	}
	case SBM_GETRANGE:
		if (wparam)
			*(LPINT)wparam = state->info.nMin;
		if (lparam)
			*(LPINT)lparam = state->info.nMax;
		return 0;
	case SBM_ENABLE_ARROWS:
		state->arrows = (UINT)wparam;
		InvalidateRect(window, NULL, TRUE);
		return TRUE;
	case SBM_SETSCROLLINFO:
		if (!lparam)
			return 0;
		return set_info(window, state, (const SCROLLINFO *)lparam, wparam != 0);
	case SBM_GETSCROLLINFO: {
		SCROLLINFO *info = (SCROLLINFO *)lparam;
		if (!info)
			return FALSE;
		if (info->fMask & SIF_RANGE) {
			info->nMin = state->info.nMin;
			info->nMax = state->info.nMax;
		}
		if (info->fMask & SIF_PAGE)
			info->nPage = state->info.nPage;
		if (info->fMask & SIF_POS)
			info->nPos = state->info.nPos;
		if (info->fMask & SIF_TRACKPOS)
			info->nTrackPos = state->info.nTrackPos;
		return TRUE;
	}
	case SBM_GETSCROLLBARINFO: {
		SCROLLBARINFO *info = (SCROLLBARINFO *)lparam;
		if (!info || info->cbSize != sizeof *info)
			return FALSE;
		const Layout layout = layout_of(window, state);
		GetWindowRect(window, &info->rcScrollBar);
		info->dxyLineButton = layout.track_start;
		info->xyThumbTop = layout.thumb_start;
		info->xyThumbBottom = layout.thumb_end;
		ZeroMemory(info->rgstate, sizeof info->rgstate);
		if (!IsWindowEnabled(window))
			info->rgstate[0] = STATE_SYSTEM_UNAVAILABLE;
		return TRUE;
	}
	}
	return unicode ? DefWindowProcW(window, message, wparam, lparam) : DefWindowProcA(window, message, wparam, lparam);
}

LRESULT WINAPI RavenScrollBarProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return procedure(window, message, wparam, lparam, TRUE);
}

LRESULT WINAPI RavenScrollBarProcA(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return procedure(window, message, wparam, lparam, FALSE);
}
