/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>

/* The tooltip. Text is kept wide or as a reference (callback, resource id) and resolved when shown; the A entry points convert at the edge. */

#define SHOW_TIMER 1
#define HIDE_TIMER 2
#define LEAVE_TIMER 3
#define LEAVE_POLL_MS 100
#define ANSI_TEXT_LIMIT 80
#define MAX_TEXT 1024
#define INSET_X 4
#define INSET_Y 2
#define CURSOR_GAP 20

typedef struct {
	UINT flags;
	HWND window;
	UINT_PTR id;
	RECT rect;
	HINSTANCE instance;
	WCHAR *text;
	UINT_PTR reference;
	LPARAM lparam;
	void *reserved;
	BOOL callback, unicode_notify;
} tip_tool;

typedef struct {
	HWND window, parent;
	tip_tool *tools;
	int count, current, pending;
	BOOL active, tracking;
	POINT track_at;
	int delay_initial, delay_autopop, delay_reshow;
	int max_width;
	RECT margin;
	COLORREF background, foreground;
	HFONT font;
	WCHAR *title;
	HICON title_icon;
	WCHAR shown[MAX_TEXT];
	HTHEME theme;
} tooltip_state;

static LRESULT CALLBACK relay_procedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

static tooltip_state *state_of(HWND window)
{
	return (tooltip_state *)GetWindowLongPtrW(window, 0);
}

static void *allocate(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void release(void *block)
{
	HeapFree(GetProcessHeap(), 0, block);
}

static WCHAR *copy_wide(const WCHAR *text)
{
	WCHAR *copy = allocate((lstrlenW(text) + 1) * sizeof(WCHAR));
	if (copy)
		lstrcpyW(copy, text);
	return copy;
}

static WCHAR *copy_ansi(const char *text)
{
	int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	WCHAR *copy = allocate(length * sizeof(WCHAR));
	if (copy)
		MultiByteToWideChar(CP_ACP, 0, text, -1, copy, length);
	return copy;
}

static int find_tool(const tooltip_state *state, HWND window, UINT_PTR id)
{
	int index;
	for (index = 0; index < state->count; ++index)
		if (state->tools[index].window == window && state->tools[index].id == id)
			return index;
	return -1;
}

/* TOOLINFOA and TOOLINFOW have one layout and differ in what lpszText points at. */
static int find_tool_of(const tooltip_state *state, const TTTOOLINFOW *info)
{
	return info ? find_tool(state, info->hwnd, info->uId) : -1;
}

static void store_text(tip_tool *tool, HINSTANCE instance, const void *text, BOOL wide)
{
	release(tool->text);
	tool->text = NULL;
	tool->reference = 0;
	tool->callback = FALSE;
	tool->instance = instance;
	if (!text)
		return;
	if (text == (const void *)LPSTR_TEXTCALLBACKW) {
		tool->callback = TRUE;
	} else if (IS_INTRESOURCE(text)) {
		tool->reference = (UINT_PTR)text;
	} else {
		tool->text = wide ? copy_wide(text) : copy_ansi(text);
	}
}

static void subclass_target(tip_tool *tool, tooltip_state *state, BOOL attach)
{
	HWND target = tool->flags & TTF_IDISHWND ? (HWND)tool->id : tool->window;
	if (!(tool->flags & TTF_SUBCLASS) || !target || !IsWindow(target))
		return;
	if (attach)
		SetWindowSubclass(target, relay_procedure, (UINT_PTR)state->window, (DWORD_PTR)state->window);
	else
		RemoveWindowSubclass(target, relay_procedure, (UINT_PTR)state->window);
}

static BOOL add_tool(tooltip_state *state, const TTTOOLINFOW *info, BOOL wide)
{
	tip_tool *grown, *tool;
	if (wide && info->cbSize > TTTOOLINFOW_V3_SIZE)
		return FALSE;
	grown = state->tools ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, state->tools, (state->count + 1) * sizeof(*grown))
		: allocate(sizeof(*grown));
	if (!grown)
		return FALSE;
	state->tools = grown;
	tool = &grown[state->count];
	tool->flags = info->uFlags;
	tool->window = info->hwnd;
	tool->id = info->uId;
	tool->rect = info->rect;
	store_text(tool, info->hinst, info->lpszText, wide);
	if (info->cbSize >= TTTOOLINFOW_V2_SIZE)
		tool->lparam = info->lParam;
	if (info->cbSize >= TTTOOLINFOW_V3_SIZE)
		tool->reserved = info->lpReserved;
	tool->unicode_notify = info->hwnd && SendMessageW(info->hwnd, WM_NOTIFYFORMAT, (WPARAM)state->window, NF_QUERY) == NFR_UNICODE;
	++state->count;
	subclass_target(tool, state, TRUE);
	return TRUE;
}

static void hide(tooltip_state *state)
{
	KillTimer(state->window, SHOW_TIMER);
	KillTimer(state->window, HIDE_TIMER);
	KillTimer(state->window, LEAVE_TIMER);
	state->pending = -1;
	if (state->current >= 0 && IsWindowVisible(state->window)) {
		tip_tool *tool = &state->tools[state->current];
		NMHDR notice;
		notice.hwndFrom = state->window;
		notice.idFrom = tool->id;
		notice.code = TTN_POP;
		SendMessageW(tool->window ? tool->window : state->parent, WM_NOTIFY, tool->id, (LPARAM)&notice);
	}
	state->tracking = FALSE;
	state->current = -1;
	ShowWindow(state->window, SW_HIDE);
}

static void delete_tool(tooltip_state *state, int index)
{
	subclass_target(&state->tools[index], state, FALSE);
	if (state->current == index)
		hide(state);
	else if (state->current > index)
		--state->current;
	if (state->pending == index)
		state->pending = -1;
	else if (state->pending > index)
		--state->pending;
	release(state->tools[index].text);
	--state->count;
	memmove(&state->tools[index], &state->tools[index + 1], (state->count - index) * sizeof(*state->tools));
}

/* Resolves what the tool shows. A callback may answer with text, a resource or its own buffer, and may ask to be remembered. */
static void resolve_text(tooltip_state *state, tip_tool *tool, WCHAR *out, int size)
{
	out[0] = 0;
	if (tool->callback) {
		HWND receiver = tool->window ? tool->window : state->parent;
		if (tool->unicode_notify) {
			NMTTDISPINFOW answer = {0};
			answer.hdr.hwndFrom = state->window;
			answer.hdr.idFrom = tool->id;
			answer.hdr.code = TTN_GETDISPINFOW;
			answer.lpszText = answer.szText;
			answer.hinst = tool->instance;
			answer.uFlags = tool->flags;
			answer.lParam = tool->lparam;
			SendMessageW(receiver, WM_NOTIFY, tool->id, (LPARAM)&answer);
			if (IS_INTRESOURCE(answer.lpszText))
				LoadStringW(answer.hinst, LOWORD(answer.lpszText), out, size);
			else if (answer.lpszText)
				lstrcpynW(out, answer.lpszText, size);
			if (answer.uFlags & TTF_DI_SETITEM)
				store_text(tool, answer.hinst, answer.lpszText, TRUE);
		} else {
			NMTTDISPINFOA answer = {0};
			answer.hdr.hwndFrom = state->window;
			answer.hdr.idFrom = tool->id;
			answer.hdr.code = TTN_GETDISPINFOA;
			answer.lpszText = answer.szText;
			answer.hinst = tool->instance;
			answer.uFlags = tool->flags;
			answer.lParam = tool->lparam;
			SendMessageW(receiver, WM_NOTIFY, tool->id, (LPARAM)&answer);
			if (IS_INTRESOURCE(answer.lpszText))
				LoadStringW(answer.hinst, LOWORD(answer.lpszText), out, size);
			else if (answer.lpszText)
				MultiByteToWideChar(CP_ACP, 0, answer.lpszText, -1, out, size);
			if (answer.uFlags & TTF_DI_SETITEM)
				store_text(tool, answer.hinst, answer.lpszText, FALSE);
		}
	} else if (tool->reference) {
		LoadStringW(tool->instance, (UINT)tool->reference, out, size);
	} else if (tool->text) {
		lstrcpynW(out, tool->text, size);
	}
}

static void copy_out(const WCHAR *text, void *buffer, BOOL wide)
{
	if (wide) {
		lstrcpyW(buffer, text);
		return;
	}
	if (!WideCharToMultiByte(CP_ACP, 0, text, -1, buffer, ANSI_TEXT_LIMIT + 1, NULL, NULL))
		((char *)buffer)[ANSI_TEXT_LIMIT] = 0;
}

static void fill_info(tooltip_state *state, tip_tool *tool, TTTOOLINFOW *info, BOOL wide)
{
	info->uFlags = tool->flags;
	info->hwnd = tool->window;
	info->uId = tool->id;
	info->rect = tool->rect;
	info->hinst = tool->instance;
	if (info->cbSize >= TTTOOLINFOW_V2_SIZE)
		info->lParam = tool->lparam;
	if (info->cbSize >= TTTOOLINFOW_V3_SIZE)
		info->lpReserved = tool->reserved;
	if (tool->callback) {
		info->lpszText = LPSTR_TEXTCALLBACKW;
	} else if (tool->reference) {
		info->lpszText = (WCHAR *)tool->reference;
	} else if (!tool->text) {
		info->lpszText = NULL;
	} else if (info->lpszText) {
		copy_out(tool->text, info->lpszText, wide);
	}
}

static void text_size(tooltip_state *state, HDC dc, SIZE *size)
{
	RECT area = {0, 0, state->max_width > 0 ? state->max_width : 0, 0};
	UINT flags = DT_CALCRECT | DT_EXPANDTABS | DT_LEFT;
	flags |= state->max_width > 0 ? DT_WORDBREAK : DT_SINGLELINE;
	if (GetWindowLongPtrW(state->window, GWL_STYLE) & TTS_NOPREFIX)
		flags |= DT_NOPREFIX;
	if (state->max_width <= 0 && (wcschr(state->shown, L'\n') || wcschr(state->shown, L'\r')))
		flags &= ~DT_SINGLELINE;
	DrawTextW(dc, state->shown, -1, &area, flags);
	size->cx = area.right + 2 * INSET_X + state->margin.left + state->margin.right;
	size->cy = area.bottom + 2 * INSET_Y + state->margin.top + state->margin.bottom;
	if (state->title) {
		RECT line = {0};
		DrawTextW(dc, state->title, -1, &line, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
		size->cx = max(size->cx, line.right + 2 * INSET_X);
		size->cy += line.bottom + INSET_Y;
	}
}

static SIZE bubble_size(tooltip_state *state)
{
	SIZE size;
	HDC dc = GetDC(state->window);
	HGDIOBJ old = SelectObject(dc, state->font ? state->font : GetStockObject(DEFAULT_GUI_FONT));
	text_size(state, dc, &size);
	SelectObject(dc, old);
	ReleaseDC(state->window, dc);
	return size;
}

static void keep_on_screen(POINT *at, const SIZE *size)
{
	HMONITOR monitor = MonitorFromPoint(*at, MONITOR_DEFAULTTONEAREST);
	MONITORINFO info = {sizeof(info)};
	if (!GetMonitorInfoW(monitor, &info))
		return;
	if (at->x + size->cx > info.rcMonitor.right)
		at->x = info.rcMonitor.right - size->cx;
	if (at->y + size->cy > info.rcMonitor.bottom)
		at->y = info.rcMonitor.bottom - size->cy;
	at->x = max(at->x, info.rcMonitor.left);
	at->y = max(at->y, info.rcMonitor.top);
}

static void place_and_show(tooltip_state *state)
{
	tip_tool *tool = &state->tools[state->current];
	NMHDR notice;
	SIZE size;
	POINT at;
	resolve_text(state, tool, state->shown, ARRAYSIZE(state->shown));
	size = bubble_size(state);
	if (tool->flags & TTF_TRACK) {
		at = state->track_at;
		if (!(tool->flags & TTF_ABSOLUTE)) {
			at.x -= size.cx / 2;
			at.y += CURSOR_GAP;
		}
	} else if (tool->flags & TTF_CENTERTIP) {
		RECT area;
		if (tool->flags & TTF_IDISHWND) {
			GetWindowRect((HWND)tool->id, &area);
		} else {
			area = tool->rect;
			MapWindowPoints(tool->window, HWND_DESKTOP, (POINT *)&area, 2);
		}
		at.x = (area.left + area.right - size.cx) / 2;
		at.y = area.bottom + 2;
	} else {
		GetCursorPos(&at);
		at.y += CURSOR_GAP;
	}
	if (!(tool->flags & TTF_ABSOLUTE))
		keep_on_screen(&at, &size);
	notice.hwndFrom = state->window;
	notice.idFrom = tool->id;
	notice.code = TTN_SHOW;
	if (!SendMessageW(tool->window ? tool->window : state->parent, WM_NOTIFY, tool->id, (LPARAM)&notice))
		SetWindowPos(state->window, HWND_TOPMOST, at.x, at.y, size.cx, size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	else
		SetWindowPos(state->window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
	InvalidateRect(state->window, NULL, TRUE);
	UpdateWindow(state->window);
}

static void show_tool(tooltip_state *state, int index)
{
	KillTimer(state->window, SHOW_TIMER);
	state->current = index;
	state->pending = -1;
	place_and_show(state);
	if (!(state->tools[index].flags & TTF_TRACK)) {
		SetTimer(state->window, HIDE_TIMER, state->delay_autopop, NULL);
		SetTimer(state->window, LEAVE_TIMER, LEAVE_POLL_MS, NULL);
	}
}

static BOOL tool_contains(const tip_tool *tool, HWND under, POINT client_point)
{
	if (tool->flags & TTF_IDISHWND)
		return (HWND)tool->id == under;
	return tool->window == under && PtInRect(&tool->rect, client_point);
}

static int tool_at(const tooltip_state *state, HWND under, POINT client_point)
{
	int index;
	for (index = 0; index < state->count; ++index)
		if (tool_contains(&state->tools[index], under, client_point))
			return index;
	return -1;
}

static void relay_mouse_move(tooltip_state *state, const MSG *message)
{
	POINT point = {(short)LOWORD(message->lParam), (short)HIWORD(message->lParam)};
	int index;
	if (!state->active)
		return;
	if (!(GetWindowLongPtrW(state->window, GWL_STYLE) & TTS_ALWAYSTIP)) {
		HWND active = GetActiveWindow();
		if (!active || !(GetAncestor(message->hwnd, GA_ROOT) == active))
			return;
	}
	index = tool_at(state, message->hwnd, point);
	if (index == state->current && index >= 0)
		return;
	if (index != state->pending) {
		BOOL was_shown = state->current >= 0;
		hide(state);
		if (index >= 0) {
			state->pending = index;
			SetTimer(state->window, SHOW_TIMER, was_shown ? state->delay_reshow : state->delay_initial, NULL);
		}
	}
}

static void relay(tooltip_state *state, const MSG *message)
{
	switch (message->message) {
	case WM_MOUSEMOVE:
		relay_mouse_move(state, message);
		break;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
		if (!state->tracking)
			hide(state);
		break;
	}
}

static LRESULT CALLBACK relay_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data)
{
	tooltip_state *state = state_of((HWND)data);
	if (state && ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_NCMOUSEMOVE)) {
		MSG relayed = {window, message, wparam, lparam};
		relay(state, &relayed);
	}
	if (message == WM_NCDESTROY)
		RemoveWindowSubclass(window, relay_procedure, id);
	return DefSubclassProc(window, message, wparam, lparam);
}

static void paint(tooltip_state *state, HDC dc)
{
	NMTTCUSTOMDRAW draw = {0};
	RECT client, inner;
	LRESULT answer;
	HGDIOBJ old;
	HWND receiver;
	GetClientRect(state->window, &client);
	inner = client;
	old = SelectObject(dc, state->font ? state->font : GetStockObject(DEFAULT_GUI_FONT));
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, state->foreground);
	draw.nmcd.hdr.hwndFrom = state->window;
	draw.nmcd.hdr.code = NM_CUSTOMDRAW;
	draw.nmcd.hdc = dc;
	draw.nmcd.rc = client;
	draw.nmcd.dwDrawStage = CDDS_PREPAINT;
	draw.uDrawFlags = DT_EXPANDTABS | DT_LEFT;
	if (state->current >= 0) {
		draw.nmcd.hdr.idFrom = state->tools[state->current].id;
		receiver = state->tools[state->current].window;
	} else {
		receiver = state->parent;
	}
	if (!receiver)
		receiver = state->parent;
	{
		/* Filled first: a theme without this class draws nothing, and the surface underneath is black. */
		HBRUSH brush = CreateSolidBrush(state->background);
		FillRect(dc, &client, brush);
		DeleteObject(brush);
		FrameRect(dc, &client, GetSysColorBrush(COLOR_WINDOWFRAME));
	}
	if (state->theme)
		DrawThemeBackground(state->theme, dc, TTP_STANDARD, 0, &client, NULL);
	answer = SendMessageW(receiver, WM_NOTIFY, draw.nmcd.hdr.idFrom, (LPARAM)&draw);
	if (!(answer & CDRF_SKIPDEFAULT)) {
		UINT flags = DT_EXPANDTABS | DT_LEFT;
		inner.left += INSET_X + state->margin.left;
		inner.right -= INSET_X + state->margin.right;
		inner.top += INSET_Y + state->margin.top;
		inner.bottom -= INSET_Y + state->margin.bottom;
		if (GetWindowLongPtrW(state->window, GWL_STYLE) & TTS_NOPREFIX)
			flags |= DT_NOPREFIX;
		if (state->max_width > 0)
			flags |= DT_WORDBREAK;
		if (state->title) {
			RECT line = inner;
			DrawTextW(dc, state->title, -1, &line, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
			DrawTextW(dc, state->title, -1, &line, DT_SINGLELINE | DT_NOPREFIX);
			inner.top = line.bottom + INSET_Y;
		}
		DrawTextW(dc, state->shown, -1, &inner, flags);
	}
	if (answer & CDRF_NOTIFYPOSTPAINT) {
		draw.nmcd.dwDrawStage = CDDS_POSTPAINT;
		SendMessageW(receiver, WM_NOTIFY, draw.nmcd.hdr.idFrom, (LPARAM)&draw);
	}
	SelectObject(dc, old);
}

static void reset_delays(tooltip_state *state)
{
	int base = GetDoubleClickTime();
	state->delay_initial = base;
	state->delay_autopop = base * 10;
	state->delay_reshow = base / 5;
}

static LRESULT set_delay(tooltip_state *state, WPARAM kind, int value)
{
	switch (kind) {
	case TTDT_AUTOMATIC:
		if (value < 0) {
			reset_delays(state);
		} else {
			state->delay_initial = value;
			state->delay_autopop = value * 10;
			state->delay_reshow = value / 5;
		}
		break;
	case TTDT_RESHOW:
		state->delay_reshow = value < 0 ? (int)GetDoubleClickTime() / 5 : value;
		break;
	case TTDT_AUTOPOP:
		state->delay_autopop = value < 0 ? (int)GetDoubleClickTime() * 10 : value;
		break;
	case TTDT_INITIAL:
		state->delay_initial = value < 0 ? (int)GetDoubleClickTime() : value;
		break;
	}
	return 0;
}

static void set_tool_info(tooltip_state *state, tip_tool *tool, const TTTOOLINFOW *info, BOOL wide)
{
	subclass_target(tool, state, FALSE);
	tool->flags = info->uFlags;
	tool->rect = info->rect;
	store_text(tool, info->hinst, info->lpszText, wide);
	if (info->cbSize >= TTTOOLINFOW_V2_SIZE)
		tool->lparam = info->lParam;
	if (info->cbSize >= TTTOOLINFOW_V3_SIZE)
		tool->reserved = info->lpReserved;
	subclass_target(tool, state, TRUE);
}

static LRESULT CALLBACK tooltip_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	tooltip_state *state = state_of(window);
	BOOL wide = TRUE;
	if (!state && message != WM_NCCREATE)
		return DefWindowProcW(window, message, wparam, lparam);
	switch (message) {
	case WM_NCCREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		DWORD style = (create->style | WS_POPUP) & ~(WS_CHILD | WS_MAXIMIZE | WS_BORDER | WS_DLGFRAME);
		state = allocate(sizeof(*state));
		if (!state)
			return FALSE;
		state->window = window;
		state->parent = create->hwndParent;
		state->active = TRUE;
		state->current = state->pending = -1;
		state->max_width = -1;
		state->background = GetSysColor(COLOR_INFOBK);
		state->foreground = GetSysColor(COLOR_INFOTEXT);
		reset_delays(state);
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		SetWindowLongPtrW(window, GWL_STYLE, style);
		SetWindowLongPtrW(window, GWL_EXSTYLE, create->dwExStyle | WS_EX_TOOLWINDOW);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_CREATE:
		state->theme = OpenThemeData(window, L"Tooltip");
		return 0;
	case WM_DESTROY:
		while (state->count)
			delete_tool(state, state->count - 1);
		CloseThemeData(state->theme);
		state->theme = NULL;
		return 0;
	case WM_NCDESTROY:
		release(state->tools);
		release(state->title);
		release(state);
		SetWindowLongPtrW(window, 0, 0);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_THEMECHANGED:
		CloseThemeData(state->theme);
		state->theme = OpenThemeData(window, L"Tooltip");
		return 0;
	case WM_MOUSEMOVE:
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
		hide(state);
		return 0;
	case WM_NCHITTEST:
		if (state->current >= 0 && (state->tools[state->current].flags & TTF_TRANSPARENT))
			return HTTRANSPARENT;
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_TIMER:
		if (wparam == SHOW_TIMER) {
			KillTimer(window, SHOW_TIMER);
			if (state->pending >= 0)
				show_tool(state, state->pending);
		} else if (wparam == HIDE_TIMER) {
			hide(state);
		} else if (wparam == LEAVE_TIMER && state->current >= 0) {
			POINT point, inside;
			HWND under;
			GetCursorPos(&point);
			under = WindowFromPoint(point);
			inside = point;
			if (under)
				ScreenToClient(under, &inside);
			if (under != window && !tool_contains(&state->tools[state->current], under, inside))
				hide(state);
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
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		if (LOWORD(lparam))
			InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_GETTEXTLENGTH:
		return 0;
	case TTM_ACTIVATE:
		state->active = wparam != 0;
		if (!state->active)
			hide(state);
		return 0;
	case TTM_ADDTOOLA:
		wide = FALSE;
		/* fall through */
	case TTM_ADDTOOLW:
		return lparam ? add_tool(state, (const TTTOOLINFOW *)lparam, wide) : FALSE;
	case TTM_DELTOOLA:
	case TTM_DELTOOLW: {
		int index = find_tool_of(state, (const TTTOOLINFOW *)lparam);
		if (index >= 0)
			delete_tool(state, index);
		return 0;
	}
	case TTM_GETTOOLCOUNT:
		return state->count;
	case TTM_ENUMTOOLSA:
		wide = FALSE;
		/* fall through */
	case TTM_ENUMTOOLSW:
		if ((int)wparam < 0 || (int)wparam >= state->count || !lparam)
			return FALSE;
		fill_info(state, &state->tools[wparam], (TTTOOLINFOW *)lparam, wide);
		return TRUE;
	case TTM_GETTOOLINFOA:
		wide = FALSE;
		/* fall through */
	case TTM_GETTOOLINFOW: {
		int index = find_tool_of(state, (const TTTOOLINFOW *)lparam);
		if (index < 0)
			return FALSE;
		fill_info(state, &state->tools[index], (TTTOOLINFOW *)lparam, wide);
		return TRUE;
	}
	case TTM_SETTOOLINFOA:
		wide = FALSE;
		/* fall through */
	case TTM_SETTOOLINFOW: {
		int index = find_tool_of(state, (const TTTOOLINFOW *)lparam);
		if (index >= 0) {
			set_tool_info(state, &state->tools[index], (const TTTOOLINFOW *)lparam, wide);
			if (index == state->current && IsWindowVisible(window))
				place_and_show(state);
		}
		return 0;
	}
	case TTM_UPDATETIPTEXTA:
		wide = FALSE;
		/* fall through */
	case TTM_UPDATETIPTEXTW: {
		const TTTOOLINFOW *info = (const TTTOOLINFOW *)lparam;
		int index = find_tool_of(state, info);
		if (index >= 0) {
			store_text(&state->tools[index], info->hinst, info->lpszText, wide);
			if (index == state->current && IsWindowVisible(window))
				place_and_show(state);
		}
		return 0;
	}
	case TTM_GETTEXTA:
		wide = FALSE;
		/* fall through */
	case TTM_GETTEXTW: {
		TTTOOLINFOW *info = (TTTOOLINFOW *)lparam;
		int index = find_tool_of(state, info);
		WCHAR text[MAX_TEXT];
		if (index < 0 || !info->lpszText)
			return 0;
		resolve_text(state, &state->tools[index], text, ARRAYSIZE(text));
		copy_out(text, info->lpszText, wide);
		return 0;
	}
	case TTM_GETCURRENTTOOLA:
		wide = FALSE;
		/* fall through */
	case TTM_GETCURRENTTOOLW: {
		if (state->current < 0)
			return FALSE;
		if (lparam)
			fill_info(state, &state->tools[state->current], (TTTOOLINFOW *)lparam, wide);
		return TRUE;
	}
	case TTM_NEWTOOLRECTA:
	case TTM_NEWTOOLRECTW: {
		const TTTOOLINFOW *info = (const TTTOOLINFOW *)lparam;
		int index = find_tool_of(state, info);
		if (index >= 0)
			state->tools[index].rect = info->rect;
		return 0;
	}
	case TTM_HITTESTA:
		wide = FALSE;
		/* fall through */
	case TTM_HITTESTW: {
		TTHITTESTINFOW *hit = (TTHITTESTINFOW *)lparam;
		POINT point;
		int index;
		if (!hit)
			return FALSE;
		point = hit->pt;
		index = tool_at(state, hit->hwnd, point);
		if (index < 0)
			return FALSE;
		fill_info(state, &state->tools[index], &hit->ti, wide);
		return TRUE;
	}
	case TTM_RELAYEVENT:
		if (lparam)
			relay(state, (const MSG *)lparam);
		return 0;
	case TTM_WINDOWFROMPOINT:
		return (LRESULT)WindowFromPoint(*(const POINT *)lparam);
	case TTM_TRACKACTIVATE: {
		int index = find_tool_of(state, (const TTTOOLINFOW *)lparam);
		if (!wparam) {
			hide(state);
			return 0;
		}
		if (index >= 0) {
			state->tracking = TRUE;
			state->current = index;
			place_and_show(state);
		}
		return 0;
	}
	case TTM_TRACKPOSITION:
		state->track_at.x = (short)LOWORD(lparam);
		state->track_at.y = (short)HIWORD(lparam);
		if (state->tracking && state->current >= 0)
			place_and_show(state);
		return 0;
	case TTM_POP:
		hide(state);
		return 0;
	case TTM_POPUP:
		if (state->count)
			show_tool(state, state->current >= 0 ? state->current : 0);
		return 0;
	case TTM_UPDATE:
		if (state->current >= 0)
			place_and_show(state);
		return 0;
	case TTM_GETBUBBLESIZE: {
		SIZE size;
		int index = find_tool_of(state, (const TTTOOLINFOW *)lparam);
		if (index < 0)
			return 0;
		resolve_text(state, &state->tools[index], state->shown, ARRAYSIZE(state->shown));
		size = bubble_size(state);
		return MAKELONG(size.cx, size.cy);
	}
	case TTM_ADJUSTRECT: {
		RECT *area = (RECT *)lparam;
		int x = INSET_X, y = INSET_Y;
		if (!area)
			return FALSE;
		if (wparam)
			InflateRect(area, x, y);
		else
			InflateRect(area, -x, -y);
		return TRUE;
	}
	case TTM_SETDELAYTIME:
		return set_delay(state, wparam, (short)LOWORD(lparam));
	case TTM_GETDELAYTIME:
		switch (wparam) {
		case TTDT_RESHOW:
			return state->delay_reshow;
		case TTDT_AUTOPOP:
			return state->delay_autopop;
		case TTDT_INITIAL:
		case TTDT_AUTOMATIC:
			return state->delay_initial;
		}
		return -1;
	case TTM_SETMAXTIPWIDTH: {
		int previous = state->max_width;
		state->max_width = (int)lparam;
		return previous;
	}
	case TTM_GETMAXTIPWIDTH:
		return state->max_width;
	case TTM_SETMARGIN:
		if (lparam)
			state->margin = *(const RECT *)lparam;
		return 0;
	case TTM_GETMARGIN:
		if (lparam)
			*(RECT *)lparam = state->margin;
		return 0;
	case TTM_SETTIPBKCOLOR:
		state->background = (COLORREF)wparam;
		return 0;
	case TTM_GETTIPBKCOLOR:
		return state->background;
	case TTM_SETTIPTEXTCOLOR:
		state->foreground = (COLORREF)wparam;
		return 0;
	case TTM_GETTIPTEXTCOLOR:
		return state->foreground;
	case TTM_SETTITLEA:
	case TTM_SETTITLEW:
		release(state->title);
		state->title = !lparam ? NULL : message == TTM_SETTITLEW ? copy_wide((const WCHAR *)lparam) : copy_ansi((const char *)lparam);
		return TRUE;
	case TTM_GETTITLE: {
		TTGETTITLE *title = (TTGETTITLE *)lparam;
		if (title && title->pszTitle && title->cch) {
			lstrcpynW(title->pszTitle, state->title ? state->title : L"", title->cch);
			title->uTitleBitmap = 0;
		}
		return 0;
	}
	case TTM_SETWINDOWTHEME:
		SetWindowTheme(window, (LPCWSTR)lparam, NULL);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void TOOLTIPS_Register(void)
{
	WNDCLASSW definition = {0};
	definition.style = CS_GLOBALCLASS | CS_DBLCLKS | CS_SAVEBITS;
	definition.lpfnWndProc = tooltip_procedure;
	definition.cbWndExtra = sizeof(void *);
	definition.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	definition.lpszClassName = TOOLTIPS_CLASSW;
	RegisterClassW(&definition);
}

void TOOLTIPS_Unregister(void)
{
	UnregisterClassW(TOOLTIPS_CLASSW, NULL);
}
