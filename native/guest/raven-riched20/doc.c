/* SPDX-License-Identifier: MIT */
#include "re.h"

void *re_alloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
}

void *re_calloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1);
}

void *re_realloc(void *block, SIZE_T size)
{
	return block ? HeapReAlloc(GetProcessHeap(), 0, block, size ? size : 1) : re_alloc(size);
}

void re_free(void *block)
{
	if (block)
		HeapFree(GetProcessHeap(), 0, block);
}

WCHAR *re_ansi_to_wide(const char *text, INT length, UINT code_page, INT *out_length)
{
	INT count = length ? MultiByteToWideChar(code_page, 0, text, length, NULL, 0) : 0;
	WCHAR *wide = re_alloc((count + 1) * sizeof(WCHAR));
	if (!wide)
		return NULL;
	if (count)
		MultiByteToWideChar(code_page, 0, text, length, wide, count);
	if (length < 0 && count)
		count--;
	wide[count] = 0;
	if (out_length)
		*out_length = count;
	return wide;
}

char *re_wide_to_ansi(const WCHAR *text, INT length, UINT code_page, INT *out_length)
{
	INT count = length ? WideCharToMultiByte(code_page, 0, text, length, NULL, 0, NULL, NULL) : 0;
	char *ansi = re_alloc(count + 1);
	if (!ansi)
		return NULL;
	if (count)
		WideCharToMultiByte(code_page, 0, text, length, ansi, count, NULL, NULL);
	if (length < 0 && count)
		count--;
	ansi[count] = 0;
	if (out_length)
		*out_length = count;
	return ansi;
}

static BOOL reserve_text(re_doc *doc, INT length)
{
	if (length <= doc->capacity)
		return TRUE;
	INT capacity = doc->capacity * 2 > length ? doc->capacity * 2 : length + 64;
	WCHAR *grown = re_realloc(doc->text, capacity * sizeof(WCHAR));
	if (!grown)
		return FALSE;
	doc->text = grown;
	doc->capacity = capacity;
	return TRUE;
}

static BOOL reserve_runs(re_doc *doc, INT count)
{
	if (count <= doc->run_capacity)
		return TRUE;
	INT capacity = doc->run_capacity * 2 > count ? doc->run_capacity * 2 : count + 8;
	re_run *grown = re_realloc(doc->runs, capacity * sizeof(re_run));
	if (!grown)
		return FALSE;
	doc->runs = grown;
	doc->run_capacity = capacity;
	return TRUE;
}

static BOOL reserve_paras(re_doc *doc, INT count)
{
	if (count <= doc->para_capacity)
		return TRUE;
	INT capacity = doc->para_capacity * 2 > count ? doc->para_capacity * 2 : count + 8;
	re_para *grown = re_realloc(doc->paras, capacity * sizeof(re_para));
	if (!grown)
		return FALSE;
	doc->paras = grown;
	doc->para_capacity = capacity;
	return TRUE;
}

void re_default_para_format(PARAFORMAT2 *out)
{
	memset(out, 0, sizeof(*out));
	out->cbSize = sizeof(*out);
	out->dwMask = PFM_ALL2;
	out->wAlignment = PFA_LEFT;
	out->bLineSpacingRule = 0;
	out->wNumberingStart = 0;
	out->wNumberingStyle = 0;
	out->sStyle = -1;
	out->wBorderSpace = 0;
}

void re_default_char_format(CHARFORMAT2W *out, HFONT font)
{
	LOGFONTW description;
	HFONT used = font ? font : GetStockObject(SYSTEM_FONT);
	memset(&description, 0, sizeof(description));
	GetObjectW(used, sizeof(description), &description);
	memset(out, 0, sizeof(*out));
	out->cbSize = sizeof(*out);
	out->dwMask = CFM_ALL2;
	out->dwEffects = CFE_AUTOCOLOR | CFE_AUTOBACKCOLOR;
	if (description.lfWeight >= FW_BOLD)
		out->dwEffects |= CFE_BOLD;
	if (description.lfItalic)
		out->dwEffects |= CFE_ITALIC;
	if (description.lfUnderline)
		out->dwEffects |= CFE_UNDERLINE;
	if (description.lfStrikeOut)
		out->dwEffects |= CFE_STRIKEOUT;
	// Twips: the logical height counts pixels at the screen's resolution.
	HDC screen = GetDC(NULL);
	INT dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
	if (screen)
		ReleaseDC(NULL, screen);
	if (dpi <= 0)
		dpi = 96;
	LONG height = description.lfHeight < 0 ? -description.lfHeight : description.lfHeight;
	if (!height)
		height = 13;
	out->yHeight = height * 1440 / dpi;
	out->wWeight = description.lfWeight ? description.lfWeight : FW_NORMAL;
	out->lcid = GetSystemDefaultLCID();
	out->bCharSet = description.lfCharSet;
	out->bPitchAndFamily = description.lfPitchAndFamily;
	lstrcpynW(out->szFaceName, description.lfFaceName, LF_FACESIZE);
	out->bUnderlineType = CFU_UNDERLINE;
}

BOOL re_doc_init(re_doc *doc, const CHARFORMAT2W *format)
{
	memset(doc, 0, sizeof(*doc));
	if (!reserve_text(doc, 64) || !reserve_runs(doc, 1) || !reserve_paras(doc, 1))
		return FALSE;
	doc->text[0] = RE_CR;
	doc->length = 1;
	doc->runs[0].start = 0;
	doc->runs[0].format = *format;
	doc->run_count = 1;
	doc->paras[0].start = 0;
	re_default_para_format(&doc->paras[0].format);
	doc->para_count = 1;
	return TRUE;
}

void re_doc_clear(re_doc *doc)
{
	re_free(doc->text);
	re_free(doc->runs);
	re_free(doc->paras);
	memset(doc, 0, sizeof(*doc));
}

re_doc *re_doc_clone(const re_doc *doc)
{
	re_doc *copy = re_calloc(sizeof(*copy));
	if (!copy || !reserve_text(copy, doc->length) || !reserve_runs(copy, doc->run_count) || !reserve_paras(copy, doc->para_count)) {
		if (copy)
			re_doc_clear(copy);
		re_free(copy);
		return NULL;
	}
	memcpy(copy->text, doc->text, doc->length * sizeof(WCHAR));
	memcpy(copy->runs, doc->runs, doc->run_count * sizeof(re_run));
	memcpy(copy->paras, doc->paras, doc->para_count * sizeof(re_para));
	copy->length = doc->length;
	copy->run_count = doc->run_count;
	copy->para_count = doc->para_count;
	return copy;
}

void re_doc_assign(re_doc *doc, re_doc *from)
{
	re_doc_clear(doc);
	*doc = *from;
	memset(from, 0, sizeof(*from));
}

INT re_doc_run_at(const re_doc *doc, INT at)
{
	INT low = 0, high = doc->run_count - 1;
	while (low < high) {
		INT middle = (low + high + 1) / 2;
		if (doc->runs[middle].start <= at)
			low = middle;
		else
			high = middle - 1;
	}
	return low;
}

INT re_doc_para_at(const re_doc *doc, INT at)
{
	INT low = 0, high = doc->para_count - 1;
	while (low < high) {
		INT middle = (low + high + 1) / 2;
		if (doc->paras[middle].start <= at)
			low = middle;
		else
			high = middle - 1;
	}
	return low;
}

INT re_doc_para_end(const re_doc *doc, INT para)
{
	return para + 1 < doc->para_count ? doc->paras[para + 1].start : doc->length;
}

void re_doc_char_format(const re_doc *doc, INT at, CHARFORMAT2W *out)
{
	*out = doc->runs[re_doc_run_at(doc, at)].format;
}

static BOOL same_format(const CHARFORMAT2W *a, const CHARFORMAT2W *b)
{
	return !memcmp(a, b, sizeof(*a));
}

static void merge_runs(re_doc *doc)
{
	INT kept = 0;
	for (INT i = 0; i < doc->run_count; i++) {
		// A run left without characters disappears, and so does the wall between equal neighbours.
		INT end = i + 1 < doc->run_count ? doc->runs[i + 1].start : doc->length;
		if (end <= doc->runs[i].start)
			continue;
		if (kept && same_format(&doc->runs[kept - 1].format, &doc->runs[i].format))
			continue;
		if (kept != i)
			doc->runs[kept] = doc->runs[i];
		kept++;
	}
	doc->run_count = kept;
}

// Splits so that a run starts exactly at this position.
static BOOL split_run(re_doc *doc, INT at)
{
	if (at <= 0 || at >= doc->length)
		return TRUE;
	INT index = re_doc_run_at(doc, at);
	if (doc->runs[index].start == at)
		return TRUE;
	if (!reserve_runs(doc, doc->run_count + 1))
		return FALSE;
	memmove(doc->runs + index + 2, doc->runs + index + 1, (doc->run_count - index - 1) * sizeof(re_run));
	doc->runs[index + 1] = doc->runs[index];
	doc->runs[index + 1].start = at;
	doc->run_count++;
	return TRUE;
}

BOOL re_doc_replace(re_doc *doc, INT at, INT remove, const WCHAR *text, INT length, const CHARFORMAT2W *format)
{
	if (at < 0 || at > doc->length - 1)
		return FALSE;
	if (at + remove > doc->length - 1)
		remove = doc->length - 1 - at;
	if (remove > 0) {
		// The paragraph holding the end of the removal lends its settings to what is joined.
		INT tail = re_doc_para_at(doc, at + remove);
		INT head = re_doc_para_at(doc, at);
		PARAFORMAT2 joined = doc->paras[tail].format;
		if (!split_run(doc, at) || !split_run(doc, at + remove))
			return FALSE;
		if (tail != head)
			doc->paras[head].format = joined;
		// Paragraphs that begin inside the removal go; the first keeps its place.
		INT write = head + 1;
		for (INT read = head + 1; read < doc->para_count; read++) {
			if (doc->paras[read].start <= at + remove)
				continue;
			doc->paras[write] = doc->paras[read];
			doc->paras[write].start -= remove;
			write++;
		}
		doc->para_count = write;
		memmove(doc->text + at, doc->text + at + remove, (doc->length - at - remove) * sizeof(WCHAR));
		doc->length -= remove;
		for (INT i = 0; i < doc->run_count; i++) {
			if (doc->runs[i].start >= at + remove)
				doc->runs[i].start -= remove;
			else if (doc->runs[i].start > at)
				doc->runs[i].start = at;
		}
		merge_runs(doc);
	}
	if (length <= 0)
		return TRUE;
	if (!reserve_text(doc, doc->length + length) || !split_run(doc, at))
		return FALSE;
	memmove(doc->text + at + length, doc->text + at, (doc->length - at) * sizeof(WCHAR));
	memcpy(doc->text + at, text, length * sizeof(WCHAR));
	doc->length += length;
	INT marks = 0;
	for (INT i = 0; i < length; i++) {
		if (text[i] == RE_CR)
			marks++;
	}
	for (INT i = 0; i < doc->run_count; i++) {
		if (doc->runs[i].start >= at)
			doc->runs[i].start += length;
	}
	// New text sits in its own run, placed before the run that now starts past it.
	INT place = 0;
	while (place < doc->run_count && doc->runs[place].start < at)
		place++;
	if (!reserve_runs(doc, doc->run_count + 1))
		return FALSE;
	memmove(doc->runs + place + 1, doc->runs + place, (doc->run_count - place) * sizeof(re_run));
	doc->runs[place].start = at;
	doc->runs[place].format = *format;
	doc->run_count++;
	merge_runs(doc);
	INT para = re_doc_para_at(doc, at);
	for (INT i = para + 1; i < doc->para_count; i++)
		doc->paras[i].start += length;
	if (marks) {
		if (!reserve_paras(doc, doc->para_count + marks))
			return FALSE;
		memmove(doc->paras + para + 1 + marks, doc->paras + para + 1, (doc->para_count - para - 1) * sizeof(re_para));
		INT added = 0;
		for (INT i = 0; i < length; i++) {
			if (text[i] == RE_CR) {
				added++;
				doc->paras[para + added].start = at + i + 1;
				doc->paras[para + added].format = doc->paras[para].format;
			}
		}
		doc->para_count += marks;
	}
	return TRUE;
}

void re_format_merge(CHARFORMAT2W *format, const CHARFORMAT2W *with)
{
	static const struct { DWORD mask, effect; } effects[] = {
		{CFM_BOLD, CFE_BOLD}, {CFM_ITALIC, CFE_ITALIC}, {CFM_UNDERLINE, CFE_UNDERLINE}, {CFM_STRIKEOUT, CFE_STRIKEOUT},
		{CFM_PROTECTED, CFE_PROTECTED}, {CFM_LINK, CFE_LINK}, {CFM_SMALLCAPS, CFE_SMALLCAPS}, {CFM_ALLCAPS, CFE_ALLCAPS},
		{CFM_HIDDEN, CFE_HIDDEN}, {CFM_OUTLINE, CFE_OUTLINE}, {CFM_SHADOW, CFE_SHADOW}, {CFM_EMBOSS, CFE_EMBOSS},
		{CFM_IMPRINT, CFE_IMPRINT}, {CFM_DISABLED, CFE_DISABLED}, {CFM_REVISED, CFE_REVISED},
		{CFM_SUBSCRIPT, CFE_SUBSCRIPT}, {CFM_SUPERSCRIPT, CFE_SUPERSCRIPT},
	};
	DWORD mask = with->dwMask;
	for (size_t i = 0; i < ARRAYSIZE(effects); i++) {
		if (mask & effects[i].mask)
			format->dwEffects = (format->dwEffects & ~effects[i].effect) | (with->dwEffects & effects[i].effect);
	}
	if (mask & CFM_COLOR) {
		format->dwEffects = (format->dwEffects & ~CFE_AUTOCOLOR) | (with->dwEffects & CFE_AUTOCOLOR);
		format->crTextColor = with->crTextColor;
	}
	if (mask & CFM_BACKCOLOR) {
		format->dwEffects = (format->dwEffects & ~CFE_AUTOBACKCOLOR) | (with->dwEffects & CFE_AUTOBACKCOLOR);
		format->crBackColor = with->crBackColor;
	}
	if (mask & CFM_SIZE)
		format->yHeight = with->yHeight;
	if (mask & CFM_OFFSET)
		format->yOffset = with->yOffset;
	if (mask & CFM_CHARSET)
		format->bCharSet = with->bCharSet;
	if (mask & CFM_FACE) {
		format->bPitchAndFamily = with->bPitchAndFamily;
		lstrcpynW(format->szFaceName, with->szFaceName, LF_FACESIZE);
	}
	if (mask & CFM_WEIGHT) {
		format->wWeight = with->wWeight ? with->wWeight : FW_NORMAL;
		format->dwEffects = format->wWeight > 550 ? format->dwEffects | CFE_BOLD : format->dwEffects & ~CFE_BOLD;
	} else if (mask & CFM_BOLD) {
		format->wWeight = with->dwEffects & CFE_BOLD ? FW_BOLD : FW_NORMAL;
	}
	if (mask & CFM_SPACING)
		format->sSpacing = with->sSpacing;
	if (mask & CFM_LCID)
		format->lcid = with->lcid;
	if (mask & CFM_STYLE)
		format->sStyle = with->sStyle;
	if (mask & CFM_KERNING)
		format->wKerning = with->wKerning;
	if (mask & CFM_UNDERLINETYPE) {
		format->bUnderlineType = with->bUnderlineType;
		if (!(mask & CFM_UNDERLINE))
			format->dwEffects = with->bUnderlineType ? format->dwEffects | CFE_UNDERLINE : format->dwEffects & ~CFE_UNDERLINE;
	}
	if (mask & CFM_ANIMATION)
		format->bAnimation = with->bAnimation;
	if (mask & CFM_REVAUTHOR)
		format->bRevAuthor = with->bRevAuthor;
	format->dwMask = CFM_ALL2;
}

void re_para_merge(PARAFORMAT2 *format, const PARAFORMAT2 *with)
{
	DWORD mask = with->dwMask;
	if (mask & PFM_NUMBERING)
		format->wNumbering = with->wNumbering;
	if (mask & PFM_STARTINDENT)
		format->dxStartIndent = with->dxStartIndent;
	if (mask & PFM_OFFSETINDENT)
		format->dxStartIndent += with->dxStartIndent;
	if (mask & PFM_RIGHTINDENT)
		format->dxRightIndent = with->dxRightIndent;
	if (mask & PFM_OFFSET)
		format->dxOffset = with->dxOffset;
	if (mask & PFM_ALIGNMENT)
		format->wAlignment = with->wAlignment;
	if (mask & PFM_TABSTOPS) {
		format->cTabCount = with->cTabCount < 0 ? 0 : with->cTabCount > MAX_TAB_STOPS ? MAX_TAB_STOPS : with->cTabCount;
		memcpy(format->rgxTabs, with->rgxTabs, sizeof(format->rgxTabs));
	}
	static const struct { DWORD mask; WORD effect; } effects[] = {
		{PFM_RTLPARA, PFE_RTLPARA}, {PFM_KEEP, PFE_KEEP}, {PFM_KEEPNEXT, PFE_KEEPNEXT}, {PFM_PAGEBREAKBEFORE, PFE_PAGEBREAKBEFORE},
		{PFM_NOLINENUMBER, PFE_NOLINENUMBER}, {PFM_NOWIDOWCONTROL, PFE_NOWIDOWCONTROL}, {PFM_DONOTHYPHEN, PFE_DONOTHYPHEN},
		{PFM_SIDEBYSIDE, PFE_SIDEBYSIDE}, {PFM_COLLAPSED, PFE_COLLAPSED}, {PFM_TABLE, PFE_TABLE},
	};
	for (size_t i = 0; i < ARRAYSIZE(effects); i++) {
		if (mask & effects[i].mask)
			format->wEffects = (format->wEffects & ~effects[i].effect) | (with->wEffects & effects[i].effect);
	}
	if (with->cbSize >= sizeof(PARAFORMAT2)) {
		if (mask & PFM_SPACEBEFORE)
			format->dySpaceBefore = with->dySpaceBefore;
		if (mask & PFM_SPACEAFTER)
			format->dySpaceAfter = with->dySpaceAfter;
		if (mask & PFM_LINESPACING) {
			format->dyLineSpacing = with->dyLineSpacing;
			format->bLineSpacingRule = with->bLineSpacingRule;
		}
		if (mask & PFM_STYLE)
			format->sStyle = with->sStyle;
		if (mask & PFM_SHADING) {
			format->wShadingWeight = with->wShadingWeight;
			format->wShadingStyle = with->wShadingStyle;
		}
		if (mask & PFM_NUMBERINGSTART)
			format->wNumberingStart = with->wNumberingStart;
		if (mask & PFM_NUMBERINGSTYLE)
			format->wNumberingStyle = with->wNumberingStyle;
		if (mask & PFM_NUMBERINGTAB)
			format->wNumberingTab = with->wNumberingTab;
		if (mask & PFM_BORDER) {
			format->wBorderSpace = with->wBorderSpace;
			format->wBorderWidth = with->wBorderWidth;
			format->wBorders = with->wBorders;
		}
	}
	format->dwMask = PFM_ALL2;
}

BOOL re_doc_set_format(re_doc *doc, INT start, INT end, const CHARFORMAT2W *format)
{
	if (start >= end)
		return TRUE;
	if (!split_run(doc, start) || !split_run(doc, end))
		return FALSE;
	for (INT i = re_doc_run_at(doc, start); i < doc->run_count && doc->runs[i].start < end; i++)
		re_format_merge(&doc->runs[i].format, format);
	merge_runs(doc);
	return TRUE;
}

void re_doc_set_para(re_doc *doc, INT start, INT end, const PARAFORMAT2 *format)
{
	INT first = re_doc_para_at(doc, start), last = re_doc_para_at(doc, end > start ? end - 1 : start);
	for (INT i = first; i <= last; i++)
		re_para_merge(&doc->paras[i].format, format);
}

BOOL re_doc_save_piece(const re_doc *doc, INT at, INT length, re_piece *out)
{
	memset(out, 0, sizeof(*out));
	if (length <= 0)
		return TRUE;
	out->length = length;
	out->text = re_alloc(length * sizeof(WCHAR));
	INT first = re_doc_run_at(doc, at), last = re_doc_run_at(doc, at + length - 1);
	INT para_first = re_doc_para_at(doc, at), para_last = re_doc_para_at(doc, at + length);
	out->run_count = last - first + 1;
	out->runs = re_alloc(out->run_count * sizeof(re_run));
	out->para_count = para_last - para_first + 1;
	out->paras = re_alloc(out->para_count * sizeof(re_para));
	if (!out->text || !out->runs || !out->paras) {
		re_doc_free_piece(out);
		return FALSE;
	}
	memcpy(out->text, doc->text + at, length * sizeof(WCHAR));
	for (INT i = 0; i < out->run_count; i++) {
		out->runs[i] = doc->runs[first + i];
		out->runs[i].start = out->runs[i].start < at ? 0 : out->runs[i].start - at;
	}
	for (INT i = 0; i < out->para_count; i++) {
		out->paras[i] = doc->paras[para_first + i];
		out->paras[i].start = out->paras[i].start < at ? 0 : out->paras[i].start - at;
	}
	return TRUE;
}

void re_doc_free_piece(re_piece *piece)
{
	re_free(piece->text);
	re_free(piece->runs);
	re_free(piece->paras);
	memset(piece, 0, sizeof(*piece));
}

BOOL re_doc_put_piece(re_doc *doc, INT at, const re_piece *piece)
{
	for (INT i = 0; i < piece->run_count; i++) {
		INT start = piece->runs[i].start, end = i + 1 < piece->run_count ? piece->runs[i + 1].start : piece->length;
		if (!re_doc_replace(doc, at + start, 0, piece->text + start, end - start, &piece->runs[i].format))
			return FALSE;
	}
	// The removal gave the joined paragraph the settings of the last one: each mark now gets its own back.
	for (INT i = 0; i < piece->para_count; i++) {
		INT para = re_doc_para_at(doc, at + piece->paras[i].start);
		doc->paras[para].format = piece->paras[i].format;
	}
	return TRUE;
}
