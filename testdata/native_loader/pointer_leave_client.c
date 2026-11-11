#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HWND window;
static int result=1;

static HWND under_cursor(void)
{
	POINT at;
	return GetCursorPos(&at) ? WindowFromPoint(at) : (HWND)-1;
}

static LRESULT CALLBACK procedure(HWND handle,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_MOUSEMOVE && result==1) {
		TRACKMOUSEEVENT track={sizeof track,TME_LEAVE,handle,0};
		result=under_cursor()!=handle ? 10 : !TrackMouseEvent(&track) ? 11 : 2;
		return 0;
	}
	if (message==WM_MOUSELEAVE) {
		// The pointer is over another program's window now: none of this program's is under the cursor, as a tooltip checks.
		result=result!=2 ? 12 : under_cursor() ? 13 : 0;
		return 0;
	}
	if (message==WM_CLOSE) { PostQuitMessage(0); return 0; }
	return DefWindowProcW(handle,message,wparam,lparam);
}

// The pointer leaving every window of the program for another program's: WM_MOUSELEAVE, and the point under the cursor is not the
// program's any more.
int raven_entry(void)
{
	WNDCLASSW definition={0};
	MSG message;
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.lpszClassName=L"raven.pointer.leave";
	if (!RegisterClassW(&definition)) return 3;
	window=CreateWindowExW(0,definition.lpszClassName,L"leave",WS_POPUP|WS_VISIBLE,0,0,64,32,NULL,NULL,definition.hInstance,NULL);
	if (!window) return 4;
	while (GetMessageW(&message,NULL,0,0)>0) DispatchMessageW(&message);
	DestroyWindow(window);
	return result;
}
