#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static LONG waiting_moves, rounds;

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_MOUSEMOVE) {
		// Ten times the program posts itself a message behind the move it has not taken yet, and looks at its queue again once the
		// pointer has moved on.
		while (rounds<10) {
			MSG waiting;
			while (!PeekMessageW(&waiting,window,WM_MOUSEMOVE,WM_MOUSEMOVE,PM_NOREMOVE)) Sleep(1);
			PostMessageW(window,WM_APP+1,0,0);
			Sleep(10);
			PeekMessageW(&waiting,window,WM_APP+2,WM_APP+2,PM_NOREMOVE);
			if (++rounds==10) {
				// The moves that wait now are taken at once: one, with those that came while they were being taken.
				while (PeekMessageW(&waiting,window,WM_MOUSEMOVE,WM_MOUSEMOVE,PM_REMOVE)) ++waiting_moves;
			}
		}
		return 0;
	}
	if (message==WM_CLOSE) { PostQuitMessage(0); return 0; }
	return DefWindowProcW(window,message,wparam,lparam);
}

// The pointer keeps moving over the window of a program that posts itself messages while a move waits in its queue. The waiting move
// gives way to the newer one whatever was posted behind it, as on Windows, so one move waits in the end and not one for each time
// the program looked at its queue.
int raven_entry(void)
{
	WNDCLASSW definition={0};
	MSG message;
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.lpszClassName=L"raven.move.merge";
	if (!RegisterClassW(&definition)) return 1;
	if (!CreateWindowExW(0,definition.lpszClassName,L"main",WS_POPUP|WS_VISIBLE,0,0,64,64,NULL,NULL,definition.hInstance,NULL)) return 2;
	while (GetMessageW(&message,NULL,0,0)>0) DispatchMessageW(&message);
	if (rounds!=10) return 3;
	return waiting_moves>=1 && waiting_moves<=3 ? 0 : 100+(waiting_moves>150 ? 150 : waiting_moves);
}
