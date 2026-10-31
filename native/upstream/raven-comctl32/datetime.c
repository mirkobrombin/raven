/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>

/* The format is cut once into pieces, each a literal or an editable part of the date. The text and the place of every
 * piece are rebuilt whenever the date, the font or the size change, so painting and hit testing only read them. */

#define MAX_PIECES 64
#define PIECE_TEXT 64
#define CHECK_SIZE 13
#define BUTTON_WIDTH 17
#define TEXT_INSET 2
#define FIRST_VALID_YEAR 1601
#define CLOSE_POPUP (WM_USER + 100)
#define POPUP_SUBCLASS 1

#ifndef DTM_GETIDEALSIZE
#define DTM_GETIDEALSIZE (DTM_FIRST + 15)
#endif
#ifndef DTM_CLOSEMONTHCAL
#define DTM_CLOSEMONTHCAL (DTM_FIRST + 13)
#endif
#ifndef DTM_SETMCSTYLE
#define DTM_SETMCSTYLE (DTM_FIRST + 11)
#define DTM_GETMCSTYLE (DTM_FIRST + 12)
#endif

enum piece_kind {
	PIECE_LITERAL,
	PIECE_ERA,
	PIECE_DAY,
	PIECE_WEEKDAY,
	PIECE_HOUR12,
	PIECE_HOUR24,
	PIECE_MINUTE,
	PIECE_SECOND,
	PIECE_MONTH,
	PIECE_AMPM,
	PIECE_YEAR,
	PIECE_CALLBACK,
};

typedef struct {
	enum piece_kind kind;
	int length;
	WCHAR token[16];
	WCHAR text[PIECE_TEXT];
	RECT area;
} date_piece;

typedef struct {
	HWND window, parent, spin, popup, editor;
	HFONT font, popup_font;
	SYSTEMTIME date, limits[2];
	DWORD limit_flags, popup_style;
	BOOL has_date, narrow_parent, focused;
	WCHAR *format;
	date_piece pieces[MAX_PIECES];
	int count, current, typed, typed_digits;
	COLORREF popup_colors[6];
	RECT check, button;
} picker_state;

static picker_state *state_of(HWND window)
{
	return (picker_state *)GetWindowLongPtrW(window, 0);
}

static DWORD style_of(const picker_state *state)
{
	return (DWORD)GetWindowLongPtrW(state->window, GWL_STYLE);
}

static BOOL is_leap(int year)
{
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int month_length(int year, int month)
{
	static const BYTE lengths[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	if (month == 2 && is_leap(year))
		return 29;
	return lengths[month - 1];
}

static int weekday_of(int year, int month, int day)
{
	static const BYTE offsets[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
	if (month < 3)
		year--;
	return (year + year / 4 - year / 100 + year / 400 + offsets[month - 1] + day) % 7;
}

static BOOL fields_are_valid(const SYSTEMTIME *date)
{
	if (date->wMonth < 1 || date->wMonth > 12 || date->wDay < 1)
		return FALSE;
	if (date->wDay > month_length(date->wYear, date->wMonth))
		return FALSE;
	return date->wHour < 24 && date->wMinute < 60 && date->wSecond < 60 && date->wMilliseconds < 1000;
}

static int compare_times(const SYSTEMTIME *one, const SYSTEMTIME *two)
{
	const WORD first[] = { one->wYear, one->wMonth, one->wDay, one->wHour, one->wMinute, one->wSecond, one->wMilliseconds };
	const WORD second[] = { two->wYear, two->wMonth, two->wDay, two->wHour, two->wMinute, two->wSecond, two->wMilliseconds };
	int index;
	for (index = 0; index < 7; index++) {
		if (first[index] != second[index])
			return first[index] < second[index] ? -1 : 1;
	}
	return 0;
}

static BOOL within_limits(const picker_state *state, const SYSTEMTIME *date)
{
	if ((state->limit_flags & GDTR_MIN) && compare_times(date, &state->limits[0]) < 0)
		return FALSE;
	if ((state->limit_flags & GDTR_MAX) && compare_times(date, &state->limits[1]) > 0)
		return FALSE;
	return TRUE;
}

/* Notifications */

static LRESULT notify_parent(const picker_state *state, NMHDR *header, UINT code)
{
	header->hwndFrom = state->window;
	header->idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	header->code = code;
	return SendMessageW(state->parent, WM_NOTIFY, header->idFrom, (LPARAM)header);
}

static void notify_plain(const picker_state *state, UINT code)
{
	NMHDR header;
	notify_parent(state, &header, code);
}

static void notify_change(const picker_state *state)
{
	NMDATETIMECHANGE info;
	ZeroMemory(&info, sizeof(info));
	info.dwFlags = state->has_date ? GDT_VALID : GDT_NONE;
	if (state->has_date)
		info.st = state->date;
	notify_parent(state, &info.nmhdr, DTN_DATETIMECHANGE);
}

/* A callback piece takes its text from the parent, in the parent's own character set. */
static void ask_callback_text(const picker_state *state, const WCHAR *token, WCHAR *text, int size)
{
	text[0] = 0;
	if (state->narrow_parent) {
		NMDATETIMEFORMATA info;
		char narrow[16];
		ZeroMemory(&info, sizeof(info));
		WideCharToMultiByte(CP_ACP, 0, token, -1, narrow, sizeof(narrow), NULL, NULL);
		info.pszFormat = narrow;
		info.st = state->date;
		info.pszDisplay = info.szDisplay;
		notify_parent(state, &info.nmhdr, DTN_FORMATA);
		if (info.pszDisplay)
			MultiByteToWideChar(CP_ACP, 0, info.pszDisplay, -1, text, size);
	}
	else {
		NMDATETIMEFORMATW info;
		ZeroMemory(&info, sizeof(info));
		info.pszFormat = token;
		info.st = state->date;
		info.pszDisplay = info.szDisplay;
		notify_parent(state, &info.nmhdr, DTN_FORMATW);
		if (info.pszDisplay)
			lstrcpynW(text, info.pszDisplay, size);
	}
}

static SIZE ask_callback_width(const picker_state *state, const WCHAR *token)
{
	SIZE result = { 0, 0 };
	if (state->narrow_parent) {
		NMDATETIMEFORMATQUERYA info;
		char narrow[16];
		ZeroMemory(&info, sizeof(info));
		WideCharToMultiByte(CP_ACP, 0, token, -1, narrow, sizeof(narrow), NULL, NULL);
		info.pszFormat = narrow;
		notify_parent(state, &info.nmhdr, DTN_FORMATQUERYA);
		result = info.szMax;
	}
	else {
		NMDATETIMEFORMATQUERYW info;
		ZeroMemory(&info, sizeof(info));
		info.pszFormat = token;
		notify_parent(state, &info.nmhdr, DTN_FORMATQUERYW);
		result = info.szMax;
	}
	return result;
}

/* Format */

static void default_format(const picker_state *state, WCHAR *format, int size)
{
	DWORD style = style_of(state);
	LCTYPE which = LOCALE_SSHORTDATE;
	if ((style & 0x0C) == 0x04)
		which = LOCALE_SLONGDATE;
	else if (!(style & 0x04) && (style & 0x08))
		which = LOCALE_STIMEFORMAT;
	if (!GetLocaleInfoW(LOCALE_USER_DEFAULT, which, format, size))
		lstrcpynW(format, L"M/d/yyyy", size);
	if ((style & 0x0C) == 0x0C) {
		/* The century form is the short date with its year written out. */
		WCHAR wide[128];
		int from, to = 0, length = lstrlenW(format);
		for (from = 0; from < length && to < 124; from++) {
			if (format[from] == 'y') {
				while (from + 1 < length && format[from + 1] == 'y')
					from++;
				wide[to++] = 'y';
				wide[to++] = 'y';
				wide[to++] = 'y';
				wide[to++] = 'y';
			}
			else {
				wide[to++] = format[from];
			}
		}
		wide[to] = 0;
		lstrcpynW(format, wide, size);
	}
}

static int longest_run(WCHAR letter)
{
	switch (letter) {
	case 'd':
	case 'M':
		return 4;
	case 'y':
		return 5;
	case 'X':
		return 15;
	default:
		return 2;
	}
}

static BOOL is_format_letter(WCHAR letter)
{
	switch (letter) {
	case 'd': case 'h': case 'H': case 'm': case 'M': case 's': case 't': case 'y': case 'X': case 'g':
		return TRUE;
	default:
		return FALSE;
	}
}

static enum piece_kind kind_of(WCHAR letter)
{
	switch (letter) {
	case 'd':
		return PIECE_DAY;
	case 'h':
		return PIECE_HOUR12;
	case 'H':
		return PIECE_HOUR24;
	case 'm':
		return PIECE_MINUTE;
	case 's':
		return PIECE_SECOND;
	case 'M':
		return PIECE_MONTH;
	case 't':
		return PIECE_AMPM;
	case 'y':
		return PIECE_YEAR;
	case 'X':
		return PIECE_CALLBACK;
	default:
		return PIECE_ERA;
	}
}

static date_piece *new_piece(picker_state *state, enum piece_kind kind)
{
	date_piece *piece;
	if (state->count == MAX_PIECES)
		return NULL;
	piece = &state->pieces[state->count++];
	ZeroMemory(piece, sizeof(*piece));
	piece->kind = kind;
	return piece;
}

static void add_literal(picker_state *state, WCHAR letter)
{
	date_piece *piece = state->count ? &state->pieces[state->count - 1] : NULL;
	if (!piece || piece->kind != PIECE_LITERAL || piece->length == PIECE_TEXT - 1)
		piece = new_piece(state, PIECE_LITERAL);
	if (piece)
		piece->text[piece->length++] = letter;
}

static void parse_format(picker_state *state)
{
	WCHAR fallback[128];
	const WCHAR *format = state->format;
	int at = 0;

	if (!format) {
		default_format(state, fallback, 128);
		format = fallback;
	}
	state->count = 0;
	while (format[at]) {
		WCHAR letter = format[at];
		if (letter == '\'') {
			at++;
			if (format[at] == '\'') {
				add_literal(state, '\'');
				at++;
				continue;
			}
			while (format[at]) {
				if (format[at] == '\'') {
					if (format[at + 1] != '\'')
						break;
					at++;
				}
				add_literal(state, format[at++]);
			}
			if (format[at])
				at++;
		}
		else if (is_format_letter(letter)) {
			int run = 0, limit = longest_run(letter);
			date_piece *piece;
			while (format[at + run] == letter && run < limit)
				run++;
			piece = new_piece(state, kind_of(letter));
			if (piece) {
				if (letter == 'd' && run > 2)
					piece->kind = PIECE_WEEKDAY;
				piece->length = run;
				lstrcpynW(piece->token, format + at, run + 1);
			}
			at += run;
		}
		else {
			add_literal(state, letter);
			at++;
		}
	}
}

static BOOL piece_is_editable(const date_piece *piece)
{
	return piece->kind != PIECE_LITERAL && piece->kind != PIECE_ERA;
}

/* Text */

static void locale_text(LCTYPE which, WCHAR *text)
{
	if (!GetLocaleInfoW(LOCALE_USER_DEFAULT, which, text, PIECE_TEXT))
		text[0] = 0;
}

static void write_number(WCHAR *text, int value, int digits)
{
	if (digits >= 4)
		wsprintfW(text, L"%04d", value);
	else if (digits == 2)
		wsprintfW(text, L"%02d", value);
	else
		wsprintfW(text, L"%d", value);
}

static void render_piece(const picker_state *state, date_piece *piece)
{
	const SYSTEMTIME *date = &state->date;
	int hour12 = date->wHour % 12 ? date->wHour % 12 : 12;

	switch (piece->kind) {
	case PIECE_LITERAL:
		break;
	case PIECE_ERA:
		if (!GetDateFormatW(LOCALE_USER_DEFAULT, 0, date, L"gg", piece->text, PIECE_TEXT))
			piece->text[0] = 0;
		break;
	case PIECE_DAY:
		write_number(piece->text, date->wDay, piece->length);
		break;
	case PIECE_WEEKDAY:
		/* The locale tables start on Monday and the date on Sunday. */
		locale_text((piece->length == 3 ? LOCALE_SABBREVDAYNAME1 : LOCALE_SDAYNAME1) + (date->wDayOfWeek + 6) % 7, piece->text);
		break;
	case PIECE_HOUR12:
		write_number(piece->text, hour12, piece->length);
		break;
	case PIECE_HOUR24:
		write_number(piece->text, date->wHour, piece->length);
		break;
	case PIECE_MINUTE:
		write_number(piece->text, date->wMinute, piece->length);
		break;
	case PIECE_SECOND:
		write_number(piece->text, date->wSecond, piece->length);
		break;
	case PIECE_MONTH:
		if (piece->length <= 2)
			write_number(piece->text, date->wMonth, piece->length);
		else
			locale_text((piece->length == 3 ? LOCALE_SABBREVMONTHNAME1 : LOCALE_SMONTHNAME1) + date->wMonth - 1, piece->text);
		break;
	case PIECE_AMPM:
		locale_text(date->wHour < 12 ? LOCALE_S1159 : LOCALE_S2359, piece->text);
		if (piece->length == 1 && piece->text[0])
			piece->text[1] = 0;
		break;
	case PIECE_YEAR:
		if (piece->length == 1)
			write_number(piece->text, date->wYear % 10, 1);
		else if (piece->length == 2)
			write_number(piece->text, date->wYear % 100, 2);
		else
			write_number(piece->text, date->wYear, 4);
		break;
	case PIECE_CALLBACK:
		ask_callback_text(state, piece->token, piece->text, PIECE_TEXT);
		break;
	}
}

static HFONT font_of(const picker_state *state)
{
	return state->font ? state->font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

/* Places every piece and the check and drop boxes, left to right. */
static void lay_out(picker_state *state)
{
	RECT client;
	HDC dc = GetDC(state->window);
	HGDIOBJ old = SelectObject(dc, font_of(state));
	TEXTMETRICW metrics;
	int x = TEXT_INSET, index, total = 0;
	DWORD style = style_of(state);

	GetClientRect(state->window, &client);
	GetTextMetricsW(dc, &metrics);
	SetRectEmpty(&state->check);
	SetRectEmpty(&state->button);
	if (style & DTS_SHOWNONE) {
		int top = (client.bottom - CHECK_SIZE) / 2;
		SetRect(&state->check, TEXT_INSET, top, TEXT_INSET + CHECK_SIZE, top + CHECK_SIZE);
		x = state->check.right + TEXT_INSET;
	}
	if (!(style & DTS_UPDOWN))
		SetRect(&state->button, client.right - BUTTON_WIDTH, 0, client.right, client.bottom);
	for (index = 0; index < state->count; index++) {
		date_piece *piece = &state->pieces[index];
		SIZE size = { 0, 0 };
		render_piece(state, piece);
		GetTextExtentPoint32W(dc, piece->text, lstrlenW(piece->text), &size);
		if (piece->kind == PIECE_CALLBACK) {
			SIZE wanted = ask_callback_width(state, piece->token);
			if (wanted.cx > size.cx)
				size.cx = wanted.cx;
		}
		SetRect(&piece->area, x, (client.bottom - metrics.tmHeight) / 2, x + size.cx, (client.bottom + metrics.tmHeight) / 2);
		x += size.cx;
		total += size.cx;
	}
	if (style & DTS_RIGHTALIGN) {
		int room = (state->spin || !IsRectEmpty(&state->button) ? client.right - BUTTON_WIDTH : client.right) - x - TEXT_INSET;
		if (room > 0) {
			for (index = 0; index < state->count; index++)
				OffsetRect(&state->pieces[index].area, room, 0);
		}
	}
	SelectObject(dc, old);
	ReleaseDC(state->window, dc);
}

static void refresh(picker_state *state)
{
	lay_out(state);
	InvalidateRect(state->window, NULL, FALSE);
}

static int whole_text(picker_state *state, WCHAR *text, int size)
{
	int index, length = 0;
	lay_out(state);
	for (index = 0; index < state->count; index++) {
		const WCHAR *part = state->pieces[index].text;
		while (*part) {
			if (text && length < size - 1)
				text[length] = *part;
			length++;
			part++;
		}
	}
	if (text && size > 0)
		text[length < size ? length : size - 1] = 0;
	return text && length >= size ? size - 1 : length;
}

/* Painting */

static void draw(picker_state *state, HDC dc)
{
	RECT client;
	int index;
	BOOL enabled = IsWindowEnabled(state->window);
	DWORD style = style_of(state);
	HGDIOBJ old = SelectObject(dc, font_of(state));
	COLORREF normal = GetSysColor(enabled && state->has_date ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT);

	GetClientRect(state->window, &client);
	FillRect(dc, &client, GetSysColorBrush(enabled ? COLOR_WINDOW : COLOR_BTNFACE));
	SetBkMode(dc, TRANSPARENT);
	if (style & DTS_SHOWNONE) {
		UINT flags = DFCS_BUTTONCHECK | (state->has_date ? DFCS_CHECKED : 0) | (enabled ? 0 : DFCS_INACTIVE);
		RECT box = state->check;
		DrawFrameControl(dc, &box, DFC_BUTTON, flags);
	}
	for (index = 0; index < state->count; index++) {
		date_piece *piece = &state->pieces[index];
		RECT area = piece->area;
		if (index == state->current && state->focused && state->has_date && !state->popup) {
			FillRect(dc, &area, GetSysColorBrush(COLOR_HIGHLIGHT));
			SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
		}
		else {
			SetTextColor(dc, normal);
		}
		DrawTextW(dc, piece->text, -1, &area, DT_SINGLELINE | DT_NOPREFIX | DT_LEFT | DT_TOP);
	}
	if (!IsRectEmpty(&state->button)) {
		RECT box = state->button;
		DrawFrameControl(dc, &box, DFC_SCROLL, DFCS_SCROLLDOWN | (state->popup ? DFCS_PUSHED : 0) | (enabled ? 0 : DFCS_INACTIVE));
	}
	SelectObject(dc, old);
}

static void paint(picker_state *state, HDC given)
{
	PAINTSTRUCT info;
	RECT client;
	HDC dc = given ? given : BeginPaint(state->window, &info);
	if (dc) {
		GetClientRect(state->window, &client);
		if (!given && client.right > 0 && client.bottom > 0) {
			HDC memory = CreateCompatibleDC(dc);
			HBITMAP bitmap = memory ? CreateCompatibleBitmap(dc, client.right, client.bottom) : NULL;
			if (bitmap) {
				HGDIOBJ old = SelectObject(memory, bitmap);
				draw(state, memory);
				BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
				SelectObject(memory, old);
				DeleteObject(bitmap);
			}
			else {
				draw(state, dc);
			}
			if (memory)
				DeleteDC(memory);
		}
		else {
			draw(state, dc);
		}
	}
	if (!given)
		EndPaint(state->window, &info);
}

/* Changing the date */

static void stamp_weekday(SYSTEMTIME *date)
{
	date->wDayOfWeek = (WORD)weekday_of(date->wYear, date->wMonth, date->wDay);
}

/* Applies a whole new date when the limits allow it. */
static BOOL try_date(picker_state *state, const SYSTEMTIME *date)
{
	SYSTEMTIME value = *date;
	if (!fields_are_valid(&value) || value.wYear < FIRST_VALID_YEAR || !within_limits(state, &value))
		return FALSE;
	stamp_weekday(&value);
	if (!compare_times(&value, &state->date))
		return FALSE;
	state->date = value;
	notify_change(state);
	refresh(state);
	return TRUE;
}

static int wrapped(int value, int low, int high)
{
	int span = high - low + 1;
	return low + ((value - low) % span + span) % span;
}

static void step_piece(picker_state *state, int delta)
{
	const date_piece *piece;
	SYSTEMTIME moved = state->date;
	if (state->current < 0 || !state->has_date)
		return;
	piece = &state->pieces[state->current];
	switch (piece->kind) {
	case PIECE_DAY:
	case PIECE_WEEKDAY:
		moved.wDay = (WORD)wrapped(moved.wDay + delta, 1, month_length(moved.wYear, moved.wMonth));
		break;
	case PIECE_MONTH:
		moved.wMonth = (WORD)wrapped(moved.wMonth + delta, 1, 12);
		break;
	case PIECE_YEAR:
		moved.wYear = (WORD)(moved.wYear + delta);
		break;
	case PIECE_HOUR12:
	case PIECE_HOUR24:
		moved.wHour = (WORD)wrapped(moved.wHour + delta, 0, 23);
		break;
	case PIECE_MINUTE:
		moved.wMinute = (WORD)wrapped(moved.wMinute + delta, 0, 59);
		break;
	case PIECE_SECOND:
		moved.wSecond = (WORD)wrapped(moved.wSecond + delta, 0, 59);
		break;
	case PIECE_AMPM:
		moved.wHour = (WORD)((moved.wHour + 12) % 24);
		break;
	default:
		return;
	}
	if (moved.wMonth != state->date.wMonth || moved.wYear != state->date.wYear) {
		int length = month_length(moved.wYear, moved.wMonth);
		if (moved.wDay > length)
			moved.wDay = (WORD)length;
	}
	try_date(state, &moved);
}

static void end_piece(picker_state *state, BOOL last)
{
	const date_piece *piece;
	SYSTEMTIME moved = state->date;
	if (state->current < 0 || !state->has_date)
		return;
	piece = &state->pieces[state->current];
	switch (piece->kind) {
	case PIECE_DAY:
	case PIECE_WEEKDAY:
		moved.wDay = (WORD)(last ? month_length(moved.wYear, moved.wMonth) : 1);
		break;
	case PIECE_MONTH:
		moved.wMonth = (WORD)(last ? 12 : 1);
		break;
	case PIECE_HOUR12:
	case PIECE_HOUR24:
		moved.wHour = (WORD)(last ? 23 : 0);
		break;
	case PIECE_MINUTE:
		moved.wMinute = (WORD)(last ? 59 : 0);
		break;
	case PIECE_SECOND:
		moved.wSecond = (WORD)(last ? 59 : 0);
		break;
	default:
		return;
	}
	try_date(state, &moved);
}

static void change_current(picker_state *state, int direction)
{
	int index = state->current;
	state->typed = state->typed_digits = 0;
	while (state->count) {
		index += direction;
		if (index < 0 || index >= state->count)
			return;
		if (piece_is_editable(&state->pieces[index])) {
			state->current = index;
			InvalidateRect(state->window, NULL, FALSE);
			return;
		}
	}
}

static void first_editable(picker_state *state)
{
	int index;
	state->current = -1;
	for (index = 0; index < state->count; index++) {
		if (piece_is_editable(&state->pieces[index])) {
			state->current = index;
			return;
		}
	}
}

/* Digits build up a value for the selected piece, and a piece that can take no more moves on to the next one. */
static void type_digit(picker_state *state, int digit)
{
	const date_piece *piece;
	SYSTEMTIME moved = state->date;
	int value, room = 2, top = 0;
	if (state->current < 0 || !state->has_date)
		return;
	piece = &state->pieces[state->current];
	value = state->typed * 10 + digit;
	switch (piece->kind) {
	case PIECE_DAY:
		top = month_length(moved.wYear, moved.wMonth);
		if (value >= 1 && value <= top)
			moved.wDay = (WORD)value;
		break;
	case PIECE_MONTH:
		top = 12;
		if (value >= 1 && value <= 12) {
			int length = month_length(moved.wYear, value);
			moved.wMonth = (WORD)value;
			if (moved.wDay > length)
				moved.wDay = (WORD)length;
		}
		break;
	case PIECE_HOUR12:
		top = 12;
		if (value >= 1 && value <= 12)
			moved.wHour = (WORD)(value % 12 + (moved.wHour >= 12 ? 12 : 0));
		break;
	case PIECE_HOUR24:
		top = 23;
		if (value <= 23)
			moved.wHour = (WORD)value;
		break;
	case PIECE_MINUTE:
		top = 59;
		if (value <= 59)
			moved.wMinute = (WORD)value;
		break;
	case PIECE_SECOND:
		top = 59;
		if (value <= 59)
			moved.wSecond = (WORD)value;
		break;
	case PIECE_YEAR:
		room = piece->length <= 2 ? 2 : 4;
		top = room == 2 ? 99 : 9999;
		if (room == 2)
			moved.wYear = (WORD)(moved.wYear / 100 * 100 + value);
		else if (value >= FIRST_VALID_YEAR)
			moved.wYear = (WORD)value;
		break;
	default:
		return;
	}
	state->typed = value;
	state->typed_digits++;
	try_date(state, &moved);
	if (state->typed_digits >= room || state->typed * 10 > top)
		change_current(state, 1);
}

static void toggle_check(picker_state *state)
{
	state->has_date = !state->has_date;
	notify_change(state);
	refresh(state);
}

/* Dropping the calendar */

static void close_popup(picker_state *state)
{
	HWND popup = state->popup;
	if (!popup)
		return;
	state->popup = NULL;
	DestroyWindow(popup);
	notify_plain(state, DTN_CLOSEUP);
	InvalidateRect(state->window, NULL, FALSE);
	SetFocus(state->window);
}

static LRESULT CALLBACK popup_filter(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data)
{
	picker_state *state = (picker_state *)data;
	switch (message) {
	case WM_KILLFOCUS:
		PostMessageW(state->window, CLOSE_POPUP, 0, 0);
		break;
	case WM_KEYDOWN:
		if (wparam == VK_ESCAPE || wparam == VK_F4) {
			PostMessageW(state->window, CLOSE_POPUP, 0, 0);
			return 0;
		}
		break;
	case WM_NCDESTROY:
		RemoveWindowSubclass(window, popup_filter, id);
		break;
	}
	return DefSubclassProc(window, message, wparam, lparam);
}

static void open_popup(picker_state *state)
{
	RECT frame, needed;
	int index, width, height, x;
	SYSTEMTIME today;
	HWND popup;

	if (state->popup)
		return;
	popup = CreateWindowExW(0, MONTHCAL_CLASSW, L"", WS_POPUP | WS_BORDER | state->popup_style, 0, 0, 200, 200, state->window, NULL, (HINSTANCE)GetWindowLongPtrW(state->window, GWLP_HINSTANCE), NULL);
	if (!popup)
		return;
	if (state->popup_font)
		SendMessageW(popup, WM_SETFONT, (WPARAM)state->popup_font, 0);
	for (index = 0; index < 6; index++)
		SendMessageW(popup, MCM_SETCOLOR, index, state->popup_colors[index]);
	if (state->limit_flags)
		SendMessageW(popup, MCM_SETRANGE, state->limit_flags, (LPARAM)state->limits);
	GetLocalTime(&today);
	SendMessageW(popup, MCM_SETCURSEL, 0, (LPARAM)(state->has_date ? &state->date : &today));
	SendMessageW(popup, MCM_GETMINREQRECT, 0, (LPARAM)&needed);
	GetWindowRect(state->window, &frame);
	width = needed.right + 2;
	height = needed.bottom + 2;
	x = (style_of(state) & DTS_RIGHTALIGN) ? frame.right - width : frame.left;
	state->popup = popup;
	SetWindowSubclass(popup, popup_filter, POPUP_SUBCLASS, (DWORD_PTR)state);
	notify_plain(state, DTN_DROPDOWN);
	SetWindowPos(popup, HWND_TOPMOST, x, frame.bottom, width, height, SWP_SHOWWINDOW);
	SetFocus(popup);
	InvalidateRect(state->window, NULL, FALSE);
}

/* Free typing, for parents that parse their own dates */

static void close_editor(picker_state *state, BOOL keep)
{
	HWND editor = state->editor;
	WCHAR text[128];
	if (!editor)
		return;
	state->editor = NULL;
	text[0] = 0;
	GetWindowTextW(editor, text, 128);
	DestroyWindow(editor);
	if (keep) {
		SYSTEMTIME before = state->date;
		DWORD flags;
		if (state->narrow_parent) {
			NMDATETIMESTRINGA info;
			char narrow[256];
			ZeroMemory(&info, sizeof(info));
			WideCharToMultiByte(CP_ACP, 0, text, -1, narrow, sizeof(narrow), NULL, NULL);
			info.pszUserString = narrow;
			info.st = before;
			info.dwFlags = GDT_VALID;
			notify_parent(state, &info.nmhdr, DTN_USERSTRINGA);
			flags = info.dwFlags;
			if (flags == GDT_VALID)
				before = info.st;
		}
		else {
			NMDATETIMESTRINGW info;
			ZeroMemory(&info, sizeof(info));
			info.pszUserString = text;
			info.st = before;
			info.dwFlags = GDT_VALID;
			notify_parent(state, &info.nmhdr, DTN_USERSTRINGW);
			flags = info.dwFlags;
			if (flags == GDT_VALID)
				before = info.st;
		}
		if (flags == GDT_VALID)
			try_date(state, &before);
		else if (flags == GDT_NONE && (style_of(state) & DTS_SHOWNONE) && state->has_date)
			toggle_check(state);
	}
	SetFocus(state->window);
}

static LRESULT CALLBACK editor_filter(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data)
{
	picker_state *state = (picker_state *)data;
	switch (message) {
	case WM_GETDLGCODE:
		return DLGC_WANTALLKEYS;
	case WM_KEYDOWN:
		if (wparam == VK_RETURN || wparam == VK_ESCAPE) {
			close_editor(state, wparam == VK_RETURN);
			return 0;
		}
		break;
	case WM_KILLFOCUS:
		if (state->editor == window)
			close_editor(state, TRUE);
		return 0;
	case WM_NCDESTROY:
		RemoveWindowSubclass(window, editor_filter, id);
		break;
	}
	return DefSubclassProc(window, message, wparam, lparam);
}

static void open_editor(picker_state *state)
{
	RECT client;
	WCHAR text[128];
	int left = state->check.right ? state->check.right + TEXT_INSET : TEXT_INSET;

	if (state->editor || !(style_of(state) & DTS_APPCANPARSE))
		return;
	GetClientRect(state->window, &client);
	whole_text(state, text, 128);
	state->editor = CreateWindowExW(0, L"EDIT", text, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, left, 0, client.right - left - BUTTON_WIDTH, client.bottom, state->window, NULL, (HINSTANCE)GetWindowLongPtrW(state->window, GWLP_HINSTANCE), NULL);
	if (!state->editor)
		return;
	SendMessageW(state->editor, WM_SETFONT, (WPARAM)font_of(state), 0);
	SetWindowSubclass(state->editor, editor_filter, 1, (DWORD_PTR)state);
	SendMessageW(state->editor, EM_SETSEL, 0, -1);
	SetFocus(state->editor);
}

/* Input */

static void callback_key(picker_state *state, const date_piece *piece, int key)
{
	if (state->narrow_parent) {
		NMDATETIMEWMKEYDOWNA info;
		char narrow[16];
		ZeroMemory(&info, sizeof(info));
		WideCharToMultiByte(CP_ACP, 0, piece->token, -1, narrow, sizeof(narrow), NULL, NULL);
		info.nVirtKey = key;
		info.pszFormat = narrow;
		info.st = state->date;
		notify_parent(state, &info.nmhdr, DTN_WMKEYDOWNA);
		try_date(state, &info.st);
	}
	else {
		NMDATETIMEWMKEYDOWNW info;
		ZeroMemory(&info, sizeof(info));
		info.nVirtKey = key;
		info.pszFormat = piece->token;
		info.st = state->date;
		notify_parent(state, &info.nmhdr, DTN_WMKEYDOWNW);
		try_date(state, &info.st);
	}
}

static void key_down(picker_state *state, int key)
{
	if (state->current >= 0 && state->pieces[state->current].kind == PIECE_CALLBACK && key != VK_LEFT && key != VK_RIGHT && key != VK_F4) {
		callback_key(state, &state->pieces[state->current], key);
		return;
	}
	switch (key) {
	case VK_LEFT:
		change_current(state, -1);
		break;
	case VK_RIGHT:
		change_current(state, 1);
		break;
	case VK_UP:
		state->typed = state->typed_digits = 0;
		step_piece(state, 1);
		break;
	case VK_DOWN:
		state->typed = state->typed_digits = 0;
		step_piece(state, -1);
		break;
	case VK_HOME:
		end_piece(state, FALSE);
		break;
	case VK_END:
		end_piece(state, TRUE);
		break;
	case VK_SPACE:
		if (style_of(state) & DTS_SHOWNONE)
			toggle_check(state);
		break;
	case VK_F2:
		open_editor(state);
		break;
	case VK_F4:
		if (!(style_of(state) & DTS_UPDOWN))
			open_popup(state);
		break;
	}
}

static void mouse_down(picker_state *state, POINT at)
{
	int index;
	BOOL had_focus = state->focused;
	SetFocus(state->window);
	if (IsWindowEnabled(state->window) && PtInRect(&state->check, at)) {
		toggle_check(state);
		return;
	}
	if (PtInRect(&state->button, at)) {
		open_popup(state);
		return;
	}
	for (index = 0; index < state->count; index++) {
		if (PtInRect(&state->pieces[index].area, at) && piece_is_editable(&state->pieces[index])) {
			if (index == state->current && had_focus && (style_of(state) & DTS_APPCANPARSE)) {
				open_editor(state);
				return;
			}
			state->current = index;
			state->typed = state->typed_digits = 0;
			InvalidateRect(state->window, NULL, FALSE);
			return;
		}
	}
}

static void place_spin(picker_state *state)
{
	RECT client;
	if (!state->spin)
		return;
	GetClientRect(state->window, &client);
	SetWindowPos(state->spin, NULL, client.right - BUTTON_WIDTH, 0, BUTTON_WIDTH, client.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT set_time(picker_state *state, DWORD flags, const SYSTEMTIME *date)
{
	SYSTEMTIME value;
	if (flags == GDT_NONE) {
		if (!(style_of(state) & DTS_SHOWNONE))
			return FALSE;
		state->has_date = FALSE;
		refresh(state);
		return TRUE;
	}
	if (flags != GDT_VALID || !date || !fields_are_valid(date))
		return FALSE;
	/* A date that is well formed but out of reach is accepted and ignored. */
	if (date->wYear < FIRST_VALID_YEAR || !within_limits(state, date))
		return TRUE;
	value = *date;
	stamp_weekday(&value);
	state->date = value;
	state->has_date = TRUE;
	refresh(state);
	return TRUE;
}

static LRESULT set_limits(picker_state *state, DWORD flags, const SYSTEMTIME *dates)
{
	SYSTEMTIME first, second;
	if (flags) {
		if (!dates)
			return FALSE;
		if ((flags & GDTR_MIN) && (!fields_are_valid(&dates[0]) || dates[0].wYear < FIRST_VALID_YEAR))
			return FALSE;
		if ((flags & GDTR_MAX) && (!fields_are_valid(&dates[1]) || dates[1].wYear < FIRST_VALID_YEAR))
			return FALSE;
	}
	ZeroMemory(&first, sizeof(first));
	ZeroMemory(&second, sizeof(second));
	if (flags & GDTR_MIN)
		first = dates[0];
	if (flags & GDTR_MAX)
		second = dates[1];
	if ((flags & GDTR_MIN) && (flags & GDTR_MAX) && compare_times(&first, &second) > 0) {
		SYSTEMTIME swap = first;
		first = second;
		second = swap;
	}
	state->limits[0] = first;
	state->limits[1] = second;
	state->limit_flags = flags & (GDTR_MIN | GDTR_MAX);
	return TRUE;
}

static void set_format(picker_state *state, const WCHAR *format)
{
	if (state->format)
		HeapFree(GetProcessHeap(), 0, state->format);
	state->format = NULL;
	if (format) {
		SIZE_T bytes = (lstrlenW(format) + 1) * sizeof(WCHAR);
		state->format = HeapAlloc(GetProcessHeap(), 0, bytes);
		if (state->format)
			CopyMemory(state->format, format, bytes);
	}
	parse_format(state);
	first_editable(state);
	refresh(state);
}

static LRESULT ideal_size(picker_state *state, SIZE *size)
{
	HDC dc;
	HGDIOBJ old;
	TEXTMETRICW metrics;
	WCHAR text[256];
	SIZE extent = { 0, 0 };
	RECT frame = { 0, 0, 0, 0 };
	int length;

	if (!size)
		return FALSE;
	length = whole_text(state, text, 256);
	dc = GetDC(state->window);
	old = SelectObject(dc, font_of(state));
	GetTextMetricsW(dc, &metrics);
	GetTextExtentPoint32W(dc, text, length, &extent);
	SelectObject(dc, old);
	ReleaseDC(state->window, dc);
	AdjustWindowRectEx(&frame, (DWORD)GetWindowLongPtrW(state->window, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(state->window, GWL_EXSTYLE));
	size->cx = extent.cx + 2 * TEXT_INSET + BUTTON_WIDTH + (frame.right - frame.left) + ((style_of(state) & DTS_SHOWNONE) ? CHECK_SIZE + TEXT_INSET : 0);
	size->cy = metrics.tmHeight + 4 + (frame.bottom - frame.top);
	return TRUE;
}

static LRESULT CALLBACK picker_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	picker_state *state = state_of(window);

	if (message == WM_NCCREATE) {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
		if (!state)
			return FALSE;
		state->window = window;
		state->parent = create->hwndParent;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		SetWindowLongPtrW(window, GWL_EXSTYLE, create->dwExStyle | WS_EX_CLIENTEDGE);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	if (!state)
		return DefWindowProcW(window, message, wparam, lparam);

	switch (message) {
	case WM_CREATE: {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state->narrow_parent = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_ANSI;
		GetLocalTime(&state->date);
		state->has_date = TRUE;
		state->popup_colors[0] = GetSysColor(COLOR_WINDOW);
		state->popup_colors[1] = GetSysColor(COLOR_WINDOWTEXT);
		state->popup_colors[2] = GetSysColor(COLOR_ACTIVECAPTION);
		state->popup_colors[3] = GetSysColor(COLOR_WINDOW);
		state->popup_colors[4] = GetSysColor(COLOR_WINDOW);
		state->popup_colors[5] = GetSysColor(COLOR_GRAYTEXT);
		if (create->style & DTS_UPDOWN) {
			state->spin = CreateWindowExW(0, UPDOWN_CLASSW, NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, NULL, create->hInstance, NULL);
			if (state->spin) {
				SendMessageW(state->spin, UDM_SETRANGE32, 0, 100);
				SendMessageW(state->spin, UDM_SETPOS32, 0, 50);
			}
			place_spin(state);
		}
		parse_format(state);
		first_editable(state);
		lay_out(state);
		return 0;
	}
	case WM_NCDESTROY:
		SetWindowLongPtrW(window, 0, 0);
		if (state->format)
			HeapFree(GetProcessHeap(), 0, state->format);
		HeapFree(GetProcessHeap(), 0, state);
		break;
	case WM_DESTROY:
		if (state->popup) {
			HWND popup = state->popup;
			state->popup = NULL;
			DestroyWindow(popup);
		}
		break;
	case WM_STYLECHANGING:
		if (wparam == GWL_STYLE) {
			STYLESTRUCT *change = (STYLESTRUCT *)lparam;
			change->styleNew = (change->styleNew & ~(DWORD)DTS_SHOWNONE) | (change->styleOld & DTS_SHOWNONE);
		}
		return 0;
	case WM_STYLECHANGED:
		if (wparam == GWL_STYLE) {
			if (!state->format)
				parse_format(state);
			first_editable(state);
			refresh(state);
		}
		return 0;
	case WM_NOTIFYFORMAT:
		if (lparam == NF_REQUERY)
			state->narrow_parent = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_ANSI;
		return state->narrow_parent ? NFR_ANSI : NFR_UNICODE;
	case WM_NOTIFY: {
		const NMHDR *header = (const NMHDR *)lparam;
		if (header->hwndFrom == state->popup && header->code == MCN_SELECT) {
			const NMSELCHANGE *change = (const NMSELCHANGE *)lparam;
			SYSTEMTIME moved = state->date;
			moved.wYear = change->stSelStart.wYear;
			moved.wMonth = change->stSelStart.wMonth;
			moved.wDay = change->stSelStart.wDay;
			state->has_date = TRUE;
			try_date(state, &moved);
			PostMessageW(window, CLOSE_POPUP, 0, 0);
			return 0;
		}
		if (header->hwndFrom == state->spin && header->code == UDN_DELTAPOS) {
			const NMUPDOWN *step = (const NMUPDOWN *)lparam;
			state->typed = state->typed_digits = 0;
			step_piece(state, step->iDelta > 0 ? 1 : -1);
			return 1;
		}
		return 0;
	}
	case CLOSE_POPUP:
		close_popup(state);
		return 0;
	case WM_SIZE:
		place_spin(state);
		refresh(state);
		return 0;
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		lay_out(state);
		if (LOWORD(lparam))
			InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_ENABLE:
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
	case WM_PRINTCLIENT:
		paint(state, (HDC)wparam);
		return 0;
	case WM_GETDLGCODE:
		return DLGC_WANTARROWS | DLGC_WANTCHARS;
	case WM_SETFOCUS:
		state->focused = TRUE;
		notify_plain(state, NM_SETFOCUS);
		InvalidateRect(window, NULL, FALSE);
		return 0;
	case WM_KILLFOCUS:
		state->focused = FALSE;
		state->typed = state->typed_digits = 0;
		notify_plain(state, NM_KILLFOCUS);
		InvalidateRect(window, NULL, FALSE);
		return 0;
	case WM_LBUTTONDOWN: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		mouse_down(state, at);
		return 0;
	}
	case WM_KEYDOWN:
		key_down(state, (int)wparam);
		return 0;
	case WM_SYSKEYDOWN:
		if (wparam == VK_DOWN && !(style_of(state) & DTS_UPDOWN)) {
			open_popup(state);
			return 0;
		}
		break;
	case WM_CHAR:
		if (wparam >= '0' && wparam <= '9') {
			type_digit(state, (int)wparam - '0');
			return 0;
		}
		break;
	case WM_SETTEXT:
		return CB_ERR;
	case WM_GETTEXT:
		if (!wparam || !lparam)
			return 0;
		return whole_text(state, (WCHAR *)lparam, (int)wparam);
	case WM_GETTEXTLENGTH:
		return whole_text(state, NULL, 0);

	case DTM_GETSYSTEMTIME:
		if (!lparam)
			return GDT_ERROR;
		if (!state->has_date)
			return GDT_NONE;
		*(SYSTEMTIME *)lparam = state->date;
		return GDT_VALID;
	case DTM_SETSYSTEMTIME:
		return set_time(state, (DWORD)wparam, (const SYSTEMTIME *)lparam);
	case DTM_GETRANGE: {
		SYSTEMTIME *dates = (SYSTEMTIME *)lparam;
		if (dates) {
			dates[0] = state->limits[0];
			dates[1] = state->limits[1];
		}
		return state->limit_flags;
	}
	case DTM_SETRANGE:
		return set_limits(state, (DWORD)wparam, (const SYSTEMTIME *)lparam);
	case DTM_SETFORMATA: {
		WCHAR *wide = NULL;
		if (lparam) {
			int length = MultiByteToWideChar(CP_ACP, 0, (const char *)lparam, -1, NULL, 0);
			wide = HeapAlloc(GetProcessHeap(), 0, length * sizeof(WCHAR));
			if (!wide)
				return FALSE;
			MultiByteToWideChar(CP_ACP, 0, (const char *)lparam, -1, wide, length);
		}
		set_format(state, wide);
		if (wide)
			HeapFree(GetProcessHeap(), 0, wide);
		return TRUE;
	}
	case DTM_SETFORMATW:
		set_format(state, (const WCHAR *)lparam);
		return TRUE;
	case DTM_SETMCCOLOR: {
		COLORREF old;
		if (wparam >= 6)
			return -1;
		old = state->popup_colors[wparam];
		state->popup_colors[wparam] = (COLORREF)lparam;
		if (state->popup)
			SendMessageW(state->popup, MCM_SETCOLOR, wparam, lparam);
		return old;
	}
	case DTM_GETMCCOLOR:
		return wparam < 6 ? (LRESULT)state->popup_colors[wparam] : -1;
	case DTM_GETMONTHCAL:
		return (LRESULT)state->popup;
	case DTM_SETMCFONT:
		state->popup_font = (HFONT)wparam;
		if (state->popup)
			SendMessageW(state->popup, WM_SETFONT, wparam, lparam);
		return 0;
	case DTM_GETMCFONT:
		return (LRESULT)state->popup_font;
	case DTM_SETMCSTYLE: {
		DWORD old = state->popup_style;
		state->popup_style = (DWORD)lparam;
		return old;
	}
	case DTM_GETMCSTYLE:
		return state->popup_style;
	case DTM_CLOSEMONTHCAL:
		close_popup(state);
		return 0;
	case DTM_GETIDEALSIZE:
		return ideal_size(state, (SIZE *)lparam);
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void DATETIME_Register(void)
{
	WNDCLASSW info;

	ZeroMemory(&info, sizeof(info));
	info.style = CS_GLOBALCLASS;
	info.lpfnWndProc = picker_proc;
	info.cbWndExtra = sizeof(picker_state *);
	info.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	info.lpszClassName = DATETIMEPICK_CLASSW;
	RegisterClassW(&info);
}

void DATETIME_Unregister(void)
{
	UnregisterClassW(DATETIMEPICK_CLASSW, NULL);
}
