/* SPDX-License-Identifier: MIT */
#include "js.h"
#include <math.h>

/* Date. A time is milliseconds since 1970 in UTC, as ECMAScript defines it; local time comes from the time zone of Windows. */

#define MS_PER_DAY 86400000.0
#define MAX_TIME 8.64e15

static double day_of(double t) { return floor(t / MS_PER_DAY); }

static double time_in_day(double t)
{
	double r = fmod(t, MS_PER_DAY);
	return r < 0 ? r + MS_PER_DAY : r;
}

static double days_in_year(double y)
{
	if (fmod(y, 4) != 0)
		return 365;
	if (fmod(y, 100) != 0)
		return 366;
	return fmod(y, 400) != 0 ? 365 : 366;
}

static double day_from_year(double y) { return 365 * (y - 1970) + floor((y - 1969) / 4) - floor((y - 1901) / 100) + floor((y - 1601) / 400); }

static double year_from_time(double t)
{
	double y = floor(t / (MS_PER_DAY * 365.2425)) + 1970;
	while (day_from_year(y) * MS_PER_DAY > t)
		y--;
	while (day_from_year(y + 1) * MS_PER_DAY <= t)
		y++;
	return y;
}

static const int month_starts[2][13] = {
	{ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
	{ 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 },
};

/* The year, month (0 to 11), date (1 to 31) and weekday (0 is Sunday) of a time. */
static void split_day(double t, double *year, int *month, int *date, int *weekday)
{
	double y = year_from_time(t);
	int leap = days_in_year(y) == 366;
	int in_year = (int)(day_of(t) - day_from_year(y));
	int m = 0;
	while (m < 11 && in_year >= month_starts[leap][m + 1])
		m++;
	*year = y;
	*month = m;
	*date = in_year - month_starts[leap][m] + 1;
	double w = fmod(day_of(t) + 4, 7);
	*weekday = (int)(w < 0 ? w + 7 : w);
}

static double make_time(double hour, double minute, double second, double ms)
{
	if (!js_is_finite(hour) || !js_is_finite(minute) || !js_is_finite(second) || !js_is_finite(ms))
		return js_nan();
	return js_integer(hour) * 3600000.0 + js_integer(minute) * 60000.0 + js_integer(second) * 1000.0 + js_integer(ms);
}

static double make_day(double year, double month, double date)
{
	if (!js_is_finite(year) || !js_is_finite(month) || !js_is_finite(date))
		return js_nan();
	year = js_integer(year);
	month = js_integer(month);
	double y = year + floor(month / 12), m = fmod(month, 12);
	if (m < 0)
		m += 12;
	if (fabs(y) > 400000)
		return js_nan();
	return day_from_year(y) + month_starts[days_in_year(y) == 366][(int)m] + js_integer(date) - 1;
}

static double make_date(double day, double time)
{
	if (!js_is_finite(day) || !js_is_finite(time))
		return js_nan();
	return day * MS_PER_DAY + time;
}

static double time_clip(double t)
{
	if (!js_is_finite(t) || fabs(t) > MAX_TIME)
		return js_nan();
	return js_integer(t) + 0.0;
}

/* The local offset from UTC at a UTC time, in milliseconds. Times outside what SYSTEMTIME holds take the offset of a year with the
 * same calendar inside it. */
static double local_offset(double t)
{
	if (js_is_nan(t))
		return 0;
	double year = year_from_time(t);
	double shift = 0;
	if (year < 1602 || year > 30000) {
		/* 28 years repeat the weekdays and the leap years within a century; 400 years repeat the calendar entirely. */
		double target = 2000 + fmod(year - 2000, 400);
		if (target < 2000)
			target += 400;
		shift = (day_from_year(target) - day_from_year(year)) * MS_PER_DAY;
	}
	double shifted = t + shift;
	ULONGLONG ticks = (ULONGLONG)((shifted + 11644473600000.0) * 10000.0);
	FILETIME ft = { (DWORD)ticks, (DWORD)(ticks >> 32) };
	SYSTEMTIME utc, local;
	if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(NULL, &utc, &local)) {
		TIME_ZONE_INFORMATION zone;
		GetTimeZoneInformation(&zone);
		return -zone.Bias * 60000.0;
	}
	FILETIME lft;
	SystemTimeToFileTime(&local, &lft);
	ULONGLONG local_ticks = ((ULONGLONG)lft.dwHighDateTime << 32) | lft.dwLowDateTime;
	return ((double)local_ticks - (double)ticks) / 10000.0;
}

static double local_time(double t) { return t + local_offset(t); }

static double utc_of_local(double t)
{
	if (!js_is_finite(t))
		return js_nan();
	double guess = t - local_offset(t);
	return t - local_offset(guess);
}

double js_date_now(void)
{
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	ULONGLONG ticks = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	return floor((double)ticks / 10000.0 - 11644473600000.0);
}

/* VT_DATE is days since 30 December 1899 in local time, the fraction the time of day, counted away from zero before that day. */
double js_from_var_date(double date)
{
	double days = date < 0 ? ceil(date) : floor(date);
	double fraction = fabs(date - days);
	double local = (days - 25569) * MS_PER_DAY + floor(fraction * MS_PER_DAY + 0.5);
	return time_clip(utc_of_local(local));
}

double js_to_var_date(double time)
{
	if (js_is_nan(time))
		return 0;
	double local = local_time(time);
	double days = day_of(local) + 25569, fraction = time_in_day(local) / MS_PER_DAY;
	return days < 0 ? days - fraction : days + fraction;
}

static const WCHAR *const day_names[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
static const WCHAR *const month_names[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };

/* A year as JScript writes it: B.C. after years before the first. */
static void year_text(double year, WCHAR *out, UINT size)
{
	if (year > 0)
		wsprintfW(out, L"%d", (int)year);
	else
		wsprintfW(out, L"%d B.C.", (int)(1 - year));
	(void)size;
}

static void zone_text(double offset, WCHAR *out)
{
	int minutes = (int)(offset / 60000);
	if (!minutes) {
		lstrcpyW(out, L"UTC");
		return;
	}
	int a = minutes < 0 ? -minutes : minutes;
	wsprintfW(out, L"UTC%c%02d%02d", minutes < 0 ? L'-' : L'+', a / 60, a % 60);
}

static BOOL system_time_of(double local, SYSTEMTIME *st)
{
	double year;
	int month, date, weekday;
	split_day(local, &year, &month, &date, &weekday);
	if (year < 1601 || year > 30827)
		return FALSE;
	double ms = time_in_day(local);
	st->wYear = (WORD)year;
	st->wMonth = (WORD)(month + 1);
	st->wDay = (WORD)date;
	st->wDayOfWeek = (WORD)weekday;
	st->wHour = (WORD)(ms / 3600000);
	st->wMinute = (WORD)(fmod(ms, 3600000) / 60000);
	st->wSecond = (WORD)(fmod(ms, 60000) / 1000);
	st->wMilliseconds = (WORD)fmod(ms, 1000);
	return TRUE;
}

/* which: 0 toString, 1 toDateString, 2 toTimeString, 3 toUTCString, 4 toLocaleString, 5 toLocaleDateString, 6 toLocaleTimeString. */
js_string *js_date_string(double time, int which)
{
	if (js_is_nan(time))
		return js_string_of(L"NaN");
	double offset = which == 3 ? 0 : local_offset(time), t = time + offset;
	if (which >= 4) {
		SYSTEMTIME st;
		if (system_time_of(t, &st)) {
			WCHAR date[128] = L"", clock[64] = L"", text[200];
			if (which != 6)
				GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &st, NULL, date, ARRAYSIZE(date));
			if (which != 5)
				GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &st, NULL, clock, ARRAYSIZE(clock));
			wsprintfW(text, L"%s%s%s", date, which == 4 ? L" " : L"", clock);
			return js_string_of(text);
		}
		which = which == 4 ? 0 : which == 5 ? 1 : 2;
	}
	double year;
	int month, date, weekday;
	split_day(t, &year, &month, &date, &weekday);
	double ms = time_in_day(t);
	int hour = (int)(ms / 3600000), minute = (int)(fmod(ms, 3600000) / 60000), second = (int)(fmod(ms, 60000) / 1000);
	WCHAR years[32], zone[16], text[160];
	year_text(year, years, ARRAYSIZE(years));
	zone_text(offset, zone);
	switch (which) {
	case 1:
		wsprintfW(text, L"%s %s %d %s", day_names[weekday], month_names[month], date, years);
		break;
	case 2:
		wsprintfW(text, L"%02d:%02d:%02d %s", hour, minute, second, zone);
		break;
	case 3:
		wsprintfW(text, L"%s, %d %s %s %02d:%02d:%02d UTC", day_names[weekday], date, month_names[month], years, hour, minute, second);
		break;
	default:
		wsprintfW(text, L"%s %s %d %02d:%02d:%02d %s %s", day_names[weekday], month_names[month], date, hour, minute, second, zone, years);
		break;
	}
	return js_string_of(text);
}

/* Date.parse: the forms JScript writes, and the usual others: month names, m/d/y, h:m:s, AM and PM, a zone, B.C. */
static BOOL word_is(const WCHAR *word, UINT length, const WCHAR *name, UINT shortest)
{
	UINT n = lstrlenW(name);
	if (length < shortest || length > n)
		return FALSE;
	for (UINT i = 0; i < length; i++)
		if ((word[i] | 0x20) != (name[i] | 0x20))
			return FALSE;
	return TRUE;
}

double js_date_parse(const js_string *text)
{
	static const WCHAR *const long_months[] = { L"january", L"february", L"march", L"april", L"may", L"june", L"july", L"august",
		L"september", L"october", L"november", L"december" };
	static const WCHAR *const long_days[] = { L"sunday", L"monday", L"tuesday", L"wednesday", L"thursday", L"friday", L"saturday" };
	static const struct {
		const WCHAR *name;
		int offset;
	} zones[] = {
		{ L"UTC", 0 }, { L"UT", 0 }, { L"GMT", 0 }, { L"Z", 0 }, { L"EST", -300 }, { L"EDT", -240 }, { L"CST", -360 }, { L"CDT", -300 },
		{ L"MST", -420 }, { L"MDT", -360 }, { L"PST", -480 }, { L"PDT", -420 },
	};
	double year = js_nan(), month = js_nan(), date = js_nan();
	double hour = 0, minute = 0, second = 0;
	BOOL zoned = FALSE, after_zone = FALSE, pm = FALSE, am = FALSE, bc = FALSE;
	int zone = 0;
	const WCHAR *c = text->text, *end = text->text + text->length;
	while (c < end) {
		if (*c == L'(') {
			/* A comment, as in "(Pacific Standard Time)". */
			int depth = 0;
			do {
				depth += *c == L'(' ? 1 : *c == L')' ? -1 : 0;
				c++;
			} while (c < end && depth);
			continue;
		}
		if ((*c | 0x20) >= L'a' && (*c | 0x20) <= L'z') {
			const WCHAR *w = c;
			while (c < end && (((*c | 0x20) >= L'a' && (*c | 0x20) <= L'z') || *c == L'.'))
				c++;
			UINT n = (UINT)(c - w);
			BOOL known = FALSE;
			for (int i = 0; i < 12 && !known; i++)
				if (word_is(w, n, long_months[i], 3)) {
					month = i;
					known = TRUE;
				}
			for (int i = 0; i < 7 && !known; i++)
				known = word_is(w, n, long_days[i], 3);
			if (!known && (word_is(w, n, L"am", 2) || word_is(w, n, L"a.m.", 4)))
				known = am = TRUE;
			if (!known && (word_is(w, n, L"pm", 2) || word_is(w, n, L"p.m.", 4)))
				known = pm = TRUE;
			if (!known && (word_is(w, n, L"bc", 2) || word_is(w, n, L"b.c.", 4)))
				known = bc = TRUE;
			if (!known && (word_is(w, n, L"ad", 2) || word_is(w, n, L"a.d.", 4)))
				known = TRUE;
			for (UINT i = 0; i < ARRAYSIZE(zones) && !known; i++)
				if (n == (UINT)lstrlenW(zones[i].name) && word_is(w, n, zones[i].name, n)) {
					zone = zones[i].offset;
					known = zoned = after_zone = TRUE;
				}
			if (!known)
				return js_nan();
			continue;
		}
		if ((*c == L'+' || *c == L'-') && after_zone && c + 1 < end && c[1] >= L'0' && c[1] <= L'9') {
			/* UTC+0100 or GMT-5. */
			BOOL negative = *c++ == L'-';
			int value = 0, digits = 0;
			while (c < end && *c >= L'0' && *c <= L'9')
				value = value * 10 + (*c++ - L'0'), digits++;
			int minutes = digits <= 2 ? value * 60 : (value / 100) * 60 + value % 100;
			zone += negative ? -minutes : minutes;
			after_zone = FALSE;
			continue;
		}
		if (*c >= L'0' && *c <= L'9') {
			double value = 0;
			while (c < end && *c >= L'0' && *c <= L'9')
				value = value * 10 + (*c++ - L'0');
			after_zone = FALSE;
			if (c < end && *c == L':') {
				/* h:m[:s] */
				hour = value;
				c++;
				minute = 0;
				while (c < end && *c >= L'0' && *c <= L'9')
					minute = minute * 10 + (*c++ - L'0');
				if (c < end && *c == L':') {
					c++;
					second = 0;
					while (c < end && *c >= L'0' && *c <= L'9')
						second = second * 10 + (*c++ - L'0');
				}
				continue;
			}
			if (c < end && (*c == L'/' || (*c == L'-' && js_is_nan(month)))) {
				/* m/d[/y], or y-m-d when the first number is a year. */
				WCHAR separator = *c++;
				double parts[3] = { value, js_nan(), js_nan() };
				for (int k = 1; k < 3; k++) {
					if (c >= end || *c < L'0' || *c > L'9')
						break;
					double part = 0;
					while (c < end && *c >= L'0' && *c <= L'9')
						part = part * 10 + (*c++ - L'0');
					parts[k] = part;
					if (k < 2 && c < end && *c == separator)
						c++;
					else
						break;
				}
				if (separator == L'-' && parts[0] > 31) {
					year = parts[0];
					month = parts[1] - 1;
					date = js_is_nan(parts[2]) ? 1 : parts[2];
				} else {
					month = parts[0] - 1;
					date = parts[1];
					if (!js_is_nan(parts[2]))
						year = parts[2];
				}
				continue;
			}
			if (js_is_nan(date) && value <= 31 && !js_is_nan(month))
				date = value;
			else if (js_is_nan(date) && value <= 31 && js_is_nan(year))
				date = value;
			else if (js_is_nan(year))
				year = value;
			else
				return js_nan();
			continue;
		}
		if (*c == L' ' || *c == L',' || *c == L'\t' || *c == L'-' || *c == L'.' || *c == L'/' || *c == L'\r' || *c == L'\n') {
			c++;
			continue;
		}
		return js_nan();
	}
	if (js_is_nan(year) || js_is_nan(month) || js_is_nan(date))
		return js_nan();
	if (year < 100 && !bc)
		year += 1900;
	if (bc)
		year = 1 - year;
	if (pm && hour < 12)
		hour += 12;
	if (am && hour == 12)
		hour = 0;
	double t = make_date(make_day(year, month, date), make_time(hour, minute, second, 0));
	t = zoned ? t - zone * 60000.0 : utc_of_local(t);
	return time_clip(t);
}

/* The Date object. */

static BOOL this_date(engine *en, js_value self, js_object **out)
{
	if (self.type == JV_OBJECT && self.object->cls == JC_DATE) {
		*out = self.object;
		return TRUE;
	}
	*out = NULL;
	return js_throw_error(en, L"TypeError", JS_E_DATE_EXPECTED, NULL);
}

/* The time of the arguments year, month[, date, hours, minutes, seconds, ms], in local time or UTC. */
static BOOL time_of_parts(engine *en, UINT argc, js_value *argv, BOOL utc, double *out)
{
	double parts[7] = { 0, 0, 1, 0, 0, 0, 0 };
	for (UINT i = 0; i < argc && i < 7; i++)
		if (!js_to_number(en, argv[i], &parts[i]))
			return FALSE;
	if (!js_is_nan(parts[0])) {
		double y = js_integer(parts[0]);
		if (y >= 0 && y <= 99)
			parts[0] = 1900 + y;
	}
	double t = make_date(make_day(parts[0], parts[1], parts[2]), make_time(parts[3], parts[4], parts[5], parts[6]));
	*out = time_clip(utc ? t : utc_of_local(t));
	return TRUE;
}

static BOOL date_ctor(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	if (!(self.type == JV_OBJECT && self.object == callee)) {
		*result = js_str(js_date_string(js_date_now(), 0));
		return TRUE;
	}
	double t;
	if (!argc)
		t = js_date_now();
	else if (argc == 1) {
		js_value v = argv[0];
		if (v.type == JV_DATE)
			t = v.number;
		else if (v.type == JV_OBJECT && v.object->cls == JC_DATE)
			t = v.object->primitive.number;
		else {
			js_value p;
			if (!js_to_primitive(en, v, HINT_NONE, &p))
				return FALSE;
			if (p.type == JV_STRING)
				t = js_date_parse(p.string);
			else if (!js_to_number(en, p, &t)) {
				js_release(p);
				return FALSE;
			}
			js_release(p);
			t = time_clip(t);
		}
	} else if (!time_of_parts(en, argc, argv, FALSE, &t))
		return FALSE;
	js_object *o = js_object_new(en, JC_DATE, en->date_prototype);
	o->primitive = js_number(t);
	*result = js_obj(o);
	return TRUE;
}

static BOOL date_parse(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_string *s;
	if (!js_to_string(en, js_arg(argc, argv, 0), &s))
		return FALSE;
	*result = js_number(js_date_parse(s));
	js_string_release(s);
	return TRUE;
}

static BOOL date_utc(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	double t;
	if (!time_of_parts(en, argc, argv, TRUE, &t))
		return FALSE;
	*result = js_number(t);
	return TRUE;
}

static BOOL date_text(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	if (!this_date(en, self, &o))
		return FALSE;
	*result = js_str(js_date_string(o->primitive.number, (int)(ULONG_PTR)callee->native_data));
	return TRUE;
}

static BOOL date_value(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	if (!this_date(en, self, &o))
		return FALSE;
	*result = js_number(o->primitive.number);
	return TRUE;
}

static BOOL date_var_date(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	if (!this_date(en, self, &o))
		return FALSE;
	js_value v = { JV_DATE };
	v.number = o->primitive.number;
	*result = v;
	return TRUE;
}

static BOOL date_zone_offset(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	if (!this_date(en, self, &o))
		return FALSE;
	double t = o->primitive.number;
	*result = js_number(js_is_nan(t) ? t : -local_offset(t) / 60000);
	return TRUE;
}

/* The fields a getter or setter works on, in order: year, month, date, hours, minutes, seconds, milliseconds; 7 is the weekday. */
#define FIELD_UTC 0x10
#define FIELD_SHORT_YEAR 0x20

static void fields_of(double t, double *f)
{
	double year;
	int month, date, weekday;
	split_day(t, &year, &month, &date, &weekday);
	double ms = time_in_day(t);
	f[0] = year;
	f[1] = month;
	f[2] = date;
	f[3] = floor(ms / 3600000);
	f[4] = floor(fmod(ms, 3600000) / 60000);
	f[5] = floor(fmod(ms, 60000) / 1000);
	f[6] = fmod(ms, 1000);
	f[7] = weekday;
}

static BOOL date_get(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	if (!this_date(en, self, &o))
		return FALSE;
	UINT which = (UINT)(ULONG_PTR)callee->native_data;
	double t = o->primitive.number;
	if (js_is_nan(t)) {
		*result = js_number(t);
		return TRUE;
	}
	double f[8];
	fields_of(which & FIELD_UTC ? t : local_time(t), f);
	double value = f[which & 7];
	/* getYear gives the years of the twentieth century as two digits, and the others whole. */
	if ((which & FIELD_SHORT_YEAR) && value >= 1900 && value < 2000)
		value -= 1900;
	*result = js_number(value);
	return TRUE;
}

static BOOL date_set(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	if (!this_date(en, self, &o))
		return FALSE;
	UINT which = (UINT)(ULONG_PTR)callee->native_data, field = which & 7;
	BOOL utc = (which & FIELD_UTC) != 0;
	/* setFullYear takes year, month, date; setHours hours, minutes, seconds, ms; and so on down. */
	UINT most = field < 3 ? 3 - field : 7 - field;
	if (!argc)
		return js_throw_error(en, L"TypeError", JS_E_INVALID_CALL, NULL);
	double t = o->primitive.number;
	if (js_is_nan(t)) {
		if (field)
			return *result = js_number(t), TRUE;
		t = 0;
	} else if (!utc)
		t = local_time(t);
	double f[8];
	fields_of(t, f);
	for (UINT i = 0; i < argc && i < most; i++)
		if (!js_to_number(en, argv[i], &f[field + i]))
			return FALSE;
	if ((which & FIELD_SHORT_YEAR) && !js_is_nan(f[0]) && js_integer(f[0]) >= 0 && js_integer(f[0]) <= 99)
		f[0] = 1900 + js_integer(f[0]);
	double n = make_date(make_day(f[0], f[1], f[2]), make_time(f[3], f[4], f[5], f[6]));
	n = time_clip(utc ? n : utc_of_local(n));
	o->primitive = js_number(n);
	*result = js_number(n);
	return TRUE;
}

static BOOL date_set_time(engine *en, js_value self, UINT argc, js_value *argv, js_value *result, js_object *callee)
{
	js_object *o;
	double t;
	if (!this_date(en, self, &o) || !js_to_number(en, js_arg(argc, argv, 0), &t))
		return FALSE;
	o->primitive = js_number(time_clip(t));
	*result = js_number(o->primitive.number);
	return TRUE;
}

void js_init_date(engine *en)
{
	js_object *proto = en->date_prototype;
	proto->primitive = js_number(js_nan());
	js_object *ctor = js_define_native(en->global, L"Date", date_ctor, 7);
	js_define_text(ctor, L"prototype", js_obj(js_object_retain(proto)), JP_HIDDEN | JP_READONLY);
	js_define_text(proto, L"constructor", js_obj(js_object_retain(ctor)), JP_DONTENUM);
	js_define_native(ctor, L"parse", date_parse, 1);
	js_define_native(ctor, L"UTC", date_utc, 7);
	static const struct {
		const WCHAR *name;
		int which;
	} texts[] = {
		{ L"toString", 0 }, { L"toDateString", 1 }, { L"toTimeString", 2 }, { L"toUTCString", 3 }, { L"toGMTString", 3 },
		{ L"toLocaleString", 4 }, { L"toLocaleDateString", 5 }, { L"toLocaleTimeString", 6 },
	};
	for (UINT i = 0; i < ARRAYSIZE(texts); i++)
		js_define_native(proto, texts[i].name, date_text, 0)->native_data = (const void *)(ULONG_PTR)texts[i].which;
	js_define_native(proto, L"valueOf", date_value, 0);
	js_define_native(proto, L"getTime", date_value, 0);
	js_define_native(proto, L"setTime", date_set_time, 1);
	js_define_native(proto, L"getVarDate", date_var_date, 0);
	js_define_native(proto, L"getTimezoneOffset", date_zone_offset, 0);
	static const struct {
		const WCHAR *name;
		UINT field;
	} fields[] = {
		{ L"FullYear", 0 }, { L"Month", 1 }, { L"Date", 2 }, { L"Hours", 3 }, { L"Minutes", 4 }, { L"Seconds", 5 }, { L"Milliseconds", 6 },
		{ L"Day", 7 },
	};
	for (UINT i = 0; i < ARRAYSIZE(fields); i++)
		for (UINT utc = 0; utc < 2; utc++) {
			WCHAR name[40];
			UINT which = fields[i].field | (utc ? FIELD_UTC : 0);
			wsprintfW(name, L"get%s%s", utc ? L"UTC" : L"", fields[i].name);
			js_define_native(proto, name, date_get, 0)->native_data = (const void *)(ULONG_PTR)which;
			if (fields[i].field == 7)
				continue;
			wsprintfW(name, L"set%s%s", utc ? L"UTC" : L"", fields[i].name);
			UINT most = fields[i].field < 3 ? 3 - fields[i].field : 7 - fields[i].field;
			js_define_native(proto, name, date_set, most)->native_data = (const void *)(ULONG_PTR)which;
		}
	js_define_native(proto, L"getYear", date_get, 0)->native_data = (const void *)(ULONG_PTR)(0 | FIELD_SHORT_YEAR);
	js_define_native(proto, L"setYear", date_set, 1)->native_data = (const void *)(ULONG_PTR)(0 | FIELD_SHORT_YEAR);
}
