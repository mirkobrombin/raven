// Windows.Globalization.Calendar as an app uses it: a Gregorian calendar in English and in UTC read field by field, written as text,
// moved by adding, set field by field, on either clock and in other digits, copied and compared, then one in the user's zone that must
// agree with the local time Windows gives. The exit code names the first check that failed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <roapi.h>
#include <winstring.h>

typedef struct object object;
struct object {
	void **table;
};
#define CALL(self, place, type) ((type)(self)->table[place])

typedef ULONG (WINAPI *release_method)(object *);
typedef HRESULT (WINAPI *query_method)(object *, const GUID *, void **);
typedef HRESULT (WINAPI *object_method)(object *, object **);
typedef HRESULT (WINAPI *text_method)(object *, HSTRING *);
typedef HRESULT (WINAPI *sized_text_method)(object *, INT32, HSTRING *);
typedef HRESULT (WINAPI *number_method)(object *, INT32 *);
typedef HRESULT (WINAPI *set_method)(object *, INT32);
typedef HRESULT (WINAPI *name_method)(object *, HSTRING);
typedef HRESULT (WINAPI *moment_method)(object *, INT64 *);
typedef HRESULT (WINAPI *set_moment_method)(object *, INT64);

static const GUID calendar_iid = {0xca30221d, 0x86d9, 0x40fb, {0xa2, 0x6b, 0xd4, 0x4e, 0xb7, 0xcf, 0x08, 0xea}};
static const GUID factory_iid = {0x83f58412, 0xe56b, 0x4c75, {0xa6, 0x6e, 0x0f, 0x63, 0xd5, 0x77, 0x58, 0xa6}};
static const GUID factory2_iid = {0xb44b378c, 0xca7e, 0x4590, {0x9e, 0x72, 0xea, 0x2b, 0xec, 0x1a, 0x51, 0x15}};
static const GUID zone_iid = {0xbb3c25e5, 0x46cf, 0x4317, {0xa3, 0xf5, 0x02, 0x62, 0x1a, 0xd5, 0x44, 0x78}};
static const GUID activation_iid = {0x00000035, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

/* The places of ICalendar's methods. */
enum {
	CLONE = 6, SET_TO_MIN, SET_TO_MAX, LANGUAGES, NUMERALS, PUT_NUMERALS, GET_SYSTEM, CHANGE_SYSTEM, GET_CLOCK, CHANGE_CLOCK, GET_TIME,
	SET_TIME, SET_TO_NOW, FIRST_ERA, LAST_ERA, ERAS, ERA, PUT_ERA, ADD_ERAS, ERA_FULL, ERA_TEXT, FIRST_YEAR, LAST_YEAR, YEARS, YEAR,
	PUT_YEAR, ADD_YEARS, YEAR_TEXT, YEAR_TRUNCATED, YEAR_PADDED, FIRST_MONTH, LAST_MONTH, MONTHS, MONTH, PUT_MONTH, ADD_MONTHS, MONTH_FULL,
	MONTH_TEXT, MONTH_FULL_SOLO, MONTH_SOLO, MONTH_NUMERIC, MONTH_PADDED, ADD_WEEKS, FIRST_DAY, LAST_DAY, DAYS, DAY, PUT_DAY, ADD_DAYS,
	DAY_TEXT, DAY_PADDED, WEEKDAY, WEEKDAY_FULL, WEEKDAY_TEXT, WEEKDAY_FULL_SOLO, WEEKDAY_SOLO, FIRST_PERIOD, LAST_PERIOD, PERIODS, PERIOD,
	PUT_PERIOD, ADD_PERIODS, PERIOD_FULL, PERIOD_TEXT, FIRST_HOUR, LAST_HOUR, HOURS, HOUR, PUT_HOUR, ADD_HOURS, HOUR_TEXT, HOUR_PADDED,
	MINUTE, PUT_MINUTE, ADD_MINUTES, MINUTE_TEXT, MINUTE_PADDED, SECOND, PUT_SECOND, ADD_SECONDS, SECOND_TEXT, SECOND_PADDED, NANOSECOND,
	PUT_NANOSECOND, ADD_NANOSECONDS, NANOSECOND_TEXT, NANOSECOND_PADDED, COMPARE, COMPARE_TIME, COPY_TO, FIRST_MINUTE, LAST_MINUTE, MINUTES,
	FIRST_SECOND, LAST_SECOND, SECONDS, RESOLVED_LANGUAGE, DAYLIGHT,
};

struct reference {
	UINT32 flags, length, unused[2];
	const WCHAR *text;
};

/* A string the callee does not own, as the language projections pass them. */
static HSTRING text(struct reference *reference, const WCHAR *value)
{
	reference->flags = 1;
	reference->length = (UINT32)lstrlenW(value);
	reference->unused[0] = reference->unused[1] = 0;
	reference->text = value;
	return (HSTRING)reference;
}

static void release(object *self)
{
	if (self)
		CALL(self, 2, release_method)(self);
}

/* IIterable<String> of one language, and its iterator. */
struct iterator {
	void **table;
	LONG references;
	const WCHAR *name;
	BOOL done;
};

static HRESULT WINAPI unknown_query(struct iterator *self, const GUID *iid, void **out)
{
	*out = self;
	InterlockedIncrement(&self->references);
	return S_OK;
}
static ULONG WINAPI unknown_reference(struct iterator *self) { return (ULONG)InterlockedIncrement(&self->references); }
static ULONG WINAPI unknown_release(struct iterator *self) { return (ULONG)InterlockedDecrement(&self->references); }
static HRESULT WINAPI no_iids(void *self, ULONG *count, GUID **iids) { *count = 0; *iids = NULL; return S_OK; }
static HRESULT WINAPI no_name(void *self, HSTRING *name) { *name = NULL; return S_OK; }
static HRESULT WINAPI base_trust(void *self, INT32 *level) { *level = 0; return S_OK; }

static HRESULT WINAPI iterator_current(struct iterator *self, HSTRING *out)
{
	return self->done ? E_BOUNDS : WindowsCreateString(self->name, (UINT32)lstrlenW(self->name), out);
}
static HRESULT WINAPI iterator_has(struct iterator *self, BYTE *out) { *out = !self->done; return S_OK; }
static HRESULT WINAPI iterator_next(struct iterator *self, BYTE *out) { self->done = TRUE; *out = FALSE; return S_OK; }

static void *iterator_table[] = {(void *)unknown_query, (void *)unknown_reference, (void *)unknown_release, (void *)no_iids, (void *)no_name,
	(void *)base_trust, (void *)iterator_current, (void *)iterator_has, (void *)iterator_next};
static struct iterator language_iterator = {iterator_table, 1, L"en-US", FALSE};

static HRESULT WINAPI iterable_first(struct iterator *self, struct iterator **out)
{
	language_iterator.done = FALSE;
	InterlockedIncrement(&language_iterator.references);
	*out = &language_iterator;
	return S_OK;
}

static void *iterable_table[] = {(void *)unknown_query, (void *)unknown_reference, (void *)unknown_release, (void *)no_iids, (void *)no_name,
	(void *)base_trust, (void *)iterable_first};
static struct iterator languages = {iterable_table, 1, NULL, FALSE};

static BOOL same_text(HSTRING value, const WCHAR *expected)
{
	const BOOL same = !lstrcmpW(WindowsGetStringRawBuffer(value, NULL), expected);
	WindowsDeleteString(value);
	return same;
}

static BOOL says(object *self, int slot, const WCHAR *expected)
{
	HSTRING value = NULL;
	return CALL(self, slot, text_method)(self, &value) == S_OK && same_text(value, expected);
}

static BOOL says_sized(object *self, int slot, INT32 size, const WCHAR *expected)
{
	HSTRING value = NULL;
	return CALL(self, slot, sized_text_method)(self, size, &value) == S_OK && same_text(value, expected);
}

static BOOL is(object *self, int slot, INT32 expected)
{
	INT32 value = -12345;
	return CALL(self, slot, number_method)(self, &value) == S_OK && value == expected;
}

static BOOL put(object *self, int slot, INT32 value) { return CALL(self, slot, set_method)(self, value) == S_OK; }

static INT64 ticks_of(WORD year, WORD month, WORD day, WORD hour, WORD minute, WORD second)
{
	SYSTEMTIME when = {year, month, 0, day, hour, minute, second, 0};
	FILETIME file = {0, 0};
	SystemTimeToFileTime(&when, &file);
	return (INT64)(((UINT64)file.dwHighDateTime << 32) | file.dwLowDateTime);
}

static INT64 moment_of(object *self)
{
	INT64 value = 0;
	return CALL(self, GET_TIME, moment_method)(self, &value) == S_OK ? value : -1;
}

/* Every field of the calendar at once, as the checks give them. */
static BOOL at(object *self, INT32 year, INT32 month, INT32 day, INT32 hour, INT32 minute, INT32 second)
{
	return is(self, YEAR, year) && is(self, MONTH, month) && is(self, DAY, day) && is(self, HOUR, hour) && is(self, MINUTE, minute) &&
		is(self, SECOND, second);
}

int raven_entry(void)
{
	struct reference names[4];
	if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return 1;
	HSTRING class_name = text(&names[0], L"Windows.Globalization.Calendar");
	object *factory = NULL, *factory2 = NULL, *activation = NULL, *calendar = NULL, *copy = NULL, *zone = NULL, *list = NULL;
	if (RoGetActivationFactory(class_name, &factory_iid, (void **)&factory) != S_OK ||
		RoGetActivationFactory(class_name, &factory2_iid, (void **)&factory2) != S_OK ||
		RoGetActivationFactory(class_name, &activation_iid, (void **)&activation) != S_OK)
		return 2;
	if (CALL(factory2, 6, HRESULT (WINAPI *)(object *, void *, HSTRING, HSTRING, HSTRING, object **))(factory2, &languages,
			text(&names[1], L"GregorianCalendar"), text(&names[2], L"24HourClock"), text(&names[3], L"UTC"), &calendar) != S_OK)
		return 3;
	/* What it is. */
	if (!says(calendar, GET_SYSTEM, L"GregorianCalendar") || !says(calendar, GET_CLOCK, L"24HourClock") ||
		!says(calendar, NUMERALS, L"Latn") || !says(calendar, RESOLVED_LANGUAGE, L"en-US"))
		return 4;
	UINT32 size = 0;
	HSTRING first = NULL;
	if (CALL(calendar, LANGUAGES, object_method)(calendar, &list) != S_OK ||
		CALL(list, 7, HRESULT (WINAPI *)(object *, UINT32 *))(list, &size) != S_OK || size != 1 ||
		CALL(list, 6, HRESULT (WINAPI *)(object *, UINT32, HSTRING *))(list, 0, &first) != S_OK || !same_text(first, L"en-US"))
		return 5;
	release(list);
	if (CALL(calendar, 0, query_method)(calendar, &zone_iid, (void **)&zone) != S_OK || !says(zone, 6, L"UTC"))
		return 6;
	/* Monday 5 January 2026, 14:07:09.1234567 UTC, field by field. */
	const INT64 moment = ticks_of(2026, 1, 5, 14, 7, 9) + 1234567;
	if (CALL(calendar, SET_TIME, set_moment_method)(calendar, moment) != S_OK || moment_of(calendar) != moment)
		return 7;
	if (!at(calendar, 2026, 1, 5, 14, 7, 9) || !is(calendar, NANOSECOND, 123456700) || !is(calendar, WEEKDAY, 1) || !is(calendar, ERA, 1))
		return 8;
	if (!is(calendar, FIRST_ERA, 1) || !is(calendar, LAST_ERA, 1) || !is(calendar, ERAS, 1) || !is(calendar, FIRST_MONTH, 1) ||
		!is(calendar, LAST_MONTH, 12) || !is(calendar, MONTHS, 12) || !is(calendar, FIRST_DAY, 1) || !is(calendar, LAST_DAY, 31) ||
		!is(calendar, DAYS, 31) || !is(calendar, PERIODS, 1) || !is(calendar, FIRST_HOUR, 0) || !is(calendar, LAST_HOUR, 23) ||
		!is(calendar, HOURS, 24) || !is(calendar, FIRST_MINUTE, 0) || !is(calendar, LAST_MINUTE, 59) || !is(calendar, MINUTES, 60) ||
		!is(calendar, SECONDS, 60) || !is(calendar, FIRST_YEAR, 1) || !is(calendar, LAST_YEAR, 9999))
		return 9;
	BYTE daylight = 1;
	if (CALL(calendar, DAYLIGHT, HRESULT (WINAPI *)(object *, BYTE *))(calendar, &daylight) != S_OK || daylight)
		return 10;
	/* As text. */
	if (!says(calendar, MONTH_FULL, L"January") || !says_sized(calendar, MONTH_TEXT, 0, L"Jan") ||
		!says(calendar, MONTH_FULL_SOLO, L"January") || !says_sized(calendar, MONTH_SOLO, 3, L"Jan") ||
		!says(calendar, MONTH_NUMERIC, L"1") || !says_sized(calendar, MONTH_PADDED, 2, L"01"))
		return 11;
	if (!says(calendar, WEEKDAY_FULL, L"Monday") || !says_sized(calendar, WEEKDAY_TEXT, 0, L"Mon") ||
		!says(calendar, WEEKDAY_FULL_SOLO, L"Monday") || !says_sized(calendar, WEEKDAY_SOLO, 0, L"Mon"))
		return 12;
	if (!says(calendar, YEAR_TEXT, L"2026") || !says_sized(calendar, YEAR_TRUNCATED, 2, L"26") ||
		!says_sized(calendar, YEAR_PADDED, 6, L"002026") || !says(calendar, DAY_TEXT, L"5") || !says_sized(calendar, DAY_PADDED, 2, L"05") ||
		!says(calendar, HOUR_TEXT, L"14") || !says_sized(calendar, MINUTE_PADDED, 2, L"07") || !says(calendar, MINUTE_TEXT, L"7") ||
		!says(calendar, SECOND_TEXT, L"9") || !says_sized(calendar, SECOND_PADDED, 2, L"09") ||
		!says(calendar, NANOSECOND_TEXT, L"123456700") || !says_sized(calendar, NANOSECOND_PADDED, 12, L"000123456700"))
		return 13;
	if (!says(calendar, ERA_FULL, L"A.D.") || !says(calendar, PERIOD_FULL, L"PM") || !says_sized(calendar, PERIOD_TEXT, 0, L"PM"))
		return 14;
	/* The 12-hour clock: the afternoon is the second period, its hours 1 to 12. */
	if (CALL(calendar, CHANGE_CLOCK, name_method)(calendar, text(&names[1], L"12HourClock")) != S_OK ||
		!says(calendar, GET_CLOCK, L"12HourClock") || !is(calendar, HOUR, 2) || !is(calendar, PERIOD, 2) || !is(calendar, PERIODS, 2) ||
		!is(calendar, FIRST_HOUR, 1) || !is(calendar, LAST_HOUR, 12) || !is(calendar, HOURS, 12))
		return 15;
	if (!put(calendar, PUT_HOUR, 12) || !is(calendar, PERIOD, 2) || !is(calendar, HOUR, 12) || !put(calendar, PUT_PERIOD, 1) ||
		!is(calendar, PERIOD, 1) || !is(calendar, HOUR, 12) || !says(calendar, PERIOD_FULL, L"AM") || put(calendar, PUT_HOUR, 0) ||
		put(calendar, PUT_PERIOD, 3))
		return 16;
	if (!put(calendar, ADD_PERIODS, 1) || !is(calendar, PERIOD, 2) || !is(calendar, HOUR, 12) || !is(calendar, DAY, 5))
		return 17;
	/* Back on 24 hours: noon of the same day. */
	if (CALL(calendar, CHANGE_CLOCK, name_method)(calendar, text(&names[1], L"24HourClock")) != S_OK || !at(calendar, 2026, 1, 5, 12, 7, 9))
		return 18;
	/* Adding keeps the day in the month it lands in, and carries over. */
	if (CALL(calendar, SET_TIME, set_moment_method)(calendar, ticks_of(2026, 1, 31, 23, 59, 30)) != S_OK || !put(calendar, ADD_MONTHS, 1) ||
		!at(calendar, 2026, 2, 28, 23, 59, 30) || !is(calendar, DAYS, 28) || !put(calendar, ADD_SECONDS, 30) || !at(calendar, 2026, 3, 1, 0, 0, 0))
		return 19;
	if (!put(calendar, ADD_DAYS, -1) || !at(calendar, 2026, 2, 28, 0, 0, 0) || !put(calendar, ADD_YEARS, -2) || !at(calendar, 2024, 2, 28, 0, 0, 0) ||
		!is(calendar, DAYS, 29) || !put(calendar, ADD_WEEKS, 1) || !at(calendar, 2024, 3, 6, 0, 0, 0) || !put(calendar, ADD_HOURS, -25) ||
		!at(calendar, 2024, 3, 4, 23, 0, 0) || !put(calendar, ADD_MINUTES, 61) || !at(calendar, 2024, 3, 5, 0, 1, 0) ||
		!put(calendar, ADD_NANOSECONDS, 1500) || !is(calendar, NANOSECOND, 1500))
		return 20;
	/* Setting a field leaves the others; a day the month does not have is refused. */
	if (!put(calendar, PUT_YEAR, 2025) || !put(calendar, PUT_MONTH, 12) || !put(calendar, PUT_DAY, 24) || !put(calendar, PUT_HOUR, 18) ||
		!put(calendar, PUT_MINUTE, 30) || !put(calendar, PUT_SECOND, 15) || !put(calendar, PUT_NANOSECOND, 0) || !at(calendar, 2025, 12, 24, 18, 30, 15) ||
		moment_of(calendar) != ticks_of(2025, 12, 24, 18, 30, 15) || !is(calendar, WEEKDAY, 3))
		return 21;
	if (put(calendar, PUT_DAY, 32) || put(calendar, PUT_MONTH, 13) || put(calendar, PUT_HOUR, 24) || put(calendar, PUT_MINUTE, 60) ||
		put(calendar, PUT_NANOSECOND, 1000000000) || !at(calendar, 2025, 12, 24, 18, 30, 15))
		return 22;
	/* Copies, comparisons and the ends of the calendar. */
	INT32 order = 5;
	if (CALL(calendar, CLONE, object_method)(calendar, &copy) != S_OK || moment_of(copy) != moment_of(calendar) ||
		CALL(calendar, COMPARE, HRESULT (WINAPI *)(object *, object *, INT32 *))(calendar, copy, &order) != S_OK || order != 0 ||
		!put(copy, ADD_SECONDS, 1) || CALL(calendar, COMPARE, HRESULT (WINAPI *)(object *, object *, INT32 *))(calendar, copy, &order) != S_OK ||
		order != -1 || CALL(copy, COMPARE_TIME, HRESULT (WINAPI *)(object *, INT64, INT32 *))(copy, moment_of(calendar), &order) != S_OK ||
		order != 1)
		return 23;
	if (CALL(calendar, COPY_TO, HRESULT (WINAPI *)(object *, object *))(calendar, copy) != S_OK || moment_of(copy) != moment_of(calendar))
		return 24;
	if (CALL(copy, SET_TO_MIN, HRESULT (WINAPI *)(object *))(copy) != S_OK || !is(copy, YEAR, 1) || !is(copy, MONTH, 1) || !is(copy, DAY, 1) ||
		CALL(copy, SET_TO_MAX, HRESULT (WINAPI *)(object *))(copy) != S_OK || !at(copy, 9999, 12, 31, 23, 59, 59) || put(copy, ADD_DAYS, 1))
		return 25;
	/* Other digits; a numeral system, a calendar or a clock that does not exist is refused. */
	if (CALL(calendar, PUT_NUMERALS, name_method)(calendar, text(&names[1], L"Arab")) != S_OK || !says(calendar, NUMERALS, L"Arab") ||
		!says(calendar, YEAR_TEXT, L"\x0662\x0660\x0662\x0665"))
		return 26;
	if (CALL(calendar, PUT_NUMERALS, name_method)(calendar, text(&names[1], L"Nope")) == S_OK ||
		CALL(calendar, CHANGE_SYSTEM, name_method)(calendar, text(&names[1], L"NopeCalendar")) == S_OK ||
		CALL(calendar, CHANGE_CLOCK, name_method)(calendar, text(&names[1], L"7HourClock")) == S_OK)
		return 27;
	release(copy);
	release(zone);
	release(calendar);
	/* One with the user's languages, calendar, clock and zone: its fields are the local time Windows gives for the same moment. */
	calendar = NULL;
	if (CALL(activation, 6, object_method)(activation, &calendar) != S_OK)
		return 28;
	const INT64 noon = ticks_of(2026, 7, 15, 12, 0, 0);
	FILETIME file = {(DWORD)noon, (DWORD)((UINT64)noon >> 32)};
	SYSTEMTIME universal, local;
	FileTimeToSystemTime(&file, &universal);
	SystemTimeToTzSpecificLocalTime(NULL, &universal, &local);
	if (CALL(calendar, SET_TIME, set_moment_method)(calendar, noon) != S_OK || !is(calendar, YEAR, local.wYear) ||
		!is(calendar, MONTH, local.wMonth) || !is(calendar, DAY, local.wDay) || !is(calendar, MINUTE, local.wMinute) ||
		!says(calendar, GET_SYSTEM, L"GregorianCalendar"))
		return 29;
	if (CALL(calendar, 0, query_method)(calendar, &calendar_iid, (void **)&copy) != S_OK)
		return 30;
	release(copy);
	release(calendar);
	/* The other factory: the user's calendar and clock for the languages given. */
	if (CALL(factory, 6, HRESULT (WINAPI *)(object *, void *, object **))(factory, &languages, &calendar) != S_OK ||
		!says(calendar, RESOLVED_LANGUAGE, L"en-US") || CALL(calendar, SET_TO_NOW, HRESULT (WINAPI *)(object *))(calendar) != S_OK ||
		CALL(calendar, YEAR, number_method)(calendar, &order) != S_OK || order < 2026)
		return 31;
	release(calendar);
	release(activation);
	release(factory2);
	release(factory);
	return 0;
}
