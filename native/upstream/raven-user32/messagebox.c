/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include "user32_private.h"

/* MessageBox, from Microsoft's documentation of its styles and return values: a modal dialog with an empty template whose controls are placed on
 * WM_INITDIALOG, once the text is measured. */

#define MESSAGE_BOX_TIMEOUT_RESULT 32000
#define BUTTON_ROOM 4
#define ICON_ID 20
#define TEXT_ID 0xffff
#define TIMER_ID 1

typedef struct {
	const MSGBOXPARAMSW *params;
	DWORD timeout;
	int buttons[BUTTON_ROOM];
	int button_count;
} box_state;

static const WCHAR *button_text(int id)
{
	switch (id) {
	case IDOK: return L"OK";
	case IDCANCEL: return L"Cancel";
	case IDABORT: return L"&Abort";
	case IDRETRY: return L"&Retry";
	case IDIGNORE: return L"&Ignore";
	case IDYES: return L"&Yes";
	case IDNO: return L"&No";
	case IDTRYAGAIN: return L"&Try Again";
	case IDCONTINUE: return L"&Continue";
	case IDHELP: return L"Help";
	}
	return L"";
}

static BOOL set_buttons(box_state *state, UINT type)
{
	static const int sets[][3] = {
		{IDOK, 0, 0}, {IDOK, IDCANCEL, 0}, {IDABORT, IDRETRY, IDIGNORE}, {IDYES, IDNO, IDCANCEL},
		{IDYES, IDNO, 0}, {IDRETRY, IDCANCEL, 0}, {IDCANCEL, IDTRYAGAIN, IDCONTINUE},
	};
	UINT kind = type & MB_TYPEMASK;
	if (kind >= sizeof(sets) / sizeof(sets[0]))
		return FALSE;
	state->button_count = 0;
	for (int index = 0; index < 3; index++)
		if (sets[kind][index])
			state->buttons[state->button_count++] = sets[kind][index];
	if (type & MB_HELP)
		state->buttons[state->button_count++] = IDHELP;
	return TRUE;
}

static HICON box_icon(const MSGBOXPARAMSW *params)
{
	switch (params->dwStyle & MB_ICONMASK) {
	case MB_ICONHAND: return LoadIconW(NULL, (LPCWSTR)IDI_HAND);
	case MB_ICONQUESTION: return LoadIconW(NULL, (LPCWSTR)IDI_QUESTION);
	case MB_ICONEXCLAMATION: return LoadIconW(NULL, (LPCWSTR)IDI_EXCLAMATION);
	case MB_ICONASTERISK: return LoadIconW(NULL, (LPCWSTR)IDI_ASTERISK);
	case MB_USERICON: return LoadIconW(params->hInstance, params->lpszIcon);
	}
	return NULL;
}

static void lay_out(HWND dialog, box_state *state)
{
	const MSGBOXPARAMSW *params = state->params;
	NONCLIENTMETRICSW metrics = {sizeof(metrics)};
	HFONT font = NULL;
	if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
		font = CreateFontIndirectW(&metrics.lfMessageFont);
	if (font)
		SendMessageW(dialog, WM_SETFONT, (WPARAM)font, FALSE);
	HDC dc = GetDC(dialog);
	HGDIOBJ previous = font ? SelectObject(dc, font) : NULL;
	const int margin = 12, gap = 8, button_height = 26, screen_width = GetSystemMetrics(SM_CXSCREEN), screen_height = GetSystemMetrics(SM_CYSCREEN);
	const WCHAR *text = params->lpszText ? params->lpszText : L"";
	RECT text_rect = {0, 0, screen_width * 2 / 3, 0};
	UINT draw_flags = DT_WORDBREAK | DT_CALCRECT | DT_EXPANDTABS | DT_NOCLIP;
	if (params->dwStyle & MB_RTLREADING)
		draw_flags |= DT_RTLREADING;
	DrawTextW(dc, text, -1, &text_rect, draw_flags);

	int button_width = 75;
	for (int index = 0; index < state->button_count; index++) {
		RECT measure = {0, 0, 0, 0};
		DrawTextW(dc, button_text(state->buttons[index]), -1, &measure, DT_CALCRECT | DT_SINGLELINE);
		if (measure.right + 16 > button_width)
			button_width = measure.right + 16;
	}
	if (previous)
		SelectObject(dc, previous);
	ReleaseDC(dialog, dc);

	HICON icon = box_icon(params);
	int icon_size = 0;
	if (icon) {
		/* A static control takes the icon's own size, which need not be SM_CXICON. */
		HWND picture = CreateWindowExW(0, L"Static", NULL, WS_CHILD | WS_VISIBLE | SS_ICON, margin, margin, 0, 0, dialog, (HMENU)ICON_ID, NULL, NULL);
		RECT area;
		SendMessageW(picture, STM_SETICON, (WPARAM)icon, 0);
		GetClientRect(picture, &area);
		icon_size = area.right > 0 ? area.right : GetSystemMetrics(SM_CXICON);
	}
	int text_left = margin + (icon ? icon_size + margin : 0);
	int row_width = state->button_count * button_width + (state->button_count - 1) * gap;
	int content_width = text_left + text_rect.right + margin;
	int client_width = content_width > row_width + 2 * margin ? content_width : row_width + 2 * margin;
	int content_height = text_rect.bottom > icon_size ? text_rect.bottom : icon_size;
	int client_height = margin + content_height + margin + button_height + margin;

	DWORD static_style = WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_LEFT;
	if (params->dwStyle & MB_RIGHT)
		static_style = (static_style & ~(DWORD)SS_LEFT) | SS_RIGHT;
	HWND label = CreateWindowExW(params->dwStyle & MB_RTLREADING ? WS_EX_RTLREADING : 0, L"Static", text, static_style, text_left,
		margin + (content_height - text_rect.bottom) / 2, text_rect.right, text_rect.bottom, dialog, (HMENU)TEXT_ID, NULL, NULL);
	if (font)
		SendMessageW(label, WM_SETFONT, (WPARAM)font, FALSE);

	/* MB_DEFBUTTON1 to 4 in steps of 0x100 pick the default, the first when there are fewer. */
	int chosen = (int)((params->dwStyle & MB_DEFMASK) >> 8);
	if (chosen >= state->button_count)
		chosen = 0;
	int x = (client_width - row_width) / 2, y = margin + content_height + margin;
	HWND focused = NULL;
	for (int index = 0; index < state->button_count; index++) {
		DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (index == chosen ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON) | (index ? 0 : WS_GROUP);
		HWND button = CreateWindowExW(0, L"Button", button_text(state->buttons[index]), style, x, y, button_width, button_height, dialog,
			(HMENU)(ULONG_PTR)state->buttons[index], NULL, NULL);
		if (font)
			SendMessageW(button, WM_SETFONT, (WPARAM)font, FALSE);
		if (index == chosen)
			focused = button;
		x += button_width + gap;
	}
	SendMessageW(dialog, DM_SETDEFID, state->buttons[chosen], 0);
	if (focused)
		SetFocus(focused);

	RECT frame = {0, 0, client_width, client_height};
	DWORD style = (DWORD)GetWindowLongPtrW(dialog, GWL_STYLE), ex_style = (DWORD)GetWindowLongPtrW(dialog, GWL_EXSTYLE);
	AdjustWindowRectEx(&frame, style, FALSE, ex_style);
	int width = frame.right - frame.left, height = frame.bottom - frame.top;
	SetWindowPos(dialog, params->dwStyle & MB_TOPMOST ? HWND_TOPMOST : NULL, (screen_width - width) / 2, (screen_height - height) / 2, width, height,
		SWP_NOACTIVATE | (params->dwStyle & MB_TOPMOST ? 0 : SWP_NOZORDER));
	/* Without a Cancel button, nothing can answer the close button. */
	if (!GetDlgItem(dialog, IDCANCEL) && (params->dwStyle & MB_TYPEMASK) != MB_OK)
		EnableMenuItem(GetSystemMenu(dialog, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
	SetWindowTextW(dialog, params->lpszCaption ? params->lpszCaption : L"Error");
	if (params->dwStyle & MB_SETFOREGROUND)
		SetForegroundWindow(dialog);
	if (state->timeout)
		SetTimer(dialog, TIMER_ID, state->timeout, NULL);
}

static INT_PTR CALLBACK box_procedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
	box_state *state = (box_state *)GetWindowLongPtrW(dialog, DWLP_USER);
	switch (message) {
	case WM_INITDIALOG:
		SetWindowLongPtrW(dialog, DWLP_USER, lparam);
		lay_out(dialog, (box_state *)lparam);
		return FALSE;
	case WM_TIMER:
		if (wparam == TIMER_ID) {
			KillTimer(dialog, TIMER_ID);
			EndDialog(dialog, MESSAGE_BOX_TIMEOUT_RESULT);
			return TRUE;
		}
		break;
	case WM_COMMAND: {
		int id = LOWORD(wparam);
		if (HIWORD(wparam) != BN_CLICKED)
			break;
		if (id == IDHELP) {
			HELPINFO help = {sizeof(help), HELPINFO_WINDOW, 0, (HANDLE)dialog, state ? state->params->dwContextHelpId : 0};
			GetCursorPos(&help.MousePos);
			if (state && state->params->lpfnMsgBoxCallback)
				state->params->lpfnMsgBoxCallback(&help);
			else if (GetWindow(dialog, GW_OWNER))
				SendMessageW(GetWindow(dialog, GW_OWNER), WM_HELP, 0, (LPARAM)&help);
			return TRUE;
		}
		/* A box with only OK takes Escape and the close button as OK. */
		if (id == IDCANCEL && !GetDlgItem(dialog, IDCANCEL)) {
			if (!GetDlgItem(dialog, IDOK) || GetDlgItem(dialog, IDNO) || GetDlgItem(dialog, IDRETRY))
				return TRUE;
			id = IDOK;
		}
		EndDialog(dialog, id);
		return TRUE;
	}
	}
	return FALSE;
}

/* Windows of the thread that a task-modal box without an owner turns off while it runs. */
typedef struct {
	HWND windows[256];
	int count;
} disabled_list;

static BOOL CALLBACK disable_window(HWND window, LPARAM parameter)
{
	disabled_list *list = (disabled_list *)parameter;
	if (list->count < 256 && IsWindowEnabled(window)) {
		list->windows[list->count++] = window;
		EnableWindow(window, FALSE);
	}
	return TRUE;
}

static int show_box(const MSGBOXPARAMSW *params, DWORD timeout)
{
	box_state state = {params, timeout};
	if (!set_buttons(&state, params->dwStyle)) {
		SetLastError(ERROR_INVALID_MSGBOX_STYLE);
		return 0;
	}
	/* The template: a captioned popup with no controls, no menu and no class of its own. */
	struct {
		DLGTEMPLATE header;
		WORD menu, window_class, title;
	} blank = {{WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, 0, 0, 0, 0, 0, 0}, 0, 0, 0};
	if (params->dwStyle & MB_TOPMOST)
		blank.header.dwExtendedStyle |= WS_EX_TOPMOST;
	disabled_list disabled;
	disabled.count = 0;
	BOOL task_modal = (params->dwStyle & MB_TASKMODAL) && !params->hwndOwner;
	if (task_modal)
		EnumThreadWindows(GetCurrentThreadId(), disable_window, (LPARAM)&disabled);
	MessageBeep(params->dwStyle & MB_ICONMASK);
	INT_PTR result = DialogBoxIndirectParamW(params->hInstance, &blank.header, params->hwndOwner, box_procedure, (LPARAM)&state);
	for (int index = 0; index < disabled.count; index++)
		EnableWindow(disabled.windows[index], TRUE);
	return result < 0 ? 0 : (int)result;
}

int WINAPI MessageBoxIndirectW(const MSGBOXPARAMSW *params)
{
	return show_box(params, 0);
}

static WCHAR *to_wide(LPCSTR text)
{
	if (!text)
		return NULL;
	int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	WCHAR *wide = HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * length);
	if (wide)
		MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length);
	return wide;
}

int WINAPI MessageBoxIndirectA(const MSGBOXPARAMSA *params)
{
	MSGBOXPARAMSW wide = {sizeof(wide), params->hwndOwner, params->hInstance, NULL, NULL, params->dwStyle, NULL, params->dwContextHelpId, params->lpfnMsgBoxCallback, params->dwLanguageId};
	WCHAR *text = to_wide(params->lpszText), *caption = to_wide(params->lpszCaption), *icon = NULL;
	wide.lpszText = text;
	wide.lpszCaption = caption;
	if ((ULONG_PTR)params->lpszIcon >> 16)
		wide.lpszIcon = icon = to_wide(params->lpszIcon);
	else
		wide.lpszIcon = (LPCWSTR)params->lpszIcon;
	int result = show_box(&wide, 0);
	HeapFree(GetProcessHeap(), 0, text);
	HeapFree(GetProcessHeap(), 0, caption);
	HeapFree(GetProcessHeap(), 0, icon);
	return result;
}

int WINAPI MessageBoxTimeoutW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type, WORD language, DWORD timeout)
{
	MSGBOXPARAMSW params = {sizeof(params), owner, NULL, text, caption, type, NULL, 0, NULL, language};
	return show_box(&params, timeout == INFINITE ? 0 : timeout);
}

int WINAPI MessageBoxTimeoutA(HWND owner, LPCSTR text, LPCSTR caption, UINT type, WORD language, DWORD timeout)
{
	WCHAR *wide_text = to_wide(text), *wide_caption = to_wide(caption);
	int result = MessageBoxTimeoutW(owner, wide_text, wide_caption, type, language, timeout);
	HeapFree(GetProcessHeap(), 0, wide_text);
	HeapFree(GetProcessHeap(), 0, wide_caption);
	return result;
}

int WINAPI MessageBoxExW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type, WORD language)
{
	return MessageBoxTimeoutW(owner, text, caption, type, language, INFINITE);
}

int WINAPI MessageBoxExA(HWND owner, LPCSTR text, LPCSTR caption, UINT type, WORD language)
{
	return MessageBoxTimeoutA(owner, text, caption, type, language, INFINITE);
}

int WINAPI MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type)
{
	return MessageBoxExW(owner, text, caption, type, 0);
}

int WINAPI MessageBoxA(HWND owner, LPCSTR text, LPCSTR caption, UINT type)
{
	return MessageBoxExA(owner, text, caption, type, 0);
}

/* A sound would come from the host's audio only; the call succeeds as it does on a machine without one. */
BOOL WINAPI MessageBeep(UINT type)
{
	return TRUE;
}
