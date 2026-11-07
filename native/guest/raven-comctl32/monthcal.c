/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>

/* One calendar is ten rows tall: two for the title, one for the weekday names, six of days and one for the today line. The
 * calendars of a window are one grid centred in the client area, and every drawing or hit test starts from day_of_cell. */

#define TITLE_ROWS 2
#define HEAD_ROWS 1
#define DAY_ROWS 6
#define TODAY_ROWS 1
#define DAYS_PER_ROW 7
#define BUTTON_INSET 4
#define BUTTON_SIZE 14
#define DEFAULT_GAP 4
#define MAX_CALENDARS 12
#define DEFAULT_SPAN 7
#define REPEAT_TIMER 1
#define REPEAT_DELAY 350
#define FIRST_YEAR 1752
#define FIRST_MONTH 9
#define FIRST_DAY 14
#define LAST_YEAR 9999

enum { COLOR_BACK, COLOR_TEXT, COLOR_TITLE_BACK, COLOR_TITLE_TEXT, COLOR_MONTH_BACK, COLOR_TRAILING, COLOR_COUNT };

typedef struct {
	HWND window, parent;
	HFONT font, bold;
	SYSTEMTIME today, selection[2], limits[2], first;
	DWORD limit_flags;
	COLORREF colors[COLOR_COUNT];
	int first_day_value;
	BOOL first_day_set;
	int delta, span, day_width, day_height, week_width, gap;
	int columns, rows, origin_x, origin_y;
	MONTHDAYSTATE states[MAX_CALENDARS + 2];
	BOOL narrow_format, today_set, dragging;
	int press, repeat_step;
	SYSTEMTIME anchor;
	DWORD view_style;
} calendar_state;

typedef struct {
	int calendar, row, column;
	UINT hit;
	SYSTEMTIME day;
	RECT area;
} calendar_hit;

static calendar_state *state_of(HWND window)
{
	return (calendar_state *)GetWindowLongPtrW(window, 0);
}

static DWORD style_of(const calendar_state *state)
{
	return (DWORD)GetWindowLongPtrW(state->window, GWL_STYLE);
}

/* Dates */

static BOOL is_leap(int year)
{
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_in_month(int year, int month)
{
	static const BYTE lengths[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	/* The month of the calendar reform lost its days to the jump, and it is the first month a calendar can show. */
	if (year == FIRST_YEAR && month == FIRST_MONTH)
		return 19;
	if (month == 2 && is_leap(year))
		return 29;
	return lengths[month - 1];
}

static int day_of_week(int year, int month, int day)
{
	static const BYTE offsets[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
	if (month < 3)
		year--;
	return (year + year / 4 - year / 100 + year / 400 + offsets[month - 1] + day) % 7;
}

static BOOL date_is_valid(const SYSTEMTIME *date)
{
	if (date->wMonth < 1 || date->wMonth > 12 || date->wDay < 1 || date->wYear > LAST_YEAR)
		return FALSE;
	if (date->wDay > days_in_month(date->wYear, date->wMonth) && !(date->wYear == FIRST_YEAR && date->wMonth == FIRST_MONTH && date->wDay <= 30))
		return FALSE;
	if (date->wYear < FIRST_YEAR)
		return FALSE;
	if (date->wYear == FIRST_YEAR && (date->wMonth < FIRST_MONTH || (date->wMonth == FIRST_MONTH && date->wDay < FIRST_DAY)))
		return FALSE;
	return TRUE;
}

static BOOL time_is_valid(const SYSTEMTIME *date)
{
	return date->wHour < 24 && date->wMinute < 60 && date->wSecond < 60;
}

static int compare_dates(const SYSTEMTIME *one, const SYSTEMTIME *two)
{
	if (one->wYear != two->wYear)
		return one->wYear < two->wYear ? -1 : 1;
	if (one->wMonth != two->wMonth)
		return one->wMonth < two->wMonth ? -1 : 1;
	if (one->wDay != two->wDay)
		return one->wDay < two->wDay ? -1 : 1;
	return 0;
}

static int compare_moments(const SYSTEMTIME *one, const SYSTEMTIME *two)
{
	int order = compare_dates(one, two);
	if (order)
		return order;
	if (one->wHour != two->wHour)
		return one->wHour < two->wHour ? -1 : 1;
	if (one->wMinute != two->wMinute)
		return one->wMinute < two->wMinute ? -1 : 1;
	if (one->wSecond != two->wSecond)
		return one->wSecond < two->wSecond ? -1 : 1;
	return 0;
}

static void fix_weekday(SYSTEMTIME *date)
{
	date->wDayOfWeek = (WORD)day_of_week(date->wYear, date->wMonth, date->wDay);
}

static void add_months(SYSTEMTIME *date, int months)
{
	int total = date->wYear * 12 + (date->wMonth - 1) + months;
	int length;
	date->wYear = (WORD)(total / 12);
	date->wMonth = (WORD)(total % 12 + 1);
	length = days_in_month(date->wYear, date->wMonth);
	if (date->wDay > length)
		date->wDay = (WORD)length;
}

static void add_days(SYSTEMTIME *date, int days)
{
	int day = date->wDay + days;
	while (day < 1) {
		int total = date->wYear * 12 + (date->wMonth - 1) - 1;
		date->wYear = (WORD)(total / 12);
		date->wMonth = (WORD)(total % 12 + 1);
		day += days_in_month(date->wYear, date->wMonth);
	}
	while (day > days_in_month(date->wYear, date->wMonth)) {
		int total = date->wYear * 12 + (date->wMonth - 1) + 1;
		day -= days_in_month(date->wYear, date->wMonth);
		date->wYear = (WORD)(total / 12);
		date->wMonth = (WORD)(total % 12 + 1);
	}
	date->wDay = (WORD)day;
	fix_weekday(date);
}

static int days_between(const SYSTEMTIME *from, const SYSTEMTIME *to)
{
	SYSTEMTIME walk = *from;
	int days = 0;
	while (compare_dates(&walk, to) < 0 && days < 400) {
		add_days(&walk, 1);
		days++;
	}
	return days;
}

/* The first column's weekday, as the system numbers them with Sunday as 0. */
static int first_column_weekday(const calendar_state *state)
{
	int value = state->first_day_value;
	if (value < 0)
		value = (value % 7 + 7) % 7;
	return (value + 1) % 7;
}

static int locale_first_day(void)
{
	WCHAR text[4];
	if (GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, text, 4) && text[0] >= '0' && text[0] <= '6')
		return text[0] - '0';
	return 6;
}

/* The grid of a calendar always starts in the previous month, a whole week back if the first falls on the first column. */
static void first_cell_of(const calendar_state *state, int calendar, SYSTEMTIME *date)
{
	int lead;
	*date = state->first;
	add_months(date, calendar);
	date->wDay = 1;
	lead = (day_of_week(date->wYear, date->wMonth, 1) - first_column_weekday(state) + 7) % 7;
	if (lead == 0)
		lead = 7;
	add_days(date, -lead);
}

static void day_of_cell(const calendar_state *state, int calendar, int row, int column, SYSTEMTIME *date)
{
	first_cell_of(state, calendar, date);
	add_days(date, row * DAYS_PER_ROW + column);
}

static int shown_calendars(const calendar_state *state)
{
	return state->columns * state->rows;
}

/* Sizes */

static int today_rows(const calendar_state *state)
{
	return (style_of(state) & MCS_NOTODAY) ? 0 : TODAY_ROWS;
}

static int calendar_width(const calendar_state *state)
{
	return state->day_width * DAYS_PER_ROW + state->week_width;
}

static int calendar_height(const calendar_state *state)
{
	return state->day_height * (TITLE_ROWS + HEAD_ROWS + DAY_ROWS + today_rows(state));
}

static HFONT font_of(const calendar_state *state)
{
	return state->font ? state->font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static void remeasure(calendar_state *state)
{
	HDC dc = GetDC(state->window);
	HGDIOBJ old = SelectObject(dc, font_of(state));
	TEXTMETRICW metrics;
	LOGFONTW description;
	SIZE size;
	int index, widest;

	GetTextMetricsW(dc, &metrics);
	state->day_height = metrics.tmHeight + 2;
	GetTextExtentPoint32W(dc, L"00", 2, &size);
	widest = size.cx;
	for (index = 0; index < DAYS_PER_ROW; index++) {
		WCHAR name[32];
		if (GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SSHORTESTDAYNAME1 + index, name, 32) || GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SABBREVDAYNAME1 + index, name, 32)) {
			GetTextExtentPoint32W(dc, name, lstrlenW(name), &size);
			if (size.cx > widest)
				widest = size.cx;
		}
	}
	state->day_width = widest + 8;
	GetTextExtentPoint32W(dc, L"00", 2, &size);
	state->week_width = (style_of(state) & MCS_WEEKNUMBERS) ? size.cx + 8 : 0;
	SelectObject(dc, old);
	ReleaseDC(state->window, dc);

	if (state->bold)
		DeleteObject(state->bold);
	state->bold = NULL;
	if (GetObjectW(font_of(state), sizeof(description), &description)) {
		description.lfWeight = FW_BOLD;
		state->bold = CreateFontIndirectW(&description);
	}
}

static void arrange_grid(calendar_state *state)
{
	RECT client;
	int width = calendar_width(state), height = calendar_height(state), gap = state->gap;
	int columns, rows;
	GetClientRect(state->window, &client);
	columns = (client.right + gap) / (width + gap);
	rows = (client.bottom + gap) / (height + gap);
	if (columns < 1)
		columns = 1;
	if (rows < 1)
		rows = 1;
	while (columns * rows > MAX_CALENDARS) {
		if (columns >= rows)
			columns--;
		else
			rows--;
	}
	state->columns = columns;
	state->rows = rows;
	state->origin_x = (client.right - (columns * width + (columns - 1) * gap)) / 2;
	state->origin_y = (client.bottom - (rows * height + (rows - 1) * gap)) / 2;
	if (state->origin_x < 0)
		state->origin_x = 0;
	if (state->origin_y < 0)
		state->origin_y = 0;
}

static void calendar_rect(const calendar_state *state, int calendar, RECT *area)
{
	int width = calendar_width(state), height = calendar_height(state);
	int column = calendar % state->columns, row = calendar / state->columns;
	area->left = state->origin_x + column * (width + state->gap);
	area->top = state->origin_y + row * (height + state->gap);
	area->right = area->left + width;
	area->bottom = area->top + height;
}

/* Notifications */

static LRESULT notify_parent(const calendar_state *state, NMHDR *header, UINT code)
{
	header->hwndFrom = state->window;
	header->idFrom = GetWindowLongPtrW(state->window, GWLP_ID);
	header->code = code;
	return SendMessageW(state->parent, WM_NOTIFY, header->idFrom, (LPARAM)header);
}

static void ask_day_states(calendar_state *state)
{
	NMDAYSTATE info;
	int count = shown_calendars(state) + 2, index;
	MONTHDAYSTATE scratch[MAX_CALENDARS + 2];
	if (!(style_of(state) & MCS_DAYSTATE))
		return;
	ZeroMemory(&info, sizeof(info));
	ZeroMemory(scratch, sizeof(scratch));
	info.stStart = state->first;
	info.stStart.wDay = 1;
	add_months(&info.stStart, -1);
	fix_weekday(&info.stStart);
	info.cDayState = count;
	info.prgDayState = scratch;
	if (notify_parent(state, &info.nmhdr, MCN_GETDAYSTATE) && info.prgDayState) {
		for (index = 0; index < count; index++)
			state->states[index] = info.prgDayState[index];
	}
}

static void notify_selection(calendar_state *state, UINT code)
{
	NMSELCHANGE info;
	ZeroMemory(&info, sizeof(info));
	info.stSelStart = state->selection[0];
	fix_weekday(&info.stSelStart);
	if (style_of(state) & MCS_MULTISELECT) {
		info.stSelEnd = state->selection[1];
		fix_weekday(&info.stSelEnd);
	}
	notify_parent(state, &info.nmhdr, code);
}

static BOOL month_is_shown(const calendar_state *state, const SYSTEMTIME *date)
{
	int first = state->first.wYear * 12 + state->first.wMonth - 1;
	int wanted = date->wYear * 12 + date->wMonth - 1;
	return wanted >= first && wanted < first + shown_calendars(state);
}

static void show_month_of(calendar_state *state, const SYSTEMTIME *date)
{
	if (month_is_shown(state, date))
		return;
	state->first = *date;
	state->first.wDay = 1;
	ask_day_states(state);
}

static BOOL within_limits(const calendar_state *state, const SYSTEMTIME *date)
{
	if ((state->limit_flags & GDTR_MIN) && compare_dates(date, &state->limits[0]) < 0)
		return FALSE;
	if ((state->limit_flags & GDTR_MAX) && compare_dates(date, &state->limits[1]) > 0)
		return FALSE;
	return TRUE;
}

static void scroll_months(calendar_state *state, int months)
{
	SYSTEMTIME moved = state->first;
	int total = moved.wYear * 12 + moved.wMonth - 1 + months;
	if (total < FIRST_YEAR * 12 + FIRST_MONTH - 1 || total >= (LAST_YEAR + 1) * 12)
		return;
	add_months(&moved, months);
	moved.wDay = 1;
	state->first = moved;
	ask_day_states(state);
	if (!(style_of(state) & MCS_MULTISELECT)) {
		/* The selected day follows the view, and the day of the month stays what it was. */
		add_months(&state->selection[0], months);
		state->selection[1] = state->selection[0];
		notify_selection(state, MCN_SELCHANGE);
	}
	InvalidateRect(state->window, NULL, FALSE);
}

static int scroll_step(const calendar_state *state)
{
	return state->delta ? state->delta : shown_calendars(state);
}

/* Hit test */

static void button_rects(const calendar_state *state, const RECT *calendar, RECT *previous, RECT *next)
{
	int height = state->day_height * TITLE_ROWS;
	int top = calendar->top + (height - BUTTON_SIZE) / 2;
	SetRect(previous, calendar->left + BUTTON_INSET, top, calendar->left + BUTTON_INSET + BUTTON_SIZE, top + BUTTON_SIZE);
	SetRect(next, calendar->right - BUTTON_INSET - BUTTON_SIZE, top, calendar->right - BUTTON_INSET, top + BUTTON_SIZE);
}

static void format_title(const calendar_state *state, int calendar, WCHAR *month, int month_size, WCHAR *year, int year_size)
{
	SYSTEMTIME date = state->first;
	add_months(&date, calendar);
	date.wDay = 1;
	fix_weekday(&date);
	if (!GetDateFormatW(LOCALE_USER_DEFAULT, 0, &date, L"MMMM", month, month_size))
		month[0] = 0;
	if (!GetDateFormatW(LOCALE_USER_DEFAULT, 0, &date, L"yyyy", year, year_size))
		year[0] = 0;
}

/* The month and year sit side by side, centred between the buttons. */
static void title_rects(const calendar_state *state, int calendar, const RECT *area, RECT *month_area, RECT *year_area)
{
	WCHAR month[64], year[16];
	HDC dc = GetDC(state->window);
	HGDIOBJ old = SelectObject(dc, state->bold ? state->bold : font_of(state));
	SIZE month_size = { 0, 0 }, year_size = { 0, 0 }, space = { 0, 0 };
	int total, left, height = state->day_height * TITLE_ROWS;
	format_title(state, calendar, month, 64, year, 16);
	GetTextExtentPoint32W(dc, month, lstrlenW(month), &month_size);
	GetTextExtentPoint32W(dc, year, lstrlenW(year), &year_size);
	GetTextExtentPoint32W(dc, L" ", 1, &space);
	SelectObject(dc, old);
	ReleaseDC(state->window, dc);
	total = month_size.cx + space.cx + year_size.cx;
	left = (area->left + area->right - total) / 2;
	SetRect(month_area, left, area->top, left + month_size.cx, area->top + height);
	SetRect(year_area, left + month_size.cx + space.cx, area->top, left + total, area->top + height);
}

static void hit_test(const calendar_state *state, POINT at, calendar_hit *result)
{
	int calendar;
	ZeroMemory(result, sizeof(*result));
	result->calendar = result->row = result->column = -1;
	result->hit = MCHT_NOWHERE;
	for (calendar = 0; calendar < shown_calendars(state); calendar++) {
		RECT area, previous, next, month_area, year_area;
		int y, title_height = state->day_height * TITLE_ROWS;
		calendar_rect(state, calendar, &area);
		if (!PtInRect(&area, at))
			continue;
		result->calendar = calendar;
		y = at.y - area.top;
		if (y < title_height) {
			SetRect(&result->area, area.left, area.top, area.right, area.top + title_height);
			button_rects(state, &area, &previous, &next);
			title_rects(state, calendar, &area, &month_area, &year_area);
			if (PtInRect(&previous, at))
				result->hit = MCHT_TITLEBTNPREV;
			else if (PtInRect(&next, at))
				result->hit = MCHT_TITLEBTNNEXT;
			else if (PtInRect(&month_area, at))
				result->hit = MCHT_TITLEMONTH;
			else if (PtInRect(&year_area, at))
				result->hit = MCHT_TITLEYEAR;
			else
				result->hit = MCHT_TITLE;
			return;
		}
		y -= title_height;
		if (y < state->day_height * HEAD_ROWS) {
			int column = (at.x - area.left - state->week_width) / state->day_width;
			result->hit = at.x - area.left < state->week_width ? MCHT_CALENDARBK : MCHT_CALENDARDAY;
			result->column = column;
			result->row = -1;
			SetRect(&result->area, area.left + state->week_width + column * state->day_width, area.top + title_height, area.left + state->week_width + (column + 1) * state->day_width, area.top + title_height + state->day_height);
			first_cell_of(state, calendar, &result->day);
			add_days(&result->day, column);
			return;
		}
		y -= state->day_height * HEAD_ROWS;
		if (y < state->day_height * DAY_ROWS) {
			int row = y / state->day_height;
			int column;
			SYSTEMTIME month = state->first;
			if (at.x - area.left < state->week_width) {
				result->hit = MCHT_CALENDARWEEKNUM;
				result->row = row;
				day_of_cell(state, calendar, row, 0, &result->day);
				return;
			}
			column = (at.x - area.left - state->week_width) / state->day_width;
			if (column >= DAYS_PER_ROW)
				column = DAYS_PER_ROW - 1;
			result->row = row;
			result->column = column;
			day_of_cell(state, calendar, row, column, &result->day);
			add_months(&month, calendar);
			SetRect(&result->area, area.left + state->week_width + column * state->day_width, area.top + title_height + state->day_height * (HEAD_ROWS + row), area.left + state->week_width + (column + 1) * state->day_width, area.top + title_height + state->day_height * (HEAD_ROWS + row + 1));
			if (result->day.wMonth == month.wMonth)
				result->hit = MCHT_CALENDARDATE;
			else if ((result->day.wYear * 12 + result->day.wMonth) < (month.wYear * 12 + month.wMonth))
				result->hit = MCHT_CALENDARDATEPREV;
			else
				result->hit = MCHT_CALENDARDATENEXT;
			return;
		}
		if (today_rows(state)) {
			result->hit = MCHT_TODAYLINK;
			result->day = state->today;
			SetRect(&result->area, area.left, area.bottom - state->day_height, area.right, area.bottom);
			return;
		}
	}
}

/* Painting */

static void draw_arrow(HDC dc, const RECT *button, BOOL left, COLORREF color)
{
	POINT points[3];
	HBRUSH brush = CreateSolidBrush(color);
	HPEN pen = CreatePen(PS_SOLID, 1, color);
	HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);
	int mid_y = (button->top + button->bottom) / 2, mid_x = (button->left + button->right) / 2;
	if (left) {
		points[0].x = mid_x - 3; points[0].y = mid_y;
		points[1].x = mid_x + 2; points[1].y = mid_y - 5;
		points[2].x = mid_x + 2; points[2].y = mid_y + 5;
	}
	else {
		points[0].x = mid_x + 3; points[0].y = mid_y;
		points[1].x = mid_x - 2; points[1].y = mid_y - 5;
		points[2].x = mid_x - 2; points[2].y = mid_y + 5;
	}
	Polygon(dc, points, 3);
	SelectObject(dc, old_brush);
	SelectObject(dc, old_pen);
	DeleteObject(brush);
	DeleteObject(pen);
}

static BOOL day_selected(const calendar_state *state, const SYSTEMTIME *date)
{
	return compare_dates(date, &state->selection[0]) >= 0 && compare_dates(date, &state->selection[1]) <= 0;
}

static BOOL day_is_bold(const calendar_state *state, int calendar, const SYSTEMTIME *date, const SYSTEMTIME *month)
{
	int index;
	if (!(style_of(state) & MCS_DAYSTATE))
		return FALSE;
	index = calendar + 1 + (date->wYear * 12 + date->wMonth) - (month->wYear * 12 + month->wMonth);
	if (index < 0 || index >= MAX_CALENDARS + 2)
		return FALSE;
	return (state->states[index] >> (date->wDay - 1)) & 1;
}

static void text_centered(HDC dc, const WCHAR *text, const RECT *area)
{
	RECT copy = *area;
	DrawTextW(dc, text, -1, &copy, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
}

static int iso_week(const SYSTEMTIME *monday_row)
{
	SYSTEMTIME thursday = *monday_row;
	SYSTEMTIME january;
	int weekday = day_of_week(thursday.wYear, thursday.wMonth, thursday.wDay);
	add_days(&thursday, ((4 - weekday + 7) % 7));
	ZeroMemory(&january, sizeof(january));
	january.wYear = thursday.wYear;
	january.wMonth = 1;
	january.wDay = 1;
	return days_between(&january, &thursday) / 7 + 1;
}

static void draw_calendar(calendar_state *state, HDC dc, int calendar)
{
	RECT area, title, previous, next;
	WCHAR month_text[64], year_text[16], line[96];
	HBRUSH title_brush, month_brush;
	SYSTEMTIME month = state->first;
	int row, column, top, title_height = state->day_height * TITLE_ROWS;
	BOOL first_of_grid = calendar == 0, last_of_grid = calendar == shown_calendars(state) - 1;
	HFONT plain = font_of(state);

	calendar_rect(state, calendar, &area);
	add_months(&month, calendar);
	title = area;
	title.bottom = area.top + title_height;
	title_brush = CreateSolidBrush(state->colors[COLOR_TITLE_BACK]);
	FillRect(dc, &title, title_brush);
	DeleteObject(title_brush);
	month_brush = CreateSolidBrush(state->colors[COLOR_MONTH_BACK]);
	{
		RECT body = area;
		body.top = title.bottom;
		FillRect(dc, &body, month_brush);
	}
	DeleteObject(month_brush);

	SetBkMode(dc, TRANSPARENT);
	format_title(state, calendar, month_text, 64, year_text, 16);
	SelectObject(dc, state->bold ? state->bold : plain);
	SetTextColor(dc, state->colors[COLOR_TITLE_TEXT]);
	wsprintfW(line, L"%s %s", month_text, year_text);
	text_centered(dc, line, &title);
	button_rects(state, &area, &previous, &next);
	if (calendar == 0)
		draw_arrow(dc, &previous, TRUE, state->colors[COLOR_TITLE_TEXT]);
	if (calendar == state->columns - 1 || shown_calendars(state) == 1)
		draw_arrow(dc, &next, FALSE, state->colors[COLOR_TITLE_TEXT]);

	/* Weekday names, with a line beneath. */
	SelectObject(dc, plain);
	SetTextColor(dc, state->colors[COLOR_TITLE_BACK]);
	top = title.bottom;
	for (column = 0; column < DAYS_PER_ROW; column++) {
		RECT cell;
		WCHAR name[32];
		int weekday = (first_column_weekday(state) + column) % 7;
		/* The locale tables start on Monday, the arithmetic above on Sunday. */
		int locale_index = (weekday + 6) % 7;
		if (!GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SSHORTESTDAYNAME1 + locale_index, name, 32) && !GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SABBREVDAYNAME1 + locale_index, name, 32))
			name[0] = 0;
		SetRect(&cell, area.left + state->week_width + column * state->day_width, top, area.left + state->week_width + (column + 1) * state->day_width, top + state->day_height);
		text_centered(dc, name, &cell);
	}
	{
		HPEN pen = CreatePen(PS_SOLID, 1, state->colors[COLOR_TRAILING]);
		HGDIOBJ old = SelectObject(dc, pen);
		MoveToEx(dc, area.left + state->week_width, top + state->day_height - 1, NULL);
		LineTo(dc, area.right, top + state->day_height - 1);
		SelectObject(dc, old);
		DeleteObject(pen);
	}
	top += state->day_height * HEAD_ROWS;

	/* Days. Only the outer calendars of the grid show the days of the months around them. */
	for (row = 0; row < DAY_ROWS; row++) {
		if (style_of(state) & MCS_WEEKNUMBERS) {
			SYSTEMTIME start;
			RECT cell;
			WCHAR number[8];
			day_of_cell(state, calendar, row, 0, &start);
			SetRect(&cell, area.left, top + row * state->day_height, area.left + state->week_width, top + (row + 1) * state->day_height);
			wsprintfW(number, L"%d", iso_week(&start));
			SetTextColor(dc, state->colors[COLOR_TRAILING]);
			text_centered(dc, number, &cell);
		}
		for (column = 0; column < DAYS_PER_ROW; column++) {
			SYSTEMTIME day;
			RECT cell;
			WCHAR number[8];
			BOOL inside, picked;
			day_of_cell(state, calendar, row, column, &day);
			inside = day.wMonth == month.wMonth;
			if (!inside) {
				BOOL before = (day.wYear * 12 + day.wMonth) < (month.wYear * 12 + month.wMonth);
				if ((before && !first_of_grid) || (!before && !last_of_grid))
					continue;
			}
			SetRect(&cell, area.left + state->week_width + column * state->day_width, top + row * state->day_height, area.left + state->week_width + (column + 1) * state->day_width, top + (row + 1) * state->day_height);
			picked = day_selected(state, &day);
			if (picked && inside) {
				HBRUSH mark = CreateSolidBrush(state->colors[COLOR_TITLE_BACK]);
				FillRect(dc, &cell, mark);
				DeleteObject(mark);
				SetTextColor(dc, state->colors[COLOR_TITLE_TEXT]);
			}
			else {
				SetTextColor(dc, state->colors[inside ? COLOR_TEXT : COLOR_TRAILING]);
			}
			SelectObject(dc, inside && day_is_bold(state, calendar, &day, &month) && state->bold ? state->bold : plain);
			wsprintfW(number, L"%d", day.wDay);
			text_centered(dc, number, &cell);
			if (!(style_of(state) & MCS_NOTODAYCIRCLE) && inside && !compare_dates(&day, &state->today)) {
				HPEN pen = CreatePen(PS_SOLID, 1, RGB(200, 40, 40));
				HGDIOBJ old_pen = SelectObject(dc, pen), old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
				Rectangle(dc, cell.left, cell.top, cell.right, cell.bottom);
				SelectObject(dc, old_pen);
				SelectObject(dc, old_brush);
				DeleteObject(pen);
			}
		}
	}
	SelectObject(dc, plain);

	if (today_rows(state) && last_of_grid) {
		RECT line_area = area;
		WCHAR date_text[64], label[32];
		line_area.top = area.bottom - state->day_height;
		SetTextColor(dc, state->colors[COLOR_TEXT]);
		if (!LoadStringW(GetModuleHandleW(L"comctl32"), 0, label, 32))
			lstrcpyW(label, L"Today:");
		if (!GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &state->today, NULL, date_text, 64))
			date_text[0] = 0;
		wsprintfW(line, L"%s %s", label, date_text);
		SelectObject(dc, state->bold ? state->bold : plain);
		text_centered(dc, line, &line_area);
		SelectObject(dc, plain);
	}
}

static void draw_all(calendar_state *state, HDC dc)
{
	RECT client;
	HBRUSH back;
	int calendar;
	HGDIOBJ old = SelectObject(dc, font_of(state));
	GetClientRect(state->window, &client);
	back = CreateSolidBrush(state->colors[COLOR_BACK]);
	FillRect(dc, &client, back);
	DeleteObject(back);
	for (calendar = 0; calendar < shown_calendars(state); calendar++)
		draw_calendar(state, dc, calendar);
	SelectObject(dc, old);
}

static void paint(calendar_state *state, HDC given)
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

/* Messages */

static LRESULT set_range(calendar_state *state, DWORD flags, SYSTEMTIME *dates)
{
	SYSTEMTIME first, second;
	if (!dates)
		return FALSE;
	if ((flags & GDTR_MIN) && !date_is_valid(&dates[0]))
		return FALSE;
	if ((flags & GDTR_MAX) && !date_is_valid(&dates[1]))
		return FALSE;
	/* A time that makes no sense is replaced by today's, in the caller's own structure too. */
	if ((flags & GDTR_MIN) && !time_is_valid(&dates[0])) {
		dates[0].wHour = state->today.wHour;
		dates[0].wMinute = state->today.wMinute;
		dates[0].wSecond = state->today.wSecond;
	}
	if ((flags & GDTR_MAX) && !time_is_valid(&dates[1])) {
		dates[1].wHour = state->today.wHour;
		dates[1].wMinute = state->today.wMinute;
		dates[1].wSecond = state->today.wSecond;
	}
	if (!(flags & (GDTR_MIN | GDTR_MAX))) {
		state->limit_flags = 0;
		ZeroMemory(state->limits, sizeof(state->limits));
		return TRUE;
	}
	first = dates[0];
	second = dates[1];
	if ((flags & GDTR_MIN) && (flags & GDTR_MAX)) {
		if (compare_moments(&first, &second) > 0) {
			SYSTEMTIME swap = first;
			first = second;
			second = swap;
		}
		state->limits[0] = first;
		state->limits[1] = second;
		state->limit_flags = GDTR_MIN | GDTR_MAX;
	}
	else if (flags & GDTR_MIN) {
		/* A limit that crosses the other one replaces it. */
		state->limits[0] = first;
		if ((state->limit_flags & GDTR_MAX) && compare_moments(&first, &state->limits[1]) >= 0) {
			ZeroMemory(&state->limits[1], sizeof(state->limits[1]));
			state->limit_flags &= ~GDTR_MAX;
		}
		state->limit_flags |= GDTR_MIN;
	}
	else {
		state->limits[1] = second;
		if ((state->limit_flags & GDTR_MIN) && compare_moments(&second, &state->limits[0]) <= 0) {
			ZeroMemory(&state->limits[0], sizeof(state->limits[0]));
			state->limit_flags &= ~GDTR_MIN;
		}
		state->limit_flags |= GDTR_MAX;
	}
	InvalidateRect(state->window, NULL, FALSE);
	return TRUE;
}

static LRESULT month_range(const calendar_state *state, DWORD which, SYSTEMTIME *dates)
{
	int count = shown_calendars(state);
	if (which == GMR_DAYSTATE) {
		if (dates) {
			first_cell_of(state, 0, &dates[0]);
			day_of_cell(state, count - 1, DAY_ROWS - 1, DAYS_PER_ROW - 1, &dates[1]);
		}
		return count + 2;
	}
	if (dates) {
		SYSTEMTIME last = state->first;
		dates[0] = state->first;
		dates[0].wDay = 1;
		if (dates[0].wYear == FIRST_YEAR && dates[0].wMonth == FIRST_MONTH)
			dates[0].wDay = FIRST_DAY;
		fix_weekday(&dates[0]);
		add_months(&last, count - 1);
		last.wDay = (WORD)days_in_month(last.wYear, last.wMonth);
		fix_weekday(&last);
		dates[1] = last;
	}
	return count;
}

static LRESULT set_selection(calendar_state *state, const SYSTEMTIME *date)
{
	SYSTEMTIME value;
	if (!date || (style_of(state) & MCS_MULTISELECT))
		return FALSE;
	if (!date_is_valid(date))
		return FALSE;
	if (!compare_dates(date, &state->selection[0]) && !within_limits(state, date))
		return TRUE;
	if (!within_limits(state, date))
		return FALSE;
	value = *date;
	state->selection[0] = state->selection[1] = value;
	show_month_of(state, &value);
	InvalidateRect(state->window, NULL, FALSE);
	return TRUE;
}

static LRESULT set_selection_range(calendar_state *state, const SYSTEMTIME *dates)
{
	SYSTEMTIME first, second;
	if (!dates || !(style_of(state) & MCS_MULTISELECT))
		return FALSE;
	if (!date_is_valid(&dates[0]) || !date_is_valid(&dates[1]))
		return FALSE;
	first = dates[0];
	second = dates[1];
	if (compare_dates(&first, &second) > 0) {
		SYSTEMTIME swap = first;
		first = second;
		second = swap;
	}
	if (days_between(&first, &second) >= state->span)
		return FALSE;
	if (!within_limits(state, &first) || !within_limits(state, &second))
		return FALSE;
	state->selection[0] = first;
	state->selection[1] = second;
	fix_weekday(&state->selection[0]);
	fix_weekday(&state->selection[1]);
	show_month_of(state, &first);
	InvalidateRect(state->window, NULL, FALSE);
	return TRUE;
}

static LRESULT hit_message(const calendar_state *state, MCHITTESTINFO *info)
{
	calendar_hit found;
	if (!info || info->cbSize < MCHITTESTINFO_V1_SIZE)
		return -1;
	hit_test(state, info->pt, &found);
	info->uHit = found.hit;
	if (found.hit != MCHT_NOWHERE)
		info->st = found.day;
	if (info->cbSize >= sizeof(MCHITTESTINFO) && found.hit != MCHT_NOWHERE) {
		info->rc = found.area;
		info->iOffset = found.calendar;
		info->iRow = found.row;
		info->iCol = found.column;
	}
	return found.hit;
}

static void select_day(calendar_state *state, const SYSTEMTIME *date)
{
	SYSTEMTIME value = *date;
	WORD hour = state->selection[0].wHour, minute = state->selection[0].wMinute, second = state->selection[0].wSecond, ms = state->selection[0].wMilliseconds;
	value.wHour = hour;
	value.wMinute = minute;
	value.wSecond = second;
	value.wMilliseconds = ms;
	fix_weekday(&value);
	state->selection[0] = state->selection[1] = value;
	show_month_of(state, &value);
	InvalidateRect(state->window, NULL, FALSE);
}

static void extend_selection(calendar_state *state, const SYSTEMTIME *date)
{
	SYSTEMTIME first = state->anchor, second = *date;
	WORD hour = state->selection[0].wHour;
	if (compare_dates(&first, &second) > 0) {
		SYSTEMTIME swap = first;
		first = second;
		second = swap;
	}
	while (days_between(&first, &second) >= state->span) {
		if (compare_dates(date, &state->anchor) > 0)
			add_days(&second, -1);
		else
			add_days(&first, 1);
	}
	first.wHour = second.wHour = hour;
	state->selection[0] = first;
	state->selection[1] = second;
	InvalidateRect(state->window, NULL, FALSE);
}

static void show_month_menu(calendar_state *state, POINT at)
{
	HMENU menu = CreatePopupMenu();
	POINT screen = at;
	int month, picked;
	if (!menu)
		return;
	for (month = 0; month < 12; month++) {
		WCHAR name[64];
		if (!GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SMONTHNAME1 + month, name, 64))
			name[0] = 0;
		AppendMenuW(menu, MF_STRING | (month + 1 == state->first.wMonth ? MF_CHECKED : 0), month + 1, name);
	}
	ClientToScreen(state->window, &screen);
	picked = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, 0, state->window, NULL);
	DestroyMenu(menu);
	if (picked)
		scroll_months(state, picked - state->first.wMonth);
}

static void mouse_down(calendar_state *state, POINT at)
{
	calendar_hit found;
	hit_test(state, at, &found);
	SetFocus(state->window);
	switch (found.hit) {
	case MCHT_TITLEBTNPREV:
	case MCHT_TITLEBTNNEXT:
		state->repeat_step = found.hit == MCHT_TITLEBTNPREV ? -scroll_step(state) : scroll_step(state);
		scroll_months(state, state->repeat_step);
		SetCapture(state->window);
		SetTimer(state->window, REPEAT_TIMER, REPEAT_DELAY, NULL);
		state->press = 1;
		break;
	case MCHT_TITLEMONTH:
		show_month_menu(state, at);
		break;
	case MCHT_TODAYLINK:
		if (within_limits(state, &state->today)) {
			if (style_of(state) & MCS_MULTISELECT) {
				state->selection[0] = state->selection[1] = state->today;
				show_month_of(state, &state->today);
			}
			else {
				select_day(state, &state->today);
			}
			InvalidateRect(state->window, NULL, FALSE);
			notify_selection(state, MCN_SELCHANGE);
			notify_selection(state, MCN_SELECT);
		}
		break;
	case MCHT_CALENDARDATE:
	case MCHT_CALENDARDATEPREV:
	case MCHT_CALENDARDATENEXT:
		if (!within_limits(state, &found.day))
			break;
		if (style_of(state) & MCS_MULTISELECT) {
			state->anchor = found.day;
			select_day(state, &found.day);
			notify_selection(state, MCN_SELCHANGE);
			state->dragging = TRUE;
			SetCapture(state->window);
		}
		else {
			state->anchor = found.day;
			state->dragging = TRUE;
		}
		break;
	default:
		break;
	}
}

static void mouse_up(calendar_state *state, POINT at)
{
	calendar_hit found;
	BOOL was_dragging = state->dragging;
	state->dragging = FALSE;
	if (state->press) {
		state->press = 0;
		KillTimer(state->window, REPEAT_TIMER);
	}
	if (GetCapture() == state->window)
		ReleaseCapture();
	if (!was_dragging)
		return;
	hit_test(state, at, &found);
	if (style_of(state) & MCS_MULTISELECT) {
		notify_selection(state, MCN_SELECT);
		return;
	}
	if ((found.hit == MCHT_CALENDARDATE || found.hit == MCHT_CALENDARDATEPREV || found.hit == MCHT_CALENDARDATENEXT) && !compare_dates(&found.day, &state->anchor) && within_limits(state, &found.day)) {
		select_day(state, &found.day);
		notify_selection(state, MCN_SELCHANGE);
		notify_selection(state, MCN_SELECT);
	}
}

static void move_selection(calendar_state *state, int days)
{
	SYSTEMTIME moved = state->selection[0];
	add_days(&moved, days);
	if (!date_is_valid(&moved) || !within_limits(state, &moved))
		return;
	select_day(state, &moved);
	notify_selection(state, MCN_SELCHANGE);
}

static LRESULT CALLBACK calendar_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	calendar_state *state = state_of(window);

	if (message == WM_NCCREATE) {
		const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
		state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
		if (!state)
			return FALSE;
		state->window = window;
		state->parent = create->hwndParent;
		SetWindowLongPtrW(window, 0, (LONG_PTR)state);
		return DefWindowProcW(window, message, wparam, lparam);
	}
	if (!state)
		return DefWindowProcW(window, message, wparam, lparam);

	switch (message) {
	case WM_CREATE:
		GetLocalTime(&state->today);
		state->selection[0] = state->selection[1] = state->today;
		state->first = state->today;
		state->first.wDay = 1;
		state->colors[COLOR_BACK] = GetSysColor(COLOR_WINDOW);
		state->colors[COLOR_TEXT] = GetSysColor(COLOR_WINDOWTEXT);
		state->colors[COLOR_TITLE_BACK] = GetSysColor(COLOR_ACTIVECAPTION);
		state->colors[COLOR_TITLE_TEXT] = GetSysColor(COLOR_WINDOW);
		state->colors[COLOR_MONTH_BACK] = GetSysColor(COLOR_WINDOW);
		state->colors[COLOR_TRAILING] = GetSysColor(COLOR_GRAYTEXT);
		state->first_day_value = locale_first_day();
		state->span = (GetWindowLongPtrW(window, GWL_STYLE) & MCS_MULTISELECT) ? DEFAULT_SPAN : 1;
		state->gap = DEFAULT_GAP;
		state->press = 0;
		remeasure(state);
		arrange_grid(state);
		ask_day_states(state);
		return 0;
	case WM_NCDESTROY:
		SetWindowLongPtrW(window, 0, 0);
		if (state->bold)
			DeleteObject(state->bold);
		HeapFree(GetProcessHeap(), 0, state);
		break;
	case WM_STYLECHANGING:
		if (wparam == GWL_STYLE) {
			STYLESTRUCT *change = (STYLESTRUCT *)lparam;
			const DWORD locked = MCS_MULTISELECT | MCS_DAYSTATE;
			change->styleNew = (change->styleNew & ~locked) | (change->styleOld & locked);
		}
		return 0;
	case WM_STYLECHANGED:
		remeasure(state);
		arrange_grid(state);
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_SIZE:
		arrange_grid(state);
		InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_SETFONT:
		state->font = (HFONT)wparam;
		remeasure(state);
		arrange_grid(state);
		if (LOWORD(lparam))
			InvalidateRect(window, NULL, TRUE);
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
		paint(state, (HDC)wparam);
		return 0;
	case WM_PRINTCLIENT:
		paint(state, (HDC)wparam);
		return 0;
	case WM_GETDLGCODE:
		return DLGC_WANTARROWS | DLGC_WANTCHARS;
	case WM_LBUTTONDOWN: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		mouse_down(state, at);
		return 0;
	}
	case WM_LBUTTONUP: {
		POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
		mouse_up(state, at);
		return 0;
	}
	case WM_MOUSEMOVE:
		if (state->dragging && (style_of(state) & MCS_MULTISELECT)) {
			POINT at = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
			calendar_hit found;
			hit_test(state, at, &found);
			if (found.hit == MCHT_CALENDARDATE || found.hit == MCHT_CALENDARDATEPREV || found.hit == MCHT_CALENDARDATENEXT) {
				SYSTEMTIME old_first = state->selection[0], old_last = state->selection[1];
				extend_selection(state, &found.day);
				if (compare_dates(&old_first, &state->selection[0]) || compare_dates(&old_last, &state->selection[1]))
					notify_selection(state, MCN_SELCHANGE);
			}
		}
		return 0;
	case WM_TIMER:
		if (wparam == REPEAT_TIMER && state->press) {
			scroll_months(state, state->repeat_step);
			return 0;
		}
		break;
	case WM_CAPTURECHANGED:
		state->press = 0;
		state->dragging = FALSE;
		KillTimer(window, REPEAT_TIMER);
		return 0;
	case WM_KEYDOWN:
		switch (wparam) {
		case VK_LEFT:
			move_selection(state, -1);
			break;
		case VK_RIGHT:
			move_selection(state, 1);
			break;
		case VK_UP:
			move_selection(state, -DAYS_PER_ROW);
			break;
		case VK_DOWN:
			move_selection(state, DAYS_PER_ROW);
			break;
		case VK_PRIOR:
			scroll_months(state, -1);
			break;
		case VK_NEXT:
			scroll_months(state, 1);
			break;
		case VK_RETURN:
			notify_selection(state, MCN_SELECT);
			break;
		}
		return 0;

	case MCM_GETCURSEL:
		if (!lparam || (style_of(state) & MCS_MULTISELECT))
			return FALSE;
		*(SYSTEMTIME *)lparam = state->selection[0];
		fix_weekday((SYSTEMTIME *)lparam);
		return TRUE;
	case MCM_SETCURSEL:
		return set_selection(state, (const SYSTEMTIME *)lparam);
	case MCM_GETMAXSELCOUNT:
		return state->span;
	case MCM_SETMAXSELCOUNT:
		if (!(style_of(state) & MCS_MULTISELECT) || (int)wparam < 1)
			return FALSE;
		state->span = (int)wparam;
		return TRUE;
	case MCM_GETSELRANGE: {
		SYSTEMTIME *dates = (SYSTEMTIME *)lparam;
		if (!dates || !(style_of(state) & MCS_MULTISELECT))
			return FALSE;
		dates[0] = state->selection[0];
		dates[1] = state->selection[1];
		fix_weekday(&dates[0]);
		fix_weekday(&dates[1]);
		return TRUE;
	}
	case MCM_SETSELRANGE:
		return set_selection_range(state, (const SYSTEMTIME *)lparam);
	case MCM_GETMONTHRANGE:
		return month_range(state, (DWORD)wparam, (SYSTEMTIME *)lparam);
	case MCM_SETDAYSTATE: {
		const MONTHDAYSTATE *states = (const MONTHDAYSTATE *)lparam;
		int count = (int)wparam, index;
		if (!(style_of(state) & MCS_DAYSTATE) || !states || count != shown_calendars(state) + 2)
			return FALSE;
		for (index = 0; index < count; index++)
			state->states[index] = states[index];
		InvalidateRect(window, NULL, FALSE);
		return TRUE;
	}
	case MCM_GETMINREQRECT: {
		RECT *area = (RECT *)lparam;
		if (!area)
			return FALSE;
		SetRect(area, 0, 0, calendar_width(state), calendar_height(state));
		return TRUE;
	}
	case MCM_SIZERECTTOMIN: {
		RECT *area = (RECT *)lparam;
		int width, height, columns, rows;
		if (!area)
			return FALSE;
		width = calendar_width(state);
		height = calendar_height(state);
		columns = (area->right - area->left + state->gap) / (width + state->gap);
		rows = (area->bottom - area->top + state->gap) / (height + state->gap);
		if (columns < 1)
			columns = 1;
		if (rows < 1)
			rows = 1;
		area->right = area->left + columns * width + (columns - 1) * state->gap;
		area->bottom = area->top + rows * height + (rows - 1) * state->gap;
		return TRUE;
	}
	case MCM_GETCOLOR:
		return (int)wparam >= 0 && (int)wparam < COLOR_COUNT ? (LRESULT)state->colors[wparam] : -1;
	case MCM_SETCOLOR: {
		COLORREF old;
		if ((int)wparam < 0 || (int)wparam >= COLOR_COUNT)
			return -1;
		old = state->colors[wparam];
		state->colors[wparam] = (COLORREF)lparam;
		InvalidateRect(window, NULL, TRUE);
		return old;
	}
	case MCM_GETTODAY:
		if (!lparam)
			return FALSE;
		*(SYSTEMTIME *)lparam = state->today;
		return TRUE;
	case MCM_SETTODAY:
		if (lparam) {
			state->today = *(const SYSTEMTIME *)lparam;
			state->today_set = TRUE;
			InvalidateRect(window, NULL, FALSE);
		}
		return 0;
	case MCM_HITTEST:
		return hit_message(state, (MCHITTESTINFO *)lparam);
	case MCM_GETFIRSTDAYOFWEEK:
		return MAKELONG(state->first_day_value, state->first_day_set);
	case MCM_SETFIRSTDAYOFWEEK: {
		LRESULT old = MAKELONG(state->first_day_value, state->first_day_set);
		int value = (int)lparam;
		if (value == -1) {
			state->first_day_value = locale_first_day();
			state->first_day_set = FALSE;
		}
		else {
			state->first_day_value = value >= 7 ? 6 : value;
			state->first_day_set = TRUE;
		}
		InvalidateRect(window, NULL, FALSE);
		return old;
	}
	case MCM_GETRANGE: {
		SYSTEMTIME *dates = (SYSTEMTIME *)lparam;
		if (dates) {
			dates[0] = state->limits[0];
			dates[1] = state->limits[1];
		}
		return state->limit_flags;
	}
	case MCM_SETRANGE:
		return set_range(state, (DWORD)wparam, (SYSTEMTIME *)lparam);
	case MCM_GETMONTHDELTA:
		return scroll_step(state);
	case MCM_SETMONTHDELTA: {
		int old = state->delta;
		state->delta = (int)wparam;
		return old;
	}
	case MCM_GETMAXTODAYWIDTH: {
		SIZE size = { 0, 0 };
		HDC dc = GetDC(window);
		WCHAR date_text[64];
		HGDIOBJ old = SelectObject(dc, state->bold ? state->bold : font_of(state));
		if (GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &state->today, NULL, date_text, 64))
			GetTextExtentPoint32W(dc, date_text, lstrlenW(date_text), &size);
		SelectObject(dc, old);
		ReleaseDC(window, dc);
		return size.cx;
	}
	case MCM_GETUNICODEFORMAT:
		return !state->narrow_format;
	case MCM_SETUNICODEFORMAT: {
		BOOL old = !state->narrow_format;
		state->narrow_format = !wparam;
		return old;
	}
	case MCM_GETCURRENTVIEW:
		return MCMV_MONTH;
	case MCM_SETCURRENTVIEW:
		return lparam == MCMV_MONTH;
	case MCM_GETCALENDARCOUNT:
		return shown_calendars(state);
	case MCM_GETCALENDARBORDER:
		return state->gap;
	case MCM_SETCALENDARBORDER: {
		state->gap = wparam ? (int)lparam : DEFAULT_GAP;
		arrange_grid(state);
		InvalidateRect(window, NULL, TRUE);
		return 0;
	}
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

void MONTHCAL_Register(void)
{
	WNDCLASSW info;

	ZeroMemory(&info, sizeof(info));
	info.style = CS_GLOBALCLASS;
	info.lpfnWndProc = calendar_proc;
	info.cbWndExtra = sizeof(calendar_state *);
	info.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	info.lpszClassName = MONTHCAL_CLASSW;
	RegisterClassW(&info);
}

void MONTHCAL_Unregister(void)
{
	UnregisterClassW(MONTHCAL_CLASSW, NULL);
}
