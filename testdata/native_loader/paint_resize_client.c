#include <windows.h>

/* A window that changes its own size while it paints, as a dialog laying itself out does: the paint still ends, and the window paints
   again afterwards on its new surface. */

static unsigned paints, errors, flat_paints;

/* A control whose paints are counted: one left without any height has nothing to paint. */
static LRESULT CALLBACK flat_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_PAINT) {
		PAINTSTRUCT ps;
		BeginPaint(window, &ps);
		EndPaint(window, &ps);
		++flat_paints;
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

static void pump(void)
{
	MSG message;
	int round;
	for (round = 0; round < 20; ++round) {
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
			DispatchMessageW(&message);
		Sleep(5);
	}
}

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
	/* A control paints while it has a height, and no more once it is left with none: its update region is no part of a client area
	   that is empty, however it was invalidated. paint.net lays a label out to no height and makes a bitmap of its size when it paints. */
	WNDCLASSW flat = {0};
	flat.lpfnWndProc = flat_proc;
	flat.hInstance = instance;
	flat.lpszClassName = L"RavenPaintFlatFixture";
	if (!RegisterClassW(&flat))
		return 6;
	HWND control = CreateWindowExW(0, flat.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 4, 4, 40, 10, window, NULL, instance, NULL);
	if (!control)
		return 6;
	pump();
	if (!flat_paints)
		return 7;
	InvalidateRect(control, NULL, TRUE);
	if (!SetWindowPos(control, NULL, 0, 0, 40, 0, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
		return 8;
	flat_paints = 0;
	pump();
	if (flat_paints)
		return 9;
	RECT once = {0, 0, 40, 10};
	InvalidateRect(control, &once, TRUE);
	RedrawWindow(window, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
	pump();
	if (flat_paints)
		return 10;
	DestroyWindow(window);
	return 0;
}
