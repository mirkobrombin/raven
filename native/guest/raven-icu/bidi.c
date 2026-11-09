/* SPDX-License-Identifier: MIT */
#include "internal.h"

/* Scripts, the bidirectional algorithm and transliteration, from ICU's API documentation and Unicode's UAX #9 and #24. The script
 * and bidirectional classes come from the Unicode blocks, not from the character database. */

typedef struct {
	UChar32 low, high;
	BYTE value;
} code_class;

static BYTE class_in(const code_class *table, UINT count, UChar32 c, BYTE otherwise)
{
	for (UINT i = 0; i < count; i++)
		if (c >= table[i].low && c <= table[i].high)
			return table[i].value;
	return otherwise;
}

/* UScriptCode values. */
enum {
	script_common = 0, script_inherited = 1, script_arabic = 2, script_armenian = 3, script_bengali = 4, script_bopomofo = 5,
	script_cherokee = 6, script_cyrillic = 8, script_devanagari = 10, script_ethiopic = 11, script_georgian = 12, script_greek = 14,
	script_gujarati = 15, script_gurmukhi = 16, script_han = 17, script_hangul = 18, script_hebrew = 19, script_hiragana = 20,
	script_kannada = 21, script_katakana = 22, script_khmer = 23, script_lao = 24, script_latin = 25, script_malayalam = 26,
	script_mongolian = 27, script_myanmar = 28, script_oriya = 31, script_sinhala = 33, script_syriac = 34, script_tamil = 35,
	script_telugu = 36, script_thaana = 37, script_thai = 38, script_tibetan = 39, script_yi = 41, script_unknown = 103,
};

static const code_class script_blocks[] = {
	{0x41, 0x5a, script_latin}, {0x61, 0x7a, script_latin}, {0xaa, 0xaa, script_latin}, {0xba, 0xba, script_latin},
	{0xc0, 0x24f, script_latin}, {0x250, 0x2af, script_latin}, {0x300, 0x36f, script_inherited}, {0x370, 0x3ff, script_greek},
	{0x400, 0x52f, script_cyrillic}, {0x531, 0x58f, script_armenian}, {0x591, 0x5ff, script_hebrew}, {0x64b, 0x655, script_inherited},
	{0x600, 0x6ff, script_arabic}, {0x700, 0x74f, script_syriac}, {0x750, 0x77f, script_arabic}, {0x780, 0x7bf, script_thaana},
	{0x860, 0x86f, script_syriac}, {0x870, 0x8ff, script_arabic}, {0x900, 0x97f, script_devanagari}, {0x980, 0x9ff, script_bengali},
	{0xa00, 0xa7f, script_gurmukhi}, {0xa80, 0xaff, script_gujarati}, {0xb00, 0xb7f, script_oriya}, {0xb80, 0xbff, script_tamil},
	{0xc00, 0xc7f, script_telugu}, {0xc80, 0xcff, script_kannada}, {0xd00, 0xd7f, script_malayalam}, {0xd80, 0xdff, script_sinhala},
	{0xe00, 0xe7f, script_thai}, {0xe80, 0xeff, script_lao}, {0xf00, 0xfff, script_tibetan}, {0x1000, 0x109f, script_myanmar},
	{0x10a0, 0x10ff, script_georgian}, {0x1100, 0x11ff, script_hangul}, {0x1200, 0x139f, script_ethiopic}, {0x13a0, 0x13ff, script_cherokee},
	{0x1780, 0x17ff, script_khmer}, {0x1800, 0x18af, script_mongolian}, {0x19e0, 0x19ff, script_khmer}, {0x1ab0, 0x1aff, script_inherited},
	{0x1c80, 0x1c8f, script_cyrillic}, {0x1c90, 0x1cbf, script_georgian}, {0x1d00, 0x1d7f, script_latin}, {0x1dc0, 0x1dff, script_inherited},
	{0x1e00, 0x1eff, script_latin}, {0x1f00, 0x1fff, script_greek}, {0x200c, 0x200d, script_inherited}, {0x20d0, 0x20ff, script_inherited},
	{0x2c60, 0x2c7f, script_latin}, {0x2d00, 0x2d2f, script_georgian}, {0x2d80, 0x2ddf, script_ethiopic}, {0x2de0, 0x2dff, script_cyrillic},
	{0x2e80, 0x2fdf, script_han}, {0x3005, 0x3005, script_han}, {0x3007, 0x3007, script_han}, {0x3021, 0x3029, script_han},
	{0x302a, 0x302d, script_inherited}, {0x3038, 0x303b, script_han}, {0x3041, 0x3096, script_hiragana}, {0x3099, 0x309a, script_inherited},
	{0x309d, 0x309f, script_hiragana}, {0x30a1, 0x30fa, script_katakana}, {0x30fd, 0x30ff, script_katakana}, {0x3105, 0x312f, script_bopomofo},
	{0x3131, 0x318e, script_hangul}, {0x31a0, 0x31bf, script_bopomofo}, {0x31f0, 0x31ff, script_katakana}, {0x3400, 0x4dbf, script_han},
	{0x4e00, 0x9fff, script_han}, {0xa000, 0xa4cf, script_yi}, {0xa640, 0xa69f, script_cyrillic}, {0xa720, 0xa7ff, script_latin},
	{0xa960, 0xa97f, script_hangul}, {0xab30, 0xab6f, script_latin}, {0xab70, 0xabbf, script_cherokee}, {0xac00, 0xd7ff, script_hangul},
	{0xf900, 0xfaff, script_han}, {0xfb00, 0xfb06, script_latin}, {0xfb13, 0xfb17, script_armenian}, {0xfb1d, 0xfb4f, script_hebrew},
	{0xfb50, 0xfdff, script_arabic}, {0xfe00, 0xfe0f, script_inherited}, {0xfe20, 0xfe2f, script_inherited}, {0xfe70, 0xfefc, script_arabic},
	{0xff21, 0xff3a, script_latin}, {0xff41, 0xff5a, script_latin}, {0xff66, 0xff6f, script_katakana}, {0xff71, 0xff9d, script_katakana},
	{0xffa0, 0xffdc, script_hangul}, {0x1b000, 0x1b000, script_katakana}, {0x1b001, 0x1b11f, script_hiragana}, {0x20000, 0x3134f, script_han},
	{0xe0100, 0xe01ef, script_inherited},
};

/* A letter, digit or mark has its block's script; other characters of a block, its punctuation and symbols, are Common. */
INT __cdecl uscript_getScript(UChar32 c, UErrorCode *status)
{
	if (!status || U_FAILED(*status))
		return -1;
	if (c < 0 || c > 0x10ffff) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return script_common;
	}
	if ((c >= 0xd800 && c <= 0xdfff) || (c >= 0xe000 && c <= 0xf8ff) || c >= 0xf0000)
		return script_unknown;
	BYTE script = class_in(script_blocks, ARRAYSIZE(script_blocks), c, script_common);
	if (script == script_common || script == script_inherited)
		return script;
	if (c < 0x10000 && !icu_is_letter(c) && !icu_is_digit(c) && !icu_is_mark(c))
		return script_common;
	return script;
}

/* Bidirectional classes, as UAX #9 names them. Explicit embeddings and isolates are not applied, so their controls count as BN. */
enum { bidi_l, bidi_r, bidi_al, bidi_en, bidi_es, bidi_et, bidi_an, bidi_cs, bidi_nsm, bidi_bn, bidi_b, bidi_s, bidi_ws, bidi_on };

static const code_class bidi_points[] = {
	{0x0, 0x8, bidi_bn}, {0x9, 0x9, bidi_s}, {0xa, 0xa, bidi_b}, {0xb, 0xb, bidi_s}, {0xc, 0xc, bidi_ws}, {0xd, 0xd, bidi_b},
	{0xe, 0x1b, bidi_bn}, {0x1c, 0x1e, bidi_b}, {0x1f, 0x1f, bidi_s}, {0x20, 0x20, bidi_ws}, {0x23, 0x25, bidi_et}, {0x2b, 0x2b, bidi_es},
	{0x2c, 0x2c, bidi_cs}, {0x2d, 0x2d, bidi_es}, {0x2e, 0x2f, bidi_cs}, {0x30, 0x39, bidi_en}, {0x3a, 0x3a, bidi_cs},
	{0x7f, 0x84, bidi_bn}, {0x85, 0x85, bidi_b}, {0x86, 0x9f, bidi_bn}, {0xa0, 0xa0, bidi_cs}, {0xa2, 0xa5, bidi_et}, {0xad, 0xad, bidi_bn},
	{0xb0, 0xb1, bidi_et}, {0xb2, 0xb3, bidi_en}, {0xb9, 0xb9, bidi_en}, {0xd7, 0xd7, bidi_on}, {0xf7, 0xf7, bidi_on},
	{0x58f, 0x58f, bidi_et}, {0x600, 0x605, bidi_an}, {0x609, 0x60a, bidi_et}, {0x60c, 0x60c, bidi_cs}, {0x660, 0x669, bidi_an},
	{0x66a, 0x66a, bidi_et}, {0x66b, 0x66c, bidi_an}, {0x6dd, 0x6dd, bidi_an}, {0x6f0, 0x6f9, bidi_en}, {0x890, 0x891, bidi_an},
	{0x8e2, 0x8e2, bidi_an}, {0x9f2, 0x9f3, bidi_et}, {0x9fb, 0x9fb, bidi_et}, {0xaf1, 0xaf1, bidi_et}, {0xbf9, 0xbf9, bidi_et},
	{0xe3f, 0xe3f, bidi_et}, {0x1680, 0x1680, bidi_ws}, {0x17db, 0x17db, bidi_et}, {0x180e, 0x180e, bidi_bn}, {0x2000, 0x200a, bidi_ws},
	{0x200b, 0x200d, bidi_bn}, {0x200e, 0x200e, bidi_l}, {0x200f, 0x200f, bidi_r}, {0x2028, 0x2028, bidi_ws}, {0x2029, 0x2029, bidi_b},
	{0x202a, 0x202e, bidi_bn}, {0x202f, 0x202f, bidi_cs}, {0x2030, 0x2034, bidi_et}, {0x2044, 0x2044, bidi_cs}, {0x205f, 0x205f, bidi_ws},
	{0x2060, 0x206f, bidi_bn}, {0x2070, 0x2070, bidi_en}, {0x2074, 0x2079, bidi_en}, {0x207a, 0x207b, bidi_es}, {0x2080, 0x2089, bidi_en},
	{0x208a, 0x208b, bidi_es}, {0x20a0, 0x20cf, bidi_et}, {0x212e, 0x212e, bidi_et}, {0x2212, 0x2212, bidi_es}, {0x2213, 0x2213, bidi_et},
	{0x2488, 0x249b, bidi_en}, {0x3000, 0x3000, bidi_ws}, {0xa838, 0xa839, bidi_et}, {0xfb29, 0xfb29, bidi_es}, {0xfe50, 0xfe50, bidi_cs},
	{0xfe52, 0xfe52, bidi_cs}, {0xfe55, 0xfe55, bidi_cs}, {0xfe5f, 0xfe5f, bidi_et}, {0xfe62, 0xfe63, bidi_es}, {0xfe69, 0xfe6a, bidi_et},
	{0xfeff, 0xfeff, bidi_bn}, {0xff03, 0xff05, bidi_et}, {0xff0b, 0xff0b, bidi_es}, {0xff0c, 0xff0c, bidi_cs}, {0xff0d, 0xff0d, bidi_es},
	{0xff0e, 0xff0f, bidi_cs}, {0xff10, 0xff19, bidi_en}, {0xff1a, 0xff1a, bidi_cs}, {0xffe0, 0xffe1, bidi_et}, {0xffe5, 0xffe6, bidi_et},
	{0x10e60, 0x10e7e, bidi_an}, {0x1d7ce, 0x1d7ff, bidi_en},
};

/* The right-to-left blocks: Hebrew and the other R scripts, and the Arabic letter scripts. */
static const code_class bidi_blocks[] = {
	{0x590, 0x5ff, bidi_r}, {0x600, 0x7bf, bidi_al}, {0x7c0, 0x85f, bidi_r}, {0x860, 0x8ff, bidi_al}, {0xfb1d, 0xfb4f, bidi_r},
	{0xfb50, 0xfdff, bidi_al}, {0xfe70, 0xfefe, bidi_al}, {0x10800, 0x10cff, bidi_r}, {0x10d00, 0x10d3f, bidi_al}, {0x10d40, 0x10eff, bidi_r},
	{0x10f00, 0x10f2f, bidi_r}, {0x10f30, 0x10f6f, bidi_al}, {0x10f70, 0x10fff, bidi_r}, {0x1e800, 0x1edff, bidi_r},
	{0x1ee00, 0x1eeff, bidi_al}, {0x1ef00, 0x1efff, bidi_r},
};

static BYTE bidi_class(UChar32 c)
{
	BYTE kind = class_in(bidi_points, ARRAYSIZE(bidi_points), c, 0xff);
	if (kind != 0xff)
		return kind;
	if (icu_is_mark(c))
		return bidi_nsm;
	kind = class_in(bidi_blocks, ARRAYSIZE(bidi_blocks), c, 0xff);
	if (kind != 0xff)
		return kind;
	if (c >= 0x10000 || icu_is_letter(c) || icu_is_digit(c))
		return bidi_l;
	WORD type = icu_kind(c);
	if (type & (C1_SPACE | C1_BLANK))
		return bidi_ws;
	if (type & (C1_PUNCT | C1_CNTRL))
		return bidi_on;
	/* Symbols outside the letters' blocks are neutral; the rest of the unassigned or letter-like characters are left to right. */
	return (c >= 0x2010 && c <= 0x2bff) || (c >= 0x3001 && c <= 0x3020) || (c >= 0xfe10 && c <= 0xfe6f) || (c >= 0xff01 && c <= 0xff20) ? bidi_on : bidi_l;
}

/* The class of each unit of a text: a surrogate pair gives both units the class of its character. */
static void classes_of(const UChar *text, INT length, BYTE *classes)
{
	for (INT at = 0; at < length;) {
		INT next = at + 1;
		BYTE kind = bidi_class(icu_at(text, length, at, &next));
		while (at < next)
			classes[at++] = kind;
	}
}

#define UBIDI_LTR 0
#define UBIDI_RTL 1
#define UBIDI_MIXED 2
#define UBIDI_NEUTRAL 3
#define UBIDI_DEFAULT_LTR 0xfe
#define UBIDI_DEFAULT_RTL 0xff
#define UBIDI_MAX_EXPLICIT_LEVEL 125

INT __cdecl ubidi_getBaseDirection(const UChar *text, INT length)
{
	if (!text || length < -1)
		return UBIDI_NEUTRAL;
	length = icu_length(text, length);
	for (INT at = 0; at < length;) {
		INT next = at + 1;
		BYTE kind = bidi_class(icu_at(text, length, at, &next));
		if (kind == bidi_l)
			return UBIDI_LTR;
		if (kind == bidi_r || kind == bidi_al)
			return UBIDI_RTL;
		at = next;
	}
	return UBIDI_NEUTRAL;
}

typedef struct UBiDi {
	INT capacity;
	INT length;
	BYTE para_level;
	BYTE *levels;
	INT *visual;
} UBiDi;

UBiDi *__cdecl ubidi_openSized(INT capacity, INT runs, UErrorCode *status)
{
	if (!status || U_FAILED(*status))
		return NULL;
	if (capacity < 0 || runs < 0) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return NULL;
	}
	UBiDi *bidi = icu_alloc(sizeof *bidi);
	if (!bidi) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	bidi->capacity = capacity;
	return bidi;
}

UBiDi *__cdecl ubidi_open(void)
{
	UErrorCode status = U_ZERO_ERROR;
	return ubidi_openSized(0, 0, &status);
}

void __cdecl ubidi_close(UBiDi *bidi)
{
	if (!bidi)
		return;
	icu_free(bidi->levels);
	icu_free(bidi->visual);
	icu_free(bidi);
}

static BOOL is_strong_or_number(BYTE kind)
{
	return kind == bidi_l || kind == bidi_r || kind == bidi_en || kind == bidi_an;
}

static BYTE strong_of(BYTE kind)
{
	return kind == bidi_l ? bidi_l : bidi_r;
}

/* The weak, neutral and implicit rules over one paragraph whose characters all sit at its level, BN set aside (X9). The order holds
 * the positions taking part, types their classes. */
static void resolve_paragraph(const BYTE *classes, INT start, INT end, BYTE level, BYTE *levels, INT *order, BYTE *types)
{
	INT count = 0;
	for (INT at = start; at < end; at++)
		if (classes[at] != bidi_bn) {
			order[count] = at;
			types[count++] = classes[at];
		}
	const BYTE edge = (level & 1) ? bidi_r : bidi_l;
	/* W1: a mark takes the class of what it follows. */
	for (INT k = 0; k < count; k++)
		if (types[k] == bidi_nsm)
			types[k] = k ? types[k - 1] : edge;
	/* W2 and W3: a European number after Arabic letters is an Arabic number; Arabic letters are then right to left. */
	BYTE strong = edge;
	for (INT k = 0; k < count; k++) {
		if (types[k] == bidi_l || types[k] == bidi_r || types[k] == bidi_al)
			strong = types[k];
		else if (types[k] == bidi_en && strong == bidi_al)
			types[k] = bidi_an;
	}
	for (INT k = 0; k < count; k++)
		if (types[k] == bidi_al)
			types[k] = bidi_r;
	/* W4: one separator between two numbers of the same kind joins them. */
	for (INT k = 1; k + 1 < count; k++) {
		if (types[k - 1] == bidi_en && types[k + 1] == bidi_en && (types[k] == bidi_es || types[k] == bidi_cs))
			types[k] = bidi_en;
		else if (types[k - 1] == bidi_an && types[k + 1] == bidi_an && types[k] == bidi_cs)
			types[k] = bidi_an;
	}
	/* W5: terminators next to a European number belong to it. */
	for (INT k = 0; k < count;) {
		if (types[k] != bidi_et) {
			k++;
			continue;
		}
		INT last = k;
		while (last < count && types[last] == bidi_et)
			last++;
		if ((k && types[k - 1] == bidi_en) || (last < count && types[last] == bidi_en))
			for (INT j = k; j < last; j++)
				types[j] = bidi_en;
		k = last;
	}
	/* W6 and W7: the separators and terminators left are neutral; a European number in left-to-right text reads left to right. */
	strong = edge;
	for (INT k = 0; k < count; k++) {
		if (types[k] == bidi_es || types[k] == bidi_et || types[k] == bidi_cs)
			types[k] = bidi_on;
		else if (types[k] == bidi_l || types[k] == bidi_r)
			strong = types[k];
		else if (types[k] == bidi_en && strong == bidi_l)
			types[k] = bidi_l;
	}
	/* N1 and N2: neutrals between text of one direction take it, numbers counting as right to left; otherwise the paragraph's. */
	for (INT k = 0; k < count;) {
		if (is_strong_or_number(types[k])) {
			k++;
			continue;
		}
		INT last = k;
		while (last < count && !is_strong_or_number(types[last]))
			last++;
		BYTE before = k ? strong_of(types[k - 1]) : edge, after = last < count ? strong_of(types[last]) : edge;
		for (INT j = k; j < last; j++)
			types[j] = before == after ? before : edge;
		k = last;
	}
	/* I1 and I2. */
	for (INT k = 0; k < count; k++) {
		BYTE kind = types[k];
		if (!(level & 1))
			levels[order[k]] = level + (kind == bidi_r ? 1 : kind == bidi_an || kind == bidi_en ? 2 : 0);
		else
			levels[order[k]] = level + (kind == bidi_l || kind == bidi_en || kind == bidi_an ? 1 : 0);
	}
	/* BN takes the level of what it follows. */
	for (INT at = start; at < end; at++)
		if (classes[at] == bidi_bn)
			levels[at] = at > start ? levels[at - 1] : level;
	/* L1: separators, and the white space before them and at the end of the paragraph, return to its level. */
	BOOL trailing = TRUE;
	for (INT at = end - 1; at >= start; at--) {
		BYTE kind = classes[at];
		if (kind == bidi_s || kind == bidi_b) {
			levels[at] = level;
			trailing = TRUE;
		} else if (trailing && (kind == bidi_ws || kind == bidi_bn))
			levels[at] = level;
		else
			trailing = FALSE;
	}
}

static BYTE paragraph_level(const BYTE *classes, INT start, INT end, BYTE requested)
{
	if (requested <= UBIDI_MAX_EXPLICIT_LEVEL)
		return requested;
	for (INT at = start; at < end; at++) {
		if (classes[at] == bidi_l)
			return 0;
		if (classes[at] == bidi_r || classes[at] == bidi_al)
			return 1;
	}
	return requested == UBIDI_DEFAULT_RTL ? 1 : 0;
}

/* Each paragraph, ended by a paragraph separator, is resolved on its own. Explicit levels from the caller are not supported. */
void __cdecl ubidi_setPara(UBiDi *bidi, const UChar *text, INT length, BYTE level, BYTE *explicit_levels, UErrorCode *status)
{
	if (!status || U_FAILED(*status))
		return;
	if (!bidi || (!text && length) || length < -1 || (level > UBIDI_MAX_EXPLICIT_LEVEL && level < UBIDI_DEFAULT_LTR)) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return;
	}
	if (explicit_levels) {
		*status = U_UNSUPPORTED_ERROR;
		return;
	}
	length = text ? icu_length(text, length) : 0;
	if (bidi->capacity && length > bidi->capacity) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return;
	}
	icu_free(bidi->levels);
	icu_free(bidi->visual);
	bidi->visual = NULL;
	bidi->length = 0;
	bidi->levels = icu_alloc(length + 1);
	BYTE *classes = icu_alloc(length + 1), *types = icu_alloc(length + 1);
	INT *order = icu_alloc((length + 1) * sizeof(INT));
	if (!bidi->levels || !classes || !types || !order) {
		icu_free(classes);
		icu_free(types);
		icu_free(order);
		*status = U_MEMORY_ALLOCATION_ERROR;
		return;
	}
	classes_of(text, length, classes);
	bidi->para_level = paragraph_level(classes, 0, length, level);
	for (INT start = 0; start < length;) {
		INT end = start;
		while (end < length && classes[end] != bidi_b)
			end++;
		if (end < length)
			end += text[end] == '\r' && end + 1 < length && text[end + 1] == '\n' ? 2 : 1;
		resolve_paragraph(classes, start, end, paragraph_level(classes, start, end, level), bidi->levels, order, types);
		start = end;
	}
	icu_free(classes);
	icu_free(types);
	icu_free(order);
	bidi->length = length;
}

INT __cdecl ubidi_getLength(const UBiDi *bidi)
{
	return bidi ? bidi->length : 0;
}

BYTE __cdecl ubidi_getParaLevel(const UBiDi *bidi)
{
	return bidi ? bidi->para_level : 0;
}

INT __cdecl ubidi_getDirection(const UBiDi *bidi)
{
	if (!bidi || !bidi->length)
		return bidi && (bidi->para_level & 1) ? UBIDI_RTL : UBIDI_LTR;
	BOOL even = FALSE, odd = FALSE;
	for (INT at = 0; at < bidi->length; at++)
		if (bidi->levels[at] & 1)
			odd = TRUE;
		else
			even = TRUE;
	return even && odd ? UBIDI_MIXED : odd ? UBIDI_RTL : UBIDI_LTR;
}

const BYTE *__cdecl ubidi_getLevels(UBiDi *bidi, UErrorCode *status)
{
	if (!status || U_FAILED(*status))
		return NULL;
	if (!bidi || !bidi->levels) {
		*status = U_INVALID_STATE_ERROR;
		return NULL;
	}
	return bidi->levels;
}

BYTE __cdecl ubidi_getLevelAt(const UBiDi *bidi, INT index)
{
	return bidi && bidi->levels && index >= 0 && index < bidi->length ? bidi->levels[index] : 0;
}

/* L2: from the highest level down to the lowest odd one, every run at that level or above is reversed. The map gives, for each
 * visual position, the logical one. */
void __cdecl ubidi_reorderVisual(const BYTE *levels, INT length, INT *map)
{
	if (!levels || !map || length <= 0)
		return;
	BYTE highest = 0, lowest_odd = 0xff;
	for (INT at = 0; at < length; at++) {
		map[at] = at;
		if (levels[at] > highest)
			highest = levels[at];
		if ((levels[at] & 1) && levels[at] < lowest_odd)
			lowest_odd = levels[at];
	}
	for (INT level = highest; level >= lowest_odd && level > 0; level--)
		for (INT at = 0; at < length;) {
			if (levels[map[at]] < level) {
				at++;
				continue;
			}
			INT last = at;
			while (last < length && levels[map[last]] >= level)
				last++;
			for (INT left = at, right = last - 1; left < right; left++, right--) {
				INT swap = map[left];
				map[left] = map[right];
				map[right] = swap;
			}
			at = last;
		}
}

/* The same order seen from the other side: for each logical position, the visual one. */
void __cdecl ubidi_reorderLogical(const BYTE *levels, INT length, INT *map)
{
	if (!levels || !map || length <= 0)
		return;
	INT *visual = icu_alloc(length * sizeof(INT));
	if (!visual)
		return;
	ubidi_reorderVisual(levels, length, visual);
	for (INT at = 0; at < length; at++)
		map[visual[at]] = at;
	icu_free(visual);
}

static const INT *visual_map(UBiDi *bidi, UErrorCode *status)
{
	if (U_FAILED(*status))
		return NULL;
	if (!bidi || !bidi->levels) {
		*status = U_INVALID_STATE_ERROR;
		return NULL;
	}
	if (!bidi->visual) {
		bidi->visual = icu_alloc((bidi->length + 1) * sizeof(INT));
		if (!bidi->visual) {
			*status = U_MEMORY_ALLOCATION_ERROR;
			return NULL;
		}
		ubidi_reorderVisual(bidi->levels, bidi->length, bidi->visual);
	}
	return bidi->visual;
}

void __cdecl ubidi_getVisualMap(UBiDi *bidi, INT *map, UErrorCode *status)
{
	if (!status)
		return;
	const INT *visual = visual_map(bidi, status);
	if (visual && map)
		CopyMemory(map, visual, bidi->length * sizeof(INT));
}

void __cdecl ubidi_getLogicalMap(UBiDi *bidi, INT *map, UErrorCode *status)
{
	if (!status)
		return;
	const INT *visual = visual_map(bidi, status);
	if (visual && map)
		for (INT at = 0; at < bidi->length; at++)
			map[visual[at]] = at;
}

INT __cdecl ubidi_getLogicalIndex(UBiDi *bidi, INT index, UErrorCode *status)
{
	if (!status)
		return -1;
	const INT *visual = visual_map(bidi, status);
	if (!visual)
		return -1;
	if (index < 0 || index >= bidi->length) {
		*status = U_INDEX_OUTOFBOUNDS_ERROR;
		return -1;
	}
	return visual[index];
}

INT __cdecl ubidi_getVisualIndex(UBiDi *bidi, INT index, UErrorCode *status)
{
	if (!status)
		return -1;
	const INT *visual = visual_map(bidi, status);
	if (!visual)
		return -1;
	if (index < 0 || index >= bidi->length) {
		*status = U_INDEX_OUTOFBOUNDS_ERROR;
		return -1;
	}
	for (INT at = 0; at < bidi->length; at++)
		if (visual[at] == index)
			return at;
	return -1;
}

/* Visual runs: the stretches of the visual order at one level. */
INT __cdecl ubidi_countRuns(UBiDi *bidi, UErrorCode *status)
{
	if (!status)
		return -1;
	const INT *visual = visual_map(bidi, status);
	if (!visual)
		return -1;
	INT runs = 0;
	for (INT at = 0; at < bidi->length; at++)
		if (!at || bidi->levels[visual[at]] != bidi->levels[visual[at - 1]])
			runs++;
	return runs;
}

INT __cdecl ubidi_getVisualRun(UBiDi *bidi, INT run, INT *logical_start, INT *length)
{
	UErrorCode status = U_ZERO_ERROR;
	const INT *visual = visual_map(bidi, &status);
	if (!visual || run < 0)
		return UBIDI_LTR;
	for (INT at = 0, index = -1; at < bidi->length; at++) {
		if (at && bidi->levels[visual[at]] == bidi->levels[visual[at - 1]])
			continue;
		if (++index != run)
			continue;
		INT last = at;
		while (last + 1 < bidi->length && bidi->levels[visual[last + 1]] == bidi->levels[visual[at]])
			last++;
		BOOL rtl = bidi->levels[visual[at]] & 1;
		if (logical_start)
			*logical_start = rtl ? visual[last] : visual[at];
		if (length)
			*length = last - at + 1;
		return rtl ? UBIDI_RTL : UBIDI_LTR;
	}
	return UBIDI_LTR;
}

/* Transliterators by ID: Null, Lower, Upper (with or without "Any-"), and Latin-ASCII or "NFD; [:Nonspacing Mark:] Remove; NFC",
 * which take the accents off letters. Rule-based transliterators are not supported. */
typedef enum { transform_null, transform_lower, transform_upper, transform_strip } transform_kind;

typedef struct UTransliterator {
	transform_kind kind;
} UTransliterator;

#define U_INVALID_ID 0x10021
#define UTRANS_FORWARD 0

static BOOL id_is(const UChar *id, INT length, const char *name)
{
	INT at = 0;
	for (const char *c = name; *c; c++) {
		if (*c == ' ')
			continue;
		while (at < length && id[at] == ' ')
			at++;
		if (at >= length || icu_lower(id[at]) != icu_lower((BYTE)*c))
			return FALSE;
		at++;
	}
	while (at < length && id[at] == ' ')
		at++;
	return at == length;
}

UTransliterator *__cdecl utrans_openU(const UChar *id, INT id_length, INT direction, const UChar *rules, INT rules_length, void *parse_error,
	UErrorCode *status)
{
	(void)rules_length;
	(void)parse_error;
	if (!status || U_FAILED(*status))
		return NULL;
	if (!id) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return NULL;
	}
	if (rules) {
		*status = U_UNSUPPORTED_ERROR;
		return NULL;
	}
	id_length = icu_length(id, id_length);
	transform_kind kind;
	if (id_is(id, id_length, "Null") || id_is(id, id_length, "Any-Null"))
		kind = transform_null;
	else if (direction != UTRANS_FORWARD)
		kind = (transform_kind)-1;
	else if (id_is(id, id_length, "Lower") || id_is(id, id_length, "Any-Lower"))
		kind = transform_lower;
	else if (id_is(id, id_length, "Upper") || id_is(id, id_length, "Any-Upper"))
		kind = transform_upper;
	else if (id_is(id, id_length, "Latin-ASCII") || id_is(id, id_length, "NFD; [:Nonspacing Mark:] Remove; NFC") ||
		id_is(id, id_length, "NFD; [:Mn:] Remove; NFC") || id_is(id, id_length, "NFD; [:M:] Remove; NFC"))
		kind = transform_strip;
	else
		kind = (transform_kind)-1;
	if (kind == (transform_kind)-1) {
		*status = U_INVALID_ID;
		return NULL;
	}
	UTransliterator *transliterator = icu_alloc(sizeof *transliterator);
	if (!transliterator) {
		*status = U_MEMORY_ALLOCATION_ERROR;
		return NULL;
	}
	transliterator->kind = kind;
	return transliterator;
}

void __cdecl utrans_close(UTransliterator *transliterator)
{
	icu_free(transliterator);
}

/* The span from start to limit is replaced by its transliteration; the text after it moves, and the limit and the length follow. */
void __cdecl utrans_transUChars(const UTransliterator *transliterator, UChar *text, INT *text_length, INT capacity, INT start, INT *limit,
	UErrorCode *status)
{
	if (!status || U_FAILED(*status))
		return;
	if (!transliterator || !text) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return;
	}
	INT length = text_length ? *text_length : -1;
	length = icu_length(text, length);
	INT end = limit ? *limit : length;
	if (start < 0 || end < start || end > length) {
		*status = U_ILLEGAL_ARGUMENT_ERROR;
		return;
	}
	INT span = end - start, made = span;
	UChar *result = NULL;
	if (transliterator->kind == transform_lower || transliterator->kind == transform_upper) {
		result = icu_alloc((span * 3 + 1) * sizeof(UChar));
		if (!result) {
			*status = U_MEMORY_ALLOCATION_ERROR;
			return;
		}
		UErrorCode mapped = U_ZERO_ERROR;
		made = transliterator->kind == transform_lower ? u_strToLower(result, span * 3 + 1, text + start, span, "", &mapped)
			: u_strToUpper(result, span * 3 + 1, text + start, span, "", &mapped);
		if (U_FAILED(mapped)) {
			icu_free(result);
			*status = mapped;
			return;
		}
	} else if (transliterator->kind == transform_strip && span) {
		INT decomposed = NormalizeString(NormalizationD, text + start, span, NULL, 0);
		UChar *parts = decomposed > 0 ? icu_alloc((decomposed + 1) * sizeof(UChar)) : NULL;
		result = parts ? icu_alloc((decomposed + 1) * sizeof(UChar)) : NULL;
		if (!result) {
			icu_free(parts);
			*status = U_MEMORY_ALLOCATION_ERROR;
			return;
		}
		decomposed = NormalizeString(NormalizationD, text + start, span, parts, decomposed);
		INT kept = 0;
		for (INT at = 0; at < decomposed; at++)
			if (!icu_is_mark(parts[at]))
				parts[kept++] = parts[at];
		made = kept ? NormalizeString(NormalizationC, parts, kept, result, decomposed + 1) : 0;
		icu_free(parts);
		if (made < 0) {
			icu_free(result);
			*status = U_INVALID_CHAR_FOUND;
			return;
		}
	}
	INT total = length - span + made;
	if (total > capacity) {
		icu_free(result);
		*status = U_BUFFER_OVERFLOW_ERROR;
		if (text_length)
			*text_length = total;
		return;
	}
	if (result) {
		MoveMemory(text + start + made, text + end, (length - end) * sizeof(UChar));
		CopyMemory(text + start, result, made * sizeof(UChar));
		icu_free(result);
	}
	if (total < capacity)
		text[total] = 0;
	if (text_length)
		*text_length = total;
	if (limit)
		*limit = start + made;
}
