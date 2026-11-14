// Inserting a menu item past the end of a menu appends it, as Mp3tag builds its owner-drawn menus from position 0 of an empty one; so
// does inserting before a command the menu does not have. A radio group by command takes the ids in its range, wherever they are. The
// exit code names the first check that failed.
#include <windows.h>

static UINT id_at(HMENU menu, int position)
{
	return GetMenuItemID(menu, position);
}

int raven_entry(void)
{
	HMENU menu = CreatePopupMenu();
	if (!menu)
		return 1;
	if (!InsertMenuW(menu, 0, MF_BYPOSITION | MF_STRING, 10, L"first"))
		return 2;
	if (!InsertMenuW(menu, 7, MF_BYPOSITION | MF_STRING, 20, L"second"))
		return 3;
	MENUITEMINFOW item = {sizeof item};
	item.fMask = MIIM_ID | MIIM_STRING;
	item.wID = 30;
	item.dwTypeData = L"third";
	if (!InsertMenuItemW(menu, 9, TRUE, &item))
		return 4;
	if (!InsertMenuW(menu, 99, MF_BYCOMMAND | MF_STRING, 40, L"fourth"))
		return 5;
	if (GetMenuItemCount(menu) != 4)
		return 6;
	if (id_at(menu, 0) != 10 || id_at(menu, 1) != 20 || id_at(menu, 2) != 30 || id_at(menu, 3) != 40)
		return 7;
	/* A radio group by command spans the ids from first to last, which need not all be in the menu: the ids 10 to 35 here. */
	if (!CheckMenuRadioItem(menu, 10, 35, 20, MF_BYCOMMAND))
		return 8;
	if (!(GetMenuState(menu, 20, MF_BYCOMMAND) & MF_CHECKED) || GetMenuState(menu, 10, MF_BYCOMMAND) & MF_CHECKED ||
		GetMenuState(menu, 30, MF_BYCOMMAND) & MF_CHECKED)
		return 9;
	if (CheckMenuRadioItem(menu, 10, 35, 33, MF_BYCOMMAND) || GetMenuState(menu, 20, MF_BYCOMMAND) & MF_CHECKED)
		return 10;
	DestroyMenu(menu);
	return 0;
}
