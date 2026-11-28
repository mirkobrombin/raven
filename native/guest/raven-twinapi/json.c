/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.Data.Json: JsonValue, JsonObject and JsonArray, what JSON text parses into and what Stringify writes back, and JsonError, which
 * names a failure. A JsonObject is a map from names to IJsonValue and a JsonArray a vector of them, so an app may also build and walk them
 * as collections. Numbers are read and printed by the CRT, whose conversions are exact, and written as JavaScript writes them: the fewest
 * digits that read back as the same double. */
#include "object.h"
#include "../raven-winrt/winrt.h"
#include <stdarg.h>

static const IID iid_json_value = {0xa3219ecb, 0xf0b3, 0x4dcd, {0xbe, 0xee, 0x19, 0xd4, 0x8c, 0xd3, 0xed, 0x1e}};
static const IID iid_json_object = {0x064e24dd, 0x29c2, 0x4f83, {0x9a, 0xc1, 0x9e, 0xe1, 0x15, 0x78, 0xbe, 0xb3}};
static const IID iid_json_array = {0x08c1ddb6, 0x0cbd, 0x4a9a, {0xb5, 0xd3, 0x2f, 0x85, 0x2d, 0xc3, 0x7e, 0x81}};
static const IID iid_object_defaults = {0xd960d2a2, 0xb7f0, 0x4f00, {0x8e, 0x44, 0xd8, 0x2c, 0xf4, 0x15, 0xea, 0x13}};
static const IID iid_value_statics = {0x5f6b544a, 0x2f53, 0x48e1, {0x91, 0xa3, 0xf7, 0x8b, 0x50, 0xa6, 0x34, 0x5c}};
static const IID iid_value_statics2 = {0x1d9ecbe4, 0x3fe8, 0x4335, {0x83, 0x92, 0x93, 0xd8, 0xe3, 0x68, 0x65, 0xf0}};
static const IID iid_object_statics = {0x2289f159, 0x54de, 0x45d8, {0xab, 0xcc, 0x22, 0x60, 0x3f, 0xa0, 0x66, 0xa0}};
static const IID iid_array_statics = {0xdb1434a9, 0xe164, 0x499f, {0x93, 0xe2, 0x8a, 0x8f, 0x49, 0xbb, 0x90, 0xba}};
static const IID iid_error_statics2 = {0x404030da, 0x87d0, 0x436c, {0x83, 0xab, 0xfc, 0x7b, 0x12, 0xc0, 0xcc, 0x26}};
static const IID iid_stringable = {0x96369f54, 0x8eb6, 0x48f0, {0xab, 0xce, 0xc1, 0xb2, 0x11, 0xe6, 0x27, 0xc3}};
/* IMap, IMapView, IKeyValuePair, IIterable and IIterator of pinterface(...;string;Windows.Data.Json.IJsonValue). */
static const IID iid_value_map = {0xc9d9a725, 0x786b, 0x5113, {0xb4, 0xb7, 0x9b, 0x61, 0x76, 0x4c, 0x22, 0x0b}};
static const IID iid_value_map_view = {0xeecd690c, 0x1ff3, 0x529f, {0x92, 0x3f, 0x9b, 0x1c, 0x31, 0xfd, 0x3d, 0x0f}};
static const IID iid_value_pair = {0x4deecc89, 0xb0b8, 0x5ee8, {0xa5, 0x1d, 0x1c, 0x25, 0xad, 0x9a, 0x5b, 0x01}};
static const IID iid_value_pairs = {0xdfabb6e1, 0x0411, 0x5a8f, {0xaa, 0x87, 0x35, 0x4e, 0x71, 0x10, 0xf0, 0x99}};
static const IID iid_value_pair_iterator = {0xf948eac5, 0x33eb, 0x50f5, {0xb5, 0xaf, 0xe7, 0xce, 0xcf, 0x0e, 0x45, 0x01}};
/* IVector, IVectorView, IIterable and IIterator of Windows.Data.Json.IJsonValue. */
static const IID iid_value_vector = {0xd44662bc, 0xdce3, 0x59a8, {0x92, 0x72, 0x4b, 0x21, 0x0f, 0x33, 0x90, 0x8b}};
static const IID iid_value_vector_view = {0xcffabb0f, 0x6bc4, 0x5ff6, {0x9b, 0x9e, 0x7a, 0x9d, 0xf6, 0xc6, 0x87, 0xc8}};
static const IID iid_value_iterable = {0xcb0492b6, 0x4113, 0x55cf, {0xb2, 0xc5, 0x99, 0xeb, 0x42, 0x8b, 0xa4, 0x93}};
static const IID iid_value_iterator = {0x189eb512, 0x5a20, 0x5ec6, {0x98, 0x66, 0x60, 0xaf, 0x96, 0xf0, 0xd2, 0x3b}};

/* What Windows.Data.Json reports, and the JsonErrorStatus each stands for. */
#define INVALID_STRING ((HRESULT)0x83750007)
#define INVALID_NUMBER ((HRESULT)0x83750008)
#define VALUE_NOT_FOUND ((HRESULT)0x83750009)

enum { JSON_NULL, JSON_BOOLEAN, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

/* Nesting deeper than this is refused, so that parsing cannot run the thread out of stack. */
#define DEEPEST 512

/* The places of IJsonValue's own methods, after IInspectable's. */
enum { VALUE_TYPE = 6, VALUE_STRINGIFY, VALUE_STRING, VALUE_NUMBER, VALUE_BOOLEAN, VALUE_ARRAY, VALUE_OBJECT };

typedef HRESULT (WINAPI *type_method)(void *, INT32 *);
typedef HRESULT (WINAPI *text_method)(void *, HSTRING *);
typedef HRESULT (WINAPI *number_method)(void *, double *);
typedef HRESULT (WINAPI *flag_method)(void *, boolean *);
typedef HRESULT (WINAPI *object_method)(void *, void **);

/* The CRT's wcstod and the function behind its wide printf family. */
static double (__cdecl *crt_read)(const WCHAR *, WCHAR **);
static int (__cdecl *crt_print)(unsigned __int64, WCHAR *, size_t, const WCHAR *, void *, va_list);
static INIT_ONCE crt_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK find_crt(INIT_ONCE *once, void *parameter, void **context)
{
	const HMODULE crt = LoadLibraryW(L"ucrtbase.dll");
	if (crt) {
		crt_read = (double (__cdecl *)(const WCHAR *, WCHAR **))GetProcAddress(crt, "wcstod");
		crt_print = (int (__cdecl *)(unsigned __int64, WCHAR *, size_t, const WCHAR *, void *, va_list))GetProcAddress(crt,
			"__stdio_common_vswprintf");
	}
	return TRUE;
}

static BOOL crt_ready(void)
{
	InitOnceExecuteOnce(&crt_once, find_crt, NULL, NULL);
	return crt_read && crt_print;
}

static int print(WCHAR *buffer, size_t room, const WCHAR *pattern, ...)
{
	va_list arguments;
	va_start(arguments, pattern);
	const int length = crt_print(0, buffer, room, pattern, NULL, arguments);
	va_end(arguments);
	return length;
}

static BOOL is_finite(double value)
{
	UINT64 bits;
	CopyMemory(&bits, &value, sizeof bits);
	return ((bits >> 52) & 0x7ff) != 0x7ff;
}

/* Text being written, which becomes a string. */
struct text {
	WCHAR *chars;
	UINT32 length, capacity;
	HRESULT status;
};

static void put_chars(struct text *text, const WCHAR *chars, UINT32 count)
{
	if (FAILED(text->status))
		return;
	if (text->length + count > text->capacity) {
		UINT32 capacity = text->capacity ? text->capacity : 64;
		while (capacity < text->length + count) {
			if (capacity > 0x3fffffff) {
				text->status = E_OUTOFMEMORY;
				return;
			}
			capacity *= 2;
		}
		WCHAR *grown = text->chars ? HeapReAlloc(GetProcessHeap(), 0, text->chars, capacity * sizeof(WCHAR)) :
			HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(WCHAR));
		if (!grown) {
			text->status = E_OUTOFMEMORY;
			return;
		}
		text->chars = grown;
		text->capacity = capacity;
	}
	CopyMemory(text->chars + text->length, chars, count * sizeof(WCHAR));
	text->length += count;
}

static void put_char(struct text *text, WCHAR c) { put_chars(text, &c, 1); }
static void put_word(struct text *text, const WCHAR *word) { put_chars(text, word, (UINT32)lstrlenW(word)); }

static void text_drop(struct text *text)
{
	if (text->chars)
		HeapFree(GetProcessHeap(), 0, text->chars);
}

static HRESULT text_finish(struct text *text, HSTRING *out)
{
	const HRESULT result = SUCCEEDED(text->status) ? WindowsCreateString(text->chars, text->length, out) : text->status;
	text_drop(text);
	return result;
}

/* A string as JSON writes it: quoted, with quotes, backslashes and control characters escaped. */
static void put_quoted(struct text *text, HSTRING string)
{
	static const WCHAR digits[] = L"0123456789abcdef";
	UINT32 length = 0;
	const WCHAR *chars = WindowsGetStringRawBuffer(string, &length);
	put_char(text, '"');
	for (UINT32 at = 0; at < length; ++at) {
		const WCHAR c = chars[at];
		if (c == '"' || c == '\\') {
			put_char(text, '\\');
			put_char(text, c);
		} else if (c == '\b')
			put_word(text, L"\\b");
		else if (c == '\f')
			put_word(text, L"\\f");
		else if (c == '\n')
			put_word(text, L"\\n");
		else if (c == '\r')
			put_word(text, L"\\r");
		else if (c == '\t')
			put_word(text, L"\\t");
		else if (c < 0x20) {
			const WCHAR escape[] = {'\\', 'u', '0', '0', digits[c >> 4], digits[c & 15]};
			put_chars(text, escape, 6);
		} else
			put_char(text, c);
	}
	put_char(text, '"');
}

/* A number as JavaScript's Number::toString writes it: the shortest digits that read back as the number, in plain notation from 1e-6 up
 * to 1e21 and with an exponent outside. Only a finite number is written; JSON has no other. */
static void put_number(struct text *text, double value)
{
	if (!is_finite(value) || !crt_ready()) {
		put_word(text, L"null");
		return;
	}
	if (value == 0) {
		put_char(text, '0');
		return;
	}
	WCHAR scientific[40], digits[20];
	for (int precision = 0; precision < 17; ++precision) {
		print(scientific, ARRAYSIZE(scientific), L"%.*e", precision, value);
		if (crt_read(scientific, NULL) == value)
			break;
	}
	/* -d.ddde+x: the digits without point and sign, and the place of the point after the first (x + 1). */
	const WCHAR *at = scientific;
	if (*at == '-') {
		put_char(text, '-');
		++at;
	}
	int count = 0;
	for (; *at && *at != 'e'; ++at)
		if (*at != '.')
			digits[count++] = *at;
	while (count > 1 && digits[count - 1] == '0')
		--count;
	int exponent = 0;
	BOOL negative = FALSE;
	if (*at == 'e') {
		++at;
		if (*at == '-' || *at == '+')
			negative = *at++ == '-';
		for (; *at >= '0' && *at <= '9'; ++at)
			exponent = exponent * 10 + (*at - '0');
	}
	const int point = (negative ? -exponent : exponent) + 1;
	if (count <= point && point <= 21) {
		put_chars(text, digits, count);
		for (int zero = count; zero < point; ++zero)
			put_char(text, '0');
	} else if (0 < point && point <= 21) {
		put_chars(text, digits, point);
		put_char(text, '.');
		put_chars(text, digits + point, count - point);
	} else if (-6 < point && point <= 0) {
		put_word(text, L"0.");
		for (int zero = point; zero < 0; ++zero)
			put_char(text, '0');
		put_chars(text, digits, count);
	} else {
		WCHAR power[8];
		put_char(text, digits[0]);
		if (count > 1) {
			put_char(text, '.');
			put_chars(text, digits + 1, count - 1);
		}
		put_char(text, 'e');
		put_char(text, point - 1 < 0 ? '-' : '+');
		put_chars(text, power, winrt_decimal((UINT64)(point - 1 < 0 ? 1 - point : point - 1), power));
	}
}

/* Any IJsonValue, ours or an app's, through its interface. */
static HRESULT value_type(void *value, INT32 *out) { return ((type_method)method(value, VALUE_TYPE))(value, out); }
static HRESULT value_stringify(void *value, HSTRING *out) { return ((text_method)method(value, VALUE_STRINGIFY))(value, out); }

/* A member or an element as JSON: its own Stringify, and null for a slot that holds nothing. */
static void put_value(struct text *text, void *value)
{
	HSTRING written = NULL;
	if (!value) {
		put_word(text, L"null");
		return;
	}
	const HRESULT result = value_stringify(value, &written);
	if (FAILED(result)) {
		if (SUCCEEDED(text->status))
			text->status = result;
		return;
	}
	UINT32 length = 0;
	const WCHAR *chars = WindowsGetStringRawBuffer(written, &length);
	put_chars(text, chars, length);
	WindowsDeleteString(written);
}

/* JsonValue: a null, a boolean, a number or a string; or, as Parse, GetNamedValue and the array's GetAt hand one out, an object or an
 * array it stands for, whose IJsonValue it forwards to. */
struct json_value {
	struct object base;
	INT32 type;
	boolean flag;
	double number;
	HSTRING string;
	/* The IJsonValue of the object or array, or of an app's value, that this one stands for. */
	void *inner;
};

static void destroy_value(struct object *base)
{
	struct json_value *self = (struct json_value *)base;
	WindowsDeleteString(self->string);
	release(self->inner);
}

static const struct face_iid value_iids[] = {{&iid_json_value, 0}, {&iid_stringable, 1}, {NULL, 0}};
static const struct kind value_kind = {"JsonValue", L"Windows.Data.Json.JsonValue", value_iids, destroy_value};

static struct json_value *value_of(void *iface) { return (struct json_value *)owner(iface); }

static HRESULT WINAPI value_get_type(void *iface, INT32 *out)
{
	struct json_value *self = value_of(iface);
	if (!out)
		return E_POINTER;
	if (self->inner)
		return value_type(self->inner, out);
	*out = self->type;
	return S_OK;
}

static HRESULT WINAPI value_write(void *iface, HSTRING *out)
{
	struct json_value *self = value_of(iface);
	struct text text = {0};
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->inner)
		return value_stringify(self->inner, out);
	if (self->type == JSON_BOOLEAN)
		put_word(&text, self->flag ? L"true" : L"false");
	else if (self->type == JSON_NUMBER)
		put_number(&text, self->number);
	else if (self->type == JSON_STRING)
		put_quoted(&text, self->string);
	else
		put_word(&text, L"null");
	return text_finish(&text, out);
}

static HRESULT WINAPI value_string(void *iface, HSTRING *out)
{
	struct json_value *self = value_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->inner)
		return ((text_method)method(self->inner, VALUE_STRING))(self->inner, out);
	return self->type == JSON_STRING ? WindowsDuplicateString(self->string, out) : E_ILLEGAL_METHOD_CALL;
}

static HRESULT WINAPI value_number(void *iface, double *out)
{
	struct json_value *self = value_of(iface);
	if (!out)
		return E_POINTER;
	if (self->inner)
		return ((number_method)method(self->inner, VALUE_NUMBER))(self->inner, out);
	if (self->type != JSON_NUMBER)
		return E_ILLEGAL_METHOD_CALL;
	*out = self->number;
	return S_OK;
}

static HRESULT WINAPI value_boolean(void *iface, boolean *out)
{
	struct json_value *self = value_of(iface);
	if (!out)
		return E_POINTER;
	if (self->inner)
		return ((flag_method)method(self->inner, VALUE_BOOLEAN))(self->inner, out);
	if (self->type != JSON_BOOLEAN)
		return E_ILLEGAL_METHOD_CALL;
	*out = self->flag;
	return S_OK;
}

static HRESULT WINAPI value_array(void *iface, void **out)
{
	struct json_value *self = value_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	return self->inner ? ((object_method)method(self->inner, VALUE_ARRAY))(self->inner, out) : E_ILLEGAL_METHOD_CALL;
}

static HRESULT WINAPI value_object(void *iface, void **out)
{
	struct json_value *self = value_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	return self->inner ? ((object_method)method(self->inner, VALUE_OBJECT))(self->inner, out) : E_ILLEGAL_METHOD_CALL;
}

TABLE(value_table, 0, INSPECTABLE, (const void *)value_get_type, (const void *)value_write, (const void *)value_string,
	(const void *)value_number, (const void *)value_boolean, (const void *)value_array, (const void *)value_object);
TABLE(value_stringable_table, 1, INSPECTABLE, (const void *)value_write);

/* A new JsonValue of a type, with one reference, through its IJsonValue. */
static struct json_value *value_make(INT32 type)
{
	static const void *const *const tables[] = {value_table.slots, value_stringable_table.slots};
	struct json_value *self = make(sizeof *self, &value_kind, tables, 2);
	if (self)
		self->type = type;
	return self;
}

static HRESULT value_out(struct json_value *self, void **out)
{
	if (!(*out = self ? face_of(&self->base, 0) : NULL))
		return E_OUTOFMEMORY;
	return S_OK;
}

static BOOL is_our_value(void *iface)
{
	return iface && method(iface, 0) == (void *)object_query && owner(iface)->kind == &value_kind;
}

/* The JsonValue a method that answers one hands out for an IJsonValue: the value itself when it is one, otherwise a JsonValue that stands
 * for it. */
static HRESULT as_value(void *iface, void **out)
{
	if (is_our_value(iface)) {
		reference(*out = iface);
		return S_OK;
	}
	struct json_value *self = value_make(JSON_NULL);
	if (self && iface)
		reference(self->inner = iface);
	return value_out(self, out);
}

/* JsonArray: an IVector<IJsonValue> that is a value too. */
struct json_array {
	struct object base;
	SRWLOCK lock;
	void **items;
	UINT32 count, capacity;
};

static void destroy_array(struct object *base)
{
	struct json_array *self = (struct json_array *)base;
	for (UINT32 at = 0; at < self->count; ++at)
		release(self->items[at]);
	if (self->items)
		FREE(self->items);
}

static const struct face_iid array_iids[] = {{&iid_json_array, 0}, {&iid_json_value, 1}, {&iid_value_vector, 2}, {&iid_value_iterable, 3},
	{&iid_stringable, 4}, {NULL, 0}};
static const struct kind array_kind = {"JsonArray", L"Windows.Data.Json.JsonArray", array_iids, destroy_array};
static const struct winrt_items_kind value_list = {&iid_value_vector_view, &iid_value_iterable, &iid_value_iterator, FALSE, NULL};

static struct json_array *array_of(void *iface) { return (struct json_array *)owner(iface); }

static HRESULT array_grow(struct json_array *self, UINT32 count)
{
	if (count <= self->capacity)
		return S_OK;
	UINT32 capacity = self->capacity ? self->capacity : 8;
	while (capacity < count) {
		if (capacity > 0x7fffffff / 2)
			return E_OUTOFMEMORY;
		capacity *= 2;
	}
	void **items = self->items ? HeapReAlloc(GetProcessHeap(), 0, self->items, capacity * sizeof *items) : ALLOC(capacity * sizeof *items);
	if (!items)
		return E_OUTOFMEMORY;
	self->items = items;
	self->capacity = capacity;
	return S_OK;
}

/* Inserts at an index, or at the end when index is NULL. */
static HRESULT array_insert(struct json_array *self, const UINT32 *place, void *value)
{
	AcquireSRWLockExclusive(&self->lock);
	const UINT32 index = place ? *place : self->count;
	HRESULT result = index > self->count ? E_BOUNDS : array_grow(self, self->count + 1);
	if (SUCCEEDED(result)) {
		MoveMemory(self->items + index + 1, self->items + index, (self->count - index) * sizeof *self->items);
		reference(self->items[index] = value);
		++self->count;
	}
	ReleaseSRWLockExclusive(&self->lock);
	return result;
}

/* The element at an index, with a reference; E_BOUNDS past the end. */
static HRESULT array_at(struct json_array *self, UINT32 index, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	AcquireSRWLockShared(&self->lock);
	const HRESULT result = index < self->count ? S_OK : E_BOUNDS;
	if (SUCCEEDED(result) && (*out = self->items[index]))
		reference(*out);
	ReleaseSRWLockShared(&self->lock);
	return result;
}

/* A copy of the elements with a reference to each, which items_free gives back. */
static HRESULT array_items(struct json_array *self, void ***out, UINT32 *count)
{
	HRESULT result = S_OK;
	*out = NULL;
	AcquireSRWLockShared(&self->lock);
	*count = self->count;
	if (*count && !(*out = ALLOC(*count * sizeof **out)))
		result = E_OUTOFMEMORY;
	for (UINT32 at = 0; SUCCEEDED(result) && at < *count; ++at)
		if (((*out)[at] = self->items[at]))
			reference((*out)[at]);
	ReleaseSRWLockShared(&self->lock);
	if (FAILED(result))
		*count = 0;
	return result;
}

static void items_free(void **items, UINT32 count)
{
	for (UINT32 at = 0; at < count; ++at)
		release(items[at]);
	if (items)
		FREE(items);
}

/* An element read as a type, through the element's own IJsonValue method at a place. */
static HRESULT element_as(void *iface, UINT32 index, size_t place, void *out)
{
	void *element = NULL;
	HRESULT result = array_at(array_of(iface), index, &element);
	if (SUCCEEDED(result))
		result = element ? ((HRESULT (WINAPI *)(void *, void *))method(element, place))(element, out) : E_ILLEGAL_METHOD_CALL;
	release(element);
	return result;
}

static HRESULT WINAPI array_object_at(void *iface, UINT32 index, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return element_as(iface, index, VALUE_OBJECT, out);
}

static HRESULT WINAPI array_array_at(void *iface, UINT32 index, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return element_as(iface, index, VALUE_ARRAY, out);
}

static HRESULT WINAPI array_string_at(void *iface, UINT32 index, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return element_as(iface, index, VALUE_STRING, out);
}

static HRESULT WINAPI array_number_at(void *iface, UINT32 index, double *out)
{
	return out ? element_as(iface, index, VALUE_NUMBER, out) : E_POINTER;
}

static HRESULT WINAPI array_boolean_at(void *iface, UINT32 index, boolean *out)
{
	return out ? element_as(iface, index, VALUE_BOOLEAN, out) : E_POINTER;
}

static HRESULT WINAPI array_type(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = JSON_ARRAY;
	return S_OK;
}

static HRESULT WINAPI array_write(void *iface, HSTRING *out)
{
	struct text text = {0};
	void **items;
	UINT32 count;
	if (!out)
		return E_POINTER;
	*out = NULL;
	const HRESULT result = array_items(array_of(iface), &items, &count);
	if (FAILED(result))
		return result;
	put_char(&text, '[');
	for (UINT32 at = 0; at < count; ++at) {
		if (at)
			put_char(&text, ',');
		put_value(&text, items[at]);
	}
	put_char(&text, ']');
	items_free(items, count);
	return text_finish(&text, out);
}

static HRESULT WINAPI not_string(void *iface, HSTRING *out) { return out ? (*out = NULL, E_ILLEGAL_METHOD_CALL) : E_POINTER; }
static HRESULT WINAPI not_number(void *iface, double *out) { return out ? E_ILLEGAL_METHOD_CALL : E_POINTER; }
static HRESULT WINAPI not_boolean(void *iface, boolean *out) { return out ? E_ILLEGAL_METHOD_CALL : E_POINTER; }
static HRESULT WINAPI not_object(void *iface, void **out) { return out ? (*out = NULL, E_ILLEGAL_METHOD_CALL) : E_POINTER; }

static HRESULT WINAPI array_self(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = face_of(owner(iface), 0));
	return S_OK;
}

static HRESULT WINAPI vector_at(void *iface, UINT32 index, void **out) { return array_at(array_of(iface), index, out); }

static HRESULT WINAPI vector_size(void *iface, UINT32 *out)
{
	struct json_array *self = array_of(iface);
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	*out = self->count;
	ReleaseSRWLockShared(&self->lock);
	return S_OK;
}

static HRESULT snapshot(void *iface, BOOL iterator, void **out)
{
	void **items;
	UINT32 count;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = array_items(array_of(iface), &items, &count);
	if (SUCCEEDED(result))
		result = winrt_items(&value_list, (IInspectable **)items, count, iterator, out);
	items_free(items, count);
	return result;
}

static HRESULT WINAPI vector_view(void *iface, void **out) { return snapshot(iface, FALSE, out); }
static HRESULT WINAPI vector_first(void *iface, void **out) { return snapshot(iface, TRUE, out); }

static HRESULT WINAPI vector_index(void *iface, void *value, UINT32 *index, boolean *found)
{
	struct json_array *self = array_of(iface);
	if (!index || !found)
		return E_POINTER;
	*index = 0;
	*found = FALSE;
	AcquireSRWLockShared(&self->lock);
	for (UINT32 at = 0; at < self->count && !*found; ++at)
		if (self->items[at] == value) {
			*index = at;
			*found = TRUE;
		}
	ReleaseSRWLockShared(&self->lock);
	return S_OK;
}

static HRESULT WINAPI vector_set(void *iface, UINT32 index, void *value)
{
	struct json_array *self = array_of(iface);
	void *old = NULL;
	AcquireSRWLockExclusive(&self->lock);
	const HRESULT result = index < self->count ? S_OK : E_BOUNDS;
	if (SUCCEEDED(result)) {
		old = self->items[index];
		if ((self->items[index] = value))
			reference(value);
	}
	ReleaseSRWLockExclusive(&self->lock);
	release(old);
	return result;
}

static HRESULT WINAPI vector_insert(void *iface, UINT32 index, void *value) { return array_insert(array_of(iface), &index, value); }

static HRESULT WINAPI vector_remove(void *iface, UINT32 index)
{
	struct json_array *self = array_of(iface);
	void *old = NULL;
	AcquireSRWLockExclusive(&self->lock);
	const HRESULT result = index < self->count ? S_OK : E_BOUNDS;
	if (SUCCEEDED(result)) {
		old = self->items[index];
		MoveMemory(self->items + index, self->items + index + 1, (self->count - index - 1) * sizeof *self->items);
		--self->count;
	}
	ReleaseSRWLockExclusive(&self->lock);
	release(old);
	return result;
}

static HRESULT WINAPI vector_append(void *iface, void *value) { return array_insert(array_of(iface), NULL, value); }

static HRESULT WINAPI vector_remove_last(void *iface)
{
	struct json_array *self = array_of(iface);
	void *old = NULL;
	AcquireSRWLockExclusive(&self->lock);
	const HRESULT result = self->count ? S_OK : E_BOUNDS;
	if (SUCCEEDED(result))
		old = self->items[--self->count];
	ReleaseSRWLockExclusive(&self->lock);
	release(old);
	return result;
}

static void array_take_items(struct json_array *self, void **items, UINT32 count, void ***old, UINT32 *old_count)
{
	*old = self->items;
	*old_count = self->count;
	self->items = items;
	self->count = self->capacity = count;
}

static HRESULT WINAPI vector_clear(void *iface)
{
	struct json_array *self = array_of(iface);
	void **old;
	UINT32 old_count;
	AcquireSRWLockExclusive(&self->lock);
	array_take_items(self, NULL, 0, &old, &old_count);
	ReleaseSRWLockExclusive(&self->lock);
	items_free(old, old_count);
	return S_OK;
}

static HRESULT WINAPI vector_many(void *iface, UINT32 start, UINT32 capacity, void **into, UINT32 *count)
{
	void **items;
	UINT32 total;
	if (!count || (capacity && !into))
		return E_POINTER;
	*count = 0;
	HRESULT result = array_items(array_of(iface), &items, &total);
	if (SUCCEEDED(result) && start > total)
		result = E_BOUNDS;
	for (UINT32 at = start; SUCCEEDED(result) && at < total && *count < capacity; ++at) {
		into[(*count)++] = items[at];
		items[at] = NULL;
	}
	items_free(items, total);
	return result;
}

static HRESULT WINAPI vector_replace(void *iface, UINT32 count, void **values)
{
	struct json_array *self = array_of(iface);
	void **items = NULL, **old;
	UINT32 old_count;
	if (count && !values)
		return E_POINTER;
	if (count && !(items = ALLOC(count * sizeof *items)))
		return E_OUTOFMEMORY;
	for (UINT32 at = 0; at < count; ++at)
		if ((items[at] = values[at]))
			reference(items[at]);
	AcquireSRWLockExclusive(&self->lock);
	array_take_items(self, items, count, &old, &old_count);
	ReleaseSRWLockExclusive(&self->lock);
	items_free(old, old_count);
	return S_OK;
}

TABLE(array_table, 0, INSPECTABLE, (const void *)array_object_at, (const void *)array_array_at, (const void *)array_string_at,
	(const void *)array_number_at, (const void *)array_boolean_at);
TABLE(array_value_table, 1, INSPECTABLE, (const void *)array_type, (const void *)array_write, (const void *)not_string,
	(const void *)not_number, (const void *)not_boolean, (const void *)array_self, (const void *)not_object);
TABLE(array_vector_table, 2, INSPECTABLE, (const void *)vector_at, (const void *)vector_size, (const void *)vector_view,
	(const void *)vector_index, (const void *)vector_set, (const void *)vector_insert, (const void *)vector_remove,
	(const void *)vector_append, (const void *)vector_remove_last, (const void *)vector_clear, (const void *)vector_many,
	(const void *)vector_replace);
TABLE(array_iterable_table, 3, INSPECTABLE, (const void *)vector_first);
TABLE(array_stringable_table, 4, INSPECTABLE, (const void *)array_write);

static struct json_array *array_make(void)
{
	static const void *const *const tables[] = {array_table.slots, array_value_table.slots, array_vector_table.slots,
		array_iterable_table.slots, array_stringable_table.slots};
	struct json_array *self = make(sizeof *self, &array_kind, tables, 5);
	if (self)
		InitializeSRWLock(&self->lock);
	return self;
}

/* JsonObject: a map from names to IJsonValue, with the faces of the class at places 3 on. */
enum { OBJECT_FACE = 3, OBJECT_VALUE_FACE, OBJECT_DEFAULTS_FACE, OBJECT_STRINGABLE_FACE };

static HRESULT WINAPI object_named_value(void *iface, HSTRING name, void **out);

/* A member read as a type, through the member's own IJsonValue method at a place. */
static HRESULT member_as(void *iface, HSTRING name, size_t place, void *out)
{
	IInspectable *member = NULL;
	HRESULT result = map_lookup_any(iface, name, &member);
	if (result == E_BOUNDS)
		return VALUE_NOT_FOUND;
	if (SUCCEEDED(result))
		result = member ? ((HRESULT (WINAPI *)(void *, void *))method(member, place))(member, out) : E_ILLEGAL_METHOD_CALL;
	release(member);
	return result;
}

static HRESULT WINAPI object_named_value(void *iface, HSTRING name, void **out)
{
	IInspectable *member = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = map_lookup_any(iface, name, &member);
	if (result == E_BOUNDS)
		return VALUE_NOT_FOUND;
	if (SUCCEEDED(result))
		result = as_value(member, out);
	release(member);
	return result;
}

static HRESULT WINAPI object_set_named(void *iface, HSTRING name, void *value)
{
	return value ? map_insert_any(iface, name, value) : E_INVALIDARG;
}

static HRESULT WINAPI object_named_object(void *iface, HSTRING name, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return member_as(iface, name, VALUE_OBJECT, out);
}

static HRESULT WINAPI object_named_array(void *iface, HSTRING name, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return member_as(iface, name, VALUE_ARRAY, out);
}

static HRESULT WINAPI object_named_string(void *iface, HSTRING name, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return member_as(iface, name, VALUE_STRING, out);
}

static HRESULT WINAPI object_named_number(void *iface, HSTRING name, double *out)
{
	return out ? member_as(iface, name, VALUE_NUMBER, out) : E_POINTER;
}

static HRESULT WINAPI object_named_boolean(void *iface, HSTRING name, boolean *out)
{
	return out ? member_as(iface, name, VALUE_BOOLEAN, out) : E_POINTER;
}

static HRESULT WINAPI object_type(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = JSON_OBJECT;
	return S_OK;
}

static HRESULT WINAPI object_write(void *iface, HSTRING *out)
{
	struct text text = {0};
	struct entry *entries;
	UINT32 count;
	if (!out)
		return E_POINTER;
	*out = NULL;
	const HRESULT result = property_set_entries((struct property_set *)owner(iface), &entries, &count);
	if (FAILED(result))
		return result;
	put_char(&text, '{');
	for (UINT32 at = 0; at < count; ++at) {
		if (at)
			put_char(&text, ',');
		put_quoted(&text, entries[at].key);
		put_char(&text, ':');
		put_value(&text, entries[at].value);
	}
	put_char(&text, '}');
	entries_free(entries, count);
	return text_finish(&text, out);
}

static HRESULT WINAPI object_self(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = face_of(owner(iface), OBJECT_FACE));
	return S_OK;
}

/* The OrDefault methods: the default when the object has no member of the name, the member read as the type when it has. */
static HRESULT WINAPI object_value_or(void *iface, HSTRING name, void *fallback, void **out)
{
	if (!out)
		return E_POINTER;
	const HRESULT result = object_named_value(iface, name, out);
	if (result != VALUE_NOT_FOUND)
		return result;
	if ((*out = fallback))
		reference(fallback);
	return S_OK;
}

static HRESULT WINAPI object_object_or(void *iface, HSTRING name, void *fallback, void **out)
{
	if (!out)
		return E_POINTER;
	const HRESULT result = object_named_object(iface, name, out);
	if (result != VALUE_NOT_FOUND)
		return result;
	if ((*out = fallback))
		reference(fallback);
	return S_OK;
}

static HRESULT WINAPI object_string_or(void *iface, HSTRING name, HSTRING fallback, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	const HRESULT result = object_named_string(iface, name, out);
	return result == VALUE_NOT_FOUND ? WindowsDuplicateString(fallback, out) : result;
}

static HRESULT WINAPI object_array_or(void *iface, HSTRING name, void *fallback, void **out)
{
	if (!out)
		return E_POINTER;
	const HRESULT result = object_named_array(iface, name, out);
	if (result != VALUE_NOT_FOUND)
		return result;
	if ((*out = fallback))
		reference(fallback);
	return S_OK;
}

static HRESULT WINAPI object_number_or(void *iface, HSTRING name, double fallback, double *out)
{
	if (!out)
		return E_POINTER;
	const HRESULT result = object_named_number(iface, name, out);
	if (result != VALUE_NOT_FOUND)
		return result;
	*out = fallback;
	return S_OK;
}

static HRESULT WINAPI object_boolean_or(void *iface, HSTRING name, boolean fallback, boolean *out)
{
	if (!out)
		return E_POINTER;
	const HRESULT result = object_named_boolean(iface, name, out);
	if (result != VALUE_NOT_FOUND)
		return result;
	*out = fallback;
	return S_OK;
}

TABLE(object_table, OBJECT_FACE, INSPECTABLE, (const void *)object_named_value, (const void *)object_set_named,
	(const void *)object_named_object, (const void *)object_named_array, (const void *)object_named_string,
	(const void *)object_named_number, (const void *)object_named_boolean);
TABLE(object_value_table, OBJECT_VALUE_FACE, INSPECTABLE, (const void *)object_type, (const void *)object_write, (const void *)not_string,
	(const void *)not_number, (const void *)not_boolean, (const void *)not_object, (const void *)object_self);
TABLE(object_defaults_table, OBJECT_DEFAULTS_FACE, INSPECTABLE, (const void *)object_value_or, (const void *)object_object_or,
	(const void *)object_string_or, (const void *)object_array_or, (const void *)object_number_or, (const void *)object_boolean_or);
TABLE(object_stringable_table, OBJECT_STRINGABLE_FACE, INSPECTABLE, (const void *)object_write);
static const struct map_face object_faces[] = {{&iid_json_object, object_table.slots}, {&iid_json_value, object_value_table.slots},
	{&iid_object_defaults, object_defaults_table.slots}, {&iid_stringable, object_stringable_table.slots}};
static struct map_types object_types = {&iid_value_map_view, &iid_value_pairs, &iid_value_pair_iterator, &iid_value_pair,
	L"Windows.Foundation.Collections.IMapView`2<String, Windows.Data.Json.IJsonValue>",
	L"Windows.Foundation.Collections.IKeyValuePair`2<String, Windows.Data.Json.IJsonValue>",
	L"Windows.Foundation.Collections.IIterator`1<Windows.Foundation.Collections.IKeyValuePair`2<String, Windows.Data.Json.IJsonValue>>",
	&iid_value_map, L"Windows.Data.Json.JsonObject", object_faces, NULL, ARRAYSIZE(object_faces), 0, "JsonObject", "IMapView"};
static INIT_ONCE object_types_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK make_object_types(INIT_ONCE *once, void *parameter, void **context)
{
	map_types_init(&object_types);
	return TRUE;
}

static struct property_set *object_make(void)
{
	InitOnceExecuteOnce(&object_types_once, make_object_types, NULL, NULL);
	return typed_map_make(&object_types);
}

static void *object_face(struct property_set *set, size_t face) { return face_of((struct object *)set, face); }

/* JSON text, RFC 8259, read into values: each comes out through its IJsonValue, with its type. */
struct parser {
	const WCHAR *at, *end;
};

static void skip_blanks(struct parser *parser)
{
	while (parser->at < parser->end && (*parser->at == ' ' || *parser->at == '\t' || *parser->at == '\n' || *parser->at == '\r'))
		++parser->at;
}

static BOOL take_word(struct parser *parser, const WCHAR *word)
{
	const WCHAR *at = parser->at;
	for (; *word; ++word, ++at)
		if (at == parser->end || *at != *word)
			return FALSE;
	parser->at = at;
	return TRUE;
}

static int hex_digit(WCHAR c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* The character an escape stands for, the parser past its backslash; -1 for no valid escape. */
static int parse_escape(struct parser *parser)
{
	static const WCHAR plain[] = L"\"\\/bfnrt", meant[] = L"\"\\/\b\f\n\r\t";
	if (parser->at == parser->end)
		return -1;
	const WCHAR escape = *parser->at++;
	for (size_t at = 0; plain[at]; ++at)
		if (plain[at] == escape)
			return meant[at];
	if (escape != 'u' || parser->end - parser->at < 4)
		return -1;
	int code = 0;
	for (int digit = 0; digit < 4; ++digit) {
		const int value = hex_digit(*parser->at++);
		if (value < 0)
			return -1;
		code = code * 16 + value;
	}
	return code;
}

/* A quoted string, the parser at its opening quote. A control character must be escaped, and the string must end. */
static HRESULT parse_string(struct parser *parser, HSTRING *out)
{
	struct text text = {0};
	int meant = 0;
	*out = NULL;
	++parser->at;
	while (parser->at < parser->end && *parser->at != '"') {
		const WCHAR c = *parser->at++;
		if ((meant = c == '\\' ? parse_escape(parser) : c < 0x20 ? -1 : c) < 0)
			break;
		put_char(&text, (WCHAR)meant);
	}
	if (meant < 0 || parser->at == parser->end) {
		text_drop(&text);
		return INVALID_STRING;
	}
	++parser->at;
	return text_finish(&text, out);
}

/* A number, checked against JSON's grammar and read by the CRT. */
static HRESULT parse_number(struct parser *parser, double *out)
{
	const WCHAR *start = parser->at, *at = parser->at;
	if (at < parser->end && *at == '-')
		++at;
	if (at < parser->end && *at == '0')
		++at;
	else if (at < parser->end && *at >= '1' && *at <= '9')
		while (at < parser->end && *at >= '0' && *at <= '9')
			++at;
	else
		return INVALID_NUMBER;
	if (at < parser->end && *at == '.') {
		if (++at == parser->end || *at < '0' || *at > '9')
			return INVALID_NUMBER;
		while (at < parser->end && *at >= '0' && *at <= '9')
			++at;
	}
	if (at < parser->end && (*at == 'e' || *at == 'E')) {
		if (++at < parser->end && (*at == '+' || *at == '-'))
			++at;
		if (at == parser->end || *at < '0' || *at > '9')
			return INVALID_NUMBER;
		while (at < parser->end && *at >= '0' && *at <= '9')
			++at;
	}
	if (!crt_ready())
		return E_FAIL;
	const size_t length = (size_t)(at - start);
	WCHAR *copy = ALLOC((length + 1) * sizeof(WCHAR));
	if (!copy)
		return E_OUTOFMEMORY;
	CopyMemory(copy, start, length * sizeof(WCHAR));
	*out = crt_read(copy, NULL);
	FREE(copy);
	parser->at = at;
	return is_finite(*out) ? S_OK : INVALID_NUMBER;
}

static HRESULT parse_value(struct parser *parser, UINT32 depth, void **out, INT32 *type);

static HRESULT parse_array(struct parser *parser, UINT32 depth, void **out)
{
	struct json_array *self = array_make();
	if (!self)
		return E_OUTOFMEMORY;
	HRESULT result = S_OK;
	++parser->at;
	skip_blanks(parser);
	if (parser->at < parser->end && *parser->at == ']')
		++parser->at;
	else
		for (;;) {
			void *element;
			INT32 type;
			if (FAILED(result = parse_value(parser, depth + 1, &element, &type)))
				break;
			result = array_insert(self, NULL, element);
			release(element);
			if (FAILED(result))
				break;
			skip_blanks(parser);
			if (parser->at < parser->end && *parser->at == ',') {
				++parser->at;
				continue;
			}
			if (parser->at < parser->end && *parser->at == ']')
				++parser->at;
			else
				result = INVALID_STRING;
			break;
		}
	if (FAILED(result)) {
		release(face_of(&self->base, 0));
		return result;
	}
	*out = face_of(&self->base, 1);
	return S_OK;
}

static HRESULT parse_object(struct parser *parser, UINT32 depth, void **out)
{
	struct property_set *self = object_make();
	if (!self)
		return E_OUTOFMEMORY;
	HRESULT result = S_OK;
	++parser->at;
	skip_blanks(parser);
	if (parser->at < parser->end && *parser->at == '}')
		++parser->at;
	else
		for (;;) {
			HSTRING name = NULL;
			void *member;
			INT32 type;
			skip_blanks(parser);
			if (parser->at == parser->end || *parser->at != '"') {
				result = INVALID_STRING;
				break;
			}
			if (FAILED(result = parse_string(parser, &name)))
				break;
			skip_blanks(parser);
			if (parser->at == parser->end || *parser->at != ':') {
				WindowsDeleteString(name);
				result = INVALID_STRING;
				break;
			}
			++parser->at;
			if (SUCCEEDED(result = parse_value(parser, depth + 1, &member, &type))) {
				result = property_set_insert(self, name, member);
				release(member);
			}
			WindowsDeleteString(name);
			if (FAILED(result))
				break;
			skip_blanks(parser);
			if (parser->at < parser->end && *parser->at == ',') {
				++parser->at;
				continue;
			}
			if (parser->at < parser->end && *parser->at == '}')
				++parser->at;
			else
				result = INVALID_STRING;
			break;
		}
	if (FAILED(result)) {
		release(object_face(self, OBJECT_FACE));
		return result;
	}
	*out = object_face(self, OBJECT_VALUE_FACE);
	return S_OK;
}

static HRESULT parse_value(struct parser *parser, UINT32 depth, void **out, INT32 *type)
{
	*out = NULL;
	skip_blanks(parser);
	if (parser->at == parser->end || depth > DEEPEST)
		return INVALID_STRING;
	const WCHAR c = *parser->at;
	struct json_value *value = NULL;
	HRESULT result = S_OK;
	if (c == '{') {
		*type = JSON_OBJECT;
		return parse_object(parser, depth, out);
	}
	if (c == '[') {
		*type = JSON_ARRAY;
		return parse_array(parser, depth, out);
	}
	if (c == '"') {
		HSTRING string = NULL;
		if (FAILED(result = parse_string(parser, &string)))
			return result;
		if ((value = value_make(JSON_STRING)))
			value->string = string;
		else
			WindowsDeleteString(string);
	} else if (c == '-' || (c >= '0' && c <= '9')) {
		double number;
		if (FAILED(result = parse_number(parser, &number)))
			return result;
		if ((value = value_make(JSON_NUMBER)))
			value->number = number;
	} else if (take_word(parser, L"true")) {
		if ((value = value_make(JSON_BOOLEAN)))
			value->flag = TRUE;
	} else if (take_word(parser, L"false"))
		value = value_make(JSON_BOOLEAN);
	else if (take_word(parser, L"null"))
		value = value_make(JSON_NULL);
	else
		return INVALID_STRING;
	*type = value ? value->type : JSON_NULL;
	return value_out(value, out);
}

/* A whole text: one value with nothing but blanks around it. */
static HRESULT parse_text(HSTRING input, void **out, INT32 *type)
{
	UINT32 length = 0;
	const WCHAR *chars = WindowsGetStringRawBuffer(input, &length);
	struct parser parser = {chars, chars + length};
	HRESULT result = parse_value(&parser, 0, out, type);
	if (SUCCEEDED(result)) {
		skip_blanks(&parser);
		if (parser.at != parser.end) {
			release(*out);
			*out = NULL;
			result = INVALID_STRING;
		}
	}
	return result;
}

/* Parse of each class: a JsonValue for any value, a JsonObject or a JsonArray only for one of those. */
static HRESULT parse_as(HSTRING input, INT32 wanted, void **out)
{
	void *value;
	INT32 type;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = parse_text(input, &value, &type);
	if (FAILED(result))
		return result;
	if (wanted == JSON_NULL)
		result = (type == JSON_OBJECT || type == JSON_ARRAY) ? as_value(value, out) : (reference(*out = value), S_OK);
	else if (type != wanted)
		result = INVALID_STRING;
	else
		reference(*out = face_of(owner(value), wanted == JSON_OBJECT ? OBJECT_FACE : 0));
	release(value);
	return result;
}

static HRESULT try_parse_as(HSTRING input, INT32 wanted, void **out, boolean *succeeded)
{
	if (!out || !succeeded)
		return E_POINTER;
	const HRESULT result = parse_as(input, wanted, out);
	*succeeded = SUCCEEDED(result);
	return result == E_OUTOFMEMORY ? result : S_OK;
}

static HRESULT WINAPI value_parse(void *iface, HSTRING input, void **out) { return parse_as(input, JSON_NULL, out); }

static HRESULT WINAPI value_try_parse(void *iface, HSTRING input, void **out, boolean *succeeded)
{
	return try_parse_as(input, JSON_NULL, out, succeeded);
}

static HRESULT WINAPI create_boolean(void *iface, boolean input, void **out)
{
	if (!out)
		return E_POINTER;
	struct json_value *self = value_make(JSON_BOOLEAN);
	if (self)
		self->flag = input != 0;
	return value_out(self, out);
}

static HRESULT WINAPI create_number(void *iface, double input, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!is_finite(input))
		return E_INVALIDARG;
	struct json_value *self = value_make(JSON_NUMBER);
	if (self)
		self->number = input;
	return value_out(self, out);
}

static HRESULT WINAPI create_string(void *iface, HSTRING input, void **out)
{
	if (!out)
		return E_POINTER;
	struct json_value *self = value_make(JSON_STRING);
	if (self && FAILED(WindowsDuplicateString(input, &self->string))) {
		release(face_of(&self->base, 0));
		self = NULL;
	}
	return value_out(self, out);
}

static HRESULT WINAPI create_null(void *iface, void **out) { return out ? value_out(value_make(JSON_NULL), out) : E_POINTER; }

static HRESULT WINAPI object_parse(void *iface, HSTRING input, void **out) { return parse_as(input, JSON_OBJECT, out); }

static HRESULT WINAPI object_try_parse(void *iface, HSTRING input, void **out, boolean *succeeded)
{
	return try_parse_as(input, JSON_OBJECT, out, succeeded);
}

static HRESULT WINAPI array_parse(void *iface, HSTRING input, void **out) { return parse_as(input, JSON_ARRAY, out); }

static HRESULT WINAPI array_try_parse(void *iface, HSTRING input, void **out, boolean *succeeded)
{
	return try_parse_as(input, JSON_ARRAY, out, succeeded);
}

/* new JsonObject() and new JsonArray(): empty, through their default interfaces. */
static HRESULT WINAPI object_activate(void *iface, IInspectable **out)
{
	if (!out)
		return E_POINTER;
	struct property_set *self = object_make();
	*out = self ? object_face(self, OBJECT_FACE) : NULL;
	return self ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI array_activate(void *iface, IInspectable **out)
{
	if (!out)
		return E_POINTER;
	struct json_array *self = array_make();
	*out = self ? face_of(&self->base, 0) : NULL;
	return self ? S_OK : E_OUTOFMEMORY;
}

/* JsonError.GetJsonStatus: the JsonErrorStatus a failure of this component stands for, Unknown for any other. */
static HRESULT WINAPI error_status(void *iface, INT32 failure, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = failure == INVALID_STRING ? 1 : failure == INVALID_NUMBER ? 2 : failure == VALUE_NOT_FOUND ? 3 : 0;
	return S_OK;
}

TABLE(value_factory_table, 0, FACTORY);
TABLE(value_statics_table, 1, INSPECTABLE, (const void *)value_parse, (const void *)value_try_parse, (const void *)create_boolean,
	(const void *)create_number, (const void *)create_string);
TABLE(value_statics2_table, 2, INSPECTABLE, (const void *)create_null);
static const struct face_iid value_class_iids[] = {{&iid_factory, 0}, {&iid_value_statics, 1}, {&iid_value_statics2, 2}, {NULL, 0}};
static const struct kind value_class_kind = {"JsonValue", L"Windows.Data.Json.JsonValue", value_class_iids, NULL};
struct object json_value_class = {{value_factory_table.slots, value_statics_table.slots, value_statics2_table.slots}, &value_class_kind, 1};

TABLE(object_factory_table, 0, INSPECTABLE, (const void *)object_activate);
TABLE(object_statics_table, 1, INSPECTABLE, (const void *)object_parse, (const void *)object_try_parse);
static const struct face_iid object_class_iids[] = {{&iid_factory, 0}, {&iid_object_statics, 1}, {NULL, 0}};
static const struct kind object_class_kind = {"JsonObject", L"Windows.Data.Json.JsonObject", object_class_iids, NULL};
struct object json_object_class = {{object_factory_table.slots, object_statics_table.slots}, &object_class_kind, 1};

TABLE(array_factory_table, 0, INSPECTABLE, (const void *)array_activate);
TABLE(array_statics_table, 1, INSPECTABLE, (const void *)array_parse, (const void *)array_try_parse);
static const struct face_iid array_class_iids[] = {{&iid_factory, 0}, {&iid_array_statics, 1}, {NULL, 0}};
static const struct kind array_class_kind = {"JsonArray", L"Windows.Data.Json.JsonArray", array_class_iids, NULL};
struct object json_array_class = {{array_factory_table.slots, array_statics_table.slots}, &array_class_kind, 1};

TABLE(error_factory_table, 0, FACTORY);
TABLE(error_statics_table, 1, INSPECTABLE, (const void *)error_status);
static const struct face_iid error_class_iids[] = {{&iid_factory, 0}, {&iid_error_statics2, 1}, {NULL, 0}};
static const struct kind error_class_kind = {"JsonError", L"Windows.Data.Json.JsonError", error_class_iids, NULL};
struct object json_error_class = {{error_factory_table.slots, error_statics_table.slots}, &error_class_kind, 1};
