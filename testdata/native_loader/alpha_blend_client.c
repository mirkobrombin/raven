// GdiAlphaBlend between two 32-bit surfaces: a premultiplied source composed by its own alpha, a constant alpha over an opaque source,
// the clip region kept, and a window-sized blend done in the time a program that redraws itself this way many times a second can
// spend on it. The exit code names the first check that failed.
#include <windows.h>

static HDC make_surface(DWORD **pixels, HBITMAP *bitmap)
{
	BITMAPINFO info = {{sizeof(info.bmiHeader), 512, -512, 1, 32, BI_RGB}};
	HDC dc = CreateCompatibleDC(NULL);
	*bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, (void **)pixels, NULL, 0) : NULL;
	if (!*bitmap || !SelectObject(dc, *bitmap))
		return NULL;
	return dc;
}

static void fill(DWORD *pixels, DWORD value)
{
	for (int index = 0; index < 512 * 512; ++index)
		pixels[index] = value;
}

static BOOL near_color(DWORD pixel, int red, int green, int blue)
{
	const int r = (pixel >> 16) & 255, g = (pixel >> 8) & 255, b = pixel & 255;
	return r >= red - 2 && r <= red + 2 && g >= green - 2 && g <= green + 2 && b >= blue - 2 && b <= blue + 2;
}

int raven_entry(void)
{
	DWORD *from = NULL, *to = NULL;
	HBITMAP from_bitmap, to_bitmap;
	HDC source = make_surface(&from, &from_bitmap), destination = make_surface(&to, &to_bitmap);
	if (!source || !destination)
		return 1;
	/* Half a red, premultiplied, over white: full red and half of the green and blue. */
	const BLENDFUNCTION by_pixel = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
	fill(from, 0x80800000);
	fill(to, 0x00ffffff);
	if (!GdiAlphaBlend(destination, 0, 0, 512, 512, source, 0, 0, 512, 512, by_pixel))
		return 2;
	if (!near_color(to[0], 255, 127, 127) || !near_color(to[511 * 512 + 511], 255, 127, 127))
		return 3;
	/* An opaque blue at a constant half over white, inside a clip only. */
	const BLENDFUNCTION by_constant = {AC_SRC_OVER, 0, 128, 0};
	HRGN clip = CreateRectRgn(10, 10, 20, 20);
	fill(from, 0x000000ff);
	fill(to, 0x00ffffff);
	if (!clip || SelectClipRgn(destination, clip) == ERROR || !GdiAlphaBlend(destination, 0, 0, 512, 512, source, 0, 0, 512, 512, by_constant))
		return 4;
	if (!near_color(to[15 * 512 + 15], 127, 127, 255) || !near_color(to[5 * 512 + 5], 255, 255, 255) || !near_color(to[20 * 512 + 15], 255, 255, 255))
		return 5;
	SelectClipRgn(destination, NULL);
	/* A source stretched to twice its size. */
	fill(to, 0x00ffffff);
	from[0] = 0x0000ff00;
	if (!GdiAlphaBlend(destination, 0, 0, 512, 512, source, 0, 0, 256, 256, by_constant))
		return 6;
	if (!near_color(to[1 * 512 + 1], 127, 255, 127) || !near_color(to[2 * 512 + 2], 127, 127, 255))
		return 7;
	/* Forty blends of the whole surface through a clip of many pieces, as a window that others cover in part has: 8 says they took
	   over 80 milliseconds. */
	HRGN pieces = CreateRectRgn(0, 0, 0, 0);
	for (int index = 0; index < 120; ++index) {
		HRGN piece = CreateRectRgn((index * 37) % 400, index * 4, (index * 37) % 400 + 100, index * 4 + 3);
		CombineRgn(pieces, pieces, piece, RGN_OR);
		DeleteObject(piece);
	}
	if (SelectClipRgn(destination, pieces) == ERROR)
		return 4;
	fill(from, 0x80800000);
	fill(to, 0x00ffffff);
	const ULONGLONG began = GetTickCount64();
	for (int round = 0; round < 40; ++round)
		if (!GdiAlphaBlend(destination, 0, 0, 512, 512, source, 0, 0, 512, 512, by_pixel))
			return 2;
	const ULONGLONG took = GetTickCount64() - began;
	if (to[0] == 0x00ffffff || !near_color(to[3 * 512 + 450], 255, 255, 255))
		return 9;
	if (took > 80)
		return 8;
	DeleteObject(pieces);
	DeleteObject(clip);
	DeleteDC(source);
	DeleteDC(destination);
	DeleteObject(from_bitmap);
	DeleteObject(to_bitmap);
	return 0;
}
