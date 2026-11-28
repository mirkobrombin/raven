/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.Globalization.DateTimeFormatting.DateTimeFormatter: dates and times as the user's language writes them.
 *
 * A formatter is made from a template ("shortdate", "longtime", "month day", "dayofweek.abbreviated year"...), from a pattern
 * ("{month.full} {day.integer}, {year.full}") or from the parts to include. Each becomes a picture of the language's own (its long date,
 * short date, year and month, month and day or time picture), with the parts left out removed and the forms asked for put in, and the
 * picture is written with GetDateFormatEx and GetTimeFormatEx. Format writes the local time of the date; FormatUsingTimeZone takes UTC,
 * the only zone known by its IANA name here. Only the Gregorian calendar is offered. The numeral system changes the digits written. */
#include "object.h"
#include "../raven-winrt/winrt.h"

static const IID iid_formatter = {0x95eeca10, 0x73e0, 0x4e4b, {0xa1, 0x83, 0x3d, 0x6a, 0xd0, 0xba, 0x35, 0xec}};
static const IID iid_formatter2 = {0x27c91a86, 0xbdaa, 0x4fd0, {0x9e, 0x36, 0x67, 0x1d, 0x5a, 0xa5, 0xee, 0x03}};
static const IID iid_formatter_factory = {0xec8d8a53, 0x1a2e, 0x412d, {0x88, 0x15, 0x3b, 0x74, 0x5f, 0xb1, 0xa2, 0xa0}};
static const IID iid_formatter_statics = {0xbfcde7c0, 0xdf4c, 0x4a2e, {0x90, 0x12, 0xf4, 0x7d, 0xaf, 0x3f, 0x12, 0x12}};

/* YearFormat, MonthFormat, DayOfWeekFormat, DayFormat, HourFormat, MinuteFormat and SecondFormat share None and Default; the year, the
 * month and the day of the week can also be abbreviated or full, and the month numeric. */
enum { NONE = 0, DEFAULT = 1, ABBREVIATED = 2, FULL = 3, NUMERIC = 4 };
enum part { YEAR, MONTH, DAY, WEEKDAY, HOUR, MINUTE, SECOND, PARTS };
static const INT32 part_most[PARTS] = {FULL, NUMERIC, DEFAULT, FULL, DEFAULT, DEFAULT, DEFAULT};

#define PICTURE 160
#define LANGUAGES 8

struct formatter {
	struct object base;
	WCHAR languages[LANGUAGES][LOCALE_NAME_MAX_LENGTH];
	UINT32 language_count;
	WCHAR language[LOCALE_NAME_MAX_LENGTH], region[16], numerals[16], template_name[PICTURE];
	BOOL clock24;
	INT32 parts[PARTS];
	/* The picture written, in the syntax of GetDateFormatEx and GetTimeFormatEx. */
	WCHAR picture[PICTURE];
};

static struct formatter *formatter_of(void *iface) { return (struct formatter *)owner(iface); }

/* ---- Pictures ---- */

/* A picture in pieces: a run of one field letter, or literal text. */
struct piece {
	WCHAR letter;
	UINT32 count;
	WCHAR text[24];
};

static BOOL clock_letter(WCHAR c) { return c == L'h' || c == L'H' || c == L'm' || c == L's' || c == L't'; }
static BOOL field_letter(WCHAR c) { return c == L'd' || c == L'M' || c == L'y' || c == L'g' || c == L'h' || c == L'H' || c == L'm' || c == L's' || c == L't'; }

static UINT32 split(const WCHAR *picture, struct piece *pieces, UINT32 most)
{
	UINT32 count = 0;
	while (*picture && count < most) {
		struct piece *piece = &pieces[count];
		piece->letter = 0;
		piece->count = 0;
		piece->text[0] = 0;
		if (field_letter(*picture)) {
			piece->letter = *picture;
			while (*picture == piece->letter) {
				++piece->count;
				++picture;
			}
			++count;
			continue;
		}
		/* Literal text up to the next field: quoted text is taken as it is, and two quotes are one. */
		UINT32 length = 0;
		while (*picture && !field_letter(*picture)) {
			if (*picture == L'\'') {
				for (++picture; *picture; ++picture) {
					if (*picture == L'\'') {
						if (picture[1] != L'\'')
							break;
						++picture;
					}
					if (length + 1 < ARRAYSIZE(piece->text))
						piece->text[length++] = *picture;
				}
				if (*picture)
					++picture;
				continue;
			}
			if (length + 1 < ARRAYSIZE(piece->text))
				piece->text[length++] = *picture;
			++picture;
		}
		piece->text[length] = 0;
		++count;
	}
	return count;
}

static BOOL append(WCHAR *out, UINT32 size, const WCHAR *text)
{
	const UINT32 used = (UINT32)lstrlenW(out), length = (UINT32)lstrlenW(text);
	if (used + length >= size)
		return FALSE;
	lstrcpyW(out + used, text);
	return TRUE;
}

/* Literal text quoted, so no letter of it is read as a field. */
static BOOL append_literal(WCHAR *out, UINT32 size, const WCHAR *text)
{
	WCHAR quoted[2 * ARRAYSIZE(((struct piece *)0)->text) + 3];
	UINT32 length = 0;
	quoted[length++] = L'\'';
	for (; *text; ++text) {
		if (*text == L'\'')
			quoted[length++] = L'\'';
		quoted[length++] = *text;
	}
	quoted[length++] = L'\'';
	quoted[length] = 0;
	return append(out, size, quoted);
}

static BOOL append_field(WCHAR *out, UINT32 size, WCHAR letter, UINT32 count)
{
	WCHAR run[8];
	if (count >= ARRAYSIZE(run))
		count = ARRAYSIZE(run) - 1;
	for (UINT32 index = 0; index < count; ++index)
		run[index] = letter;
	run[count] = 0;
	return append(out, size, run);
}

/* The part a field writes, or PARTS for the era, which no part names, and the period, which goes with the hour. */
static enum part part_of(const struct piece *piece)
{
	switch (piece->letter) {
	case L'y': return YEAR;
	case L'M': return MONTH;
	case L'd': return piece->count >= 3 ? WEEKDAY : DAY;
	case L'h': case L'H': return HOUR;
	case L'm': return MINUTE;
	case L's': return SECOND;
	default: return PARTS;
	}
}

static BOOL piece_kept(const struct formatter *self, const struct piece *piece)
{
	if (piece->letter == L't')
		return self->parts[HOUR] != NONE && !self->clock24;
	const enum part part = part_of(piece);
	return part != PARTS && self->parts[part] != NONE;
}

/* A field in the form its part asks for and the formatter's clock. */
static void reshape(const struct formatter *self, struct piece *piece)
{
	const INT32 form = part_of(piece) == PARTS ? DEFAULT : self->parts[part_of(piece)];
	switch (part_of(piece)) {
	case YEAR:
		if (form == ABBREVIATED) piece->count = 2;
		else if (form == FULL) piece->count = 4;
		break;
	case MONTH:
		if (form == FULL) piece->count = 4;
		else if (form == ABBREVIATED) piece->count = 3;
		else if (form == NUMERIC && piece->count > 2) piece->count = 1;
		break;
	case WEEKDAY:
		if (form == ABBREVIATED) piece->count = 3;
		else if (form == FULL) piece->count = 4;
		break;
	case HOUR:
		piece->letter = self->clock24 ? L'H' : L'h';
		break;
	default:
		break;
	}
}

/* The pieces of the formatter's parts out of a picture of the language: the fields of parts left out go, and so does the text that
 * joined them to the rest; text before the first field and after the last stays only with that field. */
static BOOL keep_parts(const struct formatter *self, const WCHAR *source, WCHAR *out, UINT32 size)
{
	struct piece pieces[32];
	const UINT32 count = split(source, pieces, ARRAYSIZE(pieces));
	INT32 first = -1, last = -1, previous_kept = -1;
	BOOL period = FALSE, hours = FALSE;
	for (UINT32 index = 0; index < count; ++index)
		if (pieces[index].letter) {
			if (first < 0) first = (INT32)index;
			last = (INT32)index;
			period = period || pieces[index].letter == L't';
			hours = hours || part_of(&pieces[index]) == HOUR;
		}
	out[0] = 0;
	for (UINT32 index = 0; index < count; ++index) {
		struct piece *piece = &pieces[index];
		if (!piece->letter || !piece_kept(self, piece))
			continue;
		if (previous_kept < 0) {
			if ((INT32)index == first && index > 0 && !append_literal(out, size, pieces[index - 1].text))
				return FALSE;
		} else {
			/* The text right before the field when the field before it stays, or else the text that followed the last field kept. */
			const UINT32 joint = index > 0 && !pieces[index - 1].letter && index >= 2 && pieces[index - 2].letter &&
				piece_kept(self, &pieces[index - 2]) ? index - 1 : (UINT32)previous_kept + 1;
			if (joint < index && !pieces[joint].letter && !append_literal(out, size, pieces[joint].text))
				return FALSE;
		}
		reshape(self, piece);
		if (!append_field(out, size, piece->letter, piece->count))
			return FALSE;
		previous_kept = (INT32)index;
	}
	if (previous_kept >= 0 && previous_kept == last && (UINT32)last + 1 < count && !append_literal(out, size, pieces[last + 1].text))
		return FALSE;
	/* A 12-hour clock asked of a language that writes 24 hours gets the period after the time. */
	if (hours && self->parts[HOUR] != NONE && !self->clock24 && !period && (!append_literal(out, size, L" ") || !append(out, size, L"tt")))
		return FALSE;
	return TRUE;
}

static BOOL locale_text(const WCHAR *language, LCTYPE type, WCHAR *out, int size)
{
	return GetLocaleInfoEx(language, type, out, size) > 0;
}

/* The picture of the formatter's parts. */
static HRESULT make_picture(struct formatter *self)
{
	WCHAR source[PICTURE], time[PICTURE];
	const INT32 *parts = self->parts;
	const BOOL date = parts[YEAR] || parts[MONTH] || parts[DAY] || parts[WEEKDAY], clock = parts[HOUR] || parts[MINUTE] || parts[SECOND];
	if (!date && !clock)
		return E_INVALIDARG;
	self->picture[0] = 0;
	if (date) {
		LCTYPE type = LOCALE_SLONGDATE;
		if (parts[MONTH] == NUMERIC)
			type = LOCALE_SSHORTDATE;
		else if (parts[YEAR] && parts[MONTH] && !parts[DAY] && !parts[WEEKDAY])
			type = LOCALE_SYEARMONTH;
		else if (!parts[YEAR] && parts[MONTH] && parts[DAY] && !parts[WEEKDAY])
			type = LOCALE_SMONTHDAY;
		if (!locale_text(self->language, type, source, ARRAYSIZE(source)) &&
			!locale_text(self->language, LOCALE_SLONGDATE, source, ARRAYSIZE(source)))
			return HRESULT_FROM_WIN32(GetLastError());
		if (!keep_parts(self, source, self->picture, ARRAYSIZE(self->picture)))
			return E_INVALIDARG;
	}
	if (clock) {
		if (!locale_text(self->language, parts[SECOND] ? LOCALE_STIMEFORMAT : LOCALE_SSHORTTIME, source, ARRAYSIZE(source)) &&
			!locale_text(self->language, LOCALE_STIMEFORMAT, source, ARRAYSIZE(source)))
			return HRESULT_FROM_WIN32(GetLastError());
		if (!keep_parts(self, source, time, ARRAYSIZE(time)) || (date && !append_literal(self->picture, ARRAYSIZE(self->picture), L" ")) ||
			!append(self->picture, ARRAYSIZE(self->picture), time))
			return E_INVALIDARG;
	}
	return S_OK;
}

/* ---- Templates and patterns ---- */

struct word {
	const WCHAR *name;
	enum part part;
	INT32 form;
};

static const struct word template_words[] = {
	{L"year", YEAR, DEFAULT}, {L"year.full", YEAR, FULL}, {L"year.abbreviated", YEAR, ABBREVIATED},
	{L"month", MONTH, DEFAULT}, {L"month.full", MONTH, FULL}, {L"month.abbreviated", MONTH, ABBREVIATED}, {L"month.numeric", MONTH, NUMERIC},
	{L"day", DAY, DEFAULT}, {L"dayofweek", WEEKDAY, DEFAULT}, {L"dayofweek.full", WEEKDAY, FULL},
	{L"dayofweek.abbreviated", WEEKDAY, ABBREVIATED}, {L"hour", HOUR, DEFAULT}, {L"minute", MINUTE, DEFAULT}, {L"second", SECOND, DEFAULT},
};

/* The four templates that are a whole picture of the language, and the parts they write. */
static const struct {
	const WCHAR *name;
	LCTYPE type;
	INT32 parts[PARTS];
} picture_templates[] = {
	{L"shortdate", LOCALE_SSHORTDATE, {DEFAULT, NUMERIC, DEFAULT, NONE, NONE, NONE, NONE}},
	{L"longdate", LOCALE_SLONGDATE, {FULL, FULL, DEFAULT, FULL, NONE, NONE, NONE}},
	{L"shorttime", LOCALE_SSHORTTIME, {NONE, NONE, NONE, NONE, DEFAULT, DEFAULT, NONE}},
	{L"longtime", LOCALE_STIMEFORMAT, {NONE, NONE, NONE, NONE, DEFAULT, DEFAULT, DEFAULT}},
};

static BOOL same_word(const WCHAR *word, UINT32 length, const WCHAR *name)
{
	return (UINT32)lstrlenW(name) == length && CompareStringOrdinal(word, (int)length, name, (int)length, TRUE) == CSTR_EQUAL;
}

/* A template: words naming parts, or one or two of the whole pictures, a date's and a time's. */
static HRESULT read_template(struct formatter *self, const WCHAR *text)
{
	WCHAR whole[PICTURE] = {0};
	BOOL any = FALSE, whole_date = FALSE, whole_time = FALSE;
	for (const WCHAR *word = text; *word;) {
		while (*word == L' ')
			++word;
		const WCHAR *end = word;
		while (*end && *end != L' ')
			++end;
		const UINT32 length = (UINT32)(end - word);
		if (!length)
			break;
		BOOL known = FALSE;
		for (size_t index = 0; index < ARRAYSIZE(template_words) && !known; ++index)
			if (same_word(word, length, template_words[index].name)) {
				const enum part part = template_words[index].part;
				if (self->parts[part] || whole_date || whole_time)
					return E_INVALIDARG;
				self->parts[part] = template_words[index].form;
				known = TRUE;
			}
		for (size_t index = 0; index < ARRAYSIZE(picture_templates) && !known; ++index)
			if (same_word(word, length, picture_templates[index].name)) {
				const BOOL is_date = picture_templates[index].type == LOCALE_SSHORTDATE || picture_templates[index].type == LOCALE_SLONGDATE;
				if ((is_date ? whole_date : whole_time) || (any && !whole_date && !whole_time) || (whole_time && is_date))
					return E_INVALIDARG;
				WCHAR source[PICTURE];
				if (!locale_text(self->language, picture_templates[index].type, source, ARRAYSIZE(source)) &&
					(picture_templates[index].type != LOCALE_SSHORTTIME ||
						!locale_text(self->language, LOCALE_STIMEFORMAT, source, ARRAYSIZE(source))))
					return HRESULT_FROM_WIN32(GetLastError());
				for (int part = 0; part < PARTS; ++part)
					if (picture_templates[index].parts[part])
						self->parts[part] = picture_templates[index].parts[part];
				/* The language's own picture, with the hour of the formatter's clock. */
				if (is_date)
					whole_date = TRUE;
				else {
					whole_time = TRUE;
					WCHAR time[PICTURE];
					if (!keep_parts(self, source, time, ARRAYSIZE(time)))
						return E_INVALIDARG;
					lstrcpyW(source, time);
				}
				if ((whole[0] && !append_literal(whole, ARRAYSIZE(whole), L" ")) || !append(whole, ARRAYSIZE(whole), source))
					return E_INVALIDARG;
				known = TRUE;
			}
		if (!known)
			return E_INVALIDARG;
		any = TRUE;
		word = end;
	}
	if (!any)
		return E_INVALIDARG;
	if (whole_date || whole_time) {
		lstrcpyW(self->picture, whole);
		return S_OK;
	}
	return make_picture(self);
}

/* The fields of a pattern, each with the picture it writes: a count of zero takes the formatter's clock for the hour. */
static const struct {
	const WCHAR *name;
	WCHAR letter;
	UINT32 count;
	enum part part;
	INT32 form;
} pattern_fields[] = {
	{L"year.full", L'y', 4, YEAR, FULL}, {L"year.abbreviated", L'y', 2, YEAR, ABBREVIATED}, {L"year.abbreviated(2)", L'y', 2, YEAR, ABBREVIATED},
	{L"month.full", L'M', 4, MONTH, FULL}, {L"month.abbreviated", L'M', 3, MONTH, ABBREVIATED}, {L"month.integer", L'M', 1, MONTH, NUMERIC},
	{L"month.integer(2)", L'M', 2, MONTH, NUMERIC}, {L"day.integer", L'd', 1, DAY, DEFAULT}, {L"day.integer(2)", L'd', 2, DAY, DEFAULT},
	{L"dayofweek.full", L'd', 4, WEEKDAY, FULL}, {L"dayofweek.abbreviated", L'd', 3, WEEKDAY, ABBREVIATED},
	{L"hour", 0, 1, HOUR, DEFAULT}, {L"hour.integer", 0, 1, HOUR, DEFAULT}, {L"hour.integer(2)", 0, 2, HOUR, DEFAULT},
	{L"minute", L'm', 2, MINUTE, DEFAULT}, {L"minute.integer", L'm', 1, MINUTE, DEFAULT}, {L"minute.integer(2)", L'm', 2, MINUTE, DEFAULT},
	{L"second", L's', 2, SECOND, DEFAULT}, {L"second.integer", L's', 1, SECOND, DEFAULT}, {L"second.integer(2)", L's', 2, SECOND, DEFAULT},
	{L"period.abbreviated", L't', 2, PARTS, DEFAULT}, {L"period.abbreviated(2)", L't', 2, PARTS, DEFAULT},
};

/* A pattern names its fields in braces; a template has none. */
static BOOL is_pattern(const WCHAR *text)
{
	for (; *text; ++text)
		if (*text == L'{')
			return TRUE;
	return FALSE;
}

static HRESULT read_pattern(struct formatter *self, const WCHAR *text)
{
	WCHAR literal[24];
	UINT32 literal_length = 0;
	self->picture[0] = 0;
	for (const WCHAR *at = text;; ++at) {
		if (*at && *at != L'{') {
			if (literal_length + 1 >= ARRAYSIZE(literal))
				return E_INVALIDARG;
			literal[literal_length++] = *at;
			continue;
		}
		if (literal_length) {
			literal[literal_length] = 0;
			if (!append_literal(self->picture, ARRAYSIZE(self->picture), literal))
				return E_INVALIDARG;
			literal_length = 0;
		}
		if (!*at)
			break;
		const WCHAR *end = at + 1;
		while (*end && *end != L'}')
			++end;
		if (!*end)
			return E_INVALIDARG;
		const UINT32 length = (UINT32)(end - at - 1);
		BOOL known = FALSE;
		for (size_t index = 0; index < ARRAYSIZE(pattern_fields) && !known; ++index)
			if (same_word(at + 1, length, pattern_fields[index].name)) {
				const WCHAR letter = pattern_fields[index].letter ? pattern_fields[index].letter : self->clock24 ? L'H' : L'h';
				if (!append_field(self->picture, ARRAYSIZE(self->picture), letter, pattern_fields[index].count))
					return E_INVALIDARG;
				if (pattern_fields[index].part != PARTS)
					self->parts[pattern_fields[index].part] = pattern_fields[index].form;
				known = TRUE;
			}
		if (!known)
			return E_INVALIDARG;
		at = end;
	}
	/* What a pattern writes is all it writes: a clock of twelve hours without the period leaves the period out. */
	for (int part = 0; part < PARTS; ++part)
		if (self->parts[part])
			return S_OK;
	return E_INVALIDARG;
}

/* The pattern of a picture, as Patterns gives it. */
static void picture_pattern(const WCHAR *picture, WCHAR *out, UINT32 size)
{
	struct piece pieces[32];
	const UINT32 count = split(picture, pieces, ARRAYSIZE(pieces));
	out[0] = 0;
	for (UINT32 index = 0; index < count; ++index) {
		const struct piece *piece = &pieces[index];
		const WCHAR *name = NULL;
		switch (piece->letter) {
		case 0: append(out, size, piece->text); continue;
		case L'y': name = piece->count <= 2 ? L"{year.abbreviated(2)}" : L"{year.full}"; break;
		case L'M': name = piece->count == 1 ? L"{month.integer}" : piece->count == 2 ? L"{month.integer(2)}" : piece->count == 3 ? L"{month.abbreviated}" : L"{month.full}"; break;
		case L'd': name = piece->count == 1 ? L"{day.integer}" : piece->count == 2 ? L"{day.integer(2)}" : piece->count == 3 ? L"{dayofweek.abbreviated}" : L"{dayofweek.full}"; break;
		case L'h': case L'H': name = piece->count == 1 ? L"{hour.integer}" : L"{hour.integer(2)}"; break;
		case L'm': name = piece->count == 1 ? L"{minute.integer}" : L"{minute.integer(2)}"; break;
		case L's': name = piece->count == 1 ? L"{second.integer}" : L"{second.integer(2)}"; break;
		case L't': name = L"{period.abbreviated}"; break;
		default: continue;
		}
		append(out, size, name);
	}
}

/* ---- Numeral systems ---- */

static const struct {
	const WCHAR *name;
	WCHAR zero;
} numeral_systems[] = {
	{L"Latn", L'0'}, {L"Arab", 0x660}, {L"ArabExt", 0x6f0}, {L"Beng", 0x9e6}, {L"Deva", 0x966}, {L"FullWide", 0xff10}, {L"Gujr", 0xae6},
	{L"Guru", 0xa66}, {L"Khmr", 0x17e0}, {L"Knda", 0xce6}, {L"Laoo", 0xed0}, {L"Mlym", 0xd66}, {L"Mong", 0x1810}, {L"Mymr", 0x1040},
	{L"Orya", 0xb66}, {L"Tamldec", 0xbe6}, {L"Telu", 0xc66}, {L"Thai", 0xe50}, {L"Tibt", 0xf20},
};

static INT32 numeral_index(const WCHAR *name)
{
	for (size_t index = 0; index < ARRAYSIZE(numeral_systems); ++index)
		if (!lstrcmpiW(name, numeral_systems[index].name))
			return (INT32)index;
	return -1;
}

/* ---- Writing ---- */

/* The picture written for a time: runs of the date's fields through GetDateFormatEx and of the time's through GetTimeFormatEx, the text
 * between them with the run it follows, so a language that declines a month after its day still reads the day. */
static HRESULT write_time(struct formatter *self, const SYSTEMTIME *time, HSTRING *out)
{
	struct piece pieces[32];
	WCHAR text[256] = {0}, run[PICTURE], written[128];
	const UINT32 count = split(self->picture, pieces, ARRAYSIZE(pieces));
	for (UINT32 index = 0; index < count;) {
		/* A run of the date's or the time's fields with the text inside it. */
		BOOL clock = FALSE, field = FALSE;
		run[0] = 0;
		for (; index < count; ++index) {
			const struct piece *piece = &pieces[index];
			if (piece->letter) {
				const BOOL is_clock = clock_letter(piece->letter);
				if (field && is_clock != clock)
					break;
				clock = is_clock;
				field = TRUE;
				append_field(run, ARRAYSIZE(run), piece->letter, piece->count);
			} else
				append_literal(run, ARRAYSIZE(run), piece->text);
		}
		const int length = !field ? 0 : clock ? GetTimeFormatEx(self->language, 0, time, run, written, ARRAYSIZE(written)) :
			GetDateFormatEx(self->language, 0, time, run, written, ARRAYSIZE(written), NULL);
		if (field && !length)
			return HRESULT_FROM_WIN32(GetLastError());
		if (!field) {
			/* Text alone: only the text of the run, written as it is. */
			struct piece alone[4];
			const UINT32 parts = split(run, alone, ARRAYSIZE(alone));
			for (UINT32 part = 0; part < parts; ++part)
				append(text, ARRAYSIZE(text), alone[part].text);
		} else if (!append(text, ARRAYSIZE(text), written))
			return E_OUTOFMEMORY;
	}
	const INT32 numerals = numeral_index(self->numerals);
	if (numerals > 0)
		for (WCHAR *digit = text; *digit; ++digit)
			if (*digit >= L'0' && *digit <= L'9')
				*digit = (WCHAR)(numeral_systems[numerals].zero + (*digit - L'0'));
	return WindowsCreateString(text, (UINT32)lstrlenW(text), out);
}

static HRESULT write_date(struct formatter *self, INT64 value, BOOL local, HSTRING *out)
{
	FILETIME file = {(DWORD)value, (DWORD)((UINT64)value >> 32)};
	SYSTEMTIME universal, time;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (value < 0 || !FileTimeToSystemTime(&file, &universal))
		return E_INVALIDARG;
	if (!local)
		time = universal;
	else if (!SystemTimeToTzSpecificLocalTime(NULL, &universal, &time))
		return HRESULT_FROM_WIN32(GetLastError());
	return write_time(self, &time, out);
}

/* ---- The formatter ---- */

static HRESULT text_answer(const WCHAR *text, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsCreateString(text, (UINT32)lstrlenW(text), out);
}

static HRESULT names_answer(const WCHAR *const *names, UINT32 count, void **out)
{
	HSTRING_HEADER headers[LANGUAGES];
	HSTRING items[LANGUAGES];
	if (!out)
		return E_POINTER;
	*out = NULL;
	for (UINT32 index = 0; index < count; ++index) {
		const HRESULT result = WindowsCreateStringReference(names[index], (UINT32)lstrlenW(names[index]), &headers[index], &items[index]);
		if (FAILED(result))
			return result;
	}
	return winrt_items(&string_list, (IInspectable **)items, count, FALSE, out);
}

static HRESULT WINAPI formatter_languages(void *iface, void **out)
{
	struct formatter *self = formatter_of(iface);
	const WCHAR *names[LANGUAGES];
	for (UINT32 index = 0; index < self->language_count; ++index)
		names[index] = self->languages[index];
	return names_answer(names, self->language_count, out);
}

static HRESULT WINAPI formatter_region(void *iface, HSTRING *out) { return text_answer(formatter_of(iface)->region, out); }
static HRESULT WINAPI formatter_calendar(void *iface, HSTRING *out) { return text_answer(L"GregorianCalendar", out); }
static HRESULT WINAPI formatter_clock(void *iface, HSTRING *out) { return text_answer(formatter_of(iface)->clock24 ? L"24HourClock" : L"12HourClock", out); }
static HRESULT WINAPI formatter_numerals(void *iface, HSTRING *out) { return text_answer(formatter_of(iface)->numerals, out); }

static HRESULT WINAPI formatter_set_numerals(void *iface, HSTRING value)
{
	const INT32 index = numeral_index(WindowsGetStringRawBuffer(value, NULL));
	if (index < 0)
		return E_INVALIDARG;
	lstrcpyW(formatter_of(iface)->numerals, numeral_systems[index].name);
	return S_OK;
}

static HRESULT WINAPI formatter_patterns(void *iface, void **out)
{
	WCHAR pattern[2 * PICTURE];
	const WCHAR *names[1] = {pattern};
	picture_pattern(formatter_of(iface)->picture, pattern, ARRAYSIZE(pattern));
	return names_answer(names, 1, out);
}

static HRESULT WINAPI formatter_template(void *iface, HSTRING *out) { return text_answer(formatter_of(iface)->template_name, out); }
static HRESULT WINAPI formatter_format(void *iface, INT64 value, HSTRING *out) { return write_date(formatter_of(iface), value, TRUE, out); }

static HRESULT part_answer(void *iface, enum part part, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = formatter_of(iface)->parts[part];
	return S_OK;
}

static HRESULT WINAPI include_year(void *iface, INT32 *out) { return part_answer(iface, YEAR, out); }
static HRESULT WINAPI include_month(void *iface, INT32 *out) { return part_answer(iface, MONTH, out); }
static HRESULT WINAPI include_weekday(void *iface, INT32 *out) { return part_answer(iface, WEEKDAY, out); }
static HRESULT WINAPI include_day(void *iface, INT32 *out) { return part_answer(iface, DAY, out); }
static HRESULT WINAPI include_hour(void *iface, INT32 *out) { return part_answer(iface, HOUR, out); }
static HRESULT WINAPI include_minute(void *iface, INT32 *out) { return part_answer(iface, MINUTE, out); }
static HRESULT WINAPI include_second(void *iface, INT32 *out) { return part_answer(iface, SECOND, out); }
static HRESULT WINAPI resolved_language(void *iface, HSTRING *out) { return text_answer(formatter_of(iface)->language, out); }
static HRESULT WINAPI resolved_region(void *iface, HSTRING *out) { return text_answer(formatter_of(iface)->region, out); }

static HRESULT WINAPI format_in_zone(void *iface, INT64 value, HSTRING zone, HSTRING *out)
{
	static const WCHAR *const universal[] = {L"UTC", L"Etc/UTC", L"Etc/GMT", L"GMT", L"Etc/Universal", L"Etc/Zulu"};
	const WCHAR *name = WindowsGetStringRawBuffer(zone, NULL);
	if (out)
		*out = NULL;
	for (size_t index = 0; index < ARRAYSIZE(universal); ++index)
		if (!lstrcmpiW(name, universal[index]))
			return write_date(formatter_of(iface), value, FALSE, out);
	trace("DateTimeFormatter.FormatUsingTimeZone", "zone");
	return E_INVALIDARG;
}

TABLE(formatter_table, 0, INSPECTABLE, (const void *)formatter_languages, (const void *)formatter_region, (const void *)formatter_calendar,
	(const void *)formatter_clock, (const void *)formatter_numerals, (const void *)formatter_set_numerals, (const void *)formatter_patterns,
	(const void *)formatter_template, (const void *)formatter_format, (const void *)include_year, (const void *)include_month,
	(const void *)include_weekday, (const void *)include_day, (const void *)include_hour, (const void *)include_minute,
	(const void *)include_second, (const void *)resolved_language, (const void *)resolved_region);
TABLE(formatter2_table, 1, INSPECTABLE, (const void *)format_in_zone);
static const struct face_iid formatter_iids[] = {{&iid_formatter, 0}, {&iid_formatter2, 1}, {NULL, 0}};
/* A formatter holds nothing beyond itself. */
static void destroy_formatter(struct object *base) {}
static const struct kind formatter_kind = {"DateTimeFormatter", L"Windows.Globalization.DateTimeFormatting.DateTimeFormatter", formatter_iids,
	destroy_formatter};

/* The languages given, the first one Windows knows the one written in; none given, the user's. */
static HRESULT take_languages(struct formatter *self, void *languages)
{
	if (languages) {
		void *iterator = NULL;
		boolean more = FALSE;
		HRESULT result = ((HRESULT (WINAPI *)(void *, void **))method(languages, 6))(languages, &iterator);
		if (FAILED(result))
			return result;
		result = ((HRESULT (WINAPI *)(void *, boolean *))method(iterator, 7))(iterator, &more);
		while (SUCCEEDED(result) && more && self->language_count < LANGUAGES) {
			HSTRING name = NULL;
			WCHAR resolved[LOCALE_NAME_MAX_LENGTH];
			if (FAILED(result = ((HRESULT (WINAPI *)(void *, HSTRING *))method(iterator, 6))(iterator, &name)))
				break;
			if (GetLocaleInfoEx(WindowsGetStringRawBuffer(name, NULL), LOCALE_SNAME, resolved, ARRAYSIZE(resolved)))
				lstrcpyW(self->languages[self->language_count++], resolved);
			WindowsDeleteString(name);
			result = ((HRESULT (WINAPI *)(void *, boolean *))method(iterator, 8))(iterator, &more);
		}
		release(iterator);
		if (FAILED(result))
			return result;
		if (!self->language_count)
			return E_INVALIDARG;
	} else {
		if (!GetUserDefaultLocaleName(self->languages[0], LOCALE_NAME_MAX_LENGTH))
			return HRESULT_FROM_WIN32(GetLastError());
		self->language_count = 1;
	}
	lstrcpyW(self->language, self->languages[0]);
	return S_OK;
}

/* A formatter for the languages, region, calendar and clock given, any of them NULL for the user's, with the parts or the template. */
static HRESULT make_formatter(const WCHAR *template_text, const INT32 *parts, void *languages, HSTRING region, HSTRING calendar, HSTRING clock,
	void **out)
{
	static const void *const *const tables[] = {formatter_table.slots, formatter2_table.slots};
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct formatter *self = make(sizeof *self, &formatter_kind, tables, 2);
	if (!self)
		return E_OUTOFMEMORY;
	HRESULT result = take_languages(self, languages);
	const WCHAR *calendar_name = calendar ? WindowsGetStringRawBuffer(calendar, NULL) : L"GregorianCalendar";
	const WCHAR *clock_name = clock ? WindowsGetStringRawBuffer(clock, NULL) : NULL;
	if (SUCCEEDED(result) && lstrcmpW(calendar_name, L"GregorianCalendar")) {
		trace("DateTimeFormatter", "calendar");
		result = E_INVALIDARG;
	}
	if (SUCCEEDED(result)) {
		if (clock_name && !lstrcmpW(clock_name, L"24HourClock"))
			self->clock24 = TRUE;
		else if (clock_name && lstrcmpW(clock_name, L"12HourClock"))
			result = E_INVALIDARG;
		else if (!clock_name) {
			UINT32 hours = 0;
			GetLocaleInfoEx(self->language, LOCALE_ITIME | LOCALE_RETURN_NUMBER, (WCHAR *)&hours, sizeof hours / sizeof(WCHAR));
			self->clock24 = hours != 0;
		}
	}
	if (SUCCEEDED(result)) {
		const WCHAR *given = region ? WindowsGetStringRawBuffer(region, NULL) : NULL;
		if (given && *given) {
			if (lstrlenW(given) >= (int)ARRAYSIZE(self->region))
				result = E_INVALIDARG;
			else
				lstrcpyW(self->region, given);
		} else if (!GetLocaleInfoEx(self->language, LOCALE_SISO3166CTRYNAME, self->region, ARRAYSIZE(self->region)))
			result = HRESULT_FROM_WIN32(GetLastError());
	}
	if (SUCCEEDED(result)) {
		WCHAR digits[32] = {0};
		/* The language's own digits when it names them first (LOCALE_SNATIVEDIGITS gives them; the numeral system is Latn otherwise). */
		lstrcpyW(self->numerals, L"Latn");
		if (GetLocaleInfoEx(self->language, LOCALE_SNATIVEDIGITS, digits, ARRAYSIZE(digits)))
			for (size_t index = 1; index < ARRAYSIZE(numeral_systems); ++index)
				if (digits[0] == numeral_systems[index].zero)
					lstrcpyW(self->numerals, numeral_systems[index].name);
	}
	if (SUCCEEDED(result)) {
		if (template_text) {
			if (lstrlenW(template_text) >= (int)ARRAYSIZE(self->template_name))
				result = E_INVALIDARG;
			else {
				lstrcpyW(self->template_name, template_text);
				result = is_pattern(template_text) ? read_pattern(self, template_text) : read_template(self, template_text);
			}
		} else {
			for (int part = 0; part < PARTS && SUCCEEDED(result); ++part)
				if (parts[part] < NONE || parts[part] > part_most[part])
					result = E_INVALIDARG;
				else
					self->parts[part] = parts[part];
			if (SUCCEEDED(result))
				result = make_picture(self);
		}
	}
	if (FAILED(result)) {
		release(face_of(&self->base, 0));
		return result;
	}
	*out = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT WINAPI create_template(void *iface, HSTRING text, void **out)
{
	return make_formatter(WindowsGetStringRawBuffer(text, NULL), NULL, NULL, NULL, NULL, NULL, out);
}
static HRESULT WINAPI create_template_languages(void *iface, HSTRING text, void *languages, void **out)
{
	return languages ? make_formatter(WindowsGetStringRawBuffer(text, NULL), NULL, languages, NULL, NULL, NULL, out) : E_INVALIDARG;
}
static HRESULT WINAPI create_template_context(void *iface, HSTRING text, void *languages, HSTRING region, HSTRING calendar, HSTRING clock,
	void **out)
{
	return languages ? make_formatter(WindowsGetStringRawBuffer(text, NULL), NULL, languages, region, calendar, clock, out) : E_INVALIDARG;
}
static HRESULT WINAPI create_date(void *iface, INT32 year, INT32 month, INT32 day, INT32 weekday, void **out)
{
	const INT32 parts[PARTS] = {year, month, day, weekday, NONE, NONE, NONE};
	return make_formatter(NULL, parts, NULL, NULL, NULL, NULL, out);
}
static HRESULT WINAPI create_time(void *iface, INT32 hour, INT32 minute, INT32 second, void **out)
{
	const INT32 parts[PARTS] = {NONE, NONE, NONE, NONE, hour, minute, second};
	return make_formatter(NULL, parts, NULL, NULL, NULL, NULL, out);
}
static HRESULT WINAPI create_date_time_languages(void *iface, INT32 year, INT32 month, INT32 day, INT32 weekday, INT32 hour, INT32 minute,
	INT32 second, void *languages, void **out)
{
	const INT32 parts[PARTS] = {year, month, day, weekday, hour, minute, second};
	return languages ? make_formatter(NULL, parts, languages, NULL, NULL, NULL, out) : E_INVALIDARG;
}
static HRESULT WINAPI create_date_time_context(void *iface, INT32 year, INT32 month, INT32 day, INT32 weekday, INT32 hour, INT32 minute,
	INT32 second, void *languages, HSTRING region, HSTRING calendar, HSTRING clock, void **out)
{
	const INT32 parts[PARTS] = {year, month, day, weekday, hour, minute, second};
	return languages ? make_formatter(NULL, parts, languages, region, calendar, clock, out) : E_INVALIDARG;
}

static HRESULT WINAPI long_date(void *iface, void **out) { return make_formatter(L"longdate", NULL, NULL, NULL, NULL, NULL, out); }
static HRESULT WINAPI long_time(void *iface, void **out) { return make_formatter(L"longtime", NULL, NULL, NULL, NULL, NULL, out); }
static HRESULT WINAPI short_date(void *iface, void **out) { return make_formatter(L"shortdate", NULL, NULL, NULL, NULL, NULL, out); }
static HRESULT WINAPI short_time(void *iface, void **out) { return make_formatter(L"shorttime", NULL, NULL, NULL, NULL, NULL, out); }

TABLE(formatter_class_table, 0, FACTORY);
TABLE(formatter_factory_table, 1, INSPECTABLE, (const void *)create_template, (const void *)create_template_languages,
	(const void *)create_template_context, (const void *)create_date, (const void *)create_time, (const void *)create_date_time_languages,
	(const void *)create_date_time_context);
TABLE(formatter_statics_table, 2, INSPECTABLE, (const void *)long_date, (const void *)long_time, (const void *)short_date,
	(const void *)short_time);
static const struct face_iid formatter_class_iids[] = {{&iid_factory, 0}, {&iid_formatter_factory, 1}, {&iid_formatter_statics, 2}, {NULL, 0}};
static const struct kind formatter_class_kind = {"DateTimeFormatter", L"Windows.Globalization.DateTimeFormatting.DateTimeFormatter",
	formatter_class_iids, NULL};
struct object date_time_formatter_class = {{formatter_class_table.slots, formatter_factory_table.slots, formatter_statics_table.slots},
	&formatter_class_kind, 1};
