/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>

/* Layout works in two axes, "along" the bar and "across" it, so a vertical rebar is the same code with the results swapped
 * on the way out. Bands are kept in display order; a row is a run of bands, and only the last of a row absorbs what is left. */

#define GRIPPER_WIDTH 5
#define GRIPPER_ALONE_EXTRA 4
#define TEXT_PADDING 8
#define IMAGE_PADDING 3
#define POST_CHILD 4
#define CHILD_EDGE 4
#define CHEVRON_WIDTH 11
#define ROW_BORDER 2
#define DIVIDER_HEIGHT 2
#define CUSTOM_HEADER 0x40000000
#define PUBLIC_V3_SIZE 80

typedef struct {
	UINT style;
	COLORREF fore, back;
	WCHAR *text;
	int image;
	HWND child;
	int min_along, min_across, width;
	HBITMAP bitmap;
	UINT id;
	int current_across, max_across, step_across;
	int ideal;
	LPARAM data;
	int header;
	RECT area;
	int row;
	BOOL chevron_shown;
} rebar_band;

typedef struct {
	HWND window, parent, tooltips;
	rebar_band *bands;
	int count, capacity;
	int *rows;
	int row_count;
	HIMAGELIST images;
	HFONT font;
	HPALETTE palette;
	COLORREF text_color, back_color;
	COLORSCHEME scheme;
	int bar_height;
	BOOL ansi_parent, narrow_format, resizing;
	int drag_band, drag_start, drag_width, press_chevron, hot_chevron;
	POINT press_point;
	BOOL drawing;
} rebar_state;

static rebar_state *state_of(HWND window)
{
	return (rebar_state *)GetWindowLongPtrW(window, 0);
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

static DWORD style_of(const rebar_state *state)
{
	return (DWORD)GetWindowLongPtrW(state->window, GWL_STYLE);
}

static BOOL is_vertical(const rebar_state *state)
{
	return (style_of(state) & CCS_VERT) != 0;
}

static BOOL is_hidden(const rebar_state *state, const rebar_band *band)
{
	if (band->style & RBBS_HIDDEN)
		return TRUE;
	return is_vertical(state) && (band->style & RBBS_NOVERT);
}

static LRESULT send_notify(const rebar_state *state, NMHDR *header, UINT code)
{
	header->hwndFrom = state->window;
	header->idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	header->code = code;
	return SendMessageW(state->parent, WM_NOTIFY, header->idFrom, (LPARAM)header);
}

static HFONT font_of(const rebar_state *state)
{
	return state->font ? state->font : (HFONT)GetStockObject(SYSTEM_FONT);
}

/* The sizes that follow from a band's own settings */

static int visible_count(const rebar_state *state)
{
	int index, seen = 0;
	for (index = 0; index < state->count; index++) {
		if (!is_hidden(state, &state->bands[index]))
			seen++;
	}
	return seen;
}

static BOOL has_gripper(const rebar_state *state, const rebar_band *band)
{
	if (band->style & (RBBS_FIXEDSIZE | RBBS_NOGRIPPER))
		return (band->style & RBBS_GRIPPERALWAYS) != 0 && !(band->style & RBBS_FIXEDSIZE);
	if (band->style & RBBS_GRIPPERALWAYS)
		return TRUE;
	return visible_count(state) > 1;
}

static int text_extent(const rebar_state *state, const WCHAR *text, int *height)
{
	HDC dc = GetDC(state->window);
	HGDIOBJ old = SelectObject(dc, font_of(state));
	SIZE size = { 0, 0 };
	TEXTMETRICW metrics;
	GetTextMetricsW(dc, &metrics);
	if (text && text[0])
		GetTextExtentPoint32W(dc, text, lstrlenW(text), &size);
	SelectObject(dc, old);
	ReleaseDC(state->window, dc);
	if (height)
		*height = metrics.tmHeight;
	return size.cx;
}

static void image_size(const rebar_state *state, const rebar_band *band, int *width, int *height)
{
	*width = *height = 0;
	if (state->images && band->image != -1)
		ImageList_GetIconSize(state->images, width, height);
}

static void measure_header(const rebar_state *state, rebar_band *band)
{
	int width = 0, picture_width, picture_height;
	BOOL grip = has_gripper(state, band);
	if (band->style & CUSTOM_HEADER)
		return;
	if (grip)
		width += GRIPPER_WIDTH;
	image_size(state, band, &picture_width, &picture_height);
	if (picture_width)
		width += picture_width + IMAGE_PADDING;
	if (band->text && band->text[0] && !(band->style & RBBS_HIDETITLE))
		width += text_extent(state, band->text, NULL) + TEXT_PADDING;
	else if (grip && !picture_width)
		width += GRIPPER_ALONE_EXTRA;
	band->header = width;
}

static int least_width(const rebar_band *band)
{
	return band->header + band->min_along + POST_CHILD;
}

static int wanted_width(const rebar_band *band)
{
	int least = least_width(band);
	return band->width > least ? band->width : least;
}

static int band_across(const rebar_state *state, const rebar_band *band)
{
	int across = band->min_across, text_height = 0, picture_width, picture_height;
	if (band->style & RBBS_VARIABLEHEIGHT) {
		if (band->current_across > across)
			across = band->current_across;
		if (band->max_across && band->max_across != 0x7fffffff && across > band->max_across)
			across = band->max_across;
	}
	if (band->style & RBBS_CHILDEDGE)
		across += CHILD_EDGE;
	if (band->text && band->text[0] && !(band->style & RBBS_HIDETITLE)) {
		text_extent(state, band->text, &text_height);
		if (text_height + POST_CHILD > across)
			across = text_height + POST_CHILD;
	}
	image_size(state, band, &picture_width, &picture_height);
	if (picture_height + POST_CHILD > across && picture_height)
		across = picture_height + POST_CHILD;
	return across;
}

/* Layout */

static int available_along(const rebar_state *state)
{
	RECT client;
	GetClientRect(state->window, &client);
	return is_vertical(state) ? client.bottom : client.right;
}

static void fill_row(rebar_state *state, int first, int after, int room)
{
	int total = 0, index, last = -1;
	for (index = first; index < after; index++) {
		if (is_hidden(state, &state->bands[index]))
			continue;
		total += wanted_width(&state->bands[index]);
		last = index;
	}
	if (last < 0)
		return;
	if (total < room) {
		state->bands[last].area.right = room - total;
	}
	else {
		state->bands[last].area.right = 0;
		for (index = after - 1; index >= first && total > room; index--) {
			rebar_band *band = &state->bands[index];
			int spare;
			if (is_hidden(state, band))
				continue;
			spare = wanted_width(band) - least_width(band);
			if (spare > total - room)
				spare = total - room;
			band->area.right = -spare;
			total -= spare;
		}
	}
}

/* Places the bands; the extra stored in area.right by fill_row becomes part of each width here. */
static void assign_rows(rebar_state *state, int room)
{
	int index, first = 0, used = 0, row = 0;
	BOOL any = FALSE;
	release(state->rows);
	state->rows = allocate((state->count + 1) * sizeof(int));
	state->row_count = 0;
	for (index = 0; index < state->count; index++) {
		rebar_band *band = &state->bands[index];
		int width;
		SetRectEmpty(&band->area);
		band->row = -1;
		if (is_hidden(state, band))
			continue;
		width = wanted_width(band);
		if (any && ((band->style & RBBS_BREAK) || used + width > room)) {
			fill_row(state, first, index, room);
			row++;
			first = index;
			used = 0;
		}
		band->row = row;
		used += width;
		any = TRUE;
	}
	if (any) {
		fill_row(state, first, state->count, room);
		state->row_count = row + 1;
	}
}

static void place_child(rebar_state *state, rebar_band *band)
{
	RECT area = band->area;
	int across, height;
	BOOL vertical = is_vertical(state);
	NMREBARCHILDSIZE ask;
	if (!band->child)
		return;
	if (band->row < 0) {
		ShowWindow(band->child, SW_HIDE);
		return;
	}
	if (vertical) {
		area.top += band->header;
		area.bottom -= POST_CHILD;
		if (band->chevron_shown)
			area.bottom -= CHEVRON_WIDTH;
	}
	else {
		area.left += band->header;
		area.right -= POST_CHILD;
		if (band->chevron_shown)
			area.right -= CHEVRON_WIDTH;
	}
	ZeroMemory(&ask, sizeof(ask));
	ask.uBand = (UINT)(band - state->bands);
	ask.wID = band->id;
	ask.rcBand = band->area;
	ask.rcChild = area;
	send_notify(state, &ask.hdr, RBN_CHILDSIZE);
	area = ask.rcChild;
	across = vertical ? area.right - area.left : area.bottom - area.top;
	height = band->min_across;
	if (band->style & RBBS_VARIABLEHEIGHT)
		height = across;
	if (height < across && !(band->style & RBBS_TOPALIGN)) {
		if (vertical)
			area.left += (across - height) / 2;
		else
			area.top += (across - height) / 2;
	}
	if (height < across) {
		if (vertical)
			area.right = area.left + height;
		else
			area.bottom = area.top + height;
	}
	SetWindowPos(band->child, 0, area.left, area.top, area.right - area.left, area.bottom - area.top, SWP_NOZORDER | SWP_NOACTIVATE);
	ShowWindow(band->child, SW_SHOWNA);
}

static void position_bands(rebar_state *state)
{
	BOOL vertical = is_vertical(state);
	BOOL uniform = !(style_of(state) & RBS_VARHEIGHT);
	BOOL borders = (style_of(state) & RBS_BANDBORDERS) != 0;
	int tallest = 0, row, offset = 0, index;

	for (index = 0; index < state->count; index++) {
		const rebar_band *band = &state->bands[index];
		int across;
		if (band->row < 0)
			continue;
		across = band_across(state, band);
		if (across > tallest)
			tallest = across;
		if (across > state->rows[band->row])
			state->rows[band->row] = across;
	}
	for (row = 0; row < state->row_count; row++) {
		int height = uniform ? tallest : state->rows[row], along = 0;
		state->rows[row] = height;
		for (index = 0; index < state->count; index++) {
			rebar_band *band = &state->bands[index];
			int extra, width, ideal_room;
			if (band->row != row)
				continue;
			extra = band->area.right;
			width = wanted_width(band) + extra;
			if (vertical)
				SetRect(&band->area, offset, along, offset + height, along + width);
			else
				SetRect(&band->area, along, offset, along + width, offset + height);
			along += width;
			ideal_room = width - band->header - POST_CHILD;
			band->chevron_shown = (band->style & RBBS_USECHEVRON) && band->ideal > ideal_room;
		}
		offset += height;
		if (borders && row + 1 < state->row_count)
			offset += ROW_BORDER;
	}
	state->bar_height = offset;
}

static void fit_window(rebar_state *state)
{
	DWORD style = style_of(state);
	RECT frame, parent;
	BOOL vertical = (style & CCS_VERT) != 0;
	int extra_across, x, y, cx, cy;
	UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
	if (style & CCS_NORESIZE)
		return;
	GetWindowRect(state->window, &frame);
	MapWindowPoints(HWND_DESKTOP, state->parent, (POINT *)&frame, 2);
	/* Computed from the style: a window still too small for its own frame reports no client area to compare with. */
	extra_across = 0;
	if (style & WS_BORDER)
		extra_across += 2 * (vertical ? GetSystemMetrics(SM_CXBORDER) : GetSystemMetrics(SM_CYBORDER));
	if (!vertical && !(style & CCS_NODIVIDER))
		extra_across += DIVIDER_HEIGHT;
	GetClientRect(state->parent, &parent);
	x = frame.left;
	y = frame.top;
	cx = frame.right - frame.left;
	cy = frame.bottom - frame.top;
	if (vertical) {
		cx = state->bar_height + extra_across;
		if (!(style & CCS_NOPARENTALIGN)) {
			y = 0;
			cy = parent.bottom;
			x = (style & CCS_BOTTOM) == CCS_BOTTOM ? parent.right - cx : 0;
		}
	}
	else {
		cy = state->bar_height + extra_across;
		if (!(style & CCS_NOPARENTALIGN)) {
			x = 0;
			cx = parent.right;
			if ((style & CCS_NOMOVEY) != CCS_NOMOVEY)
				y = (style & CCS_BOTTOM) == CCS_BOTTOM ? parent.bottom - cy : 0;
		}
	}
	if (x == frame.left && y == frame.top && cx == frame.right - frame.left && cy == frame.bottom - frame.top)
		return;
	state->resizing = TRUE;
	SetWindowPos(state->window, 0, x, y, cx, cy, flags);
	state->resizing = FALSE;
}

static void relayout(rebar_state *state)
{
	int old_height = state->bar_height, index;
	NMHDR header;

	for (index = 0; index < state->count; index++)
		measure_header(state, &state->bands[index]);
	assign_rows(state, available_along(state));
	position_bands(state);
	fit_window(state);
	/* Filling the parent can change the width that was just laid out against. */
	if (available_along(state) != 0) {
		assign_rows(state, available_along(state));
		position_bands(state);
	}
	for (index = 0; index < state->count; index++)
		place_child(state, &state->bands[index]);
	InvalidateRect(state->window, NULL, TRUE);
	if (state->bar_height != old_height)
		send_notify(state, &header, RBN_HEIGHTCHANGE);
}

/* Bands */

static WCHAR *narrow_to_wide(const char *text)
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

static WCHAR *wide_copy(const WCHAR *text)
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

static void attach_child(rebar_state *state, rebar_band *band, HWND child)
{
	if (band->child == child)
		return;
	if (band->child && IsWindow(band->child))
		ShowWindow(band->child, SW_HIDE);
	band->child = child;
	if (child) {
		SetParent(child, state->window);
	}
}

/* REBARBANDINFOA and W have the same layout: only the text differs. */
static BOOL apply_band(rebar_state *state, rebar_band *band, const REBARBANDINFOW *info, BOOL narrow)
{
	UINT mask = info->fMask;
	if (mask & RBBIM_STYLE) {
		band->style = info->fStyle & ~CUSTOM_HEADER;
		if ((band->style & RBBS_VARIABLEHEIGHT) && !band->max_across) {
			band->current_across = band->min_across;
			band->max_across = 0x7fffffff;
		}
	}
	if (mask & RBBIM_COLORS) {
		band->fore = info->clrFore;
		band->back = info->clrBack;
	}
	if (mask & RBBIM_TEXT) {
		release(band->text);
		band->text = narrow ? narrow_to_wide((const char *)info->lpText) : wide_copy(info->lpText);
	}
	if (mask & RBBIM_IMAGE)
		band->image = info->iImage;
	if (mask & RBBIM_CHILD)
		attach_child(state, band, info->hwndChild);
	if (mask & RBBIM_CHILDSIZE) {
		band->min_along = info->cxMinChild;
		band->min_across = info->cyMinChild;
		if (band->style & RBBS_VARIABLEHEIGHT) {
			band->current_across = info->cyChild;
			band->max_across = info->cyMaxChild;
			band->step_across = info->cyIntegral;
		}
	}
	if (mask & RBBIM_SIZE)
		band->width = info->cx;
	if (mask & RBBIM_BACKGROUND)
		band->bitmap = info->hbmBack;
	if (mask & RBBIM_ID)
		band->id = info->wID;
	if ((mask & RBBIM_IDEALSIZE) && info->cbSize >= REBARBANDINFOA_V6_SIZE)
		band->ideal = info->cxIdeal;
	if ((mask & RBBIM_LPARAM) && info->cbSize >= REBARBANDINFOA_V6_SIZE)
		band->data = info->lParam;
	if ((mask & RBBIM_HEADERSIZE) && info->cbSize >= REBARBANDINFOA_V6_SIZE) {
		band->header = info->cxHeader;
		band->style |= CUSTOM_HEADER;
	}
	return TRUE;
}

static BOOL size_is_valid(const REBARBANDINFOW *info)
{
	return info && info->cbSize >= PUBLIC_V3_SIZE && info->cbSize <= sizeof(REBARBANDINFOW);
}

static LRESULT insert_band(rebar_state *state, int index, const REBARBANDINFOW *info, BOOL narrow)
{
	rebar_band *band;
	if (!size_is_valid(info))
		return FALSE;
	if (index < 0 || index > state->count)
		index = state->count;
	if (state->count == state->capacity) {
		int capacity = state->capacity ? state->capacity * 2 : 8;
		rebar_band *grown = allocate(capacity * sizeof(*grown));
		if (!grown)
			return FALSE;
		if (state->bands)
			CopyMemory(grown, state->bands, state->count * sizeof(*grown));
		release(state->bands);
		state->bands = grown;
		state->capacity = capacity;
	}
	MoveMemory(state->bands + index + 1, state->bands + index, (state->count - index) * sizeof(*band));
	band = &state->bands[index];
	ZeroMemory(band, sizeof(*band));
	band->image = -1;
	band->fore = RGB(0, 0, 0);
	band->back = GetSysColor(COLOR_3DFACE);
	state->count++;
	apply_band(state, band, info, narrow);
	relayout(state);
	return TRUE;
}

static LRESULT set_band(rebar_state *state, int index, const REBARBANDINFOW *info, BOOL narrow)
{
	if (!size_is_valid(info) || index < 0 || index >= state->count)
		return FALSE;
	apply_band(state, &state->bands[index], info, narrow);
	relayout(state);
	return TRUE;
}

static LRESULT get_band(const rebar_state *state, int index, REBARBANDINFOW *info, BOOL narrow)
{
	const rebar_band *band;
	UINT mask;
	if (!size_is_valid(info) || index < 0 || index >= state->count)
		return FALSE;
	band = &state->bands[index];
	mask = info->fMask;
	if (mask & RBBIM_STYLE)
		info->fStyle = band->style;
	if (mask & RBBIM_COLORS) {
		info->clrFore = band->fore;
		info->clrBack = band->back;
	}
	if ((mask & RBBIM_TEXT) && info->lpText && info->cch > 0) {
		const WCHAR *text = band->text ? band->text : L"";
		if (narrow) {
			if (!WideCharToMultiByte(CP_ACP, 0, text, -1, (char *)info->lpText, info->cch, NULL, NULL))
				((char *)info->lpText)[info->cch - 1] = 0;
		}
		else {
			lstrcpynW(info->lpText, text, info->cch);
		}
	}
	if (mask & RBBIM_IMAGE)
		info->iImage = band->image;
	if (mask & RBBIM_CHILD)
		info->hwndChild = band->child;
	if (mask & RBBIM_CHILDSIZE) {
		info->cxMinChild = band->min_along;
		info->cyMinChild = band->min_across;
		if (band->style & RBBS_VARIABLEHEIGHT) {
			info->cyChild = band->current_across;
			info->cyMaxChild = band->max_across;
			info->cyIntegral = band->step_across;
		}
	}
	if (mask & RBBIM_SIZE)
		info->cx = band->width;
	if (mask & RBBIM_BACKGROUND)
		info->hbmBack = band->bitmap;
	if (mask & RBBIM_ID)
		info->wID = band->id;
	if ((mask & RBBIM_IDEALSIZE) && info->cbSize >= REBARBANDINFOA_V6_SIZE)
		info->cxIdeal = band->ideal;
	if ((mask & RBBIM_LPARAM) && info->cbSize >= REBARBANDINFOA_V6_SIZE)
		info->lParam = band->data;
	if ((mask & RBBIM_HEADERSIZE) && info->cbSize >= REBARBANDINFOA_V6_SIZE)
		info->cxHeader = band->header;
	return TRUE;
}

static LRESULT delete_band(rebar_state *state, int index)
{
	NMREBAR info;
	rebar_band *band;
	if (index < 0 || index >= state->count)
		return FALSE;
	band = &state->bands[index];
	ZeroMemory(&info, sizeof(info));
	info.dwMask = RBNM_ID | RBNM_STYLE | RBNM_LPARAM;
	info.uBand = index;
	info.fStyle = band->style;
	info.wID = band->id;
	info.lParam = band->data;
	send_notify(state, &info.hdr, RBN_DELETINGBAND);
	if (band->child && IsWindow(band->child))
		ShowWindow(band->child, SW_HIDE);
	release(band->text);
	MoveMemory(band, band + 1, (state->count - index - 1) * sizeof(*band));
	state->count--;
	send_notify(state, &info.hdr, RBN_DELETEDBAND);
	relayout(state);
	return TRUE;
}

/* The space a band may grow to on its row, taken from the bands to its right and then its left. */
static void set_width(rebar_state *state, int index, int width)
{
	rebar_band *band = &state->bands[index];
	int walk, delta;
	int current = is_vertical(state) ? band->area.bottom - band->area.top : band->area.right - band->area.left;
	if (width < least_width(band))
		width = least_width(band);
	delta = width - current;
	for (walk = 0; walk < state->count; walk++) {
		rebar_band *other = &state->bands[walk];
		if (other->row == band->row && band->row >= 0) {
			int now = is_vertical(state) ? other->area.bottom - other->area.top : other->area.right - other->area.left;
			other->width = now;
		}
	}
	band->width = width;
	if (delta > 0) {
		for (walk = index + 1; walk < state->count && delta > 0; walk++) {
			rebar_band *other = &state->bands[walk];
			int spare;
			if (other->row != band->row)
				continue;
			spare = other->width - least_width(other);
			if (spare > delta)
				spare = delta;
			other->width -= spare;
			delta -= spare;
		}
		for (walk = index - 1; walk >= 0 && delta > 0; walk--) {
			rebar_band *other = &state->bands[walk];
			int spare;
			if (other->row != band->row)
				continue;
			spare = other->width - least_width(other);
			if (spare > delta)
				spare = delta;
			other->width -= spare;
			delta -= spare;
		}
		band->width -= delta;
	}
	else if (delta < 0) {
		for (walk = index + 1; walk < state->count; walk++) {
			if (state->bands[walk].row == band->row) {
				state->bands[walk].width -= delta;
				break;
			}
		}
	}
	relayout(state);
}

static void maximize_band(rebar_state *state, int index, BOOL ideal)
{
	rebar_band *band = &state->bands[index];
	int room = available_along(state), walk;
	if (ideal) {
		set_width(state, index, band->header + band->ideal + POST_CHILD);
		return;
	}
	for (walk = 0; walk < state->count; walk++) {
		if (walk != index && state->bands[walk].row == band->row)
			room -= least_width(&state->bands[walk]);
	}
	set_width(state, index, room);
}

/* Hit testing */

static void gripper_rect(const rebar_state *state, const rebar_band *band, RECT *area)
{
	*area = band->area;
	if (is_vertical(state))
		area->bottom = area->top + band->header;
	else
		area->right = area->left + band->header;
}

static void chevron_rect(const rebar_state *state, const rebar_band *band, RECT *area)
{
	*area = band->area;
	if (is_vertical(state)) {
		area->bottom -= POST_CHILD - 2;
		area->top = area->bottom - CHEVRON_WIDTH;
	}
	else {
		area->right -= POST_CHILD - 2;
		area->left = area->right - CHEVRON_WIDTH;
	}
}

static int hit_test(const rebar_state *state, POINT at, UINT *flags)
{
	int index;
	*flags = RBHT_NOWHERE;
	for (index = 0; index < state->count; index++) {
		const rebar_band *band = &state->bands[index];
		RECT area;
		if (band->row < 0 || !PtInRect(&band->area, at))
			continue;
		if (band->chevron_shown) {
			chevron_rect(state, band, &area);
			if (PtInRect(&area, at)) {
				*flags = RBHT_CHEVRON;
				return index;
			}
		}
		gripper_rect(state, band, &area);
		if (PtInRect(&area, at)) {
			int grip = has_gripper(state, band) ? GRIPPER_WIDTH : 0;
			RECT only = area;
			if (is_vertical(state))
				only.bottom = only.top + grip;
			else
				only.right = only.left + grip;
			*flags = PtInRect(&only, at) ? RBHT_GRABBER : RBHT_CAPTION;
			return index;
		}
		*flags = RBHT_CLIENT;
		return index;
	}
	return -1;
}

static void push_chevron(rebar_state *state, int index, LPARAM extra)
{
	rebar_band *band;
	NMREBARCHEVRON info;
	if (index < 0 || index >= state->count)
		return;
	band = &state->bands[index];
	if (!band->chevron_shown)
		return;
	ZeroMemory(&info, sizeof(info));
	info.uBand = index;
	info.wID = band->id;
	info.lParam = band->data;
	chevron_rect(state, band, &info.rc);
	info.lParamNM = extra;
	send_notify(state, &info.hdr, RBN_CHEVRONPUSHED);
}

/* Painting */

static void draw_gripper(const rebar_state *state, HDC dc, const rebar_band *band)
{
	RECT bar = band->area;
	if (is_vertical(state)) {
		InflateRect(&bar, -2, 0);
		bar.top += 2;
		bar.bottom = bar.top + 3;
	}
	else {
		InflateRect(&bar, 0, -2);
		bar.left += 2;
		bar.right = bar.left + 3;
	}
	DrawEdge(dc, &bar, BDR_RAISEDINNER, BF_RECT);
}

static void draw_chevron(const rebar_state *state, HDC dc, const rebar_band *band, BOOL pressed)
{
	RECT area;
	HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNTEXT));
	HGDIOBJ old;
	int x, y, repeat;
	chevron_rect(state, band, &area);
	if (pressed)
		DrawEdge(dc, &area, BDR_SUNKENOUTER, BF_RECT);
	old = SelectObject(dc, pen);
	x = (area.left + area.right) / 2 - 2;
	y = (area.top + area.bottom) / 2 - 3;
	for (repeat = 0; repeat < 2; repeat++, x += 4) {
		MoveToEx(dc, x, y, NULL);
		LineTo(dc, x + 2, y + 3);
		LineTo(dc, x - 1, y + 6);
	}
	SelectObject(dc, old);
	DeleteObject(pen);
}

static void draw_band(rebar_state *state, HDC dc, int index)
{
	rebar_band *band = &state->bands[index];
	RECT text_area = band->area;
	HBRUSH brush;
	int x = is_vertical(state) ? band->area.top : band->area.left;
	int picture_width, picture_height;

	if (band->bitmap) {
		brush = CreatePatternBrush(band->bitmap);
		SetBrushOrgEx(dc, band->area.left, band->area.top, NULL);
	}
	else {
		/* A band without its own color shows the background the erase message drew. */
		brush = band->back == CLR_DEFAULT || band->back == CLR_NONE ? NULL : CreateSolidBrush(band->back);
	}
	if (brush) {
		FillRect(dc, &band->area, brush);
		DeleteObject(brush);
	}
	if (has_gripper(state, band)) {
		draw_gripper(state, dc, band);
		x += GRIPPER_WIDTH;
	}
	image_size(state, band, &picture_width, &picture_height);
	if (picture_width && band->image >= 0) {
		if (is_vertical(state))
			ImageList_Draw(state->images, band->image, dc, (band->area.left + band->area.right - picture_width) / 2, x, ILD_TRANSPARENT);
		else
			ImageList_Draw(state->images, band->image, dc, x, (band->area.top + band->area.bottom - picture_height) / 2, ILD_TRANSPARENT);
		x += picture_width + IMAGE_PADDING;
	}
	if (band->text && band->text[0] && !(band->style & RBBS_HIDETITLE)) {
		COLORREF color = state->text_color != CLR_NONE ? state->text_color : band->fore;
		COLORREF old_color = SetTextColor(dc, color == CLR_DEFAULT ? GetSysColor(COLOR_BTNTEXT) : color);
		int old_mode = SetBkMode(dc, TRANSPARENT);
		HGDIOBJ old_font = SelectObject(dc, font_of(state));
		if (is_vertical(state))
			text_area.top = x;
		else
			text_area.left = x + 2;
		DrawTextW(dc, band->text, -1, &text_area, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
		SelectObject(dc, old_font);
		SetBkMode(dc, old_mode);
		SetTextColor(dc, old_color);
	}
	if (band->chevron_shown)
		draw_chevron(state, dc, band, state->press_chevron == index);
	if ((style_of(state) & RBS_BANDBORDERS)) {
		int row_last = TRUE, walk;
		for (walk = index + 1; walk < state->count; walk++) {
			if (state->bands[walk].row == band->row) {
				row_last = FALSE;
				break;
			}
		}
		if (!row_last) {
			RECT line = band->area;
			if (is_vertical(state))
				line.top = line.bottom - ROW_BORDER;
			else
				line.left = line.right - ROW_BORDER;
			DrawEdge(dc, &line, EDGE_ETCHED, is_vertical(state) ? BF_BOTTOM : BF_RIGHT);
		}
		if (band->row + 1 < state->row_count) {
			RECT line = band->area;
			if (is_vertical(state)) {
				line.left = line.right;
				line.right = line.left + ROW_BORDER;
				DrawEdge(dc, &line, EDGE_ETCHED, BF_RIGHT);
			}
			else {
				line.top = line.bottom;
				line.bottom = line.top + ROW_BORDER;
				DrawEdge(dc, &line, EDGE_ETCHED, BF_BOTTOM);
			}
		}
	}
}

static void draw_all(rebar_state *state, HDC dc)
{
	RECT client;
	int index;
	GetClientRect(state->window, &client);
	/* The background goes through the message, so that a program that subclasses the bar to color it is heard. */
	state->drawing = TRUE;
	SendMessageW(state->window, WM_ERASEBKGND, (WPARAM)dc, 0);
	state->drawing = FALSE;
	for (index = 0; index < state->count; index++) {
		if (state->bands[index].row >= 0)
			draw_band(state, dc, index);
	}
}

static void paint(rebar_state *state, HDC given)
{
	PAINTSTRUCT info;
	RECT client;
	HDC dc = given ? given : BeginPaint(state->window, &info);
	if (dc) {
		GetClientRect(state->window, &client);
		if (client.right > 0 && client.bottom > 0 && !given) {
			HDC memory = CreateCompatibleDC(dc);
			HBITMAP bitmap = memory ? CreateCompatibleBitmap(dc, client.right, client.bottom) : NULL;
			if (bitmap) {
				HGDIOBJ old = SelectObject(memory, bitmap);
				draw_all(state, memory);
				BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
				SelectObject(memory, old);
				DeleteObject(bitmap);
			}
			else {
				draw_all(state, dc);
			}
			if (memory)
				DeleteDC(memory);
		}
		else {
			draw_all(state, dc);
		}
	}
	if (!given)
		EndPaint(state->window, &info);
}

/* Dragging */

static void begin_drag(rebar_state *state, int index, POINT at)
{
	NMREBAR info;
	ZeroMemory(&info, sizeof(info));
	info.dwMask = RBNM_ID | RBNM_STYLE | RBNM_LPARAM;
	info.uBand = index;
	info.fStyle = state->bands[index].style;
	info.wID = state->bands[index].id;
	info.lParam = state->bands[index].data;
	if (send_notify(state, &info.hdr, RBN_BEGINDRAG))
		return;
	state->drag_band = index;
	state->drag_start = is_vertical(state) ? at.y : at.x;
	state->drag_width = is_vertical(state) ? state->bands[index].area.bottom - state->bands[index].area.top : state->bands[index].area.right - state->bands[index].area.left;
	state->press_point = at;
	SetCapture(state->window);
}

/* Moving a gripper along the row trades width with the band before it. */
static void drag_to(rebar_state *state, POINT at)
{
	int index = state->drag_band, walk, previous = -1;
	int offset = (is_vertical(state) ? at.y : at.x) - state->drag_start;
	for (walk = index - 1; walk >= 0; walk--) {
		if (state->bands[walk].row == state->bands[index].row) {
			previous = walk;
			break;
		}
	}
	if (previous < 0) {
		return;
	}
	set_width(state, previous, (is_vertical(state) ? state->bands[previous].area.bottom - state->bands[previous].area.top : state->bands[previous].area.right - state->bands[previous].area.left) + offset);
	state->drag_start = is_vertical(state) ? at.y : at.x;
}

static void end_drag(rebar_state *state)
{
	int index = state->drag_band;
	NMREBAR info;
	NMHDR changed;
	if (index < 0)
		return;
	state->drag_band = -1;
	if (GetCapture() == state->window)
		ReleaseCapture();
	ZeroMemory(&info, sizeof(info));
	info.dwMask = RBNM_ID | RBNM_STYLE | RBNM_LPARAM;
	info.uBand = index;
	info.fStyle = state->bands[index].style;
	info.wID = state->bands[index].id;
	info.lParam = state->bands[index].data;
	send_notify(state, &info.hdr, RBN_ENDDRAG);
	send_notify(state, &changed, RBN_LAYOUTCHANGED);
}

static LRESULT size_to_rect(rebar_state *state, const RECT *wanted)
{
	if (!wanted)
		return FALSE;
	SetWindowPos(state->window, 0, 0, 0, wanted->right - wanted->left, wanted->bottom - wanted->top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	return TRUE;
}

static LRESULT move_band(rebar_state *state, int from, int to)
{
	rebar_band moved;
	if (from < 0 || from >= state->count || to < 0 || to >= state->count)
		return FALSE;
	moved = state->bands[from];
	if (from < to)
		MoveMemory(&state->bands[from], &state->bands[from + 1], (to - from) * sizeof(moved));
	else if (from > to)
		MoveMemory(&state->bands[to + 1], &state->bands[to], (from - to) * sizeof(moved));
	state->bands[to] = moved;
	relayout(state);
	return TRUE;
}

static LRESULT CALLBACK rebar_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	rebar_state *state = state_of(window);

	if (message == WM_NCCREATE) {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state = allocate(sizeof(*state));
		if (!state)
			return FALSE;
		state->window = window;
		state->parent = create->hwndParent;
		state->text_color = state->back_color = CLR_NONE;
		state->scheme.dwSize = sizeof(state->scheme);
		state->scheme.clrBtnHighlight = state->scheme.clrBtnShadow = CLR_DEFAULT;
		state->drag_band = state->press_chevron = state->hot_chevron = -1;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	if (!state)
		return DefWindowProcW(window, message, wparam, lparam);

	switch (message) {
	/* This library is always version 6, whatever is asked for. */
	case CCM_SETVERSION:
	case CCM_GETVERSION:
		return 6;
	case WM_CREATE:
		state->ansi_parent = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_ANSI;
		relayout(state);
		return 0;
	case WM_NCDESTROY: {
		int index;
		SetWindowLongPtrW(window, 0, 0);
		for (index = 0; index < state->count; index++)
			release(state->bands[index].text);
		release(state->bands);
		release(state->rows);
		release(state);
		break;
	}
	case WM_NCCALCSIZE:
		if (wparam) {
			NCCALCSIZE_PARAMS *sizes = (NCCALCSIZE_PARAMS *)lparam;
			DWORD style = style_of(state);
			RECT *view = &sizes->rgrc[0];
			if (style & WS_BORDER)
				InflateRect(view, -GetSystemMetrics(SM_CXBORDER), -GetSystemMetrics(SM_CYBORDER));
			if (!(style & CCS_NODIVIDER))
				view->top += DIVIDER_HEIGHT;
			return 0;
		}
		break;
	case WM_NCPAINT:
		DefWindowProcW(window, message, wparam, lparam);
		if (!(style_of(state) & CCS_NODIVIDER)) {
			HDC dc = GetWindowDC(window);
			if (dc) {
				RECT frame, line;
				GetWindowRect(window, &frame);
				SetRect(&line, 0, 0, frame.right - frame.left, DIVIDER_HEIGHT);
				DrawEdge(dc, &line, EDGE_ETCHED, BF_TOP);
				ReleaseDC(window, dc);
			}
		}
		return 0;
	case WM_SIZE:
		if (!state->resizing)
			relayout(state);
		return 0;
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		relayout(state);
		return 0;
	case WM_GETFONT:
		return (LRESULT)font_of(state);
	case WM_ERASEBKGND:
		/* The painting erases into its own buffer: on the screen the erase would only flash. */
		if (state->drawing) {
			RECT client;
			GetClientRect(window, &client);
			FillRect((HDC)wparam, &client, GetSysColorBrush(COLOR_3DFACE));
		}
		return 1;
	case WM_PAINT:
		paint(state, (HDC)wparam);
		return 0;
	case WM_PRINTCLIENT:
		paint(state, (HDC)wparam);
		return 0;
	case WM_NOTIFY:
	case WM_COMMAND:
	case WM_MEASUREITEM:
	case WM_DRAWITEM:
		return SendMessageW(state->parent, message, wparam, lparam);
	case WM_NOTIFYFORMAT:
		if (lparam == NF_REQUERY)
			state->ansi_parent = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_ANSI;
		return state->ansi_parent ? NFR_ANSI : NFR_UNICODE;
	case WM_LBUTTONDOWN: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		UINT flags;
		int index = hit_test(state, at, &flags);
		if (flags == RBHT_CHEVRON) {
			state->press_chevron = index;
			InvalidateRect(window, NULL, FALSE);
			UpdateWindow(window);
			push_chevron(state, index, 0);
			state->press_chevron = -1;
			InvalidateRect(window, NULL, FALSE);
			return 0;
		}
		if (flags == RBHT_GRABBER || flags == RBHT_CAPTION) {
			if (!(state->bands[index].style & RBBS_FIXEDSIZE))
				begin_drag(state, index, at);
		}
		return 0;
	}
	case WM_LBUTTONDBLCLK: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		UINT flags;
		int index = hit_test(state, at, &flags);
		if (index >= 0 && (flags == RBHT_GRABBER || flags == RBHT_CAPTION) && (style_of(state) & RBS_DBLCLKTOGGLE)) {
			const rebar_band *band = &state->bands[index];
			if ((is_vertical(state) ? band->area.bottom - band->area.top : band->area.right - band->area.left) > least_width(band))
				set_width(state, index, least_width(band));
			else
				maximize_band(state, index, FALSE);
		}
		return 0;
	}
	case WM_MOUSEMOVE: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		if (state->drag_band >= 0)
			drag_to(state, at);
		return 0;
	}
	case WM_LBUTTONUP:
		end_drag(state);
		return 0;
	case WM_CAPTURECHANGED:
		if ((HWND)lparam != window && state->drag_band >= 0)
			state->drag_band = -1;
		return 0;
	case WM_SETCURSOR:
		if (LOWORD(lparam) == HTCLIENT) {
			POINT at;
			UINT flags;
			GetCursorPos(&at);
			ScreenToClient(window, &at);
			hit_test(state, at, &flags);
			if (flags == RBHT_GRABBER) {
				SetCursor(LoadCursorW(NULL, (LPCWSTR)(is_vertical(state) ? IDC_SIZENS : IDC_SIZEWE)));
				return TRUE;
			}
		}
		break;

	case RB_INSERTBANDA:
		return insert_band(state, (int)wparam, (const REBARBANDINFOW *)lparam, TRUE);
	case RB_INSERTBANDW:
		return insert_band(state, (int)wparam, (const REBARBANDINFOW *)lparam, FALSE);
	case RB_SETBANDINFOA:
		return set_band(state, (int)wparam, (const REBARBANDINFOW *)lparam, TRUE);
	case RB_SETBANDINFOW:
		return set_band(state, (int)wparam, (const REBARBANDINFOW *)lparam, FALSE);
	case RB_GETBANDINFOA:
		return get_band(state, (int)wparam, (REBARBANDINFOW *)lparam, TRUE);
	case RB_GETBANDINFOW:
		return get_band(state, (int)wparam, (REBARBANDINFOW *)lparam, FALSE);
	case RB_DELETEBAND:
		return delete_band(state, (int)wparam);
	case RB_GETBANDCOUNT:
		return state->count;
	case RB_GETROWCOUNT:
		return state->row_count;
	case RB_GETROWHEIGHT: {
		int index = (int)wparam;
		return index >= 0 && index < state->row_count ? state->rows[index] : 0;
	}
	case RB_GETBARHEIGHT:
		return state->bar_height;
	case RB_GETRECT:
		if ((int)wparam < 0 || (int)wparam >= state->count || !lparam)
			return FALSE;
		*(RECT *)lparam = state->bands[wparam].area;
		return TRUE;
	case RB_HITTEST: {
		RBHITTESTINFO *info = (RBHITTESTINFO *)lparam;
		if (!info)
			return -1;
		info->iBand = hit_test(state, info->pt, &info->flags);
		return info->iBand;
	}
	case RB_IDTOINDEX: {
		int index;
		for (index = 0; index < state->count; index++) {
			if (state->bands[index].id == (UINT)wparam)
				return index;
		}
		return -1;
	}
	case RB_MAXIMIZEBAND:
		if ((int)wparam >= 0 && (int)wparam < state->count)
			maximize_band(state, (int)wparam, (BOOL)lparam);
		return 0;
	case RB_MINIMIZEBAND:
		if ((int)wparam >= 0 && (int)wparam < state->count)
			set_width(state, (int)wparam, least_width(&state->bands[wparam]));
		return 0;
	case RB_MOVEBAND:
		return move_band(state, (int)wparam, (int)lparam);
	case RB_SHOWBAND:
		if ((int)wparam < 0 || (int)wparam >= state->count)
			return FALSE;
		if (lparam)
			state->bands[wparam].style &= ~RBBS_HIDDEN;
		else
			state->bands[wparam].style |= RBBS_HIDDEN;
		relayout(state);
		return TRUE;
	case RB_SIZETORECT:
		return size_to_rect(state, (const RECT *)lparam);
	case RB_PUSHCHEVRON:
		push_chevron(state, (int)wparam, lparam);
		return 0;
	case RB_BEGINDRAG:
		if ((int)wparam >= 0 && (int)wparam < state->count) {
			POINT at;
			GetCursorPos(&at);
			ScreenToClient(window, &at);
			begin_drag(state, (int)wparam, at);
		}
		return 0;
	case RB_DRAGMOVE:
		if (state->drag_band >= 0) {
			POINT at;
			GetCursorPos(&at);
			ScreenToClient(window, &at);
			drag_to(state, at);
		}
		return 0;
	case RB_ENDDRAG:
		end_drag(state);
		return 0;
	case RB_GETBANDBORDERS:
		if ((int)wparam < 0 || (int)wparam >= state->count || !lparam)
			return 0;
		SetRect((RECT *)lparam, state->bands[wparam].header, 0, POST_CHILD, 0);
		return 0;
	case RB_GETBANDMARGINS:
		if (lparam)
			ZeroMemory((void *)lparam, 4 * sizeof(int));
		return 0;
	case RB_GETBARINFO: {
		REBARINFO *info = (REBARINFO *)lparam;
		if (!info || info->cbSize < sizeof(*info))
			return FALSE;
		if (info->fMask & RBIM_IMAGELIST)
			info->himl = state->images;
		return TRUE;
	}
	case RB_SETBARINFO: {
		const REBARINFO *info = (const REBARINFO *)lparam;
		if (!info || info->cbSize < sizeof(*info))
			return FALSE;
		if (info->fMask & RBIM_IMAGELIST)
			state->images = info->himl;
		relayout(state);
		return TRUE;
	}
	case RB_GETBKCOLOR:
		return state->back_color;
	case RB_SETBKCOLOR: {
		COLORREF old = state->back_color;
		state->back_color = (COLORREF)lparam;
		InvalidateRect(window, NULL, TRUE);
		return old;
	}
	case RB_GETTEXTCOLOR:
		return state->text_color;
	case RB_SETTEXTCOLOR: {
		COLORREF old = state->text_color;
		state->text_color = (COLORREF)lparam;
		InvalidateRect(window, NULL, TRUE);
		return old;
	}
	case RB_GETCOLORSCHEME: {
		COLORSCHEME *scheme = (COLORSCHEME *)lparam;
		if (!scheme)
			return FALSE;
		scheme->clrBtnHighlight = state->scheme.clrBtnHighlight;
		scheme->clrBtnShadow = state->scheme.clrBtnShadow;
		return TRUE;
	}
	case RB_SETCOLORSCHEME: {
		const COLORSCHEME *scheme = (const COLORSCHEME *)lparam;
		if (scheme) {
			state->scheme.clrBtnHighlight = scheme->clrBtnHighlight;
			state->scheme.clrBtnShadow = scheme->clrBtnShadow;
		}
		return 0;
	}
	case RB_GETPALETTE:
		return (LRESULT)state->palette;
	case RB_SETPALETTE: {
		HPALETTE old = state->palette;
		state->palette = (HPALETTE)lparam;
		return (LRESULT)old;
	}
	case RB_SETPARENT: {
		HWND old = state->parent;
		state->parent = (HWND)wparam;
		return (LRESULT)old;
	}
	case RB_GETTOOLTIPS:
		return (LRESULT)state->tooltips;
	case RB_SETTOOLTIPS:
		state->tooltips = (HWND)wparam;
		return 0;
	case RB_GETUNICODEFORMAT:
		return !state->narrow_format;
	case RB_SETUNICODEFORMAT: {
		BOOL old = !state->narrow_format;
		state->narrow_format = !wparam;
		return old;
	}
	case RB_GETDROPTARGET:
		if (lparam)
			*(void **)lparam = NULL;
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void REBAR_Register(void)
{
	WNDCLASSW info;

	ZeroMemory(&info, sizeof(info));
	info.style = CS_GLOBALCLASS | CS_DBLCLKS;
	info.lpfnWndProc = rebar_proc;
	info.cbWndExtra = sizeof(rebar_state *);
	info.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	info.lpszClassName = REBARCLASSNAMEW;
	RegisterClassW(&info);
}

void REBAR_Unregister(void)
{
	UnregisterClassW(REBARCLASSNAMEW, NULL);
}
