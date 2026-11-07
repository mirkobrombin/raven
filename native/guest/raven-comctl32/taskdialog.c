/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>

/* A task dialog is an empty modal dialog whose children are built from the configuration, once when it opens and again on
 * every navigation. The children are stacked by lay_out, which also sizes the window, so every text change goes through it. */

#define MARGIN 11
#define GAP 7
#define BAR_PADDING 10
#define BUTTON_HEIGHT 23
#define BUTTON_MIN_WIDTH 75
#define LINK_HEIGHT 41
#define RADIO_HEIGHT 17
#define PROGRESS_HEIGHT 15
#define ICON_SIZE 32
#define DEFAULT_WIDTH 360
#define TIMER_ID 1
#define TIMER_PERIOD 200
#define EXPANDER_ID 0x7F01

#ifndef BCM_SETNOTE
#define BCM_SETNOTE 0x1609
#endif
#ifndef BS_COMMANDLINK
#define BS_COMMANDLINK 0x0000000EL
#define BS_DEFCOMMANDLINK 0x0000000FL
#endif
#ifndef BCM_SETSHIELD
#define BCM_SETSHIELD 0x160C
#endif

typedef struct {
	int id;
	HWND window;
} button_slot;

typedef struct {
	HWND dialog;
	TASKDIALOGCONFIG config;
	int *result_button, *result_radio;
	BOOL *result_verification;
	button_slot *buttons, *radios;
	int button_count, radio_count, default_button, selected_radio;
	BOOL verification, expanded, has_cancel, in_navigation, ended, ready;
	HWND icon, instruction, content, expanded_text, progress, expander, verify, footer_icon, footer;
	HFONT plain, bold;
	DWORD started;
	int bar_top, footer_top;
} dialog_state;

static const struct {
	TASKDIALOG_COMMON_BUTTON_FLAGS flag;
	int id;
	const WCHAR *text;
} common_buttons[] = {
	/* The order of creation decides which one is the default. */
	{ TDCBF_OK_BUTTON, IDOK, L"OK" },
	{ TDCBF_YES_BUTTON, IDYES, L"&Yes" },
	{ TDCBF_NO_BUTTON, IDNO, L"&No" },
	{ TDCBF_RETRY_BUTTON, IDRETRY, L"&Retry" },
	{ TDCBF_CANCEL_BUTTON, IDCANCEL, L"Cancel" },
	{ TDCBF_CLOSE_BUTTON, IDCLOSE, L"&Close" },
};

static HRESULT notify(const dialog_state *state, UINT code, WPARAM wparam, LPARAM lparam)
{
	if (!state->config.pfCallback)
		return S_OK;
	return state->config.pfCallback(state->dialog, code, wparam, lparam, state->config.lpCallbackData);
}

/* A text is either the characters themselves or the number of a string in the caller's module. */
static const WCHAR *text_of(const dialog_state *state, const WCHAR *given, WCHAR *buffer, int size)
{
	if (!given)
		return NULL;
	if (!IS_INTRESOURCE(given))
		return given;
	if (!LoadStringW(state->config.hInstance, (UINT)(ULONG_PTR)given, buffer, size))
		return NULL;
	return buffer;
}

static HICON icon_of(const dialog_state *state, const WCHAR *given, BOOL large)
{
	const int size = large ? ICON_SIZE : 16;
	const WCHAR *system = NULL;
	if (!given)
		return NULL;
	if (given == TD_WARNING_ICON)
		system = (const WCHAR *)IDI_WARNING;
	else if (given == TD_ERROR_ICON)
		system = (const WCHAR *)IDI_ERROR;
	else if (given == TD_INFORMATION_ICON)
		system = (const WCHAR *)IDI_INFORMATION;
	else if (given == TD_SHIELD_ICON)
		system = (const WCHAR *)IDI_SHIELD;
	if (system)
		return LoadIconW(NULL, system);
	return LoadImageW(state->config.hInstance, given, IMAGE_ICON, size, size, LR_SHARED);
}

static HWND child(const dialog_state *state, const WCHAR *type, const WCHAR *text, DWORD style, int id)
{
	HWND window = CreateWindowExW(0, type, text ? text : L"", WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, state->dialog, (HMENU)(LONG_PTR)id, (HINSTANCE)GetWindowLongPtrW(state->dialog, GWLP_HINSTANCE), NULL);
	if (window)
		SendMessageW(window, WM_SETFONT, (WPARAM)state->plain, FALSE);
	return window;
}

static int text_height(const dialog_state *state, HFONT font, HWND window, int width)
{
	int length = GetWindowTextLengthW(window);
	WCHAR *text;
	RECT area;
	HDC dc;
	HGDIOBJ old;
	if (!length)
		return 0;
	text = HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
	if (!text)
		return 0;
	GetWindowTextW(window, text, length + 1);
	SetRect(&area, 0, 0, width, 0);
	dc = GetDC(state->dialog);
	old = SelectObject(dc, font);
	DrawTextW(dc, text, length, &area, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
	SelectObject(dc, old);
	ReleaseDC(state->dialog, dc);
	HeapFree(GetProcessHeap(), 0, text);
	return area.bottom;
}

static int place_text(const dialog_state *state, HWND window, HFONT font, int x, int y, int width)
{
	int height;
	/* The dialog itself is still hidden the first time through, so only the child's own flag counts. */
	if (!window || !(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE))
		return y;
	if (GetWindowTextLengthW(window) == 0) {
		ShowWindow(window, SW_HIDE);
		return y;
	}
	height = text_height(state, font, window, width);
	MoveWindow(window, x, y, width, height, FALSE);
	return y + height + GAP;
}

static int button_width(const dialog_state *state, HWND window)
{
	WCHAR text[128];
	SIZE size = { 0, 0 };
	HDC dc = GetDC(state->dialog);
	HGDIOBJ old = SelectObject(dc, state->plain);
	int length = GetWindowTextW(window, text, 128);
	GetTextExtentPoint32W(dc, text, length, &size);
	SelectObject(dc, old);
	ReleaseDC(state->dialog, dc);
	return size.cx + 16 > BUTTON_MIN_WIDTH ? size.cx + 16 : BUTTON_MIN_WIDTH;
}

static BOOL is_link_slot(const dialog_state *state, int index)
{
	/* The custom buttons come first in the table, and only they can be links. */
	return (state->config.dwFlags & (TDF_USE_COMMAND_LINKS | TDF_USE_COMMAND_LINKS_NO_ICON)) && index < (int)state->config.cButtons;
}

/* Places everything top to bottom and resizes the window around it. */
static void lay_out(dialog_state *state)
{
	const TASKDIALOGCONFIG *config = &state->config;
	int client_width, x = MARGIN, y = MARGIN, text_width, index, row_width = 0, right;
	RECT frame;
	DWORD style = (DWORD)GetWindowLongPtrW(state->dialog, GWL_STYLE), extra = (DWORD)GetWindowLongPtrW(state->dialog, GWL_EXSTYLE);

	if (config->cxWidth)
		client_width = MulDiv((int)config->cxWidth, LOWORD(GetDialogBaseUnits()), 4);
	else
		client_width = DEFAULT_WIDTH;
	for (index = 0; index < state->button_count; index++) {
		if (!is_link_slot(state, index))
			row_width += button_width(state, state->buttons[index].window) + GAP;
	}
	if (row_width + 2 * MARGIN > client_width)
		client_width = row_width + 2 * MARGIN;

	if (state->icon) {
		MoveWindow(state->icon, MARGIN, MARGIN, ICON_SIZE, ICON_SIZE, FALSE);
		x = MARGIN + ICON_SIZE + MARGIN;
	}
	text_width = client_width - x - MARGIN;
	y = place_text(state, state->instruction, state->bold, x, y, text_width);
	y = place_text(state, state->content, state->plain, x, y, text_width);
	if (state->expanded_text && !(config->dwFlags & TDF_EXPAND_FOOTER_AREA)) {
		ShowWindow(state->expanded_text, state->expanded ? SW_SHOWNA : SW_HIDE);
		y = place_text(state, state->expanded_text, state->plain, x, y, text_width);
	}
	if (state->progress) {
		MoveWindow(state->progress, x, y, text_width, PROGRESS_HEIGHT, FALSE);
		y += PROGRESS_HEIGHT + GAP;
	}
	for (index = 0; index < state->radio_count; index++) {
		MoveWindow(state->radios[index].window, x, y, text_width, RADIO_HEIGHT, FALSE);
		y += RADIO_HEIGHT;
	}
	if (state->radio_count)
		y += GAP;
	for (index = 0; index < state->button_count; index++) {
		if (is_link_slot(state, index)) {
			MoveWindow(state->buttons[index].window, x, y, text_width, LINK_HEIGHT, FALSE);
			y += LINK_HEIGHT + 2;
		}
	}
	if (state->icon && y < MARGIN + ICON_SIZE + GAP)
		y = MARGIN + ICON_SIZE + GAP;

	/* The bar along the bottom holds the rest of the buttons, and to their left the expander and the check box. */
	state->bar_top = y + GAP;
	y = state->bar_top + BAR_PADDING;
	{
		int left = MARGIN, top = y;
		if (state->expander) {
			int width = button_width(state, state->expander);
			MoveWindow(state->expander, left, top, width, BUTTON_HEIGHT, FALSE);
			left += width + GAP;
		}
		right = client_width - MARGIN;
		for (index = state->button_count - 1; index >= 0; index--) {
			if (!is_link_slot(state, index)) {
				int width = button_width(state, state->buttons[index].window);
				right -= width;
				MoveWindow(state->buttons[index].window, right, top, width, BUTTON_HEIGHT, FALSE);
				right -= GAP;
			}
		}
		if (state->verify) {
			/* A check box that does not fit beside the buttons gets its own line. */
			int width = button_width(state, state->verify) + 8;
			if (left + width > right) {
				top += BUTTON_HEIGHT + GAP;
				left = MARGIN;
				width = client_width - 2 * MARGIN;
			}
			MoveWindow(state->verify, left, top + 3, width, RADIO_HEIGHT, FALSE);
		}
		y = top + BUTTON_HEIGHT + BAR_PADDING;
	}

	state->footer_top = 0;
	if (state->footer || (state->expanded_text && (config->dwFlags & TDF_EXPAND_FOOTER_AREA) && state->expanded)) {
		int footer_x = MARGIN;
		state->footer_top = y;
		y += GAP;
		if (state->footer_icon) {
			MoveWindow(state->footer_icon, MARGIN, y, 16, 16, FALSE);
			footer_x += 16 + GAP;
		}
		y = place_text(state, state->footer, state->plain, footer_x, y, client_width - footer_x - MARGIN);
	}
	if (state->expanded_text && (config->dwFlags & TDF_EXPAND_FOOTER_AREA)) {
		ShowWindow(state->expanded_text, state->expanded ? SW_SHOWNA : SW_HIDE);
		y = place_text(state, state->expanded_text, state->plain, MARGIN, y, client_width - 2 * MARGIN);
	}
	y += state->footer_top ? GAP : 0;

	SetRect(&frame, 0, 0, client_width, y);
	AdjustWindowRectEx(&frame, style, FALSE, extra);
	SetWindowPos(state->dialog, NULL, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	InvalidateRect(state->dialog, NULL, TRUE);
}

static void center_window(const dialog_state *state)
{
	RECT own, around;
	HWND parent = state->config.hwndParent;
	if (parent && (state->config.dwFlags & TDF_POSITION_RELATIVE_TO_WINDOW))
		GetWindowRect(parent, &around);
	else
		SetRect(&around, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
	GetWindowRect(state->dialog, &own);
	SetWindowPos(state->dialog, NULL, (around.left + around.right - (own.right - own.left)) / 2, (around.top + around.bottom - (own.bottom - own.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Building */

static void add_button(dialog_state *state, int id, const WCHAR *text, BOOL as_link)
{
	button_slot *slot = &state->buttons[state->button_count++];
	const WCHAR *line_end = as_link ? wcschr(text, L'\n') : NULL;
	WCHAR *first = NULL;
	if (line_end) {
		SIZE_T length = line_end - text;
		first = HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
		if (first) {
			CopyMemory(first, text, length * sizeof(WCHAR));
			first[length] = 0;
		}
	}
	slot->id = id;
	slot->window = child(state, L"BUTTON", first ? first : text, WS_TABSTOP | (as_link ? BS_COMMANDLINK | ((state->config.dwFlags & TDF_USE_COMMAND_LINKS_NO_ICON) ? BS_BITMAP : 0) : BS_PUSHBUTTON), id);
	if (line_end)
		SendMessageW(slot->window, BCM_SETNOTE, 0, (LPARAM)(line_end + 1));
	if (first)
		HeapFree(GetProcessHeap(), 0, first);
}

static HWND find_button(const dialog_state *state, int id)
{
	int index;
	for (index = 0; index < state->button_count; index++) {
		if (state->buttons[index].id == id)
			return state->buttons[index].window;
	}
	return NULL;
}

static HWND find_radio(const dialog_state *state, int id)
{
	int index;
	for (index = 0; index < state->radio_count; index++) {
		if (state->radios[index].id == id)
			return state->radios[index].window;
	}
	return NULL;
}

static void free_tables(dialog_state *state)
{
	if (state->buttons)
		HeapFree(GetProcessHeap(), 0, state->buttons);
	if (state->radios)
		HeapFree(GetProcessHeap(), 0, state->radios);
	state->buttons = state->radios = NULL;
	state->button_count = state->radio_count = 0;
}

static void clear_children(dialog_state *state)
{
	HWND window;
	while ((window = GetWindow(state->dialog, GW_CHILD)))
		DestroyWindow(window);
	free_tables(state);
	state->icon = state->instruction = state->content = state->expanded_text = NULL;
	state->progress = state->expander = state->verify = state->footer_icon = state->footer = NULL;
}

static HWND add_icon(const dialog_state *state, HICON icon)
{
	HWND window;
	if (!icon)
		return NULL;
	window = child(state, L"STATIC", NULL, SS_ICON | SS_REALSIZECONTROL, 0);
	if (window)
		SendMessageW(window, STM_SETICON, (WPARAM)icon, 0);
	return window;
}

static void set_expander_text(const dialog_state *state)
{
	WCHAR buffer[128];
	const WCHAR *given = state->expanded ? state->config.pszCollapsedControlText : state->config.pszExpandedControlText;
	const WCHAR *text = text_of(state, given, buffer, 128);
	SetWindowTextW(state->expander, text ? text : state->expanded ? L"Hide details" : L"See details");
}

static void make_fonts(dialog_state *state)
{
	NONCLIENTMETRICSW metrics;
	LOGFONTW description;
	metrics.cbSize = sizeof(metrics);
	if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
		description = metrics.lfMessageFont;
	else
		GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(description), &description);
	state->plain = CreateFontIndirectW(&description);
	description.lfWeight = FW_BOLD;
	description.lfHeight = description.lfHeight * 5 / 4;
	state->bold = CreateFontIndirectW(&description);
}

static void build(dialog_state *state)
{
	const TASKDIALOGCONFIG *config = &state->config;
	WCHAR buffer[1024];
	const WCHAR *text;
	HICON icon;
	UINT index;
	int common_count = 0, chosen;

	if (config->dwFlags & TDF_CAN_BE_MINIMIZED)
		SetWindowLongPtrW(state->dialog, GWL_STYLE, GetWindowLongPtrW(state->dialog, GWL_STYLE) | WS_MINIMIZEBOX);
	text = text_of(state, config->pszWindowTitle, buffer, 1024);
	if (text) {
		SetWindowTextW(state->dialog, text);
	}
	else {
		WCHAR path[MAX_PATH];
		const WCHAR *name = path, *walk;
		GetModuleFileNameW(NULL, path, MAX_PATH);
		for (walk = path; *walk; walk++) {
			if (*walk == '\\' || *walk == '/')
				name = walk + 1;
		}
		SetWindowTextW(state->dialog, name);
	}

	icon = (config->dwFlags & TDF_USE_HICON_MAIN) ? config->hMainIcon : icon_of(state, config->pszMainIcon, TRUE);
	state->icon = add_icon(state, icon);
	state->instruction = child(state, L"STATIC", text_of(state, config->pszMainInstruction, buffer, 1024), SS_LEFT | SS_NOPREFIX, 0);
	SendMessageW(state->instruction, WM_SETFONT, (WPARAM)state->bold, FALSE);
	if ((config->dwFlags & TDF_ENABLE_HYPERLINKS) && config->pszContent)
		state->content = child(state, WC_LINK, text_of(state, config->pszContent, buffer, 1024), 0, 0);
	else
		state->content = child(state, L"STATIC", text_of(state, config->pszContent, buffer, 1024), SS_LEFT | SS_NOPREFIX, 0);

	if (config->pszExpandedInformation) {
		state->expanded = (config->dwFlags & TDF_EXPANDED_BY_DEFAULT) != 0;
		state->expanded_text = child(state, L"STATIC", text_of(state, config->pszExpandedInformation, buffer, 1024), SS_LEFT | SS_NOPREFIX, 0);
		state->expander = child(state, L"BUTTON", NULL, WS_TABSTOP | BS_PUSHBUTTON, EXPANDER_ID);
		set_expander_text(state);
	}

	if (config->dwFlags & (TDF_SHOW_PROGRESS_BAR | TDF_SHOW_MARQUEE_PROGRESS_BAR)) {
		BOOL marquee = (config->dwFlags & TDF_SHOW_MARQUEE_PROGRESS_BAR) != 0;
		state->progress = child(state, PROGRESS_CLASSW, NULL, marquee ? PBS_MARQUEE : 0, 0);
	}

	state->radios = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (config->cRadioButtons + 1) * sizeof(button_slot));
	for (index = 0; state->radios && index < config->cRadioButtons; index++) {
		button_slot *slot = &state->radios[state->radio_count++];
		slot->id = config->pRadioButtons[index].nButtonID;
		text = text_of(state, config->pRadioButtons[index].pszButtonText, buffer, 1024);
		slot->window = child(state, L"BUTTON", text, WS_TABSTOP | BS_RADIOBUTTON | (index ? 0 : WS_GROUP), slot->id);
	}

	for (index = 0; index < sizeof(common_buttons) / sizeof(common_buttons[0]); index++) {
		if (config->dwCommonButtons & common_buttons[index].flag)
			common_count++;
	}
	state->buttons = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (config->cButtons + common_count + 1) * sizeof(button_slot));
	if (state->buttons) {
		BOOL links = (config->dwFlags & (TDF_USE_COMMAND_LINKS | TDF_USE_COMMAND_LINKS_NO_ICON)) != 0;
		for (index = 0; index < config->cButtons; index++) {
			text = text_of(state, config->pButtons[index].pszButtonText, buffer, 1024);
			add_button(state, config->pButtons[index].nButtonID, text ? text : L"", links);
		}
		for (index = 0; index < sizeof(common_buttons) / sizeof(common_buttons[0]); index++) {
			if (config->dwCommonButtons & common_buttons[index].flag)
				add_button(state, common_buttons[index].id, common_buttons[index].text, FALSE);
		}
		/* A dialog always has a way out. */
		if (!state->button_count)
			add_button(state, IDOK, L"OK", FALSE);
	}

	state->has_cancel = (config->dwFlags & TDF_ALLOW_DIALOG_CANCELLATION) || find_button(state, IDCANCEL);
	EnableMenuItem(GetSystemMenu(state->dialog, FALSE), SC_CLOSE, MF_BYCOMMAND | (state->has_cancel ? MF_ENABLED : MF_GRAYED));

	if (config->pszVerificationText) {
		state->verify = child(state, L"BUTTON", text_of(state, config->pszVerificationText, buffer, 1024), WS_TABSTOP | BS_CHECKBOX, 0);
		SendMessageW(state->verify, BM_SETCHECK, state->verification ? BST_CHECKED : BST_UNCHECKED, 0);
	}

	icon = (config->dwFlags & TDF_USE_HICON_FOOTER) ? config->hFooterIcon : icon_of(state, config->pszFooterIcon, FALSE);
	if (config->pszFooter) {
		state->footer_icon = add_icon(state, icon);
		if ((config->dwFlags & TDF_ENABLE_HYPERLINKS))
			state->footer = child(state, WC_LINK, text_of(state, config->pszFooter, buffer, 1024), 0, 0);
		else
			state->footer = child(state, L"STATIC", text_of(state, config->pszFooter, buffer, 1024), SS_LEFT | SS_NOPREFIX, 0);
	}

	/* An unknown default falls back on the first button made. */
	chosen = find_button(state, config->nDefaultButton) ? config->nDefaultButton : state->buttons[0].id;
	state->default_button = chosen;
	SendMessageW(find_button(state, chosen), BM_SETSTYLE, BS_DEFPUSHBUTTON, FALSE);
	SendMessageW(state->dialog, DM_SETDEFID, (WPARAM)chosen, 0);
	lay_out(state);
	SetFocus(find_button(state, chosen));
}

/* Picks the first selected radio button and says so. */
static void select_default_radio(dialog_state *state)
{
	const TASKDIALOGCONFIG *config = &state->config;
	int wanted = config->nDefaultRadioButton;
	state->selected_radio = 0;
	if (!state->radio_count)
		return;
	if (!find_radio(state, wanted)) {
		if (config->dwFlags & TDF_NO_DEFAULT_RADIO_BUTTON)
			return;
		wanted = state->radios[0].id;
	}
	state->selected_radio = wanted;
	SendMessageW(find_radio(state, wanted), BM_SETCHECK, BST_CHECKED, 0);
	notify(state, TDN_RADIO_BUTTON_CLICKED, (WPARAM)wanted, 0);
}

static void click_radio(dialog_state *state, int id)
{
	int index;
	if (!find_radio(state, id))
		return;
	for (index = 0; index < state->radio_count; index++)
		SendMessageW(state->radios[index].window, BM_SETCHECK, state->radios[index].id == id ? BST_CHECKED : BST_UNCHECKED, 0);
	state->selected_radio = id;
	notify(state, TDN_RADIO_BUTTON_CLICKED, (WPARAM)id, 0);
}

static void click_button(dialog_state *state, int id)
{
	if (notify(state, TDN_BUTTON_CLICKED, (WPARAM)id, 0) == S_OK) {
		/* -1 is a valid button number, so it cannot tell by itself that the dialog failed to open. */
		state->ended = TRUE;
		EndDialog(state->dialog, id);
	}
}

static void set_verification(dialog_state *state, BOOL checked, BOOL focus)
{
	state->verification = checked;
	if (state->verify) {
		SendMessageW(state->verify, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
		if (focus)
			SetFocus(state->verify);
	}
	notify(state, TDN_VERIFICATION_CLICKED, (WPARAM)checked, 0);
}

static void toggle_expanded(dialog_state *state)
{
	state->expanded = !state->expanded;
	set_expander_text(state);
	lay_out(state);
	notify(state, TDN_EXPANDO_BUTTON_CLICKED, (WPARAM)state->expanded, 0);
}

static void set_element_text(dialog_state *state, int element, const WCHAR *given)
{
	WCHAR buffer[1024];
	const WCHAR *text = text_of(state, given, buffer, 1024);
	HWND window = NULL;
	switch (element) {
	case TDE_CONTENT:
		window = state->content;
		break;
	case TDE_EXPANDED_INFORMATION:
		window = state->expanded_text;
		break;
	case TDE_FOOTER:
		window = state->footer;
		break;
	case TDE_MAIN_INSTRUCTION:
		window = state->instruction;
		break;
	}
	if (window) {
		SetWindowTextW(window, text ? text : L"");
		ShowWindow(window, SW_SHOWNA);
	}
}

static void navigate(dialog_state *state, const TASKDIALOGCONFIG *config)
{
	state->config = *config;
	state->verification = (config->dwFlags & TDF_VERIFICATION_FLAG_CHECKED) != 0;
	state->in_navigation = TRUE;
	state->ready = FALSE;
	clear_children(state);
	build(state);
	notify(state, TDN_DIALOG_CONSTRUCTED, 0, 0);
	select_default_radio(state);
	state->in_navigation = FALSE;
	state->ready = TRUE;
	notify(state, TDN_NAVIGATED, 0, 0);
}

static void reply(HWND dialog, LRESULT value)
{
	SetWindowLongPtrW(dialog, DWLP_MSGRESULT, value);
}

static INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
	dialog_state *state = (dialog_state *)GetWindowLongPtrW(dialog, DWLP_USER);

	if (message == WM_INITDIALOG) {
		state = (dialog_state *)lparam;
		state->dialog = dialog;
		SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)state);
		make_fonts(state);
		build(state);
		center_window(state);
		state->started = GetTickCount();
		if (state->config.dwFlags & TDF_CALLBACK_TIMER)
			SetTimer(dialog, TIMER_ID, TIMER_PERIOD, NULL);
		notify(state, TDN_DIALOG_CONSTRUCTED, 0, 0);
		notify(state, TDN_CREATED, 0, 0);
		select_default_radio(state);
		state->ready = TRUE;
		return FALSE;
	}
	if (!state)
		return FALSE;

	switch (message) {
	case WM_DESTROY:
		KillTimer(dialog, TIMER_ID);
		notify(state, TDN_DESTROYED, 0, 0);
		return FALSE;
	case WM_ERASEBKGND: {
		RECT client, top;
		HDC dc = (HDC)wparam;
		GetClientRect(dialog, &client);
		top = client;
		top.bottom = state->bar_top;
		FillRect(dc, &top, GetSysColorBrush(COLOR_WINDOW));
		top.top = state->bar_top;
		top.bottom = client.bottom;
		FillRect(dc, &top, GetSysColorBrush(COLOR_BTNFACE));
		top.bottom = top.top + 1;
		FillRect(dc, &top, GetSysColorBrush(COLOR_3DLIGHT));
		reply(dialog, 1);
		return TRUE;
	}
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN: {
		HDC dc = (HDC)wparam;
		RECT area;
		HWND window = (HWND)lparam;
		POINT corner;
		GetWindowRect(window, &area);
		corner.x = area.left;
		corner.y = area.top;
		ScreenToClient(dialog, &corner);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, window == state->instruction ? RGB(0, 51, 153) : GetSysColor(COLOR_WINDOWTEXT));
		return (INT_PTR)GetSysColorBrush(corner.y >= state->bar_top ? COLOR_BTNFACE : COLOR_WINDOW);
	}
	case WM_COMMAND:
		/* A radio button reports a click as soon as it gets the focus, which must not count while the page is being built. */
		if (HIWORD(wparam) == BN_CLICKED && state->ready) {
			const int id = (short)LOWORD(wparam);
			HWND from = (HWND)lparam;
			if (from && from == state->expander) {
				toggle_expanded(state);
				return TRUE;
			}
			if (from && from == state->verify) {
				set_verification(state, !state->verification, FALSE);
				return TRUE;
			}
			if (find_radio(state, id) && from == find_radio(state, id)) {
				click_radio(state, id);
				return TRUE;
			}
			if (find_button(state, id)) {
				click_button(state, id);
				return TRUE;
			}
			/* Return comes as OK, which stands for the default button when there is no such button. */
			if (id == IDOK) {
				click_button(state, state->default_button);
				return TRUE;
			}
			if (id == IDCANCEL && state->has_cancel) {
				click_button(state, IDCANCEL);
				return TRUE;
			}
		}
		return FALSE;
	case WM_NOTIFY: {
		const NMHDR *header = (const NMHDR *)lparam;
		if ((header->code == NM_CLICK || header->code == NM_RETURN) && (header->hwndFrom == state->content || header->hwndFrom == state->footer)) {
			const NMLINK *link = (const NMLINK *)lparam;
			notify(state, TDN_HYPERLINK_CLICKED, 0, (LPARAM)link->item.szUrl);
			return TRUE;
		}
		return FALSE;
	}
	case WM_TIMER:
		if (wparam == TIMER_ID) {
			if (notify(state, TDN_TIMER, GetTickCount() - state->started, 0) == S_FALSE)
				state->started = GetTickCount();
			return TRUE;
		}
		return FALSE;
	case WM_HELP:
		notify(state, TDN_HELP, 0, 0);
		return TRUE;
	case WM_CLOSE:
		if (state->has_cancel)
			click_button(state, IDCANCEL);
		return TRUE;

	case TDM_CLICK_BUTTON:
		click_button(state, (int)wparam);
		return TRUE;
	case TDM_CLICK_RADIO_BUTTON:
		click_radio(state, (int)wparam);
		return TRUE;
	case TDM_CLICK_VERIFICATION:
		set_verification(state, (BOOL)wparam, (BOOL)lparam);
		return TRUE;
	case TDM_ENABLE_BUTTON: {
		HWND window = find_button(state, (int)wparam);
		if (window)
			EnableWindow(window, lparam != 0);
		return TRUE;
	}
	case TDM_ENABLE_RADIO_BUTTON: {
		HWND window = find_radio(state, (int)wparam);
		if (window)
			EnableWindow(window, lparam != 0);
		return TRUE;
	}
	case TDM_SET_BUTTON_ELEVATION_REQUIRED_STATE: {
		HWND window = find_button(state, (int)wparam);
		if (window)
			SendMessageW(window, BCM_SETSHIELD, 0, lparam);
		return TRUE;
	}
	case TDM_SET_ELEMENT_TEXT:
		set_element_text(state, (int)wparam, (const WCHAR *)lparam);
		lay_out(state);
		return TRUE;
	case TDM_UPDATE_ELEMENT_TEXT:
		set_element_text(state, (int)wparam, (const WCHAR *)lparam);
		return TRUE;
	case TDM_UPDATE_ICON: {
		const WCHAR *given = (const WCHAR *)lparam;
		HICON icon;
		if (wparam == TDIE_ICON_MAIN) {
			icon = (state->config.dwFlags & TDF_USE_HICON_MAIN) ? (HICON)lparam : icon_of(state, given, TRUE);
			if (state->icon)
				SendMessageW(state->icon, STM_SETICON, (WPARAM)icon, 0);
		}
		else {
			icon = (state->config.dwFlags & TDF_USE_HICON_FOOTER) ? (HICON)lparam : icon_of(state, given, FALSE);
			if (state->footer_icon)
				SendMessageW(state->footer_icon, STM_SETICON, (WPARAM)icon, 0);
		}
		return TRUE;
	}
	case TDM_SET_MARQUEE_PROGRESS_BAR:
		if (state->progress) {
			LONG_PTR style = GetWindowLongPtrW(state->progress, GWL_STYLE);
			SetWindowLongPtrW(state->progress, GWL_STYLE, wparam ? style | PBS_MARQUEE : style & ~(LONG_PTR)PBS_MARQUEE);
		}
		return TRUE;
	case TDM_SET_PROGRESS_BAR_STATE:
		reply(dialog, state->progress ? SendMessageW(state->progress, PBM_SETSTATE, wparam, 0) : 0);
		return TRUE;
	case TDM_SET_PROGRESS_BAR_RANGE:
		reply(dialog, state->progress ? SendMessageW(state->progress, PBM_SETRANGE, 0, lparam) : 0);
		return TRUE;
	case TDM_SET_PROGRESS_BAR_POS:
		/* A marquee has no position to report. */
		if (state->progress && (GetWindowLongPtrW(state->progress, GWL_STYLE) & PBS_MARQUEE))
			reply(dialog, 0);
		else
			reply(dialog, state->progress ? SendMessageW(state->progress, PBM_SETPOS, wparam, 0) : 0);
		return TRUE;
	case TDM_SET_PROGRESS_BAR_MARQUEE:
		if (state->progress)
			SendMessageW(state->progress, PBM_SETMARQUEE, wparam, lparam);
		return TRUE;
	case TDM_NAVIGATE_PAGE:
		navigate(state, (const TASKDIALOGCONFIG *)lparam);
		return TRUE;
	}
	return FALSE;
}

HRESULT WINAPI TaskDialogIndirect(const TASKDIALOGCONFIG *config, int *button, int *radio, BOOL *verification)
{
	/* The window has no children in its template: everything is made from the configuration. */
	static const struct {
		DLGTEMPLATE head;
		WORD menu, type, title;
	} shell = { { WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, 0, 0, 0, 0, 0, 0 }, 0, 0, 0 };
	dialog_state state;
	INT_PTR result;

	if (!config || config->cbSize != sizeof(*config))
		return E_INVALIDARG;
	ZeroMemory(&state, sizeof(state));
	state.config = *config;
	state.verification = (config->dwFlags & TDF_VERIFICATION_FLAG_CHECKED) != 0;
	result = DialogBoxIndirectParamW(config->hInstance, &shell.head, config->hwndParent, dialog_proc, (LPARAM)&state);
	free_tables(&state);
	if (state.plain)
		DeleteObject(state.plain);
	if (state.bold)
		DeleteObject(state.bold);
	if (result == -1 && !state.ended)
		return HRESULT_FROM_WIN32(GetLastError());
	if (button)
		*button = (int)result;
	if (radio)
		*radio = state.selected_radio;
	if (verification)
		*verification = state.verification;
	return S_OK;
}

HRESULT WINAPI TaskDialog(HWND owner, HINSTANCE instance, const WCHAR *title, const WCHAR *instruction, const WCHAR *content, TASKDIALOG_COMMON_BUTTON_FLAGS buttons, const WCHAR *icon, int *button)
{
	TASKDIALOGCONFIG config;
	ZeroMemory(&config, sizeof(config));
	config.cbSize = sizeof(config);
	config.hwndParent = owner;
	config.hInstance = instance;
	config.dwCommonButtons = buttons;
	config.pszWindowTitle = title;
	config.pszMainInstruction = instruction;
	config.pszContent = content;
	config.pszMainIcon = icon;
	return TaskDialogIndirect(&config, button, NULL, NULL);
}
