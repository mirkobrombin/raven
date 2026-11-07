/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 *
 * The Format$ language. A format text is read at the moment it is used: tokenizing only checks
 * the arguments and keeps the text, so the opaque token buffer is simply that text with a header.
 */
#include <stddef.h>
#include "oa.h"
#include "number.h"

/* Growing text. */
struct text
{
	WCHAR *chars;
	SIZE_T used, room;
	BOOL failed;
};

static void text_add(struct text *text, WCHAR value)
{
	if (text->failed)
		return;
	if (text->used + 1 >= text->room)
	{
		SIZE_T room = text->room ? text->room * 2 : 128;
		WCHAR *grown = text->chars ? HeapReAlloc(GetProcessHeap(), 0, text->chars, room * sizeof(WCHAR)) : HeapAlloc(GetProcessHeap(), 0, room * sizeof(WCHAR));
		if (!grown)
		{
			text->failed = TRUE;
			return;
		}
		text->chars = grown;
		text->room = room;
	}
	text->chars[text->used++] = value;
}

static void text_add_string(struct text *text, const WCHAR *value)
{
	while (*value)
		text_add(text, *value++);
}

static void text_add_digits(struct text *text, int value, int width)
{
	WCHAR buffer[16];
	int count = 0;
	do
	{
		buffer[count++] = L'0' + value % 10;
		value /= 10;
	} while (value);
	while (width-- > count)
		text_add(text, L'0');
	while (count)
		text_add(text, buffer[--count]);
}

static HRESULT text_finish(struct text *text, BSTR *out)
{
	HRESULT status = S_OK;
	if (text->failed)
		status = E_OUTOFMEMORY;
	else
	{
		*out = SysAllocStringLen(text->chars ? text->chars : L"", (UINT)text->used);
		if (!*out)
			status = E_OUTOFMEMORY;
	}
	if (text->chars)
		HeapFree(GetProcessHeap(), 0, text->chars);
	return status;
}

static WCHAR locale_first(LCID lcid, LCTYPE type, ULONG flags, WCHAR fallback)
{
	WCHAR value[8];
	if (GetLocaleInfoW(lcid, type | (flags & LOCALE_NOUSEROVERRIDE), value, ARRAYSIZE(value)) > 1)
		return value[0];
	return fallback;
}

static int locale_number(LCID lcid, LCTYPE type, ULONG flags, int fallback)
{
	WCHAR value[8];
	int result = 0, index;
	if (GetLocaleInfoW(lcid, type | (flags & LOCALE_NOUSEROVERRIDE), value, ARRAYSIZE(value)) <= 1)
		return fallback;
	for (index = 0; value[index] >= L'0' && value[index] <= L'9'; index++)
		result = result * 10 + (value[index] - L'0');
	return result;
}

static BOOL same_word(const WCHAR *text, const WCHAR *word)
{
	while (*word)
	{
		WCHAR a = *text++, b = *word++;
		if (a >= L'A' && a <= L'Z')
			a += 32;
		if (b >= L'A' && b <= L'Z')
			b += 32;
		if (a != b)
			return FALSE;
	}
	return TRUE;
}

static BOOL is_word(const WCHAR *text, const WCHAR *word)
{
	return same_word(text, word) && !text[lstrlenW(word)];
}

/* The sections of a format are separated by semicolons outside of any quoting. */
struct section
{
	const WCHAR *start;
	const WCHAR *end;
};

static const WCHAR *skip_quoted(const WCHAR *at)
{
	if (*at == L'\\')
		return at[1] ? at + 2 : at + 1;
	if (*at == L'"')
	{
		at++;
		while (*at && *at != L'"')
			at++;
		return *at ? at + 1 : at;
	}
	return at + 1;
}

static int split_sections(const WCHAR *format, struct section *sections, int limit)
{
	const WCHAR *at = format;
	int count = 0;
	sections[0].start = format;
	while (*at)
	{
		if (*at == L';')
		{
			sections[count].end = at;
			if (++count == limit)
				return count;
			sections[count].start = at + 1;
			at++;
		}
		else
			at = skip_quoted(at);
	}
	sections[count].end = at;
	return count + 1;
}

enum format_kind
{
	KIND_NUMBER,
	KIND_DATE,
	KIND_STRING,
};

static BOOL starts_marker(const WCHAR *at)
{
	return same_word(at, L"am/pm") || same_word(at, L"a/p") || same_word(at, L"ampm");
}

/* What the first meaningful character asks for decides how the whole format reads. */
static enum format_kind classify(const WCHAR *start, const WCHAR *end)
{
	const WCHAR *at = start;
	while (at < end)
	{
		switch (*at)
		{
		case L'#':
		case L'0':
		case L'?':
		case L'.':
			return KIND_NUMBER;
		case L'@':
		case L'&':
		case L'<':
		case L'>':
		case L'!':
			return KIND_STRING;
		case L'd': case L'D': case L'm': case L'M': case L'y': case L'Y': case L'h': case L'H': case L'n': case L'N':
		case L's': case L'S': case L'w': case L'W': case L'q': case L'Q': case L'c': case L'C': case L't': case L'T':
			return KIND_DATE;
		case L'a':
		case L'A':
			if (starts_marker(at))
				return KIND_DATE;
			break;
		}
		at = skip_quoted(at);
	}
	return KIND_NUMBER;
}

/* Numbers as a run of decimal digits and the place of the point: 12.5 is 125 with point 2. */
struct digits
{
	BYTE value[64];
	int count, point;
	BOOL negative;
};

static void digits_trim(struct digits *number)
{
	int lead = 0;
	while (lead < number->count && !number->value[lead])
		lead++;
	if (lead)
	{
		memmove(number->value, number->value + lead, number->count - lead);
		number->count -= lead;
		number->point -= lead;
	}
	while (number->count && !number->value[number->count - 1])
		number->count--;
	if (!number->count)
		number->point = 0;
}

static HRESULT digits_of_variant(const VARIANT *value, struct digits *number)
{
	VARIANT wide;
	HRESULT status;
	memset(number, 0, sizeof(*number));
	if (V_VT(value) == VT_R4 || V_VT(value) == VT_R8 || V_VT(value) == VT_DATE)
	{
		double real = V_VT(value) == VT_R4 ? V_R4(value) : V_R8(value);
		int point = 0;
		number->negative = real < 0;
		number->count = (int)oa_digits_of_r8(number->negative ? -real : real, V_VT(value) == VT_R4 ? 7 : 15, number->value, &point);
		number->point = point;
		digits_trim(number);
		return S_OK;
	}
	V_VT(&wide) = VT_EMPTY;
	status = VariantChangeTypeEx(&wide, (VARIANT *)value, LOCALE_USER_DEFAULT, 0, VT_DECIMAL);
	if (FAILED(status))
	{
		V_VT(&wide) = VT_EMPTY;
		status = VariantChangeTypeEx(&wide, (VARIANT *)value, LOCALE_USER_DEFAULT, 0, VT_R8);
		if (FAILED(status))
			return status;
		return digits_of_variant(&wide, number);
	}
	{
		BSTR shown = NULL;
		const WCHAR *at;
		BOOL fraction = FALSE;
		DECIMAL plain = V_DECIMAL(&wide);
		number->negative = (DEC_SIGN(&plain) & DECIMAL_NEG) != 0;
		DEC_SIGN(&plain) = 0;
		status = VarBstrFromDec(&plain, MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT), LOCALE_NOUSEROVERRIDE, &shown);
		if (FAILED(status))
			return status;
		for (at = shown; *at; at++)
		{
			if (*at >= L'0' && *at <= L'9')
			{
				if (number->count < (int)ARRAYSIZE(number->value))
					number->value[number->count++] = (BYTE)(*at - L'0');
				if (!fraction)
					number->point++;
			}
			else
				fraction = TRUE;
		}
		SysFreeString(shown);
	}
	digits_trim(number);
	return S_OK;
}

/* Half up on the digits: keep only those down to the given place after the point. */
static void digits_round(struct digits *number, int places)
{
	int keep = number->point + places, index;
	if (keep >= number->count)
		return;
	if (keep < 0)
	{
		number->count = 0;
		number->point = 0;
		return;
	}
	if (number->value[keep] >= 5)
	{
		for (index = keep - 1; index >= 0; index--)
		{
			if (number->value[index] < 9)
			{
				number->value[index]++;
				break;
			}
			number->value[index] = 0;
		}
		if (index < 0)
		{
			memmove(number->value + 1, number->value, keep > 0 ? keep : 0);
			number->value[0] = 1;
			keep++;
			number->point++;
		}
	}
	number->count = keep;
	digits_trim(number);
}

static int digit_at(const struct digits *number, int place)
{
	/* Place counts from the point: 0 is the first after it, -1 the last before it. */
	int index = number->point + place;
	return index >= 0 && index < number->count ? number->value[index] : 0;
}

/* What a number format section contains. */
struct number_shape
{
	int whole, fraction, last_fraction_zero, percent, scale;
	BOOL grouped, has_exponent, has_point;
	int exponent_zeros;
};

static const WCHAR *exponent_at(const WCHAR *at, const WCHAR *end)
{
	if ((*at == L'e' || *at == L'E') && at + 1 < end && (at[1] == L'+' || at[1] == L'-'))
		return at + 2;
	return NULL;
}

static void read_shape(const WCHAR *start, const WCHAR *end, struct number_shape *shape)
{
	const WCHAR *at = start;
	memset(shape, 0, sizeof(*shape));
	shape->last_fraction_zero = -1;
	while (at < end)
	{
		const WCHAR *after;
		if (shape->has_exponent)
		{
			if (*at == L'0')
				shape->exponent_zeros++;
			else if (*at != L'#')
			{
				at = skip_quoted(at);
				continue;
			}
			at++;
			continue;
		}
		if ((after = exponent_at(at, end)))
		{
			shape->has_exponent = TRUE;
			at = after;
			continue;
		}
		switch (*at)
		{
		case L'0':
		case L'#':
		case L'?':
			if (shape->has_point)
			{
				if (*at == L'0')
					shape->last_fraction_zero = shape->fraction;
				shape->fraction++;
			}
			else
				shape->whole++;
			break;
		case L'.':
			shape->has_point = TRUE;
			break;
		case L'%':
			shape->percent++;
			break;
		case L',':
			if (!shape->has_point)
			{
				const WCHAR *next = at + 1;
				if (next < end && (*next == L'#' || *next == L'0' || *next == L'?' || *next == L' '))
					shape->grouped = TRUE;
				else if (next == end || *next == L'.' || *next == L',')
					shape->scale++;
			}
			break;
		}
		at = skip_quoted(at);
	}
}

static void add_literal(struct text *out, const WCHAR *at)
{
	if (*at == L'\\')
	{
		if (at[1])
			text_add(out, at[1]);
	}
	else if (*at == L'"')
	{
		at++;
		while (*at && *at != L'"')
			text_add(out, *at++);
	}
	else
		text_add(out, *at);
}

static HRESULT format_number(const WCHAR *start, const WCHAR *end, struct digits number, BOOL show_minus, LCID lcid, ULONG flags, struct text *out)
{
	struct number_shape shape;
	WCHAR decimal = locale_first(lcid, LOCALE_SDECIMAL, flags, L'.'), group = locale_first(lcid, LOCALE_STHOUSAND, flags, L',');
	const WCHAR *at = start;
	int exponent = 0, whole_seen = 0, fraction_seen = 0, digits_before, place;
	BOOL started = FALSE, in_exponent = FALSE, minus_pending;
	read_shape(start, end, &shape);
	number.point += shape.percent * 2 - shape.scale * 3;
	if (shape.has_exponent)
	{
		if (number.count)
		{
			/* The first digit fills the leftmost whole place, and the exponent makes up for it. */
			int shift = shape.whole ? shape.whole : 0;
			exponent = number.point - shift;
			number.point = shift;
		}
		digits_round(&number, shape.fraction);
		if (number.count && number.point > (shape.whole ? shape.whole : 0))
		{
			exponent += number.point - shape.whole;
			number.point = shape.whole;
		}
	}
	else
		digits_round(&number, shape.fraction);
	digits_before = number.point > 0 ? number.point : 0;
	minus_pending = show_minus && number.negative;
	if (minus_pending)
		text_add(out, L'-');
	while (at < end)
	{
		const WCHAR *after;
		if (in_exponent)
		{
			if (*at == L'0' || *at == L'#')
			{
				while (at < end && (*at == L'0' || *at == L'#'))
					at++;
				text_add_digits(out, exponent < 0 ? -exponent : exponent, shape.exponent_zeros);
				continue;
			}
		}
		else if ((after = exponent_at(at, end)))
		{
			text_add(out, *at);
			if (at[1] == L'+')
				text_add(out, exponent < 0 ? L'-' : L'+');
			else if (exponent < 0)
				text_add(out, L'-');
			in_exponent = TRUE;
			at = after;
			/* An exponent with only literals still shows its value first. */
			if (at >= end || (*at != L'0' && *at != L'#'))
				text_add_digits(out, exponent < 0 ? -exponent : exponent, shape.exponent_zeros);
			continue;
		}
		switch (*at)
		{
		case L'0':
		case L'#':
		case L'?':
			if (whole_seen < shape.whole)
			{
				int left = shape.whole - whole_seen, extra;
				/* Digits with no place of their own all come out at the first placeholder. */
				if (!whole_seen)
					for (extra = digits_before; extra > left; extra--)
					{
						text_add(out, L'0' + digit_at(&number, -extra));
						started = TRUE;
						if (shape.grouped && extra > 1 && !((extra - 1) % 3))
							text_add(out, group);
					}
				if (left <= digits_before)
				{
					text_add(out, L'0' + digit_at(&number, -left));
					started = TRUE;
				}
				else if (*at == L'0' || started)
				{
					text_add(out, L'0');
					started = TRUE;
				}
				else if (*at == L'?')
					text_add(out, L' ');
				if (started && shape.grouped && left > 1 && !((left - 1) % 3))
					text_add(out, group);
				whole_seen++;
			}
			else
			{
				place = fraction_seen;
				if (*at == L'0' || place <= shape.last_fraction_zero || digit_at(&number, place) || number.point + place + 1 < number.count)
					text_add(out, L'0' + digit_at(&number, place));
				fraction_seen++;
			}
			at++;
			break;
		case L'.':
			if (!shape.whole && digits_before)
			{
				/* No whole places at all, but digits still have to appear. */
				int extra;
				for (extra = digits_before; extra > 0; extra--)
					text_add(out, L'0' + digit_at(&number, -extra));
			}
			text_add(out, decimal);
			at++;
			break;
		case L',':
			at++;
			break;
		default:
			add_literal(out, at);
			at = skip_quoted(at);
			break;
		}
	}
	return S_OK;
}

/* Dates. */
enum date_piece
{
	PIECE_NONE,
	PIECE_DAY,
	PIECE_HOUR,
	PIECE_MINUTE,
	PIECE_MONTH,
	PIECE_OTHER,
};

static BOOL uses_twelve_hours(const WCHAR *start, const WCHAR *end)
{
	const WCHAR *at = start;
	while (at < end)
	{
		if (starts_marker(at))
			return TRUE;
		at = skip_quoted(at);
	}
	return FALSE;
}

static int run_of(const WCHAR *at, const WCHAR *end)
{
	WCHAR first = *at | 0x20;
	int count = 0;
	while (at + count < end && (at[count] | 0x20) == first)
		count++;
	return count;
}

static void add_system_date(struct text *out, const UDATE *date, DWORD which, LCID lcid)
{
	WCHAR buffer[128];
	if (GetDateFormatW(lcid, which, &date->st, NULL, buffer, ARRAYSIZE(buffer)))
		text_add_string(out, buffer);
}

static void add_system_time(struct text *out, const UDATE *date, DWORD which, LCID lcid)
{
	WCHAR buffer[128];
	if (GetTimeFormatW(lcid, which, &date->st, NULL, buffer, ARRAYSIZE(buffer)))
		text_add_string(out, buffer);
}

static void add_locale_name(struct text *out, LCID lcid, LCTYPE type, ULONG flags)
{
	WCHAR buffer[64];
	if (GetLocaleInfoW(lcid, type | (flags & LOCALE_NOUSEROVERRIDE), buffer, ARRAYSIZE(buffer)))
		text_add_string(out, buffer);
}

static void add_general_date(struct text *out, double value, const UDATE *date, LCID lcid)
{
	double fraction = value - (double)(LONGLONG)value;
	if ((LONGLONG)value)
		add_system_date(out, date, DATE_SHORTDATE, lcid);
	if (fraction != 0.0)
	{
		if ((LONGLONG)value)
			text_add(out, L' ');
		add_system_time(out, date, 0, lcid);
	}
}

static HRESULT format_date(const WCHAR *start, const WCHAR *end, double value, int first_day, LCID lcid, ULONG flags, struct text *out)
{
	UDATE date;
	const WCHAR *at = start;
	enum date_piece last = PIECE_NONE;
	BOOL twelve = uses_twelve_hours(start, end);
	HRESULT status = VarUdateFromDate(value, 0, &date);
	if (FAILED(status))
		return status;
	if (!first_day)
		first_day = (locale_number(lcid, LOCALE_IFIRSTDAYOFWEEK, flags, 6) + 1) % 7 + 1;
	while (at < end)
	{
		int run;
		WCHAR lower = *at | 0x20;
		if (starts_marker(at))
		{
			BOOL afternoon = date.st.wHour >= 12;
			if (same_word(at, L"am/pm"))
			{
				const WCHAR *word = afternoon ? at + 3 : at;
				text_add(out, word[0]);
				text_add(out, word[1]);
				at += 5;
			}
			else if (same_word(at, L"a/p"))
			{
				text_add(out, afternoon ? at[2] : at[0]);
				at += 3;
			}
			else
			{
				add_locale_name(out, lcid, afternoon ? LOCALE_S2359 : LOCALE_S1159, flags);
				at += 4;
			}
			continue;
		}
		switch (lower)
		{
		case L'd':
			run = run_of(at, end);
			at += run;
			if (run <= 2)
				text_add_digits(out, date.st.wDay, run);
			else if (run <= 4)
			{
				/* The locale's names start from Monday, the weekday count from Sunday. */
				int monday = (date.st.wDayOfWeek + 6) % 7;
				add_locale_name(out, lcid, (run == 3 ? LOCALE_SABBREVDAYNAME1 : LOCALE_SDAYNAME1) + monday, flags);
			}
			else
				add_system_date(out, &date, run == 5 ? DATE_SHORTDATE : DATE_LONGDATE, lcid);
			last = PIECE_DAY;
			break;
		case L'h':
			run = run_of(at, end);
			at += run;
			text_add_digits(out, twelve && date.st.wHour % 12 ? date.st.wHour % 12 : twelve ? 12 : date.st.wHour, run >= 2 ? 2 : 1);
			last = PIECE_HOUR;
			break;
		case L'n':
			run = run_of(at, end);
			at += run;
			text_add_digits(out, date.st.wMinute, run >= 2 ? 2 : 1);
			last = PIECE_MINUTE;
			break;
		case L'm':
			run = run_of(at, end);
			at += run;
			if (last == PIECE_HOUR && run <= 2)
			{
				text_add_digits(out, date.st.wMinute, run);
				last = PIECE_MINUTE;
			}
			else
			{
				if (run <= 2)
					text_add_digits(out, date.st.wMonth, run);
				else
					add_locale_name(out, lcid, (run == 3 ? LOCALE_SABBREVMONTHNAME1 : LOCALE_SMONTHNAME1) + date.st.wMonth - 1, flags);
				last = PIECE_MONTH;
			}
			break;
		case L's':
			run = run_of(at, end);
			at += run;
			text_add_digits(out, date.st.wSecond, run >= 2 ? 2 : 1);
			last = PIECE_OTHER;
			break;
		case L'y':
			run = run_of(at, end);
			at += run;
			if (run == 1)
				text_add_digits(out, date.wDayOfYear, 1);
			else if (run == 2)
				text_add_digits(out, date.st.wYear % 100, 2);
			else if (run == 3)
				text_add_digits(out, date.wDayOfYear, 3);
			else
				text_add_digits(out, date.st.wYear, 4);
			last = PIECE_OTHER;
			break;
		case L'q':
			at++;
			text_add_digits(out, (date.st.wMonth - 1) / 3 + 1, 1);
			last = PIECE_OTHER;
			break;
		case L'w':
			run = run_of(at, end);
			at += run;
			if (run == 1)
				text_add_digits(out, (date.st.wDayOfWeek + 1 - first_day + 7) % 7 + 1, 1);
			else
				text_add_digits(out, (date.wDayOfYear - 1) / 7 + 1, 1);
			last = PIECE_OTHER;
			break;
		case L't':
			run = run_of(at, end);
			if (run >= 5)
			{
				add_system_time(out, &date, 0, lcid);
				at += run;
			}
			else
			{
				text_add(out, *at);
				at++;
			}
			break;
		case L'c':
			at++;
			add_general_date(out, value, &date, lcid);
			last = PIECE_OTHER;
			break;
		case L':':
			text_add(out, locale_first(lcid, LOCALE_STIME, flags, L':'));
			at++;
			break;
		case L'/':
			text_add(out, locale_first(lcid, LOCALE_SDATE, flags, L'/'));
			at++;
			break;
		default:
			add_literal(out, at);
			at = skip_quoted(at);
			break;
		}
	}
	return S_OK;
}

/* Strings: each placeholder takes one character, what remains follows at the very end. */
static HRESULT format_string(const WCHAR *start, const WCHAR *end, const WCHAR *value, UINT length, struct text *out)
{
	const WCHAR *at = start;
	int places = 0, blanks;
	UINT used = 0;
	WCHAR case_rule = 0;
	while (at < end)
	{
		if (*at == L'@' || *at == L'&')
			places++;
		else if ((*at == L'<' || *at == L'>') && !case_rule)
			case_rule = *at;
		at = skip_quoted(at);
	}
	blanks = places > (int)length ? places - (int)length : 0;
	at = start;
	while (at < end)
	{
		switch (*at)
		{
		case L'@':
		case L'&':
			if (blanks)
			{
				if (*at == L'@')
					text_add(out, L' ');
				blanks--;
			}
			else if (used < length)
				text_add(out, value[used++]);
			at++;
			break;
		case L'<':
		case L'>':
		case L'!':
			at++;
			break;
		default:
			add_literal(out, at);
			at = skip_quoted(at);
			break;
		}
	}
	while (used < length)
		text_add(out, value[used++]);
	if (case_rule && out->chars)
	{
		SIZE_T index;
		for (index = 0; index < out->used; index++)
			out->chars[index] = (WCHAR)(ULONG_PTR)(case_rule == L'<' ? CharLowerW((WCHAR *)(ULONG_PTR)out->chars[index]) : CharUpperW((WCHAR *)(ULONG_PTR)out->chars[index]));
	}
	return S_OK;
}

static BOOL value_is_true(const VARIANT *value)
{
	VARIANT state;
	V_VT(&state) = VT_EMPTY;
	if (FAILED(VariantChangeTypeEx(&state, (VARIANT *)value, LOCALE_USER_DEFAULT, 0, VT_BOOL)))
		return FALSE;
	return V_BOOL(&state) != VARIANT_FALSE;
}

static HRESULT as_text_value(const VARIANT *value, LCID lcid, BSTR *out)
{
	VARIANT shown;
	HRESULT status;
	V_VT(&shown) = VT_EMPTY;
	status = VariantChangeTypeEx(&shown, (VARIANT *)value, lcid, 0, VT_BSTR);
	if (SUCCEEDED(status))
		*out = V_BSTR(&shown);
	return status;
}

static HRESULT format_with(const WCHAR *format, const VARIANT *value, int first_day, LCID lcid, ULONG flags, struct text *out);

/* The formats known by name; each is another format, or says what to do by itself. */
static HRESULT format_named(const WCHAR *format, const VARIANT *value, int first_day, LCID lcid, ULONG flags, struct text *out, BOOL *done)
{
	static const struct
	{
		const WCHAR *name, *real;
	} simple[] = {
		{L"Percent", L"0.00%"},
		{L"Standard", L"#,##0.00"},
		{L"Fixed", L"0.00"},
		{L"Scientific", L"0.00E+00"},
		{L"Long Date", L"dddddd"},
		{L"Short Date", L"ddddd"},
		{L"Medium Date", L"dd-mmm-yy"},
		{L"Long Time", L"ttttt"},
		{L"Medium Time", L"hh:mm AMPM"},
		{L"Short Time", L"hh:mm"},
		{L"General Date", L"c"},
	};
	static const struct
	{
		const WCHAR *name, *yes, *no;
	} choice[] = {
		{L"True/False", L"True", L"False"},
		{L"On/Off", L"On", L"Off"},
		{L"Yes/No", L"Yes", L"No"},
	};
	unsigned index;
	*done = TRUE;
	if (is_word(format, L"General Number"))
		format = L"";
	for (index = 0; index < ARRAYSIZE(simple); index++)
		if (is_word(format, simple[index].name))
			return format_with(simple[index].real, value, first_day, lcid, flags, out);
	for (index = 0; index < ARRAYSIZE(choice); index++)
		if (is_word(format, choice[index].name))
		{
			text_add_string(out, value_is_true(value) ? choice[index].yes : choice[index].no);
			return S_OK;
		}
	if (is_word(format, L"Currency"))
	{
		BSTR shown = NULL;
		HRESULT status = VarFormatCurrency((VARIANT *)value, -1, -2, -2, -2, 0, &shown);
		if (SUCCEEDED(status))
		{
			text_add_string(out, shown);
			SysFreeString(shown);
		}
		return status;
	}
	if (!*format)
	{
		BSTR shown = NULL;
		HRESULT status = as_text_value(value, lcid, &shown);
		if (SUCCEEDED(status))
		{
			text_add_string(out, shown);
			SysFreeString(shown);
		}
		return status;
	}
	*done = FALSE;
	return S_OK;
}

static HRESULT format_with(const WCHAR *format, const VARIANT *value, int first_day, LCID lcid, ULONG flags, struct text *out)
{
	struct section sections[4];
	enum format_kind kind;
	BOOL done;
	int count, chosen = 0;
	HRESULT status = format_named(format, value, first_day, lcid, flags, out, &done);
	if (done)
		return status;
	count = split_sections(format, sections, ARRAYSIZE(sections));
	kind = classify(sections[0].start, sections[0].end);
	if (kind == KIND_STRING)
	{
		BSTR shown = NULL;
		status = as_text_value(value, lcid, &shown);
		if (SUCCEEDED(status))
		{
			status = format_string(sections[0].start, sections[0].end, shown, SysStringLen(shown), out);
			SysFreeString(shown);
		}
		return status;
	}
	if (kind == KIND_DATE)
	{
		VARIANT date;
		V_VT(&date) = VT_EMPTY;
		status = VariantChangeTypeEx(&date, (VARIANT *)value, lcid, 0, VT_DATE);
		if (FAILED(status))
			return status;
		return format_date(sections[0].start, sections[0].end, V_DATE(&date), first_day, lcid, flags, out);
	}
	{
		struct digits number;
		BOOL zero;
		status = digits_of_variant(value, &number);
		if (FAILED(status))
			return status;
		zero = !number.count;
		if (zero && count >= 3)
			chosen = 2;
		else if (number.negative && count >= 2)
			chosen = 1;
		return format_number(sections[chosen].start, sections[chosen].end, number, chosen == 0 && number.negative, lcid, flags, out);
	}
}

static HRESULT check_week_args(int first_day, int first_week)
{
	if (first_day < 0 || first_day > 7 || first_week < 0 || first_week > 3)
		return E_INVALIDARG;
	return S_OK;
}

static HRESULT format_to_bstr(VARIANT *from, const WCHAR *format, int first_day, int first_week, ULONG flags, LCID lcid, BSTR *out)
{
	VARIANT plain;
	struct text text = {NULL, 0, 0, FALSE};
	HRESULT status;
	if (!from || !out)
		return E_INVALIDARG;
	status = check_week_args(first_day, first_week);
	if (FAILED(status))
		return status;
	V_VT(&plain) = VT_EMPTY;
	status = VariantCopyInd(&plain, from);
	if (FAILED(status))
		return status;
	if (V_VT(&plain) == VT_NULL)
	{
		*out = NULL;
		return S_OK;
	}
	status = format_with(format ? format : L"", &plain, first_day, lcid, flags, &text);
	VariantClear(&plain);
	if (FAILED(status))
	{
		if (text.chars)
			HeapFree(GetProcessHeap(), 0, text.chars);
		return status;
	}
	return text_finish(&text, out);
}

HRESULT WINAPI VarFormat(LPVARIANT in, LPOLESTR format, int first_day, int first_week, ULONG flags, BSTR *out)
{
	return format_to_bstr(in, format, first_day, first_week, flags, LOCALE_USER_DEFAULT, out);
}

/* Tokens are this header and the format's own text. */
struct format_tokens
{
	DWORD magic;
	int first_day, first_week;
	LCID lcid;
	WCHAR text[1];
};

#define TOKENS_MAGIC 0x4f464d54

HRESULT WINAPI VarTokenizeFormatString(LPOLESTR format, LPBYTE tokens, int size, int first_day, int first_week, LCID lcid, int *used)
{
	struct format_tokens *header = (struct format_tokens *)tokens;
	int length, needed;
	HRESULT status = check_week_args(first_day, first_week);
	if (FAILED(status))
		return status;
	if (!tokens || size < 0)
		return E_INVALIDARG;
	length = format ? lstrlenW(format) : 0;
	needed = (int)(offsetof(struct format_tokens, text) + (length + 1) * sizeof(WCHAR));
	if (used)
		*used = needed;
	if (size < needed)
		return TYPE_E_BUFFERTOOSMALL;
	header->magic = TOKENS_MAGIC;
	header->first_day = first_day;
	header->first_week = first_week;
	header->lcid = lcid;
	if (length)
		memcpy(header->text, format, length * sizeof(WCHAR));
	header->text[length] = 0;
	return S_OK;
}

HRESULT WINAPI VarFormatFromTokens(LPVARIANT in, LPOLESTR format, LPBYTE tokens, ULONG flags, BSTR *out, LCID lcid)
{
	const struct format_tokens *header = (const struct format_tokens *)tokens;
	if (!tokens)
		return E_INVALIDARG;
	if (header->magic != TOKENS_MAGIC)
		return E_INVALIDARG;
	return format_to_bstr(in, header->text, header->first_day, header->first_week, flags, lcid, out);
}

/* What the fixed shape formats share: optional parentheses around a shape built here. */
static int choice_of(int given, LCTYPE type)
{
	if (given == -2)
		return locale_number(LOCALE_USER_DEFAULT, type, 0, 0) != 0;
	return given != 0;
}

static void build_shape(WCHAR *shape, int digits, int leading, int group)
{
	int index;
	WCHAR *at = shape;
	if (group)
	{
		lstrcpyW(at, L"#,##");
		at += 4;
	}
	else
		*at++ = L'#';
	if (leading)
		*at++ = L'0';
	else if (!group)
		*at++ = L'#';
	else
		*at++ = L'#';
	if (digits > 0)
	{
		*at++ = L'.';
		for (index = 0; index < digits; index++)
			*at++ = L'0';
	}
	*at = 0;
}

static HRESULT plain_number(VARIANT *in, VARIANT *number)
{
	VARIANT plain;
	HRESULT status;
	V_VT(&plain) = VT_EMPTY;
	status = VariantCopyInd(&plain, in);
	if (FAILED(status))
		return status;
	V_VT(number) = VT_EMPTY;
	if (V_VT(&plain) == VT_BSTR || V_VT(&plain) == VT_EMPTY || V_VT(&plain) == VT_BOOL)
		status = VariantChangeTypeEx(number, &plain, LOCALE_USER_DEFAULT, 0, VT_R8);
	else if (V_VT(&plain) == VT_DATE)
		status = VariantChangeTypeEx(number, &plain, LOCALE_USER_DEFAULT, 0, VT_R8);
	else
		status = VariantCopy(number, &plain);
	VariantClear(&plain);
	return status;
}

static HRESULT shaped_output(VARIANT *in, int digits, int leading, int parens, int group, const WCHAR *before, const WCHAR *after, int scale, ULONG flags, BSTR *out)
{
	WCHAR shape[64];
	struct digits number;
	struct text text = {NULL, 0, 0, FALSE};
	VARIANT value;
	BOOL negative, wrapped;
	HRESULT status;
	if (!in || !out || digits > 32)
		return E_INVALIDARG;
	status = plain_number(in, &value);
	if (FAILED(status))
		return status;
	status = digits_of_variant(&value, &number);
	VariantClear(&value);
	if (FAILED(status))
		return status;
	number.point += scale;
	negative = number.negative;
	number.negative = FALSE;
	if (digits < 0)
		digits = locale_number(LOCALE_USER_DEFAULT, LOCALE_IDIGITS, flags, 2);
	build_shape(shape, digits, choice_of(leading, LOCALE_ILZERO), group == -2 ? locale_number(LOCALE_USER_DEFAULT, LOCALE_SGROUPING, flags, 3) != 0 : group != 0);
	wrapped = negative && (parens == -2 ? !locale_number(LOCALE_USER_DEFAULT, LOCALE_INEGNUMBER, flags, 1) : parens != 0);
	if (wrapped)
		text_add(&text, L'(');
	else if (negative)
		text_add(&text, L'-');
	if (before)
		text_add_string(&text, before);
	status = format_number(shape, shape + lstrlenW(shape), number, FALSE, LOCALE_USER_DEFAULT, flags, &text);
	if (SUCCEEDED(status))
	{
		if (after)
			text_add_string(&text, after);
		if (wrapped)
			text_add(&text, L')');
		return text_finish(&text, out);
	}
	if (text.chars)
		HeapFree(GetProcessHeap(), 0, text.chars);
	return status;
}

HRESULT WINAPI VarFormatNumber(LPVARIANT in, int digits, int leading, int parens, int group, ULONG flags, BSTR *out)
{
	return shaped_output(in, digits, leading, parens, group, NULL, NULL, 0, flags, out);
}

HRESULT WINAPI VarFormatPercent(LPVARIANT in, int digits, int leading, int parens, int group, ULONG flags, BSTR *out)
{
	return shaped_output(in, digits, leading, parens, group, NULL, L"%", 2, flags, out);
}

HRESULT WINAPI VarFormatCurrency(LPVARIANT in, int digits, int leading, int parens, int group, ULONG flags, BSTR *out)
{
	WCHAR sign[16];
	VARIANT plain, number;
	HRESULT status;
	BOOL sign_first;
	if (!in || !out)
		return E_INVALIDARG;
	V_VT(&plain) = VT_EMPTY;
	status = VariantCopyInd(&plain, in);
	if (FAILED(status))
		return status;
	V_VT(&number) = VT_EMPTY;
	/* Text must read as money, which may carry the sign and separators of the locale. */
	status = V_VT(&plain) == VT_BSTR ? VariantChangeTypeEx(&number, &plain, LOCALE_USER_DEFAULT, 0, VT_CY) : VariantCopy(&number, &plain);
	VariantClear(&plain);
	if (FAILED(status))
		return status;
	if (!GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SCURRENCY | (flags & LOCALE_NOUSEROVERRIDE), sign, ARRAYSIZE(sign)))
		lstrcpyW(sign, L"$");
	sign_first = !(locale_number(LOCALE_USER_DEFAULT, LOCALE_ICURRENCY, flags, 0) & 1);
	if (digits < 0)
		digits = locale_number(LOCALE_USER_DEFAULT, LOCALE_ICURRDIGITS, flags, 2);
	status = shaped_output(&number, digits, leading, parens, group, sign_first ? sign : NULL, sign_first ? NULL : sign, 0, flags, out);
	VariantClear(&number);
	return status;
}

HRESULT WINAPI VarFormatDateTime(LPVARIANT in, int named, ULONG flags, BSTR *out)
{
	static const WCHAR *const names[] = {L"General Date", L"Long Date", L"Short Date", L"Long Time", L"Short Time"};
	if (named < 0 || named >= (int)ARRAYSIZE(names))
		return E_INVALIDARG;
	return format_to_bstr(in, names[named], 0, 0, flags, LOCALE_USER_DEFAULT, out);
}

HRESULT WINAPI VarWeekdayName(int weekday, int abbreviate, int first_day, ULONG flags, BSTR *out)
{
	WCHAR name[64];
	int day;
	if (!out)
		return E_INVALIDARG;
	if (weekday < 1 || weekday > 7 || first_day < 0 || first_day > 7)
		return E_INVALIDARG;
	if (!first_day)
		first_day = (locale_number(LOCALE_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, 0, 6) + 1) % 7 + 1;
	/* The locale's names run from Monday; first_day counts from Sunday as 1. */
	day = (first_day - 2 + weekday - 1 + 7) % 7;
	if (!GetLocaleInfoW(LOCALE_USER_DEFAULT, (abbreviate ? LOCALE_SABBREVDAYNAME1 : LOCALE_SDAYNAME1) + day, name, ARRAYSIZE(name)))
		return HRESULT_FROM_WIN32(GetLastError());
	*out = SysAllocString(name);
	return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT WINAPI VarMonthName(int month, int abbreviate, ULONG flags, BSTR *out)
{
	WCHAR name[64];
	if (!out)
		return E_INVALIDARG;
	if (month < 1 || month > 12)
		return E_INVALIDARG;
	if (!GetLocaleInfoW(LOCALE_USER_DEFAULT, (abbreviate ? LOCALE_SABBREVMONTHNAME1 : LOCALE_SMONTHNAME1) + month - 1, name, ARRAYSIZE(name)))
		return HRESULT_FROM_WIN32(GetLastError());
	*out = SysAllocString(name);
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* Month names in the form a date needs, for the languages where that differs. */
HRESULT WINAPI GetAltMonthNames(LCID lcid, LPOLESTR **out)
{
	static const WCHAR *const arabic[] = {L"\x064a\x0646\x0627\x064a\x0631", L"\x0641\x0628\x0631\x0627\x064a\x0631", L"\x0645\x0627\x0631\x0633", L"\x0623\x0628\x0631\x064a\x0644", L"\x0645\x0627\x064a\x0648", L"\x064a\x0648\x0646\x064a\x0648", L"\x064a\x0648\x0644\x064a\x0648", L"\x0623\x063a\x0633\x0637\x0633", L"\x0633\x0628\x062a\x0645\x0628\x0631", L"\x0623\x0643\x062a\x0648\x0628\x0631", L"\x0646\x0648\x0641\x0645\x0628\x0631", L"\x062f\x064a\x0633\x0645\x0628\x0631", L"", NULL};
	static const WCHAR *const polish[] = {L"stycznia", L"lutego", L"marca", L"kwietnia", L"maja", L"czerwca", L"lipca", L"sierpnia", L"wrze\x015bnia", L"pa\x017a\x0064ziernika", L"listopada", L"grudnia", L"", NULL};
	static const WCHAR *const russian[] = {L"\x044f\x043d\x0432\x0430\x0440\x044f", L"\x0444\x0435\x0432\x0440\x0430\x043b\x044f", L"\x043c\x0430\x0440\x0442\x0430", L"\x0430\x043f\x0440\x0435\x043b\x044f", L"\x043c\x0430\x044f", L"\x0438\x044e\x043d\x044f", L"\x0438\x044e\x043b\x044f", L"\x0430\x0432\x0433\x0443\x0441\x0442\x0430", L"\x0441\x0435\x043d\x0442\x044f\x0431\x0440\x044f", L"\x043e\x043a\x0442\x044f\x0431\x0440\x044f", L"\x043d\x043e\x044f\x0431\x0440\x044f", L"\x0434\x0435\x043a\x0430\x0431\x0440\x044f", L"", NULL};
	if (!out)
		return E_INVALIDARG;
	switch (PRIMARYLANGID(LANGIDFROMLCID(lcid)))
	{
	case LANG_ARABIC:
		*out = (LPOLESTR *)arabic;
		break;
	case LANG_POLISH:
		*out = (LPOLESTR *)polish;
		break;
	case LANG_RUSSIAN:
		*out = (LPOLESTR *)russian;
		break;
	default:
		*out = NULL;
		break;
	}
	return S_OK;
}
