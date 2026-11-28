/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.Globalization.Calendar: a moment read and changed as the fields of a calendar, in a language, on a clock and in a time zone.
 *
 * The calendar holds the moment as a Windows.Foundation.DateTime (100-nanosecond ticks since 1601 in UTC) between 1 January of the
 * year 1 and the last tick of 9999, and works out the fields of the local time in its zone from it. Years, months, weeks and days are
 * added to the local date, keeping the time of day and putting a day past the end of a month on its last day; periods, hours, minutes,
 * seconds and nanoseconds are added to the moment itself. A field set to a value the calendar does not have is refused and changes
 * nothing. Only the Gregorian calendar is offered, as by the formatter, with its one era. The zone is the user's or UTC: the host knows
 * only its own zone. Names come from the language through GetCalendarInfoEx and GetLocaleInfoEx, digits from the numeral system. */
#include "object.h"

static const IID iid_calendar = {0xca30221d, 0x86d9, 0x40fb, {0xa2, 0x6b, 0xd4, 0x4e, 0xb7, 0xcf, 0x08, 0xea}};
static const IID iid_calendar_factory = {0x83f58412, 0xe56b, 0x4c75, {0xa6, 0x6e, 0x0f, 0x63, 0xd5, 0x77, 0x58, 0xa6}};
static const IID iid_calendar_factory2 = {0xb44b378c, 0xca7e, 0x4590, {0x9e, 0x72, 0xea, 0x2b, 0xec, 0x1a, 0x51, 0x15}};
static const IID iid_time_zone = {0xbb3c25e5, 0x46cf, 0x4317, {0xa3, 0xf5, 0x02, 0x62, 0x1a, 0xd5, 0x44, 0x78}};

#define TICKS_PER_SECOND 10000000LL
#define TICKS_PER_DAY (86400LL * TICKS_PER_SECOND)
/* Days from 1 January 1970 to 1 January 1601, where DateTime counts from. */
#define DAYS_TO_1601 (-134774LL)

struct calendar {
	struct object base;
	WCHAR languages[GLOBALIZATION_LANGUAGES][LOCALE_NAME_MAX_LENGTH];
	UINT32 language_count;
	WCHAR numerals[16];
	BOOL clock24, universal;
	/* The zone by the name the app reads back. */
	WCHAR zone[64];
	INT64 moment;
};

/* The local time of the moment, field by field. */
struct fields {
	INT64 year;
	INT32 month, day, hour, minute, second, ticks, weekday;
};

static struct calendar *calendar_of(void *iface) { return (struct calendar *)owner(iface); }

/* ---- Days and fields ---- */

/* The days from 1 January 1970 to a date of the proleptic Gregorian calendar, and back: whole cycles of 400 years, then the years,
 * counted from March so the leap day ends the year. */
static INT64 days_from_date(INT64 year, INT32 month, INT32 day)
{
	year -= month <= 2;
	const INT64 era = (year >= 0 ? year : year - 399) / 400;
	const INT64 of_era = year - era * 400;
	const INT64 of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	const INT64 of_cycle = of_era * 365 + of_era / 4 - of_era / 100 + of_year;
	return era * 146097 + of_cycle - 719468;
}

static void date_from_days(INT64 days, struct fields *out)
{
	days += 719468;
	const INT64 era = (days >= 0 ? days : days - 146096) / 146097;
	const INT64 of_cycle = days - era * 146097;
	const INT64 of_era = (of_cycle - of_cycle / 1460 + of_cycle / 36524 - of_cycle / 146096) / 365;
	const INT64 of_year = of_cycle - (365 * of_era + of_era / 4 - of_era / 100);
	const INT64 shifted = (5 * of_year + 2) / 153;
	out->day = (INT32)(of_year - (153 * shifted + 2) / 5 + 1);
	out->month = (INT32)(shifted < 10 ? shifted + 3 : shifted - 9);
	out->year = of_era + era * 400 + (out->month <= 2);
}

static BOOL leap(INT64 year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

static INT32 month_days(INT64 year, INT32 month)
{
	static const INT32 days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	return month == 2 && leap(year) ? 29 : days[month - 1];
}

static INT64 floor_div(INT64 value, INT64 by) { return value / by - (value % by < 0); }

static void split(INT64 local, struct fields *out)
{
	const INT64 days = floor_div(local, TICKS_PER_DAY);
	INT64 rest = local - days * TICKS_PER_DAY;
	date_from_days(days + DAYS_TO_1601, out);
	out->hour = (INT32)(rest / (3600 * TICKS_PER_SECOND));
	rest %= 3600 * TICKS_PER_SECOND;
	out->minute = (INT32)(rest / (60 * TICKS_PER_SECOND));
	rest %= 60 * TICKS_PER_SECOND;
	out->second = (INT32)(rest / TICKS_PER_SECOND);
	out->ticks = (INT32)(rest % TICKS_PER_SECOND);
	/* 1 January 1970 was a Thursday; DayOfWeek counts from Sunday. */
	out->weekday = (INT32)((days + DAYS_TO_1601 + 4) - floor_div(days + DAYS_TO_1601 + 4, 7) * 7);
}

static INT64 join(const struct fields *fields)
{
	return (days_from_date(fields->year, fields->month, fields->day) - DAYS_TO_1601) * TICKS_PER_DAY +
		((fields->hour * 60LL + fields->minute) * 60 + fields->second) * TICKS_PER_SECOND + fields->ticks;
}

static INT64 lowest(void)
{
	const struct fields first = {1, 1, 1, 0, 0, 0, 0, 0};
	return join(&first);
}

static INT64 highest(void)
{
	const struct fields last = {9999, 12, 31, 23, 59, 59, TICKS_PER_SECOND - 1, 0};
	return join(&last);
}

/* ---- The zone ---- */

static BOOL universal_name(const WCHAR *name)
{
	static const WCHAR *const names[] = {L"UTC", L"Etc/UTC", L"Etc/GMT", L"GMT", L"Etc/Universal", L"Etc/Zulu", L"Coordinated Universal Time"};
	for (size_t index = 0; index < ARRAYSIZE(names); ++index)
		if (!lstrcmpiW(name, names[index]))
			return TRUE;
	return FALSE;
}

/* The user's zone by its IANA name, as ICU maps Windows' key name, or by the key name when ICU does not know it. */
static void local_zone_name(WCHAR *out, size_t size)
{
	DYNAMIC_TIME_ZONE_INFORMATION zone;
	typedef INT (__cdecl *map_function)(const WCHAR *, INT, const char *, WCHAR *, INT, INT *);
	ZeroMemory(&zone, sizeof zone);
	GetDynamicTimeZoneInformation(&zone);
	lstrcpynW(out, zone.TimeZoneKeyName[0] ? zone.TimeZoneKeyName : L"UTC", (int)size);
	HMODULE icu = LoadLibraryW(L"icu.dll");
	map_function map = icu ? (map_function)(void *)GetProcAddress(icu, "ucal_getTimeZoneIDForWindowsID") : NULL;
	WCHAR found[64];
	INT status = 0;
	if (map && map(zone.TimeZoneKeyName, -1, NULL, found, ARRAYSIZE(found), &status) > 0 && status <= 0)
		lstrcpynW(out, found, (int)size);
	if (icu)
		FreeLibrary(icu);
}

/* The rules of the user's zone apply from 1601 on, the earliest SYSTEMTIME; before that its standard offset does. */
static BOOL in_rules(INT64 moment) { return moment >= 0; }

static INT64 standard_offset(void)
{
	TIME_ZONE_INFORMATION zone;
	GetTimeZoneInformation(&zone);
	return -(INT64)(zone.Bias + zone.StandardBias) * 60 * TICKS_PER_SECOND;
}

static INT64 to_local(const struct calendar *self, INT64 moment)
{
	if (self->universal)
		return moment;
	if (in_rules(moment)) {
		FILETIME file = {(DWORD)moment, (DWORD)((UINT64)moment >> 32)}, back;
		SYSTEMTIME universal, local;
		if (FileTimeToSystemTime(&file, &universal) && SystemTimeToTzSpecificLocalTime(NULL, &universal, &local) &&
			SystemTimeToFileTime(&local, &back))
			return (INT64)(((UINT64)back.dwHighDateTime << 32) | back.dwLowDateTime) + moment % 10000;
	}
	return moment + standard_offset();
}

static INT64 from_local(const struct calendar *self, INT64 local)
{
	if (self->universal)
		return local;
	if (in_rules(local)) {
		FILETIME file = {(DWORD)local, (DWORD)((UINT64)local >> 32)}, back;
		SYSTEMTIME given, universal;
		if (FileTimeToSystemTime(&file, &given) && TzSpecificLocalTimeToSystemTime(NULL, &given, &universal) &&
			SystemTimeToFileTime(&universal, &back))
			return (INT64)(((UINT64)back.dwHighDateTime << 32) | back.dwLowDateTime) + local % 10000;
	}
	return local - standard_offset();
}

static void fields_of(const struct calendar *self, struct fields *out) { split(to_local(self, self->moment), out); }

/* A moment the calendar can hold becomes its moment; any other is refused and the calendar keeps the one it had. */
static HRESULT move_to(struct calendar *self, INT64 moment)
{
	if (moment < lowest() || moment > highest())
		return E_INVALIDARG;
	self->moment = moment;
	return S_OK;
}

static HRESULT set_fields(struct calendar *self, const struct fields *fields)
{
	if (fields->year < 1 || fields->year > 9999)
		return E_INVALIDARG;
	return move_to(self, from_local(self, join(fields)));
}

/* ---- Text ---- */

static HRESULT text_out(const WCHAR *text, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsCreateString(text, (UINT32)lstrlenW(text), out);
}

/* A number in the calendar's digits: all of it, its last digits only, or at least so many digits with zeros in front. */
static HRESULT number_out(const struct calendar *self, INT64 value, INT32 last, INT32 least, HSTRING *out)
{
	WCHAR digits[32], text[48];
	int count = 0;
	if (last < 0 || least < 0)
		return E_INVALIDARG;
	do {
		digits[count++] = (WCHAR)(L'0' + value % 10);
		value /= 10;
	} while (value && count < 24);
	if (last && count > last)
		count = last;
	int at = 0;
	for (int pad = count; pad < least && at < 24; ++pad)
		text[at++] = L'0';
	while (count)
		text[at++] = digits[--count];
	text[at] = 0;
	numeral_digits(self->numerals, text);
	return text_out(text, out);
}

static HRESULT calendar_text(const struct calendar *self, CALTYPE type, WCHAR *text, int size)
{
	if (GetCalendarInfoEx(self->languages[0], CAL_GREGORIAN, NULL, type, text, size, NULL))
		return S_OK;
	if (GetCalendarInfoEx(self->languages[0], CAL_GREGORIAN, NULL, type & ~CAL_RETURN_GENITIVE_NAMES, text, size, NULL))
		return S_OK;
	return HRESULT_FROM_WIN32(GetLastError());
}

/* A name in full, or of about the length asked for: its abbreviation unless the full name fits; 0 asks for the usual abbreviation. */
static HRESULT name_out(const struct calendar *self, CALTYPE full, CALTYPE abbreviated, INT32 length, HSTRING *out)
{
	WCHAR text[80];
	if (!out)
		return E_POINTER;
	if (length < 0)
		return E_INVALIDARG;
	HRESULT result = calendar_text(self, full, text, ARRAYSIZE(text));
	if (SUCCEEDED(result) && (length == 0 || lstrlenW(text) > length))
		result = calendar_text(self, abbreviated, text, ARRAYSIZE(text));
	return FAILED(result) ? result : text_out(text, out);
}

static HRESULT period_out(const struct calendar *self, HSTRING *out)
{
	struct fields fields;
	WCHAR text[32];
	fields_of(self, &fields);
	if (!GetLocaleInfoEx(self->languages[0], fields.hour < 12 ? LOCALE_S1159 : LOCALE_S2359, text, ARRAYSIZE(text)))
		return HRESULT_FROM_WIN32(GetLastError());
	return text_out(text, out);
}

/* ---- The calendar ---- */

static HRESULT number_answer(INT32 *out, INT32 value)
{
	if (!out)
		return E_POINTER;
	*out = value;
	return S_OK;
}

static INT32 shown_hour(const struct calendar *self, INT32 hour)
{
	return self->clock24 ? hour : hour % 12 ? hour % 12 : 12;
}

static HRESULT make_calendar(const struct calendar *model, void **out);
static const struct kind calendar_kind;

static HRESULT WINAPI calendar_clone(void *iface, void **out) { return make_calendar(calendar_of(iface), out); }
static HRESULT WINAPI calendar_to_min(void *iface) { calendar_of(iface)->moment = lowest(); return S_OK; }
static HRESULT WINAPI calendar_to_max(void *iface) { calendar_of(iface)->moment = highest(); return S_OK; }

static HRESULT WINAPI calendar_languages(void *iface, void **out)
{
	struct calendar *self = calendar_of(iface);
	return language_names((const WCHAR (*)[LOCALE_NAME_MAX_LENGTH])self->languages, self->language_count, out);
}

static HRESULT WINAPI calendar_numerals(void *iface, HSTRING *out) { return text_out(calendar_of(iface)->numerals, out); }

static HRESULT WINAPI calendar_set_numerals(void *iface, HSTRING value)
{
	const WCHAR *system = numeral_system(WindowsGetStringRawBuffer(value, NULL));
	if (!system)
		return E_INVALIDARG;
	lstrcpyW(calendar_of(iface)->numerals, system);
	return S_OK;
}

/* The identifiers of CalendarIdentifiers: the Gregorian calendar is served; another one Windows has is not, and any other name is no
 * calendar. */
static HRESULT calendar_system(const WCHAR *name)
{
	static const WCHAR *const others[] = {L"HebrewCalendar", L"HijriCalendar", L"JapaneseCalendar", L"JulianCalendar", L"KoreanCalendar",
		L"TaiwanCalendar", L"ThaiCalendar", L"UmAlQuraCalendar", L"PersianCalendar", L"ChineseLunarCalendar", L"JapaneseLunarCalendar",
		L"KoreanLunarCalendar", L"TaiwanLunarCalendar", L"VietnameseLunarCalendar"};
	if (name && !lstrcmpW(name, L"GregorianCalendar"))
		return S_OK;
	for (size_t index = 0; name && index < ARRAYSIZE(others); ++index)
		if (!lstrcmpW(name, others[index]))
			return missing("Calendar.ChangeCalendarSystem");
	return E_INVALIDARG;
}

static HRESULT WINAPI calendar_get_system(void *iface, HSTRING *out) { return text_out(L"GregorianCalendar", out); }
static HRESULT WINAPI calendar_change_system(void *iface, HSTRING value) { return calendar_system(WindowsGetStringRawBuffer(value, NULL)); }

static HRESULT WINAPI calendar_get_clock(void *iface, HSTRING *out)
{
	return text_out(calendar_of(iface)->clock24 ? L"24HourClock" : L"12HourClock", out);
}

static HRESULT clock_of(const WCHAR *name, BOOL *clock24)
{
	if (name && !lstrcmpW(name, L"24HourClock"))
		*clock24 = TRUE;
	else if (name && !lstrcmpW(name, L"12HourClock"))
		*clock24 = FALSE;
	else
		return E_INVALIDARG;
	return S_OK;
}

static HRESULT WINAPI calendar_change_clock(void *iface, HSTRING value)
{
	return clock_of(WindowsGetStringRawBuffer(value, NULL), &calendar_of(iface)->clock24);
}

static HRESULT WINAPI calendar_get_moment(void *iface, INT64 *out)
{
	if (!out)
		return E_POINTER;
	*out = calendar_of(iface)->moment;
	return S_OK;
}

static HRESULT WINAPI calendar_set_moment(void *iface, INT64 value) { return move_to(calendar_of(iface), value); }

static INT64 now(void)
{
	FILETIME file;
	GetSystemTimeAsFileTime(&file);
	return (INT64)(((UINT64)file.dwHighDateTime << 32) | file.dwLowDateTime);
}

static HRESULT WINAPI calendar_to_now(void *iface) { calendar_of(iface)->moment = now(); return S_OK; }

/* The one era of the Gregorian calendar. */
static HRESULT WINAPI calendar_one(void *iface, INT32 *out) { return number_answer(out, 1); }
static HRESULT WINAPI calendar_put_era(void *iface, INT32 value) { return value == 1 ? S_OK : E_INVALIDARG; }
static HRESULT WINAPI calendar_add_eras(void *iface, INT32 value) { return value == 0 ? S_OK : E_INVALIDARG; }
static HRESULT WINAPI calendar_era_full(void *iface, HSTRING *out) { return name_out(calendar_of(iface), CAL_SERASTRING, CAL_SERASTRING, 80, out); }
static HRESULT WINAPI calendar_era_text(void *iface, INT32 length, HSTRING *out)
{
	return name_out(calendar_of(iface), CAL_SERASTRING, CAL_SABBREVERASTRING, length, out);
}

static HRESULT WINAPI calendar_last_year(void *iface, INT32 *out) { return number_answer(out, 9999); }

static HRESULT WINAPI calendar_year(void *iface, INT32 *out)
{
	struct fields fields;
	fields_of(calendar_of(iface), &fields);
	return number_answer(out, (INT32)fields.year);
}

static HRESULT WINAPI calendar_put_year(void *iface, INT32 value)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	if (value < 1 || value > 9999)
		return E_INVALIDARG;
	fields.year = value;
	if (fields.day > month_days(fields.year, fields.month))
		fields.day = month_days(fields.year, fields.month);
	return set_fields(self, &fields);
}

/* Years and months move the date, a day past the end of the month landed in becoming its last day. */
static HRESULT add_months(struct calendar *self, INT64 months)
{
	struct fields fields;
	fields_of(self, &fields);
	const INT64 index = fields.year * 12 + (fields.month - 1) + months;
	fields.year = floor_div(index, 12);
	fields.month = (INT32)(index - fields.year * 12) + 1;
	if (fields.year < 1 || fields.year > 9999)
		return E_INVALIDARG;
	if (fields.day > month_days(fields.year, fields.month))
		fields.day = month_days(fields.year, fields.month);
	return set_fields(self, &fields);
}

static HRESULT WINAPI calendar_add_years(void *iface, INT32 value) { return add_months(calendar_of(iface), value * 12LL); }

static HRESULT WINAPI calendar_year_text(void *iface, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	return number_out(self, fields.year, 0, 0, out);
}

static HRESULT WINAPI calendar_year_truncated(void *iface, INT32 digits, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	return digits < 1 ? E_INVALIDARG : number_out(self, fields.year, digits, 0, out);
}

static HRESULT WINAPI calendar_year_padded(void *iface, INT32 digits, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	return number_out(self, fields.year, 0, digits, out);
}

static HRESULT WINAPI calendar_twelve(void *iface, INT32 *out) { return number_answer(out, 12); }

static HRESULT WINAPI calendar_month(void *iface, INT32 *out)
{
	struct fields fields;
	fields_of(calendar_of(iface), &fields);
	return number_answer(out, fields.month);
}

static HRESULT WINAPI calendar_put_month(void *iface, INT32 value)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	if (value < 1 || value > 12)
		return E_INVALIDARG;
	fields.month = value;
	if (fields.day > month_days(fields.year, fields.month))
		fields.day = month_days(fields.year, fields.month);
	return set_fields(self, &fields);
}

static HRESULT WINAPI calendar_add_months(void *iface, INT32 value) { return add_months(calendar_of(iface), value); }

/* The month by name: as a date writes it (the genitive of languages that have one), or standing alone. */
static HRESULT month_name(void *iface, BOOL alone, INT32 length, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	const CALTYPE form = alone ? 0 : CAL_RETURN_GENITIVE_NAMES;
	return name_out(self, (CAL_SMONTHNAME1 + fields.month - 1) | form, (CAL_SABBREVMONTHNAME1 + fields.month - 1) | form, length, out);
}

static HRESULT WINAPI calendar_month_full(void *iface, HSTRING *out) { return month_name(iface, FALSE, 80, out); }
static HRESULT WINAPI calendar_month_text(void *iface, INT32 length, HSTRING *out) { return month_name(iface, FALSE, length, out); }
static HRESULT WINAPI calendar_month_full_solo(void *iface, HSTRING *out) { return month_name(iface, TRUE, 80, out); }
static HRESULT WINAPI calendar_month_solo(void *iface, INT32 length, HSTRING *out) { return month_name(iface, TRUE, length, out); }

static HRESULT WINAPI calendar_month_numeric(void *iface, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	return number_out(self, fields.month, 0, 0, out);
}

static HRESULT WINAPI calendar_month_padded(void *iface, INT32 digits, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	return number_out(self, fields.month, 0, digits, out);
}

/* Weeks and days move the date and keep the local time of day. */
static HRESULT add_days(struct calendar *self, INT64 days)
{
	struct fields fields;
	fields_of(self, &fields);
	if (days > 3700000 || days < -3700000)
		return E_INVALIDARG;
	const INT64 local = join(&fields) + days * TICKS_PER_DAY;
	if (local < lowest() || local > highest())
		return E_INVALIDARG;
	split(local, &fields);
	return set_fields(self, &fields);
}

static HRESULT WINAPI calendar_add_weeks(void *iface, INT32 value) { return add_days(calendar_of(iface), value * 7LL); }
static HRESULT WINAPI calendar_first(void *iface, INT32 *out) { return number_answer(out, 1); }

static HRESULT WINAPI calendar_last_day(void *iface, INT32 *out)
{
	struct fields fields;
	fields_of(calendar_of(iface), &fields);
	return number_answer(out, month_days(fields.year, fields.month));
}

static HRESULT WINAPI calendar_day(void *iface, INT32 *out)
{
	struct fields fields;
	fields_of(calendar_of(iface), &fields);
	return number_answer(out, fields.day);
}

static HRESULT WINAPI calendar_put_day(void *iface, INT32 value)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	if (value < 1 || value > month_days(fields.year, fields.month))
		return E_INVALIDARG;
	fields.day = value;
	return set_fields(self, &fields);
}

static HRESULT WINAPI calendar_add_days(void *iface, INT32 value) { return add_days(calendar_of(iface), value); }

static HRESULT WINAPI calendar_day_text(void *iface, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	return number_out(self, fields.day, 0, 0, out);
}

static HRESULT WINAPI calendar_day_padded(void *iface, INT32 digits, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	return number_out(self, fields.day, 0, digits, out);
}

static HRESULT WINAPI calendar_weekday(void *iface, INT32 *out)
{
	struct fields fields;
	fields_of(calendar_of(iface), &fields);
	return number_answer(out, fields.weekday);
}

/* The day of the week by name; Windows numbers its names from Monday. */
static HRESULT weekday_name(void *iface, INT32 length, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	const INT32 from_monday = (fields.weekday + 6) % 7;
	return name_out(self, CAL_SDAYNAME1 + from_monday, CAL_SABBREVDAYNAME1 + from_monday, length, out);
}

static HRESULT WINAPI calendar_weekday_full(void *iface, HSTRING *out) { return weekday_name(iface, 80, out); }
static HRESULT WINAPI calendar_weekday_text(void *iface, INT32 length, HSTRING *out) { return weekday_name(iface, length, out); }

/* The 12-hour clock splits the day into two periods, morning and afternoon; the 24-hour clock has one. */
static HRESULT WINAPI calendar_periods(void *iface, INT32 *out) { return number_answer(out, calendar_of(iface)->clock24 ? 1 : 2); }

static HRESULT WINAPI calendar_period(void *iface, INT32 *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	return number_answer(out, self->clock24 || fields.hour < 12 ? 1 : 2);
}

static HRESULT WINAPI calendar_put_period(void *iface, INT32 value)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	if (value < 1 || value > (self->clock24 ? 1 : 2))
		return E_INVALIDARG;
	if (!self->clock24)
		fields.hour = fields.hour % 12 + (value == 2 ? 12 : 0);
	return set_fields(self, &fields);
}

/* Periods, hours, minutes, seconds and nanoseconds move the moment itself, whatever the zone's clock does meanwhile. */
static HRESULT add_ticks(struct calendar *self, INT64 count, INT64 unit)
{
	if (count > highest() / unit || count < -(highest() / unit))
		return E_INVALIDARG;
	return move_to(self, self->moment + count * unit);
}

static HRESULT WINAPI calendar_add_periods(void *iface, INT32 value)
{
	struct calendar *self = calendar_of(iface);
	return add_ticks(self, value, (self->clock24 ? 24 : 12) * 3600 * TICKS_PER_SECOND);
}

static HRESULT WINAPI calendar_period_full(void *iface, HSTRING *out) { return period_out(calendar_of(iface), out); }
static HRESULT WINAPI calendar_period_text(void *iface, INT32 length, HSTRING *out)
{
	return length < 0 ? E_INVALIDARG : period_out(calendar_of(iface), out);
}

static HRESULT WINAPI calendar_first_hour(void *iface, INT32 *out) { return number_answer(out, calendar_of(iface)->clock24 ? 0 : 1); }
static HRESULT WINAPI calendar_last_hour(void *iface, INT32 *out) { return number_answer(out, calendar_of(iface)->clock24 ? 23 : 12); }
static HRESULT WINAPI calendar_hours(void *iface, INT32 *out) { return number_answer(out, calendar_of(iface)->clock24 ? 24 : 12); }

static HRESULT WINAPI calendar_hour(void *iface, INT32 *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	return number_answer(out, shown_hour(self, fields.hour));
}

static HRESULT WINAPI calendar_put_hour(void *iface, INT32 value)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	if (self->clock24 ? value < 0 || value > 23 : value < 1 || value > 12)
		return E_INVALIDARG;
	fields.hour = self->clock24 ? value : value % 12 + (fields.hour >= 12 ? 12 : 0);
	return set_fields(self, &fields);
}

static HRESULT WINAPI calendar_add_hours(void *iface, INT32 value) { return add_ticks(calendar_of(iface), value, 3600 * TICKS_PER_SECOND); }

static HRESULT WINAPI calendar_hour_text(void *iface, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	return number_out(self, shown_hour(self, fields.hour), 0, 0, out);
}

static HRESULT WINAPI calendar_hour_padded(void *iface, INT32 digits, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	return number_out(self, shown_hour(self, fields.hour), 0, digits, out);
}

/* Minutes and seconds: the field, set, added and written alone or padded. */
static HRESULT clock_field(void *iface, BOOL minute, INT32 *out)
{
	struct fields fields;
	fields_of(calendar_of(iface), &fields);
	return number_answer(out, minute ? fields.minute : fields.second);
}

static HRESULT put_clock_field(void *iface, BOOL minute, INT32 value)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	if (value < 0 || value > 59)
		return E_INVALIDARG;
	*(minute ? &fields.minute : &fields.second) = value;
	return set_fields(self, &fields);
}

static HRESULT clock_field_text(void *iface, BOOL minute, INT32 digits, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	return number_out(self, minute ? fields.minute : fields.second, 0, digits, out);
}

static HRESULT WINAPI calendar_minute(void *iface, INT32 *out) { return clock_field(iface, TRUE, out); }
static HRESULT WINAPI calendar_put_minute(void *iface, INT32 value) { return put_clock_field(iface, TRUE, value); }
static HRESULT WINAPI calendar_add_minutes(void *iface, INT32 value) { return add_ticks(calendar_of(iface), value, 60 * TICKS_PER_SECOND); }
static HRESULT WINAPI calendar_minute_text(void *iface, HSTRING *out) { return clock_field_text(iface, TRUE, 0, out); }
static HRESULT WINAPI calendar_minute_padded(void *iface, INT32 digits, HSTRING *out) { return clock_field_text(iface, TRUE, digits, out); }
static HRESULT WINAPI calendar_second(void *iface, INT32 *out) { return clock_field(iface, FALSE, out); }
static HRESULT WINAPI calendar_put_second(void *iface, INT32 value) { return put_clock_field(iface, FALSE, value); }
static HRESULT WINAPI calendar_add_seconds(void *iface, INT32 value) { return add_ticks(calendar_of(iface), value, TICKS_PER_SECOND); }
static HRESULT WINAPI calendar_second_text(void *iface, HSTRING *out) { return clock_field_text(iface, FALSE, 0, out); }
static HRESULT WINAPI calendar_second_padded(void *iface, INT32 digits, HSTRING *out) { return clock_field_text(iface, FALSE, digits, out); }

/* Nanoseconds, in the hundreds a DateTime holds. */
static HRESULT WINAPI calendar_nanosecond(void *iface, INT32 *out)
{
	struct fields fields;
	fields_of(calendar_of(iface), &fields);
	return number_answer(out, fields.ticks * 100);
}

static HRESULT WINAPI calendar_put_nanosecond(void *iface, INT32 value)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	if (value < 0 || value > 999999999)
		return E_INVALIDARG;
	fields.ticks = value / 100;
	return set_fields(self, &fields);
}

static HRESULT WINAPI calendar_add_nanoseconds(void *iface, INT32 value) { return add_ticks(calendar_of(iface), value / 100, 1); }

static HRESULT WINAPI calendar_nanosecond_text(void *iface, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	return number_out(self, fields.ticks * 100LL, 0, 0, out);
}

static HRESULT WINAPI calendar_nanosecond_padded(void *iface, INT32 digits, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	struct fields fields;
	fields_of(self, &fields);
	return number_out(self, fields.ticks * 100LL, 0, digits, out);
}

static struct calendar *ours(void *iface)
{
	return iface && owner(iface)->kind == &calendar_kind ? calendar_of(iface) : NULL;
}

static HRESULT order_answer(INT64 left, INT64 right, INT32 *out) { return number_answer(out, left < right ? -1 : left > right ? 1 : 0); }

static HRESULT WINAPI calendar_compare(void *iface, void *other, INT32 *out)
{
	const struct calendar *that = ours(other);
	return that ? order_answer(calendar_of(iface)->moment, that->moment, out) : E_INVALIDARG;
}

static HRESULT WINAPI calendar_compare_moment(void *iface, INT64 other, INT32 *out) { return order_answer(calendar_of(iface)->moment, other, out); }

static HRESULT WINAPI calendar_copy_to(void *iface, void *other)
{
	struct calendar *that = ours(other), *self = calendar_of(iface);
	if (!that)
		return E_INVALIDARG;
	if (that != self) {
		const struct object base = that->base;
		*that = *self;
		that->base = base;
	}
	return S_OK;
}

static HRESULT WINAPI calendar_zero(void *iface, INT32 *out) { return number_answer(out, 0); }
static HRESULT WINAPI calendar_fifty_nine(void *iface, INT32 *out) { return number_answer(out, 59); }
static HRESULT WINAPI calendar_sixty(void *iface, INT32 *out) { return number_answer(out, 60); }
static HRESULT WINAPI calendar_resolved_language(void *iface, HSTRING *out) { return text_out(calendar_of(iface)->languages[0], out); }

static BOOL daylight(const struct calendar *self)
{
	return !self->universal && to_local(self, self->moment) - self->moment != standard_offset();
}

static HRESULT WINAPI calendar_daylight(void *iface, boolean *out) { return answer_flag(out, daylight(calendar_of(iface))); }

/* ---- ITimeZoneOnCalendar ---- */

static HRESULT WINAPI zone_get(void *iface, HSTRING *out) { return text_out(calendar_of(iface)->zone, out); }

static HRESULT WINAPI zone_change(void *iface, HSTRING value)
{
	struct calendar *self = calendar_of(iface);
	const WCHAR *name = WindowsGetStringRawBuffer(value, NULL);
	WCHAR local[64];
	DYNAMIC_TIME_ZONE_INFORMATION zone;
	local_zone_name(local, ARRAYSIZE(local));
	ZeroMemory(&zone, sizeof zone);
	GetDynamicTimeZoneInformation(&zone);
	if (!name || !*name || lstrlenW(name) >= (int)ARRAYSIZE(self->zone))
		return E_INVALIDARG;
	if (universal_name(name))
		self->universal = TRUE;
	else if (!lstrcmpiW(name, local) || !lstrcmpiW(name, zone.TimeZoneKeyName))
		self->universal = FALSE;
	else {
		trace("Calendar.ChangeTimeZone", "a zone other than the user's or UTC");
		return E_INVALIDARG;
	}
	lstrcpyW(self->zone, name);
	return S_OK;
}

static HRESULT WINAPI zone_full(void *iface, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	TIME_ZONE_INFORMATION zone;
	if (self->universal)
		return text_out(L"Coordinated Universal Time", out);
	GetTimeZoneInformation(&zone);
	return text_out(daylight(self) ? zone.DaylightName : zone.StandardName, out);
}

/* The short name of a zone: its offset from GMT, with the minutes when there are any. */
static HRESULT WINAPI zone_text(void *iface, INT32 length, HSTRING *out)
{
	struct calendar *self = calendar_of(iface);
	const INT64 minutes = (to_local(self, self->moment) - self->moment) / (60 * TICKS_PER_SECOND);
	const INT64 size = minutes < 0 ? -minutes : minutes;
	WCHAR text[24] = L"GMT";
	int at = 3;
	if (length < 0)
		return E_INVALIDARG;
	if (minutes) {
		text[at++] = minutes < 0 ? L'-' : L'+';
		if (size / 60 >= 10)
			text[at++] = (WCHAR)(L'0' + size / 600);
		text[at++] = (WCHAR)(L'0' + size / 60 % 10);
		if (size % 60) {
			text[at++] = L':';
			text[at++] = (WCHAR)(L'0' + size % 60 / 10);
			text[at++] = (WCHAR)(L'0' + size % 10);
		}
		text[at] = 0;
	}
	return text_out(text, out);
}

TABLE(calendar_table, 0, INSPECTABLE, (const void *)calendar_clone, (const void *)calendar_to_min, (const void *)calendar_to_max,
	(const void *)calendar_languages, (const void *)calendar_numerals, (const void *)calendar_set_numerals, (const void *)calendar_get_system,
	(const void *)calendar_change_system, (const void *)calendar_get_clock, (const void *)calendar_change_clock,
	(const void *)calendar_get_moment, (const void *)calendar_set_moment, (const void *)calendar_to_now,
	/* Eras. */
	(const void *)calendar_one, (const void *)calendar_one, (const void *)calendar_one, (const void *)calendar_one,
	(const void *)calendar_put_era, (const void *)calendar_add_eras, (const void *)calendar_era_full, (const void *)calendar_era_text,
	/* Years. */
	(const void *)calendar_first, (const void *)calendar_last_year, (const void *)calendar_last_year, (const void *)calendar_year,
	(const void *)calendar_put_year, (const void *)calendar_add_years, (const void *)calendar_year_text, (const void *)calendar_year_truncated,
	(const void *)calendar_year_padded,
	/* Months. */
	(const void *)calendar_first, (const void *)calendar_twelve, (const void *)calendar_twelve, (const void *)calendar_month,
	(const void *)calendar_put_month, (const void *)calendar_add_months, (const void *)calendar_month_full, (const void *)calendar_month_text,
	(const void *)calendar_month_full_solo, (const void *)calendar_month_solo, (const void *)calendar_month_numeric,
	(const void *)calendar_month_padded, (const void *)calendar_add_weeks,
	/* Days and days of the week. */
	(const void *)calendar_first, (const void *)calendar_last_day, (const void *)calendar_last_day, (const void *)calendar_day,
	(const void *)calendar_put_day, (const void *)calendar_add_days, (const void *)calendar_day_text, (const void *)calendar_day_padded,
	(const void *)calendar_weekday, (const void *)calendar_weekday_full, (const void *)calendar_weekday_text,
	(const void *)calendar_weekday_full, (const void *)calendar_weekday_text,
	/* Periods. */
	(const void *)calendar_first, (const void *)calendar_periods, (const void *)calendar_periods, (const void *)calendar_period,
	(const void *)calendar_put_period, (const void *)calendar_add_periods, (const void *)calendar_period_full,
	(const void *)calendar_period_text,
	/* Hours, minutes, seconds and nanoseconds. */
	(const void *)calendar_first_hour, (const void *)calendar_last_hour, (const void *)calendar_hours, (const void *)calendar_hour,
	(const void *)calendar_put_hour, (const void *)calendar_add_hours, (const void *)calendar_hour_text, (const void *)calendar_hour_padded,
	(const void *)calendar_minute, (const void *)calendar_put_minute, (const void *)calendar_add_minutes, (const void *)calendar_minute_text,
	(const void *)calendar_minute_padded, (const void *)calendar_second, (const void *)calendar_put_second,
	(const void *)calendar_add_seconds, (const void *)calendar_second_text, (const void *)calendar_second_padded,
	(const void *)calendar_nanosecond, (const void *)calendar_put_nanosecond, (const void *)calendar_add_nanoseconds,
	(const void *)calendar_nanosecond_text, (const void *)calendar_nanosecond_padded,
	/* Comparing and copying, and the ends of an hour and of a minute. */
	(const void *)calendar_compare, (const void *)calendar_compare_moment, (const void *)calendar_copy_to, (const void *)calendar_zero,
	(const void *)calendar_fifty_nine, (const void *)calendar_sixty, (const void *)calendar_zero, (const void *)calendar_fifty_nine,
	(const void *)calendar_sixty, (const void *)calendar_resolved_language, (const void *)calendar_daylight);
TABLE(zone_table, 1, INSPECTABLE, (const void *)zone_get, (const void *)zone_change, (const void *)zone_full, (const void *)zone_text);
static const struct face_iid calendar_iids[] = {{&iid_calendar, 0}, {&iid_time_zone, 1}, {NULL, 0}};
static void destroy_calendar(struct object *base) {}
static const struct kind calendar_kind = {"Calendar", L"Windows.Globalization.Calendar", calendar_iids, destroy_calendar};

/* A new calendar: a copy of the model, or, without one, the user's languages, clock and zone at the present moment. */
static HRESULT make_calendar(const struct calendar *model, void **out)
{
	static const void *const *const tables[] = {calendar_table.slots, zone_table.slots};
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct calendar *self = make(sizeof *self, &calendar_kind, tables, 2);
	if (!self)
		return E_OUTOFMEMORY;
	if (model) {
		const struct object base = self->base;
		*self = *model;
		self->base = base;
	} else {
		HRESULT result = language_list(NULL, self->languages, &self->language_count);
		if (FAILED(result)) {
			release(face_of(&self->base, 0));
			return result;
		}
		UINT32 hours = 0;
		GetLocaleInfoEx(self->languages[0], LOCALE_ITIME | LOCALE_RETURN_NUMBER, (WCHAR *)&hours, sizeof hours / sizeof(WCHAR));
		self->clock24 = hours != 0;
		numeral_default(self->languages[0], self->numerals);
		local_zone_name(self->zone, ARRAYSIZE(self->zone));
		self->moment = now();
	}
	*out = face_of(&self->base, 0);
	return S_OK;
}

/* A calendar for the languages given, with the calendar system and clock named or the user's, and the zone named or the user's. */
static HRESULT create(void *languages, HSTRING system, HSTRING clock, HSTRING zone, void **out)
{
	void *made = NULL;
	if (out)
		*out = NULL;
	if (!languages)
		return E_INVALIDARG;
	HRESULT result = make_calendar(NULL, &made);
	if (FAILED(result))
		return result;
	struct calendar *self = calendar_of(made);
	result = language_list(languages, self->languages, &self->language_count);
	if (SUCCEEDED(result)) {
		UINT32 hours = 0;
		GetLocaleInfoEx(self->languages[0], LOCALE_ITIME | LOCALE_RETURN_NUMBER, (WCHAR *)&hours, sizeof hours / sizeof(WCHAR));
		self->clock24 = hours != 0;
		numeral_default(self->languages[0], self->numerals);
	}
	if (SUCCEEDED(result) && system)
		result = calendar_system(WindowsGetStringRawBuffer(system, NULL));
	if (SUCCEEDED(result) && clock)
		result = clock_of(WindowsGetStringRawBuffer(clock, NULL), &self->clock24);
	if (SUCCEEDED(result) && zone)
		result = zone_change(face_of(&self->base, 1), zone);
	if (FAILED(result)) {
		release(made);
		return result;
	}
	*out = made;
	return S_OK;
}

static HRESULT WINAPI calendar_activate(void *iface, IInspectable **out) { return make_calendar(NULL, (void **)out); }
static HRESULT WINAPI create_default(void *iface, void *languages, void **out) { return create(languages, NULL, NULL, NULL, out); }
static HRESULT WINAPI create_with(void *iface, void *languages, HSTRING system, HSTRING clock, void **out)
{
	return system && clock ? create(languages, system, clock, NULL, out) : E_INVALIDARG;
}
static HRESULT WINAPI create_in_zone(void *iface, void *languages, HSTRING system, HSTRING clock, HSTRING zone, void **out)
{
	return system && clock && zone ? create(languages, system, clock, zone, out) : E_INVALIDARG;
}

TABLE(calendar_class_table, 0, INSPECTABLE, (const void *)calendar_activate);
TABLE(calendar_factory_table, 1, INSPECTABLE, (const void *)create_default, (const void *)create_with);
TABLE(calendar_factory2_table, 2, INSPECTABLE, (const void *)create_in_zone);
static const struct face_iid calendar_class_iids[] = {{&iid_factory, 0}, {&iid_calendar_factory, 1}, {&iid_calendar_factory2, 2}, {NULL, 0}};
static const struct kind calendar_class_kind = {"Calendar", L"Windows.Globalization.Calendar", calendar_class_iids, NULL};
struct object calendar_class = {{calendar_class_table.slots, calendar_factory_table.slots, calendar_factory2_table.slots},
	&calendar_class_kind, 1};
