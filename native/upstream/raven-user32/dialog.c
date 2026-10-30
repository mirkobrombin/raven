/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include "user32_private.h"

/* The dialog manager, from Microsoft's documentation of dialog boxes and of the DLGTEMPLATE and DLGTEMPLATEEX formats. What the manager keeps for a dialog sits
 * in a property, because the window's extra bytes belong to DWLP_MSGRESULT, DWLP_DLGPROC and DWLP_USER. */

static const WCHAR state_property[] = L"RavenDialogState";

typedef struct {
	HFONT font;
	BOOL font_owned;
	HMENU menu;
	HWND saved_focus;
	UINT default_id;
	int base_x, base_y;
	/* A DLGPROC taking ANSI messages, from the A creation functions. */
	BOOL ansi;
	BOOL ended;
	INT_PTR result;
} dialog_state;

static dialog_state *state_of(HWND window)
{
	return (dialog_state *)GetPropW(window, state_property);
}

/* The base units of a font, the measure the documentation of MapDialogRect gives: the alphabet's mean width and the font's height. */
static void font_units(HDC dc, HFONT font, int *base_x, int *base_y)
{
	HGDIOBJ previous = font ? SelectObject(dc, font) : NULL;
	TEXTMETRICW metrics;
	SIZE size = {0, 0};
	GetTextMetricsW(dc, &metrics);
	GetTextExtentPoint32W(dc, L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 52, &size);
	*base_x = (size.cx / 26 + 1) / 2;
	*base_y = metrics.tmHeight;
	if (previous)
		SelectObject(dc, previous);
}

LONG WINAPI GetDialogBaseUnits(VOID)
{
	static LONG units;
	if (!units) {
		HDC dc = GetDC(NULL);
		int base_x = 8, base_y = 16;
		if (dc) {
			font_units(dc, GetStockObject(SYSTEM_FONT), &base_x, &base_y);
			ReleaseDC(NULL, dc);
		}
		units = MAKELONG(base_x, base_y);
	}
	return units;
}

BOOL WINAPI MapDialogRect(HWND window, LPRECT rect)
{
	dialog_state *state = state_of(window);
	if (!state)
		return FALSE;
	rect->left = MulDiv(rect->left, state->base_x, 4);
	rect->right = MulDiv(rect->right, state->base_x, 4);
	rect->top = MulDiv(rect->top, state->base_y, 8);
	rect->bottom = MulDiv(rect->bottom, state->base_y, 8);
	return TRUE;
}

/* ---- reading a template ---- */

typedef struct {
	DWORD style, ex_style, help_id;
	short x, y, cx, cy;
	DWORD id;
	const WCHAR *class_name, *title;
	const void *extra;
} item_template;

/* A menu, class or title field: nothing, a 16-bit ordinal behind 0xffff, or a string. An ordinal comes back as an integer-atom style pointer. */
static const WORD *read_name(const WORD *cursor, const WCHAR **name)
{
	if (*cursor == 0xffff) {
		*name = (const WCHAR *)(ULONG_PTR)cursor[1];
		return cursor + 2;
	}
	*name = (const WCHAR *)cursor;
	while (*cursor)
		cursor++;
	return cursor + 1;
}

static const WORD *align_dword(const WORD *cursor)
{
	return (const WORD *)(((ULONG_PTR)cursor + 3) & ~(ULONG_PTR)3);
}

static const WORD *read_item(const WORD *cursor, BOOL extended, item_template *item)
{
	cursor = align_dword(cursor);
	if (extended) {
		item->help_id = *(const DWORD *)cursor;
		item->ex_style = *(const DWORD *)(cursor + 2);
		item->style = *(const DWORD *)(cursor + 4);
		cursor += 6;
	} else {
		item->help_id = 0;
		item->style = *(const DWORD *)cursor;
		item->ex_style = *(const DWORD *)(cursor + 2);
		cursor += 4;
	}
	item->x = (short)cursor[0];
	item->y = (short)cursor[1];
	item->cx = (short)cursor[2];
	item->cy = (short)cursor[3];
	cursor += 4;
	if (extended) {
		item->id = *(const DWORD *)cursor;
		cursor += 2;
	} else {
		item->id = *cursor++;
	}
	cursor = read_name(cursor, &item->class_name);
	cursor = read_name(cursor, &item->title);
	WORD extra = *cursor++;
	item->extra = extra ? cursor : NULL;
	return (const WORD *)((const BYTE *)cursor + extra);
}

static const WCHAR *predefined_class(ULONG_PTR ordinal)
{
	switch (ordinal) {
	case 0x80: return L"Button";
	case 0x81: return L"Edit";
	case 0x82: return L"Static";
	case 0x83: return L"ListBox";
	case 0x84: return L"ScrollBar";
	case 0x85: return L"ComboBox";
	}
	return NULL;
}

static BOOL is_ordinal(const WCHAR *name)
{
	return !((ULONG_PTR)name >> 16);
}

/* An ordinal title is a resource id, which a static control shows as its icon. */
static const WCHAR *item_title(const item_template *item, WCHAR *buffer)
{
	if (!is_ordinal(item->title))
		return item->title;
	buffer[0] = 0xffff;
	buffer[1] = (WCHAR)(ULONG_PTR)item->title;
	buffer[2] = 0;
	return buffer;
}

static BOOL create_children(HWND dialog, HINSTANCE instance, const WORD *cursor, int count, BOOL extended, const dialog_state *state, DWORD dialog_style)
{
	for (int index = 0; index < count; index++) {
		item_template item;
		WCHAR ordinal_title[3];
		cursor = read_item(cursor, extended, &item);
		const WCHAR *class_name = is_ordinal(item.class_name) ? predefined_class((ULONG_PTR)item.class_name) : item.class_name;
		if (!class_name)
			class_name = item.class_name;
		HWND child = CreateWindowExW(item.ex_style | WS_EX_NOPARENTNOTIFY, class_name, item_title(&item, ordinal_title), item.style | WS_CHILD,
			MulDiv(item.x, state->base_x, 4), MulDiv(item.y, state->base_y, 8), MulDiv(item.cx, state->base_x, 4), MulDiv(item.cy, state->base_y, 8),
			dialog, (HMENU)(ULONG_PTR)item.id, instance, (LPVOID)item.extra);
		if (!child) {
			if (!(dialog_style & DS_NOFAILCREATE))
				return FALSE;
			continue;
		}
		if (state->font)
			SendMessageW(child, WM_SETFONT, (WPARAM)state->font, FALSE);
		if (item.help_id)
			SetWindowContextHelpId(child, item.help_id);
		/* The first default push button named by the template is the dialog's default. */
		if ((SendMessageW(child, WM_GETDLGCODE, 0, 0) & DLGC_DEFPUSHBUTTON) && !((dialog_state *)state)->default_id)
			((dialog_state *)state)->default_id = item.id;
	}
	return TRUE;
}

/* ---- calling the dialog procedure ---- */

static INT_PTR call_procedure(HWND window, DLGPROC procedure, UINT message, WPARAM wparam, LPARAM lparam, BOOL ansi)
{
	if (ansi)
		return (INT_PTR)CallWindowProcA((WNDPROC)procedure, window, message, wparam, lparam);
	return procedure(window, message, wparam, lparam);
}

/* The messages whose answer is the procedure's return value itself, as the documentation of DialogProc lists them. */
static BOOL returns_directly(UINT message)
{
	switch (message) {
	case WM_CHARTOITEM:
	case WM_COMPAREITEM:
	case WM_CTLCOLORBTN:
	case WM_CTLCOLORDLG:
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX:
	case WM_CTLCOLORSCROLLBAR:
	case WM_CTLCOLORSTATIC:
	case WM_INITDIALOG:
	case WM_QUERYDRAGICON:
	case WM_VKEYTOITEM:
		return TRUE;
	}
	return FALSE;
}

/* ---- creation ---- */

static HWND create_dialog(HINSTANCE instance, const void *resource, HWND owner, DLGPROC procedure, LPARAM parameter, BOOL ansi, BOOL modal)
{
	const WORD *cursor = resource;
	BOOL extended = cursor[0] == 1 && cursor[1] == 0xffff;
	DWORD style, ex_style, help_id = 0;
	if (extended) {
		help_id = *(const DWORD *)(cursor + 2);
		ex_style = *(const DWORD *)(cursor + 4);
		style = *(const DWORD *)(cursor + 6);
		cursor += 8;
	} else {
		style = *(const DWORD *)cursor;
		ex_style = *(const DWORD *)(cursor + 2);
		cursor += 4;
	}
	int count = *cursor++;
	short x = (short)cursor[0], y = (short)cursor[1], cx = (short)cursor[2], cy = (short)cursor[3];
	cursor += 4;
	const WCHAR *menu_name, *class_name, *title;
	cursor = read_name(cursor, &menu_name);
	cursor = read_name(cursor, &class_name);
	cursor = read_name(cursor, &title);
	if (is_ordinal(title))
		title = L"";

	dialog_state *state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
	if (!state)
		return NULL;
	state->ansi = ansi;
	if (style & DS_SETFONT) {
		int points = (short)*cursor++, weight = FW_DONTCARE;
		BOOL italic = FALSE;
		BYTE charset = DEFAULT_CHARSET;
		if (extended) {
			weight = (short)cursor[0];
			italic = LOBYTE(cursor[1]);
			charset = HIBYTE(cursor[1]);
			cursor += 2;
		}
		const WCHAR *face = (const WCHAR *)cursor;
		while (*cursor)
			cursor++;
		cursor++;
		HDC screen = GetDC(NULL);
		int height = -MulDiv(points, screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96, 72);
		if (screen)
			ReleaseDC(NULL, screen);
		/* The shell font stands for the theme's dialog font. */
		BOOL shell = (style & DS_SHELLFONT) == DS_SHELLFONT && !lstrcmpiW(face, L"MS Shell Dlg");
		state->font = CreateFontW(height, 0, 0, 0, weight, italic, FALSE, FALSE, charset, 0, 0, 0, 0, shell ? L"MS Shell Dlg 2" : face);
		state->font_owned = state->font != NULL;
	}
	HDC dc = GetDC(NULL);
	state->base_x = LOWORD(GetDialogBaseUnits());
	state->base_y = HIWORD(GetDialogBaseUnits());
	if (dc && state->font)
		font_units(dc, state->font, &state->base_x, &state->base_y);
	if (dc)
		ReleaseDC(NULL, dc);

	RECT frame = {0, 0, MulDiv(cx, state->base_x, 4), MulDiv(cy, state->base_y, 8)};
	if (style & DS_MODALFRAME)
		ex_style |= WS_EX_DLGMODALFRAME;
	if (style & DS_CONTEXTHELP)
		ex_style |= WS_EX_CONTEXTHELP;
	/* A dialog is created with a caption whatever else it asks for, unless it is a child. */
	HMENU menu = NULL;
	if (!is_ordinal(menu_name) ? *menu_name : (menu_name != NULL))
		menu = LoadMenuW(instance, menu_name);
	AdjustWindowRectEx(&frame, style & ~WS_VISIBLE, menu != NULL, ex_style);
	int width = frame.right - frame.left, height = frame.bottom - frame.top, left, top;
	if (x == (short)CW_USEDEFAULT && !(style & WS_CHILD)) {
		left = CW_USEDEFAULT;
		top = CW_USEDEFAULT;
	} else if (style & (DS_CENTER | DS_CENTERMOUSE)) {
		RECT work;
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		if (style & DS_CENTERMOUSE) {
			POINT mouse;
			GetCursorPos(&mouse);
			left = mouse.x - width / 2;
			top = mouse.y - height / 2;
		} else {
			left = work.left + (work.right - work.left - width) / 2;
			top = work.top + (work.bottom - work.top - height) / 2;
		}
	} else {
		POINT origin = {MulDiv(x, state->base_x, 4) + frame.left, MulDiv(y, state->base_y, 8) + frame.top};
		/* The template's position is in the owner's client area, except with DS_ABSALIGN, unless it is a child, whose coordinates are its parent's already. */
		if (owner && !(style & (WS_CHILD | DS_ABSALIGN)))
			ClientToScreen(owner, &origin);
		left = origin.x;
		top = origin.y;
	}
	/* Modal dialogs show themselves once their first messages are through, so nothing of their first paint is seen half done. */
	DWORD create_style = style & ~WS_VISIBLE;
	const WCHAR *window_class = is_ordinal(class_name) ? (class_name ? class_name : DIALOG_CLASS_NAME) : (*class_name ? class_name : DIALOG_CLASS_NAME);
	HWND window = CreateWindowExW(ex_style, window_class, title, create_style, left, top, width, height, owner, menu, instance, NULL);
	if (!window) {
		if (state->font_owned)
			DeleteObject(state->font);
		if (menu)
			DestroyMenu(menu);
		HeapFree(GetProcessHeap(), 0, state);
		return NULL;
	}
	SetPropW(window, state_property, state);
	state->menu = menu;
	if (help_id)
		SetWindowContextHelpId(window, help_id);
	SetWindowLongPtrW(window, DWLP_DLGPROC, (LONG_PTR)procedure);
	if (state->font)
		SendMessageW(window, WM_SETFONT, (WPARAM)state->font, FALSE);
	if (!create_children(window, instance, cursor, count, extended, state, style)) {
		DestroyWindow(window);
		return NULL;
	}

	/* The documentation of WM_INITDIALOG: wparam is the control to get the focus, and the procedure returns whether the system sets it. */
	HWND first = GetNextDlgTabItem(window, NULL, FALSE);
	if (procedure && call_procedure(window, procedure, WM_INITDIALOG, (WPARAM)first, parameter, ansi)) {
		first = GetNextDlgTabItem(window, NULL, FALSE);
		if (first && IsWindow(window)) {
			SetFocus(first);
			if (SendMessageW(first, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL)
				SendMessageW(first, EM_SETSEL, 0, -1);
		}
	}
	if (!IsWindow(window))
		return NULL;
	/* Whatever has the focus now gets it back when the window first activates. */
	HWND focused = GetFocus();
	if (focused && IsChild(window, focused))
		state->saved_focus = focused;
	if ((style & WS_VISIBLE) && !modal && !(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE))
		ShowWindow(window, SW_SHOWNORMAL);
	if (style & DS_SETFOREGROUND)
		SetForegroundWindow(window);
	return window;
}

HWND WINAPI CreateDialogIndirectParamW(HINSTANCE instance, LPCDLGTEMPLATEW resource, HWND owner, DLGPROC procedure, LPARAM parameter)
{
	return resource ? create_dialog(instance, resource, owner, procedure, parameter, FALSE, FALSE) : NULL;
}

HWND WINAPI CreateDialogIndirectParamA(HINSTANCE instance, LPCDLGTEMPLATEA resource, HWND owner, DLGPROC procedure, LPARAM parameter)
{
	return resource ? create_dialog(instance, resource, owner, procedure, parameter, TRUE, FALSE) : NULL;
}

HWND WINAPI CreateDialogIndirectParamAorW(HINSTANCE instance, LPCVOID resource, HWND owner, DLGPROC procedure, LPARAM parameter, DWORD flags)
{
	return resource ? create_dialog(instance, resource, owner, procedure, parameter, !(flags & 1), FALSE) : NULL;
}

static const void *load_template(HINSTANCE instance, const void *name, BOOL ansi)
{
	HRSRC found = ansi ? FindResourceA(instance, name, (LPCSTR)RT_DIALOG) : FindResourceW(instance, name, (LPCWSTR)RT_DIALOG);
	HGLOBAL data = found ? LoadResource(instance, found) : NULL;
	return data ? LockResource(data) : NULL;
}

HWND WINAPI CreateDialogParamW(HINSTANCE instance, LPCWSTR name, HWND owner, DLGPROC procedure, LPARAM parameter)
{
	const void *resource = load_template(instance, name, FALSE);
	return resource ? create_dialog(instance, resource, owner, procedure, parameter, FALSE, FALSE) : NULL;
}

HWND WINAPI CreateDialogParamA(HINSTANCE instance, LPCSTR name, HWND owner, DLGPROC procedure, LPARAM parameter)
{
	const void *resource = load_template(instance, name, TRUE);
	return resource ? create_dialog(instance, resource, owner, procedure, parameter, TRUE, FALSE) : NULL;
}

/* ---- the modal loop ---- */

static HWND top_level_of(HWND window)
{
	HWND ancestor = window ? GetAncestor(window, GA_ROOT) : NULL;
	return ancestor;
}

static INT_PTR run_modal(HINSTANCE instance, const void *resource, HWND parent, DLGPROC procedure, LPARAM parameter, BOOL ansi)
{
	if (!resource)
		return -1;
	HWND owner = top_level_of(parent);
	HWND window = create_dialog(instance, resource, owner, procedure, parameter, ansi, TRUE);
	if (!window)
		return -1;
	dialog_state *state = state_of(window);
	BOOL owner_was_enabled = owner && IsWindowEnabled(owner);
	if (owner_was_enabled)
		EnableWindow(owner, FALSE);
	MSG message;
	BOOL shown = FALSE;
	const DWORD style = (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
	while (state && !state->ended) {
		if (!PeekMessageW(&message, NULL, 0, 0, PM_NOREMOVE)) {
			/* The message queue ran dry: the moment the dialog first shows, and when its owner hears the modal loop is idle. */
			if (!shown && !(style & DS_NOIDLEMSG))
				SendMessageW(owner, WM_ENTERIDLE, MSGF_DIALOGBOX, (LPARAM)window);
			if (!shown && !state->ended) {
				shown = TRUE;
				ShowWindow(window, SW_SHOWNORMAL);
				UpdateWindow(window);
				continue;
			}
		}
		BOOL got = GetMessageW(&message, NULL, 0, 0);
		if (got <= 0) {
			/* WM_QUIT ends the dialog and goes back on the queue for the outer loop. */
			if (!got)
				PostQuitMessage((int)message.wParam);
			state->result = -1;
			break;
		}
		if (!IsWindow(window)) {
			state = NULL;
			TranslateMessage(&message);
			DispatchMessageW(&message);
			break;
		}
		if (!IsDialogMessageW(window, &message)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
		if (!IsWindow(window))
			state = NULL;
	}
	INT_PTR result = state ? state->result : 0;
	if (owner_was_enabled && owner)
		EnableWindow(owner, TRUE);
	if (state && IsWindow(window))
		DestroyWindow(window);
	return result;
}

INT_PTR WINAPI DialogBoxIndirectParamW(HINSTANCE instance, LPCDLGTEMPLATEW resource, HWND owner, DLGPROC procedure, LPARAM parameter)
{
	return run_modal(instance, resource, owner, procedure, parameter, FALSE);
}

INT_PTR WINAPI DialogBoxIndirectParamA(HINSTANCE instance, LPCDLGTEMPLATEA resource, HWND owner, DLGPROC procedure, LPARAM parameter)
{
	return run_modal(instance, resource, owner, procedure, parameter, TRUE);
}

INT_PTR WINAPI DialogBoxIndirectParamAorW(HINSTANCE instance, LPCVOID resource, HWND owner, DLGPROC procedure, LPARAM parameter, DWORD flags)
{
	return run_modal(instance, resource, owner, procedure, parameter, !(flags & 1));
}

INT_PTR WINAPI DialogBoxParamW(HINSTANCE instance, LPCWSTR name, HWND owner, DLGPROC procedure, LPARAM parameter)
{
	return run_modal(instance, load_template(instance, name, FALSE), owner, procedure, parameter, FALSE);
}

INT_PTR WINAPI DialogBoxParamA(HINSTANCE instance, LPCSTR name, HWND owner, DLGPROC procedure, LPARAM parameter)
{
	return run_modal(instance, load_template(instance, name, TRUE), owner, procedure, parameter, TRUE);
}

BOOL WINAPI EndDialog(HWND window, INT_PTR result)
{
	dialog_state *state = state_of(window);
	if (!state)
		return FALSE;
	state->result = result;
	state->ended = TRUE;
	/* The dialog goes out of sight now, while its destruction waits for the loop. */
	SetWindowPos(window, NULL, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	PostMessageW(window, WM_NULL, 0, 0);
	return TRUE;
}

/* ---- finding controls ---- */

HWND WINAPI GetDlgItem(HWND dialog, int id)
{
	for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
		if (GetWindowLongPtrW(child, GWLP_ID) == id)
			return child;
	}
	return NULL;
}

int WINAPI GetDlgCtrlID(HWND window)
{
	return (int)GetWindowLongPtrW(window, GWLP_ID);
}

static BOOL is_tab_stop(HWND window)
{
	DWORD style = (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
	return (style & (WS_VISIBLE | WS_DISABLED | WS_TABSTOP)) == (WS_VISIBLE | WS_TABSTOP);
}

static BOOL is_control_parent(HWND window)
{
	DWORD style = (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
	return (style & (WS_VISIBLE | WS_DISABLED)) == WS_VISIBLE && (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_CONTROLPARENT);
}

/* The controls in tab order, control parents standing for what is inside them. */
static void collect_tab_stops(HWND parent, HWND *list, int *count, int capacity)
{
	for (HWND child = GetWindow(parent, GW_CHILD); child && *count < capacity; child = GetWindow(child, GW_HWNDNEXT)) {
		if (is_control_parent(child))
			collect_tab_stops(child, list, count, capacity);
		else if (is_tab_stop(child))
			list[(*count)++] = child;
	}
}

HWND WINAPI GetNextDlgTabItem(HWND dialog, HWND control, BOOL previous)
{
	HWND list[1024];
	int count = 0;
	collect_tab_stops(dialog, list, &count, 1024);
	if (!count)
		return NULL;
	if (control)
		for (int index = 0; index < count; index++)
			if (list[index] == control)
				return list[(index + (previous ? count - 1 : 1)) % count];
	/* With no control, or one out of the order: the first, or the last going back. */
	return previous && control ? list[count - 1] : list[0];
}

HWND WINAPI GetNextDlgGroupItem(HWND dialog, HWND control, BOOL previous)
{
	HWND parent = control ? GetParent(control) : dialog;
	HWND start = control;
	if (!parent)
		return NULL;
	if (!start)
		start = GetWindow(dialog, GW_CHILD);
	if (!start)
		return NULL;
	/* The group is the run of siblings from a WS_GROUP control to before the next one, closed into a ring. */
	HWND first = start;
	while (!(GetWindowLongPtrW(first, GWL_STYLE) & WS_GROUP)) {
		HWND before = GetWindow(first, GW_HWNDPREV);
		if (!before)
			break;
		first = before;
	}
	HWND found = NULL, walk = first;
	BOOL after_start = FALSE;
	do {
		if (walk == start) {
			after_start = TRUE;
			if (found && previous)
				return found;
		} else if ((GetWindowLongPtrW(walk, GWL_STYLE) & (WS_VISIBLE | WS_DISABLED)) == WS_VISIBLE) {
			if (!previous && after_start)
				return walk;
			if (!previous && !found)
				found = walk;
			if (previous)
				found = walk;
		}
		walk = GetWindow(walk, GW_HWNDNEXT);
	} while (walk && !(GetWindowLongPtrW(walk, GWL_STYLE) & WS_GROUP));
	return found ? found : start;
}

/* ---- text and state of controls ---- */

LRESULT WINAPI SendDlgItemMessageW(HWND dialog, int id, UINT message, WPARAM wparam, LPARAM lparam)
{
	HWND item = GetDlgItem(dialog, id);
	return item ? SendMessageW(item, message, wparam, lparam) : 0;
}

LRESULT WINAPI SendDlgItemMessageA(HWND dialog, int id, UINT message, WPARAM wparam, LPARAM lparam)
{
	HWND item = GetDlgItem(dialog, id);
	return item ? SendMessageA(item, message, wparam, lparam) : 0;
}

BOOL WINAPI SetDlgItemTextW(HWND dialog, int id, LPCWSTR text)
{
	HWND item = GetDlgItem(dialog, id);
	return item ? SetWindowTextW(item, text) : FALSE;
}

BOOL WINAPI SetDlgItemTextA(HWND dialog, int id, LPCSTR text)
{
	HWND item = GetDlgItem(dialog, id);
	return item ? SetWindowTextA(item, text) : FALSE;
}

UINT WINAPI GetDlgItemTextW(HWND dialog, int id, LPWSTR text, int capacity)
{
	HWND item = GetDlgItem(dialog, id);
	if (item)
		return (UINT)GetWindowTextW(item, text, capacity);
	if (capacity > 0)
		text[0] = 0;
	return 0;
}

UINT WINAPI GetDlgItemTextA(HWND dialog, int id, LPSTR text, int capacity)
{
	HWND item = GetDlgItem(dialog, id);
	if (item)
		return (UINT)GetWindowTextA(item, text, capacity);
	if (capacity > 0)
		text[0] = 0;
	return 0;
}

BOOL WINAPI SetDlgItemInt(HWND dialog, int id, UINT value, BOOL is_signed)
{
	WCHAR text[16];
	wsprintfW(text, is_signed ? L"%d" : L"%u", value);
	return SetDlgItemTextW(dialog, id, text);
}

UINT WINAPI GetDlgItemInt(HWND dialog, int id, BOOL *translated, BOOL is_signed)
{
	WCHAR text[32];
	if (translated)
		*translated = FALSE;
	if (!GetDlgItemTextW(dialog, id, text, 32))
		return 0;
	const WCHAR *cursor = text;
	while (*cursor == L' ' || *cursor == L'\t')
		cursor++;
	BOOL negative = FALSE;
	if (is_signed && (*cursor == L'-' || *cursor == L'+'))
		negative = *cursor++ == L'-';
	if (*cursor < L'0' || *cursor > L'9')
		return 0;
	ULONGLONG value = 0;
	for (; *cursor >= L'0' && *cursor <= L'9'; cursor++) {
		value = value * 10 + (ULONGLONG)(*cursor - L'0');
		if (value > (is_signed ? (ULONGLONG)0x80000000u : (ULONGLONG)0xffffffffu))
			return 0;
	}
	while (*cursor == L' ' || *cursor == L'\t')
		cursor++;
	if (*cursor || (is_signed && !negative && value == 0x80000000u))
		return 0;
	if (translated)
		*translated = TRUE;
	return negative ? (UINT)-(LONGLONG)value : (UINT)value;
}

BOOL WINAPI CheckDlgButton(HWND dialog, int id, UINT state)
{
	SendDlgItemMessageW(dialog, id, BM_SETCHECK, state, 0);
	return TRUE;
}

UINT WINAPI IsDlgButtonChecked(HWND dialog, int id)
{
	return (UINT)SendDlgItemMessageW(dialog, id, BM_GETCHECK, 0, 0);
}

BOOL WINAPI CheckRadioButton(HWND dialog, int first, int last, int checked)
{
	for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
		int id = GetDlgCtrlID(child);
		if (id >= first && id <= last)
			SendMessageW(child, BM_SETCHECK, id == checked, 0);
	}
	return TRUE;
}

/* ---- directory lists ---- */

static int fill_directory_list(HWND dialog, LPWSTR path, int list_id, int static_id, UINT type, BOOL combo)
{
	WCHAR current[MAX_PATH], *name = NULL;
	const WCHAR *pattern = path && *path ? path : L"*.*";
	/* A directory in front of the pattern becomes the current one. */
	if (path && *path) {
		WCHAR *cut = NULL;
		for (WCHAR *cursor = path; *cursor; cursor++)
			if (*cursor == L'\\' || *cursor == L'/' || *cursor == L':')
				cut = cursor;
		if (cut) {
			WCHAR saved = cut[1];
			cut[1] = 0;
			if (!SetCurrentDirectoryW(path)) {
				cut[1] = saved;
				return 0;
			}
			cut[1] = saved;
			pattern = cut + 1;
			if (!*pattern)
				pattern = L"*.*";
		}
	}
	(void)name;
	if (list_id) {
		HWND list = GetDlgItem(dialog, list_id);
		if (!list)
			return 0;
		SendMessageW(list, combo ? CB_RESETCONTENT : LB_RESETCONTENT, 0, 0);
		if (type & DDL_DIRECTORY) {
			/* Asked apart, so that the pattern narrows only the files and every directory shows. */
			SendMessageW(list, combo ? CB_DIR : LB_DIR, (type & ~(UINT)DDL_DRIVES & ~(UINT)DDL_DIRECTORY) | DDL_EXCLUSIVE | DDL_DIRECTORY, (LPARAM)L"*.*");
			if (!(type & DDL_EXCLUSIVE))
				SendMessageW(list, combo ? CB_DIR : LB_DIR, type & ~(UINT)DDL_DIRECTORY & ~(UINT)DDL_DRIVES, (LPARAM)pattern);
			if (type & DDL_DRIVES)
				SendMessageW(list, combo ? CB_DIR : LB_DIR, DDL_EXCLUSIVE | DDL_DRIVES, (LPARAM)L"*.*");
		} else {
			SendMessageW(list, combo ? CB_DIR : LB_DIR, type, (LPARAM)pattern);
		}
	}
	if (static_id && GetCurrentDirectoryW(MAX_PATH, current))
		SetDlgItemTextW(dialog, static_id, current);
	return 1;
}

int WINAPI DlgDirListW(HWND dialog, LPWSTR path, int list, int static_id, UINT type)
{
	return fill_directory_list(dialog, path, list, static_id, type, FALSE);
}

int WINAPI DlgDirListComboBoxW(HWND dialog, LPWSTR path, int list, int static_id, UINT type)
{
	return fill_directory_list(dialog, path, list, static_id, type, TRUE);
}

static int list_ansi(HWND dialog, LPSTR path, int list, int static_id, UINT type, BOOL combo)
{
	WCHAR wide[MAX_PATH] = {0};
	if (path)
		MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
	return fill_directory_list(dialog, path ? wide : NULL, list, static_id, type, combo);
}

int WINAPI DlgDirListA(HWND dialog, LPSTR path, int list, int static_id, UINT type)
{
	return list_ansi(dialog, path, list, static_id, type, FALSE);
}

int WINAPI DlgDirListComboBoxA(HWND dialog, LPSTR path, int list, int static_id, UINT type)
{
	return list_ansi(dialog, path, list, static_id, type, TRUE);
}

/* Returns whether the selection is a directory or a drive, cleaned of its list decoration: [name] and [-c-]. */
static BOOL directory_selection(HWND dialog, LPWSTR text, int capacity, int list_id, BOOL combo)
{
	HWND list = GetDlgItem(dialog, list_id);
	WCHAR buffer[MAX_PATH + 4];
	if (!list)
		return FALSE;
	LRESULT selected = SendMessageW(list, combo ? CB_GETCURSEL : LB_GETCURSEL, 0, 0);
	if (selected < 0)
		return FALSE;
	if (SendMessageW(list, combo ? CB_GETLBTEXT : LB_GETTEXT, (WPARAM)selected, (LPARAM)buffer) <= 0)
		return FALSE;
	WCHAR *start = buffer;
	BOOL directory = FALSE;
	if (buffer[0] == L'[') {
		directory = TRUE;
		int length = lstrlenW(buffer);
		if (buffer[1] == L'-') {
			buffer[3] = L':';
			buffer[4] = 0;
			start = buffer + 2;
		} else {
			buffer[length - 1] = L'\\';
			start = buffer + 1;
		}
	}
	lstrcpynW(text, start, capacity);
	return directory;
}

BOOL WINAPI DlgDirSelectExW(HWND dialog, LPWSTR text, int capacity, int list)
{
	return directory_selection(dialog, text, capacity, list, FALSE);
}

BOOL WINAPI DlgDirSelectComboBoxExW(HWND dialog, LPWSTR text, int capacity, int list)
{
	return directory_selection(dialog, text, capacity, list, TRUE);
}

static BOOL selection_ansi(HWND dialog, LPSTR text, int capacity, int list, BOOL combo)
{
	WCHAR wide[MAX_PATH + 4];
	BOOL directory = directory_selection(dialog, wide, MAX_PATH + 4, list, combo);
	if (capacity > 0)
		WideCharToMultiByte(CP_ACP, 0, wide, -1, text, capacity, NULL, NULL);
	return directory;
}

BOOL WINAPI DlgDirSelectExA(HWND dialog, LPSTR text, int capacity, int list)
{
	return selection_ansi(dialog, text, capacity, list, FALSE);
}

BOOL WINAPI DlgDirSelectComboBoxExA(HWND dialog, LPSTR text, int capacity, int list)
{
	return selection_ansi(dialog, text, capacity, list, TRUE);
}

/* ---- focus and the default button ---- */

static HWND default_button(HWND dialog)
{
	dialog_state *state = state_of(dialog);
	if (!state || !state->default_id)
		return NULL;
	return GetDlgItem(dialog, (int)state->default_id);
}

/* The default look follows the focus: a focused push button shows as default, and otherwise the dialog's default does. */
static void show_default_for(HWND dialog, HWND old_focus, HWND new_focus)
{
	LRESULT new_code = new_focus ? SendMessageW(new_focus, WM_GETDLGCODE, 0, 0) : 0;
	LRESULT old_code = old_focus ? SendMessageW(old_focus, WM_GETDLGCODE, 0, 0) : 0;
	HWND standing = default_button(dialog);
	if ((old_code & DLGC_DEFPUSHBUTTON) && old_focus != new_focus)
		SendMessageW(old_focus, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
	if (new_code & (DLGC_UNDEFPUSHBUTTON | DLGC_DEFPUSHBUTTON)) {
		if (standing && standing != new_focus)
			SendMessageW(standing, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
		SendMessageW(new_focus, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
	} else if (standing) {
		SendMessageW(standing, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
	}
}

static void focus_control(HWND dialog, HWND control)
{
	HWND old_focus = GetFocus();
	if (!control)
		return;
	SetFocus(control);
	show_default_for(dialog, old_focus, control);
	if (SendMessageW(control, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL)
		SendMessageW(control, EM_SETSEL, 0, -1);
}

static void set_default_id(HWND dialog, dialog_state *state, UINT id)
{
	HWND old = default_button(dialog);
	if (old && (SendMessageW(old, WM_GETDLGCODE, 0, 0) & DLGC_DEFPUSHBUTTON))
		SendMessageW(old, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
	state->default_id = id;
	HWND replacement = default_button(dialog);
	if (replacement && (SendMessageW(replacement, WM_GETDLGCODE, 0, 0) & DLGC_UNDEFPUSHBUTTON))
		SendMessageW(replacement, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
}

/* ---- the default dialog procedure ---- */

static LRESULT default_behavior(HWND window, dialog_state *state, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message) {
	case WM_ERASEBKGND: {
		HBRUSH brush = (HBRUSH)SendMessageW(window, WM_CTLCOLORDLG, wparam, (LPARAM)window);
		if (!brush)
			brush = (HBRUSH)DefWindowProcW(window, WM_CTLCOLORDLG, wparam, (LPARAM)window);
		RECT client;
		GetClientRect(window, &client);
		FillRect((HDC)wparam, &client, brush ? brush : GetSysColorBrush(COLOR_3DFACE));
		return 1;
	}
	case WM_CTLCOLORMSGBOX:
	case WM_CTLCOLORDLG:
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
		SetTextColor((HDC)wparam, GetSysColor(COLOR_WINDOWTEXT));
		SetBkColor((HDC)wparam, GetSysColor(COLOR_3DFACE));
		return (LRESULT)GetSysColorBrush(COLOR_3DFACE);
	case WM_SHOWWINDOW:
		if (!wparam && state)
			state->saved_focus = GetFocus();
		break;
	case WM_ACTIVATE:
		if (!state)
			break;
		if (LOWORD(wparam) == WA_INACTIVE) {
			HWND focus = GetFocus();
			if (focus && IsChild(window, focus))
				state->saved_focus = focus;
		} else if (state->saved_focus && IsWindow(state->saved_focus)) {
			SetFocus(state->saved_focus);
		} else {
			/* A control the procedure already focused during WM_INITDIALOG keeps it. */
			HWND focus = GetFocus();
			if (!focus || !IsChild(window, focus)) {
				HWND first = GetNextDlgTabItem(window, NULL, FALSE);
				if (first)
					SetFocus(first);
			}
		}
		return 0;
	case WM_SETFOCUS:
		if (state && state->saved_focus && IsWindow(state->saved_focus))
			SetFocus(state->saved_focus);
		else {
			HWND first = GetNextDlgTabItem(window, NULL, FALSE);
			if (first)
				SetFocus(first);
		}
		return 0;
	case WM_CLOSE: {
		HWND cancel = GetDlgItem(window, IDCANCEL);
		if (!cancel || IsWindowEnabled(cancel))
			PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), (LPARAM)cancel);
		return 0;
	}
	case WM_NEXTDLGCTL: {
		HWND next;
		if (lparam & 0xffff)
			next = (HWND)wparam;
		else
			next = GetNextDlgTabItem(window, GetFocus(), (BOOL)wparam);
		focus_control(window, next);
		return 0;
	}
	case WM_SETFONT:
		if (state) {
			if (state->font_owned && state->font != (HFONT)wparam)
				DeleteObject(state->font);
			if (state->font != (HFONT)wparam)
				state->font_owned = FALSE;
			state->font = (HFONT)wparam;
			if (lparam)
				InvalidateRect(window, NULL, TRUE);
		}
		return 0;
	case WM_GETFONT:
		return state ? (LRESULT)state->font : 0;
	case DM_GETDEFID:
		if (state && state->default_id)
			return MAKELONG(state->default_id, DC_HASDEFID);
		return 0;
	case DM_SETDEFID:
		if (state)
			set_default_id(window, state, (UINT)wparam);
		return TRUE;
	case DM_REPOSITION: {
		RECT frame, work;
		GetWindowRect(window, &frame);
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		int x = frame.left, y = frame.top;
		if (frame.right > work.right)
			x -= frame.right - work.right;
		if (frame.bottom > work.bottom)
			y -= frame.bottom - work.bottom;
		if (x < work.left)
			x = work.left;
		if (y < work.top)
			y = work.top;
		SetWindowPos(window, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	}
	case WM_NCDESTROY:
		if (state) {
			if (state->font_owned)
				DeleteObject(state->font);
			if (state->menu && !GetMenu(window))
				DestroyMenu(state->menu);
			RemovePropW(window, state_property);
			/* The modal loop reads the result after this, through a window it no longer finds: nothing else holds the record. */
			HeapFree(GetProcessHeap(), 0, state);
		}
		break;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

static LRESULT dialog_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	dialog_state *state = state_of(window);
	DLGPROC procedure = (DLGPROC)GetWindowLongPtrW(window, DWLP_DLGPROC);
	if (procedure) {
		SetWindowLongPtrW(window, DWLP_MSGRESULT, 0);
		INT_PTR answer = call_procedure(window, procedure, message, wparam, lparam, state && state->ansi);
		if (answer) {
			if (returns_directly(message))
				return answer;
			return IsWindow(window) ? GetWindowLongPtrW(window, DWLP_MSGRESULT) : 0;
		}
		if (!IsWindow(window))
			return 0;
		state = state_of(window);
	}
	return default_behavior(window, state, message, wparam, lparam);
}

LRESULT WINAPI DefDlgProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return dialog_procedure(window, message, wparam, lparam);
}

LRESULT WINAPI DefDlgProcA(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return dialog_procedure(window, message, wparam, lparam);
}

/* ---- keyboard navigation ---- */

static BOOL press_button(HWND dialog, int id)
{
	HWND button = GetDlgItem(dialog, id);
	if (button && !IsWindowEnabled(button)) {
		MessageBeep(0);
		return TRUE;
	}
	SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)button);
	return TRUE;
}

/* Finds the control a mnemonic belongs to: the next one after a static, the control itself otherwise. */
static BOOL handle_mnemonic(HWND dialog, HWND from, WCHAR character)
{
	HWND list[1024];
	int count = 0;
	for (HWND child = GetWindow(dialog, GW_CHILD); child && count < 1024; child = GetWindow(child, GW_HWNDNEXT))
		list[count++] = child;
	character = (WCHAR)(ULONG_PTR)CharUpperW((LPWSTR)(ULONG_PTR)character);
	int start = 0;
	for (int index = 0; index < count; index++)
		if (list[index] == from)
			start = index + 1;
	for (int offset = 0; offset < count; offset++) {
		HWND control = list[(start + offset) % count];
		DWORD style = (DWORD)GetWindowLongPtrW(control, GWL_STYLE);
		LRESULT code = SendMessageW(control, WM_GETDLGCODE, 0, 0);
		if (!(style & WS_VISIBLE) || (style & WS_DISABLED))
			continue;
		if ((code & DLGC_STATIC) && (style & SS_NOPREFIX))
			continue;
		WCHAR text[256];
		if (!GetWindowTextW(control, text, 256))
			continue;
		for (WCHAR *cursor = text; *cursor; cursor++) {
			if (cursor[0] != L'&')
				continue;
			if (cursor[1] == L'&') {
				cursor++;
				continue;
			}
			if ((WCHAR)(ULONG_PTR)CharUpperW((LPWSTR)(ULONG_PTR)cursor[1]) != character)
				break;
			if (code & DLGC_STATIC) {
				HWND next = GetNextDlgTabItem(dialog, control, FALSE);
				if (next)
					focus_control(dialog, next);
				return TRUE;
			}
			focus_control(dialog, control);
			if (code & (DLGC_BUTTON | DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON | DLGC_RADIOBUTTON))
				SendMessageW(control, BM_CLICK, 0, 0);
			return TRUE;
		}
	}
	return FALSE;
}

BOOL WINAPI IsDialogMessageW(HWND dialog, LPMSG message)
{
	if (!message || (dialog != message->hwnd && !IsChild(dialog, message->hwnd)))
		return FALSE;
	/* A hook may take the message first, as the documentation of MessageProc says. */
	if (CallMsgFilterW(message, MSGF_DIALOGBOX))
		return TRUE;
	HWND target = message->hwnd;
	LRESULT code = SendMessageW(target, WM_GETDLGCODE, message->wParam, (LPARAM)message);
	BOOL wants_message = code & (DLGC_WANTMESSAGE | DLGC_WANTALLKEYS);
	switch (message->message) {
	case WM_KEYDOWN:
		switch (message->wParam) {
		case VK_TAB:
			if (code & (DLGC_WANTTAB | DLGC_WANTMESSAGE))
				break;
			SendMessageW(dialog, WM_NEXTDLGCTL, GetKeyState(VK_SHIFT) < 0, 0);
			return TRUE;
		case VK_LEFT:
		case VK_RIGHT:
		case VK_UP:
		case VK_DOWN:
			if (code & (DLGC_WANTARROWS | DLGC_WANTMESSAGE))
				break;
			{
				HWND next = GetNextDlgGroupItem(dialog, target, message->wParam == VK_LEFT || message->wParam == VK_UP);
				if (next) {
					focus_control(dialog, next);
					if (SendMessageW(next, WM_GETDLGCODE, 0, 0) & DLGC_RADIOBUTTON)
						SendMessageW(next, BM_CLICK, 0, 0);
				}
			}
			return TRUE;
		case VK_ESCAPE:
		case VK_CANCEL:
			if (wants_message)
				break;
			return press_button(dialog, IDCANCEL);
		case VK_EXECUTE:
		case VK_RETURN:
			if (wants_message)
				break;
			if (code & DLGC_DEFPUSHBUTTON) {
				SendMessageW(target, BM_CLICK, 0, 0);
				return TRUE;
			}
			{
				LRESULT answer = SendMessageW(dialog, DM_GETDEFID, 0, 0);
				int id = HIWORD(answer) == DC_HASDEFID ? LOWORD(answer) : IDOK;
				return press_button(dialog, id);
			}
		}
		break;
	case WM_CHAR:
		if (code & (DLGC_WANTCHARS | DLGC_WANTMESSAGE))
			break;
		if (message->wParam == L'\t' && !(code & DLGC_WANTTAB))
			return TRUE;
		if (handle_mnemonic(dialog, target, (WCHAR)message->wParam))
			return TRUE;
		break;
	case WM_SYSCHAR:
		if (handle_mnemonic(dialog, target, (WCHAR)message->wParam))
			return TRUE;
		break;
	}
	TranslateMessage(message);
	DispatchMessageW(message);
	return TRUE;
}

BOOL WINAPI IsDialogMessageA(HWND dialog, LPMSG message)
{
	return IsDialogMessageW(dialog, message);
}

