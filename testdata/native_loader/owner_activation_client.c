#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HWND reclaimer, field;

// Like a dialog, the reclaiming window hands the keyboard to its field whenever it is activated or given the focus.
static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (window==reclaimer && ((message==WM_ACTIVATE && LOWORD(wparam)) || message==WM_SETFOCUS)) SetFocus(field);
	return DefWindowProcW(window,message,wparam,lparam);
}

static HANDLE switched;
static HWND other;

static DWORD WINAPI switch_twice(void *parameter)
{
	SetForegroundWindow(reclaimer);
	other=CreateWindowExW(0,L"raven.owner.activation",L"other",WS_OVERLAPPEDWINDOW,320,0,200,100,NULL,NULL,(HINSTANCE)parameter,NULL);
	ShowWindow(other,SW_SHOW);
	SetEvent(switched);
	MSG message;
	while (GetMessageW(&message,NULL,0,0) > 0) DispatchMessageW(&message);
	return 0;
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
	// A tooltip shows itself with SWP_NOACTIVATE: it appears on top and the window the person types into keeps the keyboard.
	HWND tip=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,definition.lpszClassName,L"tip",WS_POPUP,0,0,10,10,NULL,NULL,definition.hInstance,
		NULL);
	if (!tip) return 8;
	SetWindowPos(tip,HWND_TOPMOST,20,20,60,20,SWP_NOACTIVATE|SWP_SHOWWINDOW);
	pump();
	if (!IsWindowVisible(tip) || GetActiveWindow()!=owner || GetFocus()!=owner) return 9;
	// Another thread activates this thread's window and then its own before this thread reads its messages. The activation that was
	// taken back reaches nobody, so the window does not take the keyboard from the window active now.
	reclaimer=CreateWindowExW(0,definition.lpszClassName,L"reclaimer",WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,220,200,100,NULL,NULL,
		definition.hInstance,NULL);
	field=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE,10,10,100,20,reclaimer,NULL,definition.hInstance,NULL);
	pump();
	switched=CreateEventW(NULL,TRUE,FALSE,NULL);
	DWORD id;
	HANDLE thread=CreateThread(NULL,0,switch_twice,definition.hInstance,0,&id);
	if (!reclaimer || !switched || !thread || WaitForSingleObject(switched,5000)!=WAIT_OBJECT_0) return 10;
	pump();
	if (GetForegroundWindow()!=other) return 11;
	PostThreadMessageW(id,WM_QUIT,0,0);
	WaitForSingleObject(thread,5000);
	return 0;
}
