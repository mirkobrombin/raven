// The two controls of 7-Zip's file panel. Its list inserts each item with LPSTR_TEXTCALLBACK and never sets a subitem: the list asks
// its parent for every column, so the sizes and dates show. Its address box is a ComboBoxEx whose text it sets with SetWindowText,
// which goes to the box's edit control. The exit code names the first check that failed.
#include <windows.h>
#include <commctrl.h>

static int asked_column = -1;

static LRESULT CALLBACK parent_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_NOTIFY && ((NMHDR *)lparam)->code == LVN_GETDISPINFOW) {
		NMLVDISPINFOW *info = (NMLVDISPINFOW *)lparam;
		if (info->item.mask & LVIF_TEXT) {
			asked_column = info->item.iSubItem;
			lstrcpynW(info->item.pszText, info->item.iSubItem ? L"1234" : L"a.txt", info->item.cchTextMax);
		}
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

static int same(const WCHAR *left, const WCHAR *right)
{
	return lstrcmpW(left, right) == 0;
}

int raven_entry(void)
{
	INITCOMMONCONTROLSEX classes = {sizeof classes, ICC_LISTVIEW_CLASSES | ICC_USEREX_CLASSES};
	InitCommonControlsEx(&classes);
	WNDCLASSW type = {0};
	type.lpfnWndProc = parent_procedure;
	type.lpszClassName = L"FilePanel";
	if (!RegisterClassW(&type))
		return 1;
	HWND panel = CreateWindowExW(0, L"FilePanel", L"file panel", WS_OVERLAPPEDWINDOW, 0, 0, 600, 400, 0, 0, 0, 0);
	HWND list = CreateWindowExW(0, WC_LISTVIEWW, NULL, WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_EDITLABELS, 0, 30, 600, 370,
		panel, NULL, NULL, NULL);
	HWND address = CreateWindowExW(0, WC_COMBOBOXEXW, NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | CBS_AUTOHSCROLL, 0, 0, 600, 200, panel,
		NULL, NULL, NULL);
	if (!panel || !list || !address)
		return 2;

	LVCOLUMNW column = {LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM};
	column.cx = 100;
	column.pszText = L"Name";
	SendMessageW(list, LVM_INSERTCOLUMNW, 0, (LPARAM)&column);
	column.pszText = L"Size";
	column.iSubItem = 1;
	SendMessageW(list, LVM_INSERTCOLUMNW, 1, (LPARAM)&column);
	LVITEMW item = {LVIF_TEXT};
	item.pszText = LPSTR_TEXTCALLBACKW;
	if (SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item) != 0)
		return 3;
	WCHAR text[64] = {0};
	LVITEMW query = {0};
	query.iSubItem = 1;
	query.pszText = text;
	query.cchTextMax = 64;
	SendMessageW(list, LVM_GETITEMTEXTW, 0, (LPARAM)&query);
	if (asked_column != 1 || !same(text, L"1234"))
		return 4;

	SetWindowTextW(address, L"C:\\Users\\Public\\");
	HWND edit = (HWND)SendMessageW(address, CBEM_GETEDITCONTROL, 0, 0);
	if (!edit)
		return 5;
	text[0] = 0;
	GetWindowTextW(edit, text, 64);
	if (!same(text, L"C:\\Users\\Public\\"))
		return 6;
	// Then it sets the picture of the edit item alone, which leaves the text.
	COMBOBOXEXITEMW picture = {CBEIF_IMAGE | CBEIF_SELECTEDIMAGE};
	picture.iItem = -1;
	SendMessageW(address, CBEM_SETITEMW, 0, (LPARAM)&picture);
	text[0] = 0;
	if (GetWindowTextW(address, text, 64) != 16 || !same(text, L"C:\\Users\\Public\\"))
		return 7;
	DestroyWindow(panel);
	return 0;
}
