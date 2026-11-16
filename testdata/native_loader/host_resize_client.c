#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// A sizable window that paints itself red. The host gives it several sizes at once, as a user drags its edge faster than the program
// lays itself out: the window hears of the last alone. What went wrong comes back as the exit code when the host closes the window.
static unsigned sizes;
static BOOL shown;

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_SIZE && shown) ++sizes;
	if (message==WM_PAINT) {
		PAINTSTRUCT paint;
		RECT client;
		HDC dc=BeginPaint(window,&paint);
		HBRUSH red=CreateSolidBrush(RGB(255,0,0));
		GetClientRect(window,&client);
		FillRect(dc,&client,red);
		DeleteObject(red);
		EndPaint(window,&paint);
		return 0;
	}
	if (message==WM_ERASEBKGND) return 1;
	if (message==WM_CLOSE) {
		RECT client;
		GetClientRect(window,&client);
		PostQuitMessage(sizes!=1 ? 10+(int)sizes : client.right<=400 ? 0 : 3);
		return 0;
	}
	return DefWindowProcW(window,message,wparam,lparam);
}

int raven_entry(void)
{
	WNDCLASSW definition={0};
	MSG message;
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.lpszClassName=L"raven.host.resize";
	if (!RegisterClassW(&definition)) return 1;
	HWND window=CreateWindowExW(0,definition.lpszClassName,L"sized",WS_OVERLAPPEDWINDOW,50,50,500,300,NULL,NULL,definition.hInstance,NULL);
	if (!window) return 2;
	ShowWindow(window,SW_SHOWNORMAL);
	UpdateWindow(window);
	shown=TRUE;
	while (GetMessageW(&message,NULL,0,0)>0) DispatchMessageW(&message);
	return (int)message.wParam;
}
