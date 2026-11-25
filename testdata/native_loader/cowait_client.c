#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

/* CoWaitForMultipleHandles in a single-threaded apartment serves only what the modal loop of COM serves, as Windows documents for a wait
   without COWAIT_DISPATCH_WINDOW_MESSAGES: a message another thread sends is answered while the thread waits, but a message posted to
   the thread's window stays queued for the thread's own loop. With that flag the posted message is dispatched during the wait too. */

#ifndef COWAIT_DISPATCH_WINDOW_MESSAGES
#define COWAIT_DISPATCH_WINDOW_MESSAGES 8
#endif

static volatile LONG posted_seen, sent_seen;

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_APP + 1) {
		InterlockedIncrement(&posted_seen);
		return 0;
	}
	if (message == WM_APP + 2) {
		InterlockedIncrement(&sent_seen);
		return 42;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

struct sender {
	HWND window;
	HANDLE done;
	LRESULT answer;
};

static DWORD WINAPI send_from_thread(void *parameter)
{
	struct sender *sender = parameter;
	sender->answer = SendMessageW(sender->window, WM_APP + 2, 0, 0);
	SetEvent(sender->done);
	return 0;
}

int raven_entry(void)
{
	static WNDCLASSW kind;
	static struct sender sender;
	HANDLE never, thread;
	HWND window;
	MSG message;
	DWORD index = 0;
	if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
		return 1;
	kind.lpfnWndProc = procedure;
	kind.hInstance = GetModuleHandleW(NULL);
	kind.lpszClassName = L"RavenCoWait";
	if (!RegisterClassW(&kind))
		return 2;
	window = CreateWindowExW(0, L"RavenCoWait", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, kind.hInstance, NULL);
	never = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!window || !never)
		return 3;

	/* A posted message stays queued through the wait, which runs out its time, and the thread's own loop finds it afterwards. */
	if (!PostMessageW(window, WM_APP + 1, 0, 0))
		return 4;
	if (CoWaitForMultipleHandles(0, 200, 1, &never, &index) != RPC_S_CALLPENDING)
		return 5;
	if (posted_seen)
		return 6;
	if (!PeekMessageW(&message, window, WM_APP + 1, WM_APP + 1, PM_REMOVE))
		return 7;
	DispatchMessageW(&message);
	if (posted_seen != 1)
		return 8;

	/* A message another thread sends is answered while the thread waits for that thread: the wait would never end otherwise. */
	sender.window = window;
	sender.done = CreateEventW(NULL, TRUE, FALSE, NULL);
	thread = sender.done ? CreateThread(NULL, 0, send_from_thread, &sender, 0, NULL) : NULL;
	if (!thread)
		return 9;
	if (CoWaitForMultipleHandles(0, 5000, 1, &sender.done, &index) != S_OK || index != 0)
		return 10;
	if (sender.answer != 42 || sent_seen != 1)
		return 11;
	WaitForSingleObject(thread, 5000);
	CloseHandle(thread);
	CloseHandle(sender.done);

	/* With COWAIT_DISPATCH_WINDOW_MESSAGES the posted message is dispatched during the wait. */
	if (!PostMessageW(window, WM_APP + 1, 0, 0))
		return 12;
	if (CoWaitForMultipleHandles(COWAIT_DISPATCH_WINDOW_MESSAGES, 200, 1, &never, &index) != RPC_S_CALLPENDING)
		return 13;
	if (posted_seen != 2)
		return 14;

	CloseHandle(never);
	DestroyWindow(window);
	CoUninitialize();
	return 0;
}
