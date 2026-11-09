#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	return DefWindowProcW(window,message,wparam,lparam);
}

static void pump(void)
{
	MSG message;
	while (PeekMessageW(&message,NULL,0,0,PM_REMOVE)) DispatchMessageW(&message);
}

static HWND open_owned(HWND owner,HINSTANCE instance)
{
	HWND owned=CreateWindowExW(WS_EX_DLGMODALFRAME,L"raven.owner.activation",L"owned",WS_POPUP|WS_CAPTION|WS_SYSMENU,50,50,150,100,owner,NULL,
		instance,NULL);
	HWND button=CreateWindowExW(0,L"BUTTON",L"OK",WS_CHILD|WS_VISIBLE,10,10,50,20,owned,NULL,instance,NULL);
	EnableWindow(owner,FALSE);
	ShowWindow(owned,SW_SHOW);
	SetFocus(button);
	pump();
	EnableWindow(owner,TRUE);
	return GetActiveWindow()==owned && GetFocus()==button ? owned : NULL;
}

// An owned window that closes or hides while active hands the activation back to its owner, which takes the keyboard again.
int raven_entry(void)
{
	WNDCLASSW definition={0};
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.lpszClassName=L"raven.owner.activation";
	if (!RegisterClassW(&definition)) return 1;
	HWND owner=CreateWindowExW(0,definition.lpszClassName,L"owner",WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,0,300,200,NULL,NULL,definition.hInstance,NULL);
	if (!owner) return 2;
	pump();
	HWND owned=open_owned(owner,definition.hInstance);
	if (!owned) return 3;
	DestroyWindow(owned);
	pump();
	if (GetActiveWindow()!=owner) return 4;
	if (GetFocus()!=owner) return 5;
	owned=open_owned(owner,definition.hInstance);
	if (!owned) return 6;
	ShowWindow(owned,SW_HIDE);
	pump();
	if (GetActiveWindow()!=owner || GetFocus()!=owner) return 7;
	return 0;
}
