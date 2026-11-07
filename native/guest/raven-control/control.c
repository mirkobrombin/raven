/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven control panel: the settings of the Windows the runtime provides and of the runtime itself, in the layout of Windows 11 Settings.
 *
 * A navigation column picks a page; each page is a column of cards whose rows name a setting and hold its control. Every setting is
 * kept where Windows or the runtime reads it: the visual style under ThemeManager, the installed programs under the Uninstall keys, the
 * runtime options as values of HKCU\Software\Raven\Environment, which the runtime applies to the programs it starts afterwards. */
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>

#define NAV_WIDTH 280
#define CARD_RADIUS 8
#define ROW_HEIGHT 64
#define MAX_ROWS 16
#define ID_FIRST 0x200

enum { PAGE_SYSTEM, PAGE_PERSONALIZATION, PAGE_APPS, PAGE_RAVEN, PAGE_COUNT };

typedef struct {
	const WCHAR *title, *description;
	HWND control;
	int card; /* rows with the same card number share one rounded card */
	WCHAR value[128]; /* the text a row shows at its right when it has no control */
} Row;

typedef struct {
	const WCHAR *label;
	const WCHAR *variable;
} RavenOption;

static const WCHAR *const page_titles[PAGE_COUNT] = {L"System", L"Personalization", L"Apps", L"Raven"};
static const RavenOption raven_toggles[] = {
	{L"Trace system calls", L"RAVEN_TRACE_API"},
	{L"Trace window messages", L"RAVEN_TRACE_MSG"},
	{L"Trace failing calls", L"RAVEN_TRACE_FAIL"},
	{L"Continue past unimplemented functions", L"RAVEN_CONTINUE_UNIMPLEMENTED"},
};

static HWND main_window, apps_list, apps_search;
static HFONT body_font, small_font, title_font, header_font;
static int page = PAGE_SYSTEM, row_count, hover_nav = -1;
static Row rows[MAX_ROWS];
static int scroll_top;

/* Colors of Windows 11 Settings in light mode. */
static const COLORREF base = RGB(0xf3, 0xf3, 0xf3), card = RGB(0xfb, 0xfb, 0xfb), stroke = RGB(0xe5, 0xe5, 0xe5), text = RGB(0x1a, 0x1a, 0x1a),
	secondary = RGB(0x60, 0x60, 0x60), accent = RGB(0x00, 0x5f, 0xb8), selected = RGB(0xea, 0xea, 0xea);

/* Registry ------------------------------------------------------------------------------------------------- */

static BOOL read_string(HKEY root, const WCHAR *path, const WCHAR *name, WCHAR *out, DWORD capacity)
{
	HKEY key;
	DWORD type = 0, size = capacity * sizeof(WCHAR);
	BOOL found = FALSE;
	out[0] = 0;
	if (RegOpenKeyExW(root, path, 0, KEY_READ, &key))
		return FALSE;
	if (!RegQueryValueExW(key, name, NULL, &type, (BYTE *)out, &size) && (type == REG_SZ || type == REG_EXPAND_SZ)) {
		out[capacity - 1] = 0;
		if (size / sizeof(WCHAR) < capacity)
			out[size / sizeof(WCHAR)] = 0;
		found = TRUE;
	} else if (!RegQueryValueExW(key, name, NULL, &type, NULL, &size) && type == REG_DWORD) {
		DWORD number = 0;
		size = sizeof number;
		RegQueryValueExW(key, name, NULL, NULL, (BYTE *)&number, &size);
		wsprintfW(out, L"%u", number);
		found = TRUE;
	}
	RegCloseKey(key);
	return found;
}

static void write_string(HKEY root, const WCHAR *path, const WCHAR *name, const WCHAR *value)
{
	HKEY key;
	if (RegCreateKeyExW(root, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL))
		return;
	if (value)
		RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (lstrlenW(value) + 1) * sizeof(WCHAR));
	else
		RegDeleteValueW(key, name);
	RegCloseKey(key);
}

static const WCHAR environment_key[] = L"Software\\Raven\\Environment";
static const WCHAR theme_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager";

/* Drawing --------------------------------------------------------------------------------------------------- */

static void fill(HDC dc, const RECT *rect, COLORREF color)
{
	HBRUSH brush = CreateSolidBrush(color);
	FillRect(dc, rect, brush);
	DeleteObject(brush);
}

static void rounded(HDC dc, const RECT *rect, int radius, COLORREF inside, COLORREF border)
{
	HBRUSH brush = CreateSolidBrush(inside);
	HPEN pen = CreatePen(PS_SOLID, 1, border);
	HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);
	RoundRect(dc, rect->left, rect->top, rect->right, rect->bottom, radius * 2, radius * 2);
	SelectObject(dc, old_brush);
	SelectObject(dc, old_pen);
	DeleteObject(brush);
	DeleteObject(pen);
}

static void write_text(HDC dc, const WCHAR *string, RECT rect, HFONT font, COLORREF color, UINT flags)
{
	HGDIOBJ old = SelectObject(dc, font);
	SetTextColor(dc, color);
	SetBkMode(dc, TRANSPARENT);
	DrawTextW(dc, string, -1, &rect, flags | DT_NOPREFIX);
	SelectObject(dc, old);
}

/* The small line icons of the navigation column. */
static void nav_icon(HDC dc, int which, int x, int y, COLORREF color)
{
	HPEN pen = CreatePen(PS_SOLID, 1, color);
	HGDIOBJ old_pen = SelectObject(dc, pen), old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
	switch (which) {
	case PAGE_SYSTEM:
		RoundRect(dc, x, y + 1, x + 16, y + 12, 3, 3);
		MoveToEx(dc, x + 5, y + 15, NULL);
		LineTo(dc, x + 12, y + 15);
		break;
	case PAGE_PERSONALIZATION:
		Ellipse(dc, x, y, x + 16, y + 16);
		Ellipse(dc, x + 4, y + 4, x + 7, y + 7);
		Ellipse(dc, x + 9, y + 4, x + 12, y + 7);
		Ellipse(dc, x + 4, y + 9, x + 7, y + 12);
		break;
	case PAGE_APPS:
		Rectangle(dc, x, y, x + 7, y + 7);
		Rectangle(dc, x + 9, y, x + 16, y + 7);
		Rectangle(dc, x, y + 9, x + 7, y + 16);
		Rectangle(dc, x + 9, y + 9, x + 16, y + 16);
		break;
	default:
		MoveToEx(dc, x + 2, y + 15, NULL);
		LineTo(dc, x + 8, y + 1);
		LineTo(dc, x + 14, y + 15);
		MoveToEx(dc, x + 5, y + 10, NULL);
		LineTo(dc, x + 12, y + 10);
		break;
	}
	SelectObject(dc, old_pen);
	SelectObject(dc, old_brush);
	DeleteObject(pen);
}

static RECT nav_item(int index)
{
	RECT rect = {8, 112 + index * 40, NAV_WIDTH - 8, 112 + index * 40 + 36};
	return rect;
}

/* The whole window into a device context: for WM_PAINT, and for WM_PRINTCLIENT, through which controls with rounded corners ask for
   what shows behind them. */
static void draw(HWND window, HDC dc)
{
	RECT client, area;
	WCHAR user[64] = L"User", initial[2] = {0, 0};
	DWORD size = 64;
	int index;
	GetClientRect(window, &client);
	fill(dc, &client, base);
	/* The account at the top of the navigation column. */
	GetUserNameW(user, &size);
	initial[0] = user[0] >= L'a' && user[0] <= L'z' ? (WCHAR)(user[0] - 32) : user[0];
	{
		RECT avatar = {16, 24, 64, 72};
		HBRUSH brush = CreateSolidBrush(RGB(0xd6, 0xe4, 0xf2));
		HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
		Ellipse(dc, avatar.left, avatar.top, avatar.right, avatar.bottom);
		SelectObject(dc, old_brush);
		SelectObject(dc, old_pen);
		DeleteObject(brush);
		write_text(dc, initial, avatar, header_font, accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		area.left = 76; area.top = 28; area.right = NAV_WIDTH; area.bottom = 48;
		write_text(dc, user, area, body_font, text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
		area.top = 48; area.bottom = 68;
		write_text(dc, L"Local account", area, small_font, secondary, DT_LEFT | DT_SINGLELINE);
	}
	for (index = 0; index < PAGE_COUNT; ++index) {
		RECT item = nav_item(index), label = item;
		if (index == page || index == hover_nav)
			rounded(dc, &item, 4, index == page ? selected : RGB(0xee, 0xee, 0xee), index == page ? selected : RGB(0xee, 0xee, 0xee));
		if (index == page) {
			RECT bar = {item.left, item.top + 10, item.left + 3, item.bottom - 10};
			rounded(dc, &bar, 1, accent, accent);
		}
		nav_icon(dc, index, item.left + 16, item.top + 10, text);
		label.left += 48;
		write_text(dc, page_titles[index], label, body_font, text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	}
	/* The page: its title, then the cards with their rows. */
	area.left = NAV_WIDTH + 24; area.top = 24 - scroll_top; area.right = client.right - 32; area.bottom = area.top + 48;
	write_text(dc, page_titles[page], area, title_font, text, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
	for (index = 0; index < row_count; ++index) {
		const Row *row = &rows[index];
		const int top = 96 + index * (ROW_HEIGHT + 4) + row->card * 20 - scroll_top;
		RECT shape = {NAV_WIDTH + 24, top, client.right - 32, top + ROW_HEIGHT}, label = shape;
		rounded(dc, &shape, CARD_RADIUS / 2, card, stroke);
		label.left += 20;
		label.right -= 260;
		if (row->description && row->description[0]) {
			label.top += 12; label.bottom = label.top + 20;
			write_text(dc, row->title, label, body_font, text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
			label.top += 20; label.bottom += 20;
			write_text(dc, row->description, label, small_font, secondary, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
		} else {
			write_text(dc, row->title, label, body_font, text, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
		}
		if (!row->control && row->value[0]) {
			RECT value = {shape.right - 360, shape.top, shape.right - 20, shape.bottom};
			write_text(dc, row->value, value, body_font, secondary, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
		}
	}
}

static void paint(HWND window)
{
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(window, &ps);
	draw(window, dc);
	EndPaint(window, &ps);
}

/* Toggle switches: owner-drawn buttons that keep their state in the window's user data. */
static void draw_toggle(const DRAWITEMSTRUCT *item)
{
	const BOOL on = (BOOL)GetWindowLongPtrW(item->hwndItem, GWLP_USERDATA);
	RECT all = item->rcItem, pill = {all.right - 42, (all.top + all.bottom) / 2 - 10, all.right - 2, (all.top + all.bottom) / 2 + 10}, label = all;
	fill(item->hDC, &all, card);
	rounded(item->hDC, &pill, 10, on ? accent : RGB(0xfd, 0xfd, 0xfd), on ? accent : RGB(0x8a, 0x8a, 0x8a));
	{
		const int cx = on ? pill.right - 10 : pill.left + 10, cy = (pill.top + pill.bottom) / 2;
		HBRUSH brush = CreateSolidBrush(on ? RGB(0xff, 0xff, 0xff) : RGB(0x5c, 0x5c, 0x5c));
		HGDIOBJ old_brush = SelectObject(item->hDC, brush), old_pen = SelectObject(item->hDC, GetStockObject(NULL_PEN));
		Ellipse(item->hDC, cx - 6, cy - 6, cx + 7, cy + 7);
		SelectObject(item->hDC, old_brush);
		SelectObject(item->hDC, old_pen);
		DeleteObject(brush);
	}
	label.right = pill.left - 12;
	write_text(item->hDC, on ? L"On" : L"Off", label, body_font, text, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

/* Pages ------------------------------------------------------------------------------------------------------ */

static void clear_page(void)
{
	int index;
	for (index = 0; index < row_count; ++index)
		if (rows[index].control)
			DestroyWindow(rows[index].control);
	if (apps_list)
		DestroyWindow(apps_list);
	if (apps_search)
		DestroyWindow(apps_search);
	apps_list = apps_search = NULL;
	row_count = 0;
	scroll_top = 0;
}

static Row *add_row(const WCHAR *title, const WCHAR *description, int group)
{
	Row *row = &rows[row_count++];
	row->title = title;
	row->description = description;
	row->control = NULL;
	row->card = group;
	row->value[0] = 0;
	return row;
}

static HWND add_child(const WCHAR *klass, const WCHAR *caption, DWORD style, int id)
{
	HWND child = CreateWindowExW(0, klass, caption, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 10, 10, main_window, (HMENU)(INT_PTR)id,
		GetModuleHandleW(NULL), NULL);
	SendMessageW(child, WM_SETFONT, (WPARAM)body_font, FALSE);
	return child;
}

static void show_system(void)
{
	SYSTEM_INFO system;
	MEMORYSTATUSEX memory = {sizeof memory};
	OSVERSIONINFOW version = {sizeof version};
	WCHAR name[MAX_COMPUTERNAME_LENGTH + 1];
	DWORD size = ARRAYSIZE(name);
	Row *row;
	GetNativeSystemInfo(&system);
	GlobalMemoryStatusEx(&memory);
	GetVersionExW(&version);
	row = add_row(L"Device name", NULL, 0);
	if (GetComputerNameW(name, &size))
		lstrcpynW(row->value, name, 128);
	row = add_row(L"Processor", NULL, 0);
	if (!read_string(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", row->value, 128))
		wsprintfW(row->value, L"%u logical processors", system.dwNumberOfProcessors);
	row = add_row(L"Installed RAM", NULL, 0);
	wsprintfW(row->value, L"%u MB", (UINT)(memory.ullTotalPhys / (1024 * 1024)));
	row = add_row(L"System type", NULL, 0);
	lstrcpyW(row->value, system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? L"64-bit operating system, x64-based processor"
		: L"32-bit operating system");
	row = add_row(L"Windows version", L"The version programs see", 1);
	wsprintfW(row->value, L"%u.%u build %u", version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
	row = add_row(L"Runtime", L"What runs the Windows programs on this computer", 1);
	lstrcpyW(row->value, L"Raven");
}

static void show_personalization(void)
{
	WCHAR active[8];
	Row *row = add_row(L"Visual style", L"Windows 11, or the classic look of earlier Windows; programs started afterwards use it", 0);
	row->control = add_child(L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_FIRST);
	SendMessageW(row->control, CB_ADDSTRING, 0, (LPARAM)L"Windows 11");
	SendMessageW(row->control, CB_ADDSTRING, 0, (LPARAM)L"Classic");
	read_string(HKEY_CURRENT_USER, theme_key, L"ThemeActive", active, 8);
	SendMessageW(row->control, CB_SETCURSEL, active[0] == L'0' ? 1 : 0, 0);
	row = add_row(L"Color mode", L"Light", 1);
	row = add_row(L"Accent color", L"Blue, as Windows 11 starts", 1);
}

/* The programs Windows lists in Installed apps: every Uninstall entry with a display name, from the machine of both widths and the user. */
static void fill_apps(void)
{
	static const struct { HKEY root; const WCHAR *path; } sources[] = {
		{HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
		{HKEY_LOCAL_MACHINE, L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
		{HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"}};
	WCHAR filter[128];
	UINT source;
	int count = 0;
	GetWindowTextW(apps_search, filter, 128);
	CharLowerW(filter);
	SendMessageW(apps_list, LVM_DELETEALLITEMS, 0, 0);
	for (source = 0; source < ARRAYSIZE(sources); ++source) {
		HKEY key;
		WCHAR sub[256];
		DWORD index;
		if (RegOpenKeyExW(sources[source].root, sources[source].path, 0, KEY_READ, &key))
			continue;
		for (index = 0;; ++index) {
			DWORD length = ARRAYSIZE(sub);
			WCHAR path[512], name[256], publisher[256], version[64], lowered[256];
			LVITEMW item = {0};
			int row;
			if (RegEnumKeyExW(key, index, sub, &length, NULL, NULL, NULL, NULL))
				break;
			lstrcpyW(path, sources[source].path);
			lstrcatW(path, L"\\");
			lstrcatW(path, sub);
			if (!read_string(sources[source].root, path, L"DisplayName", name, 256) || !name[0])
				continue;
			lstrcpyW(lowered, name);
			CharLowerW(lowered);
			if (filter[0] && !wcsstr(lowered, filter))
				continue;
			read_string(sources[source].root, path, L"Publisher", publisher, 256);
			read_string(sources[source].root, path, L"DisplayVersion", version, 64);
			item.mask = LVIF_TEXT | LVIF_PARAM;
			item.iItem = count;
			item.pszText = name;
			item.lParam = (LPARAM)source;
			row = (int)SendMessageW(apps_list, LVM_INSERTITEMW, 0, (LPARAM)&item);
			if (row < 0)
				continue;
			item.iSubItem = 1;
			item.pszText = publisher;
			SendMessageW(apps_list, LVM_SETITEMTEXTW, row, (LPARAM)&item);
			item.iSubItem = 2;
			item.pszText = version;
			SendMessageW(apps_list, LVM_SETITEMTEXTW, row, (LPARAM)&item);
			item.iSubItem = 3;
			item.pszText = path;
			SendMessageW(apps_list, LVM_SETITEMTEXTW, row, (LPARAM)&item);
			++count;
		}
		RegCloseKey(key);
	}
}

static void show_apps(void)
{
	static const struct { const WCHAR *title; int width; } columns[] = {{L"Name", 300}, {L"Publisher", 200}, {L"Version", 110}, {L"", 0}};
	Row *row = add_row(L"Installed apps", L"Programs installers registered, as Windows lists them", 0);
	UINT index;
	row->control = add_child(L"Button", L"Uninstall", BS_PUSHBUTTON, ID_FIRST + 1);
	apps_search = add_child(L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER, ID_FIRST + 2);
	SendMessageW(apps_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search apps");
	apps_list = add_child(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER, ID_FIRST + 3);
	SendMessageW(apps_list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
	for (index = 0; index < ARRAYSIZE(columns); ++index) {
		LVCOLUMNW column = {LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM, 0, columns[index].width, (WCHAR *)columns[index].title, 0, (int)index};
		SendMessageW(apps_list, LVM_INSERTCOLUMNW, index, (LPARAM)&column);
	}
	fill_apps();
}

static void show_raven(void)
{
	UINT index;
	WCHAR value[64];
	for (index = 0; index < ARRAYSIZE(raven_toggles); ++index) {
		Row *row = add_row(raven_toggles[index].label, raven_toggles[index].variable, 0);
		row->control = add_child(L"Button", L"", BS_OWNERDRAW, ID_FIRST + 10 + (int)index);
		SetWindowLongPtrW(row->control, GWLP_USERDATA, read_string(HKEY_CURRENT_USER, environment_key, raven_toggles[index].variable, value, 64) &&
			value[0] && value[0] != L'0');
	}
	{
		Row *row = add_row(L"Scheduler time slice", L"Milliseconds each thread runs before another gets the processor; empty uses the default", 1);
		row->control = add_child(L"Edit", L"", ES_AUTOHSCROLL | ES_NUMBER | WS_BORDER, ID_FIRST + 30);
		read_string(HKEY_CURRENT_USER, environment_key, L"RAVEN_SLICE_MS", value, 64);
		SetWindowTextW(row->control, value);
		row = add_row(L"Memory commit limit", L"Megabytes a program may commit; empty leaves it to the host", 1);
		row->control = add_child(L"Edit", L"", ES_AUTOHSCROLL | ES_NUMBER | WS_BORDER, ID_FIRST + 31);
		read_string(HKEY_CURRENT_USER, environment_key, L"RAVEN_COMMIT_LIMIT_MB", value, 64);
		SetWindowTextW(row->control, value);
	}
	{
		Row *row = add_row(L"These options apply to programs started afterwards", NULL, 2);
		(void)row;
	}
}

static void layout(void)
{
	RECT client;
	int index;
	GetClientRect(main_window, &client);
	for (index = 0; index < row_count; ++index) {
		const int top = 96 + index * (ROW_HEIGHT + 4) + rows[index].card * 20 - scroll_top;
		if (rows[index].control) {
			const BOOL toggle = (GetWindowLongW(rows[index].control, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW;
			const int width = toggle ? 110 : 220, height = toggle ? 40 : 28;
			MoveWindow(rows[index].control, client.right - 52 - width, top + (ROW_HEIGHT - height) / 2, width, toggle ? height : height + (
				(GetWindowLongW(rows[index].control, GWL_STYLE) & CBS_DROPDOWNLIST) == CBS_DROPDOWNLIST ? 120 : 0), TRUE);
		}
	}
	if (apps_list) {
		const int top = 96 + ROW_HEIGHT + 12;
		MoveWindow(apps_search, NAV_WIDTH + 24, top, 320, 30, TRUE);
		MoveWindow(apps_list, NAV_WIDTH + 24, top + 40, client.right - NAV_WIDTH - 56, client.bottom - top - 64, TRUE);
	}
	InvalidateRect(main_window, NULL, TRUE);
}

static void show_page(int which)
{
	clear_page();
	page = which;
	switch (page) {
	case PAGE_SYSTEM: show_system(); break;
	case PAGE_PERSONALIZATION: show_personalization(); break;
	case PAGE_APPS: show_apps(); break;
	default: show_raven(); break;
	}
	layout();
}

/* Runs the uninstaller of the selected program and lists the programs again when it has finished. */
static void uninstall_selected(void)
{
	WCHAR path[512], command[1024], name[256], question[400];
	LVITEMW item = {0};
	const int selected = (int)SendMessageW(apps_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
	HKEY root;
	STARTUPINFOW startup = {sizeof startup};
	PROCESS_INFORMATION started;
	if (selected < 0) {
		MessageBoxW(main_window, L"Choose an app in the list first.", L"Uninstall", MB_OK | MB_ICONINFORMATION);
		return;
	}
	item.mask = LVIF_PARAM;
	item.iItem = selected;
	SendMessageW(apps_list, LVM_GETITEMW, 0, (LPARAM)&item);
	root = item.lParam == 2 ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
	item.mask = LVIF_TEXT;
	item.iSubItem = 3;
	item.pszText = path;
	item.cchTextMax = ARRAYSIZE(path);
	SendMessageW(apps_list, LVM_GETITEMTEXTW, selected, (LPARAM)&item);
	item.iSubItem = 0;
	item.pszText = name;
	item.cchTextMax = ARRAYSIZE(name);
	SendMessageW(apps_list, LVM_GETITEMTEXTW, selected, (LPARAM)&item);
	if (!read_string(root, path, L"UninstallString", command, ARRAYSIZE(command)) || !command[0]) {
		MessageBoxW(main_window, L"This app does not say how to uninstall it.", L"Uninstall", MB_OK | MB_ICONWARNING);
		return;
	}
	wsprintfW(question, L"%s will be uninstalled.", name);
	if (MessageBoxW(main_window, question, L"Uninstall", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
		return;
	if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &started)) {
		MessageBoxW(main_window, L"The uninstaller could not be started.", L"Uninstall", MB_OK | MB_ICONERROR);
		return;
	}
	EnableWindow(main_window, FALSE);
	for (;;) {
		MSG message;
		if (MsgWaitForMultipleObjects(1, &started.hProcess, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0)
			break;
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
	EnableWindow(main_window, TRUE);
	CloseHandle(started.hThread);
	CloseHandle(started.hProcess);
	fill_apps();
}

static void store_number(int id, const WCHAR *variable)
{
	WCHAR value[32];
	GetWindowTextW(GetDlgItem(main_window, id), value, 32);
	write_string(HKEY_CURRENT_USER, environment_key, variable, value[0] ? value : NULL);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message) {
	case WM_PAINT:
		paint(window);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PRINTCLIENT:
		draw(window, (HDC)wparam);
		return 0;
	case WM_SIZE:
		if (main_window)
			layout();
		return 0;
	case WM_GETMINMAXINFO:
		((MINMAXINFO *)lparam)->ptMinTrackSize.x = 820;
		((MINMAXINFO *)lparam)->ptMinTrackSize.y = 560;
		return 0;
	case WM_MOUSEMOVE: {
		const POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		int index, hit = -1;
		for (index = 0; index < PAGE_COUNT; ++index) {
			const RECT item = nav_item(index);
			if (PtInRect(&item, point))
				hit = index;
		}
		if (hit != hover_nav) {
			RECT column = {0, 0, NAV_WIDTH, 600};
			hover_nav = hit;
			InvalidateRect(window, &column, FALSE);
		}
		return 0;
	}
	case WM_LBUTTONDOWN: {
		const POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		int index;
		for (index = 0; index < PAGE_COUNT; ++index) {
			const RECT item = nav_item(index);
			if (PtInRect(&item, point) && index != page)
				show_page(index);
		}
		return 0;
	}
	case WM_KEYDOWN:
		if (wparam == VK_UP && page > 0)
			show_page(page - 1);
		else if (wparam == VK_DOWN && page < PAGE_COUNT - 1)
			show_page(page + 1);
		return 0;
	case WM_DRAWITEM:
		draw_toggle((const DRAWITEMSTRUCT *)lparam);
		return TRUE;
	case WM_COMMAND: {
		const int id = LOWORD(wparam), code = HIWORD(wparam);
		if (page == PAGE_PERSONALIZATION && id == ID_FIRST && code == CBN_SELCHANGE) {
			const LRESULT chosen = SendMessageW((HWND)lparam, CB_GETCURSEL, 0, 0);
			write_string(HKEY_CURRENT_USER, theme_key, L"ThemeActive", chosen == 1 ? L"0" : L"1");
		} else if (page == PAGE_APPS && id == ID_FIRST + 1) {
			uninstall_selected();
		} else if (page == PAGE_APPS && id == ID_FIRST + 2 && code == EN_CHANGE) {
			fill_apps();
		} else if (page == PAGE_RAVEN && id >= ID_FIRST + 10 && id < ID_FIRST + 10 + (int)ARRAYSIZE(raven_toggles)) {
			HWND toggle = (HWND)lparam;
			const BOOL on = !GetWindowLongPtrW(toggle, GWLP_USERDATA);
			SetWindowLongPtrW(toggle, GWLP_USERDATA, on);
			write_string(HKEY_CURRENT_USER, environment_key, raven_toggles[id - ID_FIRST - 10].variable, on ? L"1" : NULL);
			InvalidateRect(toggle, NULL, TRUE);
		} else if (page == PAGE_RAVEN && id == ID_FIRST + 30 && code == EN_CHANGE) {
			store_number(id, L"RAVEN_SLICE_MS");
		} else if (page == PAGE_RAVEN && id == ID_FIRST + 31 && code == EN_CHANGE) {
			store_number(id, L"RAVEN_COMMIT_LIMIT_MB");
		}
		return 0;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

static HFONT make_font(int height, int weight)
{
	return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH, L"Segoe UI");
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line, int show)
{
	INITCOMMONCONTROLSEX classes = {sizeof classes, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
	WNDCLASSEXW klass = {sizeof klass};
	MSG message;
	int which;
	InitCommonControlsEx(&classes);
	body_font = make_font(14, FW_NORMAL);
	small_font = make_font(12, FW_NORMAL);
	header_font = make_font(20, FW_SEMIBOLD);
	title_font = make_font(28, FW_SEMIBOLD);
	klass.lpfnWndProc = window_proc;
	klass.hInstance = instance;
	klass.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	klass.lpszClassName = L"RavenControlPanel";
	RegisterClassExW(&klass);
	/* A page can be named on the command line, as control.exe opens a panel: system, personalization, apps or raven. */
	page = PAGE_SYSTEM;
	for (which = 0; command_line && which < PAGE_COUNT; ++which) {
		const WCHAR *name = command_line;
		WCHAR word[32];
		int length = 0;
		while (*name == L' ' || *name == L'"')
			++name;
		while (name[length] && name[length] != L' ' && name[length] != L'"' && length < 31)
			word[length] = name[length], ++length;
		word[length] = 0;
		if (!lstrcmpiW(word, page_titles[which]))
			page = which;
	}
	main_window = CreateWindowExW(0, klass.lpszClassName, L"Settings", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
		1000, 680, NULL, NULL, instance, NULL);
	if (!main_window)
		return 1;
	show_page(page);
	ShowWindow(main_window, show);
	UpdateWindow(main_window);
	while (GetMessageW(&message, NULL, 0, 0) > 0) {
		if (IsDialogMessageW(main_window, &message))
			continue;
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	return (int)message.wParam;
}

/* The start of a program built without a C runtime start file: wWinMain with the command line after the program name. */
void WINAPI raven_program_entry(void)
{
	LPWSTR line = GetCommandLineW();
	BOOL quoted = FALSE;
	STARTUPINFOW startup = {sizeof startup};
	while (*line && (quoted || (*line != L' ' && *line != L'\t'))) {
		if (*line == L'"')
			quoted = !quoted;
		++line;
	}
	while (*line == L' ' || *line == L'\t')
		++line;
	GetStartupInfoW(&startup);
	ExitProcess((UINT)wWinMain(GetModuleHandleW(NULL), NULL, line, startup.dwFlags & STARTF_USESHOWWINDOW ? startup.wShowWindow : SW_SHOWDEFAULT));
}
