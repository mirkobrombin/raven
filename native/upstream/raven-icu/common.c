/* SPDX-License-Identifier: MIT */
#include "internal.h"

void *icu_alloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1);
}

void icu_free(void *memory)
{
	if (memory)
		HeapFree(GetProcessHeap(), 0, memory);
}

// ICU's contract for every output buffer: the needed length is returned, a short buffer is an error, and a full one is not terminated.
INT icu_finish(INT length, INT capacity, UErrorCode *status)
{
	if (U_FAILED(*status))
		return length;
	if (length > capacity)
		*status = U_BUFFER_OVERFLOW_ERROR;
	else if (length == capacity)
		*status = U_STRING_NOT_TERMINATED_WARNING;
	return length;
}

INT icu_length(const UChar *text, INT length)
{
	if (length >= 0)
		return length;
	INT count = 0;
	while (text[count])
		count++;
	return count;
}

UChar32 icu_at(const UChar *text, INT length, INT index, INT *next)
{
	UChar32 c = text[index];
	INT after = index + 1;
	if (c >= 0xd800 && c < 0xdc00 && after < length && text[after] >= 0xdc00 && text[after] < 0xe000) {
		c = 0x10000 + ((c - 0xd800) << 10) + (text[after] - 0xdc00);
		after++;
	}
	if (next)
		*next = after;
	return c;
}

static UChar32 map_one(UChar32 c, DWORD flag)
{
	WCHAR in[2], out[4];
	INT size = 1;
	if (c >= 0x10000) {
		in[0] = (WCHAR)(0xd800 + ((c - 0x10000) >> 10));
		in[1] = (WCHAR)(0xdc00 + ((c - 0x10000) & 0x3ff));
		size = 2;
	} else
		in[0] = (WCHAR)c;
	INT got = LCMapStringW(LOCALE_INVARIANT, flag, in, size, out, 4);
	if (got == 1)
		return out[0];
	if (got == 2 && out[0] >= 0xd800 && out[0] < 0xdc00)
		return 0x10000 + ((out[0] - 0xd800) << 10) + (out[1] - 0xdc00);
	return c;
}

UChar32 icu_lower(UChar32 c)
{
	if (c < 0x80)
		return c >= 'A' && c <= 'Z' ? c + 32 : c;
	return map_one(c, LCMAP_LOWERCASE);
}

UChar32 icu_upper(UChar32 c)
{
	if (c < 0x80)
		return c >= 'a' && c <= 'z' ? c - 32 : c;
	return map_one(c, LCMAP_UPPERCASE);
}

WORD icu_kind(UChar32 c)
{
	WCHAR unit = (WCHAR)c;
	WORD kind = 0;
	if (c >= 0x10000 || !GetStringTypeW(CT_CTYPE1, &unit, 1, &kind))
		return 0;
	return kind;
}

// Above the BMP the ctype answer is not available one unit at a time: the emoji and symbol blocks are not letters, the rest mostly are.
BOOL icu_is_letter(UChar32 c)
{
	if (c >= 0x10000)
		return !(c >= 0x1f000 && c <= 0x1faff) && !(c >= 0xe0000 && c <= 0xe0fff);
	return (icu_kind(c) & C1_ALPHA) != 0;
}

BOOL icu_is_digit(UChar32 c)
{
	return c < 0x10000 && (icu_kind(c) & C1_DIGIT);
}

BOOL icu_is_space(UChar32 c)
{
	return c < 0x10000 && (icu_kind(c) & (C1_SPACE | C1_BLANK));
}

BOOL icu_is_word(UChar32 c)
{
	return c == '_' || icu_is_letter(c) || icu_is_digit(c) || icu_is_mark(c);
}

typedef struct {
	UChar32 low, high;
} code_range;

static const code_range mark_ranges[] = {
	{0x300, 0x36f}, {0x483, 0x489}, {0x591, 0x5bd}, {0x5bf, 0x5bf}, {0x5c1, 0x5c2}, {0x5c4, 0x5c5}, {0x610, 0x61a}, {0x64b, 0x65f},
	{0x670, 0x670}, {0x6d6, 0x6dc}, {0x6df, 0x6e4}, {0x900, 0x903}, {0x93a, 0x93c}, {0x93e, 0x94f}, {0xe31, 0xe31}, {0xe34, 0xe3a},
	{0xe47, 0xe4e}, {0x1ab0, 0x1aff}, {0x1dc0, 0x1dff}, {0x200c, 0x200d}, {0x20d0, 0x20f0}, {0x302a, 0x302f}, {0x3099, 0x309a},
	{0xfe00, 0xfe0f}, {0xfe20, 0xfe2f}, {0x1f3fb, 0x1f3ff}, {0xe0020, 0xe007f}, {0xe0100, 0xe01ef},
};

BOOL icu_is_mark(UChar32 c)
{
	for (UINT i = 0; i < ARRAYSIZE(mark_ranges); i++)
		if (c >= mark_ranges[i].low && c <= mark_ranges[i].high)
			return TRUE;
	return FALSE;
}

/* UAX #11: only the two classes a terminal cell count depends on are complete, and the blocks are given whole. */
static const code_range wide_ranges[] = {
	{0x1100, 0x115f}, {0x231a, 0x231b}, {0x2329, 0x232a}, {0x23e9, 0x23ec}, {0x23f0, 0x23f0}, {0x23f3, 0x23f3}, {0x25fd, 0x25fe},
	{0x2614, 0x2615}, {0x2648, 0x2653}, {0x267f, 0x267f}, {0x2693, 0x2693}, {0x26a1, 0x26a1}, {0x26aa, 0x26ab}, {0x26bd, 0x26be},
	{0x26c4, 0x26c5}, {0x26ce, 0x26ce}, {0x26d4, 0x26d4}, {0x26ea, 0x26ea}, {0x26f2, 0x26f3}, {0x26f5, 0x26f5}, {0x26fa, 0x26fa},
	{0x26fd, 0x26fd}, {0x2705, 0x2705}, {0x270a, 0x270b}, {0x2728, 0x2728}, {0x274c, 0x274c}, {0x274e, 0x274e}, {0x2753, 0x2755},
	{0x2757, 0x2757}, {0x2795, 0x2797}, {0x27b0, 0x27b0}, {0x27bf, 0x27bf}, {0x2b1b, 0x2b1c}, {0x2b50, 0x2b50}, {0x2b55, 0x2b55},
	{0x2e80, 0x303e}, {0x3041, 0x33ff}, {0x3400, 0x4dbf}, {0x4e00, 0xa4c6}, {0xa960, 0xa97c}, {0xac00, 0xd7a3}, {0xf900, 0xfaff},
	{0xfe10, 0xfe19}, {0xfe30, 0xfe6b}, {0x16fe0, 0x18aff}, {0x1b000, 0x1b2ff}, {0x1f004, 0x1f004}, {0x1f0cf, 0x1f0cf}, {0x1f18e, 0x1f18e},
	{0x1f191, 0x1f19a}, {0x1f200, 0x1f265}, {0x1f300, 0x1f64f}, {0x1f680, 0x1f6ff}, {0x1f900, 0x1f9ff}, {0x1fa70, 0x1faff}, {0x20000, 0x3fffd},
};

#define UCHAR_EAST_ASIAN_WIDTH 0x1004
enum { EA_NEUTRAL, EA_AMBIGUOUS, EA_HALFWIDTH, EA_FULLWIDTH, EA_NARROW, EA_WIDE };

static INT east_asian_width(UChar32 c)
{
	if ((c >= 0xff01 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6))
		return EA_FULLWIDTH;
	if ((c >= 0xff61 && c <= 0xffdc) || (c >= 0xffe8 && c <= 0xffee) || c == 0x20a9)
		return EA_HALFWIDTH;
	if (c >= 0x20 && c <= 0x7e)
		return EA_NARROW;
	for (UINT i = 0; i < ARRAYSIZE(wide_ranges); i++)
		if (c >= wide_ranges[i].low && c <= wide_ranges[i].high)
			return EA_WIDE;
	return EA_NEUTRAL;
}

INT __cdecl u_getIntPropertyValue(UChar32 c, INT which)
{
	return which == UCHAR_EAST_ASIAN_WIDTH ? east_asian_width(c) : 0;
}

UChar32 __cdecl u_tolower(UChar32 c)
{
	return icu_lower(c);
}

UChar32 __cdecl u_toupper(UChar32 c)
{
	return icu_upper(c);
}

UChar32 __cdecl u_foldCase(UChar32 c, UINT options)
{
	(void)options;
	return c == 0x3c2 ? 0x3c3 : c == 0xb5 ? 0x3bc : c == 0x17f ? 's' : icu_lower(c);
}

INT __cdecl u_strlen(const UChar *text)
{
	return icu_length(text, -1);
}

const char *__cdecl u_errorName(UErrorCode code)
{
	switch (code) {
	case U_ZERO_ERROR: return "U_ZERO_ERROR";
	case U_STRING_NOT_TERMINATED_WARNING: return "U_STRING_NOT_TERMINATED_WARNING";
	case U_ILLEGAL_ARGUMENT_ERROR: return "U_ILLEGAL_ARGUMENT_ERROR";
	case U_MEMORY_ALLOCATION_ERROR: return "U_MEMORY_ALLOCATION_ERROR";
	case U_INDEX_OUTOFBOUNDS_ERROR: return "U_INDEX_OUTOFBOUNDS_ERROR";
	case U_INVALID_CHAR_FOUND: return "U_INVALID_CHAR_FOUND";
	case U_BUFFER_OVERFLOW_ERROR: return "U_BUFFER_OVERFLOW_ERROR";
	case U_UNSUPPORTED_ERROR: return "U_UNSUPPORTED_ERROR";
	case U_REGEX_RULE_SYNTAX: return "U_REGEX_RULE_SYNTAX";
	case U_REGEX_INVALID_STATE: return "U_REGEX_INVALID_STATE";
	case U_REGEX_BAD_ESCAPE_SEQUENCE: return "U_REGEX_BAD_ESCAPE_SEQUENCE";
	case U_REGEX_UNIMPLEMENTED: return "U_REGEX_UNIMPLEMENTED";
	case U_REGEX_MISMATCHED_PAREN: return "U_REGEX_MISMATCHED_PAREN";
	case U_REGEX_TIME_OUT: return "U_REGEX_TIME_OUT";
	default: return "[BOGUS UErrorCode]";
	}
}

/* Case mapping. Full mappings that change length are the ones the Unicode tables list for these characters. */

typedef enum { MAP_LOWER, MAP_UPPER, MAP_FOLD } map_kind;

static const struct { UChar32 c; const WCHAR *upper; } expanding[] = {
	{0xdf, L"SS"}, {0x149, L"\x2bcN"}, {0x1f0, L"J\x30c"}, {0xfb00, L"FF"}, {0xfb01, L"FI"}, {0xfb02, L"FL"}, {0xfb03, L"FFI"}, {0xfb04, L"FFL"}, {0xfb05, L"ST"}, {0xfb06, L"ST"},
};

static const WCHAR *expansion(UChar32 c)
{
	for (UINT i = 0; i < ARRAYSIZE(expanding); i++)
		if (expanding[i].c == c)
			return expanding[i].upper;
	return NULL;
}

static void put_unit(UChar *dest, INT capacity, INT *count, WCHAR unit)
{
	if (*count < capacity)
		dest[*count] = unit;
	(*count)++;
}

static void put_char(UChar *dest, INT capacity, INT *count, UChar32 c)
{
	if (c >= 0x10000) {
		put_unit(dest, capacity, count, (WCHAR)(0xd800 + ((c - 0x10000) >> 10)));
		put_unit(dest, capacity, count, (WCHAR)(0xdc00 + ((c - 0x10000) & 0x3ff)));
	} else
		put_unit(dest, capacity, count, (WCHAR)c);
}

static INT map_text(map_kind kind, UChar *dest, INT capacity, const UChar *source, INT length, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	if (!source || capacity < 0 || (capacity && !dest)) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return 0;
	}
	length = icu_length(source, length);
	INT count = 0;
	for (INT i = 0; i < length;) {
		INT next;
		UChar32 c = icu_at(source, length, i, &next);
		const WCHAR *longer = kind != MAP_LOWER ? expansion(c == 0x1e9e && kind == MAP_FOLD ? 0xdf : c) : NULL;
		if (longer) {
			for (; *longer; longer++)
				put_char(dest, capacity, &count, kind == MAP_FOLD ? icu_lower(*longer) : *longer);
		} else if (kind == MAP_LOWER)
			put_char(dest, capacity, &count, icu_lower(c));
		else if (kind == MAP_UPPER)
			put_char(dest, capacity, &count, icu_upper(c));
		else
			put_char(dest, capacity, &count, u_foldCase(c, 0));
		i = next;
	}
	if (count < capacity)
		dest[count] = 0;
	return icu_finish(count, capacity, status);
}

INT __cdecl u_strToLower(UChar *dest, INT capacity, const UChar *source, INT length, const char *locale, UErrorCode *status)
{
	(void)locale;
	return map_text(MAP_LOWER, dest, capacity, source, length, status);
}

INT __cdecl u_strToUpper(UChar *dest, INT capacity, const UChar *source, INT length, const char *locale, UErrorCode *status)
{
	(void)locale;
	return map_text(MAP_UPPER, dest, capacity, source, length, status);
}

INT __cdecl u_strFoldCase(UChar *dest, INT capacity, const UChar *source, INT length, UINT options, UErrorCode *status)
{
	(void)options;
	return map_text(MAP_FOLD, dest, capacity, source, length, status);
}

typedef struct UCaseMap {
	UINT options;
} UCaseMap;

UCaseMap *__cdecl ucasemap_open(const char *locale, UINT options, UErrorCode *status)
{
	(void)locale;
	if (U_FAILED(*status))
		return NULL;
	UCaseMap *map = icu_alloc(sizeof *map);
	if (!map) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	map->options = options;
	return map;
}

void __cdecl ucasemap_close(UCaseMap *map)
{
	icu_free(map);
}

static INT map_utf8(map_kind kind, char *dest, INT capacity, const char *source, INT length, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	if (!source || capacity < 0 || (capacity && !dest)) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return 0;
	}
	if (length < 0)
		length = lstrlenA(source);
	if (!length)
		return icu_finish(0, capacity, status);
	INT wide = MultiByteToWideChar(CP_UTF8, 0, source, length, NULL, 0);
	UChar *in = icu_alloc(wide * sizeof(UChar));
	UErrorCode inner = U_ZERO_ERROR;
	INT mapped = 0;
	MultiByteToWideChar(CP_UTF8, 0, source, length, in, wide);
	mapped = map_text(kind, NULL, 0, in, wide, &inner);
	UChar *out = icu_alloc((mapped + 1) * sizeof(UChar));
	inner = U_ZERO_ERROR;
	map_text(kind, out, mapped + 1, in, wide, &inner);
	INT bytes = WideCharToMultiByte(CP_UTF8, 0, out, mapped, NULL, 0, NULL, NULL);
	if (bytes <= capacity)
		WideCharToMultiByte(CP_UTF8, 0, out, mapped, dest, capacity, NULL, NULL);
	if (bytes < capacity)
		dest[bytes] = 0;
	icu_free(in);
	icu_free(out);
	return icu_finish(bytes, capacity, status);
}

INT __cdecl ucasemap_utf8FoldCase(const UCaseMap *map, char *dest, INT capacity, const char *source, INT length, UErrorCode *status)
{
	(void)map;
	return map_utf8(MAP_FOLD, dest, capacity, source, length, status);
}

INT __cdecl ucasemap_utf8ToLower(const UCaseMap *map, char *dest, INT capacity, const char *source, INT length, UErrorCode *status)
{
	(void)map;
	return map_utf8(MAP_LOWER, dest, capacity, source, length, status);
}

INT __cdecl ucasemap_utf8ToUpper(const UCaseMap *map, char *dest, INT capacity, const char *source, INT length, UErrorCode *status)
{
	(void)map;
	return map_utf8(MAP_UPPER, dest, capacity, source, length, status);
}

/* Converters by name, on the system's code pages. No name means the system's default. */

static BOOL same_name(const char *a, const char *b)
{
	for (; *a || *b; a++, b++) {
		char x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a, y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
		if (x != y)
			return FALSE;
	}
	return TRUE;
}

#define PAGE_UTF16 1200
#define PAGE_UNKNOWN 0xffffffff

static UINT page_of(const char *name)
{
	if (!name || !*name)
		return CP_ACP;
	if (same_name(name, "UTF-8") || same_name(name, "UTF8") || same_name(name, "CP65001"))
		return CP_UTF8;
	if (same_name(name, "UTF-16") || same_name(name, "UTF-16LE") || same_name(name, "UTF16_PlatformEndian") || same_name(name, "UTF16LE"))
		return PAGE_UTF16;
	if (same_name(name, "US-ASCII") || same_name(name, "ASCII"))
		return 20127;
	if (same_name(name, "ISO-8859-1") || same_name(name, "LATIN1"))
		return 28591;
	const char *digits = name;
	while (*digits && (*digits < '0' || *digits > '9'))
		digits++;
	if (!*digits)
		return PAGE_UNKNOWN;
	UINT page = 0;
	for (; *digits >= '0' && *digits <= '9'; digits++)
		page = page * 10 + (*digits - '0');
	return *digits ? PAGE_UNKNOWN : page;
}

INT __cdecl ucnv_convert(const char *to_name, const char *from_name, char *target, INT capacity, const char *source, INT length, UErrorCode *status)
{
	if (U_FAILED(*status))
		return 0;
	if (!source || capacity < 0 || (capacity && !target) || length < -1) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return 0;
	}
	UINT to = page_of(to_name), from = page_of(from_name);
	if (to == PAGE_UNKNOWN || from == PAGE_UNKNOWN) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return 0;
	}
	if (length < 0)
		length = from == PAGE_UTF16 ? icu_length((const UChar *)source, -1) * 2 : lstrlenA(source);
	if (!length)
		return icu_finish(0, capacity, status);
	INT units = from == PAGE_UTF16 ? length / 2 : MultiByteToWideChar(from, 0, source, length, NULL, 0);
	UChar *wide = icu_alloc(units * sizeof(UChar));
	if (from == PAGE_UTF16)
		CopyMemory(wide, source, units * sizeof(UChar));
	else
		MultiByteToWideChar(from, 0, source, length, wide, units);
	INT bytes;
	if (to == PAGE_UTF16) {
		bytes = units * 2;
		if (bytes <= capacity)
			CopyMemory(target, wide, bytes);
	} else {
		bytes = WideCharToMultiByte(to, 0, wide, units, NULL, 0, NULL, NULL);
		if (bytes <= capacity)
			WideCharToMultiByte(to, 0, wide, units, target, capacity, NULL, NULL);
	}
	if (bytes < capacity)
		target[bytes] = 0;
	icu_free(wide);
	return icu_finish(bytes, capacity, status);
}

/* Collation is the system's: the order of a locale is what CompareStringEx gives. */

typedef struct UCollator {
	WCHAR locale[LOCALE_NAME_MAX_LENGTH];
	DWORD flags;
} UCollator;

enum { UCOL_PRIMARY = 0, UCOL_SECONDARY = 1, UCOL_TERTIARY = 2 };

UCollator *__cdecl ucol_open(const char *locale, UErrorCode *status)
{
	if (U_FAILED(*status))
		return NULL;
	UCollator *collator = icu_alloc(sizeof *collator);
	if (!collator) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	UINT at = 0;
	for (; locale && locale[at] && at + 1 < LOCALE_NAME_MAX_LENGTH; at++)
		collator->locale[at] = locale[at] == '_' ? '-' : locale[at];
	return collator;
}

void __cdecl ucol_close(UCollator *collator)
{
	icu_free(collator);
}

void __cdecl ucol_setStrength(UCollator *collator, INT strength)
{
	collator->flags = strength == UCOL_PRIMARY ? NORM_IGNORECASE | NORM_IGNORENONSPACE : strength == UCOL_SECONDARY ? NORM_IGNORECASE : 0;
}

INT __cdecl ucol_strcoll(const UCollator *collator, const UChar *left, INT left_length, const UChar *right, INT right_length)
{
	INT order = CompareStringEx(collator->locale, collator->flags, left, left_length, right, right_length, NULL, NULL, 0);
	return order == CSTR_LESS_THAN ? -1 : order == CSTR_GREATER_THAN ? 1 : 0;
}
