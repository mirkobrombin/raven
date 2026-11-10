/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven control panel: the settings of the Windows the runtime provides and of the runtime itself, in the layout of Windows 11 Settings.
 *
 * A navigation column picks a page; each page is a column of cards whose rows name a setting and hold its control. Every setting is
 * kept where Windows or the runtime reads it: the visual style under ThemeManager, the installed programs under the Uninstall keys, the
 * runtime options as values of HKCU\Software\Raven\Environment, which the runtime applies to the programs it starts afterwards.
 *
 * The sandbox and the snapshots are the runtime's own: the panel asks for them through ravenhost.dll, which answers only the panel the
 * runtime trusts, the one started from the runtime's folder. */
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>

#define NAV_WIDTH 280
#define CARD_RADIUS 8
#define ROW_HEIGHT 64
#define MAX_ROWS 16
#define ID_FIRST 0x200

enum { PAGE_SYSTEM, PAGE_PERSONALIZATION, PAGE_APPS, PAGE_DEFAULTS, PAGE_PRIVACY, PAGE_SNAPSHOTS, PAGE_RAVEN, PAGE_COUNT };

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

static const WCHAR *const page_titles[PAGE_COUNT] = {L"System", L"Personalization", L"Apps", L"Default apps", L"Privacy & security", L"Snapshots", L"Raven"};
/* The names control.exe takes for its pages on the command line. */
static const WCHAR *const page_names[PAGE_COUNT] = {L"system", L"personalization", L"apps", L"defaults", L"privacy", L"snapshots", L"raven"};

/* The runtime's private calls (ravenhost.dll): the prefix's sandbox.conf and sandbox.log, and its snapshots. */
DWORD WINAPI RavenSandboxRead(WCHAR *buffer, DWORD capacity);
BOOL WINAPI RavenSandboxWrite(const WCHAR *text);
DWORD WINAPI RavenSandboxLog(WCHAR *buffer, DWORD capacity);
DWORD WINAPI RavenSnapshotList(WCHAR *buffer, DWORD capacity);
BOOL WINAPI RavenSnapshotTake(const WCHAR *label);
BOOL WINAPI RavenSnapshotRestore(const WCHAR *name);
BOOL WINAPI RavenSnapshotDelete(const WCHAR *name);
DWORD WINAPI RavenHostApps(WCHAR *buffer, DWORD capacity);
static const RavenOption raven_toggles[] = {
	{L"Trace system calls", L"RAVEN_TRACE_API"},
	{L"Trace window messages", L"RAVEN_TRACE_MSG"},
	{L"Trace failing calls", L"RAVEN_TRACE_FAIL"},
	{L"Continue past unimplemented functions", L"RAVEN_CONTINUE_UNIMPLEMENTED"},
};

static HWND main_window, apps_list, apps_search, grants_list, log_list, snapshots_list, list_buttons[4];
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
	case PAGE_DEFAULTS:
		/* A file and an arrow out of it. */
		Rectangle(dc, x + 1, y, x + 11, y + 16);
		MoveToEx(dc, x + 6, y + 8, NULL);
		LineTo(dc, x + 16, y + 8);
		LineTo(dc, x + 12, y + 4);
		MoveToEx(dc, x + 16, y + 8, NULL);
		LineTo(dc, x + 12, y + 12);
		break;
	case PAGE_PRIVACY:
		/* A shield. */
		MoveToEx(dc, x + 8, y, NULL);
		LineTo(dc, x + 15, y + 3);
		LineTo(dc, x + 14, y + 10);
		LineTo(dc, x + 8, y + 16);
		LineTo(dc, x + 2, y + 10);
		LineTo(dc, x + 1, y + 3);
		LineTo(dc, x + 8, y);
		break;
	case PAGE_SNAPSHOTS:
		/* A clock. */
		Ellipse(dc, x, y, x + 16, y + 16);
		MoveToEx(dc, x + 8, y + 4, NULL);
		LineTo(dc, x + 8, y + 8);
		LineTo(dc, x + 11, y + 10);
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
	for (index = 0; index < 4; ++index)
		if (list_buttons[index])
			DestroyWindow(list_buttons[index]);
	if (grants_list)
		DestroyWindow(grants_list);
	if (log_list)
		DestroyWindow(log_list);
	if (snapshots_list)
		DestroyWindow(snapshots_list);
	grants_list = log_list = snapshots_list = list_buttons[0] = list_buttons[1] = list_buttons[2] = list_buttons[3] = NULL;
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

/* Privacy & security ------------------------------------------------------------------------------------------- */

/* sandbox.conf as lines: the panel changes the lines it knows and keeps the others as they are. */
#define MAX_LINES 256
static WCHAR *config_lines[MAX_LINES];
static int config_count;

static void read_config(void)
{
	DWORD size = RavenSandboxRead(NULL, 0);
	WCHAR *text = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size + 1) * sizeof(WCHAR)), *at, *end;
	int index;
	for (index = 0; index < config_count; ++index)
		HeapFree(GetProcessHeap(), 0, config_lines[index]);
	config_count = 0;
	if (!text)
		return;
	if (size)
		RavenSandboxRead(text, size + 1);
	for (at = text; *at && config_count < MAX_LINES; at = *end ? end + 1 : end) {
		int length;
		for (end = at; *end && *end != L'\n'; ++end)
			;
		length = (int)(end - at);
		if (!length)
			continue;
		config_lines[config_count] = HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
		if (!config_lines[config_count])
			break;
		lstrcpynW(config_lines[config_count], at, length + 1);
		++config_count;
	}
	HeapFree(GetProcessHeap(), 0, text);
}

static BOOL write_config(void)
{
	int index, size = 1;
	WCHAR *text;
	BOOL done;
	for (index = 0; index < config_count; ++index)
		size += lstrlenW(config_lines[index]) + 1;
	text = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size * sizeof(WCHAR));
	if (!text)
		return FALSE;
	for (index = 0; index < config_count; ++index) {
		lstrcatW(text, config_lines[index]);
		lstrcatW(text, L"\n");
	}
	done = RavenSandboxWrite(text);
	HeapFree(GetProcessHeap(), 0, text);
	if (!done)
		MessageBoxW(main_window, L"The settings could not be saved.", L"Privacy & security", MB_OK | MB_ICONERROR);
	return done;
}

static void add_config_line(const WCHAR *line)
{
	if (config_count >= MAX_LINES)
		return;
	config_lines[config_count] = HeapAlloc(GetProcessHeap(), 0, (lstrlenW(line) + 1) * sizeof(WCHAR));
	if (config_lines[config_count])
		lstrcpyW(config_lines[config_count++], line);
}

static void remove_config_line(int index)
{
	if (index < 0 || index >= config_count)
		return;
	HeapFree(GetProcessHeap(), 0, config_lines[index]);
	for (; index + 1 < config_count; ++index)
		config_lines[index] = config_lines[index + 1];
	--config_count;
}

static BOOL starts_with(const WCHAR *text, const WCHAR *start)
{
	while (*start)
		if (*text++ != *start++)
			return FALSE;
	return TRUE;
}

/* A grant line: "read PATH", "write PATH", "app-read PROGRAM<tab>PATH" or "app-write PROGRAM<tab>PATH". */
static BOOL parse_grant(const WCHAR *line, BOOL *write, WCHAR *program, const WCHAR **path)
{
	const WCHAR *rest;
	program[0] = 0;
	if (starts_with(line, L"read ") || starts_with(line, L"write ")) {
		*write = line[0] == L'w';
		*path = line + (*write ? 6 : 5);
		return TRUE;
	}
	if (!starts_with(line, L"app-read ") && !starts_with(line, L"app-write "))
		return FALSE;
	*write = line[4] == L'w';
	rest = line + (*write ? 10 : 9);
	{
		const WCHAR *tab = rest;
		int length;
		while (*tab && *tab != L'\t')
			++tab;
		if (!*tab)
			return FALSE;
		length = (int)(tab - rest) < MAX_PATH - 1 ? (int)(tab - rest) : MAX_PATH - 1;
		lstrcpynW(program, rest, length + 1);
		*path = tab + 1;
	}
	return TRUE;
}

static void set_cell(HWND list, int row, int column, const WCHAR *value)
{
	LVITEMW item = {0};
	item.iSubItem = column;
	item.pszText = (WCHAR *)value;
	SendMessageW(list, LVM_SETITEMTEXTW, row, (LPARAM)&item);
}

static int add_list_row(HWND list, const WCHAR *first, LPARAM data)
{
	LVITEMW item = {0};
	item.mask = LVIF_TEXT | LVIF_PARAM;
	item.iItem = (int)SendMessageW(list, LVM_GETITEMCOUNT, 0, 0);
	item.pszText = (WCHAR *)first;
	item.lParam = data;
	return (int)SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
}

static HWND make_list(int id, const WCHAR *const *titles, const int *widths, int count)
{
	HWND list = add_child(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER, id);
	int index;
	SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
	for (index = 0; index < count; ++index) {
		LVCOLUMNW column = {LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM, 0, widths[index], (WCHAR *)titles[index], 0, index};
		SendMessageW(list, LVM_INSERTCOLUMNW, index, (LPARAM)&column);
	}
	return list;
}

static void fill_grants(void)
{
	int index;
	SendMessageW(grants_list, LVM_DELETEALLITEMS, 0, 0);
	for (index = 0; index < config_count; ++index) {
		BOOL write;
		WCHAR program[MAX_PATH];
		const WCHAR *path;
		int row;
		if (!parse_grant(config_lines[index], &write, program, &path))
			continue;
		row = add_list_row(grants_list, path, index);
		set_cell(grants_list, row, 1, write ? L"Read and change" : L"Read");
		set_cell(grants_list, row, 2, program[0] ? program : L"Every program");
	}
}

/* The refusals the runtime noted, newest first: "time<tab>program<tab>what<tab>path" a line. */
static void fill_log(void)
{
	DWORD size = RavenSandboxLog(NULL, 0);
	WCHAR *text = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size + 1) * sizeof(WCHAR)), *end;
	int shown = 0;
	SendMessageW(log_list, LVM_DELETEALLITEMS, 0, 0);
	if (!text)
		return;
	if (size)
		RavenSandboxLog(text, size + 1);
	end = text + lstrlenW(text);
	while (end > text && shown < 300) {
		WCHAR *line = end, *fields[4];
		int field = 0;
		LVITEMW item = {0};
		int row;
		if (line > text && line[-1] == L'\n')
			*--line = 0;
		while (line > text && line[-1] != L'\n')
			--line;
		end = line;
		if (!*line)
			continue;
		fields[0] = line;
		for (; *line && field < 3; ++line)
			if (*line == L'\t') {
				*line = 0;
				fields[++field] = line + 1;
			}
		if (field < 3)
			continue;
		item.mask = LVIF_TEXT;
		item.iItem = shown;
		item.pszText = fields[0];
		row = (int)SendMessageW(log_list, LVM_INSERTITEMW, 0, (LPARAM)&item);
		set_cell(log_list, row, 1, fields[1]);
		set_cell(log_list, row, 2, !lstrcmpW(fields[2], L"open") ? L"Open" : !lstrcmpW(fields[2], L"change") ? L"Change" :
			!lstrcmpW(fields[2], L"link") ? L"Link out of the prefix" : !lstrcmpW(fields[2], L"open-on-host") ? L"Hand to a Linux program" :
			!lstrcmpW(fields[2], L"open-address") ? L"Open an address" : fields[2]);
		set_cell(log_list, row, 3, fields[3]);
		++shown;
	}
	HeapFree(GetProcessHeap(), 0, text);
}

static int network_line(void)
{
	int index;
	for (index = 0; index < config_count; ++index)
		if (starts_with(config_lines[index], L"network "))
			return index;
	return -1;
}

static void show_privacy(void)
{
	static const WCHAR *const grant_titles[] = {L"Path", L"Access", L"For"};
	static const int grant_widths[] = {340, 130, 220};
	static const WCHAR *const log_titles[] = {L"Time", L"Program", L"Refused", L"Path"};
	static const int log_widths[] = {150, 220, 160, 300};
	Row *row;
	const int network = (read_config(), network_line());
	row = add_row(L"Internet access", L"Programs of this prefix may reach the network", 0);
	row->control = add_child(L"Button", L"", BS_OWNERDRAW, ID_FIRST + 40);
	SetWindowLongPtrW(row->control, GWLP_USERDATA, network < 0 || lstrcmpW(config_lines[network], L"network off"));
	row = add_row(L"Linux files programs can open", L"Besides these, a program sees only the files you choose in its file dialogs", 1);
	row->control = add_child(L"Button", L"Add folder", BS_PUSHBUTTON, ID_FIRST + 41);
	list_buttons[0] = add_child(L"Button", L"Allow changes", BS_PUSHBUTTON, ID_FIRST + 42);
	list_buttons[1] = add_child(L"Button", L"Remove", BS_PUSHBUTTON, ID_FIRST + 43);
	grants_list = make_list(ID_FIRST + 44, grant_titles, grant_widths, 3);
	log_list = make_list(ID_FIRST + 45, log_titles, log_widths, 4);
	fill_grants();
	fill_log();
}

static int selected_param(HWND list)
{
	LVITEMW item = {0};
	item.iItem = (int)SendMessageW(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
	if (item.iItem < 0)
		return -1;
	item.mask = LVIF_PARAM;
	SendMessageW(list, LVM_GETITEMW, 0, (LPARAM)&item);
	return (int)item.lParam;
}

/* A Linux folder for every program: chosen in the file dialog, which shows the whole computer to the panel. */
static void add_folder(void)
{
	IFileOpenDialog *dialog = NULL;
	IShellItem *item = NULL;
	WCHAR *chosen = NULL, line[MAX_PATH + 8];
	DWORD options = 0;
	int index;
	if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dialog)))
		return;
	IFileOpenDialog_GetOptions(dialog, &options);
	IFileOpenDialog_SetOptions(dialog, options | FOS_PICKFOLDERS);
	IFileOpenDialog_SetTitle(dialog, L"Choose a folder programs can open");
	if (SUCCEEDED(IFileOpenDialog_Show(dialog, main_window)) && SUCCEEDED(IFileOpenDialog_GetResult(dialog, &item)) &&
		SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &chosen)) && chosen) {
		if ((chosen[0] | 0x20) != L'z' || chosen[1] != L':') {
			MessageBoxW(main_window, L"The folders of the prefix are open to every program already.", L"Add folder", MB_OK | MB_ICONINFORMATION);
		} else {
			/* Z: holds the host's files at their own spelling. */
			lstrcpyW(line, L"read ");
			lstrcpynW(line + 5, chosen[2] ? chosen + 2 : L"\\", MAX_PATH);
			for (index = 5; line[index]; ++index)
				if (line[index] == L'\\')
					line[index] = L'/';
			if (!line[6] && line[5] == L'/')
				line[6] = 0;
			add_config_line(line);
			if (write_config())
				fill_grants();
		}
	}
	if (chosen)
		CoTaskMemFree(chosen);
	if (item)
		IShellItem_Release(item);
	IFileOpenDialog_Release(dialog);
}

static void change_grant(BOOL remove)
{
	const int index = selected_param(grants_list);
	BOOL write;
	WCHAR program[MAX_PATH], *line;
	const WCHAR *path;
	if (index < 0 || index >= config_count || !parse_grant(config_lines[index], &write, program, &path)) {
		MessageBoxW(main_window, L"Choose a path in the list first.", L"Privacy & security", MB_OK | MB_ICONINFORMATION);
		return;
	}
	if (remove) {
		remove_config_line(index);
	} else {
		/* Read becomes read and change, and back. */
		line = HeapAlloc(GetProcessHeap(), 0, (lstrlenW(config_lines[index]) + 8) * sizeof(WCHAR));
		if (!line)
			return;
		lstrcpyW(line, program[0] ? (write ? L"app-read " : L"app-write ") : (write ? L"read " : L"write "));
		if (program[0]) {
			lstrcatW(line, program);
			lstrcatW(line, L"\t");
		}
		lstrcatW(line, path);
		HeapFree(GetProcessHeap(), 0, config_lines[index]);
		config_lines[index] = line;
	}
	if (write_config())
		fill_grants();
}

static void toggle_network(HWND toggle)
{
	const BOOL on = !GetWindowLongPtrW(toggle, GWLP_USERDATA);
	const int index = network_line();
	if (index >= 0)
		remove_config_line(index);
	if (!on)
		add_config_line(L"network off");
	if (write_config())
		SetWindowLongPtrW(toggle, GWLP_USERDATA, on);
	InvalidateRect(toggle, NULL, TRUE);
}

/* Snapshots ---------------------------------------------------------------------------------------------------- */

/* A snapshot is named by when it was taken and why: "20261110-145012-before-setup.exe". */
static void fill_snapshots(void)
{
	DWORD size = RavenSnapshotList(NULL, 0);
	WCHAR *text = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size + 1) * sizeof(WCHAR)), *at, *end;
	SendMessageW(snapshots_list, LVM_DELETEALLITEMS, 0, 0);
	if (!text)
		return;
	if (size)
		RavenSnapshotList(text, size + 1);
	for (at = text; *at; at = end) {
		WCHAR when[32] = L"", *name;
		LVITEMW item = {0};
		int row;
		for (end = at; *end && *end != L'\n'; ++end)
			;
		if (*end)
			*end++ = 0;
		if (!*at)
			continue;
		name = HeapAlloc(GetProcessHeap(), 0, (lstrlenW(at) + 1) * sizeof(WCHAR));
		if (!name)
			break;
		lstrcpyW(name, at);
		if (lstrlenW(at) >= 15 && at[8] == L'-')
			wsprintfW(when, L"%.4s-%.2s-%.2s %.2s:%.2s:%.2s", at, at + 4, at + 6, at + 9, at + 11, at + 13);
		/* Newest first. */
		item.mask = LVIF_TEXT | LVIF_PARAM;
		item.iItem = 0;
		item.pszText = when[0] ? when : at;
		item.lParam = (LPARAM)name;
		row = (int)SendMessageW(snapshots_list, LVM_INSERTITEMW, 0, (LPARAM)&item);
		set_cell(snapshots_list, row, 1, lstrlenW(at) > 16 && at[15] == L'-' ? at + 16 : L"");
	}
	HeapFree(GetProcessHeap(), 0, text);
}

static void free_snapshot_names(void)
{
	int index, count;
	if (!snapshots_list)
		return;
	count = (int)SendMessageW(snapshots_list, LVM_GETITEMCOUNT, 0, 0);
	for (index = 0; index < count; ++index) {
		LVITEMW item = {0};
		item.mask = LVIF_PARAM;
		item.iItem = index;
		if (SendMessageW(snapshots_list, LVM_GETITEMW, 0, (LPARAM)&item) && item.lParam)
			HeapFree(GetProcessHeap(), 0, (void *)item.lParam);
	}
}

static void show_snapshots(void)
{
	static const WCHAR *const titles[] = {L"Taken", L"Reason"};
	static const int widths[] = {200, 420};
	Row *row = add_row(L"Snapshots of this prefix", L"Raven takes one before every installer; restoring one keeps the prefix as it is now as another", 0);
	row->control = add_child(L"Button", L"Take snapshot", BS_PUSHBUTTON, ID_FIRST + 50);
	list_buttons[0] = add_child(L"Button", L"Restore", BS_PUSHBUTTON, ID_FIRST + 51);
	list_buttons[1] = add_child(L"Button", L"Delete", BS_PUSHBUTTON, ID_FIRST + 52);
	snapshots_list = make_list(ID_FIRST + 53, titles, widths, 2);
	fill_snapshots();
}

static void snapshot_action(int id)
{
	const WCHAR *name;
	LVITEMW item = {0};
	WCHAR question[400];
	if (id == ID_FIRST + 50) {
		if (!RavenSnapshotTake(L"manual"))
			MessageBoxW(main_window, L"The snapshot could not be taken.", L"Snapshots", MB_OK | MB_ICONERROR);
		free_snapshot_names();
		fill_snapshots();
		return;
	}
	item.iItem = (int)SendMessageW(snapshots_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
	item.mask = LVIF_PARAM;
	if (item.iItem < 0 || !SendMessageW(snapshots_list, LVM_GETITEMW, 0, (LPARAM)&item) || !item.lParam) {
		MessageBoxW(main_window, L"Choose a snapshot in the list first.", L"Snapshots", MB_OK | MB_ICONINFORMATION);
		return;
	}
	name = (const WCHAR *)item.lParam;
	if (id == ID_FIRST + 51) {
		wsprintfW(question, L"The prefix goes back to the snapshot %s. Close the programs that run in it first; the prefix as it is now is kept "
			L"as a snapshot.", name);
		if (MessageBoxW(main_window, question, L"Restore", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
			return;
		if (!RavenSnapshotRestore(name))
			MessageBoxW(main_window, L"The snapshot could not be restored.", L"Snapshots", MB_OK | MB_ICONERROR);
	} else {
		wsprintfW(question, L"The snapshot %s will be deleted.", name);
		if (MessageBoxW(main_window, question, L"Delete", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
			return;
		if (!RavenSnapshotDelete(name))
			MessageBoxW(main_window, L"The snapshot could not be deleted.", L"Snapshots", MB_OK | MB_ICONERROR);
	}
	free_snapshot_names();
	fill_snapshots();
}

/* Default apps ------------------------------------------------------------------------------------------------- */

/* The Linux programs the desktop lists, "ID<tab>Name" a line, as the runtime found them. */
static WCHAR *host_text, *host_ids[256], *host_names[256];
static int host_count;
static HWND host_choice, open_choice;

static void read_host_apps(void)
{
	DWORD size = RavenHostApps(NULL, 0);
	WCHAR *at;
	host_count = 0;
	if (host_text)
		HeapFree(GetProcessHeap(), 0, host_text);
	host_text = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size + 1) * sizeof(WCHAR));
	if (!host_text)
		return;
	if (size)
		RavenHostApps(host_text, size + 1);
	for (at = host_text; *at && host_count < 256;) {
		WCHAR *tab = at, *end;
		while (*tab && *tab != L'\t' && *tab != L'\n')
			++tab;
		for (end = tab; *end && *end != L'\n'; ++end)
			;
		if (*tab == L'\t') {
			*tab = 0;
			host_ids[host_count] = at;
			host_names[host_count++] = tab + 1;
		}
		at = *end ? end + 1 : end;
		*end = 0;
	}
}

/* The value of a "word NAME<tab>VALUE" line of sandbox.conf, with its name; NULL when the line is not one. */
static const WCHAR *pair_line(const WCHAR *line, const WCHAR *word, WCHAR *name, int capacity)
{
	const WCHAR *tab;
	if (!starts_with(line, word))
		return NULL;
	line += lstrlenW(word);
	for (tab = line; *tab && *tab != L'\t'; ++tab)
		;
	if (!*tab || tab == line)
		return NULL;
	lstrcpynW(name, line, (int)(tab - line) + 1 < capacity ? (int)(tab - line) + 1 : capacity);
	return tab + 1;
}

static void fill_defaults(void)
{
	int index;
	WCHAR name[128], text[160];
	SendMessageW(grants_list, LVM_DELETEALLITEMS, 0, 0);
	SendMessageW(log_list, LVM_DELETEALLITEMS, 0, 0);
	SendMessageW(open_choice, CB_RESETCONTENT, 0, 0);
	SendMessageW(open_choice, CB_ADDSTRING, 0, (LPARAM)L"A program of the prefix");
	SendMessageW(open_choice, CB_ADDSTRING, 0, (LPARAM)L"Ask every time");
	for (index = 0; index < config_count; ++index) {
		const WCHAR *value;
		if ((value = pair_line(config_lines[index], L"host-app ", name, 128))) {
			const int row = add_list_row(grants_list, name, index);
			set_cell(grants_list, row, 1, value);
			SendMessageW(open_choice, CB_ADDSTRING, 0, (LPARAM)name);
		}
	}
	for (index = 0; index < config_count; ++index) {
		const WCHAR *value;
		if ((value = pair_line(config_lines[index], L"handler ", name, 128))) {
			const int row = add_list_row(log_list, name, index);
			if (starts_with(value, L"linux:"))
				wsprintfW(text, L"%s, on Linux", value + 6);
			set_cell(log_list, row, 1, !lstrcmpW(value, L"windows") ? L"A program of the prefix" : !lstrcmpW(value, L"ask") ? L"Ask every time" :
				starts_with(value, L"linux:") ? text : value);
		}
	}
	SendMessageW(open_choice, CB_SETCURSEL, 0, 0);
}

static void show_defaults(void)
{
	static const WCHAR *const app_titles[] = {L"Linux program", L"Desktop entry"};
	static const int app_widths[] = {260, 360};
	static const WCHAR *const open_titles[] = {L"Kind", L"Opened by"};
	static const int open_widths[] = {200, 420};
	Row *row;
	int index;
	read_config();
	read_host_apps();
	row = add_row(L"Show a Linux program to Windows programs", L"It appears as C:\\Program Files\\Raven\\Host\\NAME.exe and gets only the files they reach", 0);
	row->control = host_choice = add_child(L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_FIRST + 63);
	for (index = 0; index < host_count; ++index)
		SendMessageW(host_choice, CB_ADDSTRING, 0, (LPARAM)host_names[index]);
	SendMessageW(host_choice, CB_SETCURSEL, 0, 0);
	row = add_row(L"Open a kind of file or address with", L"Type the kind (.pdf, mailto) in the box under this, then choose who opens it", 1);
	row->control = open_choice = add_child(L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_FIRST + 64);
	list_buttons[0] = add_child(L"Button", L"Show it", BS_PUSHBUTTON, ID_FIRST + 60);
	list_buttons[1] = add_child(L"Edit", L".pdf", ES_AUTOHSCROLL | WS_BORDER, ID_FIRST + 65);
	list_buttons[2] = add_child(L"Button", L"Set", BS_PUSHBUTTON, ID_FIRST + 61);
	list_buttons[3] = add_child(L"Button", L"Remove", BS_PUSHBUTTON, ID_FIRST + 62);
	grants_list = make_list(ID_FIRST + 66, app_titles, app_widths, 2);
	log_list = make_list(ID_FIRST + 67, open_titles, open_widths, 2);
	fill_defaults();
}

static void expose_app(void)
{
	const int chosen = (int)SendMessageW(host_choice, CB_GETCURSEL, 0, 0);
	WCHAR name[64], line[400], existing[128];
	int index, length = 0;
	const WCHAR *at;
	if (chosen < 0 || chosen >= host_count)
		return;
	/* The bridge's name: the program's name, as a file name can hold it. */
	for (at = host_names[chosen]; *at && length < 60; ++at)
		if ((*at >= L'a' && *at <= L'z') || (*at >= L'A' && *at <= L'Z') || (*at >= L'0' && *at <= L'9') || *at == L' ' || *at == L'-' || *at == L'_')
			name[length++] = *at;
	name[length] = 0;
	if (!length)
		lstrcpyW(name, L"Program");
	for (index = 0; index < config_count; ++index)
		if (pair_line(config_lines[index], L"host-app ", existing, 128) && !lstrcmpiW(existing, name))
			return;
	wsprintfW(line, L"host-app %s\t%s", name, host_ids[chosen]);
	add_config_line(line);
	if (write_config())
		fill_defaults();
}

static void set_handler(void)
{
	WCHAR kind[32], line[200], name[128];
	const int chosen = (int)SendMessageW(open_choice, CB_GETCURSEL, 0, 0);
	int index;
	const WCHAR *at;
	GetWindowTextW(list_buttons[1], kind, 32);
	/* A kind is an extension (".pdf") or the scheme of an address ("mailto"). */
	for (at = kind[0] == L'.' ? kind + 1 : kind; *at; ++at)
		if (!((*at >= L'a' && *at <= L'z') || (*at >= L'A' && *at <= L'Z') || (*at >= L'0' && *at <= L'9') || *at == L'-' || *at == L'+'))
			break;
	if (!kind[0] || !kind[1] || *at || chosen < 0) {
		MessageBoxW(main_window, L"Type a kind such as .pdf or mailto first.", L"Default apps", MB_OK | MB_ICONINFORMATION);
		return;
	}
	for (index = config_count - 1; index >= 0; --index)
		if (pair_line(config_lines[index], L"handler ", name, 128) && !lstrcmpiW(name, kind))
			remove_config_line(index);
	if (chosen == 0)
		wsprintfW(line, L"handler %s\twindows", kind);
	else if (chosen == 1)
		wsprintfW(line, L"handler %s\task", kind);
	else {
		SendMessageW(open_choice, CB_GETLBTEXT, chosen, (LPARAM)name);
		wsprintfW(line, L"handler %s\tlinux:%s", kind, name);
	}
	add_config_line(line);
	if (write_config())
		fill_defaults();
}

static void remove_default(void)
{
	HWND list = SendMessageW(log_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED) >= 0 && GetFocus() != grants_list ? log_list : grants_list;
	const int index = selected_param(list);
	if (index < 0) {
		MessageBoxW(main_window, L"Choose a line in a list first.", L"Default apps", MB_OK | MB_ICONINFORMATION);
		return;
	}
	remove_config_line(index);
	if (write_config())
		fill_defaults();
}

/* Open with --------------------------------------------------------------------------------------------------- */

/* control.exe openwith KIND WINDOWS NAME: the question a program's runtime asks when the person chose to be asked who opens a kind of
 * file. The exit code is the answer: 0 for none, 1 for the program of the prefix, 2 and up for the Linux programs the prefix shows, in
 * the order sandbox.conf names them. With "Always" the choice is kept for the kind. */
static HWND open_list, open_always;
static int open_answers[64];

static LRESULT CALLBACK open_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_COMMAND && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL || (LOWORD(wparam) == 3 && HIWORD(wparam) == LBN_DBLCLK))) {
		const int chosen = (int)SendMessageW(open_list, LB_GETCURSEL, 0, 0);
		const int answer = LOWORD(wparam) == IDCANCEL || chosen < 0 ? 0 : open_answers[chosen];
		WCHAR kind[32];
		GetWindowTextW(GetDlgItem(window, 5), kind, 32);
		if (answer && SendMessageW(open_always, BM_GETCHECK, 0, 0) == BST_CHECKED) {
			WCHAR line[200] = L"", name[128];
			int index, exposed = 0;
			for (index = config_count - 1; index >= 0; --index)
				if (pair_line(config_lines[index], L"handler ", name, 128) && !lstrcmpiW(name, kind))
					remove_config_line(index);
			if (answer == 1)
				wsprintfW(line, L"handler %s\twindows", kind);
			for (index = 0; index < config_count && answer > 1; ++index)
				if (pair_line(config_lines[index], L"host-app ", name, 128) && ++exposed == answer - 1)
					wsprintfW(line, L"handler %s\tlinux:%s", kind, name);
			if (line[0]) {
				add_config_line(line);
				write_config();
			}
		}
		PostQuitMessage(answer);
		return 0;
	}
	if (message == WM_CLOSE) {
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

static int open_with(HINSTANCE instance, const WCHAR *kind, BOOL windows, const WCHAR *target)
{
	WNDCLASSEXW klass = {sizeof klass};
	WCHAR question[400], name[128];
	HWND window;
	MSG message;
	int index, count = 0, exposed = 0;
	klass.lpfnWndProc = open_proc;
	klass.hInstance = instance;
	klass.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	klass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	klass.lpszClassName = L"RavenOpenWith";
	RegisterClassExW(&klass);
	read_config();
	window = CreateWindowExW(WS_EX_DLGMODALFRAME, klass.lpszClassName, L"Open with", WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 440,
		360, NULL, NULL, instance, NULL);
	if (!window)
		return 0;
	wsprintfW(question, L"Which program opens %.300s?", target);
	CreateWindowExW(0, L"Static", question, WS_CHILD | WS_VISIBLE, 16, 12, 400, 36, window, (HMENU)4, instance, NULL);
	CreateWindowExW(0, L"Static", kind, WS_CHILD, 0, 0, 0, 0, window, (HMENU)5, instance, NULL);
	open_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"ListBox", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, 16, 52, 392, 180, window,
		(HMENU)3, instance, NULL);
	if (windows) {
		SendMessageW(open_list, LB_ADDSTRING, 0, (LPARAM)L"The program of the prefix for this kind");
		open_answers[count++] = 1;
	}
	for (index = 0; index < config_count && count < 64; ++index)
		if (pair_line(config_lines[index], L"host-app ", name, 128)) {
			WCHAR label[160];
			wsprintfW(label, L"%s, on Linux", name);
			SendMessageW(open_list, LB_ADDSTRING, 0, (LPARAM)label);
			open_answers[count++] = 2 + exposed++;
		}
	SendMessageW(open_list, LB_SETCURSEL, 0, 0);
	open_always = CreateWindowExW(0, L"Button", L"Always use this program for this kind", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, 16, 240,
		392, 24, window, (HMENU)6, instance, NULL);
	CreateWindowExW(0, L"Button", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 228, 276, 86, 28, window, (HMENU)IDOK, instance, NULL);
	CreateWindowExW(0, L"Button", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 322, 276, 86, 28, window, (HMENU)IDCANCEL, instance, NULL);
	ShowWindow(window, SW_SHOWNORMAL);
	while (GetMessageW(&message, NULL, 0, 0) > 0) {
		if (IsDialogMessageW(window, &message))
			continue;
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	return (int)message.wParam;
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
	if (grants_list || snapshots_list) {
		/* Below the page's rows: the buttons of the list, then the list, and on Privacy & security the refusals under it. */
		const int top = 96 + row_count * (ROW_HEIGHT + 4) + row_count * 20 - 8, width = client.right - NAV_WIDTH - 56;
		const int bottom = client.bottom - 24, lists = grants_list ? 2 : 1, height = (bottom - top - 40) / lists;
		int button;
		for (button = 0; button < 4; ++button)
			if (list_buttons[button])
				MoveWindow(list_buttons[button], NAV_WIDTH + 24 + button * 150, top, 140, 30, TRUE);
		MoveWindow(grants_list ? grants_list : snapshots_list, NAV_WIDTH + 24, top + 40, width, height - 12, TRUE);
		if (log_list)
			MoveWindow(log_list, NAV_WIDTH + 24, top + 40 + height, width, height - 12, TRUE);
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
	free_snapshot_names();
	clear_page();
	page = which;
	switch (page) {
	case PAGE_SYSTEM: show_system(); break;
	case PAGE_PERSONALIZATION: show_personalization(); break;
	case PAGE_APPS: show_apps(); break;
	case PAGE_DEFAULTS: show_defaults(); break;
	case PAGE_PRIVACY: show_privacy(); break;
	case PAGE_SNAPSHOTS: show_snapshots(); break;
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
		} else if (page == PAGE_DEFAULTS && id == ID_FIRST + 60) {
			expose_app();
		} else if (page == PAGE_DEFAULTS && id == ID_FIRST + 61) {
			set_handler();
		} else if (page == PAGE_DEFAULTS && id == ID_FIRST + 62) {
			remove_default();
		} else if (page == PAGE_PRIVACY && id == ID_FIRST + 40) {
			toggle_network((HWND)lparam);
		} else if (page == PAGE_PRIVACY && id == ID_FIRST + 41) {
			add_folder();
		} else if (page == PAGE_PRIVACY && (id == ID_FIRST + 42 || id == ID_FIRST + 43)) {
			change_grant(id == ID_FIRST + 43);
		} else if (page == PAGE_SNAPSHOTS && id >= ID_FIRST + 50 && id <= ID_FIRST + 52) {
			snapshot_action(id);
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
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	body_font = make_font(14, FW_NORMAL);
	small_font = make_font(12, FW_NORMAL);
	header_font = make_font(20, FW_SEMIBOLD);
	title_font = make_font(28, FW_SEMIBOLD);
	klass.lpfnWndProc = window_proc;
	klass.hInstance = instance;
	klass.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	klass.lpszClassName = L"RavenControlPanel";
	RegisterClassExW(&klass);
	/* openwith KIND WINDOWS TARGET asks who opens a file (see open_with). */
	if (command_line && starts_with(command_line, L"openwith ")) {
		WCHAR kind[32];
		const WCHAR *at = command_line + 9;
		int length = 0;
		while (*at && *at != L' ' && length < 31)
			kind[length++] = *at++;
		kind[length] = 0;
		while (*at == L' ')
			++at;
		return open_with(instance, kind, *at == L'1', at[0] && at[1] ? at + 2 : L"this file");
	}
	/* A page can be named on the command line, as control.exe opens a panel: system, personalization, apps, defaults, privacy, snapshots or
	 * raven. */
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
		if (!lstrcmpiW(word, page_names[which]))
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
