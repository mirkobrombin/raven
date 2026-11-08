#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int result=1, seen;

static int held(int key) { return (GetKeyState(key)&0x8000)!=0; }

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if ((message==WM_KEYDOWN || message==WM_SYSKEYDOWN) && wparam=='Q') {
		++seen;
		// Left CONTROL with right ALT (AltGr): an ordinary key, and each side known apart.
		if (seen==1)
			result=message!=WM_KEYDOWN ? 10 : (lparam>>29)&1 ? 11 : !held(VK_LCONTROL) || held(VK_RCONTROL) ? 12 :
				!held(VK_RMENU) || held(VK_LMENU) ? 13 : !held(VK_MENU) || !held(VK_CONTROL) ? 14 : 0;
		// Left ALT alone: a system key with the context bit.
		if (seen==2 && !result)
			result=message!=WM_SYSKEYDOWN ? 20 : !((lparam>>29)&1) ? 21 : !held(VK_LMENU) || held(VK_RMENU) || held(VK_CONTROL) ? 22 : 0;
		return 0;
	}
	if (message==WM_CLOSE) { DestroyWindow(window); return 0; }
	if (message==WM_DESTROY) { PostQuitMessage(0); return 0; }
	return DefWindowProcW(window,message,wparam,lparam);
}

// CTRL+ALT+key arrives as WM_KEYDOWN, ALT+key as WM_SYSKEYDOWN, and GetKeyState tells the left and right modifiers apart.
int raven_entry(void)
{
	WNDCLASSW definition={0};
	MSG message;
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.lpszClassName=L"raven.modifier.keys";
	if (!RegisterClassW(&definition)) return 2;
	HWND window=CreateWindowExW(0,definition.lpszClassName,L"keys",WS_POPUP|WS_VISIBLE,0,0,32,32,NULL,NULL,definition.hInstance,NULL);
	if (!window) return 3;
	while (GetMessageW(&message,NULL,0,0)>0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	return seen==2 ? result : 30+seen;
}
