#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Menus read back by position right after they change, whether the runtime or the program answers the reads. */

static int same(const WCHAR *left, const WCHAR *right)
{
	while (*left && *left == *right)
		++left, ++right;
	return *left == *right;
}

int raven_entry(void)
{
	HMENU bar = CreateMenu(), popup = CreatePopupMenu();
	if (!bar || !popup || !AppendMenuW(popup, MF_STRING, 101, L"&Open") || !AppendMenuW(popup, MF_SEPARATOR, 0, NULL) ||
		!AppendMenuW(popup, MF_STRING, 102, L"E&xit") || !AppendMenuW(bar, MF_POPUP, (UINT_PTR)popup, L"&File") ||
		!AppendMenuW(bar, MF_STRING, 200, L"&Help"))
		return 1;
	WCHAR text[64];
	for (int round = 0; round < 3; ++round) {
		if (GetMenuItemCount(bar) != 2 || GetMenuItemCount(popup) != 3 || GetSubMenu(bar, 0) != popup || GetSubMenu(bar, 1) || GetSubMenu(bar, 7) ||
			GetSubMenu(bar, -1))
			return 2;
		if (GetMenuItemID(bar, 0) != (UINT)-1 || GetMenuItemID(bar, 1) != 200 || GetMenuItemID(popup, 2) != 102 || GetMenuItemID(popup, 9) != (UINT)-1)
			return 3;
		if (GetMenuStringW(bar, 0, NULL, 0, MF_BYPOSITION) != 5 || GetMenuStringW(bar, 0, text, 64, MF_BYPOSITION) != 5 || !same(text, L"&File"))
			return 4;
		if (GetMenuStringW(popup, 2, text, 3, MF_BYPOSITION) != 2 || !same(text, L"E&"))
			return 5;
		if (GetMenuStringW(popup, 1, text, 64, MF_BYPOSITION) != 0 || GetMenuStringW(popup, 5, text, 64, MF_BYPOSITION) != 0)
			return 6;
		if (GetMenuStringW(bar, 200, text, 64, MF_BYCOMMAND) != 5 || !same(text, L"&Help"))
			return 7;
	}
	/* Changes are seen at once. */
	if (!ModifyMenuW(popup, 0, MF_BYPOSITION | MF_STRING, 101, L"&Open file") || GetMenuStringW(popup, 0, text, 64, MF_BYPOSITION) != 10 ||
		!same(text, L"&Open file"))
		return 8;
	if (!DeleteMenu(popup, 1, MF_BYPOSITION) || GetMenuItemCount(popup) != 2 || GetMenuItemID(popup, 1) != 102)
		return 9;
	for (int index = 0; index < 50; ++index)
		if (!AppendMenuW(popup, MF_STRING, 300 + index, L"Recent file with a long enough name"))
			return 10;
	if (GetMenuItemCount(popup) != 52 || GetMenuItemID(popup, 51) != 349 || GetMenuStringW(popup, 51, text, 64, MF_BYPOSITION) != 35)
		return 11;
	/* By command: through nested submenus, in the order Windows searches, with states. */
	HMENU deep = CreatePopupMenu();
	if (!deep || !AppendMenuW(deep, MF_STRING, 700, L"Deep item") || !AppendMenuW(popup, MF_POPUP, (UINT_PTR)deep, L"&More"))
		return 14;
	for (int round = 0; round < 3; ++round) {
		if (GetMenuStringW(bar, 700, text, 64, MF_BYCOMMAND) != 9 || !same(text, L"Deep item") || GetMenuStringW(bar, 999, text, 64, MF_BYCOMMAND))
			return 15;
		if (GetMenuState(bar, 700, MF_BYCOMMAND) != 0 || GetMenuState(bar, 999, MF_BYCOMMAND) != (UINT)-1 ||
			GetMenuState(bar, 0, MF_BYPOSITION) != (MF_POPUP | (GetMenuItemCount(popup) << 8)) || GetMenuState(bar, 5, MF_BYPOSITION) != (UINT)-1)
			return 16;
		CheckMenuItem(bar, 700, MF_BYCOMMAND | (round & 1 ? MF_UNCHECKED : MF_CHECKED));
		if (!!(GetMenuState(bar, 700, MF_BYCOMMAND) & MF_CHECKED) != !(round & 1))
			return 17;
		CheckMenuItem(bar, 700, MF_BYCOMMAND | MF_UNCHECKED);
	}
	EnableMenuItem(bar, 102, MF_BYCOMMAND | MF_GRAYED);
	if ((GetMenuState(bar, 102, MF_BYCOMMAND) & MF_GRAYED) != MF_GRAYED || (GetMenuState(popup, 1, MF_BYPOSITION) & MF_GRAYED) != MF_GRAYED)
		return 18;
	/* New text for a submenu's item, as a program shows its shortcut, keeps the submenu: without MF_POPUP it is not replaced. */
	if (!ModifyMenuW(bar, 0, MF_BYPOSITION | MF_STRING, 0, L"&File\tAlt+F") || GetSubMenu(bar, 0) != popup ||
		GetMenuStringW(bar, 0, text, 64, MF_BYPOSITION) != 11 || !same(text, L"&File\tAlt+F"))
		return 19;
	HMENU other = CreatePopupMenu();
	if (!DestroyMenu(other) || GetMenuItemCount(other) != -1 || GetSubMenu(other, 0))
		return 12;
	if (!DestroyMenu(bar))
		return 13;
	return 0;
}
