// DrawTextEx with the margins WinForms measures its menu and button text with: they are pixels, so a measured line grows by exactly their
// sum, and the drawn text starts after the left one. The exit code names the first check that failed.
#include <windows.h>

int raven_entry(void)
{
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP bitmap = dc ? CreateCompatibleBitmap(dc, 200, 40) : NULL;
	if (!bitmap)
		return 1;
	HGDIOBJ old = SelectObject(dc, bitmap);
	SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
	WCHAR text[] = L"&File";
	RECT plain = {0, 0, 0, 0}, padded = {0, 0, 0, 0};
	if (!DrawTextExW(dc, text, -1, &plain, DT_CALCRECT | DT_SINGLELINE, NULL))
		return 2;
	DRAWTEXTPARAMS margins = {sizeof margins, 0, 3, 4, 0};
	if (!DrawTextExW(dc, text, -1, &padded, DT_CALCRECT | DT_SINGLELINE, &margins))
		return 3;
	if (plain.right <= 0 || padded.right - plain.right != 7)
		return 4;

	RECT white = {0, 0, 200, 40};
	FillRect(dc, &white, GetStockObject(WHITE_BRUSH));
	RECT where = {0, 0, 200, 40};
	DrawTextExW(dc, text, -1, &where, DT_SINGLELINE | DT_NOPREFIX, &margins);
	int first = -1;
	for (int x = 0; x < 200 && first < 0; ++x)
		for (int y = 0; y < 40; ++y)
			if (GetPixel(dc, x, y) != RGB(255, 255, 255)) {
				first = x;
				break;
			}
	SelectObject(dc, old);
	DeleteObject(bitmap);
	DeleteDC(dc);
	return first >= 3 && first <= 5 ? 0 : 5;
}
