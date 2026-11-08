#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	return DefWindowProcW(window,message,wparam,lparam);
}

static LRESULT hit(HWND window,int x,int y)
{
	return SendMessageW(window,WM_NCHITTEST,0,MAKELPARAM(x,y));
}

// The edges of a sizable window size it, until it is maximized: then they answer HTBORDER, and the window can only be restored.
int raven_entry(void)
{
	WNDCLASSW definition={0};
	RECT outer;
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.lpszClassName=L"raven.maximized.edges";
	if (!RegisterClassW(&definition)) return 1;
	HWND window=CreateWindowExW(0,definition.lpszClassName,L"t",WS_OVERLAPPEDWINDOW,40,40,300,200,NULL,NULL,definition.hInstance,NULL);
	if (!window) return 2;
	ShowWindow(window,SW_SHOWNORMAL);
	GetWindowRect(window,&outer);
	if (hit(window,outer.left,(outer.top+outer.bottom)/2)!=HTLEFT) return 3;
	if (hit(window,outer.right-1,outer.bottom-1)!=HTBOTTOMRIGHT) return 4;
	ShowWindow(window,SW_SHOWMAXIMIZED);
	GetWindowRect(window,&outer);
	if (!IsZoomed(window)) return 5;
	if (hit(window,outer.left,(outer.top+outer.bottom)/2)!=HTBORDER) return 6;
	if (hit(window,outer.right-1,outer.bottom-1)!=HTBORDER) return 7;
	if (hit(window,(outer.left+outer.right)/2,outer.bottom-1)!=HTBORDER) return 8;
	return 0;
}
