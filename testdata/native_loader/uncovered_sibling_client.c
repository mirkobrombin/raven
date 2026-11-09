#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HWND below;
static int paints;

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_PAINT && window==below) {
		PAINTSTRUCT paint;
		BeginPaint(window,&paint);
		++paints;
		EndPaint(window,&paint);
		return 0;
	}
	return DefWindowProcW(window,message,wparam,lparam);
}

static void pump(void)
{
	MSG message;
	while (PeekMessageW(&message,NULL,0,0,PM_REMOVE)) DispatchMessageW(&message);
}

// A child stacked over a sibling uncovers part of it when it hides, moves away or is destroyed: the sibling repaints what shows again,
// not only the parent.
int raven_entry(void)
{
	WNDCLASSW definition={0};
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
	definition.lpszClassName=L"raven.uncovered.sibling";
	if (!RegisterClassW(&definition)) return 1;
	HWND parent=CreateWindowExW(0,definition.lpszClassName,L"p",WS_POPUP|WS_VISIBLE|WS_CLIPCHILDREN,0,0,200,200,NULL,NULL,definition.hInstance,NULL);
	below=CreateWindowExW(0,definition.lpszClassName,NULL,WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,0,0,100,100,parent,NULL,definition.hInstance,NULL);
	HWND above=CreateWindowExW(0,definition.lpszClassName,NULL,WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,50,50,100,100,parent,NULL,definition.hInstance,NULL);
	if (!parent || !below || !above) return 2;
	SetWindowPos(above,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
	pump();
	paints=0;
	ShowWindow(above,SW_HIDE);
	pump();
	if (paints!=1) return 3;
	ShowWindow(above,SW_SHOW);
	pump();
	paints=0;
	SetWindowPos(above,NULL,120,120,0,0,SWP_NOSIZE|SWP_NOZORDER);
	pump();
	if (paints!=1) return 4;
	SetWindowPos(above,NULL,50,50,0,0,SWP_NOSIZE|SWP_NOZORDER);
	pump();
	paints=0;
	DestroyWindow(above);
	pump();
	if (paints!=1) return 5;
	return 0;
}
