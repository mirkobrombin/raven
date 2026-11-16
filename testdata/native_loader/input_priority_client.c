#include <windows.h>

/* The thread that reads the user's input comes before threads that only compute. Windows raises the priority of a thread whose window
   takes input, and gives each thread a processor besides; a program's compiler or renderer working in the background does not slow the
   thread that follows the pointer. Here two threads count without rest while the first thread reads a mouse message and counts a fixed
   stretch, again and again: of all the counting done meanwhile, the first thread's is the larger part, where three threads taking equal
   turns would leave it a third. */

static volatile LONG stop;
static volatile unsigned counted[2];

/* The counting every thread does, so that a count is the same work whichever thread did it. */
static void count_to(volatile unsigned *count, unsigned limit)
{
	while (!stop && *count < limit)
		++*count;
}

static DWORD WINAPI computing(void *which)
{
	count_to(&counted[(ULONG_PTR)which], 0xffffffffU);
	return 0;
}

static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return DefWindowProcW(window, message, wparam, lparam);
}

int raven_entry(void)
{
	WNDCLASSW definition = {0};
	HWND window;
	HANDLE threads[2];
	MSG message;
	volatile unsigned mine;
	unsigned work = 2000000, before, others;
	DWORD began;
	int round;
	definition.lpfnWndProc = proc;
	definition.hInstance = GetModuleHandleW(NULL);
	definition.lpszClassName = L"raven.input.priority";
	if (!RegisterClassW(&definition))
		return 1;
	window = CreateWindowExW(0, definition.lpszClassName, L"input", WS_POPUP | WS_VISIBLE, 0, 0, 32, 32, NULL, NULL, definition.hInstance, NULL);
	if (!window)
		return 2;
	/* The stretch is sized to a few hundredths of a second, several turns long. */
	for (;;) {
		began = GetTickCount();
		mine = 0;
		count_to(&mine, work);
		if (GetTickCount() - began >= 30 || work > 0x20000000)
			break;
		work *= 2;
	}
	threads[0] = CreateThread(NULL, 0, computing, (void *)0, 0, NULL);
	threads[1] = CreateThread(NULL, 0, computing, (void *)1, 0, NULL);
	if (!threads[0] || !threads[1])
		return 3;
	Sleep(50);
	before = counted[0] + counted[1];
	for (round = 0; round < 10; ++round) {
		PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(round, 4));
		if (GetMessageW(&message, NULL, 0, 0) <= 0)
			return 4;
		DispatchMessageW(&message);
		mine = 0;
		count_to(&mine, work);
	}
	others = counted[0] + counted[1] - before;
	stop = 1;
	WaitForMultipleObjects(2, threads, TRUE, 5000);
	if (!before)
		return 5;
	if (others / 10 >= work)
		return 6;
	DestroyWindow(window);
	return 0;
}
