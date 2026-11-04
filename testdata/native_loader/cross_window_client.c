// The windows of one process are visible to another of the same prefix: a child finds its parent's window by class and title, reads
// its title and visibility, sees it in EnumWindows and learns which process owns it.
#include <windows.h>

static const wchar_t class_name[] = L"RavenCrossWindow";
static const wchar_t title[] = L"Cross window probe é";

struct search { HWND wanted; int seen; };

static BOOL CALLBACK visit(HWND window, LPARAM parameter)
{
	struct search *search = (struct search *)parameter;
	if (window == search->wanted)
		search->seen = 1;
	return TRUE;
}

static int same(const wchar_t *left, const wchar_t *right)
{
	return lstrcmpW(left, right) == 0;
}

static int child(DWORD parent)
{
	HWND window = FindWindowW(class_name, title);
	if (!window)
		return 10;
	if (FindWindowW(class_name, NULL) != window || FindWindowW(NULL, title) != window || FindWindowA("RavenCrossWindow", NULL) != window)
		return 11;
	if (FindWindowW(class_name, L"Another title"))
		return 12;
	wchar_t text[64];
	if (GetWindowTextLengthW(window) != lstrlenW(title) || GetWindowTextW(window, text, 64) != lstrlenW(title) || !same(text, title))
		return 13;
	if (GetWindowTextW(window, text, 6) != 5 || text[5] || text[0] != L'C')
		return 14;
	if (GetClassNameW(window, text, 64) != lstrlenW(class_name) || !same(text, class_name))
		return 15;
	if (!IsWindow(window) || !IsWindowVisible(window))
		return 16;
	DWORD owner = 0;
	GetWindowThreadProcessId(window, &owner);
	if (owner != parent)
		return 17;
	struct search search = {window, 0};
	if (!EnumWindows(visit, (LPARAM)&search) || !search.seen)
		return 18;
	return 0;
}

int raven_entry(void)
{
	const wchar_t *line = GetCommandLineW();
	const wchar_t *marker = line;
	for (; *marker; ++marker)
		if (marker[0] == L'/' && marker[1] == L'c' && marker[2] == L':') {
			DWORD parent = 0;
			for (marker += 3; *marker >= L'0' && *marker <= L'9'; ++marker)
				parent = parent * 10 + (DWORD)(*marker - L'0');
			return child(parent);
		}

	WNDCLASSW definition = {0};
	definition.lpfnWndProc = DefWindowProcW;
	definition.hInstance = GetModuleHandleW(NULL);
	definition.lpszClassName = class_name;
	if (!RegisterClassW(&definition))
		return 1;
	HWND window = CreateWindowExW(0, class_name, title, WS_OVERLAPPEDWINDOW, 10, 10, 200, 120, NULL, NULL, definition.hInstance, NULL);
	if (!window)
		return 2;
	ShowWindow(window, SW_SHOWNORMAL);

	wchar_t path[MAX_PATH], command[MAX_PATH + 64];
	if (!GetModuleFileNameW(NULL, path, MAX_PATH))
		return 3;
	wsprintfW(command, L"\"%s\" /c:%lu", path, GetCurrentProcessId());
	STARTUPINFOW startup = {sizeof(startup)};
	PROCESS_INFORMATION information;
	if (!CreateProcessW(path, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &information))
		return 4;
	if (WaitForSingleObject(information.hProcess, 20000) != WAIT_OBJECT_0)
		return 5;
	DWORD code = 6;
	GetExitCodeProcess(information.hProcess, &code);
	CloseHandle(information.hThread);
	CloseHandle(information.hProcess);
	DestroyWindow(window);
	return (int)code;
}
