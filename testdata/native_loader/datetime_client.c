// Windows.Globalization.DateTimeFormatting.DateTimeFormatter as an app uses it: the four standard formatters, templates of parts,
// patterns, a context with its own languages and clock, the parts given one by one, a numeral system of other digits, and the failures.
// Monday 5 January 2026, 14:07:09 UTC is written in UTC, so the text does not depend on the machine's zone; Format, which writes the
// local time, is checked against the local time GetTimeFormatEx writes. The exit code names the first check that failed.
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
typedef HRESULT (WINAPI *number_method)(object *, INT32 *);
typedef HRESULT (WINAPI *template_method)(object *, HSTRING, object **);

static const GUID factory_iid = {0xec8d8a53, 0x1a2e, 0x412d, {0x88, 0x15, 0x3b, 0x74, 0x5f, 0xb1, 0xa2, 0xa0}};
static const GUID statics_iid = {0xbfcde7c0, 0xdf4c, 0x4a2e, {0x90, 0x12, 0xf4, 0x7d, 0xaf, 0x3f, 0x12, 0x12}};
static const GUID formatter2_iid = {0x27c91a86, 0xbdaa, 0x4fd0, {0x9e, 0x36, 0x67, 0x1d, 0x5a, 0xa5, 0xee, 0x03}};

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

static INT64 moment;

/* What a formatter writes for the moment in UTC is the text expected. */
static BOOL writes(object *formatter, const WCHAR *expected)
{
	object *second = NULL;
	HSTRING written = NULL;
	struct reference zone;
	BOOL same = formatter && CALL(formatter, 0, query_method)(formatter, &formatter2_iid, (void **)&second) == S_OK &&
		CALL(second, 6, HRESULT (WINAPI *)(object *, INT64, HSTRING, HSTRING *))(second, moment, text(&zone, L"UTC"), &written) == S_OK &&
		!lstrcmpW(WindowsGetStringRawBuffer(written, NULL), expected);
	WindowsDeleteString(written);
	release(second);
	return same;
}

static BOOL answers(object *self, int slot, const WCHAR *expected)
{
	HSTRING value = NULL;
	BOOL same = CALL(self, slot, text_method)(self, &value) == S_OK && !lstrcmpW(WindowsGetStringRawBuffer(value, NULL), expected);
	WindowsDeleteString(value);
	return same;
}

/* Patterns: one pattern, the one expected. */
static BOOL patterned(object *self, const WCHAR *expected)
{
	object *list = NULL;
	HSTRING value = NULL;
	UINT32 size = 0;
	BOOL same = CALL(self, 12, object_method)(self, &list) == S_OK && CALL(list, 7, HRESULT (WINAPI *)(object *, UINT32 *))(list, &size) == S_OK &&
		size == 1 && CALL(list, 6, HRESULT (WINAPI *)(object *, UINT32, HSTRING *))(list, 0, &value) == S_OK &&
		!lstrcmpW(WindowsGetStringRawBuffer(value, NULL), expected);
	WindowsDeleteString(value);
	release(list);
	return same;
}

static object *from_template(object *factory, const WCHAR *template_text)
{
	struct reference reference;
	object *formatter = NULL;
	return CALL(factory, 6, template_method)(factory, text(&reference, template_text), &formatter) == S_OK ? formatter : NULL;
}

int raven_entry(void)
{
	struct reference reference;
	if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return 1;
	SYSTEMTIME when = {2026, 1, 1, 5, 14, 7, 9, 0};
	FILETIME file;
	if (!SystemTimeToFileTime(&when, &file)) return 2;
	moment = (INT64)(((UINT64)file.dwHighDateTime << 32) | file.dwLowDateTime);
	HSTRING name = text(&reference, L"Windows.Globalization.DateTimeFormatting.DateTimeFormatter");
	object *factory = NULL, *statics = NULL, *formatter = NULL, *list = NULL;
	if (RoGetActivationFactory(name, &factory_iid, (void **)&factory) != S_OK ||
		RoGetActivationFactory(name, &statics_iid, (void **)&statics) != S_OK)
		return 3;
	/* The four standard formatters. */
	static const WCHAR *const standard[] = {L"Monday, January 5, 2026", L"2:07:09 PM", L"1/5/2026", L"2:07 PM"};
	static const WCHAR *const standard_names[] = {L"longdate", L"longtime", L"shortdate", L"shorttime"};
	for (int index = 0; index < 4; ++index) {
		if (CALL(statics, 6 + index, object_method)(statics, &formatter) != S_OK || !writes(formatter, standard[index]) ||
			!answers(formatter, 13, standard_names[index]))
			return 10 + index;
		release(formatter);
	}
	/* What a formatter of the user's says about itself. */
	formatter = from_template(factory, L"shortdate");
	INT32 part = -1;
	UINT32 size = 0;
	HSTRING written = NULL;
	if (!formatter || !answers(formatter, 7, L"US") || !answers(formatter, 8, L"GregorianCalendar") || !answers(formatter, 9, L"12HourClock") ||
		!answers(formatter, 10, L"Latn") || !answers(formatter, 22, L"en-US") || !answers(formatter, 23, L"US") ||
		CALL(formatter, 15, number_method)(formatter, &part) != S_OK || part != 1 ||
		CALL(formatter, 17, number_method)(formatter, &part) != S_OK || part != 0)
		return 20;
	if (CALL(formatter, 6, object_method)(formatter, &list) != S_OK || CALL(list, 7, HRESULT (WINAPI *)(object *, UINT32 *))(list, &size) != S_OK ||
		size != 1 || CALL(list, 6, HRESULT (WINAPI *)(object *, UINT32, HSTRING *))(list, 0, &written) != S_OK ||
		lstrcmpW(WindowsGetStringRawBuffer(written, NULL), L"en-US") || !patterned(formatter, L"{month.integer}/{day.integer}/{year.full}"))
		return 21;
	WindowsDeleteString(written);
	release(list);
	/* Other digits, and a numeral system that does not exist. */
	if (CALL(formatter, 11, HRESULT (WINAPI *)(object *, HSTRING))(formatter, text(&reference, L"Arab")) != S_OK ||
		!answers(formatter, 10, L"Arab") || !writes(formatter, L"\x0661/\x0665/\x0662\x0660\x0662\x0666"))
		return 22;
	if (CALL(formatter, 11, HRESULT (WINAPI *)(object *, HSTRING))(formatter, text(&reference, L"Nope")) != E_INVALIDARG)
		return 23;
	/* Format writes the local time. */
	SYSTEMTIME local;
	WCHAR expected[64];
	written = NULL;
	release(formatter);
	formatter = from_template(factory, L"shorttime");
	if (!SystemTimeToTzSpecificLocalTime(NULL, &when, &local) || !GetTimeFormatEx(L"en-US", 0, &local, L"h:mm tt", expected, 64) ||
		CALL(formatter, 14, HRESULT (WINAPI *)(object *, INT64, HSTRING *))(formatter, moment, &written) != S_OK ||
		lstrcmpW(WindowsGetStringRawBuffer(written, NULL), expected))
		return 24;
	WindowsDeleteString(written);
	/* A zone with no rules here is refused rather than written in another. */
	object *second = NULL;
	CALL(formatter, 0, query_method)(formatter, &formatter2_iid, (void **)&second);
	written = NULL;
	if (CALL(second, 6, HRESULT (WINAPI *)(object *, INT64, HSTRING, HSTRING *))(second, moment, text(&reference, L"Mars/Base"), &written) !=
		E_INVALIDARG || written)
		return 25;
	release(second);
	release(formatter);
	/* Templates of parts, in the forms asked for. */
	static const struct {
		const WCHAR *template_text, *expected;
	} templates[] = {
		{L"month day", L"January 5"},
		{L"dayofweek.abbreviated month.abbreviated day year.abbreviated", L"Mon, Jan 5, 26"},
		{L"month.numeric day", L"1/5"},
		{L"hour minute second", L"2:07:09 PM"},
		{L"day month year hour minute", L"January 5, 2026 2:07 PM"},
		{L"{dayofweek.full} {day.integer(2)}.{month.integer(2)}.{year.full}", L"Monday 05.01.2026"},
		{L"shortdate shorttime", L"1/5/2026 2:07 PM"},
	};
	for (int index = 0; index < (int)(sizeof templates / sizeof *templates); ++index) {
		formatter = from_template(factory, templates[index].template_text);
		if (!writes(formatter, templates[index].expected))
			return 30 + index;
		release(formatter);
	}
	/* Templates and parts that name nothing, or a part twice. */
	static const WCHAR *const refused[] = {L"bogus", L"", L"year year", L"{nothing}", L"{month.full"};
	for (int index = 0; index < (int)(sizeof refused / sizeof *refused); ++index) {
		formatter = NULL;
		if (CALL(factory, 6, template_method)(factory, text(&reference, refused[index]), &formatter) != E_INVALIDARG || formatter)
			return 40 + index;
	}
	if (CALL(factory, 9, HRESULT (WINAPI *)(object *, INT32, INT32, INT32, INT32, object **))(factory, 0, 0, 0, 0, &formatter) != E_INVALIDARG ||
		CALL(factory, 10, HRESULT (WINAPI *)(object *, INT32, INT32, INT32, object **))(factory, 0, 7, 0, &formatter) != E_INVALIDARG)
		return 46;
	/* The parts one by one: the date with an abbreviated month, the time without seconds. */
	if (CALL(factory, 9, HRESULT (WINAPI *)(object *, INT32, INT32, INT32, INT32, object **))(factory, 3, 2, 1, 0, &formatter) != S_OK ||
		!writes(formatter, L"Jan 5, 2026") || CALL(formatter, 16, number_method)(formatter, &part) != S_OK || part != 2)
		return 50;
	release(formatter);
	if (CALL(factory, 10, HRESULT (WINAPI *)(object *, INT32, INT32, INT32, object **))(factory, 1, 1, 0, &formatter) != S_OK ||
		!writes(formatter, L"2:07 PM"))
		return 51;
	release(formatter);
	/* A context of its own: the languages given, a region and a 24-hour clock. */
	struct reference region, calendar, clock;
	if (CALL(factory, 8, HRESULT (WINAPI *)(object *, HSTRING, void *, HSTRING, HSTRING, HSTRING, object **))(factory,
			text(&reference, L"hour minute"), &languages, text(&region, L"GB"), text(&calendar, L"GregorianCalendar"),
			text(&clock, L"24HourClock"), &formatter) != S_OK ||
		!writes(formatter, L"14:07") || !answers(formatter, 9, L"24HourClock") || !answers(formatter, 7, L"GB") ||
		!patterned(formatter, L"{hour.integer}:{minute.integer(2)}"))
		return 60;
	release(formatter);
	if (CALL(factory, 8, HRESULT (WINAPI *)(object *, HSTRING, void *, HSTRING, HSTRING, HSTRING, object **))(factory,
			text(&reference, L"hour minute"), &languages, text(&region, L"GB"), text(&calendar, L"GregorianCalendar"),
			text(&clock, L"25HourClock"), &formatter) != E_INVALIDARG)
		return 61;
	if (languages.references != 1 || language_iterator.references != 1)
		return 62;
	/* The year with its month comes from the language's own picture of the two. */
	formatter = from_template(factory, L"month year");
	if (!writes(formatter, L"January 2026"))
		return 70;
	release(formatter);
	release(statics);
	release(factory);
	return 0;
}
