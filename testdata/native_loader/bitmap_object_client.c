#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* GetObject on bitmaps, whether the runtime or the program answers it: sizes, the BITMAP, a DIB section's DIBSECTION and deleted
   bitmaps. */

int raven_entry(void)
{
	HDC screen = CreateCompatibleDC(NULL);
	BITMAPINFO info = {0};
	info.bmiHeader.biSize = sizeof info.bmiHeader;
	info.bmiHeader.biWidth = 30;
	info.bmiHeader.biHeight = -20;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	void *bits = NULL;
	HBITMAP section = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, NULL, 0);
	HBITMAP plain = CreateCompatibleBitmap(screen, 17, 9);
	if (!screen || !section || !bits || !plain)
		return 1;
	for (int round = 0; round < 3; ++round) {
		BITMAP bitmap;
		if (GetObjectW(plain, 0, NULL) != sizeof bitmap || GetObjectW(plain, sizeof bitmap, &bitmap) != sizeof bitmap ||
			bitmap.bmWidth != 17 || bitmap.bmHeight != 9 || bitmap.bmPlanes != 1 || bitmap.bmBits)
			return 2;
		if (GetObjectW(section, sizeof bitmap, &bitmap) != sizeof bitmap || bitmap.bmWidth != 30 || bitmap.bmHeight != 20 ||
			bitmap.bmBitsPixel != 32 || bitmap.bmWidthBytes != 120 || bitmap.bmBits != bits)
			return 3;
		if (GetObjectA(section, sizeof bitmap, &bitmap) != sizeof bitmap || bitmap.bmBits != bits)
			return 4;
		if (GetObjectW(section, 31, &bitmap) != 0)
			return 5;
		DIBSECTION full;
		if (GetObjectW(section, sizeof full, &full) != sizeof full || full.dsBmih.biWidth != 30 || full.dsBm.bmBits != bits)
			return 6;
	}
	/* An image stretched with SRCPAINT adds to the pixels, alpha included, and does not replace them. */
	HGDIOBJ previous = SelectObject(screen, section);
	DWORD *pixels = bits;
	for (int index = 0; index < 600; ++index)
		pixels[index] = 0x00102030;
	static const DWORD opaque = 0xff000000;
	BITMAPINFO one = info;
	one.bmiHeader.biWidth = 1;
	one.bmiHeader.biHeight = 1;
	if (StretchDIBits(screen, 0, 0, 30, 20, 0, 0, 1, 1, &opaque, &one, DIB_RGB_COLORS, SRCPAINT) != 20)
		return 9;
	for (int index = 0; index < 600; ++index)
		if (pixels[index] != 0xff102030)
			return 10;
	/* SRCAND with black leaves nothing, SRCCOPY still replaces. */
	static const DWORD blue = 0x000000ff;
	if (StretchDIBits(screen, 0, 0, 30, 20, 0, 0, 1, 1, &blue, &one, DIB_RGB_COLORS, SRCAND) != 20 || pixels[0] != 0x00000030)
		return 11;
	if (StretchDIBits(screen, 0, 0, 30, 20, 0, 0, 1, 1, &opaque, &one, DIB_RGB_COLORS, SRCCOPY) != 20 || pixels[0] != 0xff000000)
		return 12;
	SelectObject(screen, previous);
	if (!DeleteObject(plain) || GetObjectW(plain, 0, NULL) != 0)
		return 7;
	if (!DeleteObject(section) || !DeleteDC(screen))
		return 8;
	return 0;
}
