#include <windows.h>

/* A window that changes its own size while it paints, as a dialog laying itself out does: the paint still ends, and the window paints
   again afterwards on its new surface. */

static unsigned paints, errors;

static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_PAINT) {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(window, &ps);
		if (!dc) {
			++errors;
			return 0;
		}
		if (++paints == 1 && !SetWindowPos(window, NULL, 0, 0, 64, 48, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
			++errors;
		RECT all = {0, 0, 64, 48};
		FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
		if (!EndPaint(window, &ps))
			++errors;
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

int raven_entry(void)
{
	HINSTANCE instance = GetModuleHandleW(NULL);
	WNDCLASSW wc = {0};
	wc.lpfnWndProc = proc;
	wc.hInstance = instance;
	wc.lpszClassName = L"RavenPaintResizeFixture";
	if (!RegisterClassW(&wc))
		return 1;
	HWND window = CreateWindowExW(0, wc.lpszClassName, L"paint resize", WS_POPUP, 100, 100, 32, 24, NULL, NULL, instance, NULL);
	if (!window)
		return 2;
	ShowWindow(window, SW_SHOW);
	if (!UpdateWindow(window) || paints < 1 || errors)
		return 3;
	RECT size;
	if (!GetWindowRect(window, &size) || size.right - size.left != 64 || size.bottom - size.top != 48)
		return 4;
	/* The window is not left inside a paint: it paints again when asked. */
	const unsigned before = paints;
	if (!InvalidateRect(window, NULL, FALSE) || !UpdateWindow(window) || paints == before || errors)
		return 5;
	DestroyWindow(window);
	return 0;
}
