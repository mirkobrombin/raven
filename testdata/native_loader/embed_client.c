// A window shown inside a window of another program on the host, as a plugin's editor shows in its host's: the X11 window's id comes as
// the command line's last word, in hex. The window is a popup of 200 by 100 pixels painted red; it stays until the host's program closes
// it or five seconds pass. Returns 0 when the window could be embedded, 10 for a missing id, 20 when the runtime refused it.

#include <windows.h>

__declspec(dllimport) BOOL WINAPI RavenEmbedWindow(HWND window, UINT64 host);

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_PAINT) {
		PAINTSTRUCT paint;
		const HDC context = BeginPaint(window, &paint);
		const HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
		RECT area;
		GetClientRect(window, &area);
		FillRect(context, &area, red);
		DeleteObject(red);
		EndPaint(window, &paint);
		return 0;
	}
	if (message == WM_TIMER || message == WM_DESTROY) {
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

DWORD raven_entry(void)
{
	const wchar_t *line = GetCommandLineW(), *word = line;
	for (const wchar_t *at = line; *at; ++at)
		if (*at == L' ' && at[1] && at[1] != L' ')
			word = at + 1;
	UINT64 host = 0;
	for (; *word && *word != L' '; ++word) {
		const wchar_t digit = *word;
		const int value = digit >= L'0' && digit <= L'9' ? digit - L'0' : digit >= L'a' && digit <= L'f' ? digit - L'a' + 10 :
			digit >= L'A' && digit <= L'F' ? digit - L'A' + 10 : -1;
		if (value < 0)
			return 10;
		host = host * 16 + (UINT64)value;
	}
	if (!host)
		return 10;
	WNDCLASSW type = {0};
	type.lpfnWndProc = procedure;
	type.hInstance = GetModuleHandleW(NULL);
	type.lpszClassName = L"RavenEmbedded";
	RegisterClassW(&type);
	const HWND window = CreateWindowExW(0, type.lpszClassName, L"Embedded", WS_POPUP, 0, 0, 200, 100, NULL, NULL, type.hInstance, NULL);
	if (!window || !RavenEmbedWindow(window, host))
		return 20;
	ShowWindow(window, SW_SHOW);
	UpdateWindow(window);
	SetTimer(window, 1, 5000, NULL);
	MSG message;
	while (GetMessageW(&message, NULL, 0, 0) > 0)
		DispatchMessageW(&message);
	return 0;
}
