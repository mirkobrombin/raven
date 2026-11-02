#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// A window with a caption and a close button over one control, as a dialog without WS_CLIPCHILDREN is. Once both have painted, the
// pointer crossing the caption and lighting the close button changes only the frame: neither window hears WM_NCCALCSIZE, WM_ERASEBKGND
// or WM_PAINT again. The host moves the pointer, then posts WM_APP, and the counts come back as the exit code.
static unsigned frames,erases,paints,child_paints;

static LRESULT CALLBACK child_procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_PAINT) {
		PAINTSTRUCT paint;
		HDC dc=BeginPaint(window,&paint);
		FillRect(dc,&paint.rcPaint,(HBRUSH)GetStockObject(BLACK_BRUSH));
		EndPaint(window,&paint);
		++child_paints;
		return 0;
	}
	return DefWindowProcW(window,message,wparam,lparam);
}

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_NCCALCSIZE) ++frames;
	if (message==WM_ERASEBKGND) ++erases;
	if (message==WM_PAINT) {
		PAINTSTRUCT paint;
		HDC dc=BeginPaint(window,&paint);
		FillRect(dc,&paint.rcPaint,(HBRUSH)GetStockObject(WHITE_BRUSH));
		EndPaint(window,&paint);
		++paints;
		return 0;
	}
	if (message==WM_APP) {
		PostQuitMessage(frames ? 10 : erases ? 11 : paints ? 12 : child_paints ? 13 : 0);
		return 0;
	}
	return DefWindowProcW(window,message,wparam,lparam);
}

int raven_entry(void)
{
	WNDCLASSW definition={0};
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.hbrBackground=(HBRUSH)GetStockObject(WHITE_BRUSH);
	definition.lpszClassName=L"raven.frame.hover";
	if (!RegisterClassW(&definition)) return 1;
	definition.lpfnWndProc=child_procedure;
	definition.hbrBackground=NULL;
	definition.lpszClassName=L"raven.frame.hover.child";
	if (!RegisterClassW(&definition)) return 2;
	HWND window=CreateWindowExW(0,L"raven.frame.hover",L"frame hover",WS_POPUP | WS_CAPTION | WS_SYSMENU,40,50,300,200,NULL,NULL,
		definition.hInstance,NULL);
	if (!window) return 3;
	HWND child=CreateWindowExW(0,L"raven.frame.hover.child",NULL,WS_CHILD | WS_VISIBLE,20,20,100,40,window,NULL,definition.hInstance,NULL);
	if (!child) return 4;
	ShowWindow(window,SW_SHOW);
	UpdateWindow(window);
	UpdateWindow(child);
	RECT pending;
	if (GetUpdateRect(window,&pending,FALSE) || GetUpdateRect(child,&pending,FALSE) || !paints || !child_paints) return 5;
	frames=erases=paints=child_paints=0;
	MSG message;
	while (GetMessageW(&message,NULL,0,0)>0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	DestroyWindow(window);
	return (int)message.wParam;
}
