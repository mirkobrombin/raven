#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* A layered popup drawn only through UpdateLayeredWindow, as WPF draws its popups and message boxes: a 32-bit image with alpha per
   pixel whose left half is opaque magenta and whose right half is fully transparent. It lies over a larger plain window painted green,
   so the script, finding the magenta on the screen, can require the right half to show that green and not black. With the argument
   "hidden" the pixels are handed over before the window is shown, and showing it must show them. */

enum { width = 240, height = 160 };

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_PAINT) {
		PAINTSTRUCT paint;
		RECT client;
		HDC device = BeginPaint(window, &paint);
		HBRUSH green = CreateSolidBrush(RGB(0, 255, 0));
		GetClientRect(window, &client);
		FillRect(device, &client, green);
		DeleteObject(green);
		EndPaint(window, &paint);
		return 0;
	}
	return DefWindowProcA(window, message, wparam, lparam);
}

int raven_entry(void)
{
	const char *line = GetCommandLineA();
	int hidden = 0;
	for (const char *at = line; *at; ++at)
		if (at[0] == 'h' && at[1] == 'i' && at[2] == 'd' && at[3] == 'd' && at[4] == 'e' && at[5] == 'n' && !at[6])
			hidden = 1;
	WNDCLASSA definition = {0};
	definition.lpfnWndProc = procedure;
	definition.hInstance = GetModuleHandleA(NULL);
	definition.lpszClassName = "Layered";
	if (!RegisterClassA(&definition)) return 1;
	HWND under = CreateWindowExA(0, "Layered", "", WS_POPUP | WS_VISIBLE, 120, 100, width + 160, height + 120, NULL, NULL,
		definition.hInstance, NULL);
	if (!under) return 7;
	UpdateWindow(under);
	/* Its own class, which paints nothing: a window drawn through UpdateLayeredWindow shows only what it hands over. */
	definition.lpfnWndProc = DefWindowProcA;
	definition.lpszClassName = "LayeredPopup";
	if (!RegisterClassA(&definition)) return 8;
	HWND window = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOPMOST, "LayeredPopup", "", WS_POPUP, 200, 150, width, height, NULL, NULL,
		definition.hInstance, NULL);
	if (!window) return 2;
	HDC screen = GetDC(NULL), memory = CreateCompatibleDC(screen);
	if (!screen || !memory) return 3;
	BITMAPINFO format = {0};
	format.bmiHeader.biSize = sizeof format.bmiHeader;
	format.bmiHeader.biWidth = width;
	format.bmiHeader.biHeight = -height;
	format.bmiHeader.biPlanes = 1;
	format.bmiHeader.biBitCount = 32;
	format.bmiHeader.biCompression = BI_RGB;
	void *bits = NULL;
	HBITMAP image = CreateDIBSection(memory, &format, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!image || !bits) return 4;
	SelectObject(memory, image);
	/* Premultiplied BGRA, as ULW_ALPHA expects: magenta at full alpha, then nothing. */
	DWORD *pixels = bits;
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
			pixels[y * width + x] = x < width / 2 ? 0xffff00ffu : 0;
	if (!hidden)
		ShowWindow(window, SW_SHOWNOACTIVATE);
	POINT origin = {200, 150}, source = {0, 0};
	SIZE size = {width, height};
	BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
	if (!UpdateLayeredWindow(window, screen, &origin, &size, memory, &source, 0, &blend, ULW_ALPHA)) return 5;
	if (hidden)
		ShowWindow(window, SW_SHOWNOACTIVATE);
	OutputDebugStringA("Layered window updated");
	Sleep(4000);
	DeleteDC(memory);
	DeleteObject(image);
	ReleaseDC(NULL, screen);
	if (!DestroyWindow(window) || !DestroyWindow(under)) return 6;
	return 0;
}
