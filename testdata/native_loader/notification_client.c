#include <windows.h>
#include <shellapi.h>

/* A tray icon added with a balloon, changed through the ANSI entry point with another balloon, then removed: each call succeeds, and
 * each balloon reaches the desktop as a notification, its title and text under the program's name. */
int raven_entry(void)
{
	NOTIFYICONDATAW icon = {sizeof icon};
	NOTIFYICONDATAA narrow = {sizeof narrow};
	icon.uID = narrow.uID = 7;
	icon.uFlags = NIF_INFO | NIF_TIP;
	lstrcpyW(icon.szTip, L"Raven");
	lstrcpyW(icon.szInfoTitle, L"Backup");
	lstrcpyW(icon.szInfo, L"It's done\nall files");
	if (!Shell_NotifyIconW(NIM_ADD, &icon))
		return 1;
	narrow.uFlags = NIF_INFO;
	lstrcpyA(narrow.szInfoTitle, "Second");
	lstrcpyA(narrow.szInfo, "Plain text");
	if (!Shell_NotifyIconA(NIM_MODIFY, &narrow))
		return 2;
	icon.uVersion = NOTIFYICON_VERSION_4;
	if (!Shell_NotifyIconW(NIM_SETVERSION, &icon))
		return 3;
	if (!Shell_NotifyIconW(NIM_DELETE, &icon))
		return 4;
	if (Shell_NotifyIconW(9, &icon))
		return 5;
	return 0;
}
