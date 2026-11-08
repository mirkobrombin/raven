#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HWND left, right;
static int result=1;

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_MOUSEWHEEL) {
		const POINT at={(short)LOWORD(lparam),(short)HIWORD(lparam)};
		RECT outer;
		GetWindowRect(right,&outer);
		// The pane under the pointer scrolls, though the other one has the keyboard.
		result=window!=right ? 10 : (short)HIWORD(wparam)!=-120 ? 11 : !PtInRect(&outer,at) ? 12 : GetFocus()!=left ? 13 : 0;
		return 0;
	}
	if (message==WM_CLOSE) { PostQuitMessage(0); return 0; }
	return DefWindowProcW(window,message,wparam,lparam);
}

// The wheel goes to the window under the pointer, not to the one with the focus.
int raven_entry(void)
{
	WNDCLASSW definition={0};
	MSG message;
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.lpszClassName=L"raven.wheel.target";
	if (!RegisterClassW(&definition)) return 2;
	HWND window=CreateWindowExW(0,definition.lpszClassName,L"wheel",WS_POPUP|WS_VISIBLE,0,0,64,32,NULL,NULL,definition.hInstance,NULL);
	left=CreateWindowExW(0,definition.lpszClassName,NULL,WS_CHILD|WS_VISIBLE,0,0,32,32,window,NULL,definition.hInstance,NULL);
	right=CreateWindowExW(0,definition.lpszClassName,NULL,WS_CHILD|WS_VISIBLE,32,0,32,32,window,NULL,definition.hInstance,NULL);
	if (!window || !left || !right) return 3;
	SetFocus(left);
	while (GetMessageW(&message,NULL,0,0)>0) DispatchMessageW(&message);
	DestroyWindow(window);
	return result;
}
