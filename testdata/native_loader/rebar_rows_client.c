// A rebar breaks a row only where the least widths of its bands do not fit: 7-Zip puts its folder button and an address box that asks
// for 1000 pixels in one row, and the row shrinks the box to the bar. A bar too narrow for both least widths takes two rows. A child
// window's placement is in its parent's client coordinates, which 7-Zip adds to the toolbar's height to place what is under it.
#include <windows.h>
#include <commctrl.h>

static HWND rebar;

static int band_left(int index)
{
	RECT rect;
	return SendMessageW(rebar, RB_GETRECT, index, (LPARAM)&rect) ? rect.left : -1;
}

int raven_entry(void)
{
	INITCOMMONCONTROLSEX classes = {sizeof classes, ICC_BAR_CLASSES | ICC_COOL_CLASSES};
	InitCommonControlsEx(&classes);
	HWND main = CreateWindowExW(0, L"STATIC", L"rebar rows", WS_OVERLAPPEDWINDOW, 0, 0, 800, 400, 0, 0, 0, 0);
	rebar = CreateWindowExW(WS_EX_TOOLWINDOW, REBARCLASSNAMEW, NULL, WS_VISIBLE | WS_BORDER | WS_CHILD | WS_CLIPCHILDREN |
		WS_CLIPSIBLINGS | CCS_NODIVIDER | CCS_NOPARENTALIGN | CCS_TOP | RBS_VARHEIGHT | RBS_BANDBORDERS, 0, 0, 0, 0, main, NULL, NULL, NULL);
	HWND button = CreateWindowExW(0, L"BUTTON", L"^", WS_CHILD | WS_VISIBLE, 0, 0, 23, 22, rebar, NULL, NULL, NULL);
	HWND address = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 0, 0, 100, 22, rebar, NULL, NULL, NULL);
	if (!main || !rebar || !button || !address)
		return 1;
	REBARBANDINFOW band = {sizeof band};
	band.fMask = RBBIM_STYLE | RBBIM_CHILD | RBBIM_CHILDSIZE | RBBIM_SIZE;
	band.fStyle = RBBS_NOGRIPPER;
	band.cxMinChild = band.cx = 23;
	band.cyMinChild = band.cyChild = 22;
	band.hwndChild = button;
	SendMessageW(rebar, RB_INSERTBANDW, -1, (LPARAM)&band);
	band.cxMinChild = 30;
	band.cx = 1000;
	band.hwndChild = address;
	SendMessageW(rebar, RB_INSERTBANDW, -1, (LPARAM)&band);

	MoveWindow(rebar, 0, 0, 600, 30, TRUE);
	if (SendMessageW(rebar, RB_GETROWCOUNT, 0, 0) != 1)
		return 2;
	RECT box, bar;
	GetWindowRect(address, &box);
	GetClientRect(rebar, &bar);
	if (band_left(1) <= band_left(0) || box.right - box.left > bar.right)
		return 3;

	MoveWindow(rebar, 0, 0, 50, 30, TRUE);
	if (SendMessageW(rebar, RB_GETROWCOUNT, 0, 0) != 2)
		return 4;

	MoveWindow(rebar, 7, 9, 600, 30, TRUE);
	WINDOWPLACEMENT place = {sizeof place};
	if (!GetWindowPlacement(rebar, &place) || place.rcNormalPosition.left != 7 || place.rcNormalPosition.top != 9)
		return 5;
	place.rcNormalPosition.left = 3;
	place.rcNormalPosition.top = 4;
	place.rcNormalPosition.right = 603;
	place.rcNormalPosition.bottom = 34;
	RECT moved;
	if (!SetWindowPlacement(rebar, &place) || !GetWindowPlacement(rebar, &place) || place.rcNormalPosition.left != 3 ||
		place.rcNormalPosition.top != 4)
		return 6;
	GetWindowRect(rebar, &moved);
	POINT origin = {0, 0};
	ClientToScreen(main, &origin);
	if (moved.left != origin.x + 3 || moved.top != origin.y + 4)
		return 7;
	return 0;
}
