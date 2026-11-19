#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HWND window;
static HANDLE started, drawn;

static LRESULT CALLBACK procedure(HWND target, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_ERASEBKGND) return 1;
	return DefWindowProcW(target, message, wparam, lparam);
}

static void fill(COLORREF color)
{
	const RECT area = {0, 0, 32, 32};
	HDC context = GetDC(window);
	HBRUSH brush = CreateSolidBrush(color);
	FillRect(context, &area, brush);
	DeleteObject(brush);
	ReleaseDC(window, context);
}

static DWORD WINAPI starter(void *unused)
{
	(void)unused;
	Sleep(400);
	SetEvent(started);
	return 0;
}

static ULONGLONG ticks(FILETIME time)
{
	return ((ULONGLONG)time.dwHighDateTime << 32) | time.dwLowDateTime;
}

// The processor time of the process so far, in units of 100 ns.
static ULONGLONG processor_time(void)
{
	FILETIME created, exited, kernel, user;
	return GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) ? ticks(kernel) + ticks(user) : 0;
}

static DWORD WINAPI worker(void *unused)
{
	(void)unused;
	fill(RGB(255, 0, 0));
	Sleep(300);
	fill(RGB(0, 0, 255));
	Sleep(300);
	fill(RGB(0, 255, 0));
	SetEvent(drawn);
	return 0;
}

// A thread draws on a window whose own thread waits for messages and takes none, as the message pump of Chromium waits while its
// compositor thread draws. The first two frames show while the wait goes on; the last one shows when the process ends at the latest.
int raven_entry(void)
{
	WNDCLASSW definition = {0};
	MSG message;
	definition.lpfnWndProc = procedure;
	definition.hInstance = GetModuleHandleW(NULL);
	definition.lpszClassName = L"raven.wait.present";
	if (!RegisterClassW(&definition)) return 1;
	window = CreateWindowExW(0, definition.lpszClassName, L"wait", WS_POPUP | WS_VISIBLE, 0, 0, 32, 32, NULL, NULL, definition.hInstance, NULL);
	started = CreateEventW(NULL, TRUE, FALSE, NULL);
	drawn = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!window || !started || !drawn) return 2;
	// The window is drawn on before it has painted, so its frame is put off for up to two seconds. A wait that does not ask for the
	// paint message sleeps meanwhile: another thread ends it after 400 ms, long before the frame is due, and the wait has used far
	// less processor time than that.
	fill(RGB(0, 0, 0));
	const DWORD before = GetTickCount();
	const ULONGLONG used = processor_time();
	if (!CreateThread(NULL, 0, starter, NULL, 0, NULL)) return 3;
	if (MsgWaitForMultipleObjects(1, &started, FALSE, 5000, QS_POSTMESSAGE) != WAIT_OBJECT_0 || GetTickCount() - before > 1500) return 5;
	if (processor_time() - used > 2000000) return 6;
	while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
	if (!CreateThread(NULL, 0, worker, NULL, 0, NULL)) return 3;
	for (;;) {
		const DWORD result = MsgWaitForMultipleObjects(1, &drawn, FALSE, 5000, QS_ALLINPUT);
		if (result == WAIT_OBJECT_0) return 0;
		if (result != WAIT_OBJECT_0 + 1) return 4;
	}
}
