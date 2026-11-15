#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// A window and a tool window it owns, as a program's floating palette. The palette is moved, then its owner is, with nothing drawn in
// either: the host shows the palette each time where it now lies from its owner.
static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	return DefWindowProcW(window,message,wparam,lparam);
}

static void wait(DWORD milliseconds)
{
	const DWORD end=GetTickCount()+milliseconds;
	MSG message;
	while ((LONG)(end-GetTickCount())>0) {
		while (PeekMessageW(&message,NULL,0,0,PM_REMOVE)) DispatchMessageW(&message);
		Sleep(10);
	}
}

int raven_entry(void)
{
	WNDCLASSW definition={0};
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
	definition.lpszClassName=L"raven.owned.place";
	if (!RegisterClassW(&definition)) return 1;
	HWND owner=CreateWindowExW(0,definition.lpszClassName,L"owner",WS_POPUP,100,100,300,200,NULL,NULL,definition.hInstance,NULL);
	if (!owner) return 2;
	ShowWindow(owner,SW_SHOWNORMAL);
	HWND palette=CreateWindowExW(WS_EX_TOOLWINDOW,definition.lpszClassName,L"palette",WS_POPUP,150,160,60,40,owner,NULL,definition.hInstance,NULL);
	if (!palette) return 3;
	ShowWindow(palette,SW_SHOWNOACTIVATE);
	wait(500);
	if (!SetWindowPos(palette,NULL,180,200,0,0,SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) return 4;
	wait(500);
	if (!SetWindowPos(owner,NULL,60,70,0,0,SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) return 5;
	wait(500);
	return 0;
}
