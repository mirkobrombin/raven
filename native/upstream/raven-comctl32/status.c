/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>

/* The status bar. It stores wide text only; the A messages convert on the way in and out, so one code path paints and measures. */

#define MAX_PARTS 256
#define SIMPLE_PART 255
#define HORIZONTAL_GAP 0
#define VERTICAL_GAP 2
#define BETWEEN_PARTS 2
#define PART_PADDING 3

typedef struct {
	int right;
	WCHAR *text;
	DWORD_PTR owner_data;
	UINT flags;
	HICON icon;
	WCHAR *tip;
} status_part;

typedef struct {
	HWND window, parent;
	status_part *parts;
	status_part simple;
	int count;
	BOOL simple_mode, unicode;
	HFONT font;
	COLORREF background;
	int min_height, height;
	HTHEME theme;
} status_state;

static status_state *state_of(HWND window)
{
	return (status_state *)GetWindowLongPtrW(window, 0);
}

static DWORD style_of(HWND window)
{
	return (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
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
	WCHAR *copy = text ? allocate((lstrlenW(text) + 1) * sizeof(WCHAR)) : NULL;
	if (copy)
		lstrcpyW(copy, text);
	return copy;
}

static WCHAR *copy_ansi(const char *text)
{
	int length;
	WCHAR *copy;
	if (!text)
		return NULL;
	length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	copy = allocate(length * sizeof(WCHAR));
	if (copy)
		MultiByteToWideChar(CP_ACP, 0, text, -1, copy, length);
	return copy;
}

/* Controls would break the line; tabs stay, they split the part into its left, centre and right text. */
static void sanitize(WCHAR *text)
{
	WCHAR *from, *to;
	if (!text)
		return;
	for (from = to = text; *from; ++from)
		if (*from < L' ' && *from != L'\t') {
			if (to == text || to[-1] != L' ')
				*to++ = L' ';
		} else {
			*to++ = *from;
		}
	*to = 0;
}

static status_part *part_at(status_state *state, int index)
{
	if (state->simple_mode && index == SIMPLE_PART)
		return &state->simple;
	return index >= 0 && index < state->count ? &state->parts[index] : NULL;
}

static int grip_width(void)
{
	return GetSystemMetrics(SM_CXVSCROLL);
}

static BOOL shows_grip(const status_state *state)
{
	return (style_of(state->window) & SBARS_SIZEGRIP) && !IsZoomed(state->parent);
}

static RECT part_rect(status_state *state, int index)
{
	RECT client, area;
	GetClientRect(state->window, &client);
	area.top = client.top + VERTICAL_GAP;
	area.bottom = client.bottom;
	if (state->simple_mode) {
		area.left = client.left;
		area.right = client.right;
		return area;
	}
	area.left = index ? state->parts[index - 1].right + BETWEEN_PARTS : HORIZONTAL_GAP;
	area.right = state->parts[index].right < 0 ? client.right : state->parts[index].right;
	return area;
}

static void compute_height(status_state *state)
{
	TEXTMETRICW metrics;
	HDC dc = GetDC(state->window);
	HGDIOBJ old = SelectObject(dc, state->font ? state->font : GetStockObject(DEFAULT_GUI_FONT));
	int floor = GetSystemMetrics(SM_CYSIZE) + 2;
	floor &= ~1;
	GetTextMetricsW(dc, &metrics);
	SelectObject(dc, old);
	ReleaseDC(state->window, dc);
	state->height = metrics.tmHeight + (metrics.tmInternalLeading ? metrics.tmInternalLeading : 2) + 4;
	state->height = max(state->height, floor);
	if (state->min_height)
		state->height = max(state->height, state->min_height + 2);
}

/* Sits along the parent's bottom edge unless told otherwise; a WM_SIZE is the only thing that moves it. */
static void fit_parent(status_state *state)
{
	DWORD style = style_of(state->window);
	RECT area;
	compute_height(state);
	if (style & CCS_NORESIZE)
		return;
	if (style & CCS_NOPARENTALIGN) {
		GetWindowRect(state->window, &area);
		SetWindowPos(state->window, NULL, 0, 0, area.right - area.left, state->height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		return;
	}
	GetClientRect(state->parent, &area);
	SetWindowPos(state->window, NULL, 0, (style & CCS_BOTTOM) == CCS_TOP ? 0 : area.bottom - state->height,
		area.right, state->height, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void draw_text_columns(HDC dc, const WCHAR *text, RECT area)
{
	static const UINT aligns[] = {DT_LEFT, DT_CENTER, DT_RIGHT};
	const WCHAR *start = text;
	int column = 0;
	for (;;) {
		const WCHAR *end = start;
		while (*end && *end != L'\t')
			++end;
		DrawTextW(dc, start, (int)(end - start), &area, aligns[min(column, 2)] | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
		if (!*end)
			break;
		start = end + 1;
		++column;
	}
}

static void draw_part(status_state *state, HDC dc, status_part *part, int index, RECT area)
{
	if (state->theme) {
		if (!(part->flags & SBT_NOBORDERS))
			DrawThemeBackground(state->theme, dc, SP_PANE, 0, &area, NULL);
	} else if (!(part->flags & SBT_NOBORDERS)) {
		DrawEdge(dc, &area, part->flags & SBT_POPOUT ? BDR_RAISEDINNER : BDR_SUNKENOUTER, BF_RECT | BF_ADJUST);
	}
	if (part->flags & SBT_OWNERDRAW) {
		DRAWITEMSTRUCT item = {0};
		item.CtlID = (UINT)GetWindowLongPtrW(state->window, GWLP_ID);
		item.itemID = index;
		item.itemAction = ODA_DRAWENTIRE;
		item.hwndItem = state->window;
		item.hDC = dc;
		item.rcItem = area;
		item.itemData = part->owner_data;
		SendMessageW(state->parent, WM_DRAWITEM, item.CtlID, (LPARAM)&item);
		return;
	}
	area.left += PART_PADDING;
	if (part->icon) {
		int size = GetSystemMetrics(SM_CXSMICON);
		DrawIconEx(dc, area.left, area.top + (area.bottom - area.top - size) / 2, part->icon, size, size, 0, NULL, DI_NORMAL);
		area.left += size + PART_PADDING;
	}
	if (part->text)
		draw_text_columns(dc, part->text, area);
}

static void paint(status_state *state, HDC dc)
{
	RECT client;
	HGDIOBJ old;
	int index;
	GetClientRect(state->window, &client);
	if (state->background != CLR_DEFAULT) {
		SetBkColor(dc, state->background);
		ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &client, NULL, 0, NULL);
	} else if (state->theme) {
		DrawThemeBackground(state->theme, dc, 0, 0, &client, NULL);
	} else {
		FillRect(dc, &client, GetSysColorBrush(COLOR_3DFACE));
	}
	old = SelectObject(dc, state->font ? state->font : GetStockObject(DEFAULT_GUI_FONT));
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
	if (state->simple_mode) {
		draw_part(state, dc, &state->simple, SIMPLE_PART, part_rect(state, 0));
	} else {
		for (index = 0; index < state->count; ++index)
			draw_part(state, dc, &state->parts[index], index, part_rect(state, index));
	}
	if (shows_grip(state)) {
		RECT grip = client;
		grip.left = client.right - grip_width();
		if (state->theme)
			DrawThemeBackground(state->theme, dc, SP_GRIPPER, 0, &grip, NULL);
		else
			DrawFrameControl(dc, &grip, DFC_SCROLL, DFCS_SCROLLSIZEGRIP);
	}
	SelectObject(dc, old);
}

static BOOL set_parts(status_state *state, int count, const int *rights)
{
	status_part *grown;
	int index;
	if (count <= 0 || count > MAX_PARTS)
		return FALSE;
	for (index = count; index < state->count; ++index) {
		release(state->parts[index].text);
		release(state->parts[index].tip);
	}
	grown = state->parts ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, state->parts, count * sizeof(*grown))
		: allocate(count * sizeof(*grown));
	if (!grown)
		return FALSE;
	state->parts = grown;
	state->count = count;
	for (index = 0; index < count; ++index)
		state->parts[index].right = rights[index];
	InvalidateRect(state->window, NULL, TRUE);
	return TRUE;
}

static BOOL set_text(status_state *state, WPARAM wparam, LPARAM lparam, BOOL wide)
{
	status_part *part = part_at(state, LOBYTE(LOWORD(wparam)));
	UINT flags = (UINT)wparam & 0xff00;
	WCHAR *text = NULL;
	BOOL changed;
	if (!part)
		return FALSE;
	if (flags & SBT_OWNERDRAW) {
		changed = !(part->flags & SBT_OWNERDRAW) || part->owner_data != (DWORD_PTR)lparam || part->flags != flags;
		release(part->text);
		part->text = NULL;
		part->owner_data = (DWORD_PTR)lparam;
	} else {
		text = wide ? copy_wide((const WCHAR *)lparam) : copy_ansi((const char *)lparam);
		sanitize(text);
		changed = (part->flags & SBT_OWNERDRAW) || part->flags != flags ||
			(text ? !part->text || lstrcmpW(text, part->text) : part->text != NULL);
		release(part->text);
		part->text = text;
		part->owner_data = 0;
	}
	part->flags = flags;
	if (changed) {
		InvalidateRect(state->window, NULL, TRUE);
		UpdateWindow(state->window);
	}
	return TRUE;
}

static LRESULT text_length(const status_part *part, BOOL wide)
{
	int length = 0;
	if (part->text)
		length = wide ? lstrlenW(part->text) : WideCharToMultiByte(CP_ACP, 0, part->text, -1, NULL, 0, NULL, NULL) - 1;
	return MAKELONG(length, part->flags);
}

static LRESULT get_text(const status_part *part, void *buffer, BOOL wide)
{
	if (part->flags & SBT_OWNERDRAW)
		return (LRESULT)part->owner_data;
	if (buffer) {
		if (wide)
			lstrcpyW(buffer, part->text ? part->text : L"");
		else if (part->text)
			WideCharToMultiByte(CP_ACP, 0, part->text, -1, buffer, INT_MAX, NULL, NULL);
		else
			*(char *)buffer = 0;
	}
	return text_length(part, wide);
}

static void get_tip(const status_part *part, WPARAM wparam, void *buffer, BOOL wide)
{
	int size = HIWORD(wparam);
	if (!buffer || !size)
		return;
	if (wide) {
		lstrcpynW(buffer, part->tip ? part->tip : L"", size);
	} else if (!part->tip || !WideCharToMultiByte(CP_ACP, 0, part->tip, -1, buffer, size, NULL, NULL)) {
		*(char *)buffer = 0;
	}
}

static LRESULT notify_click(status_state *state, UINT code)
{
	NMMOUSE notice = {0};
	POINT point;
	int index;
	GetCursorPos(&point);
	ScreenToClient(state->window, &point);
	notice.hdr.hwndFrom = state->window;
	notice.hdr.idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	notice.hdr.code = code;
	notice.pt = point;
	notice.dwItemSpec = -1;
	for (index = 0; !state->simple_mode && index < state->count; ++index) {
		RECT area = part_rect(state, index);
		if (PtInRect(&area, point)) {
			notice.dwItemSpec = index;
			break;
		}
	}
	return SendMessageW(state->parent, WM_NOTIFY, notice.hdr.idFrom, (LPARAM)&notice);
}

static LRESULT hit_test(status_state *state, LPARAM lparam, HWND window)
{
	RECT area;
	int x = (short)LOWORD(lparam);
	if (!shows_grip(state))
		return HTCLIENT;
	GetWindowRect(window, &area);
	if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYOUTRTL)
		return x <= area.left + grip_width() ? HTBOTTOMLEFT : HTCLIENT;
	return x >= area.right - grip_width() ? HTBOTTOMRIGHT : HTCLIENT;
}

static void free_part(status_part *part)
{
	release(part->text);
	release(part->tip);
}

static LRESULT CALLBACK status_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	status_state *state = state_of(window);
	if (!state && message != WM_NCCREATE)
		return DefWindowProcW(window, message, wparam, lparam);
	switch (message) {
	case WM_NCCREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		static const int whole = -1;
		state = allocate(sizeof(*state));
		if (!state)
			return FALSE;
		state->window = window;
		state->parent = create->hwndParent;
		state->background = CLR_DEFAULT;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		if (!set_parts(state, 1, &whole))
			return FALSE;
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_CREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state->unicode = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_UNICODE;
		state->theme = OpenThemeData(window, L"Status");
		if (create->lpszName && (ULONG_PTR)create->lpszName > 0xffff)
			state->parts[0].text = copy_wide(create->lpszName);
		fit_parent(state);
		return 0;
	}
	case WM_DESTROY:
		CloseThemeData(state->theme);
		state->theme = NULL;
		return 0;
	case WM_NCDESTROY: {
		int index;
		for (index = 0; index < state->count; ++index)
			free_part(&state->parts[index]);
		free_part(&state->simple);
		release(state->parts);
		release(state);
		SetWindowLongPtrW(window, 0, 0);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_THEMECHANGED:
		CloseThemeData(state->theme);
		state->theme = OpenThemeData(window, L"Status");
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_SIZE:
		fit_parent(state);
		return 0;
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		SendMessageW(window, WM_SIZE, 0, 0);
		if (LOWORD(lparam))
			InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
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
	case WM_NCHITTEST:
		return hit_test(state, lparam, window);
	case WM_NCLBUTTONDOWN:
	case WM_NCLBUTTONUP:
		return SendMessageW(state->parent, message, wparam, lparam);
	case WM_LBUTTONUP:
		notify_click(state, NM_CLICK);
		return 0;
	case WM_LBUTTONDBLCLK:
		notify_click(state, NM_DBLCLK);
		return 0;
	case WM_RBUTTONUP:
		if (!notify_click(state, NM_RCLICK))
			return DefWindowProcW(window, message, wparam, lparam);
		return 0;
	case WM_RBUTTONDBLCLK:
		notify_click(state, NM_RDBLCLK);
		return 0;
	case WM_GETTEXTLENGTH: {
		status_part *part = part_at(state, state->simple_mode ? SIMPLE_PART : 0);
		return part && part->text ? lstrlenW(part->text) : 0;
	}
	case WM_GETTEXT: {
		status_part *part = part_at(state, state->simple_mode ? SIMPLE_PART : 0);
		int length = part && part->text ? lstrlenW(part->text) : 0;
		if (!wparam)
			return length;
		length = min(length, (int)wparam - 1);
		if (length)
			memcpy((void *)lparam, part->text, length * sizeof(WCHAR));
		((WCHAR *)lparam)[length] = 0;
		return length;
	}
	case WM_SETTEXT:
		return set_text(state, state->simple_mode ? SIMPLE_PART : 0, lparam, TRUE);
	case SB_SETPARTS:
		return set_parts(state, (int)wparam, (const int *)lparam);
	case SB_GETPARTS: {
		int index, *rights = (int *)lparam;
		for (index = 0; rights && index < min((int)wparam, state->count); ++index)
			rights[index] = state->parts[index].right;
		return state->simple_mode ? 1 : state->count;
	}
	case SB_GETBORDERS: {
		int *borders = (int *)lparam;
		borders[0] = HORIZONTAL_GAP;
		borders[1] = VERTICAL_GAP;
		borders[2] = BETWEEN_PARTS;
		return TRUE;
	}
	case SB_GETRECT: {
		status_part *part = part_at(state, (int)wparam);
		if (!part)
			return FALSE;
		*(RECT *)lparam = part_rect(state, state->simple_mode ? 0 : (int)wparam);
		return TRUE;
	}
	case SB_SETTEXTA:
	case SB_SETTEXTW:
		return set_text(state, wparam, lparam, message == SB_SETTEXTW);
	case SB_GETTEXTA:
	case SB_GETTEXTW: {
		status_part *part = part_at(state, LOBYTE(LOWORD(wparam)));
		return part ? get_text(part, (void *)lparam, message == SB_GETTEXTW) : 0;
	}
	case SB_GETTEXTLENGTHA:
	case SB_GETTEXTLENGTHW: {
		status_part *part = part_at(state, LOBYTE(LOWORD(wparam)));
		return part ? text_length(part, message == SB_GETTEXTLENGTHW) : 0;
	}
	case SB_SETTIPTEXTA:
	case SB_SETTIPTEXTW: {
		status_part *part = part_at(state, (int)wparam);
		if (part) {
			release(part->tip);
			part->tip = message == SB_SETTIPTEXTW ? copy_wide((const WCHAR *)lparam) : copy_ansi((const char *)lparam);
		}
		return 0;
	}
	case SB_GETTIPTEXTA:
	case SB_GETTIPTEXTW: {
		status_part *part = part_at(state, LOWORD(wparam));
		if (part)
			get_tip(part, wparam, (void *)lparam, message == SB_GETTIPTEXTW);
		return 0;
	}
	case SB_SETMINHEIGHT:
		state->min_height = (int)wparam;
		return 0;
	case SB_SIMPLE:
		state->simple_mode = wparam != 0;
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case SB_ISSIMPLE:
		return state->simple_mode;
	case SB_SETBKCOLOR: {
		COLORREF previous = state->background;
		state->background = (COLORREF)lparam;
		InvalidateRect(window, NULL, TRUE);
		return previous;
	}
	case SB_SETICON: {
		status_part *part = (int)wparam == -1 ? &state->simple : part_at(state, (int)wparam);
		if (!part)
			return FALSE;
		part->icon = (HICON)lparam;
		InvalidateRect(window, NULL, TRUE);
		return TRUE;
	}
	case SB_GETICON: {
		status_part *part = (int)wparam == -1 ? &state->simple : part_at(state, (int)wparam);
		return part ? (LRESULT)part->icon : 0;
	}
	case SB_SETUNICODEFORMAT: {
		BOOL previous = state->unicode;
		state->unicode = wparam != 0;
		return previous;
	}
	case SB_GETUNICODEFORMAT:
		return state->unicode;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void STATUS_Register(void)
{
	WNDCLASSW definition = {0};
	definition.style = CS_GLOBALCLASS | CS_DBLCLKS | CS_VREDRAW;
	definition.lpfnWndProc = status_procedure;
	definition.cbWndExtra = sizeof(void *);
	definition.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	definition.lpszClassName = STATUSCLASSNAMEW;
	RegisterClassW(&definition);
}

void STATUS_Unregister(void)
{
	UnregisterClassW(STATUSCLASSNAMEW, NULL);
}
