/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The header collections of requests, responses and content: maps of header names to values, names compared ignoring case and values
 * of a repeated header joined by a comma (RFC 9110, 5.2 and 5.3). The typed properties read and write the same map: dates as HTTP-dates
 * (5.6.7), lengths and ages as numbers, Retry-After as a date or a number of seconds (10.2.3), Connection as a list of options and
 * User-Agent as products and comments (10.1.5). Typed values this component does not model yet answer E_NOTIMPL; their headers are
 * still in the map. */
#include "web.h"

typedef __x_ABI_CWindows_CFoundation_CDateTime date_time;
typedef __x_ABI_CWindows_CFoundation_CTimeSpan time_span;
typedef __FIReference_1_DateTime date_reference;
typedef __FIReference_1_TimeSpan span_reference;
typedef __FIReference_1_UINT64 length_reference;
typedef __FIReference_1_UINT32 count_reference;
typedef __x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpDateOrDeltaHeaderValue date_or_delta;

struct headers {
	struct winrt_object base;
	struct winrt_interface collection, stringable;
	struct winrt_map map;
	enum headers_kind kind;
};

static struct headers *headers_of(void *iface) { return (struct headers *)winrt_object_of(iface); }

/* Names and values. */

static BOOL token_character(WCHAR character)
{
	static const char others[] = "!#$%&'*+-.^_`|~";
	if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9'))
		return TRUE;
	for (const char *other = others; *other; ++other)
		if (character == *other)
			return TRUE;
	return FALSE;
}

static BOOL valid_name(const WCHAR *name, UINT32 length)
{
	if (!length)
		return FALSE;
	for (UINT32 index = 0; index < length; ++index)
		if (!token_character(name[index]))
			return FALSE;
	return TRUE;
}

static BOOL valid_value(const WCHAR *value, UINT32 length)
{
	for (UINT32 index = 0; index < length; ++index)
		if (value[index] == '\r' || value[index] == '\n' || !value[index])
			return FALSE;
	return TRUE;
}

BOOL header_is_content(const WCHAR *name, UINT32 length)
{
	static const WCHAR prefix[] = L"Content-", expires[] = L"Expires", modified[] = L"Last-Modified";
	return (length > 8 && winrt_equal_ascii(name, 8, prefix, 8)) || winrt_equal_ascii(name, length, expires, 7) ||
		winrt_equal_ascii(name, length, modified, 13);
}

static HRESULT get_text(void *iface, const WCHAR *name, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	const HRESULT result = winrt_map_get(&headers_of(iface)->map, name, out);
	return result == E_BOUNDS ? S_OK : result;
}

static HRESULT put_text(void *iface, const WCHAR *name, const WCHAR *value, UINT32 length)
{
	struct winrt_map *map = &headers_of(iface)->map;
	if (!value) {
		winrt_map_remove(map, name, lstrlenW(name));
		return S_OK;
	}
	return winrt_map_set(map, name, lstrlenW(name), value, length, FALSE);
}

/* HTTP-dates. */

static const char weekdays[7][4] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char months[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/* Two digits, or four for a year. */
static WCHAR *put_digits(WCHAR *at, unsigned value, unsigned count)
{
	for (unsigned index = count; index--;) {
		at[index] = (WCHAR)('0' + value % 10);
		value /= 10;
	}
	return at + count;
}

static WCHAR *put_name(WCHAR *at, const char *name)
{
	while (*name)
		*at++ = (WCHAR)*name++;
	return at;
}

/* The IMF-fixdate form: "Sun, 06 Nov 1994 08:49:37 GMT". */
static HRESULT format_date(date_time when, HSTRING *out)
{
	SYSTEMTIME time;
	FILETIME file = {(DWORD)when.UniversalTime, (DWORD)((UINT64)when.UniversalTime >> 32)};
	if (!FileTimeToSystemTime(&file, &time))
		return E_INVALIDARG;
	WCHAR text[32], *at = text;
	at = put_name(at, weekdays[time.wDayOfWeek]);
	at = put_name(at, ", ");
	at = put_digits(at, time.wDay, 2);
	*at++ = ' ';
	at = put_name(at, months[time.wMonth - 1]);
	*at++ = ' ';
	at = put_digits(at, time.wYear, 4);
	*at++ = ' ';
	at = put_digits(at, time.wHour, 2);
	*at++ = ':';
	at = put_digits(at, time.wMinute, 2);
	*at++ = ':';
	at = put_digits(at, time.wSecond, 2);
	at = put_name(at, " GMT");
	return winrt_string_n(text, (UINT32)(at - text), out);
}

static BOOL read_number(const WCHAR **at, const WCHAR *end, unsigned digits_min, unsigned digits_max, unsigned *value)
{
	unsigned count = 0, number = 0;
	while (*at < end && **at == ' ' && digits_min == 1)
		++*at;
	while (*at < end && count < digits_max && **at >= '0' && **at <= '9') {
		number = number * 10 + (unsigned)(**at - '0');
		++*at;
		++count;
	}
	*value = number;
	return count >= digits_min;
}

static BOOL read_month(const WCHAR **at, const WCHAR *end, unsigned *month)
{
	if (end - *at < 3)
		return FALSE;
	for (unsigned index = 0; index < 12; ++index)
		if (winrt_equal_ascii(*at, 3, (const WCHAR[]){months[index][0], months[index][1], months[index][2]}, 3)) {
			*month = index + 1;
			*at += 3;
			return TRUE;
		}
	return FALSE;
}

static BOOL expect(const WCHAR **at, const WCHAR *end, WCHAR character)
{
	if (*at >= end || **at != character)
		return FALSE;
	++*at;
	return TRUE;
}

static BOOL read_clock(const WCHAR **at, const WCHAR *end, SYSTEMTIME *time)
{
	unsigned hour, minute, second;
	if (!read_number(at, end, 2, 2, &hour) || !expect(at, end, ':') || !read_number(at, end, 2, 2, &minute) || !expect(at, end, ':') ||
		!read_number(at, end, 2, 2, &second))
		return FALSE;
	time->wHour = (WORD)hour;
	time->wMinute = (WORD)minute;
	time->wSecond = (WORD)second;
	return TRUE;
}

/* Any of the three forms a recipient accepts (RFC 9110, 5.6.7): IMF-fixdate, the obsolete RFC 850 form and asctime's. */
static BOOL parse_date(const WCHAR *text, UINT32 length, date_time *out)
{
	const WCHAR *at = text, *end = text + length;
	SYSTEMTIME time = {0};
	unsigned day, month, year;
	while (at < end && *at == ' ')
		++at;
	while (end > at && end[-1] == ' ')
		--end;
	const WCHAR *comma = at;
	while (comma < end && *comma != ',' && *comma != ' ')
		++comma;
	if (comma < end && *comma == ',') {
		at = comma + 1;
		while (at < end && *at == ' ')
			++at;
		if (end - at > 2 && at[2] == '-') {
			/* RFC 850: 06-Nov-94 08:49:37 GMT, with the century taken as the nearest one. */
			if (!read_number(&at, end, 2, 2, &day) || !expect(&at, end, '-') || !read_month(&at, end, &month) || !expect(&at, end, '-') ||
				!read_number(&at, end, 2, 2, &year))
				return FALSE;
			year += year < 70 ? 2000 : 1900;
		} else if (!read_number(&at, end, 1, 2, &day) || !expect(&at, end, ' ') || !read_month(&at, end, &month) ||
			!expect(&at, end, ' ') || !read_number(&at, end, 4, 4, &year))
			return FALSE;
		if (!expect(&at, end, ' ') || !read_clock(&at, end, &time))
			return FALSE;
	} else {
		/* asctime: Sun Nov  6 08:49:37 1994 */
		at = comma;
		if (!expect(&at, end, ' ') || !read_month(&at, end, &month) || !expect(&at, end, ' ') || !read_number(&at, end, 1, 2, &day) ||
			!expect(&at, end, ' ') || !read_clock(&at, end, &time) || !expect(&at, end, ' ') || !read_number(&at, end, 4, 4, &year))
			return FALSE;
	}
	time.wYear = (WORD)year;
	time.wMonth = (WORD)month;
	time.wDay = (WORD)day;
	FILETIME file;
	if (!SystemTimeToFileTime(&time, &file))
		return FALSE;
	out->UniversalTime = (INT64)(((UINT64)file.dwHighDateTime << 32) | file.dwLowDateTime);
	return TRUE;
}

static HRESULT get_date(void *iface, const WCHAR *name, date_reference **out)
{
	HSTRING text = NULL;
	date_time when;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = get_text(iface, name, &text);
	UINT32 length;
	const WCHAR *characters = winrt_text(text, &length);
	if (SUCCEEDED(result) && text && parse_date(characters, length, &when))
		result = winrt_box(&IID___FIReference_1_DateTime, &when, sizeof(when), (void **)out);
	WindowsDeleteString(text);
	return result;
}

static HRESULT put_date(void *iface, const WCHAR *name, date_reference *value)
{
	HSTRING text = NULL;
	date_time when;
	if (!value)
		return put_text(iface, name, NULL, 0);
	HRESULT result = value->lpVtbl->get_Value(value, &when);
	if (SUCCEEDED(result))
		result = format_date(when, &text);
	UINT32 length;
	const WCHAR *characters = winrt_text(text, &length);
	if (SUCCEEDED(result))
		result = put_text(iface, name, characters, length);
	WindowsDeleteString(text);
	return result;
}

static BOOL parse_unsigned(const WCHAR *text, UINT32 length, UINT64 *out)
{
	UINT64 number = 0;
	if (!length)
		return FALSE;
	for (UINT32 index = 0; index < length; ++index) {
		if (text[index] < '0' || text[index] > '9' || number > (~(UINT64)0 - 9) / 10)
			return FALSE;
		number = number * 10 + (UINT64)(text[index] - '0');
	}
	*out = number;
	return TRUE;
}

static HRESULT put_number(void *iface, const WCHAR *name, UINT64 number)
{
	WCHAR text[20];
	return put_text(iface, name, text, winrt_decimal(number, text));
}

static HRESULT get_number(void *iface, const WCHAR *name, BOOL *present, UINT64 *number)
{
	HSTRING text = NULL;
	*present = FALSE;
	HRESULT result = get_text(iface, name, &text);
	UINT32 length;
	const WCHAR *characters = winrt_text(text, &length);
	if (SUCCEEDED(result) && text)
		*present = parse_unsigned(characters, length, number);
	WindowsDeleteString(text);
	return result;
}

static HRESULT get_uri(void *iface, const WCHAR *name, uri_class **out)
{
	HSTRING text = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = get_text(iface, name, &text);
	UINT32 length;
	const WCHAR *characters = winrt_text(text, &length);
	if (SUCCEEDED(result) && text && FAILED(uri_create(characters, length, out)))
		*out = NULL;
	WindowsDeleteString(text);
	return result;
}

static HRESULT put_uri(void *iface, const WCHAR *name, uri_class *value)
{
	HSTRING text = NULL;
	if (!value)
		return put_text(iface, name, NULL, 0);
	HRESULT result = uri_text(value, &text);
	UINT32 length;
	const WCHAR *characters = winrt_text(text, &length);
	if (SUCCEEDED(result))
		result = put_text(iface, name, characters, length);
	WindowsDeleteString(text);
	return result;
}

/* Header values: Retry-After, a date or a delay in seconds. */

struct date_or_delta_value {
	struct winrt_object base;
	struct winrt_interface value, stringable;
	BOOL is_date;
	date_time date;
	time_span delta;
};

static struct date_or_delta_value *date_or_delta_of(void *iface) { return (struct date_or_delta_value *)winrt_object_of(iface); }

static HRESULT STDMETHODCALLTYPE date_or_delta_date(void *iface, date_reference **out)
{
	struct date_or_delta_value *self = date_or_delta_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	return self->is_date ? winrt_box(&IID___FIReference_1_DateTime, &self->date, sizeof(self->date), (void **)out) : S_OK;
}

static HRESULT STDMETHODCALLTYPE date_or_delta_delta(void *iface, span_reference **out)
{
	struct date_or_delta_value *self = date_or_delta_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	return self->is_date ? S_OK : winrt_box(&IID___FIReference_1_TimeSpan, &self->delta, sizeof(self->delta), (void **)out);
}

static HRESULT date_or_delta_text(struct date_or_delta_value *self, HSTRING *out);

static HRESULT STDMETHODCALLTYPE date_or_delta_string(void *iface, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return date_or_delta_text(date_or_delta_of(iface), out);
}

static const __x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpDateOrDeltaHeaderValueVtbl date_or_delta_table = {WINRT_INSPECTABLE,
	(void *)date_or_delta_date, (void *)date_or_delta_delta};
static const __x_ABI_CWindows_CFoundation_CIStringableVtbl date_or_delta_stringable_table = {WINRT_INSPECTABLE, (void *)date_or_delta_string};
static const struct winrt_entry date_or_delta_entries[] = {
	WINRT_ENTRY(struct date_or_delta_value, value, IID___x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpDateOrDeltaHeaderValue),
	WINRT_ENTRY(struct date_or_delta_value, stringable, IID___x_ABI_CWindows_CFoundation_CIStringable), WINRT_END};
static const struct winrt_class date_or_delta_kind = {L"Windows.Web.Http.Headers.HttpDateOrDeltaHeaderValue", date_or_delta_entries, NULL};

static HRESULT date_or_delta_text(struct date_or_delta_value *self, HSTRING *out)
{
	if (self->is_date)
		return format_date(self->date, out);
	WCHAR text[20];
	return winrt_string_n(text, winrt_decimal((UINT64)self->delta.Duration / 10000000, text), out);
}

static HRESULT parse_date_or_delta(const WCHAR *text, UINT32 length, date_or_delta **out)
{
	UINT64 seconds;
	date_time when;
	while (length && *text == ' ') {
		++text;
		--length;
	}
	while (length && text[length - 1] == ' ')
		--length;
	const BOOL is_delta = parse_unsigned(text, length, &seconds);
	if (!is_delta && !parse_date(text, length, &when))
		return E_INVALIDARG;
	struct date_or_delta_value *self = winrt_new(&date_or_delta_kind, sizeof(*self), NULL);
	if (!self)
		return E_OUTOFMEMORY;
	self->value.table = &date_or_delta_table;
	self->stringable.table = &date_or_delta_stringable_table;
	self->is_date = !is_delta;
	if (is_delta)
		self->delta.Duration = (INT64)(seconds * 10000000);
	else
		self->date = when;
	*out = (void *)&self->value;
	return S_OK;
}

/* HttpDateOrDeltaHeaderValue's statics: Parse and TryParse. */
static HRESULT STDMETHODCALLTYPE statics_parse(void *iface, HSTRING input, date_or_delta **out)
{
	UINT32 length;
	const WCHAR *text = winrt_text(input, &length);
	if (!out)
		return E_POINTER;
	*out = NULL;
	return parse_date_or_delta(text, length, out);
}

static HRESULT STDMETHODCALLTYPE statics_try_parse(void *iface, HSTRING input, date_or_delta **value, boolean *succeeded)
{
	UINT32 length;
	const WCHAR *text = winrt_text(input, &length);
	if (!value || !succeeded)
		return E_POINTER;
	*value = NULL;
	const HRESULT result = parse_date_or_delta(text, length, value);
	*succeeded = SUCCEEDED(result);
	return result == E_OUTOFMEMORY ? result : S_OK;
}

struct statics {
	struct winrt_object base;
	struct winrt_interface activation, statics;
};

static HRESULT STDMETHODCALLTYPE statics_activate(void *iface, IInspectable **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return E_NOTIMPL;
}

static const IActivationFactoryVtbl statics_activation_table = {WINRT_INSPECTABLE, (void *)statics_activate};
static const __x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpDateOrDeltaHeaderValueStaticsVtbl statics_table = {WINRT_INSPECTABLE,
	(void *)statics_parse, (void *)statics_try_parse};
static const struct winrt_entry statics_entries[] = {
	WINRT_ENTRY(struct statics, activation, IID_IActivationFactory),
	WINRT_ENTRY(struct statics, statics, IID___x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpDateOrDeltaHeaderValueStatics), WINRT_END};
static const struct winrt_class statics_kind = {L"Windows.Web.Http.Headers.HttpDateOrDeltaHeaderValue", statics_entries, NULL};

HRESULT date_or_delta_statics(REFIID iid, void **out)
{
	struct statics *self = winrt_new(&statics_kind, sizeof(*self), NULL);
	if (!self)
		return E_OUTOFMEMORY;
	self->activation.table = &statics_activation_table;
	self->statics.table = &statics_table;
	const HRESULT result = winrt_query(&self->activation, iid, out);
	winrt_release(&self->activation);
	return result;
}

/* Lists of values in one header: the options of Connection, separated by commas, and the products and comments of User-Agent, separated
 * by spaces with comments in parentheses. A list reads and writes its header in the collection; its items are new value objects. */

enum list_kind {
	LIST_CONNECTION,
	LIST_PRODUCT
};

struct list {
	struct winrt_object base;
	struct winrt_interface collection, vector, iterable, stringable;
	struct headers *owner;
	enum list_kind kind;
	const WCHAR *name;
};

static struct list *list_of(void *iface) { return (struct list *)winrt_object_of(iface); }

static void list_destroy(struct winrt_object *object) { winrt_release(&((struct list *)object)->owner->collection); }

/* A value: an option (Token) or a product (Product: Name and Version) or comment (Comment), with its text. */
struct item {
	struct winrt_object base;
	struct winrt_interface value, stringable;
	HSTRING text, name, version;
	BOOL comment;
};

static struct item *item_of(void *iface) { return (struct item *)winrt_object_of(iface); }

static void item_destroy(struct winrt_object *object)
{
	struct item *self = (struct item *)object;
	WindowsDeleteString(self->text);
	WindowsDeleteString(self->name);
	WindowsDeleteString(self->version);
}

static HRESULT STDMETHODCALLTYPE item_text(void *iface, HSTRING *out) { return WindowsDuplicateString(item_of(iface)->text, out); }

static HRESULT STDMETHODCALLTYPE item_comment(void *iface, HSTRING *out)
{
	struct item *self = item_of(iface);
	if (!out)
		return E_POINTER;
	if (!self->comment) {
		*out = NULL;
		return S_OK;
	}
	UINT32 length;
	const WCHAR *text = winrt_text(self->text, &length);
	return winrt_string_n(text + 1, length >= 2 ? length - 2 : 0, out);
}

static HRESULT STDMETHODCALLTYPE item_name(void *iface, HSTRING *out) { return WindowsDuplicateString(item_of(iface)->name, out); }
static HRESULT STDMETHODCALLTYPE item_version(void *iface, HSTRING *out) { return WindowsDuplicateString(item_of(iface)->version, out); }
static HRESULT make_item(enum list_kind kind, const WCHAR *text, UINT32 length, BOOL product_only, IInspectable **out);

static HRESULT STDMETHODCALLTYPE item_product(void *iface, void **out)
{
	struct item *self = item_of(iface);
	UINT32 length;
	const WCHAR *text = winrt_text(self->text, &length);
	if (!out)
		return E_POINTER;
	*out = NULL;
	return self->comment ? S_OK : make_item(LIST_PRODUCT, text, length, TRUE, (IInspectable **)out);
}

static const __x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpConnectionOptionHeaderValueVtbl option_table = {WINRT_INSPECTABLE,
	(void *)item_text};
static const __x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpProductInfoHeaderValueVtbl product_info_table = {WINRT_INSPECTABLE,
	(void *)item_product, (void *)item_comment};
static const __x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpProductHeaderValueVtbl product_table = {WINRT_INSPECTABLE, (void *)item_name,
	(void *)item_version};
static const __x_ABI_CWindows_CFoundation_CIStringableVtbl item_stringable_table = {WINRT_INSPECTABLE, (void *)item_text};
static const struct winrt_entry option_entries[] = {
	WINRT_ENTRY(struct item, value, IID___x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpConnectionOptionHeaderValue),
	WINRT_ENTRY(struct item, stringable, IID___x_ABI_CWindows_CFoundation_CIStringable), WINRT_END};
static const struct winrt_entry product_info_entries[] = {
	WINRT_ENTRY(struct item, value, IID___x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpProductInfoHeaderValue),
	WINRT_ENTRY(struct item, stringable, IID___x_ABI_CWindows_CFoundation_CIStringable), WINRT_END};
static const struct winrt_entry product_entries[] = {
	WINRT_ENTRY(struct item, value, IID___x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpProductHeaderValue),
	WINRT_ENTRY(struct item, stringable, IID___x_ABI_CWindows_CFoundation_CIStringable), WINRT_END};
static const struct winrt_class option_kind = {L"Windows.Web.Http.Headers.HttpConnectionOptionHeaderValue", option_entries, item_destroy};
static const struct winrt_class product_info_kind = {L"Windows.Web.Http.Headers.HttpProductInfoHeaderValue", product_info_entries,
	item_destroy};
static const struct winrt_class product_kind = {L"Windows.Web.Http.Headers.HttpProductHeaderValue", product_entries, item_destroy};

/* One item of the list's syntax: a token for an option; a product (token, optionally /version) or a parenthesized comment for
 * User-Agent. */
static BOOL valid_item(enum list_kind kind, const WCHAR *text, UINT32 length)
{
	if (kind == LIST_PRODUCT && length >= 2 && text[0] == '(' && text[length - 1] == ')')
		return valid_value(text, length);
	UINT32 slash = length;
	for (UINT32 index = 0; index < length; ++index)
		if (text[index] == '/' && kind == LIST_PRODUCT && slash == length)
			slash = index;
		else if (!token_character(text[index]))
			return FALSE;
	return slash > 0 && (slash == length || slash + 1 < length);
}

static HRESULT make_item(enum list_kind kind, const WCHAR *text, UINT32 length, BOOL product_only, IInspectable **out)
{
	const struct winrt_class *class = kind == LIST_CONNECTION ? &option_kind : product_only ? &product_kind : &product_info_kind;
	struct item *self = winrt_new(class, sizeof(*self), NULL);
	if (!self)
		return E_OUTOFMEMORY;
	self->value.table = kind == LIST_CONNECTION ? (const void *)&option_table : product_only ? (const void *)&product_table :
		(const void *)&product_info_table;
	self->stringable.table = &item_stringable_table;
	self->comment = kind == LIST_PRODUCT && length && text[0] == '(';
	HRESULT result = winrt_string_n(text, length, &self->text);
	if (SUCCEEDED(result) && kind == LIST_PRODUCT && !self->comment) {
		UINT32 slash = 0;
		while (slash < length && text[slash] != '/')
			++slash;
		result = winrt_string_n(text, slash, &self->name);
		if (SUCCEEDED(result) && slash < length)
			result = winrt_string_n(text + slash + 1, length - slash - 1, &self->version);
	}
	if (FAILED(result)) {
		winrt_release(&self->value);
		return result;
	}
	*out = (void *)&self->value;
	return S_OK;
}

/* The items of the header's value: start and length of each. */
struct span {
	UINT32 start, length;
};

static UINT32 split(enum list_kind kind, const WCHAR *text, UINT32 length, struct span *spans, UINT32 capacity)
{
	UINT32 count = 0, at = 0;
	while (at < length) {
		while (at < length && (text[at] == ' ' || text[at] == '\t' || (kind == LIST_CONNECTION && text[at] == ',')))
			++at;
		if (at >= length)
			break;
		UINT32 end = at;
		if (kind == LIST_PRODUCT && text[at] == '(') {
			unsigned depth = 0;
			for (; end < length; ++end)
				if (text[end] == '(')
					++depth;
				else if (text[end] == ')' && !--depth) {
					++end;
					break;
				}
		} else
			while (end < length && text[end] != (kind == LIST_CONNECTION ? ',' : ' '))
				++end;
		UINT32 trimmed = end;
		while (trimmed > at && (text[trimmed - 1] == ' ' || text[trimmed - 1] == '\t'))
			--trimmed;
		if (count < capacity) {
			spans[count].start = at;
			spans[count].length = trimmed - at;
		}
		++count;
		at = end;
	}
	return count;
}

/* The list's items, made from its header: the caller releases each and frees the array. */
static HRESULT list_items(struct list *self, IInspectable ***out, UINT32 *count)
{
	HSTRING value = NULL;
	*out = NULL;
	*count = 0;
	HRESULT result = get_text(&self->owner->collection, self->name, &value);
	UINT32 length;
	const WCHAR *text = winrt_text(value, &length);
	const UINT32 total = SUCCEEDED(result) ? split(self->kind, text, length, NULL, 0) : 0;
	struct span *spans = total ? WINRT_ALLOC(total * sizeof(*spans)) : NULL;
	IInspectable **items = total ? WINRT_ALLOC(total * sizeof(*items)) : NULL;
	if (total && (!spans || !items))
		result = E_OUTOFMEMORY;
	if (SUCCEEDED(result) && total) {
		split(self->kind, text, length, spans, total);
		for (UINT32 index = 0; index < total && SUCCEEDED(result); ++index)
			if (SUCCEEDED(result = make_item(self->kind, text + spans[index].start, spans[index].length, FALSE, &items[index])))
				++*count;
	}
	WINRT_FREE(spans);
	WindowsDeleteString(value);
	if (FAILED(result)) {
		while (*count)
			winrt_put(items[--*count]);
		WINRT_FREE(items);
		return result;
	}
	*out = items;
	return S_OK;
}

static void free_items(IInspectable **items, UINT32 count)
{
	while (count)
		winrt_put(items[--count]);
	WINRT_FREE(items);
}

/* Writes the items back as the header's value; an empty list removes the header. */
static HRESULT list_store(struct list *self, IInspectable **items, UINT32 count)
{
	HSTRING texts[64];
	UINT32 total = 0, length = 0;
	HRESULT result = S_OK;
	if (count > ARRAYSIZE(texts))
		return E_OUTOFMEMORY;
	for (UINT32 index = 0; index < count && SUCCEEDED(result); ++index) {
		__x_ABI_CWindows_CFoundation_CIStringable *stringable = NULL;
		result = items[index]->lpVtbl->QueryInterface(items[index], &IID___x_ABI_CWindows_CFoundation_CIStringable, (void **)&stringable);
		if (SUCCEEDED(result)) {
			result = stringable->lpVtbl->ToString(stringable, &texts[total]);
			stringable->lpVtbl->Release(stringable);
		}
		if (SUCCEEDED(result))
			length += WindowsGetStringLen(texts[total++]) + 2;
	}
	WCHAR *joined = SUCCEEDED(result) ? WINRT_ALLOC(((size_t)length + 1) * sizeof(WCHAR)) : NULL;
	if (SUCCEEDED(result) && !joined)
		result = E_OUTOFMEMORY;
	if (SUCCEEDED(result)) {
		UINT32 used = 0;
		for (UINT32 index = 0; index < total; ++index) {
			UINT32 part;
			const WCHAR *text = winrt_text(texts[index], &part);
			if (index && self->kind == LIST_CONNECTION)
				joined[used++] = ',';
			if (index)
				joined[used++] = ' ';
			memcpy(joined + used, text, part * sizeof(WCHAR));
			used += part;
		}
		result = put_text(&self->owner->collection, self->name, total ? joined : NULL, used);
	}
	WINRT_FREE(joined);
	while (total)
		WindowsDeleteString(texts[--total]);
	return result;
}

static HRESULT add_parsed(struct list *self, HSTRING input)
{
	UINT32 length;
	const WCHAR *text = winrt_text(input, &length);
	const UINT32 total = split(self->kind, text, length, NULL, 0);
	struct span spans[64];
	if (!total || total > ARRAYSIZE(spans))
		return E_INVALIDARG;
	split(self->kind, text, length, spans, total);
	for (UINT32 index = 0; index < total; ++index)
		if (!valid_item(self->kind, text + spans[index].start, spans[index].length))
			return E_INVALIDARG;
	return winrt_map_set(&self->owner->map, self->name, lstrlenW(self->name), text, length, TRUE);
}

static HRESULT STDMETHODCALLTYPE list_parse_add(void *iface, HSTRING input) { return add_parsed(list_of(iface), input); }

static HRESULT STDMETHODCALLTYPE list_try_parse_add(void *iface, HSTRING input, boolean *out)
{
	if (!out)
		return E_POINTER;
	const HRESULT result = add_parsed(list_of(iface), input);
	*out = SUCCEEDED(result);
	return result == E_OUTOFMEMORY ? result : S_OK;
}

static HRESULT STDMETHODCALLTYPE list_get_at(void *iface, UINT32 index, IInspectable **out)
{
	IInspectable **items;
	UINT32 count;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = list_items(list_of(iface), &items, &count);
	if (FAILED(result))
		return result;
	if (index < count) {
		*out = items[index];
		(*out)->lpVtbl->AddRef(*out);
	} else
		result = E_BOUNDS;
	free_items(items, count);
	return result;
}

static HRESULT STDMETHODCALLTYPE list_size(void *iface, UINT32 *out)
{
	IInspectable **items;
	if (!out)
		return E_POINTER;
	const HRESULT result = list_items(list_of(iface), &items, out);
	if (SUCCEEDED(result))
		free_items(items, *out);
	return result;
}

static const struct winrt_items_kind option_items = {&IID___FIVectorView_1_Windows__CWeb__CHttp__CHeaders__CHttpConnectionOptionHeaderValue,
	&IID___FIIterable_1_Windows__CWeb__CHttp__CHeaders__CHttpConnectionOptionHeaderValue,
	&IID___FIIterator_1_Windows__CWeb__CHttp__CHeaders__CHttpConnectionOptionHeaderValue};
static const struct winrt_items_kind product_items = {&IID___FIVectorView_1_Windows__CWeb__CHttp__CHeaders__CHttpProductInfoHeaderValue,
	&IID___FIIterable_1_Windows__CWeb__CHttp__CHeaders__CHttpProductInfoHeaderValue,
	&IID___FIIterator_1_Windows__CWeb__CHttp__CHeaders__CHttpProductInfoHeaderValue};

static HRESULT list_snapshot(void *iface, BOOL iterator, void **out)
{
	struct list *self = list_of(iface);
	IInspectable **items;
	UINT32 count;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = list_items(self, &items, &count);
	if (SUCCEEDED(result)) {
		result = winrt_items(self->kind == LIST_CONNECTION ? &option_items : &product_items, items, count, iterator, out);
		free_items(items, count);
	}
	return result;
}

static HRESULT STDMETHODCALLTYPE list_view(void *iface, void **out) { return list_snapshot(iface, FALSE, out); }
static HRESULT STDMETHODCALLTYPE list_first(void *iface, void **out) { return list_snapshot(iface, TRUE, out); }

static HRESULT STDMETHODCALLTYPE list_index_of(void *iface, IInspectable *value, UINT32 *index, boolean *found)
{
	/* The items are made anew on each read, so no object the caller holds is one of them. */
	if (!index || !found)
		return E_POINTER;
	*index = 0;
	*found = FALSE;
	return S_OK;
}

/* Replaces count items at index by the one given (NULL to remove), or inserts it when insert is set. */
static HRESULT list_edit(void *iface, UINT32 index, IInspectable *value, BOOL insert, BOOL remove)
{
	struct list *self = list_of(iface);
	IInspectable **items, **edited;
	UINT32 count;
	HRESULT result = list_items(self, &items, &count);
	if (FAILED(result))
		return result;
	if (!remove && !value)
		result = E_INVALIDARG;
	else if (insert ? index > count : index >= count)
		result = E_BOUNDS;
	edited = SUCCEEDED(result) ? WINRT_ALLOC((count + 1) * sizeof(*edited)) : NULL;
	if (SUCCEEDED(result) && !edited)
		result = E_OUTOFMEMORY;
	if (SUCCEEDED(result)) {
		UINT32 used = 0;
		for (UINT32 at = 0; at <= count; ++at) {
			if (at == index && !remove)
				edited[used++] = value;
			if (at < count && !(at == index && !insert))
				edited[used++] = items[at];
		}
		result = list_store(self, edited, used);
	}
	WINRT_FREE(edited);
	free_items(items, count);
	return result;
}

static HRESULT STDMETHODCALLTYPE list_set_at(void *iface, UINT32 index, IInspectable *value) { return list_edit(iface, index, value, FALSE, FALSE); }
static HRESULT STDMETHODCALLTYPE list_insert_at(void *iface, UINT32 index, IInspectable *value) { return list_edit(iface, index, value, TRUE, FALSE); }
static HRESULT STDMETHODCALLTYPE list_remove_at(void *iface, UINT32 index) { return list_edit(iface, index, NULL, FALSE, TRUE); }

static HRESULT STDMETHODCALLTYPE list_append(void *iface, IInspectable *value)
{
	UINT32 count;
	HRESULT result = list_size(iface, &count);
	return SUCCEEDED(result) ? list_edit(iface, count, value, TRUE, FALSE) : result;
}

static HRESULT STDMETHODCALLTYPE list_remove_at_end(void *iface)
{
	UINT32 count;
	HRESULT result = list_size(iface, &count);
	return FAILED(result) ? result : count ? list_edit(iface, count - 1, NULL, FALSE, TRUE) : E_BOUNDS;
}

static HRESULT STDMETHODCALLTYPE list_clear(void *iface)
{
	struct list *self = list_of(iface);
	winrt_map_remove(&self->owner->map, self->name, lstrlenW(self->name));
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE list_get_many(void *iface, UINT32 start, UINT32 capacity, IInspectable **into, UINT32 *count)
{
	IInspectable **items;
	UINT32 total;
	if (!count || (capacity && !into))
		return E_POINTER;
	HRESULT result = list_items(list_of(iface), &items, &total);
	if (FAILED(result))
		return result;
	*count = 0;
	for (UINT32 index = start; index < total && *count < capacity; ++index) {
		into[(*count)++] = items[index];
		items[index]->lpVtbl->AddRef(items[index]);
	}
	free_items(items, total);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE list_replace_all(void *iface, UINT32 count, IInspectable **items)
{
	if (count && !items)
		return E_POINTER;
	return list_store(list_of(iface), items, count);
}

static HRESULT STDMETHODCALLTYPE list_string(void *iface, HSTRING *out) { return get_text(&list_of(iface)->owner->collection, list_of(iface)->name, out); }

static const struct {
	void *query, *add_ref, *release, *iids, *name, *trust, *parse_add, *try_parse_add;
} list_table = {WINRT_INSPECTABLE, list_parse_add, list_try_parse_add};
static const struct {
	void *query, *add_ref, *release, *iids, *name, *trust, *get_at, *size, *view, *index_of, *set_at, *insert_at, *remove_at, *append,
		*remove_at_end, *clear, *get_many, *replace_all;
} list_vector_table = {WINRT_INSPECTABLE, list_get_at, list_size, list_view, list_index_of, list_set_at, list_insert_at, list_remove_at,
	list_append, list_remove_at_end, list_clear, list_get_many, list_replace_all};
static const struct {
	void *query, *add_ref, *release, *iids, *name, *trust, *first;
} list_iterable_table = {WINRT_INSPECTABLE, list_first};
static const __x_ABI_CWindows_CFoundation_CIStringableVtbl list_stringable_table = {WINRT_INSPECTABLE, (void *)list_string};
static const struct winrt_entry list_entries[] = {
	WINRT_ENTRY(struct list, collection, WINRT_OWN), WINRT_ENTRY(struct list, vector, WINRT_OWN_2),
	WINRT_ENTRY(struct list, iterable, WINRT_OWN_3),
	WINRT_ENTRY(struct list, stringable, IID___x_ABI_CWindows_CFoundation_CIStringable), WINRT_END};
static const struct winrt_class connection_list_kind = {L"Windows.Web.Http.Headers.HttpConnectionOptionHeaderValueCollection",
	list_entries, list_destroy};
static const struct winrt_class product_list_kind = {L"Windows.Web.Http.Headers.HttpProductInfoHeaderValueCollection", list_entries,
	list_destroy};

static HRESULT make_list(void *iface, enum list_kind kind, const WCHAR *name, void **out)
{
	struct headers *owner = headers_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct list *self = kind == LIST_CONNECTION ?
		winrt_new_own(&connection_list_kind, sizeof(struct list),
			&IID___x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpConnectionOptionHeaderValueCollection,
			&IID___FIVector_1_Windows__CWeb__CHttp__CHeaders__CHttpConnectionOptionHeaderValue,
			&IID___FIIterable_1_Windows__CWeb__CHttp__CHeaders__CHttpConnectionOptionHeaderValue) :
		winrt_new_own(&product_list_kind, sizeof(struct list),
			&IID___x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpProductInfoHeaderValueCollection,
			&IID___FIVector_1_Windows__CWeb__CHttp__CHeaders__CHttpProductInfoHeaderValue,
			&IID___FIIterable_1_Windows__CWeb__CHttp__CHeaders__CHttpProductInfoHeaderValue);
	if (!self)
		return E_OUTOFMEMORY;
	self->collection.table = &list_table;
	self->vector.table = &list_vector_table;
	self->iterable.table = &list_iterable_table;
	self->stringable.table = &list_stringable_table;
	winrt_add_ref(&owner->collection);
	self->owner = owner;
	self->kind = kind;
	self->name = name;
	*out = &self->collection;
	return S_OK;
}

/* The collections. */

static HRESULT append_header(void *iface, HSTRING name, HSTRING value, BOOL validate)
{
	UINT32 name_length, value_length;
	const WCHAR *name_text = winrt_text(name, &name_length), *value_text = winrt_text(value, &value_length);
	if (!valid_name(name_text, name_length) || (validate && !valid_value(value_text, value_length)))
		return E_INVALIDARG;
	return winrt_map_set(&headers_of(iface)->map, name_text, name_length, value_text, value_length, TRUE);
}

static HRESULT STDMETHODCALLTYPE headers_append(void *iface, HSTRING name, HSTRING value) { return append_header(iface, name, value, TRUE); }

static HRESULT STDMETHODCALLTYPE headers_try_append(void *iface, HSTRING name, HSTRING value, boolean *out)
{
	if (!out)
		return E_POINTER;
	const HRESULT result = append_header(iface, name, value, FALSE);
	*out = SUCCEEDED(result);
	return result == E_OUTOFMEMORY ? result : S_OK;
}

static HRESULT not_modelled(void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE get_not_modelled(void *iface, void **out) { return not_modelled(out); }
static HRESULT STDMETHODCALLTYPE put_not_modelled(void *iface, void *value) { return E_NOTIMPL; }

static HRESULT STDMETHODCALLTYPE request_connection(void *iface, void **out) { return make_list(iface, LIST_CONNECTION, L"Connection", out); }
static HRESULT STDMETHODCALLTYPE request_user_agent(void *iface, void **out) { return make_list(iface, LIST_PRODUCT, L"User-Agent", out); }
static HRESULT STDMETHODCALLTYPE header_get_date(void *iface, date_reference **out) { return get_date(iface, L"Date", out); }
static HRESULT STDMETHODCALLTYPE header_put_date(void *iface, date_reference *value) { return put_date(iface, L"Date", value); }
static HRESULT STDMETHODCALLTYPE request_get_from(void *iface, HSTRING *out) { return get_text(iface, L"From", out); }

static HRESULT STDMETHODCALLTYPE request_put_from(void *iface, HSTRING value)
{
	UINT32 length;
	const WCHAR *text = winrt_text(value, &length);
	return put_text(iface, L"From", value ? text : NULL, length);
}

static HRESULT STDMETHODCALLTYPE request_get_modified_since(void *iface, date_reference **out) { return get_date(iface, L"If-Modified-Since", out); }
static HRESULT STDMETHODCALLTYPE request_put_modified_since(void *iface, date_reference *value) { return put_date(iface, L"If-Modified-Since", value); }
static HRESULT STDMETHODCALLTYPE request_get_unmodified_since(void *iface, date_reference **out) { return get_date(iface, L"If-Unmodified-Since", out); }
static HRESULT STDMETHODCALLTYPE request_put_unmodified_since(void *iface, date_reference *value) { return put_date(iface, L"If-Unmodified-Since", value); }

static HRESULT STDMETHODCALLTYPE request_get_max_forwards(void *iface, count_reference **out)
{
	BOOL present;
	UINT64 number = 0;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = get_number(iface, L"Max-Forwards", &present, &number);
	if (SUCCEEDED(result) && present && number <= 0xffffffff) {
		const UINT32 value = (UINT32)number;
		result = winrt_box(&IID___FIReference_1_UINT32, &value, sizeof(value), (void **)out);
	}
	return result;
}

static HRESULT STDMETHODCALLTYPE request_put_max_forwards(void *iface, count_reference *value)
{
	UINT32 number;
	if (!value)
		return put_text(iface, L"Max-Forwards", NULL, 0);
	HRESULT result = value->lpVtbl->get_Value(value, &number);
	return SUCCEEDED(result) ? put_number(iface, L"Max-Forwards", number) : result;
}

static HRESULT STDMETHODCALLTYPE request_get_referer(void *iface, uri_class **out) { return get_uri(iface, L"Referer", out); }
static HRESULT STDMETHODCALLTYPE request_put_referer(void *iface, uri_class *value) { return put_uri(iface, L"Referer", value); }

static HRESULT STDMETHODCALLTYPE response_get_age(void *iface, span_reference **out)
{
	BOOL present;
	UINT64 seconds = 0;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = get_number(iface, L"Age", &present, &seconds);
	if (SUCCEEDED(result) && present) {
		const time_span age = {(INT64)(seconds * 10000000)};
		result = winrt_box(&IID___FIReference_1_TimeSpan, &age, sizeof(age), (void **)out);
	}
	return result;
}

static HRESULT STDMETHODCALLTYPE response_put_age(void *iface, span_reference *value)
{
	time_span age;
	if (!value)
		return put_text(iface, L"Age", NULL, 0);
	HRESULT result = value->lpVtbl->get_Value(value, &age);
	return SUCCEEDED(result) ? put_number(iface, L"Age", (UINT64)age.Duration / 10000000) : result;
}

static HRESULT STDMETHODCALLTYPE response_connection(void *iface, void **out) { return make_list(iface, LIST_CONNECTION, L"Connection", out); }
static HRESULT STDMETHODCALLTYPE response_get_location(void *iface, uri_class **out) { return get_uri(iface, L"Location", out); }
static HRESULT STDMETHODCALLTYPE response_put_location(void *iface, uri_class *value) { return put_uri(iface, L"Location", value); }

static HRESULT STDMETHODCALLTYPE response_get_retry_after(void *iface, date_or_delta **out)
{
	HSTRING text = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = get_text(iface, L"Retry-After", &text);
	UINT32 length;
	const WCHAR *characters = winrt_text(text, &length);
	if (SUCCEEDED(result) && text && FAILED(parse_date_or_delta(characters, length, out)))
		*out = NULL;
	WindowsDeleteString(text);
	return result;
}

static HRESULT STDMETHODCALLTYPE response_put_retry_after(void *iface, date_or_delta *value)
{
	__x_ABI_CWindows_CFoundation_CIStringable *stringable = NULL;
	HSTRING text = NULL;
	if (!value)
		return put_text(iface, L"Retry-After", NULL, 0);
	HRESULT result = value->lpVtbl->QueryInterface(value, &IID___x_ABI_CWindows_CFoundation_CIStringable, (void **)&stringable);
	if (SUCCEEDED(result)) {
		result = stringable->lpVtbl->ToString(stringable, &text);
		stringable->lpVtbl->Release(stringable);
	}
	UINT32 length;
	const WCHAR *characters = winrt_text(text, &length);
	if (SUCCEEDED(result))
		result = put_text(iface, L"Retry-After", characters, length);
	WindowsDeleteString(text);
	return result;
}

static HRESULT STDMETHODCALLTYPE content_get_length(void *iface, length_reference **out)
{
	BOOL present;
	UINT64 length = 0;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = get_number(iface, L"Content-Length", &present, &length);
	if (SUCCEEDED(result) && present)
		result = winrt_box(&IID___FIReference_1_UINT64, &length, sizeof(length), (void **)out);
	return result;
}

static HRESULT STDMETHODCALLTYPE content_put_length(void *iface, length_reference *value)
{
	UINT64 length;
	if (!value)
		return put_text(iface, L"Content-Length", NULL, 0);
	HRESULT result = value->lpVtbl->get_Value(value, &length);
	return SUCCEEDED(result) ? put_number(iface, L"Content-Length", length) : result;
}

static HRESULT STDMETHODCALLTYPE content_get_location(void *iface, uri_class **out) { return get_uri(iface, L"Content-Location", out); }
static HRESULT STDMETHODCALLTYPE content_put_location(void *iface, uri_class *value) { return put_uri(iface, L"Content-Location", value); }
static HRESULT STDMETHODCALLTYPE content_get_expires(void *iface, date_reference **out) { return get_date(iface, L"Expires", out); }
static HRESULT STDMETHODCALLTYPE content_put_expires(void *iface, date_reference *value) { return put_date(iface, L"Expires", value); }
static HRESULT STDMETHODCALLTYPE content_get_modified(void *iface, date_reference **out) { return get_date(iface, L"Last-Modified", out); }
static HRESULT STDMETHODCALLTYPE content_put_modified(void *iface, date_reference *value) { return put_date(iface, L"Last-Modified", value); }

static const __x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpRequestHeaderCollectionVtbl request_table = {WINRT_INSPECTABLE,
	.get_Accept = (void *)get_not_modelled, .get_AcceptEncoding = (void *)get_not_modelled, .get_AcceptLanguage = (void *)get_not_modelled,
	.get_Authorization = (void *)get_not_modelled, .put_Authorization = (void *)put_not_modelled,
	.get_CacheControl = (void *)get_not_modelled, .get_Connection = (void *)request_connection, .get_Cookie = (void *)get_not_modelled,
	.get_Date = (void *)header_get_date, .put_Date = (void *)header_put_date, .get_Expect = (void *)get_not_modelled, .get_From = (void *)request_get_from,
	.put_From = (void *)request_put_from, .get_Host = (void *)get_not_modelled, .put_Host = (void *)put_not_modelled,
	.get_IfModifiedSince = (void *)request_get_modified_since, .put_IfModifiedSince = (void *)request_put_modified_since,
	.get_IfUnmodifiedSince = (void *)request_get_unmodified_since, .put_IfUnmodifiedSince = (void *)request_put_unmodified_since,
	.get_MaxForwards = (void *)request_get_max_forwards, .put_MaxForwards = (void *)request_put_max_forwards,
	.get_ProxyAuthorization = (void *)get_not_modelled, .put_ProxyAuthorization = (void *)put_not_modelled,
	.get_Referer = (void *)request_get_referer, .put_Referer = (void *)request_put_referer, .get_TransferEncoding = (void *)get_not_modelled,
	.get_UserAgent = (void *)request_user_agent, .Append = (void *)headers_append, .TryAppendWithoutValidation = (void *)headers_try_append};
static const __x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpResponseHeaderCollectionVtbl response_table = {WINRT_INSPECTABLE,
	.get_Age = (void *)response_get_age, .put_Age = (void *)response_put_age, .get_Allow = (void *)get_not_modelled,
	.get_CacheControl = (void *)get_not_modelled, .get_Connection = (void *)response_connection, .get_Date = (void *)header_get_date,
	.put_Date = (void *)header_put_date, .get_Location = (void *)response_get_location, .put_Location = (void *)response_put_location,
	.get_ProxyAuthenticate = (void *)get_not_modelled, .get_RetryAfter = (void *)response_get_retry_after,
	.put_RetryAfter = (void *)response_put_retry_after, .get_TransferEncoding = (void *)get_not_modelled,
	.get_WwwAuthenticate = (void *)get_not_modelled, .Append = (void *)headers_append, .TryAppendWithoutValidation = (void *)headers_try_append};
static const __x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpContentHeaderCollectionVtbl content_table = {WINRT_INSPECTABLE,
	.get_ContentDisposition = (void *)get_not_modelled, .put_ContentDisposition = (void *)put_not_modelled,
	.get_ContentEncoding = (void *)get_not_modelled, .get_ContentLanguage = (void *)get_not_modelled,
	.get_ContentLength = (void *)content_get_length, .put_ContentLength = (void *)content_put_length, .get_ContentLocation = (void *)content_get_location,
	.put_ContentLocation = (void *)content_put_location, .get_ContentMD5 = (void *)get_not_modelled, .put_ContentMD5 = (void *)put_not_modelled,
	.get_ContentRange = (void *)get_not_modelled, .put_ContentRange = (void *)put_not_modelled,
	.get_ContentType = (void *)get_not_modelled, .put_ContentType = (void *)put_not_modelled, .get_Expires = (void *)content_get_expires,
	.put_Expires = (void *)content_put_expires, .get_LastModified = (void *)content_get_modified, .put_LastModified = (void *)content_put_modified,
	.Append = (void *)headers_append, .TryAppendWithoutValidation = (void *)headers_try_append};

/* "Name: value" lines, separated by CRLF, as a head carries them. */
struct lines {
	WCHAR *text;
	UINT32 used, capacity;
};

static HRESULT add_line(void *context, HSTRING key, HSTRING value)
{
	struct lines *lines = context;
	UINT32 key_length, value_length;
	const WCHAR *key_text = winrt_text(key, &key_length), *value_text = winrt_text(value, &value_length);
	const UINT32 needed = lines->used + key_length + value_length + 4;
	if (needed > lines->capacity) {
		const UINT32 capacity = needed * 2;
		WCHAR *grown = lines->text ? WINRT_REALLOC(lines->text, capacity * sizeof(WCHAR)) : WINRT_ALLOC(capacity * sizeof(WCHAR));
		if (!grown)
			return E_OUTOFMEMORY;
		lines->text = grown;
		lines->capacity = capacity;
	}
	memcpy(lines->text + lines->used, key_text, key_length * sizeof(WCHAR));
	lines->used += key_length;
	lines->text[lines->used++] = ':';
	lines->text[lines->used++] = ' ';
	memcpy(lines->text + lines->used, value_text, value_length * sizeof(WCHAR));
	lines->used += value_length;
	lines->text[lines->used++] = '\r';
	lines->text[lines->used++] = '\n';
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE headers_string(void *iface, HSTRING *out)
{
	struct lines lines = {0};
	if (!out)
		return E_POINTER;
	HRESULT result = winrt_map_each(&headers_of(iface)->map, add_line, &lines);
	if (SUCCEEDED(result))
		result = winrt_string_n(lines.text, lines.used, out);
	WINRT_FREE(lines.text);
	return result;
}

static void headers_destroy(struct winrt_object *object) { winrt_map_free(&((struct headers *)object)->map); }

static const __x_ABI_CWindows_CFoundation_CIStringableVtbl headers_stringable_table = {WINRT_INSPECTABLE, (void *)headers_string};
static const struct winrt_entry headers_entries[] = {
	WINRT_ENTRY(struct headers, collection, WINRT_OWN), WINRT_MAP_ENTRIES(struct headers, map),
	WINRT_ENTRY(struct headers, stringable, IID___x_ABI_CWindows_CFoundation_CIStringable), WINRT_END};
static const struct winrt_class request_headers_kind = {L"Windows.Web.Http.Headers.HttpRequestHeaderCollection", headers_entries,
	headers_destroy};
static const struct winrt_class response_headers_kind = {L"Windows.Web.Http.Headers.HttpResponseHeaderCollection", headers_entries,
	headers_destroy};
static const struct winrt_class content_headers_kind = {L"Windows.Web.Http.Headers.HttpContentHeaderCollection", headers_entries,
	headers_destroy};

HRESULT headers_new(enum headers_kind kind, struct headers **out)
{
	static const struct {
		const struct winrt_class *class;
		const IID *iid;
		const void *table;
	} kinds[] = {
		{&request_headers_kind, &IID___x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpRequestHeaderCollection, &request_table},
		{&response_headers_kind, &IID___x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpResponseHeaderCollection, &response_table},
		{&content_headers_kind, &IID___x_ABI_CWindows_CWeb_CHttp_CHeaders_CIHttpContentHeaderCollection, &content_table}};
	struct headers *self = winrt_new(kinds[kind].class, sizeof(*self), kinds[kind].iid);
	if (!self)
		return E_OUTOFMEMORY;
	self->collection.table = kinds[kind].table;
	self->stringable.table = &headers_stringable_table;
	self->kind = kind;
	winrt_map_init(&self->map, TRUE);
	*out = self;
	return S_OK;
}

void *headers_interface(struct headers *headers)
{
	winrt_add_ref(&headers->collection);
	return &headers->collection;
}

void headers_release(struct headers *headers)
{
	if (headers)
		winrt_release(&headers->collection);
}

struct winrt_map *headers_map(struct headers *headers) { return &headers->map; }
