/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "oa.h"
#include "number.h"

/* A DATE counts days from 30 December 1899. Below zero the whole part goes back in time while
 * the fraction still moves forward through the day. */
#define DAY_ZERO 2415019
#define SECONDS_A_DAY 86400.0

static int julian_of(int year, int month, int day)
{
	int a = (14 - month) / 12, y = year + 4800 - a, m = month + 12 * a - 3;
	return day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
}

static void civil_of(int julian, int *year, int *month, int *day)
{
	int a = julian + 32044, b = (4 * a + 3) / 146097, c = a - 146097 * b / 4;
	int d = (4 * c + 3) / 1461, e = c - 1461 * d / 4, m = (5 * e + 2) / 153;
	*day = e - (153 * m + 2) / 5 + 1;
	*month = m + 3 - 12 * (m / 10);
	*year = 100 * b + d - 4800 + m / 10;
}

/* Two digit years belong to 1950..2049, as the default calendar window has it. */
static int full_year(int year)
{
	if (year >= 0 && year < 50)
		return year + 2000;
	if (year >= 50 && year < 100)
		return year + 1900;
	return year;
}

static BOOL date_is_real(int year, int month, int day)
{
	static const BYTE lengths[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	BOOL leap;
	if (month < 1 || month > 12 || day < 1 || day > 31 || year < 100 || year > 9999)
		return FALSE;
	leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	return day <= lengths[month - 1] && !(month == 2 && day == 29 && !leap);
}

HRESULT WINAPI VarDateFromUdateEx(UDATE *in, LCID lcid, ULONG flags, DATE *out)
{
	int year, month, day;
	double fraction;
	if (!in || !out)
		return E_INVALIDARG;
	if (lcid != MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT))
		flags |= 0;
	year = (SHORT)in->st.wYear;
	/* Months out of range roll over into neighbouring years. */
	month = (SHORT)in->st.wMonth;
	if (!(flags & VAR_VALIDDATE) || 1)
	{
		while (month > 12)
		{
			month -= 12;
			year++;
		}
		while (month < 1)
		{
			month += 12;
			year--;
		}
	}
	year = year < 0 && year > -100 ? 2000 + year : full_year(year);
	day = (SHORT)in->st.wDay;
	fraction = ((SHORT)in->st.wHour * 3600.0 + (SHORT)in->st.wMinute * 60.0 + (SHORT)in->st.wSecond) / SECONDS_A_DAY;
	if (year < 100 || year > 9999)
		return E_INVALIDARG;
	{
		double base = julian_of(year, month, 1) + day - 1 - DAY_ZERO;
		double whole = base + (fraction < 0 ? -(double)(LONGLONG)(-fraction) - 1.0 : (double)(LONGLONG)fraction);
		fraction -= (double)(LONGLONG)fraction;
		if (fraction < 0)
			fraction += 1.0;
		if (flags & VAR_TIMEVALUEONLY)
			whole = 0;
		else if (flags & VAR_DATEVALUEONLY)
			fraction = 0;
		*out = whole < 0 ? whole - fraction : whole + fraction;
		if (*out < -657434.0 || *out >= 2958466.0)
			return E_INVALIDARG;
	}
	return S_OK;
}

HRESULT WINAPI VarDateFromUdate(UDATE *in, ULONG flags, DATE *out)
{
	return VarDateFromUdateEx(in, LOCALE_USER_DEFAULT, flags, out);
}

HRESULT WINAPI VarUdateFromDate(DATE in, ULONG flags, UDATE *out)
{
	double whole, fraction, seconds;
	int year, month, day, julian, weekday;
	LONGLONG total;
	if (!out)
		return E_INVALIDARG;
	if (in < -657434.0 || in >= 2958466.0)
		return E_INVALIDARG;
	whole = (double)(LONGLONG)in;
	fraction = in - whole;
	if (fraction < 0)
		fraction = -fraction;
	/* Half a second up, as times round to whole seconds. */
	seconds = fraction * SECONDS_A_DAY + 0.5;
	total = (LONGLONG)seconds;
	if (total >= 86400)
	{
		/* The very last moment has no next day to round up into. */
		if (whole >= 2958465.0)
			total = 86399;
		else
		{
			total -= 86400;
			whole += in < 0 ? -1.0 : 1.0;
		}
	}
	julian = (int)whole + DAY_ZERO;
	civil_of(julian, &year, &month, &day);
	weekday = (julian + 1) % 7;
	out->st.wYear = (WORD)year;
	out->st.wMonth = (WORD)month;
	out->st.wDay = (WORD)day;
	out->st.wDayOfWeek = (WORD)weekday;
	out->st.wHour = (WORD)(total / 3600);
	out->st.wMinute = (WORD)((total / 60) % 60);
	out->st.wSecond = (WORD)(total % 60);
	out->st.wMilliseconds = 0;
	out->wDayOfYear = (USHORT)(julian - julian_of(year, 1, 1) + 1);
	return S_OK;
}

INT WINAPI SystemTimeToVariantTime(LPSYSTEMTIME in, DOUBLE *out)
{
	UDATE parts;
	if (!in || !out || in->wMonth > 12 || in->wYear > 9999 || in->wDay > 31)
		return FALSE;
	parts.st = *in;
	parts.wDayOfYear = 0;
	return SUCCEEDED(VarDateFromUdate(&parts, 0, out));
}

INT WINAPI VariantTimeToSystemTime(DOUBLE in, LPSYSTEMTIME out)
{
	UDATE parts;
	if (!out || FAILED(VarUdateFromDate(in, 0, &parts)))
		return FALSE;
	*out = parts.st;
	return TRUE;
}

INT WINAPI DosDateTimeToVariantTime(USHORT dos_date, USHORT dos_time, DOUBLE *out)
{
	UDATE parts;
	if (!out)
		return FALSE;
	parts.st.wYear = (dos_date >> 9) + 1980;
	parts.st.wMonth = (dos_date >> 5) & 15;
	parts.st.wDay = dos_date & 31;
	parts.st.wHour = dos_time >> 11;
	parts.st.wMinute = (dos_time >> 5) & 63;
	parts.st.wSecond = (dos_time & 31) * 2;
	parts.st.wMilliseconds = 0;
	parts.st.wDayOfWeek = 0;
	parts.wDayOfYear = 0;
	if (parts.st.wYear > 2099 || parts.st.wMonth > 12 || parts.st.wDay > 31 || parts.st.wHour > 23 || parts.st.wMinute > 59 || parts.st.wSecond > 59)
		return FALSE;
	return SUCCEEDED(VarDateFromUdate(&parts, 0, out));
}

INT WINAPI VariantTimeToDosDateTime(DOUBLE in, USHORT *dos_date, USHORT *dos_time)
{
	UDATE parts;
	if (FAILED(VarUdateFromDate(in, 0, &parts)) || parts.st.wYear < 1980 || parts.st.wYear > 2099)
		return FALSE;
	*dos_date = (USHORT)(parts.st.wDay | parts.st.wMonth << 5 | (parts.st.wYear - 1980) << 9);
	*dos_time = (USHORT)(parts.st.wSecond / 2 | parts.st.wMinute << 5 | parts.st.wHour << 11);
	return TRUE;
}

/* --- Text out: the short date, and the time, by locale. --- */

HRESULT WINAPI VarBstrFromDate(DATE in, LCID lcid, ULONG flags, BSTR *out)
{
	UDATE parts;
	WCHAR text[128];
	int used = 0;
	BOOL has_date, has_time;
	DWORD override = flags & LOCALE_NOUSEROVERRIDE;
	HRESULT result;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	result = VarUdateFromDate(in, 0, &parts);
	if (FAILED(result))
		return DISP_E_OVERFLOW;
	has_date = (LONGLONG)in != 0;
	has_time = in != (double)(LONGLONG)in;
	if (flags & VAR_DATEVALUEONLY)
	{
		has_date = TRUE;
		has_time = FALSE;
	}
	else if (flags & VAR_TIMEVALUEONLY)
	{
		has_date = FALSE;
		has_time = TRUE;
	}
	else if (flags & VARIANT_USE_NLS)
		has_date = has_time = TRUE;
	else if (!has_date && !has_time)
		has_time = TRUE;
	if (has_date)
	{
		used = GetDateFormatW(lcid, override | DATE_SHORTDATE, &parts.st, NULL, text, ARRAYSIZE(text));
		if (!used)
			return E_INVALIDARG;
		used--;
	}
	if (has_time)
	{
		int more;
		if (used)
			text[used++] = L' ';
		more = GetTimeFormatW(lcid, override, &parts.st, NULL, text + used, ARRAYSIZE(text) - used);
		if (!more)
			return E_INVALIDARG;
		used += more - 1;
	}
	*out = SysAllocStringLen(text, used);
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* --- Text in. Tokens first: numbers, month names, day names, AM and PM; then the numbers are
 * read as date or time by what separates them and by the locale's date order. --- */

static BOOL word_is(const WCHAR *at, const WCHAR *word, int *used)
{
	int index = 0;
	if (!word[0])
		return FALSE;
	while (word[index])
	{
		if (!at[index] || CharUpperW((WCHAR *)(ULONG_PTR)at[index]) != CharUpperW((WCHAR *)(ULONG_PTR)word[index]))
			return FALSE;
		index++;
	}
	*used = index;
	return TRUE;
}

static BOOL is_letter(WCHAR c)
{
	return IsCharAlphaW(c);
}

/* A month by its long or short name in the locale, or English. */
static int month_word(const WCHAR *at, LCID lcid, ULONG flags, int *used)
{
	static const WCHAR english[12][10] = {L"january", L"february", L"march", L"april", L"may", L"june", L"july", L"august", L"september", L"october", L"november", L"december"};
	WCHAR name[80];
	int month;
	DWORD override = flags & LOCALE_NOUSEROVERRIDE;
	for (month = 0; month < 12; month++)
	{
		if (GetLocaleInfoW(lcid, override | (LOCALE_SMONTHNAME1 + month), name, ARRAYSIZE(name)) && word_is(at, name, used))
			return month + 1;
	}
	for (month = 0; month < 12; month++)
		if (word_is(at, english[month], used))
			return month + 1;
	for (month = 0; month < 12; month++)
	{
		if (GetLocaleInfoW(lcid, override | (LOCALE_SABBREVMONTHNAME1 + month), name, ARRAYSIZE(name)) && word_is(at, name, used))
			return month + 1;
	}
	for (month = 0; month < 12; month++)
	{
		WCHAR brief[4] = {english[month][0], english[month][1], english[month][2], 0};
		if (word_is(at, brief, used))
			return month + 1;
	}
	return 0;
}

static BOOL day_word(const WCHAR *at, LCID lcid, ULONG flags, int *used)
{
	WCHAR name[80];
	int day;
	DWORD override = flags & LOCALE_NOUSEROVERRIDE;
	for (day = 0; day < 7; day++)
		if (GetLocaleInfoW(lcid, override | (LOCALE_SDAYNAME1 + day), name, ARRAYSIZE(name)) && word_is(at, name, used))
			return TRUE;
	for (day = 0; day < 7; day++)
		if (GetLocaleInfoW(lcid, override | (LOCALE_SABBREVDAYNAME1 + day), name, ARRAYSIZE(name)) && word_is(at, name, used))
			return TRUE;
	return FALSE;
}

typedef struct
{
	int value[6];
	WCHAR after[6];
	int count;
	int month_name;
	int month_slot;
	int ampm;
	BOOL has_date_separator;
} date_tokens;

static HRESULT read_tokens(const WCHAR *text, LCID lcid, ULONG flags, date_tokens *tokens)
{
	const WCHAR *at = text;
	WCHAR date_sep[8], time_sep[8], am[16], pm[16];
	DWORD override = flags & LOCALE_NOUSEROVERRIDE;
	memset(tokens, 0, sizeof(*tokens));
	if (!GetLocaleInfoW(lcid, override | LOCALE_SDATE, date_sep, ARRAYSIZE(date_sep)))
		date_sep[0] = L'/', date_sep[1] = 0;
	if (!GetLocaleInfoW(lcid, override | LOCALE_STIME, time_sep, ARRAYSIZE(time_sep)))
		time_sep[0] = L':', time_sep[1] = 0;
	if (!GetLocaleInfoW(lcid, override | LOCALE_S1159, am, ARRAYSIZE(am)))
		am[0] = 0;
	if (!GetLocaleInfoW(lcid, override | LOCALE_S2359, pm, ARRAYSIZE(pm)))
		pm[0] = 0;
	while (*at)
	{
		int used = 0, month;
		if (*at >= L'0' && *at <= L'9')
		{
			int value = 0;
			if (tokens->count >= (int)ARRAYSIZE(tokens->value))
				return DISP_E_TYPEMISMATCH;
			while (*at >= L'0' && *at <= L'9')
			{
				value = value * 10 + (*at - L'0');
				if (value > 100000)
					return DISP_E_TYPEMISMATCH;
				at++;
			}
			tokens->value[tokens->count] = value;
			tokens->after[tokens->count] = 0;
			tokens->count++;
			if (*at == L':' || (*at == time_sep[0] && !time_sep[1]))
			{
				tokens->after[tokens->count - 1] = L':';
				at++;
			}
			else if (*at == L'.' && date_sep[0] != L'.' && at[1] >= L'0' && at[1] <= L'9')
			{
				/* A dot between numbers may still turn out to be part of a date. */
				tokens->after[tokens->count - 1] = L'.';
				at++;
			}
			continue;
		}
		if (is_letter(*at))
		{
			if (word_is(at, am, &used) || word_is(at, L"am", &used) || word_is(at, L"a", &used))
			{
				if (!is_letter(at[used]))
				{
					tokens->ampm = 1;
					if (tokens->count && !tokens->after[tokens->count - 1] && !(tokens->count > 1 && tokens->after[tokens->count - 2]))
						tokens->after[tokens->count - 1] = L'H';
					at += used;
					continue;
				}
			}
			if (word_is(at, pm, &used) || word_is(at, L"pm", &used) || word_is(at, L"p", &used))
			{
				if (!is_letter(at[used]))
				{
					tokens->ampm = 2;
					if (tokens->count && !tokens->after[tokens->count - 1] && !(tokens->count > 1 && tokens->after[tokens->count - 2]))
						tokens->after[tokens->count - 1] = L'H';
					at += used;
					continue;
				}
			}
			month = month_word(at, lcid, flags, &used);
			if (month && !tokens->month_name)
			{
				tokens->month_name = month;
				tokens->month_slot = tokens->count;
				at += used;
				if (*at == L'.')
					at++;
				continue;
			}
			if (day_word(at, lcid, flags, &used))
			{
				at += used;
				if (*at == L'.')
					at++;
				continue;
			}
			return DISP_E_TYPEMISMATCH;
		}
		/* A minus only joins parts of a date, it never starts or ends one. */
		if (*at == L'-' && (!at[1] || (!tokens->count && !tokens->month_name)))
			return DISP_E_TYPEMISMATCH;
		if (*at == date_sep[0] || *at == L'/' || *at == L'-' || *at == L'.')
			tokens->has_date_separator = TRUE;
		else if (*at != L' ' && *at != L',' && *at != 0xa0 && *at != 0x3000 && *at != L'\t')
			return DISP_E_TYPEMISMATCH;
		at++;
	}
	return tokens->count || tokens->month_name ? S_OK : DISP_E_TYPEMISMATCH;
}

/* 0 is month first, 1 day first, 2 year first; as the locale's short date says. */
static int date_order(LCID lcid, ULONG flags)
{
	WCHAR text[4];
	if (GetLocaleInfoW(lcid, (flags & LOCALE_NOUSEROVERRIDE) | LOCALE_IDATE, text, ARRAYSIZE(text)) >= 2)
		return text[0] - L'0';
	return 0;
}

HRESULT WINAPI VarDateFromStr(LPCOLESTR text, LCID lcid, ULONG flags, DATE *out)
{
	date_tokens tokens;
	int date_part[3], date_count = 0, time_part[3] = {0, 0, 0}, time_count = 0, index;
	int year, month, day, order;
	double fraction, whole;
	SYSTEMTIME now;
	HRESULT result;
	if (!text)
		return DISP_E_TYPEMISMATCH;
	if (!out)
		return E_INVALIDARG;
	*out = 0.0;
	result = read_tokens(text, lcid, flags, &tokens);
	if (FAILED(result))
		return result;
	/* A dotted group is a time unless a real time exists or its numbers cannot be one. */
	{
		BOOL colon = FALSE, dots_are_time = TRUE;
		int place = 0;
		for (index = 0; index < tokens.count; index++)
		{
			if (tokens.after[index] == L':')
				colon = TRUE;
			if (tokens.after[index] == L'.' || (index && tokens.after[index - 1] == L'.'))
			{
				int limit = (!index || tokens.after[index - 1] != L'.') ? 23 : 59;
				if (tokens.value[index] > limit)
					dots_are_time = FALSE;
				/* Dots cannot carry a year unless the locale writes dates with them. */
				if (tokens.value[index] > 99)
					return DISP_E_TYPEMISMATCH;
				place++;
			}
		}
		(void)place;
		if (colon || !dots_are_time)
			for (index = 0; index < tokens.count; index++)
				if (tokens.after[index] == L'.')
				{
					tokens.after[index] = 0;
					tokens.has_date_separator = TRUE;
				}
	}
	/* All of the time must be one group, and date numbers cannot stand on both sides of it. */
	{
		int groups = 0, before = 0, after = 0;
		BOOL inside = FALSE;
		for (index = 0; index < tokens.count; index++)
		{
			BOOL in_time = tokens.after[index] == L':' || tokens.after[index] == L'.' || tokens.after[index] == L'H' ||
				(index && (tokens.after[index - 1] == L':' || tokens.after[index - 1] == L'.'));
			if (in_time && !inside)
				groups++;
			else if (!in_time)
			{
				if (groups)
					after++;
				else
					before++;
			}
			inside = in_time && tokens.after[index] != L'H';
		}
		if (groups > 1 || (before && after))
			return DISP_E_TYPEMISMATCH;
	}
	for (index = 0; index < tokens.count; index++)
	{
		BOOL in_time = tokens.after[index] == L':' || tokens.after[index] == L'.' || tokens.after[index] == L'H' ||
			(index && (tokens.after[index - 1] == L':' || tokens.after[index - 1] == L'.'));
		if (in_time)
		{
			if (time_count >= 3)
				return DISP_E_TYPEMISMATCH;
			time_part[time_count++] = tokens.value[index];
		}
		else
		{
			if (date_count >= 3)
				return DISP_E_TYPEMISMATCH;
			date_part[date_count++] = tokens.value[index];
		}
	}
	if (tokens.ampm)
	{
		if (!time_count)
			return DISP_E_TYPEMISMATCH;
		/* A marker closes a dotted time, so that time cannot also lead date numbers. */
		for (index = 0; index < tokens.count; index++)
			if (tokens.after[index] == L'.' && time_count == 3 && date_count)
				return DISP_E_TYPEMISMATCH;
		if (time_part[0] <= 12)
		{
			if (tokens.ampm == 2 && time_part[0] < 12)
				time_part[0] += 12;
			else if (tokens.ampm == 1 && time_part[0] == 12)
				time_part[0] = 0;
		}
	}
	if (time_part[0] > 23 || time_part[1] > 59 || time_part[2] > 59)
		return DISP_E_TYPEMISMATCH;
	fraction = (time_part[0] * 3600.0 + time_part[1] * 60.0 + time_part[2]) / SECONDS_A_DAY;
	whole = 0.0;
	order = date_order(lcid, flags);
	GetLocalTime(&now);
	if (date_count || tokens.month_name)
	{
		year = now.wYear;
		if (tokens.month_name)
		{
			month = tokens.month_name;
			if (date_count == 0)
				return DISP_E_TYPEMISMATCH;
			if (date_count == 1)
			{
				/* "Jan 2000" names a year; "Jan 5" a day. */
				if (date_part[0] > 31)
				{
					year = date_part[0];
					day = 1;
				}
				else
					day = date_part[0];
			}
			else
			{
				if (tokens.month_slot == 0 || date_part[0] <= 31)
				{
					day = date_part[0];
					year = date_part[1];
				}
				else
				{
					year = date_part[0];
					day = date_part[1];
				}
			}
		}
		else if (date_count == 1)
			return DISP_E_TYPEMISMATCH;
		else if (date_count == 2)
		{
			/* Month and day if they can be, in the locale's order first; else month and year. */
			int first = order == 1 ? date_part[1] : date_part[0], second = order == 1 ? date_part[0] : date_part[1];
			day = 1;
			if (date_is_real(year, first, second))
			{
				month = first;
				day = second;
			}
			else if (date_is_real(year, second, first))
			{
				month = second;
				day = first;
			}
			else if (first >= 1 && first <= 12 && second <= 99 + 9900)
			{
				month = first;
				year = second;
			}
			else if (second >= 1 && second <= 12)
			{
				month = second;
				year = first;
			}
			else
				return DISP_E_TYPEMISMATCH;
		}
		else
		{
			/* The locale's reading first, then the others that make a real date. */
			static const BYTE readings[3][3][3] = {
				{{2, 0, 1}, {0, 1, 2}, {2, 1, 0}},
				{{2, 1, 0}, {0, 1, 2}, {2, 0, 1}},
				{{0, 1, 2}, {2, 0, 1}, {2, 1, 0}},
			};
			const BYTE (*tried)[3] = readings[order >= 0 && order <= 2 ? order : 0];
			int attempt;
			for (attempt = 0; attempt < 3; attempt++)
			{
				/* Each entry says which slot holds year, month, day. */
				year = full_year(date_part[tried[attempt][0]]);
				month = date_part[tried[attempt][1]];
				day = date_part[tried[attempt][2]];
				if (date_is_real(year, month, day))
					break;
			}
			if (attempt == 3)
				return DISP_E_TYPEMISMATCH;
		}
		year = full_year(year);
		if (!date_is_real(year, month, day))
			return DISP_E_TYPEMISMATCH;
		whole = julian_of(year, month, day) - DAY_ZERO;
	}
	if (flags & VAR_TIMEVALUEONLY)
		whole = 0.0;
	if (flags & VAR_DATEVALUEONLY)
		fraction = 0.0;
	*out = whole < 0 ? whole - fraction : whole + fraction;
	return S_OK;
}
