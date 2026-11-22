#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int raven_entry(void)
{
	BOOL (WINAPI *is_shell_managed)(HWND) =
		(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), MAKEINTRESOURCEA(2574));
	if (!is_shell_managed)
		return 1;
	HINSTANCE instance = GetModuleHandleW(NULL);
	WNDCLASSW window_class = {0};
	window_class.lpfnWndProc = DefWindowProcW;
	window_class.hInstance = instance;
	window_class.lpszClassName = L"Raven.Test.OrdinaryWindow";
	if (!RegisterClassW(&window_class))
		return 2;
	HWND window = CreateWindowExW(0, window_class.lpszClassName, L"", WS_POPUP,
		0, 0, 100, 100, NULL, NULL, instance, NULL);
	if (!window)
		return 3;
	const BOOL managed = is_shell_managed(window);
	DestroyWindow(window);
	return managed || is_shell_managed(NULL) ? 4 : 0;
}
