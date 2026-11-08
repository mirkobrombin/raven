#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// A shortcut menu whose first item opens a submenu: a click on that item leaves the submenu open, and a click inside the submenu
// chooses its command.
int raven_entry(void)
{
	WNDCLASSW definition={0};
	MSG message;
	definition.lpfnWndProc=DefWindowProcW;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.hbrBackground=(HBRUSH)GetStockObject(WHITE_BRUSH);
	definition.lpszClassName=L"raven.menu.track";
	if (!RegisterClassW(&definition)) return 1;
	HWND window=CreateWindowExW(0,definition.lpszClassName,L"menu track",WS_POPUP|WS_VISIBLE,0,0,200,150,NULL,NULL,definition.hInstance,NULL);
	HMENU deep=CreatePopupMenu(),popup=CreatePopupMenu();
	if (!window || !deep || !popup || !AppendMenuW(deep,MF_STRING,700,L"Deep") || !AppendMenuW(popup,MF_POPUP,(UINT_PTR)deep,L"More") ||
		!AppendMenuW(popup,MF_STRING,701,L"Other"))
		return 2;
	UpdateWindow(window);
	const DWORD end=GetTickCount()+300;
	while (GetTickCount()<end) {
		while (PeekMessageW(&message,NULL,0,0,PM_REMOVE))
			DispatchMessageW(&message);
		Sleep(20);
	}
	const UINT chosen=TrackPopupMenu(popup,TPM_RETURNCMD,10,10,0,window,NULL);
	DestroyMenu(popup);
	DestroyWindow(window);
	return chosen==700 ? 0 : chosen==0 ? 3 : 4;
}
