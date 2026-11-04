// TB_CUSTOMIZE opens the Customize Toolbar dialog: the parent offers its buttons, the dialog adds, moves and removes them on the
// toolbar, tells the parent of every change and closes. A timer drives the dialog the way a person would. With /show the dialog stays
// open for a person to use.
#include <windows.h>
#include <commctrl.h>

enum { AVAILABLE = 201, CURRENT = 202, HELP = 204, ADD = 205, REMOVE = 206, DOWN = 208 };

static HWND toolbar;
static int began, initialized, changes, ended, step, failure, interactive;
static const wchar_t *names[] = {L"New", L"Open", L"Save", L"Print"};
static const int images[] = {STD_FILENEW, STD_FILEOPEN, STD_FILESAVE, STD_PRINT};

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_NOTIFY) {
		NMHDR *header = (NMHDR *)lparam;
		if (header->hwndFrom != toolbar)
			return 0;
		switch (header->code) {
		case TBN_BEGINADJUST: ++began; return 0;
		case TBN_INITCUSTOMIZE: ++initialized; return TBNRF_HIDEHELP;
		case TBN_QUERYINSERT: case TBN_QUERYDELETE: return TRUE;
		case TBN_TOOLBARCHANGE: ++changes; return 0;
		case TBN_ENDADJUST: ++ended; return 0;
		case TBN_GETBUTTONINFOW: {
			NMTOOLBARW *question = (NMTOOLBARW *)lparam;
			if (question->iItem < 0 || question->iItem >= 4)
				return FALSE;
			question->tbButton.iBitmap = images[question->iItem];
			question->tbButton.idCommand = 100 + question->iItem;
			question->tbButton.fsState = TBSTATE_ENABLED;
			question->tbButton.fsStyle = BTNS_BUTTON;
			lstrcpynW(question->pszText, names[question->iItem], question->cchText);
			question->tbButton.iString = (INT_PTR)question->pszText;
			return TRUE;
		}
		}
		return 0;
	}
	if (message == WM_TIMER && !interactive) {
		HWND dialog = FindWindowW(NULL, L"Customize Toolbar");
		HWND available = dialog ? GetDlgItem(dialog, AVAILABLE) : NULL, current = dialog ? GetDlgItem(dialog, CURRENT) : NULL;
		TBBUTTON button;
		if (!dialog || !available || !current)
			return 0;
		KillTimer(window, 1);
		// Help is hidden at the parent's word; the separator comes first among the available buttons, then Print, the one missing.
		if (GetDlgItem(dialog, HELP) || SendMessageW(available, LB_GETCOUNT, 0, 0) != 2 || SendMessageW(current, LB_GETCOUNT, 0, 0) != 5)
			failure = 10;
		// Print goes before the first button, then moves one place down, then New, now first, goes away.
		SendMessageW(available, LB_SETCURSEL, 1, 0);
		SendMessageW(current, LB_SETCURSEL, 0, 0);
		SendMessageW(dialog, WM_COMMAND, ADD, 0);
		if (!failure && (SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0) != 5 || !SendMessageW(toolbar, TB_GETBUTTON, 0, (LPARAM)&button) || button.idCommand != 103))
			failure = 11;
		SendMessageW(current, LB_SETCURSEL, 0, 0);
		SendMessageW(dialog, WM_COMMAND, DOWN, 0);
		if (!failure && (!SendMessageW(toolbar, TB_GETBUTTON, 1, (LPARAM)&button) || button.idCommand != 103))
			failure = 12;
		SendMessageW(current, LB_SETCURSEL, 0, 0);
		SendMessageW(dialog, WM_COMMAND, REMOVE, 0);
		if (!failure && (SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0) != 4 || !SendMessageW(toolbar, TB_GETBUTTON, 0, (LPARAM)&button) || button.idCommand != 103))
			failure = 13;
		SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

int raven_entry(void)
{
	const wchar_t *line = GetCommandLineW();
	for (; *line; ++line)
		if (line[0] == L'/' && line[1] == L's' && line[2] == L'h')
			interactive = 1;
	INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_BAR_CLASSES};
	InitCommonControlsEx(&controls);
	WNDCLASSW definition = {0};
	definition.lpfnWndProc = procedure;
	definition.hInstance = GetModuleHandleW(NULL);
	definition.lpszClassName = L"RavenCustomizeParent";
	definition.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	if (!RegisterClassW(&definition))
		return 1;
	HWND parent = CreateWindowExW(0, definition.lpszClassName, L"Toolbar customization", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 480, 200, NULL, NULL,
		definition.hInstance, NULL);
	toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE | CCS_ADJUSTABLE | TBSTYLE_FLAT, 0, 0, 0, 0, parent, (HMENU)1,
		definition.hInstance, NULL);
	if (!parent || !toolbar)
		return 2;
	SendMessageW(toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
	TBADDBITMAP bitmaps = {HINST_COMMCTRL, IDB_STD_SMALL_COLOR};
	SendMessageW(toolbar, TB_ADDBITMAP, 0, (LPARAM)&bitmaps);
	TBBUTTON buttons[] = {
		{STD_FILENEW, 100, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, (INT_PTR)L"New"},
		{STD_FILEOPEN, 101, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, (INT_PTR)L"Open"},
		{0, 0, 0, BTNS_SEP, {0}, 0, 0},
		{STD_FILESAVE, 102, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, (INT_PTR)L"Save"},
	};
	SendMessageW(toolbar, TB_ADDBUTTONSW, 4, (LPARAM)buttons);
	SendMessageW(toolbar, TB_AUTOSIZE, 0, 0);
	if (!interactive)
		SetTimer(parent, 1, 50, NULL);
	SendMessageW(toolbar, TB_CUSTOMIZE, 0, 0);
	if (failure)
		return failure;
	if (began != 1 || initialized != 1 || ended != 1)
		return 3;
	if (!interactive && changes != 3)
		return 4;
	DestroyWindow(parent);
	return 0;
}
