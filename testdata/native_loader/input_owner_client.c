#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HWND main_window, worker_window;
static volatile LONG worker_ready, main_busy, worker_took, moves, cursor_thread;
static DWORD worker_thread;

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_SETCURSOR && window==main_window) InterlockedExchange(&cursor_thread,(LONG)GetCurrentThreadId());
	if (message==WM_MOUSEMOVE && window==main_window) InterlockedIncrement(&moves);
	if (message==WM_CLOSE && window==main_window) { PostQuitMessage(0); return 0; }
	return DefWindowProcW(window,message,wparam,lparam);
}

static DWORD WINAPI worker(void *unused)
{
	MSG message;
	(void)unused;
	worker_window=CreateWindowExW(0,L"raven.input.owner",L"worker",WS_POPUP|WS_VISIBLE,100,0,32,32,NULL,NULL,GetModuleHandleW(NULL),NULL);
	InterlockedExchange(&worker_ready,1);
	while (!main_busy) Sleep(1);
	// Twenty messages of its own, read one by one while the pointer moves over the window of the thread that reads none.
	const DWORD began=GetTickCount();
	for (int round=0;round<20;++round) {
		PostMessageW(worker_window,WM_APP,0,0);
		if (GetMessageW(&message,NULL,0,0)<=0) return 1;
		DispatchMessageW(&message);
		Sleep(5);
	}
	InterlockedExchange(&worker_took,(LONG)(GetTickCount()-began)+1);
	while (GetMessageW(&message,NULL,0,0)>0) DispatchMessageW(&message);
	return 0;
}

// The pointer's moves over a window wait for the thread that owns it. Another thread that reads messages of its own is not held up by
// them while the owner is busy, and the owner chooses the cursor and hears the move itself once it reads its queue again.
int raven_entry(void)
{
	WNDCLASSW definition={0};
	MSG message;
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.lpszClassName=L"raven.input.owner";
	if (!RegisterClassW(&definition)) return 1;
	main_window=CreateWindowExW(0,definition.lpszClassName,L"main",WS_POPUP|WS_VISIBLE,0,0,32,32,NULL,NULL,definition.hInstance,NULL);
	if (!main_window) return 2;
	HANDLE thread=CreateThread(NULL,0,worker,NULL,0,&worker_thread);
	if (!thread) return 3;
	while (!worker_ready) Sleep(1);
	InterlockedExchange(&main_busy,1);
	Sleep(800);
	if (!worker_took) return 4;
	if (worker_took>400) return 5;
	while (GetMessageW(&message,NULL,0,0)>0) DispatchMessageW(&message);
	PostThreadMessageW(worker_thread,WM_QUIT,0,0);
	WaitForSingleObject(thread,5000);
	if (!moves) return 6;
	if ((DWORD)cursor_thread!=GetCurrentThreadId()) return 7;
	return 0;
}
