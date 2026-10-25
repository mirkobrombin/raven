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
 * Scripts that need contextual shaping (Arabic joining, Indic reordering) are placed as their separate characters. */

/* Uniscribe's justification classes, which the glyph properties carry: SCRIPT_JUSTIFY_CHARACTER and SCRIPT_JUSTIFY_BLANK. */
enum { JUSTIFY_CHARACTER = 2, JUSTIFY_BLANK = 4 };

typedef struct {
	const IDWriteTextAnalyzerVtbl *vtbl;
	LONG refs;
} TextAnalyzer;

static HRESULT STDMETHODCALLTYPE ta_query(IDWriteTextAnalyzer *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteTextAnalyzer)) {
		InterlockedIncrement(&((TextAnalyzer *)self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
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
		codepoints[made] = rtl ? mirrored(code) : code;
		clusters[position] = (UINT16)made;
		memset(&text_properties[position], 0, sizeof(text_properties[position]));
		if (units == 2) {
			clusters[position + 1] = (UINT16)made;
			memset(&text_properties[position + 1], 0, sizeof(text_properties[position + 1]));
		}
		memset(&glyph_properties[made], 0, sizeof(glyph_properties[made]));
		glyph_properties[made].justification = is_space((WCHAR)code) ? JUSTIFY_BLANK : JUSTIFY_CHARACTER;
		glyph_properties[made].isClusterStart = 1;
		glyph_properties[made].isZeroWidthSpace = (analysis && analysis->shapes == DWRITE_SCRIPT_SHAPES_NO_VISUAL) || code == 0x200b;
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

static const IDWriteTextAnalyzerVtbl text_analyzer_vtbl = {
	ta_query, ta_add_ref, ta_release, ta_script, ta_bidi, ta_numbers, ta_breaks, ta_glyphs, ta_placements, ta_gdi_placements,
};

static HRESULT STDMETHODCALLTYPE factory_text_analyzer(IDWriteFactory2 *self, IDWriteTextAnalyzer **analyzer)
{
	TextAnalyzer *object;
	if (!analyzer)
		return E_INVALIDARG;
	*analyzer = NULL;
	if (!(object = ALLOC(sizeof(TextAnalyzer))))
		return E_OUTOFMEMORY;
	object->vtbl = &text_analyzer_vtbl;
	object->refs = 1;
	*analyzer = (IDWriteTextAnalyzer *)object;
	return S_OK;
}
