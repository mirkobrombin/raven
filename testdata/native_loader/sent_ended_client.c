// A message sent to a window of another thread waits for that thread to take its messages; when the thread ends without taking it,
// the sender goes on, with 0, and the window's procedure never runs for it. The exit code names the first check that failed.
#include <windows.h>

static HWND window;
static HANDLE made, go;
static volatile LONG received, returned;
static LRESULT answer = 5;

static LRESULT CALLBACK procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_APP + 3) {
		InterlockedExchange(&received, 1);
		return 9;
	}
	return DefWindowProcW(handle, message, wparam, lparam);
}

// Makes the window and ends without taking a message once told to.
static DWORD WINAPI owner(void *parameter)
{
	(void)parameter;
	window = CreateWindowExW(0, L"RavenSentEnded", L"", WS_OVERLAPPEDWINDOW, 0, 0, 40, 40, NULL, NULL, GetModuleHandleW(NULL), NULL);
	SetEvent(made);
	WaitForSingleObject(go, INFINITE);
	return 0;
}

static DWORD WINAPI sender(void *parameter)
{
	(void)parameter;
	answer = SendMessageW(window, WM_APP + 3, 1, 2);
	InterlockedExchange(&returned, 1);
	return 0;
}

int raven_entry(void)
{
	WNDCLASSW type = {0};
	type.lpfnWndProc = procedure;
	type.hInstance = GetModuleHandleW(NULL);
	type.lpszClassName = L"RavenSentEnded";
	if (!RegisterClassW(&type))
		return 1;
	made = CreateEventW(NULL, TRUE, FALSE, NULL);
	go = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!made || !go)
		return 2;
	HANDLE owning = CreateThread(NULL, 0, owner, NULL, 0, NULL);
	if (!owning || WaitForSingleObject(made, 5000) != WAIT_OBJECT_0 || !window)
		return 3;
	HANDLE sending = CreateThread(NULL, 0, sender, NULL, 0, NULL);
	if (!sending)
		return 4;
	// The owner takes no message, so the message waits.
	Sleep(200);
	if (returned || received)
		return 5;
	SetEvent(go);
	if (WaitForSingleObject(owning, 5000) != WAIT_OBJECT_0)
		return 6;
	if (WaitForSingleObject(sending, 5000) != WAIT_OBJECT_0)
		return 7;
	if (received)
		return 8;
	if (answer)
		return 9;
	CloseHandle(sending);
	CloseHandle(owning);
	return 0;
}
