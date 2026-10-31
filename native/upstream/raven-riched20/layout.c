/* SPDX-License-Identifier: MIT */
#include "re.h"

void re_layout_free(re_layout *layout)
{
	re_free(layout->lines);
	memset(layout, 0, sizeof(*layout));
}

INT re_zoomed(const re_editor *editor, INT value)
{
	return editor->zoom_den > 0 && editor->zoom_num > 0 ? MulDiv(value, editor->zoom_num, editor->zoom_den) : value;
}

static INT twips_to_pixels(INT twips, INT dpi)
{
	return MulDiv(twips, dpi, 1440);
}

HFONT re_create_font(const CHARFORMAT2W *format, HDC dc, const re_editor *editor)
{
	LOGFONTW description;
	memset(&description, 0, sizeof(description));
	INT dpi = GetDeviceCaps(dc, LOGPIXELSY);
	if (dpi <= 0)
		dpi = 96;
	INT height = twips_to_pixels(format->yHeight, dpi);
	// Sub- and superscript shrink, as the zoom scales.
	if (format->dwEffects & (CFE_SUBSCRIPT | CFE_SUPERSCRIPT))
		height = height * 2 / 3;
	if (editor)
		height = re_zoomed(editor, height);
	description.lfHeight = -height;
	description.lfWeight = format->wWeight ? format->wWeight : (format->dwEffects & CFE_BOLD ? FW_BOLD : FW_NORMAL);
	if ((format->dwEffects & CFE_BOLD) && description.lfWeight < FW_BOLD)
		description.lfWeight = FW_BOLD;
	description.lfItalic = !!(format->dwEffects & CFE_ITALIC);
	description.lfUnderline = !!((format->dwEffects & CFE_UNDERLINE) || (format->dwEffects & CFE_LINK));
	description.lfStrikeOut = !!(format->dwEffects & CFE_STRIKEOUT);
	description.lfCharSet = format->bCharSet;
	description.lfPitchAndFamily = format->bPitchAndFamily;
	lstrcpynW(description.lfFaceName, format->szFaceName, LF_FACESIZE);
	return CreateFontIndirectW(&description);
}

// The text as measured or painted: the password character hides it all, and tabs do not go to the font.
typedef struct
{
	HDC dc;
	re_editor *editor;
	HFONT font, previous;
	INT run;
	TEXTMETRICW metrics;
} measure;

static void measure_begin(measure *state, HDC dc, re_editor *editor)
{
	state->dc = dc;
	state->editor = editor;
	state->font = NULL;
	state->previous = NULL;
	state->run = -1;
}

static void measure_font(measure *state, INT run)
{
	if (state->run == run)
		return;
	HFONT font = re_create_font(&state->editor->doc.runs[run].format, state->dc, state->editor);
	HFONT old = SelectObject(state->dc, font);
	if (!state->previous)
		state->previous = old;
	if (state->font)
		DeleteObject(state->font);
	state->font = font;
	state->run = run;
	GetTextMetricsW(state->dc, &state->metrics);
}

static void measure_end(measure *state)
{
	if (state->previous)
		SelectObject(state->dc, state->previous);
	if (state->font)
		DeleteObject(state->font);
	state->font = state->previous = NULL;
}

static INT default_tab_width(const measure *state)
{
	INT dpi = GetDeviceCaps(state->dc, LOGPIXELSX);
	return re_zoomed(state->editor, MulDiv(720, dpi > 0 ? dpi : 96, 1440));
}

static INT next_tab(const measure *state, const PARAFORMAT2 *para, INT x)
{
	INT dpi = GetDeviceCaps(state->dc, LOGPIXELSX);
	if (dpi <= 0)
		dpi = 96;
	for (INT i = 0; i < para->cTabCount && i < MAX_TAB_STOPS; i++) {
		INT stop = re_zoomed(state->editor, MulDiv(para->rgxTabs[i] & 0xffffff, dpi, 1440));
		if (stop > x)
			return stop;
	}
	INT step = default_tab_width(state);
	return step > 0 ? (x / step + 1) * step : x + 1;
}

// How wide a stretch of one run is, starting at a pixel offset that matters only for tabs.
static INT stretch_width(measure *state, INT start, INT count, const PARAFORMAT2 *para, INT x)
{
	const re_doc *doc = &state->editor->doc;
	INT left = x;
	while (count > 0) {
		INT run = re_doc_run_at(doc, start);
		INT run_end = run + 1 < doc->run_count ? doc->runs[run + 1].start : doc->length;
		INT take = run_end - start < count ? run_end - start : count;
		measure_font(state, run);
		INT i = 0;
		while (i < take) {
			WCHAR c = doc->text[start + i];
			if (c == RE_TAB && !state->editor->password) {
				x = next_tab(state, para, x);
				i++;
				continue;
			}
			if (c == RE_CR) {
				i++;
				continue;
			}
			SIZE object;
			if (c == RE_OBJECT_MARK && re_ole_object_size(state->editor, state->dc, start + i, &object)) {
				x += object.cx;
				i++;
				continue;
			}
			INT j = i;
			while (j < take && doc->text[start + j] != RE_CR && doc->text[start + j] != RE_OBJECT_MARK && (doc->text[start + j] != RE_TAB || state->editor->password))
				j++;
			if (j == i)
				j = i + 1;
			SIZE size;
			if (state->editor->password) {
				WCHAR hidden = state->editor->password;
				GetTextExtentPoint32W(state->dc, &hidden, 1, &size);
				size.cx *= j - i;
			} else {
				GetTextExtentPoint32W(state->dc, doc->text + start + i, j - i, &size);
			}
			x += size.cx;
			i = j;
		}
		start += take;
		count -= take;
	}
	return x - left;
}

static BOOL push_line(re_layout *out, const re_line *line)
{
	if (out->count == out->capacity) {
		INT capacity = out->capacity ? out->capacity * 2 : 32;
		re_line *grown = re_realloc(out->lines, capacity * sizeof(re_line));
		if (!grown)
			return FALSE;
		out->lines = grown;
		out->capacity = capacity;
	}
	out->lines[out->count++] = *line;
	return TRUE;
}

static BOOL is_break_space(WCHAR c)
{
	return c == ' ' || c == RE_TAB;
}

// Finds how far a line reaches. Returns the characters on it, a run of blanks staying with the line they follow.
static INT fit_line(measure *state, INT start, INT end, const PARAFORMAT2 *para, INT first_x, INT limit)
{
	const re_doc *doc = &state->editor->doc;
	INT x = first_x, at = start, last_break = -1;
	while (at < end) {
		WCHAR c = doc->text[at];
		INT width = stretch_width(state, at, 1, para, x);
		if (x + width > limit && !is_break_space(c) && c != RE_CR) {
			if (last_break > start)
				return last_break - start;
			return at > start ? at - start : 1;
		}
		x += width;
		at++;
		if (is_break_space(c) && (at >= end || !is_break_space(doc->text[at])))
			last_break = at;
	}
	return end - start;
}

void re_layout_document(re_editor *editor, HDC dc, re_layout *out, INT width, INT num, INT den)
{
	(void)num;
	(void)den;
	re_doc *doc = &editor->doc;
	measure state;
	measure_begin(&state, dc, editor);
	out->count = 0;
	out->total_height = 0;
	out->widest = 0;
	INT dpi_x = GetDeviceCaps(dc, LOGPIXELSX), dpi_y = GetDeviceCaps(dc, LOGPIXELSY);
	if (dpi_x <= 0)
		dpi_x = 96;
	if (dpi_y <= 0)
		dpi_y = 96;
	INT y = 0;
	for (INT p = 0; p < doc->para_count; p++) {
		const PARAFORMAT2 *para = &doc->paras[p].format;
		INT start = doc->paras[p].start, end = re_doc_para_end(doc, p);
		INT indent = re_zoomed(editor, twips_to_pixels(para->dxStartIndent, dpi_x));
		INT right = re_zoomed(editor, twips_to_pixels(para->dxRightIndent, dpi_x));
		INT offset = re_zoomed(editor, twips_to_pixels(para->dxOffset, dpi_x));
		y += re_zoomed(editor, twips_to_pixels(para->dySpaceBefore, dpi_y));
		INT at = start;
		BOOL first = TRUE;
		while (at < end) {
			INT left = first ? indent : indent + offset;
			if (left < 0)
				left = 0;
			INT limit = editor->wrap && width > 0 ? width - right : 0x3fffffff;
			INT count = fit_line(&state, at, end, para, left, limit);
			if (count <= 0)
				count = end - at;
			re_line line;
			memset(&line, 0, sizeof(line));
			line.start = at;
			line.length = count;
			line.para = p;
			line.y = y;
			// The tallest piece on the line sets its height and baseline.
			INT ascent = 0, descent = 0;
			for (INT i = re_doc_run_at(doc, at); i < doc->run_count && doc->runs[i].start < at + count; i++) {
				measure_font(&state, i);
				if (state.metrics.tmAscent > ascent)
					ascent = state.metrics.tmAscent;
				if (state.metrics.tmDescent > descent)
					descent = state.metrics.tmDescent;
			}
			// An object stands on the baseline.
			for (INT i = at; i < at + count; i++) {
				SIZE object;
				if (doc->text[i] == RE_OBJECT_MARK && re_ole_object_size(editor, dc, i, &object) && object.cy > ascent)
					ascent = object.cy;
			}
			line.ascent = ascent;
			line.height = ascent + descent;
			INT visible = count;
			while (visible > 0 && (doc->text[at + visible - 1] == RE_CR || doc->text[at + visible - 1] == ' '))
				visible--;
			line.visible = visible;
			line.width = stretch_width(&state, at, visible, para, left);
			INT room = limit == 0x3fffffff ? line.width : limit - left;
			line.x = left;
			if (para->wAlignment == PFA_RIGHT && room > line.width)
				line.x = left + room - line.width;
			else if (para->wAlignment == PFA_CENTER && room > line.width)
				line.x = left + (room - line.width) / 2;
			if (para->dwMask & PFM_LINESPACING && para->dyLineSpacing) {
				switch (para->bLineSpacingRule) {
				case 1: line.height = line.height * 3 / 2; break;
				case 2: line.height = line.height * 2; break;
				case 3: if (twips_to_pixels(para->dyLineSpacing, dpi_y) > line.height) line.height = twips_to_pixels(para->dyLineSpacing, dpi_y); break;
				case 4: line.height = twips_to_pixels(para->dyLineSpacing, dpi_y); break;
				case 5: line.height = line.height * para->dyLineSpacing / 20; break;
				default: break;
				}
			}
			if (!push_line(out, &line))
				break;
			y += line.height;
			if (line.x + line.width > out->widest)
				out->widest = line.x + line.width;
			at += count;
			first = FALSE;
		}
		y += re_zoomed(editor, twips_to_pixels(para->dySpaceAfter, dpi_y));
	}
	out->total_height = y;
	measure_end(&state);
}

void re_view_rect(re_editor *editor, RECT *out)
{
	RECT client = {0, 0, 0, 0};
	if (editor->has_format_rect) {
		*out = editor->format_rect;
		return;
	}
	// Out of place the only rectangle known is where it was last told to draw.
	if (editor->host && !editor->in_place && editor->client_valid)
		client = editor->client;
	else if (editor->host)
		ITextHost_TxGetClientRect(editor->host, &client);
	else if (editor->hwnd)
		GetClientRect(editor->hwnd, &client);
	*out = client;
	if (editor->style & ES_MULTILINE) {
		out->left += 1;
		out->right -= 1;
		if (editor->style & WS_BORDER || editor->ex_style & (WS_EX_CLIENTEDGE | WS_EX_STATICEDGE))
			out->top += 1;
	}
	out->left += editor->margin_left;
	out->right -= editor->margin_right;
}

static HDC editor_dc(re_editor *editor)
{
	if (editor->host)
		return ITextHost_TxGetDC(editor->host);
	if (editor->hwnd)
		return GetDC(editor->hwnd);
	return GetDC(NULL);
}

static void release_editor_dc(re_editor *editor, HDC dc)
{
	if (editor->host)
		ITextHost_TxReleaseDC(editor->host, dc);
	else if (editor->hwnd)
		ReleaseDC(editor->hwnd, dc);
	else
		ReleaseDC(NULL, dc);
}

void re_layout_update(re_editor *editor)
{
	RECT view;
	re_view_rect(editor, &view);
	HDC dc = editor_dc(editor);
	if (!dc)
		return;
	INT width = view.right - view.left;
	if (editor->target_width)
		width = MulDiv(editor->target_width, GetDeviceCaps(dc, LOGPIXELSX), 1440);
	re_layout_document(editor, dc, &editor->layout, width, 1, 1);
	release_editor_dc(editor, dc);
	if (editor->event_mask & ENM_REQUESTRESIZE) {
		REQRESIZE request;
		memset(&request, 0, sizeof(request));
		request.rc = view;
		request.rc.bottom = request.rc.top + editor->layout.total_height;
		re_send_notify(editor, EN_REQUESTRESIZE, &request.nmhdr);
	}
}

INT re_line_at(const re_layout *layout, INT at, BOOL at_line_start)
{
	if (!layout->count)
		return 0;
	INT low = 0, high = layout->count - 1;
	while (low < high) {
		INT middle = (low + high + 1) / 2;
		if (layout->lines[middle].start <= at)
			low = middle;
		else
			high = middle - 1;
	}
	// A caret at a soft break sits at the end of the upper line unless it was placed at the line's start.
	if (!at_line_start && low > 0 && layout->lines[low].start == at && layout->lines[low - 1].para == layout->lines[low].para)
		return low - 1;
	return low;
}

void re_position(re_editor *editor, INT at, BOOL at_line_start, POINT *out, INT *height)
{
	RECT view;
	re_view_rect(editor, &view);
	const re_layout *layout = &editor->layout;
	if (!layout->count) {
		out->x = view.left;
		out->y = view.top;
		if (height)
			*height = 0;
		return;
	}
	if (at >= editor->doc.length)
		at = editor->doc.length - 1;
	INT index = re_line_at(layout, at, at_line_start);
	const re_line *line = &layout->lines[index];
	HDC dc = editor_dc(editor);
	measure state;
	measure_begin(&state, dc, editor);
	INT width = stretch_width(&state, line->start, at - line->start, &editor->doc.paras[line->para].format, line->x);
	measure_end(&state);
	release_editor_dc(editor, dc);
	out->x = view.left + line->x + width - editor->scroll_x;
	out->y = view.top + line->y - editor->scroll_y;
	if (height)
		*height = line->height;
}

INT re_char_from_point(re_editor *editor, INT x, INT y, BOOL *at_line_start)
{
	RECT view;
	re_view_rect(editor, &view);
	const re_layout *layout = &editor->layout;
	if (at_line_start)
		*at_line_start = FALSE;
	if (!layout->count)
		return 0;
	INT doc_x = x - view.left + editor->scroll_x, doc_y = y - view.top + editor->scroll_y;
	INT index = 0;
	while (index + 1 < layout->count && doc_y >= layout->lines[index].y + layout->lines[index].height)
		index++;
	const re_line *line = &layout->lines[index];
	HDC dc = editor_dc(editor);
	measure state;
	measure_begin(&state, dc, editor);
	const PARAFORMAT2 *para = &editor->doc.paras[line->para].format;
	INT result = line->start, pen = line->x;
	INT usable = line->length;
	while (usable > 0 && editor->doc.text[line->start + usable - 1] == RE_CR)
		usable--;
	for (INT i = 0; i < usable; i++) {
		INT width = stretch_width(&state, line->start + i, 1, para, pen);
		// Past the middle of a character the caret goes after it.
		if (doc_x < pen + width / 2)
			break;
		pen += width;
		result = line->start + i + 1;
	}
	measure_end(&state);
	release_editor_dc(editor, dc);
	if (at_line_start)
		*at_line_start = result == line->start;
	return result;
}

static COLORREF text_color(const re_editor *editor, const CHARFORMAT2W *format)
{
	if (format->dwEffects & CFE_LINK)
		return RGB(0, 0, 255);
	if (format->dwEffects & CFE_AUTOCOLOR) {
		(void)editor;
		return GetSysColor(COLOR_WINDOWTEXT);
	}
	return format->crTextColor;
}

// Draws one line's stretch that shares a run and a selection state.
static void draw_piece(re_editor *editor, HDC dc, measure *state, const re_line *line, INT start, INT count, INT *x, INT top, BOOL selected)
{
	const re_doc *doc = &editor->doc;
	const PARAFORMAT2 *para = &doc->paras[line->para].format;
	INT run = re_doc_run_at(doc, start);
	const CHARFORMAT2W *format = &doc->runs[run].format;
	measure_font(state, run);
	INT baseline = top + line->ascent - state->metrics.tmAscent;
	INT dpi = GetDeviceCaps(dc, LOGPIXELSY);
	if (format->dwEffects & CFE_SUBSCRIPT)
		baseline += state->metrics.tmHeight / 3;
	if (format->dwEffects & CFE_SUPERSCRIPT)
		baseline -= state->metrics.tmHeight / 3;
	baseline -= re_zoomed(editor, twips_to_pixels(format->yOffset, dpi > 0 ? dpi : 96));
	COLORREF fore = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : text_color(editor, format);
	COLORREF back = selected ? GetSysColor(COLOR_HIGHLIGHT) : format->crBackColor;
	BOOL opaque = selected || !(format->dwEffects & CFE_AUTOBACKCOLOR);
	SetTextColor(dc, fore);
	SetBkColor(dc, back);
	SetBkMode(dc, opaque ? OPAQUE : TRANSPARENT);
	INT i = 0;
	while (i < count) {
		INT j = i;
		BOOL tab = doc->text[start + i] == RE_TAB && !editor->password;
		BOOL object = doc->text[start + i] == RE_OBJECT_MARK;
		if (tab || object) {
			j = i + 1;
		} else {
			while (j < count && doc->text[start + j] != RE_OBJECT_MARK && (doc->text[start + j] != RE_TAB || editor->password))
				j++;
		}
		INT width = stretch_width(state, start + i, j - i, para, *x - (line->x - line->x));
		measure_font(state, run);
		if (object) {
			re_ole_draw(editor, dc, start + i, *x, top + line->ascent);
		} else if (tab) {
			if (opaque) {
				RECT area = {*x, top, *x + width, top + line->height};
				ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &area, NULL, 0, NULL);
			}
		} else if (editor->password) {
			INT n = j - i;
			WCHAR *hidden = re_alloc(n * sizeof(WCHAR));
			if (hidden) {
				for (INT k = 0; k < n; k++)
					hidden[k] = editor->password;
				TextOutW(dc, *x, baseline, hidden, n);
				re_free(hidden);
			}
		} else {
			TextOutW(dc, *x, baseline, doc->text + start + i, j - i);
		}
		*x += width;
		i = j;
	}
}

static void draw_line(re_editor *editor, HDC dc, measure *state, const re_line *line, INT left, INT top, BOOL selection, INT sel_start, INT sel_end)
{
	const re_doc *doc = &editor->doc;
	INT x = left + line->x;
	INT at = line->start, end = line->start + line->length;
	while (at < end) {
		INT run = re_doc_run_at(doc, at);
		INT stop = run + 1 < doc->run_count ? doc->runs[run + 1].start : doc->length;
		if (stop > end)
			stop = end;
		BOOL selected = selection && at >= sel_start && at < sel_end;
		if (selection) {
			if (at < sel_start && sel_start < stop)
				stop = sel_start;
			if (at < sel_end && sel_end < stop)
				stop = sel_end;
		}
		INT count = stop - at;
		if (count > 0 && doc->text[stop - 1] == RE_CR)
			count--;
		if (count > 0)
			draw_piece(editor, dc, state, line, at, count, &x, top, selected);
		at = stop;
	}
}

void re_paint(re_editor *editor, HDC dc, const RECT *update)
{
	RECT view;
	re_view_rect(editor, &view);
	COLORREF back = editor->back_system ? GetSysColor(COLOR_WINDOW) : editor->back_color;
	// A transparent one leaves its background to what the parent painted, as labels made of the control rely on.
	enum TXTBACKSTYLE back_style = TXTBACK_OPAQUE;
	if (editor->host)
		ITextHost_TxGetBackStyle(editor->host, &back_style);
	if (!(editor->ex_style & WS_EX_TRANSPARENT) && back_style != TXTBACK_TRANSPARENT) {
		HBRUSH brush = CreateSolidBrush(back);
		RECT fill;
		if (update)
			fill = *update;
		else
			fill = view;
		FillRect(dc, &fill, brush);
		DeleteObject(brush);
	}
	INT saved = SaveDC(dc);
	IntersectClipRect(dc, view.left, view.top, view.right, view.bottom);
	INT sel_start = editor->sel_start < editor->sel_end ? editor->sel_start : editor->sel_end;
	INT sel_end = editor->sel_start < editor->sel_end ? editor->sel_end : editor->sel_start;
	BOOL selection = sel_start != sel_end && (editor->focused || !editor->hide_selection);
	measure state;
	measure_begin(&state, dc, editor);
	for (INT i = 0; i < editor->layout.count; i++) {
		const re_line *line = &editor->layout.lines[i];
		INT top = view.top + line->y - editor->scroll_y;
		if (top >= view.bottom)
			break;
		if (top + line->height <= view.top)
			continue;
		draw_line(editor, dc, &state, line, view.left - editor->scroll_x, top, selection, sel_start, sel_end);
	}
	measure_end(&state);
	RestoreDC(dc, saved);
}

void re_invalidate(re_editor *editor)
{
	if (editor->updating)
		return;
	if (editor->host) {
		ITextHost_TxInvalidateRect(editor->host, NULL, FALSE);
		editor->host->lpVtbl->TxViewChange(editor->host, TRUE);
	} else if (editor->hwnd) {
		InvalidateRect(editor->hwnd, NULL, TRUE);
	}
}

void re_update_caret(re_editor *editor)
{
	if (editor->host) {
		POINT at;
		INT height;
		if (!editor->focused)
			return;
		re_position(editor, editor->sel_end, editor->caret_at_line_start, &at, &height);
		ITextHost_TxCreateCaret(editor->host, NULL, 1, height ? height : 1);
		ITextHost_TxSetCaretPos(editor->host, at.x, at.y);
		ITextHost_TxShowCaret(editor->host, editor->sel_start == editor->sel_end);
		return;
	}
	if (!editor->hwnd || !editor->focused)
		return;
	POINT at;
	INT height;
	re_position(editor, editor->sel_end, editor->caret_at_line_start, &at, &height);
	if (editor->has_caret)
		DestroyCaret();
	CreateCaret(editor->hwnd, NULL, 1, height ? height : 1);
	editor->has_caret = TRUE;
	SetCaretPos(at.x, at.y);
	if (editor->sel_start == editor->sel_end && !editor->hide_caret)
		ShowCaret(editor->hwnd);
}

static INT view_height(re_editor *editor)
{
	RECT view;
	re_view_rect(editor, &view);
	return view.bottom - view.top;
}

static INT view_width(re_editor *editor)
{
	RECT view;
	re_view_rect(editor, &view);
	return view.right - view.left;
}

void re_scroll_to(re_editor *editor, INT x, INT y)
{
	INT most_y = editor->layout.total_height - view_height(editor);
	if (most_y < 0)
		most_y = 0;
	INT most_x = editor->layout.widest - view_width(editor);
	if (most_x < 0)
		most_x = 0;
	if (y > most_y)
		y = most_y;
	if (y < 0)
		y = 0;
	if (x > most_x)
		x = most_x;
	if (x < 0)
		x = 0;
	if (x == editor->scroll_x && y == editor->scroll_y)
		return;
	INT moved_x = editor->scroll_x - x, moved_y = editor->scroll_y - y;
	editor->scroll_x = x;
	editor->scroll_y = y;
	re_update_scrollbars(editor);
	if (editor->host && editor->in_place)
		editor->host->lpVtbl->TxScrollWindowEx(editor->host, moved_x, moved_y, NULL, NULL, NULL, NULL, SW_INVALIDATE);
	else
		re_invalidate(editor);
	re_update_caret(editor);
}

void re_scroll_to_caret(re_editor *editor)
{
	if (!editor->layout.count)
		return;
	POINT at;
	INT height;
	RECT view;
	re_view_rect(editor, &view);
	re_position(editor, editor->sel_end, editor->caret_at_line_start, &at, &height);
	INT x = editor->scroll_x, y = editor->scroll_y;
	if (at.y < view.top)
		y -= view.top - at.y;
	else if (at.y >= view.bottom)
		y += at.y + height - view.bottom;
	if (at.x < view.left)
		x -= view.left - at.x;
	else if (at.x >= view.right)
		x += at.x - view.right + 1;
	re_scroll_to(editor, x, y);
}

// A bar the text no longer needs is hidden with its range left as it was. The range is only sent when it changes, since sending it shows the bar
// again and the application may have hidden it on purpose.
static void update_bar(re_editor *editor, INT bar, DWORD flag, BOOL need, INT total, INT page, INT position, BOOL *shown, INT *sent)
{
	if (!(editor->scroll_bars & flag))
		return;
	BOOL keep = (editor->scroll_bars & ES_DISABLENOSCROLL) != 0;
	if (need && (sent[0] != total || sent[1] != page || sent[2] != position)) {
		SCROLLINFO info;
		memset(&info, 0, sizeof(info));
		info.cbSize = sizeof(info);
		info.fMask = SIF_PAGE | SIF_POS | SIF_RANGE;
		info.nMax = total - 1;
		info.nPage = page;
		info.nPos = position;
		SetScrollInfo(editor->hwnd, bar, &info, TRUE);
		sent[0] = total;
		sent[1] = page;
		sent[2] = position;
	}
	if (!keep && need != *shown)
		ShowScrollBar(editor->hwnd, bar, need);
	*shown = need;
}

void re_update_scrollbars(re_editor *editor)
{
	if (editor->in_scrollbars)
		return;
	editor->in_scrollbars = TRUE;
	INT page_v = view_height(editor), page_h = view_width(editor);
	INT total_v = editor->layout.total_height, total_h = editor->layout.widest;
	BOOL was_v = editor->scroll_v_shown;
	BOOL need_v = total_v > page_v, need_h = total_h > page_h;
	if (editor->host) {
		ITextHost_TxSetScrollRange(editor->host, SB_VERT, 0, total_v > 0 ? total_v - 1 : 0, FALSE);
		ITextHost_TxSetScrollPos(editor->host, SB_VERT, editor->scroll_y, TRUE);
		editor->in_scrollbars = FALSE;
		return;
	}
	if (editor->hwnd) {
		update_bar(editor, SB_VERT, WS_VSCROLL, need_v, total_v, page_v, editor->scroll_y, &editor->scroll_v_shown, editor->scroll_v_sent);
		update_bar(editor, SB_HORZ, WS_HSCROLL, need_h, total_h, page_h, editor->scroll_x, &editor->scroll_h_shown, editor->scroll_h_sent);
		if (need_v != was_v && editor->wrap)
			re_layout_update(editor);
	}
	editor->in_scrollbars = FALSE;
}

// Prints from the first character asked for: a page's worth of lines, measured on the printing device.
LRESULT re_format_range(re_editor *editor, FORMATRANGE *range, BOOL render)
{
	if (!range)
		return 0;
	HDC dc = range->hdc;
	HDC target = range->hdcTarget ? range->hdcTarget : dc;
	INT dpi_x = GetDeviceCaps(target, LOGPIXELSX), dpi_y = GetDeviceCaps(target, LOGPIXELSY);
	INT width = MulDiv(range->rc.right - range->rc.left, dpi_x, 1440);
	INT bottom_limit = MulDiv(range->rc.bottom - range->rc.top, dpi_y, 1440);
	re_layout layout;
	memset(&layout, 0, sizeof(layout));
	INT zoom_num = editor->zoom_num, zoom_den = editor->zoom_den;
	editor->zoom_num = editor->zoom_den = 0;
	re_layout_document(editor, target, &layout, width, 1, 1);
	INT start = range->chrg.cpMin, stop = range->chrg.cpMax < 0 ? editor->doc.length : range->chrg.cpMax;
	if (stop > editor->doc.length)
		stop = editor->doc.length;
	INT first = re_line_at(&layout, start, TRUE), reached = start;
	INT origin = first < layout.count ? layout.lines[first].y : 0;
	INT left = MulDiv(range->rc.left, dpi_x, 1440), top = MulDiv(range->rc.top, dpi_y, 1440);
	measure state;
	measure_begin(&state, dc, editor);
	for (INT i = first; i < layout.count; i++) {
		const re_line *line = &layout.lines[i];
		if (line->start >= stop)
			break;
		if (line->y - origin + line->height > bottom_limit)
			break;
		if (render)
			draw_line(editor, dc, &state, line, left, top + line->y - origin, FALSE, 0, 0);
		reached = line->start + line->length;
	}
	measure_end(&state);
	editor->zoom_num = zoom_num;
	editor->zoom_den = zoom_den;
	re_layout_free(&layout);
	if (reached > editor->doc.length - 1 && start < reached)
		reached = stop > editor->doc.length - 1 ? editor->doc.length : reached;
	return reached;
}
