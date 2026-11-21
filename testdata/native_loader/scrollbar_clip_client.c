#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>

// Children with scroll bars in a 64x48 popup: one keeps its whole rectangle as client area, as Chromium's render widget window does, so
// its bars lie outside it; one leaves room for them; one lies in a smaller parent, so its bar lies outside that parent.
static LRESULT CALLBACK whole_client(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_NCCALCSIZE) return 0;
	return DefWindowProcW(window,message,wparam,lparam);
}

static LRESULT CALLBACK top_procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_DESTROY) PostQuitMessage(0);
	return DefWindowProcW(window,message,wparam,lparam);
}

int raven_entry(void)
{
	HINSTANCE instance=GetModuleHandleW(NULL);
	WNDCLASSW top={0}; top.hInstance=instance; top.lpfnWndProc=top_procedure; top.lpszClassName=L"RavenScrollClipTop";
	top.hbrBackground=CreateSolidBrush(RGB(37,91,173));
	WNDCLASSW child={0}; child.hInstance=instance; child.lpfnWndProc=whole_client; child.lpszClassName=L"RavenScrollClipWhole";
	child.hbrBackground=CreateSolidBrush(RGB(10,200,30));
	WNDCLASSW framed=child; framed.lpfnWndProc=DefWindowProcW; framed.lpszClassName=L"RavenScrollClipFramed";
	if (!RegisterClassW(&top) || !RegisterClassW(&child) || !RegisterClassW(&framed)) return 1;
	HWND window=CreateWindowExW(0,top.lpszClassName,L"Scroll clip",WS_POPUP | WS_VISIBLE,0,0,64,48,NULL,NULL,instance,NULL);
	if (!window) return 2;
	const DWORD style=WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL;
	HWND whole=CreateWindowExW(0,child.lpszClassName,L"",style,0,0,24,16,window,NULL,instance,NULL);
	HWND room=CreateWindowExW(0,framed.lpszClassName,L"",style,40,0,24,40,window,NULL,instance,NULL);
	HWND holder=CreateWindowExW(0,framed.lpszClassName,L"",WS_CHILD | WS_VISIBLE,0,34,32,14,window,NULL,instance,NULL);
	HWND wide=holder ? CreateWindowExW(0,framed.lpszClassName,L"",WS_CHILD | WS_VISIBLE | WS_VSCROLL,0,0,40,14,holder,NULL,instance,NULL) : NULL;
	if (!whole || !room || !wide) return 3;
	RECT client;
	if (!GetClientRect(whole,&client) || client.right!=24 || client.bottom!=16) return 4;
	if (!GetClientRect(room,&client) || client.right!=24-GetSystemMetrics(SM_CXVSCROLL) || client.bottom!=40-GetSystemMetrics(SM_CYHSCROLL)) return 5;
	if (!UpdateWindow(window) || !UpdateWindow(whole) || !UpdateWindow(room) || !UpdateWindow(holder) || !UpdateWindow(wide)) return 6;
	if (!SetWindowTextW(window,L"Scroll clip: ready")) return 7;
	MSG message;
	while (GetMessageW(&message,NULL,0,0)>0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	return 0;
}
