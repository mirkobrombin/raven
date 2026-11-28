// Windows.Data.Json as an app uses it: a document parsed into a JsonObject and read member by member, array element by element, and
// written back by Stringify; values made with JsonValue's Create methods; an object and an array built through IMap and IVector; numbers
// written in the fewest digits that read back; text that is not JSON refused with the documented failures, which JsonError names. The exit
// code names the first check that failed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <roapi.h>
#include <winstring.h>

typedef struct object object;
struct object {
	void **table;
};
#define CALL(self, place, type) ((type)(self)->table[place])

typedef HRESULT (WINAPI *query_method)(object *, const GUID *, object **);
typedef ULONG (WINAPI *release_method)(object *);
typedef HRESULT (WINAPI *string_method)(object *, HSTRING *);
typedef HRESULT (WINAPI *object_method)(object *, object **);
typedef HRESULT (WINAPI *type_method)(object *, INT32 *);
typedef HRESULT (WINAPI *number_method)(object *, double *);
typedef HRESULT (WINAPI *flag_method)(object *, boolean *);
typedef HRESULT (WINAPI *parse_method)(object *, HSTRING, object **);
typedef HRESULT (WINAPI *try_parse_method)(object *, HSTRING, object **, boolean *);
typedef HRESULT (WINAPI *named_object_method)(object *, HSTRING, object **);
typedef HRESULT (WINAPI *named_string_method)(object *, HSTRING, HSTRING *);
typedef HRESULT (WINAPI *named_number_method)(object *, HSTRING, double *);
typedef HRESULT (WINAPI *named_flag_method)(object *, HSTRING, boolean *);
typedef HRESULT (WINAPI *at_object_method)(object *, UINT32, object **);
typedef HRESULT (WINAPI *at_string_method)(object *, UINT32, HSTRING *);
typedef HRESULT (WINAPI *at_number_method)(object *, UINT32, double *);
typedef HRESULT (WINAPI *at_flag_method)(object *, UINT32, boolean *);
typedef HRESULT (WINAPI *size_method)(object *, UINT32 *);
typedef HRESULT (WINAPI *put_object_method)(object *, object *);

static const GUID value_iid = {0xa3219ecb, 0xf0b3, 0x4dcd, {0xbe, 0xee, 0x19, 0xd4, 0x8c, 0xd3, 0xed, 0x1e}};
static const GUID object_iid = {0x064e24dd, 0x29c2, 0x4f83, {0x9a, 0xc1, 0x9e, 0xe1, 0x15, 0x78, 0xbe, 0xb3}};
static const GUID array_iid = {0x08c1ddb6, 0x0cbd, 0x4a9a, {0xb5, 0xd3, 0x2f, 0x85, 0x2d, 0xc3, 0x7e, 0x81}};
static const GUID defaults_iid = {0xd960d2a2, 0xb7f0, 0x4f00, {0x8e, 0x44, 0xd8, 0x2c, 0xf4, 0x15, 0xea, 0x13}};
static const GUID value_statics = {0x5f6b544a, 0x2f53, 0x48e1, {0x91, 0xa3, 0xf7, 0x8b, 0x50, 0xa6, 0x34, 0x5c}};
static const GUID value_statics2 = {0x1d9ecbe4, 0x3fe8, 0x4335, {0x83, 0x92, 0x93, 0xd8, 0xe3, 0x68, 0x65, 0xf0}};
static const GUID object_statics = {0x2289f159, 0x54de, 0x45d8, {0xab, 0xcc, 0x22, 0x60, 0x3f, 0xa0, 0x66, 0xa0}};
static const GUID array_statics = {0xdb1434a9, 0xe164, 0x499f, {0x93, 0xe2, 0x8a, 0x8f, 0x49, 0xbb, 0x90, 0xba}};
static const GUID error_statics2 = {0x404030da, 0x87d0, 0x436c, {0x83, 0xab, 0xfc, 0x7b, 0x12, 0xc0, 0xcc, 0x26}};
static const GUID stringable_iid = {0x96369f54, 0x8eb6, 0x48f0, {0xab, 0xce, 0xc1, 0xb2, 0x11, 0xe6, 0x27, 0xc3}};
static const GUID map_iid = {0xc9d9a725, 0x786b, 0x5113, {0xb4, 0xb7, 0x9b, 0x61, 0x76, 0x4c, 0x22, 0x0b}};
static const GUID vector_iid = {0xd44662bc, 0xdce3, 0x59a8, {0x92, 0x72, 0x4b, 0x21, 0x0f, 0x33, 0x90, 0x8b}};
static const GUID iterable_iid = {0xcb0492b6, 0x4113, 0x55cf, {0xb2, 0xc5, 0x99, 0xeb, 0x42, 0x8b, 0xa4, 0x93}};

#define INVALID_STRING ((HRESULT)0x83750007)
#define INVALID_NUMBER ((HRESULT)0x83750008)
#define VALUE_NOT_FOUND ((HRESULT)0x83750009)
#define ILLEGAL_METHOD_CALL ((HRESULT)0x8000000E)

struct reference {
	UINT32 flags, length, unused[2];
	const WCHAR *text;
};

static object *values, *values2, *objects, *arrays, *errors;

static object *factory(const WCHAR *name, const GUID *iid)
{
	struct reference reference = {1, (UINT32)lstrlenW(name), {0, 0}, name};
	object *out = NULL;
	return RoGetActivationFactory((HSTRING)&reference, iid, (void **)&out) == S_OK ? out : NULL;
}

static object *activate(const WCHAR *name, const GUID *iid)
{
	struct reference reference = {1, (UINT32)lstrlenW(name), {0, 0}, name};
	object *made = NULL, *out = NULL;
	if (RoActivateInstance((HSTRING)&reference, (IInspectable **)&made) != S_OK || !made)
		return NULL;
	CALL(made, 0, query_method)(made, iid, &out);
	CALL(made, 2, release_method)(made);
	return out;
}

static void release(object *self)
{
	if (self)
		CALL(self, 2, release_method)(self);
}

static object *query(object *self, const GUID *iid)
{
	object *out = NULL;
	return self && CALL(self, 0, query_method)(self, iid, &out) == S_OK ? out : NULL;
}

static HSTRING text(const WCHAR *value)
{
	HSTRING made = NULL;
	WindowsCreateString(value, lstrlenW(value), &made);
	return made;
}

static BOOL same(HSTRING value, const WCHAR *expected)
{
	UINT32 length = 0;
	const WCHAR *chars = WindowsGetStringRawBuffer(value, &length);
	const BOOL equal = length == (UINT32)lstrlenW(expected) && CompareStringOrdinal(chars, (int)length, expected, -1, FALSE) == CSTR_EQUAL;
	WindowsDeleteString(value);
	return equal;
}

/* Stringify of any IJsonValue, compared with the text expected. */
static BOOL written_as(object *value, const WCHAR *expected)
{
	HSTRING written = NULL;
	return value && CALL(value, 7, string_method)(value, &written) == S_OK && same(written, expected);
}

static HRESULT parse(object *statics, const WCHAR *input, object **out)
{
	HSTRING string = text(input);
	*out = NULL;
	const HRESULT result = CALL(statics, 6, parse_method)(statics, string, out);
	WindowsDeleteString(string);
	return result;
}

static HRESULT named_string(object *json, const WCHAR *name, HSTRING *out)
{
	HSTRING key = text(name);
	const HRESULT result = CALL(json, 10, named_string_method)(json, key, out);
	WindowsDeleteString(key);
	return result;
}

static HRESULT named_number(object *json, const WCHAR *name, double *out)
{
	HSTRING key = text(name);
	const HRESULT result = CALL(json, 11, named_number_method)(json, key, out);
	WindowsDeleteString(key);
	return result;
}

static HRESULT named_object(object *json, size_t place, const WCHAR *name, object **out)
{
	HSTRING key = text(name);
	*out = NULL;
	const HRESULT result = CALL(json, place, named_object_method)(json, key, out);
	WindowsDeleteString(key);
	return result;
}

static const WCHAR document[] = L" {\"name\": \"Raven\", \"size\": 3.5, \"ok\": true, \"none\": null,\n"
	L"\t\"list\": [1, \"two\", false, {\"x\": -0.25}], \"nested\": {\"a\": \"b\\n\\u00e9\\/\"}} ";
static const WCHAR document_written[] = L"{\"name\":\"Raven\",\"size\":3.5,\"ok\":true,\"none\":null,"
	L"\"list\":[1,\"two\",false,{\"x\":-0.25}],\"nested\":{\"a\":\"b\\n\u00e9/\"}}";

/* A document read member by member and written back. */
static int check_document(void)
{
	object *json = NULL, *list = NULL, *nested = NULL, *item = NULL, *member = NULL, *vector = NULL, *value = NULL;
	HSTRING string = NULL;
	double number = 0;
	boolean flag = TRUE;
	INT32 type = -1;
	UINT32 size = 0;
	if (parse(objects, document, &json) != S_OK || !json) return 10;
	if (named_string(json, L"name", &string) != S_OK || !same(string, L"Raven")) return 11;
	if (named_number(json, L"size", &number) != S_OK || number != 3.5) return 12;
	HSTRING key = text(L"ok");
	if (CALL(json, 12, named_flag_method)(json, key, &flag) != S_OK || flag != TRUE) return 13;
	WindowsDeleteString(key);
	if (named_object(json, 6, L"none", &member) != S_OK || CALL(member, 6, type_method)(member, &type) != S_OK || type != 0) return 14;
	release(member);
	if (named_object(json, 9, L"list", &list) != S_OK || !list) return 15;
	if (!(vector = query(list, &vector_iid)) || CALL(vector, 7, size_method)(vector, &size) != S_OK || size != 4) return 16;
	if (CALL(list, 9, at_number_method)(list, 0, &number) != S_OK || number != 1) return 17;
	if (CALL(list, 8, at_string_method)(list, 1, &string) != S_OK || !same(string, L"two")) return 18;
	if (CALL(list, 10, at_flag_method)(list, 2, &flag) != S_OK || flag != FALSE) return 19;
	if (CALL(list, 6, at_object_method)(list, 3, &item) != S_OK || named_number(item, L"x", &number) != S_OK || number != -0.25) return 20;
	if (CALL(list, 8, at_string_method)(list, 9, &string) != E_BOUNDS) return 21;
	if (named_object(json, 8, L"nested", &nested) != S_OK || named_string(nested, L"a", &string) != S_OK ||
		!same(string, L"b\n\u00e9/")) return 22;
	if (named_string(json, L"missing", &string) != VALUE_NOT_FOUND) return 23;
	if (named_number(json, L"name", &number) != ILLEGAL_METHOD_CALL) return 24;
	if (!(value = query(json, &value_iid)) || CALL(value, 6, type_method)(value, &type) != S_OK || type != 5) return 25;
	if (!written_as(value, document_written)) return 26;
	object *stringable = query(json, &stringable_iid);
	HSTRING shown = NULL;
	if (!stringable || CALL(stringable, 6, string_method)(stringable, &shown) != S_OK || !same(shown, document_written)) return 27;
	if (!query(json, &map_iid) || !query(list, &iterable_iid)) return 28;
	/* The defaults stand in only for a member that is missing. */
	object *defaults = query(json, &defaults_iid);
	key = text(L"absent");
	if (!defaults || CALL(defaults, 10, HRESULT (WINAPI *)(object *, HSTRING, double, double *))(defaults, key, 7.0, &number) != S_OK ||
		number != 7) return 29;
	WindowsDeleteString(key);
	key = text(L"size");
	if (CALL(defaults, 10, HRESULT (WINAPI *)(object *, HSTRING, double, double *))(defaults, key, 7.0, &number) != S_OK ||
		number != 3.5) return 30;
	WindowsDeleteString(key);
	release(defaults);
	release(stringable);
	release(value);
	release(vector);
	release(item);
	release(nested);
	release(list);
	release(json);
	return 0;
}

static HRESULT number_value(double input, object **out)
{
	*out = NULL;
	return CALL(values, 9, HRESULT (WINAPI *)(object *, double, object **))(values, input, out);
}

/* Numbers as JavaScript writes them, and values that JsonValue makes. */
static int check_values(void)
{
	static const struct {
		double number;
		const WCHAR *written;
	} numbers[] = {{0.1, L"0.1"}, {100, L"100"}, {-1.5, L"-1.5"}, {1e21, L"1e+21"}, {1e-7, L"1e-7"}, {123456789012.0, L"123456789012"},
		{0.000001, L"0.000001"}, {1.0 / 3, L"0.3333333333333333"}, {-0.0, L"0"}, {5e-324, L"5e-324"}, {2.5e25, L"2.5e+25"}};
	object *value = NULL;
	for (size_t at = 0; at < sizeof numbers / sizeof *numbers; ++at) {
		if (number_value(numbers[at].number, &value) != S_OK || !written_as(value, numbers[at].written)) return 40 + (int)at;
		release(value);
	}
	HSTRING input = text(L"quote \" back \\ tab \t bell \x07");
	if (CALL(values, 10, parse_method)(values, input, &value) != S_OK ||
		!written_as(value, L"\"quote \\\" back \\\\ tab \\t bell \\u0007\"")) return 60;
	WindowsDeleteString(input);
	release(value);
	if (CALL(values, 8, HRESULT (WINAPI *)(object *, boolean, object **))(values, TRUE, &value) != S_OK || !written_as(value, L"true"))
		return 61;
	release(value);
	if (CALL(values2, 6, object_method)(values2, &value) != S_OK || !written_as(value, L"null")) return 62;
	release(value);
	/* JsonValue.Parse of an object hands out a JsonValue that stands for it. */
	HSTRING name = NULL;
	object *inner = NULL;
	if (parse(values, L"{\"k\": [true]}", &value) != S_OK || CALL(value, 4, string_method)(value, &name) != S_OK ||
		!same(name, L"Windows.Data.Json.JsonValue")) return 63;
	if (CALL(value, 12, object_method)(value, &inner) != S_OK || !inner || !written_as(value, L"{\"k\":[true]}")) return 64;
	release(inner);
	if (CALL(value, 11, object_method)(value, &inner) != ILLEGAL_METHOD_CALL) return 65;
	release(value);
	return 0;
}

/* Text that is not JSON, and what JsonError makes of the failures. */
static int check_failures(void)
{
	static const WCHAR *const broken[] = {L"", L"{", L"[1,]", L"01", L"{\"a\" 1}", L"tru", L"\"open", L"\"bad \\q\"", L"[1] 2",
		L"{'a': 1}", L"\"tab\there\"", L"NaN"};
	object *value = NULL;
	for (size_t at = 0; at < sizeof broken / sizeof *broken; ++at)
		if (parse(values, broken[at], &value) != INVALID_STRING || value) return 70 + (int)at;
	if (parse(values, L"1e999", &value) != INVALID_NUMBER || value) return 85;
	if (parse(values, L"-", &value) != INVALID_NUMBER || value) return 86;
	if (parse(objects, L"[1]", &value) == S_OK || value) return 87;
	if (parse(arrays, L"{}", &value) == S_OK || value) return 88;
	HSTRING input = text(L"[1,]");
	boolean succeeded = TRUE;
	if (CALL(arrays, 7, try_parse_method)(arrays, input, &value, &succeeded) != S_OK || succeeded || value) return 89;
	WindowsDeleteString(input);
	input = text(L"[[[]]]");
	if (CALL(arrays, 7, try_parse_method)(arrays, input, &value, &succeeded) != S_OK || !succeeded || !value) return 90;
	WindowsDeleteString(input);
	release(value);
	INT32 status = -1;
	if (CALL(errors, 6, HRESULT (WINAPI *)(object *, INT32, INT32 *))(errors, VALUE_NOT_FOUND, &status) != S_OK || status != 3) return 91;
	if (CALL(errors, 6, HRESULT (WINAPI *)(object *, INT32, INT32 *))(errors, INVALID_STRING, &status) != S_OK || status != 1) return 92;
	if (CALL(errors, 6, HRESULT (WINAPI *)(object *, INT32, INT32 *))(errors, E_FAIL, &status) != S_OK || status != 0) return 93;
	return 0;
}

/* An object and an array built by an app. */
static int check_building(void)
{
	object *json = activate(L"Windows.Data.Json.JsonObject", &object_iid), *array = activate(L"Windows.Data.Json.JsonArray", &array_iid);
	object *value = NULL, *vector = NULL, *map = NULL, *as_value = NULL;
	if (!json || !array) return 100;
	HSTRING input = text(L"v\""), key = text(L"k");
	if (CALL(values, 10, parse_method)(values, input, &value) != S_OK) return 101;
	if (CALL(json, 7, HRESULT (WINAPI *)(object *, HSTRING, object *))(json, key, value) != S_OK) return 102;
	release(value);
	if (!(as_value = query(json, &value_iid)) || !written_as(as_value, L"{\"k\":\"v\\\"\"}")) return 103;
	UINT32 size = 0;
	if (!(map = query(json, &map_iid)) || CALL(map, 7, size_method)(map, &size) != S_OK || size != 1) return 104;
	if (!(vector = query(array, &vector_iid))) return 105;
	if (CALL(values, 8, HRESULT (WINAPI *)(object *, boolean, object **))(values, TRUE, &value) != S_OK ||
		CALL(vector, 13, put_object_method)(vector, value) != S_OK) return 106;
	release(value);
	if (CALL(values2, 6, object_method)(values2, &value) != S_OK || CALL(vector, 13, put_object_method)(vector, value) != S_OK) return 107;
	release(value);
	/* The object goes in the array as the value it is. */
	if (CALL(vector, 13, put_object_method)(vector, as_value) != S_OK) return 108;
	release(as_value);
	object *array_value = query(array, &value_iid);
	if (!written_as(array_value, L"[true,null,{\"k\":\"v\\\"\"}]")) return 109;
	if (CALL(vector, 14, HRESULT (WINAPI *)(object *))(vector) != S_OK || !written_as(array_value, L"[true,null]")) return 110;
	if (CALL(vector, 12, HRESULT (WINAPI *)(object *, UINT32))(vector, 0) != S_OK || !written_as(array_value, L"[null]")) return 111;
	if (CALL(vector, 15, HRESULT (WINAPI *)(object *))(vector) != S_OK || !written_as(array_value, L"[]")) return 112;
	WindowsDeleteString(input);
	WindowsDeleteString(key);
	release(array_value);
	release(vector);
	release(map);
	release(array);
	release(json);
	return 0;
}

int raven_entry(void)
{
	if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return 1;
	if (!(values = factory(L"Windows.Data.Json.JsonValue", &value_statics))) return 2;
	if (!(values2 = factory(L"Windows.Data.Json.JsonValue", &value_statics2))) return 3;
	if (!(objects = factory(L"Windows.Data.Json.JsonObject", &object_statics))) return 4;
	if (!(arrays = factory(L"Windows.Data.Json.JsonArray", &array_statics))) return 5;
	if (!(errors = factory(L"Windows.Data.Json.JsonError", &error_statics2))) return 6;
	int failed = check_document();
	if (!failed)
		failed = check_values();
	if (!failed)
		failed = check_failures();
	if (!failed)
		failed = check_building();
	return failed;
}
