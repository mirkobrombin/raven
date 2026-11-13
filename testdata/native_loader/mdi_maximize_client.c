#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* A document that ShowWindow maximizes covers the MDI client's area with its own, hears WM_SIZE with SIZE_MAXIMIZED, and stays that
   size when the client grows; a restore gives back the rectangle it had. A plain child window is maximized the same way. */
static WPARAM last_size = 99;

static LRESULT CALLBACK document(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_SIZE)
		last_size = wparam;
	return DefMDIChildProcW(window, message, wparam, lparam);
}

static LRESULT CALLBACK frame(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return DefFrameProcW(window, (HWND)GetWindowLongPtrW(window, GWLP_USERDATA), message, wparam, lparam);
}

static int covers(HWND child, HWND parent)
{
	RECT own, area;
	POINT origin = {0, 0};
	GetClientRect(child, &own);
	GetClientRect(parent, &area);
	MapWindowPoints(child, parent, &origin, 1);
	return origin.x == 0 && origin.y == 0 && own.right == area.right && own.bottom == area.bottom;
}

int raven_entry(void)
{
	WNDCLASSW definition = {0};
	CLIENTCREATESTRUCT create = {NULL, 100};
	RECT before, after;
	BOOL maximized = FALSE;
	definition.hInstance = GetModuleHandleW(NULL);
	definition.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
	definition.lpfnWndProc = frame;
	definition.lpszClassName = L"raven.mdi.frame";
	if (!RegisterClassW(&definition))
		return 1;
	definition.lpfnWndProc = document;
	definition.lpszClassName = L"raven.mdi.document";
	if (!RegisterClassW(&definition))
		return 2;
	definition.lpfnWndProc = DefWindowProcW;
	definition.lpszClassName = L"raven.mdi.plain";
	if (!RegisterClassW(&definition))
		return 3;
	HWND top = CreateWindowExW(0, L"raven.mdi.frame", L"frame", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, definition.hInstance, NULL);
	HWND client = CreateWindowExW(0, L"MDICLIENT", NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 300, 200, top, NULL,
		definition.hInstance, &create);
	if (!top || !client)
		return 4;
	SetWindowLongPtrW(top, GWLP_USERDATA, (LONG_PTR)client);
	HWND child = CreateMDIWindowW(L"raven.mdi.document", L"document", WS_OVERLAPPEDWINDOW, 10, 10, 150, 100, client, definition.hInstance, 0);
	if (!child)
		return 5;
	GetWindowRect(child, &before);
	if (IsZoomed(child) || covers(child, client))
		return 6;

	ShowWindow(child, SW_SHOWMAXIMIZED);
	if (!IsZoomed(child) || last_size != SIZE_MAXIMIZED)
		return 7;
	if (!covers(child, client))
		return 8;
	if ((HWND)SendMessageW(client, WM_MDIGETACTIVE, 0, (LPARAM)&maximized) != child || !maximized)
		return 9;
	/* The client grows and its maximized document follows. */
	MoveWindow(client, 0, 0, 340, 230, TRUE);
	if (!covers(child, client) || !IsZoomed(child))
		return 10;

	ShowWindow(child, SW_RESTORE);
	GetWindowRect(child, &after);
	if (IsZoomed(child) || last_size != SIZE_RESTORED || after.left != before.left || after.top != before.top ||
		after.right != before.right || after.bottom != before.bottom)
		return 11;
	SendMessageW(client, WM_MDIGETACTIVE, 0, (LPARAM)&maximized);
	if (maximized)
		return 12;

	/* The message a frame sends does the same as ShowWindow. */
	SendMessageW(client, WM_MDIMAXIMIZE, (WPARAM)child, 0);
	if (!IsZoomed(child) || !covers(child, client))
		return 13;

	/* A child that is no document. */
	HWND plain = CreateWindowExW(0, L"raven.mdi.plain", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 5, 5, 50, 40, top, NULL, definition.hInstance, NULL);
	if (!plain)
		return 14;
	ShowWindow(plain, SW_MAXIMIZE);
	if (!IsZoomed(plain) || !covers(plain, top))
		return 15;
	DestroyWindow(top);
	return 0;
}
