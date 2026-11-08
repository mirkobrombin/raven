#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int sizes, moves, swallow;
static LPARAM last_size;

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_SIZE) { ++sizes; last_size=lparam; }
	if (message==WM_MOVE) ++moves;
	if (message==WM_WINDOWPOSCHANGED && swallow) return 0;
	return DefWindowProcW(window,message,wparam,lparam);
}

static int place(HWND window,int x,int y,int width,int height,UINT flags,int quiet)
{
	swallow=quiet;
	sizes=moves=0;
	SetWindowPos(window,NULL,x,y,width,height,flags|SWP_NOZORDER|SWP_NOACTIVATE);
	return sizes*10+moves;
}

// WM_MOVE and WM_SIZE are what DefWindowProc makes of WM_WINDOWPOSCHANGED: a window that answers it itself hears neither, and one that
// passes it on hears only what changed of its client area.
int raven_entry(void)
{
	WNDCLASSW definition={0};
	RECT client;
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.lpszClassName=L"raven.position.changed";
	if (!RegisterClassW(&definition)) return 1;
	HWND window=CreateWindowExW(0,definition.lpszClassName,L"t",WS_OVERLAPPEDWINDOW,10,10,300,200,NULL,NULL,definition.hInstance,NULL);
	HWND child=CreateWindowExW(0,definition.lpszClassName,NULL,WS_CHILD|WS_VISIBLE,5,5,50,40,window,NULL,definition.hInstance,NULL);
	if (!window || !child) return 2;
	if (place(window,20,20,320,220,0,1)!=0) return 3;
	if (place(window,30,30,330,230,0,0)!=11) return 4;
	GetClientRect(window,&client);
	if (LOWORD(last_size)!=client.right || HIWORD(last_size)!=client.bottom) return 5;
	if (place(window,0,0,340,240,SWP_NOMOVE,0)!=10) return 6;
	if (place(window,40,40,0,0,SWP_NOSIZE,0)!=1) return 7;
	if (place(child,8,8,60,50,0,1)!=0) return 8;
	if (place(child,9,9,0,0,SWP_NOSIZE,0)!=1) return 9;
	if (place(child,0,0,70,60,SWP_NOMOVE,0)!=10) return 10;
	return 0;
}
