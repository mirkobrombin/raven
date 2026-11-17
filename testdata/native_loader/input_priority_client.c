#include <windows.h>

/* The thread that reads the user's input comes before threads that only compute. Windows raises the priority of a thread whose window
   takes input, and gives each thread a processor besides; a program's compiler or renderer working in the background does not slow the
   thread that follows the pointer. Here two threads count without rest while the first thread reads a mouse message and counts a fixed
   stretch, again and again: its stretches take not much longer than the same stretch alone, where three threads taking equal turns on
   one processor would make them three times as long. How the others fare depends on how many processors there are, and is not looked
   at. */

/* Each on a cache line of its own: the first thread reads `stop` at every count, and a line the counting threads keep writing would
   slow it down whatever the scheduler does. */
static volatile LONG stop __attribute__((aligned(64)));
static volatile struct {
	unsigned count __attribute__((aligned(64)));
} counted[2];

/* The counting every thread does, so that a count is the same work whichever thread did it. */
static void count_to(volatile unsigned *count, unsigned limit)
{
	while (!stop && *count < limit)
		++*count;
}

static DWORD WINAPI computing(void *which)
{
	count_to(&counted[(ULONG_PTR)which].count, 0xffffffffU);
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
	unsigned work = 2000000, before;
	LARGE_INTEGER frequency, began, ended, alone;
	int round;
	definition.lpfnWndProc = proc;
	definition.hInstance = GetModuleHandleW(NULL);
	definition.lpszClassName = L"raven.input.priority";
	if (!RegisterClassW(&definition))
		return 1;
	window = CreateWindowExW(0, definition.lpszClassName, L"input", WS_POPUP | WS_VISIBLE, 0, 0, 32, 32, NULL, NULL, definition.hInstance, NULL);
	if (!window)
		return 2;
	/* The stretch is sized to a few hundredths of a second, several turns long, and timed alone. */
	QueryPerformanceFrequency(&frequency);
	for (;;) {
		QueryPerformanceCounter(&began);
		mine = 0;
		count_to(&mine, work);
		QueryPerformanceCounter(&ended);
		if ((ended.QuadPart - began.QuadPart) * 1000 >= frequency.QuadPart * 30 || work > 0x20000000)
			break;
		work *= 2;
	}
	alone.QuadPart = ended.QuadPart - began.QuadPart;
	threads[0] = CreateThread(NULL, 0, computing, (void *)0, 0, NULL);
	threads[1] = CreateThread(NULL, 0, computing, (void *)1, 0, NULL);
	if (!threads[0] || !threads[1])
		return 3;
	Sleep(50);
	before = counted[0].count + counted[1].count;
	QueryPerformanceCounter(&began);
	for (round = 0; round < 10; ++round) {
		PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(round, 4));
		if (GetMessageW(&message, NULL, 0, 0) <= 0)
			return 4;
		DispatchMessageW(&message);
		mine = 0;
		count_to(&mine, work);
	}
	QueryPerformanceCounter(&ended);
	stop = 1;
	WaitForMultipleObjects(2, threads, TRUE, 5000);
	if (!before)
		return 5;
	if ((ended.QuadPart - began.QuadPart) * 2 >= alone.QuadPart * 10 * 5)
		return 6;
	DestroyWindow(window);
	return 0;
}
