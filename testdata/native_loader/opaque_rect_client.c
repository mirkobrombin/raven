// ExtTextOut fills an opaque rectangle given with its edges the wrong way round as the ordered one, as MFC draws the bottom and right
// lines of a control bar's border through FillSolidRect with a negative height or width. The exit code names the first check that failed.
#include <windows.h>

enum { SIDE = 20 };

static const DWORD *pixels;

static COLORREF at(int x, int y)
{
	const DWORD value = pixels[y * SIDE + x];
	return RGB((value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff);
}

int raven_entry(void)
{
	BITMAPINFOHEADER header = {sizeof header, SIDE, -SIDE, 1, 32, BI_RGB};
	void *bits = NULL;
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP bitmap = CreateDIBSection(dc, (BITMAPINFO *)&header, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!bitmap || !bits)
		return 1;
	SelectObject(dc, bitmap);
	pixels = bits;
	const COLORREF red = RGB(255, 0, 0);
	SetBkColor(dc, red);
	/* FillSolidRect(0, 10, 20, -1): the row above y 10. */
	RECT row = {0, 10, SIDE, 9};
	if (!ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &row, NULL, 0, NULL))
		return 2;
	GdiFlush();
	if (at(5, 9) != red || at(5, 10) == red || at(5, 8) == red)
		return 3;
	/* FillSolidRect(10, 0, -1, 20): the column left of x 10. */
	RECT column = {10, 0, 9, SIDE};
	if (!ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &column, NULL, 0, NULL))
		return 4;
	GdiFlush();
	if (at(9, 2) != red || at(10, 2) == red)
		return 5;
	DeleteDC(dc);
	DeleteObject(bitmap);
	return 0;
}
