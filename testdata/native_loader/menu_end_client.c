#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// A shortcut menu that another thread has closed: while TrackPopupMenu runs, a second thread sends the owner window a message, which
// the menu's loop answers, and the window procedure ends the menu with EndMenu. The menu comes back without a choice.
#define CLOSE_MENU (WM_APP + 1)
static volatile LONG tracking, closed_in_menu;

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == CLOSE_MENU) {
		if (tracking)
			closed_in_menu = 1;
		EndMenu();
		return 7;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

static DWORD WINAPI close_later(void *window)
{
	Sleep(300);
	return SendMessageW((HWND)window, CLOSE_MENU, 0, 0) == 7 ? 0 : 1;
}

int raven_entry(void)
{
	WNDCLASSW definition = {0};
	definition.lpfnWndProc = procedure;
	definition.hInstance = GetModuleHandleW(NULL);
	definition.lpszClassName = L"raven.menu.end";
	if (!RegisterClassW(&definition)) return 1;
	HWND window = CreateWindowExW(0, definition.lpszClassName, L"menu end", WS_POPUP | WS_VISIBLE, 0, 0, 200, 150, NULL, NULL,
		definition.hInstance, NULL);
	HMENU popup = CreatePopupMenu();
	if (!window || !popup || !AppendMenuW(popup, MF_STRING, 701, L"Other")) return 2;
	HANDLE closer = CreateThread(NULL, 0, close_later, window, 0, NULL);
	if (!closer) return 3;
	tracking = 1;
	const UINT chosen = TrackPopupMenu(popup, TPM_RETURNCMD, 10, 10, 0, window, NULL);
	tracking = 0;
	DWORD sent = 1;
	if (WaitForSingleObject(closer, 5000) || !GetExitCodeThread(closer, &sent)) return 4;
	DestroyMenu(popup);
	DestroyWindow(window);
	return chosen ? 5 : !closed_in_menu ? 6 : sent ? 7 : 0;
}
