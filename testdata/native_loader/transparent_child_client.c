#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* A parent that clips its children still draws under a child with WS_EX_TRANSPARENT, while an ordinary child keeps the drawing off
 * its area. Returns 0, or the number of the first failing check. */

int raven_entry(void)
{
	HINSTANCE instance = GetModuleHandleW(NULL);
	WNDCLASSW description = {0};
	description.lpfnWndProc = DefWindowProcW;
	description.hInstance = instance;
	description.lpszClassName = L"transparent child";
	if (!RegisterClassW(&description))
		return 1;
	HWND parent = CreateWindowExW(0, L"transparent child", L"", WS_POPUP | WS_CLIPCHILDREN, 100, 100, 40, 20, NULL, NULL, instance, NULL);
	if (!parent)
		return 2;
	HWND opaque = CreateWindowExW(0, L"transparent child", L"", WS_CHILD | WS_VISIBLE, 0, 0, 20, 20, parent, NULL, instance, NULL);
	HWND transparent = CreateWindowExW(WS_EX_TRANSPARENT, L"transparent child", L"", WS_CHILD | WS_VISIBLE, 20, 0, 20, 20, parent, NULL, instance, NULL);
	if (!opaque || !transparent)
		return 3;
	ShowWindow(parent, SW_SHOWNA);
	HDC dc = GetDC(parent);
	if (!dc)
		return 4;
	HBRUSH clear = CreateSolidBrush(RGB(0, 0, 0)), red = CreateSolidBrush(RGB(200, 20, 20));
	RECT all = {0, 0, 40, 20};
	HDC whole = GetDCEx(parent, NULL, DCX_CACHE);
	FillRect(whole, &all, clear);
	ReleaseDC(parent, whole);
	FillRect(dc, &all, red);
	const COLORREF under_transparent = GetPixel(dc, 30, 10), under_opaque = GetPixel(dc, 10, 10);
	ReleaseDC(parent, dc);
	if (under_transparent != RGB(200, 20, 20))
		return 5;
	if (under_opaque == RGB(200, 20, 20))
		return 6;
	DestroyWindow(parent);
	return 0;
}
