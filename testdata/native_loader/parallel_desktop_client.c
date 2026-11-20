#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Threads that each keep a window of their own and call USER and GDI at once, as the threads of a program with windows do: queries of
// the window and of the desktop, window data and properties, menus, regions and bitmaps, input state, the message queue, invalidation
// and device contexts. What each costs, and whether the runtime answers it beside the turn or the guest answers it alone, is what
// RAVEN_CYCLES reports.

static volatile LONG ready;
static HANDLE start;
static HWND painted;
static HANDLE painter_ready;
static HMENU menu, popup;
static HRGN region;
static HBITMAP bitmap;
static ATOM count_atom;

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_USER)
		return (LRESULT)wparam + 1;
	return DefWindowProcW(window, message, wparam, lparam);
}

static DWORD WINAPI calls(void *parameter)
{
	const int index = (int)(INT_PTR)parameter;
	HWND window = CreateWindowExW(0, L"raven.parallel.desktop", L"desktop", WS_OVERLAPPEDWINDOW, 10 * index, 10 * index, 160, 120,
		NULL, NULL, GetModuleHandleW(NULL), NULL);
	if (!window)
		return 10;
	InterlockedIncrement(&ready);
	if (WaitForSingleObject(start, 5000) != WAIT_OBJECT_0)
		return 11;
	for (unsigned i = 0; i < 300; ++i) {
		RECT frame, client;
		if (!GetWindowRect(window, &frame) || !GetClientRect(window, &client) || client.right <= 0)
			return 12;
		if (IsWindowVisible(window) || !IsWindow(window) || GetParent(window) || GetAncestor(window, GA_ROOT) != window)
			return 13;
		SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)i);
		if (GetWindowLongPtrW(window, GWLP_USERDATA) != (LONG_PTR)i)
			return 14;
		if (!SetPropW(window, (LPCWSTR)(ULONG_PTR)count_atom, (HANDLE)(UINT_PTR)(i + 1)) ||
			GetPropW(window, (LPCWSTR)(ULONG_PTR)count_atom) != (HANDLE)(UINT_PTR)(i + 1))
			return 15;
		if (!SetPropW(window, L"raven.name", (HANDLE)(UINT_PTR)(i + 2)) || RemovePropW(window, L"raven.name") != (HANDLE)(UINT_PTR)(i + 2) ||
			RemovePropW(window, L"raven.name"))
			return 24;
		if (GetMenuItemCount(menu) != 2 || GetMenuItemID(menu, 0) != 100 || GetSubMenu(menu, 1) != popup)
			return 21;
		RECT bounds, inside = {2, 2, 4, 4};
		if (!PtInRegion(region, 5, 5) || PtInRegion(region, 50, 50) || !RectInRegion(region, &inside) ||
			GetRgnBox(region, &bounds) != SIMPLEREGION || bounds.right != 10)
			return 22;
		BITMAP described;
		if (GetObjectW(bitmap, sizeof(described), &described) != sizeof(described) || described.bmWidth != 16)
			return 23;
		GetFocus();
		if (GetSystemMetrics(SM_CXSCREEN) <= 0 || !MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST))
			return 16;
		POINT cursor;
		if (!GetCursorPos(&cursor))
			return 17;
		GetKeyState(VK_SHIFT);
		GetForegroundWindow();
		if (!InvalidateRect(window, NULL, FALSE))
			return 18;
		HDC context = GetDC(window);
		if (!context || !ReleaseDC(window, context))
			return 19;
		if (SendMessageW(window, WM_USER, i, 0) != (LRESULT)i + 1)
			return 20;
		MSG message;
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
			DispatchMessageW(&message);
	}
	DestroyWindow(window);
	return 0;
}

// A thread that waits for its messages and paints its window when another thread invalidates it: the invalidation, served beside
// the turn, has to wake it.
static DWORD WINAPI painter(void *unused)
{
	(void)unused;
	painted = CreateWindowExW(0, L"raven.parallel.desktop", L"painted", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 160, 120, NULL, NULL,
		GetModuleHandleW(NULL), NULL);
	if (!painted)
		return 30;
	MSG message;
	while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
		DispatchMessageW(&message);
	ValidateRect(painted, NULL);
	SetEvent(painter_ready);
	while (GetMessageW(&message, NULL, 0, 0) > 0) {
		if (message.message == WM_PAINT && message.hwnd == painted) {
			PAINTSTRUCT paint;
			BeginPaint(painted, &paint);
			EndPaint(painted, &paint);
			DestroyWindow(painted);
			return 0;
		}
		DispatchMessageW(&message);
	}
	return 31;
}

int raven_entry(void)
{
	WNDCLASSW definition = {0};
	definition.lpfnWndProc = procedure;
	definition.hInstance = GetModuleHandleW(NULL);
	definition.lpszClassName = L"raven.parallel.desktop";
	if (!RegisterClassW(&definition))
		return 1;
	start = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!start)
		return 2;
	menu = CreateMenu();
	popup = CreatePopupMenu();
	region = CreateRectRgn(0, 0, 10, 10);
	bitmap = CreateBitmap(16, 16, 1, 32, NULL);
	count_atom = GlobalAddAtomW(L"raven.count");
	if (!menu || !popup || !AppendMenuW(menu, MF_STRING, 100, L"Open") || !AppendMenuW(menu, MF_POPUP, (UINT_PTR)popup, L"More") ||
		!region || !bitmap || !count_atom)
		return 5;
	HANDLE threads[4];
	for (int index = 0; index < 4; ++index)
		if (!(threads[index] = CreateThread(NULL, 0, calls, (void *)(INT_PTR)index, 0, NULL)))
			return 3;
	while (ready < 4)
		Sleep(1);
	SetEvent(start);
	if (WaitForMultipleObjects(4, threads, TRUE, 30000) != WAIT_OBJECT_0)
		return 4;
	for (int index = 0; index < 4; ++index) {
		DWORD code = 0;
		if (!GetExitCodeThread(threads[index], &code) || code)
			return 100 + index * 20 + (int)code;
	}
	painter_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	HANDLE painting = painter_ready ? CreateThread(NULL, 0, painter, NULL, 0, NULL) : NULL;
	if (!painting || WaitForSingleObject(painter_ready, 5000) != WAIT_OBJECT_0)
		return 5;
	// Long enough for the painter to be waiting for its messages.
	Sleep(50);
	if (!InvalidateRect(painted, NULL, FALSE))
		return 6;
	DWORD code = 0;
	if (WaitForSingleObject(painting, 10000) != WAIT_OBJECT_0 || !GetExitCodeThread(painting, &code))
		return 7;
	return (int)code;
}
