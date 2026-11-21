#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Threads that each draw into a bitmap of their own through a memory context, as programs that render thumbnails or off-screen
// buffers on worker threads do: fills, block transfers and reads of pixels. What each costs, and whether the runtime answers it beside
// the turn, is what RAVEN_CYCLES reports; the pixels each thread leaves are checked once all are done.

#define THREADS 4
#define ROUNDS 300
#define SIZE 32

static volatile LONG ready;
static HANDLE start;
static DWORD *pixels[THREADS];

static DWORD WINAPI draw(void *parameter)
{
	const int index = (int)(INT_PTR)parameter;
	BITMAPINFO info = {0};
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = SIZE;
	info.bmiHeader.biHeight = -SIZE;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	HDC target = CreateCompatibleDC(NULL), source = CreateCompatibleDC(NULL);
	void *target_bits = NULL, *source_bits = NULL;
	HBITMAP target_bitmap = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &target_bits, NULL, 0);
	HBITMAP source_bitmap = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &source_bits, NULL, 0);
	if (!target || !source || !target_bitmap || !source_bitmap)
		return 10;
	HGDIOBJ target_old = SelectObject(target, target_bitmap), source_old = SelectObject(source, source_bitmap);
	const COLORREF color = RGB(0x10 * (index + 1), 0x20, 0x40 + index);
	HBRUSH brush = CreateSolidBrush(color);
	if (!target_old || !source_old || !brush)
		return 11;
	InterlockedIncrement(&ready);
	if (WaitForSingleObject(start, 5000) != WAIT_OBJECT_0)
		return 12;
	const RECT half = {0, 0, SIZE, SIZE / 2};
	for (unsigned round = 0; round < ROUNDS; ++round) {
		// The source turns white and black by turns; its top-left quarter is copied over the target's bottom half.
		if (!PatBlt(source, 0, 0, SIZE, SIZE, round & 1 ? BLACKNESS : WHITENESS))
			return 13;
		if (!FillRect(target, &half, brush))
			return 14;
		if (!BitBlt(target, 0, SIZE / 2, SIZE / 2, SIZE / 2, source, 0, 0, SRCCOPY))
			return 15;
		if (GetPixel(target, 1, 1) != color || GetPixel(target, 1, SIZE - 1) != (round & 1 ? RGB(0, 0, 0) : RGB(0xff, 0xff, 0xff)))
			return 16;
	}
	pixels[index] = target_bits;
	const DWORD expected_top = (DWORD)GetRValue(color) << 16 | (DWORD)GetGValue(color) << 8 | GetBValue(color);
	if ((pixels[index][0] & 0xffffff) != expected_top || (pixels[index][(SIZE - 1) * SIZE] & 0xffffff) != ((ROUNDS - 1) & 1 ? 0 : 0xffffff))
		return 17;
	SelectObject(target, target_old);
	SelectObject(source, source_old);
	DeleteObject(target_bitmap);
	DeleteObject(source_bitmap);
	DeleteObject(brush);
	DeleteDC(target);
	DeleteDC(source);
	return 0;
}

int raven_entry(void)
{
	start = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!start)
		return 1;
	HANDLE threads[THREADS];
	for (int index = 0; index < THREADS; ++index)
		if (!(threads[index] = CreateThread(NULL, 0, draw, (void *)(INT_PTR)index, 0, NULL)))
			return 2;
	while (ready < THREADS) {
		DWORD code = STILL_ACTIVE;
		for (int index = 0; index < THREADS; ++index)
			if (GetExitCodeThread(threads[index], &code) && code != STILL_ACTIVE)
				return 100 + index * 20 + (int)code;
		Sleep(1);
	}
	SetEvent(start);
	if (WaitForMultipleObjects(THREADS, threads, TRUE, 60000) != WAIT_OBJECT_0)
		return 3;
	for (int index = 0; index < THREADS; ++index) {
		DWORD code = 0;
		if (!GetExitCodeThread(threads[index], &code) || code)
			return 100 + index * 20 + (int)code;
	}
	return 0;
}
