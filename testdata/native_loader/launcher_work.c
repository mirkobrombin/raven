#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The library of launcher_client.c: a visible owner window of the built-in STATIC class, as an updater makes before it starts a program
   through the shell. */

__declspec(dllexport) HWND WINAPI make_window(void)
{
	return CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP | WS_VISIBLE, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
