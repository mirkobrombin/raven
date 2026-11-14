/* SPDX-License-Identifier: MIT */
#include "internal.h"

/* Collation is the system's: the order of a locale is what CompareStringEx gives, a sort key what LCMapStringEx gives, a match what
 * FindNLSStringEx finds, under the flags the collator's attributes name. */

enum { UCOL_DEFAULT = -1, UCOL_PRIMARY = 0, UCOL_SECONDARY = 1, UCOL_TERTIARY = 2, UCOL_QUATERNARY = 3, UCOL_IDENTICAL = 15,
	UCOL_OFF = 16, UCOL_ON = 17, UCOL_SHIFTED = 20, UCOL_NON_IGNORABLE = 21 };
enum { UCOL_FRENCH_COLLATION, UCOL_ALTERNATE_HANDLING, UCOL_CASE_FIRST, UCOL_CASE_LEVEL, UCOL_NORMALIZATION_MODE, UCOL_STRENGTH,
	UCOL_HIRAGANA_QUATERNARY_MODE, UCOL_NUMERIC_COLLATION };

#define UCOL_NULLORDER (-1)
#define USEARCH_DONE (-1)
#define SORT_DIGITSASNUMBERS_FLAG 0x8
#define FIND_FROMSTART_FLAG 0x400000
#define FIND_FROMEND_FLAG 0x800000

typedef struct UCollator {
	WCHAR locale[LOCALE_NAME_MAX_LENGTH];
	INT strength;
	BOOL shifted;
	BOOL case_level;
	BOOL numeric;
	BOOL ignore_kana;
	BOOL ignore_width;
	UChar *rules;
	INT rules_length;
} UCollator;

// The flags that make the system's comparison follow the collator: a strength below tertiary ignores case unless the case level is on,
// primary also ignores accents; shifted symbols are ignored; numeric collation orders digits as numbers.
static DWORD compare_flags(const UCollator *collator)
{
	DWORD flags = 0;
	if (collator->strength < UCOL_TERTIARY && !collator->case_level)
		flags |= NORM_IGNORECASE;
	if (collator->strength == UCOL_PRIMARY)
		flags |= NORM_IGNORENONSPACE;
	if (collator->shifted)
		flags |= NORM_IGNORESYMBOLS;
	if (collator->numeric)
		flags |= SORT_DIGITSASNUMBERS_FLAG;
	if (collator->ignore_kana)
		flags |= NORM_IGNOREKANATYPE;
	if (collator->ignore_width)
		flags |= NORM_IGNOREWIDTH;
	return flags;
}

static UCollator *new_collator(const char *locale, UErrorCode *status)
{
	if (U_FAILED(*status))
		return NULL;
	UCollator *collator = icu_alloc(sizeof *collator);
	if (!collator) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	UINT at = 0;
	for (; locale && locale[at] && locale[at] != '@' && at + 1 < LOCALE_NAME_MAX_LENGTH; at++)
		collator->locale[at] = locale[at] == '_' ? '-' : locale[at];
	collator->strength = UCOL_TERTIARY;
	return collator;
}

UCollator *__cdecl ucol_open(const char *locale, UErrorCode *status)
{
	UCollator *collator = new_collator(locale, status);
	if (collator && locale && locale[0] && !icu_known_locale(locale))
		*status = U_USING_DEFAULT_WARNING;
	return collator;
}

void __cdecl ucol_close(UCollator *collator)
{
	if (!collator)
		return;
	icu_free(collator->rules);
	icu_free(collator);
}

UCollator *__cdecl ucol_clone(const UCollator *collator, UErrorCode *status)
{
	if (U_FAILED(*status))
		return NULL;
	UCollator *copy = icu_alloc(sizeof *copy);
	if (!copy) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	CopyMemory(copy, collator, sizeof *copy);
	copy->rules = NULL;
	if (collator->rules) {
		copy->rules = icu_alloc((collator->rules_length + 1) * sizeof(UChar));
		if (!copy->rules) {
			icu_free(copy);
			*status = U_MEMORY_ALLOCATION_ERROR;
			return NULL;
		}
		CopyMemory(copy->rules, collator->rules, (collator->rules_length + 1) * sizeof(UChar));
	}
	return copy;
}

// The buffer the caller may offer is not used: the clone is always allocated, as ICU does when the buffer is too small or absent.
UCollator *__cdecl ucol_safeClone(const UCollator *collator, void *buffer, INT *size, UErrorCode *status)
{
	(void)buffer;
	if (size && *size == 0) {
		*size = 1;
		return NULL;
	}
	return ucol_clone(collator, status);
}

// A locale's own rules tailor the root order; the order here is the system's, so no locale adds rules of its own.
const UChar *__cdecl ucol_getRules(const UCollator *collator, INT *length)
{
	static const UChar none[1] = {0};
	if (length)
		*length = collator->rules ? collator->rules_length : 0;
	return collator->rules ? collator->rules : none;
}

// Rules are kept to be given back, not compiled. Of what they can say, the system's order follows two relations programs state for
// whole sets: hiragana and katakana made equal ("&ぁ=ァ") or kept apart ("&ぁ<ァ"), and half and full width forms the same way.
UCollator *__cdecl ucol_openRules(const UChar *rules, INT rules_length, INT normalization, INT strength, void *parse_error, UErrorCode *status)
{
	(void)normalization;
	(void)parse_error;
	UCollator *collator = new_collator(NULL, status);
	if (!collator)
		return NULL;
	INT length = rules ? icu_length(rules, rules_length) : 0;
	collator->rules = icu_alloc((length + 1) * sizeof(UChar));
	if (!collator->rules) {
		icu_free(collator);
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	CopyMemory(collator->rules, rules, length * sizeof(UChar));
	collator->rules[length] = 0;
	collator->rules_length = length;
	collator->strength = strength == UCOL_DEFAULT ? UCOL_TERTIARY : strength;
	for (INT at = 0; at + 3 < length; at++) {
		if (rules[at] != '&')
			continue;
		INT first = at + 1;
		if (rules[first] == '\\')
			first++;
		if (first + 2 >= length || rules[first + 1] != '=')
			continue;
		UChar lower = rules[first], higher = rules[first + 2];
		if (lower >= 0x3041 && lower <= 0x3096 && higher == lower + 0x60)
			collator->ignore_kana = TRUE;
		else if (higher >= 0xff01 && higher <= 0xffef)
			collator->ignore_width = TRUE;
	}
	return collator;
}

INT __cdecl ucol_getStrength(const UCollator *collator)
{
	return collator->strength;
}

void __cdecl ucol_setStrength(UCollator *collator, INT strength)
{
	collator->strength = strength == UCOL_DEFAULT ? UCOL_TERTIARY : strength;
}

void __cdecl ucol_setAttribute(UCollator *collator, INT attribute, INT value, UErrorCode *status)
{
	if (U_FAILED(*status))
		return;
	switch (attribute) {
	case UCOL_STRENGTH: ucol_setStrength(collator, value); break;
	case UCOL_ALTERNATE_HANDLING: collator->shifted = value == UCOL_SHIFTED; break;
	case UCOL_CASE_LEVEL: collator->case_level = value == UCOL_ON; break;
	case UCOL_NUMERIC_COLLATION: collator->numeric = value == UCOL_ON; break;
	case UCOL_FRENCH_COLLATION: case UCOL_CASE_FIRST: case UCOL_NORMALIZATION_MODE: case UCOL_HIRAGANA_QUATERNARY_MODE: break;
	default: *status = U_ILLEGAL_ARGUMENT_ERROR; break;
	}
}

INT __cdecl ucol_getAttribute(const UCollator *collator, INT attribute, UErrorCode *status)
{
	if (U_FAILED(*status))
		return UCOL_DEFAULT;
	switch (attribute) {
	case UCOL_STRENGTH: return collator->strength;
	case UCOL_ALTERNATE_HANDLING: return collator->shifted ? UCOL_SHIFTED : UCOL_NON_IGNORABLE;
	case UCOL_CASE_LEVEL: return collator->case_level ? UCOL_ON : UCOL_OFF;
	case UCOL_NUMERIC_COLLATION: return collator->numeric ? UCOL_ON : UCOL_OFF;
	case UCOL_FRENCH_COLLATION: case UCOL_CASE_FIRST: case UCOL_NORMALIZATION_MODE: case UCOL_HIRAGANA_QUATERNARY_MODE: return UCOL_OFF;
	default: *status = U_ILLEGAL_ARGUMENT_ERROR; return UCOL_DEFAULT;
	}
}

// Characters up to a reorder group are variable when shifted; the system's symbol flag covers punctuation and symbols alike.
void __cdecl ucol_setMaxVariable(UCollator *collator, INT group, UErrorCode *status)
{
	(void)collator;
	(void)group;
	(void)status;
}

void __cdecl ucol_getVersion(const UCollator *collator, BYTE version[4])
{
	(void)collator;
	version[0] = 0x99;
	version[1] = 0x40;
	version[2] = 0x2a;
	version[3] = 0x01;
}

INT __cdecl ucol_strcoll(const UCollator *collator, const UChar *left, INT left_length, const UChar *right, INT right_length)
{
	INT order = CompareStringEx(collator->locale, compare_flags(collator), left, left_length, right, right_length, NULL, NULL, 0);
	return order == CSTR_LESS_THAN ? -1 : order == CSTR_GREATER_THAN ? 1 : 0;
}

// The whole key's length is returned, its closing zero included; a buffer too small receives as much of the key as fits.
INT __cdecl ucol_getSortKey(const UCollator *collator, const UChar *source, INT length, BYTE *result, INT capacity)
{
	DWORD flags = LCMAP_SORTKEY | compare_flags(collator);
	INT count = icu_length(source, length);
	INT needed = LCMapStringEx(collator->locale, flags, source, count, NULL, 0, NULL, NULL, 0);
	if (needed <= 0) {
		if (result && capacity > 0)
			result[0] = 0;
		return 1;
	}
	if (!result || capacity <= 0)
		return needed;
	if (capacity >= needed) {
		LCMapStringEx(collator->locale, flags, source, count, (WCHAR *)result, capacity, NULL, NULL, 0);
		return needed;
	}
	BYTE *whole = icu_alloc(needed);
	if (!whole)
		return needed;
	LCMapStringEx(collator->locale, flags, source, count, (WCHAR *)whole, needed, NULL, NULL, 0);
	CopyMemory(result, whole, capacity);
	icu_free(whole);
	return needed;
}

/* Collation elements: one per character, primary weight in the high 16 bits, secondary in the next 8, tertiary in the low 8. A letter's
 * primary weight is its lower case, so case lives in the tertiary weight alone; a combining mark weighs only at the secondary level;
 * characters the order ignores weigh nothing. */

typedef struct UCollationElements {
	const UCollator *collator;
	UChar *text;
	INT length;
	INT offset;
	BOOL fresh;
} UCollationElements;

static BOOL ignorable(UChar32 c)
{
	return c == 0 || c == 0xad || (c >= 0x200b && c <= 0x200f) || c == 0x2060 || c == 0xfeff;
}

static INT element_of(UChar32 c)
{
	if (ignorable(c))
		return 0;
	if (icu_is_mark(c))
		return ((c * 7 & 0x7f) | 0x80) << 8;
	UChar32 lower = icu_lower(c);
	UINT primary = lower <= 0xffff ? (UINT)lower : 0xf000 | (lower & 0xfff);
	if (!primary)
		primary = 1;
	return (INT)(primary << 16 | 0x05 << 8 | (lower != c ? 0x8f : 0x05));
}

UCollationElements *__cdecl ucol_openElements(const UCollator *collator, const UChar *text, INT length, UErrorCode *status)
{
	if (U_FAILED(*status))
		return NULL;
	UCollationElements *elements = icu_alloc(sizeof *elements);
	INT count = text ? icu_length(text, length) : 0;
	UChar *copy = icu_alloc((count + 1) * sizeof(UChar));
	if (!elements || !copy) {
		icu_free(elements);
		icu_free(copy);
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	CopyMemory(copy, text, count * sizeof(UChar));
	copy[count] = 0;
	elements->collator = collator;
	elements->text = copy;
	elements->length = count;
	elements->fresh = TRUE;
	return elements;
}

void __cdecl ucol_closeElements(UCollationElements *elements)
{
	if (!elements)
		return;
	icu_free(elements->text);
	icu_free(elements);
}

INT __cdecl ucol_next(UCollationElements *elements, UErrorCode *status)
{
	if (U_FAILED(*status))
		return UCOL_NULLORDER;
	elements->fresh = FALSE;
	if (elements->offset >= elements->length)
		return UCOL_NULLORDER;
	INT next = elements->offset;
	UChar32 c = icu_at(elements->text, elements->length, elements->offset, &next);
	elements->offset = next;
	return element_of(c);
}

// An iterator just opened walks back from the end of its text.
INT __cdecl ucol_previous(UCollationElements *elements, UErrorCode *status)
{
	if (U_FAILED(*status))
		return UCOL_NULLORDER;
	if (elements->fresh) {
		elements->offset = elements->length;
		elements->fresh = FALSE;
	}
	if (elements->offset <= 0)
		return UCOL_NULLORDER;
	INT at = elements->offset - 1;
	if (at > 0 && elements->text[at] >= 0xdc00 && elements->text[at] <= 0xdfff && elements->text[at - 1] >= 0xd800 && elements->text[at - 1] <= 0xdbff)
		at--;
	INT next = at;
	UChar32 c = icu_at(elements->text, elements->length, at, &next);
	elements->offset = at;
	return element_of(c);
}

INT __cdecl ucol_getOffset(const UCollationElements *elements)
{
	return elements->fresh ? 0 : elements->offset;
}

/* String search: where a pattern occurs in a text as the collator compares them. */

typedef struct UStringSearch {
	const UCollator *collator;
	void *breaker;
	UChar *pattern;
	INT pattern_length;
	const UChar *text;
	INT text_length;
	INT matched;
} UStringSearch;

static BOOL keep_pattern(UStringSearch *search, const UChar *pattern, INT length, UErrorCode *status)
{
	INT count = icu_length(pattern, length);
	UChar *copy = icu_alloc((count + 1) * sizeof(UChar));
	if (!copy) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return FALSE;
	}
	CopyMemory(copy, pattern, count * sizeof(UChar));
	copy[count] = 0;
	icu_free(search->pattern);
	search->pattern = copy;
	search->pattern_length = count;
	return TRUE;
}

// The text stays the caller's, as ICU keeps a pointer to it; the break iterator stays the caller's too.
UStringSearch *__cdecl usearch_openFromCollator(const UChar *pattern, INT pattern_length, const UChar *text, INT text_length,
	const UCollator *collator, void *breaker, UErrorCode *status)
{
	if (U_FAILED(*status))
		return NULL;
	if (!pattern || !text || !collator) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return NULL;
	}
	UStringSearch *search = icu_alloc(sizeof *search);
	if (!search) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	search->collator = collator;
	search->breaker = breaker;
	search->text = text;
	search->text_length = icu_length(text, text_length);
	if (!keep_pattern(search, pattern, pattern_length, status)) {
		icu_free(search);
		return NULL;
	}
	return search;
}

void __cdecl usearch_close(UStringSearch *search)
{
	if (!search)
		return;
	icu_free(search->pattern);
	icu_free(search);
}

void __cdecl usearch_setText(UStringSearch *search, const UChar *text, INT length, UErrorCode *status)
{
	if (U_FAILED(*status))
		return;
	search->text = text;
	search->text_length = icu_length(text, length);
	search->matched = 0;
}

void __cdecl usearch_setPattern(UStringSearch *search, const UChar *pattern, INT length, UErrorCode *status)
{
	if (U_FAILED(*status))
		return;
	keep_pattern(search, pattern, length, status);
	search->matched = 0;
}

void *__cdecl usearch_getBreakIterator(const UStringSearch *search)
{
	return search->breaker;
}

static INT find(UStringSearch *search, DWORD where, UErrorCode *status)
{
	if (U_FAILED(*status))
		return USEARCH_DONE;
	INT found = 0;
	INT index = FindNLSStringEx(search->collator->locale, where | compare_flags(search->collator), search->text, search->text_length,
		search->pattern, search->pattern_length, &found, NULL, NULL, 0);
	search->matched = index < 0 ? 0 : found;
	return index < 0 ? USEARCH_DONE : index;
}

INT __cdecl usearch_first(UStringSearch *search, UErrorCode *status)
{
	return find(search, FIND_FROMSTART_FLAG, status);
}

INT __cdecl usearch_last(UStringSearch *search, UErrorCode *status)
{
	return find(search, FIND_FROMEND_FLAG, status);
}

INT __cdecl usearch_getMatchedLength(const UStringSearch *search)
{
	return search->matched;
}
