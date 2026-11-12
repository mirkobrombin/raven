// A tooltip whose tool has no text does not come up: owners such as WinForms answer an empty text to keep a tip hidden. Once the tool
// has text, the same tip shows. The exit code names the first check that failed.
#include <windows.h>
#include <commctrl.h>

static const WCHAR *answer = L"";

static LRESULT CALLBACK owner_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_NOTIFY && ((NMHDR *)lparam)->code == TTN_GETDISPINFOW) {
		((NMTTDISPINFOW *)lparam)->lpszText = (WCHAR *)answer;
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

int raven_entry(void)
{
	INITCOMMONCONTROLSEX classes = {sizeof classes, ICC_WIN95_CLASSES};
	InitCommonControlsEx(&classes);
	WNDCLASSW type = {0};
	type.lpfnWndProc = owner_procedure;
	type.lpszClassName = L"TipOwner";
	if (!RegisterClassW(&type))
		return 1;
	HWND owner = CreateWindowExW(0, L"TipOwner", L"owner", WS_POPUP | WS_VISIBLE, 0, 0, 200, 100, 0, 0, 0, 0);
	HWND tip = owner ? CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0, owner, NULL, NULL, NULL) : NULL;
	if (!tip)
		return 2;
	TTTOOLINFOW tool = {sizeof tool};
	tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
	tool.hwnd = owner;
	tool.uId = 1;
	tool.lpszText = LPSTR_TEXTCALLBACKW;
	if (!SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&tool))
		return 3;
	SendMessageW(tip, TTM_TRACKPOSITION, 0, MAKELPARAM(40, 40));
	SendMessageW(tip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&tool);
	if (IsWindowVisible(tip))
		return 4;
	SendMessageW(tip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&tool);
	answer = L"Copy the password";
	SendMessageW(tip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&tool);
	if (!IsWindowVisible(tip))
		return 5;
	DestroyWindow(owner);
	return 0;
}
