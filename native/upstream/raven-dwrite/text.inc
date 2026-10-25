/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* ---- text formats, text layouts and the GDI bitmap render target ----
 *
 * A text format keeps the font and paragraph settings. A text layout breaks a string into lines, measures them with the design metrics
 * of the faces and draws each line as glyph runs through the program's own text renderer, which is how Windows draws. The layout is
 * computed again whenever something it depends on changed. A bitmap render target is a memory device context whose bitmap receives the
 * glyph runs composed from the host's glyph textures. */

typedef struct {
	IDWriteFontFace *face;
	float size;
	UINT32 start, length, glyph_count;
	UINT16 *glyphs, *cluster_map;
	FLOAT *advances;
	float width, ascent, descent, gap;
} TextRun;

typedef struct {
	UINT32 start, length, newline, trailing;
	float width, height, baseline;
	TextRun *runs;
	UINT32 run_count;
} TextLine;

typedef struct {
	const IDWriteTextLayoutVtbl *vtbl;
	LONG refs;
	WCHAR *text, *family, *locale;
	UINT32 length;
	float *sizes;
	UINT32 *weights, *styles;
	WCHAR **families;
	DWRITE_FONT_STRETCH stretch;
	DWRITE_TEXT_ALIGNMENT alignment;
	DWRITE_PARAGRAPH_ALIGNMENT paragraph;
	DWRITE_WORD_WRAPPING wrapping;
	DWRITE_READING_DIRECTION reading;
	DWRITE_FLOW_DIRECTION flow;
	float tab, max_width, max_height, ppd, line_spacing, line_baseline;
	DWRITE_LINE_SPACING_METHOD spacing_method;
	DWRITE_TRIMMING trimming;
	BOOL gdi_compatible, natural, is_layout, dirty;
	TextLine *lines;
	UINT32 line_count;
	float total_height, widest, widest_with_space;
} TextLayout;

#define LAYOUT_OF(self) ((TextLayout *)(self))

static void free_lines(TextLayout *layout)
{
	UINT32 line, run;
	for (line = 0; line < layout->line_count; ++line) {
		for (run = 0; run < layout->lines[line].run_count; ++run) {
			TextRun *item = &layout->lines[line].runs[run];
			if (item->face)
				IDWriteFontFace_Release(item->face);
			FREE(item->glyphs);
			FREE(item->cluster_map);
			FREE(item->advances);
		}
		FREE(layout->lines[line].runs);
	}
	FREE(layout->lines);
	layout->lines = NULL;
	layout->line_count = 0;
}

static HRESULT STDMETHODCALLTYPE tl_query(IDWriteTextLayout *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteTextFormat) || (LAYOUT_OF(self)->is_layout && same_guid(iid, &IID_IDWriteTextLayout))) {
		InterlockedIncrement(&LAYOUT_OF(self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE tl_add_ref(IDWriteTextLayout *self) { return (ULONG)InterlockedIncrement(&LAYOUT_OF(self)->refs); }
static ULONG STDMETHODCALLTYPE tl_release(IDWriteTextLayout *self)
{
	TextLayout *layout = LAYOUT_OF(self);
	LONG refs = InterlockedDecrement(&layout->refs);
	if (!refs) {
		UINT32 index;
		free_lines(layout);
		if (layout->families)
			for (index = 0; index < layout->length; ++index)
				FREE(layout->families[index]);
		FREE(layout->families);
		FREE(layout->text);
		FREE(layout->family);
		FREE(layout->locale);
		FREE(layout->sizes);
		FREE(layout->weights);
		FREE(layout->styles);
		FREE(layout);
	}
	return (ULONG)refs;
}

/* Format settings: the setters mark the layout dirty. */
static HRESULT STDMETHODCALLTYPE tl_set_alignment(IDWriteTextLayout *self, DWRITE_TEXT_ALIGNMENT value) { LAYOUT_OF(self)->alignment = value; return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_set_paragraph(IDWriteTextLayout *self, DWRITE_PARAGRAPH_ALIGNMENT value) { LAYOUT_OF(self)->paragraph = value; return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_set_wrapping(IDWriteTextLayout *self, DWRITE_WORD_WRAPPING value) { LAYOUT_OF(self)->wrapping = value; LAYOUT_OF(self)->dirty = TRUE; return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_set_reading(IDWriteTextLayout *self, DWRITE_READING_DIRECTION value) { LAYOUT_OF(self)->reading = value; return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_set_flow(IDWriteTextLayout *self, DWRITE_FLOW_DIRECTION value) { LAYOUT_OF(self)->flow = value; return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_set_tab(IDWriteTextLayout *self, FLOAT value) { LAYOUT_OF(self)->tab = value; LAYOUT_OF(self)->dirty = TRUE; return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_set_trimming(IDWriteTextLayout *self, const DWRITE_TRIMMING *value, IDWriteInlineObject *sign)
{
	if (value)
		LAYOUT_OF(self)->trimming = *value;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE tl_set_line_spacing(IDWriteTextLayout *self, DWRITE_LINE_SPACING_METHOD method, FLOAT spacing, FLOAT baseline)
{
	LAYOUT_OF(self)->spacing_method = method;
	LAYOUT_OF(self)->line_spacing = spacing;
	LAYOUT_OF(self)->line_baseline = baseline;
	LAYOUT_OF(self)->dirty = TRUE;
	return S_OK;
}
static DWRITE_TEXT_ALIGNMENT STDMETHODCALLTYPE tl_get_alignment(IDWriteTextLayout *self) { return LAYOUT_OF(self)->alignment; }
static DWRITE_PARAGRAPH_ALIGNMENT STDMETHODCALLTYPE tl_get_paragraph(IDWriteTextLayout *self) { return LAYOUT_OF(self)->paragraph; }
static DWRITE_WORD_WRAPPING STDMETHODCALLTYPE tl_get_wrapping(IDWriteTextLayout *self) { return LAYOUT_OF(self)->wrapping; }
static DWRITE_READING_DIRECTION STDMETHODCALLTYPE tl_get_reading(IDWriteTextLayout *self) { return LAYOUT_OF(self)->reading; }
static DWRITE_FLOW_DIRECTION STDMETHODCALLTYPE tl_get_flow(IDWriteTextLayout *self) { return LAYOUT_OF(self)->flow; }
static FLOAT STDMETHODCALLTYPE tl_get_tab(IDWriteTextLayout *self) { return LAYOUT_OF(self)->tab; }
static HRESULT STDMETHODCALLTYPE tl_get_trimming(IDWriteTextLayout *self, DWRITE_TRIMMING *value, IDWriteInlineObject **sign)
{
	*value = LAYOUT_OF(self)->trimming;
	if (sign)
		*sign = NULL;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE tl_get_line_spacing(IDWriteTextLayout *self, DWRITE_LINE_SPACING_METHOD *method, FLOAT *spacing, FLOAT *baseline)
{
	*method = LAYOUT_OF(self)->spacing_method;
	*spacing = LAYOUT_OF(self)->line_spacing;
	*baseline = LAYOUT_OF(self)->line_baseline;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE tl_format_collection(IDWriteTextLayout *self, IDWriteFontCollection **collection)
{
	*collection = get_collection();
	return S_OK;
}
static UINT32 STDMETHODCALLTYPE tl_format_family_length(IDWriteTextLayout *self) { return (UINT32)wlen(LAYOUT_OF(self)->family); }
static HRESULT STDMETHODCALLTYPE tl_format_family(IDWriteTextLayout *self, WCHAR *name, UINT32 size)
{
	size_t length = wlen(LAYOUT_OF(self)->family);
	if (size <= length)
		return E_NOT_SUFFICIENT_BUFFER;
	memcpy(name, LAYOUT_OF(self)->family, (length + 1) * sizeof(WCHAR));
	return S_OK;
}
static DWRITE_FONT_WEIGHT STDMETHODCALLTYPE tl_format_weight(IDWriteTextLayout *self) { return (DWRITE_FONT_WEIGHT)(LAYOUT_OF(self)->length ? LAYOUT_OF(self)->weights[0] : 400); }
static DWRITE_FONT_STYLE STDMETHODCALLTYPE tl_format_style(IDWriteTextLayout *self) { return (DWRITE_FONT_STYLE)(LAYOUT_OF(self)->length ? LAYOUT_OF(self)->styles[0] : 0); }
static DWRITE_FONT_STRETCH STDMETHODCALLTYPE tl_format_stretch(IDWriteTextLayout *self) { return LAYOUT_OF(self)->stretch; }
static FLOAT STDMETHODCALLTYPE tl_format_size(IDWriteTextLayout *self) { return LAYOUT_OF(self)->length ? LAYOUT_OF(self)->sizes[0] : 0.0f; }
static UINT32 STDMETHODCALLTYPE tl_format_locale_length(IDWriteTextLayout *self) { return (UINT32)wlen(LAYOUT_OF(self)->locale); }
static HRESULT STDMETHODCALLTYPE tl_format_locale(IDWriteTextLayout *self, WCHAR *name, UINT32 size)
{
	size_t length = wlen(LAYOUT_OF(self)->locale);
	if (size <= length)
		return E_NOT_SUFFICIENT_BUFFER;
	memcpy(name, LAYOUT_OF(self)->locale, (length + 1) * sizeof(WCHAR));
	return S_OK;
}

/* Layout settings. */
static HRESULT STDMETHODCALLTYPE tl_set_max_width(IDWriteTextLayout *self, FLOAT value) { LAYOUT_OF(self)->max_width = value; LAYOUT_OF(self)->dirty = TRUE; return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_set_max_height(IDWriteTextLayout *self, FLOAT value) { LAYOUT_OF(self)->max_height = value; return S_OK; }
static FLOAT STDMETHODCALLTYPE tl_get_max_width(IDWriteTextLayout *self) { return LAYOUT_OF(self)->max_width; }
static FLOAT STDMETHODCALLTYPE tl_get_max_height(IDWriteTextLayout *self) { return LAYOUT_OF(self)->max_height; }
static HRESULT STDMETHODCALLTYPE tl_set_collection(IDWriteTextLayout *self, IDWriteFontCollection *collection, DWRITE_TEXT_RANGE range) { return S_OK; }

static void clamp_range(const TextLayout *layout, DWRITE_TEXT_RANGE *range)
{
	if (range->startPosition > layout->length)
		range->startPosition = layout->length;
	if (range->length > layout->length - range->startPosition)
		range->length = layout->length - range->startPosition;
}
static HRESULT STDMETHODCALLTYPE tl_set_family(IDWriteTextLayout *self, const WCHAR *name, DWRITE_TEXT_RANGE range)
{
	TextLayout *layout = LAYOUT_OF(self);
	UINT32 index;
	clamp_range(layout, &range);
	for (index = range.startPosition; index < range.startPosition + range.length; ++index) {
		FREE(layout->families[index]);
		layout->families[index] = wdup(name);
	}
	layout->dirty = TRUE;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE tl_set_weight(IDWriteTextLayout *self, DWRITE_FONT_WEIGHT value, DWRITE_TEXT_RANGE range)
{
	TextLayout *layout = LAYOUT_OF(self);
	UINT32 index;
	clamp_range(layout, &range);
	for (index = range.startPosition; index < range.startPosition + range.length; ++index)
		layout->weights[index] = value;
	layout->dirty = TRUE;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE tl_set_style(IDWriteTextLayout *self, DWRITE_FONT_STYLE value, DWRITE_TEXT_RANGE range)
{
	TextLayout *layout = LAYOUT_OF(self);
	UINT32 index;
	clamp_range(layout, &range);
	for (index = range.startPosition; index < range.startPosition + range.length; ++index)
		layout->styles[index] = value;
	layout->dirty = TRUE;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE tl_set_stretch(IDWriteTextLayout *self, DWRITE_FONT_STRETCH value, DWRITE_TEXT_RANGE range) { LAYOUT_OF(self)->stretch = value; LAYOUT_OF(self)->dirty = TRUE; return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_set_size(IDWriteTextLayout *self, FLOAT value, DWRITE_TEXT_RANGE range)
{
	TextLayout *layout = LAYOUT_OF(self);
	UINT32 index;
	clamp_range(layout, &range);
	for (index = range.startPosition; index < range.startPosition + range.length; ++index)
		layout->sizes[index] = value;
	layout->dirty = TRUE;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE tl_set_underline(IDWriteTextLayout *self, WINBOOL value, DWRITE_TEXT_RANGE range) { return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_set_strikethrough(IDWriteTextLayout *self, WINBOOL value, DWRITE_TEXT_RANGE range) { return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_set_effect(IDWriteTextLayout *self, IUnknown *effect, DWRITE_TEXT_RANGE range) { return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_set_locale(IDWriteTextLayout *self, const WCHAR *name, DWRITE_TEXT_RANGE range) { return S_OK; }

/* Per-position getters report the value at the position and the range it holds over. */
static HRESULT position_range(TextLayout *layout, UINT32 position, DWRITE_TEXT_RANGE *range)
{
	if (position >= layout->length)
		return E_INVALIDARG;
	if (range) {
		range->startPosition = position;
		range->length = layout->length - position;
	}
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE tl_get_weight_at(IDWriteTextLayout *self, UINT32 position, DWRITE_FONT_WEIGHT *value, DWRITE_TEXT_RANGE *range)
{
	HRESULT result = position_range(LAYOUT_OF(self), position, range);
	if (SUCCEEDED(result))
		*value = (DWRITE_FONT_WEIGHT)LAYOUT_OF(self)->weights[position];
	return result;
}
static HRESULT STDMETHODCALLTYPE tl_get_style_at(IDWriteTextLayout *self, UINT32 position, DWRITE_FONT_STYLE *value, DWRITE_TEXT_RANGE *range)
{
	HRESULT result = position_range(LAYOUT_OF(self), position, range);
	if (SUCCEEDED(result))
		*value = (DWRITE_FONT_STYLE)LAYOUT_OF(self)->styles[position];
	return result;
}
static HRESULT STDMETHODCALLTYPE tl_get_stretch_at(IDWriteTextLayout *self, UINT32 position, DWRITE_FONT_STRETCH *value, DWRITE_TEXT_RANGE *range)
{
	HRESULT result = position_range(LAYOUT_OF(self), position, range);
	if (SUCCEEDED(result))
		*value = LAYOUT_OF(self)->stretch;
	return result;
}
static HRESULT STDMETHODCALLTYPE tl_get_size_at(IDWriteTextLayout *self, UINT32 position, FLOAT *value, DWRITE_TEXT_RANGE *range)
{
	HRESULT result = position_range(LAYOUT_OF(self), position, range);
	if (SUCCEEDED(result))
		*value = LAYOUT_OF(self)->sizes[position];
	return result;
}
static HRESULT STDMETHODCALLTYPE tl_get_collection_at(IDWriteTextLayout *self, UINT32 position, IDWriteFontCollection **collection, DWRITE_TEXT_RANGE *range)
{
	HRESULT result = position_range(LAYOUT_OF(self), position, range);
	if (SUCCEEDED(result))
		*collection = get_collection();
	return result;
}
static const WCHAR *family_at(const TextLayout *layout, UINT32 position) { return layout->families[position] ? layout->families[position] : layout->family; }
static HRESULT STDMETHODCALLTYPE tl_get_family_length_at(IDWriteTextLayout *self, UINT32 position, UINT32 *length, DWRITE_TEXT_RANGE *range)
{
	HRESULT result = position_range(LAYOUT_OF(self), position, range);
	if (SUCCEEDED(result))
		*length = (UINT32)wlen(family_at(LAYOUT_OF(self), position));
	return result;
}
static HRESULT STDMETHODCALLTYPE tl_get_family_at(IDWriteTextLayout *self, UINT32 position, WCHAR *name, UINT32 size, DWRITE_TEXT_RANGE *range)
{
	HRESULT result = position_range(LAYOUT_OF(self), position, range);
	const WCHAR *family;
	size_t length;
	if (FAILED(result))
		return result;
	family = family_at(LAYOUT_OF(self), position);
	length = wlen(family);
	if (size <= length)
		return E_NOT_SUFFICIENT_BUFFER;
	memcpy(name, family, (length + 1) * sizeof(WCHAR));
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE tl_get_underline_at(IDWriteTextLayout *self, UINT32 position, WINBOOL *value, DWRITE_TEXT_RANGE *range)
{
	HRESULT result = position_range(LAYOUT_OF(self), position, range);
	if (SUCCEEDED(result))
		*value = FALSE;
	return result;
}
static HRESULT STDMETHODCALLTYPE tl_get_effect_at(IDWriteTextLayout *self, UINT32 position, IUnknown **effect, DWRITE_TEXT_RANGE *range)
{
	HRESULT result = position_range(LAYOUT_OF(self), position, range);
	if (SUCCEEDED(result))
		*effect = NULL;
	return result;
}
static HRESULT STDMETHODCALLTYPE tl_get_locale_length_at(IDWriteTextLayout *self, UINT32 position, UINT32 *length, DWRITE_TEXT_RANGE *range)
{
	HRESULT result = position_range(LAYOUT_OF(self), position, range);
	if (SUCCEEDED(result))
		*length = (UINT32)wlen(LAYOUT_OF(self)->locale);
	return result;
}
static HRESULT STDMETHODCALLTYPE tl_get_locale_at(IDWriteTextLayout *self, UINT32 position, WCHAR *name, UINT32 size, DWRITE_TEXT_RANGE *range)
{
	HRESULT result = position_range(LAYOUT_OF(self), position, range);
	size_t length = wlen(LAYOUT_OF(self)->locale);
	if (FAILED(result))
		return result;
	if (size <= length)
		return E_NOT_SUFFICIENT_BUFFER;
	memcpy(name, LAYOUT_OF(self)->locale, (length + 1) * sizeof(WCHAR));
	return S_OK;
}

/* The face for one run's font settings, from the system collection with the default family as the fallback. */
static IDWriteFontFace *face_for(const WCHAR *family_name, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STRETCH stretch, DWRITE_FONT_STYLE style)
{
	FamilyEntry *family;
	FontEntry *entry;
	IDWriteFont *font;
	IDWriteFontFace *face = NULL;
	ensure_system();
	family = find_family(family_name);
	if (!family)
		family = find_family(default_family);
	if (!family && family_count)
		family = &families[0];
	if (!family)
		return NULL;
	entry = best_match(family, weight, stretch, style);
	if (!entry)
		return NULL;
	font = make_font(entry, family);
	if (!font)
		return NULL;
	if (FAILED(IDWriteFont_CreateFontFace(font, &face)))
		face = NULL;
	IDWriteFont_Release(font);
	return face;
}

static float round_float(float value) { return (float)(int)(value + (value < 0 ? -0.5f : 0.5f)); }

/* Builds a run of the characters [start, end) that share their font settings: glyphs, advances and the font's vertical metrics. */
static BOOL build_run(TextLayout *layout, UINT32 start, UINT32 end, TextRun *run)
{
	DWRITE_FONT_METRICS metrics;
	UINT32 position, count = 0;
	UINT32 *codepoints;
	memset(run, 0, sizeof(*run));
	run->start = start;
	run->length = end - start;
	run->size = layout->sizes[start];
	run->face = face_for(family_at(layout, start), (DWRITE_FONT_WEIGHT)layout->weights[start], layout->stretch, (DWRITE_FONT_STYLE)layout->styles[start]);
	if (!run->face)
		return FALSE;
	codepoints = ALLOC((end - start) * sizeof(UINT32));
	run->glyphs = ALLOC((end - start) * sizeof(UINT16));
	run->cluster_map = ALLOC((end - start) * sizeof(UINT16));
	run->advances = ALLOC((end - start) * sizeof(FLOAT));
	if (!codepoints || !run->glyphs || !run->cluster_map || !run->advances) {
		FREE(codepoints);
		return FALSE;
	}
	for (position = start; position < end;) {
		UINT32 units = 1, code = layout->text[position];
		if (code >= 0xd800 && code < 0xdc00 && position + 1 < end && layout->text[position + 1] >= 0xdc00 && layout->text[position + 1] < 0xe000) {
			code = 0x10000 + ((code - 0xd800) << 10) + (layout->text[position + 1] - 0xdc00);
			units = 2;
		}
		/* A tab or a line break takes no glyph of its own: it is a space as wide as the text around it. */
		if (code == '\t' || code == '\n' || code == '\r')
			code = ' ';
		codepoints[count] = code;
		run->cluster_map[position - start] = (UINT16)count;
		if (units == 2)
			run->cluster_map[position - start + 1] = (UINT16)count;
		position += units;
		++count;
	}
	run->glyph_count = count;
	IDWriteFontFace_GetGlyphIndices(run->face, codepoints, count, run->glyphs);
	IDWriteFontFace_GetMetrics(run->face, &metrics);
	{
		UINT32 glyph;
		float scale = run->size / (float)metrics.designUnitsPerEm;
		for (glyph = 0; glyph < count; ++glyph) {
			DWRITE_GLYPH_METRICS design;
			float advance;
			if (SUCCEEDED(IDWriteFontFace_GetDesignGlyphMetrics(run->face, &run->glyphs[glyph], 1, &design, FALSE)))
				advance = (float)design.advanceWidth * scale;
			else
				advance = run->size * 0.5f;
			/* Layouts made for GDI use whole-pixel advances, as GDI does. */
			if (layout->gdi_compatible)
				advance = round_float(advance * layout->ppd) / layout->ppd;
			run->advances[glyph] = advance;
			run->width += advance;
		}
		run->ascent = (float)metrics.ascent * scale;
		run->descent = (float)metrics.descent * scale;
		run->gap = (float)metrics.lineGap * scale;
		if (layout->gdi_compatible) {
			run->ascent = round_float(run->ascent * layout->ppd) / layout->ppd;
			run->descent = round_float(run->descent * layout->ppd) / layout->ppd;
			run->gap = round_float(run->gap * layout->ppd) / layout->ppd;
		}
	}
	FREE(codepoints);
	return TRUE;
}

/* Does the character before `position` allow a line to end there? */
static BOOL can_break_after(const TextLayout *layout, UINT32 position)
{
	WCHAR c = layout->text[position];
	return c == ' ' || c == '\t' || c == '-' || c == 0x200b;
}

static void relayout(TextLayout *layout)
{
	UINT32 position = 0, capacity = 0;
	float limit = layout->wrapping == DWRITE_WORD_WRAPPING_NO_WRAP ? 3.4e38f : layout->max_width;
	if (!layout->dirty)
		return;
	free_lines(layout);
	layout->dirty = FALSE;
	layout->total_height = layout->widest = layout->widest_with_space = 0;
	while (position < layout->length || (!layout->line_count && !layout->length)) {
		UINT32 start = position, end = position, brk = 0, content_end, newline = 0, run_start, trailing = 0;
		float width = 0, last_break_width = 0;
		TextLine *line;
		BOOL hard = FALSE;
		/* Find where the line ends: at a line break, or at the last break opportunity before the text overflows. */
		while (end < layout->length) {
			WCHAR c = layout->text[end];
			float advance = 0;
			if (c == '\r' || c == '\n') {
				hard = TRUE;
				break;
			}
			{
				/* The advance of one character, from a one-character run of its settings. */
				TextRun probe;
				if (build_run(layout, end, end + 1, &probe)) {
					advance = probe.advances[0];
					IDWriteFontFace_Release(probe.face);
					FREE(probe.glyphs);
					FREE(probe.cluster_map);
					FREE(probe.advances);
				}
			}
			if (c == '\t' && layout->tab > 0)
				advance = layout->tab - (float)(int)(width / layout->tab) * layout->tab;
			if (width + advance > limit && end > start && c != ' ' && c != '\t') {
				if (brk > start) {
					end = brk;
					width = last_break_width;
				}
				break;
			}
			width += advance;
			++end;
			if (can_break_after(layout, end - 1)) {
				brk = end;
				last_break_width = width;
			}
		}
		content_end = end;
		if (hard) {
			newline = (layout->text[end] == '\r' && end + 1 < layout->length && layout->text[end + 1] == '\n') ? 2 : 1;
		} else if (end < layout->length) {
			/* A wrapped line keeps the spaces that follow its last word. */
			while (end < layout->length && layout->text[end] == ' ')
				++end;
			content_end = end;
		}
		while (content_end > start && layout->text[content_end - 1] == ' ') {
			--content_end;
			++trailing;
		}
		if (layout->line_count == capacity) {
			TextLine *grown;
			capacity = capacity ? capacity * 2 : 8;
			grown = ALLOC(capacity * sizeof(TextLine));
			if (!grown)
				return;
			if (layout->lines) {
				memcpy(grown, layout->lines, layout->line_count * sizeof(TextLine));
				FREE(layout->lines);
			}
			layout->lines = grown;
		}
		line = &layout->lines[layout->line_count++];
		memset(line, 0, sizeof(*line));
		line->start = start;
		line->length = end - start;
		line->newline = newline;
		line->trailing = trailing;
		/* The runs of the line: stretches of characters with the same font settings. */
		run_start = start;
		{
			UINT32 runs = 0, limit_runs = 4;
			line->runs = ALLOC(limit_runs * sizeof(TextRun));
			while (run_start < end) {
				UINT32 run_end = run_start + 1;
				TextRun *run;
				while (run_end < end && layout->sizes[run_end] == layout->sizes[run_start] && layout->weights[run_end] == layout->weights[run_start] &&
					layout->styles[run_end] == layout->styles[run_start] && family_at(layout, run_end) == family_at(layout, run_start))
					++run_end;
				if (runs == limit_runs) {
					TextRun *grown = ALLOC(limit_runs * 2 * sizeof(TextRun));
					memcpy(grown, line->runs, runs * sizeof(TextRun));
					FREE(line->runs);
					line->runs = grown;
					limit_runs *= 2;
				}
				run = &line->runs[runs];
				if (build_run(layout, run_start, run_end, run)) {
					float ascent = run->ascent, height = run->ascent + run->descent + run->gap;
					if (ascent > line->baseline)
						line->baseline = ascent;
					if (height > line->height)
						line->height = height;
					++runs;
				}
				run_start = run_end;
			}
			line->run_count = runs;
		}
		if (!line->height && layout->line_count) {
			/* An empty line is as tall as the text's font. */
			TextRun probe;
			if (layout->length && build_run(layout, layout->length - 1, layout->length, &probe)) {
				line->baseline = probe.ascent;
				line->height = probe.ascent + probe.descent + probe.gap;
				IDWriteFontFace_Release(probe.face);
				FREE(probe.glyphs);
				FREE(probe.cluster_map);
				FREE(probe.advances);
			}
		}
		{
			UINT32 run;
			for (run = 0; run < line->run_count; ++run)
				line->width += line->runs[run].width;
		}
		/* Spaces at the end of a line do not count toward its width. */
		{
			float spaces = 0;
			UINT32 index;
			for (index = 0; index < trailing && line->run_count; ++index) {
				TextRun *last = &line->runs[line->run_count - 1];
				UINT32 glyph = (UINT32)last->cluster_map[end - last->start - 1 - index];
				spaces += last->advances[glyph];
			}
			if (line->width > layout->widest_with_space)
				layout->widest_with_space = line->width;
			line->width -= spaces;
		}
		if (layout->spacing_method == DWRITE_LINE_SPACING_METHOD_UNIFORM && layout->line_spacing > 0) {
			line->height = layout->line_spacing;
			line->baseline = layout->line_baseline;
		}
		if (line->width > layout->widest)
			layout->widest = line->width;
		layout->total_height += line->height;
		position = end + newline;
		if (hard && position >= layout->length)
			break;
		if (!layout->length)
			break;
	}
}

static HRESULT STDMETHODCALLTYPE tl_line_metrics(IDWriteTextLayout *self, DWRITE_LINE_METRICS *metrics, UINT32 max, UINT32 *count)
{
	TextLayout *layout = LAYOUT_OF(self);
	UINT32 index;
	relayout(layout);
	*count = layout->line_count;
	if (!metrics || max < layout->line_count)
		return max < layout->line_count && metrics ? E_NOT_SUFFICIENT_BUFFER : S_OK;
	for (index = 0; index < layout->line_count; ++index) {
		const TextLine *line = &layout->lines[index];
		metrics[index].length = line->length + line->newline;
		metrics[index].trailingWhitespaceLength = line->trailing;
		metrics[index].newlineLength = line->newline;
		metrics[index].height = line->height;
		metrics[index].baseline = line->baseline;
		metrics[index].isTrimmed = FALSE;
	}
	return S_OK;
}

static float alignment_offset(const TextLayout *layout, float width)
{
	float room = layout->max_width - width;
	if (layout->max_width > 1e9f)
		return 0;
	if (layout->alignment == DWRITE_TEXT_ALIGNMENT_TRAILING)
		return room;
	if (layout->alignment == DWRITE_TEXT_ALIGNMENT_CENTER)
		return room / 2;
	return 0;
}

static float paragraph_offset(const TextLayout *layout)
{
	float room = layout->max_height - layout->total_height;
	if (layout->max_height > 1e9f)
		return 0;
	if (layout->paragraph == DWRITE_PARAGRAPH_ALIGNMENT_FAR)
		return room;
	if (layout->paragraph == DWRITE_PARAGRAPH_ALIGNMENT_CENTER)
		return room / 2;
	return 0;
}

static HRESULT STDMETHODCALLTYPE tl_metrics(IDWriteTextLayout *self, DWRITE_TEXT_METRICS *metrics)
{
	TextLayout *layout = LAYOUT_OF(self);
	relayout(layout);
	memset(metrics, 0, sizeof(*metrics));
	metrics->left = alignment_offset(layout, layout->widest);
	metrics->top = paragraph_offset(layout);
	metrics->width = layout->widest;
	metrics->widthIncludingTrailingWhitespace = layout->widest_with_space;
	metrics->height = layout->total_height;
	metrics->layoutWidth = layout->max_width;
	metrics->layoutHeight = layout->max_height;
	metrics->maxBidiReorderingDepth = 1;
	metrics->lineCount = layout->line_count;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE tl_overhang(IDWriteTextLayout *self, DWRITE_OVERHANG_METRICS *metrics)
{
	memset(metrics, 0, sizeof(*metrics));
	return S_OK;
}

/* One cluster per character, with the width of its glyph. */
static HRESULT STDMETHODCALLTYPE tl_clusters(IDWriteTextLayout *self, DWRITE_CLUSTER_METRICS *metrics, UINT32 max, UINT32 *count)
{
	TextLayout *layout = LAYOUT_OF(self);
	UINT32 line_index, total = 0;
	relayout(layout);
	for (line_index = 0; line_index < layout->line_count; ++line_index) {
		const TextLine *line = &layout->lines[line_index];
		UINT32 run_index;
		for (run_index = 0; run_index < line->run_count; ++run_index) {
			const TextRun *run = &line->runs[run_index];
			UINT32 glyph;
			for (glyph = 0; glyph < run->glyph_count; ++glyph) {
				if (metrics && total < max) {
					UINT32 unit, first = run->length;
					DWRITE_CLUSTER_METRICS *item = &metrics[total];
					memset(item, 0, sizeof(*item));
					for (unit = 0; unit < run->length; ++unit)
						if (run->cluster_map[unit] == glyph) {
							first = unit;
							break;
						}
					item->width = run->advances[glyph];
					item->length = (run->glyph_count && glyph + 1 < run->glyph_count) ? 1 : 1;
					if (first < run->length) {
						WCHAR c = layout->text[run->start + first];
						item->isWhitespace = c == ' ' || c == '\t';
						item->isNewline = c == '\n' || c == '\r';
						item->canWrapLineAfter = can_break_after(layout, run->start + first);
						if (c >= 0xd800 && c < 0xdc00)
							item->length = 2;
					}
				}
				++total;
			}
		}
	}
	*count = total;
	return metrics && max < total ? E_NOT_SUFFICIENT_BUFFER : S_OK;
}

static HRESULT STDMETHODCALLTYPE tl_min_width(IDWriteTextLayout *self, FLOAT *width)
{
	TextLayout *layout = LAYOUT_OF(self);
	float best = 0, current = 0;
	UINT32 line_index;
	relayout(layout);
	for (line_index = 0; line_index < layout->line_count; ++line_index) {
		const TextLine *line = &layout->lines[line_index];
		UINT32 run_index;
		for (run_index = 0; run_index < line->run_count; ++run_index) {
			const TextRun *run = &line->runs[run_index];
			UINT32 unit;
			for (unit = 0; unit < run->length; ++unit) {
				WCHAR c = layout->text[run->start + unit];
				float advance = run->advances[run->cluster_map[unit]];
				if (c == ' ' || c == '\t') {
					current = 0;
				} else {
					current += advance;
					if (current > best)
						best = current;
				}
			}
		}
		current = 0;
	}
	*width = best;
	return S_OK;
}

/* The x of a text position on its line, from the advances of the characters before it. */
static BOOL locate_position(TextLayout *layout, UINT32 position, UINT32 *line_index, float *x, float *width)
{
	UINT32 index;
	relayout(layout);
	for (index = 0; index < layout->line_count; ++index) {
		const TextLine *line = &layout->lines[index];
		if (position < line->start + line->length + line->newline || index + 1 == layout->line_count) {
			float pen = alignment_offset(layout, line->width);
			UINT32 run_index;
			*width = 0;
			for (run_index = 0; run_index < line->run_count; ++run_index) {
				const TextRun *run = &line->runs[run_index];
				UINT32 unit;
				for (unit = 0; unit < run->length; ++unit) {
					float advance = run->cluster_map[unit] < run->glyph_count ? run->advances[run->cluster_map[unit]] : 0;
					if (run->start + unit == position) {
						*line_index = index;
						*x = pen;
						*width = advance;
						return TRUE;
					}
					pen += advance;
				}
			}
			*line_index = index;
			*x = pen;
			return TRUE;
		}
	}
	return FALSE;
}

static float line_top(const TextLayout *layout, UINT32 index)
{
	float top = paragraph_offset(layout);
	UINT32 before;
	for (before = 0; before < index; ++before)
		top += layout->lines[before].height;
	return top;
}

static HRESULT STDMETHODCALLTYPE tl_hit_point(IDWriteTextLayout *self, FLOAT x, FLOAT y, WINBOOL *trailing, WINBOOL *inside, DWRITE_HIT_TEST_METRICS *metrics)
{
	TextLayout *layout = LAYOUT_OF(self);
	UINT32 index, found = 0;
	float top;
	relayout(layout);
	memset(metrics, 0, sizeof(*metrics));
	*trailing = FALSE;
	*inside = FALSE;
	if (!layout->line_count)
		return S_OK;
	for (index = 0; index < layout->line_count; ++index) {
		found = index;
		top = line_top(layout, index);
		if (y < top + layout->lines[index].height)
			break;
	}
	{
		const TextLine *line = &layout->lines[found];
		float pen = alignment_offset(layout, line->width);
		UINT32 run_index, position = line->start;
		top = line_top(layout, found);
		for (run_index = 0; run_index < line->run_count; ++run_index) {
			const TextRun *run = &line->runs[run_index];
			UINT32 unit;
			for (unit = 0; unit < run->length; ++unit) {
				float advance = run->cluster_map[unit] < run->glyph_count ? run->advances[run->cluster_map[unit]] : 0;
				position = run->start + unit;
				if (x < pen + advance) {
					metrics->textPosition = position;
					metrics->length = 1;
					metrics->left = pen;
					metrics->top = top;
					metrics->width = advance;
					metrics->height = line->height;
					metrics->isText = TRUE;
					*trailing = x >= pen + advance / 2;
					*inside = x >= pen && y >= top && y < top + line->height;
					return S_OK;
				}
				pen += advance;
			}
		}
		metrics->textPosition = line->start + line->length;
		metrics->left = pen;
		metrics->top = top;
		metrics->height = line->height;
		*trailing = TRUE;
	}
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE tl_hit_position(IDWriteTextLayout *self, UINT32 position, WINBOOL trailing, FLOAT *x, FLOAT *y, DWRITE_HIT_TEST_METRICS *metrics)
{
	TextLayout *layout = LAYOUT_OF(self);
	UINT32 line_index = 0;
	float left = 0, width = 0;
	if (!locate_position(layout, position, &line_index, &left, &width))
		return E_FAIL;
	*x = left + (trailing ? width : 0);
	*y = line_top(layout, line_index);
	if (metrics) {
		memset(metrics, 0, sizeof(*metrics));
		metrics->textPosition = position;
		metrics->length = 1;
		metrics->left = left;
		metrics->top = *y;
		metrics->width = width;
		metrics->height = layout->lines[line_index].height;
		metrics->isText = TRUE;
	}
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE tl_hit_range(IDWriteTextLayout *self, UINT32 position, UINT32 length, FLOAT x, FLOAT y, DWRITE_HIT_TEST_METRICS *metrics, UINT32 max, UINT32 *count)
{
	TextLayout *layout = LAYOUT_OF(self);
	UINT32 line_index = 0, last_line = 0;
	float left = 0, width = 0, right = 0, last_left = 0, last_width = 0;
	*count = 1;
	if (!locate_position(layout, position, &line_index, &left, &width) ||
		!locate_position(layout, length ? position + length - 1 : position, &last_line, &last_left, &last_width))
		return E_FAIL;
	right = last_left + last_width;
	if (!metrics || !max)
		return metrics ? E_NOT_SUFFICIENT_BUFFER : S_OK;
	memset(&metrics[0], 0, sizeof(metrics[0]));
	metrics[0].textPosition = position;
	metrics[0].length = length;
	metrics[0].left = x + left;
	metrics[0].top = y + line_top(layout, line_index);
	metrics[0].width = (last_line == line_index ? right : layout->widest) - left;
	metrics[0].height = layout->lines[line_index].height;
	metrics[0].isText = TRUE;
	return S_OK;
}

/* Draws each line as a glyph run through the program's renderer. */
static HRESULT STDMETHODCALLTYPE tl_draw(IDWriteTextLayout *self, void *context, IDWriteTextRenderer *renderer, FLOAT origin_x, FLOAT origin_y)
{
	TextLayout *layout = LAYOUT_OF(self);
	UINT32 line_index;
	float top;
	relayout(layout);
	top = origin_y + paragraph_offset(layout);
	for (line_index = 0; line_index < layout->line_count; ++line_index) {
		const TextLine *line = &layout->lines[line_index];
		float pen = origin_x + alignment_offset(layout, line->width);
		UINT32 run_index;
		for (run_index = 0; run_index < line->run_count; ++run_index) {
			const TextRun *run = &line->runs[run_index];
			DWRITE_GLYPH_RUN glyph_run;
			DWRITE_GLYPH_RUN_DESCRIPTION description;
			HRESULT result;
			memset(&glyph_run, 0, sizeof(glyph_run));
			glyph_run.fontFace = run->face;
			glyph_run.fontEmSize = run->size;
			glyph_run.glyphCount = run->glyph_count;
			glyph_run.glyphIndices = run->glyphs;
			glyph_run.glyphAdvances = run->advances;
			description.localeName = layout->locale;
			description.string = layout->text + run->start;
			description.stringLength = run->length;
			description.clusterMap = run->cluster_map;
			description.textPosition = run->start;
			result = renderer->lpVtbl->DrawGlyphRun(renderer, context, pen, top + line->baseline,
				layout->gdi_compatible && !layout->natural ? DWRITE_MEASURING_MODE_GDI_CLASSIC : layout->gdi_compatible ? DWRITE_MEASURING_MODE_GDI_NATURAL : DWRITE_MEASURING_MODE_NATURAL,
				&glyph_run, &description, NULL);
			if (FAILED(result))
				return result;
			pen += run->width;
		}
		top += line->height;
	}
	return S_OK;
}

static void *Layout_table_[IDWriteTextLayout_methods];
static void *Format_table_[IDWriteTextFormat_methods];
static BOOL text_tables_ready;

static void init_text_tables(void)
{
	if (text_tables_ready)
		return;
	memcpy(Layout_table_, IDWriteTextLayout_table, sizeof(Layout_table_));
#define SET(name, function) Layout_table_[IDWriteTextLayout_##name] = (void *)function
	SET(QueryInterface, tl_query); SET(AddRef, tl_add_ref); SET(Release, tl_release);
	SET(SetTextAlignment, tl_set_alignment); SET(SetParagraphAlignment, tl_set_paragraph); SET(SetWordWrapping, tl_set_wrapping);
	SET(SetReadingDirection, tl_set_reading); SET(SetFlowDirection, tl_set_flow); SET(SetIncrementalTabStop, tl_set_tab);
	SET(SetTrimming, tl_set_trimming); SET(SetLineSpacing, tl_set_line_spacing);
	SET(GetTextAlignment, tl_get_alignment); SET(GetParagraphAlignment, tl_get_paragraph); SET(GetWordWrapping, tl_get_wrapping);
	SET(GetReadingDirection, tl_get_reading); SET(GetFlowDirection, tl_get_flow); SET(GetIncrementalTabStop, tl_get_tab);
	SET(GetTrimming, tl_get_trimming); SET(GetLineSpacing, tl_get_line_spacing);
	SET(GetFontCollection, tl_format_collection); SET(GetFontFamilyNameLength, tl_format_family_length); SET(GetFontFamilyName, tl_format_family);
	SET(GetFontWeight, tl_format_weight); SET(GetFontStyle, tl_format_style); SET(GetFontStretch, tl_format_stretch); SET(GetFontSize, tl_format_size);
	SET(GetLocaleNameLength, tl_format_locale_length); SET(GetLocaleName, tl_format_locale);
	SET(SetMaxWidth, tl_set_max_width); SET(SetMaxHeight, tl_set_max_height); SET(SetFontCollection, tl_set_collection);
	SET(SetFontFamilyName, tl_set_family); SET(SetFontWeight, tl_set_weight); SET(SetFontStyle, tl_set_style); SET(SetFontStretch, tl_set_stretch);
	SET(SetFontSize, tl_set_size); SET(SetUnderline, tl_set_underline); SET(SetStrikethrough, tl_set_strikethrough);
	SET(SetDrawingEffect, tl_set_effect); SET(SetLocaleName, tl_set_locale);
	SET(GetMaxWidth, tl_get_max_width); SET(GetMaxHeight, tl_get_max_height);
	SET(IDWriteTextLayout_GetFontCollection, tl_get_collection_at); SET(IDWriteTextLayout_GetFontFamilyNameLength, tl_get_family_length_at);
	SET(IDWriteTextLayout_GetFontFamilyName, tl_get_family_at); SET(IDWriteTextLayout_GetFontWeight, tl_get_weight_at);
	SET(IDWriteTextLayout_GetFontStyle, tl_get_style_at); SET(IDWriteTextLayout_GetFontStretch, tl_get_stretch_at); SET(IDWriteTextLayout_GetFontSize, tl_get_size_at);
	SET(GetUnderline, tl_get_underline_at); SET(GetStrikethrough, tl_get_underline_at); SET(GetDrawingEffect, tl_get_effect_at);
	SET(IDWriteTextLayout_GetLocaleNameLength, tl_get_locale_length_at); SET(IDWriteTextLayout_GetLocaleName, tl_get_locale_at);
	SET(Draw, tl_draw); SET(GetLineMetrics, tl_line_metrics); SET(GetMetrics, tl_metrics); SET(GetOverhangMetrics, tl_overhang);
	SET(GetClusterMetrics, tl_clusters); SET(DetermineMinWidth, tl_min_width); SET(HitTestPoint, tl_hit_point);
	SET(HitTestTextPosition, tl_hit_position); SET(HitTestTextRange, tl_hit_range);
#undef SET
	/* A text format is the front of the same object: its table is the layout's methods that a format has. */
	memcpy(Format_table_, Layout_table_, sizeof(Format_table_));
	text_tables_ready = TRUE;
}

static TextLayout *make_layout(const WCHAR *text, UINT32 length, const WCHAR *family, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style,
	DWRITE_FONT_STRETCH stretch, FLOAT size, const WCHAR *locale, BOOL is_layout)
{
	TextLayout *layout;
	UINT32 index;
	init_text_tables();
	layout = ALLOC(sizeof(TextLayout));
	if (!layout)
		return NULL;
	layout->vtbl = (const IDWriteTextLayoutVtbl *)(is_layout ? Layout_table_ : Format_table_);
	layout->refs = 1;
	layout->length = length;
	layout->text = ALLOC((length + 1) * sizeof(WCHAR));
	layout->family = wdup(family);
	layout->locale = wdup(locale ? locale : L"en-us");
	layout->sizes = ALLOC((length + 1) * sizeof(float));
	layout->weights = ALLOC((length + 1) * sizeof(UINT32));
	layout->styles = ALLOC((length + 1) * sizeof(UINT32));
	layout->families = ALLOC((length + 1) * sizeof(WCHAR *));
	if (!layout->text || !layout->family || !layout->locale || !layout->sizes || !layout->weights || !layout->styles || !layout->families) {
		tl_release((IDWriteTextLayout *)layout);
		return NULL;
	}
	if (text)
		memcpy(layout->text, text, length * sizeof(WCHAR));
	for (index = 0; index <= length; ++index) {
		layout->sizes[index] = size;
		layout->weights[index] = weight;
		layout->styles[index] = style;
	}
	layout->stretch = stretch;
	layout->max_width = layout->max_height = 3.4e38f;
	layout->ppd = 1.0f;
	layout->is_layout = is_layout;
	layout->dirty = TRUE;
	layout->tab = 0;
	return layout;
}

static HRESULT STDMETHODCALLTYPE factory_create_format(IDWriteFactory2 *self, const WCHAR *family, IDWriteFontCollection *collection, DWRITE_FONT_WEIGHT weight,
	DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch, FLOAT size, const WCHAR *locale, IDWriteTextFormat **format)
{
	TextLayout *layout;
	if (!format)
		return E_POINTER;
	*format = NULL;
	if (!family || size <= 0)
		return E_INVALIDARG;
	layout = make_layout(NULL, 0, family, weight, style, stretch, size, locale, FALSE);
	if (!layout)
		return E_OUTOFMEMORY;
	*format = (IDWriteTextFormat *)layout;
	return S_OK;
}

/* The layout copies the format's settings for the whole text. */
static HRESULT layout_from_format(const WCHAR *text, UINT32 length, IDWriteTextFormat *format, float width, float height, float ppd, BOOL gdi, BOOL natural, IDWriteTextLayout **result)
{
	TextLayout *source = (TextLayout *)format, *layout;
	if (!result)
		return E_POINTER;
	*result = NULL;
	if (!format || (length && !text))
		return E_INVALIDARG;
	layout = make_layout(text, length, source->family, source->length ? 0 : 0, 0, source->stretch, 0, source->locale, TRUE);
	if (!layout)
		return E_OUTOFMEMORY;
	{
		/* A format carries its settings in the first slot of the arrays kept for it. */
		UINT32 index;
		for (index = 0; index <= length; ++index) {
			layout->sizes[index] = source->sizes[0];
			layout->weights[index] = source->weights[0];
			layout->styles[index] = source->styles[0];
		}
	}
	layout->alignment = source->alignment;
	layout->paragraph = source->paragraph;
	layout->wrapping = source->wrapping;
	layout->reading = source->reading;
	layout->flow = source->flow;
	layout->tab = source->tab;
	layout->trimming = source->trimming;
	layout->spacing_method = source->spacing_method;
	layout->line_spacing = source->line_spacing;
	layout->line_baseline = source->line_baseline;
	layout->max_width = width;
	layout->max_height = height;
	layout->ppd = ppd > 0 ? ppd : 1.0f;
	layout->gdi_compatible = gdi;
	layout->natural = natural;
	*result = (IDWriteTextLayout *)layout;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE factory_create_layout(IDWriteFactory2 *self, const WCHAR *text, UINT32 length, IDWriteTextFormat *format, FLOAT width, FLOAT height, IDWriteTextLayout **layout)
{
	return layout_from_format(text, length, format, width, height, 1.0f, FALSE, FALSE, layout);
}
static HRESULT STDMETHODCALLTYPE factory_create_gdi_layout(IDWriteFactory2 *self, const WCHAR *text, UINT32 length, IDWriteTextFormat *format, FLOAT width, FLOAT height,
	FLOAT ppd, const DWRITE_MATRIX *transform, WINBOOL natural, IDWriteTextLayout **layout)
{
	return layout_from_format(text, length, format, width, height, ppd, TRUE, natural, layout);
}

/* ---- the ellipsis trimming sign: an inline object that measures and draws U+2026 in the format it was made for ---- */
typedef struct {
	const IDWriteInlineObjectVtbl *vtbl;
	LONG refs;
	IDWriteTextLayout *layout;
} TrimmingSign;

#define SIGN_OF(self) ((TrimmingSign *)(self))

static HRESULT STDMETHODCALLTYPE sign_query(IDWriteInlineObject *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteInlineObject)) {
		InterlockedIncrement(&SIGN_OF(self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE sign_add_ref(IDWriteInlineObject *self) { return (ULONG)InterlockedIncrement(&SIGN_OF(self)->refs); }
static ULONG STDMETHODCALLTYPE sign_release(IDWriteInlineObject *self)
{
	const LONG refs = InterlockedDecrement(&SIGN_OF(self)->refs);
	if (!refs) {
		IDWriteTextLayout_Release(SIGN_OF(self)->layout);
		FREE(self);
	}
	return (ULONG)refs;
}
static HRESULT STDMETHODCALLTYPE sign_draw(IDWriteInlineObject *self, void *context, IDWriteTextRenderer *renderer, FLOAT x, FLOAT y, WINBOOL sideways,
	WINBOOL rtl, IUnknown *effect)
{
	return IDWriteTextLayout_Draw(SIGN_OF(self)->layout, context, renderer, x, y);
}
static HRESULT STDMETHODCALLTYPE sign_metrics(IDWriteInlineObject *self, DWRITE_INLINE_OBJECT_METRICS *metrics)
{
	DWRITE_TEXT_METRICS text;
	DWRITE_LINE_METRICS line;
	UINT32 lines = 0;
	HRESULT result = IDWriteTextLayout_GetMetrics(SIGN_OF(self)->layout, &text);
	if (FAILED(result))
		return result;
	metrics->width = text.widthIncludingTrailingWhitespace;
	metrics->height = text.height;
	metrics->baseline = SUCCEEDED(IDWriteTextLayout_GetLineMetrics(SIGN_OF(self)->layout, &line, 1, &lines)) && lines ? line.baseline : text.height;
	metrics->supportsSideways = FALSE;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE sign_overhang(IDWriteInlineObject *self, DWRITE_OVERHANG_METRICS *overhangs)
{
	memset(overhangs, 0, sizeof(*overhangs));
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE sign_breaks(IDWriteInlineObject *self, DWRITE_BREAK_CONDITION *before, DWRITE_BREAK_CONDITION *after)
{
	*before = *after = DWRITE_BREAK_CONDITION_NEUTRAL;
	return S_OK;
}

static const IDWriteInlineObjectVtbl sign_vtbl = {sign_query, sign_add_ref, sign_release, sign_draw, sign_metrics, sign_overhang, sign_breaks};

static HRESULT STDMETHODCALLTYPE factory_ellipsis_sign(IDWriteFactory2 *self, IDWriteTextFormat *format, IDWriteInlineObject **out)
{
	static const WCHAR ellipsis[] = {0x2026, 0};
	TrimmingSign *sign;
	HRESULT result;
	if (!format || !out)
		return E_INVALIDARG;
	if (!(sign = ALLOC(sizeof(*sign))))
		return E_OUTOFMEMORY;
	if (FAILED(result = layout_from_format(ellipsis, 1, format, 1e6f, 1e6f, 1.0f, FALSE, FALSE, &sign->layout))) {
		FREE(sign);
		return result;
	}
	sign->vtbl = &sign_vtbl;
	sign->refs = 1;
	*out = (IDWriteInlineObject *)sign;
	return S_OK;
}

/* ---- the bitmap render target ---- */
typedef struct {
	const IDWriteBitmapRenderTargetVtbl *vtbl;
	LONG refs;
	HDC dc;
	HBITMAP bitmap, previous;
	UINT32 *bits;
	UINT32 width, height;
	float ppd;
	DWRITE_MATRIX transform;
} Target;

#define TARGET_OF(self) ((Target *)(self))

static HRESULT STDMETHODCALLTYPE target_query(IDWriteBitmapRenderTarget *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteBitmapRenderTarget)) {
		InterlockedIncrement(&TARGET_OF(self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE target_add_ref(IDWriteBitmapRenderTarget *self) { return (ULONG)InterlockedIncrement(&TARGET_OF(self)->refs); }
static ULONG STDMETHODCALLTYPE target_release(IDWriteBitmapRenderTarget *self)
{
	Target *target = TARGET_OF(self);
	LONG refs = InterlockedDecrement(&target->refs);
	if (!refs) {
		if (target->dc && target->previous)
			SelectObject(target->dc, target->previous);
		if (target->bitmap)
			DeleteObject(target->bitmap);
		if (target->dc)
			DeleteDC(target->dc);
		FREE(target);
	}
	return (ULONG)refs;
}

/* A glyph run is drawn from the alpha texture of its analysis: each pixel moves toward the text color by the coverage of its color channels. */
static HRESULT STDMETHODCALLTYPE target_draw(IDWriteBitmapRenderTarget *self, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN *run,
	IDWriteRenderingParams *params, COLORREF color, RECT *black_box)
{
	Target *target = TARGET_OF(self);
	IDWriteGlyphRunAnalysis *analysis = NULL;
	RECT bounds;
	BYTE *alpha;
	HRESULT result;
	int row, column;
	if (black_box)
		memset(black_box, 0, sizeof(*black_box));
	if (!run || !run->glyphCount)
		return S_OK;
	result = create_analysis(run, &target->transform, mode == DWRITE_MEASURING_MODE_NATURAL ? DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC : DWRITE_RENDERING_MODE_CLEARTYPE_GDI_CLASSIC, x, y, &analysis);
	if (FAILED(result))
		return result;
	result = IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(analysis, DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds);
	if (FAILED(result) || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
		IDWriteGlyphRunAnalysis_Release(analysis);
		return FAILED(result) ? result : S_OK;
	}
	alpha = ALLOC((size_t)(bounds.right - bounds.left) * (bounds.bottom - bounds.top) * 3);
	if (!alpha) {
		IDWriteGlyphRunAnalysis_Release(analysis);
		return E_OUTOFMEMORY;
	}
	result = IDWriteGlyphRunAnalysis_CreateAlphaTexture(analysis, DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds, alpha, (UINT32)((bounds.right - bounds.left) * (bounds.bottom - bounds.top) * 3));
	if (SUCCEEDED(result)) {
		const BYTE red = GetRValue(color), green = GetGValue(color), blue = GetBValue(color);
		for (row = bounds.top; row < bounds.bottom; ++row) {
			for (column = bounds.left; column < bounds.right; ++column) {
				const BYTE *coverage = alpha + ((size_t)(row - bounds.top) * (bounds.right - bounds.left) + (column - bounds.left)) * 3;
				UINT32 *pixel;
				UINT32 old;
				BYTE out_red, out_green, out_blue;
				if (column < 0 || row < 0 || (UINT32)column >= target->width || (UINT32)row >= target->height || (!coverage[0] && !coverage[1] && !coverage[2]))
					continue;
				pixel = &target->bits[(size_t)row * target->width + column];
				old = *pixel;
				out_red = (BYTE)(((old >> 16) & 255) + ((int)red - (int)((old >> 16) & 255)) * coverage[0] / 255);
				out_green = (BYTE)(((old >> 8) & 255) + ((int)green - (int)((old >> 8) & 255)) * coverage[1] / 255);
				out_blue = (BYTE)((old & 255) + ((int)blue - (int)(old & 255)) * coverage[2] / 255);
				*pixel = 0xff000000U | (UINT32)out_red << 16 | (UINT32)out_green << 8 | out_blue;
			}
		}
		if (black_box)
			*black_box = bounds;
	}
	FREE(alpha);
	IDWriteGlyphRunAnalysis_Release(analysis);
	return result;
}
static HDC STDMETHODCALLTYPE target_dc(IDWriteBitmapRenderTarget *self) { return TARGET_OF(self)->dc; }
static FLOAT STDMETHODCALLTYPE target_get_ppd(IDWriteBitmapRenderTarget *self) { return TARGET_OF(self)->ppd; }
static HRESULT STDMETHODCALLTYPE target_set_ppd(IDWriteBitmapRenderTarget *self, FLOAT value) { TARGET_OF(self)->ppd = value; return S_OK; }
static HRESULT STDMETHODCALLTYPE target_get_transform(IDWriteBitmapRenderTarget *self, DWRITE_MATRIX *transform) { *transform = TARGET_OF(self)->transform; return S_OK; }
static HRESULT STDMETHODCALLTYPE target_set_transform(IDWriteBitmapRenderTarget *self, const DWRITE_MATRIX *transform)
{
	DWRITE_MATRIX identity = {1, 0, 0, 1, 0, 0};
	TARGET_OF(self)->transform = transform ? *transform : identity;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE target_size(IDWriteBitmapRenderTarget *self, SIZE *size)
{
	size->cx = (LONG)TARGET_OF(self)->width;
	size->cy = (LONG)TARGET_OF(self)->height;
	return S_OK;
}

static HRESULT make_target_bitmap(Target *target, UINT32 width, UINT32 height)
{
	BITMAPINFO info;
	void *bits = NULL;
	HBITMAP bitmap;
	memset(&info, 0, sizeof(info));
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = (LONG)width;
	info.bmiHeader.biHeight = -(LONG)height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	bitmap = CreateDIBSection(target->dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!bitmap || !bits)
		return E_OUTOFMEMORY;
	if (target->bitmap) {
		SelectObject(target->dc, bitmap);
		DeleteObject(target->bitmap);
	} else {
		target->previous = (HBITMAP)SelectObject(target->dc, bitmap);
	}
	target->bitmap = bitmap;
	target->bits = bits;
	target->width = width;
	target->height = height;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE target_resize(IDWriteBitmapRenderTarget *self, UINT32 width, UINT32 height)
{
	Target *target = TARGET_OF(self);
	if (width == target->width && height == target->height)
		return S_OK;
	return make_target_bitmap(target, width, height);
}

static void *Target_table_[IDWriteBitmapRenderTarget_methods];

static HRESULT STDMETHODCALLTYPE interop_create_target(IDWriteGdiInterop *self, HDC dc, UINT32 width, UINT32 height, IDWriteBitmapRenderTarget **result)
{
	Target *target;
	HRESULT status;
	if (!result)
		return E_POINTER;
	*result = NULL;
	if (!Target_table_[0]) {
		memcpy(Target_table_, IDWriteBitmapRenderTarget_table, sizeof(Target_table_));
		Target_table_[IDWriteBitmapRenderTarget_QueryInterface] = (void *)target_query;
		Target_table_[IDWriteBitmapRenderTarget_AddRef] = (void *)target_add_ref;
		Target_table_[IDWriteBitmapRenderTarget_Release] = (void *)target_release;
		Target_table_[IDWriteBitmapRenderTarget_DrawGlyphRun] = (void *)target_draw;
		Target_table_[IDWriteBitmapRenderTarget_GetMemoryDC] = (void *)target_dc;
		Target_table_[IDWriteBitmapRenderTarget_GetPixelsPerDip] = (void *)target_get_ppd;
		Target_table_[IDWriteBitmapRenderTarget_SetPixelsPerDip] = (void *)target_set_ppd;
		Target_table_[IDWriteBitmapRenderTarget_GetCurrentTransform] = (void *)target_get_transform;
		Target_table_[IDWriteBitmapRenderTarget_SetCurrentTransform] = (void *)target_set_transform;
		Target_table_[IDWriteBitmapRenderTarget_GetSize] = (void *)target_size;
		Target_table_[IDWriteBitmapRenderTarget_Resize] = (void *)target_resize;
	}
	target = ALLOC(sizeof(Target));
	if (!target)
		return E_OUTOFMEMORY;
	target->vtbl = (const IDWriteBitmapRenderTargetVtbl *)Target_table_;
	target->refs = 1;
	target->ppd = 1.0f;
	target->transform.m11 = target->transform.m22 = 1.0f;
	target->dc = CreateCompatibleDC(dc);
	if (!target->dc) {
		FREE(target);
		return E_FAIL;
	}
	status = make_target_bitmap(target, width, height);
	if (FAILED(status)) {
		target_release((IDWriteBitmapRenderTarget *)target);
		return status;
	}
	*result = (IDWriteBitmapRenderTarget *)target;
	return S_OK;
}
