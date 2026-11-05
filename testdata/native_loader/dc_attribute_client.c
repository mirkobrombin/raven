#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Device context attributes set and read back, whether the runtime or the program answers, and the runtime drawing with them. */

int raven_entry(void)
{
	HDC dc = CreateCompatibleDC(NULL), screen = GetDC(NULL);
	/* A bitmap compatible with the screen: one made for the new memory context would be monochrome. */
	HBITMAP bitmap = CreateCompatibleBitmap(screen, 40, 20);
	ReleaseDC(NULL, screen);
	if (!dc || !bitmap || !SelectObject(dc, bitmap))
		return 1;
	for (int round = 0; round < 3; ++round) {
		if (SetBkColor(dc, RGB(1, 2, 3)) != (round ? RGB(9, 8, 7) : RGB(255, 255, 255)) || GetBkColor(dc) != RGB(1, 2, 3))
			return 2;
		if (SetTextColor(dc, RGB(4, 5, 6)) != (round ? RGB(4, 5, 6) : 0) || GetTextColor(dc) != RGB(4, 5, 6))
			return 3;
		if (SetBkMode(dc, TRANSPARENT) != (round ? OPAQUE : OPAQUE) || GetBkMode(dc) != TRANSPARENT || SetBkMode(dc, OPAQUE) != TRANSPARENT)
			return 4;
		if (SetTextAlign(dc, TA_BASELINE) != (round ? TA_TOP : TA_TOP) || GetTextAlign(dc) != TA_BASELINE || SetTextAlign(dc, TA_TOP) != TA_BASELINE)
			return 5;
		SetBkColor(dc, RGB(9, 8, 7));
	}
	SetLastError(0);
	if (SetBkColor(dc, 0x01000000) != CLR_INVALID || GetLastError() != ERROR_CALL_NOT_IMPLEMENTED || GetBkColor(dc) != RGB(9, 8, 7))
		return 6;
	SetLastError(0);
	if (SetBkMode(dc, 5) != 0 || GetLastError() != ERROR_INVALID_PARAMETER || GetBkMode(dc) != OPAQUE)
		return 7;
	/* The runtime's SaveDC and RestoreDC are seen by the program's reads. */
	int saved = SaveDC(dc);
	SetBkColor(dc, RGB(50, 60, 70));
	SetTextAlign(dc, TA_RIGHT);
	if (!saved || !RestoreDC(dc, saved) || GetBkColor(dc) != RGB(9, 8, 7) || GetTextAlign(dc) != TA_TOP)
		return 8;
	/* The runtime draws with what the program set: an opaque background fills the text cell. */
	SetBkColor(dc, RGB(200, 100, 50));
	SetBkMode(dc, OPAQUE);
	RECT cell = {0, 0, 40, 20};
	if (!ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &cell, L"", 0, NULL) || GetPixel(dc, 5, 5) != RGB(200, 100, 50))
		return 9;
	SetBkColor(dc, RGB(10, 20, 30));
	if (!ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &cell, L"", 0, NULL) || GetPixel(dc, 5, 5) != RGB(10, 20, 30))
		return 10;
	/* The stretching mode is the DC's own, and SaveDC keeps it. */
	if (GetStretchBltMode(dc) != BLACKONWHITE || SetStretchBltMode(dc, HALFTONE) != BLACKONWHITE || GetStretchBltMode(dc) != HALFTONE)
		return 13;
	saved = SaveDC(dc);
	SetStretchBltMode(dc, COLORONCOLOR);
	SetLastError(0);
	if (!RestoreDC(dc, saved) || GetStretchBltMode(dc) != HALFTONE || SetStretchBltMode(dc, 9) || GetLastError() != ERROR_INVALID_PARAMETER)
		return 14;
	if (!DeleteDC(dc))
		return 11;
	SetLastError(0);
	if (SetBkColor(dc, 0) != CLR_INVALID || GetLastError() != ERROR_INVALID_HANDLE || GetBkMode(dc) != 0)
		return 12;
	DeleteObject(bitmap);
	return 0;
}
