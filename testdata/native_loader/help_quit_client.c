// A program closing tells WinHelp it is done with help, as every MFC frame window does on WM_CLOSE: with no help open that succeeds, in
// either character set. The exit code names the first check that failed.
#include <windows.h>

int raven_entry(void)
{
	HWND owner = CreateWindowExW(0, L"STATIC", L"owner", WS_POPUP, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
	if (!owner)
		return 1;
	if (!WinHelpW(owner, NULL, HELP_QUIT, 0))
		return 2;
	if (!WinHelpA(owner, NULL, HELP_QUIT, 0))
		return 3;
	DestroyWindow(owner);
	return 0;
}
