#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>

static int painted, paint_error, frames;

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_PAINT) {
		PAINTSTRUCT paint;
		RECT client;
		HDC device = BeginPaint(window, &paint);
		HBRUSH green = CreateSolidBrush(RGB(0, 255, 0)), red = CreateSolidBrush(RGB(255, 0, 0));
		if (!device || !green || !red || !GetClientRect(window, &client) || !FillRect(device, &client, green)) paint_error = 1;
		else for (unsigned corner = 0; corner < 4; ++corner) {
			const int x = corner & 1 ? client.right - 16 : 0, y = corner & 2 ? client.bottom - 16 : 0;
			RECT marker = {x, y, x + 16, y + 16};
			if (!FillRect(device, &marker, red)) paint_error = 1;
		}
		DeleteObject(green); DeleteObject(red);
		EndPaint(window, &paint);
		painted = 1;
		return 0;
	}
	return DefWindowProcA(window, message, wparam, lparam);
}

/* Children with a client edge that draw their border in WM_NCPAINT: the first keeps it, the second passes the message on afterwards and
   gets the default border over it. The script counts the blue left on the screen. */
static LRESULT CALLBACK bordered(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_NCPAINT) {
		HDC device = GetWindowDC(window);
		HBRUSH blue = CreateSolidBrush(RGB(0, 0, 255));
		RECT outer;
		if (!device || !blue || !GetWindowRect(window, &outer)) paint_error = 1;
		else {
			OffsetRect(&outer, -outer.left, -outer.top);
			if (!FrameRect(device, &outer, blue)) paint_error = 1;
		}
		DeleteObject(blue);
		ReleaseDC(window, device);
		++frames;
		if (GetDlgCtrlID(window) == 2) return DefWindowProcA(window, message, wparam, lparam);
		return 0;
	}
	if (message == WM_PAINT) {
		PAINTSTRUCT paint;
		RECT client;
		HDC device = BeginPaint(window, &paint);
		HBRUSH green = CreateSolidBrush(RGB(0, 255, 0));
		if (!device || !green || !GetClientRect(window, &client) || !FillRect(device, &client, green)) paint_error = 1;
		DeleteObject(green);
		EndPaint(window, &paint);
		return 0;
	}
	return DefWindowProcA(window, message, wparam, lparam);
}

static int number(const char **cursor)
{
	int value = 0;
	while (**cursor == ' ') ++*cursor;
	while (**cursor && **cursor != ' ') {
		if (**cursor < '0' || **cursor > '9' || value > 8192) return 0;
		value = value * 10 + *(*cursor)++ - '0';
	}
	return value;
}

static int metrics(int width, int height, HDC screen)
{
	if (GetSystemMetrics(SM_CXSCREEN) != width || GetSystemMetrics(SM_CYSCREEN) != height ||
		GetSystemMetrics(SM_CXVIRTUALSCREEN) != width || GetSystemMetrics(SM_CYVIRTUALSCREEN) != height) {
		OutputDebugStringA("Screen metric API mismatch");
		return 1;
	}
	MONITORINFO monitor = {sizeof monitor};
	if (!GetMonitorInfoW(MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor) ||
		monitor.rcMonitor.left || monitor.rcMonitor.top || monitor.rcMonitor.right != width || monitor.rcMonitor.bottom != height) return 2;
	DEVMODEW mode = {0};
	mode.dmSize = sizeof mode;
	if (!EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &mode) || mode.dmPelsWidth != (DWORD)width || mode.dmPelsHeight != (DWORD)height) return 3;
	if (GetDeviceCaps(screen, HORZRES) != width || GetDeviceCaps(screen, VERTRES) != height) return 4;
	/* At another DPI the sizes of interface parts scale and the screen does not. */
	typedef int (WINAPI *metrics_for_dpi)(int, UINT);
	metrics_for_dpi for_dpi = (metrics_for_dpi)GetProcAddress(GetModuleHandleA("user32.dll"), "GetSystemMetricsForDpi");
	if (!for_dpi || !for_dpi(SM_CXFRAME, 96) || for_dpi(SM_CXFRAME, 96) != GetSystemMetrics(SM_CXFRAME) ||
		for_dpi(SM_CYCAPTION, 192) != 2 * GetSystemMetrics(SM_CYCAPTION) || for_dpi(SM_CXSCREEN, 192) != width)
		return 5;
	if (GetSystemMetrics(SM_CXVSCROLL) <= 0 || GetSystemMetrics(SM_CYHSCROLL) <= 0 || GetSystemMetrics(SM_CXICON) != 32) return 6;
	return 0;
}

int raven_entry(void)
{
	const char *cursor = GetCommandLineA();
	if (*cursor == '"') { ++cursor; while (*cursor && *cursor != '"') ++cursor; if (*cursor) ++cursor; }
	else while (*cursor && *cursor != ' ') ++cursor;
	int width = number(&cursor), height = number(&cursor);
	/* The last number picks a variant: 1 the dark frame, 2 the bordered children. */
	const int next_width = number(&cursor), next_height = number(&cursor), variant = number(&cursor), dark = variant == 1;
	if (!width || !height) return 10;
	HDC screen = GetDC(NULL);
	if (!screen) return 11;
	int result = metrics(width, height, screen);
	if (result) return result;
	WNDCLASSA definition = {0};
	definition.lpfnWndProc = procedure;
	definition.lpszClassName = "ScreenMetrics";
	if (!RegisterClassA(&definition)) return 19;
	HWND window = CreateWindowExA(0, "ScreenMetrics", "Screen metrics", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		0, 0, 160, 120, NULL, NULL, NULL, NULL);
	if (!window) return 12;
	/* DWMWA_USE_IMMERSIVE_DARK_MODE: the script looks for a dark caption. */
	if (dark) {
		const BOOL on = TRUE;
		if (DwmSetWindowAttribute(window, 20, &on, sizeof(on)) != S_OK || DwmSetWindowAttribute(NULL, 20, &on, sizeof(on)) != E_HANDLE) return 17;
	}
	ShowWindow(window, SW_MAXIMIZE);
	if (next_width && next_height) {
		OutputDebugStringA("Screen change ready");
		const ULONGLONG deadline = GetTickCount64() + 8000;
		while (GetSystemMetrics(SM_CXSCREEN) != next_width || GetSystemMetrics(SM_CYSCREEN) != next_height) {
			MSG message;
			while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
			if (GetTickCount64() >= deadline) return 13;
			Sleep(10);
		}
		width = next_width; height = next_height;
		result = metrics(width, height, screen);
		if (result) return result + 20;
	}
	RECT outer, client;
	if (!GetWindowRect(window, &outer) || outer.left || outer.top || outer.right != width || outer.bottom != height) return 14;
	if (!GetClientRect(window, &client) || client.right <= 32 || client.bottom <= 32 || client.right > width || client.bottom > height) return 15;
	UpdateWindow(window);
	if (!painted || paint_error) return 16;
	if (variant == 2) {
		definition.lpfnWndProc = bordered;
		definition.lpszClassName = "Bordered";
		if (!RegisterClassA(&definition)) return 30;
		for (int id = 1; id <= 2; ++id) {
			HWND child = CreateWindowExA(WS_EX_CLIENTEDGE, "Bordered", "", WS_CHILD | WS_VISIBLE, 200 * id + 200, 300, 100, 40, window,
				(HMENU)(INT_PTR)id, NULL, NULL);
			if (!child) return 31;
			UpdateWindow(child);
		}
		if (frames < 2 || paint_error) return 32;
	}
	OutputDebugStringA("Screen metrics frame presented");
	Sleep(3000);
	if (!DestroyWindow(window) || !ReleaseDC(NULL, screen)) return 18;
	return 0;
}
