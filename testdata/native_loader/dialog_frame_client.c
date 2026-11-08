#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// A child with a modal dialog frame and no caption, as a modeless page inside an MDI client: its border is a raised edge in the face
// colour. The title carries the system colours the frame should show, for the side that sees the presented pixels.
int raven_entry(void)
{
	WNDCLASSW definition={0};
	WCHAR title[64];
	MSG message;
	definition.lpfnWndProc=DefWindowProcW;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.hbrBackground=(HBRUSH)GetStockObject(WHITE_BRUSH);
	definition.lpszClassName=L"raven.dialog.frame";
	if (!RegisterClassW(&definition)) return 1;
	wsprintfW(title,L"%06x %06x %06x",GetSysColor(COLOR_3DLIGHT),GetSysColor(COLOR_BTNFACE),GetSysColor(COLOR_3DDKSHADOW));
	HWND window=CreateWindowExW(0,definition.lpszClassName,title,WS_POPUP|WS_VISIBLE,0,0,120,80,NULL,NULL,definition.hInstance,NULL);
#ifdef DESTROYED_FRAME
	/* Inside a client that clips its children, as an MDI client holds its pages. */
	HWND holder=CreateWindowExW(0,definition.lpszClassName,L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,0,0,120,80,window,NULL,definition.hInstance,NULL);
#else
	HWND holder=window;
#endif
	HWND child=CreateWindowExW(WS_EX_DLGMODALFRAME,definition.lpszClassName,L"",WS_CHILD|WS_VISIBLE,10,10,60,40,holder,NULL,definition.hInstance,NULL);
	RECT client;
	if (!window || !child) return 2;
	if (!GetClientRect(child,&client) || client.right!=54 || client.bottom!=34) return 3;
	UpdateWindow(window);
	for (int round=0;round<2;++round) {
		const DWORD end=GetTickCount()+600;
		while (GetTickCount()<end) {
			while (PeekMessageW(&message,NULL,0,0,PM_REMOVE))
				DispatchMessageW(&message);
			Sleep(20);
		}
#ifdef DESTROYED_FRAME
		/* Once the frame was shown, the window goes away and what was under it shows again. */
		if (!round && !DestroyWindow(child)) return 4;
#else
		break;
#endif
	}
	DestroyWindow(window);
	return 0;
}
