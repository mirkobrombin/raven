/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>

/* The up-down control. The range is kept as given: its low end may lie above its high end, and "up" always walks towards the high end. */

#define REPEAT_TIMER 1
#define FIRST_REPEAT_MS 500
#define NEXT_REPEAT_MS 100
#define BUDDY_OVERLAP 2
#define SUBCLASS_ID 1
#define DIGITS 40

enum { NOT_PRESSED, PRESSED_UP, PRESSED_DOWN };

typedef struct {
	HWND buddy, parent;
	int low, high, position, base;
	UDACCEL *accel;
	UINT accel_count;
	BOOL writes_buddy, unicode, subclassed;
	int pressed, hot;
	DWORD pressed_at;
	HTHEME theme;
} updown_state;

static const UDACCEL default_accel[] = {{0, 1}, {2, 5}, {5, 20}};
static const WCHAR listbox_class[] = L"ListBox";

static LRESULT CALLBACK buddy_procedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

static updown_state *state_of(HWND window)
{
	return (updown_state *)GetWindowLongPtrW(window, 0);
}

static DWORD style_of(HWND window)
{
	return (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
}

static int clamped(const updown_state *state, int value)
{
	int least = min(state->low, state->high), most = max(state->low, state->high);
	return value < least ? least : value > most ? most : value;
}

static BOOL buddy_is_listbox(const updown_state *state)
{
	WCHAR name[16];
	return GetClassNameW(state->buddy, name, ARRAYSIZE(name)) && !lstrcmpiW(name, listbox_class);
}

static WCHAR thousands_separator(void)
{
	WCHAR text[4];
	return GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_STHOUSAND, text, ARRAYSIZE(text)) ? text[0] : 0;
}

static void format_number(const updown_state *state, HWND window, int value, WCHAR *text)
{
	WCHAR reversed[DIGITS];
	int length = 0, out = 0;
	if (state->base == 16) {
		unsigned rest = (unsigned)value;
		for (; rest || length < 4; rest >>= 4)
			reversed[length++] = L"0123456789ABCDEF"[rest & 15];
		text[out++] = L'0';
		text[out++] = L'x';
	} else {
		WCHAR separator = style_of(window) & UDS_NOTHOUSANDS ? 0 : thousands_separator();
		unsigned rest = value < 0 ? 0U - (unsigned)value : (unsigned)value;
		int digits = 0;
		do {
			if (separator && digits && !(digits % 3))
				reversed[length++] = separator;
			reversed[length++] = (WCHAR)(L'0' + rest % 10);
			rest /= 10;
			++digits;
		} while (rest);
		if (value < 0)
			text[out++] = L'-';
	}
	while (length)
		text[out++] = reversed[--length];
	text[out] = 0;
}

static BOOL parse_number(const updown_state *state, const WCHAR *text, int *value)
{
	unsigned long long total = 0;
	BOOL negative = FALSE, any = FALSE;
	if (state->base == 16) {
		if (text[0] == L'0' && (text[1] == L'x' || text[1] == L'X'))
			text += 2;
	} else if (*text == L'-') {
		negative = TRUE;
		++text;
	}
	WCHAR separator = state->base == 16 ? 0 : thousands_separator();
	for (; *text; ++text) {
		int digit;
		if (separator && *text == separator)
			continue;
		if (*text >= L'0' && *text <= L'9')
			digit = *text - L'0';
		else if (state->base == 16 && *text >= L'a' && *text <= L'f')
			digit = *text - L'a' + 10;
		else if (state->base == 16 && *text >= L'A' && *text <= L'F')
			digit = *text - L'A' + 10;
		else
			return FALSE;
		total = total * state->base + digit;
		if (total > 0xffffffffULL)
			return FALSE;
		any = TRUE;
	}
	if (!any)
		return FALSE;
	*value = negative ? (int)(0U - (unsigned)total) : (int)(unsigned)total;
	return TRUE;
}

/* What the buddy shows wins over the stored position. TRUE only when it held a number inside the range. */
static BOOL read_buddy(updown_state *state)
{
	int value;
	if (!state->buddy || !IsWindow(state->buddy))
		return FALSE;
	if (buddy_is_listbox(state)) {
		value = (int)SendMessageW(state->buddy, LB_GETCARETINDEX, 0, 0);
		if (value < 0)
			return FALSE;
	} else {
		WCHAR text[DIGITS];
		GetWindowTextW(state->buddy, text, ARRAYSIZE(text));
		if (!parse_number(state, text, &value))
			return FALSE;
	}
	state->position = clamped(state, value);
	return state->position == value;
}

static void write_buddy(HWND window, const updown_state *state)
{
	WCHAR text[DIGITS], shown[DIGITS];
	if (!state->writes_buddy || !state->buddy || !IsWindow(state->buddy))
		return;
	if (buddy_is_listbox(state)) {
		SendMessageW(state->buddy, LB_SETCURSEL, state->position, 0);
		return;
	}
	format_number(state, window, state->position, text);
	GetWindowTextW(state->buddy, shown, ARRAYSIZE(shown));
	if (lstrcmpW(text, shown))
		SetWindowTextW(state->buddy, text);
}

static int set_position(HWND window, updown_state *state, int value)
{
	int previous = state->position;
	state->position = clamped(state, value);
	write_buddy(window, state);
	return previous;
}

static void attach_beside_buddy(HWND window, const updown_state *state)
{
	DWORD style = style_of(window);
	RECT area, own;
	int width;
	if (!(style & (UDS_ALIGNLEFT | UDS_ALIGNRIGHT)))
		return;
	GetWindowRect(state->buddy, &area);
	GetWindowRect(window, &own);
	MapWindowPoints(HWND_DESKTOP, state->parent, (POINT *)&area, 2);
	width = own.right - own.left;
	if (style & UDS_ALIGNLEFT) {
		SetWindowPos(window, NULL, area.left, area.top, width, area.bottom - area.top, SWP_NOZORDER | SWP_NOACTIVATE);
		area.left += width - BUDDY_OVERLAP;
	} else {
		area.right -= width - BUDDY_OVERLAP;
		SetWindowPos(window, NULL, area.right - BUDDY_OVERLAP, area.top, width, area.bottom - area.top, SWP_NOZORDER | SWP_NOACTIVATE);
	}
	SetWindowPos(state->buddy, NULL, area.left, area.top, area.right - area.left, area.bottom - area.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void detach_subclass(updown_state *state)
{
	if (state->subclassed && IsWindow(state->buddy))
		RemoveWindowSubclass(state->buddy, buddy_procedure, SUBCLASS_ID);
	state->subclassed = FALSE;
}

static HWND set_buddy(HWND window, updown_state *state, HWND buddy)
{
	HWND previous = state->buddy;
	detach_subclass(state);
	state->buddy = buddy && IsWindow(buddy) ? buddy : NULL;
	if (!state->buddy)
		return previous;
	if (style_of(window) & UDS_ARROWKEYS)
		state->subclassed = SetWindowSubclass(state->buddy, buddy_procedure, SUBCLASS_ID, (DWORD_PTR)window);
	attach_beside_buddy(window, state);
	return previous;
}

static int current_increment(const updown_state *state)
{
	DWORD seconds = (GetTickCount() - state->pressed_at) / 1000;
	int increment = 1;
	UINT index;
	for (index = 0; index < state->accel_count; ++index)
		if (state->accel[index].nSec <= seconds)
			increment = (int)state->accel[index].nInc;
	return increment;
}

static void tell_parent_scroll(HWND window, const updown_state *state, WORD code)
{
	UINT message = style_of(window) & UDS_HORZ ? WM_HSCROLL : WM_VSCROLL;
	SendMessageW(state->parent, message, MAKEWPARAM(code, (WORD)state->position), (LPARAM)window);
}

static void step(HWND window, updown_state *state, BOOL up)
{
	NMUPDOWN notice;
	int delta = current_increment(state), target, least, most;
	if (state->low > state->high)
		delta = -delta;
	read_buddy(state);
	notice.hdr.hwndFrom = window;
	notice.hdr.idFrom = GetWindowLongPtrW(window, GWLP_ID);
	notice.hdr.code = UDN_DELTAPOS;
	notice.iPos = state->position;
	notice.iDelta = up ? delta : -delta;
	/* A non-zero answer refuses the change; the program may also have rewritten the delta. */
	if (SendMessageW(state->parent, WM_NOTIFY, notice.hdr.idFrom, (LPARAM)&notice))
		return;
	target = state->position + notice.iDelta;
	least = min(state->low, state->high);
	most = max(state->low, state->high);
	if (target > most)
		target = style_of(window) & UDS_WRAP ? least : most;
	else if (target < least)
		target = style_of(window) & UDS_WRAP ? most : least;
	state->position = target;
	write_buddy(window, state);
	tell_parent_scroll(window, state, SB_THUMBPOSITION);
}

static void press(HWND window, updown_state *state, int pressed)
{
	state->pressed = pressed;
	state->pressed_at = GetTickCount();
	InvalidateRect(window, NULL, FALSE);
	step(window, state, pressed == PRESSED_UP);
}

static void release(HWND window, updown_state *state)
{
	if (!state->pressed)
		return;
	state->pressed = NOT_PRESSED;
	KillTimer(window, REPEAT_TIMER);
	InvalidateRect(window, NULL, FALSE);
	tell_parent_scroll(window, state, SB_ENDSCROLL);
}

static int part_at(HWND window, POINT point)
{
	RECT area;
	GetClientRect(window, &area);
	if (!PtInRect(&area, point))
		return NOT_PRESSED;
	if (style_of(window) & UDS_HORZ)
		return point.x >= (area.left + area.right) / 2 ? PRESSED_UP : PRESSED_DOWN;
	return point.y < (area.top + area.bottom) / 2 ? PRESSED_UP : PRESSED_DOWN;
}

static void draw_part(HWND window, const updown_state *state, HDC dc, const RECT *area, int part)
{
	BOOL horizontal = style_of(window) & UDS_HORZ, up = part == PRESSED_UP;
	BOOL pressed = state->pressed == part, enabled = IsWindowEnabled(window);
	if (state->theme) {
		/* The up and down states are numbered alike in the four parts of the class. */
		int theme_part = horizontal ? (up ? SPNP_UPHORZ : SPNP_DOWNHORZ) : (up ? SPNP_UP : SPNP_DOWN);
		int theme_state = !enabled ? UPS_DISABLED : pressed ? UPS_PRESSED : state->hot == part ? UPS_HOT : UPS_NORMAL;
		DrawThemeBackground(state->theme, dc, theme_part, theme_state, area, NULL);
		return;
	}
	DrawFrameControl(dc, (RECT *)area, DFC_SCROLL, (horizontal ? (up ? DFCS_SCROLLRIGHT : DFCS_SCROLLLEFT) : (up ? DFCS_SCROLLUP : DFCS_SCROLLDOWN)) |
		(enabled ? 0 : DFCS_INACTIVE) | (pressed ? DFCS_PUSHED : 0));
}

static void paint(HWND window, const updown_state *state, HDC dc)
{
	RECT area, first, second;
	BOOL horizontal = style_of(window) & UDS_HORZ;
	GetClientRect(window, &area);
	first = second = area;
	if (horizontal)
		first.right = second.left = (area.left + area.right) / 2;
	else
		first.bottom = second.top = (area.top + area.bottom) / 2;
	draw_part(window, state, dc, &first, horizontal ? PRESSED_DOWN : PRESSED_UP);
	draw_part(window, state, dc, &second, horizontal ? PRESSED_UP : PRESSED_DOWN);
}

static BOOL replace_accel(updown_state *state, UINT count, const UDACCEL *accel)
{
	UDACCEL *copy = NULL;
	if (count) {
		if (!accel || !(copy = HeapAlloc(GetProcessHeap(), 0, count * sizeof(*copy))))
			return FALSE;
		memcpy(copy, accel, count * sizeof(*copy));
	}
	HeapFree(GetProcessHeap(), 0, state->accel);
	state->accel = copy;
	state->accel_count = count;
	return TRUE;
}

static LRESULT CALLBACK buddy_procedure(HWND buddy, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data)
{
	HWND window = (HWND)data;
	updown_state *state = state_of(window);
	switch (message) {
	case WM_KEYDOWN:
		if (wparam == VK_UP || wparam == VK_DOWN) {
			press(window, state, wparam == VK_UP ? PRESSED_UP : PRESSED_DOWN);
			return 0;
		}
		break;
	case WM_KEYUP:
		if (wparam == VK_UP || wparam == VK_DOWN)
			release(window, state);
		break;
	case WM_MOUSEWHEEL:
		state->pressed_at = GetTickCount();
		step(window, state, GET_WHEEL_DELTA_WPARAM(wparam) > 0);
		return 0;
	case WM_NCDESTROY:
		RemoveWindowSubclass(buddy, buddy_procedure, id);
		state->subclassed = FALSE;
		state->buddy = NULL;
		break;
	}
	return DefSubclassProc(buddy, message, wparam, lparam);
}

static void apply_default_size(HWND window, const CREATESTRUCTW *create)
{
	int width = create->cx, height = create->cy;
	if (create->style & UDS_HORZ)
		height = min(height, GetSystemMetrics(SM_CYHSCROLL));
	else
		width = GetSystemMetrics(SM_CXVSCROLL);
	SetWindowPos(window, NULL, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK updown_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	updown_state *state = state_of(window);
	if (!state && message != WM_NCCREATE)
		return DefWindowProcW(window, message, wparam, lparam);
	switch (message) {
	case WM_NCCREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
		if (!state)
			return FALSE;
		state->parent = create->hwndParent;
		state->low = 100;
		state->base = 10;
		state->writes_buddy = (create->style & UDS_SETBUDDYINT) != 0;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		if (!replace_accel(state, ARRAYSIZE(default_accel), default_accel))
			return FALSE;
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_CREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state->unicode = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_UNICODE;
		state->theme = OpenThemeData(window, L"Spin");
		apply_default_size(window, create);
		if (create->style & UDS_AUTOBUDDY)
			set_buddy(window, state, GetWindow(window, GW_HWNDPREV));
		return 0;
	}
	case WM_DESTROY:
		detach_subclass(state);
		CloseThemeData(state->theme);
		state->theme = NULL;
		return 0;
	case WM_THEMECHANGED:
		CloseThemeData(state->theme);
		state->theme = OpenThemeData(window, L"Spin");
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_MOUSEMOVE: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		int part = part_at(window, point);
		if (part != state->hot) {
			TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
			state->hot = part;
			TrackMouseEvent(&track);
			InvalidateRect(window, NULL, FALSE);
		}
		return 0;
	}
	case WM_MOUSELEAVE:
		state->hot = NOT_PRESSED;
		InvalidateRect(window, NULL, FALSE);
		return 0;
	case WM_NCDESTROY:
		HeapFree(GetProcessHeap(), 0, state->accel);
		HeapFree(GetProcessHeap(), 0, state);
		SetWindowLongPtrW(window, 0, 0);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_ENABLE:
		InvalidateRect(window, NULL, FALSE);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
	case WM_PRINTCLIENT:
		if (wparam) {
			paint(window, state, (HDC)wparam);
		} else {
			PAINTSTRUCT painting;
			HDC dc = BeginPaint(window, &painting);
			paint(window, state, dc);
			EndPaint(window, &painting);
		}
		return 0;
	case WM_LBUTTONDOWN: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		int part = part_at(window, point);
		if (!part || !IsWindowEnabled(window))
			return 0;
		SetFocus(state->buddy && (style_of(window) & UDS_ARROWKEYS) ? state->buddy : window);
		SetCapture(window);
		press(window, state, part);
		SetTimer(window, REPEAT_TIMER, FIRST_REPEAT_MS, NULL);
		return 0;
	}
	case WM_LBUTTONUP:
		release(window, state);
		if (GetCapture() == window)
			ReleaseCapture();
		return 0;
	case WM_CAPTURECHANGED:
		release(window, state);
		return 0;
	case WM_TIMER:
		if (wparam == REPEAT_TIMER && state->pressed) {
			POINT point;
			SetTimer(window, REPEAT_TIMER, NEXT_REPEAT_MS, NULL);
			GetCursorPos(&point);
			ScreenToClient(window, &point);
			if (part_at(window, point) == state->pressed)
				step(window, state, state->pressed == PRESSED_UP);
		}
		return 0;
	case WM_KEYDOWN:
		if ((style_of(window) & UDS_ARROWKEYS) && (wparam == VK_UP || wparam == VK_DOWN))
			press(window, state, wparam == VK_UP ? PRESSED_UP : PRESSED_DOWN);
		return 0;
	case WM_KEYUP:
		if (wparam == VK_UP || wparam == VK_DOWN)
			release(window, state);
		return 0;
	case WM_MOUSEWHEEL:
		state->pressed_at = GetTickCount();
		step(window, state, GET_WHEEL_DELTA_WPARAM(wparam) > 0);
		return 0;
	case UDM_SETRANGE:
		state->high = (short)LOWORD(lparam);
		state->low = (short)HIWORD(lparam);
		return 0;
	case UDM_GETRANGE:
		return MAKELONG((short)state->high, (short)state->low);
	case UDM_SETRANGE32:
		state->low = (int)wparam;
		state->high = (int)lparam;
		return 0;
	case UDM_GETRANGE32:
		if (wparam)
			*(int *)wparam = state->low;
		if (lparam)
			*(int *)lparam = state->high;
		return 0;
	case UDM_SETPOS:
		return (WORD)set_position(window, state, (short)LOWORD(lparam));
	case UDM_SETPOS32:
		return set_position(window, state, (int)lparam);
	case UDM_GETPOS: {
		BOOL valid = read_buddy(state);
		return MAKELONG((short)state->position, !valid);
	}
	case UDM_GETPOS32: {
		BOOL valid = read_buddy(state);
		if (lparam)
			*(BOOL *)lparam = !valid;
		return state->position;
	}
	case UDM_SETBUDDY:
		return (LRESULT)set_buddy(window, state, (HWND)wparam);
	case UDM_GETBUDDY:
		return (LRESULT)state->buddy;
	case UDM_SETBASE: {
		int previous = state->base;
		if (wparam != 10 && wparam != 16)
			return 0;
		state->base = (int)wparam;
		write_buddy(window, state);
		return previous;
	}
	case UDM_GETBASE:
		return state->base;
	case UDM_SETACCEL:
		return replace_accel(state, (UINT)wparam, (const UDACCEL *)lparam);
	case UDM_GETACCEL:
		if (lparam && wparam)
			memcpy((void *)lparam, state->accel, min((UINT)wparam, state->accel_count) * sizeof(UDACCEL));
		return state->accel_count;
	case UDM_SETUNICODEFORMAT: {
		BOOL previous = state->unicode;
		state->unicode = wparam != 0;
		return previous;
	}
	case UDM_GETUNICODEFORMAT:
		return state->unicode;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void UPDOWN_Register(void)
{
	WNDCLASSW definition = {0};
	definition.style = CS_GLOBALCLASS | CS_VREDRAW | CS_HREDRAW;
	definition.lpfnWndProc = updown_procedure;
	definition.cbWndExtra = sizeof(void *);
	definition.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	definition.lpszClassName = UPDOWN_CLASSW;
	RegisterClassW(&definition);
}

void UPDOWN_Unregister(void)
{
	UnregisterClassW(UPDOWN_CLASSW, NULL);
}
