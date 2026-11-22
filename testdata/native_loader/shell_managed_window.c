#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static BOOL (WINAPI *in_destroy)(HWND);
static BOOL destroying;

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_DESTROY)
		destroying = in_destroy(window);
	return DefWindowProcW(window, message, wparam, lparam);
}

int raven_entry(void)
{
	BOOL (WINAPI *is_shell_managed)(HWND) =
		(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), MAKEINTRESOURCEA(2574));
	if (!is_shell_managed)
		return 1;
	HMODULE private_user = LoadLibraryW(L"api-ms-win-rtcore-ntuser-private-l1-1-0.dll");
	in_destroy = (void *)GetProcAddress(private_user, "IsWindowInDestroy");
	if (!in_destroy)
		return 5;
	HINSTANCE instance = GetModuleHandleW(NULL);
	WNDCLASSW window_class = {0};
	window_class.lpfnWndProc = window_proc;
	window_class.hInstance = instance;
	window_class.lpszClassName = L"Raven.Test.OrdinaryWindow";
	if (!RegisterClassW(&window_class))
		return 2;
	HWND window = CreateWindowExW(0, window_class.lpszClassName, L"", WS_POPUP,
		0, 0, 100, 100, NULL, NULL, instance, NULL);
	if (!window)
		return 3;
	const BOOL managed = is_shell_managed(window);
	if (managed || is_shell_managed(NULL))
		return 4;
	if (in_destroy(window) || in_destroy(NULL))
		return 6;
	if (!DestroyWindow(window) || !destroying || in_destroy(window))
		return 7;
	return 0;
}
