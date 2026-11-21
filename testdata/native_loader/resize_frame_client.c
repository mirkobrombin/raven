#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>

// A framed window paints its client area red once, then grows without painting the new area: its class has no background brush. Then
// it loses its frame and grows again in one step.
static int painted;

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_PAINT) {
		PAINTSTRUCT paint;
		HDC dc=BeginPaint(window,&paint);
		if (!painted) {
			RECT client;
			GetClientRect(window,&client);
			HBRUSH red=CreateSolidBrush(RGB(200,20,20));
			FillRect(dc,&client,red);
			DeleteObject(red);
			painted=1;
		}
		EndPaint(window,&paint);
		return 0;
	}
	if (message==WM_DESTROY) PostQuitMessage(0);
	return DefWindowProcW(window,message,wparam,lparam);
}

int raven_entry(void)
{
	HINSTANCE instance=GetModuleHandleW(NULL);
	WNDCLASSW definition={0}; definition.hInstance=instance; definition.lpfnWndProc=procedure; definition.lpszClassName=L"RavenResizeFrame";
	if (!RegisterClassW(&definition)) return 1;
	HWND window=CreateWindowExW(0,definition.lpszClassName,L"Grow",WS_OVERLAPPEDWINDOW | WS_VISIBLE,0,0,240,160,NULL,NULL,instance,NULL);
	RECT outer,client;
	if (!window || !UpdateWindow(window) || !painted || !GetWindowRect(window,&outer) || outer.right-outer.left!=240 || outer.bottom-outer.top!=160) return 2;
	if (!SetWindowPos(window,NULL,0,0,320,220,SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) return 3;
	POINT origin={0,0};
	if (!GetWindowRect(window,&outer) || !GetClientRect(window,&client) || !ClientToScreen(window,&origin)) return 4;
	if (outer.right-outer.left!=320 || outer.bottom-outer.top!=220) return 5;
	// The harness learns the client area's insets from the title.
	WCHAR title[64];
	const int left=origin.x-outer.left,top=origin.y-outer.top;
	wsprintfW(title,L"Grow: %d %d %d %d",left,top,320-left-client.right,220-top-client.bottom);
	if (!SetWindowTextW(window,title)) return 6;
	SetWindowLongPtrW(window,GWL_STYLE,WS_POPUP | WS_VISIBLE);
	if (!SetWindowPos(window,NULL,0,0,360,240,SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED)) return 7;
	if (!GetClientRect(window,&client) || client.right!=360 || client.bottom!=240) return 8;
	if (!SetWindowTextW(window,L"Popup")) return 9;
	MSG message;
	while (GetMessageW(&message,NULL,0,0)>0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	return 0;
}
