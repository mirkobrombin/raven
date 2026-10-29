#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>

static HWND early;
static unsigned bad;

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	/* A window may build its children while it is still being created, before its frame is measured. */
	if (message==WM_NCCREATE && ((CREATESTRUCTW *)lparam)->lpCreateParams) {
		early=CreateWindowExW(0,L"RavenEarlyChild",L"early",WS_CHILD,5,6,20,15,window,(HMENU)7,NULL,NULL);
		if (!early) ++bad;
	}
	return DefWindowProcW(window,message,wparam,lparam);
}

int raven_entry(void)
{
	WNDCLASSW definition={0};
	definition.lpfnWndProc=procedure; definition.hInstance=GetModuleHandleW(NULL); definition.lpszClassName=L"RavenEarlyChild";
	if (!RegisterClassW(&definition)) return 1;
	HWND parent=CreateWindowExW(0,L"RavenEarlyChild",L"parent",WS_OVERLAPPEDWINDOW,100,200,300,200,NULL,NULL,definition.hInstance,(void *)1);
	if (!parent || !early || bad) return 2;
	if (GetParent(early)!=parent || !IsChild(parent,early) || GetDlgItem(parent,7)!=early) return 3;
	POINT point={0,0};
	MapWindowPoints(early,parent,&point,1);
	if (point.x!=5 || point.y!=6) return 4;
	RECT client,outer;
	if (!GetClientRect(early,&client) || client.right!=20 || client.bottom!=15 || !GetWindowRect(parent,&outer)) return 5;
	if (!DestroyWindow(parent) || IsWindow(early)) return 6;
	return 0;
}
