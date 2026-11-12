#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Window state read back right after it changes, including from inside the messages the change sends, whether the runtime or the
   program answers the reads. */

static int size_seen, size_failed;

static LRESULT CALLBACK message_filter(int code, WPARAM wparam, LPARAM lparam)
{
	return code == 0x1234 ? 77 : 0;
}

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_SIZE) {
		RECT client;
		++size_seen;
		if (!GetClientRect(window, &client) || client.left || client.top || client.right != LOWORD(lparam) || client.bottom != HIWORD(lparam))
			size_failed = 1;
		if (GetWindowLongPtrW(window, GWLP_USERDATA) != 0x1234 && GetWindowLongPtrW(window, GWLP_USERDATA) != 0)
			size_failed = 1;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

int raven_entry(void)
{
	WNDCLASSW type = {0};
	type.lpfnWndProc = procedure;
	type.hInstance = GetModuleHandleW(NULL);
	type.lpszClassName = L"RavenWindowState";
	type.cbWndExtra = 16;
	if (!RegisterClassW(&type))
		return 1;
	HWND window = CreateWindowExW(0, L"RavenWindowState", L"state", WS_OVERLAPPEDWINDOW, 10, 20, 300, 200, NULL, NULL, type.hInstance, NULL);
	if (!window || !IsWindow(window) || !IsWindowUnicode(window))
		return 2;
	if (GetWindowLongPtrW(window, GWLP_WNDPROC) != (LONG_PTR)procedure || GetWindowLongPtrW(window, GWLP_HINSTANCE) != (LONG_PTR)type.hInstance)
		return 3;
	/* Every read follows the write before it. */
	for (LONG_PTR value = 1; value < 40; ++value) {
		SetWindowLongPtrW(window, GWLP_USERDATA, value);
		SetWindowLongPtrW(window, 8, value * 3);
		SetWindowLongW(window, 4, (LONG)value * 5);
		if (GetWindowLongPtrW(window, GWLP_USERDATA) != value || GetWindowLongPtrW(window, 8) != value * 3 || GetWindowLongW(window, 4) != value * 5 ||
			GetWindowLongW(window, -21) != (LONG)value)
			return 4;
	}
	SetWindowLongPtrW(window, GWLP_USERDATA, 0x1234);
	/* Indexes past the extra bytes fail as before. */
	SetLastError(0);
	if (GetWindowLongPtrW(window, 12) != 0 || GetLastError() != ERROR_INVALID_INDEX)
		return 5;
	if ((GetWindowLongW(window, GWL_STYLE) & WS_OVERLAPPEDWINDOW) != WS_OVERLAPPEDWINDOW)
		return 6;
	SetWindowLongW(window, GWL_STYLE, GetWindowLongW(window, GWL_STYLE) | WS_CLIPCHILDREN);
	if (!(GetWindowLongW(window, GWL_STYLE) & WS_CLIPCHILDREN))
		return 7;
	/* Properties named by an atom, set and removed. */
	if (GetPropW(window, (LPCWSTR)(ULONG_PTR)0xc123) || !SetPropW(window, (LPCWSTR)(ULONG_PTR)0xc123, (HANDLE)0x55))
		return 8;
	if (GetPropW(window, (LPCWSTR)(ULONG_PTR)0xc123) != (HANDLE)0x55 || GetPropW(window, (LPCWSTR)(ULONG_PTR)0xc124))
		return 9;
	RemovePropW(window, (LPCWSTR)(ULONG_PTR)0xc123);
	if (GetPropW(window, (LPCWSTR)(ULONG_PTR)0xc123))
		return 10;
	/* Rectangles after a move, and inside the size message the move sends. */
	size_seen = 0;
	if (!MoveWindow(window, 30, 40, 420, 330, TRUE) || !size_seen || size_failed)
		return 11;
	RECT outer, client;
	if (!GetWindowRect(window, &outer) || outer.left != 30 || outer.top != 40 || outer.right != 450 || outer.bottom != 370)
		return 12;
	if (!GetClientRect(window, &client) || client.right <= 0 || client.right > 420 || client.bottom <= 0 || client.bottom > 330)
		return 13;
	/* Ancestry, focus and resolution. */
	HWND child = CreateWindowExW(0, L"RavenWindowState", L"child", WS_CHILD, 0, 0, 50, 50, window, NULL, type.hInstance, NULL);
	HWND grandchild = CreateWindowExW(0, L"RavenWindowState", L"grandchild", WS_CHILD, 0, 0, 10, 10, child, NULL, type.hInstance, NULL);
	HWND other = CreateWindowExW(0, L"RavenWindowState", L"other", WS_OVERLAPPEDWINDOW, 0, 0, 50, 50, NULL, NULL, type.hInstance, NULL);
	if (!child || !grandchild || !other)
		return 15;
	if (!IsChild(window, child) || !IsChild(window, grandchild) || !IsChild(child, grandchild) || IsChild(grandchild, window) ||
		IsChild(window, window) || IsChild(window, other) || IsChild(NULL, child) || IsChild(window, NULL))
		return 16;
	SetFocus(child);
	if (GetFocus() != child)
		return 17;
	SetFocus(other);
	if (GetFocus() != other)
		return 18;
	if (!AllowSetForegroundWindow(GetCurrentProcessId()) || !SetForegroundWindow(window) || GetForegroundWindow() != window ||
		!AllowSetForegroundWindow(ASFW_ANY))
		return 40;
	SetLastError(0);
	if (AllowSetForegroundWindow(0) || GetLastError() != ERROR_INVALID_PARAMETER || GetForegroundWindow() != window)
		return 41;
	if (GetDpiForWindow(window) != 96)
		return 19;
	/* The message loop's helpers on messages they leave alone. */
	MSG message = {window, WM_MOUSEMOVE, 0, 0, 0, {0, 0}};
	for (int round = 0; round < 3; ++round) {
		if (TranslateMessage(&message) || CallMsgFilterW(&message, 0x1234) || !GetDesktopWindow() || GetDesktopWindow() != GetDesktopWindow())
			return 20;
	}
	HHOOK hook = SetWindowsHookExW(WH_MSGFILTER, message_filter, NULL, GetCurrentThreadId());
	if (!hook || CallMsgFilterW(&message, 0x1234) != 77 || !UnhookWindowsHookEx(hook) || CallMsgFilterW(&message, 0x1234))
		return 21;
	ACCEL keys[] = {{FVIRTKEY | FCONTROL, 'S', 900}};
	HACCEL table = CreateAcceleratorTableW(keys, 1);
	SetLastError(0);
	if (!table || TranslateAcceleratorW(window, table, &message) || GetLastError())
		return 22;
	if (!DestroyAcceleratorTable(table) || TranslateAcceleratorW(window, table, &message) || GetLastError() != ERROR_INVALID_WINDOW_HANDLE)
		return 23;
	SetLastError(0);
	if (TranslateAcceleratorW(window, NULL, &message) || GetLastError() != ERROR_INVALID_WINDOW_HANDLE)
		return 24;
	/* A child sized from a parent that has no size yet comes out negative; CreateWindowEx makes it empty instead of refusing it, as a
	   tab page made inside a tab control that is not laid out yet is. */
	HWND empty = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 4, 4, -8, -6, window, NULL, NULL, NULL);
	RECT bounds;
	if (!empty || !GetClientRect(empty, &bounds) || bounds.right || bounds.bottom)
		return 25;
	DestroyWindow(empty);
	DestroyWindow(other);
	if (!DestroyWindow(window) || IsWindow(window) || GetWindowLongPtrW(window, GWLP_USERDATA) || GetLastError() != ERROR_INVALID_WINDOW_HANDLE)
		return 14;
	return 0;
}
