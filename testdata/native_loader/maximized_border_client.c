#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// A window shown maximized that places its sizing border past the screen's edges, as Windows 10 lays out a maximized window and as
// Chromium places its own: the screen shows its client area, a fill of COLOR. A click on the screen's centre must reach the window at
// that point of the screen, and opens a menu there; once the menu closes, the window restores itself, for the host to maximize it again.
// The host closes the window; the exit code says what went wrong.
#define COLOR RGB(32,128,224)
static POINT clicked={-1,-1};

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_WINDOWPOSCHANGING && IsZoomed(window)) {
		WINDOWPOS *place=(WINDOWPOS *)lparam;
		if (!(place->flags & SWP_NOSIZE)) {
			const int border=8;
			place->x=-border; place->y=-border;
			place->cx=GetSystemMetrics(SM_CXSCREEN)+2*border; place->cy=GetSystemMetrics(SM_CYSCREEN)+2*border;
			place->flags&=~SWP_NOMOVE;
		}
	}
	if (message==WM_LBUTTONDOWN && clicked.x<0) {
		clicked.x=(short)LOWORD(lparam); clicked.y=(short)HIWORD(lparam);
		ClientToScreen(window,&clicked);
		HMENU menu=CreatePopupMenu();
		AppendMenuW(menu,MF_STRING,1,L"menu");
		TrackPopupMenu(menu,TPM_RETURNCMD,clicked.x,clicked.y,0,window,NULL);
		DestroyMenu(menu);
		ShowWindow(window,SW_RESTORE);
	}
	if (message==WM_CLOSE) {
		if (clicked.x<0) PostQuitMessage(20);
		else if (clicked.x!=GetSystemMetrics(SM_CXSCREEN)/2 || clicked.y!=GetSystemMetrics(SM_CYSCREEN)/2) PostQuitMessage(21);
		else PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(window,message,wparam,lparam);
}

int raven_entry(void)
{
	WNDCLASSW definition={0};
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.hbrBackground=CreateSolidBrush(COLOR);
	definition.lpszClassName=L"raven.maximized.border";
	if (!RegisterClassW(&definition)) return 1;
	HWND window=CreateWindowExW(0,L"raven.maximized.border",L"maximized border",WS_OVERLAPPEDWINDOW,100,100,400,300,NULL,NULL,
		definition.hInstance,NULL);
	if (!window) return 2;
	ShowWindow(window,SW_SHOWMAXIMIZED);
	UpdateWindow(window);
	RECT outer;
	if (!GetWindowRect(window,&outer) || outer.left!=-8 || outer.top!=-8) return 3;
	MSG message;
	while (GetMessageW(&message,NULL,0,0)>0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	DestroyWindow(window);
	return (int)message.wParam;
}
