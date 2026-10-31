/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>

/* The library's start, its window subclassing, and the small functions that do not belong to any one control.
 * The standard controls of the versioned library (button, edit and the rest) are the ones in raven-user32. */

HMODULE COMCTL32_hModule;

void ANIMATE_Register(void);
void ANIMATE_Unregister(void);
void COMBOEX_Register(void);
void COMBOEX_Unregister(void);
void DATETIME_Register(void);
void DATETIME_Unregister(void);
void FLATSB_Register(void);
void FLATSB_Unregister(void);
void HEADER_Register(void);
void HEADER_Unregister(void);
void HOTKEY_Register(void);
void HOTKEY_Unregister(void);
void IPADDRESS_Register(void);
void IPADDRESS_Unregister(void);
void LISTVIEW_Register(void);
void LISTVIEW_Unregister(void);
void MONTHCAL_Register(void);
void MONTHCAL_Unregister(void);
void NATIVEFONT_Register(void);
void NATIVEFONT_Unregister(void);
void PAGER_Register(void);
void PAGER_Unregister(void);
void PROGRESS_Register(void);
void PROGRESS_Unregister(void);
void REBAR_Register(void);
void REBAR_Unregister(void);
void STATUS_Register(void);
void STATUS_Unregister(void);
void SYSLINK_Register(void);
void SYSLINK_Unregister(void);
void TAB_Register(void);
void TAB_Unregister(void);
void TOOLBAR_Register(void);
void TOOLBAR_Unregister(void);
void TOOLTIPS_Register(void);
void TOOLTIPS_Unregister(void);
void TRACKBAR_Register(void);
void TRACKBAR_Unregister(void);
void TREEVIEW_Register(void);
void TREEVIEW_Unregister(void);
void UPDOWN_Register(void);
void UPDOWN_Unregister(void);

/* A class without functions is a standard control, always there because raven-user32 provides it. */
static const struct {
	const WCHAR *name;
	void (*add)(void);
	void (*remove)(void);
} classes[] = {
	{ ANIMATE_CLASSW, ANIMATE_Register, ANIMATE_Unregister },
	{ WC_COMBOBOXEXW, COMBOEX_Register, COMBOEX_Unregister },
	{ DATETIMEPICK_CLASSW, DATETIME_Register, DATETIME_Unregister },
	{ WC_HEADERW, HEADER_Register, HEADER_Unregister },
	{ HOTKEY_CLASSW, HOTKEY_Register, HOTKEY_Unregister },
	{ WC_IPADDRESSW, IPADDRESS_Register, IPADDRESS_Unregister },
	{ WC_LISTVIEWW, LISTVIEW_Register, LISTVIEW_Unregister },
	{ MONTHCAL_CLASSW, MONTHCAL_Register, MONTHCAL_Unregister },
	{ WC_NATIVEFONTCTLW, NATIVEFONT_Register, NATIVEFONT_Unregister },
	{ WC_PAGESCROLLERW, PAGER_Register, PAGER_Unregister },
	{ PROGRESS_CLASSW, PROGRESS_Register, PROGRESS_Unregister },
	{ REBARCLASSNAMEW, REBAR_Register, REBAR_Unregister },
	{ STATUSCLASSNAMEW, STATUS_Register, STATUS_Unregister },
	{ L"SysLink", SYSLINK_Register, SYSLINK_Unregister },
	{ WC_TABCONTROLW, TAB_Register, TAB_Unregister },
	{ TOOLBARCLASSNAMEW, TOOLBAR_Register, TOOLBAR_Unregister },
	{ TOOLTIPS_CLASSW, TOOLTIPS_Register, TOOLTIPS_Unregister },
	{ TRACKBAR_CLASSW, TRACKBAR_Register, TRACKBAR_Unregister },
	{ WC_TREEVIEWW, TREEVIEW_Register, TREEVIEW_Unregister },
	{ UPDOWN_CLASSW, UPDOWN_Register, UPDOWN_Unregister },
	{ L"Button", NULL, NULL },
	{ L"ComboBox", NULL, NULL },
	{ L"ComboLBox", NULL, NULL },
	{ L"Edit", NULL, NULL },
	{ L"ListBox", NULL, NULL },
	{ L"Static", NULL, NULL },
};

#define CLASS_COUNT (sizeof(classes) / sizeof(classes[0]))

static LANGID interface_language;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	size_t index;
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(instance);
		COMCTL32_hModule = instance;
		for (index = 0; index < CLASS_COUNT; index++) {
			if (classes[index].add)
				classes[index].add();
		}
		FLATSB_Register();
		break;
	case DLL_PROCESS_DETACH:
		if (reserved)
			break;
		for (index = 0; index < CLASS_COUNT; index++) {
			if (classes[index].remove)
				classes[index].remove();
		}
		FLATSB_Unregister();
		break;
	}
	return TRUE;
}

void WINAPI InitCommonControls(void)
{
}

BOOL WINAPI InitCommonControlsEx(const INITCOMMONCONTROLSEX *request)
{
	return request && request->dwSize == sizeof(*request);
}

/* The system asks for this when a class is looked up after someone took it away. */
BOOLEAN WINAPI RegisterClassNameW(const WCHAR *name)
{
	size_t index;
	for (index = 0; index < CLASS_COUNT; index++) {
		if (!lstrcmpiW(name, classes[index].name)) {
			if (classes[index].add)
				classes[index].add();
			return TRUE;
		}
	}
	return FALSE;
}

HRESULT WINAPI DllGetVersion(DLLVERSIONINFO *info)
{
	if (!info || (info->cbSize != sizeof(DLLVERSIONINFO) && info->cbSize != sizeof(DLLVERSIONINFO2)))
		return E_INVALIDARG;
	info->dwMajorVersion = 6;
	info->dwMinorVersion = 10;
	info->dwBuildNumber = 7601;
	info->dwPlatformID = DLLVER_PLATFORM_NT;
	return S_OK;
}

HRESULT WINAPI DllInstall(BOOL install, const WCHAR *command)
{
	return S_OK;
}

LANGID WINAPI GetMUILanguage(void)
{
	return interface_language;
}

void WINAPI InitMUILanguage(LANGID language)
{
	interface_language = language;
}

BOOL WINAPI _TrackMouseEvent(TRACKMOUSEEVENT *request)
{
	return TrackMouseEvent(request);
}

/* Subclassing */

#define SUBCLASS_PROPERTY L"RavenSubclassChain"

typedef struct subclass_entry {
	struct subclass_entry *next;
	SUBCLASSPROC proc;
	UINT_PTR id;
	DWORD_PTR data;
} subclass_entry;

/* One for each message still being handled on the window: nested sends keep their own place in the chain. */
typedef struct subclass_frame {
	struct subclass_frame *outer;
	subclass_entry *upcoming;
} subclass_frame;

typedef struct {
	subclass_entry *chain;
	subclass_frame *frames;
	WNDPROC original;
} subclass_chain;

static LRESULT CALLBACK chain_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

static subclass_chain *chain_of(HWND window)
{
	return GetPropW(window, SUBCLASS_PROPERTY);
}

static void leave_window(HWND window, subclass_chain *chain)
{
	if ((WNDPROC)GetWindowLongPtrW(window, GWLP_WNDPROC) != chain_proc)
		return;
	SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)chain->original);
	RemovePropW(window, SUBCLASS_PROPERTY);
	HeapFree(GetProcessHeap(), 0, chain);
}

BOOL WINAPI SetWindowSubclass(HWND window, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR data)
{
	subclass_chain *chain;
	subclass_entry *entry;
	if (!IsWindow(window) || !proc)
		return FALSE;
	chain = chain_of(window);
	if (!chain) {
		chain = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*chain));
		if (!chain)
			return FALSE;
		SetPropW(window, SUBCLASS_PROPERTY, chain);
		chain->original = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)chain_proc);
	}
	for (entry = chain->chain; entry; entry = entry->next) {
		if (entry->proc == proc && entry->id == id) {
			entry->data = data;
			return TRUE;
		}
	}
	entry = HeapAlloc(GetProcessHeap(), 0, sizeof(*entry));
	if (!entry)
		return FALSE;
	entry->proc = proc;
	entry->id = id;
	entry->data = data;
	entry->next = chain->chain;
	chain->chain = entry;
	return TRUE;
}

BOOL WINAPI GetWindowSubclass(HWND window, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR *data)
{
	const subclass_chain *chain = IsWindow(window) ? chain_of(window) : NULL;
	const subclass_entry *entry;
	if (!chain || !proc)
		return FALSE;
	for (entry = chain->chain; entry; entry = entry->next) {
		if (entry->proc == proc && entry->id == id) {
			if (data)
				*data = entry->data;
			return TRUE;
		}
	}
	return FALSE;
}

BOOL WINAPI RemoveWindowSubclass(HWND window, SUBCLASSPROC proc, UINT_PTR id)
{
	subclass_chain *chain = IsWindow(window) ? chain_of(window) : NULL;
	subclass_entry **slot;
	if (!chain || !proc)
		return FALSE;
	for (slot = &chain->chain; *slot; slot = &(*slot)->next) {
		subclass_entry *entry = *slot;
		if (entry->proc == proc && entry->id == id) {
			subclass_frame *frame;
			for (frame = chain->frames; frame; frame = frame->outer) {
				if (frame->upcoming == entry)
					frame->upcoming = entry->next;
			}
			*slot = entry->next;
			HeapFree(GetProcessHeap(), 0, entry);
			if (!chain->chain && !chain->frames)
				leave_window(window, chain);
			return TRUE;
		}
	}
	return FALSE;
}

LRESULT WINAPI DefSubclassProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	subclass_chain *chain = chain_of(window);
	subclass_entry *entry;
	if (!chain || !chain->frames)
		return DefWindowProcW(window, message, wparam, lparam);
	entry = chain->frames->upcoming;
	if (!entry)
		return CallWindowProcW(chain->original, window, message, wparam, lparam);
	chain->frames->upcoming = entry->next;
	return entry->proc(window, message, wparam, lparam, entry->id, entry->data);
}

static LRESULT CALLBACK chain_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	subclass_chain *chain = chain_of(window);
	subclass_frame frame;
	LRESULT result;
	if (!chain)
		return DefWindowProcW(window, message, wparam, lparam);
	frame.outer = chain->frames;
	frame.upcoming = chain->chain;
	chain->frames = &frame;
	result = DefSubclassProc(window, message, wparam, lparam);
	chain->frames = frame.outer;
	if (message == WM_NCDESTROY && !chain->frames) {
		while (chain->chain) {
			subclass_entry *entry = chain->chain;
			chain->chain = entry->next;
			HeapFree(GetProcessHeap(), 0, entry);
		}
		RemovePropW(window, SUBCLASS_PROPERTY);
		HeapFree(GetProcessHeap(), 0, chain);
		return result;
	}
	if (!chain->chain && !chain->frames)
		leave_window(window, chain);
	return result;
}

/* Icons */

HRESULT WINAPI LoadIconWithScaleDown(HINSTANCE module, const WCHAR *name, int width, int height, HICON *icon)
{
	if (!icon)
		return E_INVALIDARG;
	*icon = NULL;
	if (!name || width <= 0 || height <= 0)
		return E_INVALIDARG;
	if (module)
		*icon = LoadImageW(module, name, IMAGE_ICON, width, height, 0);
	else if (IS_INTRESOURCE(name))
		*icon = CopyImage(LoadIconW(NULL, name), IMAGE_ICON, width, height, 0);
	else
		*icon = LoadImageW(NULL, name, IMAGE_ICON, width, height, LR_LOADFROMFILE);
	return *icon ? S_OK : HRESULT_FROM_WIN32(ERROR_RESOURCE_TYPE_NOT_FOUND);
}

HRESULT WINAPI LoadIconMetric(HINSTANCE module, const WCHAR *name, int metric, HICON *icon)
{
	if (!icon)
		return E_INVALIDARG;
	*icon = NULL;
	if (metric == LIM_SMALL)
		return LoadIconWithScaleDown(module, name, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), icon);
	if (metric == LIM_LARGE)
		return LoadIconWithScaleDown(module, name, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), icon);
	return E_INVALIDARG;
}

BOOL WINAPI MirrorIcon(HICON *first, HICON *second)
{
	return FALSE;
}

/* Text */

static WCHAR *widen(const char *text)
{
	WCHAR *wide;
	int length;
	if (!text)
		return NULL;
	length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	wide = HeapAlloc(GetProcessHeap(), 0, length * sizeof(WCHAR));
	if (wide)
		MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length);
	return wide;
}

static void release_text(WCHAR *text)
{
	if (text)
		HeapFree(GetProcessHeap(), 0, text);
}

/* The text is split at tabs into the parts that go to the left, the middle and the right. */
void WINAPI DrawStatusTextW(HDC dc, const RECT *given, const WCHAR *text, UINT style)
{
	static const UINT aligned[] = { DT_LEFT, DT_CENTER, DT_RIGHT };
	RECT area = *given;
	int part = 0, saved;
	const WCHAR *walk = text;
	if (!(style & SBT_NOBORDERS))
		DrawEdge(dc, &area, (style & SBT_POPOUT) ? BDR_RAISEDINNER : BDR_SUNKENOUTER, BF_RECT | BF_ADJUST);
	else
		InflateRect(&area, -1, -1);
	FillRect(dc, &area, GetSysColorBrush(COLOR_3DFACE));
	if (!text)
		return;
	saved = SetBkMode(dc, TRANSPARENT);
	area.left += 3;
	area.right -= 3;
	while (part < 3) {
		const WCHAR *end = walk;
		while (*end && *end != '\t')
			end++;
		if (end != walk)
			DrawTextW(dc, walk, (int)(end - walk), &area, aligned[part] | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | ((style & SBT_RTLREADING) ? DT_RTLREADING : 0));
		if (!*end)
			break;
		walk = end + 1;
		part++;
	}
	SetBkMode(dc, saved);
}

void WINAPI DrawStatusTextA(HDC dc, const RECT *given, const char *text, UINT style)
{
	WCHAR *wide = widen(text);
	DrawStatusTextW(dc, given, wide, style);
	release_text(wide);
}

int WINAPI DrawShadowText(HDC dc, const WCHAR *text, UINT length, RECT *area, DWORD flags, COLORREF color, COLORREF shadow, int x_offset, int y_offset)
{
	const int saved_mode = SetBkMode(dc, TRANSPARENT);
	const COLORREF saved_color = GetTextColor(dc);
	RECT shifted = *area;
	int result;
	OffsetRect(&shifted, x_offset, y_offset);
	SetTextColor(dc, shadow);
	DrawTextW(dc, text, (int)length, &shifted, flags);
	SetTextColor(dc, color);
	result = DrawTextW(dc, text, (int)length, area, flags);
	SetTextColor(dc, saved_color);
	SetBkMode(dc, saved_mode);
	return result;
}

/* Paths break only after their separators. */
static int CALLBACK path_word_break(WCHAR *text, int position, int length, int code)
{
	int walk = position;
	switch (code) {
	case WB_ISDELIMITER:
		return text[position] == '\\' || text[position] == '/';
	case WB_LEFT:
		walk = position - 1;
		while (walk > 0 && text[walk - 1] != '\\' && text[walk - 1] != '/')
			walk--;
		return walk < 0 ? 0 : walk;
	case WB_RIGHT:
		while (walk < length && text[walk] != '\\' && text[walk] != '/')
			walk++;
		return walk < length ? walk + 1 : length;
	}
	return 0;
}

LRESULT WINAPI SetPathWordBreakProc(HWND window, BOOL enable)
{
	return SendMessageW(window, EM_SETWORDBREAKPROC, 0, (LPARAM)(enable ? (EDITWORDBREAKPROCW)path_word_break : NULL));
}

/* Control factories */

HWND WINAPI CreateStatusWindowW(LONG style, const WCHAR *text, HWND parent, UINT id)
{
	return CreateWindowExW(0, STATUSCLASSNAMEW, text, style, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, parent, (HMENU)(ULONG_PTR)id, COMCTL32_hModule, NULL);
}

HWND WINAPI CreateStatusWindowA(LONG style, const char *text, HWND parent, UINT id)
{
	return CreateWindowExA(0, STATUSCLASSNAMEA, text, style, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, parent, (HMENU)(ULONG_PTR)id, COMCTL32_hModule, NULL);
}

HWND WINAPI CreateUpDownControl(DWORD style, int x, int y, int width, int height, HWND parent, int id, HINSTANCE instance, HWND buddy, int upper, int lower, int position)
{
	HWND control = CreateWindowExW(0, UPDOWN_CLASSW, NULL, style, x, y, width, height, parent, (HMENU)(LONG_PTR)id, instance, NULL);
	if (!control)
		return NULL;
	if (buddy)
		SendMessageW(control, UDM_SETBUDDY, (WPARAM)buddy, 0);
	SendMessageW(control, UDM_SETRANGE, 0, MAKELONG(upper, lower));
	SendMessageW(control, UDM_SETPOS, 0, MAKELONG(position, 0));
	return control;
}

HWND WINAPI CreateToolbarEx(HWND parent, DWORD style, UINT id, int bitmaps, HINSTANCE instance, UINT_PTR bitmap, const TBBUTTON *buttons, int count, int button_width, int button_height, int bitmap_width, int bitmap_height, UINT structure_size)
{
	HWND toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL, style | WS_CHILD, 0, 0, 100, 30, parent, (HMENU)(ULONG_PTR)id, instance, NULL);
	if (!toolbar)
		return NULL;
	SendMessageW(toolbar, TB_BUTTONSTRUCTSIZE, structure_size, 0);
	/* Both pairs size the images, the button's last: the button itself follows from them. A zero leaves a pair out. */
	if (bitmap_width && bitmap_height)
		SendMessageW(toolbar, TB_SETBITMAPSIZE, 0, MAKELONG(bitmap_width, bitmap_height));
	if (button_width && button_height)
		SendMessageW(toolbar, TB_SETBITMAPSIZE, 0, MAKELONG(button_width, button_height));
	if (bitmaps > 0) {
		TBADDBITMAP add;
		add.hInst = instance;
		add.nID = bitmap;
		SendMessageW(toolbar, TB_ADDBITMAP, bitmaps, (LPARAM)&add);
	}
	if (buttons && count > 0)
		SendMessageW(toolbar, TB_ADDBUTTONSW, count, (LPARAM)buttons);
	return toolbar;
}

HWND WINAPI CreateToolbar(HWND parent, DWORD style, UINT id, int bitmaps, HINSTANCE instance, UINT bitmap, const TBBUTTON *buttons, int count)
{
	return CreateToolbarEx(parent, style, id, bitmaps, instance, bitmap, buttons, count, 0, 0, 0, 0, sizeof(TBBUTTON));
}

/* A palette bitmap resource with some of its colors swapped, by default the greys for the system's button colors. */
HBITMAP WINAPI CreateMappedBitmap(HINSTANCE instance, INT_PTR id, UINT flags, COLORMAP *map, int map_count)
{
	COLORMAP standard[4];
	HRSRC resource = FindResourceW(instance, (const WCHAR *)id, (const WCHAR *)RT_BITMAP);
	const BITMAPINFOHEADER *found;
	BITMAPINFOHEADER *copy;
	HBITMAP result = NULL;
	HDC screen, target;
	DWORD colors, index;
	SIZE_T header_size;
	int entry;
	if (!resource)
		return NULL;
	found = LockResource(LoadResource(instance, resource));
	if (!found)
		return NULL;
	if (!map) {
		standard[0].from = RGB(0, 0, 0);
		standard[0].to = GetSysColor(COLOR_BTNTEXT);
		standard[1].from = RGB(128, 128, 128);
		standard[1].to = GetSysColor(COLOR_BTNSHADOW);
		standard[2].from = RGB(192, 192, 192);
		standard[2].to = GetSysColor(COLOR_BTNFACE);
		standard[3].from = RGB(255, 255, 255);
		standard[3].to = GetSysColor(COLOR_BTNHIGHLIGHT);
		map = standard;
		map_count = 4;
	}
	colors = found->biClrUsed ? found->biClrUsed : found->biBitCount <= 8 ? 1u << found->biBitCount : 0;
	header_size = found->biSize + colors * sizeof(RGBQUAD);
	copy = HeapAlloc(GetProcessHeap(), 0, header_size);
	if (!copy)
		return NULL;
	CopyMemory(copy, found, header_size);
	for (index = 0; index < colors; index++) {
		RGBQUAD *color = (RGBQUAD *)((BYTE *)copy + found->biSize) + index;
		const COLORREF value = RGB(color->rgbRed, color->rgbGreen, color->rgbBlue);
		for (entry = 0; entry < map_count; entry++) {
			if (map[entry].from == value) {
				color->rgbRed = GetRValue(map[entry].to);
				color->rgbGreen = GetGValue(map[entry].to);
				color->rgbBlue = GetBValue(map[entry].to);
				break;
			}
		}
	}
	screen = GetDC(NULL);
	target = CreateCompatibleDC(screen);
	result = CreateCompatibleBitmap(screen, found->biWidth, found->biHeight < 0 ? -found->biHeight : found->biHeight);
	if (result) {
		HGDIOBJ previous = SelectObject(target, result);
		const int height = found->biHeight < 0 ? -found->biHeight : found->biHeight;
		StretchDIBits(target, 0, 0, found->biWidth, height, 0, 0, found->biWidth, height, (const BYTE *)found + header_size, (const BITMAPINFO *)copy, DIB_RGB_COLORS, SRCCOPY);
		SelectObject(target, previous);
	}
	DeleteDC(target);
	ReleaseDC(NULL, screen);
	HeapFree(GetProcessHeap(), 0, copy);
	return result;
}

/* Menus */

/* The list holds pairs after a first pair for the menu itself: a command, then a control it shows or hides. */
BOOL WINAPI ShowHideMenuCtl(HWND window, UINT_PTR command, LPINT list)
{
	HMENU menu;
	const INT *walk;
	if (!list)
		return FALSE;
	menu = (HMENU)(LONG_PTR)list[1];
	for (walk = list + 2; walk[0]; walk += 2) {
		if ((UINT_PTR)walk[0] == command) {
			const BOOL shown = (GetMenuState(menu, (UINT)command, MF_BYCOMMAND) & MF_CHECKED) != 0;
			HWND control = GetDlgItem(window, walk[1]);
			CheckMenuItem(menu, (UINT)command, shown ? MF_UNCHECKED : MF_CHECKED);
			if (control)
				ShowWindow(control, shown ? SW_HIDE : SW_SHOW);
			return TRUE;
		}
	}
	return FALSE;
}

void WINAPI GetEffectiveClientRect(HWND window, RECT *area, const INT *list)
{
	GetClientRect(window, area);
	if (!list)
		return;
	/* After the same first pair, every visible control in the list takes its rectangle out. */
	for (list += 2; list[0]; list += 2) {
		HWND control = GetDlgItem(window, list[1]);
		RECT taken;
		if (control && IsWindowVisible(control)) {
			GetWindowRect(control, &taken);
			MapWindowPoints(NULL, window, (POINT *)&taken, 2);
			SubtractRect(area, area, &taken);
		}
	}
}

void WINAPI MenuHelp(UINT message, WPARAM wparam, LPARAM lparam, HMENU main_menu, HINSTANCE instance, HWND status, UINT *ids)
{
	WCHAR text[256];
	UINT offset = 0;
	if (message == WM_COMMAND) {
		SendMessageW(status, SB_SIMPLE, FALSE, 0);
		return;
	}
	if (message != WM_MENUSELECT)
		return;
	if (HIWORD(wparam) == 0xFFFF && !lparam) {
		SendMessageW(status, SB_SIMPLE, FALSE, 0);
		return;
	}
	text[0] = 0;
	SendMessageW(status, SB_SIMPLE, TRUE, 0);
	if (HIWORD(wparam) & MF_POPUP) {
		/* Popups are listed by handle, the first pair being for the commands. */
		const HMENU opened = GetSubMenu((HMENU)lparam, LOWORD(wparam));
		const UINT *walk = ids + 2;
		for (; walk[0]; walk += 2) {
			if ((HMENU)(ULONG_PTR)walk[1] == opened) {
				offset = walk[0];
				break;
			}
		}
	}
	else if (ids) {
		offset = ids[0] + LOWORD(wparam);
	}
	if (offset)
		LoadStringW(instance, offset, text, 256);
	SendMessageW(status, SB_SETTEXTW, SB_SIMPLEID | SBT_NOBORDERS, (LPARAM)text);
}

/* Entry points that only ever existed for the shell's own use. */

HWND WINAPI CreatePage(DWORD parameter, HWND parent)
{
	return NULL;
}

HWND WINAPI CreateProxyPage(DWORD first, DWORD second)
{
	return NULL;
}

void WINAPI DoReaderMode(void *info)
{
}
