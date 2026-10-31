/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>

/* The header control. Items keep their insertion index; order is a per-item number walked when laying out. Text is stored wide, A messages
 * convert at the edge and notifications follow the format the parent asked for. */

#define KNOWN_V4_FIELDS (HDI_WIDTH | HDI_TEXT | HDI_FORMAT | HDI_LPARAM | HDI_BITMAP)
#define KNOWN_FIELDS (KNOWN_V4_FIELDS | HDI_IMAGE | HDI_ORDER | HDI_FILTER)
#define DIVIDER_ZONE 5
#define TEXT_MARGIN 6
#define DEFAULT_FILTER_TIMEOUT 1000
#define DRAG_THRESHOLD 4
#define NO_ITEM (-1)
#define DIVIDER_MARK_WIDTH 2

enum { GESTURE_NONE, GESTURE_PRESS, GESTURE_TRACK, GESTURE_DRAG };

typedef struct {
	int width, order, image, format;
	WCHAR *text;
	BOOL callback;
	LPARAM lparam;
	HBITMAP bitmap;
} header_item;

typedef struct {
	HWND window, parent;
	header_item *items;
	int count;
	HFONT font;
	HIMAGELIST images;
	BOOL unicode;
	int bitmap_margin, filter_timeout, hot_divider, hot_item;
	int gesture, active, grip_offset, track_start;
	POINT press_at;
	HTHEME theme;
} header_state;

static header_state *state_of(HWND window)
{
	return (header_state *)GetWindowLongPtrW(window, 0);
}

static DWORD style_of(const header_state *state)
{
	return (DWORD)GetWindowLongPtrW(state->window, GWL_STYLE);
}

static void *allocate(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void release(void *block)
{
	HeapFree(GetProcessHeap(), 0, block);
}

static WCHAR *widen(const char *text)
{
	int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	WCHAR *copy = allocate(length * sizeof(WCHAR));
	if (copy)
		MultiByteToWideChar(CP_ACP, 0, text, -1, copy, length);
	return copy;
}

static WCHAR *duplicate(const WCHAR *text)
{
	WCHAR *copy = allocate((lstrlenW(text) + 1) * sizeof(WCHAR));
	if (copy)
		lstrcpyW(copy, text);
	return copy;
}

static char *narrow(const WCHAR *text)
{
	int length = WideCharToMultiByte(CP_ACP, 0, text, -1, NULL, 0, NULL, NULL);
	char *copy = allocate(length);
	if (copy)
		WideCharToMultiByte(CP_ACP, 0, text, -1, copy, length, NULL, NULL);
	return copy;
}

static int index_of_order(const header_state *state, int order)
{
	int index;
	for (index = 0; index < state->count; ++index)
		if (state->items[index].order == order)
			return index;
	return NO_ITEM;
}

static RECT item_rect(const header_state *state, int wanted)
{
	RECT client, area = {0};
	int order, x = 0;
	GetClientRect(state->window, &client);
	for (order = 0; order < state->count; ++order) {
		int index = index_of_order(state, order);
		if (index == wanted) {
			SetRect(&area, x, 0, x + state->items[index].width, client.bottom);
			break;
		}
		x += state->items[index].width;
	}
	return area;
}

static int total_width(const header_state *state)
{
	int index, total = 0;
	for (index = 0; index < state->count; ++index)
		total += state->items[index].width;
	return total;
}

static BOOL item_can_size(const header_state *state, int index)
{
	return !(style_of(state) & HDS_NOSIZING) && !(state->items[index].format & HDF_FIXEDWIDTH);
}

static int hit_test(const header_state *state, POINT point, UINT *flags)
{
	RECT client;
	int order, x = 0;
	GetClientRect(state->window, &client);
	*flags = 0;
	if (point.y < 0)
		*flags |= HHT_ABOVE;
	else if (point.y >= client.bottom)
		*flags |= HHT_BELOW;
	if (point.x < 0)
		*flags |= HHT_TOLEFT;
	else if (point.x >= client.right)
		*flags |= HHT_TORIGHT;
	if (*flags)
		return NO_ITEM;
	for (order = 0; order < state->count; ++order) {
		int index = index_of_order(state, order), right = x + state->items[index].width;
		if (point.x >= x && point.x < right) {
			if (point.x >= right - DIVIDER_ZONE && item_can_size(state, index)) {
				*flags = HHT_ONDIVIDER;
			} else if (point.x < x + DIVIDER_ZONE && order && item_can_size(state, index_of_order(state, order - 1))) {
				*flags = HHT_ONDIVIDER;
				return index_of_order(state, order - 1);
			} else {
				*flags = HHT_ONHEADER;
			}
			return index;
		}
		x = right;
	}
	*flags = HHT_NOWHERE;
	return NO_ITEM;
}

static int text_height(const header_state *state)
{
	TEXTMETRICW metrics;
	HDC dc = GetDC(state->window);
	HGDIOBJ old = SelectObject(dc, state->font ? state->font : GetStockObject(DEFAULT_GUI_FONT));
	GetTextMetricsW(dc, &metrics);
	SelectObject(dc, old);
	ReleaseDC(state->window, dc);
	return metrics.tmHeight;
}

/* A notification carries a copy of the item in the format the parent reads. */
static LRESULT notify_item(const header_state *state, UINT wide_code, UINT narrow_code, int index, int button, HDITEMW *item)
{
	NMHEADERW notice;
	HDITEMW sent;
	char *text = NULL;
	LRESULT answer;
	notice.hdr.hwndFrom = state->window;
	notice.hdr.idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	notice.hdr.code = state->unicode ? wide_code : narrow_code;
	notice.iItem = index;
	notice.iButton = button;
	notice.pitem = item;
	if (item && !state->unicode && (item->mask & HDI_TEXT) && item->pszText && item->pszText != LPSTR_TEXTCALLBACKW) {
		sent = *item;
		text = narrow(item->pszText);
		sent.pszText = (WCHAR *)text;
		notice.pitem = &sent;
	}
	answer = SendMessageW(state->parent, WM_NOTIFY, notice.hdr.idFrom, (LPARAM)&notice);
	release(text);
	return answer;
}

static void notify_plain(const header_state *state, UINT code)
{
	NMHDR notice;
	notice.hwndFrom = state->window;
	notice.idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	notice.code = code;
	SendMessageW(state->parent, WM_NOTIFY, notice.idFrom, (LPARAM)&notice);
}

static void move_order(header_state *state, header_item *item, int order)
{
	int index, old = item->order;
	for (index = 0; index < state->count; ++index) {
		header_item *other = &state->items[index];
		if (other == item)
			continue;
		if (old < order && other->order > old && other->order <= order)
			--other->order;
		else if (old > order && other->order >= order && other->order < old)
			++other->order;
	}
	item->order = order;
}

static void apply_fields(header_state *state, header_item *item, const HDITEMW *given, BOOL wide)
{
	UINT mask = given->mask;
	if (mask & ~KNOWN_FIELDS)
		mask &= KNOWN_V4_FIELDS;
	if (mask & HDI_WIDTH)
		item->width = given->cxy;
	if (mask & HDI_FORMAT) {
		/* These three follow what the item actually holds; only HDF_IMAGE can be asked for without one. */
		int kept = item->format & (HDF_STRING | HDF_BITMAP);
		item->format = (given->fmt & ~(HDF_STRING | HDF_BITMAP)) | kept;
	}
	if (mask & HDI_LPARAM)
		item->lparam = given->lParam;
	if (mask & HDI_IMAGE) {
		item->image = given->iImage;
		item->format |= HDF_IMAGE;
	}
	if (mask & HDI_BITMAP) {
		item->bitmap = given->hbm;
		item->format = given->hbm ? item->format | HDF_BITMAP : item->format & ~HDF_BITMAP;
	}
	if (mask & HDI_TEXT) {
		release(item->text);
		item->text = NULL;
		item->callback = FALSE;
		if (given->pszText == LPSTR_TEXTCALLBACKW) {
			item->callback = TRUE;
		} else if (given->pszText) {
			item->text = wide ? duplicate(given->pszText) : widen((const char *)given->pszText);
		}
		item->format = given->pszText ? item->format | HDF_STRING : item->format & ~HDF_STRING;
	}
	if ((mask & HDI_ORDER) && given->iOrder >= 0 && given->iOrder < state->count)
		move_order(state, item, given->iOrder);
}

static int insert_item(header_state *state, int index, const HDITEMW *given, BOOL wide)
{
	header_item *grown;
	int at, order;
	if (!(given->mask & KNOWN_FIELDS))
		return -1;
	grown = state->items ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, state->items, (state->count + 1) * sizeof(*grown))
		: allocate(sizeof(*grown));
	if (!grown)
		return -1;
	state->items = grown;
	index = max(min(index, state->count), 0);
	order = (given->mask & HDI_ORDER) ? max(min(given->iOrder, state->count), 0) : index;
	memmove(&grown[index + 1], &grown[index], (state->count - index) * sizeof(*grown));
	memset(&grown[index], 0, sizeof(*grown));
	++state->count;
	for (at = 0; at < state->count; ++at)
		if (at != index && grown[at].order >= order)
			++grown[at].order;
	grown[index].order = order;
	{
		HDITEMW fields = *given;
		fields.mask &= ~HDI_ORDER;
		apply_fields(state, &grown[index], &fields, wide);
	}
	InvalidateRect(state->window, NULL, TRUE);
	return index;
}

static BOOL delete_item(header_state *state, int index)
{
	int at, order;
	if (index < 0 || index >= state->count)
		return FALSE;
	order = state->items[index].order;
	release(state->items[index].text);
	--state->count;
	memmove(&state->items[index], &state->items[index + 1], (state->count - index) * sizeof(*state->items));
	for (at = 0; at < state->count; ++at)
		if (state->items[at].order > order)
			--state->items[at].order;
	InvalidateRect(state->window, NULL, TRUE);
	return TRUE;
}

/* Asks the parent for what a callback item shows. Only text and images can be left to it. */
static void query_callback(const header_state *state, int index, WCHAR *out, int size)
{
	const header_item *item = &state->items[index];
	NMHDDISPINFOW answer = {0};
	out[0] = 0;
	answer.hdr.hwndFrom = state->window;
	answer.hdr.idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	answer.hdr.code = state->unicode ? HDN_GETDISPINFOW : HDN_GETDISPINFOA;
	answer.iItem = index;
	answer.mask = HDI_TEXT;
	answer.lParam = item->lparam;
	answer.pszText = out;
	answer.cchTextMax = size;
	SendMessageW(state->parent, WM_NOTIFY, answer.hdr.idFrom, (LPARAM)&answer);
	if (state->unicode) {
		if (answer.pszText && answer.pszText != out)
			lstrcpynW(out, answer.pszText, size);
	} else if (answer.pszText) {
		char copy[MAX_PATH];
		lstrcpynA(copy, (const char *)answer.pszText, sizeof(copy));
		MultiByteToWideChar(CP_ACP, 0, copy, -1, out, size);
	}
}

static BOOL get_item(const header_state *state, int index, HDITEMW *out, BOOL wide)
{
	const header_item *item;
	UINT mask = out->mask;
	if (index < 0 || index >= state->count)
		return FALSE;
	item = &state->items[index];
	if (!mask)
		return TRUE;
	if (mask & ~KNOWN_FIELDS)
		mask &= KNOWN_V4_FIELDS;
	if (out->mask & ~KNOWN_FIELDS) {
		if (out->mask & HDI_IMAGE)
			out->iImage = 0;
		if (out->mask & HDI_ORDER)
			out->iOrder = 0;
	}
	if (mask & HDI_WIDTH)
		out->cxy = item->width;
	if (mask & HDI_FORMAT)
		out->fmt = item->format;
	if (mask & HDI_LPARAM)
		out->lParam = item->lparam;
	if (mask & HDI_BITMAP)
		out->hbm = item->bitmap;
	if (mask & HDI_IMAGE)
		out->iImage = item->image;
	if (mask & HDI_ORDER)
		out->iOrder = item->order;
	if (mask & HDI_TEXT) {
		if (item->callback && out->pszText == NULL) {
			out->pszText = LPSTR_TEXTCALLBACKW;
		} else if (out->pszText && out->cchTextMax > 0) {
			WCHAR queried[MAX_PATH];
			const WCHAR *text = item->text ? item->text : L"";
			if (item->callback) {
				query_callback(state, index, queried, ARRAYSIZE(queried));
				text = queried;
			}
			if (wide) {
				lstrcpynW(out->pszText, text, out->cchTextMax);
			} else if (!WideCharToMultiByte(CP_ACP, 0, text, -1, (char *)out->pszText, out->cchTextMax, NULL, NULL)) {
				((char *)out->pszText)[out->cchTextMax - 1] = 0;
			}
		}
	}
	return TRUE;
}

static BOOL set_item(header_state *state, int index, const HDITEMW *given, BOOL wide)
{
	HDITEMW sent;
	WCHAR *converted = NULL;
	if (index < 0 || index >= state->count)
		return FALSE;
	sent = *given;
	if (!wide && (given->mask & HDI_TEXT) && given->pszText && given->pszText != LPSTR_TEXTCALLBACKW) {
		converted = widen((const char *)given->pszText);
		sent.pszText = converted;
	}
	if (notify_item(state, HDN_ITEMCHANGINGW, HDN_ITEMCHANGINGA, index, 0, &sent)) {
		release(converted);
		return FALSE;
	}
	apply_fields(state, &state->items[index], &sent, TRUE);
	notify_item(state, HDN_ITEMCHANGEDW, HDN_ITEMCHANGEDA, index, 0, &sent);
	release(converted);
	InvalidateRect(state->window, NULL, TRUE);
	return TRUE;
}

static void draw_sort_arrow(HDC dc, const RECT *area, BOOL up)
{
	int center = area->right - TEXT_MARGIN - 3, middle = (area->top + area->bottom) / 2, row;
	HBRUSH brush = GetSysColorBrush(COLOR_BTNTEXT);
	for (row = 0; row < 4; ++row) {
		RECT line = {center - row, up ? middle - 2 + row : middle + 2 - row, center + row + 1, 0};
		line.bottom = line.top + 1;
		FillRect(dc, &line, brush);
	}
}

static void draw_item_content(header_state *state, HDC dc, int index, RECT area)
{
	header_item *item = &state->items[index];
	WCHAR queried[MAX_PATH];
	const WCHAR *text = item->text;
	int alignment = item->format & HDF_JUSTIFYMASK;
	UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
	flags |= alignment == HDF_CENTER ? DT_CENTER : alignment == HDF_RIGHT ? DT_RIGHT : DT_LEFT;
	if (item->callback) {
		query_callback(state, index, queried, ARRAYSIZE(queried));
		text = queried;
	}
	area.left += TEXT_MARGIN;
	area.right -= TEXT_MARGIN;
	if (item->format & (HDF_SORTUP | HDF_SORTDOWN)) {
		RECT whole = area;
		whole.right += TEXT_MARGIN;
		draw_sort_arrow(dc, &whole, (item->format & HDF_SORTUP) != 0);
		area.right -= 2 * TEXT_MARGIN;
	}
	if ((item->format & HDF_IMAGE) && state->images && item->image >= 0) {
		int width, height;
		ImageList_GetIconSize(state->images, &width, &height);
		ImageList_Draw(state->images, item->image, dc, area.left, area.top + (area.bottom - area.top - height) / 2, ILD_TRANSPARENT);
		area.left += width + state->bitmap_margin;
	}
	if ((item->format & HDF_BITMAP) && item->bitmap) {
		BITMAP info;
		HDC source = CreateCompatibleDC(dc);
		if (source && GetObjectW(item->bitmap, sizeof(info), &info)) {
			HGDIOBJ old = SelectObject(source, item->bitmap);
			BitBlt(dc, area.left, area.top + (area.bottom - area.top - info.bmHeight) / 2, info.bmWidth, info.bmHeight, source, 0, 0, SRCCOPY);
			SelectObject(source, old);
			area.left += info.bmWidth + state->bitmap_margin;
		}
		if (source)
			DeleteDC(source);
	}
	if (text && ((item->format & HDF_STRING) || item->callback))
		DrawTextW(dc, text, -1, &area, flags);
}

static void draw_item(header_state *state, HDC dc, NMCUSTOMDRAW *draw, BOOL wants_items, int index)
{
	header_item *item = &state->items[index];
	RECT area = item_rect(state, index);
	BOOL pressed = state->gesture == GESTURE_PRESS && state->active == index && (style_of(state) & HDS_BUTTONS);
	LRESULT answer = CDRF_DODEFAULT;
	if (area.right <= area.left)
		return;
	if (state->theme) {
		DrawThemeBackground(state->theme, dc, HP_HEADERITEM, pressed ? HIS_PRESSED : state->hot_item == index ? HIS_HOT : HIS_NORMAL, &area, NULL);
	} else {
		FillRect(dc, &area, GetSysColorBrush(COLOR_3DFACE));
		DrawEdge(dc, &area, pressed ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT | BF_SOFT);
	}
	if (wants_items) {
		draw->dwDrawStage = CDDS_ITEMPREPAINT;
		draw->dwItemSpec = index;
		draw->rc = area;
		draw->lItemlParam = item->lparam;
		draw->uItemState = pressed ? CDIS_SELECTED : 0;
		answer = SendMessageW(state->parent, WM_NOTIFY, draw->hdr.idFrom, (LPARAM)draw);
		if (answer & CDRF_SKIPDEFAULT)
			return;
	}
	if (item->format & HDF_OWNERDRAW) {
		DRAWITEMSTRUCT owner = {0};
		owner.CtlType = ODT_HEADER;
		owner.CtlID = (UINT)GetWindowLongPtrW(state->window, GWLP_ID);
		owner.itemID = index;
		owner.itemAction = ODA_DRAWENTIRE;
		owner.itemState = pressed ? ODS_SELECTED : 0;
		owner.hwndItem = state->window;
		owner.hDC = dc;
		owner.rcItem = area;
		owner.itemData = item->lparam;
		SendMessageW(state->parent, WM_DRAWITEM, owner.CtlID, (LPARAM)&owner);
	} else {
		draw_item_content(state, dc, index, area);
	}
	if (wants_items && (answer & CDRF_NOTIFYPOSTPAINT)) {
		draw->dwDrawStage = CDDS_ITEMPOSTPAINT;
		SendMessageW(state->parent, WM_NOTIFY, draw->hdr.idFrom, (LPARAM)draw);
	}
}

static void paint(header_state *state, HDC dc)
{
	NMCUSTOMDRAW draw = {0};
	RECT client, rest;
	HGDIOBJ old;
	LRESULT answer;
	int order;
	GetClientRect(state->window, &client);
	old = SelectObject(dc, state->font ? state->font : GetStockObject(DEFAULT_GUI_FONT));
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
	rest = client;
	rest.left = total_width(state);
	FillRect(dc, &client, GetSysColorBrush(COLOR_3DFACE));
	if (state->theme)
		DrawThemeBackground(state->theme, dc, HP_HEADERITEM, HIS_NORMAL, &rest, NULL);
	draw.hdr.hwndFrom = state->window;
	draw.hdr.idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	draw.hdr.code = NM_CUSTOMDRAW;
	draw.hdc = dc;
	draw.rc = client;
	draw.dwDrawStage = CDDS_PREPAINT;
	answer = SendMessageW(state->parent, WM_NOTIFY, draw.hdr.idFrom, (LPARAM)&draw);
	if (!(answer & CDRF_SKIPDEFAULT))
		for (order = 0; order < state->count; ++order)
			draw_item(state, dc, &draw, (answer & CDRF_NOTIFYITEMDRAW) != 0, index_of_order(state, order));
	if (state->hot_divider >= 0) {
		int x = 0;
		for (order = 0; order < state->hot_divider && order < state->count; ++order)
			x += state->items[index_of_order(state, order)].width;
		rest = client;
		rest.left = x - DIVIDER_MARK_WIDTH / 2;
		rest.right = rest.left + DIVIDER_MARK_WIDTH;
		FillRect(dc, &rest, GetSysColorBrush(COLOR_HIGHLIGHT));
	}
	if (answer & CDRF_NOTIFYPOSTPAINT) {
		draw.dwDrawStage = CDDS_POSTPAINT;
		draw.rc = client;
		SendMessageW(state->parent, WM_NOTIFY, draw.hdr.idFrom, (LPARAM)&draw);
	}
	SelectObject(dc, old);
}

static void finish_gesture(header_state *state)
{
	state->gesture = GESTURE_NONE;
	state->active = NO_ITEM;
	if (GetCapture() == state->window)
		ReleaseCapture();
}

static void track_notice(header_state *state, UINT wide_code, UINT narrow_code, int width)
{
	HDITEMW sent = {0};
	sent.mask = HDI_WIDTH;
	sent.cxy = max(width, 0);
	notify_item(state, wide_code, narrow_code, state->active, 0, &sent);
}

static void mouse_down(header_state *state, POINT point)
{
	UINT flags;
	int index = hit_test(state, point, &flags);
	if (index == NO_ITEM)
		return;
	state->press_at = point;
	SetCapture(state->window);
	state->active = index;
	if (flags == HHT_ONDIVIDER) {
		HDITEMW sent = {0};
		sent.mask = HDI_WIDTH;
		sent.cxy = state->items[index].width;
		if (notify_item(state, HDN_BEGINTRACKW, HDN_BEGINTRACKA, index, 0, &sent)) {
			finish_gesture(state);
			return;
		}
		state->gesture = GESTURE_TRACK;
		state->track_start = state->items[index].width;
		state->grip_offset = point.x;
		return;
	}
	state->gesture = GESTURE_PRESS;
	InvalidateRect(state->window, NULL, TRUE);
}

static void mouse_move(header_state *state, POINT point)
{
	UINT flags;
	if (state->gesture == GESTURE_TRACK) {
		int width = max(state->track_start + point.x - state->grip_offset, 0);
		track_notice(state, HDN_TRACKW, HDN_TRACKA, width);
		if (style_of(state) & HDS_FULLDRAG) {
			state->items[state->active].width = width;
			InvalidateRect(state->window, NULL, TRUE);
		}
		return;
	}
	if (state->gesture == GESTURE_PRESS && (style_of(state) & HDS_DRAGDROP) && (point.x > state->press_at.x + DRAG_THRESHOLD || point.x < state->press_at.x - DRAG_THRESHOLD)) {
		if (!notify_item(state, HDN_BEGINDRAG, HDN_BEGINDRAG, state->active, 0, NULL))
			state->gesture = GESTURE_DRAG;
		return;
	}
	if (style_of(state) & HDS_HOTTRACK) {
		int hot = hit_test(state, point, &flags);
		if (hot != state->hot_item) {
			TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, state->window, 0};
			state->hot_item = hot;
			TrackMouseEvent(&track);
			InvalidateRect(state->window, NULL, TRUE);
		}
	}
}

static void mouse_up(header_state *state, POINT point)
{
	int gesture = state->gesture, active = state->active;
	UINT flags;
	if (gesture == GESTURE_TRACK) {
		int width = max(state->track_start + point.x - state->grip_offset, 0);
		HDITEMW sent = {0};
		sent.mask = HDI_WIDTH;
		sent.cxy = width;
		finish_gesture(state);
		notify_item(state, HDN_ENDTRACKW, HDN_ENDTRACKA, active, 0, &sent);
		set_item(state, active, &sent, TRUE);
	} else if (gesture == GESTURE_DRAG) {
		int target = hit_test(state, point, &flags);
		HDITEMW sent = {0};
		finish_gesture(state);
		sent.mask = HDI_ORDER;
		sent.iOrder = target == NO_ITEM ? state->count - 1 : state->items[target].order;
		if (!notify_item(state, HDN_ENDDRAG, HDN_ENDDRAG, active, 0, &sent)) {
			move_order(state, &state->items[active], sent.iOrder);
			InvalidateRect(state->window, NULL, TRUE);
		}
	} else if (gesture == GESTURE_PRESS) {
		BOOL inside = hit_test(state, point, &flags) == active;
		finish_gesture(state);
		InvalidateRect(state->window, NULL, TRUE);
		if (inside)
			notify_item(state, HDN_ITEMCLICKW, HDN_ITEMCLICKA, active, 0, NULL);
	}
}

static LRESULT CALLBACK header_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	header_state *state = state_of(window);
	BOOL wide = TRUE;
	if (!state && message != WM_NCCREATE)
		return DefWindowProcW(window, message, wparam, lparam);
	switch (message) {
	case WM_NCCREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state = allocate(sizeof(*state));
		if (!state)
			return FALSE;
		state->window = window;
		state->parent = create->hwndParent;
		state->filter_timeout = DEFAULT_FILTER_TIMEOUT;
		state->hot_divider = -1;
		state->hot_item = NO_ITEM;
		state->active = NO_ITEM;
		state->bitmap_margin = 3 * GetSystemMetrics(SM_CXEDGE);
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_CREATE:
		state->unicode = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_UNICODE;
		state->theme = OpenThemeData(window, L"Header");
		return 0;
	case WM_DESTROY:
		CloseThemeData(state->theme);
		state->theme = NULL;
		return 0;
	case WM_NCDESTROY:
		while (state->count)
			delete_item(state, state->count - 1);
		release(state->items);
		release(state);
		SetWindowLongPtrW(window, 0, 0);
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_THEMECHANGED:
		CloseThemeData(state->theme);
		state->theme = OpenThemeData(window, L"Header");
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_NOTIFYFORMAT:
		if (lparam == NF_REQUERY)
			state->unicode = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_UNICODE;
		return state->unicode ? NFR_UNICODE : NFR_ANSI;
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		if (LOWORD(lparam))
			InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
	case WM_PRINTCLIENT:
		if (wparam) {
			paint(state, (HDC)wparam);
		} else {
			PAINTSTRUCT painting;
			HDC dc = BeginPaint(window, &painting);
			paint(state, dc);
			EndPaint(window, &painting);
		}
		return 0;
	case WM_SETCURSOR: {
		POINT point;
		UINT flags;
		GetCursorPos(&point);
		ScreenToClient(window, &point);
		hit_test(state, point, &flags);
		if (flags == HHT_ONDIVIDER) {
			SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEWE));
			return TRUE;
		}
		return DefWindowProcW(window, message, wparam, lparam);
	}
	case WM_LBUTTONDOWN: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		mouse_down(state, point);
		return 0;
	}
	case WM_MOUSEMOVE: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		mouse_move(state, point);
		return 0;
	}
	case WM_LBUTTONUP: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		mouse_up(state, point);
		return 0;
	}
	case WM_MOUSELEAVE:
		state->hot_item = NO_ITEM;
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_CAPTURECHANGED:
		state->gesture = GESTURE_NONE;
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_LBUTTONDBLCLK: {
		POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
		UINT flags;
		int index = hit_test(state, point, &flags);
		if (index == NO_ITEM)
			return 0;
		if (flags == HHT_ONDIVIDER)
			notify_item(state, HDN_DIVIDERDBLCLICKW, HDN_DIVIDERDBLCLICKA, index, 0, NULL);
		else
			notify_item(state, HDN_ITEMDBLCLICKW, HDN_ITEMDBLCLICKA, index, 0, NULL);
		return 0;
	}
	case WM_RBUTTONUP:
		notify_plain(state, NM_RCLICK);
		return DefWindowProcW(window, message, wparam, lparam);
	case HDM_GETITEMCOUNT:
		return state->count;
	case HDM_INSERTITEMA:
		wide = FALSE;
		/* fall through */
	case HDM_INSERTITEMW:
		return lparam ? insert_item(state, (int)wparam, (const HDITEMW *)lparam, wide) : -1;
	case HDM_DELETEITEM:
		return delete_item(state, (int)wparam);
	case HDM_GETITEMA:
		wide = FALSE;
		/* fall through */
	case HDM_GETITEMW:
		return lparam ? get_item(state, (int)wparam, (HDITEMW *)lparam, wide) : FALSE;
	case HDM_SETITEMA:
		wide = FALSE;
		/* fall through */
	case HDM_SETITEMW:
		return lparam ? set_item(state, (int)wparam, (const HDITEMW *)lparam, wide) : FALSE;
	case HDM_LAYOUT: {
		HDLAYOUT *layout = (HDLAYOUT *)lparam;
		int height = (style_of(state) & HDS_HIDDEN) ? 0 : text_height(state) + 2 * GetSystemMetrics(SM_CYEDGE) + 2;
		if (!layout)
			return FALSE;
		layout->pwpos->hwndInsertAfter = NULL;
		layout->pwpos->x = layout->prc->left;
		layout->pwpos->y = layout->prc->top;
		layout->pwpos->cx = layout->prc->right - layout->prc->left;
		layout->pwpos->cy = height;
		layout->pwpos->flags = SWP_NOZORDER;
		layout->prc->top += height;
		return TRUE;
	}
	case HDM_HITTEST: {
		HDHITTESTINFO *hit = (HDHITTESTINFO *)lparam;
		if (!hit)
			return -1;
		hit->iItem = hit_test(state, hit->pt, &hit->flags);
		return hit->iItem;
	}
	case HDM_GETITEMRECT: {
		RECT *area = (RECT *)lparam;
		if ((int)wparam < 0 || (int)wparam >= state->count || !area)
			return FALSE;
		*area = item_rect(state, (int)wparam);
		return TRUE;
	}
	case HDM_SETIMAGELIST: {
		HIMAGELIST previous = state->images;
		state->images = (HIMAGELIST)lparam;
		InvalidateRect(window, NULL, TRUE);
		return (LRESULT)previous;
	}
	case HDM_GETIMAGELIST:
		return (LRESULT)state->images;
	case HDM_ORDERTOINDEX: {
		int index = index_of_order(state, (int)wparam);
		return index == NO_ITEM ? (LRESULT)wparam : index;
	}
	case HDM_CREATEDRAGIMAGE: {
		RECT area;
		HIMAGELIST list;
		HDC screen, memory;
		HBITMAP bitmap;
		HGDIOBJ old, old_font;
		if ((int)wparam < 0 || (int)wparam >= state->count)
			return 0;
		area = item_rect(state, (int)wparam);
		OffsetRect(&area, -area.left, 0);
		if (area.right <= 0 || area.bottom <= 0)
			return 0;
		screen = GetDC(window);
		memory = CreateCompatibleDC(screen);
		bitmap = CreateCompatibleBitmap(screen, area.right, area.bottom);
		old = SelectObject(memory, bitmap);
		old_font = SelectObject(memory, state->font ? state->font : GetStockObject(DEFAULT_GUI_FONT));
		SetBkMode(memory, TRANSPARENT);
		FillRect(memory, &area, GetSysColorBrush(COLOR_3DFACE));
		draw_item_content(state, memory, (int)wparam, area);
		SelectObject(memory, old_font);
		SelectObject(memory, old);
		DeleteDC(memory);
		ReleaseDC(window, screen);
		list = ImageList_Create(area.right, area.bottom, ILC_COLOR32, 1, 1);
		if (list)
			ImageList_Add(list, bitmap, NULL);
		DeleteObject(bitmap);
		return (LRESULT)list;
	}
	case HDM_GETORDERARRAY: {
		int order, *out = (int *)lparam;
		if (!out || (int)wparam != state->count)
			return FALSE;
		for (order = 0; order < state->count; ++order)
			out[order] = index_of_order(state, order);
		return TRUE;
	}
	case HDM_SETORDERARRAY: {
		const int *given = (const int *)lparam;
		int order;
		if (!given || (int)wparam != state->count)
			return FALSE;
		for (order = 0; order < state->count; ++order)
			if (given[order] < 0 || given[order] >= state->count)
				return FALSE;
		for (order = 0; order < state->count; ++order)
			state->items[given[order]].order = order;
		InvalidateRect(window, NULL, TRUE);
		return TRUE;
	}
	case HDM_SETHOTDIVIDER: {
		int divider = (int)lparam;
		if (wparam) {
			POINT point = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
			UINT flags;
			int index;
			ScreenToClient(window, &point);
			index = hit_test(state, point, &flags);
			divider = index == NO_ITEM ? state->count : state->items[index].order;
		}
		state->hot_divider = divider;
		InvalidateRect(window, NULL, TRUE);
		return divider;
	}
	case HDM_SETBITMAPMARGIN: {
		int previous = state->bitmap_margin;
		state->bitmap_margin = (int)wparam;
		return previous;
	}
	case HDM_GETBITMAPMARGIN:
		return state->bitmap_margin;
	case HDM_SETFILTERCHANGETIMEOUT: {
		int previous = state->filter_timeout;
		if (lparam)
			state->filter_timeout = (int)lparam;
		return previous;
	}
	case HDM_EDITFILTER:
	case HDM_CLEARFILTER:
		return 1;
	case HDM_SETUNICODEFORMAT: {
		BOOL previous = state->unicode;
		state->unicode = wparam != 0;
		return previous;
	}
	case HDM_GETUNICODEFORMAT:
		return state->unicode;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void HEADER_Register(void)
{
	WNDCLASSW definition = {0};
	definition.style = CS_GLOBALCLASS | CS_DBLCLKS;
	definition.lpfnWndProc = header_procedure;
	definition.cbWndExtra = sizeof(void *);
	definition.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	definition.lpszClassName = WC_HEADERW;
	RegisterClassW(&definition);
}

void HEADER_Unregister(void)
{
	UnregisterClassW(WC_HEADERW, NULL);
}
