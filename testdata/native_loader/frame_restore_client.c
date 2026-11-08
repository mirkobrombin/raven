#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// An overlapped window the host maximizes with its maximize button and restores with the same button, which a maximized window shows
// as its restore button. The window hears SC_MAXIMIZE and then SC_RESTORE, and is back at its own rectangle, not maximized. The host
// then closes the window, and what went wrong comes back as the exit code. The presses on the frame reach the window first as
// WM_NCLBUTTONDOWN, with the hit code and a screen position on the frame, before the default procedure acts on them.
static UINT commands[8];
static unsigned command_count, frame_presses, wrong_presses;
static RECT placed;

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_SYSCOMMAND && command_count<8) commands[command_count++]=(UINT)(wparam & 0xfff0);
	if (message==WM_NCLBUTTONDOWN) {
		RECT outer;
		const POINT at={(short)LOWORD(lparam),(short)HIWORD(lparam)};
		GetWindowRect(window,&outer);
		++frame_presses;
		if ((wparam!=HTMAXBUTTON && wparam!=HTCAPTION) || !PtInRect(&outer,at)) ++wrong_presses;
	}
	if (message==WM_CLOSE) {
		RECT now;
		GetWindowRect(window,&now);
		if (command_count<1 || commands[0]!=SC_MAXIMIZE) PostQuitMessage(10);
		else if (command_count<2 || commands[1]!=SC_RESTORE) PostQuitMessage(11);
		else if (IsZoomed(window)) PostQuitMessage(12);
		else if (!EqualRect(&now,&placed)) PostQuitMessage(13);
		else if (frame_presses<3 || wrong_presses) PostQuitMessage(14);
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
	definition.hbrBackground=(HBRUSH)GetStockObject(WHITE_BRUSH);
	definition.lpszClassName=L"raven.frame.restore";
	if (!RegisterClassW(&definition)) return 1;
	HWND window=CreateWindowExW(0,L"raven.frame.restore",L"frame restore",WS_OVERLAPPEDWINDOW,100,100,400,300,NULL,NULL,definition.hInstance,NULL);
	if (!window) return 2;
	ShowWindow(window,SW_SHOW);
	UpdateWindow(window);
	if (!GetWindowRect(window,&placed)) return 3;
	MSG message;
	while (GetMessageW(&message,NULL,0,0)>0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	DestroyWindow(window);
	return (int)message.wParam;
}
