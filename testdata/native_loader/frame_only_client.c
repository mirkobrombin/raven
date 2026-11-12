// A child whose WM_NCCALCSIZE keeps no client area at all still hears WM_NCPAINT when it is shown, as on Windows: an empty MFC toolbar
// in Mp3tag keeps only its gripper and borders, and draws them there. The exit code names the first check that failed.
#include <windows.h>

static int frame_paints;

static LRESULT CALLBACK frame_only(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_NCCALCSIZE) {
		/* Both forms start with the rectangle that becomes the client area. */
		RECT *rect = (RECT *)lparam;
		rect->left += 9;
		rect->top += 2;
		rect->right = rect->left;
		rect->bottom = rect->top;
		return 0;
	}
	if (message == WM_NCPAINT) {
		HDC dc = GetWindowDC(window);
		RECT box;
		GetWindowRect(window, &box);
		OffsetRect(&box, -box.left, -box.top);
		FillRect(dc, &box, GetSysColorBrush(COLOR_BTNFACE));
		ReleaseDC(window, dc);
		++frame_paints;
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

int raven_entry(void)
{
	WNDCLASSW type = {0};
	type.lpfnWndProc = frame_only;
	type.lpszClassName = L"RavenFrameOnly";
	if (!RegisterClassW(&type))
		return 1;
	HWND owner = CreateWindowExW(0, L"STATIC", L"owner", WS_POPUP | WS_VISIBLE, 0, 0, 200, 100, NULL, NULL, NULL, NULL);
	HWND bar = owner ? CreateWindowExW(0, L"RavenFrameOnly", NULL, WS_CHILD | WS_VISIBLE, 2, 2, 11, 4, owner, NULL, NULL, NULL) : NULL;
	if (!bar)
		return 2;
	RECT client;
	GetClientRect(bar, &client);
	if (client.right || client.bottom)
		return 3;
	UpdateWindow(owner);
	MSG message;
	const DWORD start = GetTickCount();
	while (!frame_paints && GetTickCount() - start < 3000) {
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
			DispatchMessageW(&message);
		Sleep(10);
	}
	if (!frame_paints)
		return 4;
	DestroyWindow(owner);
	return 0;
}
