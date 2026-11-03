/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* ---- the text analyzer: what a program that lays out text itself (WPF does) asks before it places glyphs ----
 *
 * The analyzer reads the text from the program's source and reports to its sink: one script run, with the control characters marked as
 * having no visual; bidirectional levels from the paragraph direction and the strong letters (Hebrew and Arabic right to left); the
 * places a line may or must end (after spaces and hyphens, between ideographs, after a line break). Glyphs are one per character, as
 * text that needs no shaping has them, and placements are the design advances at the size, whole pixels for GDI-compatible text.
 * Scripts that need contextual shaping (Arabic joining, Indic reordering) are placed as their separate characters.
 * The same object answers for IDWriteTextAnalyzer1 and 2, the versions a program asks for at startup: spacing, baselines,
 * orientation, complexity and features, kept as simple as its one-script runs. */

/* Uniscribe's justification classes, which the glyph properties carry: SCRIPT_JUSTIFY_CHARACTER and SCRIPT_JUSTIFY_BLANK. */
enum { JUSTIFY_CHARACTER = 2, JUSTIFY_BLANK = 4 };

typedef struct {
	const void *const *vtbl;
	LONG refs;
} TextAnalyzer;

/* To check: refuse IDWriteTextAnalyzer1 and 2 here and open a document in Word. It stops with error 47, and no_interface
 * prints both GUIDs to the trace (RAVEN_TRACE_DWRITE=1). */
static HRESULT STDMETHODCALLTYPE ta_query(IDWriteTextAnalyzer *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteTextAnalyzer) || same_guid(iid, &IID_IDWriteTextAnalyzer1) ||
		same_guid(iid, &IID_IDWriteTextAnalyzer2)) {
		InterlockedIncrement(&((TextAnalyzer *)self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE ta_add_ref(IDWriteTextAnalyzer *self) { return (ULONG)InterlockedIncrement(&((TextAnalyzer *)self)->refs); }
static ULONG STDMETHODCALLTYPE ta_release(IDWriteTextAnalyzer *self)
{
	LONG left = InterlockedDecrement(&((TextAnalyzer *)self)->refs);
	if (!left)
		FREE(self);
	return (ULONG)left;
}

/* The characters [position, position + length) of the source in one buffer; the source hands them over in pieces. */
static WCHAR *source_text(IDWriteTextAnalysisSource *source, UINT32 position, UINT32 length)
{
	WCHAR *text = ALLOC((length + 1) * sizeof(WCHAR));
	UINT32 done = 0;
	if (!text)
		return NULL;
	while (done < length) {
		const WCHAR *piece = NULL;
		UINT32 count = 0;
		if (FAILED(IDWriteTextAnalysisSource_GetTextAtPosition(source, position + done, &piece, &count)) || !piece || !count)
			break;
		if (count > length - done)
			count = length - done;
		memcpy(text + done, piece, count * sizeof(WCHAR));
		done += count;
	}
	return text;
}

static BOOL is_control(WCHAR c)
{
	return c < 0x20 || (c >= 0x7f && c < 0xa0) || (c >= 0x200b && c <= 0x200f) || (c >= 0x2028 && c <= 0x202e) || (c >= 0x2060 && c <= 0x2064) ||
		c == 0xfeff;
}

static HRESULT STDMETHODCALLTYPE ta_script(IDWriteTextAnalyzer *self, IDWriteTextAnalysisSource *source, UINT32 position, UINT32 length,
	IDWriteTextAnalysisSink *sink)
{
	WCHAR *text;
	UINT32 at = 0;
	if (!source || !sink)
		return E_INVALIDARG;
	if (!length)
		return S_OK;
	if (!(text = source_text(source, position, length)))
		return E_OUTOFMEMORY;
	/* Runs of visible text and runs of control characters, which take no glyph that shows. */
	while (at < length) {
		const BOOL control = is_control(text[at]);
		DWRITE_SCRIPT_ANALYSIS analysis;
		UINT32 end = at + 1;
		while (end < length && is_control(text[end]) == control)
			++end;
		analysis.script = 0;
		analysis.shapes = control ? DWRITE_SCRIPT_SHAPES_NO_VISUAL : DWRITE_SCRIPT_SHAPES_DEFAULT;
		IDWriteTextAnalysisSink_SetScriptAnalysis(sink, position + at, end - at, &analysis);
		at = end;
	}
	FREE(text);
	return S_OK;
}

/* 1 for a letter written right to left, 0 for one written left to right, -1 for a character with no direction of its own. */
static int strong_direction(WCHAR c)
{
	if ((c >= 0x0590 && c <= 0x08ff) || (c >= 0xfb1d && c <= 0xfdff) || (c >= 0xfe70 && c <= 0xfefe))
		return 1;
	if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xc0 && c <= 0x24f && c != 0xd7 && c != 0xf7) || (c >= 0x370 && c <= 0x58f) ||
		(c >= 0x900 && c <= 0x1fff) || (c >= 0x3040 && c <= 0xd7ff) || (c >= 0xf900 && c <= 0xfaff))
		return 0;
	return -1;
}

static HRESULT STDMETHODCALLTYPE ta_bidi(IDWriteTextAnalyzer *self, IDWriteTextAnalysisSource *source, UINT32 position, UINT32 length,
	IDWriteTextAnalysisSink *sink)
{
	const UINT8 base = IDWriteTextAnalysisSource_GetParagraphReadingDirection(source) == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT ? 1 : 0;
	WCHAR *text;
	UINT8 *levels;
	UINT32 at;
	if (!source || !sink)
		return E_INVALIDARG;
	if (!length)
		return S_OK;
	text = source_text(source, position, length);
	levels = ALLOC(length);
	if (!text || !levels) {
		FREE(text);
		FREE(levels);
		return E_OUTOFMEMORY;
	}
	/* A strong letter takes the even or odd level of its direction; a neutral one between two letters of one direction takes theirs,
	 * otherwise the paragraph's. */
	for (at = 0; at < length; ++at) {
		const int direction = strong_direction(text[at]);
		levels[at] = direction < 0 ? 255 : (UINT8)(direction == (base & 1) ? base : base + 1);
	}
	for (at = 0; at < length;) {
		UINT32 end = at, before, after;
		if (levels[at] != 255) {
			++at;
			continue;
		}
		while (end < length && levels[end] == 255)
			++end;
		before = at ? levels[at - 1] : base;
		after = end < length ? levels[end] : base;
		while (at < end)
			levels[at++] = (UINT8)(before == after ? before : base);
	}
	for (at = 0; at < length;) {
		UINT32 end = at + 1;
		while (end < length && levels[end] == levels[at])
			++end;
		IDWriteTextAnalysisSink_SetBidiLevel(sink, position + at, end - at, base, levels[at]);
		at = end;
	}
	FREE(levels);
	FREE(text);
	return S_OK;
}

/* Digits are shaped as they are written, so nothing is substituted. */
static HRESULT STDMETHODCALLTYPE ta_numbers(IDWriteTextAnalyzer *self, IDWriteTextAnalysisSource *source, UINT32 position, UINT32 length,
	IDWriteTextAnalysisSink *sink)
{
	return source && sink ? S_OK : E_INVALIDARG;
}

static BOOL is_ideograph(WCHAR c)
{
	return (c >= 0x2e80 && c <= 0x9fff) || (c >= 0xac00 && c <= 0xd7af) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xff00 && c <= 0xffef);
}

static BOOL is_space(WCHAR c) { return c == ' ' || c == '\t' || c == 0x3000 || c == 0x2002 || c == 0x2003 || c == 0x2009; }

static HRESULT STDMETHODCALLTYPE ta_breaks(IDWriteTextAnalyzer *self, IDWriteTextAnalysisSource *source, UINT32 position, UINT32 length,
	IDWriteTextAnalysisSink *sink)
{
	DWRITE_LINE_BREAKPOINT *points;
	WCHAR *text;
	UINT32 at;
	if (!source || !sink)
		return E_INVALIDARG;
	if (!length)
		return S_OK;
	text = source_text(source, position, length);
	points = ALLOC(length * sizeof(DWRITE_LINE_BREAKPOINT));
	if (!text || !points) {
		FREE(text);
		FREE(points);
		return E_OUTOFMEMORY;
	}
	for (at = 0; at < length; ++at) {
		points[at].isWhitespace = is_space(text[at]);
		points[at].isSoftHyphen = text[at] == 0xad;
		points[at].breakConditionBefore = DWRITE_BREAK_CONDITION_NEUTRAL;
		points[at].breakConditionAfter = DWRITE_BREAK_CONDITION_NEUTRAL;
	}
	/* The condition between two characters is written on both sides of it. */
	for (at = 0; at + 1 < length; ++at) {
		const WCHAR c = text[at], next = text[at + 1];
		DWRITE_BREAK_CONDITION condition = DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
		if ((c == '\r' && next != '\n') || c == '\n' || c == 0x2028 || c == 0x2029 || c == 0x0b || c == 0x0c || c == 0x85)
			condition = DWRITE_BREAK_CONDITION_MUST_BREAK;
		else if (c == '\r')
			condition = DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
		else if (is_space(next) || c == 0xa0 || next == 0xa0 || c == 0x2060 || next == 0x2060)
			condition = DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
		else if (is_space(c) || c == '-' || c == 0xad || c == 0x200b || is_ideograph(c) || is_ideograph(next))
			condition = DWRITE_BREAK_CONDITION_CAN_BREAK;
		points[at].breakConditionAfter = (UINT8)condition;
		points[at + 1].breakConditionBefore = (UINT8)condition;
	}
	if (text[length - 1] == '\n' || text[length - 1] == '\r' || text[length - 1] == 0x2029)
		points[length - 1].breakConditionAfter = DWRITE_BREAK_CONDITION_MUST_BREAK;
	IDWriteTextAnalysisSink_SetLineBreakpoints(sink, position, length, points);
	FREE(points);
	FREE(text);
	return S_OK;
}

/* The mirror of a paired character, drawn in right-to-left text in its place. */
static UINT32 mirrored(UINT32 code)
{
	static const UINT32 pairs[] = {'(', ')', '<', '>', '[', ']', '{', '}', 0xab, 0xbb, 0x2039, 0x203a, 0x2264, 0x2265};
	UINT32 index;
	for (index = 0; index < sizeof(pairs) / sizeof(pairs[0]); ++index)
		if (pairs[index] == code)
			return pairs[index ^ 1];
	return code;
}

static HRESULT STDMETHODCALLTYPE ta_glyphs(IDWriteTextAnalyzer *self, const WCHAR *text, UINT32 length, IDWriteFontFace *face, WINBOOL sideways,
	WINBOOL rtl, const DWRITE_SCRIPT_ANALYSIS *analysis, const WCHAR *locale, IDWriteNumberSubstitution *substitution,
	const DWRITE_TYPOGRAPHIC_FEATURES **features, const UINT32 *feature_lengths, UINT32 feature_ranges, UINT32 capacity, UINT16 *clusters,
	DWRITE_SHAPING_TEXT_PROPERTIES *text_properties, UINT16 *glyphs, DWRITE_SHAPING_GLYPH_PROPERTIES *glyph_properties, UINT32 *count)
{
	UINT32 *codepoints, position, made = 0;
	HRESULT result;
	if (!text || !face || !clusters || !text_properties || !glyphs || !glyph_properties || !count)
		return E_INVALIDARG;
	*count = 0;
	if (!length)
		return S_OK;
	if (!(codepoints = ALLOC(length * sizeof(UINT32))))
		return E_OUTOFMEMORY;
	for (position = 0; position < length;) {
		UINT32 units = 1, code = text[position];
		if (code >= 0xd800 && code < 0xdc00 && position + 1 < length && text[position + 1] >= 0xdc00 && text[position + 1] < 0xe000) {
			code = 0x10000 + ((code - 0xd800) << 10) + (text[position + 1] - 0xdc00);
			units = 2;
		}
		if (made >= capacity) {
			FREE(codepoints);
			return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
		}
		codepoints[made] = format_control(code) ? ' ' : rtl ? mirrored(code) : code;
		clusters[position] = (UINT16)made;
		memset(&text_properties[position], 0, sizeof(text_properties[position]));
		if (units == 2) {
			clusters[position + 1] = (UINT16)made;
			memset(&text_properties[position + 1], 0, sizeof(text_properties[position + 1]));
		}
		memset(&glyph_properties[made], 0, sizeof(glyph_properties[made]));
		glyph_properties[made].justification = is_space((WCHAR)code) ? JUSTIFY_BLANK : JUSTIFY_CHARACTER;
		glyph_properties[made].isClusterStart = 1;
		glyph_properties[made].isZeroWidthSpace = (analysis && analysis->shapes == DWRITE_SCRIPT_SHAPES_NO_VISUAL) || format_control(code);
		position += units;
		++made;
	}
	result = IDWriteFontFace_GetGlyphIndices(face, codepoints, made, glyphs);
	FREE(codepoints);
	if (FAILED(result))
		return result;
	*count = made;
	return S_OK;
}

/* The design advance of each glyph at the size; a glyph with no visual takes no room. */
static HRESULT place_glyphs(const UINT16 *glyphs, const DWRITE_SHAPING_GLYPH_PROPERTIES *properties, UINT32 count, IDWriteFontFace *face, FLOAT size,
	WINBOOL sideways, float pixels, FLOAT *advances, DWRITE_GLYPH_OFFSET *offsets)
{
	DWRITE_FONT_METRICS metrics;
	UINT32 index;
	if (!glyphs || !face || !advances || !offsets)
		return E_INVALIDARG;
	IDWriteFontFace_GetMetrics(face, &metrics);
	for (index = 0; index < count; ++index) {
		DWRITE_GLYPH_METRICS design;
		float advance = 0;
		offsets[index].advanceOffset = 0;
		offsets[index].ascenderOffset = 0;
		if (properties && properties[index].isZeroWidthSpace) {
			advances[index] = 0;
			continue;
		}
		if (metrics.designUnitsPerEm && SUCCEEDED(IDWriteFontFace_GetDesignGlyphMetrics(face, &glyphs[index], 1, &design, sideways)))
			advance = (float)(sideways ? design.advanceHeight : design.advanceWidth) * size / (float)metrics.designUnitsPerEm;
		if (pixels > 0)
			advance = round_float(advance * pixels) / pixels;
		advances[index] = advance;
	}
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE ta_placements(IDWriteTextAnalyzer *self, const WCHAR *text, const UINT16 *clusters, DWRITE_SHAPING_TEXT_PROPERTIES *text_properties,
	UINT32 length, const UINT16 *glyphs, const DWRITE_SHAPING_GLYPH_PROPERTIES *glyph_properties, UINT32 count, IDWriteFontFace *face, FLOAT size,
	WINBOOL sideways, WINBOOL rtl, const DWRITE_SCRIPT_ANALYSIS *analysis, const WCHAR *locale, const DWRITE_TYPOGRAPHIC_FEATURES **features,
	const UINT32 *feature_lengths, UINT32 feature_ranges, FLOAT *advances, DWRITE_GLYPH_OFFSET *offsets)
{
	return place_glyphs(glyphs, glyph_properties, count, face, size, sideways, 0, advances, offsets);
}

static HRESULT STDMETHODCALLTYPE ta_gdi_placements(IDWriteTextAnalyzer *self, const WCHAR *text, const UINT16 *clusters, DWRITE_SHAPING_TEXT_PROPERTIES *text_properties,
	UINT32 length, const UINT16 *glyphs, const DWRITE_SHAPING_GLYPH_PROPERTIES *glyph_properties, UINT32 count, IDWriteFontFace *face, FLOAT size,
	FLOAT pixels_per_dip, const DWRITE_MATRIX *transform, WINBOOL natural, WINBOOL sideways, WINBOOL rtl, const DWRITE_SCRIPT_ANALYSIS *analysis,
	const WCHAR *locale, const DWRITE_TYPOGRAPHIC_FEATURES **features, const UINT32 *feature_lengths, UINT32 feature_ranges, FLOAT *advances,
	DWRITE_GLYPH_OFFSET *offsets)
{
	/* GDI-compatible advances are whole pixels at the scale the text is drawn at. */
	float pixels = pixels_per_dip > 0 ? pixels_per_dip : 1.0f;
	if (transform && transform->m11 > 0)
		pixels *= transform->m11;
	return place_glyphs(glyphs, glyph_properties, count, face, size, sideways, pixels, advances, offsets);
}

/* IDWriteTextAnalyzer1: spacing added around each cluster, never below the smallest advance asked for. */
static HRESULT STDMETHODCALLTYPE ta_spacing(void *self, FLOAT leading, FLOAT trailing, FLOAT minimum, UINT32 length, UINT32 count, const UINT16 *clusters,
	const FLOAT *advances, const DWRITE_GLYPH_OFFSET *offsets, const DWRITE_SHAPING_GLYPH_PROPERTIES *properties, FLOAT *out_advances,
	DWRITE_GLYPH_OFFSET *out_offsets)
{
	if (!clusters || !advances || !offsets || !out_advances || !out_offsets)
		return E_INVALIDARG;
	memmove(out_advances, advances, count * sizeof(FLOAT));
	memmove(out_offsets, offsets, count * sizeof(DWRITE_GLYPH_OFFSET));
	for (UINT32 at = 0; at < length;) {
		UINT32 next = at + 1;
		while (next < length && clusters[next] == clusters[at])
			++next;
		const UINT32 first = clusters[at], last = next < length ? (UINT32)clusters[next] - 1 : count - 1;
		if (first < count && last < count && first <= last) {
			out_advances[first] += leading;
			out_offsets[first].advanceOffset += leading;
			out_advances[last] += trailing;
			if (out_advances[first] < minimum)
				out_advances[first] = minimum;
		}
		at = next;
	}
	return S_OK;
}

/* The font tells where its own alphabetic baseline and its extents are; any other baseline is worked out from those when the caller
 * allows that, and is reported as not in the font. */
static HRESULT STDMETHODCALLTYPE ta_baseline(void *self, IDWriteFontFace *face, DWRITE_BASELINE baseline, WINBOOL vertical, WINBOOL simulate,
	DWRITE_SCRIPT_ANALYSIS analysis, const WCHAR *locale, INT32 *coordinate, WINBOOL *exists)
{
	DWRITE_FONT_METRICS metrics;
	if (!face || !coordinate || !exists)
		return E_INVALIDARG;
	*coordinate = 0;
	*exists = FALSE;
	IDWriteFontFace_GetMetrics(face, &metrics);
	INT32 value = 0;
	switch (baseline) {
	case DWRITE_BASELINE_DEFAULT:
	case DWRITE_BASELINE_ROMAN: *exists = TRUE; return S_OK;
	case DWRITE_BASELINE_CENTRAL: value = ((INT32)metrics.ascent - (INT32)metrics.descent) / 2; break;
	case DWRITE_BASELINE_MATH: value = (INT32)metrics.xHeight / 2; break;
	case DWRITE_BASELINE_HANGING: value = (INT32)metrics.capHeight; break;
	case DWRITE_BASELINE_IDEOGRAPHIC_BOTTOM:
	case DWRITE_BASELINE_MINIMUM: value = -(INT32)metrics.descent; break;
	case DWRITE_BASELINE_IDEOGRAPHIC_TOP:
	case DWRITE_BASELINE_MAXIMUM: value = (INT32)metrics.ascent; break;
	default: return E_INVALIDARG;
	}
	if (simulate)
		*coordinate = value;
	return S_OK;
}

/* Vertical text keeps every character upright: the analyzer has no table of the characters that turn sideways. */
static HRESULT STDMETHODCALLTYPE ta_vertical(void *self, IDWriteTextAnalysisSource1 *source, UINT32 position, UINT32 length, IDWriteTextAnalysisSink1 *sink)
{
	if (!source || !sink)
		return E_INVALIDARG;
	return length ? IDWriteTextAnalysisSink1_SetGlyphOrientation(sink, position, length, DWRITE_GLYPH_ORIENTATION_ANGLE_0_DEGREES, 0, FALSE, FALSE) : S_OK;
}

static HRESULT orientation(DWRITE_GLYPH_ORIENTATION_ANGLE angle, WINBOOL sideways, FLOAT x, FLOAT y, DWRITE_MATRIX *out)
{
	static const FLOAT turns[4][4] = {{1, 0, 0, 1}, {0, 1, -1, 0}, {-1, 0, 0, -1}, {0, -1, 1, 0}};
	if (!out)
		return E_INVALIDARG;
	if ((UINT32)angle > DWRITE_GLYPH_ORIENTATION_ANGLE_270_DEGREES) {
		memset(out, 0, sizeof *out);
		return E_INVALIDARG;
	}
	const FLOAT *turn = turns[((UINT32)angle + (sideways ? 1 : 0)) & 3];
	out->m11 = turn[0];
	out->m12 = turn[1];
	out->m21 = turn[2];
	out->m22 = turn[3];
	/* Turning about the origin the caller names leaves that point where it was. */
	out->dx = x - x * turn[0] - y * turn[2];
	out->dy = y - x * turn[1] - y * turn[3];
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE ta_orientation(void *self, DWRITE_GLYPH_ORIENTATION_ANGLE angle, WINBOOL sideways, DWRITE_MATRIX *out)
{
	return orientation(angle, sideways, 0, 0, out);
}

static HRESULT STDMETHODCALLTYPE ta_orientation2(void *self, DWRITE_GLYPH_ORIENTATION_ANGLE angle, WINBOOL sideways, FLOAT x, FLOAT y, DWRITE_MATRIX *out)
{
	return orientation(angle, sideways, x, y, out);
}

/* The analyzer has one script run for all text, written as words of separate letters with spaces between. */
static HRESULT STDMETHODCALLTYPE ta_script_properties(void *self, DWRITE_SCRIPT_ANALYSIS analysis, DWRITE_SCRIPT_PROPERTIES *out)
{
	if (!out)
		return E_INVALIDARG;
	memset(out, 0, sizeof *out);
	out->isoScriptCode = DWRITE_MAKE_OPENTYPE_TAG('L', 'a', 't', 'n');
	out->isoScriptNumber = 215;
	out->justificationCharacter = L' ';
	out->usesWordDividers = 1;
	out->isDiscreteWriting = 1;
	return S_OK;
}

/* Simple text maps one to one onto glyphs the font has, with no shaping: below the combining marks, and past the control characters. */
static BOOL simple_character(IDWriteFontFace *face, WCHAR letter, UINT16 *glyph)
{
	const UINT32 point = letter;
	*glyph = 0;
	if (point < 0x20 || (point >= 0x7f && point < 0xa0) || point >= 0x300)
		return FALSE;
	return SUCCEEDED(IDWriteFontFace_GetGlyphIndices(face, &point, 1, glyph)) && *glyph;
}

static HRESULT STDMETHODCALLTYPE ta_complexity(void *self, const WCHAR *text, UINT32 length, IDWriteFontFace *face, WINBOOL *simple, UINT32 *read, UINT16 *glyphs)
{
	UINT16 glyph;
	if (!simple || !read)
		return E_INVALIDARG;
	*simple = FALSE;
	*read = 0;
	if (!text || !face || !length)
		return E_INVALIDARG;
	const BOOL first = simple_character(face, text[0], &glyph);
	UINT32 at = 0;
	for (; at < length && simple_character(face, text[at], &glyph) == first; ++at)
		if (glyphs)
			glyphs[at] = first ? glyph : 0;
	*simple = first;
	*read = at;
	return S_OK;
}

/* Nothing here stretches or shrinks: a glyph offers no opportunities, so justified text keeps its advances unless a caller supplies some. */
static HRESULT STDMETHODCALLTYPE ta_opportunities(void *self, IDWriteFontFace *face, FLOAT size, DWRITE_SCRIPT_ANALYSIS analysis, UINT32 length, UINT32 count,
	const WCHAR *text, const UINT16 *clusters, const DWRITE_SHAPING_GLYPH_PROPERTIES *properties, DWRITE_JUSTIFICATION_OPPORTUNITY *out)
{
	if (count && !out)
		return E_INVALIDARG;
	memset(out, 0, count * sizeof *out);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE ta_justify(void *self, FLOAT width, UINT32 count, const DWRITE_JUSTIFICATION_OPPORTUNITY *opportunities, const FLOAT *advances,
	const DWRITE_GLYPH_OFFSET *offsets, FLOAT *out_advances, DWRITE_GLYPH_OFFSET *out_offsets)
{
	FLOAT used = 0, room = 0, shrink = 0;
	if (count && (!opportunities || !advances || !offsets))
		return E_INVALIDARG;
	for (UINT32 index = 0; index < count; ++index) {
		used += advances[index];
		room += opportunities[index].expansionMaximum;
		shrink += opportunities[index].compressionMaximum;
	}
	const FLOAT change = width - used;
	const FLOAT share = change > 0 ? (room > 0 ? (change < room ? change / room : 1) : 0) : (shrink > 0 ? (-change < shrink ? change / shrink : -1) : 0);
	for (UINT32 index = 0; index < count; ++index) {
		const FLOAT moved = share * (change > 0 ? opportunities[index].expansionMaximum : opportunities[index].compressionMaximum);
		if (out_advances)
			out_advances[index] = advances[index] + moved;
		if (out_offsets)
			out_offsets[index] = offsets[index];
	}
	return S_OK;
}

/* No glyph changes shape when justified, so the run keeps its glyphs and takes the new advances. */
static HRESULT STDMETHODCALLTYPE ta_justified_glyphs(void *self, IDWriteFontFace *face, FLOAT size, DWRITE_SCRIPT_ANALYSIS analysis, UINT32 length, UINT32 count,
	UINT32 capacity, const UINT16 *clusters, const UINT16 *glyphs, const FLOAT *advances, const FLOAT *justified, const DWRITE_GLYPH_OFFSET *offsets,
	const DWRITE_SHAPING_GLYPH_PROPERTIES *properties, UINT32 *actual, UINT16 *out_clusters, UINT16 *out_glyphs, FLOAT *out_advances,
	DWRITE_GLYPH_OFFSET *out_offsets)
{
	if (!actual)
		return E_INVALIDARG;
	*actual = count;
	if (capacity < count)
		return E_NOT_SUFFICIENT_BUFFER;
	if (out_clusters && clusters)
		memmove(out_clusters, clusters, length * sizeof(UINT16));
	memmove(out_glyphs, glyphs, count * sizeof(UINT16));
	memmove(out_advances, justified, count * sizeof(FLOAT));
	memmove(out_offsets, offsets, count * sizeof(DWRITE_GLYPH_OFFSET));
	return S_OK;
}

/* The shaping here applies no OpenType features, so a font offers none through the analyzer. */
static HRESULT STDMETHODCALLTYPE ta_features(void *self, IDWriteFontFace *face, DWRITE_SCRIPT_ANALYSIS analysis, const WCHAR *locale, UINT32 capacity, UINT32 *actual,
	DWRITE_FONT_FEATURE_TAG *tags)
{
	if (!actual)
		return E_INVALIDARG;
	*actual = 0;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE ta_check_feature(void *self, IDWriteFontFace *face, DWRITE_SCRIPT_ANALYSIS analysis, const WCHAR *locale, DWRITE_FONT_FEATURE_TAG feature,
	UINT32 count, const UINT16 *glyphs, UINT8 *applies)
{
	if (count && !applies)
		return E_INVALIDARG;
	memset(applies, 0, count);
	return S_OK;
}

/* In the order of IDWriteTextAnalyzer2, which continues IDWriteTextAnalyzer1 and IDWriteTextAnalyzer. */
static const void *const text_analyzer_vtbl[] = {
	ta_query, ta_add_ref, ta_release, ta_script, ta_bidi, ta_numbers, ta_breaks, ta_glyphs, ta_placements, ta_gdi_placements,
	ta_spacing, ta_baseline, ta_vertical, ta_orientation, ta_script_properties, ta_complexity, ta_opportunities, ta_justify, ta_justified_glyphs,
	ta_orientation2, ta_features, ta_check_feature,
};

static HRESULT STDMETHODCALLTYPE factory_text_analyzer(IDWriteFactory2 *self, IDWriteTextAnalyzer **analyzer)
{
	TextAnalyzer *object;
	if (!analyzer)
		return E_INVALIDARG;
	*analyzer = NULL;
	if (!(object = ALLOC(sizeof(TextAnalyzer))))
		return E_OUTOFMEMORY;
	object->vtbl = text_analyzer_vtbl;
	object->refs = 1;
	*analyzer = (IDWriteTextAnalyzer *)object;
	return S_OK;
}
