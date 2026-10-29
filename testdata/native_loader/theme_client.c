#include <windows.h>
#include <uxtheme.h>

/* Dark variants of visual styles as programs ask for them: a window given a "DarkMode_" application name hears that its theme changed,
   and the theme data its controls open then gives light text on dark parts, as a class list with that prefix does; taking the name
   away gives the light palette back. The dark mode functions uxtheme.dll publishes by number only: a window allowed dark mode and given an
   application name takes the dark variant once the app mode forces dark, as the light system alone does not. */

#define TMT_TEXTCOLOR 3803
#define BP_PUSHBUTTON 1
#define PBS_NORMAL 1

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
