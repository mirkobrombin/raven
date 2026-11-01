/* SPDX-License-Identifier: MIT */
#include "internal.h"

/* Break iteration by the rules of UAX #29 and, loosely, UAX #14. The boundaries are found when the text is set; the iterator only
 * walks them. */

enum { UBRK_CHARACTER, UBRK_WORD, UBRK_LINE, UBRK_SENTENCE, UBRK_TITLE };
enum { STATUS_NONE = 0, STATUS_NUMBER = 100, STATUS_LETTER = 200, STATUS_KANA = 300, STATUS_IDEO = 400 };
#define UBRK_DONE (-1)

typedef struct UBreakIterator {
	INT type;
	UText *text;
	INT *stops;
	INT *statuses;
	INT stop_count;
	INT at;
} UBreakIterator;

static BOOL is_kana(UChar32 c)
{
	return (c >= 0x3040 && c <= 0x30ff) || (c >= 0xff66 && c <= 0xff9f);
}

static BOOL is_ideograph(UChar32 c)
{
	return (c >= 0x3400 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0x20000 && c <= 0x3fffd);
}

static BOOL is_regional(UChar32 c)
{
	return c >= 0x1f1e6 && c <= 0x1f1ff;
}

static BOOL is_pictographic(UChar32 c)
{
	return (c >= 0x1f000 && c <= 0x1faff) || (c >= 0x2600 && c <= 0x27bf) || c == 0xa9 || c == 0xae;
}

// Extended grapheme clusters: the Hangul syllable rules, marks, joined emoji and flag pairs.
static INT cluster_end(const UChar *text, INT length, INT from)
{
	INT at;
	UChar32 c = icu_at(text, length, from, &at);
	if (c == '\r' && at < length && text[at] == '\n')
		return at + 1;
	if (c == '\r' || c == '\n' || c < 0x20)
		return at;
	if (is_regional(c) && at < length) {
		INT after;
		if (is_regional(icu_at(text, length, at, &after)))
			at = after;
	}
	while (at < length) {
		INT after;
		UChar32 next = icu_at(text, length, at, &after);
		BOOL hangul = (c >= 0x1100 && c <= 0x115f && ((next >= 0x1100 && next <= 0x11a7) || (next >= 0xac00 && next <= 0xd7a3))) ||
			(((c >= 0x1160 && c <= 0x11a7) || (c >= 0xac00 && c <= 0xd7a3 && (c - 0xac00) % 28 == 0)) && next >= 0x1160 && next <= 0x11ff) ||
			(((c >= 0x11a8 && c <= 0x11ff) || (c >= 0xac00 && c <= 0xd7a3)) && next >= 0x11a8 && next <= 0x11ff);
		if (icu_is_mark(next) || hangul) {
			c = next;
			at = after;
			if (next == 0x200d && after < length) {
				INT beyond;
				UChar32 joined = icu_at(text, length, after, &beyond);
				if (is_pictographic(joined)) {
					c = joined;
					at = beyond;
				}
			}
			continue;
		}
		break;
	}
	return at;
}

static BOOL joins_letters(UChar32 c)
{
	return c == '\'' || c == '.' || c == 0x2019 || c == ':' || c == 0xb7 || c == 0x2018;
}

static BOOL joins_digits(UChar32 c)
{
	return c == ',' || c == '.' || c == '\'' || c == ';' || c == 0x2019;
}

static INT skip_marks(const UChar *text, INT length, INT at)
{
	while (at < length) {
		INT after;
		if (!icu_is_mark(icu_at(text, length, at, &after)))
			break;
		at = after;
	}
	return at;
}

static INT word_end(const UChar *text, INT length, INT from, INT *status)
{
	INT at;
	UChar32 c = icu_at(text, length, from, &at);
	*status = STATUS_NONE;
	if (c == '\r' && at < length && text[at] == '\n')
		return at + 1;
	if (c == '\r' || c == '\n')
		return at;
	if (icu_is_space(c)) {
		while (at < length) {
			INT after;
			UChar32 next = icu_at(text, length, at, &after);
			if (!icu_is_space(next) || next == '\r' || next == '\n')
				break;
			at = after;
		}
		return at;
	}
	if (is_ideograph(c)) {
		*status = STATUS_IDEO;
		return skip_marks(text, length, at);
	}
	if (is_kana(c)) {
		*status = STATUS_KANA;
		while (at < length) {
			INT after;
			UChar32 next = icu_at(text, length, at, &after);
			if (!is_kana(next) && !icu_is_mark(next))
				break;
			at = after;
		}
		return at;
	}
	BOOL letter = icu_is_letter(c), digit = icu_is_digit(c);
	if (!letter && !digit && c != '_')
		return cluster_end(text, length, from);
	*status = letter ? STATUS_LETTER : STATUS_NUMBER;
	while (at < length) {
		INT after;
		UChar32 next = icu_at(text, length, at, &after);
		if (is_ideograph(next) || is_kana(next))
			break;
		BOOL next_letter = icu_is_letter(next), next_digit = icu_is_digit(next);
		if (next_letter || next_digit || next == '_' || icu_is_mark(next)) {
			if (next_letter)
				*status = STATUS_LETTER;
			at = after;
			continue;
		}
		// One joining character stays inside the word only between two of the same kind: can't, 3.14.
		if (after < length) {
			INT beyond;
			UChar32 then = icu_at(text, length, after, &beyond);
			if (joins_letters(next) && icu_is_letter(c) && icu_is_letter(then)) {
				c = then;
				at = beyond;
				continue;
			}
			if (joins_digits(next) && icu_is_digit(text[at - 1]) && icu_is_digit(then)) {
				at = beyond;
				continue;
			}
		}
		break;
	}
	return at;
}

// A line may break after spaces and after a hyphen inside a word, and around ideographs.
static INT line_end(const UChar *text, INT length, INT from)
{
	INT at = from;
	while (at < length) {
		INT after;
		UChar32 c = icu_at(text, length, at, &after);
		if (c == '\r' && after < length && text[after] == '\n')
			return after + 1;
		if (c == '\r' || c == '\n' || c == 0x2028 || c == 0x2029)
			return after;
		if (icu_is_space(c)) {
			at = after;
			while (at < length) {
				UChar32 next = icu_at(text, length, at, &after);
				if (!icu_is_space(next) || next == '\r' || next == '\n')
					break;
				at = after;
			}
			return at;
		}
		if ((c == '-' || is_ideograph(c)) && after < length) {
			INT beyond;
			UChar32 next = icu_at(text, length, after, &beyond);
			if (is_ideograph(c) || icu_is_letter(next))
				return after;
		}
		at = after;
	}
	return at;
}

static INT sentence_end(const UChar *text, INT length, INT from)
{
	INT at = from;
	while (at < length) {
		INT after;
		UChar32 c = icu_at(text, length, at, &after);
		if (c == 0x2029 || c == '\n' || c == 0x3002)
			return after;
		if (c == '.' || c == '!' || c == '?') {
			INT end = after;
			BOOL spaced = FALSE;
			while (end < length && icu_is_space(text[end])) {
				spaced = TRUE;
				end++;
			}
			if (end >= length)
				return end;
			// A period before a lower-case letter is an abbreviation, not an end.
			if (spaced && (c != '.' || icu_upper(text[end]) == text[end]))
				return end;
		}
		at = after;
	}
	return at;
}

static UBool find_stops(UBreakIterator *iterator, UErrorCode *status)
{
	icu_free(iterator->stops);
	icu_free(iterator->statuses);
	const UChar *text = iterator->text->units;
	INT length = iterator->text->length;
	iterator->stops = icu_alloc((length + 2) * sizeof(INT));
	iterator->statuses = icu_alloc((length + 2) * sizeof(INT));
	if (!iterator->stops || !iterator->statuses) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return FALSE;
	}
	INT count = 1;
	for (INT at = 0; at < length;) {
		INT kind = STATUS_NONE, end;
		if (iterator->type == UBRK_WORD)
			end = word_end(text, length, at, &kind);
		else if (iterator->type == UBRK_LINE)
			end = line_end(text, length, at);
		else if (iterator->type == UBRK_SENTENCE)
			end = sentence_end(text, length, at);
		else
			end = cluster_end(text, length, at);
		iterator->stops[count] = end;
		iterator->statuses[count++] = kind;
		at = end;
	}
	iterator->stop_count = count;
	iterator->at = 0;
	return TRUE;
}

void __cdecl ubrk_setUText(UBreakIterator *iterator, UText *text, UErrorCode *status)
{
	if (U_FAILED(*status))
		return;
	UText *copy = icu_utext_copy(text);
	if (!copy) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return;
	}
	utext_close(iterator->text);
	iterator->text = copy;
	find_stops(iterator, status);
}

void __cdecl ubrk_setText(UBreakIterator *iterator, const UChar *units, INT length, UErrorCode *status)
{
	UText *text = utext_openUChars(NULL, units, length, status);
	if (U_FAILED(*status))
		return;
	utext_close(iterator->text);
	iterator->text = text;
	find_stops(iterator, status);
}

UBreakIterator *__cdecl ubrk_open(INT type, const char *locale, const UChar *units, INT length, UErrorCode *status)
{
	(void)locale;
	if (U_FAILED(*status))
		return NULL;
	if (type < UBRK_CHARACTER || type > UBRK_SENTENCE) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return NULL;
	}
	UBreakIterator *iterator = icu_alloc(sizeof *iterator);
	if (!iterator) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	iterator->type = type;
	ubrk_setText(iterator, units ? units : L"", units ? length : 0, status);
	return iterator;
}

void __cdecl ubrk_close(UBreakIterator *iterator)
{
	if (!iterator)
		return;
	utext_close(iterator->text);
	icu_free(iterator->stops);
	icu_free(iterator->statuses);
	icu_free(iterator);
}

static INT stop_value(const UBreakIterator *iterator)
{
	return (INT)icu_utext_native(iterator->text, iterator->stops[iterator->at]);
}

INT __cdecl ubrk_current(const UBreakIterator *iterator)
{
	return stop_value(iterator);
}

INT __cdecl ubrk_first(UBreakIterator *iterator)
{
	iterator->at = 0;
	return stop_value(iterator);
}

INT __cdecl ubrk_last(UBreakIterator *iterator)
{
	iterator->at = iterator->stop_count - 1;
	return stop_value(iterator);
}

INT __cdecl ubrk_next(UBreakIterator *iterator)
{
	if (iterator->at + 1 >= iterator->stop_count)
		return UBRK_DONE;
	iterator->at++;
	return stop_value(iterator);
}

INT __cdecl ubrk_previous(UBreakIterator *iterator)
{
	if (iterator->at == 0)
		return UBRK_DONE;
	iterator->at--;
	return stop_value(iterator);
}

INT __cdecl ubrk_following(UBreakIterator *iterator, INT offset)
{
	for (INT i = 0; i < iterator->stop_count; i++) {
		iterator->at = i;
		if (stop_value(iterator) > offset)
			return stop_value(iterator);
	}
	iterator->at = iterator->stop_count - 1;
	return UBRK_DONE;
}

INT __cdecl ubrk_preceding(UBreakIterator *iterator, INT offset)
{
	for (INT i = iterator->stop_count - 1; i >= 0; i--) {
		iterator->at = i;
		if (stop_value(iterator) < offset)
			return stop_value(iterator);
	}
	iterator->at = 0;
	return UBRK_DONE;
}

UBool __cdecl ubrk_isBoundary(UBreakIterator *iterator, INT offset)
{
	for (INT i = 0; i < iterator->stop_count; i++) {
		iterator->at = i;
		if (stop_value(iterator) == offset)
			return TRUE;
	}
	ubrk_following(iterator, offset);
	return FALSE;
}

INT __cdecl ubrk_getRuleStatus(UBreakIterator *iterator)
{
	return iterator->statuses[iterator->at];
}
