/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>
#include <prsht.h>

/* A property sheet is a dialog holding either a tab control or the wizard buttons, and one modeless child dialog per page,
 * made the first time the page is shown. A page handle owns a private copy of the caller's description, which is what the
 * page's callback and WM_INITDIALOG receive, in the character set it was given in. */

#define PAGE_MAGIC 0x50414745
#define TAB_ID 12320
#define APPLY_ID 12321
#define BACK_ID 12323
#define NEXT_ID 12324
#define FINISH_ID 12325
#define LINE_ID 12326

#define MARGIN_DLU 4
#define BUTTON_WIDTH_DLU 50
#define BUTTON_HEIGHT_DLU 14
#define DEFAULT_PAGE_WIDTH 212
#define DEFAULT_PAGE_HEIGHT 188
#define HEADER_HEIGHT 58
#define WATERMARK_WIDTH 164

#ifndef PSCB_BUTTONPRESSED
#define PSCB_BUTTONPRESSED 3
#endif
#ifndef PSH_AEROWIZARD
#define PSH_AEROWIZARD 0x00004000
#endif
#ifndef PSM_SHOWWIZBUTTONS
#define PSM_SHOWWIZBUTTONS (WM_USER + 138)
#define PSM_ENABLEWIZBUTTONS (WM_USER + 139)
#define PSM_SETBUTTONTEXTW (WM_USER + 140)
#define PSM_SETBUTTONTEXT PSM_SETBUTTONTEXTW
#endif
#ifndef PSWIZB_CANCEL
#define PSWIZB_CANCEL 0x00000010
#endif

typedef struct page_handle {
	DWORD magic;
	BOOL unicode;
	/* The caller's structure with everything past the known fields, as long as dwSize says. */
	PROPSHEETPAGEW *raw;
	void *owned_title;
	WCHAR *title, *header_title, *header_subtitle;
	BYTE *template_copy;
	HWND window;
	HICON icon;
	short width, height;
	WORD resource_id;
	BOOL dirty;
} page_handle;

typedef struct {
	HWND dialog, tab;
	PROPSHEETHEADERW header;
	WCHAR *caption;
	HBITMAP watermark, header_bitmap;
	HIMAGELIST images;
	HFONT title_font;
	page_handle **pages;
	int page_count, page_capacity, current;
	int page_width, page_height;
	BOOL unicode, wizard, wizard97, modeless, ended, any_header, restart, reboot;
	INT_PTR result;
	UINT last_button;
} sheet_state;

static void *allocate(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void release(void *block)
{
	if (block)
		HeapFree(GetProcessHeap(), 0, block);
}

/* A given text is the characters or the number of a string in the module; the result is always a private wide copy. */
static WCHAR *copy_text(const void *given, BOOL unicode, HINSTANCE module)
{
	WCHAR number_text[512];
	const WCHAR *source = given;
	WCHAR *copy;
	int length;
	if (!given)
		return NULL;
	if (IS_INTRESOURCE(given)) {
		length = LoadStringW(module, (UINT)(ULONG_PTR)given, number_text, 512);
		if (!length)
			return NULL;
		source = number_text;
	}
	else if (!unicode) {
		length = MultiByteToWideChar(CP_ACP, 0, given, -1, NULL, 0);
		copy = allocate(length * sizeof(WCHAR));
		if (copy)
			MultiByteToWideChar(CP_ACP, 0, given, -1, copy, length);
		return copy;
	}
	length = lstrlenW(source) + 1;
	copy = allocate(length * sizeof(WCHAR));
	if (copy)
		CopyMemory(copy, source, length * sizeof(WCHAR));
	return copy;
}

/* Dialog templates */

static const WORD *skip_text(const WORD *walk)
{
	if (*walk == 0xFFFF)
		return walk + 2;
	while (*walk)
		walk++;
	return walk + 1;
}

static BOOL is_extended(const BYTE *template_data)
{
	return ((const WORD *)template_data)[1] == 0xFFFF;
}

static DWORD *style_of(BYTE *template_data)
{
	return (DWORD *)(template_data + (is_extended(template_data) ? 12 : 0));
}

static void size_of(const BYTE *template_data, short *width, short *height)
{
	const short *fields = (const short *)(template_data + (is_extended(template_data) ? 22 : 14));
	*width = fields[0];
	*height = fields[1];
}

/* Gives the template's total length and where its caption sits. */
static SIZE_T walk_template(const BYTE *template_data, const WCHAR **caption)
{
	const BOOL extended = is_extended(template_data);
	const DWORD style = *style_of((BYTE *)template_data);
	const WORD count = *(const WORD *)(template_data + (extended ? 16 : 8));
	const WORD *walk = (const WORD *)(template_data + (extended ? 26 : 18));
	WORD index;
	walk = skip_text(walk);
	walk = skip_text(walk);
	if (caption)
		*caption = walk;
	walk = skip_text(walk);
	if (style & DS_SETFONT) {
		walk += extended ? 3 : 1;
		walk = skip_text(walk);
	}
	for (index = 0; index < count; index++) {
		WORD extra;
		walk = (const WORD *)(((ULONG_PTR)walk + 3) & ~(ULONG_PTR)3);
		walk = (const WORD *)((const BYTE *)walk + (extended ? 24 : 18));
		walk = skip_text(walk);
		walk = skip_text(walk);
		extra = *walk++;
		walk = (const WORD *)((const BYTE *)walk + extra);
	}
	return (const BYTE *)walk - template_data;
}

/* Finds the page's template and makes a child dialog of a copy of it. */
static BOOL load_template(page_handle *page)
{
	const PROPSHEETPAGEW *raw = page->raw;
	const BYTE *found;
	SIZE_T size;
	DWORD *style;
	if (page->template_copy)
		return TRUE;
	if (raw->dwFlags & PSP_DLGINDIRECT) {
		found = (const BYTE *)raw->pResource;
		if (!found)
			return FALSE;
		size = walk_template(found, NULL);
	}
	else {
		HRSRC resource = page->unicode ? FindResourceW(raw->hInstance, raw->pszTemplate, (LPCWSTR)RT_DIALOG) : FindResourceA(raw->hInstance, (LPCSTR)raw->pszTemplate, (LPCSTR)RT_DIALOG);
		HGLOBAL loaded;
		if (!resource)
			return FALSE;
		loaded = LoadResource(raw->hInstance, resource);
		size = SizeofResource(raw->hInstance, resource);
		found = loaded ? LockResource(loaded) : NULL;
		if (!found)
			return FALSE;
		if (IS_INTRESOURCE(raw->pszTemplate))
			page->resource_id = LOWORD(raw->pszTemplate);
	}
	page->template_copy = allocate(size);
	if (!page->template_copy)
		return FALSE;
	CopyMemory(page->template_copy, found, size);
	style = style_of(page->template_copy);
	*style &= ~(WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_VISIBLE | WS_DISABLED | WS_BORDER | DS_MODALFRAME | DS_CONTEXTHELP | DS_CENTER | DS_ABSALIGN);
	*style |= WS_CHILD | WS_TABSTOP | DS_CONTROL | DS_3DLOOK;
	size_of(page->template_copy, &page->width, &page->height);
	if (!page->title) {
		const WCHAR *caption;
		walk_template(page->template_copy, &caption);
		if (*caption != 0xFFFF)
			page->title = copy_text(caption, TRUE, NULL);
	}
	return TRUE;
}

/* Pages */

static page_handle *make_page(const void *description, BOOL unicode)
{
	const PROPSHEETPAGEW *given = description;
	page_handle *page;
	if (!given || given->dwSize < (unicode ? PROPSHEETPAGEW_V1_SIZE : PROPSHEETPAGEA_V1_SIZE))
		return NULL;
	page = allocate(sizeof(*page));
	if (!page)
		return NULL;
	page->raw = allocate(given->dwSize);
	if (!page->raw) {
		release(page);
		return NULL;
	}
	page->magic = PAGE_MAGIC;
	page->unicode = unicode;
	CopyMemory(page->raw, given, given->dwSize);

	if (given->dwFlags & PSP_USETITLE) {
		page->title = copy_text(given->pszTitle, unicode, given->hInstance);
		/* The narrow copy lives as long as the handle, as a description handed to the callback must. */
		if (!unicode && !IS_INTRESOURCE(given->pszTitle)) {
			const SIZE_T size = lstrlenA((const char *)given->pszTitle) + 1;
			page->owned_title = allocate(size);
			if (page->owned_title) {
				CopyMemory(page->owned_title, given->pszTitle, size);
				page->raw->pszTitle = page->owned_title;
			}
		}
	}
	if (given->dwSize > PROPSHEETPAGEA_V1_SIZE) {
		if (given->dwFlags & PSP_USEHEADERTITLE)
			page->header_title = copy_text(given->pszHeaderTitle, unicode, given->hInstance);
		if (given->dwFlags & PSP_USEHEADERSUBTITLE)
			page->header_subtitle = copy_text(given->pszHeaderSubTitle, unicode, given->hInstance);
	}
	if (given->dwFlags & PSP_USEHICON)
		page->icon = given->hIcon;
	else if (given->dwFlags & PSP_USEICONID)
		page->icon = LoadImageW(given->hInstance, given->pszIcon, IMAGE_ICON, 16, 16, LR_SHARED);
	if ((given->dwFlags & PSP_USEREFPARENT) && given->pcRefParent)
		InterlockedIncrement((LONG *)given->pcRefParent);
	if ((given->dwFlags & PSP_USECALLBACK) && given->pfnCallback && given->dwSize > PROPSHEETPAGEA_V1_SIZE)
		given->pfnCallback(NULL, PSPCB_ADDREF, page->raw);
	return page;
}

static void free_page(page_handle *page)
{
	const PROPSHEETPAGEW *raw = page->raw;
	if ((raw->dwFlags & PSP_USECALLBACK) && raw->pfnCallback)
		raw->pfnCallback(NULL, PSPCB_RELEASE, page->raw);
	if ((raw->dwFlags & PSP_USEREFPARENT) && raw->pcRefParent)
		InterlockedDecrement((LONG *)raw->pcRefParent);
	page->magic = 0;
	release(page->owned_title);
	release(page->title);
	release(page->header_title);
	release(page->header_subtitle);
	release(page->template_copy);
	release(page->raw);
	release(page);
}

HPROPSHEETPAGE WINAPI CreatePropertySheetPageW(LPCPROPSHEETPAGEW description)
{
	return (HPROPSHEETPAGE)make_page(description, TRUE);
}

HPROPSHEETPAGE WINAPI CreatePropertySheetPageA(LPCPROPSHEETPAGEA description)
{
	return (HPROPSHEETPAGE)make_page(description, FALSE);
}

BOOL WINAPI DestroyPropertySheetPage(HPROPSHEETPAGE handle)
{
	page_handle *page = (page_handle *)handle;
	if (!page)
		return FALSE;
	free_page(page);
	return TRUE;
}

/* What the caller passed may be the description itself instead of a handle, read in the sheet's own character set. */
static page_handle *accept_page(const sheet_state *state, const void *given)
{
	page_handle *page;
	if (!given)
		return NULL;
	if (*(const DWORD *)given == PAGE_MAGIC)
		return (page_handle *)given;
	page = make_page(given, state->unicode);
	if (page && !load_template(page)) {
		free_page(page);
		return NULL;
	}
	return page;
}

static LRESULT notify_page(const sheet_state *state, const page_handle *page, UINT code, LPARAM lparam)
{
	PSHNOTIFY message;
	ZeroMemory(&message, sizeof(message));
	message.hdr.hwndFrom = state->dialog;
	message.hdr.code = code;
	message.lParam = lparam;
	return SendMessageW(page->window, WM_NOTIFY, 0, (LPARAM)&message);
}

static BOOL has_header_room(const sheet_state *state, const page_handle *page)
{
	return state->wizard97 && !(page->raw->dwFlags & PSP_HIDEHEADER) && (state->header.dwFlags & PSH_HEADER);
}

/* Layout */

static RECT dlu_rect(HWND dialog, int width, int height)
{
	RECT area;
	SetRect(&area, 0, 0, width, height);
	MapDialogRect(dialog, &area);
	return area;
}

static RECT page_rect(const sheet_state *state, const page_handle *page)
{
	RECT area;
	GetClientRect(state->dialog, &area);
	if (state->wizard) {
		const RECT button = dlu_rect(state->dialog, BUTTON_WIDTH_DLU, BUTTON_HEIGHT_DLU);
		const RECT margin = dlu_rect(state->dialog, MARGIN_DLU, MARGIN_DLU);
		area.bottom -= button.bottom + 3 * margin.bottom;
		if (page && has_header_room(state, page))
			area.top += HEADER_HEIGHT + margin.bottom;
		else if (page && state->wizard97 && (page->raw->dwFlags & PSP_HIDEHEADER) && (state->header.dwFlags & PSH_WATERMARK))
			area.left += WATERMARK_WIDTH;
		InflateRect(&area, -margin.right, 0);
		if (!state->wizard97)
			area.top += margin.bottom;
		return area;
	}
	GetWindowRect(state->tab, &area);
	MapWindowPoints(NULL, state->dialog, (POINT *)&area, 2);
	SendMessageW(state->tab, TCM_ADJUSTRECT, FALSE, (LPARAM)&area);
	return area;
}

static void place_button(HWND dialog, int id, int *right, int top, int width, int height, int gap)
{
	HWND button = GetDlgItem(dialog, id);
	if (!button)
		return;
	if (!(GetWindowLongPtrW(button, GWL_STYLE) & WS_VISIBLE))
		return;
	*right -= width;
	SetWindowPos(button, NULL, *right, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
	*right -= gap;
}

/* Sizes the window around the largest page and sets the buttons in a row under it. */
static void lay_out(sheet_state *state)
{
	HWND dialog = state->dialog;
	const RECT margin = dlu_rect(dialog, MARGIN_DLU, MARGIN_DLU);
	const RECT button = dlu_rect(dialog, BUTTON_WIDTH_DLU, BUTTON_HEIGHT_DLU);
	const RECT page_size = dlu_rect(dialog, state->page_width, state->page_height);
	const int gap = margin.right;
	RECT client, frame;
	int width, height, right, top;
	const DWORD style = (DWORD)GetWindowLongPtrW(dialog, GWL_STYLE), extended = (DWORD)GetWindowLongPtrW(dialog, GWL_EXSTYLE);

	if (state->wizard) {
		width = page_size.right + 2 * margin.right;
		height = page_size.bottom + button.bottom + 3 * margin.bottom + margin.bottom;
		if (state->any_header)
			height += HEADER_HEIGHT + margin.bottom;
	}
	else {
		SetRect(&client, 0, 0, page_size.right, page_size.bottom);
		SendMessageW(state->tab, TCM_ADJUSTRECT, TRUE, (LPARAM)&client);
		width = (client.right - client.left) + 2 * margin.right;
		height = (client.bottom - client.top) + 3 * margin.bottom + button.bottom;
		SetWindowPos(state->tab, NULL, margin.right, margin.bottom, client.right - client.left, client.bottom - client.top, SWP_NOZORDER | SWP_NOACTIVATE);
	}
	SetRect(&frame, 0, 0, width, height);
	AdjustWindowRectEx(&frame, style, FALSE, extended);
	SetWindowPos(dialog, NULL, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

	GetClientRect(dialog, &client);
	right = client.right - margin.right;
	top = client.bottom - margin.bottom - button.bottom;
	{
		HWND help = GetDlgItem(dialog, IDHELP);
		/* A hidden help button keeps its place at the end of the row, past the edge. */
		if (help && !(GetWindowLongPtrW(help, GWL_STYLE) & WS_VISIBLE))
			SetWindowPos(help, NULL, client.right + gap, top, button.right, button.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
		place_button(dialog, IDHELP, &right, top, button.right, button.bottom, gap);
	}
	if (state->wizard) {
		HWND line = GetDlgItem(dialog, LINE_ID);
		place_button(dialog, IDCANCEL, &right, top, button.right, button.bottom, gap);
		{
			int same = right;
			place_button(dialog, FINISH_ID, &same, top, button.right, button.bottom, 0);
			same = right;
			place_button(dialog, NEXT_ID, &same, top, button.right, button.bottom, 0);
			right = same;
		}
		place_button(dialog, BACK_ID, &right, top, button.right, button.bottom, 0);
		if (line)
			SetWindowPos(line, NULL, 0, top - margin.bottom, client.right, 2, SWP_NOZORDER | SWP_NOACTIVATE);
	}
	else {
		HWND apply = GetDlgItem(dialog, APPLY_ID);
		if (apply && !(GetWindowLongPtrW(apply, GWL_STYLE) & WS_VISIBLE))
			SetWindowPos(apply, NULL, client.right + gap, top, button.right, button.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
		place_button(dialog, APPLY_ID, &right, top, button.right, button.bottom, gap);
		place_button(dialog, IDCANCEL, &right, top, button.right, button.bottom, gap);
		place_button(dialog, IDOK, &right, top, button.right, button.bottom, gap);
	}
}

static void center_window(const sheet_state *state)
{
	RECT own, around;
	HWND parent = state->header.hwndParent;
	if (parent && parent != GetDesktopWindow())
		GetWindowRect(parent, &around);
	else
		SetRect(&around, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
	GetWindowRect(state->dialog, &own);
	SetWindowPos(state->dialog, NULL, (around.left + around.right - (own.right - own.left)) / 2, (around.top + around.bottom - (own.bottom - own.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The page table */

static void add_tab(sheet_state *state, int index, const page_handle *page)
{
	TCITEMW item;
	if (!state->tab)
		return;
	ZeroMemory(&item, sizeof(item));
	item.mask = TCIF_TEXT;
	item.pszText = page->title ? page->title : (WCHAR *)L"";
	item.iImage = -1;
	if (page->icon) {
		if (!state->images) {
			state->images = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 4, 4);
			SendMessageW(state->tab, TCM_SETIMAGELIST, 0, (LPARAM)state->images);
		}
		item.mask |= TCIF_IMAGE;
		item.iImage = ImageList_AddIcon(state->images, page->icon);
	}
	SendMessageW(state->tab, TCM_INSERTITEMW, index, (LPARAM)&item);
}

static BOOL insert_page(sheet_state *state, int index, page_handle *page)
{
	int walk;
	if (state->page_count == state->page_capacity) {
		const int capacity = state->page_capacity ? state->page_capacity * 2 : 8;
		page_handle **grown = allocate(capacity * sizeof(*grown));
		if (!grown)
			return FALSE;
		if (state->pages)
			CopyMemory(grown, state->pages, state->page_count * sizeof(*grown));
		release(state->pages);
		state->pages = grown;
		state->page_capacity = capacity;
	}
	if (index > state->page_count)
		index = state->page_count;
	for (walk = state->page_count; walk > index; walk--)
		state->pages[walk] = state->pages[walk - 1];
	state->pages[index] = page;
	state->page_count++;
	if (state->current >= index)
		state->current++;
	if (page->width > state->page_width)
		state->page_width = page->width;
	if (page->height > state->page_height)
		state->page_height = page->height;
	add_tab(state, index, page);
	return TRUE;
}

static void remove_page_at(sheet_state *state, int index)
{
	page_handle *page = state->pages[index];
	int walk;
	if (page->window)
		DestroyWindow(page->window);
	free_page(page);
	for (walk = index + 1; walk < state->page_count; walk++)
		state->pages[walk - 1] = state->pages[walk];
	state->page_count--;
	if (state->tab)
		SendMessageW(state->tab, TCM_DELETEITEM, index, 0);
	if (state->current == index)
		state->current = -1;
	else if (state->current > index)
		state->current--;
}

static int index_of_handle(const sheet_state *state, const void *handle)
{
	int index;
	for (index = 0; index < state->page_count; index++) {
		if (state->pages[index] == handle)
			return index;
	}
	return -1;
}

static int index_of_window(const sheet_state *state, HWND window)
{
	int index;
	for (index = 0; window && index < state->page_count; index++) {
		if (state->pages[index]->window == window)
			return index;
	}
	return -1;
}

static int index_of_id(const sheet_state *state, UINT_PTR id)
{
	int index;
	for (index = 0; index < state->page_count; index++) {
		const page_handle *page = state->pages[index];
		if (page->resource_id == id || (const void *)page->raw->pszTemplate == (const void *)id)
			return index;
	}
	return -1;
}

static BOOL create_page_window(sheet_state *state, page_handle *page)
{
	const PROPSHEETPAGEW *raw = page->raw;
	RECT area;
	if (page->window)
		return TRUE;
	if (!load_template(page))
		return FALSE;
	if ((raw->dwFlags & PSP_USECALLBACK) && raw->pfnCallback && !raw->pfnCallback(NULL, PSPCB_CREATE, page->raw))
		return FALSE;
	if (page->unicode)
		page->window = CreateDialogIndirectParamW(raw->hInstance, (const DLGTEMPLATE *)page->template_copy, state->dialog, raw->pfnDlgProc, (LPARAM)page->raw);
	else
		page->window = CreateDialogIndirectParamA(raw->hInstance, (const DLGTEMPLATE *)page->template_copy, state->dialog, raw->pfnDlgProc, (LPARAM)page->raw);
	if (!page->window)
		return FALSE;
	area = page_rect(state, page);
	SetWindowPos(page->window, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top, SWP_NOACTIVATE);
	return TRUE;
}

/* The first control the tab key would stop at. */
static HWND first_tab_stop(HWND page)
{
	HWND child;
	for (child = GetWindow(page, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
		const DWORD style = (DWORD)GetWindowLongPtrW(child, GWL_STYLE);
		if ((style & WS_TABSTOP) && (style & WS_VISIBLE) && !(style & WS_DISABLED))
			return child;
	}
	return NULL;
}

static void set_default(const sheet_state *state, int id)
{
	if (GetDlgItem(state->dialog, id) && IsWindowEnabled(GetDlgItem(state->dialog, id)))
		SendMessageW(state->dialog, DM_SETDEFID, id, 0);
}

static int default_wizard_button(const sheet_state *state)
{
	static const int order[] = { NEXT_ID, FINISH_ID, BACK_ID, IDCANCEL };
	int index;
	if (state->last_button) {
		HWND last = GetDlgItem(state->dialog, state->last_button);
		if (last && IsWindowEnabled(last) && (GetWindowLongPtrW(last, GWL_STYLE) & WS_VISIBLE))
			return state->last_button;
	}
	for (index = 0; index < 4; index++) {
		HWND button = GetDlgItem(state->dialog, order[index]);
		if (button && IsWindowEnabled(button) && (GetWindowLongPtrW(button, GWL_STYLE) & WS_VISIBLE))
			return order[index];
	}
	return 0;
}

static void focus_page(sheet_state *state)
{
	const page_handle *page;
	HWND target;
	if (state->current < 0)
		return;
	page = state->pages[state->current];
	target = first_tab_stop(page->window);
	{
		HWND asked = (HWND)notify_page(state, page, PSN_QUERYINITIALFOCUS, (LPARAM)target);
		if (asked)
			target = asked;
	}
	if (state->wizard) {
		const int button = default_wizard_button(state);
		if (button)
			set_default(state, button);
		if (!target && button)
			target = GetDlgItem(state->dialog, button);
	}
	if (target)
		SetFocus(target);
}

static void set_wizard_buttons(const sheet_state *state, DWORD flags)
{
	HWND back = GetDlgItem(state->dialog, BACK_ID), next = GetDlgItem(state->dialog, NEXT_ID), finish = GetDlgItem(state->dialog, FINISH_ID);
	const BOOL show_finish = (flags & (PSWIZB_FINISH | PSWIZB_DISABLEDFINISH)) != 0;
	if (!next)
		return;
	EnableWindow(back, (flags & PSWIZB_BACK) != 0);
	EnableWindow(next, (flags & PSWIZB_NEXT) != 0);
	EnableWindow(finish, (flags & PSWIZB_FINISH) != 0);
	ShowWindow(next, show_finish ? SW_HIDE : SW_SHOWNA);
	ShowWindow(finish, show_finish ? SW_SHOWNA : SW_HIDE);
	if (show_finish || (flags & PSWIZB_NEXT)) {
		const int button = default_wizard_button(state);
		if (button)
			set_default(state, button);
	}
}

/* Shows a page after the one in view has agreed to leave. A page may turn the visit down and name another, or none. */
static BOOL activate_page(sheet_state *state, int index, int step)
{
	while (index >= 0 && index < state->page_count) {
		page_handle *page = state->pages[index];
		LRESULT answer;
		int previous = state->current;
		if (!create_page_window(state, page)) {
			remove_page_at(state, index);
			if (state->tab && state->current >= 0)
				SendMessageW(state->tab, TCM_SETCURSEL, state->current, 0);
			return FALSE;
		}
		if (state->wizard)
			set_wizard_buttons(state, (index > 0 ? PSWIZB_BACK : 0) | PSWIZB_NEXT);
		answer = notify_page(state, page, PSN_SETACTIVE, 0);
		if (answer == -1) {
			index += step;
			continue;
		}
		if (answer) {
			const int named = index_of_id(state, answer);
			if (named >= 0 && named != index) {
				step = named > index ? 1 : -1;
				index = named;
				continue;
			}
		}
		if (previous >= 0 && previous != index && previous < state->page_count)
			ShowWindow(state->pages[previous]->window, SW_HIDE);
		state->current = index;
		if (state->tab)
			SendMessageW(state->tab, TCM_SETCURSEL, index, 0);
		SetWindowPos(page->window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
		if (state->wizard97)
			InvalidateRect(state->dialog, NULL, TRUE);
		focus_page(state);
		return TRUE;
	}
	return FALSE;
}

static BOOL page_agrees_to_leave(const sheet_state *state)
{
	if (state->current < 0)
		return TRUE;
	return notify_page(state, state->pages[state->current], PSN_KILLACTIVE, 0) == 0;
}

static BOOL go_to_page(sheet_state *state, int index, int step)
{
	if (index < 0 || index >= state->page_count)
		return FALSE;
	if (!page_agrees_to_leave(state))
		return FALSE;
	return activate_page(state, index, step);
}

/* Buttons */

static void end_sheet(sheet_state *state)
{
	state->ended = TRUE;
	/* A modeless sheet stays until its owner sees that there is no current page any more. */
	if (state->current >= 0 && state->modeless)
		state->current = -1;
}

static void tell_callback(const sheet_state *state, int button)
{
	if ((state->header.dwFlags & PSH_USECALLBACK) && state->header.pfnCallback)
		state->header.pfnCallback(state->dialog, PSCB_BUTTONPRESSED, button);
}

static BOOL apply(sheet_state *state, BOOL closing)
{
	int index;
	if (!page_agrees_to_leave(state))
		return FALSE;
	for (index = 0; index < state->page_count; index++) {
		page_handle *page = state->pages[index];
		LRESULT answer;
		if (!page->window)
			continue;
		answer = notify_page(state, page, PSN_APPLY, closing);
		if (answer == PSNRET_INVALID_NOCHANGEPAGE)
			return FALSE;
		if (answer == PSNRET_INVALID) {
			if (index != state->current)
				activate_page(state, index, 1);
			return FALSE;
		}
		page->dirty = FALSE;
	}
	state->result = 1;
	if (GetDlgItem(state->dialog, APPLY_ID))
		EnableWindow(GetDlgItem(state->dialog, APPLY_ID), FALSE);
	if (!closing && state->current >= 0) {
		notify_page(state, state->pages[state->current], PSN_SETACTIVE, 0);
		focus_page(state);
	}
	return TRUE;
}

static void cancel(sheet_state *state)
{
	int index;
	if (state->current >= 0 && notify_page(state, state->pages[state->current], PSN_QUERYCANCEL, 0))
		return;
	for (index = 0; index < state->page_count; index++) {
		if (state->pages[index]->window)
			notify_page(state, state->pages[index], PSN_RESET, 0);
	}
	end_sheet(state);
}

static void wizard_step(sheet_state *state, UINT code, int step, UINT button)
{
	LRESULT answer;
	int target;
	if (state->current < 0)
		return;
	answer = notify_page(state, state->pages[state->current], code, 0);
	if (answer == -1)
		return;
	target = answer ? index_of_id(state, answer) : state->current + step;
	if (target < 0 || target >= state->page_count)
		return;
	if (!page_agrees_to_leave(state))
		return;
	state->last_button = button;
	activate_page(state, target, step);
}

static void wizard_finish(sheet_state *state)
{
	if (state->current >= 0) {
		if (!page_agrees_to_leave(state))
			return;
		if (notify_page(state, state->pages[state->current], PSN_WIZFINISH, 0))
			return;
	}
	state->result = 1;
	end_sheet(state);
}

static void press(sheet_state *state, int button)
{
	tell_callback(state, button);
	switch (button) {
	case PSBTN_BACK:
		wizard_step(state, PSN_WIZBACK, -1, BACK_ID);
		break;
	case PSBTN_NEXT:
		wizard_step(state, PSN_WIZNEXT, 1, NEXT_ID);
		break;
	case PSBTN_FINISH:
		wizard_finish(state);
		break;
	case PSBTN_OK:
		if (apply(state, TRUE))
			end_sheet(state);
		break;
	case PSBTN_APPLYNOW:
		apply(state, FALSE);
		break;
	case PSBTN_CANCEL:
		cancel(state);
		break;
	case PSBTN_HELP:
		if (state->current >= 0)
			notify_page(state, state->pages[state->current], PSN_HELP, 0);
		break;
	}
}

/* Building */

static HWND add_button(const sheet_state *state, int id, const WCHAR *text, DWORD extra)
{
	HWND button = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_TABSTOP | extra, 0, 0, 0, 0, state->dialog, (HMENU)(LONG_PTR)id, (HINSTANCE)GetWindowLongPtrW(state->dialog, GWLP_HINSTANCE), NULL);
	if (button)
		SendMessageW(button, WM_SETFONT, SendMessageW(state->dialog, WM_GETFONT, 0, 0), FALSE);
	return button;
}

static void build(sheet_state *state)
{
	const DWORD flags = state->header.dwFlags;
	const DWORD visible_help = (flags & PSH_HASHELP) ? WS_VISIBLE : 0;
	HWND dialog = state->dialog;
	if (state->wizard) {
		CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, 0, 0, 0, 0, dialog, (HMENU)(LONG_PTR)LINE_ID, NULL, NULL);
		add_button(state, BACK_ID, L"< &Back", WS_VISIBLE);
		add_button(state, NEXT_ID, L"&Next >", WS_VISIBLE | BS_DEFPUSHBUTTON);
		add_button(state, FINISH_ID, L"Finish", 0);
		add_button(state, IDCANCEL, L"Cancel", WS_VISIBLE);
		add_button(state, IDHELP, L"Help", visible_help);
	}
	else {
		state->tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | TCS_MULTILINE, 0, 0, 0, 0, dialog, (HMENU)(LONG_PTR)TAB_ID, NULL, NULL);
		SendMessageW(state->tab, WM_SETFONT, SendMessageW(dialog, WM_GETFONT, 0, 0), FALSE);
		add_button(state, IDOK, L"OK", WS_VISIBLE | BS_DEFPUSHBUTTON);
		add_button(state, IDCANCEL, L"Cancel", WS_VISIBLE);
		add_button(state, APPLY_ID, L"&Apply", ((flags & PSH_NOAPPLYNOW) ? 0 : WS_VISIBLE) | WS_DISABLED);
		add_button(state, IDHELP, L"Help", visible_help);
	}
}

static void set_title(sheet_state *state, DWORD flags, const WCHAR *text)
{
	if ((flags & PSH_PROPTITLE) && text) {
		WCHAR line[512];
		wsprintfW(line, L"Properties for %s", text);
		SetWindowTextW(state->dialog, line);
	}
	else {
		SetWindowTextW(state->dialog, text ? text : L"");
	}
}

/* Everything in the band across the top of a wizard, and the picture down the side of its pages without one. */
static void paint_wizard(const sheet_state *state, HDC dc)
{
	const page_handle *page = state->current >= 0 ? state->pages[state->current] : NULL;
	RECT client;
	HDC source;
	BITMAP facts;
	if (!state->wizard97 || !page)
		return;
	GetClientRect(state->dialog, &client);
	if (has_header_room(state, page)) {
		RECT band = client, text_area;
		HGDIOBJ old_font = NULL;
		band.bottom = HEADER_HEIGHT;
		FillRect(dc, &band, GetSysColorBrush(COLOR_WINDOW));
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
		SetRect(&text_area, 22, 10, client.right - 64, 30);
		if (state->title_font)
			old_font = SelectObject(dc, state->title_font);
		if (page->header_title)
			DrawTextW(dc, page->header_title, -1, &text_area, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
		if (old_font)
			SelectObject(dc, old_font);
		old_font = SelectObject(dc, (HFONT)SendMessageW(state->dialog, WM_GETFONT, 0, 0));
		SetRect(&text_area, 34, 28, client.right - 64, HEADER_HEIGHT - 2);
		if (page->header_subtitle)
			DrawTextW(dc, page->header_subtitle, -1, &text_area, DT_WORDBREAK | DT_LEFT | DT_NOPREFIX);
		SelectObject(dc, old_font);
		if (state->header_bitmap && GetObjectW(state->header_bitmap, sizeof(facts), &facts)) {
			source = CreateCompatibleDC(dc);
			SelectObject(source, state->header_bitmap);
			BitBlt(dc, client.right - facts.bmWidth - 6, (HEADER_HEIGHT - facts.bmHeight) / 2, facts.bmWidth, facts.bmHeight, source, 0, 0, SRCCOPY);
			DeleteDC(source);
		}
		band.top = HEADER_HEIGHT - 1;
		FillRect(dc, &band, GetSysColorBrush(COLOR_3DSHADOW));
	}
	else if ((page->raw->dwFlags & PSP_HIDEHEADER) && state->watermark && GetObjectW(state->watermark, sizeof(facts), &facts)) {
		const RECT line = page_rect(state, NULL);
		source = CreateCompatibleDC(dc);
		SelectObject(source, state->watermark);
		StretchBlt(dc, 0, 0, WATERMARK_WIDTH, line.bottom, source, 0, 0, facts.bmWidth, facts.bmHeight, SRCCOPY);
		DeleteDC(source);
	}
}

static void free_sheet(sheet_state *state)
{
	int index;
	for (index = 0; index < state->page_count; index++)
		free_page(state->pages[index]);
	release(state->pages);
	release(state->caption);
	if (state->images)
		ImageList_Destroy(state->images);
	if (state->title_font)
		DeleteObject(state->title_font);
	release(state);
}

/* The two-character-set messages */

static LRESULT set_text_field(sheet_state *state, UINT message, WPARAM wparam, const void *given, BOOL unicode)
{
	WCHAR *text = copy_text(given, unicode, state->header.hInstance);
	HWND button;
	switch (message) {
	case PSM_SETTITLEA:
	case PSM_SETTITLEW:
		set_title(state, (DWORD)wparam, text);
		break;
	case PSM_SETFINISHTEXTA:
	case PSM_SETFINISHTEXTW:
		button = GetDlgItem(state->dialog, FINISH_ID);
		if (button) {
			SetWindowTextW(button, text ? text : L"");
			EnableWindow(button, TRUE);
			ShowWindow(button, SW_SHOWNA);
			ShowWindow(GetDlgItem(state->dialog, NEXT_ID), SW_HIDE);
			ShowWindow(GetDlgItem(state->dialog, BACK_ID), SW_HIDE);
			set_default(state, FINISH_ID);
		}
		break;
	case PSM_SETHEADERTITLEA:
	case PSM_SETHEADERTITLEW:
	case PSM_SETHEADERSUBTITLEA:
	case PSM_SETHEADERSUBTITLEW:
		if ((int)wparam >= 0 && (int)wparam < state->page_count) {
			page_handle *page = state->pages[wparam];
			WCHAR **slot = (message == PSM_SETHEADERTITLEA || message == PSM_SETHEADERTITLEW) ? &page->header_title : &page->header_subtitle;
			release(*slot);
			*slot = text;
			text = NULL;
			InvalidateRect(state->dialog, NULL, TRUE);
		}
		break;
	}
	release(text);
	return TRUE;
}

static void reply(HWND dialog, LRESULT value)
{
	SetWindowLongPtrW(dialog, DWLP_MSGRESULT, value);
}

static BOOL handle_keys(sheet_state *state, const MSG *message)
{
	int step = 0;
	if (message->message != WM_KEYDOWN || GetKeyState(VK_CONTROL) >= 0 || !state->page_count)
		return FALSE;
	if (message->wParam == VK_TAB)
		step = GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
	else if (message->wParam == VK_NEXT)
		step = 1;
	else if (message->wParam == VK_PRIOR)
		step = -1;
	if (!step)
		return FALSE;
	go_to_page(state, (state->current + step + state->page_count) % state->page_count, step);
	return TRUE;
}

static BOOL handle_sheet_message(sheet_state *state, UINT message, WPARAM wparam, LPARAM lparam)
{
	HWND dialog = state->dialog;
	int index;
	switch (message) {
	case PSM_SETCURSEL:
		index = lparam ? index_of_handle(state, (const void *)lparam) : -1;
		if (index < 0)
			index = (int)wparam;
		if (index < 0 || index >= state->page_count) {
			reply(dialog, FALSE);
			return TRUE;
		}
		if (index == state->current) {
			reply(dialog, TRUE);
			return TRUE;
		}
		if (!page_agrees_to_leave(state)) {
			reply(dialog, FALSE);
			return TRUE;
		}
		activate_page(state, index, index > state->current ? 1 : -1);
		reply(dialog, TRUE);
		return TRUE;
	case PSM_SETCURSELID:
		index = index_of_id(state, lparam);
		reply(dialog, index >= 0 && go_to_page(state, index, 1));
		return TRUE;
	case PSM_REMOVEPAGE:
		index = lparam ? index_of_handle(state, (const void *)lparam) : (int)wparam;
		if (index >= 0 && index < state->page_count) {
			const BOOL was_current = index == state->current;
			remove_page_at(state, index);
			if (was_current && state->page_count)
				activate_page(state, index < state->page_count ? index : state->page_count - 1, 1);
			InvalidateRect(dialog, NULL, TRUE);
		}
		reply(dialog, 0);
		return TRUE;
	case PSM_ADDPAGE:
	case PSM_INSERTPAGE: {
		page_handle *page = accept_page(state, (const void *)lparam);
		int where = state->page_count;
		if (message == PSM_INSERTPAGE) {
			if (IS_INTRESOURCE(wparam)) {
				where = (int)wparam;
			}
			else {
				const int after = index_of_handle(state, (const void *)wparam);
				if (after < 0) {
					reply(dialog, FALSE);
					return TRUE;
				}
				where = after + 1;
			}
		}
		if (!page) {
			reply(dialog, FALSE);
			return TRUE;
		}
		load_template(page);
		if (page->raw->dwFlags & PSP_PREMATURE) {
			if (!create_page_window(state, page)) {
				reply(dialog, FALSE);
				return TRUE;
			}
		}
		reply(dialog, insert_page(state, where, page));
		return TRUE;
	}
	case PSM_CHANGED:
		index = index_of_window(state, (HWND)wparam);
		if (index >= 0)
			state->pages[index]->dirty = TRUE;
		if (GetDlgItem(dialog, APPLY_ID))
			EnableWindow(GetDlgItem(dialog, APPLY_ID), TRUE);
		reply(dialog, TRUE);
		return TRUE;
	case PSM_UNCHANGED: {
		BOOL any = FALSE;
		index = index_of_window(state, (HWND)wparam);
		if (index >= 0)
			state->pages[index]->dirty = FALSE;
		for (index = 0; index < state->page_count; index++)
			any = any || state->pages[index]->dirty;
		if (GetDlgItem(dialog, APPLY_ID))
			EnableWindow(GetDlgItem(dialog, APPLY_ID), any);
		reply(dialog, TRUE);
		return TRUE;
	}
	case PSM_RESTARTWINDOWS:
		state->restart = TRUE;
		reply(dialog, TRUE);
		return TRUE;
	case PSM_REBOOTSYSTEM:
		state->reboot = TRUE;
		reply(dialog, TRUE);
		return TRUE;
	case PSM_CANCELTOCLOSE:
		if (GetDlgItem(dialog, IDOK))
			SetWindowTextW(GetDlgItem(dialog, IDOK), L"Close");
		if (GetDlgItem(dialog, IDCANCEL))
			EnableWindow(GetDlgItem(dialog, IDCANCEL), FALSE);
		reply(dialog, TRUE);
		return TRUE;
	case PSM_QUERYSIBLINGS: {
		LRESULT answer = 0;
		for (index = 0; !answer && index < state->page_count; index++) {
			if (state->pages[index]->window)
				answer = SendMessageW(state->pages[index]->window, PSM_QUERYSIBLINGS, wparam, lparam);
		}
		reply(dialog, answer);
		return TRUE;
	}
	case PSM_APPLY:
		reply(dialog, apply(state, FALSE));
		return TRUE;
	case PSM_SETWIZBUTTONS:
		set_wizard_buttons(state, (DWORD)lparam);
		reply(dialog, TRUE);
		return TRUE;
	case PSM_SHOWWIZBUTTONS:
	case PSM_ENABLEWIZBUTTONS: {
		static const struct {
			DWORD flag;
			int id;
		} table[] = { { PSWIZB_BACK, BACK_ID }, { PSWIZB_NEXT, NEXT_ID }, { PSWIZB_FINISH, FINISH_ID }, { PSWIZB_CANCEL, IDCANCEL } };
		for (index = 0; index < 4; index++) {
			HWND button = GetDlgItem(dialog, table[index].id);
			const DWORD mask = message == PSM_SHOWWIZBUTTONS ? (DWORD)lparam : (DWORD)wparam >> 16;
			const DWORD chosen = message == PSM_SHOWWIZBUTTONS ? (DWORD)wparam : (DWORD)wparam & 0xFFFF;
			if (!button || !(mask & table[index].flag))
				continue;
			if (message == PSM_SHOWWIZBUTTONS)
				ShowWindow(button, (chosen & table[index].flag) ? SW_SHOWNA : SW_HIDE);
			else
				EnableWindow(button, (chosen & table[index].flag) != 0);
		}
		lay_out(state);
		reply(dialog, TRUE);
		return TRUE;
	}
	case PSM_SETBUTTONTEXT: {
		const int id = wparam == PSWIZB_BACK ? BACK_ID : wparam == PSWIZB_NEXT ? NEXT_ID : wparam == PSWIZB_FINISH ? FINISH_ID : wparam == PSWIZB_CANCEL ? IDCANCEL : 0;
		if (id && GetDlgItem(dialog, id))
			SetWindowTextW(GetDlgItem(dialog, id), (const WCHAR *)lparam);
		reply(dialog, TRUE);
		return TRUE;
	}
	case PSM_PRESSBUTTON:
		press(state, (int)wparam);
		reply(dialog, TRUE);
		return TRUE;
	case PSM_GETTABCONTROL:
		reply(dialog, (LRESULT)state->tab);
		return TRUE;
	case PSM_ISDIALOGMESSAGE: {
		const MSG *given = (const MSG *)lparam;
		reply(dialog, handle_keys(state, given) || IsDialogMessageW(dialog, (MSG *)given));
		return TRUE;
	}
	case PSM_GETCURRENTPAGEHWND:
		reply(dialog, (LRESULT)(state->current >= 0 && !state->ended ? state->pages[state->current]->window : NULL));
		return TRUE;
	case PSM_HWNDTOINDEX:
		reply(dialog, index_of_window(state, (HWND)wparam));
		return TRUE;
	case PSM_INDEXTOHWND:
		reply(dialog, (int)wparam >= 0 && (int)wparam < state->page_count ? (LRESULT)state->pages[wparam]->window : 0);
		return TRUE;
	case PSM_PAGETOINDEX:
		reply(dialog, index_of_handle(state, (const void *)lparam));
		return TRUE;
	case PSM_INDEXTOPAGE:
		reply(dialog, (int)wparam >= 0 && (int)wparam < state->page_count ? (LRESULT)state->pages[wparam] : 0);
		return TRUE;
	case PSM_IDTOINDEX:
		reply(dialog, index_of_id(state, lparam));
		return TRUE;
	case PSM_INDEXTOID:
		reply(dialog, (int)wparam >= 0 && (int)wparam < state->page_count ? state->pages[wparam]->resource_id : 0);
		return TRUE;
	case PSM_GETRESULT:
		reply(dialog, state->ended ? (state->reboot ? ID_PSREBOOTSYSTEM : state->restart ? ID_PSRESTARTWINDOWS : state->result) : -1);
		return TRUE;
	case PSM_RECALCPAGESIZES:
		reply(dialog, TRUE);
		return TRUE;
	case PSM_SETTITLEA:
	case PSM_SETFINISHTEXTA:
	case PSM_SETHEADERTITLEA:
	case PSM_SETHEADERSUBTITLEA:
		reply(dialog, set_text_field(state, message, wparam, (const void *)lparam, FALSE));
		return TRUE;
	case PSM_SETTITLEW:
	case PSM_SETFINISHTEXTW:
	case PSM_SETHEADERTITLEW:
	case PSM_SETHEADERSUBTITLEW:
		reply(dialog, set_text_field(state, message, wparam, (const void *)lparam, TRUE));
		return TRUE;
	}
	return FALSE;
}

static INT_PTR CALLBACK sheet_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
	sheet_state *state = (sheet_state *)GetWindowLongPtrW(dialog, DWLP_USER);

	if (message == WM_INITDIALOG) {
		const PROPSHEETHEADERW *header;
		int index, start = 0;
		state = (sheet_state *)lparam;
		header = &state->header;
		state->dialog = dialog;
		SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)state);
		set_title(state, header->dwFlags, state->caption);
		if ((header->dwFlags & PSH_USEHICON) && header->hIcon)
			SendMessageW(dialog, WM_SETICON, ICON_BIG, (LPARAM)header->hIcon);
		else if ((header->dwFlags & PSH_USEICONID) && header->pszIcon)
			SendMessageW(dialog, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(header->hInstance, header->pszIcon));
		if (state->wizard97) {
			LOGFONTW description;
			HFONT font = (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0);
			if (font && GetObjectW(font, sizeof(description), &description)) {
				description.lfWeight = FW_BOLD;
				state->title_font = CreateFontIndirectW(&description);
			}
			if (header->dwFlags & PSH_WATERMARK)
				state->watermark = (header->dwFlags & PSH_USEHBMWATERMARK) ? header->hbmWatermark : LoadBitmapW(header->hInstance, header->pszbmWatermark);
			if (header->dwFlags & PSH_HEADER)
				state->header_bitmap = (header->dwFlags & PSH_USEHBMHEADER) ? header->hbmHeader : LoadBitmapW(header->hInstance, header->pszbmHeader);
		}
		build(state);

		/* The tab items need the window, so the pages come here and not before it exists. */
		{
			page_handle **given = (page_handle **)state->pages;
			const int count = state->page_count;
			state->pages = NULL;
			state->page_count = state->page_capacity = 0;
			for (index = 0; index < count; index++) {
				if (given[index]) {
					load_template(given[index]);
					insert_page(state, state->page_count, given[index]);
				}
			}
			release(given);
		}
		for (index = 0; index < state->page_count; index++) {
			if (!(state->pages[index]->raw->dwFlags & PSP_HIDEHEADER) && (header->dwFlags & PSH_HEADER) && state->wizard97)
				state->any_header = TRUE;
		}
		/* A sheet that starts empty still needs a body. */
		if (!state->page_width || !state->page_height) {
			state->page_width = DEFAULT_PAGE_WIDTH;
			state->page_height = DEFAULT_PAGE_HEIGHT;
		}
		lay_out(state);
		center_window(state);
		if ((header->dwFlags & PSH_USECALLBACK) && header->pfnCallback)
			header->pfnCallback(dialog, PSCB_INITIALIZED, 0);
		for (index = state->page_count - 1; index >= 0; index--) {
			page_handle *page = state->pages[index];
			if ((page->raw->dwFlags & PSP_PREMATURE) && !create_page_window(state, page))
				remove_page_at(state, index);
		}
		if (header->dwFlags & PSH_USEPSTARTPAGE) {
			const WCHAR *wanted = header->pStartPage;
			for (index = 0; wanted && index < state->page_count; index++) {
				if (state->pages[index]->title && !lstrcmpW(state->pages[index]->title, wanted))
					start = index;
			}
		}
		else if (header->nStartPage < (UINT)state->page_count) {
			start = header->nStartPage;
		}
		state->current = -1;
		while (state->page_count && state->current < 0) {
			if (start >= state->page_count)
				start = state->page_count - 1;
			const int before = state->page_count;
			activate_page(state, start, 1);
			/* A page that failed has gone and the next one gets a turn, but one that declined stays. */
			if (state->page_count == before)
				break;
		}
		/* Nothing could be shown: a modal sheet would have no way of ever ending. */
		if (!state->modeless && !state->page_count)
			state->ended = TRUE;
		return FALSE;
	}
	if (!state)
		return FALSE;

	switch (message) {
	case WM_COMMAND:
		if (HIWORD(wparam) <= 1) {
			switch (LOWORD(wparam)) {
			case IDOK:
				if (GetDlgItem(dialog, IDOK)) {
					press(state, PSBTN_OK);
					return TRUE;
				}
				break;
			case IDCANCEL:
				press(state, PSBTN_CANCEL);
				return TRUE;
			case APPLY_ID:
				press(state, PSBTN_APPLYNOW);
				return TRUE;
			case IDHELP:
				press(state, PSBTN_HELP);
				return TRUE;
			case BACK_ID:
				press(state, PSBTN_BACK);
				return TRUE;
			case NEXT_ID:
				press(state, PSBTN_NEXT);
				return TRUE;
			case FINISH_ID:
				press(state, PSBTN_FINISH);
				return TRUE;
			}
			/* The enter key reports the default button here even when it belongs to the page. */
			if (state->current >= 0)
				SendMessageW(state->pages[state->current]->window, WM_COMMAND, wparam, lparam);
			return TRUE;
		}
		return FALSE;
	case WM_NOTIFY: {
		const NMHDR *header = (const NMHDR *)lparam;
		if (header->hwndFrom == state->tab && header->code == TCN_SELCHANGING) {
			reply(dialog, !page_agrees_to_leave(state));
			return TRUE;
		}
		if (header->hwndFrom == state->tab && header->code == TCN_SELCHANGE) {
			const int chosen = (int)SendMessageW(state->tab, TCM_GETCURSEL, 0, 0);
			activate_page(state, chosen, 1);
			return TRUE;
		}
		return FALSE;
	}
	case WM_ERASEBKGND:
		if (state->wizard97) {
			RECT client;
			GetClientRect(dialog, &client);
			FillRect((HDC)wparam, &client, GetSysColorBrush(COLOR_3DFACE));
			paint_wizard(state, (HDC)wparam);
			reply(dialog, 1);
			return TRUE;
		}
		return FALSE;
	case WM_CLOSE:
		press(state, PSBTN_CANCEL);
		return TRUE;
	case WM_NCDESTROY:
		SetWindowLongPtrW(dialog, DWLP_USER, 0);
		if (state->modeless)
			free_sheet(state);
		return FALSE;
	}
	if (message >= WM_USER)
		return handle_sheet_message(state, message, wparam, lparam);
	return FALSE;
}

/* Starting a sheet */

/* The dialog with no items, in a block big enough for a callback that wants to change the template. */
static DLGTEMPLATE *make_sheet_template(const sheet_state *state)
{
	static const WCHAR font[] = L"MS Shell Dlg";
	const DWORD flags = state->header.dwFlags;
	DLGTEMPLATE *shell = allocate(4096);
	WORD *walk;
	if (!shell)
		return NULL;
	shell->style = WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CAPTION | WS_SYSMENU | DS_CONTEXTHELP | DS_MODALFRAME | DS_SETFONT | DS_3DLOOK;
	if ((flags & PSH_NOCONTEXTHELP) || (state->wizard && !(flags & PSH_WIZARDCONTEXTHELP)))
		shell->style &= ~DS_CONTEXTHELP;
	shell->dwExtendedStyle = WS_EX_CONTROLPARENT;
	shell->cx = DEFAULT_PAGE_WIDTH;
	shell->cy = DEFAULT_PAGE_HEIGHT;
	walk = (WORD *)(shell + 1);
	*walk++ = 0;
	*walk++ = 0;
	*walk++ = 0;
	*walk++ = 8;
	CopyMemory(walk, font, sizeof(font));
	return shell;
}

static INT_PTR run_sheet(const void *given, BOOL unicode)
{
	const PROPSHEETHEADERW *header = given;
	sheet_state *state;
	DLGTEMPLATE *shell;
	HWND window, owner;
	DWORD size;
	UINT index;
	INT_PTR result;

	if (!header || header->dwSize < PROPSHEETHEADERA_V1_SIZE)
		return -1;
	state = allocate(sizeof(*state));
	if (!state)
		return -1;
	size = header->dwSize < sizeof(state->header) ? header->dwSize : sizeof(state->header);
	CopyMemory(&state->header, header, size);
	state->unicode = unicode;
	state->wizard97 = (header->dwFlags & (PSH_WIZARD97 | PSH_AEROWIZARD)) != 0;
	state->wizard = state->wizard97 || (header->dwFlags & (PSH_WIZARD | PSH_WIZARD_LITE)) != 0;
	state->modeless = (header->dwFlags & PSH_MODELESS) != 0;
	state->current = -1;
	state->caption = copy_text(header->pszCaption, unicode, header->hInstance);

	state->pages = allocate((header->nPages + 1) * sizeof(*state->pages));
	state->page_count = header->nPages;
	for (index = 0; state->pages && index < header->nPages; index++) {
		if (header->dwFlags & PSH_PROPSHEETPAGE) {
			const BYTE *table = (const BYTE *)header->ppsp;
			const PROPSHEETPAGEW *walk = (const PROPSHEETPAGEW *)table;
			UINT step;
			for (step = 0; step < index; step++)
				walk = (const PROPSHEETPAGEW *)((const BYTE *)walk + walk->dwSize);
			state->pages[index] = make_page(walk, unicode);
		}
		else {
			state->pages[index] = accept_page(state, header->phpage[index]);
		}
	}

	shell = make_sheet_template(state);
	if (shell && (header->dwFlags & PSH_USECALLBACK) && header->pfnCallback)
		header->pfnCallback(NULL, PSCB_PRECREATE, (LPARAM)shell);
	owner = header->hwndParent;
	if (!state->modeless && owner && owner != GetDesktopWindow())
		EnableWindow(owner, FALSE);
	else
		owner = state->modeless ? NULL : owner;
	window = shell ? CreateDialogIndirectParamW((HINSTANCE)GetModuleHandleW(L"comctl32.dll"), shell, header->hwndParent, sheet_proc, (LPARAM)state) : NULL;
	release(shell);
	if (!window) {
		if (owner && owner != GetDesktopWindow())
			EnableWindow(owner, TRUE);
		state->modeless = FALSE;
		free_sheet(state);
		return -1;
	}
	if (state->modeless)
		return (INT_PTR)window;

	while (!state->ended) {
		MSG message;
		const int got = GetMessageW(&message, NULL, 0, 0);
		if (got <= 0) {
			if (got == 0)
				PostQuitMessage((int)message.wParam);
			break;
		}
		if (!SendMessageW(window, PSM_ISDIALOGMESSAGE, 0, (LPARAM)&message)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
	if (owner && owner != GetDesktopWindow())
		EnableWindow(owner, TRUE);
	result = state->reboot ? ID_PSREBOOTSYSTEM : state->restart ? ID_PSRESTARTWINDOWS : state->result;
	DestroyWindow(window);
	free_sheet(state);
	return result;
}

INT_PTR WINAPI PropertySheetW(LPCPROPSHEETHEADERW header)
{
	return run_sheet(header, TRUE);
}

INT_PTR WINAPI PropertySheetA(LPCPROPSHEETHEADERA header)
{
	return run_sheet(header, FALSE);
}
