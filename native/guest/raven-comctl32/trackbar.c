/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>

/* The trackbar. Geometry is worked out in "along" and "across" terms and only flipped for TBS_VERT when it reaches the window, so one
 * code path serves both orientations. The thumb rectangle is cached: changes made with the redraw flag off keep the thumb where it is. */

#ifndef TBM_SETPOSNOTIFY
#define TBM_SETPOSNOTIFY (WM_USER + 34)
#endif

#define THUMB_ALONG 11
#define DEFAULT_THUMB_LENGTH 21
#define CHANNEL_THICKNESS 4
#define TIC_LENGTH 3
#define EDGE_GAP 2
#define REPEAT_TIMER 1
#define FIRST_REPEAT_MS 500
#define NEXT_REPEAT_MS 100

enum { DRAG_NONE, DRAG_THUMB, DRAG_PAGE };

typedef struct {
	HWND window, parent;
	HWND left_buddy, right_buddy, tooltip;
	int low, high, position, line, page;
	BOOL page_automatic;
	int select_start, select_end;
	int frequency, thumb_length, tip_side;
	int *tics;
	int tic_count;
	RECT thumb, channel;
	int drag, drag_offset, page_step;
	BOOL unicode, hot;
	HTHEME theme;
} trackbar_state;

static trackbar_state *state_of(HWND window)
{
	return (trackbar_state *)GetWindowLongPtrW(window, 0);
}

static DWORD style_of(const trackbar_state *state)
{
	return (DWORD)GetWindowLongPtrW(state->window, GWL_STYLE);
}

static BOOL vertical(const trackbar_state *state)
{
	return (style_of(state) & TBS_VERT) != 0;
}

/* Hands out a rectangle given along/across, or takes a horizontal one and turns it for TBS_VERT. */
static RECT oriented(const trackbar_state *state, int along_start, int along_end, int across_start, int across_end)
{
	RECT area;
	if (vertical(state))
		SetRect(&area, across_start, along_start, across_end, along_end);
	else
		SetRect(&area, along_start, across_start, along_end, across_end);
	return area;
}

static void client_lengths(const trackbar_state *state, int *along, int *across)
{
	RECT client;
	GetClientRect(state->window, &client);
	*along = vertical(state) ? client.bottom : client.right;
	*across = vertical(state) ? client.right : client.bottom;
}

static int thumb_across(const trackbar_state *state)
{
	int along, across;
	if (style_of(state) & TBS_FIXEDLENGTH)
		return state->thumb_length;
	client_lengths(state, &along, &across);
	return min(state->thumb_length, max(across - 2 * EDGE_GAP, 1));
}

static int pixel_of(const trackbar_state *state, int position)
{
	int span = state->high - state->low;
	int start = (vertical(state) ? state->channel.top : state->channel.left) + THUMB_ALONG / 2;
	int length = (vertical(state) ? state->channel.bottom - state->channel.top : state->channel.right - state->channel.left) - THUMB_ALONG;
	if (!span)
		return start;
	return start + MulDiv(position - state->low, max(length, 0), span);
}

static int position_at(const trackbar_state *state, int pixel)
{
	int span = state->high - state->low;
	int start = (vertical(state) ? state->channel.top : state->channel.left) + THUMB_ALONG / 2;
	int length = (vertical(state) ? state->channel.bottom - state->channel.top : state->channel.right - state->channel.left) - THUMB_ALONG;
	if (length <= 0)
		return state->low;
	return state->low + MulDiv(pixel - start, span, length);
}

static void clamp_position(trackbar_state *state)
{
	if (state->position > state->high)
		state->position = state->high;
	if (state->position < state->low)
		state->position = state->low;
}

static void compute_channel(trackbar_state *state)
{
	DWORD style = style_of(state);
	int along, across, thickness = thumb_across(state), at;
	client_lengths(state, &along, &across);
	if (style & TBS_BOTH)
		at = (across - CHANNEL_THICKNESS) / 2;
	else if (style & TBS_TOP)
		at = across - thickness / 2 - CHANNEL_THICKNESS / 2 - EDGE_GAP;
	else
		at = thickness / 2 - CHANNEL_THICKNESS / 2 + EDGE_GAP;
	state->channel = oriented(state, EDGE_GAP, along - EDGE_GAP, at, at + CHANNEL_THICKNESS);
}

static void compute_thumb(trackbar_state *state)
{
	int center = pixel_of(state, state->position), length = thumb_across(state);
	int channel_center = vertical(state) ? (state->channel.left + state->channel.right) / 2 : (state->channel.top + state->channel.bottom) / 2;
	state->thumb = oriented(state, center - THUMB_ALONG / 2, center - THUMB_ALONG / 2 + THUMB_ALONG, channel_center - length / 2, channel_center - length / 2 + length);
}

static void refresh(trackbar_state *state, BOOL redraw)
{
	if (!redraw)
		return;
	compute_thumb(state);
	InvalidateRect(state->window, NULL, TRUE);
}

static void rebuild_automatic_tics(trackbar_state *state)
{
	int value, count = 0, capacity;
	int *tics;
	if (!(style_of(state) & TBS_AUTOTICKS) || state->frequency <= 0)
		return;
	capacity = state->high > state->low ? (state->high - state->low) / state->frequency + 1 : 0;
	tics = capacity ? HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(int)) : NULL;
	if (capacity && !tics)
		return;
	for (value = state->low + state->frequency; value < state->high; value += state->frequency)
		tics[count++] = value;
	HeapFree(GetProcessHeap(), 0, state->tics);
	state->tics = tics;
	state->tic_count = count;
}

static BOOL add_tic(trackbar_state *state, int value)
{
	int *grown, at;
	if (value < min(state->low, state->high) || value > max(state->low, state->high))
		return FALSE;
	grown = state->tics ? HeapReAlloc(GetProcessHeap(), 0, state->tics, (state->tic_count + 1) * sizeof(int))
		: HeapAlloc(GetProcessHeap(), 0, sizeof(int));
	if (!grown)
		return FALSE;
	state->tics = grown;
	for (at = state->tic_count; at > 0 && grown[at - 1] > value; --at)
		grown[at] = grown[at - 1];
	grown[at] = value;
	++state->tic_count;
	return TRUE;
}

static void range_changed(trackbar_state *state, BOOL redraw)
{
	clamp_position(state);
	rebuild_automatic_tics(state);
	if (state->page_automatic)
		state->page = (state->high - state->low) / 5;
	refresh(state, redraw);
}

static void place_buddy(const trackbar_state *state, HWND buddy, BOOL before)
{
	RECT own, other;
	HWND parent;
	if (!buddy)
		return;
	parent = GetParent(buddy);
	GetWindowRect(state->window, &own);
	GetWindowRect(buddy, &other);
	MapWindowPoints(HWND_DESKTOP, parent, (POINT *)&own, 2);
	if (vertical(state)) {
		int x = own.left + (own.right - own.left - (other.right - other.left)) / 2;
		int y = before ? own.top - (other.bottom - other.top) : own.bottom;
		SetWindowPos(buddy, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	} else {
		int y = own.top + (own.bottom - own.top - (other.bottom - other.top)) / 2;
		int x = before ? own.left - (other.right - other.left) : own.right;
		SetWindowPos(buddy, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	}
}

static void tell_parent(const trackbar_state *state, WORD code, BOOL with_position)
{
	SendMessageW(state->parent, vertical(state) ? WM_VSCROLL : WM_HSCROLL,
		MAKEWPARAM(code, with_position ? (WORD)state->position : 0), (LPARAM)state->window);
}

static void update_tooltip(trackbar_state *state, BOOL show)
{
	TTTOOLINFOW tool = {sizeof(tool)};
	WCHAR text[16];
	POINT point;
	if (!state->tooltip)
		return;
	tool.hwnd = state->window;
	tool.uId = (UINT_PTR)state->window;
	if (!show) {
		SendMessageW(state->tooltip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&tool);
		return;
	}
	wsprintfW(text, L"%d", state->position);
	tool.lpszText = text;
	SendMessageW(state->tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&tool);
	point.x = state->thumb.right + 2;
	point.y = state->thumb.bottom + 2;
	ClientToScreen(state->window, &point);
	SendMessageW(state->tooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(point.x, point.y));
	SendMessageW(state->tooltip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&tool);
}

static void move_to(trackbar_state *state, int position, WORD code)
{
	int previous = state->position;
	state->position = position;
	clamp_position(state);
	if (state->position == previous)
		return;
	compute_thumb(state);
	InvalidateRect(state->window, NULL, TRUE);
	UpdateWindow(state->window);
	tell_parent(state, code, code == TB_THUMBTRACK || code == TB_THUMBPOSITION);
}

static BOOL key_step(trackbar_state *state, WPARAM key)
{
	BOOL inverted = (style_of(state) & TBS_DOWNISLEFT) != 0;
	int position = state->position;
	WORD code;
	switch (key) {
	case VK_LEFT:
	case VK_UP:
		code = inverted ? TB_LINEDOWN : TB_LINEUP;
		position += inverted ? state->line : -state->line;
		break;
	case VK_RIGHT:
	case VK_DOWN:
		code = inverted ? TB_LINEUP : TB_LINEDOWN;
		position += inverted ? -state->line : state->line;
		break;
	case VK_PRIOR:
		code = inverted ? TB_PAGEDOWN : TB_PAGEUP;
		position += inverted ? state->page : -state->page;
		break;
	case VK_NEXT:
		code = inverted ? TB_PAGEUP : TB_PAGEDOWN;
		position += inverted ? -state->page : state->page;
		break;
	case VK_HOME:
		code = inverted ? TB_BOTTOM : TB_TOP;
		position = inverted ? state->high : state->low;
		break;
	case VK_END:
		code = inverted ? TB_TOP : TB_BOTTOM;
		position = inverted ? state->low : state->high;
		break;
	default:
		return FALSE;
	}
	move_to(state, position, code);
	tell_parent(state, TB_ENDTRACK, FALSE);
	return TRUE;
}

static void draw_channel(trackbar_state *state, HDC dc)
{
	RECT area = state->channel;
	DWORD style = style_of(state);
	if (state->theme)
		DrawThemeBackground(state->theme, dc, vertical(state) ? TKP_TRACKVERT : TKP_TRACK, TKS_NORMAL, &area, NULL);
	else
		DrawEdge(dc, &area, EDGE_SUNKEN, BF_RECT | BF_MIDDLE);
	if ((style & TBS_ENABLESELRANGE) && state->select_end > state->select_start) {
		int from = pixel_of(state, state->select_start), to = pixel_of(state, state->select_end);
		RECT selected = vertical(state) ? (RECT){area.left + 1, from, area.right - 1, to} : (RECT){from, area.top + 1, to, area.bottom - 1};
		FillRect(dc, &selected, GetSysColorBrush(COLOR_HIGHLIGHT));
	}
}

static void draw_one_tic(trackbar_state *state, HDC dc, int position, BOOL ends)
{
	DWORD style = style_of(state);
	int center = pixel_of(state, position), length = ends ? TIC_LENGTH + 1 : TIC_LENGTH;
	HBRUSH brush = GetSysColorBrush(COLOR_BTNTEXT);
	RECT mark;
	if (!(style & TBS_TOP) || (style & TBS_BOTH)) {
		int at = vertical(state) ? state->thumb.right : state->thumb.bottom;
		mark = oriented(state, center, center + 1, at + 1, at + 1 + length);
		FillRect(dc, &mark, brush);
	}
	if (style & (TBS_TOP | TBS_BOTH)) {
		int at = vertical(state) ? state->thumb.left : state->thumb.top;
		mark = oriented(state, center, center + 1, at - 1 - length, at - 1);
		FillRect(dc, &mark, brush);
	}
}

static void draw_tics(trackbar_state *state, HDC dc)
{
	int index;
	if (style_of(state) & TBS_NOTICKS)
		return;
	draw_one_tic(state, dc, state->low, TRUE);
	draw_one_tic(state, dc, state->high, TRUE);
	for (index = 0; index < state->tic_count; ++index)
		draw_one_tic(state, dc, state->tics[index], FALSE);
}

static void draw_thumb(trackbar_state *state, HDC dc)
{
	DWORD style = style_of(state);
	BOOL enabled = IsWindowEnabled(state->window);
	if (style & TBS_NOTHUMB)
		return;
	if (state->theme) {
		int part = vertical(state) ? ((style & TBS_BOTH) ? TKP_THUMBVERT : (style & TBS_LEFT) ? TKP_THUMBLEFT : TKP_THUMBRIGHT)
			: ((style & TBS_BOTH) ? TKP_THUMB : (style & TBS_TOP) ? TKP_THUMBTOP : TKP_THUMBBOTTOM);
		int theme_state = !enabled ? TUS_DISABLED : state->drag == DRAG_THUMB ? TUS_PRESSED : state->hot ? TUS_HOT : TUS_NORMAL;
		DrawThemeBackground(state->theme, dc, part, theme_state, &state->thumb, NULL);
		return;
	}
	DrawEdge(dc, &state->thumb, EDGE_RAISED, BF_RECT | BF_MIDDLE | BF_SOFT);
}

static LRESULT custom_draw(trackbar_state *state, NMCUSTOMDRAW *draw, DWORD stage, DWORD_PTR item, const RECT *area)
{
	draw->dwDrawStage = stage;
	draw->dwItemSpec = item;
	if (area)
		draw->rc = *area;
	return SendMessageW(state->parent, WM_NOTIFY, draw->hdr.idFrom, (LPARAM)draw);
}

static void draw_item(trackbar_state *state, HDC dc, NMCUSTOMDRAW *draw, BOOL wants_items, DWORD_PTR item, const RECT *area, void (*painter)(trackbar_state *, HDC))
{
	LRESULT answer = wants_items ? custom_draw(state, draw, CDDS_ITEMPREPAINT, item, area) : CDRF_DODEFAULT;
	if (!(answer & CDRF_SKIPDEFAULT))
		painter(state, dc);
	if (answer & CDRF_NOTIFYPOSTPAINT)
		custom_draw(state, draw, CDDS_ITEMPOSTPAINT, item, area);
}

static void paint(trackbar_state *state, HDC dc)
{
	NMCUSTOMDRAW draw = {0};
	RECT client;
	LRESULT answer;
	GetClientRect(state->window, &client);
	if (state->theme && IsThemeBackgroundPartiallyTransparent(state->theme, TKP_TRACK, TKS_NORMAL))
		DrawThemeParentBackground(state->window, dc, &client);
	else
		FillRect(dc, &client, GetSysColorBrush(COLOR_3DFACE));
	draw.hdr.hwndFrom = state->window;
	draw.hdr.idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	draw.hdr.code = NM_CUSTOMDRAW;
	draw.hdc = dc;
	answer = custom_draw(state, &draw, CDDS_PREPAINT, 0, &client);
	if (answer & CDRF_SKIPDEFAULT)
		return;
	BOOL items = (answer & CDRF_NOTIFYITEMDRAW) != 0;
	draw_item(state, dc, &draw, items, TBCD_TICS, &client, draw_tics);
	draw_item(state, dc, &draw, items, TBCD_CHANNEL, &state->channel, draw_channel);
	draw_item(state, dc, &draw, items, TBCD_THUMB, &state->thumb, draw_thumb);
	if (GetFocus() == state->window && !(SendMessageW(state->window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
		DrawFocusRect(dc, &client);
	if (answer & CDRF_NOTIFYPOSTPAINT)
		custom_draw(state, &draw, CDDS_POSTPAINT, 0, &client);
}

static void page_toward(trackbar_state *state, int cursor)
{
	int here = pixel_of(state, state->position);
	if (cursor < here - THUMB_ALONG / 2)
		move_to(state, state->position - state->page, TB_PAGEUP);
	else if (cursor > here + THUMB_ALONG / 2)
		move_to(state, state->position + state->page, TB_PAGEDOWN);
}

static LRESULT CALLBACK trackbar_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	trackbar_state *state = state_of(window);
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
		state->high = 100;
		state->line = 1;
		state->page = 20;
		state->page_automatic = TRUE;
		state->frequency = 1;
		state->thumb_length = DEFAULT_THUMB_LENGTH;
		state->tip_side = TBTS_TOP;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_CREATE:
		state->unicode = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_UNICODE;
		state->theme = OpenThemeData(window, L"Trackbar");
		compute_channel(state);
		compute_thumb(state);
		if (GetWindowLongPtrW(window, GWL_STYLE) & TBS_TOOLTIPS) {
			TTTOOLINFOW tool = {sizeof(tool)};
			state->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX, 0, 0, 0, 0, window, NULL, NULL, NULL);
			tool.uFlags = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
			tool.hwnd = window;
			tool.uId = (UINT_PTR)window;
			tool.lpszText = L"";
			if (state->tooltip)
				SendMessageW(state->tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool);
		}
		return 0;
	case WM_DESTROY:
		CloseThemeData(state->theme);
		state->theme = NULL;
		return 0;
	case WM_NCDESTROY:
		HeapFree(GetProcessHeap(), 0, state->tics);
		HeapFree(GetProcessHeap(), 0, state);
		SetWindowLongPtrW(window, 0, 0);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_THEMECHANGED:
		CloseThemeData(state->theme);
		state->theme = OpenThemeData(window, L"Trackbar");
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_SIZE:
		compute_channel(state);
		compute_thumb(state);
		place_buddy(state, state->left_buddy, TRUE);
		place_buddy(state, state->right_buddy, FALSE);
		return 0;
	case WM_ENABLE:
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_GETDLGCODE:
		return DLGC_WANTARROWS;
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
	case WM_KEYDOWN:
		key_step(state, wparam);
		return 0;
	case WM_LBUTTONDOWN: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		int cursor = vertical(state) ? point.y : point.x;
		SetFocus(window);
		SetCapture(window);
		if (!(style_of(state) & TBS_NOTHUMB) && PtInRect(&state->thumb, point)) {
			state->drag = DRAG_THUMB;
			state->drag_offset = cursor - pixel_of(state, state->position);
			update_tooltip(state, TRUE);
			InvalidateRect(window, NULL, TRUE);
		} else {
			state->drag = DRAG_PAGE;
			page_toward(state, cursor);
			SetTimer(window, REPEAT_TIMER, FIRST_REPEAT_MS, NULL);
		}
		return 0;
	}
	case WM_MOUSEMOVE: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		if (state->drag == DRAG_THUMB) {
			int cursor = (vertical(state) ? point.y : point.x) - state->drag_offset;
			move_to(state, position_at(state, cursor), TB_THUMBTRACK);
			update_tooltip(state, TRUE);
		} else if (PtInRect(&state->thumb, point) != state->hot) {
			TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
			state->hot = !state->hot;
			TrackMouseEvent(&track);
			InvalidateRect(window, &state->thumb, FALSE);
		}
		return 0;
	}
	case WM_MOUSELEAVE:
		state->hot = FALSE;
		InvalidateRect(window, &state->thumb, FALSE);
		return 0;
	case WM_TIMER:
		if (wparam == REPEAT_TIMER && state->drag == DRAG_PAGE) {
			POINT point;
			GetCursorPos(&point);
			ScreenToClient(window, &point);
			SetTimer(window, REPEAT_TIMER, NEXT_REPEAT_MS, NULL);
			page_toward(state, vertical(state) ? point.y : point.x);
		}
		return 0;
	case WM_LBUTTONUP:
	case WM_CAPTURECHANGED: {
		int was = state->drag;
		if (!was)
			return 0;
		state->drag = DRAG_NONE;
		KillTimer(window, REPEAT_TIMER);
		if (message == WM_LBUTTONUP && GetCapture() == window)
			ReleaseCapture();
		if (was == DRAG_THUMB)
			tell_parent(state, TB_THUMBPOSITION, TRUE);
		tell_parent(state, TB_ENDTRACK, FALSE);
		update_tooltip(state, FALSE);
		InvalidateRect(window, NULL, TRUE);
		return 0;
	}
	case TBM_GETPOS:
		return state->position;
	case TBM_SETPOS:
		state->position = (int)lparam;
		clamp_position(state);
		refresh(state, wparam != 0);
		return 0;
	case TBM_SETPOSNOTIFY: {
		int previous = state->position;
		state->position = (int)lparam;
		clamp_position(state);
		refresh(state, TRUE);
		if (previous != state->position)
			tell_parent(state, TB_THUMBPOSITION, TRUE);
		return 0;
	}
	case TBM_GETRANGEMIN:
		return state->low;
	case TBM_GETRANGEMAX:
		return state->high;
	case TBM_SETRANGE:
		state->low = (short)LOWORD(lparam);
		state->high = (short)HIWORD(lparam);
		range_changed(state, wparam != 0);
		return 0;
	case TBM_SETRANGEMIN:
		state->low = (int)lparam;
		range_changed(state, wparam != 0);
		return 0;
	case TBM_SETRANGEMAX:
		state->high = (int)lparam;
		range_changed(state, wparam != 0);
		return 0;
	case TBM_GETLINESIZE:
		return state->line;
	case TBM_SETLINESIZE: {
		int previous = state->line;
		state->line = (int)lparam;
		return previous;
	}
	case TBM_GETPAGESIZE:
		return state->page;
	case TBM_SETPAGESIZE: {
		int previous = state->page;
		state->page_automatic = (int)lparam == -1;
		state->page = state->page_automatic ? (state->high - state->low) / 5 : (int)lparam;
		return previous;
	}
	case TBM_SETSEL:
		if (style_of(state) & TBS_ENABLESELRANGE) {
			state->select_start = max((short)LOWORD(lparam), state->low);
			state->select_end = min((short)HIWORD(lparam), state->high);
			refresh(state, wparam != 0);
		}
		return 0;
	case TBM_SETSELSTART:
		if (style_of(state) & TBS_ENABLESELRANGE) {
			state->select_start = max((int)lparam, state->low);
			refresh(state, wparam != 0);
		}
		return 0;
	case TBM_SETSELEND:
		if (style_of(state) & TBS_ENABLESELRANGE) {
			state->select_end = min((int)lparam, state->high);
			refresh(state, wparam != 0);
		}
		return 0;
	case TBM_GETSELSTART:
		return state->select_start;
	case TBM_GETSELEND:
		return state->select_end;
	case TBM_CLEARSEL:
		state->select_start = state->select_end = 0;
		refresh(state, wparam != 0);
		return 0;
	case TBM_SETTIC: {
		BOOL added = add_tic(state, (int)lparam);
		if (added)
			InvalidateRect(window, NULL, TRUE);
		return added;
	}
	case TBM_CLEARTICS:
		HeapFree(GetProcessHeap(), 0, state->tics);
		state->tics = NULL;
		state->tic_count = 0;
		refresh(state, wparam != 0);
		return 0;
	case TBM_SETTICFREQ:
		state->frequency = (int)wparam;
		rebuild_automatic_tics(state);
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case TBM_GETNUMTICS:
		return style_of(state) & TBS_NOTICKS ? 0 : state->tic_count + 2;
	case TBM_GETPTICS:
		return (LRESULT)state->tics;
	case TBM_GETTIC:
		return (int)wparam >= 0 && (int)wparam < state->tic_count ? state->tics[wparam] : -1;
	case TBM_GETTICPOS:
		return (int)wparam >= 0 && (int)wparam < state->tic_count ? pixel_of(state, state->tics[wparam]) : -1;
	case TBM_SETTHUMBLENGTH:
		if (style_of(state) & TBS_FIXEDLENGTH) {
			state->thumb_length = (int)wparam;
			compute_channel(state);
			compute_thumb(state);
			InvalidateRect(window, NULL, TRUE);
		}
		return 0;
	case TBM_GETTHUMBLENGTH:
		return thumb_across(state);
	case TBM_GETTHUMBRECT:
		*(RECT *)lparam = state->thumb;
		return 0;
	case TBM_GETCHANNELRECT: {
		RECT *area = (RECT *)lparam;
		if (vertical(state))
			SetRect(area, state->channel.top, state->channel.left, state->channel.bottom, state->channel.right);
		else
			*area = state->channel;
		return 0;
	}
	case TBM_SETBUDDY: {
		HWND *slot = wparam ? &state->left_buddy : &state->right_buddy;
		HWND previous = *slot;
		*slot = (HWND)lparam;
		place_buddy(state, *slot, wparam != 0);
		return (LRESULT)previous;
	}
	case TBM_GETBUDDY:
		return (LRESULT)(wparam ? state->left_buddy : state->right_buddy);
	case TBM_SETTIPSIDE: {
		int previous = state->tip_side;
		state->tip_side = (int)wparam;
		return previous;
	}
	case TBM_SETTOOLTIPS:
		state->tooltip = (HWND)wparam;
		return 0;
	case TBM_GETTOOLTIPS:
		return (LRESULT)state->tooltip;
	case TBM_SETUNICODEFORMAT: {
		BOOL previous = state->unicode;
		state->unicode = wparam != 0;
		return previous;
	}
	case TBM_GETUNICODEFORMAT:
		return state->unicode;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void TRACKBAR_Register(void)
{
	WNDCLASSW definition = {0};
	definition.style = CS_GLOBALCLASS;
	definition.lpfnWndProc = trackbar_procedure;
	definition.cbWndExtra = sizeof(void *);
	definition.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	definition.lpszClassName = TRACKBAR_CLASSW;
	RegisterClassW(&definition);
}

void TRACKBAR_Unregister(void)
{
	UnregisterClassW(TRACKBAR_CLASSW, NULL);
}
