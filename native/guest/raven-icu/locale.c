/* SPDX-License-Identifier: MIT */
#include "internal.h"

/* Locales, resource bundles, number and date formats and the Gregorian calendar, as ICU's C interface gives them. The data is
 * English, the one language the system describes: other locales read their names and codes from the tables here and fall back to
 * English formats with ICU's warning that a default was used. */

#define ULOC_FULLNAME_CAPACITY 157

const char *__cdecl uloc_getDefault(void);
UChar *__cdecl u_uastrncpy(UChar *dest, const char *source, INT count);

static INT finish_chars(char *dest, INT capacity, const char *source, INT length, UErrorCode *status)
{
	if (U_FAILED(*status))
		return length;
	if (dest && capacity > 0)
		CopyMemory(dest, source, length < capacity ? length : capacity);
	if (length < capacity)
		dest[length] = 0;
	return icu_finish(length, capacity, status);
}

static INT finish_units(UChar *dest, INT capacity, const UChar *source, INT length, UErrorCode *status)
{
	if (U_FAILED(*status))
		return length;
	if (dest && capacity > 0)
		CopyMemory(dest, source, (length < capacity ? length : capacity) * sizeof(UChar));
	if (length < capacity)
		dest[length] = 0;
	return icu_finish(length, capacity, status);
}

static INT finish_ascii(UChar *dest, INT capacity, const char *source, UErrorCode *status)
{
	INT length = lstrlenA(source);
	if (U_FAILED(*status))
		return length;
	for (INT at = 0; dest && at < length && at < capacity; at++)
		dest[at] = (UChar)(BYTE)source[at];
	if (length < capacity)
		dest[length] = 0;
	return icu_finish(length, capacity, status);
}

static char lower_ascii(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static char upper_ascii(char c)
{
	return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

static BOOL same_ascii(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if (lower_ascii(*a) != lower_ascii(*b))
			return FALSE;
	return !*a && !*b;
}

static BOOL starts_ascii(const char *text, const char *prefix)
{
	for (; *prefix; text++, prefix++)
		if (lower_ascii(*text) != lower_ascii(*prefix))
			return FALSE;
	return TRUE;
}

/* A locale ID split as ICU reads it: language, script, country, variant and keywords, with '-' taken for '_'. */
typedef struct {
	char language[12];
	char script[8];
	char country[8];
	char variant[32];
	char keywords[96];
} locale_parts;

static void copy_part(char *dest, INT capacity, const char *from, INT length)
{
	INT at = 0;
	for (; at < length && at + 1 < capacity; at++)
		dest[at] = from[at];
	dest[at] = 0;
}

static BOOL all_alpha(const char *text, INT length)
{
	for (INT at = 0; at < length; at++)
		if (!((text[at] >= 'a' && text[at] <= 'z') || (text[at] >= 'A' && text[at] <= 'Z')))
			return FALSE;
	return TRUE;
}

static BOOL all_digit(const char *text, INT length)
{
	for (INT at = 0; at < length; at++)
		if (text[at] < '0' || text[at] > '9')
			return FALSE;
	return TRUE;
}

static void split_locale(const char *name, locale_parts *parts)
{
	ZeroMemory(parts, sizeof *parts);
	if (!name)
		name = uloc_getDefault();
	const char *at = name, *keywords = name;
	while (*keywords && *keywords != '@')
		keywords++;
	if (*keywords == '@')
		copy_part(parts->keywords, sizeof parts->keywords, keywords + 1, lstrlenA(keywords + 1));
	INT field = 0;
	while (at < keywords) {
		const char *end = at;
		while (end < keywords && *end != '_' && *end != '-')
			end++;
		INT length = (INT)(end - at);
		if (field == 0) {
			copy_part(parts->language, sizeof parts->language, at, length);
			for (char *c = parts->language; *c; c++)
				*c = lower_ascii(*c);
			field = 1;
		} else if (field == 1 && length == 4 && all_alpha(at, length)) {
			copy_part(parts->script, sizeof parts->script, at, length);
			parts->script[0] = upper_ascii(parts->script[0]);
			for (char *c = parts->script + 1; *c; c++)
				*c = lower_ascii(*c);
			field = 2;
		} else if (field <= 2 && ((length == 2 && all_alpha(at, length)) || (length == 3 && all_digit(at, length)))) {
			copy_part(parts->country, sizeof parts->country, at, length);
			for (char *c = parts->country; *c; c++)
				*c = upper_ascii(*c);
			field = 3;
		} else if (length) {
			if (parts->variant[0])
				lstrcatA(parts->variant, "_");
			INT start = lstrlenA(parts->variant);
			copy_part(parts->variant + start, (INT)sizeof parts->variant - start, at, length);
			for (char *c = parts->variant + start; *c; c++)
				*c = upper_ascii(*c);
			field = 4;
		}
		at = end < keywords ? end + 1 : end;
	}
}

static INT join_locale(const locale_parts *parts, BOOL keywords, char *out)
{
	out[0] = 0;
	lstrcatA(out, parts->language);
	if (parts->script[0]) {
		lstrcatA(out, "_");
		lstrcatA(out, parts->script);
	}
	if (parts->country[0] || parts->variant[0]) {
		lstrcatA(out, "_");
		lstrcatA(out, parts->country);
	}
	if (parts->variant[0]) {
		lstrcatA(out, "_");
		lstrcatA(out, parts->variant);
	}
	if (keywords && parts->keywords[0]) {
		lstrcatA(out, "@");
		lstrcatA(out, parts->keywords);
	}
	return lstrlenA(out);
}

const char *__cdecl uloc_getDefault(void)
{
	static char name[ULOC_FULLNAME_CAPACITY];
	if (!name[0]) {
		WCHAR wide[LOCALE_NAME_MAX_LENGTH];
		INT length = GetUserDefaultLocaleName(wide, LOCALE_NAME_MAX_LENGTH);
		INT at = 0;
		for (; at + 1 < length && at + 1 < ULOC_FULLNAME_CAPACITY && wide[at]; at++)
			name[at] = wide[at] == '-' ? '_' : (char)wide[at];
		if (!at)
			lstrcpyA(name, "en_US");
		else
			name[at] = 0;
	}
	return name;
}

INT __cdecl uloc_getName(const char *locale, char *name, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	locale_parts parts;
	split_locale(locale, &parts);
	char out[ULOC_FULLNAME_CAPACITY + 96];
	INT length = join_locale(&parts, TRUE, out);
	return finish_chars(name, capacity, out, length, status);
}

INT __cdecl uloc_canonicalize(const char *locale, char *name, INT capacity, UErrorCode *status)
{
	return uloc_getName(locale, name, capacity, status);
}

INT __cdecl uloc_getBaseName(const char *locale, char *name, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	locale_parts parts;
	split_locale(locale, &parts);
	char out[ULOC_FULLNAME_CAPACITY];
	INT length = join_locale(&parts, FALSE, out);
	return finish_chars(name, capacity, out, length, status);
}

INT __cdecl uloc_getLanguage(const char *locale, char *language, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	locale_parts parts;
	split_locale(locale, &parts);
	return finish_chars(language, capacity, parts.language, lstrlenA(parts.language), status);
}

INT __cdecl uloc_getCountry(const char *locale, char *country, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	locale_parts parts;
	split_locale(locale, &parts);
	return finish_chars(country, capacity, parts.country, lstrlenA(parts.country), status);
}

// The parent drops the last field of the base name; a language alone has the root, "", as its parent.
INT __cdecl uloc_getParent(const char *locale, char *parent, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	char base[ULOC_FULLNAME_CAPACITY];
	UErrorCode inner = U_ZERO_ERROR;
	uloc_getBaseName(locale, base, sizeof base, &inner);
	INT length = lstrlenA(base);
	while (length > 0 && base[length - 1] != '_')
		length--;
	if (length > 0)
		length--;
	return finish_chars(parent, capacity, base, length, status);
}

static const char *keyword_value(const char *keywords, const char *keyword, INT *length)
{
	const char *at = keywords;
	INT name_length = lstrlenA(keyword);
	while (*at) {
		const char *end = at;
		while (*end && *end != ';')
			end++;
		const char *equals = at;
		while (equals < end && *equals != '=')
			equals++;
		if (equals < end && equals - at == name_length) {
			BOOL same = TRUE;
			for (INT index = 0; index < name_length; index++)
				same = same && lower_ascii(at[index]) == lower_ascii(keyword[index]);
			if (same) {
				*length = (INT)(end - equals - 1);
				return equals + 1;
			}
		}
		at = *end ? end + 1 : end;
	}
	return NULL;
}

INT __cdecl uloc_getKeywordValue(const char *locale, const char *keyword, char *buffer, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	locale_parts parts;
	split_locale(locale, &parts);
	INT length = 0;
	const char *value = keyword_value(parts.keywords, keyword, &length);
	if (!value) {
		if (buffer && capacity > 0)
			buffer[0] = 0;
		return 0;
	}
	char copy[96];
	copy_part(copy, sizeof copy, value, length);
	for (char *c = copy; *c; c++)
		*c = lower_ascii(*c);
	return finish_chars(buffer, capacity, copy, lstrlenA(copy), status);
}

// The keyword is set, or replaced, in the locale ID the buffer holds; keywords stay in alphabetical order, as ICU keeps them.
INT __cdecl uloc_setKeywordValue(const char *keyword, const char *value, char *buffer, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	locale_parts parts;
	split_locale(buffer, &parts);
	struct { char name[32]; char value[64]; } pairs[8];
	INT count = 0;
	const char *at = parts.keywords;
	while (*at && count < 8) {
		const char *end = at, *equals = at;
		while (*end && *end != ';')
			end++;
		while (equals < end && *equals != '=')
			equals++;
		if (equals < end) {
			copy_part(pairs[count].name, sizeof pairs[count].name, at, (INT)(equals - at));
			copy_part(pairs[count].value, sizeof pairs[count].value, equals + 1, (INT)(end - equals - 1));
			if (!same_ascii(pairs[count].name, keyword))
				count++;
		}
		at = *end ? end + 1 : end;
	}
	if (value && value[0] && count < 8) {
		copy_part(pairs[count].name, sizeof pairs[count].name, keyword, lstrlenA(keyword));
		copy_part(pairs[count].value, sizeof pairs[count].value, value, lstrlenA(value));
		for (char *c = pairs[count].name; *c; c++)
			*c = lower_ascii(*c);
		count++;
	}
	for (INT i = 1; i < count; i++)
		for (INT j = i; j > 0 && lstrcmpA(pairs[j - 1].name, pairs[j].name) > 0; j--) {
			char swap[96];
			CopyMemory(swap, &pairs[j], sizeof pairs[j]);
			CopyMemory(&pairs[j], &pairs[j - 1], sizeof pairs[j]);
			CopyMemory(&pairs[j - 1], swap, sizeof pairs[j]);
		}
	parts.keywords[0] = 0;
	for (INT index = 0; index < count; index++) {
		if (index)
			lstrcatA(parts.keywords, ";");
		lstrcatA(parts.keywords, pairs[index].name);
		lstrcatA(parts.keywords, "=");
		lstrcatA(parts.keywords, pairs[index].value);
	}
	char out[ULOC_FULLNAME_CAPACITY + 96];
	INT length = join_locale(&parts, TRUE, out);
	if (length >= capacity) {
		*status = U_BUFFER_OVERFLOW_ERROR;
		return length;
	}
	lstrcpyA(buffer, out);
	return length;
}

/* Names and codes of languages and countries, in English. */

static const struct { const char *code, *code3, *name; } languages[] = {
	{"af", "afr", "Afrikaans"}, {"ar", "ara", "Arabic"}, {"bg", "bul", "Bulgarian"}, {"bn", "ben", "Bangla"}, {"ca", "cat", "Catalan"},
	{"cs", "ces", "Czech"}, {"cy", "cym", "Welsh"}, {"da", "dan", "Danish"}, {"de", "deu", "German"}, {"el", "ell", "Greek"},
	{"en", "eng", "English"}, {"es", "spa", "Spanish"}, {"et", "est", "Estonian"}, {"eu", "eus", "Basque"}, {"fa", "fas", "Persian"},
	{"fi", "fin", "Finnish"}, {"fr", "fra", "French"}, {"ga", "gle", "Irish"}, {"gl", "glg", "Galician"}, {"he", "heb", "Hebrew"},
	{"hi", "hin", "Hindi"}, {"hr", "hrv", "Croatian"}, {"hu", "hun", "Hungarian"}, {"hy", "hye", "Armenian"}, {"id", "ind", "Indonesian"},
	{"is", "isl", "Icelandic"}, {"it", "ita", "Italian"}, {"ja", "jpn", "Japanese"}, {"ka", "kat", "Georgian"}, {"kk", "kaz", "Kazakh"},
	{"ko", "kor", "Korean"}, {"lt", "lit", "Lithuanian"}, {"lv", "lav", "Latvian"}, {"mk", "mkd", "Macedonian"}, {"ms", "msa", "Malay"},
	{"mt", "mlt", "Maltese"}, {"nb", "nob", "Norwegian Bokmål"}, {"nl", "nld", "Dutch"}, {"nn", "nno", "Norwegian Nynorsk"},
	{"pl", "pol", "Polish"}, {"pt", "por", "Portuguese"}, {"ro", "ron", "Romanian"}, {"ru", "rus", "Russian"}, {"sk", "slk", "Slovak"},
	{"sl", "slv", "Slovenian"}, {"sq", "sqi", "Albanian"}, {"sr", "srp", "Serbian"}, {"sv", "swe", "Swedish"}, {"sw", "swa", "Swahili"},
	{"ta", "tam", "Tamil"}, {"th", "tha", "Thai"}, {"tr", "tur", "Turkish"}, {"uk", "ukr", "Ukrainian"}, {"ur", "urd", "Urdu"},
	{"vi", "vie", "Vietnamese"}, {"zh", "zho", "Chinese"},
};

static const struct { const char *code, *code3, *name, *currency; BOOL us_measure; BOOL sunday; BOOL first_week_full; } countries[] = {
	{"AE", "ARE", "United Arab Emirates", "AED", 0, 0, 0}, {"AR", "ARG", "Argentina", "ARS", 0, 0, 0}, {"AT", "AUT", "Austria", "EUR", 0, 0, 1},
	{"AU", "AUS", "Australia", "AUD", 0, 0, 0}, {"BE", "BEL", "Belgium", "EUR", 0, 0, 1}, {"BG", "BGR", "Bulgaria", "BGN", 0, 0, 1},
	{"BR", "BRA", "Brazil", "BRL", 0, 1, 0}, {"CA", "CAN", "Canada", "CAD", 0, 1, 0}, {"CH", "CHE", "Switzerland", "CHF", 0, 0, 1},
	{"CL", "CHL", "Chile", "CLP", 0, 0, 0}, {"CN", "CHN", "China", "CNY", 0, 0, 0}, {"CO", "COL", "Colombia", "COP", 0, 1, 0},
	{"CZ", "CZE", "Czechia", "CZK", 0, 0, 1}, {"DE", "DEU", "Germany", "EUR", 0, 0, 1}, {"DK", "DNK", "Denmark", "DKK", 0, 0, 1},
	{"EE", "EST", "Estonia", "EUR", 0, 0, 1}, {"EG", "EGY", "Egypt", "EGP", 0, 0, 0}, {"ES", "ESP", "Spain", "EUR", 0, 0, 1},
	{"FI", "FIN", "Finland", "EUR", 0, 0, 1}, {"FR", "FRA", "France", "EUR", 0, 0, 1}, {"GB", "GBR", "United Kingdom", "GBP", 0, 0, 1},
	{"GR", "GRC", "Greece", "EUR", 0, 0, 1}, {"HK", "HKG", "Hong Kong SAR China", "HKD", 0, 1, 0}, {"HR", "HRV", "Croatia", "EUR", 0, 0, 0},
	{"HU", "HUN", "Hungary", "HUF", 0, 0, 1}, {"ID", "IDN", "Indonesia", "IDR", 0, 1, 0}, {"IE", "IRL", "Ireland", "EUR", 0, 0, 1},
	{"IL", "ISR", "Israel", "ILS", 0, 1, 0}, {"IN", "IND", "India", "INR", 0, 1, 0}, {"IS", "ISL", "Iceland", "ISK", 0, 0, 1},
	{"IT", "ITA", "Italy", "EUR", 0, 0, 1}, {"JP", "JPN", "Japan", "JPY", 0, 1, 0}, {"KR", "KOR", "South Korea", "KRW", 0, 1, 0},
	{"LR", "LBR", "Liberia", "LRD", 1, 0, 0}, {"LT", "LTU", "Lithuania", "EUR", 0, 0, 1}, {"LU", "LUX", "Luxembourg", "EUR", 0, 0, 1},
	{"LV", "LVA", "Latvia", "EUR", 0, 0, 1}, {"MM", "MMR", "Myanmar (Burma)", "MMK", 1, 1, 0}, {"MX", "MEX", "Mexico", "MXN", 0, 1, 0},
	{"MY", "MYS", "Malaysia", "MYR", 0, 0, 0}, {"NL", "NLD", "Netherlands", "EUR", 0, 0, 1}, {"NO", "NOR", "Norway", "NOK", 0, 0, 1},
	{"NZ", "NZL", "New Zealand", "NZD", 0, 0, 0}, {"PH", "PHL", "Philippines", "PHP", 0, 1, 0}, {"PL", "POL", "Poland", "PLN", 0, 0, 1},
	{"PT", "PRT", "Portugal", "EUR", 0, 1, 1}, {"RO", "ROU", "Romania", "RON", 0, 0, 0}, {"RS", "SRB", "Serbia", "RSD", 0, 0, 0},
	{"RU", "RUS", "Russia", "RUB", 0, 0, 1}, {"SA", "SAU", "Saudi Arabia", "SAR", 0, 1, 0}, {"SE", "SWE", "Sweden", "SEK", 0, 0, 1},
	{"SG", "SGP", "Singapore", "SGD", 0, 1, 0}, {"SI", "SVN", "Slovenia", "EUR", 0, 0, 0}, {"SK", "SVK", "Slovakia", "EUR", 0, 0, 1},
	{"TH", "THA", "Thailand", "THB", 0, 1, 0}, {"TR", "TUR", "Türkiye", "TRY", 0, 0, 0}, {"TW", "TWN", "Taiwan", "TWD", 0, 1, 0},
	{"UA", "UKR", "Ukraine", "UAH", 0, 0, 0}, {"US", "USA", "United States", "USD", 1, 1, 0}, {"VN", "VNM", "Vietnam", "VND", 0, 0, 0},
	{"ZA", "ZAF", "South Africa", "ZAR", 0, 1, 0},
};

static const struct { const char *code, *name, *symbol; INT digits; } currencies[] = {
	{"AED", "UAE Dirham", "AED", 2}, {"ARS", "Argentine Peso", "ARS", 2}, {"AUD", "Australian Dollar", "A$", 2}, {"BGN", "Bulgarian Lev", "BGN", 2},
	{"BRL", "Brazilian Real", "R$", 2}, {"CAD", "Canadian Dollar", "CA$", 2}, {"CHF", "Swiss Franc", "CHF", 2}, {"CLP", "Chilean Peso", "CLP", 0},
	{"CNY", "Chinese Yuan", "CN\xc2\xa5", 2}, {"COP", "Colombian Peso", "COP", 2}, {"CZK", "Czech Koruna", "CZK", 2},
	{"DKK", "Danish Krone", "DKK", 2}, {"EGP", "Egyptian Pound", "EGP", 2}, {"EUR", "Euro", "\xe2\x82\xac", 2},
	{"GBP", "British Pound", "\xc2\xa3", 2}, {"HKD", "Hong Kong Dollar", "HK$", 2}, {"HUF", "Hungarian Forint", "HUF", 2},
	{"IDR", "Indonesian Rupiah", "IDR", 2}, {"ILS", "Israeli New Shekel", "\xe2\x82\xaa", 2}, {"INR", "Indian Rupee", "\xe2\x82\xb9", 2},
	{"ISK", "Icelandic Kr\xc3\xb3na", "ISK", 0}, {"JPY", "Japanese Yen", "\xc2\xa5", 0}, {"KRW", "South Korean Won", "\xe2\x82\xa9", 0},
	{"LRD", "Liberian Dollar", "LRD", 2}, {"MMK", "Myanmar Kyat", "MMK", 0}, {"MXN", "Mexican Peso", "MX$", 2},
	{"MYR", "Malaysian Ringgit", "MYR", 2}, {"NOK", "Norwegian Krone", "NOK", 2}, {"NZD", "New Zealand Dollar", "NZ$", 2},
	{"PHP", "Philippine Peso", "\xe2\x82\xb1", 2}, {"PLN", "Polish Zloty", "PLN", 2}, {"RON", "Romanian Leu", "RON", 2},
	{"RSD", "Serbian Dinar", "RSD", 0}, {"RUB", "Russian Ruble", "RUB", 2}, {"SAR", "Saudi Riyal", "SAR", 2}, {"SEK", "Swedish Krona", "SEK", 2},
	{"SGD", "Singapore Dollar", "SGD", 2}, {"THB", "Thai Baht", "THB", 2}, {"TRY", "Turkish Lira", "TRY", 2},
	{"TWD", "New Taiwan Dollar", "NT$", 2}, {"UAH", "Ukrainian Hryvnia", "UAH", 2}, {"USD", "US Dollar", "$", 2},
	{"VND", "Vietnamese Dong", "\xe2\x82\xab", 0}, {"ZAR", "South African Rand", "ZAR", 2},
};

// The country a language is most likely written in, for a locale that names none.
static const struct { const char *language, *country; } likely[] = {
	{"ar", "EG"}, {"cs", "CZ"}, {"da", "DK"}, {"de", "DE"}, {"el", "GR"}, {"en", "US"}, {"es", "ES"}, {"fi", "FI"}, {"fr", "FR"},
	{"he", "IL"}, {"hi", "IN"}, {"hu", "HU"}, {"it", "IT"}, {"ja", "JP"}, {"ko", "KR"}, {"nb", "NO"}, {"nl", "NL"}, {"pl", "PL"},
	{"pt", "BR"}, {"ro", "RO"}, {"ru", "RU"}, {"sv", "SE"}, {"th", "TH"}, {"tr", "TR"}, {"uk", "UA"}, {"vi", "VN"}, {"zh", "CN"},
};

static INT find_language(const char *code)
{
	for (INT index = 0; index < (INT)ARRAYSIZE(languages); index++)
		if (same_ascii(languages[index].code, code))
			return index;
	return -1;
}

static INT find_country(const char *code)
{
	for (INT index = 0; index < (INT)ARRAYSIZE(countries); index++)
		if (same_ascii(countries[index].code, code))
			return index;
	return -1;
}

static INT find_currency(const char *code)
{
	for (INT index = 0; index < (INT)ARRAYSIZE(currencies); index++)
		if (same_ascii(currencies[index].code, code))
			return index;
	return -1;
}

// The country of a locale, or the one its language is most likely spoken in.
static INT region_of(const char *locale)
{
	locale_parts parts;
	split_locale(locale, &parts);
	if (parts.country[0])
		return find_country(parts.country);
	for (INT index = 0; index < (INT)ARRAYSIZE(likely); index++)
		if (same_ascii(likely[index].language, parts.language))
			return find_country(likely[index].country);
	return -1;
}

BOOL icu_known_locale(const char *name)
{
	locale_parts parts;
	split_locale(name, &parts);
	return !parts.language[0] || same_ascii(parts.language, "root") || (same_ascii(parts.language, "en") && (!parts.country[0] ||
		same_ascii(parts.country, "US")) && !parts.script[0] && !parts.variant[0]);
}

const char *__cdecl uloc_getISO3Language(const char *locale)
{
	locale_parts parts;
	split_locale(locale, &parts);
	INT index = find_language(parts.language);
	return index < 0 ? "" : languages[index].code3;
}

const char *__cdecl uloc_getISO3Country(const char *locale)
{
	locale_parts parts;
	split_locale(locale, &parts);
	INT index = find_country(parts.country);
	return index < 0 ? "" : countries[index].code3;
}

UINT __cdecl uloc_getLCID(const char *locale)
{
	char base[ULOC_FULLNAME_CAPACITY];
	UErrorCode status = U_ZERO_ERROR;
	uloc_getBaseName(locale, base, sizeof base, &status);
	WCHAR wide[LOCALE_NAME_MAX_LENGTH];
	INT at = 0;
	for (; base[at] && at + 1 < LOCALE_NAME_MAX_LENGTH; at++)
		wide[at] = base[at] == '_' ? '-' : base[at];
	wide[at] = 0;
	return at ? LocaleNameToLCID(wide, 0) : 0;
}

enum { ULOC_LAYOUT_LTR = 0, ULOC_LAYOUT_RTL = 1 };

INT __cdecl uloc_getCharacterOrientation(const char *locale, UErrorCode *status)
{
	static const char *right_to_left[] = {"ar", "ckb", "dv", "fa", "he", "ks", "ps", "sd", "ug", "ur", "yi"};
	if (U_FAILED(*status))
		return ULOC_LAYOUT_LTR;
	locale_parts parts;
	split_locale(locale, &parts);
	if (same_ascii(parts.script, "Arab") || same_ascii(parts.script, "Hebr"))
		return ULOC_LAYOUT_RTL;
	if (parts.script[0])
		return ULOC_LAYOUT_LTR;
	for (INT index = 0; index < (INT)ARRAYSIZE(right_to_left); index++)
		if (same_ascii(right_to_left[index], parts.language))
			return ULOC_LAYOUT_RTL;
	return ULOC_LAYOUT_LTR;
}

static const char *const available[] = {"en", "en_US"};

INT __cdecl uloc_countAvailable(void)
{
	return ARRAYSIZE(available);
}

const char *__cdecl uloc_getAvailable(INT index)
{
	return index >= 0 && index < (INT)ARRAYSIZE(available) ? available[index] : NULL;
}

// Names are given in English. Asked in another language, the English name comes with U_USING_DEFAULT_WARNING, as ICU warns when it
// fell back to the root; a code with no name is given as itself, with the same warning.
static void display_warning(const char *display, BOOL named, UErrorCode *status)
{
	locale_parts parts;
	split_locale(display ? display : uloc_getDefault(), &parts);
	if (!named || (parts.language[0] && !same_ascii(parts.language, "en")))
		*status = U_USING_DEFAULT_WARNING;
}

static INT utf8_to_units(const char *text, UChar *out, INT capacity)
{
	return MultiByteToWideChar(CP_UTF8, 0, text, -1, out, capacity) - 1;
}

INT __cdecl uloc_getDisplayLanguage(const char *locale, const char *display, UChar *dest, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	locale_parts parts;
	split_locale(locale, &parts);
	INT index = find_language(parts.language);
	UChar name[64];
	INT length = utf8_to_units(index < 0 ? parts.language : languages[index].name, name, ARRAYSIZE(name));
	display_warning(display, index >= 0, status);
	return finish_units(dest, capacity, name, length, status);
}

INT __cdecl uloc_getDisplayCountry(const char *locale, const char *display, UChar *dest, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	locale_parts parts;
	split_locale(locale, &parts);
	INT index = find_country(parts.country);
	UChar name[64];
	INT length = utf8_to_units(index < 0 ? parts.country : countries[index].name, name, ARRAYSIZE(name));
	display_warning(display, index >= 0 || !parts.country[0], status);
	return finish_units(dest, capacity, name, length, status);
}

// "Language (Country)", or the language alone.
INT __cdecl uloc_getDisplayName(const char *locale, const char *display, UChar *dest, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	UChar name[160];
	UErrorCode language_status = U_ZERO_ERROR, country_status = U_ZERO_ERROR;
	INT length = uloc_getDisplayLanguage(locale, display, name, 64, &language_status);
	locale_parts parts;
	split_locale(locale, &parts);
	if (parts.country[0]) {
		name[length++] = ' ';
		name[length++] = '(';
		length += uloc_getDisplayCountry(locale, display, name + length, 64, &country_status);
		name[length++] = ')';
	}
	if (language_status == U_USING_DEFAULT_WARNING || country_status == U_USING_DEFAULT_WARNING)
		*status = U_USING_DEFAULT_WARNING;
	return finish_units(dest, capacity, name, length, status);
}

/* Locale data. */

enum { UMS_SI = 0, UMS_US = 1 };

INT __cdecl ulocdata_getMeasurementSystem(const char *locale, UErrorCode *status)
{
	if (U_FAILED(*status))
		return UMS_SI;
	INT index = region_of(locale);
	return index >= 0 && countries[index].us_measure ? UMS_US : UMS_SI;
}

// The CLDR release behind ICU 72.
void __cdecl ulocdata_getCLDRVersion(BYTE version[4], UErrorCode *status)
{
	if (U_FAILED(*status))
		return;
	version[0] = 42;
	version[1] = version[2] = version[3] = 0;
}

typedef struct ULocaleDisplayNames {
	INT unused;
} ULocaleDisplayNames;

ULocaleDisplayNames *__cdecl uldn_open(const char *locale, INT dialect, UErrorCode *status)
{
	(void)locale;
	(void)dialect;
	if (U_FAILED(*status))
		return NULL;
	ULocaleDisplayNames *names = icu_alloc(sizeof *names);
	if (!names)
		*status = U_MEMORY_ALLOCATION_ERROR;
	return names;
}

void __cdecl uldn_close(ULocaleDisplayNames *names)
{
	icu_free(names);
}

INT __cdecl uldn_keyValueDisplayName(const ULocaleDisplayNames *names, const char *key, const char *value, UChar *dest, INT capacity,
	UErrorCode *status)
{
	static const struct { const char *value, *name; } calendars[] = {
		{"buddhist", "Buddhist Calendar"}, {"chinese", "Chinese Calendar"}, {"dangi", "Dangi Calendar"}, {"gregorian", "Gregorian Calendar"},
		{"hebrew", "Hebrew Calendar"}, {"islamic", "Hijri Calendar"}, {"islamic-umalqura", "Hijri Calendar (Umm al-Qura)"},
		{"japanese", "Japanese Calendar"}, {"persian", "Persian Calendar"}, {"roc", "Minguo Calendar"},
	};
	(void)names;
	if (U_FAILED(*status))
		return 0;
	if (!key || !value) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return 0;
	}
	const char *name = value;
	if (same_ascii(key, "calendar"))
		for (INT index = 0; index < (INT)ARRAYSIZE(calendars); index++)
			if (same_ascii(calendars[index].value, value))
				name = calendars[index].name;
	return finish_ascii(dest, capacity, name, status);
}

/* Currencies. */

INT __cdecl ucurr_forLocale(const char *locale, UChar *buffer, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	char value[16];
	UErrorCode inner = U_ZERO_ERROR;
	if (uloc_getKeywordValue(locale, "currency", value, sizeof value, &inner) == 3) {
		for (char *c = value; *c; c++)
			*c = upper_ascii(*c);
		return finish_ascii(buffer, capacity, value, status);
	}
	INT index = region_of(locale);
	if (index < 0) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return 0;
	}
	return finish_ascii(buffer, capacity, countries[index].currency, status);
}

enum { UCURR_SYMBOL_NAME = 0, UCURR_LONG_NAME = 1 };

// The name lives as long as the library; a currency with no name here is named by its code, with U_USING_DEFAULT_WARNING.
const UChar *__cdecl ucurr_getName(const UChar *currency, const char *locale, INT style, UBool *is_choice, INT *length, UErrorCode *status)
{
	static UChar names[ARRAYSIZE(currencies)][2][32];
	static UChar unknown[4];
	(void)locale;
	if (U_FAILED(*status))
		return NULL;
	if (is_choice)
		*is_choice = 0;
	char code[4] = {0};
	for (INT at = 0; at < 3 && currency && currency[at]; at++)
		code[at] = (char)currency[at];
	INT index = find_currency(code);
	if (index < 0) {
		u_uastrncpy(unknown, code, 4);
		*status = U_USING_DEFAULT_WARNING;
		if (length)
			*length = lstrlenA(code);
		return unknown;
	}
	INT kind = style == UCURR_LONG_NAME ? 1 : 0;
	UChar *name = names[index][kind];
	INT count = utf8_to_units(kind ? currencies[index].name : currencies[index].symbol, name, 32);
	if (length)
		*length = count;
	return name;
}

/* Number formats: the English patterns and symbols, with the currency of the locale's country. */

enum { UNUM_PATTERN_DECIMAL = 0, UNUM_DECIMAL = 1, UNUM_CURRENCY = 2, UNUM_PERCENT = 3, UNUM_SCIENTIFIC = 4 };
enum {
	UNUM_GROUPING_USED = 1, UNUM_DECIMAL_ALWAYS_SHOWN = 2, UNUM_MAX_INTEGER_DIGITS = 3, UNUM_MIN_INTEGER_DIGITS = 4, UNUM_INTEGER_DIGITS = 5,
	UNUM_MAX_FRACTION_DIGITS = 6, UNUM_MIN_FRACTION_DIGITS = 7, UNUM_FRACTION_DIGITS = 8, UNUM_MULTIPLIER = 9, UNUM_GROUPING_SIZE = 10,
	UNUM_SECONDARY_GROUPING_SIZE = 15,
};

typedef struct UNumberFormat {
	INT style;
	INT currency;
	UChar pattern[64];
} UNumberFormat;

UNumberFormat *__cdecl unum_open(INT style, const UChar *pattern, INT pattern_length, const char *locale, void *parse_error, UErrorCode *status)
{
	(void)parse_error;
	if (U_FAILED(*status))
		return NULL;
	if (style < UNUM_PATTERN_DECIMAL || style > UNUM_SCIENTIFIC) {
		*status = U_UNSUPPORTED_ERROR;
		return NULL;
	}
	UNumberFormat *format = icu_alloc(sizeof *format);
	if (!format) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	format->style = style;
	INT region = region_of(locale);
	format->currency = region >= 0 ? find_currency(countries[region].currency) : find_currency("USD");
	const char *text = style == UNUM_CURRENCY ? "\xc2\xa4#,##0.00" : style == UNUM_PERCENT ? "#,##0%" : style == UNUM_SCIENTIFIC ? "#E0" : "#,##0.###";
	if (style == UNUM_PATTERN_DECIMAL && pattern) {
		INT length = icu_length(pattern, pattern_length);
		if (length > 63)
			length = 63;
		CopyMemory(format->pattern, pattern, length * sizeof(UChar));
	} else
		utf8_to_units(text, format->pattern, ARRAYSIZE(format->pattern));
	if (style == UNUM_CURRENCY && format->currency >= 0 && !currencies[format->currency].digits)
		utf8_to_units("\xc2\xa4#,##0", format->pattern, ARRAYSIZE(format->pattern));
	if (!locale || !icu_known_locale(locale))
		*status = U_USING_DEFAULT_WARNING;
	return format;
}

void __cdecl unum_close(UNumberFormat *format)
{
	icu_free(format);
}

INT __cdecl unum_getAttribute(const UNumberFormat *format, INT attribute)
{
	BOOL money = format->style == UNUM_CURRENCY, percent = format->style == UNUM_PERCENT;
	INT money_digits = format->currency >= 0 ? currencies[format->currency].digits : 2;
	switch (attribute) {
	case UNUM_GROUPING_USED: return format->style != UNUM_SCIENTIFIC;
	case UNUM_DECIMAL_ALWAYS_SHOWN: return 0;
	case UNUM_MAX_INTEGER_DIGITS: return 2000000000;
	case UNUM_MIN_INTEGER_DIGITS: case UNUM_INTEGER_DIGITS: return 1;
	case UNUM_MAX_FRACTION_DIGITS: case UNUM_FRACTION_DIGITS: return money ? money_digits : percent ? 0 : 3;
	case UNUM_MIN_FRACTION_DIGITS: return money ? money_digits : 0;
	case UNUM_MULTIPLIER: return percent ? 100 : 1;
	case UNUM_GROUPING_SIZE: return format->style == UNUM_SCIENTIFIC ? 0 : 3;
	case UNUM_SECONDARY_GROUPING_SIZE: return 0;
	default: return -1;
	}
}

INT __cdecl unum_toPattern(const UNumberFormat *format, UBool localized, UChar *dest, INT capacity, UErrorCode *status)
{
	(void)localized;
	if (U_FAILED(*status))
		return 0;
	return finish_units(dest, capacity, format->pattern, icu_length(format->pattern, -1), status);
}

// UNumberFormatSymbol: the separators, signs and digits, in ICU's order.
INT __cdecl unum_getSymbol(const UNumberFormat *format, INT symbol, UChar *dest, INT capacity, UErrorCode *status)
{
	static const char *const symbols[] = {
		".", ",", ";", "%", "0", "#", "-", "+", NULL, NULL, ".", "E", "\xe2\x80\xb0", "*", "\xe2\x88\x9e", "NaN", "@", ",",
		"1", "2", "3", "4", "5", "6", "7", "8", "9", "\xc3\x97", "-", "+",
	};
	if (U_FAILED(*status))
		return 0;
	if (symbol < 0 || symbol >= (INT)ARRAYSIZE(symbols)) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return 0;
	}
	const char *text = symbols[symbol];
	if (symbol == 8 || symbol == 9) {
		INT currency = format->currency >= 0 ? format->currency : find_currency("USD");
		text = symbol == 8 ? currencies[currency].symbol : currencies[currency].code;
	}
	UChar units[16];
	INT length = utf8_to_units(text, units, ARRAYSIZE(units));
	return finish_units(dest, capacity, units, length, status);
}

/* Enumerations of strings, as the calendar and time zone lists return them. */

typedef struct UEnumeration {
	const char **items;
	INT count;
	INT at;
} UEnumeration;

static UEnumeration *enumeration_of(const char *const *items, INT count, UErrorCode *status)
{
	if (U_FAILED(*status))
		return NULL;
	UEnumeration *list = icu_alloc(sizeof *list);
	if (!list) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	list->items = (const char **)items;
	list->count = count;
	return list;
}

void __cdecl uenum_close(UEnumeration *list)
{
	icu_free(list);
}

INT __cdecl uenum_count(UEnumeration *list, UErrorCode *status)
{
	if (U_FAILED(*status) || !list)
		return 0;
	return list->count;
}

const char *__cdecl uenum_next(UEnumeration *list, INT *length, UErrorCode *status)
{
	if (U_FAILED(*status) || !list || list->at >= list->count) {
		if (length)
			*length = 0;
		return NULL;
	}
	const char *item = list->items[list->at++];
	if (length)
		*length = lstrlenA(item);
	return item;
}

/* Resource bundles: the tree of locale data ICU keeps. Only the parts programs read here are present: the narrow era names of the
 * Gregorian calendar. Opening a locale reports, as ICU does, whether its own data was found or a fallback was used. */

typedef struct UResourceBundle {
	INT level;
	const char *const *strings;
	INT count;
} UResourceBundle;

static const char *const narrow_eras[] = {"B", "A"};
enum { RES_ROOT, RES_CALENDAR, RES_GREGORIAN, RES_ERAS, RES_NARROW };

UResourceBundle *__cdecl ures_open(const char *path, const char *locale, UErrorCode *status)
{
	if (U_FAILED(*status))
		return NULL;
	if (path) {
		*status = U_MISSING_RESOURCE_ERROR;
		return NULL;
	}
	UResourceBundle *bundle = icu_alloc(sizeof *bundle);
	if (!bundle) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	bundle->level = RES_ROOT;
	locale_parts parts;
	split_locale(locale ? locale : uloc_getDefault(), &parts);
	if (!icu_known_locale(locale ? locale : uloc_getDefault()))
		*status = same_ascii(parts.language, "en") ? U_USING_FALLBACK_WARNING : U_USING_DEFAULT_WARNING;
	return bundle;
}

void __cdecl ures_close(UResourceBundle *bundle)
{
	icu_free(bundle);
}

UResourceBundle *__cdecl ures_getByKey(const UResourceBundle *bundle, const char *key, UResourceBundle *fill, UErrorCode *status)
{
	if (U_FAILED(*status))
		return fill;
	static const char *const path[] = {"calendar", "gregorian", "eras", "narrow"};
	if (!bundle || bundle->level >= RES_NARROW || !same_ascii(path[bundle->level], key)) {
		*status = U_MISSING_RESOURCE_ERROR;
		return fill;
	}
	UResourceBundle *child = fill ? fill : icu_alloc(sizeof *child);
	if (!child) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	child->level = bundle->level + 1;
	child->strings = child->level == RES_NARROW ? narrow_eras : NULL;
	child->count = child->level == RES_NARROW ? ARRAYSIZE(narrow_eras) : 1;
	return child;
}

INT __cdecl ures_getSize(const UResourceBundle *bundle)
{
	return bundle ? bundle->count : 0;
}

const UChar *__cdecl ures_getStringByIndex(const UResourceBundle *bundle, INT index, INT *length, UErrorCode *status)
{
	static UChar narrow[ARRAYSIZE(narrow_eras)][4];
	if (U_FAILED(*status))
		return NULL;
	if (!bundle || !bundle->strings || index < 0 || index >= bundle->count) {
		*status = U_MISSING_RESOURCE_ERROR;
		return NULL;
	}
	INT count = utf8_to_units(bundle->strings[index], narrow[index], 4);
	if (length)
		*length = count;
	return narrow[index];
}

/* The Gregorian calendar: a time in milliseconds since 1970 and the fields it falls on, in a time zone given by its offset. */

typedef double UDate;

enum {
	UCAL_ERA, UCAL_YEAR, UCAL_MONTH, UCAL_WEEK_OF_YEAR, UCAL_WEEK_OF_MONTH, UCAL_DATE, UCAL_DAY_OF_YEAR, UCAL_DAY_OF_WEEK,
	UCAL_DAY_OF_WEEK_IN_MONTH, UCAL_AM_PM, UCAL_HOUR, UCAL_HOUR_OF_DAY, UCAL_MINUTE, UCAL_SECOND, UCAL_MILLISECOND, UCAL_ZONE_OFFSET,
	UCAL_DST_OFFSET, UCAL_YEAR_WOY, UCAL_DOW_LOCAL, UCAL_EXTENDED_YEAR, UCAL_JULIAN_DAY, UCAL_MILLISECONDS_IN_DAY, UCAL_IS_LEAP_MONTH,
	UCAL_FIELD_COUNT
};
enum { UCAL_LENIENT, UCAL_FIRST_DAY_OF_WEEK, UCAL_MINIMAL_DAYS_IN_FIRST_WEEK };
enum { UCAL_MINIMUM, UCAL_MAXIMUM, UCAL_GREATEST_MINIMUM, UCAL_LEAST_MAXIMUM, UCAL_ACTUAL_MINIMUM, UCAL_ACTUAL_MAXIMUM };
enum { UCAL_STANDARD, UCAL_SHORT_STANDARD, UCAL_DST, UCAL_SHORT_DST };

typedef struct UCalendar {
	UDate millis;
	INT offset;
	BOOL local_zone;
	UChar zone[64];
	INT first_day;
	INT minimal_days;
	INT fields[UCAL_FIELD_COUNT];
} UCalendar;

static INT64 floor_div(INT64 value, INT64 by)
{
	return value >= 0 ? value / by : -((-value + by - 1) / by);
}

static BOOL leap(INT64 year)
{
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

// Days from 1970-01-01 to the given proleptic Gregorian date, month 0 to 11.
static INT64 days_from_civil(INT64 year, INT64 month, INT64 day)
{
	year += floor_div(month, 12);
	month = month - floor_div(month, 12) * 12;
	INT64 y = month < 2 ? year - 1 : year;
	INT64 era = floor_div(y, 400);
	INT64 yoe = y - era * 400;
	INT64 m = month + 1;
	INT64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	INT64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static void civil_from_days(INT64 days, INT64 *year, INT *month, INT *day)
{
	days += 719468;
	INT64 era = floor_div(days, 146097);
	INT64 doe = days - era * 146097;
	INT64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	INT64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	INT64 mp = (5 * doy + 2) / 153;
	*day = (INT)(doy - (153 * mp + 2) / 5 + 1);
	*month = (INT)(mp < 10 ? mp + 2 : mp - 10);
	*year = yoe + era * 400 + (*month < 2 ? 1 : 0);
}

// The time zone's offset from GMT in milliseconds at a time: the system's own zone follows its daylight rules; a named zone is taken at
// the offset its name gives ("GMT+2", "Etc/GMT-3", UTC), or at GMT.
static INT zone_offset(const UCalendar *calendar, UDate millis)
{
	if (!calendar->local_zone)
		return calendar->offset;
	INT64 ticks = (INT64)millis * 10000 + 116444736000000000LL;
	FILETIME utc = {(DWORD)ticks, (DWORD)(ticks >> 32)};
	SYSTEMTIME system, local;
	if (!FileTimeToSystemTime(&utc, &system) || !SystemTimeToTzSpecificLocalTime(NULL, &system, &local))
		return 0;
	FILETIME local_time;
	SystemTimeToFileTime(&local, &local_time);
	INT64 local_ticks = ((INT64)local_time.dwHighDateTime << 32) | local_time.dwLowDateTime;
	return (INT)((local_ticks - ticks) / 10000);
}

static INT standard_offset(const UCalendar *calendar)
{
	if (!calendar->local_zone)
		return calendar->offset;
	TIME_ZONE_INFORMATION zone;
	GetTimeZoneInformation(&zone);
	return -(INT)zone.Bias * 60000;
}

static void compute_fields(UCalendar *calendar)
{
	INT offset = zone_offset(calendar, calendar->millis);
	INT standard = standard_offset(calendar);
	INT64 local = (INT64)calendar->millis + offset;
	INT64 days = floor_div(local, 86400000);
	INT64 in_day = local - days * 86400000;
	INT64 year;
	INT month, day;
	civil_from_days(days, &year, &month, &day);
	INT *f = calendar->fields;
	f[UCAL_EXTENDED_YEAR] = (INT)year;
	f[UCAL_ERA] = year > 0 ? 1 : 0;
	f[UCAL_YEAR] = (INT)(year > 0 ? year : 1 - year);
	f[UCAL_MONTH] = month;
	f[UCAL_DATE] = day;
	f[UCAL_DAY_OF_YEAR] = (INT)(days - days_from_civil(year, 0, 1) + 1);
	f[UCAL_DAY_OF_WEEK] = (INT)(floor_div(days + 4, 7) * -7 + days + 4) + 1;
	f[UCAL_DAY_OF_WEEK_IN_MONTH] = (day - 1) / 7 + 1;
	f[UCAL_MILLISECONDS_IN_DAY] = (INT)in_day;
	f[UCAL_HOUR_OF_DAY] = (INT)(in_day / 3600000);
	f[UCAL_HOUR] = f[UCAL_HOUR_OF_DAY] % 12;
	f[UCAL_AM_PM] = f[UCAL_HOUR_OF_DAY] >= 12;
	f[UCAL_MINUTE] = (INT)(in_day / 60000 % 60);
	f[UCAL_SECOND] = (INT)(in_day / 1000 % 60);
	f[UCAL_MILLISECOND] = (INT)(in_day % 1000);
	f[UCAL_ZONE_OFFSET] = standard;
	f[UCAL_DST_OFFSET] = offset - standard;
	f[UCAL_JULIAN_DAY] = (INT)(days + 2440588);
	f[UCAL_DOW_LOCAL] = (f[UCAL_DAY_OF_WEEK] - calendar->first_day + 7) % 7 + 1;
	// Week 1 is the first week with at least the minimal number of days in the year.
	INT first_dow = (f[UCAL_DAY_OF_WEEK] - (f[UCAL_DAY_OF_YEAR] - 1) % 7 + 6) % 7 + 1;
	INT shift = (first_dow - calendar->first_day + 7) % 7;
	INT week = (f[UCAL_DAY_OF_YEAR] - 1 + shift) / 7 + (7 - shift >= calendar->minimal_days ? 1 : 0);
	f[UCAL_WEEK_OF_YEAR] = week ? week : 52;
	f[UCAL_YEAR_WOY] = week ? f[UCAL_EXTENDED_YEAR] : f[UCAL_EXTENDED_YEAR] - 1;
	INT month_first = (f[UCAL_DAY_OF_WEEK] - (day - 1) % 7 + 6) % 7 + 1;
	INT month_shift = (month_first - calendar->first_day + 7) % 7;
	f[UCAL_WEEK_OF_MONTH] = (day - 1 + month_shift) / 7 + (7 - month_shift >= calendar->minimal_days ? 1 : 0);
	f[UCAL_IS_LEAP_MONTH] = 0;
}

static void compute_time(UCalendar *calendar)
{
	INT *f = calendar->fields;
	INT64 days = days_from_civil(f[UCAL_EXTENDED_YEAR], f[UCAL_MONTH], 1) + f[UCAL_DATE] - 1;
	INT64 local = days * 86400000 + (INT64)f[UCAL_HOUR_OF_DAY] * 3600000 + (INT64)f[UCAL_MINUTE] * 60000 + (INT64)f[UCAL_SECOND] * 1000 +
		f[UCAL_MILLISECOND];
	INT offset = zone_offset(calendar, (UDate)(local - standard_offset(calendar)));
	calendar->millis = (UDate)(local - offset);
	compute_fields(calendar);
}

UDate __cdecl ucal_getNow(void)
{
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	INT64 ticks = ((INT64)now.dwHighDateTime << 32) | now.dwLowDateTime;
	return (UDate)((ticks - 116444736000000000LL) / 10000);
}

// The offset a zone's ID names: "UTC", "GMT", "Etc/GMT+5" (five hours behind, as the IANA names count) and "GMT-03:30".
static BOOL named_offset(const UChar *id, INT *offset)
{
	char name[64];
	INT length = 0;
	for (; id[length] && length < 63; length++)
		name[length] = (char)id[length];
	name[length] = 0;
	const char *at = name;
	BOOL etc = FALSE;
	if (!lstrcmpiA(name, "UTC") || !lstrcmpiA(name, "GMT") || !lstrcmpiA(name, "Etc/UTC") || !lstrcmpiA(name, "Etc/GMT") ||
		!lstrcmpiA(name, "Universal") || !lstrcmpiA(name, "Zulu")) {
		*offset = 0;
		return TRUE;
	}
	if (starts_ascii(name, "Etc/GMT")) {
		at = name + 7;
		etc = TRUE;
	} else if (starts_ascii(name, "GMT") || starts_ascii(name, "UTC"))
		at = name + 3;
	else
		return FALSE;
	if (*at != '+' && *at != '-')
		return FALSE;
	INT sign = *at == '-' ? -1 : 1, hours = 0, minutes = 0;
	at++;
	while (*at >= '0' && *at <= '9')
		hours = hours * 10 + (*at++ - '0');
	if (*at == ':')
		at++;
	while (*at >= '0' && *at <= '9')
		minutes = minutes * 10 + (*at++ - '0');
	*offset = (etc ? -sign : sign) * (hours * 3600000 + minutes * 60000);
	return TRUE;
}

UCalendar *__cdecl ucal_open(const UChar *zone, INT zone_length, const char *locale, INT type, UErrorCode *status)
{
	(void)type;
	if (U_FAILED(*status))
		return NULL;
	char calendar_name[32];
	UErrorCode inner = U_ZERO_ERROR;
	if (uloc_getKeywordValue(locale, "calendar", calendar_name, sizeof calendar_name, &inner) > 0 && !same_ascii(calendar_name, "gregorian")) {
		*status = U_UNSUPPORTED_ERROR;
		return NULL;
	}
	UCalendar *calendar = icu_alloc(sizeof *calendar);
	if (!calendar) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	calendar->local_zone = TRUE;
	if (zone) {
		INT length = icu_length(zone, zone_length);
		if (length > 63)
			length = 63;
		CopyMemory(calendar->zone, zone, length * sizeof(UChar));
		calendar->local_zone = !named_offset(calendar->zone, &calendar->offset) && !length;
		if (!calendar->local_zone && !named_offset(calendar->zone, &calendar->offset))
			calendar->offset = 0;
	}
	INT region = region_of(locale);
	calendar->first_day = region >= 0 && !countries[region].sunday ? 2 : 1;
	calendar->minimal_days = region >= 0 && countries[region].first_week_full ? 4 : 1;
	calendar->millis = ucal_getNow();
	compute_fields(calendar);
	return calendar;
}

void __cdecl ucal_close(UCalendar *calendar)
{
	icu_free(calendar);
}

void __cdecl ucal_setMillis(UCalendar *calendar, UDate millis, UErrorCode *status)
{
	if (U_FAILED(*status))
		return;
	calendar->millis = millis;
	compute_fields(calendar);
}

INT __cdecl ucal_get(const UCalendar *calendar, INT field, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	if (field < 0 || field >= UCAL_FIELD_COUNT) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return 0;
	}
	return calendar->fields[field];
}

// A field set moves the time to match it; the fields the date is built from are year, month and day, and the time of day.
void __cdecl ucal_set(UCalendar *calendar, INT field, INT value)
{
	INT *f = calendar->fields;
	switch (field) {
	case UCAL_ERA: f[UCAL_EXTENDED_YEAR] = value ? f[UCAL_YEAR] : 1 - f[UCAL_YEAR]; break;
	case UCAL_YEAR: f[UCAL_EXTENDED_YEAR] = f[UCAL_ERA] ? value : 1 - value; break;
	case UCAL_EXTENDED_YEAR: f[UCAL_EXTENDED_YEAR] = value; break;
	case UCAL_MONTH: f[UCAL_MONTH] = value; break;
	case UCAL_DATE: f[UCAL_DATE] = value; break;
	case UCAL_DAY_OF_YEAR: f[UCAL_MONTH] = 0; f[UCAL_DATE] = value; break;
	case UCAL_HOUR_OF_DAY: f[UCAL_HOUR_OF_DAY] = value; break;
	case UCAL_HOUR: f[UCAL_HOUR_OF_DAY] = value + (f[UCAL_AM_PM] ? 12 : 0); break;
	case UCAL_AM_PM: f[UCAL_HOUR_OF_DAY] = f[UCAL_HOUR] + (value ? 12 : 0); break;
	case UCAL_MINUTE: f[UCAL_MINUTE] = value; break;
	case UCAL_SECOND: f[UCAL_SECOND] = value; break;
	case UCAL_MILLISECOND: f[UCAL_MILLISECOND] = value; break;
	default: return;
	}
	compute_time(calendar);
}

static INT month_length(INT64 year, INT month)
{
	static const INT lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	return month == 1 && leap(year) ? 29 : lengths[month];
}

// Adding months or years keeps the day within the new month, as ICU pins it.
void __cdecl ucal_add(UCalendar *calendar, INT field, INT amount, UErrorCode *status)
{
	if (U_FAILED(*status))
		return;
	INT *f = calendar->fields;
	INT64 step = 0;
	switch (field) {
	case UCAL_YEAR: case UCAL_EXTENDED_YEAR: case UCAL_MONTH: {
		INT64 months = (INT64)f[UCAL_EXTENDED_YEAR] * 12 + f[UCAL_MONTH] + (field == UCAL_MONTH ? amount : (INT64)amount * 12);
		f[UCAL_EXTENDED_YEAR] = (INT)floor_div(months, 12);
		f[UCAL_MONTH] = (INT)(months - floor_div(months, 12) * 12);
		INT days = month_length(f[UCAL_EXTENDED_YEAR], f[UCAL_MONTH]);
		if (f[UCAL_DATE] > days)
			f[UCAL_DATE] = days;
		compute_time(calendar);
		return;
	}
	case UCAL_WEEK_OF_YEAR: case UCAL_WEEK_OF_MONTH: step = 7 * 86400000LL; break;
	case UCAL_DATE: case UCAL_DAY_OF_YEAR: case UCAL_DAY_OF_WEEK: step = 86400000LL; break;
	case UCAL_HOUR: case UCAL_HOUR_OF_DAY: step = 3600000LL; break;
	case UCAL_MINUTE: step = 60000LL; break;
	case UCAL_SECOND: step = 1000LL; break;
	case UCAL_MILLISECOND: step = 1; break;
	default: *status = U_ILLEGAL_ARGUMENT_ERROR; return;
	}
	if (step >= 86400000LL) {
		// Whole days keep the wall-clock time across a daylight change.
		INT64 days = days_from_civil(f[UCAL_EXTENDED_YEAR], f[UCAL_MONTH], f[UCAL_DATE]) + amount * (step / 86400000LL);
		INT64 year;
		INT month, day;
		civil_from_days(days, &year, &month, &day);
		f[UCAL_EXTENDED_YEAR] = (INT)year;
		f[UCAL_MONTH] = month;
		f[UCAL_DATE] = day;
		compute_time(calendar);
		return;
	}
	calendar->millis += (UDate)(step * amount);
	compute_fields(calendar);
}

INT __cdecl ucal_getAttribute(const UCalendar *calendar, INT attribute)
{
	switch (attribute) {
	case UCAL_LENIENT: return 1;
	case UCAL_FIRST_DAY_OF_WEEK: return calendar->first_day;
	case UCAL_MINIMAL_DAYS_IN_FIRST_WEEK: return calendar->minimal_days;
	default: return 0;
	}
}

INT __cdecl ucal_getLimit(const UCalendar *calendar, INT field, INT type, UErrorCode *status)
{
	static const INT limits[][4] = {
		{0, 0, 1, 1}, {1, 1, 140742, 144683}, {0, 0, 11, 11}, {1, 1, 52, 53}, {0, 0, 4, 6}, {1, 1, 28, 31}, {1, 1, 365, 366}, {1, 1, 7, 7},
		{-1, -1, 4, 5}, {0, 0, 1, 1}, {0, 0, 11, 11}, {0, 0, 23, 23}, {0, 0, 59, 59}, {0, 0, 59, 59}, {0, 0, 999, 999},
		{-16 * 3600000, -16 * 3600000, 12 * 3600000, 30 * 3600000}, {0, 0, 0, 2 * 3600000}, {-140742, -140742, 140742, 144683},
		{1, 1, 7, 7}, {-140742, -140742, 140742, 144683}, {-0x7f000000, -0x7f000000, 0x7f000000, 0x7f000000},
		{0, 0, 24 * 3600000 - 1, 24 * 3600000 - 1}, {0, 0, 1, 1},
	};
	if (U_FAILED(*status))
		return 0;
	if (field < 0 || field >= UCAL_FIELD_COUNT) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return 0;
	}
	switch (type) {
	case UCAL_MINIMUM: return limits[field][0];
	case UCAL_GREATEST_MINIMUM: return limits[field][1];
	case UCAL_LEAST_MAXIMUM: return limits[field][2];
	case UCAL_MAXIMUM: return limits[field][3];
	case UCAL_ACTUAL_MINIMUM: return limits[field][0];
	case UCAL_ACTUAL_MAXIMUM:
		if (field == UCAL_DATE)
			return month_length(calendar->fields[UCAL_EXTENDED_YEAR], calendar->fields[UCAL_MONTH]);
		if (field == UCAL_DAY_OF_YEAR)
			return leap(calendar->fields[UCAL_EXTENDED_YEAR]) ? 366 : 365;
		return limits[field][3];
	default: *status = U_ILLEGAL_ARGUMENT_ERROR; return 0;
	}
}

static const char *const gregorian_only[] = {"gregorian"};

UEnumeration *__cdecl ucal_getKeywordValuesForLocale(const char *key, const char *locale, UBool common, UErrorCode *status)
{
	(void)locale;
	(void)common;
	if (U_FAILED(*status))
		return NULL;
	if (!same_ascii(key, "calendar")) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return NULL;
	}
	return enumeration_of(gregorian_only, 1, status);
}

// "GMT-08:00" for an offset, as ICU writes a zone it has no name for.
static INT gmt_name(INT offset, UChar *out)
{
	INT minutes = offset / 60000, length = 3;
	out[0] = 'G';
	out[1] = 'M';
	out[2] = 'T';
	if (minutes) {
		out[length++] = minutes < 0 ? '-' : '+';
		if (minutes < 0)
			minutes = -minutes;
		out[length++] = (UChar)('0' + minutes / 600 % 10);
		out[length++] = (UChar)('0' + minutes / 60 % 10);
		out[length++] = ':';
		out[length++] = (UChar)('0' + minutes % 60 / 10);
		out[length++] = (UChar)('0' + minutes % 10);
	}
	out[length] = 0;
	return length;
}

INT __cdecl ucal_getTimeZoneDisplayName(const UCalendar *calendar, INT type, const char *locale, UChar *dest, INT capacity, UErrorCode *status)
{
	(void)locale;
	if (U_FAILED(*status))
		return 0;
	BOOL daylight = type == UCAL_DST || type == UCAL_SHORT_DST;
	if (calendar->local_zone && (type == UCAL_STANDARD || type == UCAL_DST)) {
		TIME_ZONE_INFORMATION zone;
		GetTimeZoneInformation(&zone);
		const WCHAR *name = daylight ? zone.DaylightName : zone.StandardName;
		if (name[0])
			return finish_units(dest, capacity, name, lstrlenW(name), status);
	}
	UChar name[16];
	INT offset = calendar->fields[UCAL_ZONE_OFFSET] + (daylight ? calendar->fields[UCAL_DST_OFFSET] : 0);
	INT length = gmt_name(offset, name);
	return finish_units(dest, capacity, name, length, status);
}

/* Time zone IDs: the Windows names and the IANA zones CLDR maps them to for the world, with the countries that use another zone. */

static const struct { const char *windows, *region, *iana; } zones[] = {
	{"Dateline Standard Time", NULL, "Etc/GMT+12"}, {"UTC-11", NULL, "Etc/GMT+11"}, {"Hawaiian Standard Time", NULL, "Pacific/Honolulu"},
	{"Alaskan Standard Time", NULL, "America/Anchorage"}, {"Pacific Standard Time", NULL, "America/Los_Angeles"},
	{"US Mountain Standard Time", NULL, "America/Phoenix"}, {"Mountain Standard Time", NULL, "America/Denver"},
	{"Central America Standard Time", NULL, "America/Guatemala"}, {"Central Standard Time", NULL, "America/Chicago"},
	{"Central Standard Time (Mexico)", NULL, "America/Mexico_City"}, {"Canada Central Standard Time", NULL, "America/Regina"},
	{"SA Pacific Standard Time", NULL, "America/Bogota"}, {"Eastern Standard Time", NULL, "America/New_York"},
	{"Eastern Standard Time", "CA", "America/Toronto"}, {"US Eastern Standard Time", NULL, "America/Indianapolis"},
	{"Venezuela Standard Time", NULL, "America/Caracas"}, {"Atlantic Standard Time", NULL, "America/Halifax"},
	{"SA Western Standard Time", NULL, "America/La_Paz"}, {"Pacific SA Standard Time", NULL, "America/Santiago"},
	{"Newfoundland Standard Time", NULL, "America/St_Johns"}, {"E. South America Standard Time", NULL, "America/Sao_Paulo"},
	{"Argentina Standard Time", NULL, "America/Buenos_Aires"}, {"Greenland Standard Time", NULL, "America/Godthab"},
	{"UTC-02", NULL, "Etc/GMT+2"}, {"Azores Standard Time", NULL, "Atlantic/Azores"}, {"Cape Verde Standard Time", NULL, "Atlantic/Cape_Verde"},
	{"UTC", NULL, "Etc/UTC"}, {"GMT Standard Time", NULL, "Europe/London"}, {"GMT Standard Time", "IE", "Europe/Dublin"},
	{"GMT Standard Time", "PT", "Europe/Lisbon"}, {"Greenwich Standard Time", NULL, "Atlantic/Reykjavik"},
	{"W. Europe Standard Time", NULL, "Europe/Berlin"}, {"W. Europe Standard Time", "AT", "Europe/Vienna"},
	{"W. Europe Standard Time", "CH", "Europe/Zurich"}, {"W. Europe Standard Time", "IT", "Europe/Rome"},
	{"W. Europe Standard Time", "NL", "Europe/Amsterdam"}, {"W. Europe Standard Time", "NO", "Europe/Oslo"},
	{"W. Europe Standard Time", "SE", "Europe/Stockholm"}, {"W. Europe Standard Time", "LU", "Europe/Luxembourg"},
	{"W. Europe Standard Time", "MT", "Europe/Malta"}, {"Central Europe Standard Time", NULL, "Europe/Budapest"},
	{"Central Europe Standard Time", "CZ", "Europe/Prague"}, {"Central Europe Standard Time", "SK", "Europe/Bratislava"},
	{"Central Europe Standard Time", "SI", "Europe/Ljubljana"}, {"Central Europe Standard Time", "RS", "Europe/Belgrade"},
	{"Romance Standard Time", NULL, "Europe/Paris"}, {"Romance Standard Time", "BE", "Europe/Brussels"},
	{"Romance Standard Time", "DK", "Europe/Copenhagen"}, {"Romance Standard Time", "ES", "Europe/Madrid"},
	{"Central European Standard Time", NULL, "Europe/Warsaw"}, {"Central European Standard Time", "HR", "Europe/Zagreb"},
	{"W. Central Africa Standard Time", NULL, "Africa/Lagos"}, {"GTB Standard Time", NULL, "Europe/Bucharest"},
	{"GTB Standard Time", "GR", "Europe/Athens"}, {"Middle East Standard Time", NULL, "Asia/Beirut"}, {"Egypt Standard Time", NULL, "Africa/Cairo"},
	{"South Africa Standard Time", NULL, "Africa/Johannesburg"}, {"FLE Standard Time", NULL, "Europe/Kiev"},
	{"FLE Standard Time", "FI", "Europe/Helsinki"}, {"FLE Standard Time", "EE", "Europe/Tallinn"}, {"FLE Standard Time", "LT", "Europe/Vilnius"},
	{"FLE Standard Time", "LV", "Europe/Riga"}, {"FLE Standard Time", "BG", "Europe/Sofia"}, {"Israel Standard Time", NULL, "Asia/Jerusalem"},
	{"Arabic Standard Time", NULL, "Asia/Baghdad"}, {"Turkey Standard Time", NULL, "Europe/Istanbul"}, {"Arab Standard Time", NULL, "Asia/Riyadh"},
	{"Russian Standard Time", NULL, "Europe/Moscow"}, {"E. Africa Standard Time", NULL, "Africa/Nairobi"}, {"Iran Standard Time", NULL, "Asia/Tehran"},
	{"Arabian Standard Time", NULL, "Asia/Dubai"}, {"Afghanistan Standard Time", NULL, "Asia/Kabul"},
	{"Pakistan Standard Time", NULL, "Asia/Karachi"}, {"India Standard Time", NULL, "Asia/Calcutta"}, {"Nepal Standard Time", NULL, "Asia/Katmandu"},
	{"Bangladesh Standard Time", NULL, "Asia/Dhaka"}, {"SE Asia Standard Time", NULL, "Asia/Bangkok"},
	{"SE Asia Standard Time", "VN", "Asia/Saigon"}, {"SE Asia Standard Time", "ID", "Asia/Jakarta"}, {"China Standard Time", NULL, "Asia/Shanghai"},
	{"China Standard Time", "HK", "Asia/Hong_Kong"}, {"Singapore Standard Time", NULL, "Asia/Singapore"},
	{"Singapore Standard Time", "MY", "Asia/Kuala_Lumpur"}, {"Singapore Standard Time", "PH", "Asia/Manila"},
	{"W. Australia Standard Time", NULL, "Australia/Perth"}, {"Taipei Standard Time", NULL, "Asia/Taipei"}, {"Tokyo Standard Time", NULL, "Asia/Tokyo"},
	{"Korea Standard Time", NULL, "Asia/Seoul"}, {"Cen. Australia Standard Time", NULL, "Australia/Adelaide"},
	{"AUS Central Standard Time", NULL, "Australia/Darwin"}, {"E. Australia Standard Time", NULL, "Australia/Brisbane"},
	{"AUS Eastern Standard Time", NULL, "Australia/Sydney"}, {"Tasmania Standard Time", NULL, "Australia/Hobart"},
	{"New Zealand Standard Time", NULL, "Pacific/Auckland"}, {"Tonga Standard Time", NULL, "Pacific/Tongatapu"},
};

// Other names of the same zones: the current IANA spellings and the zones that link to one above.
static const struct { const char *alias, *iana; } zone_aliases[] = {
	{"Asia/Kolkata", "Asia/Calcutta"}, {"Asia/Kathmandu", "Asia/Katmandu"}, {"Asia/Ho_Chi_Minh", "Asia/Saigon"}, {"Europe/Kyiv", "Europe/Kiev"},
	{"America/Nuuk", "America/Godthab"}, {"America/Argentina/Buenos_Aires", "America/Buenos_Aires"},
	{"America/Indiana/Indianapolis", "America/Indianapolis"}, {"Etc/GMT", "Etc/UTC"}, {"UTC", "Etc/UTC"}, {"GMT", "Etc/UTC"},
	{"Etc/Universal", "Etc/UTC"}, {"US/Pacific", "America/Los_Angeles"}, {"US/Eastern", "America/New_York"},
	{"US/Central", "America/Chicago"}, {"US/Mountain", "America/Denver"},
};

INT __cdecl ucal_getTimeZoneIDForWindowsID(const UChar *windows, INT length, const char *region, UChar *dest, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	char name[64];
	INT count = icu_length(windows, length);
	if (count > 63)
		count = 63;
	for (INT at = 0; at < count; at++)
		name[at] = (char)windows[at];
	name[count] = 0;
	const char *found = NULL;
	for (INT index = 0; index < (INT)ARRAYSIZE(zones); index++) {
		if (!same_ascii(zones[index].windows, name))
			continue;
		if (!zones[index].region && !found)
			found = zones[index].iana;
		if (region && zones[index].region && same_ascii(zones[index].region, region))
			found = zones[index].iana;
	}
	if (!found) {
		if (dest && capacity > 0)
			dest[0] = 0;
		return 0;
	}
	return finish_ascii(dest, capacity, found, status);
}

INT __cdecl ucal_getWindowsTimeZoneID(const UChar *id, INT length, UChar *dest, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	char name[64];
	INT count = icu_length(id, length);
	if (count > 63)
		count = 63;
	for (INT at = 0; at < count; at++)
		name[at] = (char)id[at];
	name[count] = 0;
	const char *iana = name;
	for (INT index = 0; index < (INT)ARRAYSIZE(zone_aliases); index++)
		if (same_ascii(zone_aliases[index].alias, name))
			iana = zone_aliases[index].iana;
	for (INT index = 0; index < (INT)ARRAYSIZE(zones); index++)
		if (same_ascii(zones[index].iana, iana))
			return finish_ascii(dest, capacity, zones[index].windows, status);
	if (dest && capacity > 0)
		dest[0] = 0;
	return 0;
}

// The zones whose standard offset is known here: those named by their offset alone. A list by region or offset of other zones needs
// their rules, which are not here, so it is empty.
UEnumeration *__cdecl ucal_openTimeZoneIDEnumeration(INT type, const char *region, const INT *raw_offset, UErrorCode *status)
{
	static const char *const none[1] = {NULL};
	(void)type;
	(void)region;
	(void)raw_offset;
	return enumeration_of(none, 0, status);
}

/* Date formats: the English patterns of CLDR and a formatter for the fields they use. */

enum { UDAT_FULL = 0, UDAT_LONG = 1, UDAT_MEDIUM = 2, UDAT_SHORT = 3, UDAT_NONE = -1, UDAT_PATTERN = -2 };
enum {
	UDAT_ERAS, UDAT_MONTHS, UDAT_SHORT_MONTHS, UDAT_WEEKDAYS, UDAT_SHORT_WEEKDAYS, UDAT_AM_PMS, UDAT_LOCALIZED_CHARS, UDAT_ERA_NAMES,
	UDAT_NARROW_MONTHS, UDAT_NARROW_WEEKDAYS, UDAT_STANDALONE_MONTHS, UDAT_STANDALONE_SHORT_MONTHS, UDAT_STANDALONE_NARROW_MONTHS,
	UDAT_STANDALONE_WEEKDAYS, UDAT_STANDALONE_SHORT_WEEKDAYS, UDAT_STANDALONE_NARROW_WEEKDAYS, UDAT_QUARTERS, UDAT_SHORT_QUARTERS,
	UDAT_STANDALONE_QUARTERS, UDAT_STANDALONE_SHORT_QUARTERS, UDAT_SHORTER_WEEKDAYS, UDAT_STANDALONE_SHORTER_WEEKDAYS,
};

static const char *const month_names[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
	"November", "December"};
static const char *const month_short[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const month_narrow[] = {"J", "F", "M", "A", "M", "J", "J", "A", "S", "O", "N", "D"};
static const char *const day_names[] = {"", "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const day_short[] = {"", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const day_shorter[] = {"", "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
static const char *const day_narrow[] = {"", "S", "M", "T", "W", "T", "F", "S"};
static const char *const am_pm[] = {"AM", "PM"};
static const char *const eras[] = {"BC", "AD"};
static const char *const era_names[] = {"Before Christ", "Anno Domini"};
static const char *const quarters[] = {"1st quarter", "2nd quarter", "3rd quarter", "4th quarter"};
static const char *const quarters_short[] = {"Q1", "Q2", "Q3", "Q4"};
static const char *const pattern_chars[] = {"GyMdkHmsSEDFwWahKzYeugAZvcLQqVUOXxrbB"};

static const char *const *symbol_table(INT type, INT *count)
{
	switch (type) {
	case UDAT_ERAS: *count = 2; return eras;
	case UDAT_ERA_NAMES: *count = 2; return era_names;
	case UDAT_MONTHS: case UDAT_STANDALONE_MONTHS: *count = 12; return month_names;
	case UDAT_SHORT_MONTHS: case UDAT_STANDALONE_SHORT_MONTHS: *count = 12; return month_short;
	case UDAT_NARROW_MONTHS: case UDAT_STANDALONE_NARROW_MONTHS: *count = 12; return month_narrow;
	case UDAT_WEEKDAYS: case UDAT_STANDALONE_WEEKDAYS: *count = 8; return day_names;
	case UDAT_SHORT_WEEKDAYS: case UDAT_STANDALONE_SHORT_WEEKDAYS: *count = 8; return day_short;
	case UDAT_SHORTER_WEEKDAYS: case UDAT_STANDALONE_SHORTER_WEEKDAYS: *count = 8; return day_shorter;
	case UDAT_NARROW_WEEKDAYS: case UDAT_STANDALONE_NARROW_WEEKDAYS: *count = 8; return day_narrow;
	case UDAT_AM_PMS: *count = 2; return am_pm;
	case UDAT_QUARTERS: case UDAT_STANDALONE_QUARTERS: *count = 4; return quarters;
	case UDAT_SHORT_QUARTERS: case UDAT_STANDALONE_SHORT_QUARTERS: *count = 4; return quarters_short;
	case UDAT_LOCALIZED_CHARS: *count = 1; return pattern_chars;
	default: *count = 0; return NULL;
	}
}

typedef struct UDateFormat {
	UChar pattern[96];
	UCalendar *calendar;
} UDateFormat;

static const char *date_pattern(INT style)
{
	switch (style) {
	case UDAT_FULL: return "EEEE, MMMM d, y";
	case UDAT_LONG: return "MMMM d, y";
	case UDAT_MEDIUM: return "MMM d, y";
	case UDAT_SHORT: return "M/d/yy";
	default: return NULL;
	}
}

static const char *time_pattern(INT style)
{
	switch (style) {
	case UDAT_FULL: return "h:mm:ss a zzzz";
	case UDAT_LONG: return "h:mm:ss a z";
	case UDAT_MEDIUM: return "h:mm:ss a";
	case UDAT_SHORT: return "h:mm a";
	default: return NULL;
	}
}

UDateFormat *__cdecl udat_open(INT time_style, INT date_style, const char *locale, const UChar *zone, INT zone_length, const UChar *pattern,
	INT pattern_length, UErrorCode *status)
{
	if (U_FAILED(*status))
		return NULL;
	UDateFormat *format = icu_alloc(sizeof *format);
	if (!format) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	format->calendar = ucal_open(zone, zone_length, locale, 0, status);
	if (U_FAILED(*status)) {
		icu_free(format);
		return NULL;
	}
	if (time_style == UDAT_PATTERN) {
		INT length = pattern ? icu_length(pattern, pattern_length) : 0;
		if (length > 95)
			length = 95;
		CopyMemory(format->pattern, pattern, length * sizeof(UChar));
		return format;
	}
	const char *date = date_pattern(date_style), *time = time_pattern(time_style);
	char text[96] = "";
	if (date && time) {
		lstrcpyA(text, date);
		lstrcatA(text, date_style <= UDAT_LONG ? " 'at' " : ", ");
		lstrcatA(text, time);
	} else if (date || time)
		lstrcpyA(text, date ? date : time);
	utf8_to_units(text, format->pattern, ARRAYSIZE(format->pattern));
	return format;
}

void __cdecl udat_close(UDateFormat *format)
{
	if (!format)
		return;
	ucal_close(format->calendar);
	icu_free(format);
}

INT __cdecl udat_toPattern(const UDateFormat *format, UBool localized, UChar *dest, INT capacity, UErrorCode *status)
{
	(void)localized;
	if (U_FAILED(*status))
		return 0;
	return finish_units(dest, capacity, format->pattern, icu_length(format->pattern, -1), status);
}

// The format takes the calendar's settings: its zone, first day of the week and minimal days.
void __cdecl udat_setCalendar(UDateFormat *format, const UCalendar *calendar)
{
	if (format && calendar)
		CopyMemory(format->calendar, calendar, sizeof *calendar);
}

INT __cdecl udat_countSymbols(const UDateFormat *format, INT type)
{
	(void)format;
	INT count;
	symbol_table(type, &count);
	return count;
}

INT __cdecl udat_getSymbols(const UDateFormat *format, INT type, INT index, UChar *dest, INT capacity, UErrorCode *status)
{
	(void)format;
	if (U_FAILED(*status))
		return 0;
	INT count;
	const char *const *table = symbol_table(type, &count);
	if (!table || index < 0 || index >= count) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return 0;
	}
	UChar units[32];
	INT length = utf8_to_units(table[index], units, ARRAYSIZE(units));
	return finish_units(dest, capacity, units, length, status);
}

// The exemplar city of an IANA zone: the last part of its ID, with spaces for underscores.
static INT city_of(const UChar *zone, UChar *out, INT capacity)
{
	const UChar *at = zone, *last = zone;
	for (; *at; at++)
		if (*at == '/')
			last = at + 1;
	INT length = 0;
	for (; last[length] && length + 1 < capacity; length++)
		out[length] = last[length] == '_' ? ' ' : last[length];
	out[length] = 0;
	return length;
}

static void append_number(UChar *out, INT *length, INT capacity, INT64 value, INT width)
{
	char digits[24];
	INT count = 0;
	BOOL negative = value < 0;
	if (negative)
		value = -value;
	do {
		digits[count++] = (char)('0' + value % 10);
		value /= 10;
	} while (value && count < 23);
	while (count < width && count < 23)
		digits[count++] = '0';
	if (negative && *length < capacity)
		out[(*length)++] = '-';
	while (count && *length < capacity)
		out[(*length)++] = digits[--count];
}

static void append_text(UChar *out, INT *length, INT capacity, const char *text)
{
	UChar units[64];
	INT count = utf8_to_units(text, units, ARRAYSIZE(units));
	for (INT at = 0; at < count && *length < capacity; at++)
		out[(*length)++] = units[at];
}

static void append_units(UChar *out, INT *length, INT capacity, const UChar *text, INT count)
{
	for (INT at = 0; at < count && *length < capacity; at++)
		out[(*length)++] = text[at];
}

INT __cdecl udat_format(const UDateFormat *format, UDate date, UChar *dest, INT capacity, void *position, UErrorCode *status)
{
	(void)position;
	if (U_FAILED(*status))
		return 0;
	UCalendar *calendar = format->calendar;
	ucal_setMillis(calendar, date, status);
	const INT *f = calendar->fields;
	UChar out[256];
	INT length = 0;
	const UChar *pattern = format->pattern;
	for (INT at = 0; pattern[at];) {
		UChar letter = pattern[at];
		if (letter == '\'') {
			at++;
			if (pattern[at] == '\'') {
				out[length++] = '\'';
				at++;
				continue;
			}
			while (pattern[at] && length < 255) {
				if (pattern[at] == '\'') {
					if (pattern[at + 1] == '\'') {
						out[length++] = '\'';
						at += 2;
						continue;
					}
					break;
				}
				out[length++] = pattern[at++];
			}
			if (pattern[at])
				at++;
			continue;
		}
		if (!((letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z'))) {
			if (length < 255)
				out[length++] = letter;
			at++;
			continue;
		}
		INT run = 1;
		while (pattern[at + run] == letter)
			run++;
		at += run;
		switch (letter) {
		case 'G': append_text(out, &length, 255, run == 4 ? era_names[f[UCAL_ERA]] : eras[f[UCAL_ERA]]); break;
		case 'y': case 'Y': case 'u': {
			INT64 year = letter == 'u' ? f[UCAL_EXTENDED_YEAR] : letter == 'Y' ? f[UCAL_YEAR_WOY] : f[UCAL_YEAR];
			append_number(out, &length, 255, run == 2 ? year % 100 : year, run == 2 ? 2 : run);
			break;
		}
		case 'M': case 'L':
			if (run >= 5)
				append_text(out, &length, 255, month_narrow[f[UCAL_MONTH]]);
			else if (run == 4)
				append_text(out, &length, 255, month_names[f[UCAL_MONTH]]);
			else if (run == 3)
				append_text(out, &length, 255, month_short[f[UCAL_MONTH]]);
			else
				append_number(out, &length, 255, f[UCAL_MONTH] + 1, run);
			break;
		case 'd': append_number(out, &length, 255, f[UCAL_DATE], run); break;
		case 'D': append_number(out, &length, 255, f[UCAL_DAY_OF_YEAR], run); break;
		case 'E': case 'e': case 'c':
			if ((letter != 'E' && run <= 2))
				append_number(out, &length, 255, f[UCAL_DOW_LOCAL], run);
			else
				append_text(out, &length, 255, run == 4 ? day_names[f[UCAL_DAY_OF_WEEK]] : run == 5 ? day_narrow[f[UCAL_DAY_OF_WEEK]] :
					run == 6 ? day_shorter[f[UCAL_DAY_OF_WEEK]] : day_short[f[UCAL_DAY_OF_WEEK]]);
			break;
		case 'a': append_text(out, &length, 255, am_pm[f[UCAL_AM_PM]]); break;
		case 'h': append_number(out, &length, 255, f[UCAL_HOUR] ? f[UCAL_HOUR] : 12, run); break;
		case 'H': append_number(out, &length, 255, f[UCAL_HOUR_OF_DAY], run); break;
		case 'K': append_number(out, &length, 255, f[UCAL_HOUR], run); break;
		case 'k': append_number(out, &length, 255, f[UCAL_HOUR_OF_DAY] ? f[UCAL_HOUR_OF_DAY] : 24, run); break;
		case 'm': append_number(out, &length, 255, f[UCAL_MINUTE], run); break;
		case 's': append_number(out, &length, 255, f[UCAL_SECOND], run); break;
		case 'S': {
			INT64 fraction = f[UCAL_MILLISECOND];
			INT digits = run > 3 ? 3 : run;
			for (INT drop = 3; drop > digits; drop--)
				fraction /= 10;
			append_number(out, &length, 255, fraction, digits);
			for (INT extra = 3; extra < run && length < 255; extra++)
				out[length++] = '0';
			break;
		}
		case 'w': append_number(out, &length, 255, f[UCAL_WEEK_OF_YEAR], run); break;
		case 'W': append_number(out, &length, 255, f[UCAL_WEEK_OF_MONTH], run); break;
		case 'F': append_number(out, &length, 255, f[UCAL_DAY_OF_WEEK_IN_MONTH], run); break;
		case 'Q': case 'q':
			if (run >= 4)
				append_text(out, &length, 255, quarters[f[UCAL_MONTH] / 3]);
			else if (run == 3)
				append_text(out, &length, 255, quarters_short[f[UCAL_MONTH] / 3]);
			else
				append_number(out, &length, 255, f[UCAL_MONTH] / 3 + 1, run);
			break;
		case 'z': case 'v': case 'V': case 'O': case 'Z': case 'X': case 'x': {
			UChar name[64];
			INT count;
			// A zone with no names here reads as its city where one is asked, otherwise as its offset from GMT.
			if (letter == 'V' && run >= 3 && calendar->zone[0]) {
				count = city_of(calendar->zone, name, 48);
				if (run == 4)
					count += utf8_to_units(" Time", name + count, 16);
			} else if (letter == 'v' && run == 4 && calendar->zone[0] && !calendar->local_zone) {
				count = city_of(calendar->zone, name, 48);
				count += utf8_to_units(" Time", name + count, 16);
			} else if ((letter == 'z' || letter == 'v') && run == 4 && calendar->local_zone) {
				count = ucal_getTimeZoneDisplayName(calendar, f[UCAL_DST_OFFSET] ? UCAL_DST : UCAL_STANDARD, NULL, name, 64, status);
			} else
				count = gmt_name(f[UCAL_ZONE_OFFSET] + f[UCAL_DST_OFFSET], name);
			append_units(out, &length, 255, name, count);
			break;
		}
		default: break;
		}
	}
	return finish_units(dest, capacity, out, length, status);
}

/* Patterns for a skeleton, the fields a format should show in the order the language puts them. */

typedef struct UDateTimePatternGenerator {
	INT unused;
} UDateTimePatternGenerator;

UDateTimePatternGenerator *__cdecl udatpg_open(const char *locale, UErrorCode *status)
{
	(void)locale;
	if (U_FAILED(*status))
		return NULL;
	UDateTimePatternGenerator *generator = icu_alloc(sizeof *generator);
	if (!generator)
		*status = U_MEMORY_ALLOCATION_ERROR;
	return generator;
}

void __cdecl udatpg_close(UDateTimePatternGenerator *generator)
{
	icu_free(generator);
}

INT __cdecl udatpg_getBestPattern(UDateTimePatternGenerator *generator, const UChar *skeleton, INT length, UChar *dest, INT capacity,
	UErrorCode *status)
{
	static const struct { const char *skeleton, *pattern; } patterns[] = {
		{"MMMMd", "MMMM d"}, {"MMMd", "MMM d"}, {"Md", "M/d"}, {"yMd", "M/d/y"}, {"yMMMd", "MMM d, y"}, {"yMMMMd", "MMMM d, y"},
		{"yMMMM", "MMMM y"}, {"yMMM", "MMM y"}, {"yM", "M/y"}, {"y", "y"}, {"MMMM", "LLLL"}, {"MMM", "LLL"}, {"d", "d"},
		{"Hm", "HH:mm"}, {"hm", "h:mm a"}, {"Hms", "HH:mm:ss"}, {"hms", "h:mm:ss a"}, {"yMEd", "EEE, M/d/y"}, {"yMMMEd", "EEE, MMM d, y"},
		{"MEd", "EEE, M/d"}, {"MMMEd", "EEE, MMM d"}, {"yMMMMEEEEd", "EEEE, MMMM d, y"},
	};
	(void)generator;
	if (U_FAILED(*status))
		return 0;
	INT count = icu_length(skeleton, length);
	char text[32];
	if (count > 31)
		count = 31;
	for (INT at = 0; at < count; at++)
		text[at] = (char)skeleton[at];
	text[count] = 0;
	for (INT index = 0; index < (INT)ARRAYSIZE(patterns); index++)
		if (!lstrcmpA(patterns[index].skeleton, text))
			return finish_ascii(dest, capacity, patterns[index].pattern, status);
	return finish_units(dest, capacity, skeleton, count, status);
}
