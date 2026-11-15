// GDI+ draws on a memory context holding a 32 bit section within that context's clip and past its origin, whichever way the
// section's rows run. A fill over the whole section must land only where the clip lets it. The exit code names the first check that
// failed.
#include <windows.h>

typedef struct {
	UINT32 version;
	void *debug;
	BOOL no_thread;
	BOOL no_codecs;
} StartupInput;

int WINAPI GdiplusStartup(ULONG_PTR *token, const StartupInput *input, void *output);
void WINAPI GdiplusShutdown(ULONG_PTR token);
int WINAPI GdipCreateFromHDC(HDC dc, void **graphics);
int WINAPI GdipDeleteGraphics(void *graphics);
int WINAPI GdipCreateSolidFill(DWORD color, void **brush);
int WINAPI GdipDeleteBrush(void *brush);
int WINAPI GdipFillRectangleI(void *graphics, void *brush, INT x, INT y, INT width, INT height);
int WINAPI GdipCreateRegionRectI(const RECT *rect, void **region);
int WINAPI GdipCombineRegionRectI(void *region, const RECT *rect, INT mode);
int WINAPI GdipGetRegionHRgn(void *region, void *graphics, HRGN *out);
int WINAPI GdipDeleteRegion(void *region);

#define SIDE 8
#define PAPER 0x123456
#define INK 0xff0000

static DWORD *pixels;
static BOOL bottom_up;

static HDC section_context(BOOL rows_up)
{
	BITMAPINFO header = {{sizeof(BITMAPINFOHEADER), SIDE, rows_up ? SIDE : -SIDE, 1, 32, BI_RGB}};
	HDC dc = CreateCompatibleDC(NULL);
	void *bits = NULL;
	HBITMAP bitmap = dc ? CreateDIBSection(dc, &header, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
	if (!bitmap || !bits)
		return NULL;
	SelectObject(dc, bitmap);
	pixels = bits;
	bottom_up = rows_up;
	for (int i = 0; i < SIDE * SIDE; i++)
		pixels[i] = PAPER;
	return dc;
}

static DWORD at(int x, int y)
{
	return pixels[(bottom_up ? SIDE - 1 - y : y) * SIDE + x] & 0xffffff;
}

static int fill(HDC dc, INT x, INT y, INT width, INT height)
{
	void *graphics = NULL, *brush = NULL;
	if (GdipCreateFromHDC(dc, &graphics) || !graphics || GdipCreateSolidFill(0xff000000 | INK, &brush) || !brush)
		return 0;
	const int status = GdipFillRectangleI(graphics, brush, x, y, width, height);
	GdipDeleteBrush(brush);
	GdipDeleteGraphics(graphics);
	return status == 0;
}

// Ink exactly on the pixels for which inside() holds.
static int painted(BOOL (*inside)(int x, int y))
{
	for (int y = 0; y < SIDE; y++)
		for (int x = 0; x < SIDE; x++)
			if (at(x, y) != (inside(x, y) ? INK : PAPER))
				return 0;
	return 1;
}

static BOOL in_box(int x, int y)
{
	return x >= 2 && x < 6 && y >= 1 && y < 5;
}

static BOOL past_origin(int x, int y)
{
	return x >= 3 && x < 5 && y >= 1 && y < 3;
}

static BOOL in_two(int x, int y)
{
	return (x < 2 && y < 2) || (x >= 6 && y >= 6);
}

// A region given back as a handle covers what it covered, as one box or as several. GDI+ takes rectangles as x, y, width, height.
static int handed_over(void)
{
	RECT one = {1, 2, 4, 3}, other = {6, 6, 2, 2}, box;
	void *region = NULL;
	HRGN handle = NULL;
	if (GdipCreateRegionRectI(&one, &region) || !region || GdipGetRegionHRgn(region, NULL, &handle) || !handle)
		return 0;
	const int simple = GetRgnBox(handle, &box) == SIMPLEREGION && box.left == 1 && box.top == 2 && box.right == 5 && box.bottom == 5;
	DeleteObject(handle);
	if (!simple || GdipCombineRegionRectI(region, &other, 2) || GdipGetRegionHRgn(region, NULL, &handle) || !handle)
		return 0;
	const int both = GetRgnBox(handle, &box) == COMPLEXREGION && box.left == 1 && box.top == 2 && box.right == 8 && box.bottom == 8 &&
		PtInRegion(handle, 2, 3) && PtInRegion(handle, 7, 7) && !PtInRegion(handle, 5, 5);
	DeleteObject(handle);
	GdipDeleteRegion(region);
	return both;
}

int raven_entry(void)
{
	ULONG_PTR token;
	StartupInput input = {1, NULL, FALSE, FALSE};
	if (GdiplusStartup(&token, &input, NULL))
		return 1;
	for (int rows_up = 0; rows_up < 2; rows_up++) {
		const int base = rows_up * 10;
		HDC dc = section_context(rows_up);
		if (!dc)
			return base + 2;
		IntersectClipRect(dc, 2, 1, 6, 5);
		if (!fill(dc, 0, 0, SIDE, SIDE))
			return base + 3;
		if (!painted(in_box))
			return base + 4;
		DeleteDC(dc);
		if (!(dc = section_context(rows_up)))
			return base + 5;
		SetViewportOrgEx(dc, 3, 1, NULL);
		if (!fill(dc, 0, 0, 2, 2) || !painted(past_origin))
			return base + 6;
		DeleteDC(dc);
		// A clip of two separate boxes is no single rectangle.
		if (!(dc = section_context(rows_up)))
			return base + 7;
		HRGN first = CreateRectRgn(0, 0, 2, 2), second = CreateRectRgn(6, 6, SIDE, SIDE);
		CombineRgn(first, first, second, RGN_OR);
		SelectClipRgn(dc, first);
		DeleteObject(first);
		DeleteObject(second);
		if (!fill(dc, 0, 0, SIDE, SIDE) || !painted(in_two))
			return base + 8;
		DeleteDC(dc);
	}
	if (!handed_over())
		return 30;
	GdiplusShutdown(token);
	return 0;
}
