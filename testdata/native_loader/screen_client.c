#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Screen contexts are reused while clean, but a leased context belongs to one caller. */

static HDC held[4];
static volatile LONG ready;

static DWORD WINAPI screen_worker(void *argument)
{
	const unsigned index = (unsigned)(UINT_PTR)argument;
	HFONT font = CreateFontW(-30, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Arial");
	held[index] = GetDC(NULL);
	InterlockedIncrement(&ready);
	while (InterlockedCompareExchange(&ready, 4, 4) != 4)
		Sleep(0);
	DWORD error = !held[index] || !font;
	for (unsigned other = 0; other < 4; ++other)
		if (other != index && held[index] == held[other])
			error = 1;
	if (!error && (!SelectObject(held[index], font) || GetCurrentObject(held[index], OBJ_FONT) != font))
		error = 2;
	if (held[index] && ReleaseDC(NULL, held[index]) != 1)
		error = 3;
	if (font)
		DeleteObject(font);
	return error;
}

static int screen_caps_hold(HDC dc)
{
	return GetDeviceCaps(dc, LOGPIXELSX) == 96 && GetDeviceCaps(dc, LOGPIXELSY) == 96 && GetDeviceCaps(dc, BITSPIXEL) == 32 &&
		GetDeviceCaps(dc, HORZRES) == GetSystemMetrics(SM_CXSCREEN) && GetDeviceCaps(dc, VERTRES) == GetSystemMetrics(SM_CYSCREEN) &&
		GetDeviceCaps(dc, TECHNOLOGY) == DT_RASDISPLAY;
}

int raven_entry(void)
{
	HDC first = GetDC(NULL);
	if (!first || !screen_caps_hold(first) || ReleaseDC(NULL, first) != 1)
		return 1;
	HDC again = GetDC(NULL);
	if (!again || !screen_caps_hold(again) || ReleaseDC(NULL, again) != 1)
		return 2;
	HDC third = GetDC(NULL);
	if (third != again || ReleaseDC(NULL, third) != 1)
		return 3;
	/* A context somebody holds is not handed out twice. */
	HDC held = GetDC(NULL), other = GetDC(NULL);
	if (!held || !other || held == other || !screen_caps_hold(other))
		return 4;
	if (ReleaseDC(NULL, held) != 1 || ReleaseDC(NULL, other) != 1)
		return 5;
	/* A context whose state a call changed is released by the runtime, and the next one starts clean. */
	HFONT font = CreateFontW(-30, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Arial");
	HDC changed = GetDC(NULL);
	if (!changed || !SelectObject(changed, font) || ReleaseDC(NULL, changed) != 1)
		return 6;
	HDC clean = GetDC(NULL);
	if (!clean || GetCurrentObject(clean, OBJ_FONT) == font || ReleaseDC(NULL, clean) != 1)
		return 6;
	DeleteObject(font);
	/* A memory context answers for the display it is compatible with, not for the monochrome bitmap selected into it. */
	HDC memory = CreateCompatibleDC(NULL);
	if (!memory || !screen_caps_hold(memory) || GetDeviceCaps(memory, NUMCOLORS) != -1 || !DeleteDC(memory))
		return 7;
	/* Rounds enough that a thread taking the context in the guest meets the runtime publishing another one. */
	for (unsigned round = 0; round < 256; ++round) {
		HANDLE workers[4];
		ready = 0;
		for (unsigned index = 0; index < 4; ++index) {
			workers[index] = CreateThread(NULL, 0, screen_worker, (void *)(UINT_PTR)index, 0, NULL);
			if (!workers[index])
				return 8;
		}
		if (WaitForMultipleObjects(4, workers, TRUE, 5000) != WAIT_OBJECT_0)
			return 9;
		for (unsigned index = 0; index < 4; ++index) {
			DWORD code = 0;
			if (!GetExitCodeThread(workers[index], &code) || code)
				return 10;
			CloseHandle(workers[index]);
		}
	}
	return 0;
}
