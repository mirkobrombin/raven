// A report list with full row selection whose parent gives every row its own colours through NM_CUSTOMDRAW, as WinForms does with each
// item's BackColor. The selected row of the focused list is still drawn in the highlight colour, as Windows draws it, and the other row in
// the colour the parent asked for. The exit code names the first check that failed.
#include <windows.h>
#include <commctrl.h>

#define ROW_COLOR RGB(250, 250, 250)

static LRESULT CALLBACK parent_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_NOTIFY && ((NMHDR *)lparam)->code == NM_CUSTOMDRAW) {
		NMLVCUSTOMDRAW *draw = (NMLVCUSTOMDRAW *)lparam;
		if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
			return CDRF_NOTIFYITEMDRAW;
		if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
			draw->clrText = RGB(0, 0, 0);
			draw->clrTextBk = ROW_COLOR;
			return CDRF_NEWFONT;
		}
		return CDRF_DODEFAULT;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

int raven_entry(void)
{
	INITCOMMONCONTROLSEX classes = {sizeof classes, ICC_LISTVIEW_CLASSES};
	InitCommonControlsEx(&classes);
	WNDCLASSW type = {0};
	type.lpfnWndProc = parent_procedure;
	type.lpszClassName = L"SelectionPanel";
	if (!RegisterClassW(&type))
		return 1;
	HWND panel = CreateWindowExW(0, L"SelectionPanel", L"selection", WS_POPUP | WS_VISIBLE, 0, 0, 300, 200, 0, 0, 0, 0);
	HWND list = panel ? CreateWindowExW(0, WC_LISTVIEWW, NULL, WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS, 0, 0, 300, 200, panel,
		NULL, NULL, NULL) : NULL;
	if (!list)
		return 2;
	SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
	LVCOLUMNW column = {LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM};
	column.cx = 120;
	column.pszText = L"Title";
	SendMessageW(list, LVM_INSERTCOLUMNW, 0, (LPARAM)&column);
	column.pszText = L"User Name";
	column.iSubItem = 1;
	SendMessageW(list, LVM_INSERTCOLUMNW, 1, (LPARAM)&column);
	LVITEMW item = {LVIF_TEXT};
	item.pszText = L"Raven Mail";
	SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
	item.iItem = 1;
	item.pszText = L"Other";
	SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
	LVITEMW state = {0};
	state.stateMask = state.state = LVIS_SELECTED | LVIS_FOCUSED;
	SendMessageW(list, LVM_SETITEMSTATE, 0, (LPARAM)&state);
	SetActiveWindow(panel);
	SetFocus(list);
	if (GetFocus() != list)
		return 3;
	InvalidateRect(list, NULL, TRUE);
	UpdateWindow(list);

	RECT first = {LVIR_BOUNDS}, second = {LVIR_BOUNDS};
	if (!SendMessageW(list, LVM_GETITEMRECT, 0, (LPARAM)&first) || !SendMessageW(list, LVM_GETITEMRECT, 1, (LPARAM)&second))
		return 4;
	HDC dc = GetDC(list);
	// Points of each row clear of its text: in the first column after the label, and far into the second column.
	const COLORREF selected = GetPixel(dc, 110, (first.top + first.bottom) / 2), selected_far = GetPixel(dc, 230, (first.top + first.bottom) / 2);
	const COLORREF other = GetPixel(dc, 230, (second.top + second.bottom) / 2);
	ReleaseDC(list, dc);
	DestroyWindow(panel);
	if (other != ROW_COLOR)
		return 5;
	if (selected != GetSysColor(COLOR_HIGHLIGHT))
		return 6;
	return selected_far == GetSysColor(COLOR_HIGHLIGHT) ? 0 : 7;
}
