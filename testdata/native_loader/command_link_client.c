// A command link asked for its ideal size at a given width answers the height that shows its whole note, wrapped to that width, and a
// task dialog makes its links that tall, as KeePass's emergency sheet question shows a note of two lines; a dialog of links alone has no
// empty bar of buttons below them. The exit code names the first check that failed.
#include <windows.h>
#include <commctrl.h>

#ifndef BS_COMMANDLINK
#define BS_COMMANDLINK 0x0000000EL
#endif
#ifndef BCM_SETNOTE
#define BCM_SETNOTE 0x1609
#endif
#ifndef BCM_GETIDEALSIZE
#define BCM_GETIDEALSIZE 0x1601
#endif

#define LONG_NOTE L"KeePass will print an emergency sheet, which you can then fill out and keep in a safe place."

static int failure;

static int height_at(HWND link, int width)
{
	SIZE size = {width, 0};
	if (!SendMessageW(link, BCM_GETIDEALSIZE, 0, (LPARAM)&size))
		return -1;
	return size.cy;
}

static HRESULT CALLBACK dialog_events(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam, LONG_PTR data)
{
	(void)wparam;
	(void)lparam;
	(void)data;
	if (message == TDN_CREATED) {
		HWND link = GetDlgItem(dialog, 100);
		RECT box;
		if (!link || !GetWindowRect(link, &box))
			failure = 4;
		else if (box.bottom - box.top < height_at(link, box.right - box.left))
			failure = 5;
		/* With only links, no bar of buttons stays empty along the bottom: the dialog ends a margin below the last link. */
		HWND last = GetDlgItem(dialog, 101);
		RECT client;
		if (!last || !GetWindowRect(last, &box) || !GetClientRect(dialog, &client))
			failure = failure ? failure : 8;
		else {
			MapWindowPoints(NULL, dialog, (POINT *)&box, 2);
			if (client.bottom - box.bottom > 20)
				failure = failure ? failure : 9;
		}
		SendMessageW(dialog, TDM_CLICK_BUTTON, 100, 0);
	}
	return S_OK;
}

int raven_entry(void)
{
	INITCOMMONCONTROLSEX classes = {sizeof classes, ICC_STANDARD_CLASSES};
	InitCommonControlsEx(&classes);
	HWND link = CreateWindowExW(0, L"BUTTON", L"Print", WS_POPUP | BS_COMMANDLINK, 0, 0, 220, 41, NULL, NULL, NULL, NULL);
	if (!link)
		return 1;
	SendMessageW(link, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), FALSE);
	SendMessageW(link, BCM_SETNOTE, 0, (LPARAM)L"Short.");
	const int short_note = height_at(link, 220);
	SendMessageW(link, BCM_SETNOTE, 0, (LPARAM)LONG_NOTE);
	const int long_note = height_at(link, 220);
	if (short_note <= 0)
		return 2;
	if (long_note <= short_note)
		return 3;
	DestroyWindow(link);

	TASKDIALOG_BUTTON buttons[] = {{100, L"Print\n" LONG_NOTE}, {101, L"Skip"}};
	TASKDIALOGCONFIG config = {sizeof config};
	config.dwFlags = TDF_USE_COMMAND_LINKS;
	config.pszWindowTitle = L"KeePass";
	config.pszMainInstruction = L"Emergency Sheet";
	config.pszContent = L"Do you want to print an emergency sheet now?";
	config.cButtons = 2;
	config.pButtons = buttons;
	config.pfCallback = dialog_events;
	int chosen = 0;
	if (TaskDialogIndirect(&config, &chosen, NULL, NULL) != S_OK)
		return 6;
	if (failure)
		return failure;
	return chosen == 100 ? 0 : 7;
}
