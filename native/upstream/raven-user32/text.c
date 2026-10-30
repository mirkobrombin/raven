/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>

/* DrawText and its relatives, written from Microsoft's public documentation of these functions. A line is turned into plain characters first, with the
 * prefix marks taken out, and everything after that (breaking, ellipses, alignment) works on those characters. */

#define EDIT_CONTROL_ALPHABET L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define DEFAULT_TAB_CHARACTERS 8
#define ELLIPSIS L"..."
#define ELLIPSIS_LENGTH 3

typedef struct {
	HDC dc;
	UINT flags;
	int tab_pixels;
	BOOL failed;
} layout;

typedef struct {
	WCHAR *text;
	int length;
	/* Where the marked character landed, -1 without one. */
	int underline;
	/* For each character, where it came from in the caller's string. */
	int *origin;
} line_text;

/* The width of a run with its tabs expanded, tab stops counted from the start of the line. */
static int measure(layout *state, const WCHAR *text, int length)
{
	int x = 0;
	while (length > 0) {
		int run = 0;
		BOOL tabs = state->flags & DT_EXPANDTABS;
		while (run < length && (!tabs || text[run] != L'\t'))
			run++;
		if (run) {
			SIZE size;
			if (!GetTextExtentPoint32W(state->dc, text, run, &size)) {
				state->failed = TRUE;
				return 0;
			}
			x += size.cx;
		}
		text += run;
		length -= run;
		if (length) {
			x = state->tab_pixels > 0 ? (x / state->tab_pixels + 1) * state->tab_pixels : x;
			text++;
			length--;
		}
	}
	return x;
}

/* How many characters from the start fit in a width, widths growing with the count. */
static int fitting(layout *state, const WCHAR *text, int length, int width)
{
	int low = 0, high = length;
	while (low < high) {
		int middle = low + (high - low + 1) / 2;
		if (measure(state, text, middle) <= width)
			low = middle;
		else
			high = middle - 1;
	}
	return low;
}

static BOOL is_break_space(WCHAR character)
{
	return character == L' ' || character == L'\t';
}

/* Full-width characters can each end a line, which is how CJK text wraps. */
static BOOL is_wide(WCHAR character)
{
	return character >= 0x2e80 && character < 0xfb00;
}

static void strip_prefixes(const layout *state, const WCHAR *source, int length, line_text *line)
{
	line->length = 0;
	line->underline = -1;
	for (int index = 0; index < length; index++) {
		if (!(state->flags & DT_NOPREFIX) && source[index] == L'&') {
			if (index + 1 >= length)
				break;
			index++;
			if (source[index] != L'&' && line->underline < 0)
				line->underline = line->length;
		}
		line->origin[line->length] = index;
		line->text[line->length++] = source[index];
	}
}

/* Splits a line that is too wide: `shown` characters stay on it and the next line starts at character `next`. */
static void break_line(layout *state, const line_text *line, int width, int *shown, int *next)
{
	const WCHAR *text = line->text;
	int fit = fitting(state, text, line->length, width), split = fit;
	while (split > 0 && !is_break_space(text[split])) {
		if (split < line->length && is_wide(text[split - 1]) && !(state->flags & DT_NOFULLWIDTHCHARBREAK))
			break;
		if (split < line->length && is_wide(text[split]) && !(state->flags & DT_NOFULLWIDTHCHARBREAK))
			break;
		split--;
	}
	if (split == 0) {
		/* One word wider than the rectangle: an edit control cuts it, anything else lets it stand, for the rectangle to clip or the ellipsis to cut. */
		if (state->flags & DT_EDITCONTROL) {
			split = fit ? fit : 1;
			*shown = *next = split;
			return;
		}
		split = fit;
		while (split < line->length && !is_break_space(text[split]))
			split++;
	}
	*shown = split;
	while (split < line->length && is_break_space(text[split]))
		split++;
	*next = split;
	while (*shown > 0 && is_break_space(text[*shown - 1]))
		--*shown;
}

static void end_ellipsis(layout *state, line_text *line, int width)
{
	int room = width - measure(state, ELLIPSIS, ELLIPSIS_LENGTH);
	int keep = room > 0 ? fitting(state, line->text, line->length, room) : 0;
	CopyMemory(line->text + keep, ELLIPSIS, sizeof(WCHAR) * ELLIPSIS_LENGTH);
	line->length = keep + ELLIPSIS_LENGTH;
	if (line->underline >= keep)
		line->underline = -1;
}

/* Keeps the name after the last backslash and takes the characters from before it. */
static void path_ellipsis(layout *state, line_text *line, int width)
{
	int name = line->length;
	while (name > 0 && line->text[name - 1] != L'\\')
		name--;
	if (name > 0)
		name--;
	int room = width - measure(state, ELLIPSIS, ELLIPSIS_LENGTH) - measure(state, line->text + name, line->length - name);
	if (!name || room < 0) {
		if (name) {
			/* The name alone is too wide: the directories go and the name ends where the rectangle does. */
			int tail = line->length - name;
			MoveMemory(line->text + ELLIPSIS_LENGTH, line->text + name, sizeof(WCHAR) * tail);
			CopyMemory(line->text, ELLIPSIS, sizeof(WCHAR) * ELLIPSIS_LENGTH);
			line->length = fitting(state, line->text, tail + ELLIPSIS_LENGTH, width);
			line->underline = -1;
			return;
		}
		end_ellipsis(state, line, width);
		return;
	}
	int keep = fitting(state, line->text, name, room), tail = line->length - name;
	MoveMemory(line->text + keep + ELLIPSIS_LENGTH, line->text + name, sizeof(WCHAR) * tail);
	CopyMemory(line->text + keep, ELLIPSIS, sizeof(WCHAR) * ELLIPSIS_LENGTH);
	line->length = keep + ELLIPSIS_LENGTH + tail;
	if (line->underline >= keep)
		line->underline = -1;
}

static void draw_underline(layout *state, const line_text *line, int x, int y, const TEXTMETRICW *metrics)
{
	int before = measure(state, line->text, line->underline);
	int after = measure(state, line->text, line->underline + 1);
	HPEN pen = CreatePen(PS_SOLID, 1, GetTextColor(state->dc));
	if (!pen)
		return;
	HGDIOBJ previous = SelectObject(state->dc, pen);
	POINT position;
	int row = y + metrics->tmAscent + 1;
	MoveToEx(state->dc, x + before, row, &position);
	LineTo(state->dc, x + after, row);
	MoveToEx(state->dc, position.x, position.y, NULL);
	SelectObject(state->dc, previous);
	DeleteObject(pen);
}

static void draw_line(layout *state, const line_text *line, int x, int y, const RECT *rect, const TEXTMETRICW *metrics)
{
	UINT options = (state->flags & DT_NOCLIP ? 0 : ETO_CLIPPED) | (state->flags & DT_RTLREADING ? ETO_RTLREADING : 0);
	if (!(state->flags & DT_PREFIXONLY)) {
		const WCHAR *text = line->text;
		int left = line->length, at = 0;
		while (left > 0) {
			int run = 0;
			while (run < left && (!(state->flags & DT_EXPANDTABS) || text[run] != L'\t'))
				run++;
			if (run && !ExtTextOutW(state->dc, x + at, y, options, rect, text, run, NULL)) {
				state->failed = TRUE;
				return;
			}
			at = measure(state, line->text, (int)(text - line->text) + run);
			text += run;
			left -= run;
			if (left) {
				at = measure(state, line->text, (int)(text - line->text) + 1);
				text++;
				left--;
			}
		}
	}
	if (line->underline >= 0 && line->underline < line->length && !(state->flags & DT_HIDEPREFIX))
		draw_underline(state, line, x, y, metrics);
}

/* The width an edit control takes as average: the mean across the alphabet, which tmAveCharWidth is not. */
static int average_width(HDC dc, UINT flags, const TEXTMETRICW *metrics)
{
	SIZE size;
	if ((flags & DT_EDITCONTROL) && GetTextExtentPoint32W(dc, EDIT_CONTROL_ALPHABET, 52, &size))
		return (size.cx / 26 + 1) / 2;
	return metrics->tmAveCharWidth;
}

static int draw_text(HDC dc, WCHAR *text, int count, RECT *rect, UINT flags, DRAWTEXTPARAMS *params, const TEXTMETRICW *metrics, BOOL explicit_empty)
{
	layout state = {dc, flags, 0, FALSE};
	int average = average_width(dc, flags, metrics), left_margin = 0, right_margin = 0;
	int tab_characters = DEFAULT_TAB_CHARACTERS;
	if (flags & DT_TABSTOP) {
		if (!params && (flags >> 8 & 0xff))
			tab_characters = flags >> 8 & 0xff;
		/* The tab count shares these bits with the flags the documentation rules out beside DT_TABSTOP. */
		state.flags = flags &= ~(UINT)0xff00;
	}
	if (params) {
		if (params->iTabLength > 0 && (flags & DT_TABSTOP))
			tab_characters = params->iTabLength;
		left_margin = params->iLeftMargin * average;
		right_margin = params->iRightMargin * average;
		params->uiLengthDrawn = 0;
	}
	state.tab_pixels = tab_characters * average;

	int line_height = metrics->tmHeight + (flags & DT_EXTERNALLEADING ? metrics->tmExternalLeading : 0);
	/* Logical y grows upward in the mapping modes that invert it, and the lines then go toward smaller values. */
	POINT axis[2] = {{0, 0}, {0, 1}};
	LPtoDP(dc, axis, 2);
	BOOL inverted = axis[1].y < axis[0].y;
	int step = inverted ? -line_height : line_height;

	if (explicit_empty)
		return line_height;
	if (!count && !(flags & DT_SINGLELINE)) {
		if (flags & DT_CALCRECT) {
			rect->right = rect->left;
			rect->bottom = rect->top;
		}
		return line_height;
	}

	int y = rect->top;
	if (flags & DT_SINGLELINE) {
		if (flags & DT_VCENTER)
			y = rect->top + (rect->bottom - rect->top - step) / 2;
		else if (flags & DT_BOTTOM)
			y = rect->bottom - step;
	}
	int top = y, width = rect->right - rect->left - left_margin - right_margin, widest = 0;

	WCHAR *buffer = HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * (count + ELLIPSIS_LENGTH + 1));
	int *origin = HeapAlloc(GetProcessHeap(), 0, sizeof(int) * (count + 1));
	if (!buffer || !origin) {
		HeapFree(GetProcessHeap(), 0, buffer);
		HeapFree(GetProcessHeap(), 0, origin);
		return 0;
	}

	int position = 0;
	do {
		/* The line in the caller's string, up to a line end unless single-line. */
		int end = position;
		if (flags & DT_SINGLELINE)
			end = count;
		else
			while (end < count && text[end] != L'\r' && text[end] != L'\n')
				end++;
		line_text line = {buffer, 0, -1, origin};
		strip_prefixes(&state, text + position, end - position, &line);

		int shown = line.length, next = line.length;
		BOOL broke = FALSE;
		if ((flags & DT_WORDBREAK) && !(flags & DT_SINGLELINE) && measure(&state, line.text, line.length) > width) {
			break_line(&state, &line, width, &shown, &next);
			broke = next < line.length;
		}
		int consumed = broke ? line.origin[next] + position : end;
		if (!broke && !(flags & DT_SINGLELINE) && consumed < count) {
			WCHAR first = text[consumed++];
			if (consumed < count && (text[consumed] == L'\r' || text[consumed] == L'\n') && text[consumed] != first)
				consumed++;
		}
		line.length = shown;

		/* The last line that shows: the text ends or the next line would fall below the rectangle. */
		BOOL no_more = consumed >= count || (!(flags & (DT_CALCRECT | DT_NOCLIP)) && (inverted ? y + 2 * step < rect->bottom : y + 2 * step > rect->bottom));
		if (flags & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS | DT_WORD_ELLIPSIS)) {
			int measured = measure(&state, line.text, line.length);
			if (measured > width) {
				BOOL is_path = flags & DT_PATH_ELLIPSIS;
				if (flags & DT_WORD_ELLIPSIS)
					end_ellipsis(&state, &line, width);
				else if (no_more || (flags & DT_SINGLELINE)) {
					if (is_path)
						path_ellipsis(&state, &line, width);
					else
						end_ellipsis(&state, &line, width);
				}
				if ((flags & DT_MODIFYSTRING) && (flags & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS)) && no_more) {
					/* The documentation lets the string grow by up to four characters: the ellipsis and the terminator. */
					CopyMemory(text + position, line.text, sizeof(WCHAR) * line.length);
					text[position + line.length] = 0;
				}
			}
		}

		int line_width = measure(&state, line.text, line.length);
		if (line_width > widest)
			widest = line_width;
		if (state.failed)
			break;

		if (!(flags & DT_CALCRECT)) {
			int x = rect->left + left_margin;
			if (flags & DT_CENTER)
				x = rect->left + left_margin + (width - line_width) / 2;
			else if (flags & DT_RIGHT)
				x = rect->right - right_margin - line_width;
			draw_line(&state, &line, x, y, rect, metrics);
			if (state.failed)
				break;
		}
		y += step;
		if (params)
			params->uiLengthDrawn += consumed - position;
		position = consumed;
		/* An edit control leaves out a last line that would only partly show. */
		if (position < count && !(flags & (DT_CALCRECT | DT_NOCLIP))) {
			if (inverted ? y <= rect->bottom : y >= rect->bottom)
				break;
			if ((flags & DT_EDITCONTROL) && (inverted ? y + step < rect->bottom : y + step > rect->bottom))
				break;
		}
	} while (position < count);

	HeapFree(GetProcessHeap(), 0, buffer);
	HeapFree(GetProcessHeap(), 0, origin);
	if (state.failed)
		return 0;

	if (flags & DT_CALCRECT) {
		rect->right = rect->left + widest + left_margin + right_margin;
		rect->bottom = y;
	}
	int height = y - ((flags & DT_SINGLELINE) && (flags & (DT_VCENTER | DT_BOTTOM)) ? rect->top : top);
	if (height < 0)
		height = -height;
	/* A text whose bottom is at the top offset reports 1, as a zero would read as failure. */
	return height ? height : 1;
}

INT WINAPI DrawTextExW(HDC dc, LPWSTR text, INT count, LPRECT rect, UINT flags, LPDRAWTEXTPARAMS params)
{
	TEXTMETRICW metrics;
	DWORD error = GetLastError();
	HGDIOBJ previous = NULL;
	/* A bad device context fails first: the other parameters may be just as bad. */
	if (!GetTextMetricsW(dc, &metrics)) {
		SetLastError(error);
		return 0;
	}
	if (!text || !rect)
		return 0;
	BOOL explicit_empty = !count;
	if (count < 0)
		count = lstrlenW(text);
	if (params && (params->cbSize != sizeof(*params) || !*text)) {
		if (explicit_empty && *text)
			return metrics.tmHeight;
		return 0;
	}
	if (flags & DT_INTERNAL) {
		previous = SelectObject(dc, GetStockObject(SYSTEM_FONT));
		GetTextMetricsW(dc, &metrics);
	}
	int result = draw_text(dc, text, count, rect, flags, params, &metrics, explicit_empty);
	if (previous)
		SelectObject(dc, previous);
	return result;
}

/* The ANSI code page behind the font's character set, from the table in the documentation of LOGFONT and code pages. */
static UINT text_code_page(HDC dc)
{
	TEXTMETRICW metrics;
	if (!GetTextMetricsW(dc, &metrics))
		return CP_ACP;
	switch (metrics.tmCharSet) {
	case SHIFTJIS_CHARSET: return 932;
	case HANGUL_CHARSET: return 949;
	case GB2312_CHARSET: return 936;
	case CHINESEBIG5_CHARSET: return 950;
	case EASTEUROPE_CHARSET: return 1250;
	case RUSSIAN_CHARSET: return 1251;
	case ANSI_CHARSET: return 1252;
	case GREEK_CHARSET: return 1253;
	case TURKISH_CHARSET: return 1254;
	case HEBREW_CHARSET: return 1255;
	case ARABIC_CHARSET: return 1256;
	case BALTIC_CHARSET: return 1257;
	case VIETNAMESE_CHARSET: return 1258;
	case THAI_CHARSET: return 874;
	}
	return CP_ACP;
}

INT WINAPI DrawTextExA(HDC dc, LPSTR text, INT count, LPRECT rect, UINT flags, LPDRAWTEXTPARAMS params)
{
	TEXTMETRICW metrics;
	static CHAR nothing[1];
	if (!GetTextMetricsW(dc, &metrics)) {
		SetLastError(ERROR_INVALID_HANDLE);
		return 0;
	}
	if (!count || !rect || (!text && count > 0))
		return 0;
	if (!text)
		text = nothing;
	if (count < 0)
		count = lstrlenA(text);
	UINT code_page = text_code_page(dc);
	int wide = count ? MultiByteToWideChar(code_page, 0, text, count, NULL, 0) : 0;
	WCHAR *buffer = HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * (wide + ELLIPSIS_LENGTH + 1));
	if (!buffer)
		return 0;
	if (wide)
		MultiByteToWideChar(code_page, 0, text, count, buffer, wide);
	buffer[wide] = 0;
	int result = DrawTextExW(dc, buffer, wide ? wide : -1, rect, flags, params);
	if (result && (flags & DT_MODIFYSTRING) && (flags & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS)))
		WideCharToMultiByte(code_page, 0, buffer, -1, text, count + ELLIPSIS_LENGTH + 1, NULL, NULL);
	HeapFree(GetProcessHeap(), 0, buffer);
	return result;
}

INT WINAPI DrawTextW(HDC dc, LPCWSTR text, INT count, LPRECT rect, UINT flags)
{
	return DrawTextExW(dc, (LPWSTR)text, count, rect, flags, NULL);
}

INT WINAPI DrawTextA(HDC dc, LPCSTR text, INT count, LPRECT rect, UINT flags)
{
	return DrawTextExA(dc, (LPSTR)text, count, rect, flags, NULL);
}

/* The next tab stop after a position, as TabbedTextOut documents them: none means eight average characters, one is a spacing, more are positions. */
static int next_tab_stop(int position, int stops, const INT *table, int average)
{
	int spacing = 8 * average;
	if (stops == 1 && table && table[0] > 0)
		spacing = table[0];
	else if (stops > 1 && table) {
		for (int index = 0; index < stops; index++) {
			int stop = table[index] < 0 ? -table[index] : table[index];
			if (stop > position)
				return stop;
		}
	}
	if (spacing <= 0)
		spacing = 1;
	return (position / spacing + 1) * spacing;
}

static LONG tabbed_text(HDC dc, int x, int y, const WCHAR *text, int count, int stops, const INT *table, int origin, BOOL draw)
{
	TEXTMETRICW metrics;
	if (count < 0 || !GetTextMetricsW(dc, &metrics))
		return 0;
	POINT current;
	BOOL update = draw && (GetTextAlign(dc) & TA_UPDATECP);
	if (update && GetCurrentPositionEx(dc, &current)) {
		x = current.x;
		y = current.y;
	}
	int at = x, opaque = draw && GetBkMode(dc) == OPAQUE;
	UINT previous_align = 0;
	if (draw)
		previous_align = SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
	while (count > 0) {
		int run = 0;
		while (run < count && text[run] != L'\t')
			run++;
		SIZE size = {0, 0};
		if (run && !GetTextExtentPoint32W(dc, text, run, &size))
			break;
		if (draw && run)
			TextOutW(dc, at, y, text, run);
		at += size.cx;
		text += run;
		count -= run;
		if (count) {
			int stop = origin + next_tab_stop(at - origin, stops, table, metrics.tmAveCharWidth);
			if (opaque) {
				RECT gap = {at, y, stop, y + metrics.tmHeight};
				ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &gap, NULL, 0, NULL);
			}
			at = stop;
			text++;
			count--;
		}
	}
	if (draw) {
		SetTextAlign(dc, previous_align);
		if (update)
			MoveToEx(dc, at, y, NULL);
	}
	return MAKELONG(at - x, metrics.tmHeight);
}

static WCHAR *widen(HDC dc, LPCSTR text, int count, int *wide)
{
	UINT code_page = text_code_page(dc);
	*wide = count > 0 ? MultiByteToWideChar(code_page, 0, text, count, NULL, 0) : 0;
	WCHAR *buffer = HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * (*wide + 1));
	if (buffer && *wide)
		MultiByteToWideChar(code_page, 0, text, count, buffer, *wide);
	return buffer;
}

LONG WINAPI TabbedTextOutW(HDC dc, INT x, INT y, LPCWSTR text, INT count, INT stops, const INT *table, INT origin)
{
	return tabbed_text(dc, x, y, text, count, stops, table, origin, TRUE);
}

LONG WINAPI TabbedTextOutA(HDC dc, INT x, INT y, LPCSTR text, INT count, INT stops, const INT *table, INT origin)
{
	int wide;
	WCHAR *buffer = widen(dc, text, count, &wide);
	LONG result = buffer ? tabbed_text(dc, x, y, buffer, wide, stops, table, origin, TRUE) : 0;
	HeapFree(GetProcessHeap(), 0, buffer);
	return result;
}

DWORD WINAPI GetTabbedTextExtentW(HDC dc, LPCWSTR text, INT count, INT stops, const INT *table)
{
	return tabbed_text(dc, 0, 0, text, count, stops, table, 0, FALSE);
}

DWORD WINAPI GetTabbedTextExtentA(HDC dc, LPCSTR text, INT count, INT stops, const INT *table)
{
	int wide;
	WCHAR *buffer = widen(dc, text, count, &wide);
	DWORD result = buffer ? tabbed_text(dc, 0, 0, buffer, wide, stops, table, 0, FALSE) : 0;
	HeapFree(GetProcessHeap(), 0, buffer);
	return result;
}

/* Two raster operations: pattern AND destination, and the pattern where the source is set with the destination elsewhere. */
#define ROP_DESTINATION_AND_PATTERN 0x00a000c9
#define ROP_PATTERN_THROUGH_SOURCE 0x00e20746

static BOOL gray_string(HDC dc, HBRUSH brush, GRAYSTRINGPROC output, LPARAM data, int count, int x, int y, int width, int height, BOOL unicode)
{
	if (!count && !output && data)
		count = unicode ? lstrlenW((LPCWSTR)data) : lstrlenA((LPCSTR)data);
	if (!width || !height) {
		SIZE size = {0, 0};
		if (!data)
			return FALSE;
		BOOL known = unicode ? GetTextExtentPoint32W(dc, (LPCWSTR)data, count ? count : lstrlenW((LPCWSTR)data), &size)
				     : GetTextExtentPoint32A(dc, (LPCSTR)data, count ? count : lstrlenA((LPCSTR)data), &size);
		if (!known)
			return FALSE;
		if (!width)
			width = size.cx;
		if (!height)
			height = size.cy;
	}
	HDC memory = CreateCompatibleDC(dc);
	HBITMAP mask = CreateBitmap(width, height, 1, 1, NULL);
	static const WORD half_tone[8] = {0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55};
	HBITMAP pattern = CreateBitmap(8, 8, 1, 1, half_tone);
	HBRUSH gray = pattern ? CreatePatternBrush(pattern) : NULL;
	BOOL result = FALSE;
	if (memory && mask && gray) {
		HGDIOBJ old_bitmap = SelectObject(memory, mask);
		HGDIOBJ old_font = SelectObject(memory, GetCurrentObject(dc, OBJ_FONT));
		HGDIOBJ old_brush = SelectObject(memory, gray);
		PatBlt(memory, 0, 0, width, height, BLACKNESS);
		SetTextColor(memory, RGB(255, 255, 255));
		SetBkColor(memory, RGB(0, 0, 0));
		if (output)
			result = output(memory, data, count);
		else
			result = unicode ? TextOutW(memory, 0, 0, (LPCWSTR)data, count) : TextOutA(memory, 0, 0, (LPCSTR)data, count);
		if (result || (output && count == 1)) {
			/* A failed output of one character shows the image ungrayed. */
			if (result)
				PatBlt(memory, 0, 0, width, height, ROP_DESTINATION_AND_PATTERN);
			HGDIOBJ previous = SelectObject(dc, brush ? brush : GetSysColorBrush(COLOR_WINDOWTEXT));
			COLORREF text = SetTextColor(dc, RGB(0, 0, 0)), back = SetBkColor(dc, RGB(255, 255, 255));
			BitBlt(dc, x, y, width, height, memory, 0, 0, ROP_PATTERN_THROUGH_SOURCE);
			SetTextColor(dc, text);
			SetBkColor(dc, back);
			SelectObject(dc, previous);
		}
		SelectObject(memory, old_brush);
		SelectObject(memory, old_font);
		SelectObject(memory, old_bitmap);
	}
	if (gray)
		DeleteObject(gray);
	if (pattern)
		DeleteObject(pattern);
	if (mask)
		DeleteObject(mask);
	if (memory)
		DeleteDC(memory);
	return result;
}

BOOL WINAPI GrayStringW(HDC dc, HBRUSH brush, GRAYSTRINGPROC output, LPARAM data, INT count, INT x, INT y, INT width, INT height)
{
	return gray_string(dc, brush, output, data, count, x, y, width, height, TRUE);
}

BOOL WINAPI GrayStringA(HDC dc, HBRUSH brush, GRAYSTRINGPROC output, LPARAM data, INT count, INT x, INT y, INT width, INT height)
{
	return gray_string(dc, brush, output, data, count, x, y, width, height, FALSE);
}
