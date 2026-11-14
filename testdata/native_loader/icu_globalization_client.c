// The ICU surface .NET's globalization loads from icu.dll before it starts: every function resolved by name, as .NET gives up on the
// first one missing, and the answers its cultures, comparisons, calendars and domain names are built from. The paint.net setup runs on
// .NET and stops at the first missing name. The exit code names the first check that failed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const char *const required[] = {
	"u_charsToUChars", "u_getVersion", "u_strcmp", "u_strcpy", "u_strlen", "u_strncpy", "u_tolower", "u_toupper", "u_uastrncpy",
	"ubrk_close", "ubrk_openRules", "ucal_add", "ucal_close", "ucal_get", "ucal_getAttribute", "ucal_getKeywordValuesForLocale",
	"ucal_getLimit", "ucal_getNow", "ucal_getTimeZoneDisplayName", "ucal_getTimeZoneIDForWindowsID", "ucal_getWindowsTimeZoneID",
	"ucal_open", "ucal_openTimeZoneIDEnumeration", "ucal_set", "ucal_setMillis", "ucol_close", "ucol_closeElements", "ucol_getOffset",
	"ucol_getRules", "ucol_getSortKey", "ucol_getStrength", "ucol_getVersion", "ucol_next", "ucol_previous", "ucol_open",
	"ucol_openElements", "ucol_openRules", "ucol_setAttribute", "ucol_setMaxVariable", "ucol_strcoll", "udat_close", "udat_countSymbols",
	"udat_format", "udat_getSymbols", "udat_open", "udat_setCalendar", "udat_toPattern", "udatpg_close", "udatpg_getBestPattern",
	"udatpg_open", "uenum_close", "uenum_count", "uenum_next", "uidna_close", "uidna_nameToASCII", "uidna_nameToUnicode",
	"uidna_openUTS46", "uloc_canonicalize", "uloc_countAvailable", "uloc_getAvailable", "uloc_getBaseName", "uloc_getCharacterOrientation",
	"uloc_getCountry", "uloc_getDefault", "uloc_getDisplayCountry", "uloc_getDisplayLanguage", "uloc_getDisplayName", "uloc_getISO3Country",
	"uloc_getISO3Language", "uloc_getKeywordValue", "uloc_getLanguage", "uloc_getLCID", "uloc_getName", "uloc_getParent",
	"uloc_setKeywordValue", "ulocdata_getCLDRVersion", "ulocdata_getMeasurementSystem", "unorm2_getNFCInstance", "unorm2_getNFDInstance",
	"unorm2_getNFKCInstance", "unorm2_getNFKDInstance", "unorm2_isNormalized", "unorm2_normalize", "unum_close", "unum_getAttribute",
	"unum_getSymbol", "unum_open", "unum_toPattern", "ures_close", "ures_getByKey", "ures_getSize", "ures_getStringByIndex", "ures_open",
	"usearch_close", "usearch_first", "usearch_getBreakIterator", "usearch_getMatchedLength", "usearch_last", "usearch_openFromCollator",
	"usearch_setPattern", "usearch_setText", "ucurr_forLocale", "ucurr_getName", "uldn_close", "uldn_keyValueDisplayName", "uldn_open",
	"ucol_clone",
};

typedef int (__cdecl *name_function)(const char *, char *, int, int *);
typedef void *(__cdecl *bundle_open_function)(const char *, const char *, int *);
typedef void (__cdecl *close_function)(void *);
typedef void *(__cdecl *date_open_function)(int, int, const char *, const WCHAR *, int, const WCHAR *, int, int *);
typedef int (__cdecl *pattern_function)(const void *, signed char, WCHAR *, int, int *);
typedef void *(__cdecl *number_open_function)(int, const WCHAR *, int, const char *, void *, int *);
typedef int (__cdecl *symbol_function)(const void *, int, WCHAR *, int, int *);
typedef void *(__cdecl *collator_open_function)(const char *, int *);
typedef void (__cdecl *attribute_function)(void *, int, int, int *);
typedef int (__cdecl *collate_function)(const void *, const WCHAR *, int, const WCHAR *, int);
typedef int (__cdecl *sort_key_function)(const void *, const WCHAR *, int, unsigned char *, int);
typedef void *(__cdecl *search_open_function)(const WCHAR *, int, const WCHAR *, int, const void *, void *, int *);
typedef int (__cdecl *search_function)(void *, int *);
typedef int (__cdecl *matched_function)(const void *);
typedef void *(__cdecl *elements_open_function)(const void *, const WCHAR *, int, int *);
typedef int (__cdecl *element_function)(void *, int *);
typedef const void *(__cdecl *normalizer_function)(int *);
typedef int (__cdecl *normalize_function)(const void *, const WCHAR *, int, WCHAR *, int, int *);
typedef void *(__cdecl *idna_open_function)(unsigned, int *);
typedef int (__cdecl *idna_function)(const void *, const WCHAR *, int, WCHAR *, int, void *, int *);
typedef void *(__cdecl *calendar_open_function)(const WCHAR *, int, const char *, int, int *);
typedef void (__cdecl *millis_function)(void *, double, int *);
typedef int (__cdecl *field_function)(const void *, int, int *);
typedef int (__cdecl *format_function)(const void *, double, WCHAR *, int, void *, int *);
typedef int (__cdecl *zone_function)(const WCHAR *, int, const char *, WCHAR *, int, int *);

typedef struct {
	short size;
	signed char transitional_different, reserved;
	unsigned errors;
	int reserved_2, reserved_3;
} idna_info;

static int same(const WCHAR *text, const WCHAR *expected)
{
	for (; *expected; text++, expected++)
		if (*text != *expected)
			return 0;
	return !*text;
}

#define LOAD(type, name) type name = (type)GetProcAddress(icu, #name)

int raven_entry(void)
{
	// .NET looks for icu.dll among the system's own libraries only.
	HMODULE icu = LoadLibraryExW(L"icu.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!icu)
		return 1;
	for (unsigned index = 0; index < sizeof required / sizeof *required; index++)
		if (!GetProcAddress(icu, required[index]))
			return 2;

	char name[64];
	int status = 0;
	LOAD(name_function, uloc_getName);
	LOAD(name_function, uloc_getLanguage);
	if (uloc_getName("en-us", name, sizeof name, &status) != 5 || status > 0 || lstrcmpA(name, "en_US"))
		return 3;
	if (uloc_getLanguage("de_AT@collation=phonebook", name, sizeof name, &status) != 2 || lstrcmpA(name, "de"))
		return 4;

	// A culture is predefined when its own data is found, with no warning of a fallback.
	LOAD(bundle_open_function, ures_open);
	LOAD(close_function, ures_close);
	status = 0;
	void *bundle = ures_open(NULL, "en_US", &status);
	if (!bundle || status != 0)
		return 5;
	ures_close(bundle);
	status = 0;
	bundle = ures_open(NULL, "xx_YY", &status);
	if (!bundle || status == 0)
		return 6;
	ures_close(bundle);

	WCHAR text[96];
	LOAD(date_open_function, udat_open);
	LOAD(pattern_function, udat_toPattern);
	LOAD(close_function, udat_close);
	status = 0;
	void *date = udat_open(-1, 3, "en_US", NULL, 0, NULL, 0, &status);
	if (!date || udat_toPattern(date, 0, text, 96, &status) != 6 || !same(text, L"M/d/yy"))
		return 7;
	udat_close(date);

	LOAD(number_open_function, unum_open);
	LOAD(symbol_function, unum_getSymbol);
	LOAD(pattern_function, unum_toPattern);
	LOAD(close_function, unum_close);
	status = 0;
	void *money = unum_open(2, NULL, 0, "en_US", NULL, &status);
	if (!money || unum_getSymbol(money, 8, text, 96, &status) != 1 || text[0] != '$')
		return 8;
	if (unum_toPattern(money, 0, text, 96, &status) <= 0 || text[0] != 0xa4)
		return 9;
	unum_close(money);

	LOAD(collator_open_function, ucol_open);
	LOAD(attribute_function, ucol_setAttribute);
	LOAD(collate_function, ucol_strcoll);
	LOAD(sort_key_function, ucol_getSortKey);
	LOAD(close_function, ucol_close);
	status = 0;
	void *collator = ucol_open("en_US", &status);
	if (!collator || ucol_strcoll(collator, L"apple", -1, L"banana", -1) >= 0 || !ucol_strcoll(collator, L"abc", -1, L"ABC", -1))
		return 10;
	// Secondary strength ignores case, as CompareOptions.IgnoreCase sets it.
	ucol_setAttribute(collator, 5, 1, &status);
	if (status > 0 || ucol_strcoll(collator, L"abc", -1, L"ABC", -1))
		return 11;
	unsigned char lower[64], upper[64];
	int lower_length = ucol_getSortKey(collator, L"abc", -1, lower, sizeof lower);
	int upper_length = ucol_getSortKey(collator, L"ABC", -1, upper, sizeof upper);
	if (lower_length <= 1 || lower_length != upper_length || lower[lower_length - 1])
		return 12;
	for (int index = 0; index < lower_length; index++)
		if (lower[index] != upper[index])
			return 12;

	LOAD(search_open_function, usearch_openFromCollator);
	LOAD(search_function, usearch_first);
	LOAD(search_function, usearch_last);
	LOAD(matched_function, usearch_getMatchedLength);
	LOAD(close_function, usearch_close);
	status = 0;
	void *search = usearch_openFromCollator(L"LO", 2, L"hello, hello", 12, collator, NULL, &status);
	if (!search || usearch_first(search, &status) != 3 || usearch_getMatchedLength(search) != 2 || usearch_last(search, &status) != 10)
		return 13;
	usearch_close(search);
	ucol_close(collator);

	// A combining mark weighs at the secondary level only; a fresh iterator walks back from the end.
	LOAD(elements_open_function, ucol_openElements);
	LOAD(element_function, ucol_next);
	LOAD(element_function, ucol_previous);
	LOAD(close_function, ucol_closeElements);
	status = 0;
	collator = ucol_open("", &status);
	void *elements = ucol_openElements(collator, L"o\x0308", 2, &status);
	int base = ucol_next(elements, &status), mark = ucol_next(elements, &status);
	if (!(base & 0xffff0000) || (mark & 0xffff0000) || !(mark & 0xff00) || ucol_next(elements, &status) != -1)
		return 14;
	ucol_closeElements(elements);
	elements = ucol_openElements(collator, L"ab", 2, &status);
	if (ucol_previous(elements, &status) != ucol_next(ucol_openElements(collator, L"b", 1, &status), &status))
		return 15;
	ucol_closeElements(elements);
	ucol_close(collator);

	LOAD(normalizer_function, unorm2_getNFCInstance);
	LOAD(normalize_function, unorm2_normalize);
	status = 0;
	if (unorm2_normalize(unorm2_getNFCInstance(&status), L"e\x0301", 2, text, 96, &status) != 1 || text[0] != 0xe9)
		return 16;

	LOAD(idna_open_function, uidna_openUTS46);
	LOAD(idna_function, uidna_nameToASCII);
	LOAD(close_function, uidna_close);
	status = 0;
	void *idna = uidna_openUTS46(0x30, &status);
	idna_info info = {sizeof info};
	int length = uidna_nameToASCII(idna, L"b\xfc" L"cher.de", -1, text, 96, &info, &status);
	if (status > 0 || info.errors || length != 16 || !same(text, L"xn--bcher-kva.de"))
		return 17;
	uidna_close(idna);

	// The epoch fell on Thursday 1 January 1970.
	LOAD(calendar_open_function, ucal_open);
	LOAD(millis_function, ucal_setMillis);
	LOAD(field_function, ucal_get);
	LOAD(close_function, ucal_close);
	status = 0;
	void *calendar = ucal_open(L"UTC", -1, "en_US@calendar=gregorian", 1, &status);
	if (!calendar)
		return 18;
	ucal_setMillis(calendar, 0.0, &status);
	if (ucal_get(calendar, 1, &status) != 1970 || ucal_get(calendar, 2, &status) || ucal_get(calendar, 5, &status) != 1 ||
		ucal_get(calendar, 7, &status) != 5 || status > 0)
		return 19;
	ucal_close(calendar);
	status = 0;
	if (ucal_open(NULL, 0, "ja_JP@calendar=japanese", 1, &status) && status <= 0)
		return 20;

	LOAD(format_function, udat_format);
	status = 0;
	date = udat_open(-2, -2, "en_US", L"UTC", -1, L"EEEE, yyyy-MM-dd HH:mm 'h'", -1, &status);
	if (!date || udat_format(date, 86400000.0 * 365 + 3600000.0 * 13, text, 96, NULL, &status) <= 0 ||
		!same(text, L"Friday, 1971-01-01 13:00 h"))
		return 21;
	udat_close(date);

	LOAD(zone_function, ucal_getTimeZoneIDForWindowsID);
	status = 0;
	if (ucal_getTimeZoneIDForWindowsID(L"W. Europe Standard Time", -1, "IT", text, 96, &status) != 11 || !same(text, L"Europe/Rome"))
		return 22;
	return 0;
}
