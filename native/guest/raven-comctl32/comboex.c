/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>

/* The extended combo box is a thin window around a real owner drawn combo box. The real one holds only pointers to the
 * records kept here, so everything about an item (text, images, indent) lives in one place and the list just draws it. */

#define INDENT_STEP 10
#define IMAGE_GAP 3
#define EDIT_ITEM (-1)
#define COMBO_ID 1001
#define SUBCLASS_ID 1

#define HAS_CALLBACK_TEXT 0x01
#define HAS_CALLBACK_IMAGE 0x02
#define HAS_CALLBACK_SELECTED 0x04
#define HAS_CALLBACK_OVERLAY 0x08
#define HAS_CALLBACK_INDENT 0x10

typedef struct combo_record {
	WCHAR *text;
	UINT callbacks;
	int image, selected, overlay, indent;
	LPARAM data;
} combo_record;

typedef struct {
	HWND window, parent, combo, edit;
	HIMAGELIST images;
	DWORD extended;
	HFONT font;
	BOOL ansi_parent, narrow_format, edit_dirty, editing, writing_edit;
	int closed_height, drop_height;
	combo_record edit_record;
	WCHAR answer[CBEMAXSTRLEN];
} combo_state;

static combo_state *state_of(HWND window)
{
	return (combo_state *)GetWindowLongPtrW(window, 0);
}

static void *allocate(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void release(void *block)
{
	if (block)
		HeapFree(GetProcessHeap(), 0, block);
}

static WCHAR *copy_wide(const WCHAR *text)
{
	int size;
	WCHAR *copy;
	if (!text)
		return NULL;
	size = lstrlenW(text) + 1;
	copy = allocate(size * sizeof(WCHAR));
	if (copy)
		CopyMemory(copy, text, size * sizeof(WCHAR));
	return copy;
}

static WCHAR *copy_narrow(const char *text)
{
	int size;
	WCHAR *copy;
	if (!text)
		return NULL;
	size = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	copy = allocate(size * sizeof(WCHAR));
	if (copy)
		MultiByteToWideChar(CP_ACP, 0, text, -1, copy, size);
	return copy;
}

static int count_of(const combo_state *state)
{
	return (int)SendMessageW(state->combo, CB_GETCOUNT, 0, 0);
}

static combo_record *record_at(const combo_state *state, int index)
{
	LRESULT data;
	if (index == EDIT_ITEM)
		return (combo_record *)&state->edit_record;
	data = SendMessageW(state->combo, CB_GETITEMDATA, index, 0);
	return data == CB_ERR ? NULL : (combo_record *)data;
}

static void apply_item(combo_record *record, const COMBOBOXEXITEMW *item, BOOL narrow)
{
	UINT mask = item->mask;
	if (mask & CBEIF_TEXT) {
		release(record->text);
		record->text = NULL;
		record->callbacks &= ~HAS_CALLBACK_TEXT;
		if (item->pszText == LPSTR_TEXTCALLBACKW)
			record->callbacks |= HAS_CALLBACK_TEXT;
		else
			record->text = narrow ? copy_narrow((const char *)item->pszText) : copy_wide(item->pszText);
	}
#define APPLY_NUMBER(bit, field, source, sentinel, flag) \
	if (mask & (bit)) { \
		record->field = item->source; \
		if (item->source == (sentinel)) \
			record->callbacks |= (flag); \
		else \
			record->callbacks &= ~(flag); \
	}
	APPLY_NUMBER(CBEIF_IMAGE, image, iImage, I_IMAGECALLBACK, HAS_CALLBACK_IMAGE)
	APPLY_NUMBER(CBEIF_SELECTEDIMAGE, selected, iSelectedImage, I_IMAGECALLBACK, HAS_CALLBACK_SELECTED)
	APPLY_NUMBER(CBEIF_OVERLAY, overlay, iOverlay, I_IMAGECALLBACK, HAS_CALLBACK_OVERLAY)
	APPLY_NUMBER(CBEIF_INDENT, indent, iIndent, I_INDENTCALLBACK, HAS_CALLBACK_INDENT)
#undef APPLY_NUMBER
	if (mask & CBEIF_LPARAM)
		record->data = item->lParam;
}

static void init_record(combo_record *record)
{
	ZeroMemory(record, sizeof(*record));
	record->image = record->selected = record->overlay = -1;
}

static UINT callback_mask_for(const combo_record *record, UINT asked)
{
	UINT need = 0;
	if ((asked & CBEIF_TEXT) && (record->callbacks & HAS_CALLBACK_TEXT))
		need |= CBEIF_TEXT;
	if ((asked & CBEIF_IMAGE) && (record->callbacks & HAS_CALLBACK_IMAGE))
		need |= CBEIF_IMAGE;
	if ((asked & CBEIF_SELECTEDIMAGE) && (record->callbacks & HAS_CALLBACK_SELECTED))
		need |= CBEIF_SELECTEDIMAGE;
	if ((asked & CBEIF_OVERLAY) && (record->callbacks & HAS_CALLBACK_OVERLAY))
		need |= CBEIF_OVERLAY;
	if ((asked & CBEIF_INDENT) && (record->callbacks & HAS_CALLBACK_INDENT))
		need |= CBEIF_INDENT;
	return need;
}

static LRESULT send_notify(const combo_state *state, NMHDR *header, UINT wide, UINT narrow)
{
	header->hwndFrom = state->window;
	header->idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	header->code = state->ansi_parent ? narrow : wide;
	return SendMessageW(state->parent, WM_NOTIFY, header->idFrom, (LPARAM)header);
}

/* Fills the numbers and the text an item shows, asking the parent for what is left to callback. */
static void resolve_record(combo_state *state, combo_record *record, int index, UINT asked, COMBOBOXEXITEMW *result)
{
	UINT need = callback_mask_for(record, asked);
	result->iImage = record->image;
	result->iSelectedImage = record->selected;
	result->iOverlay = record->overlay;
	result->iIndent = (record->callbacks & HAS_CALLBACK_INDENT) ? 0 : record->indent;
	result->lParam = record->data;
	if (need) {
		NMCOMBOBOXEXW info;
		char narrow[CBEMAXSTRLEN];
		ZeroMemory(&info, sizeof(info));
		info.ceItem.mask = need;
		info.ceItem.iItem = index;
		info.ceItem.iImage = result->iImage;
		info.ceItem.iSelectedImage = result->iSelectedImage;
		info.ceItem.iOverlay = result->iOverlay;
		info.ceItem.iIndent = result->iIndent;
		info.ceItem.lParam = record->data;
		state->answer[0] = 0;
		narrow[0] = 0;
		info.ceItem.pszText = state->ansi_parent ? (WCHAR *)narrow : state->answer;
		info.ceItem.cchTextMax = result->cchTextMax;
		send_notify(state, &info.hdr, CBEN_GETDISPINFOW, CBEN_GETDISPINFOA);
		if (need & CBEIF_IMAGE)
			result->iImage = info.ceItem.iImage;
		if (need & CBEIF_SELECTEDIMAGE)
			result->iSelectedImage = info.ceItem.iSelectedImage;
		if (need & CBEIF_OVERLAY)
			result->iOverlay = info.ceItem.iOverlay;
		if (need & CBEIF_INDENT)
			result->iIndent = info.ceItem.iIndent;
		if (need & CBEIF_TEXT) {
			if (state->ansi_parent && info.ceItem.pszText)
				MultiByteToWideChar(CP_ACP, 0, (const char *)info.ceItem.pszText, -1, state->answer, CBEMAXSTRLEN);
			else if (info.ceItem.pszText && info.ceItem.pszText != state->answer)
				lstrcpynW(state->answer, info.ceItem.pszText, CBEMAXSTRLEN);
		}
		if (info.ceItem.mask & CBEIF_DI_SETITEM) {
			/* The parent says its answer is final: keep it and stop asking. */
			COMBOBOXEXITEMW keep;
			ZeroMemory(&keep, sizeof(keep));
			keep.mask = need;
			keep.iImage = result->iImage;
			keep.iSelectedImage = result->iSelectedImage;
			keep.iOverlay = result->iOverlay;
			keep.iIndent = result->iIndent;
			keep.pszText = state->answer;
			apply_item(record, &keep, FALSE);
		}
	}
	if (asked & CBEIF_TEXT)
		result->pszText = (need & CBEIF_TEXT) ? state->answer : record->text;
}

static const WCHAR *display_text(combo_state *state, combo_record *record, int index)
{
	COMBOBOXEXITEMW view;
	ZeroMemory(&view, sizeof(view));
	view.cchTextMax = CBEMAXSTRLEN;
	resolve_record(state, record, index, CBEIF_TEXT, &view);
	return view.pszText ? view.pszText : L"";
}

/* The edit part */

static void show_in_edit(combo_state *state, const WCHAR *text)
{
	if (!state->edit)
		return;
	state->writing_edit = TRUE;
	SendMessageW(state->edit, WM_SETTEXT, 0, (LPARAM)text);
	state->writing_edit = FALSE;
	SendMessageW(state->edit, EM_SETSEL, 0, 0);
	SendMessageW(state->edit, EM_SETSEL, 0, -1);
	state->edit_dirty = FALSE;
}

static void send_end_edit(combo_state *state, int reason)
{
	int selection = (int)SendMessageW(state->combo, CB_GETCURSEL, 0, 0);
	WCHAR text[CBEMAXSTRLEN];
	text[0] = 0;
	if (state->edit)
		GetWindowTextW(state->edit, text, CBEMAXSTRLEN);
	else if (selection != CB_ERR) {
		combo_record *record = record_at(state, selection);
		if (record)
			lstrcpynW(text, display_text(state, record, selection), CBEMAXSTRLEN);
	}
	if (state->ansi_parent) {
		NMCBEENDEDITA info;
		ZeroMemory(&info, sizeof(info));
		info.fChanged = state->edit_dirty;
		info.iNewSelection = selection;
		info.iWhy = reason;
		WideCharToMultiByte(CP_ACP, 0, text, -1, info.szText, sizeof(info.szText), NULL, NULL);
		send_notify(state, &info.hdr, CBEN_ENDEDITW, CBEN_ENDEDITA);
	}
	else {
		NMCBEENDEDITW info;
		ZeroMemory(&info, sizeof(info));
		info.fChanged = state->edit_dirty;
		info.iNewSelection = selection;
		info.iWhy = reason;
		lstrcpynW(info.szText, text, CBEMAXSTRLEN);
		send_notify(state, &info.hdr, CBEN_ENDEDITW, CBEN_ENDEDITA);
	}
	state->editing = FALSE;
	state->edit_dirty = FALSE;
}

static void selection_changed(combo_state *state)
{
	int selection = (int)SendMessageW(state->combo, CB_GETCURSEL, 0, 0);
	if (selection != CB_ERR && state->edit) {
		combo_record *record = record_at(state, selection);
		if (record)
			show_in_edit(state, display_text(state, record, selection));
	}
	if (state->combo)
		InvalidateRect(state->combo, NULL, FALSE);
}

static int image_width(const combo_state *state)
{
	int width = 0, height = 0;
	if (!state->images || !ImageList_GetIconSize(state->images, &width, &height))
		return 0;
	return width;
}

static void place_edit(const combo_state *state)
{
	COMBOBOXINFO info;
	int shift = 0;
	if (!state->edit)
		return;
	info.cbSize = sizeof(info);
	if (!GetComboBoxInfo(state->combo, &info))
		return;
	if (!(state->extended & CBES_EX_NOEDITIMAGE) && state->images)
		shift = image_width(state) + IMAGE_GAP;
	SetWindowPos(state->edit, 0, info.rcItem.left + shift, info.rcItem.top, info.rcItem.right - info.rcItem.left - shift, info.rcItem.bottom - info.rcItem.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void draw_edit_image(combo_state *state)
{
	COMBOBOXINFO info;
	int selection, height = 0, width = 0;
	combo_record *record;
	COMBOBOXEXITEMW view;
	HDC dc;
	if (!state->edit || !state->images || (state->extended & CBES_EX_NOEDITIMAGE))
		return;
	selection = (int)SendMessageW(state->combo, CB_GETCURSEL, 0, 0);
	if (selection == CB_ERR || !(record = record_at(state, selection)))
		return;
	info.cbSize = sizeof(info);
	if (!GetComboBoxInfo(state->combo, &info))
		return;
	ZeroMemory(&view, sizeof(view));
	resolve_record(state, record, selection, CBEIF_IMAGE, &view);
	if (view.iImage < 0)
		return;
	ImageList_GetIconSize(state->images, &width, &height);
	dc = GetDC(state->combo);
	if (dc) {
		ImageList_Draw(state->images, view.iImage, dc, info.rcItem.left, info.rcItem.top + (info.rcItem.bottom - info.rcItem.top - height) / 2, ILD_TRANSPARENT);
		ReleaseDC(state->combo, dc);
	}
}

static LRESULT CALLBACK edit_filter(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data)
{
	combo_state *state = (combo_state *)data;
	switch (message) {
	case WM_SETFOCUS:
		if (!state->editing) {
			NMHDR header;
			state->editing = TRUE;
			send_notify(state, &header, CBEN_BEGINEDIT, CBEN_BEGINEDIT);
		}
		break;
	case WM_KILLFOCUS:
		if (state->editing && (HWND)wparam != state->combo)
			send_end_edit(state, CBENF_KILLFOCUS);
		break;
	case WM_KEYDOWN:
		if (wparam == VK_RETURN && !SendMessageW(state->combo, CB_GETDROPPEDSTATE, 0, 0)) {
			send_end_edit(state, CBENF_RETURN);
			return 0;
		}
		if (wparam == VK_ESCAPE && !SendMessageW(state->combo, CB_GETDROPPEDSTATE, 0, 0)) {
			state->edit_dirty = FALSE;
			send_end_edit(state, CBENF_ESCAPE);
			selection_changed(state);
			return 0;
		}
		break;
	case WM_NCDESTROY:
		RemoveWindowSubclass(window, edit_filter, id);
		break;
	}
	return DefSubclassProc(window, message, wparam, lparam);
}

static LRESULT CALLBACK combo_filter(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data)
{
	combo_state *state = (combo_state *)data;
	LRESULT result;
	switch (message) {
	case WM_NCDESTROY:
		RemoveWindowSubclass(window, combo_filter, id);
		break;
	case WM_COMMAND:
		/* The real combo answers a change in its edit by dropping the selection, since it has no strings to match. What is
		 * written here comes from the selection, and what the user types is only worth remembering. */
		if ((HWND)lparam == state->edit && HIWORD(wparam) == EN_CHANGE) {
			if (!state->writing_edit)
				state->edit_dirty = TRUE;
			return 0;
		}
		break;
	case WM_PAINT:
		result = DefSubclassProc(window, message, wparam, lparam);
		draw_edit_image(state);
		return result;
	case WM_SIZE:
		result = DefSubclassProc(window, message, wparam, lparam);
		place_edit(state);
		return result;
	}
	return DefSubclassProc(window, message, wparam, lparam);
}

/* Items */

static LRESULT insert_item(combo_state *state, const COMBOBOXEXITEMW *item, BOOL narrow)
{
	int count = count_of(state), index = item->iItem;
	combo_record *record;
	LRESULT placed;
	NMCOMBOBOXEXW info;
	if (index == EDIT_ITEM)
		index = count;
	if (index < 0 || index > count)
		return -1;
	record = allocate(sizeof(*record));
	if (!record)
		return -1;
	init_record(record);
	apply_item(record, item, narrow);
	placed = SendMessageW(state->combo, CB_INSERTSTRING, index, (LPARAM)record);
	if (placed < 0) {
		release(record->text);
		release(record);
		return -1;
	}
	ZeroMemory(&info, sizeof(info));
	info.ceItem = *item;
	info.ceItem.iItem = (int)placed;
	if (state->ansi_parent == narrow)
		send_notify(state, &info.hdr, CBEN_INSERTITEM, CBEN_INSERTITEM);
	else {
		info.ceItem.mask &= ~CBEIF_TEXT;
		info.ceItem.pszText = NULL;
		send_notify(state, &info.hdr, CBEN_INSERTITEM, CBEN_INSERTITEM);
	}
	return placed;
}

static LRESULT set_item(combo_state *state, const COMBOBOXEXITEMW *item, BOOL narrow)
{
	combo_record *record;
	if (item->iItem < EDIT_ITEM || item->iItem >= count_of(state))
		return FALSE;
	record = record_at(state, item->iItem);
	if (!record)
		return FALSE;
	apply_item(record, item, narrow);
	if (item->iItem == EDIT_ITEM) {
		if ((item->mask & CBEIF_TEXT) && !(record->callbacks & HAS_CALLBACK_TEXT))
			show_in_edit(state, record->text ? record->text : L"");
		if (item->mask & (CBEIF_IMAGE | CBEIF_INDENT))
			InvalidateRect(state->combo, NULL, FALSE);
		return TRUE;
	}
	if (item->iItem == (int)SendMessageW(state->combo, CB_GETCURSEL, 0, 0))
		selection_changed(state);
	InvalidateRect(state->combo, NULL, FALSE);
	return TRUE;
}

static LRESULT get_item(combo_state *state, COMBOBOXEXITEMW *item, BOOL narrow)
{
	combo_record *record;
	COMBOBOXEXITEMW view;
	WCHAR edit_text[CBEMAXSTRLEN];
	if (item->iItem < EDIT_ITEM || item->iItem >= count_of(state))
		return FALSE;
	record = record_at(state, item->iItem);
	if (!record)
		return FALSE;
	ZeroMemory(&view, sizeof(view));
	view.cchTextMax = item->cchTextMax;
	resolve_record(state, record, item->iItem, item->mask, &view);
	if (item->mask & CBEIF_IMAGE)
		item->iImage = view.iImage;
	if (item->mask & CBEIF_SELECTEDIMAGE)
		item->iSelectedImage = view.iSelectedImage;
	if (item->mask & CBEIF_OVERLAY)
		item->iOverlay = view.iOverlay;
	if (item->mask & CBEIF_INDENT)
		item->iIndent = view.iIndent;
	if (item->mask & CBEIF_LPARAM)
		item->lParam = view.lParam;
	if (item->mask & CBEIF_TEXT) {
		const WCHAR *text = view.pszText;
		if (item->iItem == EDIT_ITEM && state->edit) {
			GetWindowTextW(state->edit, edit_text, CBEMAXSTRLEN);
			text = edit_text;
		}
		if (!text)
			text = L"";
		if (item->pszText && item->cchTextMax > 0) {
			if (narrow) {
				if (!WideCharToMultiByte(CP_ACP, 0, text, -1, (char *)item->pszText, item->cchTextMax, NULL, NULL))
					((char *)item->pszText)[item->cchTextMax - 1] = 0;
			}
			else {
				lstrcpynW(item->pszText, text, item->cchTextMax);
			}
		}
	}
	return TRUE;
}

static LRESULT delete_item(combo_state *state, int index)
{
	combo_record *record;
	NMCOMBOBOXEXW info;
	if (index < 0 || index >= count_of(state))
		return CB_ERR;
	record = record_at(state, index);
	ZeroMemory(&info, sizeof(info));
	info.ceItem.mask = CBEIF_TEXT | CBEIF_LPARAM;
	info.ceItem.iItem = index;
	info.ceItem.pszText = record ? record->text : NULL;
	info.ceItem.lParam = record ? record->data : 0;
	if (!state->ansi_parent)
		send_notify(state, &info.hdr, CBEN_DELETEITEM, CBEN_DELETEITEM);
	else {
		info.ceItem.mask &= ~CBEIF_TEXT;
		info.ceItem.pszText = NULL;
		send_notify(state, &info.hdr, CBEN_DELETEITEM, CBEN_DELETEITEM);
	}
	return SendMessageW(state->combo, CB_DELETESTRING, index, 0);
}

static void clear_items(combo_state *state)
{
	int index;
	for (index = count_of(state) - 1; index >= 0; index--)
		delete_item(state, index);
}

static int find_text(combo_state *state, const WCHAR *text, int after, BOOL whole)
{
	int count = count_of(state), step, index;
	int length = lstrlenW(text);
	for (step = 0; step < count; step++) {
		combo_record *record;
		const WCHAR *candidate;
		index = (after + 1 + step) % count;
		record = record_at(state, index);
		if (!record)
			continue;
		candidate = display_text(state, record, index);
		if (whole ? !lstrcmpiW(candidate, text) : (CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, candidate, lstrlenW(candidate) < length ? -1 : length, text, length) == CSTR_EQUAL))
			return index;
	}
	return CB_ERR;
}

/* Owner drawing */

static void draw_item(combo_state *state, const DRAWITEMSTRUCT *draw)
{
	BOOL in_edit = (draw->itemState & ODS_COMBOBOXEDIT) != 0;
	BOOL picked = (draw->itemState & ODS_SELECTED) != 0;
	combo_record *record = NULL;
	int index = (int)draw->itemID, x = draw->rcItem.left + 1;
	COMBOBOXEXITEMW view;
	RECT area = draw->rcItem;
	int image;

	if (index >= 0)
		record = (combo_record *)draw->itemData;
	if (!record)
		record = record_at(state, (int)SendMessageW(state->combo, CB_GETCURSEL, 0, 0));
	FillRect(draw->hDC, &area, GetSysColorBrush(picked ? COLOR_HIGHLIGHT : COLOR_WINDOW));
	if (!record)
		return;
	ZeroMemory(&view, sizeof(view));
	view.cchTextMax = CBEMAXSTRLEN;
	resolve_record(state, record, index, CBEIF_TEXT | CBEIF_IMAGE | CBEIF_SELECTEDIMAGE | CBEIF_OVERLAY | CBEIF_INDENT, &view);
	if (!(in_edit && (state->extended & CBES_EX_NOEDITIMAGEINDENT)))
		x += view.iIndent * INDENT_STEP;
	image = picked ? view.iSelectedImage : view.iImage;
	if (image < 0 && picked)
		image = view.iImage;
	if (state->images && !(in_edit && (state->extended & CBES_EX_NOEDITIMAGE))) {
		int width = 0, height = 0;
		ImageList_GetIconSize(state->images, &width, &height);
		if (image >= 0) {
			UINT flags = ILD_TRANSPARENT;
			if (picked)
				flags |= ILD_SELECTED;
			if (view.iOverlay > 0)
				flags |= INDEXTOOVERLAYMASK(view.iOverlay);
			ImageList_Draw(state->images, image, draw->hDC, x, area.top + (area.bottom - area.top - height) / 2, flags);
		}
		x += width + IMAGE_GAP;
	}
	if (view.pszText && view.pszText[0]) {
		COLORREF old_text = SetTextColor(draw->hDC, GetSysColor(picked ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
		int old_mode = SetBkMode(draw->hDC, TRANSPARENT);
		HGDIOBJ old_font = state->font ? SelectObject(draw->hDC, state->font) : NULL;
		RECT line = area;
		line.left = x;
		DrawTextW(draw->hDC, view.pszText, -1, &line, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
		if (old_font)
			SelectObject(draw->hDC, old_font);
		SetBkMode(draw->hDC, old_mode);
		SetTextColor(draw->hDC, old_text);
	}
	if (draw->itemState & ODS_FOCUS)
		DrawFocusRect(draw->hDC, &area);
}

static int row_height(const combo_state *state)
{
	HDC dc = GetDC(state->window);
	HGDIOBJ old = state->font ? SelectObject(dc, state->font) : NULL;
	TEXTMETRICW metrics;
	int height, icon = 0, width = 0;
	GetTextMetricsW(dc, &metrics);
	height = metrics.tmHeight;
	if (old)
		SelectObject(dc, old);
	ReleaseDC(state->window, dc);
	if (state->images && ImageList_GetIconSize(state->images, &width, &icon) && icon > height)
		height = icon;
	return height + 2;
}

/* Sets up the real combo box. */

static BOOL create_children(combo_state *state, const CREATESTRUCTW *create)
{
	DWORD kind = create->style & (CBS_SIMPLE | CBS_DROPDOWN | CBS_DROPDOWNLIST);
	DWORD style = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS | CBS_OWNERDRAWFIXED | CBS_AUTOHSCROLL | kind | (create->style & (CBS_SORT | CBS_NOINTEGRALHEIGHT | CBS_DISABLENOSCROLL));
	RECT client, frame;
	state->drop_height = create->cy > 0 ? create->cy : 100;
	state->combo = CreateWindowExW(0, WC_COMBOBOXW, NULL, style, 0, 0, create->cx, state->drop_height, state->window, (HMENU)(INT_PTR)COMBO_ID, create->hInstance, NULL);
	if (!state->combo)
		return FALSE;
	state->edit = FindWindowExW(state->combo, NULL, WC_EDITW, NULL);
	state->font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	SendMessageW(state->combo, WM_SETFONT, (WPARAM)state->font, FALSE);
	SetWindowSubclass(state->combo, combo_filter, SUBCLASS_ID, (DWORD_PTR)state);
	if (state->edit)
		SetWindowSubclass(state->edit, edit_filter, SUBCLASS_ID, (DWORD_PTR)state);
	GetWindowRect(state->combo, &frame);
	state->closed_height = frame.bottom - frame.top;
	GetClientRect(state->window, &client);
	return TRUE;
}

static int outer_height(const combo_state *state)
{
	RECT outer, inner;
	GetWindowRect(state->window, &outer);
	GetClientRect(state->window, &inner);
	return state->closed_height + (outer.bottom - outer.top) - inner.bottom;
}

static LRESULT CALLBACK comboex_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	combo_state *state = state_of(window);

	if (message == WM_NCCREATE) {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state = allocate(sizeof(*state));
		if (!state)
			return FALSE;
		state->window = window;
		state->parent = create->hwndParent;
		init_record(&state->edit_record);
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	if (!state)
		return DefWindowProcW(window, message, wparam, lparam);

	switch (message) {
	case WM_CREATE:
		state->ansi_parent = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_ANSI;
		if (!create_children(state, (const CREATESTRUCTW *)lparam))
			return -1;
		SetWindowPos(window, 0, 0, 0, ((const CREATESTRUCTW *)lparam)->cx, outer_height(state), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	case WM_NCDESTROY:
		SetWindowLongPtrW(window, 0, 0);
		release(state->edit_record.text);
		release(state);
		break;
	case WM_DESTROY:
		clear_items(state);
		break;
	case WM_DELETEITEM: {
		const DELETEITEMSTRUCT *gone = (const DELETEITEMSTRUCT *)lparam;
		combo_record *record = (combo_record *)gone->itemData;
		if (record) {
			release(record->text);
			release(record);
		}
		return TRUE;
	}
	case WM_MEASUREITEM: {
		MEASUREITEMSTRUCT *measure = (MEASUREITEMSTRUCT *)lparam;
		measure->itemHeight = row_height(state);
		return TRUE;
	}
	case WM_DRAWITEM:
		draw_item(state, (const DRAWITEMSTRUCT *)lparam);
		return TRUE;
	case WM_COMMAND:
		if ((HWND)lparam == state->combo) {
			WORD code = HIWORD(wparam);
			if (code == CBN_DROPDOWN && state->editing)
				send_end_edit(state, CBENF_DROPDOWN);
			if (code == CBN_SELCHANGE || code == CBN_SELENDOK)
				selection_changed(state);
			return SendMessageW(state->parent, WM_COMMAND, MAKEWPARAM(GetWindowLongPtrW(window, GWLP_ID), code), (LPARAM)window);
		}
		break;
	case WM_NOTIFYFORMAT:
		if (lparam == NF_REQUERY)
			state->ansi_parent = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_ANSI;
		return state->ansi_parent ? NFR_ANSI : NFR_UNICODE;
	case WM_WINDOWPOSCHANGING: {
		WINDOWPOS *position = (WINDOWPOS *)lparam;
		if (!(position->flags & SWP_NOSIZE) && state->combo) {
			RECT frame;
			GetWindowRect(state->combo, &frame);
			if (position->cy > state->closed_height)
				state->drop_height = position->cy;
			position->cy = outer_height(state);
		}
		return 0;
	}
	case WM_SIZE:
		if (state->combo) {
			SetWindowPos(state->combo, 0, 0, 0, LOWORD(lparam), state->drop_height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
			place_edit(state);
		}
		return 0;
	case WM_SETFOCUS:
		SetFocus(state->edit ? state->edit : state->combo);
		return 0;
	case WM_SETFONT:
		state->font = wparam ? (HFONT)wparam : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
		SendMessageW(state->combo, WM_SETFONT, (WPARAM)state->font, lparam);
		SendMessageW(state->combo, CB_SETITEMHEIGHT, 0, row_height(state));
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_ERASEBKGND:
		return 1;
	case WM_ENABLE:
		EnableWindow(state->combo, (BOOL)wparam);
		return 0;

	case CBEM_INSERTITEMA:
		return insert_item(state, (const COMBOBOXEXITEMW *)lparam, TRUE);
	case CBEM_INSERTITEMW:
		return insert_item(state, (const COMBOBOXEXITEMW *)lparam, FALSE);
	case CBEM_SETITEMA:
		return set_item(state, (const COMBOBOXEXITEMW *)lparam, TRUE);
	case CBEM_SETITEMW:
		return set_item(state, (const COMBOBOXEXITEMW *)lparam, FALSE);
	case CBEM_GETITEMA:
		return get_item(state, (COMBOBOXEXITEMW *)lparam, TRUE);
	case CBEM_GETITEMW:
		return get_item(state, (COMBOBOXEXITEMW *)lparam, FALSE);
	/* Also CB_DELETESTRING: the two share a number. */
	case CBEM_DELETEITEM:
		return delete_item(state, (int)wparam);
	case CBEM_GETCOMBOCONTROL:
		return (LRESULT)state->combo;
	case CBEM_GETEDITCONTROL:
		return (LRESULT)state->edit;
	case CBEM_GETIMAGELIST:
		return (LRESULT)state->images;
	case CBEM_SETIMAGELIST: {
		HIMAGELIST old = state->images;
		state->images = (HIMAGELIST)lparam;
		SendMessageW(state->combo, CB_SETITEMHEIGHT, 0, row_height(state));
		place_edit(state);
		InvalidateRect(state->combo, NULL, TRUE);
		return (LRESULT)old;
	}
	case CBEM_GETEXTENDEDSTYLE:
		return state->extended;
	case CBEM_SETEXSTYLE:
	case CBEM_SETEXTENDEDSTYLE: {
		DWORD old = state->extended;
		DWORD mask = (message == CBEM_SETEXTENDEDSTYLE && wparam) ? (DWORD)wparam : (DWORD)~0u;
		state->extended = (old & ~mask) | ((DWORD)lparam & mask);
		place_edit(state);
		InvalidateRect(state->combo, NULL, TRUE);
		return old;
	}
	case CBEM_HASEDITCHANGED:
		return state->edit_dirty;
	case CBEM_GETUNICODEFORMAT:
		return !state->narrow_format;
	case CBEM_SETUNICODEFORMAT: {
		BOOL old = !state->narrow_format;
		state->narrow_format = !wparam;
		return old;
	}
	case CBEM_SETWINDOWTHEME:
		return SendMessageW(state->combo, CCM_SETWINDOWTHEME, 0, lparam);

	/* Combo messages that speak about text must read the records, since the real combo has no strings. */
	case CB_GETLBTEXTLEN: {
		combo_record *record;
		if ((int)wparam < 0 || (int)wparam >= count_of(state) || !(record = record_at(state, (int)wparam)))
			return CB_ERR;
		return lstrlenW(display_text(state, record, (int)wparam));
	}
	case CB_GETLBTEXT: {
		combo_record *record;
		const WCHAR *text;
		int length;
		if ((int)wparam < 0 || (int)wparam >= count_of(state) || !(record = record_at(state, (int)wparam)))
			return CB_ERR;
		text = display_text(state, record, (int)wparam);
		length = lstrlenW(text);
		if (lparam)
			CopyMemory((WCHAR *)lparam, text, (length + 1) * sizeof(WCHAR));
		return length;
	}
	case CB_ADDSTRING:
	case CB_INSERTSTRING: {
		COMBOBOXEXITEMW item;
		ZeroMemory(&item, sizeof(item));
		item.mask = CBEIF_TEXT;
		item.iItem = message == CB_ADDSTRING ? -1 : (int)wparam;
		item.pszText = (WCHAR *)lparam;
		return insert_item(state, &item, FALSE);
	}
	case CB_RESETCONTENT:
		clear_items(state);
		return 0;
	case CB_GETITEMDATA: {
		combo_record *record;
		if ((int)wparam < 0 || (int)wparam >= count_of(state) || !(record = record_at(state, (int)wparam)))
			return CB_ERR;
		return record->data;
	}
	case CB_SETITEMDATA: {
		combo_record *record;
		if ((int)wparam < 0 || (int)wparam >= count_of(state) || !(record = record_at(state, (int)wparam)))
			return CB_ERR;
		record->data = lparam;
		return 1;
	}
	case CB_FINDSTRING:
		return find_text(state, (const WCHAR *)lparam, (int)wparam, FALSE);
	case CB_FINDSTRINGEXACT:
		return find_text(state, (const WCHAR *)lparam, (int)wparam, TRUE);
	case CB_SETCURSEL: {
		LRESULT result = SendMessageW(state->combo, message, wparam, lparam);
		selection_changed(state);
		return result;
	}
	case CB_SHOWDROPDOWN:
	case CB_GETCOUNT:
	case CB_GETCURSEL:
	case CB_GETDROPPEDSTATE:
	case CB_GETDROPPEDCONTROLRECT:
	case CB_GETDROPPEDWIDTH:
	case CB_SETDROPPEDWIDTH:
	case CB_GETEDITSEL:
	case CB_SETEDITSEL:
	case CB_GETEXTENDEDUI:
	case CB_SETEXTENDEDUI:
	case CB_GETHORIZONTALEXTENT:
	case CB_SETHORIZONTALEXTENT:
	case CB_GETITEMHEIGHT:
	case CB_SETITEMHEIGHT:
	case CB_GETTOPINDEX:
	case CB_SETTOPINDEX:
	case CB_LIMITTEXT:
	case CB_GETCOMBOBOXINFO:
		return SendMessageW(state->combo, message, wparam, lparam);
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void COMBOEX_Register(void)
{
	WNDCLASSW info;

	ZeroMemory(&info, sizeof(info));
	info.style = CS_GLOBALCLASS;
	info.lpfnWndProc = comboex_proc;
	info.cbWndExtra = sizeof(combo_state *);
	info.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	info.lpszClassName = WC_COMBOBOXEXW;
	RegisterClassW(&info);
}

void COMBOEX_Unregister(void)
{
	UnregisterClassW(WC_COMBOBOXEXW, NULL);
}
