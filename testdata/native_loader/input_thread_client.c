#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HWND main_window, worker_window;
static volatile LONG worker_ready, worker_clicked, asked;
static DWORD worker_thread;

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	if (message==WM_APP) { InterlockedIncrement(&asked); return 7; }
	if (message==WM_LBUTTONDOWN && window==worker_window) InterlockedExchange(&worker_clicked,(LONG)GetCurrentThreadId());
	if (message==WM_CLOSE && window==main_window) { PostQuitMessage(0); return 0; }
	return DefWindowProcW(window,message,wparam,lparam);
}

static DWORD WINAPI worker(void *unused)
{
	MSG message;
	(void)unused;
	worker_window=CreateWindowExW(0,L"raven.input.thread",L"worker",WS_POPUP|WS_VISIBLE,100,0,32,32,NULL,NULL,GetModuleHandleW(NULL),NULL);
	InterlockedExchange(&worker_ready,1);
	// The worker asks the main thread something while input for its own window is on the way.
	while (GetMessageW(&message,NULL,0,0)>0) {
		if (message.message==WM_LBUTTONDOWN) SendMessageW(main_window,WM_APP,0,0);
		DispatchMessageW(&message);
	}
	return 0;
}

// Input for a window goes to the thread that owns it, whichever thread happens to read the host's events, and the hit test sent for
// it does not block the thread that reads them while the owner waits for that thread.
int raven_entry(void)
{
	WNDCLASSW definition={0};
	MSG message;
	definition.lpfnWndProc=procedure;
	definition.hInstance=GetModuleHandleW(NULL);
	definition.lpszClassName=L"raven.input.thread";
	if (!RegisterClassW(&definition)) return 1;
	main_window=CreateWindowExW(0,definition.lpszClassName,L"main",WS_POPUP|WS_VISIBLE,0,0,32,32,NULL,NULL,definition.hInstance,NULL);
	if (!main_window) return 2;
	HANDLE thread=CreateThread(NULL,0,worker,NULL,0,&worker_thread);
	if (!thread) return 3;
	while (!worker_ready) Sleep(1);
	while (GetMessageW(&message,NULL,0,0)>0) DispatchMessageW(&message);
	PostThreadMessageW(worker_thread,WM_QUIT,0,0);
	WaitForSingleObject(thread,5000);
	if ((DWORD)worker_clicked!=worker_thread) return 4;
	if (asked!=1) return 5;
	return 0;
}
