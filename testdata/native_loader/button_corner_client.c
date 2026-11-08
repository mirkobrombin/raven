#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// A themed push button over a parent that clips its children and answers WM_CTLCOLORBTN with a hollow brush, as Delphi forms do: the
// button's rounded corners show the parent's own background, read back from the window's surface.
static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_CTLCOLORBTN) return (LRESULT)GetStockObject(NULL_BRUSH);
	return DefWindowProcW(window,message,wparam,lparam);
}

int raven_entry(void)
{
	WNDCLASSW definition={0};
	MSG message;
	if (!LoadLibraryW(L"comctl32.dll")) return 1;
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.hbrBackground=CreateSolidBrush(RGB(255,0,0));
	definition.lpszClassName=L"raven.button.corner";
	if (!RegisterClassW(&definition)) return 2;
	HWND window=CreateWindowExW(0,definition.lpszClassName,L"button corner",WS_POPUP|WS_VISIBLE|WS_CLIPCHILDREN,0,0,120,80,NULL,NULL,
		definition.hInstance,NULL);
	HWND button=window ? CreateWindowExW(0,L"BUTTON",L"OK",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,10,10,80,30,window,(HMENU)1,definition.hInstance,NULL) : NULL;
	if (!button)
		return 3;
	UpdateWindow(window);
	const DWORD end=GetTickCount()+600;
	while (GetTickCount()<end) {
		while (PeekMessageW(&message,NULL,0,0,PM_REMOVE))
			DispatchMessageW(&message);
		Sleep(20);
	}
	HDC dc=GetDC(button);
	const COLORREF corner=GetPixel(dc,0,0),opposite=GetPixel(dc,79,29),middle=GetPixel(dc,40,15);
	ReleaseDC(button,dc);
	DestroyWindow(window);
	if (middle==RGB(255,0,0)) return 4;
	return corner==RGB(255,0,0) && opposite==RGB(255,0,0) ? 0 : 5;
}
