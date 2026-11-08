#include <windows.h>
#include <uxtheme.h>

/* Dark variants of visual styles as programs ask for them: a window given a "DarkMode_" application name hears that its theme changed,
   and the theme data its controls open then gives light text on dark parts, as a class list with that prefix does; taking the name
   away gives the light palette back. The dark mode functions uxtheme.dll publishes by number only: a window allowed dark mode and given an
   application name takes the dark variant once the app mode forces dark, as the light system alone does not. The task dialog, window,
   tooltip, menu and explorer bar parts that programs paint dialog and menu bodies with draw over what was there before. */

#define TMT_TEXTCOLOR 3803
#define BP_PUSHBUTTON 1
#define PBS_NORMAL 1
#define TMT_FILLCOLOR 3802
#define TDLG_PRIMARYPANEL 1
#define TDLG_SECONDARYPANEL 8
#define WP_DIALOG 29
#define TTP_STANDARD 1
#define MENU_POPUPBACKGROUND 9
#define MENU_POPUPITEM 14
#define MPI_HOT 2
#define MPI_DISABLED 3
#define EBP_HEADERBACKGROUND 1
#define RP_BACKGROUND 6

#define SURFACE RGB(0xf3, 0xf3, 0xf3)
#define DIVIDER RGB(0xe0, 0xe0, 0xe0)

typedef BOOL (WINAPI *allow_window_function)(HWND, BOOL);
typedef BOOL (WINAPI *window_query_function)(HWND);
typedef int (WINAPI *app_mode_function)(int);
typedef BOOL (WINAPI *query_function)(void);

static int theme_changes;

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_THEMECHANGED)
		++theme_changes;
	return DefWindowProcW(window, message, wparam, lparam);
}

static COLORREF text_of(HWND window, const WCHAR *classes)
{
	HTHEME theme = OpenThemeData(window, classes);
	COLORREF color = CLR_INVALID;

	if (theme && GetThemeColor(theme, BP_PUSHBUTTON, PBS_NORMAL, TMT_TEXTCOLOR, &color) != S_OK)
		color = CLR_INVALID;
	if (theme)
		CloseThemeData(theme);
	return color;
}

/* The color at (x, y) of a part in a state drawn over a 40x20 black bitmap; CLR_INVALID if the class cannot be opened or drawn. */
static COLORREF drawn_in(const WCHAR *klass, int part, int state, int x, int y)
{
	BITMAPINFOHEADER header = {sizeof header, 40, -20, 1, 32, BI_RGB};
	RECT whole = {0, 0, 40, 20};
	void *bits = NULL;
	COLORREF color = CLR_INVALID;
	HTHEME theme = OpenThemeData(NULL, klass);
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP bitmap = CreateDIBSection(dc, (BITMAPINFO *)&header, DIB_RGB_COLORS, &bits, NULL, 0);

	if (theme && bitmap && bits) {
		volatile DWORD *pixels = bits; /* volatile: no memset call, which this program has no runtime for */
		for (int at = 0; at < 40 * 20; ++at)
			pixels[at] = 0;
		SelectObject(dc, bitmap);
		if (DrawThemeBackground(theme, dc, part, state, &whole, NULL) == S_OK) {
			const BYTE *pixel = (const BYTE *)bits + (y * 40 + x) * 4;
			color = RGB(pixel[2], pixel[1], pixel[0]);
		}
	}
	if (bitmap)
		DeleteObject(bitmap);
	DeleteDC(dc);
	if (theme)
		CloseThemeData(theme);
	return color;
}

static COLORREF drawn(const WCHAR *klass, int part, int x, int y)
{
	return drawn_in(klass, part, 0, x, y);
}

static int parts(void)
{
	HTHEME theme;
	COLORREF fill = CLR_INVALID;

	if (drawn(L"TaskDialog", TDLG_PRIMARYPANEL, 20, 10) != RGB(255, 255, 255))
		return 20;
	if (drawn(L"TaskDialog", TDLG_SECONDARYPANEL, 20, 10) != SURFACE || drawn(L"TaskDialog", TDLG_SECONDARYPANEL, 20, 0) != DIVIDER)
		return 21;
	if (drawn(L"Window", WP_DIALOG, 20, 10) != SURFACE || drawn(L"ExplorerBar", EBP_HEADERBACKGROUND, 20, 10) != SURFACE)
		return 22;
	/* A tooltip and a popup menu are white with rounded corners, so a corner keeps what was there. */
	if (drawn(L"Tooltip", TTP_STANDARD, 20, 10) != RGB(255, 255, 255) || drawn(L"Tooltip", TTP_STANDARD, 0, 0) != RGB(0, 0, 0))
		return 23;
	if (drawn(L"Menu", MENU_POPUPBACKGROUND, 20, 10) != RGB(255, 255, 255))
		return 24;
	/* A hot popup item is highlighted; a disabled one is not, though its state number is the one a pushed menu bar item has. */
	if (drawn_in(L"Menu", MENU_POPUPITEM, MPI_HOT, 20, 10) == RGB(0, 0, 0) || drawn_in(L"Menu", MENU_POPUPITEM, MPI_DISABLED, 20, 10) != RGB(0, 0, 0))
		return 29;
	/* A rebar's background, which MFC control bars draw behind their toolbars, is the window surface. */
	if (drawn(L"Rebar", RP_BACKGROUND, 20, 10) != SURFACE)
		return 30;
	theme = OpenThemeData(NULL, L"Tooltip");
	if (!theme || !IsThemeBackgroundPartiallyTransparent(theme, TTP_STANDARD, 0))
		return 25;
	CloseThemeData(theme);
	theme = OpenThemeData(NULL, L"Window");
	if (!theme || IsThemeBackgroundPartiallyTransparent(theme, WP_DIALOG, 0) ||
		GetThemeColor(theme, WP_DIALOG, 0, TMT_FILLCOLOR, &fill) != S_OK || fill != SURFACE)
		return 26;
	CloseThemeData(theme);
	theme = OpenThemeData(NULL, L"TaskDialog");
	if (!theme || IsThemeBackgroundPartiallyTransparent(theme, TDLG_PRIMARYPANEL, 0) ||
		GetThemeColor(theme, TDLG_SECONDARYPANEL, 0, TMT_FILLCOLOR, &fill) != S_OK || fill != SURFACE ||
		GetThemeColor(theme, TDLG_PRIMARYPANEL, 0, TMT_FILLCOLOR, &fill) != S_OK || fill != RGB(255, 255, 255))
		return 27;
	CloseThemeData(theme);
	return 0;
}

static int dark_mode(HWND window, COLORREF light)
{
	HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
	allow_window_function allow = (allow_window_function)(void *)GetProcAddress(uxtheme, MAKEINTRESOURCEA(133));
	window_query_function allowed = (window_query_function)(void *)GetProcAddress(uxtheme, MAKEINTRESOURCEA(137));
	app_mode_function app_mode = (app_mode_function)(void *)GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
	query_function apps_dark = (query_function)(void *)GetProcAddress(uxtheme, MAKEINTRESOURCEA(132));
	query_function app_allowed = (query_function)(void *)GetProcAddress(uxtheme, MAKEINTRESOURCEA(139));

	if (!allow || !allowed || !app_mode || !apps_dark || !app_allowed || !GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)))
		return 10;
	if (!GetProcAddress(uxtheme, "OpenThemeDataForDpi"))
		return 16;
	if (allow(NULL, TRUE) || !allow(window, TRUE) || !allowed(window) || apps_dark())
		return 11;
	if (SetWindowTheme(window, L"Explorer", NULL) != S_OK || text_of(window, L"Button") != light)
		return 12;
	if (app_mode(2) != 0 || !app_allowed() || text_of(window, L"Button") != RGB(255, 255, 255))
		return 13;
	if (!allow(window, FALSE) || allowed(window) || text_of(window, L"Button") != light)
		return 14;
	if (app_mode(0) != 2 || app_allowed())
		return 15;
	return 0;
}

int raven_entry(void)
{
	WNDCLASSW definition = {0};
	HWND window;
	COLORREF light;
	int result;

	definition.lpfnWndProc = procedure;
	definition.lpszClassName = L"ThemeProbe";
	if (!RegisterClassW(&definition))
		return 1;
	window = CreateWindowExW(0, L"ThemeProbe", L"theme", WS_POPUP, 0, 0, 40, 20, NULL, NULL, NULL, NULL);
	if (!window)
		return 2;
	light = text_of(window, L"Button");
	if (light == CLR_INVALID || light == RGB(255, 255, 255))
		return 3;
	if ((result = parts()))
		return result;
	if (SetWindowTheme(window, L"DarkMode_Explorer", NULL) != S_OK || theme_changes != 1)
		return 4;
	if (text_of(window, L"Button") != RGB(255, 255, 255))
		return 5;
	if (text_of(NULL, L"DarkMode_Explorer::Button") != RGB(255, 255, 255) || text_of(NULL, L"Explorer::Button") != light)
		return 6;
	if (SetWindowTheme(window, NULL, NULL) != S_OK || theme_changes != 2 || text_of(window, L"Button") != light)
		return 7;
	result = dark_mode(window, light);
	DestroyWindow(window);
	return result;
}
