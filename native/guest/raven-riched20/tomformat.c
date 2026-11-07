/* SPDX-License-Identifier: MIT */
#include "tom_private.h"

// Fonts and paragraphs of a range. One bound to a range reads and writes the text; a duplicate only holds values of its own.

#define POINTS(twips) ((float)(twips) / 20.0f)
#define TWIPS(points) ((LONG)((points) * 20.0f))

typedef struct
{
	ITextFont iface;
	LONG refs;
	text_range *range;
	CHARFORMAT2W value;
	BOOL cached;	// answers come from value, not from the text
	BOOL later;	// and so do changes, until told to apply them
} text_font;

typedef struct
{
	ITextPara iface;
	LONG refs;
	text_range *range;
	PARAFORMAT2 value;
} text_para;

#define FONT(object) CONTAINING_RECORD(object, text_font, iface)
#define PARA(object) CONTAINING_RECORD(object, text_para, iface)
#define RELEASED(self) ((self)->range && !(self)->range->owner)

static HRESULT tom_bool_value(LONG value, LONG current, BOOL *out)
{
	switch (value) {
	case tomTrue:
		*out = TRUE;
		return S_OK;
	case tomFalse:
		*out = FALSE;
		return S_OK;
	case tomToggle:
		*out = current == tomUndefined || !current;
		return S_OK;
	default:
		return E_INVALIDARG;
	}
}

// ---- font ----

typedef enum
{
	FONT_ANIMATION, FONT_BACK, FONT_FORE, FONT_LANGUAGE, FONT_STYLE, FONT_UNDERLINE, FONT_WEIGHT,
	FONT_KERNING, FONT_POSITION, FONT_SIZE, FONT_SPACING,
} font_value;

static LONG font_read(const CHARFORMAT2W *format, font_value which)
{
	switch (which) {
	case FONT_ANIMATION: return format->bAnimation;
	case FONT_BACK: return format->dwEffects & CFE_AUTOBACKCOLOR ? tomAutoColor : (LONG)format->crBackColor;
	case FONT_FORE: return format->dwEffects & CFE_AUTOCOLOR ? tomAutoColor : (LONG)format->crTextColor;
	case FONT_LANGUAGE: return format->lcid;
	case FONT_STYLE: return format->sStyle;
	case FONT_UNDERLINE:
		if (!(format->dwEffects & CFE_UNDERLINE))
			return tomNone;
		return format->bUnderlineType ? format->bUnderlineType : tomSingle;
	case FONT_WEIGHT: return format->wWeight;
	case FONT_KERNING: return format->wKerning;
	case FONT_POSITION: return format->yOffset;
	case FONT_SIZE: return format->yHeight;
	case FONT_SPACING: return format->sSpacing;
	}
	return tomUndefined;
}

static void font_write(CHARFORMAT2W *format, font_value which, LONG value)
{
	switch (which) {
	case FONT_ANIMATION:
		format->dwMask |= CFM_ANIMATION;
		format->bAnimation = (BYTE)value;
		break;
	case FONT_BACK:
		format->dwMask |= CFM_BACKCOLOR;
		format->dwEffects = value == tomAutoColor ? format->dwEffects | CFE_AUTOBACKCOLOR : format->dwEffects & ~CFE_AUTOBACKCOLOR;
		format->crBackColor = value == tomAutoColor ? 0 : (COLORREF)value;
		break;
	case FONT_FORE:
		format->dwMask |= CFM_COLOR;
		format->dwEffects = value == tomAutoColor ? format->dwEffects | CFE_AUTOCOLOR : format->dwEffects & ~CFE_AUTOCOLOR;
		format->crTextColor = value == tomAutoColor ? 0 : (COLORREF)value;
		break;
	case FONT_LANGUAGE:
		format->dwMask |= CFM_LCID;
		format->lcid = value;
		break;
	case FONT_STYLE:
		format->dwMask |= CFM_STYLE;
		format->sStyle = (SHORT)value;
		break;
	case FONT_UNDERLINE:
		format->dwMask |= CFM_UNDERLINE | CFM_UNDERLINETYPE;
		format->dwEffects = value == tomNone ? format->dwEffects & ~CFE_UNDERLINE : format->dwEffects | CFE_UNDERLINE;
		format->bUnderlineType = value == tomNone ? 0 : (BYTE)value;
		break;
	case FONT_WEIGHT:
		format->dwMask |= CFM_WEIGHT;
		format->wWeight = (WORD)value;
		break;
	case FONT_KERNING:
		format->dwMask |= CFM_KERNING;
		format->wKerning = (WORD)value;
		break;
	case FONT_POSITION:
		format->dwMask |= CFM_OFFSET;
		format->yOffset = value;
		break;
	case FONT_SIZE:
		format->dwMask |= CFM_SIZE;
		format->yHeight = value;
		break;
	case FONT_SPACING:
		format->dwMask |= CFM_SPACING;
		format->sSpacing = (SHORT)value;
		break;
	}
}

// The first character answers for the range; a run that disagrees makes the answer undefined.
static void font_each_run(const text_font *self, void (*visit)(const CHARFORMAT2W *format, void *context), void *context)
{
	if (!self->range || self->cached) {
		visit(&self->value, context);
		return;
	}
	const re_doc *doc = &self->range->owner->editor->doc;
	LONG start, end;
	tom_range_ends(self->range, &start, &end);
	if (self->range->is_selection && start == end && self->range->owner->editor->has_insert_format) {
		visit(&self->range->owner->editor->insert_format, context);
		return;
	}
	CHARFORMAT2W format;
	re_doc_char_format(doc, start, &format);
	visit(&format, context);
	for (INT run = re_doc_run_at(doc, start) + 1; run < doc->run_count && doc->runs[run].start < end; run++)
		visit(&doc->runs[run].format, context);
}

typedef struct
{
	font_value which;
	DWORD effect;
	LONG result;
	BOOL seen;
} font_query;

static void merge_answer(font_query *query, LONG value)
{
	if (query->seen && query->result != value)
		query->result = tomUndefined;
	else if (!query->seen)
		query->result = value;
	query->seen = TRUE;
}

static void visit_value(const CHARFORMAT2W *format, void *context)
{
	font_query *query = context;
	merge_answer(query, font_read(format, query->which));
}

static void visit_effect(const CHARFORMAT2W *format, void *context)
{
	font_query *query = context;
	merge_answer(query, format->dwEffects & query->effect ? tomTrue : tomFalse);
}

static DWORD font_value_mask(font_value which)
{
	switch (which) {
	case FONT_ANIMATION: return CFM_ANIMATION;
	case FONT_BACK: return CFM_BACKCOLOR;
	case FONT_FORE: return CFM_COLOR;
	case FONT_LANGUAGE: return CFM_LCID;
	case FONT_STYLE: return CFM_STYLE;
	case FONT_UNDERLINE: return CFM_UNDERLINE | CFM_UNDERLINETYPE;
	case FONT_WEIGHT: return CFM_WEIGHT;
	case FONT_KERNING: return CFM_KERNING;
	case FONT_POSITION: return CFM_OFFSET;
	case FONT_SIZE: return CFM_SIZE;
	case FONT_SPACING: return CFM_SPACING;
	}
	return 0;
}

static LONG font_value_of(const text_font *self, font_value which)
{
	if (!self->range && !(self->value.dwMask & font_value_mask(which)))
		return tomUndefined;
	font_query query = {.which = which};
	font_each_run(self, visit_value, &query);
	return query.result;
}

// Each effect has the mask bit of the same value, but for the ones that moved in the later formats.
static DWORD effect_mask(DWORD effect)
{
	return effect == CFE_SUBSCRIPT || effect == CFE_SUPERSCRIPT ? CFM_SUBSCRIPT : effect;
}

static LONG font_effect_of(const text_font *self, DWORD effect)
{
	if (!self->range && !(self->value.dwMask & effect_mask(effect)))
		return tomUndefined;
	font_query query = {.effect = effect};
	font_each_run(self, visit_effect, &query);
	return query.result;
}

static HRESULT font_store(text_font *self, const CHARFORMAT2W *change)
{
	if (!self->range || self->cached)
		re_format_merge(&self->value, change);
	if (!self->range || self->later)
		return S_OK;
	re_editor *editor = self->range->owner->editor;
	if (editor->read_only)
		return E_ACCESSDENIED;
	LONG start, end;
	tom_range_ends(self->range, &start, &end);
	if (start == end) {
		if (self->range->is_selection)
			re_apply_format(editor, SCF_SELECTION, change);
		return S_OK;
	}
	re_undo_format(editor);
	re_doc_set_format(&editor->doc, start, end, change);
	editor->modified = TRUE;
	re_refresh(editor);
	return S_OK;
}

static HRESULT font_get_value(text_font *self, font_value which, LONG *out)
{
	if (!out)
		return E_INVALIDARG;
	if (RELEASED(self))
		return CO_E_RELEASED;
	*out = font_value_of(self, which);
	return S_OK;
}

static HRESULT font_set_value(text_font *self, font_value which, LONG value)
{
	if (RELEASED(self))
		return CO_E_RELEASED;
	if (value == tomUndefined)
		return S_OK;
	CHARFORMAT2W change = {.cbSize = sizeof(change)};
	if (self->range && which == FONT_UNDERLINE && value == tomToggle)
		value = font_value_of(self, which) == tomNone ? tomSingle : tomNone;
	font_write(&change, which, value);
	return font_store(self, &change);
}

static HRESULT font_get_points(text_font *self, font_value which, float *out)
{
	if (!out)
		return E_INVALIDARG;
	if (RELEASED(self))
		return CO_E_RELEASED;
	LONG value = font_value_of(self, which);
	*out = value == tomUndefined ? (float)tomUndefined : POINTS(value);
	return S_OK;
}

static HRESULT font_set_points(text_font *self, font_value which, float value)
{
	if (RELEASED(self))
		return CO_E_RELEASED;
	if (value == (float)tomUndefined)
		return S_OK;
	CHARFORMAT2W change = {.cbSize = sizeof(change)};
	font_write(&change, which, TWIPS(value));
	return font_store(self, &change);
}

static HRESULT font_get_effect(text_font *self, DWORD effect, LONG *out)
{
	if (!out)
		return E_INVALIDARG;
	if (RELEASED(self))
		return CO_E_RELEASED;
	*out = font_effect_of(self, effect);
	return S_OK;
}

static HRESULT font_set_effect(text_font *self, DWORD mask, DWORD effect, LONG value)
{
	if (RELEASED(self))
		return CO_E_RELEASED;
	if (value == tomUndefined)
		return S_OK;
	BOOL on;
	if (FAILED(tom_bool_value(value, font_effect_of(self, effect), &on)))
		return E_INVALIDARG;
	CHARFORMAT2W change = {.cbSize = sizeof(change), .dwMask = mask, .dwEffects = on ? effect : 0};
	return font_store(self, &change);
}

static HRESULT STDMETHODCALLTYPE font_query_iface(ITextFont *iface, REFIID iid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDispatch) || IsEqualIID(iid, &IID_ITextFont))
		*out = iface;
	if (!*out)
		return E_NOINTERFACE;
	ITextFont_AddRef(iface);
	return S_OK;
}

static ULONG STDMETHODCALLTYPE font_add_ref(ITextFont *iface)
{
	return InterlockedIncrement(&FONT(iface)->refs);
}

static ULONG STDMETHODCALLTYPE font_release(ITextFont *iface)
{
	text_font *self = FONT(iface);
	ULONG left = InterlockedDecrement(&self->refs);
	if (!left) {
		if (self->range)
			ITextRange_Release(&self->range->iface);
		re_free(self);
	}
	return left;
}

static HRESULT STDMETHODCALLTYPE font_type_info_count(ITextFont *iface, UINT *out)
{
	(void)iface;
	*out = 1;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE font_type_info(ITextFont *iface, UINT index, LCID locale, ITypeInfo **out)
{
	(void)iface; (void)index; (void)locale;
	return tom_type_info(&IID_ITextFont, out);
}

static HRESULT STDMETHODCALLTYPE font_ids(ITextFont *iface, REFIID iid, LPOLESTR *names, UINT count, LCID locale, DISPID *ids)
{
	(void)iface; (void)iid; (void)locale;
	return tom_get_ids(&IID_ITextFont, names, count, ids);
}

static HRESULT STDMETHODCALLTYPE font_invoke(ITextFont *iface, DISPID id, REFIID iid, LCID locale, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *exception, UINT *error)
{
	(void)iid; (void)locale;
	return tom_invoke(iface, &IID_ITextFont, id, flags, params, result, exception, error);
}

static void font_snapshot(const text_font *self, CHARFORMAT2W *out)
{
	if (!self->range || self->cached) {
		*out = self->value;
		return;
	}
	LONG start, end;
	tom_range_ends(self->range, &start, &end);
	re_doc_char_format(&self->range->owner->editor->doc, start, out);
	out->dwMask = CFM_ALL2;
}

static HRESULT font_create_detached(const CHARFORMAT2W *value, ITextFont **out);

static HRESULT STDMETHODCALLTYPE font_get_duplicate(ITextFont *iface, ITextFont **out)
{
	text_font *self = FONT(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (RELEASED(self))
		return CO_E_RELEASED;
	CHARFORMAT2W value;
	font_snapshot(self, &value);
	return font_create_detached(&value, out);
}

// Any font can be the source, so its values are read through its interface.
static HRESULT STDMETHODCALLTYPE font_set_duplicate(ITextFont *iface, ITextFont *from)
{
	text_font *self = FONT(iface);
	if (RELEASED(self))
		return CO_E_RELEASED;
	if (!from)
		return E_INVALIDARG;
	CHARFORMAT2W value;
	font_snapshot(FONT(from), &value);
	return font_store(self, &value);
}

static HRESULT STDMETHODCALLTYPE font_can_change(ITextFont *iface, LONG *out)
{
	text_font *self = FONT(iface);
	if (RELEASED(self))
		return CO_E_RELEASED;
	BOOL can = self->range && !self->range->owner->editor->read_only;
	if (out)
		*out = can ? tomTrue : tomFalse;
	return can ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE font_is_equal(ITextFont *iface, ITextFont *other, LONG *out)
{
	text_font *self = FONT(iface);
	if (RELEASED(self))
		return CO_E_RELEASED;
	BOOL same = FALSE;
	if (other && !RELEASED(FONT(other))) {
		CHARFORMAT2W mine, theirs;
		font_snapshot(self, &mine);
		font_snapshot(FONT(other), &theirs);
		same = !memcmp(&mine, &theirs, sizeof(mine));
	}
	if (out)
		*out = same ? tomTrue : tomFalse;
	return same ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE font_reset(ITextFont *iface, LONG value)
{
	text_font *self = FONT(iface);
	if (RELEASED(self))
		return CO_E_RELEASED;
	switch (value) {
	case tomApplyNow:
		if (self->later) {
			self->later = FALSE;
			return font_store(self, &self->value);
		}
		return S_OK;
	case tomApplyLater:
	case tomCacheParms:
		if (self->range && !self->cached) {
			font_snapshot(self, &self->value);
			self->cached = TRUE;
		}
		self->later = value == tomApplyLater && self->range;
		return S_OK;
	case tomTrackParms:
		self->cached = self->later = FALSE;
		return S_OK;
	}
	if (self->range) {
		if (value != tomDefault)
			return E_INVALIDARG;
		CHARFORMAT2W defaults;
		re_default_char_format(&defaults, NULL);
		defaults.dwMask = CFM_ALL2;
		return font_store(self, &defaults);
	}
	if (value == tomUndefined) {
		memset(&self->value, 0, sizeof(self->value));
		self->value.cbSize = sizeof(self->value);
		return S_OK;
	}
	if (value == tomDefault) {
		re_default_char_format(&self->value, NULL);
		self->value.dwMask = CFM_ALL2;
		return S_OK;
	}
	return E_INVALIDARG;
}

#define FONT_LONG(name, which) \
	static HRESULT STDMETHODCALLTYPE font_get_##name(ITextFont *iface, LONG *out) { return font_get_value(FONT(iface), which, out); } \
	static HRESULT STDMETHODCALLTYPE font_set_##name(ITextFont *iface, LONG value) { return font_set_value(FONT(iface), which, value); }
#define FONT_FLOAT(name, which) \
	static HRESULT STDMETHODCALLTYPE font_get_##name(ITextFont *iface, float *out) { return font_get_points(FONT(iface), which, out); } \
	static HRESULT STDMETHODCALLTYPE font_set_##name(ITextFont *iface, float value) { return font_set_points(FONT(iface), which, value); }
#define FONT_EFFECT(name, mask, effect) \
	static HRESULT STDMETHODCALLTYPE font_get_##name(ITextFont *iface, LONG *out) { return font_get_effect(FONT(iface), effect, out); } \
	static HRESULT STDMETHODCALLTYPE font_set_##name(ITextFont *iface, LONG value) { return font_set_effect(FONT(iface), mask, effect, value); }

FONT_LONG(style, FONT_STYLE)
FONT_EFFECT(all_caps, CFM_ALLCAPS, CFE_ALLCAPS)
FONT_LONG(animation, FONT_ANIMATION)
FONT_LONG(back, FONT_BACK)
FONT_EFFECT(bold, CFM_BOLD, CFE_BOLD)
FONT_EFFECT(emboss, CFM_EMBOSS, CFE_EMBOSS)
FONT_LONG(fore, FONT_FORE)
FONT_EFFECT(hidden, CFM_HIDDEN, CFE_HIDDEN)
FONT_EFFECT(engrave, CFM_IMPRINT, CFE_IMPRINT)
FONT_EFFECT(italic, CFM_ITALIC, CFE_ITALIC)
FONT_FLOAT(kerning, FONT_KERNING)
FONT_LONG(language, FONT_LANGUAGE)
FONT_EFFECT(outline, CFM_OUTLINE, CFE_OUTLINE)
FONT_FLOAT(position, FONT_POSITION)
FONT_EFFECT(protected, CFM_PROTECTED, CFE_PROTECTED)
FONT_EFFECT(shadow, CFM_SHADOW, CFE_SHADOW)
FONT_FLOAT(size, FONT_SIZE)
FONT_EFFECT(small_caps, CFM_SMALLCAPS, CFE_SMALLCAPS)
FONT_FLOAT(spacing, FONT_SPACING)
FONT_EFFECT(strike, CFM_STRIKEOUT, CFE_STRIKEOUT)
FONT_EFFECT(subscript, CFM_SUBSCRIPT, CFE_SUBSCRIPT)
FONT_EFFECT(superscript, CFM_SUPERSCRIPT, CFE_SUPERSCRIPT)
FONT_LONG(underline, FONT_UNDERLINE)
FONT_LONG(weight, FONT_WEIGHT)

static HRESULT STDMETHODCALLTYPE font_get_name(ITextFont *iface, BSTR *out)
{
	text_font *self = FONT(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (RELEASED(self))
		return CO_E_RELEASED;
	CHARFORMAT2W value;
	font_snapshot(self, &value);
	*out = SysAllocString(value.szFaceName);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE font_set_name(ITextFont *iface, BSTR name)
{
	text_font *self = FONT(iface);
	if (RELEASED(self))
		return CO_E_RELEASED;
	if (!name)
		return E_INVALIDARG;
	CHARFORMAT2W change = {.cbSize = sizeof(change), .dwMask = CFM_FACE};
	lstrcpynW(change.szFaceName, name, LF_FACESIZE);
	return font_store(self, &change);
}

static const ITextFontVtbl font_vtbl = {
	font_query_iface, font_add_ref, font_release, font_type_info_count, font_type_info, font_ids, font_invoke,
	font_get_duplicate, font_set_duplicate, font_can_change, font_is_equal, font_reset,
	font_get_style, font_set_style, font_get_all_caps, font_set_all_caps, font_get_animation, font_set_animation,
	font_get_back, font_set_back, font_get_bold, font_set_bold, font_get_emboss, font_set_emboss, font_get_fore, font_set_fore,
	font_get_hidden, font_set_hidden, font_get_engrave, font_set_engrave, font_get_italic, font_set_italic,
	font_get_kerning, font_set_kerning, font_get_language, font_set_language, font_get_name, font_set_name,
	font_get_outline, font_set_outline, font_get_position, font_set_position, font_get_protected, font_set_protected,
	font_get_shadow, font_set_shadow, font_get_size, font_set_size, font_get_small_caps, font_set_small_caps,
	font_get_spacing, font_set_spacing, font_get_strike, font_set_strike, font_get_subscript, font_set_subscript,
	font_get_superscript, font_set_superscript, font_get_underline, font_set_underline, font_get_weight, font_set_weight,
};

static HRESULT font_create_detached(const CHARFORMAT2W *value, ITextFont **out)
{
	text_font *self = re_calloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = (ITextFontVtbl *)&font_vtbl;
	self->refs = 1;
	self->value = *value;
	*out = &self->iface;
	return S_OK;
}

HRESULT tom_font_create(text_range *range, ITextFont **out)
{
	text_font *self = re_calloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = (ITextFontVtbl *)&font_vtbl;
	self->refs = 1;
	self->range = range;
	ITextRange_AddRef(&range->iface);
	*out = &self->iface;
	return S_OK;
}

// ---- paragraph ----

static void para_snapshot(const text_para *self, PARAFORMAT2 *out)
{
	if (!self->range) {
		*out = self->value;
		return;
	}
	LONG start, end;
	const re_doc *doc = &self->range->owner->editor->doc;
	tom_range_ends(self->range, &start, &end);
	*out = doc->paras[re_doc_para_at(doc, start)].format;
	out->dwMask = PFM_ALL2;
}

static HRESULT para_store(text_para *self, const PARAFORMAT2 *change)
{
	if (!self->range) {
		re_para_merge(&self->value, change);
		return S_OK;
	}
	re_editor *editor = self->range->owner->editor;
	if (editor->read_only)
		return E_ACCESSDENIED;
	LONG start, end;
	tom_range_ends(self->range, &start, &end);
	re_undo_format(editor);
	re_doc_set_para(&editor->doc, start, end, change);
	editor->modified = TRUE;
	re_refresh(editor);
	return S_OK;
}

static LONG alignment_from(WORD alignment)
{
	switch (alignment) {
	case PFA_CENTER: return tomAlignCenter;
	case PFA_RIGHT: return tomAlignRight;
	case PFA_JUSTIFY: return tomAlignJustify;
	default: return tomAlignLeft;
	}
}

static HRESULT STDMETHODCALLTYPE para_query_iface(ITextPara *iface, REFIID iid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDispatch) || IsEqualIID(iid, &IID_ITextPara))
		*out = iface;
	if (!*out)
		return E_NOINTERFACE;
	ITextPara_AddRef(iface);
	return S_OK;
}

static ULONG STDMETHODCALLTYPE para_add_ref(ITextPara *iface)
{
	return InterlockedIncrement(&PARA(iface)->refs);
}

static ULONG STDMETHODCALLTYPE para_release(ITextPara *iface)
{
	text_para *self = PARA(iface);
	ULONG left = InterlockedDecrement(&self->refs);
	if (!left) {
		if (self->range)
			ITextRange_Release(&self->range->iface);
		re_free(self);
	}
	return left;
}

static HRESULT STDMETHODCALLTYPE para_type_info_count(ITextPara *iface, UINT *out)
{
	(void)iface;
	*out = 1;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE para_type_info(ITextPara *iface, UINT index, LCID locale, ITypeInfo **out)
{
	(void)iface; (void)index; (void)locale;
	return tom_type_info(&IID_ITextPara, out);
}

static HRESULT STDMETHODCALLTYPE para_ids(ITextPara *iface, REFIID iid, LPOLESTR *names, UINT count, LCID locale, DISPID *ids)
{
	(void)iface; (void)iid; (void)locale;
	return tom_get_ids(&IID_ITextPara, names, count, ids);
}

static HRESULT STDMETHODCALLTYPE para_invoke(ITextPara *iface, DISPID id, REFIID iid, LCID locale, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *exception, UINT *error)
{
	(void)iid; (void)locale;
	return tom_invoke(iface, &IID_ITextPara, id, flags, params, result, exception, error);
}

static HRESULT STDMETHODCALLTYPE para_get_duplicate(ITextPara *iface, ITextPara **out)
{
	text_para *self = PARA(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (RELEASED(self))
		return CO_E_RELEASED;
	text_para *copy = re_calloc(sizeof(*copy));
	if (!copy)
		return E_OUTOFMEMORY;
	copy->iface.lpVtbl = self->iface.lpVtbl;
	copy->refs = 1;
	para_snapshot(self, &copy->value);
	*out = &copy->iface;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE para_set_duplicate(ITextPara *iface, ITextPara *from)
{
	text_para *self = PARA(iface);
	if (RELEASED(self))
		return CO_E_RELEASED;
	if (!from)
		return E_INVALIDARG;
	PARAFORMAT2 value;
	para_snapshot(PARA(from), &value);
	return para_store(self, &value);
}

static HRESULT STDMETHODCALLTYPE para_can_change(ITextPara *iface, LONG *out)
{
	text_para *self = PARA(iface);
	if (RELEASED(self))
		return CO_E_RELEASED;
	BOOL can = self->range && !self->range->owner->editor->read_only;
	if (out)
		*out = can ? tomTrue : tomFalse;
	return can ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE para_is_equal(ITextPara *iface, ITextPara *other, LONG *out)
{
	text_para *self = PARA(iface);
	if (RELEASED(self))
		return CO_E_RELEASED;
	BOOL same = FALSE;
	if (other && !RELEASED(PARA(other))) {
		PARAFORMAT2 mine, theirs;
		para_snapshot(self, &mine);
		para_snapshot(PARA(other), &theirs);
		same = !memcmp(&mine, &theirs, sizeof(mine));
	}
	if (out)
		*out = same ? tomTrue : tomFalse;
	return same ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE para_reset(ITextPara *iface, LONG value)
{
	text_para *self = PARA(iface);
	if (RELEASED(self))
		return CO_E_RELEASED;
	if (value != tomDefault)
		return value == tomUndefined ? S_OK : E_INVALIDARG;
	PARAFORMAT2 change;
	re_default_para_format(&change);
	change.dwMask = PFM_ALL2;
	return para_store(self, &change);
}

#define PARA_GET(name, type, expression) \
	static HRESULT STDMETHODCALLTYPE para_get_##name(ITextPara *iface, type *out) \
	{ \
		text_para *self = PARA(iface); \
		if (!out) \
			return E_INVALIDARG; \
		if (RELEASED(self)) \
			return CO_E_RELEASED; \
		PARAFORMAT2 value; \
		para_snapshot(self, &value); \
		*out = (type)(expression); \
		return S_OK; \
	}
#define PARA_SET(name, type, mask, statement) \
	static HRESULT STDMETHODCALLTYPE para_set_##name(ITextPara *iface, type input) \
	{ \
		text_para *self = PARA(iface); \
		if (RELEASED(self)) \
			return CO_E_RELEASED; \
		PARAFORMAT2 change = {.cbSize = sizeof(change), .dwMask = mask}; \
		statement; \
		return para_store(self, &change); \
	}
#define PARA_LONG(name, mask, field) \
	PARA_GET(name, LONG, value.field) \
	PARA_SET(name, LONG, mask, change.field = input)
#define PARA_POINTS(name, mask, field) \
	PARA_GET(name, float, POINTS(value.field)) \
	PARA_SET(name, float, mask, change.field = TWIPS(input))
#define PARA_FLAG(name, mask, effect, inverted) \
	PARA_GET(name, LONG, (!!(value.wEffects & (effect)) != (inverted)) ? tomTrue : tomFalse) \
	PARA_SET(name, LONG, mask, change.wEffects = ((input != tomFalse) != (inverted)) ? (effect) : 0)

PARA_LONG(style, PFM_STYLE, sStyle)
PARA_GET(alignment, LONG, alignment_from(value.wAlignment))
PARA_SET(alignment, LONG, PFM_ALIGNMENT, change.wAlignment = input == tomAlignCenter ? PFA_CENTER : input == tomAlignRight ? PFA_RIGHT : input == tomAlignJustify ? PFA_JUSTIFY : PFA_LEFT)
PARA_FLAG(hyphenation, PFM_DONOTHYPHEN, PFE_DONOTHYPHEN, TRUE)
PARA_GET(first_indent, float, POINTS(-value.dxOffset))
PARA_FLAG(keep_together, PFM_KEEP, PFE_KEEP, FALSE)
PARA_FLAG(keep_next, PFM_KEEPNEXT, PFE_KEEPNEXT, FALSE)
PARA_GET(left_indent, float, POINTS(value.dxStartIndent + value.dxOffset))
PARA_GET(line_spacing, float, POINTS(value.dyLineSpacing))
PARA_GET(line_rule, LONG, value.bLineSpacingRule)
PARA_LONG(list_alignment, PFM_NUMBERINGSTYLE, wNumberingStyle)
PARA_GET(list_level, LONG, 0)
PARA_SET(list_level, LONG, 0, (void)input)
PARA_LONG(list_start, PFM_NUMBERINGSTART, wNumberingStart)
PARA_POINTS(list_tab, PFM_NUMBERINGTAB, wNumberingTab)
PARA_LONG(list_type, PFM_NUMBERING, wNumbering)
PARA_FLAG(no_line_number, PFM_NOLINENUMBER, PFE_NOLINENUMBER, FALSE)
PARA_FLAG(page_break, PFM_PAGEBREAKBEFORE, PFE_PAGEBREAKBEFORE, FALSE)
PARA_POINTS(right_indent, PFM_RIGHTINDENT, dxRightIndent)
PARA_POINTS(space_after, PFM_SPACEAFTER, dySpaceAfter)
PARA_POINTS(space_before, PFM_SPACEBEFORE, dySpaceBefore)
PARA_FLAG(widow, PFM_NOWIDOWCONTROL, PFE_NOWIDOWCONTROL, TRUE)
PARA_GET(tab_count, LONG, value.cTabCount)

static HRESULT STDMETHODCALLTYPE para_set_indents(ITextPara *iface, float first, float left, float right)
{
	text_para *self = PARA(iface);
	if (RELEASED(self))
		return CO_E_RELEASED;
	PARAFORMAT2 change = {
		.cbSize = sizeof(change), .dwMask = PFM_STARTINDENT | PFM_OFFSET | PFM_RIGHTINDENT,
		.dxStartIndent = TWIPS(left + first), .dxOffset = TWIPS(-first), .dxRightIndent = TWIPS(right),
	};
	return para_store(self, &change);
}

static HRESULT STDMETHODCALLTYPE para_set_line_spacing(ITextPara *iface, LONG rule, float spacing)
{
	text_para *self = PARA(iface);
	if (RELEASED(self))
		return CO_E_RELEASED;
	PARAFORMAT2 change = {.cbSize = sizeof(change), .dwMask = PFM_LINESPACING, .bLineSpacingRule = (BYTE)rule, .dyLineSpacing = TWIPS(spacing)};
	return para_store(self, &change);
}

// A tab keeps its position in the low bits, its alignment and leader above them.
static HRESULT STDMETHODCALLTYPE para_add_tab(ITextPara *iface, float position, LONG alignment, LONG leader)
{
	text_para *self = PARA(iface);
	if (RELEASED(self))
		return CO_E_RELEASED;
	PARAFORMAT2 change;
	para_snapshot(self, &change);
	if (change.cTabCount >= MAX_TAB_STOPS)
		return E_FAIL;
	LONG stop = TWIPS(position) | ((alignment & 7) << 24) | ((leader & 7) << 28);
	INT at = 0;
	while (at < change.cTabCount && (change.rgxTabs[at] & 0xffffff) < (stop & 0xffffff))
		at++;
	memmove(change.rgxTabs + at + 1, change.rgxTabs + at, (change.cTabCount - at) * sizeof(LONG));
	change.rgxTabs[at] = stop;
	change.cTabCount++;
	change.dwMask = PFM_TABSTOPS;
	return para_store(self, &change);
}

static HRESULT STDMETHODCALLTYPE para_clear_tabs(ITextPara *iface)
{
	text_para *self = PARA(iface);
	if (RELEASED(self))
		return CO_E_RELEASED;
	PARAFORMAT2 change = {.cbSize = sizeof(change), .dwMask = PFM_TABSTOPS};
	return para_store(self, &change);
}

static HRESULT STDMETHODCALLTYPE para_delete_tab(ITextPara *iface, float position)
{
	text_para *self = PARA(iface);
	if (RELEASED(self))
		return CO_E_RELEASED;
	PARAFORMAT2 change;
	para_snapshot(self, &change);
	for (INT i = 0; i < change.cTabCount; i++) {
		if ((change.rgxTabs[i] & 0xffffff) != TWIPS(position))
			continue;
		memmove(change.rgxTabs + i, change.rgxTabs + i + 1, (change.cTabCount - i - 1) * sizeof(LONG));
		change.cTabCount--;
		change.dwMask = PFM_TABSTOPS;
		return para_store(self, &change);
	}
	return E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE para_get_tab(ITextPara *iface, LONG index, float *position, LONG *alignment, LONG *leader)
{
	text_para *self = PARA(iface);
	if (!position)
		return E_INVALIDARG;
	if (RELEASED(self))
		return CO_E_RELEASED;
	PARAFORMAT2 value;
	para_snapshot(self, &value);
	if (index < 0 || index >= value.cTabCount)
		return S_FALSE;
	*position = POINTS(value.rgxTabs[index] & 0xffffff);
	if (alignment)
		*alignment = (value.rgxTabs[index] >> 24) & 7;
	if (leader)
		*leader = (value.rgxTabs[index] >> 28) & 7;
	return S_OK;
}

static const ITextParaVtbl para_vtbl = {
	para_query_iface, para_add_ref, para_release, para_type_info_count, para_type_info, para_ids, para_invoke,
	para_get_duplicate, para_set_duplicate, para_can_change, para_is_equal, para_reset,
	para_get_style, para_set_style, para_get_alignment, para_set_alignment, para_get_hyphenation, para_set_hyphenation,
	para_get_first_indent, para_get_keep_together, para_set_keep_together, para_get_keep_next, para_set_keep_next,
	para_get_left_indent, para_get_line_spacing, para_get_line_rule,
	para_get_list_alignment, para_set_list_alignment, para_get_list_level, para_set_list_level, para_get_list_start, para_set_list_start,
	para_get_list_tab, para_set_list_tab, para_get_list_type, para_set_list_type, para_get_no_line_number, para_set_no_line_number,
	para_get_page_break, para_set_page_break, para_get_right_indent, para_set_right_indent, para_set_indents, para_set_line_spacing,
	para_get_space_after, para_set_space_after, para_get_space_before, para_set_space_before, para_get_widow, para_set_widow,
	para_get_tab_count, para_add_tab, para_clear_tabs, para_delete_tab, para_get_tab,
};

HRESULT tom_para_create(text_range *range, ITextPara **out)
{
	text_para *self = re_calloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = (ITextParaVtbl *)&para_vtbl;
	self->refs = 1;
	self->range = range;
	ITextRange_AddRef(&range->iface);
	*out = &self->iface;
	return S_OK;
}
