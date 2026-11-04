// Text drawn with a font's escapement turns about its reference point: at 90 degrees it climbs from there, at 270 it falls.
#include <windows.h>

enum { EDGE = 200 };

struct box { int left, top, right, bottom; };

static struct box ink(const DWORD *pixels)
{
	struct box found = {EDGE, EDGE, -1, -1};
	for (int y = 0; y < EDGE; ++y)
		for (int x = 0; x < EDGE; ++x)
			if ((pixels[y * EDGE + x] & 0xffffff) != 0xffffff) {
				if (x < found.left) found.left = x;
				if (y < found.top) found.top = y;
				if (x > found.right) found.right = x;
				if (y > found.bottom) found.bottom = y;
			}
	return found;
}

static struct box draw(HDC dc, DWORD *pixels, int escapement)
{
	for (int index = 0; index < EDGE * EDGE; ++index)
		pixels[index] = 0xffffff;
	LOGFONTW definition = {0};
	definition.lfHeight = -16;
	definition.lfEscapement = escapement;
	definition.lfOrientation = escapement;
	lstrcpyW(definition.lfFaceName, L"Arial");
	HFONT font = CreateFontIndirectW(&definition);
	HGDIOBJ old = SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, 0);
	TextOutW(dc, 100, 100, L"WWWWWW", 6);
	GdiFlush();
	SelectObject(dc, old);
	DeleteObject(font);
	return ink(pixels);
}

int raven_entry(void)
{
	BITMAPINFO info = {0};
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = EDGE;
	info.bmiHeader.biHeight = -EDGE;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	void *bits = NULL;
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!dc || !bitmap || !bits)
		return 1;
	SelectObject(dc, bitmap);
	DWORD *pixels = bits;

	// Unturned, the run is wide and starts at the reference point.
	const struct box flat = draw(dc, pixels, 0);
	if (flat.right < 0 || flat.right - flat.left < 3 * (flat.bottom - flat.top) || flat.left < 98 || flat.top < 98)
		return 2;
	// A quarter turn: tall, going up from the reference point, with the glyphs' tops to its left.
	const struct box up = draw(dc, pixels, 900);
	if (up.right < 0 || up.bottom - up.top < 3 * (up.right - up.left) || up.bottom > 102 || up.left < 98)
		return 3;
	// Three quarters: tall, going down, the glyphs' tops to the right of the point.
	const struct box down = draw(dc, pixels, 2700);
	if (down.right < 0 || down.bottom - down.top < 3 * (down.right - down.left) || down.top < 98 || down.right > 102)
		return 4;
	// The run keeps its length when turned.
	const int length = flat.right - flat.left;
	if (up.bottom - up.top < length - 3 || up.bottom - up.top > length + 3)
		return 5;
	return 0;
}
