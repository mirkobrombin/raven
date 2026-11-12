// The text a combo box selects while it has no focus stays selected but is not shown highlighted, as Mp3tag selects "< blank >" in
// each empty field of its tag panel. The exit code names the first check that failed.
#include <windows.h>

int raven_entry(void)
{
	HWND owner = CreateWindowExW(0, L"STATIC", L"owner", WS_POPUP, 0, 0, 300, 100, NULL, NULL, NULL, NULL);
	HWND combo = owner ? CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | CBS_AUTOHSCROLL, 0, 0, 200, 120, owner,
		(HMENU)1, NULL, NULL) : NULL;
	if (!combo)
		return 1;
	COMBOBOXINFO info = {sizeof info};
	if (!GetComboBoxInfo(combo, &info) || !info.hwndItem)
		return 2;
	SetWindowTextW(combo, L"< blank >");
	SendMessageW(combo, CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
	DWORD start = 0, end = 0;
	SendMessageW(info.hwndItem, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
	if (start != 0 || end != 9)
		return 3;

	RECT client;
	GetClientRect(info.hwndItem, &client);
	BITMAPINFOHEADER header = {sizeof header, client.right, -client.bottom, 1, 32, BI_RGB};
	void *bits = NULL;
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP bitmap = CreateDIBSection(dc, (BITMAPINFO *)&header, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!bitmap || !bits)
		return 4;
	SelectObject(dc, bitmap);
	SendMessageW(info.hwndItem, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT | PRF_ERASEBKGND);
	const COLORREF highlight = GetSysColor(COLOR_HIGHLIGHT);
	const BYTE *pixel = bits;
	for (int at = 0; at < client.right * client.bottom; ++at, pixel += 4)
		if (RGB(pixel[2], pixel[1], pixel[0]) == highlight)
			return 5;
	DeleteObject(bitmap);
	DeleteDC(dc);
	DestroyWindow(owner);
	return 0;
}
