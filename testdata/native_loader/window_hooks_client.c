#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define CHECK(code, condition) do { if (!(condition)) return code; } while (0)

static HHOOK main_hook, worker_hook;
static volatile LONG main_creates, worker_creates;

static LRESULT CALLBACK main_cbt(int code, WPARAM w_param, LPARAM l_param)
{
	if (code == HCBT_CREATEWND)
		++main_creates;
	return CallNextHookEx(main_hook, code, w_param, l_param);
}

static LRESULT CALLBACK worker_cbt(int code, WPARAM w_param, LPARAM l_param)
{
	if (code == HCBT_CREATEWND)
		++worker_creates;
	return CallNextHookEx(worker_hook, code, w_param, l_param);
}

// A hook for one thread runs only for windows that thread creates, also while another thread has a hook of the same kind.
static DWORD WINAPI worker(void *parameter)
{
	(void)parameter;
	worker_hook = SetWindowsHookExW(WH_CBT, worker_cbt, 0, GetCurrentThreadId());
	if (!worker_hook)
		return 1;
	HWND window = CreateWindowExW(0, L"RavenHookFixture", L"", 0, 0, 0, 10, 10, 0, 0, 0, 0);
	const DWORD result = window && worker_creates == 1 && main_creates == 0 ? 0 : 2;
	if (window)
		DestroyWindow(window);
	UnhookWindowsHookEx(worker_hook);
	return result;
}

static int same(const WCHAR *left, const WCHAR *right)
{
	unsigned i = 0;
	for (; left[i] || right[i]; ++i) if (left[i] != right[i]) return 0;
	return 1;
}

int raven_entry(void)
{
	WNDCLASSW type = {0};
	type.lpfnWndProc = DefWindowProcW;
	type.lpszClassName = L"RavenHookFixture";
	CHECK(26, RegisterClassW(&type));
	HWND root = CreateWindowExW(0, L"RavenHookFixture", L"root", WS_OVERLAPPEDWINDOW, 0, 0, 300, 200, 0, 0, 0, 0);
	CHECK(1, root);
	// Without a display nothing can be shown: the children stay hidden, which ChildWindowFromPoint looks past and RealChildWindowFromPoint
	// does not. A point passed by value lands where it was meant to on either width.
	HWND child = CreateWindowExW(0, L"RavenHookFixture", L"", WS_CHILD, 10, 10, 100, 60, root, (HMENU)1, 0, 0);
	HWND beside = CreateWindowExW(0, L"RavenHookFixture", L"", WS_CHILD, 150, 10, 60, 60, root, (HMENU)2, 0, 0);
	CHECK(2, child && beside);
	CHECK(3, DefWindowProcW(root, WM_NOTIFYFORMAT, (WPARAM)child, NF_QUERY) == NFR_UNICODE);
	CHECK(4, DefWindowProcW(child, WM_NOTIFYFORMAT, (WPARAM)root, NF_REQUERY) == NFR_UNICODE);
	POINT point = {30, 30};
	CHECK(5, ChildWindowFromPoint(root, point) == child);
	CHECK(6, RealChildWindowFromPoint(root, point) == root);
	point.x = 170;
	CHECK(7, ChildWindowFromPoint(root, point) == beside && ChildWindowFromPointEx(root, point, CWP_ALL) == beside);
	CHECK(8, ChildWindowFromPointEx(root, point, CWP_SKIPINVISIBLE) == root);
	point.y = 5;
	CHECK(9, ChildWindowFromPoint(root, point) == root);

	// MENUITEMINFO has the program's pointer width.
	HMENU menu = CreatePopupMenu();
	CHECK(10, menu && AppendMenuW(menu, MF_STRING, 7, L"first"));
	MENUITEMINFOW item = {sizeof item};
	item.fMask = MIIM_STRING | MIIM_DATA | MIIM_ID;
	item.dwTypeData = L"renamed";
	item.dwItemData = 0x5a5a;
	item.wID = 9;
	CHECK(11, SetMenuItemInfoW(menu, 7, FALSE, &item));
	WCHAR text[32];
	MENUITEMINFOW read = {sizeof read};
	read.fMask = MIIM_STRING | MIIM_DATA | MIIM_ID;
	read.dwTypeData = text;
	read.cch = 32;
	CHECK(12, GetMenuItemInfoW(menu, 0, TRUE, &read) && same(text, L"renamed") && read.dwItemData == 0x5a5a && read.wID == 9 && read.cch == 7);
	CHECK(13, DestroyMenu(menu));

	main_hook = SetWindowsHookExW(WH_CBT, main_cbt, 0, GetCurrentThreadId());
	CHECK(14, main_hook);
	HANDLE thread = CreateThread(0, 0, worker, 0, 0, 0);
	DWORD code = 99;
	CHECK(15, thread && WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &code) && CloseHandle(thread));
	CHECK(16, code == 0 && main_creates == 0);
	HWND mine = CreateWindowExW(0, L"RavenHookFixture", L"", 0, 0, 0, 10, 10, 0, 0, 0, 0);
	CHECK(17, mine && main_creates == 1 && worker_creates == 1);
	CHECK(18, UnhookWindowsHookEx(main_hook) && DestroyWindow(mine));

	// An overlapped read that finishes is seen by a program that only polls with a zero timeout.
	HANDLE server = CreateNamedPipeW(L"\\\\.\\pipe\\raven-zero-wait", PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE, 1, 512, 512, 0, 0);
	CHECK(19, server != INVALID_HANDLE_VALUE);
	HANDLE client = CreateFileW(L"\\\\.\\pipe\\raven-zero-wait", GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
	CHECK(20, client != INVALID_HANDLE_VALUE);
	OVERLAPPED overlapped = {0};
	overlapped.hEvent = CreateEventW(0, TRUE, TRUE, 0);
	char buffer[16];
	DWORD count = 0;
	CHECK(21, overlapped.hEvent && !ReadFile(server, buffer, sizeof buffer, 0, &overlapped) && GetLastError() == ERROR_IO_PENDING);
	CHECK(22, WaitForSingleObject(overlapped.hEvent, 0) == WAIT_TIMEOUT);
	CHECK(23, WriteFile(client, "ping", 4, &count, 0) && count == 4);
	const DWORD start = GetTickCount();
	DWORD state = WAIT_TIMEOUT;
	while (state == WAIT_TIMEOUT && GetTickCount() - start < 5000)
		state = WaitForMultipleObjects(1, &overlapped.hEvent, FALSE, 0);
	CHECK(24, state == WAIT_OBJECT_0 && GetOverlappedResult(server, &overlapped, &count, FALSE) && count == 4 && buffer[0] == 'p');
	CHECK(25, CloseHandle(client) && CloseHandle(server) && CloseHandle(overlapped.hEvent) && DestroyWindow(root));

	HWND hidden = CreateWindowExW(0, type.lpszClassName, L"hidden", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		CW_USEDEFAULT, SW_HIDE, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, NULL, NULL);
	CHECK(27, hidden);
	CHECK(28, !IsWindowVisible(hidden) && !(GetWindowLongW(hidden, GWL_STYLE) & WS_VISIBLE));
	CHECK(29, !ShowWindow(hidden, SW_SHOW) && IsWindowVisible(hidden) && DestroyWindow(hidden));
	HWND maximized = CreateWindowExW(0, type.lpszClassName, L"maximized", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		CW_USEDEFAULT, SW_SHOWMAXIMIZED, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, NULL, NULL);
	CHECK(30, maximized && IsWindowVisible(maximized) && (GetWindowLongW(maximized, GWL_STYLE) & WS_MAXIMIZE));
	CHECK(31, DestroyWindow(maximized));
	return 0;
}
