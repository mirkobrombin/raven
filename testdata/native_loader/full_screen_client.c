#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// A popup that covers the screen, as a game makes for full screen, then moved to a part of it; and an overlapped window maximized with
// its caption. The host shows only the first while it covers the screen as a full-screen window. Returns 0, or the step that failed.
static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	return DefWindowProcW(window,message,wparam,lparam);
}

// Lets the windows paint and present what they show.
static void settle(void)
{
	const DWORD end=GetTickCount()+300;
	MSG message;
	while ((LONG)(end-GetTickCount())>0) {
		while (PeekMessageW(&message,NULL,0,0,PM_REMOVE)) DispatchMessageW(&message);
		MsgWaitForMultipleObjects(0,NULL,FALSE,20,QS_ALLINPUT);
	}
}

int raven_entry(void)
{
	WNDCLASSW definition={0};
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
	definition.lpszClassName=L"raven.full.screen";
	if (!RegisterClassW(&definition)) return 1;
	const int width=GetSystemMetrics(SM_CXSCREEN),height=GetSystemMetrics(SM_CYSCREEN);
	HWND full=CreateWindowExW(0,L"raven.full.screen",L"full screen",WS_POPUP,0,0,width,height,NULL,NULL,definition.hInstance,NULL);
	if (!full) return 2;
	ShowWindow(full,SW_SHOW);
	UpdateWindow(full);
	settle();
	HWND maximized=CreateWindowExW(0,L"raven.full.screen",L"maximized",WS_OVERLAPPEDWINDOW,50,50,300,200,NULL,NULL,definition.hInstance,NULL);
	if (!maximized) return 3;
	ShowWindow(maximized,SW_MAXIMIZE);
	UpdateWindow(maximized);
	settle();
	if (!SetWindowPos(full,NULL,100,100,400,300,SWP_NOZORDER | SWP_NOACTIVATE)) return 4;
	UpdateWindow(full);
	settle();
	DestroyWindow(maximized);
	DestroyWindow(full);
	return 0;
}
